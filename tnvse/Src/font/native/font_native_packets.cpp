#include "font_native_shape_internal.h"
#include "font_native_internal.h"

#include "load_config.h"
#include "NiBound.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>

namespace fonthook::vectorfont
{
	namespace implementation::font_native_packets {}
	using namespace implementation::font_native_packets;

	namespace implementation::font_native_packets
	{
		struct PacketSpan
		{
			size_t firstRange = 0;
			size_t rangeCount = 0;
			UInt32 firstVertex = 0;
			UInt32 vertexCount = 0;
			UInt32 startIndex = 0;
			UInt32 indexCount = 0;
			NativeFontShaderClass shaderClass = NativeFontShaderClass::Body;
			NativeFontSampling sampling = NativeFontSampling::Point;
			UInt32 layer = 3;
			UInt16 atlasPage = 0;
			bool usesSdf = false;
			bool staticSmoothSampling = false;
			bool usesLiveTileRgb = true;
			std::array<float, kNativeFontPacketConstantFloatCount> constants = {};
		};

		bool HasTileProperty(const NiTriShape* shape)
		{
			NiShadeProperty* property = shape ? shape->GetShadeProperty() : nullptr;
			return property && property->m_eShaderType == NiShadeProperty::PROP_Tile;
		}

		bool HasFiniteNativeVertex(const NativeFontGpuVertex& vertex)
		{
			return std::isfinite(vertex.x)
				&& std::isfinite(vertex.y)
				&& std::isfinite(vertex.z)
				&& std::isfinite(vertex.u)
				&& std::isfinite(vertex.v)
				&& std::isfinite(vertex.sdfSpread)
				&& std::isfinite(vertex.distanceParameterScale)
				&& std::isfinite(vertex.layerMask)
				&& std::isfinite(vertex.glyphU0)
				&& std::isfinite(vertex.glyphV0)
				&& std::isfinite(vertex.glyphU1)
				&& std::isfinite(vertex.glyphV1);
		}

		bool HasFiniteRegistrationBound(const NiBound& bound)
		{
			return std::isfinite(bound.m_kCenter.x)
				&& std::isfinite(bound.m_kCenter.y)
				&& std::isfinite(bound.m_kCenter.z)
				&& std::isfinite(bound.m_fRadius)
				&& bound.m_fRadius >= 0.0f;
		}

		bool HasValidRegistrationVertex(const NativeFontGpuVertex& vertex)
		{
			return HasFiniteNativeVertex(vertex)
				&& vertex.sdfSpread >= 0.0f
				&& vertex.distanceParameterScale >= 1.0f
				&& vertex.layerMask >= 1.0f
				&& vertex.layerMask <= 15.0f
				&& vertex.glyphU0 <= vertex.glyphU1
				&& vertex.glyphV0 <= vertex.glyphV1;
		}

		bool SealNativeFontPayloadValidation(
			NativeFontPayloadTemplate& payload)
		{
			if (!payload.pageCount || !payload.quadCount
				|| !payload.sourceRangeCount
				|| payload.quadCount > kNativeFontMaximumQuads
				|| payload.gpuVertices.size()
					< static_cast<size_t>(payload.quadCount) * 4u
				|| (payload.gpuVertices.size() & 3u)
				|| payload.gpuVertices.size() / 4u
					> kNativeFontMaximumQuads
				|| payload.gpuVertices.size()
					> std::numeric_limits<UInt32>::max()
				|| payload.packets.empty()
				|| payload.packets.size()
					> std::numeric_limits<UInt32>::max()
				|| payload.compositePackets.size()
					> std::numeric_limits<UInt32>::max()
				|| payload.pageCount != payload.atlasProperties.size()
				|| payload.pageCount != payload.atlasTextures.size()
				|| !HasFiniteRegistrationBound(payload.bound)
				|| !std::all_of(payload.gpuVertices.begin(),
					payload.gpuVertices.end(), HasValidRegistrationVertex))
			{
				return false;
			}

			auto validatePackets = [&](const std::vector<NativeFontPacketTemplate>& packets,
				bool composite)
			{
				for (const NativeFontPacketTemplate& packet : packets)
				{
					const UInt64 vertexEnd =
						static_cast<UInt64>(packet.firstVertex)
						+ packet.vertexCount;
					if (!packet.vertexCount || (packet.firstVertex & 3u)
						|| (packet.vertexCount & 3u)
						|| packet.vertexCount / 4u > kNativeFontMaximumQuads
						|| vertexEnd > payload.gpuVertices.size()
						|| packet.atlasPage >= payload.pageCount
						|| packet.layer > 3
						|| !HasFiniteRegistrationBound(packet.bound)
						|| !std::all_of(packet.constants.begin(),
							packet.constants.end(),
							[](float value) { return std::isfinite(value); })
						|| (composite
							&& (packet.shaderClass
									!= NativeFontShaderClass::Composite
								|| packet.staticCompositeLayerMask > 15u
								|| !std::isfinite(packet.uniformSdfSpread)
								|| packet.uniformSdfSpread < 0.0f
								|| !std::isfinite(
									packet.uniformDistanceParameterScale)
								|| packet.uniformDistanceParameterScale < 0.0f
								|| (packet.uniformDistanceParameterScale > 0.0f
									&& packet.uniformDistanceParameterScale < 1.0f))))
					{
						return false;
					}
				}
				return true;
			};
			if (!validatePackets(payload.packets, false)
				|| !validatePackets(payload.compositePackets, true))
			{
				return false;
			}

			NativeFontPayloadValidationSeal seal;
			seal.abi = NativeFontPayloadValidationSeal::kAbi;
			seal.pageCount = payload.pageCount;
			seal.quadCount = payload.quadCount;
			seal.sourceRangeCount = payload.sourceRangeCount;
			seal.vertexCount = static_cast<UInt32>(payload.gpuVertices.size());
			seal.packetCount = static_cast<UInt32>(payload.packets.size());
			seal.compositePacketCount = static_cast<UInt32>(
				payload.compositePackets.size());
			seal.vanillaLikeBitmapPackets =
				UsesOnlyVanillaLikeBitmapPackets(payload.packets);
			payload.validationSeal = seal;
			return true;
		}

		bool ResolveNativeShaderClass(NativeFontCompiledShaderClass source,
			NativeFontShaderClass& result)
		{
			switch (source)
			{
			case NativeFontCompiledShaderClass::Body:
				result = NativeFontShaderClass::Body;
				return true;
			case NativeFontCompiledShaderClass::Effect:
				result = NativeFontShaderClass::Effect;
				return true;
			case NativeFontCompiledShaderClass::Coverage:
				result = NativeFontShaderClass::Coverage;
				return true;
			case NativeFontCompiledShaderClass::Argb:
				result = NativeFontShaderClass::Argb;
				return true;
			default:
				return false;
			}
		}

		NativeFontSampling ResolveSampling(const NativeFontCompiledRange&)
		{
			return NativeFontSampling::LinearLod0;
		}

		bool SamePacketTarget(const PacketSpan& span,
			const NativeFontCompiledRange& range, NativeFontShaderClass shaderClass,
			NativeFontSampling sampling)
		{
			const UInt64 expectedVertex = static_cast<UInt64>(span.firstVertex)
				+ span.vertexCount;
			const UInt64 expectedIndex = static_cast<UInt64>(span.startIndex)
				+ span.indexCount;
			return expectedVertex == range.range.firstVertex
				&& expectedIndex == range.range.startIndex
				&& span.shaderClass == shaderClass
				&& span.sampling == sampling
				&& span.layer == range.range.layer
				&& span.atlasPage == range.range.atlasPage
				&& span.usesSdf == range.range.usesSdf
				&& span.staticSmoothSampling == range.staticSmoothSampling
				&& span.usesLiveTileRgb == range.range.usesLiveTileRgb
				// c1 now contains the packet layer modifier; it is part of the
				// immutable packet profile just like c2-c4.
				&& std::memcmp(span.constants.data(), range.constants.data(),
					kNativeFontPacketConstantFloatCount * sizeof(float)) == 0;
		}

		size_t HashNativePacketProfile(const NativeFontPacketTemplate& packet,
			bool writeEffectAlpha)
		{
			size_t hash = 2166136261u;
			auto mix = [&hash](UInt32 value)
			{
				hash ^= value;
				hash *= 16777619u;
			};
			mix(static_cast<UInt32>(packet.shaderClass));
			mix(static_cast<UInt32>(packet.sampling));
			mix(static_cast<UInt32>(packet.quality));
			mix(static_cast<UInt32>(packet.distanceFieldMethod));
			mix(packet.staticCompositeLayerMask);
			mix(packet.compositeShiftedShadow ? 1u : 0u);
			mix(writeEffectAlpha ? 1u : 0u);
			mix(packet.usesLiveTileRgb ? 1u : 0u);
			// Precomputed hashes describe the ordinary native layout. Keep this
			// discriminator in exact lockstep with NativeProfileKeyHash so an
			// equivalent runtime-computed key cannot land in another bucket.
			mix(0u); // NativeFontVanillaLayoutKind::None
			mix(0u); // uniformDistanceParameterScaleBits
			for (float value : packet.constants)
			{
				UInt32 bits = 0;
				std::memcpy(&bits, &value, sizeof(bits));
				mix(bits);
			}
			return hash;
		}

		void FinalizeNativePacketProfileHashes(
			NativeFontPacketTemplate& packet)
		{
			packet.profileHashes[0] =
				HashNativePacketProfile(packet, false);
			packet.profileHashes[1] =
				HashNativePacketProfile(packet, true);
		}

		NativeFontCompiledRange CompileRange(const NativeFontEffectShapeConfig& effects,
			const NativeFontDrawRange& range)
		{
			if (effects.precomposedArgb)
			{
				NativeFontCompiledRange compiled;
				compiled.range = range;
				compiled.range.layer = 3;
				compiled.range.usesSdf = false;
				compiled.range.usesLiveTileRgb = true;
				compiled.range.sdfSpreadPixels = 0.0f;
				compiled.range.sourceToLogicalScale = 1.0f;
				compiled.range.layerColorModifier =
					{ 1.0f, 1.0f, 1.0f, 1.0f };
				compiled.shaderClass = NativeFontCompiledShaderClass::Argb;
				compiled.staticSmoothSampling = true;
				return compiled;
			}
			if (effects.bakedCoverage)
			{
				NativeFontCompiledRange compiled;
				compiled.range = range;
				// Coverage, effect color and effect opacity already live in the A8
				// mask plus COLOR0. Normalize all packet-owned state so contiguous
				// Shadow/Glow/Outline/Fill ranges on one atlas page collapse into a
				// single draw while retaining their original vertex order.
				compiled.range.layer = 3;
				compiled.range.usesSdf = false;
				compiled.range.usesLiveTileRgb = true;
				compiled.range.sdfSpreadPixels = 0.0f;
				compiled.range.sourceToLogicalScale = 1.0f;
				compiled.range.layerColorModifier =
					{ 1.0f, 1.0f, 1.0f, 1.0f };
				compiled.shaderClass = NativeFontCompiledShaderClass::Coverage;
				compiled.staticSmoothSampling = true;
				return compiled;
			}

			float inverseAtlasWidth = effects.inverseAtlasWidth;
			float inverseAtlasHeight = effects.inverseAtlasHeight;
			if (range.atlasPage < effects.atlasInverseSizes.size())
			{
				const NiPoint2& inverseSize = effects.atlasInverseSizes[range.atlasPage];
				inverseAtlasWidth = inverseSize.x;
				inverseAtlasHeight = inverseSize.y;
			}
			float parameter0 = effects.shadowBlurPixels;
			float parameter1 = effects.shadowPower;
			float parameter2 = 0.0f;
			float parameter3 = 0.0f;
			float sdfFlag1 = 0.0f;
			float sdfFlag2 = 0.0f;
			float sdfFlag3 = 0.0f;
			const bool hardShadowComposite = range.layer == 0
				&& effects.shadowBlurPixels <= 0.001f
				&& (effects.shadowGlowAlpha > 0.0f
					|| effects.shadowOutlineAlpha > 0.0f);
			if (hardShadowComposite)
			{
				parameter0 = effects.glowInnerPixels;
				parameter1 = effects.glowOuterPixels;
				parameter2 = effects.glowPower;
				parameter3 = effects.outlineWidthPixels;
				sdfFlag1 = effects.outlineSoftnessPixels;
				sdfFlag2 = effects.shadowGlowAlpha;
				sdfFlag3 = effects.shadowOutlineAlpha;
			}
			if (range.layer == 1)
			{
				parameter0 = effects.glowInnerPixels;
				parameter1 = effects.glowOuterPixels;
				parameter2 = effects.glowPower;
			}
			else if (range.layer == 2)
			{
				parameter0 = effects.outlineWidthPixels;
				parameter1 = effects.outlineSoftnessPixels;
			}
			else if (range.layer == 0 && !hardShadowComposite)
			{
				parameter0 = effects.shadowBlurPixels;
			}
			else if (range.layer == 3)
			{
				parameter0 = 0.0f;
				parameter1 = 0.0f;
			}

			NativeFontCompiledRange compiled;
			compiled.range = range;
			const float layerAndFlags = static_cast<float>(range.layer)
				+ (range.usesLiveTileRgb ? 0.0f : 0.25f);
			compiled.constants = {{
				range.layerColorModifier.r, range.layerColorModifier.g,
				range.layerColorModifier.b, range.layerColorModifier.a,
				inverseAtlasWidth, inverseAtlasHeight,
				layerAndFlags, 0.0f,
				parameter0, parameter1, parameter2, parameter3,
				1.0f, sdfFlag1, sdfFlag2, sdfFlag3
			}};
			compiled.shaderClass = effects.shaderEffects && range.layer != 3
				? NativeFontCompiledShaderClass::Effect : NativeFontCompiledShaderClass::Body;
			compiled.staticSmoothSampling = true;
			return compiled;
		}

		bool BuildPacketSpans(const NativeFontEffectShapeConfig& effects,
			UInt32 vertexCount, std::vector<PacketSpan>& spans)
		{
			spans.clear();
			if (effects.ranges.empty() || effects.atlasProperties.empty()
				|| effects.atlasProperties.size() != effects.atlasTextures.size())
				return false;
			const UInt64 sourceIndexCount = static_cast<UInt64>(vertexCount / 4u) * 6u;
			for (size_t rangeIndex = 0; rangeIndex < effects.ranges.size(); ++rangeIndex)
			{
				const NativeFontDrawRange& range = effects.ranges[rangeIndex];
				const UInt64 indexCount = static_cast<UInt64>(range.primitiveCount) * 3u;
				if (!range.vertexCount || !range.primitiveCount
					|| (range.firstVertex & 3u) || (range.vertexCount & 3u)
					|| (range.startIndex % 6u) || (range.primitiveCount & 1u)
					|| range.vertexCount / 4u != range.primitiveCount / 2u
					|| static_cast<UInt64>(range.firstVertex) + range.vertexCount > vertexCount
					|| static_cast<UInt64>(range.startIndex) + indexCount > sourceIndexCount
					|| range.atlasPage >= effects.atlasProperties.size())
				{
					return false;
				}
				const NativeFontCompiledRange compiled = CompileRange(effects, range);
				NativeFontShaderClass shaderClass = NativeFontShaderClass::Body;
				if (!ResolveNativeShaderClass(compiled.shaderClass, shaderClass))
					return false;
				const NativeFontSampling sampling = ResolveSampling(compiled);
				if (!spans.empty()
					&& SamePacketTarget(spans.back(), compiled, shaderClass, sampling))
				{
					PacketSpan& span = spans.back();
					++span.rangeCount;
					span.vertexCount += range.vertexCount;
					span.indexCount += static_cast<UInt32>(indexCount);
					RecordFreeTypePerf(FreeTypePerfCounter::MergedPacketRange);
				}
				else
				{
					PacketSpan span;
					span.firstRange = rangeIndex;
					span.rangeCount = 1;
					span.firstVertex = range.firstVertex;
					span.vertexCount = range.vertexCount;
					span.startIndex = range.startIndex;
					span.indexCount = static_cast<UInt32>(indexCount);
					span.shaderClass = shaderClass;
					span.sampling = sampling;
					span.layer = range.layer;
					span.atlasPage = range.atlasPage;
					span.usesSdf = range.usesSdf;
					span.staticSmoothSampling = compiled.staticSmoothSampling;
					span.usesLiveTileRgb = range.usesLiveTileRgb;
					span.constants = compiled.constants;
					spans.push_back(span);
				}
			}
			return !spans.empty();
		}

		NativeFontPacketTemplate BuildCompositePacket(
			const NativeFontEffectShapeConfig& effects,
			const NativeFontCompositeSpan& span, const NiBound& bound,
			UInt8 staticLayerMask, float uniformSdfSpread,
			float uniformDistanceParameterScale)
		{
			NativeFontPacketTemplate packet;
			packet.firstVertex = span.firstVertex;
			packet.vertexCount = span.vertexCount;
			packet.bound = bound;
			packet.shaderClass = NativeFontShaderClass::Composite;
			packet.sampling = NativeFontSampling::LinearLod0;
			packet.quality = effects.quality;
			packet.distanceFieldMethod = effects.distanceFieldMethod;
			packet.layer = 3;
			packet.atlasPage = span.atlasPage;
			packet.staticCompositeLayerMask = staticLayerMask;
			packet.compositeShiftedShadow = (staticLayerMask & 1u)
				&& (effects.shadowOffsetX != 0.0f
					|| effects.shadowOffsetY != 0.0f);
			packet.staticSmoothSampling = true;
			packet.usesLiveTileRgb = true;

			auto writeColor = [&](size_t offset, const NiColorA& color)
			{
				packet.constants[offset + 0] = color.r;
				packet.constants[offset + 1] = color.g;
				packet.constants[offset + 2] = color.b;
				packet.constants[offset + 3] = color.a;
			};
			writeColor(0, effects.layerColorModifiers[0]);
			const NiPoint2 inverseSize =
				span.atlasPage < effects.atlasInverseSizes.size()
					? effects.atlasInverseSizes[span.atlasPage]
					: NiPoint2(effects.inverseAtlasWidth,
						effects.inverseAtlasHeight);
			packet.constants[4] = inverseSize.x;
			packet.constants[5] = inverseSize.y;
			// Keep this outside AtlasPass: ordinary composite shaders read spread
			// per vertex, so including it in their constant/profile identity would
			// fragment profile reuse and prevent otherwise valid packet merging.
			packet.uniformSdfSpread = uniformSdfSpread;
			packet.uniformDistanceParameterScale =
				uniformDistanceParameterScale;
			packet.constants[7] = effects.rasterScale;
			packet.constants[8] = effects.shadowBlurPixels;
			packet.constants[9] = effects.shadowPower;
			packet.constants[10] = effects.glowInnerPixels;
			packet.constants[11] = effects.glowOuterPixels;
			packet.constants[12] = effects.glowPower;
			packet.constants[13] = effects.outlineWidthPixels;
			packet.constants[14] = effects.outlineSoftnessPixels;
			packet.constants[15] = effects.shadowGlowAlpha;
			writeColor(16, effects.layerColorModifiers[1]);
			writeColor(20, effects.layerColorModifiers[2]);
			writeColor(24, effects.layerColorModifiers[3]);
			packet.constants[28] = effects.shadowOutlineAlpha;
			UInt32 liveTileRgbMask = 0;
			for (UInt32 layer = 0; layer < 4; ++layer)
			{
				if (effects.layerUsesLiveTileRgb[layer])
					liveTileRgbMask |= 1u << layer;
			}
			packet.constants[29] = static_cast<float>(liveTileRgbMask);
			packet.constants[30] =
				effects.shadowOffsetX * effects.shadowOffsetRasterScale;
			packet.constants[31] =
				effects.shadowOffsetY * effects.shadowOffsetRasterScale;
			FinalizeNativePacketProfileHashes(packet);
			return packet;
		}

		UInt8 ResolveStaticCompositeLayerMask(
			const std::vector<NativeFontGpuVertex>& vertices,
			const NativeFontCompositeSpan& span)
		{
			const size_t end = static_cast<size_t>(span.firstVertex)
				+ span.vertexCount;
			if (!span.vertexCount || end > vertices.size())
				return 0;
			UInt8 resolved = 0;
			for (size_t index = span.firstVertex; index < end; ++index)
			{
				const float encoded = vertices[index].layerMask;
				const UInt32 mask = static_cast<UInt32>(encoded);
				if (mask < 1u || mask > 15u
					|| encoded != static_cast<float>(mask))
				{
					return 0;
				}
				if (!resolved)
					resolved = static_cast<UInt8>(mask);
				else if (resolved != mask)
					return 0;
			}
			return resolved;
		}

		float ResolveUniformCompositeSdfSpread(
			const std::vector<NativeFontGpuVertex>& vertices,
			const NativeFontCompositeSpan& span)
		{
			const size_t end = static_cast<size_t>(span.firstVertex)
				+ span.vertexCount;
			if (!span.vertexCount || end > vertices.size())
				return 0.0f;
			const float spread = vertices[span.firstVertex].sdfSpread;
			if (!std::isfinite(spread) || spread <= 0.0f)
				return 0.0f;
			for (size_t index = span.firstVertex + 1u; index < end; ++index)
			{
				if (vertices[index].sdfSpread != spread)
					return 0.0f;
			}
			return spread;
		}

		float ResolveUniformCompositeDistanceParameterScale(
			const std::vector<NativeFontGpuVertex>& vertices,
			const NativeFontCompositeSpan& span)
		{
			const size_t end = static_cast<size_t>(span.firstVertex)
				+ span.vertexCount;
			if (!span.vertexCount || end > vertices.size())
				return 0.0f;
			const float scale =
				vertices[span.firstVertex].distanceParameterScale;
			if (!std::isfinite(scale) || scale < 1.0f)
				return 0.0f;
			for (size_t index = span.firstVertex + 1u; index < end; ++index)
			{
				if (vertices[index].distanceParameterScale != scale)
					return 0.0f;
			}
			return scale;
		}

	}

	NativeFontPayloadTemplatePtr BuildNativeFontPayloadTemplate(
		std::vector<NativeFontGpuVertex>&& vertices, UInt32 quadCount,
		const NativeFontEffectShapeConfig& effects, const NiBound& bound,
		std::vector<NativeFontCompositeSpan>&& compositeSpans)
	{
		if (!quadCount || quadCount > kNativeFontMaximumQuads
			|| vertices.size() < static_cast<size_t>(quadCount) * 4u
			|| (vertices.size() & 3u)
			|| vertices.size() / 4u > kNativeFontMaximumQuads
			|| effects.atlasProperties.empty()
			|| effects.atlasProperties.size() != effects.atlasTextures.size()
			|| effects.atlasProperties.size() > std::numeric_limits<UInt32>::max()
			|| static_cast<UInt32>(effects.distanceFieldMethod)
				> static_cast<UInt32>(DistanceFieldMethod::Mtsdf))
		{
			return {};
		}

		std::vector<PacketSpan> spans;
		if (!BuildPacketSpans(effects, static_cast<UInt32>(vertices.size()), spans))
			return {};

		auto payload = std::make_shared<NativeFontPayloadTemplate>();
		payload->pageCount = static_cast<UInt32>(effects.atlasProperties.size());
		payload->quadCount = quadCount;
		payload->sourceRangeCount = static_cast<UInt32>(effects.ranges.size());
		payload->bound = bound;
		payload->atlasProperties = effects.atlasProperties;
		payload->atlasTextures = effects.atlasTextures;
		payload->gpuVertices = std::move(vertices);
		payload->packets.reserve(spans.size());

		for (const PacketSpan& span : spans)
		{
			const UInt32 packetQuadCount = span.vertexCount / 4u;
			if (!packetQuadCount || packetQuadCount > kNativeFontMaximumQuads)
			{
				return {};
			}
			NativeFontPacketTemplate packet;
			packet.firstVertex = span.firstVertex;
			packet.vertexCount = span.vertexCount;
			packet.bound = bound;
			packet.constants = span.constants;
			if (effects.shaderEffects)
				packet.constants[7] = effects.rasterScale;
			packet.shaderClass = span.shaderClass;
			packet.sampling = span.sampling;
			packet.quality = effects.quality;
			packet.distanceFieldMethod = effects.distanceFieldMethod;
			packet.layer = span.layer;
			packet.atlasPage = span.atlasPage;
			packet.staticSmoothSampling = span.staticSmoothSampling;
			packet.usesLiveTileRgb = span.usesLiveTileRgb;
			FinalizeNativePacketProfileHashes(packet);
			payload->packets.push_back(std::move(packet));
		}
		if (effects.shaderEffects && !compositeSpans.empty())
		{
			payload->compositePackets.reserve(compositeSpans.size());
			for (const NativeFontCompositeSpan& span : compositeSpans)
			{
				const UInt64 end = static_cast<UInt64>(span.firstVertex)
					+ span.vertexCount;
				if (!span.vertexCount || (span.firstVertex & 3u)
					|| (span.vertexCount & 3u)
					|| span.vertexCount / 4u > kNativeFontMaximumQuads
					|| end > payload->gpuVertices.size()
					|| span.atlasPage >= payload->pageCount)
				{
					payload->compositePackets.clear();
					break;
				}
				const UInt8 staticLayerMask =
					ResolveStaticCompositeLayerMask(
						payload->gpuVertices, span);
				const float uniformSdfSpread =
					ResolveUniformCompositeSdfSpread(
						payload->gpuVertices, span);
				const float uniformDistanceParameterScale =
					ResolveUniformCompositeDistanceParameterScale(
						payload->gpuVertices, span);
				payload->compositePackets.push_back(
					BuildCompositePacket(effects, span, bound,
						staticLayerMask, uniformSdfSpread,
						uniformDistanceParameterScale));
			}
		}
		if (!SealNativeFontPayloadValidation(*payload))
			return {};
		payload->cpuMemory.Reset(CpuMemoryCategory::TextArtifact,
			GetNativeFontPayloadTemplateBytes(*payload));
		return payload;
	}

	bool InitializeNativeFontShapePayload(Font& font,
		NiTriShape* facade, const NativeFontShapeMetadata& metadata,
		NativeFontPayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin, NativeFontShapePayload& payload)
	{
		const bool sealed = payloadTemplate
			&& HasNativeFontPayloadValidationSeal(*payloadTemplate);
		if (!facade || !HasTileProperty(facade) || !payloadTemplate
			|| payloadTemplate->quadCount != metadata.quadCount
			|| (!sealed && (!payloadTemplate->pageCount
				|| !payloadTemplate->quadCount
				|| payloadTemplate->pageCount
					!= payloadTemplate->atlasProperties.size()
				|| payloadTemplate->pageCount
					!= payloadTemplate->atlasTextures.size()
				|| payloadTemplate->gpuVertices.empty()
				|| payloadTemplate->packets.empty()))
			|| !EnsureNativeFontProxyPool())
		{
			return false;
		}

		auto validatePackets = [&](const std::vector<NativeFontPacketTemplate>& packets)
		{
			for (const NativeFontPacketTemplate& source : packets)
			{
				const UInt64 vertexEnd = static_cast<UInt64>(source.firstVertex)
					+ source.vertexCount;
				if (!source.vertexCount || (source.vertexCount & 3u)
					|| source.vertexCount / 4u > kNativeFontMaximumQuads
					|| vertexEnd > payloadTemplate->gpuVertices.size()
					|| source.atlasPage >= payloadTemplate->pageCount)
				{
					return false;
				}
			}
			return true;
		};
		if (!sealed && (!validatePackets(payloadTemplate->packets)
			|| (!payloadTemplate->compositePackets.empty()
				&& !validatePackets(payloadTemplate->compositePackets))))
			return false;
		const bool singletonInlinePayload =
			metadata.backend
				== FreeTypeShapeBackend::SingletonFacade;
		if (!singletonInlinePayload)
		{
			payload.packetShaders.force_heap_storage();
			payload.packetPrograms.force_heap_storage();
			payload.preflightAtlasTextures.force_heap_storage();
			payload.retainedText.packets.force_heap_storage();
			payload.retainedText.runs.force_heap_storage();
		}

		payload.payloadTemplate = std::move(payloadTemplate);
		payload.geometryOrigin = geometryOrigin;
		payload.packetShaders.assign(payload.payloadTemplate->packets.size(), nullptr);
		payload.packetPrograms.assign(payload.payloadTemplate->packets.size(),
			nullptr);
		payload.preflightAtlasTextures.assign(payload.payloadTemplate->pageCount, nullptr);
		if (g_bEnableFreeTypeFontCommandBuffer)
		{
			const size_t retainedCapacity = std::max(
				payload.payloadTemplate->packets.size(),
				payload.payloadTemplate->compositePackets.size());
			payload.retainedText.packets.reserve(retainedCapacity);
			payload.retainedText.runs.reserve(retainedCapacity);
		}
		payload.preparedGeneration = 0;
		payload.compositeAttemptGeneration = 0;
		payload.preflightAtlasTextureEpoch = 0;
		payload.preflightScaledFillSampling = false;
		payload.preflightAlphaBlending = false;
		payload.useCompositePackets = false;
		payload.compositeUnavailable = false;
		payload.vanillaLikeBitmapPackets = sealed
			? payload.payloadTemplate->validationSeal.vanillaLikeBitmapPackets
			: UsesOnlyVanillaLikeBitmapPackets(
				payload.payloadTemplate->packets);
		payload.packetPrepareFailure.store(
			NativeFontPacketPrepareFailure::None, std::memory_order_relaxed);
		payload.stickyReason.store(
			NativeFontFallbackReason::None, std::memory_order_relaxed);
		payload.suppressNextSubmit.store(false, std::memory_order_relaxed);
		payload.buildComplete = true;
		return true;
	}

	size_t GetNativeFontPayloadTemplateBytes(
		const NativeFontPayloadTemplate& payloadTemplate)
	{
		size_t bytes = sizeof(payloadTemplate)
			+ payloadTemplate.atlasProperties.capacity()
				* sizeof(NiTexturingPropertyPtr)
			+ payloadTemplate.atlasTextures.capacity() * sizeof(NiTexturePtr)
			+ payloadTemplate.packets.capacity() * sizeof(NativeFontPacketTemplate)
			+ payloadTemplate.compositePackets.capacity()
				* sizeof(NativeFontPacketTemplate)
			+ payloadTemplate.gpuVertices.capacity() * sizeof(NativeFontGpuVertex);
		return bytes;
	}

	void InvalidateNativeFontRingResources(NativeFontFallbackReason reason)
	{
		ReleaseNativeFontRingResources();
		std::vector<NativeFontShapeMetadataPtr> metadataEntries;
		{
			std::lock_guard<std::mutex> lock(State().metadataMutex);
			for (const auto& entry : State().shapeMetadata)
			{
				const NativeFontShapeMetadataPtr& metadata =
					entry.second.metadata;
				if (!metadata || !metadata->nativePayload.buildComplete)
					continue;
				metadataEntries.push_back(metadata);
			}
		}

		for (const NativeFontShapeMetadataPtr& metadata : metadataEntries)
		{
			if (!metadata)
				continue;
			NativeFontShapePayload& payload = metadata->nativePayload;
			if (reason == NativeFontFallbackReason::None)
			{
				payload.stickyReason.store(reason, std::memory_order_relaxed);
				payload.suppressNextSubmit.store(false, std::memory_order_release);
			}
			else
			{
				payload.stickyReason.store(reason, std::memory_order_relaxed);
				payload.suppressNextSubmit.store(true, std::memory_order_release);
			}
			payload.packetPrepareFailure.store(
				NativeFontPacketPrepareFailure::None, std::memory_order_relaxed);
			payload.preparedGeneration = 0;
			payload.preflightAtlasTextureEpoch = 0;
			InvalidateNativeFontTileRetainedText(payload);
			std::fill(payload.preflightAtlasTextures.begin(),
				payload.preflightAtlasTextures.end(), nullptr);
			std::fill(payload.packetShaders.begin(),
				payload.packetShaders.end(), nullptr);
			std::fill(payload.packetPrograms.begin(),
				payload.packetPrograms.end(), nullptr);
		}
	}
}

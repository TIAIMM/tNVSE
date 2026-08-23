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

		struct PayloadVertexValidationWitness
		{
			UInt32 firstVertex = 0;
			UInt32 vertexCount = 0;
			bool complete = false;
			bool registrationVerticesValid = false;
			bool vanillaLayoutVerticesValid = false;
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

		bool HasValidShapeColorContract(
			const NativeFontShapeColorContract& contract)
		{
			const NiColorA& minimum = contract.minimumModifier;
			const NiColorA& maximum = contract.maximumModifier;
			return contract.abiVersion
					== NativeFontShapeColorContract::kTileUniformColorAbi
				&& std::isfinite(minimum.r) && std::isfinite(minimum.g)
				&& std::isfinite(minimum.b) && std::isfinite(minimum.a)
				&& std::isfinite(maximum.r) && std::isfinite(maximum.g)
				&& std::isfinite(maximum.b) && std::isfinite(maximum.a)
				&& minimum.r <= maximum.r && minimum.g <= maximum.g
				&& minimum.b <= maximum.b && minimum.a <= maximum.a;
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
			NativeFontPayloadTemplate& payload,
			const PayloadVertexValidationWitness* constructionWitness)
		{
			if (!payload.pageCount || !payload.quadCount
				|| !payload.sourceRangeCount
				|| payload.quadCount > kNativeFontMaximumArtifactQuads
				|| payload.gpuVertices.size()
					< static_cast<size_t>(payload.quadCount) * 4u
				|| (payload.gpuVertices.size() & 3u)
				|| payload.gpuVertices.size()
					> std::numeric_limits<UInt32>::max()
				|| payload.packets.empty()
				|| payload.packets.size()
					> std::numeric_limits<UInt32>::max()
				|| payload.compositePackets.size()
					> std::numeric_limits<UInt32>::max()
				|| payload.pageCount != payload.atlasProperties.size()
				|| payload.pageCount != payload.atlasTextures.size()
				|| payload.glyphCount > payload.quadCount
				|| !HasValidShapeColorContract(payload.colorContract)
				|| !HasFiniteRegistrationBound(payload.bound))
			{
				return false;
			}

			// Build the optional retail-layout witness inside vertex validation. The
			// common full-span Composite producer may supply the same checks from its
			// already-required profile traversal; every other producer runs the local
			// validation below. Shape creation then consumes the sealed result without
			// another vertex scan.
			const NativeFontPacketTemplate* vanillaLayoutPacket = nullptr;
			bool vanillaLayoutUniformSpread = false;
			bool vanillaLayoutUniformDistanceScale = false;
			if (payload.pageCount == 1
				&& payload.compositePackets.size() == 1)
			{
				const NativeFontPacketTemplate& candidate =
					payload.compositePackets.front();
				const bool supportedDistanceField =
					candidate.distanceFieldMethod
						== DistanceFieldMethod::TrueSdf
					|| candidate.distanceFieldMethod
						== DistanceFieldMethod::Mtsdf;
				const UInt64 vertexEnd =
					static_cast<UInt64>(candidate.firstVertex)
					+ candidate.vertexCount;
				if (candidate.shaderClass
						== NativeFontShaderClass::Composite
					&& supportedDistanceField
					&& candidate.atlasPage == 0
					&& candidate.vertexCount
					&& !(candidate.firstVertex & 3u)
					&& !(candidate.vertexCount & 3u)
					&& vertexEnd <= payload.gpuVertices.size()
					&& candidate.staticCompositeLayerMask >= 8u
					&& candidate.staticCompositeLayerMask <= 15u
					&& std::isfinite(candidate.uniformSdfSpread)
					&& candidate.uniformSdfSpread >= 0.0f
					&& std::isfinite(
						candidate.uniformDistanceParameterScale)
					&& candidate.uniformDistanceParameterScale >= 0.0f
					&& (candidate.uniformDistanceParameterScale == 0.0f
						|| candidate.uniformDistanceParameterScale >= 1.0f))
				{
					vanillaLayoutPacket = &candidate;
					vanillaLayoutUniformSpread =
						candidate.uniformSdfSpread > 0.0f;
					vanillaLayoutUniformDistanceScale =
						candidate.uniformDistanceParameterScale >= 1.0f;
				}
			}

			const bool constructionWitnessCoversPayload = constructionWitness
				&& constructionWitness->complete
				&& constructionWitness->firstVertex == 0
				&& constructionWitness->vertexCount
					== payload.gpuVertices.size()
				&& (!vanillaLayoutPacket
					|| (constructionWitness->firstVertex
							== vanillaLayoutPacket->firstVertex
						&& constructionWitness->vertexCount
							== vanillaLayoutPacket->vertexCount));
			bool vanillaLayoutVerticesValid = vanillaLayoutPacket != nullptr;
			if (constructionWitnessCoversPayload)
			{
				if (!constructionWitness->registrationVerticesValid)
					return false;
				vanillaLayoutVerticesValid = vanillaLayoutPacket
					&& constructionWitness->vanillaLayoutVerticesValid;
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						TextArtifactPayloadValidationVertexScanSaved,
					constructionWitness->vertexCount);
			}
			const size_t vanillaLayoutFirstVertex = vanillaLayoutPacket
				? vanillaLayoutPacket->firstVertex : 0;
			const size_t vanillaLayoutVertexEnd = vanillaLayoutPacket
				? vanillaLayoutFirstVertex + vanillaLayoutPacket->vertexCount : 0;
			for (size_t index = 0;
				!constructionWitnessCoversPayload
					&& index < payload.gpuVertices.size(); ++index)
			{
				const NativeFontGpuVertex& vertex = payload.gpuVertices[index];
				if (!HasValidRegistrationVertex(vertex))
					return false;
				if (!vanillaLayoutVerticesValid
					|| index < vanillaLayoutFirstVertex
					|| index >= vanillaLayoutVertexEnd)
				{
					continue;
				}

				const size_t relative = index - vanillaLayoutFirstVertex;
				const NativeFontGpuVertex& quadFirst = payload.gpuVertices[
					vanillaLayoutFirstVertex + (relative & ~size_t(3u))];
				if (vertex.sdfSpread <= 0.0f
					|| (vanillaLayoutUniformSpread
						&& vertex.sdfSpread
							!= vanillaLayoutPacket->uniformSdfSpread)
					|| (vanillaLayoutUniformDistanceScale
						&& vertex.distanceParameterScale
							!= vanillaLayoutPacket->
								uniformDistanceParameterScale)
					|| vertex.layerMask != static_cast<float>(
						vanillaLayoutPacket->staticCompositeLayerMask)
					|| vertex.sdfSpread != quadFirst.sdfSpread
					|| vertex.distanceParameterScale
						!= quadFirst.distanceParameterScale
					|| vertex.glyphU0 != quadFirst.glyphU0
					|| vertex.glyphV0 != quadFirst.glyphV0
					|| vertex.glyphU1 != quadFirst.glyphU1
					|| vertex.glyphV1 != quadFirst.glyphV1)
				{
					vanillaLayoutVerticesValid = false;
				}
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
			seal.glyphCount = payload.glyphCount;
			seal.colorContractAbi = payload.colorContract.abiVersion;
			seal.vertexCount = static_cast<UInt32>(payload.gpuVertices.size());
			seal.packetCount = static_cast<UInt32>(payload.packets.size());
			seal.compositePacketCount = static_cast<UInt32>(
				payload.compositePackets.size());
			seal.vanillaLikeBitmapPackets =
				UsesOnlyVanillaLikeBitmapPackets(payload.packets);
			if (vanillaLayoutVerticesValid)
			{
				seal.vanillaLayoutKind = vanillaLayoutUniformSpread
					&& vanillaLayoutUniformDistanceScale
					? NativeFontVanillaLayoutKind::Uniform40
					: NativeFontVanillaLayoutKind::Parametric48;
			}
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
			const NativeFontCompiledRange& compiledRange,
			NativeFontShaderClass shaderClass,
			NativeFontSampling sampling, UInt32 firstVertex,
			UInt32 startIndex)
		{
			const UInt64 expectedVertex = static_cast<UInt64>(span.firstVertex)
				+ span.vertexCount;
			const UInt64 expectedIndex = static_cast<UInt64>(span.startIndex)
				+ span.indexCount;
			return expectedVertex == firstVertex
				&& expectedIndex == startIndex
				&& span.shaderClass == shaderClass
				&& span.sampling == sampling
				&& span.layer == compiledRange.drawRange.layer
				&& span.atlasPage == compiledRange.drawRange.atlasPage
				&& span.usesSdf == compiledRange.drawRange.usesSdf
				&& span.staticSmoothSampling == compiledRange.staticSmoothSampling
				&& span.usesLiveTileRgb == compiledRange.drawRange.usesLiveTileRgb
				// c1 now contains the packet layer modifier; it is part of the
				// immutable packet profile just like c2-c4.
				&& std::memcmp(span.constants.data(), compiledRange.constants.data(),
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
				compiled.drawRange = range;
				compiled.drawRange.layer = 3;
				compiled.drawRange.usesSdf = false;
				compiled.drawRange.usesLiveTileRgb = true;
				compiled.drawRange.sdfSpreadPixels = 0.0f;
				compiled.drawRange.sourceToLogicalScale = 1.0f;
				compiled.drawRange.layerColorModifier =
					{ 1.0f, 1.0f, 1.0f, 1.0f };
				compiled.shaderClass = NativeFontCompiledShaderClass::Argb;
				compiled.staticSmoothSampling = true;
				return compiled;
			}
			if (effects.bakedCoverage)
			{
				NativeFontCompiledRange compiled;
				compiled.drawRange = range;
				// Coverage, effect color and effect opacity already live in the A8
				// mask plus COLOR0. Normalize all packet-owned state so contiguous
				// Shadow/Glow/Outline/Fill ranges on one atlas page collapse into a
				// single draw while retaining their original vertex order.
				compiled.drawRange.layer = 3;
				compiled.drawRange.usesSdf = false;
				compiled.drawRange.usesLiveTileRgb = true;
				compiled.drawRange.sdfSpreadPixels = 0.0f;
				compiled.drawRange.sourceToLogicalScale = 1.0f;
				compiled.drawRange.layerColorModifier =
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
			compiled.drawRange = range;
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
				|| effects.atlasProperties.size() != effects.atlasTextures.size()
				|| !vertexCount || (vertexCount & 3u))
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
				UInt32 remainingVertices = range.vertexCount;
				UInt32 nextVertex = range.firstVertex;
				UInt32 nextIndex = range.startIndex;
				while (remainingVertices)
				{
					const bool appendToPrevious = !spans.empty()
						&& spans.back().vertexCount
							< kNativeFontMaximumPacketVertices
						&& SamePacketTarget(spans.back(), compiled,
							shaderClass, sampling, nextVertex, nextIndex);
					const UInt32 availableVertices = appendToPrevious
						? kNativeFontMaximumPacketVertices
							- spans.back().vertexCount
						: kNativeFontMaximumPacketVertices;
					const UInt32 chunkVertices = std::min(
						remainingVertices, availableVertices);
					const UInt32 chunkIndices = chunkVertices / 4u * 6u;
					if (!chunkVertices || (chunkVertices & 3u))
						return false;

					if (appendToPrevious)
					{
						PacketSpan& span = spans.back();
						++span.rangeCount;
						span.vertexCount += chunkVertices;
						span.indexCount += chunkIndices;
						RecordFreeTypePerf(
							FreeTypePerfCounter::MergedPacketRange);
					}
					else
					{
						PacketSpan span;
						span.firstRange = rangeIndex;
						span.rangeCount = 1;
						span.firstVertex = nextVertex;
						span.vertexCount = chunkVertices;
						span.startIndex = nextIndex;
						span.indexCount = chunkIndices;
						span.shaderClass = shaderClass;
						span.sampling = sampling;
						span.layer = range.layer;
						span.atlasPage = range.atlasPage;
						span.usesSdf = range.usesSdf;
						span.staticSmoothSampling =
							compiled.staticSmoothSampling;
						span.usesLiveTileRgb = range.usesLiveTileRgb;
						span.constants = compiled.constants;
						spans.push_back(span);
					}

					remainingVertices -= chunkVertices;
					nextVertex += chunkVertices;
					nextIndex += chunkIndices;
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

		struct CompositeVertexProfile
		{
			UInt8 staticLayerMask = 0;
			float uniformSdfSpread = 0.0f;
			float uniformDistanceParameterScale = 0.0f;
			PayloadVertexValidationWitness validationWitness;
		};

		CompositeVertexProfile ResolveCompositeVertexProfile(
			const std::vector<NativeFontGpuVertex>& vertices,
			const NativeFontCompositeSpan& span,
			bool collectValidationWitness)
		{
			const size_t end = static_cast<size_t>(span.firstVertex)
				+ span.vertexCount;
			if (!span.vertexCount || end > vertices.size())
				return {};

			const NativeFontCompositeConstructionWitness& construction =
				span.constructionWitness;
			const bool constructionProfileWitnessValid = construction.complete
				&& construction.registrationVerticesValid
				&& construction.vanillaLayoutVerticesValid
				&& construction.staticLayerMask >= 1u
				&& construction.staticLayerMask <= 15u
				&& std::isfinite(construction.uniformSdfSpread)
				&& construction.uniformSdfSpread >= 0.0f
				&& std::isfinite(
					construction.uniformDistanceParameterScale)
				&& (construction.uniformDistanceParameterScale == 0.0f
					|| construction.uniformDistanceParameterScale >= 1.0f);
			if (constructionProfileWitnessValid)
			{
				PayloadVertexValidationWitness validationWitness;
				// A partial shifted-shadow span proves its own profile but not the
				// shadow/body compatibility vertices retained elsewhere in the
				// payload. Promote to a payload witness only for exact full coverage.
				if (collectValidationWitness)
				{
					validationWitness.firstVertex = span.firstVertex;
					validationWitness.vertexCount = span.vertexCount;
					validationWitness.complete = true;
					validationWitness.registrationVerticesValid = true;
					validationWitness.vanillaLayoutVerticesValid = true;
				}
				RecordFreeTypePerf(FreeTypePerfCounter::
					DirectCompositeProfileVertexScanSaved,
					span.vertexCount);
				// Retain the existing counter's meaning: the fused profile resolver
				// avoids two additional uniformity scans. The direct witness also
				// avoids the one resolver traversal, recorded separately above.
				RecordFreeTypePerf(FreeTypePerfCounter::
					TextArtifactCompositeProfileVertexScanSaved,
					static_cast<UInt64>(span.vertexCount) * 2u);
				return {
					construction.staticLayerMask,
					construction.uniformSdfSpread,
					construction.uniformDistanceParameterScale,
					validationWitness
				};
			}

			const NativeFontGpuVertex& first = vertices[span.firstVertex];
			const UInt32 firstMask = std::isfinite(first.layerMask)
				? static_cast<UInt32>(first.layerMask) : 0u;
			const bool validFirstMask = firstMask >= 1u && firstMask <= 15u
				&& first.layerMask == static_cast<float>(firstMask);
			bool uniformMask = validFirstMask;
			bool uniformSpread = std::isfinite(first.sdfSpread)
				&& first.sdfSpread > 0.0f;
			bool uniformScale = std::isfinite(first.distanceParameterScale)
				&& first.distanceParameterScale >= 1.0f;
			PayloadVertexValidationWitness validationWitness;
			if (collectValidationWitness)
			{
				validationWitness.firstVertex = span.firstVertex;
				validationWitness.vertexCount = span.vertexCount;
				validationWitness.registrationVerticesValid =
					HasValidRegistrationVertex(first);
				validationWitness.vanillaLayoutVerticesValid =
					validationWitness.registrationVerticesValid
					&& validFirstMask && first.sdfSpread > 0.0f;
			}
			UInt64 visitedVertices = 1;
			for (size_t index = span.firstVertex + 1u; index < end; ++index)
			{
				++visitedVertices;
				const NativeFontGpuVertex& vertex = vertices[index];
				if (uniformMask
					&& vertex.layerMask != static_cast<float>(firstMask))
				{
					uniformMask = false;
				}
				if (uniformSpread && vertex.sdfSpread != first.sdfSpread)
					uniformSpread = false;
				if (uniformScale
					&& vertex.distanceParameterScale
						!= first.distanceParameterScale)
				{
					uniformScale = false;
				}
				if (collectValidationWitness)
				{
					validationWitness.registrationVerticesValid =
						validationWitness.registrationVerticesValid
						&& HasValidRegistrationVertex(vertex);
					const size_t relative = index - span.firstVertex;
					const NativeFontGpuVertex& quadFirst = vertices[
						span.firstVertex + (relative & ~size_t(3u))];
					validationWitness.vanillaLayoutVerticesValid =
						validationWitness.vanillaLayoutVerticesValid
						&& vertex.sdfSpread > 0.0f
						&& vertex.layerMask == static_cast<float>(firstMask)
						&& vertex.sdfSpread == quadFirst.sdfSpread
						&& vertex.distanceParameterScale
							== quadFirst.distanceParameterScale
						&& vertex.glyphU0 == quadFirst.glyphU0
						&& vertex.glyphV0 == quadFirst.glyphV0
						&& vertex.glyphU1 == quadFirst.glyphU1
						&& vertex.glyphV1 == quadFirst.glyphV1;
				}
				if (!collectValidationWitness
					&& !uniformMask && !uniformSpread && !uniformScale)
					break;
			}
			validationWitness.complete = collectValidationWitness
				&& visitedVertices == span.vertexCount;
			RecordFreeTypePerf(
				FreeTypePerfCounter::TextArtifactCompositeProfileVertex,
				visitedVertices);
			if (uniformMask && uniformSpread && uniformScale)
			{
				// The old implementation completed three independent full-span
				// traversals in this common uniform case. The fused traversal above
				// produces the same three witnesses in one pass.
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						TextArtifactCompositeProfileVertexScanSaved,
					static_cast<UInt64>(span.vertexCount) * 2u);
			}
			return {
				uniformMask ? static_cast<UInt8>(firstMask) : 0u,
				uniformSpread ? first.sdfSpread : 0.0f,
				uniformScale ? first.distanceParameterScale : 0.0f,
				validationWitness
			};
		}

	}

	NativeFontPayloadTemplatePtr BuildNativeFontPayloadTemplate(
		std::vector<NativeFontGpuVertex>&& vertices,
		UInt32 quadCount, UInt32 glyphCount,
		const NativeFontShapeColorContract& colorContract,
		const NativeFontEffectShapeConfig& effects, const NiBound& bound,
		std::vector<NativeFontCompositeSpan>&& compositeSpans)
	{
		if (!quadCount || quadCount > kNativeFontMaximumArtifactQuads
			|| glyphCount > quadCount
			|| vertices.size() < static_cast<size_t>(quadCount) * 4u
			|| (vertices.size() & 3u)
			|| vertices.size() > std::numeric_limits<UInt32>::max()
			|| effects.atlasProperties.empty()
			|| effects.atlasProperties.size() != effects.atlasTextures.size()
			|| effects.atlasProperties.size() > std::numeric_limits<UInt32>::max()
			|| static_cast<UInt32>(effects.distanceFieldMethod)
				> static_cast<UInt32>(DistanceFieldMethod::Mtsdf))
		{
			return {};
		}

		std::vector<PacketSpan> spans;
		if (!BuildPacketSpans(effects, quadCount * 4u, spans))
			return {};

		auto payload = std::make_shared<NativeFontPayloadTemplate>();
		payload->pageCount = static_cast<UInt32>(effects.atlasProperties.size());
		payload->quadCount = quadCount;
		payload->sourceRangeCount = static_cast<UInt32>(effects.ranges.size());
		payload->glyphCount = glyphCount;
		payload->colorContract = colorContract;
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
		PayloadVertexValidationWitness constructionWitness;
		bool hasConstructionWitness = false;
		if (effects.shaderEffects && !compositeSpans.empty())
		{
			payload->compositePackets.reserve(compositeSpans.size());
			for (const NativeFontCompositeSpan& sourceSpan : compositeSpans)
			{
				const UInt64 end = static_cast<UInt64>(sourceSpan.firstVertex)
					+ sourceSpan.vertexCount;
				if (!sourceSpan.vertexCount || (sourceSpan.firstVertex & 3u)
					|| (sourceSpan.vertexCount & 3u)
					|| end > payload->gpuVertices.size()
					|| sourceSpan.atlasPage >= payload->pageCount)
				{
					payload->compositePackets.clear();
					break;
				}
				const bool collectValidationWitness =
					compositeSpans.size() == 1u
					&& sourceSpan.firstVertex == 0
					&& sourceSpan.vertexCount == payload->gpuVertices.size();
				const CompositeVertexProfile sourceProfile =
					collectValidationWitness
						? ResolveCompositeVertexProfile(payload->gpuVertices,
							sourceSpan, true)
						: CompositeVertexProfile{};
				if (collectValidationWitness
					&& sourceProfile.validationWitness.complete)
				{
					constructionWitness = sourceProfile.validationWitness;
					hasConstructionWitness = true;
				}

				UInt32 remainingVertices = sourceSpan.vertexCount;
				UInt32 firstVertex = sourceSpan.firstVertex;
				while (remainingVertices)
				{
					NativeFontCompositeSpan packetSpan = sourceSpan;
					packetSpan.firstVertex = firstVertex;
					packetSpan.vertexCount = std::min(
						remainingVertices, kNativeFontMaximumPacketVertices);
					const CompositeVertexProfile packetProfile =
						collectValidationWitness
							? sourceProfile
							: ResolveCompositeVertexProfile(
								payload->gpuVertices, packetSpan, false);
					payload->compositePackets.push_back(
						BuildCompositePacket(effects, packetSpan, bound,
							packetProfile.staticLayerMask,
							packetProfile.uniformSdfSpread,
							packetProfile.uniformDistanceParameterScale));
					remainingVertices -= packetSpan.vertexCount;
					firstVertex += packetSpan.vertexCount;
				}
			}
		}
		if (!SealNativeFontPayloadValidation(*payload,
			hasConstructionWitness ? &constructionWitness : nullptr))
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

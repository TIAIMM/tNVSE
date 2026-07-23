#include "font_a8_internal.h"
#include "font_native_internal.h"

#include "NiBound.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>

namespace fonthook::vectorfont
{
	namespace
	{
		struct PacketSpan
		{
			size_t firstRange = 0;
			size_t rangeCount = 0;
			UInt32 firstVertex = 0;
			UInt32 vertexCount = 0;
			UInt32 startIndex = 0;
			UInt32 indexCount = 0;
			NativeA8ShaderClass shaderClass = NativeA8ShaderClass::Body;
			NativeA8Sampling sampling = NativeA8Sampling::Point;
			UInt32 layer = 3;
			UInt16 atlasPage = 0;
			bool usesSdf = false;
			bool staticSmoothSampling = false;
			bool usesLiveTileRgb = true;
			std::array<float, 16> constants = {};
		};

		bool HasTileProperty(const NiTriShape* shape)
		{
			NiShadeProperty* property = shape ? shape->GetShadeProperty() : nullptr;
			return property && property->m_eShaderType == NiShadeProperty::PROP_Tile;
		}

		bool ResolveNativeShaderClass(A8CompiledShaderClass source,
			NativeA8ShaderClass& result)
		{
			switch (source)
			{
			case A8CompiledShaderClass::Body:
				result = NativeA8ShaderClass::Body;
				return true;
			case A8CompiledShaderClass::Effect:
				result = NativeA8ShaderClass::Effect;
				return true;
			default:
				return false;
			}
		}

		NativeA8Sampling ResolveSampling(const A8CompiledRange&)
		{
			return NativeA8Sampling::LinearLod0;
		}

		bool SamePacketTarget(const PacketSpan& span,
			const A8CompiledRange& range, NativeA8ShaderClass shaderClass,
			NativeA8Sampling sampling)
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
					16 * sizeof(float)) == 0;
		}

		A8CompiledRange CompileRange(const A8EffectShapeConfig& effects,
			const A8DrawRange& range)
		{
			const float distanceParameterScale =
				range.sourceToLogicalScale > 0.0f
				? 1.0f / range.sourceToLogicalScale : 1.0f;
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
				parameter0 = effects.glowInnerPixels * distanceParameterScale;
				parameter1 = effects.glowOuterPixels * distanceParameterScale;
				parameter2 = effects.glowPower;
				parameter3 = effects.outlineWidthPixels * distanceParameterScale;
				sdfFlag1 = effects.outlineSoftnessPixels * distanceParameterScale;
				sdfFlag2 = effects.shadowGlowAlpha;
				sdfFlag3 = effects.shadowOutlineAlpha;
			}
			if (range.layer == 1)
			{
				parameter0 = effects.glowInnerPixels * distanceParameterScale;
				parameter1 = effects.glowOuterPixels * distanceParameterScale;
				parameter2 = effects.glowPower;
			}
			else if (range.layer == 2)
			{
				parameter0 = effects.outlineWidthPixels * distanceParameterScale;
				parameter1 = effects.outlineSoftnessPixels * distanceParameterScale;
			}
			else if (range.layer == 0 && !hardShadowComposite)
			{
				parameter0 = effects.shadowBlurPixels * distanceParameterScale;
			}
			else if (range.layer == 3)
			{
				parameter0 = 0.0f;
				parameter1 = 0.0f;
			}

			A8CompiledRange compiled;
			compiled.range = range;
			const float layerAndFlags = static_cast<float>(range.layer)
				+ (range.usesLiveTileRgb ? 0.0f : 0.25f);
			compiled.constants = {{
				range.layerColorModifier.r, range.layerColorModifier.g,
				range.layerColorModifier.b, range.layerColorModifier.a,
				inverseAtlasWidth, inverseAtlasHeight,
				layerAndFlags, range.sdfSpreadPixels,
				parameter0, parameter1, parameter2, parameter3,
				1.0f, sdfFlag1, sdfFlag2, sdfFlag3
			}};
			compiled.shaderClass = effects.shaderEffects && range.layer != 3
				? A8CompiledShaderClass::Effect : A8CompiledShaderClass::Body;
			compiled.staticSmoothSampling = true;
			return compiled;
		}

		bool BuildPacketSpans(const A8EffectShapeConfig& effects,
			UInt32 vertexCount, std::vector<PacketSpan>& spans)
		{
			spans.clear();
			if (effects.ranges.empty() || effects.atlasProperties.empty()
				|| effects.atlasProperties.size() != effects.atlasTextures.size())
				return false;
			const UInt64 sourceIndexCount = static_cast<UInt64>(vertexCount / 4u) * 6u;
			for (size_t rangeIndex = 0; rangeIndex < effects.ranges.size(); ++rangeIndex)
			{
				const A8DrawRange& range = effects.ranges[rangeIndex];
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
				const A8CompiledRange compiled = CompileRange(effects, range);
				NativeA8ShaderClass shaderClass = NativeA8ShaderClass::Body;
				if (!ResolveNativeShaderClass(compiled.shaderClass, shaderClass))
					return false;
				const NativeA8Sampling sampling = ResolveSampling(compiled);
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
	}

	NativeA8PayloadTemplatePtr BuildNativeA8PayloadTemplate(
		std::vector<NativeA8GpuVertex>&& vertices, UInt32 quadCount,
		const A8EffectShapeConfig& effects, const NiBound& bound,
		NativeA8SubpixelOrder subpixelOrder, float subpixelStrength)
	{
		if (!quadCount || quadCount > kNativeA8MaximumQuads
			|| vertices.size() != static_cast<size_t>(quadCount) * 4u
			|| effects.atlasProperties.empty()
			|| effects.atlasProperties.size() != effects.atlasTextures.size()
			|| effects.atlasProperties.size() > std::numeric_limits<UInt32>::max()
			|| static_cast<UInt32>(effects.distanceFieldMethod)
				> static_cast<UInt32>(DistanceFieldMethod::Mtsdf)
			|| static_cast<UInt32>(subpixelOrder)
				> static_cast<UInt32>(NativeA8SubpixelOrder::BGR)
			|| !std::isfinite(subpixelStrength)
			|| subpixelStrength < 0.0f || subpixelStrength > 1.0f
			|| ((subpixelOrder == NativeA8SubpixelOrder::Disabled)
				!= (subpixelStrength == 0.0f)))
		{
			return {};
		}
		const bool subpixelRendering =
			subpixelOrder != NativeA8SubpixelOrder::Disabled;

		std::vector<PacketSpan> spans;
		if (!BuildPacketSpans(effects, static_cast<UInt32>(vertices.size()), spans))
			return {};

		auto payload = std::make_shared<NativeA8PayloadTemplate>();
		payload->pageCount = static_cast<UInt32>(effects.atlasProperties.size());
		payload->quadCount = quadCount;
		payload->sourceRangeCount = static_cast<UInt32>(effects.ranges.size());
		payload->subpixelOrder = subpixelOrder;
		payload->subpixelStrength = subpixelStrength;
		payload->bound = bound;
		payload->atlasProperties = effects.atlasProperties;
		payload->atlasTextures = effects.atlasTextures;
		payload->gpuVertices = std::move(vertices);
		const size_t fillSpanCount = static_cast<size_t>(std::count_if(
			spans.begin(), spans.end(), [](const PacketSpan& span)
			{
				return span.layer == 3;
			}));
		if (subpixelRendering
			&& fillSpanCount > (std::numeric_limits<size_t>::max()
				- spans.size()) / 2u)
		{
			return {};
		}
		payload->packets.reserve(spans.size()
			+ (subpixelRendering ? fillSpanCount * 2u : 0u));

		for (const PacketSpan& span : spans)
		{
			const UInt32 packetQuadCount = span.vertexCount / 4u;
			if (!packetQuadCount || packetQuadCount > kNativeA8MaximumQuads)
			{
				return {};
			}
			auto appendPacket = [&](NativeA8SubpixelChannel channel,
				float horizontalPixelOffset)
			{
				NativeA8PacketTemplate packet;
				packet.firstVertex = span.firstVertex;
				packet.vertexCount = span.vertexCount;
				packet.bound = bound;
				packet.constants = span.constants;
				packet.shaderClass = span.shaderClass;
				packet.sampling = span.sampling;
				packet.quality = effects.quality;
				packet.distanceFieldMethod = effects.distanceFieldMethod;
				packet.layer = span.layer;
				packet.atlasPage = span.atlasPage;
				packet.staticSmoothSampling = span.staticSmoothSampling;
				packet.usesLiveTileRgb = span.usesLiveTileRgb;
				packet.subpixelChannel = channel;
				if (channel != NativeA8SubpixelChannel::None)
				{
					// c4.x is unused by Fill in the grayscale ABI. The optional
					// subpixel shader interprets it as a horizontal output-pixel
					// offset and c4.y as its center-referenced chroma strength.
					packet.constants[12] = horizontalPixelOffset;
					packet.constants[13] = subpixelStrength;
				}
				payload->packets.push_back(std::move(packet));
			};
			if (subpixelRendering && span.layer == 3)
			{
				const bool bgr =
					subpixelOrder == NativeA8SubpixelOrder::BGR;
				const float redOffset = bgr ? 1.0f / 3.0f : -1.0f / 3.0f;
				const float blueOffset = bgr ? -1.0f / 3.0f : 1.0f / 3.0f;
				appendPacket(NativeA8SubpixelChannel::Red, redOffset);
				appendPacket(NativeA8SubpixelChannel::Green, 0.0f);
				appendPacket(NativeA8SubpixelChannel::Blue, blueOffset);
			}
			else
			{
				appendPacket(NativeA8SubpixelChannel::None, 0.0f);
			}
		}
		payload->cpuMemory.Reset(CpuMemoryCategory::TextArtifact,
			GetNativeA8PayloadTemplateBytes(*payload));
		return payload;
	}

	bool InitializeNativeA8ShapePayload(Font& font,
		NiTriShape* facade, const A8ShapeMetadata& metadata,
		NativeA8PayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin, NativeA8ShapePayload& payload)
	{
		if (!facade || !HasTileProperty(facade) || !payloadTemplate
			|| !payloadTemplate->pageCount || !payloadTemplate->quadCount
			|| payloadTemplate->pageCount != payloadTemplate->atlasProperties.size()
			|| payloadTemplate->pageCount != payloadTemplate->atlasTextures.size()
			|| payloadTemplate->quadCount != metadata.quadCount
			|| payloadTemplate->gpuVertices.empty()
			|| payloadTemplate->packets.empty() || !EnsureNativeA8ProxyPool(font))
		{
			return false;
		}

		for (const NativeA8PacketTemplate& source : payloadTemplate->packets)
		{
			const UInt64 vertexEnd = static_cast<UInt64>(source.firstVertex)
				+ source.vertexCount;
			const bool hasSubpixelChannel =
				source.subpixelChannel != NativeA8SubpixelChannel::None;
			const bool subpixelRendering = payloadTemplate->subpixelOrder
				!= NativeA8SubpixelOrder::Disabled;
			if (!source.vertexCount || (source.vertexCount & 3u)
				|| source.vertexCount / 4u > kNativeA8MaximumQuads
				|| vertexEnd > payloadTemplate->gpuVertices.size()
				|| source.atlasPage >= payloadTemplate->pageCount
				|| static_cast<UInt32>(payloadTemplate->subpixelOrder)
					> static_cast<UInt32>(NativeA8SubpixelOrder::BGR)
				|| !std::isfinite(payloadTemplate->subpixelStrength)
				|| payloadTemplate->subpixelStrength < 0.0f
				|| payloadTemplate->subpixelStrength > 1.0f
				|| (subpixelRendering
					!= (payloadTemplate->subpixelStrength > 0.0f))
				|| (source.layer != 3 && hasSubpixelChannel)
				|| (source.layer == 3
					&& subpixelRendering != hasSubpixelChannel)
				|| (hasSubpixelChannel
					&& source.constants[13]
						!= payloadTemplate->subpixelStrength))
			{
				return false;
			}
		}

		payload.payloadTemplate = std::move(payloadTemplate);
		payload.geometryOrigin = geometryOrigin;
		payload.packetShaders.assign(payload.payloadTemplate->packets.size(), nullptr);
		payload.preflightAtlasTextures.assign(payload.payloadTemplate->pageCount, nullptr);
		payload.preparedGeneration = 0;
		payload.preflightScaledFillSampling = false;
		payload.preflightAlphaBlending = false;
		payload.packetPrepareFailure.store(
			NativeA8PacketPrepareFailure::None, std::memory_order_relaxed);
		payload.stickyReason.store(
			NativeA8FallbackReason::None, std::memory_order_relaxed);
		payload.suppressNextSubmit.store(false, std::memory_order_relaxed);
		payload.buildComplete = true;
		return true;
	}

	size_t GetNativeA8PayloadTemplateBytes(
		const NativeA8PayloadTemplate& payloadTemplate)
	{
		size_t bytes = sizeof(payloadTemplate)
			+ payloadTemplate.atlasProperties.capacity()
				* sizeof(NiTexturingPropertyPtr)
			+ payloadTemplate.atlasTextures.capacity() * sizeof(NiTexturePtr)
			+ payloadTemplate.packets.capacity() * sizeof(NativeA8PacketTemplate)
			+ payloadTemplate.gpuVertices.capacity() * sizeof(NativeA8GpuVertex);
		return bytes;
	}

	void InvalidateNativeA8RingResources(NativeA8FallbackReason reason)
	{
		ReleaseNativeA8RingResources();
		std::vector<A8ShapeMetadataPtr> metadataEntries;
		{
			std::lock_guard<std::mutex> lock(State().metadataMutex);
			for (const auto& entry : State().shapeMetadata)
			{
				const A8ShapeMetadataPtr& metadata = entry.second;
				if (!metadata || !metadata->nativePayload.buildComplete)
					continue;
				metadataEntries.push_back(metadata);
			}
		}

		for (const A8ShapeMetadataPtr& metadata : metadataEntries)
		{
			if (!metadata)
				continue;
			NativeA8ShapePayload& payload = metadata->nativePayload;
			if (reason == NativeA8FallbackReason::None)
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
				NativeA8PacketPrepareFailure::None, std::memory_order_relaxed);
			payload.preparedGeneration = 0;
			std::fill(payload.preflightAtlasTextures.begin(),
				payload.preflightAtlasTextures.end(), nullptr);
			std::fill(payload.packetShaders.begin(),
				payload.packetShaders.end(), nullptr);
		}
	}
}

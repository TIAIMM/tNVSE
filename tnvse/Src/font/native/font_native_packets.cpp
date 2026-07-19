#include "font_a8_internal.h"
#include "font_native_internal.h"

#include "NiBound.hpp"
#include "NiTriShapeData.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <vector>

namespace fonthook::vectorfont
{
	namespace
	{
		UInt32 PackNativeBaseColorChannel(float value)
		{
			if (!std::isfinite(value))
				value = 1.0f;
			value = std::clamp(value, 0.0f, 1.0f);
			return static_cast<UInt32>(value * 255.0f + 0.5f);
		}

		UInt32 PackNativeBaseColor(const NiColorA& color)
		{
			const UInt32 r = PackNativeBaseColorChannel(color.r);
			const UInt32 g = PackNativeBaseColorChannel(color.g);
			const UInt32 b = PackNativeBaseColorChannel(color.b);
			const UInt32 a = PackNativeBaseColorChannel(color.a);
			return (a << 24) | (r << 16) | (g << 8) | b;
		}

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

		bool BuildPacketSpans(const A8ShapeMetadata& metadata,
			const NiTriShapeData& sourceData, std::vector<PacketSpan>& spans)
		{
			spans.clear();
			if (metadata.compiledRanges.empty() || !sourceData.m_pkVertex
				|| !sourceData.m_pkTexture || !sourceData.m_pkColor
				|| !sourceData.m_pusTriList)
			{
				return false;
			}

			const UInt32 sourceIndexCount = sourceData.m_uiTriListLength
				? sourceData.m_uiTriListLength
				: static_cast<UInt32>(sourceData.m_usTriangles) * 3;
			for (size_t rangeIndex = 0;
				rangeIndex < metadata.compiledRanges.size(); ++rangeIndex)
			{
				const A8CompiledRange& compiled = metadata.compiledRanges[rangeIndex];
				const A8DrawRange& range = compiled.range;
				if (!range.vertexCount || !range.primitiveCount
					|| (range.firstVertex & 3u) || (range.vertexCount & 3u)
					|| (range.startIndex % 6u) || (range.primitiveCount & 1u)
					|| range.vertexCount / 4u != range.primitiveCount / 2u)
				{
					return false;
				}

				const UInt64 vertexEnd = static_cast<UInt64>(range.firstVertex)
					+ range.vertexCount;
				const UInt64 indexCount = static_cast<UInt64>(range.primitiveCount) * 3;
				const UInt64 indexEnd = static_cast<UInt64>(range.startIndex) + indexCount;
				if (vertexEnd > sourceData.m_usVertices || indexEnd > sourceIndexCount
					|| indexCount > std::numeric_limits<UInt32>::max())
				{
					return false;
				}

				NativeA8ShaderClass shaderClass = NativeA8ShaderClass::Body;
				if (!ResolveNativeShaderClass(compiled.shaderClass, shaderClass))
					return false;
				const NativeA8Sampling sampling = ResolveSampling(compiled);
				if (!spans.empty()
					&& SamePacketTarget(spans.back(), compiled, shaderClass, sampling))
				{
					PacketSpan& span = spans.back();
					span.rangeCount++;
					span.vertexCount += range.vertexCount;
					span.indexCount += static_cast<UInt32>(indexCount);
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

			return metadata.vertexCount == sourceData.m_usVertices
				&& metadata.primitiveCount == sourceData.m_usTriangles
				&& metadata.indexCount == sourceIndexCount;
		}

		bool FillPacketTemplate(const PacketSpan& span,
			const NiTriShapeData& source,
			const NiPoint3& geometryOrigin, NativeA8PacketTemplate& destination)
		{
			if (static_cast<UInt64>(span.firstVertex) + span.vertexCount
				> source.m_usVertices)
				return false;
			destination.firstVertex = span.firstVertex;
			destination.vertexCount = span.vertexCount;
			std::vector<NiPoint3> boundVertices(span.vertexCount);
			for (UInt32 index = 0; index < span.vertexCount; ++index)
			{
				const NiPoint3& vertex = source.m_pkVertex[span.firstVertex + index];
				const NiPoint3 relative(vertex.x - geometryOrigin.x,
					vertex.y - geometryOrigin.y, vertex.z - geometryOrigin.z);
				boundVertices[index] = relative;
			}

			static constexpr UInt16 kCanonicalQuad[6] = { 0, 2, 1, 0, 3, 2 };
			const UInt32 quadCount = span.vertexCount / 4u;
			const UInt32 packetVertexEnd = span.firstVertex + span.vertexCount;
			if (span.indexCount != quadCount * 6u
				|| quadCount > kNativeA8MaximumQuads)
			{
				return false;
			}
			for (UInt32 quad = 0; quad < quadCount; ++quad)
			{
				for (UInt32 ordinal = 0; ordinal < 6u; ++ordinal)
				{
					const UInt32 sourceIndex = source.m_pusTriList[
						span.startIndex + quad * 6u + ordinal];
					const UInt32 expected = span.firstVertex + quad * 4u
						+ kCanonicalQuad[ordinal];
					if (sourceIndex != expected || sourceIndex >= packetVertexEnd)
						return false;
				}
			}
			ThisStdCall(0xA7EE30, &destination.bound,
				static_cast<UInt16>(boundVertices.size()), boundVertices.data());
			return true;
		}
	}

	NativeA8ShapePayloadPtr BuildNativeA8ShapePayload(Font& font,
		NiTriShape* facade, const A8ShapeMetadata& metadata)
	{
		const NativeA8PayloadTemplatePtr payloadTemplate =
			BuildNativeA8PayloadTemplate(facade, metadata, NiPoint3());
		return payloadTemplate
			? InstantiateNativeA8ShapePayload(font, facade, metadata,
				payloadTemplate, NiPoint3())
			: NativeA8ShapePayloadPtr{};
	}

	NativeA8PayloadTemplatePtr BuildNativeA8PayloadTemplate(
		NiTriShape* facade, const A8ShapeMetadata& metadata,
		const NiPoint3& geometryOrigin)
	{
		NiTriShapeData* sourceData = facade ? facade->GetModelData() : nullptr;
		if (!sourceData || !HasTileProperty(facade) || !metadata.quadCount
			|| metadata.effects.atlasProperties.empty()
			|| metadata.effects.atlasProperties.size()
				!= metadata.effects.atlasTextures.size()
			|| metadata.effects.atlasProperties.size()
				> std::numeric_limits<UInt32>::max())
		{
			return {};
		}

		std::vector<PacketSpan> spans;
		if (!BuildPacketSpans(metadata, *sourceData, spans) || spans.empty())
			return {};

		auto payload = std::make_shared<NativeA8PayloadTemplate>();
		payload->pageCount = static_cast<UInt32>(
			metadata.effects.atlasProperties.size());
		payload->quadCount = metadata.quadCount;
		payload->gpuVertices.resize(sourceData->m_usVertices);
		payload->packets.reserve(spans.size());
		for (UInt32 index = 0; index < sourceData->m_usVertices; ++index)
		{
			const NiPoint3& vertex = sourceData->m_pkVertex[index];
			const NiPoint2& texture = sourceData->m_pkTexture[index];
			const NiColorA& color = sourceData->m_pkColor[index];
			NativeA8GpuVertex& output = payload->gpuVertices[index];
			output.x = vertex.x - geometryOrigin.x;
			output.y = vertex.y - geometryOrigin.y;
			output.z = vertex.z - geometryOrigin.z;
			output.u = texture.x;
			output.v = texture.y;
			output.color = PackNativeBaseColor(color);
		}

		for (const PacketSpan& span : spans)
		{
			const UInt32 packetQuadCount = span.vertexCount / 4u;
			if (!packetQuadCount || packetQuadCount > kNativeA8MaximumQuads)
			{
				return {};
			}
			NativeA8PacketTemplate packet;
			if (!FillPacketTemplate(span, *sourceData, geometryOrigin, packet))
				return {};
			packet.constants = span.constants;
			packet.shaderClass = span.shaderClass;
			packet.sampling = span.sampling;
			packet.quality = metadata.effects.quality;
			packet.layer = span.layer;
			packet.atlasPage = span.atlasPage;
			packet.staticSmoothSampling = span.staticSmoothSampling;
			packet.usesLiveTileRgb = span.usesLiveTileRgb;
			payload->packets.push_back(std::move(packet));
		}
		if (payload->gpuVertices.size() != sourceData->m_usVertices)
			return {};
		payload->cpuMemory.Reset(CpuMemoryCategory::PacketTemplate,
			GetNativeA8PayloadTemplateBytes(*payload));
		return payload;
	}

	NativeA8ShapePayloadPtr InstantiateNativeA8ShapePayload(Font& font,
		NiTriShape* facade, const A8ShapeMetadata& metadata,
		NativeA8PayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin)
	{
		if (!facade || !HasTileProperty(facade) || !payloadTemplate
			|| !payloadTemplate->pageCount || !payloadTemplate->quadCount
			|| payloadTemplate->pageCount != metadata.effects.atlasProperties.size()
			|| payloadTemplate->quadCount != metadata.quadCount
			|| payloadTemplate->gpuVertices.empty()
			|| payloadTemplate->packets.empty() || !EnsureNativeA8ProxyPool(font))
		{
			return {};
		}

		auto payload = std::make_shared<NativeA8ShapePayload>();
		payload->fontId = metadata.fontId;
		payload->pageCount = payloadTemplate->pageCount;
		payload->quadCount = payloadTemplate->quadCount;
		payload->payloadTemplate = std::move(payloadTemplate);
		payload->geometryOrigin = geometryOrigin;
		payload->packets.reserve(payload->payloadTemplate->packets.size());
		for (size_t index = 0;
			index < payload->payloadTemplate->packets.size(); ++index)
		{
			const NativeA8PacketTemplate& source =
				payload->payloadTemplate->packets[index];
			const UInt64 vertexEnd = static_cast<UInt64>(source.firstVertex)
				+ source.vertexCount;
			if (!source.vertexCount || (source.vertexCount & 3u)
				|| source.vertexCount / 4u > kNativeA8MaximumQuads
				|| vertexEnd > payload->payloadTemplate->gpuVertices.size()
				|| index > std::numeric_limits<UInt32>::max())
			{
				return {};
			}

			NativeA8Packet packet;
			packet.templateIndex = static_cast<UInt32>(index);
			packet.constants = source.constants;
			packet.shaderClass = source.shaderClass;
			packet.sampling = source.sampling;
			packet.quality = source.quality;
			packet.layer = source.layer;
			packet.atlasPage = source.atlasPage;
			packet.staticSmoothSampling = source.staticSmoothSampling;
			packet.usesLiveTileRgb = source.usesLiveTileRgb;
			payload->packets.push_back(std::move(packet));
		}

		payload->preparedGeneration = 0;
		payload->buildComplete = true;
		return payload;
	}

	size_t GetNativeA8PayloadTemplateBytes(
		const NativeA8PayloadTemplate& payloadTemplate)
	{
		size_t bytes = sizeof(payloadTemplate)
			+ payloadTemplate.packets.capacity() * sizeof(NativeA8PacketTemplate)
			+ payloadTemplate.gpuVertices.capacity() * sizeof(NativeA8GpuVertex);
		return bytes;
	}

	void InvalidateNativeA8RingResources(NativeA8FallbackReason reason)
	{
		ReleaseNativeA8RingResources();
		std::vector<NativeA8ShapePayloadPtr> payloads;
		std::unordered_set<NativeA8ShapePayload*> seen;
		{
			std::lock_guard<std::mutex> lock(State().metadataMutex);
			for (const auto& entry : State().shapeMetadata)
			{
				const A8ShapeMetadataPtr& metadata = entry.second;
				if (!metadata || !metadata->nativePayload)
					continue;
				NativeA8ShapePayloadPtr payload = metadata->nativePayload;
				if (seen.insert(payload.get()).second)
					payloads.push_back(payload);
			}
		}

		for (const NativeA8ShapePayloadPtr& payload : payloads)
		{
			if (!payload)
				continue;
			if (reason == NativeA8FallbackReason::None)
			{
				payload->stickyReason.store(reason, std::memory_order_relaxed);
				payload->suppressNextSubmit.store(false, std::memory_order_release);
			}
			else
			{
				payload->stickyReason.store(reason, std::memory_order_relaxed);
				payload->suppressNextSubmit.store(true, std::memory_order_release);
			}
			payload->packetPrepareFailure.store(
				NativeA8PacketPrepareFailure::None, std::memory_order_relaxed);
			payload->preparedGeneration = 0;
			payload->preflightAtlasTextures.clear();
			for (NativeA8Packet& packet : payload->packets)
				packet.shader = nullptr;
		}
	}
}

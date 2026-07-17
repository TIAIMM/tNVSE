#include "font_a8_internal.h"
#include "font_native_internal.h"

#include "BSShaderProperty.hpp"
#include "NiBound.hpp"
#include "NiMemory.hpp"
#include "NiPoint4.hpp"
#include "NiTexture.hpp"
#include "NiTexturingProperty.hpp"
#include "NiTriShapeData.hpp"
#include "NiDX9Renderer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <vector>

namespace fonthook::vectorfont
{
	namespace
	{
		inline constexpr UInt32 kScissorTriShapeSize = 0xD4;
		inline constexpr UInt32 kScissorTailOffset = 0xC4;
		inline constexpr UInt32 kScissorTailSize = 0x10;

		// Font::MakeTriShape returns a BSScissorTriShape and a TileShaderProperty,
		// but this CommonLib snapshot does not expose either concrete definition.
		// Keep the two private views here, guarded by the reversed retail layouts.
		struct TileShaderPropertyView : BSShaderProperty
		{
			NiTexturePtr sourceTexture;
			NiTexturePtr alphaTexture;
			NiColorA overlayColor;
			float tileAlpha = 1.0f;
			NiPoint4 textureTransform;
			NiTexturingProperty::ClampMode clampMode =
				NiTexturingProperty::CLAMP_S_CLAMP_T;
			bool byte90 = false;
			bool rotates = false;
			bool hasVertexColors = false;
			bool noTexture = false;
			BSStringT<char> texturePath;
			RECT scissorRect = {};
			bool useScissorTest = false;
		};

		static_assert(sizeof(NiTriShape) == kScissorTailOffset);
		static_assert(kScissorTailOffset + kScissorTailSize == kScissorTriShapeSize);
		static_assert(sizeof(TileShaderPropertyView) == 0xB0);

		struct PacketSpan
		{
			size_t firstRange = 0;
			size_t rangeCount = 0;
			UInt32 firstVertex = 0;
			UInt32 vertexCount = 0;
			UInt32 startIndex = 0;
			UInt32 indexCount = 0;
			NativeA8ShaderClass shaderClass = NativeA8ShaderClass::Coverage;
			NativeA8Sampling sampling = NativeA8Sampling::Point;
			UInt32 layer = 3;
			UInt16 atlasPage = 0;
			bool usesSdf = false;
			bool staticSmoothSampling = false;
			std::array<float, 16> constants = {};
		};

		TileShaderPropertyView* GetTileProperty(NiTriShape* shape)
		{
			NiShadeProperty* property = shape ? shape->GetShadeProperty() : nullptr;
			return property && property->m_eShaderType == NiShadeProperty::PROP_Tile
				? reinterpret_cast<TileShaderPropertyView*>(property) : nullptr;
		}

		const TileShaderPropertyView* GetTileProperty(const NiTriShape* shape)
		{
			return GetTileProperty(const_cast<NiTriShape*>(shape));
		}

		bool ResolveNativeShaderClass(A8CompiledShaderClass source,
			NativeA8ShaderClass& result)
		{
			switch (source)
			{
			case A8CompiledShaderClass::Original:
				result = NativeA8ShaderClass::Original;
				return true;
			case A8CompiledShaderClass::Coverage:
				result = NativeA8ShaderClass::Coverage;
				return true;
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

		NativeA8Sampling ResolveSampling(const A8CompiledRange& range)
		{
			if (range.range.usesSdf)
				return NativeA8Sampling::LinearLod0;
			return range.staticSmoothSampling
				? NativeA8Sampling::LinearMipmapped : NativeA8Sampling::Point;
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
				// c0 contains the old per-range color. Native packets carry it in
				// their FLOAT4 vertex color, so only the remaining immutable constants
				// split a packet.
				&& std::memcmp(span.constants.data() + 4,
					range.constants.data() + 4,
					12 * sizeof(float)) == 0;
		}

		bool BuildPacketSpans(const A8ShapeMetadata& metadata,
			const NiTriShapeData& sourceData, std::vector<PacketSpan>& spans)
		{
			spans.clear();
			if (metadata.compiledRanges.empty() || !sourceData.m_pkVertex
				|| !sourceData.m_pkTexture || !sourceData.m_pusTriList)
			{
				return false;
			}

			const UInt32 sourceIndexCount = sourceData.m_uiTriListLength
				? sourceData.m_uiTriListLength
				: static_cast<UInt32>(sourceData.m_usTriangles) * 3;
			UInt64 nextVertex = 0;
			UInt64 nextIndex = 0;
			UInt64 totalQuads = 0;
			for (size_t rangeIndex = 0;
				rangeIndex < metadata.compiledRanges.size(); ++rangeIndex)
			{
				const A8CompiledRange& compiled = metadata.compiledRanges[rangeIndex];
				const A8DrawRange& range = compiled.range;
				if (!range.vertexCount || !range.primitiveCount
					|| (range.firstVertex & 3u) || (range.vertexCount & 3u)
					|| (range.startIndex % 6u) || (range.primitiveCount & 1u)
					|| range.vertexCount / 4u != range.primitiveCount / 2u
					|| range.firstVertex != nextVertex || range.startIndex != nextIndex)
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

				NativeA8ShaderClass shaderClass = NativeA8ShaderClass::Coverage;
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
					span.constants = compiled.constants;
					// Vertex modifiers replace the old uniform color without quantization.
					span.constants[0] = 1.0f;
					span.constants[1] = 1.0f;
					span.constants[2] = 1.0f;
					span.constants[3] = 1.0f;
					spans.push_back(span);
				}

				nextVertex = vertexEnd;
				nextIndex = indexEnd;
				totalQuads += range.vertexCount / 4u;
			}

			return nextVertex == sourceData.m_usVertices
				&& nextIndex == sourceIndexCount
				&& totalQuads == metadata.quadCount
				&& metadata.vertexCount == sourceData.m_usVertices
				&& metadata.primitiveCount == sourceData.m_usTriangles
				&& metadata.indexCount == sourceIndexCount;
		}

		bool InstallVertexColors(NiTriShapeData& data)
		{
			if (!data.m_usVertices || !data.m_pkTexture || data.m_pkColor
				|| data.m_pkBuffData)
				return false;
			NiColorA* colors = NiAlloc<NiColorA>(data.m_usVertices);
			if (!colors)
				return false;
			data.m_pkColor = colors;
			return true;
		}

		bool FillPacketGeometry(const PacketSpan& span,
			const A8ShapeMetadata& metadata, const NiTriShapeData& source,
			NiTriShapeData& destination)
		{
			if (destination.m_usVertices != span.vertexCount
				|| destination.m_usTriangles * 3u != span.indexCount
				|| destination.m_uiTriListLength != span.indexCount
				|| !destination.m_pkVertex || !destination.m_pkTexture
				|| !destination.m_pkColor
				|| !destination.m_pusTriList)
			{
				return false;
			}

			std::copy_n(source.m_pkVertex + span.firstVertex,
				span.vertexCount, destination.m_pkVertex);
			std::copy_n(source.m_pkTexture + span.firstVertex,
				span.vertexCount, destination.m_pkTexture);
			std::fill_n(destination.m_pkColor, span.vertexCount,
				NiColorA{ 1.0f, 1.0f, 1.0f, 1.0f });

			const UInt32 packetVertexEnd = span.firstVertex + span.vertexCount;
			for (size_t ordinal = 0; ordinal < span.rangeCount; ++ordinal)
			{
				const A8DrawRange& range =
					metadata.compiledRanges[span.firstRange + ordinal].range;
				const UInt32 localFirst = range.firstVertex - span.firstVertex;
				if (range.firstVertex < span.firstVertex
					|| range.firstVertex + range.vertexCount > packetVertexEnd)
				{
					return false;
				}
				const NiColorA& color = range.colorModifier;
				std::fill_n(destination.m_pkColor + localFirst,
					range.vertexCount, color);
			}

			for (UInt32 index = 0; index < span.indexCount; ++index)
			{
				const UInt32 sourceIndex =
					source.m_pusTriList[span.startIndex + index];
				if (sourceIndex < span.firstVertex || sourceIndex >= packetVertexEnd)
					return false;
				destination.m_pusTriList[index] = static_cast<UInt16>(
					sourceIndex - span.firstVertex);
			}

			ThisStdCall(0xA7EE30, &destination.m_kBound,
				destination.m_usVertices, destination.m_pkVertex);
			return true;
		}

		bool BindPacketAtlasPage(NiTriShape& shape,
			const A8ShapeMetadata& metadata, UInt16 page)
		{
			if (page >= metadata.effects.atlasProperties.size()
				|| page >= metadata.effects.atlasTextures.size()
				|| !metadata.effects.atlasProperties[page]
				|| !metadata.effects.atlasTextures[page])
			{
				return false;
			}

			shape.RemoveProperty(NiProperty::TEXTURING);
			shape.AddProperty(metadata.effects.atlasProperties[page]);
			shape.UpdateProperties();
			TileShaderPropertyView* tile = GetTileProperty(&shape);
			if (!tile)
				return false;
			ThisStdCall(0xBB7A10, tile,
				metadata.effects.atlasTextures[page].m_pObject);
			return tile->sourceTexture.m_pObject != nullptr;
		}

		void CopyScissorTail(const NiTriShape& source, NiTriShape& destination)
		{
			std::memcpy(reinterpret_cast<UInt8*>(&destination) + kScissorTailOffset,
				reinterpret_cast<const UInt8*>(&source) + kScissorTailOffset,
				kScissorTailSize);
		}

		void CopyTileDynamicState(const TileShaderPropertyView& source,
			TileShaderPropertyView& destination)
		{
			destination.m_usFlags = source.m_usFlags;
			destination.ulFlags[0] = source.ulFlags[0];
			destination.ulFlags[1] = source.ulFlags[1];
			destination.fAlpha = source.fAlpha;
			destination.fFadeAlpha = source.fFadeAlpha;
			destination.fEnvMapScale = source.fEnvMapScale;
			destination.fLODFade = source.fLODFade;
			destination.fDepthBias = source.fDepthBias;
			destination.uiShaderIndex = source.uiShaderIndex;
			destination.alphaTexture = source.alphaTexture;
			destination.overlayColor = source.overlayColor;
			destination.tileAlpha = source.tileAlpha;
			destination.textureTransform = source.textureTransform;
			destination.clampMode = source.clampMode;
			destination.byte90 = source.byte90;
			destination.rotates = source.rotates;
			destination.hasVertexColors = source.hasVertexColors;
			destination.noTexture = source.noTexture;
			destination.scissorRect = source.scissorRect;
			destination.useScissorTest = source.useScissorTest;
			// sourceTexture and texturePath deliberately remain page-specific.
		}

		bool SyncPacketState(const NiTriShape& facade, NiTriShape& packet)
		{
			const TileShaderPropertyView* sourceTile = GetTileProperty(&facade);
			TileShaderPropertyView* packetTile = GetTileProperty(&packet);
			if (!sourceTile || !packetTile || !packetTile->sourceTexture
				|| !packet.GetTexturingProperty())
			{
				return false;
			}

			packet.m_kLocal = facade.m_kLocal;
			packet.m_kWorld = facade.m_kWorld;
			CopyScissorTail(facade, packet);

			if (facade.m_pWorldBound)
			{
				if (!packet.m_pWorldBound)
					packet.CreateWorldBoundIfMissing();
				if (!packet.m_pWorldBound)
					return false;
				*packet.m_pWorldBound = *facade.m_pWorldBound;
			}
			packet.m_uiFlags = facade.m_uiFlags;

			packet.m_kProperties.m_spAlphaProperty =
				facade.m_kProperties.m_spAlphaProperty;
			packet.m_kProperties.m_spCullingProperty =
				facade.m_kProperties.m_spCullingProperty;
			packet.m_kProperties.m_spMaterialProperty =
				facade.m_kProperties.m_spMaterialProperty;
			packet.m_kProperties.m_spStencilProperty =
				facade.m_kProperties.m_spStencilProperty;
			packet.m_kProperties.m_spUnknownProperty =
				facade.m_kProperties.m_spUnknownProperty;
			CopyTileDynamicState(*sourceTile, *packetTile);
			return true;
		}
	}

	NativeA8ShapePayloadPtr BuildNativeA8ShapePayload(Font& font,
		NiTriShape* facade, const A8ShapeMetadata& metadata)
	{
		NiTriShapeData* sourceData = facade ? facade->GetModelData() : nullptr;
		const TileShaderPropertyView* facadeTile = GetTileProperty(facade);
		if (!sourceData || !facadeTile || !metadata.quadCount
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

		auto payload = std::make_shared<NativeA8ShapePayload>();
		payload->fontId = metadata.fontId;
		payload->pageCount = static_cast<UInt32>(
			metadata.effects.atlasProperties.size());
		payload->quadCount = metadata.quadCount;
		payload->packets.reserve(spans.size());

		for (const PacketSpan& span : spans)
		{
			const UInt32 packetQuadCount = span.vertexCount / 4u;
			if (!packetQuadCount
				|| packetQuadCount > static_cast<UInt32>(std::numeric_limits<int>::max()))
			{
				return {};
			}
			NiTriShapePtr packetShape = font.MakeTriShape(
				static_cast<int>(packetQuadCount), &facadeTile->overlayColor, false);
			NiTriShapeData* packetData = packetShape
				? packetShape->GetModelData() : nullptr;
			if (!packetData || !InstallVertexColors(*packetData)
				|| !FillPacketGeometry(span, metadata, *sourceData, *packetData)
				|| !BindPacketAtlasPage(*packetShape, metadata, span.atlasPage))
			{
				return {};
			}

			NativeA8Packet packet;
			packet.shape = packetShape;
			packet.constants = span.constants;
			packet.shaderClass = span.shaderClass;
			packet.sampling = span.sampling;
			packet.quality = metadata.effects.quality;
			packet.layer = span.layer;
			packet.atlasPage = span.atlasPage;
			packet.usesSdf = span.usesSdf;
			packet.staticSmoothSampling = span.staticSmoothSampling;
			payload->packets.push_back(packet);
		}

		if (!SyncNativeA8PacketState(facade, *payload))
			return {};
		payload->preparedGeneration = 0;
		payload->buildComplete = true;
		return payload;
	}

	bool SyncNativeA8PacketState(NiTriShape* facade,
		NativeA8ShapePayload& payload)
	{
		if (!facade || payload.packets.empty())
			return false;
		for (NativeA8Packet& packet : payload.packets)
		{
			if (!packet.shape || !SyncPacketState(*facade, *packet.shape))
				return false;
		}
		return true;
	}

	bool PurgeNativeA8PacketBuffers(NativeA8ShapePayload& payload)
	{
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		bool buffersPurged = true;
		for (NativeA8Packet& packet : payload.packets)
		{
			NiTriShape* shape = packet.shape.m_pObject;
			NiTriShapeData* data = shape ? shape->GetModelData() : nullptr;
			packet.queuedGeneration = 0;
			packet.queuedViaStock = false;
			if (!data || !data->m_pkBuffData)
				continue;
			if (!renderer || !shape)
			{
				buffersPurged = false;
				continue;
			}
			shape->PurgeRendererData(renderer);
			if (data->m_pkBuffData)
				buffersPurged = false;
		}

		payload.buffersRequirePurge.store(!buffersPurged,
			std::memory_order_release);
		if (buffersPurged)
			payload.preparedGeneration = 0;
		return buffersPurged;
	}

	void InvalidateNativeA8PacketBuffers(NativeA8FallbackReason reason)
	{
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
			payload->blockedReason.store(NativeA8FallbackReason::None,
				std::memory_order_relaxed);
			payload->blockedGeneration.store(0, std::memory_order_release);
			payload->buffersRequirePurge.store(true, std::memory_order_release);
			PurgeNativeA8PacketBuffers(*payload);
		}
	}
}

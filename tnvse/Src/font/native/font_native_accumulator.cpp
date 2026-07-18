#include "font_a8_internal.h"
#include "font_native_internal.h"
#include "load_config.h"
#include "tnvse.h"

#include "BSShaderManager.hpp"
#include "NiDX9TextureData.hpp"
#include "Utils/SafeWrite.h"

#include <atomic>

namespace fonthook::vectorfont
{
	namespace
	{
		inline constexpr UInt32 kBSShaderAccumulatorVtable = 0x10ADFF8;
		inline constexpr UInt32 kRegisterObjectVtableSlot = 38;
		inline constexpr UInt32 kRegisterObjectVtableEntry =
			kBSShaderAccumulatorVtable + kRegisterObjectVtableSlot * sizeof(void*);
		inline constexpr UInt32 kMaximumMissingMetadataLogs = 8;

		using RegisterObjectFn = bool(__thiscall*)(BSShaderAccumulator*, NiGeometry*);

		RegisterObjectFn s_originalRegisterObject = nullptr;
		bool s_hookAttempted = false;
		std::atomic<UInt32> s_missingMetadataLogCount = 0;

		bool IsFreeTypeFacade(const NiGeometry* geometry)
		{
			if (!geometry || !State().originalTriShapeVtable)
				return false;
			void* const* vtable = *reinterpret_cast<void* const* const*>(geometry);
			return vtable == &State().triShapeVtable[1];
		}

		void ClearNativePacketFailure(NativeA8ShapePayload& payload)
		{
			payload.packetPrepareFailure.store(
				NativeA8PacketPrepareFailure::None, std::memory_order_relaxed);
		}

		void InvalidateNativePreflight(NativeA8ShapePayload& payload)
		{
			payload.preparedGeneration = 0;
			payload.preflightAtlasTextures.clear();
			for (NativeA8Packet& packet : payload.packets)
				packet.shader = nullptr;
		}

		bool IsNativePreflightCacheCurrent(const A8ShapeMetadata& metadata,
			const NativeA8ShapePayload& payload, UInt32 generation,
			bool scaledFillSampling, bool alphaBlending)
		{
			if (payload.preparedGeneration != generation
				|| payload.preflightScaledFillSampling != scaledFillSampling
				|| payload.preflightAlphaBlending != alphaBlending
				|| payload.preflightAtlasTextures.size()
					!= metadata.effects.atlasTextures.size())
			{
				return false;
			}

			// Device reset changes the native generation. This page-level identity
			// check additionally catches an atlas wrapper being rebuilt or replaced
			// without paying the old per-packet validation and shader lookup cost.
			for (size_t page = 0; page < payload.preflightAtlasTextures.size(); ++page)
			{
				const void* expected = payload.preflightAtlasTextures[page];
				if (!expected)
					continue;
				NiTexture* texture = metadata.effects.atlasTextures[page].m_pObject;
				NiDX9TextureData* textureData = texture
					? texture->GetDX9RendererData() : nullptr;
				if (!textureData || textureData->GetD3DTexture() != expected)
					return false;
			}
			return true;
		}

		NativeA8FallbackReason PreflightNativeGroupImpl(NiTriShape* facade,
			const A8ShapeMetadata& metadata, NativeA8ShapePayload& payload)
		{
			if (!facade || !payload.buildComplete || !payload.payloadTemplate
				|| payload.packets.empty()
				|| payload.packets.size() != payload.payloadTemplate->packets.size())
			{
				return NativeA8FallbackReason::PacketBuild;
			}
			if (!IsNativeA8AccumulatorHookCurrent())
				return NativeA8FallbackReason::AccumulatorConflict;
			if (!IsA8TileRenderPassHookCurrent())
				return NativeA8FallbackReason::TileRouteConflict;
			if (!IsNativeA8RendererAvailable())
				return NativeA8FallbackReason::ShaderGeneration;

			const UInt32 generation = GetNativeA8ShaderGeneration();
			if (!generation)
				return NativeA8FallbackReason::ShaderGeneration;
			const bool scaledFillSampling = NeedsScaledFillSampling(facade);
			const NiAlphaProperty* alpha = facade->GetAlphaProperty();
			const bool alphaBlending = alpha && alpha->GetAlphaBlending();
			if (IsNativePreflightCacheCurrent(metadata, payload, generation,
				scaledFillSampling, alphaBlending))
			{
				ClearNativePacketFailure(payload);
				return NativeA8FallbackReason::None;
			}

			InvalidateNativePreflight(payload);
			payload.preflightScaledFillSampling = scaledFillSampling;
			payload.preflightAlphaBlending = alphaBlending;
			payload.preflightAtlasTextures.assign(
				metadata.effects.atlasTextures.size(), nullptr);
			std::vector<UInt8> referencedPages(
				metadata.effects.atlasTextures.size(), 0);
			for (NativeA8Packet& packet : payload.packets)
			{
				if (packet.templateIndex >= payload.payloadTemplate->packets.size()
					|| packet.atlasPage >= metadata.effects.atlasTextures.size())
				{
					return NativeA8FallbackReason::AtlasGeneration;
				}
				const NativeA8PacketTemplate& packetTemplate =
					payload.payloadTemplate->packets[packet.templateIndex];
				const UInt64 vertexEnd = static_cast<UInt64>(
					packetTemplate.firstVertex) + packetTemplate.vertexCount;
				if (packetTemplate.atlasPage != packet.atlasPage
					|| !packetTemplate.vertexCount
					|| (packetTemplate.vertexCount & 3u)
					|| vertexEnd > payload.payloadTemplate->gpuVertices.size())
				{
					return NativeA8FallbackReason::PacketBuild;
				}
				referencedPages[packet.atlasPage] = 1;
			}

			for (size_t page = 0; page < referencedPages.size(); ++page)
			{
				if (!referencedPages[page])
					continue;
				NiTexture* texture = metadata.effects.atlasTextures[page].m_pObject;
				NiDX9TextureData* textureData = texture
					? texture->GetDX9RendererData() : nullptr;
				const void* d3dTexture = textureData
					? textureData->GetD3DTexture() : nullptr;
				if (!d3dTexture)
					return NativeA8FallbackReason::PageTexture;
				payload.preflightAtlasTextures[page] = d3dTexture;
			}

			for (NativeA8Packet& packet : payload.packets)
			{
				packet.shader = ResolveNativeA8PacketShader(packet, facade,
					scaledFillSampling);
				if (!packet.shader)
					return NativeA8FallbackReason::ShaderGeneration;
			}

			payload.preparedGeneration = generation;
			ClearNativePacketFailure(payload);
			return NativeA8FallbackReason::None;
		}

		bool SuppressNativeGroup(NiTriShape* facade,
			const A8ShapeMetadata& metadata, NativeA8FallbackReason reason,
			const char* phase)
		{
			RecordNativeA8Suppression(facade, metadata, reason, phase);
			// Match stock's accepted/skipped result while preventing a marked facade
			// from entering a renderer path that cannot interpret native packet data.
			return true;
		}

		bool __fastcall NativeA8RegisterObject(BSShaderAccumulator* accumulator,
			void*, NiGeometry* geometry)
		{
			if (!s_originalRegisterObject || !accumulator || !geometry
				|| accumulator->eRenderMode != BSShaderManager::BSSM_RENDER_TILES
				|| !IsFreeTypeFacade(geometry))
			{
				return s_originalRegisterObject
					? s_originalRegisterObject(accumulator, geometry) : false;
			}

			NiTriShape* facade = static_cast<NiTriShape*>(geometry);
			const A8ShapeMetadataPtr metadata = FindA8ShapeMetadata(facade);
			if (!metadata || !metadata->nativePayload)
			{
				if (metadata)
					return SuppressNativeGroup(facade, *metadata,
						NativeA8FallbackReason::PacketBuild, "register-object");
				if (g_bEnableFreeTypeFontRenderingLog)
				{
					const UInt32 ordinal = s_missingMetadataLogCount.fetch_add(
						1, std::memory_order_relaxed);
					if (ordinal < kMaximumMissingMetadataLogs)
					{
						gLog.FormattedMessage(
							"tnvse_freetype_native: submission-suppressed reason=metadata-missing phase=register-object shape=%p thread=%u",
							facade, GetCurrentThreadId());
					}
					else if (ordinal == kMaximumMissingMetadataLogs)
					{
						gLog.FormattedMessage(
							"tnvse_freetype_native: metadata-missing registration logs capped at %u entries",
							kMaximumMissingMetadataLogs);
					}
				}
				return true;
			}

			// Keep one facade in the stock Tile alpha list. Equal-depth entries are
			// quicksorted unstably, so individually registered packets cannot retain
			// Glow/Shadow/Outline/Fill order. Expand only after stock UI sorting.
			if (!IsA8TileRenderPassHookCurrent())
				return SuppressNativeGroup(facade, *metadata,
					NativeA8FallbackReason::TileRouteConflict, "register-object");
			return s_originalRegisterObject(accumulator, facade);
		}
	}

	NativeA8FallbackReason PrepareNativeA8Group(NiTriShape* facade,
		const A8ShapeMetadata& metadata, NativeA8ShapePayload& payload)
	{
		return PreflightNativeGroupImpl(facade, metadata, payload);
	}

	bool HookNativeA8Accumulator()
	{
		void* current = *reinterpret_cast<void**>(kRegisterObjectVtableEntry);
		if (current == reinterpret_cast<void*>(&NativeA8RegisterObject))
			return s_originalRegisterObject != nullptr;
		if (s_hookAttempted)
			return false;
		s_hookAttempted = true;
		if (!current)
			return false;
		s_originalRegisterObject = reinterpret_cast<RegisterObjectFn>(current);
		SafeWrite32(kRegisterObjectVtableEntry,
			reinterpret_cast<UInt32>(&NativeA8RegisterObject));
		return IsNativeA8AccumulatorHookCurrent();
	}

	bool IsNativeA8AccumulatorHookCurrent()
	{
		return *reinterpret_cast<void**>(kRegisterObjectVtableEntry)
			== reinterpret_cast<void*>(&NativeA8RegisterObject)
			&& s_originalRegisterObject != nullptr;
	}
}

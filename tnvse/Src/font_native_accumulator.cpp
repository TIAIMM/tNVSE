#include "font_a8_internal.h"
#include "font_native_internal.h"

#include "BSShaderManager.hpp"
#include "NiDX9TextureData.hpp"
#include "Utils/SafeWrite.h"


namespace fonthook::vectorfont
{
	namespace
	{
		inline constexpr UInt32 kBSShaderAccumulatorVtable = 0x10ADFF8;
		inline constexpr UInt32 kRegisterObjectVtableSlot = 38;
		inline constexpr UInt32 kRegisterObjectVtableEntry =
			kBSShaderAccumulatorVtable + kRegisterObjectVtableSlot * sizeof(void*);

		using RegisterObjectFn = bool(__thiscall*)(BSShaderAccumulator*, NiGeometry*);

		RegisterObjectFn s_originalRegisterObject = nullptr;
		bool s_hookAttempted = false;

		bool IsFreeTypeFacade(const NiGeometry* geometry)
		{
			if (!geometry || !State().originalTriShapeVtable)
				return false;
			void* const* vtable = *reinterpret_cast<void* const* const*>(geometry);
			return vtable == &State().triShapeVtable[1];
		}

		void ClearNativePacketBlock(NativeA8ShapePayload& payload)
		{
			payload.packetPrepareFailure.store(
				NativeA8PacketPrepareFailure::None, std::memory_order_relaxed);
			payload.blockedReason.store(NativeA8FallbackReason::None,
				std::memory_order_relaxed);
			payload.blockedGeneration.store(0, std::memory_order_release);
		}

		void BlockNativePacketGeneration(NativeA8ShapePayload& payload,
			UInt32 generation, NativeA8FallbackReason reason,
			NativeA8PacketPrepareFailure failure)
		{
			payload.packetPrepareFailure.store(failure,
				std::memory_order_relaxed);
			payload.blockedReason.store(reason, std::memory_order_relaxed);
			payload.blockedGeneration.store(generation,
				std::memory_order_release);
		}

		NativeA8FallbackReason PreflightNativeGroupImpl(NiTriShape* facade,
			const A8ShapeMetadata& metadata, NativeA8ShapePayload& payload)
		{
			if (!payload.buildComplete || payload.packets.empty())
				return NativeA8FallbackReason::PacketBuild;
			if (!IsNativeA8AccumulatorHookCurrent())
				return NativeA8FallbackReason::AccumulatorConflict;
			if (!IsA8TileRenderPassHookCurrent())
				return NativeA8FallbackReason::TileRouteConflict;
			if (!IsNativeA8RendererAvailable())
				return NativeA8FallbackReason::ShaderGeneration;

			const UInt32 generation = GetNativeA8ShaderGeneration();
			if (!generation)
				return NativeA8FallbackReason::ShaderGeneration;
			const UInt32 blockedGeneration = payload.blockedGeneration.load(
				std::memory_order_acquire);
			if (blockedGeneration == generation)
			{
				const NativeA8FallbackReason blockedReason =
					payload.blockedReason.load(std::memory_order_relaxed);
				return blockedReason != NativeA8FallbackReason::None
					? blockedReason : NativeA8FallbackReason::PacketPrepare;
			}
			if (blockedGeneration != 0)
				ClearNativePacketBlock(payload);

			const bool generationChanged = payload.preparedGeneration
				&& payload.preparedGeneration != generation;
			if (generationChanged || payload.buffersRequirePurge.load(
				std::memory_order_acquire))
			{
				payload.buffersRequirePurge.store(true, std::memory_order_release);
				if (!PurgeNativeA8PacketBuffers(payload))
				{
					BlockNativePacketGeneration(payload, generation,
						NativeA8FallbackReason::PacketPrepare,
						NativeA8PacketPrepareFailure::Purge);
					return NativeA8FallbackReason::PacketPrepare;
				}
			}
			if (!SyncNativeA8PacketState(facade, payload))
			{
				BlockNativePacketGeneration(payload, generation,
					NativeA8FallbackReason::PropertySync,
					NativeA8PacketPrepareFailure::None);
				return NativeA8FallbackReason::PropertySync;
			}

			const bool scaledFillSampling = NeedsScaledFillSampling(facade);
			bool preparedPacket = false;
			bool pendingPacket = false;
			for (NativeA8Packet& packet : payload.packets)
			{
				if (!packet.shape || packet.atlasPage >= metadata.effects.atlasTextures.size())
					return NativeA8FallbackReason::AtlasGeneration;
				NiTexture* texture = metadata.effects.atlasTextures[packet.atlasPage];
				NiDX9TextureData* textureData = texture
					? texture->GetDX9RendererData() : nullptr;
				if (!textureData || !textureData->GetD3DTexture())
					return NativeA8FallbackReason::PageTexture;

				TileShader* shader = ResolveNativeA8PacketShader(packet,
					scaledFillSampling);
				if (!shader)
					return NativeA8FallbackReason::ShaderGeneration;
				bool rebuilt = false;
				const NativeA8PacketPrepareResult result =
					PrepareNativeA8PacketBuffer(packet, shader, rebuilt);
				if (result.status == NativeA8PacketPrepareStatus::Failed)
				{
					BlockNativePacketGeneration(payload, generation,
						NativeA8FallbackReason::PacketPrepare, result.failure);
					return NativeA8FallbackReason::PacketPrepare;
				}
				pendingPacket = pendingPacket
					|| result.status == NativeA8PacketPrepareStatus::Pending;
				preparedPacket = preparedPacket || rebuilt;
			}
			if (preparedPacket && !SyncNativeA8PacketState(facade, payload))
			{
				BlockNativePacketGeneration(payload, generation,
					NativeA8FallbackReason::PropertySync,
					NativeA8PacketPrepareFailure::None);
				return NativeA8FallbackReason::PropertySync;
			}

			payload.preparedGeneration = generation;
			ClearNativePacketBlock(payload);
			if (pendingPacket)
				return NativeA8FallbackReason::PacketPending;
			return NativeA8FallbackReason::None;
		}

		bool SubmitBridgeFallback(BSShaderAccumulator* accumulator,
			NiTriShape* facade, const A8ShapeMetadata& metadata,
			NativeA8FallbackReason reason)
		{
			const bool bridgeReady = EnsureA8BridgeFallbackReady();
			RecordNativeA8Fallback(facade, metadata,
				bridgeReady ? reason : NativeA8FallbackReason::BridgeUnavailable,
				bridgeReady);
			// Even if the bridge could not be installed, forwarding the complete facade
			// preserves the stock readable-fill path. Never submit a partial packet set.
			return s_originalRegisterObject
				? s_originalRegisterObject(accumulator, facade) : false;
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
					return SubmitBridgeFallback(accumulator, facade, *metadata,
						NativeA8FallbackReason::PacketBuild);
				return s_originalRegisterObject(accumulator, geometry);
			}

			// Keep one facade in the stock Tile alpha list. Equal-depth entries are
			// quicksorted unstably, so individually registered packets cannot retain
			// Shadow/Glow/Outline/Fill order. Expand only after stock UI sorting.
			if (!IsA8TileRenderPassHookCurrent())
				return SubmitBridgeFallback(accumulator, facade, *metadata,
					NativeA8FallbackReason::TileRouteConflict);
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

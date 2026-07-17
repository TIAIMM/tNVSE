#include "font_a8_internal.h"
#include "font_native_internal.h"
#include "load_config.h"
#include "tnvse.h"

#include "BSShaderManager.hpp"
#include "NiDX9TextureData.hpp"
#include "TESMain.hpp"
#include "Utils/SafeWrite.h"


namespace fonthook::vectorfont
{
	namespace
	{
		inline constexpr UInt32 kBSShaderAccumulatorVtable = 0x10ADFF8;
		inline constexpr UInt32 kRegisterObjectVtableSlot = 38;
		inline constexpr UInt32 kRegisterObjectVtableEntry =
			kBSShaderAccumulatorVtable + kRegisterObjectVtableSlot * sizeof(void*);
		inline constexpr UInt32 kPrecacheGeometryVtableEntry = 0x10EE5A8;
		inline constexpr UInt32 kStockPrecacheGeometry = 0xE73F60;
		inline constexpr UInt32 kStockPerformPrecacheBody = 0xE74125;

		using RegisterObjectFn = bool(__thiscall*)(BSShaderAccumulator*, NiGeometry*);

		RegisterObjectFn s_originalRegisterObject = nullptr;
		bool s_hookAttempted = false;
		std::atomic<UInt32> s_packetCompletionLogCount = 0;
		inline constexpr UInt32 kMaximumPacketLifecycleLogs = 32;
		std::atomic<UInt32> s_missingMetadataLogCount = 0;
		inline constexpr UInt32 kMaximumMissingMetadataLogs = 8;
		thread_local bool s_completingPrecache = false;

		enum class StockPrecacheCompletionResult : UInt8
		{
			Completed,
			RendererUnavailable,
			SignatureUnavailable,
			Reentrant
		};

		struct PacketPreparePass
		{
			NativeA8FallbackReason reason = NativeA8FallbackReason::None;
			NativeA8PacketPrepareFailure failure =
				NativeA8PacketPrepareFailure::None;
			NativeA8PacketPendingStage pendingStage =
				NativeA8PacketPendingStage::None;
			bool pending = false;
			bool rebuilt = false;
		};

		UInt32 GetGameMainThreadId()
		{
			TESMain* main = TESMain::GetSingleton();
			return main ? main->uiMainThreadID : 0;
		}

		bool IsCurrentThreadMainThread()
		{
			const UInt32 mainThreadId = GetGameMainThreadId();
			return mainThreadId && mainThreadId == GetCurrentThreadId();
		}

		template <size_t Size>
		bool MatchesExecutableBytes(UInt32 address,
			const UInt8 (&expected)[Size])
		{
			const UInt8* actual = reinterpret_cast<const UInt8*>(address);
			for (size_t index = 0; index < Size; ++index)
			{
				if (actual[index] != expected[index])
					return false;
			}
			return true;
		}

		bool IsStockPrecacheRouteIntact()
		{
			// FalloutNV 1.4.0.525. E74120's first five bytes may legitimately be
			// detoured by NVTF; E74125 is the unmodified continuation used by NVTF's
			// own stock thunk. Refuse the bypass if either internal body changed.
			static const UInt8 kGeometrySignature[] = {
				0x83, 0xEC, 0x14, 0x55, 0x8B, 0x6C, 0x24, 0x1C,
				0x57, 0x8B, 0xF9
			};
			static const UInt8 kCompletionSignature[] = {
				0x56, 0x8B, 0x35, 0x5C, 0xF0, 0xFD, 0x00, 0x8B,
				0xE9, 0x57
			};
			return MatchesExecutableBytes(kStockPrecacheGeometry,
				kGeometrySignature)
				&& MatchesExecutableBytes(kStockPerformPrecacheBody,
					kCompletionSignature);
		}

		bool IsVirtualPrecacheDetoured()
		{
			return *reinterpret_cast<const UInt32*>(
				kPrecacheGeometryVtableEntry) != kStockPrecacheGeometry;
		}

		__declspec(naked) void __fastcall PerformStockPrecacheBody(
			NiDX9Renderer*, void*)
		{
			__asm
			{
				sub esp, 24h
				push ebx
				push ebp
				push 0E74125h
				retn
			}
		}

		using StockPrecacheBodyFn = void(__fastcall*)(NiDX9Renderer*, void*);
		// Keep the target volatile so LTCG must honor the external x86 call ABI.
		// A direct whole-program call to the naked tail thunk can otherwise be
		// mis-analysed as preserving EAX/ECX/EDX even though the stock body reached
		// through E74125 legitimately clobbers all caller-saved registers.
		StockPrecacheBodyFn volatile s_stockPrecacheBody =
			&PerformStockPrecacheBody;

		__declspec(noinline) void ReleasePrecacheCompletionGuard()
		{
			// Keep this out of the thunk caller so the reset is an explicit constant
			// store, never a reused caller-saved byte register returned by game code.
			s_completingPrecache = false;
		}

		const char* StockPrecacheCompletionResultName(
			StockPrecacheCompletionResult result)
		{
			switch (result)
			{
			case StockPrecacheCompletionResult::Completed:
				return "stock-completed";
			case StockPrecacheCompletionResult::RendererUnavailable:
				return "stock-renderer-unavailable";
			case StockPrecacheCompletionResult::SignatureUnavailable:
				return "stock-signature-unavailable";
			case StockPrecacheCompletionResult::Reentrant:
				return "stock-reentrant";
			default:
				return "stock-completion-unknown";
			}
		}

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

		PacketPreparePass PreparePacketPass(NiTriShape* facade,
			const A8ShapeMetadata& metadata, NativeA8ShapePayload& payload,
			bool useStockPrecache)
		{
			PacketPreparePass pass;
			const bool scaledFillSampling = NeedsScaledFillSampling(facade);
			for (NativeA8Packet& packet : payload.packets)
			{
				if (!packet.shape
					|| packet.atlasPage >= metadata.effects.atlasTextures.size())
				{
					pass.reason = NativeA8FallbackReason::AtlasGeneration;
					return pass;
				}
				NiTexture* texture = metadata.effects.atlasTextures[packet.atlasPage];
				NiDX9TextureData* textureData = texture
					? texture->GetDX9RendererData() : nullptr;
				if (!textureData || !textureData->GetD3DTexture())
				{
					pass.reason = NativeA8FallbackReason::PageTexture;
					return pass;
				}

				TileShader* shader = ResolveNativeA8PacketShader(packet,
					scaledFillSampling);
				if (!shader)
				{
					pass.reason = NativeA8FallbackReason::ShaderGeneration;
					return pass;
				}
				bool rebuilt = false;
				const NativeA8PacketPrepareResult result =
					PrepareNativeA8PacketBuffer(packet, shader, rebuilt,
						useStockPrecache);
				if (result.status == NativeA8PacketPrepareStatus::Failed)
				{
					pass.reason = NativeA8FallbackReason::PacketPrepare;
					pass.failure = result.failure;
					return pass;
				}
				if (result.status == NativeA8PacketPrepareStatus::Pending)
				{
					pass.pending = true;
					// An external owner needs a stock reissue before renderer packing
					// can complete, so retain it as the dominant pending stage.
					if (pass.pendingStage == NativeA8PacketPendingStage::None
						|| result.pendingStage
							== NativeA8PacketPendingStage::ExternalQueue)
					{
						pass.pendingStage = result.pendingStage;
					}
				}
				pass.rebuilt = pass.rebuilt || rebuilt;
			}
			return pass;
		}

		struct PrecacheCompletionScope
		{
			bool active = false;

			PrecacheCompletionScope()
			{
				if (!s_completingPrecache)
				{
					s_completingPrecache = true;
					active = true;
				}
			}

			~PrecacheCompletionScope()
			{
				if (active)
					ReleasePrecacheCompletionGuard();
			}
		};

		bool CompleteRendererPrecachePublic()
		{
			if (!IsCurrentThreadMainThread())
				return false;
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			if (!renderer)
				return false;

			// On the game main thread the installed public owner keeps full control.
			PrecacheCompletionScope scope;
			if (!scope.active)
				return false;
			renderer->PerformPrecache();
			return true;
		}

		StockPrecacheCompletionResult CompleteRendererPrecacheStock()
		{
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			if (!renderer)
				return StockPrecacheCompletionResult::RendererUnavailable;
			if (!IsStockPrecacheRouteIntact())
				return StockPrecacheCompletionResult::SignatureUnavailable;
			PrecacheCompletionScope scope;
			if (!scope.active)
				return StockPrecacheCompletionResult::Reentrant;
			StockPrecacheBodyFn stockPrecacheBody = s_stockPrecacheBody;
			stockPrecacheBody(renderer, nullptr);
			return StockPrecacheCompletionResult::Completed;
		}

		void LogPacketCompletion(const NativeA8ShapePayload& payload,
			const PacketPreparePass& pass, const char* route)
		{
			if (!g_bEnableFreeTypeFontRenderingLog)
				return;
			const UInt32 ordinal = s_packetCompletionLogCount.fetch_add(1,
				std::memory_order_relaxed);
			if (ordinal < kMaximumPacketLifecycleLogs)
			{
				NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
				const UInt32 threadId = GetCurrentThreadId();
				const UInt32 mainThreadId = GetGameMainThreadId();
				void* virtualPrecache = *reinterpret_cast<void* const*>(
					kPrecacheGeometryVtableEntry);
				const char* result = pass.reason != NativeA8FallbackReason::None
					? "failed" : pass.pending ? "pending" : "ready";
				// Log directly so an overflowing deferred diagnostic queue cannot hide
				// which thread and installed virtual owner made the route decision.
				gLog.FormattedMessage(
					"tnvse_freetype_native: packet-completion route=%s result=%s generation=%u frame=%u packets=%u font=%u packetStage=%s thread=%u mainThread=%u isMain=%u virtualPrecache=%p",
					route ? route : "unknown", result,
					GetNativeA8ShaderGeneration(),
					renderer ? renderer->m_uiFrameID : 0,
					static_cast<UInt32>(payload.packets.size()), payload.fontId,
					pass.reason == NativeA8FallbackReason::PacketPrepare
						? NativeA8PacketPrepareFailureName(pass.failure)
						: pass.reason != NativeA8FallbackReason::None
							? NativeA8FallbackReasonName(pass.reason)
							: pass.pending
								? NativeA8PacketPendingStageName(pass.pendingStage)
								: "ready",
					threadId, mainThreadId,
					mainThreadId && threadId == mainThreadId ? 1 : 0,
					virtualPrecache);
			}
			else if (ordinal == kMaximumPacketLifecycleLogs)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: packet-completion further lifecycle logs suppressed");
			}
		}

		NativeA8FallbackReason PreflightNativeGroupImpl(NiTriShape* facade,
			const A8ShapeMetadata& metadata, NativeA8ShapePayload& payload)
		{
			payload.packetPendingStage.store(NativeA8PacketPendingStage::None,
				std::memory_order_relaxed);
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

			auto validatePass = [&](const PacketPreparePass& candidate)
				-> NativeA8FallbackReason
			{
				if (candidate.reason == NativeA8FallbackReason::PacketPrepare)
				{
					BlockNativePacketGeneration(payload, generation, candidate.reason,
						candidate.failure);
					return candidate.reason;
				}
				if (candidate.reason != NativeA8FallbackReason::None)
					return candidate.reason;
				if (candidate.rebuilt && !SyncNativeA8PacketState(facade, payload))
				{
					BlockNativePacketGeneration(payload, generation,
						NativeA8FallbackReason::PropertySync,
						NativeA8PacketPrepareFailure::None);
					return NativeA8FallbackReason::PropertySync;
				}
				return NativeA8FallbackReason::None;
			};

			const bool mainThread = IsCurrentThreadMainThread();
			const bool virtualPrecacheDetoured = IsVirtualPrecacheDetoured();
			const bool stockRouteIntact = IsStockPrecacheRouteIntact();
			// FinishAccumulating_Tiles may execute on the renderer thread rather than
			// TESMain::uiMainThreadID. An asynchronous virtual owner such as NVTF queues
			// that request to a worker which is deliberately paused during rendering.
			// Use the byte-validated stock entry for this tNVSE packet from the outset;
			// on the main thread, keep the installed virtual owner in full control.
			const bool useStockInitial = !mainThread && stockRouteIntact;
			PacketPreparePass pass = PreparePacketPass(facade, metadata, payload,
				useStockInitial);
			NativeA8FallbackReason passReason = validatePass(pass);
			if (passReason != NativeA8FallbackReason::None)
				return passReason;

			if (pass.pending)
			{
				const char* completionRoute = useStockInitial
					? (virtualPrecacheDetoured ? "stock-tile-detour-bypass"
						: "stock-tile-direct")
					: "pending";

				if (mainThread && CompleteRendererPrecachePublic())
				{
					completionRoute = "renderer-public";
					pass = PreparePacketPass(facade, metadata, payload, false);
					passReason = validatePass(pass);
					if (passReason != NativeA8FallbackReason::None)
						return passReason;
				}

				if (pass.pending && stockRouteIntact)
				{
					// A main-thread public owner may have declined its nonblocking lock,
					// or an older packet may still belong only to an external queue. Reissue
					// only such missing packets through stock; packets already queued through
					// stock retain their single request.
					pass = PreparePacketPass(facade, metadata, payload, true);
					passReason = validatePass(pass);
					if (passReason != NativeA8FallbackReason::None)
						return passReason;
					completionRoute = mainThread ? "stock-tile-main"
						: virtualPrecacheDetoured ? "stock-tile-detour-bypass"
						: "stock-tile-direct";

					if (pass.pending)
					{
						const StockPrecacheCompletionResult completionResult =
							CompleteRendererPrecacheStock();
						if (completionResult == StockPrecacheCompletionResult::Completed)
						{
							pass = PreparePacketPass(facade, metadata, payload, false);
							passReason = validatePass(pass);
							if (passReason != NativeA8FallbackReason::None)
								return passReason;
						}
						else
						{
							completionRoute =
								StockPrecacheCompletionResultName(completionResult);
						}
					}
				}
				else if (pass.pending)
				{
					completionRoute = "stock-signature-unavailable";
				}
				LogPacketCompletion(payload, pass, completionRoute);
			}

			payload.preparedGeneration = generation;
			ClearNativePacketBlock(payload);
			payload.packetPendingStage.store(pass.pending ? pass.pendingStage
				: NativeA8PacketPendingStage::None, std::memory_order_release);
			if (pass.pending)
				return NativeA8FallbackReason::PacketPending;
			return NativeA8FallbackReason::None;
		}

		bool SuppressNativeGroup(NiTriShape* facade,
			const A8ShapeMetadata& metadata, NativeA8FallbackReason reason,
			const char* phase)
		{
			RecordNativeA8Suppression(facade, metadata, reason, phase);
			// Match stock's accepted/skipped result while preventing a marked facade
			// from entering a renderer path that cannot interpret native packet metadata.
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
			// Shadow/Glow/Outline/Fill order. Expand only after stock UI sorting.
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

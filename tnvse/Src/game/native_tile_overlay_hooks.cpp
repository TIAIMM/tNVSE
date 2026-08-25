#include "native_tile_overlay_detail.h"

#include "PlayerCharacter.hpp"
#include "nvse/PluginAPI.h"

#include <cstdint>
#include <cstring>

namespace fonthook
{
	namespace implementation::native_tile_overlay {}
	using namespace implementation::native_tile_overlay;

	namespace implementation::native_tile_overlay
	{
		void AdvanceImeHostGeneration()
		{
			const UInt32 previous = OverlayRuntime().imeHostGeneration.fetch_add(
				1,
				std::memory_order_acq_rel);
			if (previous == std::numeric_limits<UInt32>::max())
				OverlayRuntime().imeHostGeneration.store(1, std::memory_order_release);
		}

		constexpr ULONGLONG kLoadingMenuVerboseSlowThresholdMs = 100;
		constexpr ULONGLONG kLoadingMenuForcedSlowThresholdMs = 1000;
		constexpr ULONGLONG kLoadingMenuSlowLogIntervalMs = 2000;
		constexpr ULONGLONG kLoadingTransitionPostTerminalObservationMs = 10000;
		constexpr ULONGLONG kLoadingTransitionMainLoopStaleMs = 2000;
		constexpr ULONGLONG kLoadingTransitionStaleLogIntervalMs = 10000;
		constexpr UInt32 kLoadingMenuIdlePollMask = 3;

		struct LoadingMenuEngineSnapshot
		{
			void* loadingMenu = nullptr;
			UInt8* loadingThread = nullptr;
			void* interfaceManager = nullptr;
			UInt8 pauseRequested = 0;
			UInt8 shutdownRequested = 0;
		};

		ULONGLONG LoadingMenuAgeMs(
			ULONGLONG now, ULONGLONG timestamp) noexcept
		{
			return timestamp && now >= timestamp ? now - timestamp : 0;
		}

		UInt32 LoadingMenuDurationUs(
			ULONGLONG startedAt, ULONGLONG completedAt) noexcept
		{
			const ULONGLONG durationMs = LoadingMenuAgeMs(completedAt, startedAt);
			return static_cast<UInt32>(std::min<ULONGLONG>(
				durationMs * 1000ull,
				std::numeric_limits<UInt32>::max()));
		}

		bool ClaimLoadingMenuLogInterval(
			std::atomic<ULONGLONG>& timestamp,
			ULONGLONG now,
			ULONGLONG intervalMs) noexcept
		{
			ULONGLONG previous = timestamp.load(std::memory_order_acquire);
			for (;;)
			{
				if (previous && LoadingMenuAgeMs(now, previous) < intervalMs)
					return false;
				if (timestamp.compare_exchange_weak(
						previous, now, std::memory_order_acq_rel,
						std::memory_order_acquire))
				{
					return true;
				}
			}
		}

		LoadingMenuEngineSnapshot CaptureLoadingMenuEngineSnapshot() noexcept
		{
			LoadingMenuEngineSnapshot snapshot;
			snapshot.loadingMenu = *reinterpret_cast<void* volatile*>(
				kLoadingMenu_pMe);
			snapshot.loadingThread = *reinterpret_cast<UInt8* volatile*>(
				kLoadingMenuThread_pMe);
			snapshot.interfaceManager = *reinterpret_cast<void* volatile*>(
				kInterfaceManager_pMe);
			if (snapshot.loadingThread
				&& hook_identity::IsAccessibleRegion(
					reinterpret_cast<SIZE_T>(snapshot.loadingThread),
					kLoadingMenuThreadShutdownOffset + 1u,
					false))
			{
				snapshot.pauseRequested = *reinterpret_cast<volatile UInt8*>(
					snapshot.loadingThread
						+ kLoadingMenuThreadPauseRequestedOffset);
				snapshot.shutdownRequested = *reinterpret_cast<volatile UInt8*>(
					snapshot.loadingThread
						+ kLoadingMenuThreadShutdownOffset);
			}
			return snapshot;
		}

		const char* LoadingMenuDiagnosticPhaseName(
			LoadingMenuDiagnosticPhase phase) noexcept
		{
			return phase == LoadingMenuDiagnosticPhase::LoadingTextMakeNode
				? "loading-text-makenode" : "idle";
		}

		const char* LoadingTransitionKindName(
			LoadingTransitionKind kind) noexcept
		{
			switch (kind)
			{
			case LoadingTransitionKind::SaveLoad:
				return "save-load";
			case LoadingTransitionKind::FastTravel:
				return "fast-travel";
			case LoadingTransitionKind::NonSaveLoading:
				return "non-save-loading";
			default:
				return "none";
			}
		}

		const char* LoadingTransitionTerminalReasonName(
			LoadingTransitionTerminalReason reason) noexcept
		{
			switch (reason)
			{
			case LoadingTransitionTerminalReason::SavePostLoad:
				return "save-postload";
			case LoadingTransitionTerminalReason::LoadingMenuClosed:
				return "loading-menu-closed";
			case LoadingTransitionTerminalReason::Superseded:
				return "superseded";
			case LoadingTransitionTerminalReason::Shutdown:
				return "shutdown";
			default:
				return "none";
			}
		}

		const char* LoadingMenuUpdateDiagnosticPhaseName(
			LoadingMenuUpdateDiagnosticPhase phase) noexcept
		{
			switch (phase)
			{
			case LoadingMenuUpdateDiagnosticPhase::Predecessor:
				return "predecessor";
			case LoadingMenuUpdateDiagnosticPhase::PrewarmCommand:
				return "prewarm-command";
			default:
				return "idle";
			}
		}

		const char* NativeLoadingMainThreadStageName(
			NativeLoadingMainThreadStage stage) noexcept
		{
			switch (stage)
			{
			case NativeLoadingMainThreadStage::LoadingDiagnostics:
				return "loading-diagnostics";
			case NativeLoadingMainThreadStage::PrepareConfiguredFonts:
				return "prepare-fonts";
			case NativeLoadingMainThreadStage::NativeRendererMaintenance:
				return "native-renderer";
			case NativeLoadingMainThreadStage::DefaultPoolAtlasMaintenance:
				return "default-pool-atlas";
			case NativeLoadingMainThreadStage::PerformanceReporting:
				return "performance";
			default:
				return "idle";
			}
		}

		const char* DefaultPoolResetDiagnosticPhaseName(
			vectorfont::DefaultPoolResetDiagnosticPhase phase) noexcept
		{
			switch (phase)
			{
			case vectorfont::DefaultPoolResetDiagnosticPhase::
				WaitingForPublications:
				return "wait-publications";
			case vectorfont::DefaultPoolResetDiagnosticPhase::
				ReleasingResources:
				return "release-resources";
			case vectorfont::DefaultPoolResetDiagnosticPhase::
				AwaitingDeviceReset:
				return "await-device-reset";
			case vectorfont::DefaultPoolResetDiagnosticPhase::
				RebuildingResources:
				return "rebuild-resources";
			case vectorfont::DefaultPoolResetDiagnosticPhase::Cancelled:
				return "cancelled";
			case vectorfont::DefaultPoolResetDiagnosticPhase::Shutdown:
				return "shutdown";
			default:
				return "idle";
			}
		}

		const char* NativeRendererResetDiagnosticPhaseName(
			vectorfont::NativeRendererResetDiagnosticPhase phase) noexcept
		{
			switch (phase)
			{
			case vectorfont::NativeRendererResetDiagnosticPhase::
				ReleasingResources:
				return "release-resources";
			case vectorfont::NativeRendererResetDiagnosticPhase::
				AwaitingDeviceReset:
				return "await-device-reset";
			case vectorfont::NativeRendererResetDiagnosticPhase::
				RebuildingResources:
				return "rebuild-resources";
			case vectorfont::NativeRendererResetDiagnosticPhase::Complete:
				return "complete";
			default:
				return "idle";
			}
		}

		UInt32 HashLoadingMenuTileName(const char* value) noexcept
		{
			UInt32 hash = 2166136261u;
			if (!value)
				return hash;
			for (const unsigned char* current =
				reinterpret_cast<const unsigned char*>(value);
				*current;
				++current)
			{
				hash ^= *current;
				hash *= 16777619u;
			}
			return hash;
		}

		void SetLoadingMenuDiagnosticPhase(
			LoadingMenuDiagnosticPhase phase,
			ULONGLONG now = GetTickCount64()) noexcept
		{
			LoadingMenuDiagnosticState& diagnostics =
				OverlayRuntime().loadingMenuDiagnostics;
			diagnostics.phaseEnteredAt.store(now, std::memory_order_release);
			diagnostics.phase.store(phase, std::memory_order_release);
		}

		void LogLoadingMenuDiagnosticSnapshot(const char* event)
		{
			NativeTileOverlayRuntimeState& runtime = OverlayRuntime();
			LoadingMenuDiagnosticState& diagnostics =
				runtime.loadingMenuDiagnostics;
			LoadingTransitionDiagnosticState& transition =
				runtime.loadingTransitionDiagnostics;
			const ULONGLONG now = GetTickCount64();
			const LoadingMenuEngineSnapshot engine =
				CaptureLoadingMenuEngineSnapshot();
			const PrewarmOverlayCommand command =
				runtime.prewarmMailbox.ReadLatest();
			const UInt32 published =
				runtime.prewarmMailbox.PublishedSequence();
			const UInt32 consumed =
				runtime.prewarmMailbox.ConsumedSequence();
			const UInt64 eventSequence = transition.eventSequence.fetch_add(
				1u, std::memory_order_relaxed) + 1u;
			const UInt64 transitionId = transition.activeTraceId.load(
				std::memory_order_acquire);
			const ULONGLONG transitionStartedAt = transition.startedAt.load(
				std::memory_order_acquire);
			const ULONGLONG terminalAt = transition.terminalRequestedAt.load(
				std::memory_order_acquire);
			const ULONGLONG observationAt = transition.observationStartedAt.load(
				std::memory_order_acquire);
			const vectorfont::DirectProfileDiagnosticSnapshot profile =
				vectorfont::GetDirectProfileDiagnosticSnapshot();
			const vectorfont::DefaultPoolAtlasDiagnosticSnapshot defaultPool =
				vectorfont::GetDefaultPoolAtlasDiagnosticSnapshot();
			const vectorfont::NativeRendererResetDiagnosticSnapshot nativeReset =
				vectorfont::GetNativeRendererResetDiagnosticSnapshot();

			gLog.FormattedMessage(
				"tnvse_hang_trace: event=%s eventSeq=%llu transition=%llu kind=%s transitionAgeMs=%llu terminal=%s terminalAgeMs=%llu observationAgeMs=%llu terminalSuccess=%u thread=%u main={seq:%llu,thread:%u,stage:%s,lastAgeMs:%llu} loading={active:%u,observed:%u,enter:%llu,exit:%llu,inFlight:%u,phase:%s,thread:%u,lastEnterAgeMs:%llu,lastExitAgeMs:%llu,lastUs:%u,menu:%p,root:%p,loadingThread:%p,interface:%p,pause:%u,shutdown:%u} save={read:%u,post:%u,pathHash:%08X} travel={fastRef:%p,destRef:%p,fastForm:%08X,destForm:%08X,moving:%u} d3d={defaultReset:%llu,defaultPhase:%s,defaultAgeMs:%llu,epoch:%llu,publications:%u,inProgress:%u,maintenance:%u,nativeReset:%llu,nativePhase:%s,nativeAgeMs:%llu,nativeEpoch:%u,generation:%u,nativeInProgress:%u,recovery:%u} profile={failures:%llu,suppressed:%llu,recoveries:%llu,lastFont:%u,lastStatus:%s,lastAgeMs:%llu,active:%016llX} text={trace:%llu,phase:%s,phaseAgeMs:%llu,calls:%llu,inFlight:%u,tile:%p,nameHash:%08X,fontBits:%08X,lastUs:%u,produced:%u} prewarm={published:%u,consumed:%u,pending:%u,run:%llu,command:%u,action:%u,presentation:graphical-only,progress:%.3f,requested:%u}",
				event ? event : "unspecified",
				static_cast<unsigned long long>(eventSequence),
				static_cast<unsigned long long>(transitionId),
				LoadingTransitionKindName(transition.kind.load(
					std::memory_order_acquire)),
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, transitionStartedAt)),
				LoadingTransitionTerminalReasonName(
					transition.terminalReason.load(std::memory_order_acquire)),
				static_cast<unsigned long long>(LoadingMenuAgeMs(now, terminalAt)),
				static_cast<unsigned long long>(LoadingMenuAgeMs(now, observationAt)),
				transition.terminalSucceeded.load(std::memory_order_acquire),
				GetCurrentThreadId(),
				static_cast<unsigned long long>(
					transition.mainLoopSequence.load(std::memory_order_acquire)),
				transition.mainThreadId.load(std::memory_order_acquire),
				NativeLoadingMainThreadStageName(
					transition.mainThreadStage.load(std::memory_order_acquire)),
				static_cast<unsigned long long>(LoadingMenuAgeMs(now,
					transition.lastMainLoopAt.load(std::memory_order_acquire))),
				transition.loadingMenuActive.load(std::memory_order_acquire),
				transition.loadingMenuObserved.load(std::memory_order_acquire),
				static_cast<unsigned long long>(
					transition.loadingUpdateEnterSequence.load(
						std::memory_order_acquire)),
				static_cast<unsigned long long>(
					transition.loadingUpdateExitSequence.load(
						std::memory_order_acquire)),
				transition.loadingUpdateInFlight.load(std::memory_order_acquire),
				LoadingMenuUpdateDiagnosticPhaseName(
					transition.loadingUpdatePhase.load(std::memory_order_acquire)),
				transition.loadingThreadId.load(std::memory_order_acquire),
				static_cast<unsigned long long>(LoadingMenuAgeMs(now,
					transition.lastLoadingUpdateEnterAt.load(
						std::memory_order_acquire))),
				static_cast<unsigned long long>(LoadingMenuAgeMs(now,
					transition.lastLoadingUpdateExitAt.load(
						std::memory_order_acquire))),
				transition.lastLoadingUpdateDurationUs.load(
					std::memory_order_acquire),
				engine.loadingMenu,
				reinterpret_cast<void*>(diagnostics.observedLoadingMenuRoot.load(
					std::memory_order_acquire)),
				engine.loadingThread, engine.interfaceManager,
				static_cast<UInt32>(engine.pauseRequested),
				static_cast<UInt32>(engine.shutdownRequested),
				transition.saveReadObserved.load(std::memory_order_acquire),
				transition.savePostLoadObserved.load(std::memory_order_acquire),
				transition.savePathHash.load(std::memory_order_acquire),
				reinterpret_cast<void*>(transition.fastTravelRef.load(
					std::memory_order_acquire)),
				reinterpret_cast<void*>(transition.destinationRef.load(
					std::memory_order_acquire)),
				transition.fastTravelFormId.load(std::memory_order_acquire),
				transition.destinationFormId.load(std::memory_order_acquire),
				transition.movingIntoNewSpace.load(std::memory_order_acquire),
				static_cast<unsigned long long>(defaultPool.resetSequence),
				DefaultPoolResetDiagnosticPhaseName(defaultPool.phase),
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, defaultPool.phaseEnteredAt)),
				static_cast<unsigned long long>(defaultPool.deviceEpoch),
				defaultPool.activePublications,
				defaultPool.resetInProgress ? 1u : 0u,
				defaultPool.maintenancePending ? 1u : 0u,
				static_cast<unsigned long long>(nativeReset.resetSequence),
				NativeRendererResetDiagnosticPhaseName(nativeReset.phase),
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, nativeReset.phaseEnteredAt)),
				nativeReset.deviceEpoch, nativeReset.generation,
				nativeReset.inProgress ? 1u : 0u,
				nativeReset.recoveryPending ? 1u : 0u,
				static_cast<unsigned long long>(profile.totalFailures),
				static_cast<unsigned long long>(profile.totalSuppressed),
				static_cast<unsigned long long>(profile.totalRecoveries),
				profile.lastFontId,
				vectorfont::DirectProfileAcquireStatusName(profile.lastStatus),
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, profile.lastFailureAt)),
				static_cast<unsigned long long>(profile.activeFailureSignature),
				static_cast<unsigned long long>(diagnostics.traceId.load(
					std::memory_order_acquire)),
				LoadingMenuDiagnosticPhaseName(diagnostics.phase.load(
					std::memory_order_acquire)),
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, diagnostics.phaseEnteredAt.load(std::memory_order_acquire))),
				static_cast<unsigned long long>(
					diagnostics.loadingTextMakeNodeCalls.load(
						std::memory_order_acquire)),
				diagnostics.loadingTextMakeNodeInFlight.load(
					std::memory_order_acquire),
				reinterpret_cast<void*>(diagnostics.lastLoadingTextTile.load(
					std::memory_order_acquire)),
				diagnostics.lastLoadingTextTileNameHash.load(
					std::memory_order_acquire),
				diagnostics.lastLoadingTextFontTraitBits.load(
					std::memory_order_acquire),
				diagnostics.lastLoadingTextMakeNodeDurationUs.load(
					std::memory_order_acquire),
				diagnostics.lastLoadingTextProducedNode.load(
					std::memory_order_acquire),
				published, consumed, published != consumed ? 1u : 0u,
				static_cast<unsigned long long>(command.runToken),
				command.sequence, static_cast<UInt32>(command.action),
				command.progress,
				runtime.prewarmMailbox.IsPresentationRequested() ? 1u : 0u);
		}

		// LoadingMenu runs on its own engine thread. If it is the only thread still
		// advancing during a black screen, emit a deliberately compact marker that
		// reads atomics only. In particular, do not query engine singletons, take the
		// prewarm mailbox lock, or touch renderer objects from this thread.
		void LogLoadingThreadDiagnosticMarker(const char* event,
			ULONGLONG now)
		{
			LoadingTransitionDiagnosticState& transition =
				OverlayRuntime().loadingTransitionDiagnostics;
			const vectorfont::DefaultPoolAtlasDiagnosticSnapshot defaultPool =
				vectorfont::GetDefaultPoolAtlasDiagnosticSnapshot(false);
			vectorfont::NativeRendererResetDiagnosticSnapshot nativeReset;
			if (g_bEnableFreeTypeNativeAtlas)
			{
				nativeReset =
					vectorfont::GetNativeRendererResetDiagnosticSnapshot();
			}
			const vectorfont::DirectProfileDiagnosticSnapshot profile =
				vectorfont::GetDirectProfileDiagnosticSnapshot();
			gLog.FormattedMessage(
				"tnvse_hang_marker: event=%s transition=%llu kind=%s thread=%u main={seq:%llu,thread:%u,stage:%s,lastAgeMs:%llu} loading={enter:%llu,exit:%llu,inFlight:%u,phase:%s,lastEnterAgeMs:%llu,lastExitAgeMs:%llu,lastUs:%u} d3d={defaultReset:%llu,defaultPhase:%s,defaultAgeMs:%llu,publications:%u,inProgress:%u,nativeReset:%llu,nativePhase:%s,nativeAgeMs:%llu,nativeInProgress:%u,recovery:%u} profile={failures:%llu,suppressed:%llu,lastFont:%u,lastStatus:%s,lastAgeMs:%llu}",
				event ? event : "unspecified",
				static_cast<unsigned long long>(transition.activeTraceId.load(
					std::memory_order_acquire)),
				LoadingTransitionKindName(transition.kind.load(
					std::memory_order_acquire)),
				GetCurrentThreadId(),
				static_cast<unsigned long long>(transition.mainLoopSequence.load(
					std::memory_order_acquire)),
				transition.mainThreadId.load(std::memory_order_acquire),
				NativeLoadingMainThreadStageName(
					transition.mainThreadStage.load(std::memory_order_acquire)),
				static_cast<unsigned long long>(LoadingMenuAgeMs(now,
					transition.lastMainLoopAt.load(std::memory_order_acquire))),
				static_cast<unsigned long long>(
					transition.loadingUpdateEnterSequence.load(
						std::memory_order_acquire)),
				static_cast<unsigned long long>(
					transition.loadingUpdateExitSequence.load(
						std::memory_order_acquire)),
				transition.loadingUpdateInFlight.load(std::memory_order_acquire),
				LoadingMenuUpdateDiagnosticPhaseName(
					transition.loadingUpdatePhase.load(std::memory_order_acquire)),
				static_cast<unsigned long long>(LoadingMenuAgeMs(now,
					transition.lastLoadingUpdateEnterAt.load(
						std::memory_order_acquire))),
				static_cast<unsigned long long>(LoadingMenuAgeMs(now,
					transition.lastLoadingUpdateExitAt.load(
						std::memory_order_acquire))),
				transition.lastLoadingUpdateDurationUs.load(
					std::memory_order_acquire),
				static_cast<unsigned long long>(defaultPool.resetSequence),
				DefaultPoolResetDiagnosticPhaseName(defaultPool.phase),
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, defaultPool.phaseEnteredAt)),
				defaultPool.activePublications,
				defaultPool.resetInProgress ? 1u : 0u,
				static_cast<unsigned long long>(nativeReset.resetSequence),
				NativeRendererResetDiagnosticPhaseName(nativeReset.phase),
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, nativeReset.phaseEnteredAt)),
				nativeReset.inProgress ? 1u : 0u,
				nativeReset.recoveryPending ? 1u : 0u,
				static_cast<unsigned long long>(profile.totalFailures),
				static_cast<unsigned long long>(profile.totalSuppressed),
				profile.lastFontId,
				vectorfont::DirectProfileAcquireStatusName(profile.lastStatus),
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, profile.lastFailureAt)));
		}

		UInt32 HashLoadingTransitionData(
			const void* data, UInt32 dataLength) noexcept
		{
			UInt32 hash = 2166136261u;
			if (!data || !dataLength)
				return 0;
			const unsigned char* bytes = static_cast<const unsigned char*>(data);
			const size_t maximum = std::min<size_t>(dataLength, 1024u);
			for (size_t index = 0; index < maximum && bytes[index]; ++index)
			{
				hash ^= bytes[index];
				hash *= 16777619u;
			}
			return hash;
		}

		UInt32 ReadLoadingTransitionFormId(const TESObjectREFR* reference) noexcept
		{
			if (!reference || !hook_identity::IsAccessibleRegion(
					reinterpret_cast<SIZE_T>(reference), sizeof(TESForm), false))
			{
				return 0;
			}
			return reference->GetFormID();
		}

		bool CaptureLoadingTransitionTravelState(
			LoadingTransitionDiagnosticState& transition) noexcept
		{
			PlayerCharacter* player = PlayerCharacter::GetSingleton();
			PlayerCharacter::PositionRequest* request =
				player ? player->pPositionRequest : nullptr;
			if (!request || !hook_identity::IsAccessibleRegion(
					reinterpret_cast<SIZE_T>(request), sizeof(*request), false))
			{
				transition.movingIntoNewSpace.store(
					player && player->bIsPlayerMovingIntoNewSpace ? 1u : 0u,
					std::memory_order_release);
				return false;
			}

			TESObjectREFR* fastTravel = request->pFastTravelRef;
			TESObjectREFR* destination = request->pDestRef;
			transition.fastTravelRef.store(
				reinterpret_cast<SIZE_T>(fastTravel), std::memory_order_release);
			transition.destinationRef.store(
				reinterpret_cast<SIZE_T>(destination), std::memory_order_release);
			transition.fastTravelFormId.store(
				ReadLoadingTransitionFormId(fastTravel), std::memory_order_release);
			transition.destinationFormId.store(
				ReadLoadingTransitionFormId(destination), std::memory_order_release);
			transition.movingIntoNewSpace.store(
				player && player->bIsPlayerMovingIntoNewSpace ? 1u : 0u,
				std::memory_order_release);
			return fastTravel != nullptr;
		}

		bool HasFastTravelRequestOnMainThread() noexcept
		{
			PlayerCharacter* player = PlayerCharacter::GetSingleton();
			PlayerCharacter::PositionRequest* request =
				player ? player->pPositionRequest : nullptr;
			return request && request->pFastTravelRef;
		}

		void ResetLoadingTransitionPerRunState(
			LoadingTransitionDiagnosticState& transition,
			ULONGLONG now) noexcept
		{
			transition.terminalRequestedAt.store(0, std::memory_order_release);
			transition.observationStartedAt.store(0, std::memory_order_release);
			transition.terminalReason.store(
				LoadingTransitionTerminalReason::None,
				std::memory_order_release);
			transition.terminalSucceeded.store(0, std::memory_order_release);
			transition.nextSnapshotAt.store(now + 1000u,
				std::memory_order_release);
			transition.lastLoadingThreadStaleLogAt.store(
				0, std::memory_order_release);
			transition.mainLoopSequence.store(0, std::memory_order_release);
			transition.lastMainLoopAt.store(now, std::memory_order_release);
			transition.mainThreadId.store(
				GetCurrentThreadId(), std::memory_order_release);
			transition.mainThreadStage.store(
				NativeLoadingMainThreadStage::LoadingDiagnostics,
				std::memory_order_release);
			transition.loadingUpdateEnterSequence.store(
				0, std::memory_order_release);
			transition.loadingUpdateExitSequence.store(
				0, std::memory_order_release);
			transition.lastLoadingUpdateEnterAt.store(
				0, std::memory_order_release);
			transition.lastLoadingUpdateExitAt.store(
				0, std::memory_order_release);
			transition.lastLoadingUpdateDurationUs.store(
				0, std::memory_order_release);
			transition.loadingUpdateInFlight.store(
				0, std::memory_order_release);
			transition.loadingThreadId.store(0, std::memory_order_release);
			transition.loadingUpdatePhase.store(
				LoadingMenuUpdateDiagnosticPhase::Idle,
				std::memory_order_release);
			transition.loadingMenuObserved.store(0, std::memory_order_release);
			transition.saveReadObserved.store(0, std::memory_order_release);
			transition.savePostLoadObserved.store(0, std::memory_order_release);
			transition.fastTravelRef.store(0, std::memory_order_release);
			transition.destinationRef.store(0, std::memory_order_release);
			transition.fastTravelFormId.store(0, std::memory_order_release);
			transition.destinationFormId.store(0, std::memory_order_release);
			transition.movingIntoNewSpace.store(0, std::memory_order_release);
		}

		void FinishLoadingTransition(const char* event,
			LoadingTransitionTerminalReason reason,
			bool succeeded)
		{
			LoadingTransitionDiagnosticState& transition =
				OverlayRuntime().loadingTransitionDiagnostics;
			if (!transition.activeTraceId.load(std::memory_order_acquire))
				return;
			transition.terminalReason.store(reason, std::memory_order_release);
			transition.terminalSucceeded.store(
				succeeded ? 1u : 0u, std::memory_order_release);
			LogLoadingMenuDiagnosticSnapshot(event);
			transition.captureEnabled.store(false, std::memory_order_release);
			transition.activeTraceId.store(0, std::memory_order_release);
			transition.kind.store(
				LoadingTransitionKind::None, std::memory_order_release);
			transition.startedAt.store(0, std::memory_order_release);
			transition.terminalRequestedAt.store(0, std::memory_order_release);
			transition.observationStartedAt.store(0, std::memory_order_release);
			transition.terminalReason.store(
				LoadingTransitionTerminalReason::None,
				std::memory_order_release);
			transition.terminalSucceeded.store(0, std::memory_order_release);
			transition.nextSnapshotAt.store(0, std::memory_order_release);
			transition.loadingMenuObserved.store(0, std::memory_order_release);
			transition.saveReadObserved.store(0, std::memory_order_release);
			transition.savePostLoadObserved.store(0, std::memory_order_release);
			transition.savePathHash.store(0, std::memory_order_release);
			transition.fastTravelRef.store(0, std::memory_order_release);
			transition.destinationRef.store(0, std::memory_order_release);
			transition.fastTravelFormId.store(0, std::memory_order_release);
			transition.destinationFormId.store(0, std::memory_order_release);
			transition.movingIntoNewSpace.store(0, std::memory_order_release);
			transition.mainThreadStage.store(
				NativeLoadingMainThreadStage::Idle,
				std::memory_order_release);
		}

		void BeginLoadingTransition(LoadingTransitionKind kind,
			UInt32 savePathHash, bool loadingMenuActive)
		{
			LoadingTransitionDiagnosticState& transition =
				OverlayRuntime().loadingTransitionDiagnostics;
			if (transition.activeTraceId.load(std::memory_order_acquire))
			{
				FinishLoadingTransition("transition-superseded",
					LoadingTransitionTerminalReason::Superseded, false);
			}

			const ULONGLONG now = GetTickCount64();
			ResetLoadingTransitionPerRunState(transition, now);
			const UInt64 traceId = transition.nextTraceId.fetch_add(
				1u, std::memory_order_acq_rel) + 1u;
			transition.activeTraceId.store(traceId, std::memory_order_release);
			transition.kind.store(kind, std::memory_order_release);
			transition.startedAt.store(now, std::memory_order_release);
			transition.savePathHash.store(savePathHash,
				std::memory_order_release);
			transition.loadingMenuActive.store(
				loadingMenuActive ? 1u : 0u, std::memory_order_release);
			const bool fastTravel = kind != LoadingTransitionKind::SaveLoad
				&& CaptureLoadingTransitionTravelState(transition);
			if (kind != LoadingTransitionKind::SaveLoad)
			{
				transition.kind.store((kind == LoadingTransitionKind::FastTravel
					|| fastTravel)
					? LoadingTransitionKind::FastTravel
					: LoadingTransitionKind::NonSaveLoading,
					std::memory_order_release);
			}
			transition.captureEnabled.store(true, std::memory_order_release);
			LogLoadingMenuDiagnosticSnapshot("transition-begin");
		}

		void RequestLoadingTransitionTerminal(
			LoadingTransitionTerminalReason reason, bool succeeded,
			const char* event)
		{
			LoadingTransitionDiagnosticState& transition =
				OverlayRuntime().loadingTransitionDiagnostics;
			if (!transition.activeTraceId.load(std::memory_order_acquire))
				return;
			ULONGLONG expected = 0;
			const ULONGLONG now = GetTickCount64();
			transition.terminalRequestedAt.compare_exchange_strong(
				expected, now, std::memory_order_acq_rel,
				std::memory_order_acquire);
			if (reason == LoadingTransitionTerminalReason::LoadingMenuClosed)
			{
				ULONGLONG observationExpected = 0;
				transition.observationStartedAt.compare_exchange_strong(
					observationExpected, now, std::memory_order_acq_rel,
					std::memory_order_acquire);
			}
			transition.terminalReason.store(reason, std::memory_order_release);
			transition.terminalSucceeded.store(
				succeeded ? 1u : 0u, std::memory_order_release);
			if (event)
				LogLoadingMenuDiagnosticSnapshot(event);
		}

		void AdvanceLoadingTransitionSnapshotDeadline(
			LoadingTransitionDiagnosticState& transition,
			ULONGLONG now) noexcept
		{
			const ULONGLONG startedAt = transition.startedAt.load(
				std::memory_order_acquire);
			const ULONGLONG age = LoadingMenuAgeMs(now, startedAt);
			ULONGLONG nextAge = 0;
			if (age < 3000u)
				nextAge = 3000u;
			else if (age < 10000u)
				nextAge = 10000u;
			else if (age < 30000u)
				nextAge = 30000u;
			else
			{
				transition.nextSnapshotAt.store(
					now + 30000u, std::memory_order_release);
				return;
			}
			transition.nextSnapshotAt.store(
				startedAt + nextAge, std::memory_order_release);
		}

		bool IsLoadingMenuThreadPauseOrShutdownRequested() noexcept
		{
			const UInt8* loadingThread =
				*reinterpret_cast<UInt8**>(kLoadingMenuThread_pMe);
			if (!loadingThread
				|| !hook_identity::IsAccessibleRegion(
					reinterpret_cast<SIZE_T>(loadingThread),
					kLoadingMenuThreadShutdownOffset + 1u,
					false))
			{
				return false;
			}
			return *reinterpret_cast<const volatile UInt8*>(loadingThread
					+ kLoadingMenuThreadPauseRequestedOffset) != 0
				|| *reinterpret_cast<const volatile UInt8*>(loadingThread
					+ kLoadingMenuThreadShutdownOffset) != 0;
		}

		void __fastcall LoadingMenuUpdateHook(void* loadingMenu, void*)
		{
			LoadingTransitionDiagnosticState& transition =
				OverlayRuntime().loadingTransitionDiagnostics;
			const bool captureRequested = g_bEnableFreeTypeFontRenderingLog
				&& transition.captureEnabled.load(std::memory_order_acquire);
			const UInt64 capturedTraceId = captureRequested
				? transition.activeTraceId.load(std::memory_order_acquire) : 0;
			const bool capture = captureRequested && capturedTraceId != 0;
			ULONGLONG updateStartedAt = 0;
			if (capture)
			{
				updateStartedAt = GetTickCount64();
				transition.loadingThreadId.store(
					GetCurrentThreadId(), std::memory_order_release);
				transition.lastLoadingUpdateEnterAt.store(
					updateStartedAt, std::memory_order_release);
				transition.loadingUpdateEnterSequence.fetch_add(
					1u, std::memory_order_relaxed);
				transition.loadingUpdatePhase.store(
					LoadingMenuUpdateDiagnosticPhase::Predecessor,
					std::memory_order_release);
				transition.loadingUpdateInFlight.store(
					1u, std::memory_order_release);
				LoadingMenuDiagnosticState& diagnostics =
					OverlayRuntime().loadingMenuDiagnostics;
				diagnostics.observedLoadingMenu.store(
					reinterpret_cast<SIZE_T>(loadingMenu),
					std::memory_order_release);
				Menu* menu = static_cast<Menu*>(loadingMenu);
				diagnostics.observedLoadingMenuRoot.store(
					menu ? reinterpret_cast<SIZE_T>(menu->pRootTile) : 0,
					std::memory_order_release);
			}

			// Preserve the complete predecessor chain and vanilla behavior first.
			// ThreadProc invokes vanilla presentation immediately after this call.
			LoadingMenuUpdateFn predecessor =
				OverlayRuntime().predecessorLoadingMenuUpdate;
			if (predecessor)
				predecessor(loadingMenu);
			else
			{
				if (capture)
				{
					transition.loadingUpdatePhase.store(
						LoadingMenuUpdateDiagnosticPhase::Idle,
						std::memory_order_release);
					transition.loadingUpdateInFlight.store(
						0u, std::memory_order_release);
				}
				return;
			}

			const UInt64 activeTraceAfterPredecessor =
				transition.activeTraceId.load(std::memory_order_acquire);
			const bool sameTraceAfterPredecessor = capture
				&& transition.captureEnabled.load(std::memory_order_acquire)
				&& activeTraceAfterPredecessor == capturedTraceId;
			if (sameTraceAfterPredecessor)
			{
				const ULONGLONG completedAt = GetTickCount64();
				const UInt32 durationUs = LoadingMenuDurationUs(
					updateStartedAt, completedAt);
				transition.lastLoadingUpdateDurationUs.store(
					durationUs, std::memory_order_release);
				transition.lastLoadingUpdateExitAt.store(
					completedAt, std::memory_order_release);
				transition.loadingUpdateExitSequence.fetch_add(
					1u, std::memory_order_relaxed);
				transition.loadingUpdatePhase.store(
					LoadingMenuUpdateDiagnosticPhase::Idle,
					std::memory_order_release);
				transition.loadingUpdateInFlight.store(
					0u, std::memory_order_release);

				static std::atomic<ULONGLONG> lastSlowUpdateLogAt{ 0 };
				const ULONGLONG durationMs = LoadingMenuAgeMs(
					completedAt, updateStartedAt);
				if ((durationMs >= kLoadingMenuForcedSlowThresholdMs
						|| (g_bEnableFreeTypeFontRenderingLog
							&& durationMs >= kLoadingMenuVerboseSlowThresholdMs))
					&& ClaimLoadingMenuLogInterval(lastSlowUpdateLogAt,
						completedAt, kLoadingMenuSlowLogIntervalMs))
				{
					LogLoadingThreadDiagnosticMarker(
						"slow-call:loading-update-predecessor", completedAt);
				}

				const ULONGLONG mainLoopAt = transition.lastMainLoopAt.load(
					std::memory_order_acquire);
				if (g_bEnableFreeTypeFontRenderingLog && mainLoopAt
					&& LoadingMenuAgeMs(completedAt, mainLoopAt)
						>= kLoadingTransitionMainLoopStaleMs
					&& ClaimLoadingMenuLogInterval(
						transition.lastLoadingThreadStaleLogAt,
						completedAt, kLoadingTransitionStaleLogIntervalMs))
				{
					LogLoadingThreadDiagnosticMarker(
						"stall:main-loop-stale-observed-by-loading-thread",
						completedAt);
				}
			}
			else if (capture && activeTraceAfterPredecessor == 0)
			{
				// The traced transition ended while the predecessor was running. Do
				// not write its exit data into a newer trace, but clear the orphaned
				// in-flight marker when no successor trace exists.
				transition.loadingUpdatePhase.store(
					LoadingMenuUpdateDiagnosticPhase::Idle,
					std::memory_order_release);
				transition.loadingUpdateInFlight.store(
					0u, std::memory_order_release);
			}

			PrewarmOverlayMailbox& mailbox = OverlayRuntime().prewarmMailbox;
			if (!mailbox.HasPending())
				return;
			if (IsLoadingMenuThreadPauseOrShutdownRequested())
				return;

			const bool capturePrewarmCommand = capture
				&& transition.captureEnabled.load(std::memory_order_acquire)
				&& transition.activeTraceId.load(std::memory_order_acquire)
					== capturedTraceId;
			if (capturePrewarmCommand)
			{
				transition.loadingUpdatePhase.store(
					LoadingMenuUpdateDiagnosticPhase::PrewarmCommand,
					std::memory_order_release);
				transition.loadingUpdateInFlight.store(
					1u, std::memory_order_release);
			}
			ConsumeNativePrewarmOverlayCommand(
				static_cast<Menu*>(loadingMenu));
			if (capturePrewarmCommand
				&& transition.activeTraceId.load(std::memory_order_acquire)
					== capturedTraceId)
			{
				transition.loadingUpdatePhase.store(
					LoadingMenuUpdateDiagnosticPhase::Idle,
					std::memory_order_release);
				transition.loadingUpdateInFlight.store(
					0u, std::memory_order_release);
			}
		}

		bool IsVerifiedLoadingMenuUpdateHook()
		{
			SIZE_T currentTarget = 0;
			const SIZE_T predecessorTarget = reinterpret_cast<SIZE_T>(
				OverlayRuntime().predecessorLoadingMenuUpdate);
			return OverlayRuntime().loadingMenuUpdateHookInstalled
				&& hook_identity::ReadRel32Target(
					kLoadingMenuThreadUpdateCallSite,
					hook_identity::Rel32Opcode::Call,
					currentTarget)
				&& currentTarget == reinterpret_cast<SIZE_T>(
					&LoadingMenuUpdateHook)
				&& predecessorTarget != currentTarget
				&& hook_identity::IsExecutableTarget(predecessorTarget);
		}
		UInt32 __fastcall ImeMenuGetId(Menu*, void*)
		{
			return kImeMenuClass;
		}

		Menu* CreateLocalImeMenu()
		{
			if (!OverlayRuntime().imeMenuVtableInitialized)
				return nullptr;

			void* storage = BSNew(sizeof(Menu));
			if (!storage)
				return nullptr;

			// Menu::Menu is a void constructor; the allocated object is the result.
			ThisStdCall<void>(kMenuConstructor, storage);
			Menu* menu = static_cast<Menu*>(storage);

			*reinterpret_cast<SIZE_T**>(menu) =
				OverlayRuntime().imeMenuVtable.data() + 1;
			// Menu::GetMaxDepth adds this signed field to the root Tile
			// depth. Keep the screen-space overlay above the current menus
			// without making it raise every menu created afterwards.
			menu->unk18 =
				static_cast<UInt32>(kImeMenuDepthContribution);
			return menu;
		}

		void __fastcall PipboyRenderedMenuDrawHook(
			void* renderedMenu,
			void*,
			BSRenderedTexture* currentTexture,
			NiRenderer::ClearFlags clearMode,
			BSRenderedTexture* alternateTexture)
		{
			// The dedicated IME Menu is a normal pMenuRoot child, so the
			// Pip-Boy's rendered-menu pass would otherwise capture it and the
			// later screen-space UI pass would draw the same node a second
			// time. App-cull only for this RTT call, preserving its prior state
			// and restoring it before normal UI composition.
			NiNode* imeNode = nullptr;
			bool wasAppCulled = false;
			if (OverlayRuntime().imeReady.load(std::memory_order_acquire)
				&& OverlayRuntime().state.imeVisible
				&& OverlayRuntime().state.imeRoot)
			{
				imeNode = OverlayRuntime().state.imeRoot->spNiNode;
				if (imeNode)
				{
					wasAppCulled = imeNode->GetAppCulled();
					if (!wasAppCulled)
						imeNode->SetAppCulled(true);
				}
			}

			if (OverlayRuntime().predecessorPipboyDraw)
			{
				OverlayRuntime().predecessorPipboyDraw(renderedMenu, currentTexture,
					clearMode, alternateTexture);
			}

			if (imeNode && !wasAppCulled)
			{
				imeNode->SetAppCulled(false);
				if (!OverlayRuntime().loggedPipboyRttExclusion)
				{
					OverlayRuntime().loggedPipboyRttExclusion = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: excluded IME Menu node=%p from Pip-Boy rendered-menu RTT; screen-space pass remains enabled",
						imeNode);
				}
			}
		}

		bool EnsurePipboyDrawExclusionHook()
		{
			if (OverlayRuntime().pipboyDrawHookInstalled)
			{
				if (!hook_identity::IsAccessibleRegion(
					kFOPipboyManagerDrawVTableEntry, sizeof(SIZE_T), false))
				{
					OverlayRuntime().pipboyDrawHookInstalled = false;
					OverlayRuntime().pipboyDrawHookInstallFailed = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: Pip-Boy RTT exclusion capability revoked; FORenderedMenu::Draw vtable entry became unreadable entry=0x%08X",
						static_cast<UInt32>(kFOPipboyManagerDrawVTableEntry));
					return false;
				}

				const SIZE_T currentTarget =
					*reinterpret_cast<const SIZE_T*>(
						kFOPipboyManagerDrawVTableEntry);
				const SIZE_T adapterTarget = reinterpret_cast<SIZE_T>(
					&PipboyRenderedMenuDrawHook);
				const SIZE_T predecessorTarget = reinterpret_cast<SIZE_T>(
					OverlayRuntime().predecessorPipboyDraw);
				if (currentTarget == adapterTarget
					&& predecessorTarget != adapterTarget
					&& hook_identity::IsExecutableTarget(predecessorTarget))
				{
					return true;
				}
				if (currentTarget == predecessorTarget
					&& hook_identity::IsExecutableTarget(currentTarget))
				{
					// A clean restoration to our predecessor cannot retain a chain
					// through this hook. It is safe to publish it again below.
					OverlayRuntime().pipboyDrawHookInstalled = false;
					OverlayRuntime().predecessorPipboyDraw = nullptr;
				}
				else
				{
					// A different owner may have captured this hook. Keep the saved
					// predecessor callable, but do not claim verified reachability.
					OverlayRuntime().pipboyDrawHookInstalled = false;
					OverlayRuntime().pipboyDrawHookInstallFailed = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: Pip-Boy RTT exclusion capability revoked; observed handler=0x%08X adapter=0x%08X predecessor=0x%08X",
						static_cast<UInt32>(currentTarget),
						static_cast<UInt32>(adapterTarget),
						static_cast<UInt32>(predecessorTarget));
					return false;
				}
			}
			if (OverlayRuntime().pipboyDrawHookInstallFailed)
				return false;

			if (!hook_identity::IsAccessibleRegion(
				kFOPipboyManagerDrawVTableEntry, sizeof(SIZE_T), false))
			{
				OverlayRuntime().pipboyDrawHookInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: cannot install Pip-Boy RTT exclusion hook; FORenderedMenu::Draw vtable entry is unreadable entry=0x%08X",
					static_cast<UInt32>(kFOPipboyManagerDrawVTableEntry));
				return false;
			}

			const SIZE_T currentTarget =
				*reinterpret_cast<const SIZE_T*>(
					kFOPipboyManagerDrawVTableEntry);
			if (currentTarget == reinterpret_cast<SIZE_T>(
					&PipboyRenderedMenuDrawHook))
			{
				const SIZE_T predecessorTarget = reinterpret_cast<SIZE_T>(
					OverlayRuntime().predecessorPipboyDraw);
				OverlayRuntime().pipboyDrawHookInstalled =
					predecessorTarget != currentTarget
					&& hook_identity::IsExecutableTarget(predecessorTarget);
				if (!OverlayRuntime().pipboyDrawHookInstalled)
				{
					OverlayRuntime().pipboyDrawHookInstallFailed = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: Pip-Boy RTT exclusion hook is present but its predecessor is unavailable predecessor=0x%08X",
						static_cast<UInt32>(predecessorTarget));
				}
				return OverlayRuntime().pipboyDrawHookInstalled;
			}
			if (!hook_identity::IsExecutableTarget(currentTarget))
			{
				OverlayRuntime().pipboyDrawHookInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: cannot install Pip-Boy RTT exclusion hook; non-executable FORenderedMenu::Draw target=0x%08X entry=0x%08X",
					static_cast<UInt32>(currentTarget),
					static_cast<UInt32>(kFOPipboyManagerDrawVTableEntry));
				return false;
			}

			const RenderedMenuDrawFn previousPredecessor =
				OverlayRuntime().predecessorPipboyDraw;
			OverlayRuntime().predecessorPipboyDraw =
				reinterpret_cast<RenderedMenuDrawFn>(currentTarget);
			const SIZE_T adapterTarget = reinterpret_cast<SIZE_T>(
				&PipboyRenderedMenuDrawHook);
			// FOPipboyManager::Draw vtable slot
			// (__thiscall target via __fastcall shim).
			const SafeWrite32IfEqualResult publication =
				SafeWrite32IfEqualDetailed(kFOPipboyManagerDrawVTableEntry,
					adapterTarget, currentTarget);
			const bool published = publication.WasPublished();
			if (!published)
			{
				const SIZE_T observedTarget = publication.comparisonPerformed
					? publication.observed
					: *reinterpret_cast<const SIZE_T*>(
						kFOPipboyManagerDrawVTableEntry);
				OverlayRuntime().predecessorPipboyDraw = previousPredecessor;
				OverlayRuntime().pipboyDrawHookInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: Pip-Boy FORenderedMenu::Draw CAS did not publish entry=0x%08X predecessor=0x%08X observed=0x%08X compared=%u protectionError=%lu",
					static_cast<UInt32>(kFOPipboyManagerDrawVTableEntry),
					static_cast<UInt32>(currentTarget),
					static_cast<UInt32>(observedTarget),
					publication.comparisonPerformed ? 1u : 0u,
					publication.protectionError);
				return false;
			}
			if (!publication.PostconditionsComplete())
			{
				gLog.FormattedMessage(
					"tnvse_native_overlay: Pip-Boy draw hook published with incomplete write postconditions protectionRestored=%u protectionError=%lu cacheFlushed=%u cacheError=%lu",
					publication.protectionRestored ? 1u : 0u,
					publication.protectionError,
					publication.instructionCacheFlushed ? 1u : 0u,
					publication.cacheFlushError);
			}
			const SIZE_T observedTarget =
				*reinterpret_cast<const SIZE_T*>(
					kFOPipboyManagerDrawVTableEntry);
			if (observedTarget == adapterTarget)
			{
				OverlayRuntime().pipboyDrawHookInstalled = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: installed Pip-Boy RTT exclusion hook chainedTarget=0x%08X vanilla=%d",
					static_cast<UInt32>(currentTarget),
					currentTarget == kFORenderedMenuDraw ? 1 : 0);
				return true;
			}

			if (observedTarget == currentTarget)
			{
				OverlayRuntime().predecessorPipboyDraw = previousPredecessor;
				OverlayRuntime().pipboyDrawHookInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: Pip-Boy FORenderedMenu::Draw hook was published but the slot returned to its predecessor entry=0x%08X predecessor=0x%08X",
					static_cast<UInt32>(kFOPipboyManagerDrawVTableEntry),
					static_cast<UInt32>(currentTarget));
				return false;
			}

			// Preserve a later vtable owner. It may already chain through this
			// hook, so replacing it with our predecessor could strand that chain.
			const bool successorExecutable =
				hook_identity::IsExecutableTarget(observedTarget);
			// Keeping the predecessor is required in case the observed owner
			// captured this hook, but an executable top-level target alone does not
			// prove that it did. Do not publish the RTT exclusion capability unless
			// the vtable slot itself was verified to contain our hook.
			OverlayRuntime().pipboyDrawHookInstalled = false;
			OverlayRuntime().pipboyDrawHookInstallFailed = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: Pip-Boy draw hook may be retained below observed handler=0x%08X predecessor=0x%08X executable=%u; reachability unverified, feature disabled",
				static_cast<UInt32>(observedTarget),
				static_cast<UInt32>(currentTarget),
				successorExecutable ? 1u : 0u);
			return false;
		}

		bool EnsureLocalImeMenuSupport()
		{
			if (!EnsurePipboyDrawExclusionHook())
				return false;
			if (OverlayRuntime().imeMenuVtableInitialized)
				return true;
			if (OverlayRuntime().imeMenuVtableInitializationFailed)
				return false;

			if (!hook_identity::IsExecutableTarget(kMenuConstructor)
				|| !hook_identity::IsAccessibleRegion(
					kMenuVTable - sizeof(SIZE_T),
					(kMenuVTableEntryCount + 1) * sizeof(SIZE_T),
					false))
			{
				OverlayRuntime().imeMenuVtableInitializationFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: cannot initialize local IME Menu; Menu constructor or vtable is unavailable constructor=0x%08X vtable=0x%08X",
					static_cast<UInt32>(kMenuConstructor),
					static_cast<UInt32>(kMenuVTable));
				return false;
			}

			const SIZE_T* vanillaVtable =
				reinterpret_cast<const SIZE_T*>(kMenuVTable);
			if (!std::all_of(
					vanillaVtable,
					vanillaVtable + kMenuVTableEntryCount,
					[](SIZE_T target)
					{
						return hook_identity::IsExecutableTarget(target);
					}))
			{
				OverlayRuntime().imeMenuVtableInitializationFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: cannot initialize local IME Menu; base Menu vtable contains a non-executable target vtable=0x%08X",
					static_cast<UInt32>(kMenuVTable));
				return false;
			}

			OverlayRuntime().imeMenuVtable.front() = vanillaVtable[-1];
			std::copy_n(
				vanillaVtable,
				kMenuVTableEntryCount,
				OverlayRuntime().imeMenuVtable.begin() + 1);
			OverlayRuntime().imeMenuVtable[kMenuGetIdVtableIndex + 1] =
				reinterpret_cast<SIZE_T>(&ImeMenuGetId);
			OverlayRuntime().imeMenuVtableInitialized = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: initialized local IME Menu class=%u vtable=%p; global Interface::CreateMenuByClass callsite remains untouched",
				kImeMenuClass,
				OverlayRuntime().imeMenuVtable.data() + 1);
			return true;
		}

	}

	void ArmNativeLoadingTransitionDiagnostics()
	{
		LoadingTransitionDiagnosticState& transition =
			OverlayRuntime().loadingTransitionDiagnostics;
		if (!g_bEnableFreeTypeFontRenderingLog)
		{
			transition.runtimeArmed.store(false, std::memory_order_release);
			return;
		}
		transition.inactiveBaselineObserved.store(
			false, std::memory_order_release);
		transition.runtimeArmed.store(true, std::memory_order_release);
		gLog.FormattedMessage(
			"tnvse_hang_trace: diagnostics armed policy=existing-hooks-atomics-only idleLoadingMenuPollFrames=%u postTerminalObservationMs=%llu noShowChangesHook=1 noMonitorThread=1",
			kLoadingMenuIdlePollMask + 1u,
			static_cast<unsigned long long>(
				kLoadingTransitionPostTerminalObservationMs));
	}

	void HandleNativeLoadingTransitionMessage(
		UInt32 messageType, const void* data, UInt32 dataLength)
	{
		LoadingTransitionDiagnosticState& transition =
			OverlayRuntime().loadingTransitionDiagnostics;
		if (!transition.runtimeArmed.load(std::memory_order_acquire))
			return;
		switch (messageType)
		{
		case NVSEMessagingInterface::kMessage_PreLoadGame:
			BeginLoadingTransition(LoadingTransitionKind::SaveLoad,
				HashLoadingTransitionData(data, dataLength),
				transition.loadingMenuActive.load(
					std::memory_order_acquire) != 0);
			break;
		case NVSEMessagingInterface::kMessage_LoadGame:
			if (transition.activeTraceId.load(std::memory_order_acquire)
				&& transition.kind.load(std::memory_order_acquire)
					== LoadingTransitionKind::SaveLoad)
			{
				transition.saveReadObserved.store(1u, std::memory_order_release);
				LogLoadingMenuDiagnosticSnapshot("save-read-complete");
			}
			break;
		case NVSEMessagingInterface::kMessage_PostLoadGame:
			if (transition.activeTraceId.load(std::memory_order_acquire)
				&& transition.kind.load(std::memory_order_acquire)
					== LoadingTransitionKind::SaveLoad)
			{
				const bool succeeded =
					reinterpret_cast<std::uintptr_t>(data) != 0;
				transition.savePostLoadObserved.store(
					1u, std::memory_order_release);
				RequestLoadingTransitionTerminal(
					LoadingTransitionTerminalReason::SavePostLoad,
					succeeded, "save-postload-returned");
			}
			break;
		default:
			break;
		}
	}

	void SetNativeLoadingMainThreadDiagnosticStage(
		NativeLoadingMainThreadStage stage) noexcept
	{
		OverlayRuntime().loadingTransitionDiagnostics.mainThreadStage.store(
			stage, std::memory_order_release);
	}

	UInt64 GetNativeLoadingTransitionTraceId() noexcept
	{
		return OverlayRuntime().loadingTransitionDiagnostics.activeTraceId.load(
			std::memory_order_acquire);
	}

	void ShutdownNativeLoadingTransitionDiagnostics()
	{
		LoadingTransitionDiagnosticState& transition =
			OverlayRuntime().loadingTransitionDiagnostics;
		if (transition.activeTraceId.load(std::memory_order_acquire))
		{
			FinishLoadingTransition("transition-shutdown",
				LoadingTransitionTerminalReason::Shutdown, false);
		}
		transition.runtimeArmed.store(false, std::memory_order_release);
		transition.inactiveBaselineObserved.store(
			false, std::memory_order_release);
	}

	void LogNativeLoadingMenuDiagnostic(const char* event)
	{
		LogLoadingMenuDiagnosticSnapshot(event);
	}

	bool PumpNativeLoadingMenuDiagnostics()
	{
		if (!g_bEnableFreeTypeFontRenderingLog)
			return false;
		LoadingTransitionDiagnosticState& transition =
			OverlayRuntime().loadingTransitionDiagnostics;
		static UInt32 idlePollSequence = 0;
		bool capture = transition.captureEnabled.load(std::memory_order_acquire);
		const bool armed = transition.runtimeArmed.load(
			std::memory_order_acquire);
		const bool shouldPollLoadingMenu = capture
			|| (armed && ((++idlePollSequence & kLoadingMenuIdlePollMask) == 0));
		bool loadingMenuActive = transition.loadingMenuActive.load(
			std::memory_order_acquire) != 0;
		bool loadingMenuChanged = false;
		if (shouldPollLoadingMenu)
		{
			const bool hasInactiveBaseline =
				transition.inactiveBaselineObserved.load(
					std::memory_order_acquire);
			if (!capture && armed && hasInactiveBaseline
				&& HasFastTravelRequestOnMainThread())
			{
				// Arm before querying the menu so a stall in the first transition
				// frame is still correlated with the fast-travel request.
				BeginLoadingTransition(
					LoadingTransitionKind::FastTravel, 0, loadingMenuActive);
				capture = true;
			}
			if (capture)
			{
				transition.mainThreadStage.store(
					NativeLoadingMainThreadStage::LoadingDiagnostics,
					std::memory_order_release);
			}
			InterfaceManager* manager = InterfaceManager::GetSingleton();
			const bool observedActive = manager
				&& InterfaceManager::IsMenuActive(Loading);
			const bool previous = transition.loadingMenuActive.exchange(
				observedActive ? 1u : 0u,
				std::memory_order_acq_rel) != 0;
			loadingMenuActive = observedActive;
			loadingMenuChanged = previous != observedActive;
			if (armed && !observedActive)
			{
				transition.inactiveBaselineObserved.store(
					true, std::memory_order_release);
			}
			if (!capture && armed && hasInactiveBaseline && observedActive)
			{
				BeginLoadingTransition(
					LoadingTransitionKind::NonSaveLoading, 0, true);
				capture = true;
			}
		}

		if (!capture)
			return false;

		const ULONGLONG now = GetTickCount64();
		transition.mainThreadId.store(
			GetCurrentThreadId(), std::memory_order_release);
		transition.lastMainLoopAt.store(now, std::memory_order_release);
		transition.mainLoopSequence.fetch_add(
			1u, std::memory_order_relaxed);
		if (loadingMenuActive)
		{
			transition.loadingMenuObserved.store(1u, std::memory_order_release);
		}
		const char* lifecycleEvent = nullptr;
		const LoadingTransitionKind kindBeforeTravelProbe = transition.kind.load(
			std::memory_order_acquire);
		const bool fastTravelObserved =
			kindBeforeTravelProbe == LoadingTransitionKind::NonSaveLoading
			&& HasFastTravelRequestOnMainThread()
			&& CaptureLoadingTransitionTravelState(transition);
		if (fastTravelObserved
			&& kindBeforeTravelProbe == LoadingTransitionKind::NonSaveLoading)
		{
			transition.kind.store(
				LoadingTransitionKind::FastTravel,
				std::memory_order_release);
			lifecycleEvent = "transition-classified-fast-travel";
		}

		const LoadingTransitionKind kind = transition.kind.load(
			std::memory_order_acquire);
		LoadingTransitionTerminalReason terminalReason =
			transition.terminalReason.load(std::memory_order_acquire);
		if (loadingMenuActive
			&& terminalReason
				== LoadingTransitionTerminalReason::LoadingMenuClosed)
		{
			transition.terminalRequestedAt.store(0, std::memory_order_release);
			transition.observationStartedAt.store(0, std::memory_order_release);
			transition.terminalReason.store(
				LoadingTransitionTerminalReason::None,
				std::memory_order_release);
			transition.terminalSucceeded.store(0, std::memory_order_release);
			terminalReason = LoadingTransitionTerminalReason::None;
			lifecycleEvent = "loading-menu-reopened";
		}
		else if (!loadingMenuActive
			&& kind != LoadingTransitionKind::SaveLoad
			&& transition.loadingMenuObserved.load(std::memory_order_acquire)
			&& !transition.terminalRequestedAt.load(
				std::memory_order_acquire))
		{
			RequestLoadingTransitionTerminal(
				LoadingTransitionTerminalReason::LoadingMenuClosed,
				true, nullptr);
			terminalReason = LoadingTransitionTerminalReason::LoadingMenuClosed;
			lifecycleEvent = "post-loading-observation-begin";
		}

		if (kind == LoadingTransitionKind::SaveLoad
			&& terminalReason == LoadingTransitionTerminalReason::SavePostLoad)
		{
			if (loadingMenuActive)
			{
				if (transition.observationStartedAt.exchange(
						0, std::memory_order_acq_rel))
				{
					lifecycleEvent = "save-loading-menu-reopened";
				}
			}
			else
			{
				ULONGLONG expected = 0;
				if (transition.observationStartedAt.compare_exchange_strong(
						expected, now, std::memory_order_acq_rel,
						std::memory_order_acquire))
				{
					lifecycleEvent = "save-loading-menu-settled";
				}
			}
		}

		if (!lifecycleEvent && loadingMenuChanged)
		{
			lifecycleEvent = loadingMenuActive
				? "loading-menu-visible" : "loading-menu-hidden";
		}
		if (lifecycleEvent)
			LogLoadingMenuDiagnosticSnapshot(lifecycleEvent);

		const ULONGLONG nextSnapshotAt = transition.nextSnapshotAt.load(
			std::memory_order_acquire);
		if (nextSnapshotAt && now >= nextSnapshotAt)
		{
			LogLoadingMenuDiagnosticSnapshot(
				transition.terminalRequestedAt.load(std::memory_order_acquire)
					? "post-terminal-heartbeat"
					: "transition-heartbeat");
			AdvanceLoadingTransitionSnapshotDeadline(transition, now);
		}

		const ULONGLONG observationAt = transition.observationStartedAt.load(
			std::memory_order_acquire);
		if (observationAt && LoadingMenuAgeMs(now, observationAt)
			>= kLoadingTransitionPostTerminalObservationMs)
		{
			FinishLoadingTransition("transition-observation-complete",
				transition.terminalReason.load(std::memory_order_acquire),
				transition.terminalSucceeded.load(
					std::memory_order_acquire) != 0);
		}
		return transition.captureEnabled.load(std::memory_order_acquire);
	}

	void BeginNativeLoadingMenuTextGeometryDiagnostic(
		const void* tile, const char* tileName, float fontTrait)
	{
		LoadingMenuDiagnosticState& diagnostics =
			OverlayRuntime().loadingMenuDiagnostics;
		const ULONGLONG now = GetTickCount64();
		UInt32 fontTraitBits = 0;
		static_assert(sizeof(fontTraitBits) == sizeof(fontTrait));
		std::memcpy(&fontTraitBits, &fontTrait, sizeof(fontTraitBits));
		const UInt32 nameHash = HashLoadingMenuTileName(tileName);
		Menu* loadingMenu = *reinterpret_cast<Menu**>(kLoadingMenu_pMe);
		diagnostics.observedLoadingMenu.store(
			reinterpret_cast<SIZE_T>(loadingMenu), std::memory_order_release);
		diagnostics.observedLoadingMenuRoot.store(
			loadingMenu ? reinterpret_cast<SIZE_T>(loadingMenu->pRootTile) : 0,
			std::memory_order_release);
		const UInt64 call = diagnostics.loadingTextMakeNodeCalls.fetch_add(
			1, std::memory_order_relaxed) + 1;
		UInt64 trace = diagnostics.traceId.load(std::memory_order_acquire);
		if (!trace)
		{
			diagnostics.traceId.compare_exchange_strong(
				trace, 1, std::memory_order_acq_rel);
			trace = diagnostics.traceId.load(std::memory_order_acquire);
			diagnostics.traceStartedAt.store(now, std::memory_order_release);
		}
		diagnostics.loadingTextMakeNodeInFlight.store(
			1, std::memory_order_release);
		diagnostics.lastLoadingTextMakeNodeEnterAt.store(
			now, std::memory_order_release);
		diagnostics.lastLoadingTextTile.store(
			reinterpret_cast<SIZE_T>(tile), std::memory_order_release);
		diagnostics.lastLoadingTextTileNameHash.store(
			nameHash, std::memory_order_release);
		diagnostics.lastLoadingTextFontTraitBits.store(
			fontTraitBits, std::memory_order_release);
		diagnostics.lastLoadingTextProducedNode.store(
			0, std::memory_order_release);
		diagnostics.lastActivityAt.store(now, std::memory_order_release);
		SetLoadingMenuDiagnosticPhase(
			LoadingMenuDiagnosticPhase::LoadingTextMakeNode, now);

		if (call == 1)
		{
			gLog.FormattedMessage(
				"tnvse_loading_menu_diag: event=loading-text-makenode-begin trace=%llu thread=%u call=%llu tile=%p name='%s' nameHash=%08X fontTrait=%.3f fontBits=%08X route=freetype-no-precache",
				static_cast<unsigned long long>(trace),
				GetCurrentThreadId(),
				static_cast<unsigned long long>(call),
				tile, tileName ? tileName : "", nameHash,
				fontTrait, fontTraitBits);
		}
	}

	void EndNativeLoadingMenuTextGeometryDiagnostic(
		const void* tile, bool producedNode)
	{
		LoadingMenuDiagnosticState& diagnostics =
			OverlayRuntime().loadingMenuDiagnostics;
		const ULONGLONG now = GetTickCount64();
		const ULONGLONG enteredAt =
			diagnostics.lastLoadingTextMakeNodeEnterAt.load(
				std::memory_order_acquire);
		const UInt32 durationUs = LoadingMenuDurationUs(enteredAt, now);
		diagnostics.lastLoadingTextMakeNodeDurationUs.store(
			durationUs, std::memory_order_release);
		diagnostics.lastLoadingTextMakeNodeExitAt.store(
			now, std::memory_order_release);
		diagnostics.lastLoadingTextProducedNode.store(
			producedNode ? 1u : 0u, std::memory_order_release);
		diagnostics.lastActivityAt.store(now, std::memory_order_release);
		diagnostics.loadingTextMakeNodeInFlight.store(
			0, std::memory_order_release);
		SetLoadingMenuDiagnosticPhase(
			LoadingMenuDiagnosticPhase::Idle, now);

		const UInt64 call = diagnostics.loadingTextMakeNodeCalls.load(
			std::memory_order_acquire);
		if (call == 1)
			LogLoadingMenuDiagnosticSnapshot(
				"loading-text-makenode-complete");

		const ULONGLONG durationMs = LoadingMenuAgeMs(now, enteredAt);
		static std::atomic<ULONGLONG> lastSlowLogAt{ 0 };
		if ((durationMs >= kLoadingMenuForcedSlowThresholdMs
				|| (g_bEnableFreeTypeFontRenderingLog
					&& durationMs >= kLoadingMenuVerboseSlowThresholdMs))
			&& (durationMs >= kLoadingMenuForcedSlowThresholdMs
				|| ClaimLoadingMenuLogInterval(
					lastSlowLogAt, now, kLoadingMenuSlowLogIntervalMs)))
		{
			gLog.FormattedMessage(
				"tnvse_loading_menu_diag: event=slow-call:loading-text-makenode trace=%llu thread=%u tile=%p nameHash=%08X fontBits=%08X durationUs=%u phase=complete",
				static_cast<unsigned long long>(diagnostics.traceId.load(
					std::memory_order_acquire)),
				GetCurrentThreadId(), tile,
				diagnostics.lastLoadingTextTileNameHash.load(
					std::memory_order_acquire),
				diagnostics.lastLoadingTextFontTraitBits.load(
					std::memory_order_acquire),
				durationUs);
		}
	}

	bool InstallNativePrewarmOverlayLoadingMenuUpdateHook()
	{
		NativeTileOverlayRuntimeState& runtime = OverlayRuntime();
		if (runtime.loadingMenuUpdateHookInstalled)
		{
			if (IsVerifiedLoadingMenuUpdateHook())
				return true;

			SIZE_T observedTarget = 0;
			const bool readable = hook_identity::ReadRel32Target(
				kLoadingMenuThreadUpdateCallSite,
				hook_identity::Rel32Opcode::Call,
				observedTarget);
			runtime.loadingMenuUpdateHookInstalled = false;
			runtime.loadingMenuUpdateHookInstallFailed = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: LoadingMenu Update hook successor changed after installation observed=0x%08X readable=%u policy=no-reassert-prewarm-ui-optional",
				static_cast<UInt32>(observedTarget), readable ? 1u : 0u);
			return false;
		}
		if (runtime.loadingMenuUpdateHookInstallFailed)
			return false;

		SIZE_T currentTarget = 0;
		const SIZE_T instanceLoad =
			kLoadingMenuThreadUpdateCallSite
				- kExpectedLoadingMenuInstanceLoadInstruction.size();
		const SIZE_T adapterTarget =
			reinterpret_cast<SIZE_T>(&LoadingMenuUpdateHook);
		if (!hook_identity::IsAccessibleRegion(
				instanceLoad,
				kExpectedLoadingMenuInstanceLoadInstruction.size() + 5u,
				true)
			|| !hook_identity::MatchesBytesUnchecked(
				instanceLoad,
				kExpectedLoadingMenuInstanceLoadInstruction.data(),
				kExpectedLoadingMenuInstanceLoadInstruction.size())
			|| !hook_identity::ReadRel32Target(
				kLoadingMenuThreadUpdateCallSite,
				hook_identity::Rel32Opcode::Call,
				currentTarget)
			|| !hook_identity::IsExecutableTarget(currentTarget)
			|| currentTarget == adapterTarget)
		{
			runtime.loadingMenuUpdateHookInstallFailed = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: cannot install optional LoadingMenu Update hook call=0x%08X target=0x%08X executable=%u adapterAlreadyPresent=%u policy=font-prewarm-continues-without-presentation",
				static_cast<UInt32>(kLoadingMenuThreadUpdateCallSite),
				static_cast<UInt32>(currentTarget),
				hook_identity::IsExecutableTarget(currentTarget) ? 1u : 0u,
				currentTarget == adapterTarget ? 1u : 0u);
			return false;
		}

		runtime.predecessorLoadingMenuUpdate =
			reinterpret_cast<LoadingMenuUpdateFn>(currentTarget);
		WriteRelCall(kLoadingMenuThreadUpdateCallSite, &LoadingMenuUpdateHook);

		SIZE_T observedTarget = 0;
		const bool observedReadable = hook_identity::ReadRel32Target(
			kLoadingMenuThreadUpdateCallSite,
			hook_identity::Rel32Opcode::Call,
			observedTarget);
		if (observedReadable && observedTarget == adapterTarget)
		{
			runtime.loadingMenuUpdateHookInstalled = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: installed single chained LoadingMenu Update hook call=0x%08X predecessor=0x%08X vanilla=%u presentation=graphical-only idlePath=atomic-sequence-compare originalPresentation=unmodified",
				static_cast<UInt32>(kLoadingMenuThreadUpdateCallSite),
				static_cast<UInt32>(currentTarget),
				currentTarget == kLoadingMenuUpdate ? 1u : 0u);
			return true;
		}

		if (observedReadable && observedTarget == currentTarget)
			runtime.predecessorLoadingMenuUpdate = nullptr;
		runtime.loadingMenuUpdateHookInstalled = false;
		runtime.loadingMenuUpdateHookInstallFailed = true;
		gLog.FormattedMessage(
			"tnvse_native_overlay: LoadingMenu Update hook publication failed observed=0x%08X predecessor=0x%08X readable=%u policy=no-retry-font-prewarm-continues",
			static_cast<UInt32>(observedTarget),
			static_cast<UInt32>(currentTarget),
			observedReadable ? 1u : 0u);
		return false;
	}

}

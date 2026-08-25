#include "native_tile_overlay_detail.h"

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

		constexpr ULONGLONG kLoadingMenuTraceResumeMs = 2000;
		constexpr ULONGLONG kLoadingMenuHeartbeatMs = 5000;
		constexpr ULONGLONG kLoadingMenuStallThresholdMs = 1000;
		constexpr ULONGLONG kLoadingMenuStallLogIntervalMs = 5000;
		constexpr ULONGLONG kLoadingMenuSlowLogIntervalMs = 2000;
		constexpr ULONGLONG kLoadingMenuVerboseSlowThresholdMs = 100;
		constexpr ULONGLONG kLoadingMenuForcedSlowThresholdMs = 1000;

		struct LoadingMenuEngineSnapshot
		{
			void* loadingMenu = nullptr;
			UInt8* loadingThread = nullptr;
			void* interfaceManager = nullptr;
			UInt8 startup = 0;
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
			snapshot.startup = *reinterpret_cast<volatile UInt8*>(
				kLoadingMenuStartupFlag);
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
			switch (phase)
			{
			case LoadingMenuDiagnosticPhase::Idle:
				return "idle";
			case LoadingMenuDiagnosticPhase::UpdateGuard:
				return "update-guard";
			case LoadingMenuDiagnosticPhase::UpdatePredecessor:
				return "update-predecessor";
			case LoadingMenuDiagnosticPhase::UpdateOverlayConsume:
				return "update-overlay-consume";
			case LoadingMenuDiagnosticPhase::ShowChangesGuard:
				return "showchanges-guard";
			case LoadingMenuDiagnosticPhase::ShowChangesRendererLock:
				return "showchanges-renderer-lock";
			case LoadingMenuDiagnosticPhase::ShowChangesPredecessor:
				return "showchanges-predecessor";
			case LoadingMenuDiagnosticPhase::LoadingTextMakeNode:
				return "loading-text-makenode";
			default:
				return "unknown";
			}
		}

		const char* LoadingMenuUpdateDispositionName(
			LoadingMenuUpdateDisposition disposition) noexcept
		{
			switch (disposition)
			{
			case LoadingMenuUpdateDisposition::Unknown:
				return "unknown";
			case LoadingMenuUpdateDisposition::Running:
				return "running";
			case LoadingMenuUpdateDisposition::SkippedPauseOrShutdown:
				return "skip-pause-shutdown";
			case LoadingMenuUpdateDisposition::SkippedStartupBarrier:
				return "skip-startup-barrier";
			default:
				return "invalid";
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
			const ULONGLONG now = GetTickCount64();
			const LoadingMenuEngineSnapshot engine =
				CaptureLoadingMenuEngineSnapshot();
			const LoadingMenuDiagnosticPhase phase =
				diagnostics.phase.load(std::memory_order_acquire);
			const LoadingMenuUpdateDisposition disposition =
				diagnostics.updateDisposition.load(std::memory_order_acquire);
			const UInt32 updateInFlight = diagnostics.updateInFlight.load(
				std::memory_order_acquire);
			const UInt32 showInFlight = diagnostics.showChangesInFlight.load(
				std::memory_order_acquire);

			PrewarmOverlayCommand command;
			AcquireSRWLockShared(&runtime.prewarmCommandLock);
			command = runtime.prewarmCommand;
			ReleaseSRWLockShared(&runtime.prewarmCommandLock);
			const UInt32 published = runtime.prewarmPublishedSequence.load(
				std::memory_order_acquire);
			const UInt32 consumed = runtime.prewarmConsumedSequence.load(
				std::memory_order_acquire);

			gLog.FormattedMessage(
				"tnvse_loading_menu_diag: event=%s trace=%llu traceAgeMs=%llu thread=%u phase=%s phaseAgeMs=%llu menu=%p root=%p loadingThread=%p interface=%p startup=%u pause=%u shutdown=%u update={state:%s,calls:%llu,inFlight:%u,lastEnterAgoMs:%llu,lastExitAgoMs:%llu,lastUs:%u,skipPause:%llu,skipStartup:%llu} show={calls:%llu,inFlight:%u,lastEnterAgoMs:%llu,lastExitAgoMs:%llu,lastUs:%u,skipPause:%llu,skipRenderer:%llu,skipLock:%llu,lastSkip:%u} text={calls:%llu,inFlight:%u,tile:%p,nameHash:%08X,fontBits:%08X,lastEnterAgoMs:%llu,lastExitAgoMs:%llu,lastUs:%u,produced:%u} prewarm={published:%u,consumed:%u,pending:%u,command:%u,visible:%u,ownerShutdown:%u,presentation:graphical-only,progress:%.3f,active:%u,ready:%u,disabled:%u,consumerThread:%u,ownerWork:%u,shutdownSequence:%u,publishCount:%llu,consumeAttempts:%llu,consumeCount:%llu,lastAttempt:%u,lastCompleted:%u,publishAgoMs:%llu,attemptAgoMs:%llu,consumeAgoMs:%llu}",
				event ? event : "unspecified",
				static_cast<unsigned long long>(diagnostics.traceId.load(
					std::memory_order_acquire)),
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, diagnostics.traceStartedAt.load(std::memory_order_acquire))),
				GetCurrentThreadId(),
				LoadingMenuDiagnosticPhaseName(phase),
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, diagnostics.phaseEnteredAt.load(std::memory_order_acquire))),
				engine.loadingMenu,
				reinterpret_cast<void*>(diagnostics.observedLoadingMenuRoot.load(
					std::memory_order_acquire)),
				engine.loadingThread,
				engine.interfaceManager,
				static_cast<UInt32>(engine.startup),
				static_cast<UInt32>(engine.pauseRequested),
				static_cast<UInt32>(engine.shutdownRequested),
				LoadingMenuUpdateDispositionName(disposition),
				static_cast<unsigned long long>(diagnostics.updateCalls.load(
					std::memory_order_acquire)),
				updateInFlight,
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, diagnostics.lastUpdateEnterAt.load(std::memory_order_acquire))),
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, diagnostics.lastUpdateExitAt.load(std::memory_order_acquire))),
				diagnostics.lastUpdateDurationUs.load(std::memory_order_acquire),
				static_cast<unsigned long long>(
					diagnostics.updateSkippedPauseOrShutdown.load(
						std::memory_order_acquire)),
				static_cast<unsigned long long>(
					diagnostics.updateSkippedStartupBarrier.load(
						std::memory_order_acquire)),
				static_cast<unsigned long long>(diagnostics.showChangesCalls.load(
					std::memory_order_acquire)),
				showInFlight,
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, diagnostics.lastShowChangesEnterAt.load(
						std::memory_order_acquire))),
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, diagnostics.lastShowChangesExitAt.load(
						std::memory_order_acquire))),
				diagnostics.lastShowChangesDurationUs.load(std::memory_order_acquire),
				static_cast<unsigned long long>(
					diagnostics.showChangesSkippedPauseOrShutdown.load(
						std::memory_order_acquire)),
				static_cast<unsigned long long>(
					diagnostics.showChangesSkippedRendererUnavailable.load(
						std::memory_order_acquire)),
				static_cast<unsigned long long>(
					diagnostics.showChangesSkippedRendererLock.load(
						std::memory_order_acquire)),
				diagnostics.lastShowSkipReason.load(std::memory_order_acquire),
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
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, diagnostics.lastLoadingTextMakeNodeEnterAt.load(
						std::memory_order_acquire))),
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, diagnostics.lastLoadingTextMakeNodeExitAt.load(
						std::memory_order_acquire))),
				diagnostics.lastLoadingTextMakeNodeDurationUs.load(
					std::memory_order_acquire),
				diagnostics.lastLoadingTextProducedNode.load(
					std::memory_order_acquire),
				published,
				consumed,
				published != consumed ? 1u : 0u,
				command.sequence,
				command.visible ? 1u : 0u,
				command.ownerShutdown ? 1u : 0u,
				command.progress,
				runtime.prewarmActive.load(std::memory_order_acquire) ? 1u : 0u,
				runtime.prewarmReady.load(std::memory_order_acquire) ? 1u : 0u,
				runtime.prewarmConsumerDisabled.load(std::memory_order_acquire)
					? 1u : 0u,
				runtime.prewarmConsumerThreadId.load(std::memory_order_acquire),
				runtime.prewarmOwnerShutdown.OwnerWorkInFlight(),
				runtime.prewarmOwnerShutdown.RequestedSequence(),
				static_cast<unsigned long long>(diagnostics.commandsPublished.load(
					std::memory_order_acquire)),
				static_cast<unsigned long long>(diagnostics.commandConsumeAttempts.load(
					std::memory_order_acquire)),
				static_cast<unsigned long long>(diagnostics.commandsConsumed.load(
					std::memory_order_acquire)),
				diagnostics.lastCommandAttemptSequence.load(
					std::memory_order_acquire),
				diagnostics.lastCommandConsumedSequence.load(
					std::memory_order_acquire),
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, diagnostics.lastCommandPublishedAt.load(
						std::memory_order_acquire))),
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, diagnostics.lastCommandConsumeAttemptAt.load(
						std::memory_order_acquire))),
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, diagnostics.lastCommandConsumedAt.load(
						std::memory_order_acquire))));
		}

		const char* BeginLoadingMenuTrace(
			void* loadingMenu, ULONGLONG now) noexcept
		{
			LoadingMenuDiagnosticState& diagnostics =
				OverlayRuntime().loadingMenuDiagnostics;
			const SIZE_T menuValue = reinterpret_cast<SIZE_T>(loadingMenu);
			const SIZE_T rootValue = loadingMenu
				? reinterpret_cast<SIZE_T>(
					static_cast<Menu*>(loadingMenu)->pRootTile)
				: 0;
			const SIZE_T previousMenu = diagnostics.observedLoadingMenu.exchange(
				menuValue, std::memory_order_acq_rel);
			const SIZE_T previousRoot = diagnostics.observedLoadingMenuRoot.exchange(
				rootValue, std::memory_order_acq_rel);
			const ULONGLONG previousExit = diagnostics.lastUpdateExitAt.load(
				std::memory_order_acquire);
			const UInt64 currentTrace = diagnostics.traceId.load(
				std::memory_order_acquire);
			const char* reason = nullptr;
			if (!currentTrace)
				reason = "trace-begin:first-update";
			else if (previousMenu != menuValue)
				reason = "trace-begin:menu-instance-change";
			else if (previousRoot != rootValue)
				reason = "trace-begin:root-change";
			else if (previousExit
				&& LoadingMenuAgeMs(now, previousExit) >= kLoadingMenuTraceResumeMs)
			{
				reason = "trace-begin:activity-resume";
			}
			if (!reason)
				return nullptr;

			UInt64 trace = diagnostics.traceId.fetch_add(
				1, std::memory_order_acq_rel) + 1;
			if (!trace)
			{
				diagnostics.traceId.store(1, std::memory_order_release);
				trace = 1;
			}
			diagnostics.traceStartedAt.store(now, std::memory_order_release);
			diagnostics.lastShowSkipTrace.store(0, std::memory_order_release);
			diagnostics.updateDisposition.store(
				LoadingMenuUpdateDisposition::Unknown, std::memory_order_release);
			return reason;
		}

		void MaybeLogLoadingMenuSlowCall(
			const char* event, ULONGLONG durationMs)
		{
			LoadingMenuDiagnosticState& diagnostics =
				OverlayRuntime().loadingMenuDiagnostics;
			if (durationMs < kLoadingMenuForcedSlowThresholdMs
				&& (!g_bEnableFreeTypeFontRenderingLog
					|| durationMs < kLoadingMenuVerboseSlowThresholdMs))
			{
				return;
			}
			const ULONGLONG now = GetTickCount64();
			if (durationMs < kLoadingMenuForcedSlowThresholdMs
				&& !ClaimLoadingMenuLogInterval(
					diagnostics.lastSlowLogAt, now,
					kLoadingMenuSlowLogIntervalMs))
			{
				return;
			}
			LogLoadingMenuDiagnosticSnapshot(event);
		}

		void ConsumeNativePrewarmOverlayCommand();

		bool IsLoadingMenuThreadPauseOrShutdownRequested() noexcept
		{
			const UInt8* loadingThread =
				*reinterpret_cast<UInt8**>(kLoadingMenuThread_pMe);
			if (!loadingThread)
				return false;
			return *reinterpret_cast<const volatile UInt8*>(loadingThread
					+ kLoadingMenuThreadPauseRequestedOffset) != 0
				|| *reinterpret_cast<const volatile UInt8*>(loadingThread
					+ kLoadingMenuThreadShutdownOffset) != 0;
		}

		bool IsLoadingMenuVanillaUpdateBlockedAtStartup() noexcept
		{
			return *reinterpret_cast<volatile UInt8*>(
				kLoadingMenuStartupFlag) != 0
				&& *reinterpret_cast<void* volatile*>(
					kInterfaceManager_pMe) == nullptr;
		}

		class ScopedRendererTryLock final
		{
		public:
			explicit ScopedRendererTryLock(NiRenderer* renderer) noexcept
				: m_renderer(renderer)
			{
				if (m_renderer)
				{
					m_locked = TryEnterCriticalSection(
						&m_renderer->m_kRendererLock.m_kCriticalSection) != FALSE;
				}
			}

			~ScopedRendererTryLock() noexcept
			{
				if (m_locked)
				{
					LeaveCriticalSection(
						&m_renderer->m_kRendererLock.m_kCriticalSection);
				}
			}

			explicit operator bool() const noexcept { return m_locked; }

		private:
			NiRenderer* m_renderer = nullptr;
			bool m_locked = false;
		};

		void __fastcall LoadingMenuUpdateHook(void* loadingMenu, void*)
		{
			LoadingMenuDiagnosticState& diagnostics =
				OverlayRuntime().loadingMenuDiagnostics;
			const ULONGLONG enteredAt = GetTickCount64();
			const char* traceReason = BeginLoadingMenuTrace(
				loadingMenu, enteredAt);
			diagnostics.updateCalls.fetch_add(1, std::memory_order_relaxed);
			diagnostics.updateInFlight.store(1, std::memory_order_release);
			diagnostics.lastUpdateEnterAt.store(enteredAt, std::memory_order_release);
			diagnostics.lastActivityAt.store(enteredAt, std::memory_order_release);
			SetLoadingMenuDiagnosticPhase(
				LoadingMenuDiagnosticPhase::UpdateGuard, enteredAt);
			if (traceReason)
				LogLoadingMenuDiagnosticSnapshot(traceReason);

			// ThreadProc cannot acknowledge a pause request until this hook returns.
			// Never enter a known unbounded vanilla wait or optional overlay work once
			// the main thread has requested that acknowledgement.
			const bool pauseOrShutdown =
				IsLoadingMenuThreadPauseOrShutdownRequested();
			const bool startupBlocked =
				IsLoadingMenuVanillaUpdateBlockedAtStartup();
			if (pauseOrShutdown || startupBlocked)
			{
				const LoadingMenuUpdateDisposition disposition = pauseOrShutdown
					? LoadingMenuUpdateDisposition::SkippedPauseOrShutdown
					: LoadingMenuUpdateDisposition::SkippedStartupBarrier;
				const LoadingMenuUpdateDisposition previous =
					diagnostics.updateDisposition.exchange(
						disposition, std::memory_order_acq_rel);
				if (pauseOrShutdown)
				{
					diagnostics.updateSkippedPauseOrShutdown.fetch_add(
						1, std::memory_order_relaxed);
				}
				else
				{
					diagnostics.updateSkippedStartupBarrier.fetch_add(
						1, std::memory_order_relaxed);
				}
				if (previous != disposition)
				{
					LogLoadingMenuDiagnosticSnapshot(pauseOrShutdown
						? "update-skip:pause-or-shutdown"
						: "update-skip:startup-barrier");
				}
				if (IsPrewarmOverlayOwnerShutdownRequested())
				{
					SetLoadingMenuDiagnosticPhase(
						LoadingMenuDiagnosticPhase::UpdateOverlayConsume);
					ConsumeNativePrewarmOverlayCommand();
				}
				const ULONGLONG completedAt = GetTickCount64();
				diagnostics.lastUpdateDurationUs.store(
					LoadingMenuDurationUs(enteredAt, completedAt),
					std::memory_order_release);
				diagnostics.lastUpdateExitAt.store(
					completedAt, std::memory_order_release);
				diagnostics.lastActivityAt.store(
					completedAt, std::memory_order_release);
				diagnostics.updateInFlight.store(0, std::memory_order_release);
				SetLoadingMenuDiagnosticPhase(
					LoadingMenuDiagnosticPhase::Idle, completedAt);
				return;
			}

			const LoadingMenuUpdateDisposition previousDisposition =
				diagnostics.updateDisposition.exchange(
					LoadingMenuUpdateDisposition::Running,
					std::memory_order_acq_rel);
			if (previousDisposition != LoadingMenuUpdateDisposition::Unknown
				&& previousDisposition != LoadingMenuUpdateDisposition::Running)
			{
				LogLoadingMenuDiagnosticSnapshot("update-resumed");
			}

			LoadingMenuUpdateFn predecessor =
				OverlayRuntime().predecessorLoadingMenuUpdate;
			if (!predecessor)
			{
				LogLoadingMenuDiagnosticSnapshot("update-error:missing-predecessor");
			}
			else
			{
				SetLoadingMenuDiagnosticPhase(
					LoadingMenuDiagnosticPhase::UpdatePredecessor);
				predecessor(loadingMenu);
			}
			const ULONGLONG predecessorCompletedAt = GetTickCount64();
			diagnostics.lastUpdateDurationUs.store(
				LoadingMenuDurationUs(enteredAt, predecessorCompletedAt),
				std::memory_order_release);
			MaybeLogLoadingMenuSlowCall(
				"slow-call:update-predecessor",
				LoadingMenuAgeMs(predecessorCompletedAt, enteredAt));
			if (IsPrewarmOverlayOwnerShutdownRequested())
			{
				SetLoadingMenuDiagnosticPhase(
					LoadingMenuDiagnosticPhase::UpdateOverlayConsume);
				ConsumeNativePrewarmOverlayCommand();
				const ULONGLONG completedAt = GetTickCount64();
				diagnostics.lastUpdateDurationUs.store(
					LoadingMenuDurationUs(enteredAt, completedAt),
					std::memory_order_release);
				diagnostics.lastUpdateExitAt.store(
					completedAt, std::memory_order_release);
				diagnostics.lastActivityAt.store(
					completedAt, std::memory_order_release);
				diagnostics.updateInFlight.store(0, std::memory_order_release);
				SetLoadingMenuDiagnosticPhase(
					LoadingMenuDiagnosticPhase::Idle, completedAt);
				return;
			}
			if (IsLoadingMenuThreadPauseOrShutdownRequested())
			{
				diagnostics.updateSkippedPauseOrShutdown.fetch_add(
					1, std::memory_order_relaxed);
				const LoadingMenuUpdateDisposition previous =
					diagnostics.updateDisposition.exchange(
						LoadingMenuUpdateDisposition::SkippedPauseOrShutdown,
						std::memory_order_acq_rel);
				if (previous
					!= LoadingMenuUpdateDisposition::SkippedPauseOrShutdown)
				{
					LogLoadingMenuDiagnosticSnapshot(
						"update-stop-after-predecessor:pause-or-shutdown");
				}
				const ULONGLONG completedAt = GetTickCount64();
				diagnostics.lastUpdateExitAt.store(
					completedAt, std::memory_order_release);
				diagnostics.lastActivityAt.store(
					completedAt, std::memory_order_release);
				diagnostics.updateInFlight.store(0, std::memory_order_release);
				SetLoadingMenuDiagnosticPhase(
					LoadingMenuDiagnosticPhase::Idle, completedAt);
				return;
			}
			SetLoadingMenuDiagnosticPhase(
				LoadingMenuDiagnosticPhase::UpdateOverlayConsume);
			ConsumeNativePrewarmOverlayCommand();
			const ULONGLONG completedAt = GetTickCount64();
			diagnostics.lastUpdateDurationUs.store(
				LoadingMenuDurationUs(enteredAt, completedAt),
				std::memory_order_release);
			diagnostics.lastUpdateExitAt.store(
				completedAt, std::memory_order_release);
			diagnostics.lastActivityAt.store(
				completedAt, std::memory_order_release);
			diagnostics.updateInFlight.store(0, std::memory_order_release);
			SetLoadingMenuDiagnosticPhase(
				LoadingMenuDiagnosticPhase::Idle, completedAt);
			if (g_bEnableFreeTypeFontRenderingLog
				&& ClaimLoadingMenuLogInterval(
					diagnostics.lastHeartbeatLogAt, completedAt,
					kLoadingMenuHeartbeatMs))
			{
				LogLoadingMenuDiagnosticSnapshot("owner-thread-heartbeat");
			}
		}

		void __cdecl LoadingMenuShowChangesHook()
		{
			LoadingMenuDiagnosticState& diagnostics =
				OverlayRuntime().loadingMenuDiagnostics;
			const ULONGLONG enteredAt = GetTickCount64();
			diagnostics.showChangesCalls.fetch_add(1, std::memory_order_relaxed);
			diagnostics.showChangesInFlight.store(1, std::memory_order_release);
			diagnostics.lastShowChangesEnterAt.store(
				enteredAt, std::memory_order_release);
			diagnostics.lastActivityAt.store(enteredAt, std::memory_order_release);
			SetLoadingMenuDiagnosticPhase(
				LoadingMenuDiagnosticPhase::ShowChangesGuard, enteredAt);
			auto finishCall = [&](ULONGLONG completedAt)
			{
				diagnostics.lastShowChangesDurationUs.store(
					LoadingMenuDurationUs(enteredAt, completedAt),
					std::memory_order_release);
				diagnostics.lastShowChangesExitAt.store(
					completedAt, std::memory_order_release);
				diagnostics.lastActivityAt.store(
					completedAt, std::memory_order_release);
				diagnostics.showChangesInFlight.store(
					0, std::memory_order_release);
				SetLoadingMenuDiagnosticPhase(
					LoadingMenuDiagnosticPhase::Idle, completedAt);
			};
			if (IsLoadingMenuThreadPauseOrShutdownRequested())
			{
				diagnostics.showChangesSkippedPauseOrShutdown.fetch_add(
					1, std::memory_order_relaxed);
				diagnostics.lastShowSkipReason.store(1, std::memory_order_release);
				const UInt64 trace = diagnostics.traceId.load(
					std::memory_order_acquire);
				if (diagnostics.lastShowSkipTrace.exchange(
						trace, std::memory_order_acq_rel) != trace)
				{
					LogLoadingMenuDiagnosticSnapshot(
						"showchanges-skip:pause-or-shutdown");
				}
				finishCall(GetTickCount64());
				return;
			}
			LoadingMenuShowChangesFn predecessor =
				OverlayRuntime().predecessorLoadingMenuShowChanges;
			if (!predecessor)
			{
				diagnostics.lastShowSkipReason.store(2, std::memory_order_release);
				LogLoadingMenuDiagnosticSnapshot(
					"showchanges-error:missing-predecessor");
				finishCall(GetTickCount64());
				return;
			}

			SetLoadingMenuDiagnosticPhase(
				LoadingMenuDiagnosticPhase::ShowChangesRendererLock);
			NiRenderer* renderer = NiRenderer::GetSingleton();
			UInt32 skipReason = 0;
			const char* skipEvent = nullptr;
			bool logSkip = false;
			ULONGLONG completedAt = enteredAt;
			{
				ScopedRendererTryLock rendererLock(renderer);
				if (!renderer)
				{
					skipReason = 3;
					skipEvent = "showchanges-skip:renderer-unavailable";
					diagnostics.showChangesSkippedRendererUnavailable.fetch_add(
						1, std::memory_order_relaxed);
				}
				else if (!rendererLock)
				{
					skipReason = 4;
					skipEvent = "showchanges-skip:renderer-lock-busy";
					diagnostics.showChangesSkippedRendererLock.fetch_add(
						1, std::memory_order_relaxed);
				}
				else if (IsLoadingMenuThreadPauseOrShutdownRequested())
				{
					skipReason = 1;
					skipEvent = "showchanges-skip:pause-after-lock";
					diagnostics.showChangesSkippedPauseOrShutdown.fetch_add(
						1, std::memory_order_relaxed);
				}
				else
				{
					diagnostics.lastShowSkipReason.store(
						0, std::memory_order_release);

					// ShowChanges creates every dirty LoadingMenu TileText node while
					// holding renderer/UI locks. Keep all tNVSE geometry on the
					// synchronous, non-retained route for this complete traversal.
					ScopedFreeTypeNoPrecacheRoute noPrecacheRoute;
					SetLoadingMenuDiagnosticPhase(
						LoadingMenuDiagnosticPhase::ShowChangesPredecessor);
					predecessor();
				}
				completedAt = GetTickCount64();
			}

			if (skipReason)
			{
				diagnostics.lastShowSkipReason.store(
					skipReason, std::memory_order_release);
				const UInt64 trace = diagnostics.traceId.load(
					std::memory_order_acquire);
				const bool firstForTrace =
					diagnostics.lastShowSkipTrace.exchange(
						trace, std::memory_order_acq_rel) != trace;
				logSkip = firstForTrace
					|| (g_bEnableFreeTypeFontRenderingLog
						&& ClaimLoadingMenuLogInterval(
							diagnostics.lastShowSkipLogAt, completedAt,
							kLoadingMenuHeartbeatMs));
				if (logSkip)
					LogLoadingMenuDiagnosticSnapshot(skipEvent);
				finishCall(completedAt);
				return;
			}

			if (diagnostics.pendingLoadingTextFirstCompleteLog.exchange(
					false, std::memory_order_acq_rel))
			{
				LogLoadingMenuDiagnosticSnapshot(
					"loading-text-makenode:showchanges-returned");
			}
			if (diagnostics.pendingLoadingTextSlowLog.exchange(
					false, std::memory_order_acq_rel))
			{
				gLog.FormattedMessage(
					"tnvse_loading_menu_diag: event=slow-call:loading-text-makenode trace=%llu thread=%u tile=%p nameHash=%08X fontBits=%08X durationUs=%u logPoint=after-renderer-lock-release",
					static_cast<unsigned long long>(diagnostics.traceId.load(
						std::memory_order_acquire)),
					GetCurrentThreadId(),
					reinterpret_cast<void*>(
						diagnostics.pendingLoadingTextSlowTile.load(
							std::memory_order_acquire)),
					diagnostics.pendingLoadingTextSlowNameHash.load(
						std::memory_order_acquire),
					diagnostics.pendingLoadingTextSlowFontBits.load(
						std::memory_order_acquire),
					diagnostics.pendingLoadingTextSlowDurationUs.load(
						std::memory_order_acquire));
				LogLoadingMenuDiagnosticSnapshot(
					"slow-call:loading-text-makenode");
			}
			finishCall(completedAt);
			MaybeLogLoadingMenuSlowCall(
				"slow-call:showchanges-predecessor",
				LoadingMenuAgeMs(completedAt, enteredAt));
		}

		bool IsVerifiedLoadingMenuCallHook(SIZE_T callSite,
			SIZE_T adapterTarget, SIZE_T predecessorTarget)
		{
			SIZE_T currentTarget = 0;
			return hook_identity::ReadRel32Target(
					callSite, hook_identity::Rel32Opcode::Call, currentTarget)
				&& currentTarget == adapterTarget
				&& predecessorTarget != adapterTarget
				&& hook_identity::IsExecutableTarget(predecessorTarget);
		}

		bool HasVerifiedLoadingMenuUpdateHook()
		{
			if (!OverlayRuntime().loadingMenuUpdateHookInstalled
				|| !OverlayRuntime().loadingMenuShowChangesHookInstalled)
				return false;

			return IsVerifiedLoadingMenuCallHook(
					kLoadingMenuThreadUpdateCallSite,
					reinterpret_cast<SIZE_T>(&LoadingMenuUpdateHook),
					reinterpret_cast<SIZE_T>(
						OverlayRuntime().predecessorLoadingMenuUpdate))
				&& IsVerifiedLoadingMenuCallHook(
					kLoadingMenuThreadShowChangesCallSite,
					reinterpret_cast<SIZE_T>(&LoadingMenuShowChangesHook),
					reinterpret_cast<SIZE_T>(
						OverlayRuntime().predecessorLoadingMenuShowChanges));
		}

		UInt32 __fastcall ImeMenuGetId(Menu*, void*)
		{
			return kImeMenuClass;
		}

		Menu* CreateImeMenu()
		{
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

		Menu* __fastcall CreateMenuByClassHook(
			void* factory,
			void*,
			UInt32 menuClass)
		{
			if (menuClass == kImeMenuClass && OverlayRuntime().creatingImeMenu)
				return CreateImeMenu();
			return OverlayRuntime().predecessorCreateMenuByClass
				? OverlayRuntime().predecessorCreateMenuByClass(factory, menuClass)
				: nullptr;
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

		bool EnsureImeMenuFactory()
		{
			if (!EnsurePipboyDrawExclusionHook())
				return false;
			if (OverlayRuntime().imeMenuFactoryInstalled)
			{
				SIZE_T currentTarget = 0;
				const bool targetReadable = hook_identity::ReadRel32Target(
					kCreateMenuByClassCallSite,
					hook_identity::Rel32Opcode::Call,
					currentTarget);
				const SIZE_T adapterTarget = reinterpret_cast<SIZE_T>(
					&CreateMenuByClassHook);
				const SIZE_T predecessorTarget = reinterpret_cast<SIZE_T>(
					OverlayRuntime().predecessorCreateMenuByClass);
				if (targetReadable
					&& currentTarget == adapterTarget
					&& predecessorTarget != adapterTarget
					&& hook_identity::IsExecutableTarget(predecessorTarget))
				{
					return true;
				}
				if (targetReadable
					&& currentTarget == predecessorTarget
					&& hook_identity::IsExecutableTarget(currentTarget))
				{
					OverlayRuntime().imeMenuFactoryInstalled = false;
					OverlayRuntime().predecessorCreateMenuByClass = nullptr;
				}
				else
				{
					OverlayRuntime().imeMenuFactoryInstalled = false;
					OverlayRuntime().imeMenuFactoryInstallFailed = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: IME Menu factory capability revoked; observed target=0x%08X adapter=0x%08X predecessor=0x%08X readable=%u",
						static_cast<UInt32>(currentTarget),
						static_cast<UInt32>(adapterTarget),
						static_cast<UInt32>(predecessorTarget),
						targetReadable ? 1u : 0u);
					return false;
				}
			}
			if (OverlayRuntime().imeMenuFactoryInstallFailed)
				return false;

			SIZE_T currentTarget = 0;
			if (!hook_identity::ReadRel32Target(
				kCreateMenuByClassCallSite,
				hook_identity::Rel32Opcode::Call,
				currentTarget))
			{
				OverlayRuntime().imeMenuFactoryInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: cannot install IME Menu factory hook; expected CALL at 0x%08X",
					static_cast<UInt32>(kCreateMenuByClassCallSite));
				return false;
			}

			if (currentTarget == reinterpret_cast<SIZE_T>(
					&CreateMenuByClassHook))
			{
				const SIZE_T predecessorTarget = reinterpret_cast<SIZE_T>(
					OverlayRuntime().predecessorCreateMenuByClass);
				OverlayRuntime().imeMenuFactoryInstalled =
					predecessorTarget != currentTarget
					&& hook_identity::IsExecutableTarget(predecessorTarget);
				if (!OverlayRuntime().imeMenuFactoryInstalled)
				{
					OverlayRuntime().imeMenuFactoryInstallFailed = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: IME Menu factory hook is present but its predecessor is unavailable predecessor=0x%08X",
						static_cast<UInt32>(predecessorTarget));
				}
				return OverlayRuntime().imeMenuFactoryInstalled;
			}
			if (!hook_identity::IsExecutableTarget(currentTarget)
				|| !hook_identity::IsAccessibleRegion(
					kMenuVTable - sizeof(SIZE_T),
					(kMenuVTableEntryCount + 1) * sizeof(SIZE_T),
					false))
			{
				OverlayRuntime().imeMenuFactoryInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: cannot install IME Menu factory hook; invalid CreateMenuByClass target=0x%08X or Menu vtable",
					static_cast<UInt32>(currentTarget));
				return false;
			}

			const SIZE_T* vanillaVtable =
				reinterpret_cast<const SIZE_T*>(kMenuVTable);
			OverlayRuntime().imeMenuVtable.front() = vanillaVtable[-1];
			std::copy_n(
				vanillaVtable,
				kMenuVTableEntryCount,
				OverlayRuntime().imeMenuVtable.begin() + 1);
			OverlayRuntime().imeMenuVtable[kMenuGetIdVtableIndex + 1] =
				reinterpret_cast<SIZE_T>(&ImeMenuGetId);
			OverlayRuntime().predecessorCreateMenuByClass =
				reinterpret_cast<CreateMenuByClassFn>(currentTarget);
			// InterfaceManager menu factory -> CreateMenuByClass
			// (__thiscall target via __fastcall shim).
			WriteRelCall(kCreateMenuByClassCallSite, &CreateMenuByClassHook);
			const SIZE_T adapterTarget = reinterpret_cast<SIZE_T>(
				&CreateMenuByClassHook);
			SIZE_T observedTarget = 0;
			const bool observedReadable = hook_identity::ReadRel32Target(
				kCreateMenuByClassCallSite,
				hook_identity::Rel32Opcode::Call,
				observedTarget);
			if (observedReadable && observedTarget == adapterTarget)
			{
				OverlayRuntime().imeMenuFactoryInstalled = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: installed dedicated IME Menu factory class=%u chainedTarget=0x%08X",
					kImeMenuClass,
					static_cast<UInt32>(currentTarget));
				return true;
			}

			if (observedReadable && observedTarget == currentTarget)
			{
				OverlayRuntime().predecessorCreateMenuByClass = nullptr;
				OverlayRuntime().imeMenuFactoryInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: IME Menu factory hook write did not publish call=0x%08X predecessor=0x%08X",
					static_cast<UInt32>(kCreateMenuByClassCallSite),
					static_cast<UInt32>(currentTarget));
				return false;
			}

			const bool successorExecutable = observedReadable
				&& hook_identity::IsExecutableTarget(observedTarget);
			// Preserve the saved predecessor for a possibly live chain, while
			// keeping the factory unavailable until our CALL target is observable.
			OverlayRuntime().imeMenuFactoryInstalled = false;
			OverlayRuntime().imeMenuFactoryInstallFailed = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: IME Menu factory hook may be retained below observed target=0x%08X predecessor=0x%08X readable=%u executable=%u; reachability unverified, feature disabled",
				static_cast<UInt32>(observedTarget),
				static_cast<UInt32>(currentTarget),
				observedReadable ? 1u : 0u,
				successorExecutable ? 1u : 0u);
			return false;
		}

	}

	void LogNativeLoadingMenuDiagnostic(const char* event)
	{
		LogLoadingMenuDiagnosticSnapshot(event);
	}

	void PumpNativeLoadingMenuDiagnostics()
	{
		LoadingMenuDiagnosticState& diagnostics =
			OverlayRuntime().loadingMenuDiagnostics;
		const ULONGLONG now = GetTickCount64();
		const LoadingMenuDiagnosticPhase phase = diagnostics.phase.load(
			std::memory_order_acquire);
		const ULONGLONG phaseAge = LoadingMenuAgeMs(
			now, diagnostics.phaseEnteredAt.load(std::memory_order_acquire));
		if (phase != LoadingMenuDiagnosticPhase::Idle
			&& phaseAge >= kLoadingMenuStallThresholdMs)
		{
			if (ClaimLoadingMenuLogInterval(
					diagnostics.lastStallLogAt, now,
					kLoadingMenuStallLogIntervalMs))
			{
				LogLoadingMenuDiagnosticSnapshot("main-loop-stall-watch");
			}
			return;
		}

		if (!g_bEnableFreeTypeFontRenderingLog)
			return;
		const ULONGLONG lastActivity = diagnostics.lastActivityAt.load(
			std::memory_order_acquire);
		const bool recentOwnerActivity = lastActivity
			&& LoadingMenuAgeMs(now, lastActivity) <= kLoadingMenuHeartbeatMs;
		const bool pendingCommand =
			OverlayRuntime().prewarmPublishedSequence.load(std::memory_order_acquire)
				!= OverlayRuntime().prewarmConsumedSequence.load(
					std::memory_order_acquire);
		if ((recentOwnerActivity || pendingCommand
				|| OverlayRuntime().prewarmActive.load(std::memory_order_acquire))
			&& ClaimLoadingMenuLogInterval(
				diagnostics.lastHeartbeatLogAt, now,
				kLoadingMenuHeartbeatMs))
		{
			LogLoadingMenuDiagnosticSnapshot("main-loop-heartbeat");
		}
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
		const UInt64 call = diagnostics.loadingTextMakeNodeCalls.fetch_add(
			1, std::memory_order_relaxed) + 1;
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

		const UInt64 trace = diagnostics.traceId.load(
			std::memory_order_acquire);
		const UInt64 previouslyLogged =
			diagnostics.lastLoadingTextBeginLoggedTrace.exchange(
				trace, std::memory_order_acq_rel);
			if (previouslyLogged != trace || call == 1)
			{
			gLog.FormattedMessage(
				"tnvse_loading_menu_diag: event=loading-text-makenode-begin trace=%llu thread=%u call=%llu tile=%p name='%s' nameHash=%08X fontTrait=%.3f fontBits=%08X route=freetype-no-precache",
				static_cast<unsigned long long>(trace),
				GetCurrentThreadId(),
				static_cast<unsigned long long>(call),
				tile,
				tileName ? tileName : "",
				nameHash,
				fontTrait,
				fontTraitBits);
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
		diagnostics.lastLoadingTextMakeNodeDurationUs.store(
			LoadingMenuDurationUs(enteredAt, now), std::memory_order_release);
		diagnostics.lastLoadingTextMakeNodeExitAt.store(
			now, std::memory_order_release);
		diagnostics.lastLoadingTextProducedNode.store(
			producedNode ? 1u : 0u, std::memory_order_release);
		diagnostics.lastActivityAt.store(now, std::memory_order_release);
		diagnostics.loadingTextMakeNodeInFlight.store(
			0, std::memory_order_release);
		SetLoadingMenuDiagnosticPhase(
			LoadingMenuDiagnosticPhase::ShowChangesPredecessor, now);

		const UInt64 trace = diagnostics.traceId.load(
			std::memory_order_acquire);
		const UInt64 call = diagnostics.loadingTextMakeNodeCalls.load(
			std::memory_order_acquire);
		const UInt64 previouslyLogged =
			diagnostics.lastLoadingTextEndLoggedTrace.exchange(
				trace, std::memory_order_acq_rel);
			if (previouslyLogged != trace || call == 1)
				diagnostics.pendingLoadingTextFirstCompleteLog.store(
					true, std::memory_order_release);
		const ULONGLONG durationMs = LoadingMenuAgeMs(now, enteredAt);
		if ((durationMs >= kLoadingMenuForcedSlowThresholdMs
				|| (g_bEnableFreeTypeFontRenderingLog
					&& durationMs >= kLoadingMenuVerboseSlowThresholdMs))
			&& (durationMs >= kLoadingMenuForcedSlowThresholdMs
				|| ClaimLoadingMenuLogInterval(
					diagnostics.lastSlowLogAt, now,
					kLoadingMenuSlowLogIntervalMs)))
		{
			diagnostics.pendingLoadingTextSlowDurationUs.store(
				diagnostics.lastLoadingTextMakeNodeDurationUs.load(
					std::memory_order_acquire),
				std::memory_order_release);
			diagnostics.pendingLoadingTextSlowTile.store(
				reinterpret_cast<SIZE_T>(tile), std::memory_order_release);
			diagnostics.pendingLoadingTextSlowNameHash.store(
				diagnostics.lastLoadingTextTileNameHash.load(
					std::memory_order_acquire),
				std::memory_order_release);
			diagnostics.pendingLoadingTextSlowFontBits.store(
				diagnostics.lastLoadingTextFontTraitBits.load(
					std::memory_order_acquire),
				std::memory_order_release);
			diagnostics.pendingLoadingTextSlowLog.store(
				true, std::memory_order_release);
		}
	}

	bool InstallNativePrewarmOverlayLoadingThreadHook()
	{
		NativeTileOverlayRuntimeState& runtime = OverlayRuntime();
		if (runtime.loadingMenuUpdateHookInstalled
			|| runtime.loadingMenuShowChangesHookInstalled)
		{
			if (HasVerifiedLoadingMenuUpdateHook())
				return true;

			SIZE_T currentUpdateTarget = 0;
			SIZE_T currentShowChangesTarget = 0;
			const bool updateReadable = hook_identity::ReadRel32Target(
				kLoadingMenuThreadUpdateCallSite,
				hook_identity::Rel32Opcode::Call,
				currentUpdateTarget);
			const bool showChangesReadable = hook_identity::ReadRel32Target(
				kLoadingMenuThreadShowChangesCallSite,
				hook_identity::Rel32Opcode::Call,
				currentShowChangesTarget);
			const SIZE_T updateAdapterTarget = reinterpret_cast<SIZE_T>(
				&LoadingMenuUpdateHook);
			const SIZE_T showChangesAdapterTarget = reinterpret_cast<SIZE_T>(
				&LoadingMenuShowChangesHook);
			const SIZE_T updatePredecessorTarget = reinterpret_cast<SIZE_T>(
				runtime.predecessorLoadingMenuUpdate);
			const SIZE_T showChangesPredecessorTarget = reinterpret_cast<SIZE_T>(
				runtime.predecessorLoadingMenuShowChanges);
			const bool cleanlyRestored = updateReadable
				&& showChangesReadable
				&& currentUpdateTarget == updatePredecessorTarget
				&& currentShowChangesTarget == showChangesPredecessorTarget
				&& hook_identity::IsExecutableTarget(currentUpdateTarget)
				&& hook_identity::IsExecutableTarget(currentShowChangesTarget);
			if (cleanlyRestored)
			{
				runtime.loadingMenuUpdateHookInstalled = false;
				runtime.loadingMenuShowChangesHookInstalled = false;
				runtime.predecessorLoadingMenuUpdate = nullptr;
				runtime.predecessorLoadingMenuShowChanges = nullptr;
			}
			else
			{
				runtime.loadingMenuUpdateHookInstalled = false;
				runtime.loadingMenuShowChangesHookInstalled = false;
				runtime.loadingMenuUpdateHookInstallFailed = true;
				runtime.prewarmConsumerDisabled.store(true, std::memory_order_release);
				runtime.prewarmReady.store(false, std::memory_order_release);
				runtime.prewarmActive.store(false, std::memory_order_release);
				gLog.FormattedMessage(
					"tnvse_native_overlay: LoadingMenuThread safety capability revoked; updateObserved=0x%08X updateAdapter=0x%08X updatePredecessor=0x%08X updateReadable=%u showObserved=0x%08X showAdapter=0x%08X showPredecessor=0x%08X showReadable=%u",
					static_cast<UInt32>(currentUpdateTarget),
					static_cast<UInt32>(updateAdapterTarget),
					static_cast<UInt32>(updatePredecessorTarget),
					updateReadable ? 1u : 0u,
					static_cast<UInt32>(currentShowChangesTarget),
					static_cast<UInt32>(showChangesAdapterTarget),
					static_cast<UInt32>(showChangesPredecessorTarget),
					showChangesReadable ? 1u : 0u);
				return false;
			}
		}
		if (runtime.loadingMenuUpdateHookInstallFailed)
			return false;

		SIZE_T currentUpdateTarget = 0;
		SIZE_T currentShowChangesTarget = 0;
		const SIZE_T loadingMenuPointerLoad =
			kLoadingMenuThreadUpdateCallSite
				- kExpectedLoadingMenuInstanceLoadInstruction.size();
		if (!hook_identity::IsAccessibleRegion(
				loadingMenuPointerLoad,
				kExpectedLoadingMenuInstanceLoadInstruction.size()
					+ 2u * sizeof(UInt8) + 2u * sizeof(SInt32),
				true)
			|| !hook_identity::MatchesBytesUnchecked(
				loadingMenuPointerLoad,
				kExpectedLoadingMenuInstanceLoadInstruction.data(),
				kExpectedLoadingMenuInstanceLoadInstruction.size())
			|| !hook_identity::ReadRel32Target(
				kLoadingMenuThreadUpdateCallSite,
				hook_identity::Rel32Opcode::Call,
				currentUpdateTarget)
			|| !hook_identity::ReadRel32Target(
				kLoadingMenuThreadShowChangesCallSite,
				hook_identity::Rel32Opcode::Call,
				currentShowChangesTarget)
			|| !hook_identity::IsExecutableTarget(currentUpdateTarget)
			|| !hook_identity::IsExecutableTarget(currentShowChangesTarget))
		{
			runtime.loadingMenuUpdateHookInstallFailed = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: cannot install LoadingMenuThread safety hooks; executable identity mismatch updateCall=0x%08X showChangesCall=0x%08X",
				static_cast<UInt32>(kLoadingMenuThreadUpdateCallSite),
				static_cast<UInt32>(kLoadingMenuThreadShowChangesCallSite));
			return false;
		}

		const SIZE_T updateAdapterTarget = reinterpret_cast<SIZE_T>(
			&LoadingMenuUpdateHook);
		const SIZE_T showChangesAdapterTarget = reinterpret_cast<SIZE_T>(
			&LoadingMenuShowChangesHook);
		if (currentUpdateTarget == updateAdapterTarget
			|| currentShowChangesTarget == showChangesAdapterTarget)
		{
			runtime.loadingMenuUpdateHookInstallFailed = true;
			runtime.prewarmConsumerDisabled.store(true, std::memory_order_release);
			gLog.FormattedMessage(
				"tnvse_native_overlay: LoadingMenuThread safety adapter is already present without recoverable predecessor updateTarget=0x%08X showChangesTarget=0x%08X",
				static_cast<UInt32>(currentUpdateTarget),
				static_cast<UInt32>(currentShowChangesTarget));
			return false;
		}

		// Install ShowChanges first. The prewarm consumer is not reachable until
		// every LoadingMenu geometry rebuild is already inside the safe route.
		runtime.predecessorLoadingMenuShowChanges =
			reinterpret_cast<LoadingMenuShowChangesFn>(currentShowChangesTarget);
		WriteRelCall(kLoadingMenuThreadShowChangesCallSite,
			&LoadingMenuShowChangesHook);
		SIZE_T observedShowChangesTarget = 0;
		const bool observedShowChangesReadable = hook_identity::ReadRel32Target(
			kLoadingMenuThreadShowChangesCallSite,
			hook_identity::Rel32Opcode::Call,
			observedShowChangesTarget);
		if (!observedShowChangesReadable
			|| observedShowChangesTarget != showChangesAdapterTarget)
		{
			if (observedShowChangesReadable
				&& observedShowChangesTarget == currentShowChangesTarget)
			{
				runtime.predecessorLoadingMenuShowChanges = nullptr;
			}
			runtime.loadingMenuUpdateHookInstallFailed = true;
			runtime.prewarmConsumerDisabled.store(true, std::memory_order_release);
			gLog.FormattedMessage(
				"tnvse_native_overlay: LoadingMenuThread ShowChanges safety hook publication failed observed=0x%08X predecessor=0x%08X readable=%u",
				static_cast<UInt32>(observedShowChangesTarget),
				static_cast<UInt32>(currentShowChangesTarget),
				observedShowChangesReadable ? 1u : 0u);
			return false;
		}
		runtime.loadingMenuShowChangesHookInstalled = true;

		runtime.predecessorLoadingMenuUpdate =
			reinterpret_cast<LoadingMenuUpdateFn>(currentUpdateTarget);
		// LoadingMenuThread -> LoadingMenu::Update
		// (__thiscall target via __fastcall shim).
		WriteRelCall(kLoadingMenuThreadUpdateCallSite, &LoadingMenuUpdateHook);
		SIZE_T observedUpdateTarget = 0;
		const bool observedUpdateReadable = hook_identity::ReadRel32Target(
			kLoadingMenuThreadUpdateCallSite,
			hook_identity::Rel32Opcode::Call,
			observedUpdateTarget);
		if (observedUpdateReadable
			&& observedUpdateTarget == updateAdapterTarget)
		{
			runtime.loadingMenuUpdateHookInstalled = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: installed LoadingMenuThread safety hooks updateCall=0x%08X updateTarget=0x%08X vanillaUpdate=%u showChangesCall=0x%08X showChangesTarget=0x%08X vanillaShowChanges=%u policy=pause-aware-startup-bounded rendererLock=try-enter fontRoute=loading-menu-no-precache-no-retained-command",
				static_cast<UInt32>(kLoadingMenuThreadUpdateCallSite),
				static_cast<UInt32>(currentUpdateTarget),
				currentUpdateTarget == kLoadingMenuUpdate ? 1u : 0u,
				static_cast<UInt32>(kLoadingMenuThreadShowChangesCallSite),
				static_cast<UInt32>(currentShowChangesTarget),
				currentShowChangesTarget == kLoadingMenuShowChanges ? 1u : 0u);
			LogLoadingMenuDiagnosticSnapshot("hooks-installed");
			return true;
		}

		if (observedUpdateReadable
			&& observedUpdateTarget == currentUpdateTarget)
		{
			runtime.predecessorLoadingMenuUpdate = nullptr;
		}
		runtime.loadingMenuUpdateHookInstalled = false;
		runtime.loadingMenuUpdateHookInstallFailed = true;
		runtime.prewarmConsumerDisabled.store(true, std::memory_order_release);
		runtime.prewarmReady.store(false, std::memory_order_release);
		runtime.prewarmActive.store(false, std::memory_order_release);
		gLog.FormattedMessage(
			"tnvse_native_overlay: LoadingMenuThread Update safety hook publication failed observed=0x%08X predecessor=0x%08X readable=%u; ShowChanges safety hook remains installed, prewarm consumer disabled",
			static_cast<UInt32>(observedUpdateTarget),
			static_cast<UInt32>(currentUpdateTarget),
			observedUpdateReadable ? 1u : 0u);
		return false;
	}

}

#include "font_prewarm_detail.h"
#include "font_prewarm_run_control.h"

#include <chrono>
#include <cstring>

namespace fonthook::vectorfont
{
	namespace implementation::font_prewarm
	{
		constexpr ULONGLONG kMainThreadAtlasQueueTimeoutMs = 5000;
		constexpr ULONGLONG kMainThreadAtlasExecutionTimeoutMs = 120000;
		constexpr ULONGLONG kPrewarmNoProgressTimeoutMs = 180000;
		constexpr ULONGLONG kPrewarmOverallTimeoutMs = 15ull * 60ull * 1000ull;
		constexpr ULONGLONG kPrewarmDiagnosticHeartbeatMs = 5000;
		constexpr DWORD kPrewarmWorkerExitGraceMs = 2000;

		enum class MainThreadRequestState : UInt8
		{
			Idle,
			Queued,
			Executing,
			Completed,
			Cancelled,
			Abandoned,
		};

		struct MainThreadAtlasRequest
		{
			std::mutex mutex;
			std::condition_variable condition;
			MainThreadRequestState state = MainThreadRequestState::Idle;
			PrewarmAtlasRequestKind kind = PrewarmAtlasRequestKind::LoadSnapshot;
			UInt64 nextToken = 1;
			UInt64 token = 0;
			UInt64 runToken = 0;
			UInt32 fontId = 0;
			float rasterScale = 1.0f;
			DWORD mainThreadId = 0;
			ULONGLONG queuedAt = 0;
			ULONGLONG executingAt = 0;
			ULONGLONG completedAt = 0;
			bool succeeded = false;
			bool memoryPressure = false;
			std::array<char, 160> error{};
		};

		struct PrewarmCoordinatorState
		{
			std::mutex mutex;
			std::timed_mutex workerReapMutex;
			std::condition_variable condition;
			std::thread* worker = nullptr;
			std::deque<UInt32> pendingFontIds;
			std::unordered_set<UInt32> pendingFontSet;
			std::atomic_bool stopRequested{ false };
			PrewarmRunControl runControl;
			UInt64 nextRunToken = 1;
			UInt64 runToken = 0;
			DWORD mainThreadId = 0;
			FontPrewarmPumpStatus lastStatus = FontPrewarmPumpStatus::Idle;
			bool started = false;
			bool workRequested = false;
			bool settled = false;
			bool barrierClosed = false;
			bool exitRequested = false;
			bool workerExited = false;
			bool workerQuarantined = false;
		};

		MainThreadAtlasRequest& MainThreadRequest()
		{
			// Process-lifetime allocation avoids a joinable std::thread or condition
			// variable destructor running under the Windows loader lock.
			static MainThreadAtlasRequest* request = new MainThreadAtlasRequest;
			return *request;
		}

		PrewarmCoordinatorState& PrewarmCoordinator()
		{
			static PrewarmCoordinatorState* state = new PrewarmCoordinatorState;
			return *state;
		}

		void RequestFontPrewarmStop();

		const char* PrewarmAtlasRequestKindName(PrewarmAtlasRequestKind kind)
		{
			switch (kind)
			{
			case PrewarmAtlasRequestKind::LoadSnapshot:
				return "load-snapshot";
			case PrewarmAtlasRequestKind::LoadSharedDoubleByteRole:
				return "load-shared-double-byte-role";
			case PrewarmAtlasRequestKind::RebuildPublishedSnapshot:
				return "rebuild-published-snapshot";
			case PrewarmAtlasRequestKind::PruneRetiredGenerations:
				return "prune-retired-generations";
			default:
				return "unknown";
			}
		}

		bool ExecuteMainThreadAtlasOperation(PrewarmAtlasRequestKind kind,
			UInt32 fontId, float rasterScale, bool& memoryPressure,
			std::array<char, 160>& error)
		{
			bool succeeded = false;
			memoryPressure = false;
			error.fill('\0');
			ResetAtlasAllocationMemoryPressure();
			try
			{
				RuntimeFont* runtime = nullptr;
				if (kind == PrewarmAtlasRequestKind::LoadSnapshot
					|| kind == PrewarmAtlasRequestKind::LoadSharedDoubleByteRole
					|| kind == PrewarmAtlasRequestKind::RebuildPublishedSnapshot)
				{
					runtime = FindRuntimeFont(fontId);
					if (!runtime)
					{
						strcpy_s(error.data(), error.size(), "runtime-unavailable");
						return false;
					}
				}

				switch (kind)
				{
				case PrewarmAtlasRequestKind::LoadSnapshot:
					succeeded = TryLoadGloballyRepackedGlyphAtlasSnapshot(
						*runtime, rasterScale);
					break;
				case PrewarmAtlasRequestKind::LoadSharedDoubleByteRole:
					succeeded = TryLoadGloballyRepackedGlyphAtlasSnapshotRole(
						*runtime, VectorFontByteClass::DoubleByte, rasterScale);
					break;
				case PrewarmAtlasRequestKind::RebuildPublishedSnapshot:
					succeeded = RebuildGlyphAtlasFromSnapshot(*runtime, rasterScale);
					break;
				case PrewarmAtlasRequestKind::PruneRetiredGenerations:
					PruneRetiredAtlasGenerationsSafely();
					succeeded = true;
					break;
				}
			}
			catch (const std::bad_alloc&)
			{
				memoryPressure = true;
				strcpy_s(error.data(), error.size(), "bad-alloc");
			}
			catch (const std::exception& exception)
			{
				strncpy_s(error.data(), error.size(), exception.what(), _TRUNCATE);
			}
			catch (...)
			{
				strcpy_s(error.data(), error.size(), "unknown-exception");
			}
			memoryPressure = ConsumeAtlasAllocationMemoryPressure()
				|| memoryPressure;
			return succeeded;
		}

		bool ExecutePrewarmAtlasRequestOnMainThread(
			PrewarmAtlasRequestKind kind, UInt32 fontId, float rasterScale,
			PrewarmAtlasRequestResult& result)
		{
			result = {};
			PrewarmCoordinatorState& coordinator = PrewarmCoordinator();
			const UInt64 runToken = coordinator.runControl.ActiveToken();
			if (IsFontPrewarmStopRequested()
				|| !coordinator.runControl.CanCommit(runToken))
				return false;

			MainThreadAtlasRequest& request = MainThreadRequest();
			UInt64 token = 0;
			const ULONGLONG queuedAt = GetTickCount64();
			{
				std::lock_guard<std::mutex> lock(request.mutex);
				if (request.state != MainThreadRequestState::Idle)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: main-thread atlas request rejected operation=%s font=%u reason=request-already-active state=%u",
						PrewarmAtlasRequestKindName(kind), fontId,
						static_cast<UInt32>(request.state));
					return false;
				}
				token = request.nextToken++;
				if (!request.nextToken)
					request.nextToken = 1;
				request.token = token;
				request.runToken = runToken;
				request.kind = kind;
				request.fontId = fontId;
				request.rasterScale = rasterScale;
				request.mainThreadId = 0;
				request.queuedAt = queuedAt;
				request.executingAt = 0;
				request.completedAt = 0;
				request.succeeded = false;
				request.memoryPressure = false;
				request.error.fill('\0');
				request.state = MainThreadRequestState::Queued;
			}

			std::array<char, 160> error{};
			DWORD mainThreadId = 0;
			const char* cancellationReason = nullptr;
			for (;;)
			{
				std::unique_lock<std::mutex> lock(request.mutex);
				request.condition.wait_for(lock, std::chrono::milliseconds(250),
					[&request, &coordinator, token, runToken]
					{
						return request.token != token
							|| (request.state != MainThreadRequestState::Queued
								&& request.state != MainThreadRequestState::Executing)
							|| coordinator.runControl.StopRequested(runToken);
					});
				if (request.token != token)
					return false;

				const ULONGLONG now = GetTickCount64();
				if (request.state == MainThreadRequestState::Queued
					&& now - request.queuedAt >= kMainThreadAtlasQueueTimeoutMs)
				{
					request.state = MainThreadRequestState::Cancelled;
					cancellationReason = "queue-timeout";
				}
				else if (request.state == MainThreadRequestState::Executing
					&& request.executingAt
					&& now - request.executingAt
						>= kMainThreadAtlasExecutionTimeoutMs)
				{
					request.state = MainThreadRequestState::Abandoned;
					cancellationReason = "execution-timeout";
				}
				else if (coordinator.runControl.StopRequested(runToken))
				{
					if (request.state == MainThreadRequestState::Queued)
						request.state = MainThreadRequestState::Cancelled;
					else if (request.state == MainThreadRequestState::Executing)
						request.state = MainThreadRequestState::Abandoned;
					cancellationReason = "run-cancelled";
				}

				if (request.state == MainThreadRequestState::Cancelled
					|| request.state == MainThreadRequestState::Abandoned)
				{
					const bool queuedCancellation =
						request.state == MainThreadRequestState::Cancelled;
					if (queuedCancellation)
						request.state = MainThreadRequestState::Idle;
					const ULONGLONG queueMs = request.executingAt >= request.queuedAt
						? request.executingAt - request.queuedAt
						: now - request.queuedAt;
					const ULONGLONG executionMs = request.executingAt
						? now - request.executingAt : 0;
					lock.unlock();
					gLog.FormattedMessage(
						"tnvse_freetype_font: main-thread atlas service cancelled operation=%s font=%u token=%llu run=%llu reason=%s queueMs=%llu executeMs=%llu lateCompletionCommit=closed",
						PrewarmAtlasRequestKindName(kind), fontId,
						static_cast<unsigned long long>(token),
						static_cast<unsigned long long>(runToken),
						cancellationReason ? cancellationReason : "cancelled",
						static_cast<unsigned long long>(queueMs),
						static_cast<unsigned long long>(executionMs));
					if (cancellationReason
						&& strcmp(cancellationReason, "run-cancelled") != 0)
					{
						RequestFontPrewarmStop();
					}
					return false;
				}
				if (request.state == MainThreadRequestState::Idle)
					return false;
				if (request.state != MainThreadRequestState::Completed)
					continue;

				result.succeeded = request.succeeded;
				result.memoryPressure = request.memoryPressure;
				result.queueMs = request.executingAt >= request.queuedAt
					? request.executingAt - request.queuedAt : 0;
				result.executionMs = request.completedAt >= request.executingAt
					? request.completedAt - request.executingAt : 0;
				mainThreadId = request.mainThreadId;
				error = request.error;
				request.state = MainThreadRequestState::Idle;
				break;
			}

			gLog.FormattedMessage(
				"tnvse_freetype_font: main-thread atlas service completed operation=%s font=%u workerThread=%lu mainThread=%lu queueMs=%llu executeMs=%llu result=%s memoryPressure=%u error=%s",
				PrewarmAtlasRequestKindName(kind), fontId,
				static_cast<unsigned long>(GetCurrentThreadId()),
				static_cast<unsigned long>(mainThreadId),
				static_cast<unsigned long long>(result.queueMs),
				static_cast<unsigned long long>(result.executionMs),
				result.succeeded ? "complete" : "failed",
				result.memoryPressure ? 1u : 0u,
				error[0] ? error.data() : "none");
			return true;
		}

		void QueuePendingFontPrewarm(UInt32 fontId)
		{
			if (!FindConfig(fontId))
				return;
			PrewarmCoordinatorState& coordinator = PrewarmCoordinator();
			{
				std::lock_guard<std::mutex> lock(coordinator.mutex);
				if (coordinator.stopRequested.load(std::memory_order_acquire)
					|| coordinator.barrierClosed)
					return;
				if (coordinator.pendingFontSet.insert(fontId).second)
					coordinator.pendingFontIds.push_back(fontId);
				coordinator.workRequested = true;
				coordinator.settled = false;
			}
			coordinator.condition.notify_all();
		}

		void DrainPendingFontPrewarms()
		{
			std::deque<UInt32> pending;
			PrewarmCoordinatorState& coordinator = PrewarmCoordinator();
			{
				std::lock_guard<std::mutex> lock(coordinator.mutex);
				pending.swap(coordinator.pendingFontIds);
				coordinator.pendingFontSet.clear();
			}
			for (UInt32 fontId : pending)
			{
				if (IsFontPrewarmStopRequested())
					break;
				QueueFontPrewarmOwned(fontId);
			}
		}

		bool IsFontPrewarmStopRequested()
		{
			return PrewarmCoordinator().stopRequested.load(
				std::memory_order_acquire);
		}

		void FontPrewarmWorkerMain(UInt64 runToken)
		{
			PrewarmCoordinatorState& coordinator = PrewarmCoordinator();
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			coordinator.runControl.Beat(runToken);
			gLog.FormattedMessage(
				"tnvse_freetype_font: pre-entry prewarm coordinator started workerThread=%lu run=%llu priority=below-normal presentation=LoadingMenuThread mainThreadSubmit=barrier-service",
				static_cast<unsigned long>(GetCurrentThreadId()),
				static_cast<unsigned long long>(runToken));

			for (;;)
			{
				bool failed = false;
				{
					std::unique_lock<std::mutex> lock(coordinator.mutex);
					const bool ready = coordinator.condition.wait_for(lock,
						std::chrono::milliseconds(250), [&coordinator, runToken]
					{
						return coordinator.stopRequested.load(
								std::memory_order_acquire)
							|| coordinator.runControl.StopRequested(runToken)
							|| coordinator.exitRequested
							|| coordinator.workRequested
							|| !coordinator.pendingFontIds.empty();
					});
					if (!ready)
						continue;
					if (coordinator.stopRequested.load(std::memory_order_acquire)
						|| coordinator.runControl.StopRequested(runToken)
						|| coordinator.exitRequested)
					{
						break;
					}
					coordinator.workRequested = false;
					coordinator.settled = false;
				}

				for (;;)
				{
					if (IsFontPrewarmStopRequested()
						|| coordinator.runControl.StopRequested(runToken))
						break;
					const FontPrewarmPumpStatus status = PumpFontPrewarm();
					coordinator.runControl.Beat(runToken);
					bool pending = false;
					{
						std::lock_guard<std::mutex> lock(coordinator.mutex);
						coordinator.lastStatus = status;
						pending = coordinator.workRequested
							|| !coordinator.pendingFontIds.empty();
						coordinator.settled =
							status == FontPrewarmPumpStatus::Failed
							|| (status != FontPrewarmPumpStatus::Active
								&& !pending);
					}
					coordinator.condition.notify_all();
					if (status == FontPrewarmPumpStatus::Failed)
					{
						failed = true;
						break;
					}
					if (status == FontPrewarmPumpStatus::Active)
					{
						Sleep(1);
						continue;
					}
					if (pending)
					{
						{
							std::lock_guard<std::mutex> lock(coordinator.mutex);
							coordinator.workRequested = false;
							coordinator.settled = false;
						}
						continue;
					}
					break;
				}
				if (IsFontPrewarmStopRequested()
					|| coordinator.runControl.StopRequested(runToken) || failed)
					break;
			}

			{
				std::lock_guard<std::mutex> lock(coordinator.mutex);
				if (IsFontPrewarmStopRequested()
					|| coordinator.runControl.StopRequested(runToken))
				{
					coordinator.lastStatus = FontPrewarmPumpStatus::Failed;
					coordinator.settled = true;
				}
				coordinator.workerExited = true;
			}
			gLog.FormattedMessage(
				"tnvse_freetype_font: pre-entry prewarm coordinator stopped workerThread=%lu run=%llu stopRequested=%u",
				static_cast<unsigned long>(GetCurrentThreadId()),
				static_cast<unsigned long long>(runToken),
				IsFontPrewarmStopRequested() ? 1u : 0u);
			coordinator.runControl.MarkExited(runToken);
			coordinator.condition.notify_all();
		}

		const char* MainThreadRequestStateName(
			MainThreadRequestState state) noexcept
		{
			switch (state)
			{
			case MainThreadRequestState::Idle:
				return "idle";
			case MainThreadRequestState::Queued:
				return "queued";
			case MainThreadRequestState::Executing:
				return "executing";
			case MainThreadRequestState::Completed:
				return "completed";
			case MainThreadRequestState::Cancelled:
				return "cancelled";
			case MainThreadRequestState::Abandoned:
				return "abandoned";
			default:
				return "unknown";
			}
		}

		void StartFontPrewarmWorker()
		{
			if (!g_bEnableFreeTypeFontRendering)
				return;
			PrewarmCoordinatorState& coordinator = PrewarmCoordinator();
			const DWORD currentThreadId = GetCurrentThreadId();
			{
				std::lock_guard<std::mutex> lock(coordinator.mutex);
				if (!coordinator.mainThreadId)
					coordinator.mainThreadId = currentThreadId;
				if (coordinator.mainThreadId != currentThreadId
					|| coordinator.stopRequested.load(std::memory_order_acquire))
				{
					return;
				}
				if (coordinator.barrierClosed)
					return;
				if (coordinator.started)
					return;
				coordinator.started = true;
				coordinator.stopRequested.store(false, std::memory_order_release);
				coordinator.workRequested = true;
				coordinator.settled = false;
				coordinator.barrierClosed = false;
				coordinator.exitRequested = false;
				coordinator.workerExited = false;
				coordinator.workerQuarantined = false;
				coordinator.lastStatus = FontPrewarmPumpStatus::Active;
				coordinator.runToken = coordinator.nextRunToken++;
				if (!coordinator.nextRunToken)
					coordinator.nextRunToken = 1;
				coordinator.runControl.Begin(coordinator.runToken);
				try
				{
					coordinator.worker = new std::thread(
						&FontPrewarmWorkerMain, coordinator.runToken);
				}
				catch (...)
				{
					delete coordinator.worker;
					coordinator.worker = nullptr;
					coordinator.started = false;
					coordinator.workerExited = true;
					coordinator.settled = true;
					coordinator.lastStatus = FontPrewarmPumpStatus::Failed;
					coordinator.runControl.RequestStop(coordinator.runToken);
					coordinator.runControl.MarkExited(coordinator.runToken);
					gLog.FormattedMessage(
						"tnvse_freetype_font: pre-entry prewarm coordinator creation failed; runtime demand fallback remains available");
				}
			}
		}

		void ServiceFontPrewarmMainThread()
		{
			PrewarmCoordinatorState& coordinator = PrewarmCoordinator();
			const DWORD currentThreadId = GetCurrentThreadId();
			{
				std::lock_guard<std::mutex> lock(coordinator.mutex);
				if (!coordinator.mainThreadId
					|| coordinator.mainThreadId != currentThreadId
					|| coordinator.stopRequested.load(std::memory_order_acquire))
				{
					return;
				}
			}

			MainThreadAtlasRequest& request = MainThreadRequest();
			PrewarmAtlasRequestKind kind = PrewarmAtlasRequestKind::LoadSnapshot;
			UInt64 token = 0;
			UInt64 runToken = 0;
			UInt32 fontId = 0;
			float rasterScale = 1.0f;
			ULONGLONG queuedAt = 0;
			{
				std::lock_guard<std::mutex> lock(request.mutex);
				if (request.state != MainThreadRequestState::Queued)
					return;
				kind = request.kind;
				token = request.token;
				runToken = request.runToken;
				fontId = request.fontId;
				rasterScale = request.rasterScale;
				queuedAt = request.queuedAt;
			}

			{
				std::lock_guard<std::mutex> lock(request.mutex);
				if (request.state != MainThreadRequestState::Queued
					|| request.token != token
					|| !coordinator.runControl.CanCommit(runToken))
				{
					if (request.state == MainThreadRequestState::Queued
						&& request.token == token)
					{
						request.state = MainThreadRequestState::Cancelled;
						request.condition.notify_all();
					}
					return;
				}
				request.state = MainThreadRequestState::Executing;
				request.mainThreadId = currentThreadId;
				request.executingAt = GetTickCount64();
			}
			gLog.FormattedMessage(
				"tnvse_freetype_font: main-thread atlas service started operation=%s font=%u token=%llu run=%llu mainThread=%lu queueMs=%llu policy=pre-entry-barrier-service",
				PrewarmAtlasRequestKindName(kind), fontId,
				static_cast<unsigned long long>(token),
				static_cast<unsigned long long>(runToken),
				static_cast<unsigned long>(currentThreadId),
				static_cast<unsigned long long>(GetTickCount64() - queuedAt));

			bool memoryPressure = false;
			std::array<char, 160> error{};
			const bool succeeded = ExecuteMainThreadAtlasOperation(
				kind, fontId, rasterScale, memoryPressure, error);
			bool discarded = false;
			ULONGLONG executionMs = 0;
			{
				std::lock_guard<std::mutex> lock(request.mutex);
				if (request.token != token)
				{
					return;
				}
				if (request.state == MainThreadRequestState::Abandoned
					|| !coordinator.runControl.CanCommit(runToken))
				{
					request.completedAt = GetTickCount64();
					executionMs = request.completedAt - request.executingAt;
					request.state = MainThreadRequestState::Idle;
					discarded = true;
				}
				else if (request.state != MainThreadRequestState::Executing)
				{
					return;
				}
				else
				{
					request.succeeded = succeeded;
					request.memoryPressure = memoryPressure;
					request.error = error;
					request.completedAt = GetTickCount64();
					request.state = MainThreadRequestState::Completed;
				}
			}
			request.condition.notify_all();
			if (discarded)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: main-thread atlas late completion discarded operation=%s font=%u token=%llu run=%llu executeMs=%llu commitGate=closed",
					PrewarmAtlasRequestKindName(kind), fontId,
					static_cast<unsigned long long>(token),
					static_cast<unsigned long long>(runToken),
					static_cast<unsigned long long>(executionMs));
			}
		}

		const char* FontPrewarmPumpStatusName(FontPrewarmPumpStatus status)
		{
			switch (status)
			{
			case FontPrewarmPumpStatus::Idle:
				return "idle";
			case FontPrewarmPumpStatus::Active:
				return "active";
			case FontPrewarmPumpStatus::Completed:
				return "completed";
			case FontPrewarmPumpStatus::Failed:
				return "failed";
			default:
				return "unknown";
			}
		}

		bool PumpPrewarmBarrierWindowMessages(int& quitCode)
		{
			constexpr UInt32 kMaximumMessagesPerIteration = 32;
			MSG message = {};
			for (UInt32 count = 0; count < kMaximumMessagesPerIteration; ++count)
			{
				if (!PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
					break;
				if (message.message == WM_QUIT)
				{
					quitCode = static_cast<int>(message.wParam);
					return true;
				}
				TranslateMessage(&message);
				DispatchMessageA(&message);
			}
			return false;
		}

		void RequestFontPrewarmStop()
		{
			PrewarmCoordinatorState& coordinator = PrewarmCoordinator();
			coordinator.stopRequested.store(true, std::memory_order_release);
			coordinator.runControl.RequestStop(
				coordinator.runControl.ActiveToken());
			MainThreadAtlasRequest& request = MainThreadRequest();
			{
				std::lock_guard<std::mutex> lock(request.mutex);
				if (request.state == MainThreadRequestState::Queued)
					request.state = MainThreadRequestState::Cancelled;
				else if (request.state == MainThreadRequestState::Executing)
					request.state = MainThreadRequestState::Abandoned;
			}
			request.condition.notify_all();
			coordinator.condition.notify_all();
		}

		const char* PrewarmWatchdogReasonName(PrewarmWatchdogReason reason)
		{
			switch (reason)
			{
			case PrewarmWatchdogReason::NoProgress:
				return "no-progress";
			case PrewarmWatchdogReason::OverallDeadline:
				return "overall-deadline";
			default:
				return "none";
			}
		}

		bool ReapFontPrewarmWorker(DWORD timeoutMs, bool pumpWindowMessages,
			bool& quitRequested, int& quitCode)
		{
			PrewarmCoordinatorState& coordinator = PrewarmCoordinator();
			std::unique_lock<std::timed_mutex> reapLock(
				coordinator.workerReapMutex, std::defer_lock);
			if (!reapLock.try_lock_for(std::chrono::milliseconds(timeoutMs)))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm worker reap arbitration timed out timeoutMs=%u policy=leave-process-lifetime-thread-owned",
					timeoutMs);
				return false;
			}

			std::thread* worker = nullptr;
			UInt64 runToken = 0;
			{
				std::lock_guard<std::mutex> lock(coordinator.mutex);
				worker = coordinator.worker;
				runToken = coordinator.runToken;
			}
			if (!worker)
				return true;

			const ULONGLONG startedAt = GetTickCount64();
			bool signalled = false;
			for (;;)
			{
				const ULONGLONG elapsed = GetTickCount64() - startedAt;
				if (elapsed >= timeoutMs)
					break;
				const DWORD slice = static_cast<DWORD>(std::min<ULONGLONG>(
					8, timeoutMs - elapsed));
				const DWORD waitResult = WaitForSingleObject(
					worker->native_handle(), slice);
				if (waitResult == WAIT_OBJECT_0)
				{
					signalled = true;
					break;
				}
				if (waitResult == WAIT_FAILED)
					break;
				if (pumpWindowMessages && !quitRequested
					&& PumpPrewarmBarrierWindowMessages(quitCode))
				{
					quitRequested = true;
					RequestFontPrewarmStop();
				}
			}

			if (!signalled)
			{
				std::lock_guard<std::mutex> lock(coordinator.mutex);
				coordinator.workerQuarantined = true;
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm worker exit acknowledgement timed out run=%llu timeoutMs=%u heartbeat=%llu policy=no-join-process-lifetime-quarantine",
					static_cast<unsigned long long>(runToken), timeoutMs,
					static_cast<unsigned long long>(
						coordinator.runControl.Heartbeat()));
				return false;
			}

			{
				std::lock_guard<std::mutex> lock(coordinator.mutex);
				if (coordinator.worker != worker)
					return false;
				coordinator.worker = nullptr;
				coordinator.started = false;
				coordinator.workerExited = true;
				coordinator.workerQuarantined = false;
			}
			if (worker->joinable())
				worker->join();
			delete worker;
			return true;
		}

		FontPrewarmPumpStatus RunFontPrewarmLoadingBarrier()
		{
			if (!g_bEnableFreeTypeFontRendering)
				return FontPrewarmPumpStatus::Idle;

			StartFontPrewarmWorker();
			PrewarmCoordinatorState& coordinator = PrewarmCoordinator();
			const DWORD currentThreadId = GetCurrentThreadId();
			const ULONGLONG startedAt = GetTickCount64();
			FontPrewarmPumpStatus status = FontPrewarmPumpStatus::Failed;
			UInt64 runToken = 0;
			bool quitRequested = false;
			int quitCode = 0;
			{
				std::lock_guard<std::mutex> lock(coordinator.mutex);
				if (coordinator.barrierClosed)
					return coordinator.lastStatus;
				if (!coordinator.started || !coordinator.worker
					|| coordinator.mainThreadId != currentThreadId)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: DeferredInit pre-entry prewarm barrier unavailable mainThread=%lu configuredMainThread=%lu started=%u worker=%u policy=continue-with-runtime-demand",
						static_cast<unsigned long>(currentThreadId),
						static_cast<unsigned long>(coordinator.mainThreadId),
						coordinator.started ? 1u : 0u,
						coordinator.worker ? 1u : 0u);
					coordinator.barrierClosed = true;
					return FontPrewarmPumpStatus::Failed;
				}
				runToken = coordinator.runToken;
			}

			gLog.FormattedMessage(
				"tnvse_freetype_font: DeferredInit pre-entry prewarm barrier begin mainThread=%lu run=%llu noProgressTimeoutMs=%llu overallTimeoutMs=%llu policy=worker-cpu-main-thread-d3d-loading-thread-ui",
				static_cast<unsigned long>(currentThreadId),
				static_cast<unsigned long long>(runToken),
				static_cast<unsigned long long>(kPrewarmNoProgressTimeoutMs),
				static_cast<unsigned long long>(kPrewarmOverallTimeoutMs));
			PrewarmProgressWatchdog watchdog(startedAt,
				coordinator.runControl.Heartbeat(), kPrewarmNoProgressTimeoutMs,
				kPrewarmOverallTimeoutMs);
			UInt32 iterations = 0;
			ULONGLONG nextDiagnosticAt =
				startedAt + kPrewarmDiagnosticHeartbeatMs;
			PrewarmWatchdogReason watchdogReason = PrewarmWatchdogReason::None;
			for (;;)
			{
				if (!quitRequested)
				{
					quitRequested = PumpPrewarmBarrierWindowMessages(quitCode);
					if (quitRequested)
						RequestFontPrewarmStop();
				}
				ServiceFontPrewarmMainThread();

				bool settled = false;
				{
					std::lock_guard<std::mutex> lock(coordinator.mutex);
					settled = coordinator.settled;
					status = coordinator.lastStatus;
				}
				if (settled)
					break;

				const ULONGLONG now = GetTickCount64();
				if (g_bEnableFreeTypeFontRenderingLog
					&& now >= nextDiagnosticAt)
				{
					MainThreadRequestState requestState =
						MainThreadRequestState::Idle;
					PrewarmAtlasRequestKind requestKind =
						PrewarmAtlasRequestKind::LoadSnapshot;
					UInt64 requestToken = 0;
					UInt64 requestRunToken = 0;
					UInt32 requestFontId = 0;
					ULONGLONG requestQueuedAt = 0;
					ULONGLONG requestExecutingAt = 0;
					{
						MainThreadAtlasRequest& request = MainThreadRequest();
						std::lock_guard<std::mutex> lock(request.mutex);
						requestState = request.state;
						requestKind = request.kind;
						requestToken = request.token;
						requestRunToken = request.runToken;
						requestFontId = request.fontId;
						requestQueuedAt = request.queuedAt;
						requestExecutingAt = request.executingAt;
					}
					gLog.FormattedMessage(
						"tnvse_freetype_font: DeferredInit pre-entry prewarm heartbeat run=%llu elapsedMs=%llu stalledMs=%llu heartbeat=%llu status=%s settled=%u request={state:%s,kind:%s,font:%u,token:%llu,run:%llu,queueAgeMs:%llu,executionAgeMs:%llu} loadingMenuSnapshot=next-line",
						static_cast<unsigned long long>(runToken),
						static_cast<unsigned long long>(now - startedAt),
						static_cast<unsigned long long>(
							now - watchdog.LastProgressAt()),
						static_cast<unsigned long long>(
							coordinator.runControl.Heartbeat()),
						FontPrewarmPumpStatusName(status),
						settled ? 1u : 0u,
						MainThreadRequestStateName(requestState),
						PrewarmAtlasRequestKindName(requestKind),
						requestFontId,
						static_cast<unsigned long long>(requestToken),
						static_cast<unsigned long long>(requestRunToken),
						static_cast<unsigned long long>(requestQueuedAt
							? now - requestQueuedAt : 0),
						static_cast<unsigned long long>(requestExecutingAt
							? now - requestExecutingAt : 0));
					LogNativeLoadingMenuDiagnostic(
						"prewarm-barrier-heartbeat");
					nextDiagnosticAt = now + kPrewarmDiagnosticHeartbeatMs;
				}

				watchdogReason = watchdog.Observe(now,
					coordinator.runControl.Heartbeat());
				if (watchdogReason != PrewarmWatchdogReason::None)
				{
					status = FontPrewarmPumpStatus::Failed;
					gLog.FormattedMessage(
						"tnvse_freetype_font: DeferredInit pre-entry prewarm watchdog fired run=%llu reason=%s elapsedMs=%llu stalledMs=%llu heartbeat=%llu policy=cancel-rollback-runtime-demand",
						static_cast<unsigned long long>(runToken),
						PrewarmWatchdogReasonName(watchdogReason),
						static_cast<unsigned long long>(
							GetTickCount64() - startedAt),
						static_cast<unsigned long long>(
							GetTickCount64() - watchdog.LastProgressAt()),
						static_cast<unsigned long long>(
							coordinator.runControl.Heartbeat()));
					RequestFontPrewarmStop();
					break;
				}

				++iterations;
				if ((iterations & 0x3Fu) == 0)
					FlushFreeTypeFontDebugLog();
				MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT,
					MWMO_INPUTAVAILABLE | MWMO_ALERTABLE);
			}

			{
				std::lock_guard<std::mutex> lock(coordinator.mutex);
				coordinator.barrierClosed = true;
				coordinator.exitRequested = true;
			}
			coordinator.runControl.CloseCommit(runToken);
			coordinator.condition.notify_all();
			const bool workerReaped = ReapFontPrewarmWorker(
				kPrewarmWorkerExitGraceMs, true, quitRequested, quitCode);
			if (!workerReaped)
			{
				RequestFontPrewarmStop();
				status = FontPrewarmPumpStatus::Failed;
			}
			else if (watchdogReason != PrewarmWatchdogReason::None
				|| quitRequested || IsFontPrewarmStopRequested())
			{
				// The worker owns PrewarmRuntime while active.  Roll back only after
				// the native thread handle is signalled and join therefore cannot wait.
				ShutdownFontPrewarm();
			}

			if (quitRequested)
				PostQuitMessage(quitCode);
			gLog.FormattedMessage(
				"tnvse_freetype_font: DeferredInit pre-entry prewarm barrier end status=%s run=%llu iterations=%u elapsedMs=%llu quitRequested=%u watchdog=%s workerReaped=%u runtimeFallback=%u",
				FontPrewarmPumpStatusName(status),
				static_cast<unsigned long long>(runToken), iterations,
				static_cast<unsigned long long>(GetTickCount64() - startedAt),
				quitRequested ? 1u : 0u,
				PrewarmWatchdogReasonName(watchdogReason),
				workerReaped ? 1u : 0u,
				status == FontPrewarmPumpStatus::Failed ? 1u : 0u);
			return status;
		}

		void ShutdownFontPrewarmWorker()
		{
			PrewarmCoordinatorState& coordinator = PrewarmCoordinator();
			RequestFontPrewarmStop();
			{
				std::lock_guard<std::mutex> lock(coordinator.mutex);
				coordinator.exitRequested = true;
			}
			coordinator.condition.notify_all();
			bool quitRequested = false;
			int quitCode = 0;
			if (ReapFontPrewarmWorker(kPrewarmWorkerExitGraceMs, false,
				quitRequested, quitCode))
			{
				ShutdownFontPrewarm();
			}
			else
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm shutdown left an unresponsive process-lifetime worker quarantined; shared prewarm state is intentionally retained");
			}
		}
	}

	using namespace implementation::font_prewarm;

	FontPrewarmPumpStatus RunFontPrewarmLoadingBarrier()
	{
		return implementation::font_prewarm::RunFontPrewarmLoadingBarrier();
	}

	bool IsFontPrewarmStopRequested()
	{
		return implementation::font_prewarm::IsFontPrewarmStopRequested();
	}

	void RequestFontPrewarmStop()
	{
		implementation::font_prewarm::RequestFontPrewarmStop();
	}

	void ShutdownFontPrewarmWorker()
	{
		implementation::font_prewarm::ShutdownFontPrewarmWorker();
	}
}

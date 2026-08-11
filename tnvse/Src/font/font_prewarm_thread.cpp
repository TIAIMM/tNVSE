#include "font_prewarm_detail.h"

#include <chrono>
#include <cstring>

namespace fonthook::vectorfont
{
	namespace implementation::font_prewarm
	{
		enum class MainThreadRequestState : UInt8
		{
			Idle,
			Queued,
			Executing,
			Completed,
			Cancelled,
		};

		struct MainThreadAtlasRequest
		{
			std::mutex mutex;
			std::condition_variable condition;
			MainThreadRequestState state = MainThreadRequestState::Idle;
			PrewarmAtlasRequestKind kind = PrewarmAtlasRequestKind::LoadSnapshot;
			UInt64 nextToken = 1;
			UInt64 token = 0;
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
			std::condition_variable condition;
			std::thread* worker = nullptr;
			std::deque<UInt32> pendingFontIds;
			std::unordered_set<UInt32> pendingFontSet;
			std::atomic_bool stopRequested{ false };
			DWORD mainThreadId = 0;
			FontPrewarmPumpStatus lastStatus = FontPrewarmPumpStatus::Idle;
			bool started = false;
			bool workRequested = false;
			bool settled = false;
			bool barrierClosed = false;
			bool exitRequested = false;
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
			if (IsFontPrewarmStopRequested())
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
			for (;;)
			{
				std::unique_lock<std::mutex> lock(request.mutex);
				request.condition.wait_for(lock, std::chrono::milliseconds(250),
					[&request, token]
					{
						return request.token != token
							|| request.state == MainThreadRequestState::Completed
							|| request.state == MainThreadRequestState::Cancelled;
					});
				if (request.token != token
					|| request.state == MainThreadRequestState::Cancelled)
				{
					if (request.token == token)
						request.state = MainThreadRequestState::Idle;
					return false;
				}
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

		void FontPrewarmWorkerMain()
		{
			PrewarmCoordinatorState& coordinator = PrewarmCoordinator();
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			gLog.FormattedMessage(
				"tnvse_freetype_font: pre-entry prewarm coordinator started workerThread=%lu priority=below-normal presentation=LoadingMenuThread mainThreadSubmit=barrier-service",
				static_cast<unsigned long>(GetCurrentThreadId()));

			for (;;)
			{
				bool failed = false;
				{
					std::unique_lock<std::mutex> lock(coordinator.mutex);
					coordinator.condition.wait(lock, [&coordinator]
					{
						return coordinator.stopRequested.load(
								std::memory_order_acquire)
							|| coordinator.exitRequested
							|| coordinator.workRequested
							|| !coordinator.pendingFontIds.empty();
					});
					if (coordinator.stopRequested.load(std::memory_order_acquire)
						|| coordinator.exitRequested)
					{
						break;
					}
					coordinator.workRequested = false;
					coordinator.settled = false;
				}

				for (;;)
				{
					if (IsFontPrewarmStopRequested())
						break;
					const FontPrewarmPumpStatus status = PumpFontPrewarm();
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
				if (IsFontPrewarmStopRequested() || failed)
					break;
			}

			{
				std::lock_guard<std::mutex> lock(coordinator.mutex);
				if (IsFontPrewarmStopRequested())
				{
					coordinator.lastStatus = FontPrewarmPumpStatus::Failed;
					coordinator.settled = true;
				}
			}
			coordinator.condition.notify_all();
			gLog.FormattedMessage(
				"tnvse_freetype_font: pre-entry prewarm coordinator stopped workerThread=%lu stopRequested=%u",
				static_cast<unsigned long>(GetCurrentThreadId()),
				IsFontPrewarmStopRequested() ? 1u : 0u);
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
				coordinator.workRequested = true;
				coordinator.settled = false;
				coordinator.barrierClosed = false;
				coordinator.exitRequested = false;
				coordinator.lastStatus = FontPrewarmPumpStatus::Active;
				try
				{
					coordinator.worker = new std::thread(&FontPrewarmWorkerMain);
				}
				catch (...)
				{
					delete coordinator.worker;
					coordinator.worker = nullptr;
					coordinator.started = false;
					coordinator.settled = true;
					coordinator.lastStatus = FontPrewarmPumpStatus::Failed;
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
			UInt32 fontId = 0;
			float rasterScale = 1.0f;
			ULONGLONG queuedAt = 0;
			{
				std::lock_guard<std::mutex> lock(request.mutex);
				if (request.state != MainThreadRequestState::Queued)
					return;
				kind = request.kind;
				token = request.token;
				fontId = request.fontId;
				rasterScale = request.rasterScale;
				queuedAt = request.queuedAt;
			}

			{
				std::lock_guard<std::mutex> lock(request.mutex);
				if (request.state != MainThreadRequestState::Queued
					|| request.token != token)
				{
					return;
				}
				request.state = MainThreadRequestState::Executing;
				request.mainThreadId = currentThreadId;
				request.executingAt = GetTickCount64();
			}
			gLog.FormattedMessage(
				"tnvse_freetype_font: main-thread atlas service started operation=%s font=%u mainThread=%lu queueMs=%llu policy=pre-entry-barrier-service",
				PrewarmAtlasRequestKindName(kind), fontId,
				static_cast<unsigned long>(currentThreadId),
				static_cast<unsigned long long>(GetTickCount64() - queuedAt));

			bool memoryPressure = false;
			std::array<char, 160> error{};
			const bool succeeded = ExecuteMainThreadAtlasOperation(
				kind, fontId, rasterScale, memoryPressure, error);
			{
				std::lock_guard<std::mutex> lock(request.mutex);
				if (request.state != MainThreadRequestState::Executing
					|| request.token != token)
				{
					return;
				}
				request.succeeded = succeeded;
				request.memoryPressure = memoryPressure;
				request.error = error;
				request.completedAt = GetTickCount64();
				request.state = MainThreadRequestState::Completed;
			}
			request.condition.notify_all();
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
			MainThreadAtlasRequest& request = MainThreadRequest();
			{
				std::lock_guard<std::mutex> lock(request.mutex);
				if (request.state == MainThreadRequestState::Queued)
					request.state = MainThreadRequestState::Cancelled;
			}
			request.condition.notify_all();
			coordinator.condition.notify_all();
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
			}

			gLog.FormattedMessage(
				"tnvse_freetype_font: DeferredInit pre-entry prewarm barrier begin mainThread=%lu policy=worker-cpu-main-thread-d3d-loading-thread-ui",
				static_cast<unsigned long>(currentThreadId));
			UInt32 iterations = 0;
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

				++iterations;
				if ((iterations & 0x3Fu) == 0)
					FlushFreeTypeFontDebugLog();
				MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT,
					MWMO_INPUTAVAILABLE | MWMO_ALERTABLE);
			}

			std::thread* worker = nullptr;
			{
				std::lock_guard<std::mutex> lock(coordinator.mutex);
				coordinator.barrierClosed = true;
				coordinator.exitRequested = true;
				worker = coordinator.worker;
				coordinator.worker = nullptr;
				coordinator.started = false;
			}
			coordinator.condition.notify_all();
			if (worker)
			{
				if (worker->joinable())
					worker->join();
				delete worker;
			}

			if (quitRequested)
				PostQuitMessage(quitCode);
			gLog.FormattedMessage(
				"tnvse_freetype_font: DeferredInit pre-entry prewarm barrier end status=%s iterations=%u elapsedMs=%llu quitRequested=%u",
				FontPrewarmPumpStatusName(status), iterations,
				static_cast<unsigned long long>(GetTickCount64() - startedAt),
				quitRequested ? 1u : 0u);
			return status;
		}

		void ShutdownFontPrewarmWorker()
		{
			PrewarmCoordinatorState& coordinator = PrewarmCoordinator();
			RequestFontPrewarmStop();

			std::thread* worker = nullptr;
			{
				std::lock_guard<std::mutex> lock(coordinator.mutex);
				worker = coordinator.worker;
				coordinator.worker = nullptr;
			}
			if (worker)
			{
				if (worker->joinable())
					worker->join();
				delete worker;
			}
			ShutdownFontPrewarm();
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

	void ShutdownFontPrewarmWorker()
	{
		implementation::font_prewarm::ShutdownFontPrewarmWorker();
	}
}

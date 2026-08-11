#include "font_prewarm_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_prewarm {}
	using namespace implementation::font_prewarm;

	namespace implementation::font_prewarm
	{
		PrewarmRuntimeState& PrewarmRuntime()
		{
			static PrewarmRuntimeState state;
			return state;
		}

		PrewarmThreadState& PrewarmThread()
		{
			thread_local PrewarmThreadState state;
			return state;
		}
	}

	bool TryDispatchPrewarmAtlasRebuildToMainThread(
		RuntimeFont& runtime, float rasterScale, bool& result)
	{
		result = false;
		const DWORD workerThreadId = PrewarmRuntime().prewarmWorkerThreadId.load(
			std::memory_order_acquire);
		if (!PrewarmRuntime().prewarmActive.load(std::memory_order_acquire)
			|| !workerThreadId || GetCurrentThreadId() != workerThreadId)
		{
			return false;
		}
		PrewarmAtlasRequestResult requestResult;
		const UInt32 fontId = GetRuntimeConfig(runtime).fontId;
		if (!ExecutePrewarmAtlasRequestOnMainThread(
				PrewarmAtlasRequestKind::RebuildPublishedSnapshot,
				fontId, rasterScale, requestResult))
		{
			return true;
		}
		if (requestResult.memoryPressure)
			MarkAtlasAllocationMemoryPressure();
		result = requestResult.succeeded;
		return true;
	}

	void implementation::font_prewarm::QueueFontPrewarmOwned(UInt32 fontId)
	{
		const FontConfig* config = FindConfig(fontId);
		if (!config)
			return;
		if (!EnsureRuntimeFont(fontId))
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm deferred because runtime initialization failed font=%u",
				fontId);
			return;
		}

		const FontAtlasRoute route = GetPersistentFontCacheRoute();
		const UInt64 key = BuildProfileKey(*config, route);
		PrewarmJob job = BuildQueuedPrewarmJob(
			fontId, *config, route, key);
		if (!PrewarmRuntime().scheduledProfiles.insert(key).second)
		{
			const auto shared = std::find_if(PrewarmRuntime().jobs.begin(), PrewarmRuntime().jobs.end(),
				[config](const PrewarmJob& job)
				{
					return MatchesPrewarmProfile(job, *config);
				});
			if (shared != PrewarmRuntime().jobs.end())
			{
				if (IsPrewarmJobDoubleByteAlias(*shared)
					&& !IsPrewarmJobDoubleByteAlias(job))
				{
					const UInt32 replacedFontId = shared->fontId;
					*shared = std::move(job);
					gLog.FormattedMessage(
						"tnvse_freetype_font: prewarm profile owner preferred font=%u replacedAlias=%u",
						fontId, replacedFontId);
				}
				else
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: prewarm profile alias font=%u owner=%u",
						fontId, shared->fontId);
				}
			}
			return;
		}

		const FontPrewarmRange prewarmRange = job.prewarmRange;
		const UInt32 codePage = job.codePage;
		PrewarmRuntime().jobs.push_back(std::move(job));
		SetBitmapCacheReducedAfterPrewarm(false);
		gLog.FormattedMessage(
			"tnvse_freetype_font: queued prewarm font=%u coverage=direct-first codePage=%u prewarmEncoding=%s",
			fontId, codePage,
			GetFontPrewarmRangeName(prewarmRange, codePage));
	}

	void QueueFontPrewarm(UInt32 fontId)
	{
		QueuePendingFontPrewarm(fontId);
	}

	void QueueConfiguredFontPrewarms()
	{
		if (PrewarmRuntime().configuredFontsQueued)
			return;
		PrewarmRuntime().configuredFontsQueued = true;
		std::vector<UInt32> fontIds;
		fontIds.reserve(g_configs.size());
		for (const auto& entry : g_configs)
			fontIds.push_back(entry.first);
		std::sort(fontIds.begin(), fontIds.end(),
			[](UInt32 leftId, UInt32 rightId)
			{
				const FontConfig* left = FindConfig(leftId);
				const FontConfig* right = FindConfig(rightId);
				const bool leftAlias = left && IsMtsdfAtlasAlias(
					*left, VectorFontByteClass::DoubleByte);
				const bool rightAlias = right && IsMtsdfAtlasAlias(
					*right, VectorFontByteClass::DoubleByte);
				return leftAlias != rightAlias
					? !leftAlias : leftId < rightId;
			});
		for (UInt32 fontId : fontIds)
			QueueFontPrewarmOwned(fontId);
	}


	void ServiceFontPrewarmHostMessages()
	{
		// Long CPU/disk loops run on the below-normal-priority prewarm worker.
		// Yield here so the blocked DeferredInit thread can continue pumping the
		// native window queue and servicing serialized engine/D3D requests.
		SwitchToThread();
	}

	FontPrewarmPumpStatus PumpFontPrewarmStep()
	{
		if (PrewarmRuntime().terminalPrewarmFailure)
			return FontPrewarmPumpStatus::Failed;
		if (!g_bEnableFreeTypeFontRendering)
		{
			ShutdownFontPrewarm();
			return FontPrewarmPumpStatus::Idle;
		}

		QueueConfiguredFontPrewarms();
		if (PrewarmRuntime().session.phase == PrewarmPhase::Idle)
		{
			if (PrewarmRuntime().jobs.empty())
			{
				if (PrewarmRuntime().configuredFontsPrewarmed
					&& !PrewarmRuntime().transactionRestartPending)
				{
					return FontPrewarmPumpStatus::Idle;
				}
				if (g_configs.empty())
				{
					PrewarmRuntime().prewarmActive.store(
						false, std::memory_order_release);
					PrewarmRuntime().transactionRestartPending = false;
					PrewarmRuntime().rebuildProgressTracked = false;
					PrewarmRuntime().rebuildProgressReportingStarted = false;
					PrewarmRuntime().rebuildProgressOverlayVisible = false;
					PrewarmRuntime().rebuildProgress = 0.0f;
					return FontPrewarmPumpStatus::Idle;
				}
				ResetPrewarmTransactionForRetry(
					"configured-runtime-or-job-unavailable");
				return FontPrewarmPumpStatus::Active;
			}
			PrewarmRuntime().session = {};
			PrewarmRuntime().prewarmActive.store(
				true, std::memory_order_release);
			TransitionPrewarmPhase(PrewarmPhase::Prepare);
		}

		switch (PrewarmRuntime().session.phase)
		{
		case PrewarmPhase::Prepare:
		{
			const ULONGLONG started = GetTickCount64();
			PrepareIncrementalSession();
			RecordPrewarmStep(started);
			break;
		}
		case PrewarmPhase::RestoreSnapshots:
			RestoreOnePrewarmSnapshot();
			break;
		case PrewarmPhase::BeginFont:
		{
			const ULONGLONG started = GetTickCount64();
			BeginNextPrewarmFont();
			RecordPrewarmStep(started);
			break;
		}
		case PrewarmPhase::GenerateBatch:
			GenerateOnePrewarmBatch();
			break;
		case PrewarmPhase::FinalizeFont:
			FinalizeActivePrewarmFont();
			break;
		case PrewarmPhase::CleanupFlush:
		{
			const ULONGLONG started = GetTickCount64();
			// A cache-writing cold transaction remains visibly active through final
			// profile validation, physical atlas publication, and cleanup. A cache-hit
			// startup never latches or starts progress reporting, so it stays UI-free.
			StartRebuildProgressReporting();
			ReportPrewarmTransactionProgress(
				L"Finalizing generated font cache",
				L"Flushing generated glyph cache files...",
				0.87f, true);
			EndAtlasOnlyPrewarmPolicy();
			FlushGlyphBitmapDiskCache();
			ReleaseGlyphBitmapDiskCacheMappings();
			RecordPrewarmStep(started);
			TransitionPrewarmPhase(PrewarmPhase::CleanupProfiles);
			break;
		}
		case PrewarmPhase::CleanupProfiles:
		{
			const ULONGLONG started = GetTickCount64();
			ReportPrewarmTransactionProgress(
				L"Finalizing generated font cache",
				L"Validating sealed font profiles...",
				0.89f, true);
			CollectPrewarmProfileResults();
			if (!PrewarmRuntime().session.everyConfiguredProfileVerified)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: complete codepage mask retention required complete=%u verifiedAtlas=%u queued=%u verifiedProfiles=%u configuredProfiles=%u readyRuntimes=%u configuredFonts=%u",
					PrewarmRuntime().session.completedFonts,
					static_cast<UInt32>(
						PrewarmRuntime().session.verifiedCodePageFonts.size()),
					PrewarmRuntime().session.queuedFonts,
					static_cast<UInt32>(
						PrewarmRuntime().session.verifiedProfileKeys.size()),
					static_cast<UInt32>(
						PrewarmRuntime().session.configuredProfileKeys.size()),
					PrewarmRuntime().session.readyConfiguredRuntimes,
					static_cast<UInt32>(g_configs.size()));
			}
			RecordPrewarmStep(started);
			TransitionPrewarmPhase(PrewarmRuntime().session.everyConfiguredProfileVerified
				? PrewarmPhase::CleanupPhysicalGroups
				: PrewarmPhase::CleanupMasks);
			break;
		}
		case PrewarmPhase::CleanupPhysicalGroups:
		{
			const ULONGLONG started = GetTickCount64();
			const FontAtlasPrewarmProgressReporter progressReporter = {
				&ReportAtlasPrewarmProgress, nullptr
			};
			ReportPrewarmTransactionProgress(
				L"Finalizing generated font cache",
				L"Consolidating shared font atlas groups...",
				0.90f, true);
			ReleaseGlyphBitmapDiskCacheMappings();
			PruneRetiredPrewarmAtlasGenerations();
			EnforceCpuMemoryBudget("prewarm-physical-groups");
			ReportPrewarmProcessMemoryState("physical-groups-before");
			try
			{
				PrewarmRuntime().session.physicalGroupsReady =
					ConsolidatePhysicalFontAtlasGroups(
						PrewarmRuntime().session.rasterScale, &progressReporter);
			}
			catch (const std::bad_alloc&)
			{
				PrewarmRuntime().session.physicalGroupsReady = false;
				gLog.FormattedMessage(
					"tnvse_freetype_font: optional physical atlas group consolidation allocation failure; retaining sealed per-font atlases");
			}
			catch (const std::exception& error)
			{
				PrewarmRuntime().session.physicalGroupsReady = false;
				gLog.FormattedMessage(
					"tnvse_freetype_font: optional physical atlas group consolidation failed reason=%s; retaining sealed per-font atlases",
					error.what());
			}
			catch (...)
			{
				PrewarmRuntime().session.physicalGroupsReady = false;
				gLog.FormattedMessage(
					"tnvse_freetype_font: optional physical atlas group consolidation failed reason=unknown; retaining sealed per-font atlases");
			}
			if (!RefreshNativePrewarmOverlayTextGeometry(2000))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm UI refresh acknowledgement timed out stage=physical-groups policy=retired-atlas-refcount-retention");
			}
			PruneRetiredPrewarmAtlasGenerations();
			ReportPrewarmProcessMemoryState("physical-groups-after");
			RecordPrewarmStep(started);
			TransitionPrewarmPhase(PrewarmPhase::CleanupPhysicalPools);
			break;
		}
		case PrewarmPhase::CleanupPhysicalPools:
		{
			const ULONGLONG started = GetTickCount64();
			const FontAtlasPrewarmProgressReporter progressReporter = {
				&ReportAtlasPrewarmProgress, nullptr
			};
			ReportPrewarmTransactionProgress(
				L"Finalizing generated font cache",
				L"Planning physical font texture pools...",
				0.94f, true);
			ReleaseGlyphBitmapDiskCacheMappings();
			PruneRetiredPrewarmAtlasGenerations();
			EnforceCpuMemoryBudget("prewarm-physical-pools");
			ReportPrewarmProcessMemoryState("physical-pools-before");
			try
			{
				PrewarmRuntime().session.physicalPoolsReady =
					ConsolidatePhysicalFontAtlasPools(
						PrewarmRuntime().session.rasterScale, &progressReporter);
			}
			catch (const std::bad_alloc&)
			{
				PrewarmRuntime().session.physicalPoolsReady = false;
				gLog.FormattedMessage(
					"tnvse_freetype_font: optional physical atlas pool consolidation allocation failure; retaining sealed group/per-font atlases");
			}
			catch (const std::exception& error)
			{
				PrewarmRuntime().session.physicalPoolsReady = false;
				gLog.FormattedMessage(
					"tnvse_freetype_font: optional physical atlas pool consolidation failed reason=%s; retaining sealed group/per-font atlases",
					error.what());
			}
			catch (...)
			{
				PrewarmRuntime().session.physicalPoolsReady = false;
				gLog.FormattedMessage(
					"tnvse_freetype_font: optional physical atlas pool consolidation failed reason=unknown; retaining sealed group/per-font atlases");
			}
			if (!RefreshNativePrewarmOverlayTextGeometry(2000))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm UI refresh acknowledgement timed out stage=physical-pools policy=retired-atlas-refcount-retention");
			}
			PruneRetiredPrewarmAtlasGenerations();
			ReportPrewarmProcessMemoryState("physical-pools-after");
			gLog.FormattedMessage(
				"tnvse_freetype_font: physical atlas consolidation groupV2=%s poolV3=%s",
				PrewarmRuntime().session.physicalGroupsReady ? "complete" : "partial-fallback",
				PrewarmRuntime().session.physicalPoolsReady ? "complete" : "partial-fallback");
			ReportPrewarmTransactionProgress(
				L"Finalizing generated font cache",
				L"Shared physical font textures are ready...",
				0.975f, true);
			RecordPrewarmStep(started);
			TransitionPrewarmPhase(PrewarmPhase::CleanupMasks);
			break;
		}
		case PrewarmPhase::CleanupMasks:
		{
			const ULONGLONG started = GetTickCount64();
			ReportPrewarmTransactionProgress(
				L"Finalizing generated font cache",
				L"Cleaning temporary glyph mask data...",
				0.98f, true);
			if (PrewarmRuntime().session.everyConfiguredProfileVerified
				&& GetPersistentFontCacheRoute()
					!= FontAtlasRoute::ArgbFallback
				&& !DeleteCompleteCodePageGlyphBitmapDiskCaches(
					PrewarmRuntime().session.atlasOnlyFontIds))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: complete codepage mask cleanup incomplete; residual files will not be reused in this process");
			}
			RecordPrewarmStep(started);
			TransitionPrewarmPhase(PrewarmPhase::CleanupBudget);
			break;
		}
		case PrewarmPhase::CleanupBudget:
		{
			const ULONGLONG started = GetTickCount64();
			ReportPrewarmTransactionProgress(
				L"Finalizing generated font cache",
				L"Releasing temporary font cache memory...",
				0.985f, true);
			if (PrewarmRuntime().session.everyConfiguredProfileVerified)
			{
				SetBitmapCacheReducedAfterPrewarm(true);
				EnforceCpuMemoryBudget("post-prewarm");
			}
			else
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: bitmap cache post-prewarm shrink skipped because streamed prewarm did not complete successfully complete=%u queued=%u",
					PrewarmRuntime().session.completedFonts,
					PrewarmRuntime().session.queuedFonts);
			}
			RecordPrewarmStep(started);
			TransitionPrewarmPhase(PrewarmPhase::CleanupFiles);
			break;
		}
		case PrewarmPhase::CleanupFiles:
		{
			const ULONGLONG started = GetTickCount64();
			ReportPrewarmTransactionProgress(
				L"Finalizing generated font cache",
				L"Cleaning obsolete font cache files...",
				0.995f, true);
			if (g_bDeleteUnusedFreeTypeFontCache)
			{
				DeleteUnusedFreeTypeFontCacheFiles(
					PrewarmRuntime().session.everyConfiguredProfileVerified);
				if (!PrewarmRuntime().session.everyConfiguredProfileVerified)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: incomplete prewarm retained current-mode and mode-neutral caches; inactive distance-field, invalid, and temporary caches were still cleaned");
					}
			}
			RecordPrewarmStep(started);
			FinishIncrementalSession();
			break;
		}
		case PrewarmPhase::Complete:
			if (!QuiesceNativePrewarmOverlay(2000))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm progress final hide acknowledgement timed out; LoadingMenu owns remaining Tile teardown");
			}
			PruneRetiredPrewarmAtlasGenerations();
			PrewarmRuntime().rebuildProgressTracked = false;
			PrewarmRuntime().rebuildProgressReportingStarted = false;
			PrewarmRuntime().rebuildProgressOverlayVisible = false;
			PrewarmRuntime().rebuildProgress = 0.0f;
			ReleaseFontPrewarmEmergencyAddressSpace();
			PrewarmRuntime().configuredFontsPrewarmed = true;
			PrewarmRuntime().session = {};
			PrewarmRuntime().transactionRestartPending = false;
			PrewarmRuntime().transactionRestartCount = 0;
			PrewarmRuntime().totalMemoryRetryCount = 0;
			PrewarmRuntime().prewarmWorkerThreadId.store(
				0, std::memory_order_release);
			PrewarmRuntime().prewarmActive.store(
				false, std::memory_order_release);
			return FontPrewarmPumpStatus::Completed;
		case PrewarmPhase::Idle:
		default:
			return PrewarmRuntime().transactionRestartPending
				? FontPrewarmPumpStatus::Active
				: FontPrewarmPumpStatus::Idle;
		}

		if (PrewarmRuntime().terminalPrewarmFailure)
			return FontPrewarmPumpStatus::Failed;
		return PrewarmRuntime().transactionRestartPending
			|| PrewarmRuntime().session.phase != PrewarmPhase::Idle
			? FontPrewarmPumpStatus::Active
			: FontPrewarmPumpStatus::Idle;
	}

	FontPrewarmPumpStatus PumpFontPrewarm()
	{
		if (PrewarmThread().prewarmPumpExecuting)
			return PrewarmRuntime().terminalPrewarmFailure
				? FontPrewarmPumpStatus::Failed
				: FontPrewarmPumpStatus::Active;
		PrewarmThread().prewarmPumpExecuting = true;
		struct ResetPumpFlag
		{
			~ResetPumpFlag() { PrewarmThread().prewarmPumpExecuting = false; }
		} resetPumpFlag;
		try
		{
			// Runtime initialization can enqueue a font before the coordinator starts.
			// Drain those requests inside the pump's exception boundary so allocation
			// failures use the same bounded transaction-retry policy as every other
			// prewarm state mutation.
			DrainPendingFontPrewarms();
			return PumpFontPrewarmStep();
		}
		catch (const std::bad_alloc&)
		{
			if (PrewarmRuntime().session.activeFont)
				RecordPrewarmMemoryPressure(PrewarmRuntime().session.activeFont->job);
			else
			{
				IncrementSaturating(PrewarmRuntime().session.memoryRetries);
				IncrementSaturating(PrewarmRuntime().totalMemoryRetryCount);
			}
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm pump allocation exception intercepted; restarting on the worker coordinator inside the pre-entry barrier");
			RetryPrewarmAfterPumpException(
				"unhandled-allocation-exception");
			return PrewarmRuntime().terminalPrewarmFailure
				? FontPrewarmPumpStatus::Failed
				: FontPrewarmPumpStatus::Active;
		}
		catch (const std::exception& error)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm pump exception intercepted type=std reason=%s; restarting on the worker coordinator inside the pre-entry barrier",
				error.what());
			RetryPrewarmAfterPumpException(
				"unhandled-standard-exception");
			return PrewarmRuntime().terminalPrewarmFailure
				? FontPrewarmPumpStatus::Failed
				: FontPrewarmPumpStatus::Active;
		}
		catch (...)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm pump unknown C++ exception intercepted; restarting on the worker coordinator inside the pre-entry barrier");
			RetryPrewarmAfterPumpException(
				"unhandled-cpp-exception");
			return PrewarmRuntime().terminalPrewarmFailure
				? FontPrewarmPumpStatus::Failed
				: FontPrewarmPumpStatus::Active;
		}
	}

	bool IsFontPrewarmActive()
	{
		return PrewarmRuntime().prewarmActive.load(std::memory_order_acquire);
	}

	void ShutdownFontPrewarm()
	{
		const bool cancelledTransaction =
			PrewarmRuntime().session.atlasOnlyTransactionStarted;
		if (PrewarmRuntime().session.activeFont)
		{
			if (RuntimeFont* runtime =
				FindRuntimeFont(PrewarmRuntime().session.activeFont->job.fontId))
			{
				CancelStreamingPrewarmAtlas(*runtime);
			}
		}
		ReleasePrewarmBatchReferences("prewarm-shutdown");
		EndAtlasOnlyPrewarmPolicy();
		ReleaseFontPrewarmEmergencyAddressSpace();
		ResetAtlasAllocationMemoryPressure();
		if (cancelledTransaction)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: incremental streamed prewarm atlas-only transaction result=cancelled");
		}
		PrewarmRuntime().session = {};
		PrewarmRuntime().jobs.clear();
		PrewarmRuntime().scheduledProfiles.clear();
		PrewarmRuntime().configuredFontsQueued = false;
		PrewarmRuntime().configuredFontsPrewarmed = false;
		PrewarmRuntime().transactionRestartPending = false;
		PrewarmRuntime().transactionRestartCount = 0;
		PrewarmRuntime().totalMemoryRetryCount = 0;
		PrewarmRuntime().terminalPrewarmFailure = false;
		PrewarmRuntime().prewarmActive.store(false, std::memory_order_release);
		PrewarmRuntime().prewarmWorkerThreadId.store(
			0, std::memory_order_release);
		HideNativePrewarmOverlay();
		PrewarmRuntime().rebuildProgressTracked = false;
		PrewarmRuntime().rebuildProgressReportingStarted = false;
		PrewarmRuntime().rebuildProgressOverlayVisible = false;
		PrewarmRuntime().rebuildProgress = 0.0f;
	}
}

namespace fonthook
{
	FontPrewarmPumpStatus RunFreeTypeFontPrewarmLoadingBarrier()
	{
		return vectorfont::RunFontPrewarmLoadingBarrier();
	}

	FontPrewarmPumpStatus PumpFreeTypeFontPrewarm()
	{
		return vectorfont::PumpFontPrewarm();
	}

	bool IsFreeTypeFontPrewarmActive()
	{
		return vectorfont::IsFontPrewarmActive();
	}

	void ShutdownFreeTypeFontPrewarm()
	{
		vectorfont::ShutdownFontPrewarmWorker();
	}
}

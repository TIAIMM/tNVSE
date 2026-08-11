#include "font_prewarm_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_prewarm {}
	using namespace implementation::font_prewarm;

	namespace implementation::font_prewarm
	{
		const char* PrewarmPhaseName(PrewarmPhase phase)
		{
			switch (phase)
			{
			case PrewarmPhase::Idle: return "idle";
			case PrewarmPhase::Prepare: return "prepare";
			case PrewarmPhase::RestoreSnapshots: return "restore-snapshots";
			case PrewarmPhase::BeginFont: return "begin-font";
			case PrewarmPhase::GenerateBatch: return "generate-batch";
			case PrewarmPhase::FinalizeFont: return "finalize-font";
			case PrewarmPhase::CleanupFlush: return "cleanup-flush";
			case PrewarmPhase::CleanupProfiles: return "cleanup-profiles";
			case PrewarmPhase::CleanupPhysicalGroups:
				return "cleanup-physical-groups";
			case PrewarmPhase::CleanupPhysicalPools:
				return "cleanup-physical-pools";
			case PrewarmPhase::CleanupMasks: return "cleanup-masks";
			case PrewarmPhase::CleanupBudget: return "cleanup-budget";
			case PrewarmPhase::CleanupFiles: return "cleanup-files";
			case PrewarmPhase::Complete: return "complete";
			default: return "unknown";
			}
		}

		void TransitionPrewarmPhase(PrewarmPhase phase)
		{
			if (PrewarmRuntime().session.phase == phase)
				return;
			gLog.FormattedMessage(
				"tnvse_freetype_font: incremental prewarm phase %s -> %s",
				PrewarmPhaseName(PrewarmRuntime().session.phase), PrewarmPhaseName(phase));
			PrewarmRuntime().session.phase = phase;
		}

		void EndAtlasOnlyPrewarmPolicy()
		{
			ReleasePrewarmRasterWorkers();
			if (!PrewarmRuntime().atlasOnlyPrewarmPending)
				return;
			EndCompleteCodePageAtlasOnlyPrewarm();
			PrewarmRuntime().atlasOnlyPrewarmPending = false;
		}

		bool ResolveValidPrewarmJob(
			const PrewarmJob& job,
			const FontConfig*& config,
			RuntimeFont*& runtime)
		{
			config = FindConfig(job.fontId);
			runtime = FindRuntimeFont(job.fontId);
			return config
				&& runtime
				&& config->layoutHash == job.layoutHash
				&& config->maskGenerationHash == job.maskGenerationHash
				&& config->shaderEffectHash == job.shaderEffectHash
				&& MatchesPrewarmProfile(job, *config)
				&& job.codePage == GetFreeTypeTextCodePage();
		}

		void ReleasePrewarmBatchReferences(const char* reason)
		{
			PrewarmRuntime().session.bitmapResults.clear();
			PrewarmRuntime().session.bitmapRequests.clear();
			PrewarmRuntime().session.requestedGlyphs.clear();
			EnforceCpuMemoryBudget(reason);
		}

		void PruneRetiredPrewarmAtlasGenerations()
		{
			const DWORD workerThreadId =
				PrewarmRuntime().prewarmWorkerThreadId.load(
					std::memory_order_acquire);
			if (PrewarmRuntime().prewarmActive.load(std::memory_order_acquire)
				&& workerThreadId && GetCurrentThreadId() == workerThreadId)
			{
				PrewarmAtlasRequestResult requestResult;
				if (ExecutePrewarmAtlasRequestOnMainThread(
						PrewarmAtlasRequestKind::PruneRetiredGenerations,
						0, PrewarmRuntime().session.rasterScale, requestResult)
					&& requestResult.memoryPressure)
				{
					MarkAtlasAllocationMemoryPressure();
				}
				return;
			}
			PruneRetiredAtlasGenerationsSafely();
		}

		void IncrementSaturating(UInt32& value)
		{
			if (value != std::numeric_limits<UInt32>::max())
				++value;
		}

		void RecordPrewarmMemoryPressure(PrewarmJob& job)
		{
			IncrementSaturating(job.memoryRetries);
			IncrementSaturating(PrewarmRuntime().session.memoryRetries);
			IncrementSaturating(PrewarmRuntime().totalMemoryRetryCount);
		}

		void PreparePrewarmMemoryRetry(const char* stage, UInt32 fontId,
			UInt32 retry, size_t pendingBytes)
		{
			ReleasePrewarmRasterWorkers();
			const bool releasedEmergency =
				ReleaseFontPrewarmEmergencyAddressSpace();
			ReleasePrewarmBatchReferences("prewarm-memory-retry");
			SetBitmapCacheReducedAfterPrewarm(true);
			const UInt64 releasedMappings =
				ReleaseGlyphBitmapDiskCacheMappings();
			PruneRetiredPrewarmAtlasGenerations();
			EnforceCpuMemoryBudget("prewarm-memory-retry");
			ProcessVirtualMemoryHeadroom headroom;
			QueryProcessVirtualMemoryHeadroom(headroom);
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm memory recovery stage=%s font=%u retry=%u pendingMiB=%.2f availableVirtualMiB=%.2f largestFreeMiB=%.2f emergencyReleased=%u persistentMappingsReleased=%llu policy=same-session-retry",
				stage ? stage : "unknown", fontId, retry,
				pendingBytes / (1024.0 * 1024.0),
				headroom.availableBytes / (1024.0 * 1024.0),
				headroom.largestFreeRegionBytes / (1024.0 * 1024.0),
				releasedEmergency ? 1u : 0u,
				static_cast<unsigned long long>(releasedMappings));
			if (retry >= 4)
			{
				const DWORD delayMs = std::min<DWORD>(250,
					static_cast<DWORD>(1u << std::min<UInt32>(retry - 4, 7)));
				Sleep(delayMs);
			}
		}

		void AbortPrewarmTransaction(const char* reason) noexcept
		{
			const UInt32 queuedFonts = PrewarmRuntime().session.queuedFonts;
			const UInt32 completedFonts = PrewarmRuntime().session.completedFonts;
			const UInt32 memoryRetries = PrewarmRuntime().totalMemoryRetryCount;
			const UInt32 transactionRestarts = PrewarmRuntime().transactionRestartCount;
			try
			{
				if (PrewarmRuntime().session.activeFont)
				{
					if (RuntimeFont* runtime = FindRuntimeFont(
							PrewarmRuntime().session.activeFont->job.fontId))
					{
						CancelStreamingPrewarmAtlas(*runtime);
					}
				}
			}
			catch (...) {}
			try { ReleasePrewarmBatchReferences("prewarm-terminal-failure"); }
			catch (...) {}
			try { EndAtlasOnlyPrewarmPolicy(); }
			catch (...) {}
			try { SetBitmapCacheReducedAfterPrewarm(true); }
			catch (...) {}
			try { EnforceCpuMemoryBudget("prewarm-terminal-failure"); }
			catch (...) {}
			ReleaseFontPrewarmEmergencyAddressSpace();
			ResetAtlasAllocationMemoryPressure();
			PrewarmRuntime().session = {};
			PrewarmRuntime().jobs.clear();
			PrewarmRuntime().scheduledProfiles.clear();
			PrewarmRuntime().configuredFontsQueued = true;
			PrewarmRuntime().configuredFontsPrewarmed = false;
			PrewarmRuntime().transactionRestartPending = false;
			PrewarmRuntime().terminalPrewarmFailure = true;
			PrewarmRuntime().prewarmActive.store(
				false, std::memory_order_release);
			PrewarmRuntime().prewarmWorkerThreadId.store(
				0, std::memory_order_release);
			try
			{
				if (!QuiesceNativePrewarmOverlay(2000))
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: terminal prewarm failure overlay hide acknowledgement timed out; LoadingMenu owns remaining Tile teardown");
				}
			}
			catch (...) {}
			PrewarmRuntime().rebuildProgressTracked = false;
			PrewarmRuntime().rebuildProgressReportingStarted = false;
			PrewarmRuntime().rebuildProgressOverlayVisible = false;
			PrewarmRuntime().rebuildProgress = 0.0f;
			try
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm terminal failure reason=%s queued=%u complete=%u memoryRetries=%u transactionRestarts=%u policy=retain-valid-partial-caches-and-continue-game runtimeFallback=1",
					reason ? reason : "unknown", queuedFonts, completedFonts,
					memoryRetries, transactionRestarts);
			}
			catch (...) {}
		}

		bool ResetPrewarmTransactionForRetry(const char* reason)
		{
			if (!CanRestartPrewarmTransaction(PrewarmRuntime().transactionRestartCount))
			{
				AbortPrewarmTransaction(reason);
				return false;
			}
			if (PrewarmRuntime().session.activeFont)
			{
				if (RuntimeFont* runtime =
					FindRuntimeFont(PrewarmRuntime().session.activeFont->job.fontId))
				{
					CancelStreamingPrewarmAtlas(*runtime);
				}
			}
			ReleasePrewarmBatchReferences("prewarm-transaction-retry");
			EndAtlasOnlyPrewarmPolicy();
			ReleaseFontPrewarmEmergencyAddressSpace();
			ResetAtlasAllocationMemoryPressure();
			if (PrewarmRuntime().rebuildProgressTracked)
			{
				ReportPrewarmTransactionProgress(
					L"Font cache rebuild",
					L"Restarting pre-entry cache transaction...",
					PrewarmRuntime().rebuildProgress, true);
			}
			PrewarmRuntime().session = {};
			PrewarmRuntime().jobs.clear();
			PrewarmRuntime().scheduledProfiles.clear();
			PrewarmRuntime().configuredFontsQueued = false;
			PrewarmRuntime().configuredFontsPrewarmed = false;
			PrewarmRuntime().transactionRestartPending = true;
			IncrementSaturating(PrewarmRuntime().transactionRestartCount);
			SetBitmapCacheReducedAfterPrewarm(false);
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm transaction retry reason=%s restart=%u policy=same-pre-entry-barrier runtime-demand-fallback=1",
				reason ? reason : "unknown", PrewarmRuntime().transactionRestartCount);
			if (PrewarmRuntime().transactionRestartCount >= 4)
			{
				const DWORD delayMs = std::min<DWORD>(250,
					static_cast<DWORD>(1u << std::min<UInt32>(
						PrewarmRuntime().transactionRestartCount - 4, 7)));
				Sleep(delayMs);
			}
			return true;
		}

		void ForcePrewarmTransactionRetryState() noexcept
		{
			if (!CanRestartPrewarmTransaction(PrewarmRuntime().transactionRestartCount))
			{
				AbortPrewarmTransaction("exception-retry-state-exhausted");
				return;
			}
			try
			{
				if (PrewarmRuntime().session.activeFont)
				{
					if (RuntimeFont* runtime =
						FindRuntimeFont(PrewarmRuntime().session.activeFont->job.fontId))
					{
						CancelStreamingPrewarmAtlas(*runtime);
					}
				}
			}
			catch (...) {}
			try { EndAtlasOnlyPrewarmPolicy(); }
			catch (...) {}
			try
			{
				if (PrewarmRuntime().rebuildProgressTracked)
				{
					ReportPrewarmTransactionProgress(
						L"Font cache rebuild",
						L"Restarting pre-entry cache transaction...",
						PrewarmRuntime().rebuildProgress, true);
				}
			}
			catch (...) {}
			PrewarmRuntime().session = {};
			PrewarmRuntime().jobs.clear();
			PrewarmRuntime().scheduledProfiles.clear();
			PrewarmRuntime().configuredFontsQueued = false;
			PrewarmRuntime().configuredFontsPrewarmed = false;
			if (!PrewarmRuntime().transactionRestartPending)
				IncrementSaturating(PrewarmRuntime().transactionRestartCount);
			PrewarmRuntime().transactionRestartPending = true;
			ReleaseFontPrewarmEmergencyAddressSpace();
			ResetAtlasAllocationMemoryPressure();
		}

		bool RetryPrewarmAfterPumpException(const char* reason) noexcept
		{
			try
			{
				return ResetPrewarmTransactionForRetry(reason);
			}
			catch (...)
			{
				ForcePrewarmTransactionRetryState();
				return !PrewarmRuntime().terminalPrewarmFailure;
			}
		}

		void RecordPrewarmStep(ULONGLONG started)
		{
			const ULONGLONG elapsed = GetTickCount64() - started;
			PrewarmRuntime().session.maximumStepMs =
				std::max(PrewarmRuntime().session.maximumStepMs, elapsed);
		}

		void ReportPrewarmProcessMemoryState(const char* phase)
		{
			ProcessVirtualMemoryHeadroom headroom;
			QueryProcessVirtualMemoryHeadroom(headroom);
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm process memory phase=%s privateMiB=%.2f workingSetMiB=%.2f availableVirtualMiB=%.2f largestFreeMiB=%.2f countersValid=%u virtualValid=%u",
				phase ? phase : "unknown",
				headroom.privateUsageBytes / (1024.0 * 1024.0),
				headroom.workingSetBytes / (1024.0 * 1024.0),
				headroom.availableBytes / (1024.0 * 1024.0),
				headroom.largestFreeRegionBytes / (1024.0 * 1024.0),
				headroom.processCountersValid ? 1u : 0u,
				headroom.valid ? 1u : 0u);
		}

		void PrepareIncrementalSession()
		{
			PrewarmRuntime().prewarmActive.store(
				true, std::memory_order_release);
			PrewarmRuntime().prewarmWorkerThreadId.store(
				GetCurrentThreadId(), std::memory_order_release);
			PrewarmRuntime().transactionRestartPending = false;
			PrewarmRuntime().session.transactionRestarts = PrewarmRuntime().transactionRestartCount;
			PrewarmRuntime().session.memoryRetries = PrewarmRuntime().totalMemoryRetryCount;
			const bool emergencyReserved =
				ReserveFontPrewarmEmergencyAddressSpace();
			ProcessVirtualMemoryHeadroom initialHeadroom;
			QueryProcessVirtualMemoryHeadroom(initialHeadroom);
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm address-space guard reserveMiB=%.2f reserved=%u availableVirtualMiB=%.2f largestFreeMiB=%.2f privateMiB=%.2f workingSetMiB=%.2f countersValid=%u",
				kFontPrewarmEmergencyAddressSpaceBytes / (1024.0 * 1024.0),
				emergencyReserved ? 1u : 0u,
				initialHeadroom.availableBytes / (1024.0 * 1024.0),
				initialHeadroom.largestFreeRegionBytes / (1024.0 * 1024.0),
				initialHeadroom.privateUsageBytes / (1024.0 * 1024.0),
				initialHeadroom.workingSetBytes / (1024.0 * 1024.0),
				initialHeadroom.processCountersValid ? 1u : 0u);
			const FontAtlasRoute finalRoute = GetPersistentFontCacheRoute();
			std::deque<PrewarmJob> reboundJobs;
			while (!PrewarmRuntime().jobs.empty())
			{
				PrewarmJob job = std::move(PrewarmRuntime().jobs.front());
				PrewarmRuntime().jobs.pop_front();
				const FontConfig* config = FindConfig(job.fontId);
				if (!config)
					continue;
				job.route = finalRoute;
				job.profileKey = BuildProfileKey(*config, finalRoute);
				reboundJobs.push_back(std::move(job));
			}
			// The final render route can merge profiles that were distinct when
			// first queued. Sort before deduplication so a physical MTSDF owner is
			// never discarded in favor of one of its dependent aliases.
			SortPrewarmJobsByDependencies(reboundJobs);
			std::unordered_set<UInt64> reboundProfiles;
			while (!reboundJobs.empty())
			{
				PrewarmJob job = std::move(reboundJobs.front());
				reboundJobs.pop_front();
				if (!reboundProfiles.insert(job.profileKey).second)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: final-route prewarm alias font=%u route=%u",
						job.fontId, static_cast<UInt32>(finalRoute));
					continue;
				}
				PrewarmRuntime().session.restoreJobs.push_back(std::move(job));
			}
			SortPrewarmJobsByDependencies(PrewarmRuntime().session.restoreJobs);
			PrewarmRuntime().scheduledProfiles = reboundProfiles;
			if (PrewarmRuntime().session.restoreJobs.empty())
			{
				if (g_configs.empty())
				{
					EndAtlasOnlyPrewarmPolicy();
					PrewarmRuntime().rebuildProgressTracked = false;
					PrewarmRuntime().rebuildProgressReportingStarted = false;
					PrewarmRuntime().rebuildProgressOverlayVisible = false;
					PrewarmRuntime().rebuildProgress = 0.0f;
					TransitionPrewarmPhase(PrewarmPhase::Idle);
				}
				else
				{
					ResetPrewarmTransactionForRetry(
						"no-valid-configured-prewarm-jobs");
				}
				return;
			}
			if (finalRoute == FontAtlasRoute::ArgbFallback)
				EndAtlasOnlyPrewarmPolicy();

			PrewarmRuntime().session.rasterScale = GetCanonicalFreeTypeRasterScale();
			PrewarmRuntime().session.rasterScaleMilli = static_cast<UInt32>(std::lround(
				PrewarmRuntime().session.rasterScale * 1000.0f));
			PrewarmRuntime().session.queuedFonts =
				static_cast<UInt32>(PrewarmRuntime().session.restoreJobs.size());
			PrewarmRuntime().session.verifiedCodePageFonts.reserve(PrewarmRuntime().session.queuedFonts);
			PrewarmRuntime().session.started = GetTickCount64();
			PrewarmRuntime().session.maximumStepMs = 0;

			gLog.FormattedMessage(
				"tnvse_freetype_font: incremental streamed prewarm begin fonts=%u scale=%.3f maximumBatchMiB=%.2f targetBatchMs=%llu strategy=bounded-throughput",
				PrewarmRuntime().session.queuedFonts, PrewarmRuntime().session.rasterScale,
				kMaximumPrewarmBatchBytes / (1024.0 * 1024.0),
				static_cast<unsigned long long>(kTargetPrewarmBatchMs));
			TransitionPrewarmPhase(PrewarmPhase::RestoreSnapshots);
		}

	}

}

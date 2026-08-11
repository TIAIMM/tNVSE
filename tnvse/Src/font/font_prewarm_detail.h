#pragma once

#include "font_vector_internal.h"
#include "font_render_route.h"
#include "font_atlas_stream.h"
#include "native_tile_overlay.h"

#include "encoding.h"
#include "load_config.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>


namespace fonthook::vectorfont::implementation::font_prewarm
{
		constexpr UInt32 kMaximumCandidatesPerBatch = 32768;
		constexpr UInt32 kMaximumGlyphsPerBatch = 4096;
		constexpr UInt32 kMaximumIncrementalGlyphsPerBatch = 1024;
		constexpr UInt32 kInitialIncrementalGlyphsPerBatch = 256;
		constexpr UInt32 kParallelGlyphBatchFloor =
			kFillPrewarmParallelThreshold;
		// The coordinator runs below normal priority and yields between batches.
		// Target roughly one second to amortize worker/FreeType setup and
		// persistent-cache appends; the existing byte/headroom limits remain the
		// authoritative ceiling.
		constexpr ULONGLONG kTargetPrewarmBatchMs = 1000;
		constexpr ULONGLONG kMinimumProgressUpdateIntervalMs = 100;
		constexpr float kPrewarmFontWorkProgressShare = 0.86f;
		constexpr float kPrewarmGlyphGenerationProgressShare = 0.80f;
		constexpr size_t kMinimumPrewarmBatchBytes = 1u * 1024u * 1024u;
		constexpr size_t kMaximumPrewarmBatchBytes = 24u * 1024u * 1024u;
		constexpr size_t kPrewarmPerGlyphMetadataBytes = 512u;
		constexpr size_t kPrewarmPerWorkerFixedBytes = 256u * 1024u;
		// FalloutNV.exe reserves 1 MiB for each new thread stack. The coordinator
		// already exists, so only auxiliary raster workers add this x86 VA cost.
		constexpr size_t kPrewarmAuxiliaryThreadStackReserveBytes =
			1u * 1024u * 1024u;
		constexpr size_t kMaximumPrewarmStreamingWorkingBytes =
			16u * 1024u * 1024u;
		// Worst-case 8192x8192 four-byte page. This is a recovery estimate;
		// CreateDynamicAtlasTexture admits the selected page using its exact bytes.
		constexpr size_t kMaximumPrewarmPhysicalAllocationBytes =
			256u * 1024u * 1024u;
		constexpr UInt32 kMaximumBatchAllocationFailures = 12;
		constexpr UInt32 kMaximumFontMemoryFailures = 12;
		constexpr UInt32 kMaximumFontGenerationRestarts = 2;
		constexpr UInt32 kMaximumTransactionRestarts = 2;
		constexpr bool CanRetryAfterMemoryFailure(UInt32 failureCount,
			UInt32 maximumFailures)
		{
			return failureCount < maximumFailures;
		}
		constexpr bool CanRestartFontGeneration(UInt32 completedRestarts)
		{
			return completedRestarts <= kMaximumFontGenerationRestarts;
		}
		constexpr bool CanRestartPrewarmTransaction(UInt32 completedRestarts)
		{
			return completedRestarts < kMaximumTransactionRestarts;
		}
		static_assert(CanRetryAfterMemoryFailure(11, 12));
		static_assert(!CanRetryAfterMemoryFailure(12, 12));
		static_assert(CanRestartFontGeneration(2));
		static_assert(!CanRestartFontGeneration(3));
		static_assert(CanRestartPrewarmTransaction(1));
		static_assert(!CanRestartPrewarmTransaction(2));
		struct PrewarmJob
		{
			UInt32 fontId = 0;
			UInt64 profileKey = 0;
			UInt64 layoutHash = 0;
			UInt64 maskGenerationHash = 0;
			UInt64 shaderEffectHash = 0;
			UInt32 codePage = 0;
			FontAtlasRoute route = FontAtlasRoute::ArgbFallback;
			FontPrewarmRange prewarmRange =
				FontPrewarmRange::CompleteCodePage;
			const std::vector<UInt16>* encodedUnits = nullptr;
			size_t encodedUnitIndex = 0;
			size_t encodedUnitStart = 0;
			UInt32 validDoubleByteCount = 0;
			UInt32 rasterizedGlyphCount = 0;
			UInt32 sdfGlyphCount = 0;
			UInt32 targetUnitCount = 0;
			UInt32 rasterScaleMilli = 0;
			UInt8 dependencyDeferrals = 0;
			UInt32 memoryRetries = 0;
			UInt32 generationRestarts = 0;
		};

		enum class PrewarmAtlasRequestKind : UInt8
		{
			LoadSnapshot,
			LoadSharedDoubleByteRole,
			RebuildPublishedSnapshot,
			PruneRetiredGenerations,
		};

		struct PrewarmAtlasRequestResult
		{
			bool succeeded = false;
			bool memoryPressure = false;
			ULONGLONG queueMs = 0;
			ULONGLONG executionMs = 0;
		};


		enum class PrewarmPhase : UInt8
		{
			Idle,
			Prepare,
			RestoreSnapshots,
			BeginFont,
			GenerateBatch,
			FinalizeFont,
			CleanupFlush,
			CleanupProfiles,
			CleanupPhysicalGroups,
			CleanupPhysicalPools,
			CleanupMasks,
			CleanupBudget,
			CleanupFiles,
			Complete,
		};

		struct ActivePrewarmFont
		{
			PrewarmJob job;
			bool shaderSdf = false;
			bool aggressiveComposite = false;
			bool sharedDoubleAlias = false;
			UInt32 sdfSpread = 0;
			UInt32 batchGlyphLimit = kInitialIncrementalGlyphsPerBatch;
			UInt32 maximumBatchGlyphLimit = kMaximumIncrementalGlyphsPerBatch;
			UInt32 parallelBatchFloor = kParallelGlyphBatchFloor;
			UInt32 maximumRasterWorkers = kMaximumPrewarmRasterWorkers;
			UInt32 metricsOnlyBatchGlyphLimit = kMaximumGlyphsPerBatch;
			size_t estimatedRetainedBytesPerGlyph = 1;
			size_t estimatedTransientBytesPerWorker = 1;
			size_t estimatedPeakBatchBytes = 1;
			size_t targetBatchBytes = kMinimumPrewarmBatchBytes;
			UInt32 allocationRetries = 0;
			bool exhausted = false;
			bool failed = false;
			bool terminalFailure = false;
			bool cancelled = false;
			bool constrainedMemory = false;
		};

		struct PrewarmSession
		{
			PrewarmPhase phase = PrewarmPhase::Idle;
			std::deque<PrewarmJob> restoreJobs;
			std::deque<PrewarmJob> generationJobs;
			std::optional<ActivePrewarmFont> activeFont;
			std::vector<UInt32> verifiedCodePageFonts;
			std::unordered_set<UInt64> verifiedProfileKeys;
			std::unordered_set<UInt64> configuredProfileKeys;
			std::vector<UInt32> atlasOnlyFontIds;
			std::vector<VectorEncodedGlyph> requestedGlyphs;
			std::vector<GlyphBitmapRequest> bitmapRequests;
			std::vector<std::shared_ptr<const GlyphBitmap>> bitmapResults;
			float rasterScale = 1.0f;
			UInt32 rasterScaleMilli = 1000;
			UInt32 queuedFonts = 0;
			UInt32 completedFonts = 0;
			UInt32 streamFailedFonts = 0;
			UInt32 cancelledFonts = 0;
			UInt32 finishedFonts = 0;
			UInt32 batches = 0;
			UInt32 readyConfiguredRuntimes = 0;
			ULONGLONG started = 0;
			ULONGLONG maximumStepMs = 0;
			ULONGLONG scanMs = 0;
			ULONGLONG rasterMs = 0;
			ULONGLONG streamMs = 0;
			ULONGLONG lastProgressUpdate = 0;
			UInt32 peakBatchGlyphs = 0;
			UInt32 memoryRetries = 0;
			UInt32 transactionRestarts = 0;
			bool everyConfiguredJobCompleted = false;
			bool everyConfiguredProfileVerified = false;
			bool atlasOnlyTransactionStarted = false;
			bool physicalGroupsReady = true;
			bool physicalPoolsReady = true;
			bool success = false;
		};

		struct PrewarmBatchPolicy
		{
			UInt32 initialGlyphs = 1;
			UInt32 maximumGlyphs = 1;
			UInt32 parallelFloor = 1;
			UInt32 maximumWorkers = 1;
			size_t estimatedRetainedBytesPerGlyph = 1;
			size_t estimatedTransientBytesPerWorker = 1;
			size_t estimatedPeakBytes = 1;
			size_t targetBytes = kMinimumPrewarmBatchBytes;
		};

	struct PrewarmRuntimeState
	{
		std::deque<PrewarmJob> jobs;
		std::unordered_set<UInt64> scheduledProfiles;
		bool configuredFontsQueued = false;
		bool configuredFontsPrewarmed = false;
		bool atlasOnlyPrewarmPending = false;
		PrewarmSession session;
		UInt32 transactionRestartCount = 0;
		UInt32 totalMemoryRetryCount = 0;
		bool transactionRestartPending = false;
		bool terminalPrewarmFailure = false;
		std::atomic_bool prewarmActive{ false };
		std::atomic<DWORD> prewarmWorkerThreadId{ 0 };
		bool rebuildProgressTracked = false;
		bool rebuildProgressReportingStarted = false;
		bool rebuildProgressOverlayVisible = false;
		float rebuildProgress = 0.0f;
	};

	struct PrewarmThreadState
	{
		bool prewarmPumpExecuting = false;
	};

	PrewarmRuntimeState& PrewarmRuntime();
	PrewarmThreadState& PrewarmThread();

	UInt64 BuildProfileKey(const FontConfig& config, FontAtlasRoute route);
	bool MatchesPrewarmProfile(const PrewarmJob& job,
		const FontConfig& config);
	bool IsPrewarmJobDoubleByteAlias(const PrewarmJob& job);
	void SortPrewarmJobsByDependencies(std::deque<PrewarmJob>& jobs);
	bool NextEncodedUnit(PrewarmJob& job, std::array<char, 2>& bytes,
		size_t& length);
	void ResetPrewarmScan(PrewarmJob& job, UInt32 rasterScaleMilli);
	PrewarmJob BuildQueuedPrewarmJob(UInt32 fontId,
		const FontConfig& config, FontAtlasRoute route, UInt64 profileKey);
	void PreparePrewarmScanForGeneration(PrewarmJob& job,
		const FontConfig& config, UInt32 rasterScaleMilli);
	PrewarmBatchPolicy ResolvePrewarmBatchPolicy(
		const FontConfig& config, float rasterScale, bool shaderSdf,
		bool aggressiveComposite, UInt32 sdfSpread);
	UInt32 ResolveNextPrewarmBatchLimit(UInt32 current, UInt32 maximum,
		UInt32 parallelFloor, UInt32 completedGlyphs, ULONGLONG elapsedMs);
	void LatchRebuildProgress(const char* reason);
	void StartRebuildProgressReporting();
	void ReportPrewarmTransactionProgress(const wchar_t* detail,
		const wchar_t* progressText, float progress, bool force);
	void ReportPrewarmProgress(const PrewarmJob& job, UInt32 fontOrdinal,
		UInt32 fontCount, UInt32 finishedFonts, const wchar_t* stage,
		float stageProgress, bool force);
	void ReportAtlasPrewarmProgress(FontAtlasPrewarmProgressStage stage,
		UInt32 item, UInt32 total, void* context);
	void FinishJob(const PrewarmJob& job, const char* status);
	bool PublishSealedProfileAliases(UInt32 ownerFontId, float rasterScale);

	const char* PrewarmPhaseName(PrewarmPhase phase);
	void TransitionPrewarmPhase(PrewarmPhase phase);
	void EndAtlasOnlyPrewarmPolicy();
	bool ResolveValidPrewarmJob(const PrewarmJob& job,
		const FontConfig*& config,
		RuntimeFont*& runtime);
	void ReleasePrewarmBatchReferences(const char* reason);
	void PruneRetiredPrewarmAtlasGenerations();
	void IncrementSaturating(UInt32& value);
	void RecordPrewarmMemoryPressure(PrewarmJob& job);
	void PreparePrewarmMemoryRetry(const char* stage, UInt32 fontId,
		UInt32 retry, size_t pendingBytes);
	void AbortPrewarmTransaction(const char* reason) noexcept;
	bool ResetPrewarmTransactionForRetry(const char* reason);
	void ForcePrewarmTransactionRetryState() noexcept;
	bool RetryPrewarmAfterPumpException(const char* reason) noexcept;
	void RecordPrewarmStep(ULONGLONG started);
	void ReportPrewarmProcessMemoryState(const char* phase);
	void PrepareIncrementalSession();
	bool ExecutePrewarmAtlasRequestOnMainThread(
		PrewarmAtlasRequestKind kind, UInt32 fontId, float rasterScale,
		PrewarmAtlasRequestResult& result);
	void QueueFontPrewarmOwned(UInt32 fontId);
	void QueuePendingFontPrewarm(UInt32 fontId);
	void DrainPendingFontPrewarms();
	bool IsFontPrewarmStopRequested();
	void StartFontPrewarmWorker();
	void ServiceFontPrewarmMainThread();
	FontPrewarmPumpStatus RunFontPrewarmLoadingBarrier();
	void ShutdownFontPrewarmWorker();

	void RestoreOnePrewarmSnapshot();
	void BeginNextPrewarmFont();
	void GenerateOnePrewarmBatch();
	void FinalizeActivePrewarmFont();
	void CollectPrewarmProfileResults();
	void FinishIncrementalSession();
}

#include "font_prewarm_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_prewarm {}
	using namespace implementation::font_prewarm;

	namespace implementation::font_prewarm
	{
		void RestoreOnePrewarmSnapshot()
		{
			if (PrewarmRuntime().session.restoreJobs.empty())
			{
				SortPrewarmJobsByDependencies(
					PrewarmRuntime().session.generationJobs);
				TransitionPrewarmPhase(PrewarmRuntime().session.generationJobs.empty()
					? PrewarmPhase::CleanupFlush
					: PrewarmPhase::BeginFont);
				return;
			}

			const ULONGLONG stepStarted = GetTickCount64();
			PrewarmJob job = std::move(PrewarmRuntime().session.restoreJobs.front());
			PrewarmRuntime().session.restoreJobs.pop_front();
			if (job.rasterScaleMilli != PrewarmRuntime().session.rasterScaleMilli)
				ResetPrewarmScan(job, PrewarmRuntime().session.rasterScaleMilli);

			const FontConfig* config = nullptr;
			RuntimeFont* runtime = nullptr;
			if (!ResolveValidPrewarmJob(job, config, runtime))
			{
				if (runtime)
					CancelStreamingPrewarmAtlas(*runtime);
				FinishJob(job, "cancelled");
				++PrewarmRuntime().session.cancelledFonts;
				++PrewarmRuntime().session.finishedFonts;
				RecordPrewarmStep(stepStarted);
				return;
			}

			bool snapshotReady = false;
			bool snapshotMemoryPressure = false;
			PrewarmAtlasRequestResult snapshotResult;
			if (!ExecutePrewarmAtlasRequestOnLoadingThread(
					PrewarmAtlasRequestKind::LoadSnapshot, job.fontId,
					PrewarmRuntime().session.rasterScale, snapshotResult))
			{
				AbortPrewarmTransaction(
					"loading-thread-snapshot-service-unavailable");
				RecordPrewarmStep(stepStarted);
				return;
			}
			snapshotReady = snapshotResult.succeeded;
			snapshotMemoryPressure = snapshotResult.memoryPressure;
			if (snapshotMemoryPressure)
			{
				RecordPrewarmMemoryPressure(job);
				PreparePrewarmMemoryRetry("snapshot-restore", job.fontId,
					job.memoryRetries,
					kMaximumPrewarmPhysicalAllocationBytes);
				if (!CanRetryAfterMemoryFailure(job.memoryRetries,
						kMaximumFontMemoryFailures))
				{
					AbortPrewarmTransaction(
						"snapshot-restore-memory-retries-exhausted");
					RecordPrewarmStep(stepStarted);
					return;
				}
				gLog.FormattedMessage(
					"tnvse_freetype_font: snapshot restore memory pressure font=%u scale=%.3f snapshotPreserved=1 retryInSameBarrier=1",
					job.fontId, PrewarmRuntime().session.rasterScale);
				PrewarmRuntime().session.restoreJobs.push_front(std::move(job));
				RecordPrewarmStep(stepStarted);
				return;
			}
			if (snapshotReady
				&& job.route == FontAtlasRoute::ArgbFallback
				&& !MarkCurrentFallbackBitmapProfilesUsed(
					*runtime, PrewarmRuntime().session.rasterScale))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: fallback snapshot rejected font=%u reason=incomplete-persistent-mask-set",
					job.fontId);
				snapshotReady = false;
			}
			if (snapshotReady)
			{
				const bool directReady = BuildDirectGlyphAtlasTables(
						*runtime, PrewarmRuntime().session.rasterScale)
					&& PublishSealedProfileAliases(
						job.fontId, PrewarmRuntime().session.rasterScale);
				if (directReady)
				{
					if (PrewarmRuntime().rebuildProgressTracked)
					{
						ReportPrewarmProgress(job,
							std::min(PrewarmRuntime().session.queuedFonts,
								PrewarmRuntime().session.finishedFonts + 1),
							PrewarmRuntime().session.queuedFonts,
							PrewarmRuntime().session.finishedFonts,
							L"Loading validated font snapshot...",
							1.0f, true);
					}
					FinishJob(job, "snapshot");
					PrewarmRuntime().session.verifiedCodePageFonts.push_back(job.fontId);
					++PrewarmRuntime().session.completedFonts;
					++PrewarmRuntime().session.finishedFonts;
				}
				else
				{
					LatchRebuildProgress("snapshot-direct-table-validation");
					CancelStreamingPrewarmAtlas(*runtime);
					const bool discarded = DiscardGlyphAtlasSnapshot(
						*runtime, PrewarmRuntime().session.rasterScale);
					PreparePrewarmScanForGeneration(
						job, *config, PrewarmRuntime().session.rasterScaleMilli);
					gLog.FormattedMessage(
						"tnvse_freetype_font: snapshot direct-table publication failed font=%u atlas=%s; rebuilding within the same prewarm barrier",
						job.fontId,
						discarded ? "discarded" : "delete-failed");
					PrewarmRuntime().session.generationJobs.push_back(std::move(job));
				}
			}
			else
			{
				LatchRebuildProgress("font-atlas-cache-miss");
				CancelStreamingPrewarmAtlas(*runtime);
				const bool discarded = DiscardGlyphAtlasSnapshot(
					*runtime, PrewarmRuntime().session.rasterScale);
				PreparePrewarmScanForGeneration(
					job, *config, PrewarmRuntime().session.rasterScaleMilli);
				gLog.FormattedMessage(
					"tnvse_freetype_font: cache miss rebuilding atlas font=%u atlas=%s persistent=preserved",
					job.fontId, discarded ? "discarded" : "delete-failed");
				PrewarmRuntime().session.generationJobs.push_back(std::move(job));
			}
			RefreshNativePrewarmOverlayTextGeometry();
			RecordPrewarmStep(stepStarted);
		}

		void BeginNextPrewarmFont()
		{
			if (PrewarmRuntime().session.generationJobs.empty())
			{
				TransitionPrewarmPhase(PrewarmPhase::CleanupFlush);
				return;
			}

			ActivePrewarmFont active;
			active.job = std::move(PrewarmRuntime().session.generationJobs.front());
			PrewarmRuntime().session.generationJobs.pop_front();
			const FontConfig* config = nullptr;
			RuntimeFont* runtime = nullptr;
			if (!ResolveValidPrewarmJob(active.job, config, runtime))
			{
				if (runtime)
					CancelStreamingPrewarmAtlas(*runtime);
				FinishJob(active.job, "cancelled");
				++PrewarmRuntime().session.cancelledFonts;
				++PrewarmRuntime().session.finishedFonts;
				return;
			}
			StartRebuildProgressReporting();

			EffectQuality resolvedQuality = config->effectQuality;
			active.shaderSdf =
				active.job.route == FontAtlasRoute::ShaderDistanceField
				&& ResolveNativeFontEffectQuality(
					config->effectQuality, resolvedQuality);
			active.aggressiveComposite =
				active.job.route == FontAtlasRoute::BakedArgbComposite;
			if (active.shaderSdf
				&& !ResolveSdfSpread(
					*config, PrewarmRuntime().session.rasterScale, active.sdfSpread))
			{
				active.shaderSdf = false;
			}
			active.sharedDoubleAlias = active.shaderSdf
				&& IsMtsdfAtlasAlias(
					*config, VectorFontByteClass::DoubleByte);
			if (active.sharedDoubleAlias)
			{
				ReportPrewarmProgress(
					active.job,
					std::min(PrewarmRuntime().session.queuedFonts,
						PrewarmRuntime().session.finishedFonts + 1),
					PrewarmRuntime().session.queuedFonts,
					PrewarmRuntime().session.finishedFonts,
					L"Reusing shared MTSDF double-byte atlas...",
					0.0f, true);
			}
			bool sharedRoleReady = true;
			bool sharedRoleMemoryPressure = false;
			if (active.sharedDoubleAlias)
			{
				PrewarmAtlasRequestResult sharedRoleResult;
				if (!ExecutePrewarmAtlasRequestOnLoadingThread(
						PrewarmAtlasRequestKind::LoadSharedDoubleByteRole,
						active.job.fontId,
						PrewarmRuntime().session.rasterScale,
						sharedRoleResult))
				{
					AbortPrewarmTransaction(
						"loading-thread-shared-role-service-unavailable");
					return;
				}
				sharedRoleReady = sharedRoleResult.succeeded;
				sharedRoleMemoryPressure = sharedRoleResult.memoryPressure;
			}
			if (sharedRoleMemoryPressure)
			{
				RecordPrewarmMemoryPressure(active.job);
				PreparePrewarmMemoryRetry("shared-role-restore",
					active.job.fontId, active.job.memoryRetries,
					kMaximumPrewarmPhysicalAllocationBytes);
				if (!CanRetryAfterMemoryFailure(active.job.memoryRetries,
						kMaximumFontMemoryFailures))
				{
					AbortPrewarmTransaction(
						"shared-role-memory-retries-exhausted");
					return;
				}
				gLog.FormattedMessage(
					"tnvse_freetype_font: shared double-byte role restore memory pressure font=%u owner=%u snapshotPreserved=1 retryInSameBarrier=1",
					active.job.fontId,
					config->mtsdfDoubleByteOwnerFontId);
				PrewarmRuntime().session.generationJobs.push_front(
					std::move(active.job));
				return;
			}
			if (active.sharedDoubleAlias && !sharedRoleReady)
			{
				const FontConfig* owner = FindConfig(
					config->mtsdfDoubleByteOwnerFontId);
				const UInt64 ownerProfileKey = owner
					? BuildProfileKey(*owner, active.job.route) : 0;
				const bool ownerPending = ownerProfileKey
					&& std::any_of(
						PrewarmRuntime().session.generationJobs.begin(),
						PrewarmRuntime().session.generationJobs.end(),
						[&](const PrewarmJob& pending)
						{
							return pending.profileKey
								== ownerProfileKey;
						});
				if (ownerPending
					&& active.job.dependencyDeferrals < 2)
				{
					++active.job.dependencyDeferrals;
					gLog.FormattedMessage(
						"tnvse_freetype_font: shared distance-field double-byte role unavailable font=%u owner=%u; requeued behind pending owner attempt=%u",
						active.job.fontId,
						config->mtsdfDoubleByteOwnerFontId,
						active.job.dependencyDeferrals);
					PrewarmRuntime().session.generationJobs.push_back(
						std::move(active.job));
					return;
				}
				gLog.FormattedMessage(
					"tnvse_freetype_font: shared distance-field double-byte role unavailable font=%u owner=%u; owner is not pending",
					active.job.fontId,
					config->mtsdfDoubleByteOwnerFontId);
				FinishJob(active.job, "shared-role-unavailable");
				++PrewarmRuntime().session.streamFailedFonts;
				++PrewarmRuntime().session.finishedFonts;
				return;
			}

			EnforceCpuMemoryBudget("prewarm-font-begin");
			const PrewarmBatchPolicy batchPolicy =
				ResolvePrewarmBatchPolicy(
					*config, PrewarmRuntime().session.rasterScale,
					active.shaderSdf, active.aggressiveComposite,
					active.sdfSpread);
			active.maximumBatchGlyphLimit =
				batchPolicy.maximumGlyphs;
			active.batchGlyphLimit = batchPolicy.initialGlyphs;
			active.parallelBatchFloor =
				batchPolicy.parallelFloor;
			active.maximumRasterWorkers = batchPolicy.maximumWorkers;
			active.estimatedRetainedBytesPerGlyph =
				batchPolicy.estimatedRetainedBytesPerGlyph;
			active.estimatedTransientBytesPerWorker =
				batchPolicy.estimatedTransientBytesPerWorker;
			active.estimatedPeakBatchBytes =
				batchPolicy.estimatedPeakBytes;
			active.targetBatchBytes = batchPolicy.targetBytes;
			const size_t pendingWorkingBytes =
				active.targetBatchBytes <= std::numeric_limits<size_t>::max()
					- kMaximumPrewarmStreamingWorkingBytes
				? active.targetBatchBytes
					+ kMaximumPrewarmStreamingWorkingBytes
				: std::numeric_limits<size_t>::max();
			ProcessVirtualMemoryHeadroom headroom;
			if (!HasProcessVirtualMemoryHeadroom(pendingWorkingBytes,
					kFontPrewarmVirtualReserveBytes, &headroom))
			{
				RecordPrewarmMemoryPressure(active.job);
				PreparePrewarmMemoryRetry("font-begin-headroom",
					active.job.fontId, active.job.memoryRetries,
					pendingWorkingBytes);
				active.batchGlyphLimit = 1;
				active.maximumBatchGlyphLimit = 1;
				active.parallelBatchFloor = 1;
				active.maximumRasterWorkers = 1;
				active.metricsOnlyBatchGlyphLimit = 1;
				active.constrainedMemory = true;
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm font entered constrained same-session mode font=%u batchGlyphs=1 availableVirtualMiB=%.2f largestFreeMiB=%.2f runtimeFallback=0",
					active.job.fontId,
					headroom.availableBytes / (1024.0 * 1024.0),
					headroom.largestFreeRegionBytes / (1024.0 * 1024.0));
			}
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm batch policy font=%u initial=%u parallelFloor=%u maximum=%u workers=%u metricsOnlyMaximum=%u retainedBytesPerGlyph=%llu transientBytesPerWorker=%llu estimatedPeakMiB=%.2f targetMiB=%.2f memoryMiB=%.2f/%.2f",
				active.job.fontId,
				active.batchGlyphLimit,
				active.parallelBatchFloor,
				active.maximumBatchGlyphLimit,
				active.maximumRasterWorkers,
				active.metricsOnlyBatchGlyphLimit,
				static_cast<unsigned long long>(
					active.estimatedRetainedBytesPerGlyph),
				static_cast<unsigned long long>(
					active.estimatedTransientBytesPerWorker),
				active.estimatedPeakBatchBytes
					/ (1024.0 * 1024.0),
				active.targetBatchBytes / (1024.0 * 1024.0),
				GetCpuMemoryUsage() / (1024.0 * 1024.0),
				GetCpuMemoryBudget() / (1024.0 * 1024.0));
			if (active.job.route != FontAtlasRoute::ArgbFallback
				&& !PrewarmRuntime().atlasOnlyPrewarmPending)
			{
				BeginCompleteCodePageAtlasOnlyPrewarm();
				PrewarmRuntime().atlasOnlyPrewarmPending = true;
				PrewarmRuntime().session.atlasOnlyTransactionStarted = true;
				gLog.FormattedMessage(
					"tnvse_freetype_font: incremental prewarm atlas-only transaction begin font=%u",
					active.job.fontId);
			}
			PrewarmRuntime().session.activeFont = std::move(active);
			// The progress component is generation-only. A complete cache-hit
			// startup performs snapshot validation and restoration without ever
			// loading or displaying the Tile tree.
			ReportPrewarmProgress(
				PrewarmRuntime().session.activeFont->job,
				std::min(PrewarmRuntime().session.queuedFonts,
					PrewarmRuntime().session.finishedFonts + 1),
				PrewarmRuntime().session.queuedFonts,
				PrewarmRuntime().session.finishedFonts,
				L"Preparing streamed glyph batches...",
				0.0f, true);
			TransitionPrewarmPhase(PrewarmPhase::GenerateBatch);
		}

		void GenerateOnePrewarmBatch()
		{
			if (!PrewarmRuntime().session.activeFont)
			{
				TransitionPrewarmPhase(PrewarmPhase::BeginFont);
				return;
			}
			ActivePrewarmFont& active = *PrewarmRuntime().session.activeFont;
			const FontConfig* config = nullptr;
			RuntimeFont* runtime = nullptr;
			if (!ResolveValidPrewarmJob(active.job, config, runtime))
			{
				active.cancelled = true;
				TransitionPrewarmPhase(PrewarmPhase::FinalizeFont);
				return;
			}

			const ULONGLONG stepStarted = GetTickCount64();
			const size_t encodedUnitStart =
				active.job.encodedUnitIndex;
			const UInt32 doubleByteStart =
				active.job.validDoubleByteCount;
			const UInt32 rasterizedStart =
				active.job.rasterizedGlyphCount;
			const UInt32 sdfStart = active.job.sdfGlyphCount;
			const bool metricsOnlyDoubleByte =
				active.sharedDoubleAlias
				&& active.job.encodedUnits
				&& active.job.encodedUnitIndex
					< active.job.encodedUnits->size()
				&& (*active.job.encodedUnits)[
					active.job.encodedUnitIndex] > 0xFF;
			UInt32& selectedBatchLimit = metricsOnlyDoubleByte
				? active.metricsOnlyBatchGlyphLimit
				: active.batchGlyphLimit;
			const UInt32 requestedBatchLimit =
				std::max<UInt32>(1, selectedBatchLimit);
			const UInt32 candidateLimit = std::min(
				kMaximumCandidatesPerBatch,
				std::max<UInt32>(
					256, requestedBatchLimit * 8u));

			auto rollbackBatch = [&]()
			{
				active.job.encodedUnitIndex = encodedUnitStart;
				active.job.validDoubleByteCount = doubleByteStart;
				active.job.rasterizedGlyphCount = rasterizedStart;
				active.job.sdfGlyphCount = sdfStart;
				active.exhausted = false;
			};
			auto retryBatchAfterMemoryPressure = [&](const char* stage) -> bool
			{
				const UInt32 previous = selectedBatchLimit;
				if (selectedBatchLimit > 1)
				{
					selectedBatchLimit = std::max<UInt32>(
						1, selectedBatchLimit / 2u);
					if (!metricsOnlyDoubleByte)
					{
						active.maximumBatchGlyphLimit = std::min(
							active.maximumBatchGlyphLimit,
							selectedBatchLimit);
						active.parallelBatchFloor = std::min(
							active.parallelBatchFloor,
							active.maximumBatchGlyphLimit);
						const UInt32 reducedWorkers =
							active.maximumRasterWorkers > 1
							? active.maximumRasterWorkers / 2u : 1u;
						active.maximumRasterWorkers = std::max<UInt32>(
							1, std::min(reducedWorkers, selectedBatchLimit));
					}
				}
				active.constrainedMemory = true;
				IncrementSaturating(active.allocationRetries);
				RecordPrewarmMemoryPressure(active.job);
				rollbackBatch();
				PreparePrewarmMemoryRetry(stage, active.job.fontId,
					active.job.memoryRetries,
					std::max(active.targetBatchBytes,
						kMaximumPrewarmStreamingWorkingBytes));
				const bool retryAllowed = CanRetryAfterMemoryFailure(
						active.allocationRetries,
						kMaximumBatchAllocationFailures)
					&& CanRetryAfterMemoryFailure(active.job.memoryRetries,
						kMaximumFontMemoryFailures);
				if (!retryAllowed)
				{
					active.failed = true;
					active.terminalFailure = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: prewarm batch memory retries exhausted stage=%s font=%u batchLimit=%u allocationRetries=%u fontMemoryRetries=%u policy=terminate-prewarm",
						stage ? stage : "unknown", active.job.fontId,
						selectedBatchLimit, active.allocationRetries,
						active.job.memoryRetries);
				}
				return retryAllowed;
			};

			PrewarmRuntime().session.requestedGlyphs.clear();
			PrewarmRuntime().session.bitmapRequests.clear();
			PrewarmRuntime().session.bitmapResults.clear();
			try
			{
				PrewarmRuntime().session.requestedGlyphs.reserve(
					requestedBatchLimit);
				if (!metricsOnlyDoubleByte)
				{
					PrewarmRuntime().session.bitmapRequests.reserve(
						static_cast<size_t>(
							requestedBatchLimit) * 4u);
				}
			}
			catch (const std::bad_alloc&)
			{
				const UInt32 previous = selectedBatchLimit;
				const bool retryAllowed = retryBatchAfterMemoryPressure(
					"request-buffer");
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm request-buffer retry font=%u limit=%u->%u metricsOnly=%u retry=%u sameBarrier=1",
					active.job.fontId, previous,
					selectedBatchLimit,
					metricsOnlyDoubleByte ? 1u : 0u,
					active.allocationRetries);
				RecordPrewarmStep(stepStarted);
				if (!retryAllowed)
					TransitionPrewarmPhase(PrewarmPhase::FinalizeFont);
				return;
			}

			const bool needsGrayFill =
				!active.shaderSdf
				&& !active.aggressiveComposite;
			UInt32 candidates = 0;
			UInt32 glyphCount = 0;
			const ULONGLONG scanStarted = GetTickCount64();
			try
			{
				while (candidates < candidateLimit
					&& glyphCount < requestedBatchLimit)
				{
					std::array<char, 2> bytes = {};
					size_t length = 0;
					if (!NextEncodedUnit(active.job, bytes, length))
					{
						active.exhausted = true;
						break;
					}
					++candidates;
					VectorEncodedGlyph glyph;
					if (!ResolvePrewarmGlyph(
							*runtime, bytes.data(), length, glyph))
					{
						continue;
					}
					if (length == 2)
						++active.job.validDoubleByteCount;
					PrewarmRuntime().session.requestedGlyphs.push_back(glyph);
					const VectorEncodedGlyph* requested =
						&PrewarmRuntime().session.requestedGlyphs.back();
					if (needsGrayFill)
					{
						PrewarmRuntime().session.bitmapRequests.push_back({
							requested, GlyphMaskType::Fill, 0
						});
					}
					if (active.aggressiveComposite)
					{
						PrewarmRuntime().session.bitmapRequests.push_back({
							requested, GlyphMaskType::Composite, 0
						});
					}
					if (active.shaderSdf
						&& (length == 1
							|| !active.sharedDoubleAlias))
					{
						PrewarmRuntime().session.bitmapRequests.push_back({
							requested, GlyphMaskType::DistanceField,
							active.sdfSpread
						});
						++active.job.sdfGlyphCount;
					}
					if (config->glow.enabled && needsGrayFill)
					{
						PrewarmRuntime().session.bitmapRequests.push_back({
							requested, GlyphMaskType::Glow, 0
						});
					}
					if (config->outline.enabled && needsGrayFill)
					{
						PrewarmRuntime().session.bitmapRequests.push_back({
							requested, GlyphMaskType::Outline, 0
						});
					}
					if (config->shadow.enabled && needsGrayFill)
					{
						PrewarmRuntime().session.bitmapRequests.push_back({
							requested, GlyphMaskType::Shadow, 0
						});
					}
					++glyphCount;
					++active.job.rasterizedGlyphCount;
				}
			}
			catch (const std::bad_alloc&)
			{
				PrewarmRuntime().session.scanMs += GetTickCount64() - scanStarted;
				const UInt32 previous = selectedBatchLimit;
				const bool retryAllowed =
					retryBatchAfterMemoryPressure("glyph-scan");
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm scan allocation retry font=%u limit=%u->%u metricsOnly=%u retry=%u sameBarrier=1",
					active.job.fontId, previous,
					selectedBatchLimit,
					metricsOnlyDoubleByte ? 1u : 0u,
					active.allocationRetries);
				RecordPrewarmStep(stepStarted);
				if (!retryAllowed)
					TransitionPrewarmPhase(PrewarmPhase::FinalizeFont);
				return;
			}
			catch (...)
			{
				PrewarmRuntime().session.scanMs += GetTickCount64() - scanStarted;
				rollbackBatch();
				active.failed = true;
				ReleasePrewarmBatchReferences(
					"prewarm-scan-unexpected-failure");
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm glyph scan raised an unexpected exception font=%u",
					active.job.fontId);
				TransitionPrewarmPhase(
					PrewarmPhase::FinalizeFont);
				RecordPrewarmStep(stepStarted);
				return;
			}
			PrewarmRuntime().session.scanMs += GetTickCount64() - scanStarted;
			PrewarmRuntime().session.peakBatchGlyphs = std::max(
				PrewarmRuntime().session.peakBatchGlyphs, glyphCount);

			bool retryMemoryBatch = false;
			if (!PrewarmRuntime().session.bitmapRequests.empty())
			{
				const ULONGLONG rasterStarted = GetTickCount64();
				try
				{
					GetPrewarmGlyphBitmaps(
						*runtime,
						PrewarmRuntime().session.bitmapRequests,
						PrewarmRuntime().session.rasterScale,
						PrewarmRuntime().session.bitmapResults,
						active.maximumRasterWorkers);
				}
				catch (const std::bad_alloc&)
				{
					const UInt32 previous = selectedBatchLimit;
					retryMemoryBatch = retryBatchAfterMemoryPressure(
						"glyph-raster");
					gLog.FormattedMessage(
						"tnvse_freetype_font: prewarm raster allocation retry font=%u scale=%.3f batchGlyphs=%u limit=%u->%u retry=%u sameBarrier=1",
						active.job.fontId,
						PrewarmRuntime().session.rasterScale,
						glyphCount, previous,
						selectedBatchLimit,
						active.allocationRetries);
				}
				catch (...)
				{
					active.failed = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: streamed prewarm batch raised an unexpected exception font=%u",
						active.job.fontId);
				}
				PrewarmRuntime().session.rasterMs +=
					GetTickCount64() - rasterStarted;

				if (!active.failed && !retryMemoryBatch
					&& PrewarmRuntime().session.bitmapResults.size()
						!= PrewarmRuntime().session.bitmapRequests.size())
				{
					active.failed = true;
				}
				if (!active.failed && !retryMemoryBatch)
				{
					const ULONGLONG streamStarted =
						GetTickCount64();
					bool streamAllocationFailed = false;
					const bool appended = AppendStreamingPrewarmAtlas(
						*runtime,
						PrewarmRuntime().session.bitmapRequests,
						PrewarmRuntime().session.bitmapResults,
						PrewarmRuntime().session.rasterScale,
						&streamAllocationFailed);
					PrewarmRuntime().session.streamMs +=
						GetTickCount64() - streamStarted;
					if (!appended && streamAllocationFailed)
					{
						const UInt32 previous = selectedBatchLimit;
						retryMemoryBatch = retryBatchAfterMemoryPressure(
							"stream-append");
						gLog.FormattedMessage(
							"tnvse_freetype_font: prewarm stream append allocation retry font=%u batchGlyphs=%u limit=%u->%u retry=%u statePreserved=1 sameBarrier=1",
							active.job.fontId, glyphCount, previous,
							selectedBatchLimit,
							active.allocationRetries);
					}
					else if (!appended)
					{
						active.failed = true;
					}
				}
			}

			if (!retryMemoryBatch)
			{
				ReleasePrewarmBatchReferences(
					"prewarm-stream-batch");
			}
			++PrewarmRuntime().session.batches;
			const ULONGLONG elapsed =
				GetTickCount64() - stepStarted;
			PrewarmRuntime().session.maximumStepMs =
				std::max(PrewarmRuntime().session.maximumStepMs, elapsed);
			if (!retryMemoryBatch && !active.failed)
			{
				active.allocationRetries = 0;
				if (!metricsOnlyDoubleByte && glyphCount)
				{
					active.batchGlyphLimit =
						ResolveNextPrewarmBatchLimit(
							active.batchGlyphLimit,
							active.maximumBatchGlyphLimit,
							active.parallelBatchFloor,
							glyphCount, elapsed);
				}
			}

			ReportPrewarmProgress(
				active.job,
				std::min(PrewarmRuntime().session.queuedFonts,
					PrewarmRuntime().session.finishedFonts + 1),
				PrewarmRuntime().session.queuedFonts,
				PrewarmRuntime().session.finishedFonts,
				retryMemoryBatch
					? L"Retrying this batch after memory pressure..."
					: metricsOnlyDoubleByte
						? L"Indexing shared MTSDF double-byte metrics..."
						: active.shaderSdf
						? (UsesMtsdfDistanceField()
							? L"Generating and caching MTSDF glyphs..."
							: L"Generating and caching true-SDF glyphs...")
						: active.aggressiveComposite
							? L"Generating and caching BGRA composite glyphs..."
							: L"Generating bounded fallback masks...",
				0.0f, retryMemoryBatch);

			if (active.failed || active.exhausted)
				TransitionPrewarmPhase(
					PrewarmPhase::FinalizeFont);
		}

		void FinalizeActivePrewarmFont()
		{
			// Worker faces are useful only while raster batches are active. Drop
			// them before atlas consolidation can allocate a 256 MiB 8192x8192 page.
			ReleasePrewarmRasterWorkerContexts();
			if (!PrewarmRuntime().session.activeFont)
			{
				TransitionPrewarmPhase(PrewarmPhase::BeginFont);
				return;
			}
			const ULONGLONG stepStarted = GetTickCount64();
			ActivePrewarmFont& active = *PrewarmRuntime().session.activeFont;
			const FontConfig* config = nullptr;
			RuntimeFont* runtime = nullptr;
			if (!ResolveValidPrewarmJob(
					active.job, config, runtime))
			{
				active.cancelled = true;
			}

			if (active.cancelled || !runtime || !config)
			{
				if (runtime)
					CancelStreamingPrewarmAtlas(*runtime);
				FinishJob(active.job, "cancelled");
				++PrewarmRuntime().session.cancelledFonts;
				++PrewarmRuntime().session.finishedFonts;
				RefreshNativePrewarmOverlayTextGeometry();
				PrewarmRuntime().session.activeFont.reset();
				RecordPrewarmStep(stepStarted);
				TransitionPrewarmPhase(PrewarmPhase::BeginFont);
				return;
			}
			if (active.terminalFailure)
			{
				AbortPrewarmTransaction(
					"batch-memory-retries-exhausted");
				RecordPrewarmStep(stepStarted);
				return;
			}

			if (active.failed)
			{
				CancelStreamingPrewarmAtlas(*runtime);
				const bool discarded = DiscardGlyphAtlasSnapshot(
					*runtime, PrewarmRuntime().session.rasterScale);
				IncrementSaturating(active.job.generationRestarts);
				if (!CanRestartFontGeneration(
						active.job.generationRestarts))
				{
					AbortPrewarmTransaction(
						"font-generation-restarts-exhausted");
					RecordPrewarmStep(stepStarted);
					return;
				}
				PreparePrewarmScanForGeneration(active.job, *config,
					PrewarmRuntime().session.rasterScaleMilli);
				gLog.FormattedMessage(
					"tnvse_freetype_font: streamed batch failure font=%u atlas=%s generationRestart=%u retryInSameBarrier=1 runtimeFallback=0",
					active.job.fontId,
					discarded ? "discarded" : "delete-failed",
					active.job.generationRestarts);
				PrewarmJob retryJob = std::move(active.job);
				PrewarmRuntime().session.activeFont.reset();
				PrewarmRuntime().session.generationJobs.push_front(std::move(retryJob));
				RefreshNativePrewarmOverlayTextGeometry();
				RecordPrewarmStep(stepStarted);
				TransitionPrewarmPhase(PrewarmPhase::BeginFont);
				return;
			}

			ReportPrewarmProgress(
				active.job,
				std::min(PrewarmRuntime().session.queuedFonts,
					PrewarmRuntime().session.finishedFonts + 1),
				PrewarmRuntime().session.queuedFonts,
				PrewarmRuntime().session.finishedFonts,
				L"Publishing, globally repacking, and loading atlas pages...",
				0.81f, true);
			bool finalized = false;
			bool finalizationMemoryPressure = false;
			StreamingPrewarmFinalization finalization;
			bool finalizationPrepared = false;
			ResetAtlasAllocationMemoryPressure();
			try
			{
				finalizationPrepared =
					PrepareStreamingPrewarmAtlasFinalization(
						*runtime, PrewarmRuntime().session.rasterScale,
						finalization);
			}
			catch (const std::bad_alloc&)
			{
				finalizationMemoryPressure = true;
			}
			catch (...)
			{
				ResetAtlasAllocationMemoryPressure();
				throw;
			}
			finalizationMemoryPressure =
				ConsumeAtlasAllocationMemoryPressure()
				|| finalizationMemoryPressure;
			if (finalizationPrepared && finalization.restoreRequired
				&& finalization.repacked && !finalizationMemoryPressure)
			{
				PrewarmAtlasRequestResult restoreResult;
				if (!ExecutePrewarmAtlasRequestOnLoadingThread(
						PrewarmAtlasRequestKind::RebuildPublishedSnapshot,
						active.job.fontId,
						PrewarmRuntime().session.rasterScale,
						restoreResult))
				{
					AbortPrewarmTransaction(
						"loading-thread-finalize-service-unavailable");
					RecordPrewarmStep(stepStarted);
					return;
				}
				finalizationMemoryPressure = restoreResult.memoryPressure;
				finalized = CompleteStreamingPrewarmAtlasFinalization(
					*runtime, PrewarmRuntime().session.rasterScale,
					finalization, restoreResult.succeeded,
					restoreResult.queueMs + restoreResult.executionMs);
			}
			else if (finalizationPrepared)
			{
				finalized = CompleteStreamingPrewarmAtlasFinalization(
					*runtime, PrewarmRuntime().session.rasterScale,
					finalization, !finalization.restoreRequired,
					0);
			}

			if (finalized
				&& active.job.route == FontAtlasRoute::ArgbFallback
				&& !MarkCurrentFallbackBitmapProfilesUsed(
					*runtime, PrewarmRuntime().session.rasterScale))
			{
				finalized = false;
				gLog.FormattedMessage(
					"tnvse_freetype_font: fallback prewarm validation failed font=%u reason=incomplete-persistent-mask-set",
					active.job.fontId);
			}

			bool directReady = false;
			if (finalized && !finalizationMemoryPressure)
			{
				try
				{
					directReady = BuildDirectGlyphAtlasTables(
							*runtime, PrewarmRuntime().session.rasterScale)
						&& PublishSealedProfileAliases(
							active.job.fontId,
							PrewarmRuntime().session.rasterScale);
				}
				catch (const std::bad_alloc&)
				{
					finalizationMemoryPressure = true;
				}
			}

			if (finalizationMemoryPressure)
			{
				CancelStreamingPrewarmAtlas(*runtime);
				RecordPrewarmMemoryPressure(active.job);
				PreparePrewarmMemoryRetry("stream-finalize",
					active.job.fontId, active.job.memoryRetries,
					kMaximumPrewarmPhysicalAllocationBytes);
				if (!CanRetryAfterMemoryFailure(active.job.memoryRetries,
						kMaximumFontMemoryFailures))
				{
					AbortPrewarmTransaction(
						"stream-finalize-memory-retries-exhausted");
					RecordPrewarmStep(stepStarted);
					return;
				}
				gLog.FormattedMessage(
					"tnvse_freetype_font: streamed finalization memory pressure font=%u scale=%.3f snapshotPreserved=1 restoreRetryInSameBarrier=1 runtimeFallback=0",
					active.job.fontId, PrewarmRuntime().session.rasterScale);
				PrewarmJob retryJob = std::move(active.job);
				PrewarmRuntime().session.activeFont.reset();
				PrewarmRuntime().session.restoreJobs.push_front(std::move(retryJob));
				RefreshNativePrewarmOverlayTextGeometry();
				ReportPrewarmTransactionProgress(
					L"Font cache rebuild",
					L"Retrying snapshot loading after memory pressure...",
					PrewarmRuntime().rebuildProgress, true);
				RecordPrewarmStep(stepStarted);
				TransitionPrewarmPhase(PrewarmPhase::RestoreSnapshots);
				return;
			}

			if (!finalized || !directReady)
			{
				CancelStreamingPrewarmAtlas(*runtime);
				IncrementSaturating(active.job.generationRestarts);
				if (!CanRestartFontGeneration(
						active.job.generationRestarts))
				{
					AbortPrewarmTransaction(
						"font-finalization-restarts-exhausted");
					RecordPrewarmStep(stepStarted);
					return;
				}
				gLog.FormattedMessage(
					"tnvse_freetype_font: streamed finalization validation retry font=%u finalized=%u directReady=%u generationRestart=%u snapshotPreserved=1 sameBarrier=1",
					active.job.fontId, finalized ? 1u : 0u,
					directReady ? 1u : 0u,
					active.job.generationRestarts);
				PrewarmJob retryJob = std::move(active.job);
				PrewarmRuntime().session.activeFont.reset();
				PrewarmRuntime().session.restoreJobs.push_front(std::move(retryJob));
				RefreshNativePrewarmOverlayTextGeometry();
				ReportPrewarmTransactionProgress(
					L"Font cache rebuild",
					L"Retrying final cache validation...",
					PrewarmRuntime().rebuildProgress, true);
				RecordPrewarmStep(stepStarted);
				TransitionPrewarmPhase(PrewarmPhase::RestoreSnapshots);
				return;
			}

			PrewarmRuntime().session.verifiedCodePageFonts.push_back(
				active.job.fontId);
			RefreshNativePrewarmOverlayTextGeometry();
			ReportPrewarmProgress(
				active.job,
				std::min(PrewarmRuntime().session.queuedFonts,
					PrewarmRuntime().session.finishedFonts + 1),
				PrewarmRuntime().session.queuedFonts,
				PrewarmRuntime().session.finishedFonts,
				L"Font atlas committed...", 1.0f, true);
			FinishJob(active.job, "complete");
			++PrewarmRuntime().session.completedFonts;
			++PrewarmRuntime().session.finishedFonts;
			PrewarmRuntime().session.activeFont.reset();
			RecordPrewarmStep(stepStarted);
			TransitionPrewarmPhase(PrewarmPhase::BeginFont);
		}

		void CollectPrewarmProfileResults()
		{
			for (UInt32 fontId : PrewarmRuntime().session.verifiedCodePageFonts)
			{
				if (const FontConfig* config = FindConfig(fontId))
				{
					PrewarmRuntime().session.verifiedProfileKeys.insert(
						BuildProfileKey(
							*config,
							GetPersistentFontCacheRoute()));
				}
			}
			PrewarmRuntime().session.atlasOnlyFontIds.reserve(g_configs.size());
			for (const auto& entry : g_configs)
			{
				const UInt64 profileKey = BuildProfileKey(
					entry.second, GetPersistentFontCacheRoute());
				PrewarmRuntime().session.configuredProfileKeys.insert(profileKey);
				if (FindRuntimeFont(entry.first))
				{
					++PrewarmRuntime().session.readyConfiguredRuntimes;
					if (PrewarmRuntime().session.verifiedProfileKeys.count(
							profileKey))
					{
						PrewarmRuntime().session.atlasOnlyFontIds.push_back(
							entry.first);
					}
				}
			}
			std::sort(
				PrewarmRuntime().session.atlasOnlyFontIds.begin(),
				PrewarmRuntime().session.atlasOnlyFontIds.end());
			PrewarmRuntime().session.everyConfiguredJobCompleted =
				PrewarmRuntime().session.completedFonts == PrewarmRuntime().session.queuedFonts
				&& PrewarmRuntime().session.queuedFonts
					== static_cast<UInt32>(
						PrewarmRuntime().session.configuredProfileKeys.size())
				&& PrewarmRuntime().session.readyConfiguredRuntimes
					== static_cast<UInt32>(g_configs.size());
			PrewarmRuntime().session.everyConfiguredProfileVerified =
				PrewarmRuntime().session.everyConfiguredJobCompleted
				&& PrewarmRuntime().session.verifiedCodePageFonts.size()
					== PrewarmRuntime().session.queuedFonts
				&& PrewarmRuntime().session.verifiedProfileKeys.size()
					== PrewarmRuntime().session.configuredProfileKeys.size();
		}

		void FinishIncrementalSession()
		{
			PrewarmRuntime().session.success =
				PrewarmRuntime().session.everyConfiguredJobCompleted
				&& PrewarmRuntime().session.everyConfiguredProfileVerified;
			gLog.FormattedMessage(
				"tnvse_freetype_font: incremental streamed prewarm end fonts=%u complete=%u streamFailed=%u cancelled=%u batches=%u peakBatchGlyphs=%u elapsedMs=%llu maxStepMs=%llu scanMs=%llu rasterMs=%llu streamMs=%llu memoryRetries=%u transactionRestarts=%u atlasOnlyTransaction=%s progressReporting=%s progressOverlay=loading-thread-queued runtimeFallback=0",
				PrewarmRuntime().session.queuedFonts,
				PrewarmRuntime().session.completedFonts,
				PrewarmRuntime().session.streamFailedFonts,
				PrewarmRuntime().session.cancelledFonts,
				PrewarmRuntime().session.batches,
				PrewarmRuntime().session.peakBatchGlyphs,
				static_cast<unsigned long long>(
					GetTickCount64() - PrewarmRuntime().session.started),
				static_cast<unsigned long long>(
					PrewarmRuntime().session.maximumStepMs),
				static_cast<unsigned long long>(
					PrewarmRuntime().session.scanMs),
				static_cast<unsigned long long>(
					PrewarmRuntime().session.rasterMs),
				static_cast<unsigned long long>(
					PrewarmRuntime().session.streamMs),
				PrewarmRuntime().session.memoryRetries,
				PrewarmRuntime().session.transactionRestarts,
				!PrewarmRuntime().session.atlasOnlyTransactionStarted
					? "not-started"
					: PrewarmRuntime().session.success ? "complete" : "incomplete",
				PrewarmRuntime().rebuildProgressTracked
					? "cache-write-loading-thread-queued" : "cache-hit-hidden");
			if (!PrewarmRuntime().session.success)
			{
				ResetPrewarmTransactionForRetry(
					"incomplete-profile-validation");
				return;
			}
			ReportPrewarmTransactionProgress(
				L"Font cache rebuild complete",
				L"All generated font caches are ready...",
				1.0f, true);
			TransitionPrewarmPhase(PrewarmPhase::Complete);
		}
	}
}

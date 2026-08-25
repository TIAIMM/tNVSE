#include "font_prewarm_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_prewarm {}
	using namespace implementation::font_prewarm;

	namespace implementation::font_prewarm
	{
		UInt64 BuildProfileKey(const FontConfig& config,
			FontAtlasRoute route)
		{
			UInt64 hash = 1469598103934665603ull;
			auto add = [&](const void* data, size_t size)
			{
				const UInt8* bytes = static_cast<const UInt8*>(data);
				for (size_t index = 0; index < size; ++index)
				{
					hash ^= bytes[index];
					hash *= 1099511628211ull;
				}
			};
			add(&config.layoutHash, sizeof(config.layoutHash));
			add(&config.maskGenerationHash, sizeof(config.maskGenerationHash));
			add(&route, sizeof(route));
			if (route == FontAtlasRoute::ShaderDistanceField)
			{
				const DistanceFieldMethod method =
					GetConfiguredDistanceFieldMethod();
				const UInt32 revision = DistanceFieldGeneratorRevision(method);
				add(&method, sizeof(method));
				add(&revision, sizeof(revision));
				// Distance-field pixels depend on the largest physical effect
				// radius, not colors, offsets, powers, or shader sampling quality.
				// Hash the exact unscaled maximum so equal values remain equal at
				// every raster scale while effect-only variants share one prewarm.
				float maximumRadius = 0.0f;
				if (config.glow.enabled)
					maximumRadius = std::max(maximumRadius, config.glow.outer);
				if (config.outline.enabled)
					maximumRadius = std::max(maximumRadius,
						config.outline.width + config.outline.softness);
				if (config.shadow.enabled && config.shadow.blur > 0.0f)
					maximumRadius = std::max(maximumRadius, config.shadow.blur);
				add(&maximumRadius, sizeof(maximumRadius));
			}
			else
			{
				// CPU fallback masks bake the complete effect configuration.
				add(&config.shaderEffectHash, sizeof(config.shaderEffectHash));
			}
			add(&kCompleteCodePagePrewarmIdentity,
				sizeof(kCompleteCodePagePrewarmIdentity));
			const UInt32 codePage = GetFreeTypeTextCodePage();
			add(&codePage, sizeof(codePage));
			const FontPrewarmRange prewarmRange =
				ResolveFontPrewarmRange(config);
			add(&prewarmRange, sizeof(prewarmRange));
			return hash;
		}

		bool MatchesPrewarmProfile(const PrewarmJob& job, const FontConfig& config)
		{
			return job.route == GetPersistentFontCacheRoute()
				&& job.profileKey == BuildProfileKey(config, job.route)
				&& job.codePage == GetFreeTypeTextCodePage();
		}

		bool IsPrewarmJobDoubleByteAlias(const PrewarmJob& job)
		{
			const FontConfig* config = FindConfig(job.fontId);
			return UsesDbcsTextLayout()
				&& config
				&& job.route == FontAtlasRoute::ShaderDistanceField
				&& IsMtsdfAtlasAlias(
					*config, VectorFontByteClass::DoubleByte);
		}

		void SortPrewarmJobsByDependencies(std::deque<PrewarmJob>& jobs)
		{
			if (jobs.size() < 2)
				return;
			std::vector<PrewarmJob> ordered;
			ordered.reserve(jobs.size());
			while (!jobs.empty())
			{
				ordered.push_back(std::move(jobs.front()));
				jobs.pop_front();
			}
			std::stable_sort(ordered.begin(), ordered.end(),
				[](const PrewarmJob& left, const PrewarmJob& right)
				{
					const bool leftAlias =
						IsPrewarmJobDoubleByteAlias(left);
					const bool rightAlias =
						IsPrewarmJobDoubleByteAlias(right);
					if (leftAlias != rightAlias)
						return !leftAlias;
					const FontConfig* leftConfig =
						FindConfig(left.fontId);
					const FontConfig* rightConfig =
						FindConfig(right.fontId);
					const UInt32 leftOwner = leftAlias && leftConfig
						? leftConfig->mtsdfDoubleByteOwnerFontId
						: left.fontId;
					const UInt32 rightOwner = rightAlias && rightConfig
						? rightConfig->mtsdfDoubleByteOwnerFontId
						: right.fontId;
					return leftOwner != rightOwner
						? leftOwner < rightOwner
						: left.fontId < right.fontId;
				});
			for (PrewarmJob& job : ordered)
				jobs.push_back(std::move(job));
		}

		bool NextEncodedUnit(PrewarmJob& job, std::array<char, 2>& bytes,
			size_t& length)
		{
			if (!job.encodedUnits)
				return false;
			const std::vector<UInt16>& units = *job.encodedUnits;
			if (job.encodedUnitIndex >= units.size())
				return false;
			const UInt16 encoded = units[job.encodedUnitIndex++];
			bytes[0] = static_cast<char>(encoded > 0xFF
				? encoded >> 8 : encoded);
			bytes[1] = static_cast<char>(encoded & 0xFF);
			length = encoded > 0xFF ? 2 : 1;
			return true;
		}

		void ResetPrewarmScan(PrewarmJob& job, UInt32 rasterScaleMilli)
		{
			job.encodedUnits = nullptr;
			job.encodedUnitStart = 0;
			job.encodedUnitIndex = 0;
			job.validDoubleByteCount = 0;
			job.rasterizedGlyphCount = 0;
			job.knownEmptyGlyphCount = 0;
			job.sdfGlyphCount = 0;
			job.rasterScaleMilli = rasterScaleMilli;
			job.targetUnitCount = 0;
		}

		PrewarmJob BuildQueuedPrewarmJob(UInt32 fontId,
			const FontConfig& config, FontAtlasRoute route, UInt64 profileKey)
		{
			PrewarmJob job;
			job.fontId = fontId;
			job.profileKey = profileKey;
			job.layoutHash = config.layoutHash;
			job.maskGenerationHash = config.maskGenerationHash;
			job.shaderEffectHash = config.shaderEffectHash;
			job.codePage = GetFreeTypeTextCodePage();
			job.route = route;
			job.prewarmRange = ResolveFontPrewarmRange(config);
			ResetPrewarmScan(job, 0);
			return job;
		}

		void PreparePrewarmScanForGeneration(PrewarmJob& job,
			const FontConfig& config,
			UInt32 rasterScaleMilli)
		{
			ResetPrewarmScan(job, rasterScaleMilli);
			const std::vector<UInt16>& units =
				GetFontPrewarmEncodedUnits(config);
			job.encodedUnits = &units;
			job.encodedUnitStart = static_cast<size_t>(std::lower_bound(
				units.begin(), units.end(), static_cast<UInt16>(0x20)) - units.begin());
			job.encodedUnitIndex = job.encodedUnitStart;
			job.targetUnitCount = static_cast<UInt32>(
				units.size() - job.encodedUnitStart);
		}

		size_t SaturatingMultiply(size_t left, size_t right)
		{
			return !left || right <= std::numeric_limits<size_t>::max() / left
				? left * right : std::numeric_limits<size_t>::max();
		}

		size_t SaturatingAdd(size_t left, size_t right)
		{
			return right <= std::numeric_limits<size_t>::max() - left
				? left + right : std::numeric_limits<size_t>::max();
		}


		size_t EstimatePrewarmBatchBytes(UInt32 glyphs,
			UInt32 workItemsPerGlyph, bool expensiveWork,
			size_t retainedBytesPerGlyph,
			size_t transientBytesPerWorker,
			UInt32 maximumWorkers)
		{
			const size_t workItems = SaturatingMultiply(
				glyphs, std::max<UInt32>(1, workItemsPerGlyph));
			const size_t parallelThreshold =
				expensiveWork
					? kExpensivePrewarmParallelThreshold
					: kFillPrewarmParallelThreshold;
			const size_t usefulWorkers = expensiveWork
				? workItems
				: (workItems + kFillPrewarmWorkChunk - 1u)
					/ kFillPrewarmWorkChunk;
			const size_t workers = workItems < parallelThreshold
				? 1u : std::min<size_t>(
					usefulWorkers, std::clamp<UInt32>(maximumWorkers,
						1, kMaximumPrewarmRasterWorkers));
			const size_t retainedBytes = SaturatingMultiply(
				glyphs, retainedBytesPerGlyph);
			const size_t workerScratchBytes = SaturatingMultiply(
				workers, transientBytesPerWorker);
			const size_t auxiliaryStackReserveBytes = SaturatingMultiply(
				workers > 1 ? workers - 1u : 0u,
				kPrewarmAuxiliaryThreadStackReserveBytes);
			return SaturatingAdd(retainedBytes,
				SaturatingAdd(workerScratchBytes,
					auxiliaryStackReserveBytes));
		}

		UInt32 ResolveMemoryBoundedGlyphLimit(size_t targetBytes,
			UInt32 workItemsPerGlyph, bool expensiveWork,
			size_t retainedBytesPerGlyph,
			size_t transientBytesPerWorker,
			UInt32 maximumWorkers)
		{
			UInt32 resolved = 1;
			for (UInt32 glyphs = 1;
				glyphs <= kMaximumIncrementalGlyphsPerBatch; ++glyphs)
			{
				if (EstimatePrewarmBatchBytes(
						glyphs, workItemsPerGlyph, expensiveWork,
						retainedBytesPerGlyph,
						transientBytesPerWorker,
						maximumWorkers) > targetBytes)
				{
					break;
				}
				resolved = glyphs;
			}
			return resolved;
		}

		UInt32 ResolveMemoryBoundedWorkerLimit(size_t targetBytes,
			UInt32 workItemsPerGlyph, bool expensiveWork,
			size_t retainedBytesPerGlyph,
			size_t transientBytesPerWorker)
		{
			// Worker-local FreeType/MSDF state, not the number of retained glyph
			// results, determines the parallel scratch peak. Select the widest
			// worker set that still permits a normal 64-glyph batch. This keeps the
			// same memory ceiling while avoiding thousands of tiny thread batches.
			for (UInt32 workers = kMaximumPrewarmRasterWorkers;
				workers > 1; --workers)
			{
				if (EstimatePrewarmBatchBytes(kParallelGlyphBatchFloor,
					workItemsPerGlyph, expensiveWork,
					retainedBytesPerGlyph, transientBytesPerWorker,
					workers) <= targetBytes)
				{
					return workers;
				}
			}
			return 1;
		}

		PrewarmBatchPolicy ResolvePrewarmBatchPolicy(
			const FontConfig& config, float rasterScale, bool shaderSdf,
			bool aggressiveComposite, UInt32 sdfSpread)
		{
			size_t worstRetainedBytes = 1;
			size_t worstTransientBytes = 1;
			UInt32 workItemsPerGlyph = 1;
			bool expensiveWork = shaderSdf || aggressiveComposite;
			const DistanceFieldMethod distanceFieldMethod =
				GetConfiguredDistanceFieldMethod();
			size_t workerFixedBytes = kPrewarmPerWorkerFixedBytes;
			if (shaderSdf)
			{
				// Float fields are accounted per pixel below. Post-quantization
				// repairs also own method-specific bounded staging on each worker.
				workerFixedBytes = SaturatingAdd(workerFixedBytes,
					distanceFieldMethod == DistanceFieldMethod::TrueSdf
					? kTrueSdfRepairPerWorkerScratchBudgetBytes
					: kMtsdfRescuePerWorkerScratchBudgetBytes);
			}
			for (const ByteStyle& style : config.styles)
			{
				const size_t bodyWidth = static_cast<size_t>(std::max(1.0f,
					std::ceil(style.pixelSize * style.scaleX * rasterScale)));
				const size_t bodyHeight = static_cast<size_t>(std::max(1.0f,
					std::ceil(style.pixelSize * style.scaleY * rasterScale)));
				float effectRadius = 2.0f;
				if (shaderSdf)
					effectRadius += static_cast<float>(sdfSpread);
				else
				{
					if (config.glow.enabled)
						effectRadius = std::max(effectRadius,
							config.glow.outer * rasterScale + 3.0f);
					if (config.outline.enabled)
						effectRadius = std::max(effectRadius,
							(config.outline.width + config.outline.softness)
							* rasterScale + 3.0f);
					if (config.shadow.enabled)
					{
						float shadowRadius = config.shadow.blur;
						if (HardShadowIncludesGlow(config))
							shadowRadius = std::max(
								shadowRadius, config.glow.outer);
						if (HardShadowIncludesOutline(config))
						{
							shadowRadius = std::max(shadowRadius,
								config.outline.width
								+ config.outline.softness);
						}
						effectRadius = std::max(effectRadius,
							shadowRadius * rasterScale + 3.0f);
					}
				}
				const size_t expansion = static_cast<size_t>(std::ceil(effectRadius)) * 2u + 2u;
				const size_t width = bodyWidth + expansion;
				const size_t height = bodyHeight + expansion;
				const size_t effectCount =
					(config.shadow.enabled ? 1u : 0u)
					+ (config.glow.enabled ? 1u : 0u)
					+ (config.outline.enabled ? 1u : 0u);
				size_t retainedBytesPerPixel = 1;
				size_t transientBytesPerPixel = 1;
				size_t estimatedWidth = width;
				size_t estimatedHeight = height;
				if (shaderSdf)
				{
					// The whole batch retains only the quantized result. At most
					// one float field per active worker coexists with those results.
					const size_t channels = DistanceFieldBytesPerPixel(
						distanceFieldMethod);
					retainedBytesPerPixel = channels;
					transientBytesPerPixel =
						channels * sizeof(float);
				}
				else if (aggressiveComposite)
				{
					// One BGRA result survives per glyph. A worker can also hold
					// body/effect masks plus the other BGRA target while tight
					// alpha-bound cropping swaps the final result into place.
					retainedBytesPerPixel = 4u;
					transientBytesPerPixel =
						1u + effectCount + 4u;
					if (config.shadow.enabled)
					{
						estimatedWidth = SaturatingAdd(estimatedWidth,
							static_cast<size_t>(std::ceil(
								std::abs(config.shadow.x) * rasterScale)));
						estimatedHeight = SaturatingAdd(estimatedHeight,
							static_cast<size_t>(std::ceil(
								std::abs(config.shadow.y) * rasterScale)));
					}
				}
				else
				{
					// Fill and enabled effect results remain live for the batch.
					// Only the currently executing effect in each worker owns an
					// extra rendered body and four-byte chamfer field.
					retainedBytesPerPixel = 1u + effectCount;
					transientBytesPerPixel =
						effectCount ? 1u + sizeof(float) : 1u;
					workItemsPerGlyph = std::max<UInt32>(
						workItemsPerGlyph,
						static_cast<UInt32>(1u + effectCount));
					expensiveWork = expensiveWork || effectCount != 0;
				}
				const size_t pixels = SaturatingMultiply(
					estimatedWidth, estimatedHeight);
				const size_t retainedBytes = SaturatingAdd(
					SaturatingMultiply(pixels,
						retainedBytesPerPixel),
					kPrewarmPerGlyphMetadataBytes);
				const size_t transientBytes = SaturatingAdd(
					SaturatingMultiply(pixels,
						transientBytesPerPixel),
					workerFixedBytes);
				worstRetainedBytes = std::max(
					worstRetainedBytes, retainedBytes);
				worstTransientBytes = std::max(
					worstTransientBytes, transientBytes);
			}
			const size_t configuredBudget = GetCpuMemoryBudget();
			// Raster scratch is released before atlas finalization and physical-page
			// consolidation. Permit it to use up to one quarter of the configured CPU
			// budget (still capped at 24 MiB) instead of the former one-eighth cap.
			const size_t configuredTarget = std::max(
				kMinimumPrewarmBatchBytes,
				std::min(kMaximumPrewarmBatchBytes, configuredBudget / 4u));
			const size_t currentUsage = GetCpuMemoryUsage();
			const size_t currentHeadroom = currentUsage < configuredBudget
				? configuredBudget - currentUsage : 0;
			// The batch policy is resolved before its first streamed role page is
			// allocated. Reserve that future address-space footprint before
			// assigning half of the remaining headroom to raster outputs.
			const size_t streamingBytesPerPixel =
				(shaderSdf && UsesMtsdfDistanceField())
					|| aggressiveComposite ? 4u : 1u;
			const size_t streamingReserve = SaturatingMultiply(
				2048u * 2048u, streamingBytesPerPixel);
			const size_t usableHeadroom = currentHeadroom > streamingReserve
				? currentHeadroom - streamingReserve : currentHeadroom / 4u;
			// Keep one third of the post-streaming headroom unused. This raises the
			// normal cold-build ceiling without consuming the final safety margin;
			// allocation failures still reduce both batch size and worker count.
			const size_t headroomTarget = std::max(
				kMinimumPrewarmBatchBytes,
				usableHeadroom - usableHeadroom / 3u);
			const size_t targetBytes = std::min(
				configuredTarget, headroomTarget);
			PrewarmBatchPolicy policy;
			policy.maximumWorkers = ResolveMemoryBoundedWorkerLimit(
				targetBytes, workItemsPerGlyph, expensiveWork,
				worstRetainedBytes, worstTransientBytes);
			policy.maximumGlyphs = ResolveMemoryBoundedGlyphLimit(
				targetBytes, workItemsPerGlyph, expensiveWork,
				worstRetainedBytes, worstTransientBytes,
				policy.maximumWorkers);
			policy.parallelFloor = std::min(
				policy.maximumGlyphs, kParallelGlyphBatchFloor);
			policy.initialGlyphs = std::min(
				policy.maximumGlyphs,
				std::max(policy.parallelFloor,
					kInitialIncrementalGlyphsPerBatch));
			policy.estimatedRetainedBytesPerGlyph =
				worstRetainedBytes;
			policy.estimatedTransientBytesPerWorker =
				worstTransientBytes;
			policy.estimatedPeakBytes = EstimatePrewarmBatchBytes(
				policy.maximumGlyphs, workItemsPerGlyph,
				expensiveWork, worstRetainedBytes,
				worstTransientBytes, policy.maximumWorkers);
			policy.targetBytes = targetBytes;
			return policy;
		}

		UInt32 ResolveNextPrewarmBatchLimit(UInt32 current, UInt32 maximum,
			UInt32 parallelFloor, UInt32 completedGlyphs, ULONGLONG elapsedMs)
		{
			if (!current || !maximum)
				return 1;
			const UInt32 lower = std::min(maximum,
				std::max(parallelFloor, std::max<UInt32>(1, current / 2u)));
			const UInt32 upper = std::min(maximum,
				current > maximum / 2u ? maximum : current * 2u);
			UInt32 desired = upper;
			if (elapsedMs && completedGlyphs)
			{
				const double scaled =
					static_cast<double>(completedGlyphs)
					* static_cast<double>(kTargetPrewarmBatchMs)
					/ static_cast<double>(elapsedMs);
				desired = static_cast<UInt32>(std::clamp<double>(
					std::round(scaled), 1.0,
					static_cast<double>(maximum)));
			}
			return std::clamp(desired, lower, std::max(lower, upper));
		}

		float GetPrewarmJobProgress(const PrewarmJob& job)
		{
			const size_t completed = job.encodedUnitIndex >= job.encodedUnitStart
				? job.encodedUnitIndex - job.encodedUnitStart : 0;
			return job.targetUnitCount
				? std::min(1.0f, static_cast<float>(completed) / job.targetUnitCount)
				: 0.0f;
		}

		void LatchRebuildProgress(const char* reason)
		{
			if (PrewarmRuntime().rebuildProgressTracked)
				return;
			PrewarmRuntime().rebuildProgressTracked = true;
			PrewarmRuntime().rebuildProgressReportingStarted = false;
			PrewarmRuntime().rebuildProgressPresentationSuspended = false;
			PrewarmRuntime().rebuildProgressNeedsBaselineReset = false;
			PrewarmRuntime().rebuildProgressPresentationClosed = false;
			PrewarmRuntime().rebuildProgressBaselineFinishedFonts = 0;
			PrewarmRuntime().rebuildProgressGenerationFontCount = 0;
			PrewarmRuntime().rebuildProgress = 0.0f;
			PrewarmRuntime().session.lastProgressUpdate = 0;
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm progress tracking latched reason=%s policy=cache-write-transaction presentation=graphical-only generation-only=1",
				reason ? reason : "unknown");
		}

		void StartRebuildProgressReporting()
		{
			PrewarmRuntimeState& state = PrewarmRuntime();
			if (!state.rebuildProgressTracked
				|| state.rebuildProgressPresentationClosed
				|| !state.presentationRunToken)
			{
				return;
			}

			if (state.rebuildProgressReportingStarted)
			{
				if (!state.rebuildProgressPresentationSuspended)
					return;
				if (state.rebuildProgressNeedsBaselineReset)
				{
					state.rebuildProgressBaselineFinishedFonts =
						state.session.finishedFonts;
					state.rebuildProgressGenerationFontCount = std::max<UInt32>(
						1, state.session.queuedFonts - std::min(
							state.session.queuedFonts, state.session.finishedFonts));
				}
				state.rebuildProgressPresentationSuspended = false;
				state.rebuildProgressNeedsBaselineReset = false;
				state.session.lastProgressUpdate = 0;
				PublishNativePrewarmOverlayProgress(
					state.presentationRunToken, state.rebuildProgress);
				return;
			}

			state.rebuildProgressReportingStarted = true;
			state.rebuildProgressPresentationSuspended = false;
			state.rebuildProgressNeedsBaselineReset = false;
			state.rebuildProgressBaselineFinishedFonts =
				state.session.finishedFonts;
			state.rebuildProgressGenerationFontCount = std::max<UInt32>(
				1, state.session.queuedFonts - std::min(
					state.session.queuedFonts, state.session.finishedFonts));
			state.rebuildProgress = 0.0f;
			state.session.lastProgressUpdate = 0;
			PublishNativePrewarmOverlayProgress(
				state.presentationRunToken, 0.0f);
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm progress reporting started run=%llu presentation=graphical-only scope=generation-only delivery=latest-state-mailbox producerThread=%u generationFonts=%u",
				static_cast<unsigned long long>(state.presentationRunToken),
				GetCurrentThreadId(),
				state.rebuildProgressGenerationFontCount);
		}

		void SuspendRebuildProgressReporting(bool resetGenerationBaseline)
		{
			PrewarmRuntimeState& state = PrewarmRuntime();
			if (!state.rebuildProgressReportingStarted
				|| state.rebuildProgressPresentationClosed
				|| state.rebuildProgressPresentationSuspended
				|| !state.presentationRunToken)
			{
				return;
			}
			state.rebuildProgressPresentationSuspended = true;
			state.rebuildProgressNeedsBaselineReset =
				state.rebuildProgressNeedsBaselineReset || resetGenerationBaseline;
			SuspendNativePrewarmOverlay(state.presentationRunToken);
		}

		void CloseRebuildProgressReporting(
			PrewarmOverlayCloseReason reason)
		{
			PrewarmRuntimeState& state = PrewarmRuntime();
			if (!state.rebuildProgressReportingStarted
				|| state.rebuildProgressPresentationClosed
				|| !state.presentationRunToken)
			{
				return;
			}
			if (reason == PrewarmOverlayCloseReason::Completed)
			{
				state.rebuildProgressPresentationSuspended = false;
				ReportPrewarmTransactionProgress(1.0f, true);
			}
			CloseNativePrewarmOverlay(state.presentationRunToken, reason);
			state.rebuildProgressPresentationClosed = true;
			state.rebuildProgressPresentationSuspended = false;
			state.rebuildProgressNeedsBaselineReset = false;
		}

		void ReportPrewarmTransactionProgress(
			float progress, bool force = false)
		{
			ServiceFontPrewarmHostMessages();
			PrewarmRuntimeState& state = PrewarmRuntime();
			if (!state.rebuildProgressTracked
				|| !state.rebuildProgressReportingStarted
				|| state.rebuildProgressPresentationClosed)
			{
				return;
			}
			progress = std::max(state.rebuildProgress,
				std::clamp(progress, 0.0f, 1.0f));
			state.rebuildProgress = progress;
			const ULONGLONG now = GetTickCount64();
			if (!force && state.session.lastProgressUpdate
				&& now - state.session.lastProgressUpdate
					< kMinimumProgressUpdateIntervalMs)
			{
				return;
			}
			state.session.lastProgressUpdate = now;
			if (!state.rebuildProgressPresentationSuspended)
			{
				PublishNativePrewarmOverlayProgress(
					state.presentationRunToken, progress);
			}
			if (force)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm generation progress percent=%u presentation=graphical-only delivery=latest-state-mailbox",
					static_cast<UInt32>(std::lround(progress * 100.0f)));
			}
		}

		void ReportPrewarmProgress(const PrewarmJob& job, UInt32,
			UInt32 finishedFonts,
			float minimumJobProgress = 0.0f, bool force = false)
		{
			const PrewarmRuntimeState& state = PrewarmRuntime();
			const UInt32 baseline = state.rebuildProgressBaselineFinishedFonts;
			const UInt32 completed = finishedFonts >= baseline
				? finishedFonts - baseline : 0;
			const float jobProgress = std::max(
				minimumJobProgress, GetPrewarmJobProgress(job));
			const float overall = state.rebuildProgressGenerationFontCount
				? (static_cast<float>(completed)
					+ std::clamp(jobProgress, 0.0f, 1.0f))
					/ state.rebuildProgressGenerationFontCount
				: 1.0f;
			ReportPrewarmTransactionProgress(overall, force);
		}

		void ReportAtlasPrewarmProgress(FontAtlasPrewarmProgressStage stage,
			UInt32 item, UInt32 total, void*)
		{
			const char* stageName = nullptr;
			switch (stage)
			{
			case FontAtlasPrewarmProgressStage::PublishPhysicalGroup:
				stageName = "publish-physical-group";
				break;
			case FontAtlasPrewarmProgressStage::RestorePhysicalGroup:
				stageName = "restore-physical-group";
				break;
			case FontAtlasPrewarmProgressStage::PlanPhysicalPools:
				stageName = "plan-physical-pools";
				break;
			case FontAtlasPrewarmProgressStage::PublishPhysicalPool:
				stageName = "publish-physical-pool";
				break;
			default:
				return;
			}
			gLog.FormattedMessage(
				"tnvse_freetype_font: atlas prewarm stage=%s item=%u total=%u presentation=none generation-window-closed=%u",
				stageName, item, total,
				PrewarmRuntime().rebuildProgressPresentationClosed ? 1u : 0u);
		}
		void FinishJob(const PrewarmJob& job, const char* status)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm font=%u prewarmEncoding=%s scale=%.3f glyphs=%u doubleByte=%u knownEmpty=%u renderMode=%s distanceFieldGlyphs=%u status=%s",
				job.fontId,
				GetFontPrewarmRangeName(job.prewarmRange, job.codePage),
				job.rasterScaleMilli ? job.rasterScaleMilli / 1000.0f : 0.0f,
				job.rasterizedGlyphCount, job.validDoubleByteCount,
				job.knownEmptyGlyphCount,
				GetConfiguredFontRenderModeName(),
				job.sdfGlyphCount, status);
		}

		bool PublishSealedProfileAliases(UInt32 ownerFontId,
			float rasterScale)
		{
			const FontConfig* owner = FindConfig(ownerFontId);
			if (!owner)
				return false;
			const FontAtlasRoute route = GetPersistentFontCacheRoute();
			const UInt64 profileKey = BuildProfileKey(*owner, route);
			bool complete = true;
			for (const auto& entry : g_configs)
			{
				if (entry.first == ownerFontId
					|| BuildProfileKey(entry.second, route) != profileKey)
				{
					continue;
				}
				RuntimeFont* runtime = FindRuntimeFont(entry.first);
				if (!runtime
					|| !BuildDirectGlyphAtlasTables(
						*runtime, rasterScale))
				{
					complete = false;
					gLog.FormattedMessage(
						"tnvse_freetype_font: sealed direct alias publication failed font=%u owner=%u",
						entry.first, ownerFontId);
				}
			}
			return complete;
		}
	}
}

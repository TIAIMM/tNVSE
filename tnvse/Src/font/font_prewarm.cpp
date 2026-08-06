#include "font_vector_internal.h"
#include "font_render_route.h"
#include "font_atlas_stream.h"

#include "encoding.h"
#include "load_config.h"
#include "native_tile_overlay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace fonthook::vectorfont
{
	namespace implementation::font_prewarm {}
	using namespace implementation::font_prewarm;

	namespace implementation::font_prewarm
	{
		constexpr UInt32 kMaximumCandidatesPerBatch = 32768;
		constexpr UInt32 kMaximumGlyphsPerBatch = 4096;
		constexpr UInt32 kMaximumIncrementalGlyphsPerBatch = 1024;
		constexpr UInt32 kInitialIncrementalGlyphsPerBatch = 128;
		constexpr UInt32 kParallelGlyphBatchFloor =
			kFillPrewarmParallelThreshold;
		constexpr ULONGLONG kTargetPrewarmBatchMs = 250;
		constexpr ULONGLONG kMinimumProgressUpdateIntervalMs = 100;
		constexpr size_t kMinimumPrewarmBatchBytes = 1u * 1024u * 1024u;
		constexpr size_t kMaximumPrewarmBatchBytes = 24u * 1024u * 1024u;
		constexpr size_t kPrewarmPerGlyphMetadataBytes = 512u;
		constexpr size_t kPrewarmPerWorkerFixedBytes = 256u * 1024u;
		constexpr size_t kMaximumPrewarmStreamingWorkingBytes =
			16u * 1024u * 1024u;
		// Worst-case 8192x8192 four-byte page. This is a recovery estimate;
		// CreateDynamicAtlasTexture admits the selected page using its exact bytes.
		constexpr size_t kMaximumPrewarmPhysicalAllocationBytes =
			256u * 1024u * 1024u;
		void UpdatePrewarmProgress(const std::wstring& detail,
			const std::wstring& stage, float progress)
		{
			UpdateNativePrewarmOverlay(detail, stage, progress);
		}

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

		std::deque<PrewarmJob> s_jobs;
		std::unordered_set<UInt64> s_scheduledProfiles;
		bool s_configuredFontsQueued = false;
		bool s_configuredFontsPrewarmed = false;
		bool s_atlasOnlyPrewarmPending = false;

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
			UInt32 metricsOnlyBatchGlyphLimit = kMaximumGlyphsPerBatch;
			size_t estimatedRetainedBytesPerGlyph = 1;
			size_t estimatedTransientBytesPerWorker = 1;
			size_t estimatedPeakBatchBytes = 1;
			size_t targetBatchBytes = kMinimumPrewarmBatchBytes;
			UInt32 allocationRetries = 0;
			bool exhausted = false;
			bool failed = false;
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
			bool success = false;
		};

		PrewarmSession s_session;
		UInt32 s_transactionRestartCount = 0;
		UInt32 s_totalMemoryRetryCount = 0;
		bool s_transactionRestartPending = false;

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
			return config
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

		struct PrewarmBatchPolicy
		{
			UInt32 initialGlyphs = 1;
			UInt32 maximumGlyphs = 1;
			UInt32 parallelFloor = 1;
			size_t estimatedRetainedBytesPerGlyph = 1;
			size_t estimatedTransientBytesPerWorker = 1;
			size_t estimatedPeakBytes = 1;
			size_t targetBytes = kMinimumPrewarmBatchBytes;
		};

		size_t EstimatePrewarmBatchBytes(UInt32 glyphs,
			UInt32 workItemsPerGlyph, bool expensiveWork,
			size_t retainedBytesPerGlyph,
			size_t transientBytesPerWorker)
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
					usefulWorkers, kMaximumPrewarmRasterWorkers);
			return SaturatingAdd(
				SaturatingMultiply(glyphs, retainedBytesPerGlyph),
				SaturatingMultiply(workers,
					transientBytesPerWorker));
		}

		UInt32 ResolveMemoryBoundedGlyphLimit(size_t targetBytes,
			UInt32 workItemsPerGlyph, bool expensiveWork,
			size_t retainedBytesPerGlyph,
			size_t transientBytesPerWorker)
		{
			UInt32 resolved = 1;
			for (UInt32 glyphs = 1;
				glyphs <= kMaximumIncrementalGlyphsPerBatch; ++glyphs)
			{
				if (EstimatePrewarmBatchBytes(
						glyphs, workItemsPerGlyph, expensiveWork,
						retainedBytesPerGlyph,
						transientBytesPerWorker) > targetBytes)
				{
					break;
				}
				resolved = glyphs;
			}
			return resolved;
		}

		PrewarmBatchPolicy ResolvePrewarmBatchPolicy(
			const FontConfig& config, float rasterScale, bool shaderSdf,
			bool aggressiveComposite, UInt32 sdfSpread)
		{
			size_t worstRetainedBytes = 1;
			size_t worstTransientBytes = 1;
			UInt32 workItemsPerGlyph = 1;
			bool expensiveWork = shaderSdf || aggressiveComposite;
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
						GetConfiguredDistanceFieldMethod());
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
					kPrewarmPerWorkerFixedBytes);
				worstRetainedBytes = std::max(
					worstRetainedBytes, retainedBytes);
				worstTransientBytes = std::max(
					worstTransientBytes, transientBytes);
			}
			const size_t configuredBudget = GetCpuMemoryBudget();
			const size_t configuredTarget = std::max(
				kMinimumPrewarmBatchBytes,
				std::min(kMaximumPrewarmBatchBytes, configuredBudget / 8u));
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
			const size_t headroomTarget = std::max(
				kMinimumPrewarmBatchBytes, usableHeadroom / 2u);
			const size_t targetBytes = std::min(
				configuredTarget, headroomTarget);
			PrewarmBatchPolicy policy;
			policy.maximumGlyphs = ResolveMemoryBoundedGlyphLimit(
				targetBytes, workItemsPerGlyph, expensiveWork,
				worstRetainedBytes, worstTransientBytes);
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
				worstTransientBytes);
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

		void ReportPrewarmProgress(const PrewarmJob& job, UInt32 fontOrdinal,
			UInt32 fontCount, UInt32 finishedFonts, const wchar_t* stage,
			float minimumJobProgress = 0.0f, bool force = false)
		{
			wchar_t detail[160] = {};
			const wchar_t* renderMode =
				job.route == FontAtlasRoute::ShaderDistanceField
					? (UsesMtsdfDistanceField() ? L"MTSDF" : L"true SDF")
					: job.route == FontAtlasRoute::ShaderA8Coverage
						? L"BGRA composite" : L"ARGB fallback";
			_snwprintf_s(detail, _countof(detail), _TRUNCATE,
				L"Font %u of %u  |  ID %u  |  %ls",
				fontOrdinal, fontCount, job.fontId, renderMode);
			const float jobProgress = std::max(minimumJobProgress,
				GetPrewarmJobProgress(job));
			const float overall = fontCount
				? (static_cast<float>(finishedFonts) + jobProgress) / fontCount
				: 1.0f;
			const ULONGLONG now = GetTickCount64();
			if (!force && s_session.lastProgressUpdate
				&& now - s_session.lastProgressUpdate
					< kMinimumProgressUpdateIntervalMs)
			{
				return;
			}
			s_session.lastProgressUpdate = now;
			UpdatePrewarmProgress(detail, stage ? stage : L"Preparing glyphs...", overall);
		}

		void FinishJob(const PrewarmJob& job, const char* status)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm font=%u prewarmEncoding=%s scale=%.3f glyphs=%u doubleByte=%u renderMode=%s distanceFieldGlyphs=%u status=%s",
				job.fontId,
				GetFontPrewarmRangeName(job.prewarmRange, job.codePage),
				job.rasterScaleMilli ? job.rasterScaleMilli / 1000.0f : 0.0f,
				job.rasterizedGlyphCount, job.validDoubleByteCount,
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

	void QueueFontPrewarm(UInt32 fontId)
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
		if (!s_scheduledProfiles.insert(key).second)
		{
			const auto shared = std::find_if(s_jobs.begin(), s_jobs.end(),
				[config](const PrewarmJob& job)
				{
					return MatchesPrewarmProfile(job, *config);
				});
			if (shared != s_jobs.end())
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
		s_jobs.push_back(std::move(job));
		SetBitmapCacheReducedAfterPrewarm(false);
		gLog.FormattedMessage(
			"tnvse_freetype_font: queued prewarm font=%u coverage=direct-first codePage=%u prewarmEncoding=%s",
			fontId, codePage,
			GetFontPrewarmRangeName(prewarmRange, codePage));
	}

	void QueueConfiguredFontPrewarms()
	{
		if (s_configuredFontsQueued)
			return;
		s_configuredFontsQueued = true;
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
			QueueFontPrewarm(fontId);
	}

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
			case PrewarmPhase::CleanupMasks: return "cleanup-masks";
			case PrewarmPhase::CleanupBudget: return "cleanup-budget";
			case PrewarmPhase::CleanupFiles: return "cleanup-files";
			case PrewarmPhase::Complete: return "complete";
			default: return "unknown";
			}
		}

		void TransitionPrewarmPhase(PrewarmPhase phase)
		{
			if (s_session.phase == phase)
				return;
			gLog.FormattedMessage(
				"tnvse_freetype_font: incremental prewarm phase %s -> %s",
				PrewarmPhaseName(s_session.phase), PrewarmPhaseName(phase));
			s_session.phase = phase;
		}

		void EndAtlasOnlyPrewarmPolicy()
		{
			if (!s_atlasOnlyPrewarmPending)
				return;
			EndCompleteCodePageAtlasOnlyPrewarm();
			s_atlasOnlyPrewarmPending = false;
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
			s_session.bitmapResults.clear();
			s_session.bitmapRequests.clear();
			s_session.requestedGlyphs.clear();
			EnforceCpuMemoryBudget(reason);
		}

		void IncrementSaturating(UInt32& value)
		{
			if (value != std::numeric_limits<UInt32>::max())
				++value;
		}

		void RecordPrewarmMemoryPressure(PrewarmJob& job)
		{
			IncrementSaturating(job.memoryRetries);
			IncrementSaturating(s_session.memoryRetries);
			IncrementSaturating(s_totalMemoryRetryCount);
		}

		void PreparePrewarmMemoryRetry(const char* stage, UInt32 fontId,
			UInt32 retry, size_t pendingBytes)
		{
			const bool releasedEmergency =
				ReleaseFontPrewarmEmergencyAddressSpace();
			ReleasePrewarmBatchReferences("prewarm-memory-retry");
			SetBitmapCacheReducedAfterPrewarm(true);
			const UInt64 releasedMappings =
				ReleaseGlyphBitmapDiskCacheMappings();
			PruneRetiredAtlasGenerations();
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

		void ResetPrewarmTransactionForRetry(const char* reason)
		{
			if (s_session.activeFont)
			{
				if (RuntimeFont* runtime =
					FindRuntimeFont(s_session.activeFont->job.fontId))
				{
					CancelStreamingPrewarmAtlas(*runtime);
				}
			}
			ReleasePrewarmBatchReferences("prewarm-transaction-retry");
			EndAtlasOnlyPrewarmPolicy();
			ReleaseFontPrewarmEmergencyAddressSpace();
			ResetAtlasAllocationMemoryPressure();
			HideNativePrewarmOverlay();
			s_session = {};
			s_jobs.clear();
			s_scheduledProfiles.clear();
			s_configuredFontsQueued = false;
			s_configuredFontsPrewarmed = false;
			s_transactionRestartPending = true;
			IncrementSaturating(s_transactionRestartCount);
			SetBitmapCacheReducedAfterPrewarm(false);
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm transaction retry reason=%s restart=%u policy=same-loading-barrier no-runtime-demand-fallback=1",
				reason ? reason : "unknown", s_transactionRestartCount);
			if (s_transactionRestartCount >= 4)
			{
				const DWORD delayMs = std::min<DWORD>(250,
					static_cast<DWORD>(1u << std::min<UInt32>(
						s_transactionRestartCount - 4, 7)));
				Sleep(delayMs);
			}
		}

		void ForcePrewarmTransactionRetryState() noexcept
		{
			try
			{
				if (s_session.activeFont)
				{
					if (RuntimeFont* runtime =
						FindRuntimeFont(s_session.activeFont->job.fontId))
					{
						CancelStreamingPrewarmAtlas(*runtime);
					}
				}
			}
			catch (...) {}
			try { EndAtlasOnlyPrewarmPolicy(); }
			catch (...) {}
			try { HideNativePrewarmOverlay(); }
			catch (...) {}
			s_session = {};
			s_jobs.clear();
			s_scheduledProfiles.clear();
			s_configuredFontsQueued = false;
			s_configuredFontsPrewarmed = false;
			if (!s_transactionRestartPending)
				IncrementSaturating(s_transactionRestartCount);
			s_transactionRestartPending = true;
			ReleaseFontPrewarmEmergencyAddressSpace();
			ResetAtlasAllocationMemoryPressure();
		}

		void RetryPrewarmAfterPumpException(const char* reason) noexcept
		{
			try
			{
				ResetPrewarmTransactionForRetry(reason);
			}
			catch (...)
			{
				ForcePrewarmTransactionRetryState();
			}
		}

		void RecordPrewarmStep(ULONGLONG started)
		{
			const ULONGLONG elapsed = GetTickCount64() - started;
			s_session.maximumStepMs =
				std::max(s_session.maximumStepMs, elapsed);
		}

		void PrepareIncrementalSession()
		{
			s_transactionRestartPending = false;
			s_session.transactionRestarts = s_transactionRestartCount;
			s_session.memoryRetries = s_totalMemoryRetryCount;
			const bool emergencyReserved =
				ReserveFontPrewarmEmergencyAddressSpace();
			ProcessVirtualMemoryHeadroom initialHeadroom;
			QueryProcessVirtualMemoryHeadroom(initialHeadroom);
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm address-space guard reserveMiB=%.2f reserved=%u availableVirtualMiB=%.2f largestFreeMiB=%.2f",
				kFontPrewarmEmergencyAddressSpaceBytes / (1024.0 * 1024.0),
				emergencyReserved ? 1u : 0u,
				initialHeadroom.availableBytes / (1024.0 * 1024.0),
				initialHeadroom.largestFreeRegionBytes / (1024.0 * 1024.0));
			const FontAtlasRoute finalRoute = GetPersistentFontCacheRoute();
			std::deque<PrewarmJob> reboundJobs;
			while (!s_jobs.empty())
			{
				PrewarmJob job = std::move(s_jobs.front());
				s_jobs.pop_front();
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
				s_session.restoreJobs.push_back(std::move(job));
			}
			SortPrewarmJobsByDependencies(s_session.restoreJobs);
			s_scheduledProfiles = reboundProfiles;
			if (s_session.restoreJobs.empty())
			{
				if (g_configs.empty())
				{
					EndAtlasOnlyPrewarmPolicy();
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

			s_session.rasterScale = GetCanonicalFreeTypeRasterScale();
			s_session.rasterScaleMilli = static_cast<UInt32>(std::lround(
				s_session.rasterScale * 1000.0f));
			s_session.queuedFonts =
				static_cast<UInt32>(s_session.restoreJobs.size());
			s_session.verifiedCodePageFonts.reserve(s_session.queuedFonts);
			s_session.started = GetTickCount64();
			s_session.maximumStepMs = 0;

			gLog.FormattedMessage(
				"tnvse_freetype_font: incremental streamed prewarm begin fonts=%u scale=%.3f maximumBatchMiB=%.2f targetBatchMs=%llu strategy=bounded-throughput",
				s_session.queuedFonts, s_session.rasterScale,
				kMaximumPrewarmBatchBytes / (1024.0 * 1024.0),
				static_cast<unsigned long long>(kTargetPrewarmBatchMs));
			TransitionPrewarmPhase(PrewarmPhase::RestoreSnapshots);
		}

		void RestoreOnePrewarmSnapshot()
		{
			if (s_session.restoreJobs.empty())
			{
				SortPrewarmJobsByDependencies(
					s_session.generationJobs);
				TransitionPrewarmPhase(s_session.generationJobs.empty()
					? PrewarmPhase::CleanupFlush
					: PrewarmPhase::BeginFont);
				return;
			}

			const ULONGLONG stepStarted = GetTickCount64();
			PrewarmJob job = std::move(s_session.restoreJobs.front());
			s_session.restoreJobs.pop_front();
			if (job.rasterScaleMilli != s_session.rasterScaleMilli)
				ResetPrewarmScan(job, s_session.rasterScaleMilli);

			const FontConfig* config = nullptr;
			RuntimeFont* runtime = nullptr;
			if (!ResolveValidPrewarmJob(job, config, runtime))
			{
				if (runtime)
					CancelStreamingPrewarmAtlas(*runtime);
				FinishJob(job, "cancelled");
				++s_session.cancelledFonts;
				++s_session.finishedFonts;
				RecordPrewarmStep(stepStarted);
				return;
			}

			bool snapshotReady = false;
			bool snapshotMemoryPressure = false;
			ResetAtlasAllocationMemoryPressure();
			try
			{
				snapshotReady =
					TryLoadGloballyRepackedGlyphAtlasSnapshot(
						*runtime, s_session.rasterScale);
			}
			catch (const std::bad_alloc&)
			{
				snapshotMemoryPressure = true;
			}
			catch (...)
			{
				ResetAtlasAllocationMemoryPressure();
				throw;
			}
			snapshotMemoryPressure =
				ConsumeAtlasAllocationMemoryPressure()
				|| snapshotMemoryPressure;
			if (snapshotMemoryPressure)
			{
				RecordPrewarmMemoryPressure(job);
				PreparePrewarmMemoryRetry("snapshot-restore", job.fontId,
					job.memoryRetries,
					kMaximumPrewarmPhysicalAllocationBytes);
				gLog.FormattedMessage(
					"tnvse_freetype_font: snapshot restore memory pressure font=%u scale=%.3f snapshotPreserved=1 retryInSameBarrier=1",
					job.fontId, s_session.rasterScale);
				s_session.restoreJobs.push_front(std::move(job));
				RecordPrewarmStep(stepStarted);
				return;
			}
			if (snapshotReady
				&& job.route == FontAtlasRoute::ArgbFallback
				&& !MarkCurrentFallbackBitmapProfilesUsed(
					*runtime, s_session.rasterScale))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: fallback snapshot rejected font=%u reason=incomplete-persistent-mask-set",
					job.fontId);
				snapshotReady = false;
			}
			if (snapshotReady)
			{
				const bool directReady = BuildDirectGlyphAtlasTables(
						*runtime, s_session.rasterScale)
					&& PublishSealedProfileAliases(
						job.fontId, s_session.rasterScale);
				if (directReady)
				{
					FinishJob(job, "snapshot");
					s_session.verifiedCodePageFonts.push_back(job.fontId);
					++s_session.completedFonts;
					++s_session.finishedFonts;
				}
				else
				{
					CancelStreamingPrewarmAtlas(*runtime);
					const bool discarded = DiscardGlyphAtlasSnapshot(
						*runtime, s_session.rasterScale);
					PreparePrewarmScanForGeneration(
						job, *config, s_session.rasterScaleMilli);
					gLog.FormattedMessage(
						"tnvse_freetype_font: snapshot direct-table publication failed font=%u atlas=%s; rebuilding within the same prewarm barrier",
						job.fontId,
						discarded ? "discarded" : "delete-failed");
					s_session.generationJobs.push_back(std::move(job));
				}
			}
			else
			{
				CancelStreamingPrewarmAtlas(*runtime);
				const bool discarded = DiscardGlyphAtlasSnapshot(
					*runtime, s_session.rasterScale);
				PreparePrewarmScanForGeneration(
					job, *config, s_session.rasterScaleMilli);
				gLog.FormattedMessage(
					"tnvse_freetype_font: cache miss rebuilding atlas font=%u atlas=%s persistent=preserved",
					job.fontId, discarded ? "discarded" : "delete-failed");
				s_session.generationJobs.push_back(std::move(job));
			}
			// The progress component itself uses font slot 1 and may already
			// hold a shape from the generation just replaced above. Force all
			// prewarm text nodes to rebuild before the next rendered frame.
			RefreshNativePrewarmOverlayTextGeometry();
			RecordPrewarmStep(stepStarted);
		}

		void BeginNextPrewarmFont()
		{
			if (s_session.generationJobs.empty())
			{
				HideNativePrewarmOverlay();
				TransitionPrewarmPhase(PrewarmPhase::CleanupFlush);
				return;
			}

			ActivePrewarmFont active;
			active.job = std::move(s_session.generationJobs.front());
			s_session.generationJobs.pop_front();
			const FontConfig* config = nullptr;
			RuntimeFont* runtime = nullptr;
			if (!ResolveValidPrewarmJob(active.job, config, runtime))
			{
				if (runtime)
					CancelStreamingPrewarmAtlas(*runtime);
				FinishJob(active.job, "cancelled");
				++s_session.cancelledFonts;
				++s_session.finishedFonts;
				return;
			}

			EffectQuality resolvedQuality = config->effectQuality;
			active.shaderSdf =
				active.job.route == FontAtlasRoute::ShaderDistanceField
				&& ResolveA8EffectQuality(
					config->effectQuality, resolvedQuality);
			active.aggressiveComposite =
				active.job.route == FontAtlasRoute::ShaderA8Coverage;
			if (active.shaderSdf
				&& !ResolveSdfSpread(
					*config, s_session.rasterScale, active.sdfSpread))
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
					std::min(s_session.queuedFonts,
						s_session.finishedFonts + 1),
					s_session.queuedFonts,
					s_session.finishedFonts,
					L"Reusing shared MTSDF double-byte atlas...",
					0.0f, true);
			}
			bool sharedRoleReady = true;
			bool sharedRoleMemoryPressure = false;
			if (active.sharedDoubleAlias)
			{
				ResetAtlasAllocationMemoryPressure();
				try
				{
					sharedRoleReady =
						TryLoadGloballyRepackedGlyphAtlasSnapshotRole(
							*runtime, VectorFontByteClass::DoubleByte,
							s_session.rasterScale);
				}
				catch (const std::bad_alloc&)
				{
					sharedRoleMemoryPressure = true;
				}
				catch (...)
				{
					ResetAtlasAllocationMemoryPressure();
					throw;
				}
				sharedRoleMemoryPressure =
					ConsumeAtlasAllocationMemoryPressure()
					|| sharedRoleMemoryPressure;
			}
			if (sharedRoleMemoryPressure)
			{
				RecordPrewarmMemoryPressure(active.job);
				PreparePrewarmMemoryRetry("shared-role-restore",
					active.job.fontId, active.job.memoryRetries,
					kMaximumPrewarmPhysicalAllocationBytes);
				gLog.FormattedMessage(
					"tnvse_freetype_font: shared double-byte role restore memory pressure font=%u owner=%u snapshotPreserved=1 retryInSameBarrier=1",
					active.job.fontId,
					config->mtsdfDoubleByteOwnerFontId);
				s_session.generationJobs.push_front(
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
						s_session.generationJobs.begin(),
						s_session.generationJobs.end(),
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
					s_session.generationJobs.push_back(
						std::move(active.job));
					return;
				}
				gLog.FormattedMessage(
					"tnvse_freetype_font: shared distance-field double-byte role unavailable font=%u owner=%u; owner is not pending",
					active.job.fontId,
					config->mtsdfDoubleByteOwnerFontId);
				FinishJob(active.job, "shared-role-unavailable");
				++s_session.streamFailedFonts;
				++s_session.finishedFonts;
				return;
			}

			EnforceCpuMemoryBudget("prewarm-font-begin");
			const PrewarmBatchPolicy batchPolicy =
				ResolvePrewarmBatchPolicy(
					*config, s_session.rasterScale,
					active.shaderSdf, active.aggressiveComposite,
					active.sdfSpread);
			active.maximumBatchGlyphLimit =
				batchPolicy.maximumGlyphs;
			active.batchGlyphLimit = batchPolicy.initialGlyphs;
			active.parallelBatchFloor =
				batchPolicy.parallelFloor;
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
				active.metricsOnlyBatchGlyphLimit = 1;
				active.constrainedMemory = true;
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm font entered constrained same-session mode font=%u batchGlyphs=1 availableVirtualMiB=%.2f largestFreeMiB=%.2f runtimeFallback=0",
					active.job.fontId,
					headroom.availableBytes / (1024.0 * 1024.0),
					headroom.largestFreeRegionBytes / (1024.0 * 1024.0));
			}
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm batch policy font=%u initial=%u parallelFloor=%u maximum=%u metricsOnlyMaximum=%u retainedBytesPerGlyph=%llu transientBytesPerWorker=%llu estimatedPeakMiB=%.2f targetMiB=%.2f memoryMiB=%.2f/%.2f",
				active.job.fontId,
				active.batchGlyphLimit,
				active.parallelBatchFloor,
				active.maximumBatchGlyphLimit,
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
				&& !s_atlasOnlyPrewarmPending)
			{
				BeginCompleteCodePageAtlasOnlyPrewarm();
				s_atlasOnlyPrewarmPending = true;
				s_session.atlasOnlyTransactionStarted = true;
				gLog.FormattedMessage(
					"tnvse_freetype_font: incremental prewarm atlas-only transaction begin font=%u",
					active.job.fontId);
			}
			s_session.activeFont = std::move(active);
			// The progress component is generation-only. A complete cache-hit
			// startup performs snapshot validation and restoration without ever
			// loading or displaying the Tile tree.
			ReportPrewarmProgress(
				s_session.activeFont->job,
				std::min(s_session.queuedFonts,
					s_session.finishedFonts + 1),
				s_session.queuedFonts,
				s_session.finishedFonts,
				L"Preparing streamed glyph batches...",
				0.0f, true);
			TransitionPrewarmPhase(PrewarmPhase::GenerateBatch);
		}

		void GenerateOnePrewarmBatch()
		{
			if (!s_session.activeFont)
			{
				TransitionPrewarmPhase(PrewarmPhase::BeginFont);
				return;
			}
			ActivePrewarmFont& active = *s_session.activeFont;
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
				return selectedBatchLimit < previous;
			};

			s_session.requestedGlyphs.clear();
			s_session.bitmapRequests.clear();
			s_session.bitmapResults.clear();
			try
			{
				s_session.requestedGlyphs.reserve(
					requestedBatchLimit);
				if (!metricsOnlyDoubleByte)
				{
					s_session.bitmapRequests.reserve(
						static_cast<size_t>(
							requestedBatchLimit) * 4u);
				}
			}
			catch (const std::bad_alloc&)
			{
				const UInt32 previous = selectedBatchLimit;
				retryBatchAfterMemoryPressure(
					"request-buffer");
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm request-buffer retry font=%u limit=%u->%u metricsOnly=%u retry=%u sameBarrier=1",
					active.job.fontId, previous,
					selectedBatchLimit,
					metricsOnlyDoubleByte ? 1u : 0u,
					active.allocationRetries);
				RecordPrewarmStep(stepStarted);
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
					s_session.requestedGlyphs.push_back(glyph);
					const VectorEncodedGlyph* requested =
						&s_session.requestedGlyphs.back();
					if (needsGrayFill)
					{
						s_session.bitmapRequests.push_back({
							requested, GlyphMaskType::Fill, 0
						});
					}
					if (active.aggressiveComposite)
					{
						s_session.bitmapRequests.push_back({
							requested, GlyphMaskType::Composite, 0
						});
					}
					if (active.shaderSdf
						&& (length == 1
							|| !active.sharedDoubleAlias))
					{
						s_session.bitmapRequests.push_back({
							requested, GlyphMaskType::DistanceField,
							active.sdfSpread
						});
						++active.job.sdfGlyphCount;
					}
					if (config->glow.enabled && needsGrayFill)
					{
						s_session.bitmapRequests.push_back({
							requested, GlyphMaskType::Glow, 0
						});
					}
					if (config->outline.enabled && needsGrayFill)
					{
						s_session.bitmapRequests.push_back({
							requested, GlyphMaskType::Outline, 0
						});
					}
					if (config->shadow.enabled && needsGrayFill)
					{
						s_session.bitmapRequests.push_back({
							requested, GlyphMaskType::Shadow, 0
						});
					}
					++glyphCount;
					++active.job.rasterizedGlyphCount;
				}
			}
			catch (const std::bad_alloc&)
			{
				s_session.scanMs += GetTickCount64() - scanStarted;
				const UInt32 previous = selectedBatchLimit;
				retryBatchAfterMemoryPressure("glyph-scan");
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm scan allocation retry font=%u limit=%u->%u metricsOnly=%u retry=%u sameBarrier=1",
					active.job.fontId, previous,
					selectedBatchLimit,
					metricsOnlyDoubleByte ? 1u : 0u,
					active.allocationRetries);
				RecordPrewarmStep(stepStarted);
				return;
			}
			catch (...)
			{
				s_session.scanMs += GetTickCount64() - scanStarted;
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
			s_session.scanMs += GetTickCount64() - scanStarted;
			s_session.peakBatchGlyphs = std::max(
				s_session.peakBatchGlyphs, glyphCount);

			bool retryMemoryBatch = false;
			if (!s_session.bitmapRequests.empty())
			{
				const ULONGLONG rasterStarted = GetTickCount64();
				try
				{
					GetPrewarmGlyphBitmaps(
						*runtime,
						s_session.bitmapRequests,
						s_session.rasterScale,
						s_session.bitmapResults);
				}
				catch (const std::bad_alloc&)
				{
					const UInt32 previous = selectedBatchLimit;
					retryBatchAfterMemoryPressure("glyph-raster");
					retryMemoryBatch = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: prewarm raster allocation retry font=%u scale=%.3f batchGlyphs=%u limit=%u->%u retry=%u sameBarrier=1",
						active.job.fontId,
						s_session.rasterScale,
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
				s_session.rasterMs +=
					GetTickCount64() - rasterStarted;

				if (!active.failed && !retryMemoryBatch
					&& s_session.bitmapResults.size()
						!= s_session.bitmapRequests.size())
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
						s_session.bitmapRequests,
						s_session.bitmapResults,
						s_session.rasterScale,
						&streamAllocationFailed);
					s_session.streamMs +=
						GetTickCount64() - streamStarted;
					if (!appended && streamAllocationFailed)
					{
						const UInt32 previous = selectedBatchLimit;
						retryBatchAfterMemoryPressure("stream-append");
						retryMemoryBatch = true;
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
			++s_session.batches;
			const ULONGLONG elapsed =
				GetTickCount64() - stepStarted;
			s_session.maximumStepMs =
				std::max(s_session.maximumStepMs, elapsed);
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
				std::min(s_session.queuedFonts,
					s_session.finishedFonts + 1),
				s_session.queuedFonts,
				s_session.finishedFonts,
				retryMemoryBatch
					? L"Retrying this batch after memory pressure..."
					: metricsOnlyDoubleByte
						? L"Indexing shared MTSDF double-byte metrics..."
						: active.shaderSdf
						? (UsesMtsdfDistanceField()
							? L"Streaming MTSDF glyphs to disk..."
							: L"Streaming true-SDF glyphs to disk...")
						: active.aggressiveComposite
							? L"Streaming aggressive BGRA composite glyphs to disk..."
							: L"Generating bounded fallback masks...",
				0.0f, retryMemoryBatch);

			if (active.failed || active.exhausted)
				TransitionPrewarmPhase(
					PrewarmPhase::FinalizeFont);
		}

		void FinalizeActivePrewarmFont()
		{
			if (!s_session.activeFont)
			{
				TransitionPrewarmPhase(PrewarmPhase::BeginFont);
				return;
			}
			const ULONGLONG stepStarted = GetTickCount64();
			ActivePrewarmFont& active = *s_session.activeFont;
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
				++s_session.cancelledFonts;
				++s_session.finishedFonts;
				RefreshNativePrewarmOverlayTextGeometry();
				s_session.activeFont.reset();
				RecordPrewarmStep(stepStarted);
				TransitionPrewarmPhase(PrewarmPhase::BeginFont);
				return;
			}

			if (active.failed)
			{
				CancelStreamingPrewarmAtlas(*runtime);
				const bool discarded = DiscardGlyphAtlasSnapshot(
					*runtime, s_session.rasterScale);
				IncrementSaturating(active.job.generationRestarts);
				PreparePrewarmScanForGeneration(active.job, *config,
					s_session.rasterScaleMilli);
				gLog.FormattedMessage(
					"tnvse_freetype_font: streamed batch failure font=%u atlas=%s generationRestart=%u retryInSameBarrier=1 runtimeFallback=0",
					active.job.fontId,
					discarded ? "discarded" : "delete-failed",
					active.job.generationRestarts);
				PrewarmJob retryJob = std::move(active.job);
				s_session.activeFont.reset();
				s_session.generationJobs.push_front(std::move(retryJob));
				RefreshNativePrewarmOverlayTextGeometry();
				RecordPrewarmStep(stepStarted);
				TransitionPrewarmPhase(PrewarmPhase::BeginFont);
				return;
			}

			ReportPrewarmProgress(
				active.job,
				std::min(s_session.queuedFonts,
					s_session.finishedFonts + 1),
				s_session.queuedFonts,
				s_session.finishedFonts,
				L"Publishing and globally repacking atlas pages...",
				0.95f, true);
			bool finalized = false;
			bool finalizationMemoryPressure = false;
			ResetAtlasAllocationMemoryPressure();
			try
			{
				finalized = FinalizeStreamingPrewarmAtlas(
					*runtime, s_session.rasterScale);
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

			if (finalized
				&& active.job.route == FontAtlasRoute::ArgbFallback
				&& !MarkCurrentFallbackBitmapProfilesUsed(
					*runtime, s_session.rasterScale))
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
							*runtime, s_session.rasterScale)
						&& PublishSealedProfileAliases(
							active.job.fontId,
							s_session.rasterScale);
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
				gLog.FormattedMessage(
					"tnvse_freetype_font: streamed finalization memory pressure font=%u scale=%.3f snapshotPreserved=1 restoreRetryInSameBarrier=1 runtimeFallback=0",
					active.job.fontId, s_session.rasterScale);
				PrewarmJob retryJob = std::move(active.job);
				s_session.activeFont.reset();
				s_session.restoreJobs.push_front(std::move(retryJob));
				RefreshNativePrewarmOverlayTextGeometry();
				HideNativePrewarmOverlay();
				RecordPrewarmStep(stepStarted);
				TransitionPrewarmPhase(PrewarmPhase::RestoreSnapshots);
				return;
			}

			if (!finalized || !directReady)
			{
				CancelStreamingPrewarmAtlas(*runtime);
				IncrementSaturating(active.job.generationRestarts);
				gLog.FormattedMessage(
					"tnvse_freetype_font: streamed finalization validation retry font=%u finalized=%u directReady=%u generationRestart=%u snapshotPreserved=1 sameBarrier=1",
					active.job.fontId, finalized ? 1u : 0u,
					directReady ? 1u : 0u,
					active.job.generationRestarts);
				PrewarmJob retryJob = std::move(active.job);
				s_session.activeFont.reset();
				s_session.restoreJobs.push_front(std::move(retryJob));
				RefreshNativePrewarmOverlayTextGeometry();
				HideNativePrewarmOverlay();
				RecordPrewarmStep(stepStarted);
				TransitionPrewarmPhase(PrewarmPhase::RestoreSnapshots);
				return;
			}

			s_session.verifiedCodePageFonts.push_back(
				active.job.fontId);
			FinishJob(active.job, "complete");
			++s_session.completedFonts;
			++s_session.finishedFonts;
			RefreshNativePrewarmOverlayTextGeometry();
			s_session.activeFont.reset();
			if (s_session.generationJobs.empty())
				HideNativePrewarmOverlay();
			RecordPrewarmStep(stepStarted);
			TransitionPrewarmPhase(PrewarmPhase::BeginFont);
		}

		void CollectPrewarmProfileResults()
		{
			for (UInt32 fontId : s_session.verifiedCodePageFonts)
			{
				if (const FontConfig* config = FindConfig(fontId))
				{
					s_session.verifiedProfileKeys.insert(
						BuildProfileKey(
							*config,
							GetPersistentFontCacheRoute()));
				}
			}
			s_session.atlasOnlyFontIds.reserve(g_configs.size());
			for (const auto& entry : g_configs)
			{
				const UInt64 profileKey = BuildProfileKey(
					entry.second, GetPersistentFontCacheRoute());
				s_session.configuredProfileKeys.insert(profileKey);
				if (FindRuntimeFont(entry.first))
				{
					++s_session.readyConfiguredRuntimes;
					if (s_session.verifiedProfileKeys.count(
							profileKey))
					{
						s_session.atlasOnlyFontIds.push_back(
							entry.first);
					}
				}
			}
			std::sort(
				s_session.atlasOnlyFontIds.begin(),
				s_session.atlasOnlyFontIds.end());
			s_session.everyConfiguredJobCompleted =
				s_session.completedFonts == s_session.queuedFonts
				&& s_session.queuedFonts
					== static_cast<UInt32>(
						s_session.configuredProfileKeys.size())
				&& s_session.readyConfiguredRuntimes
					== static_cast<UInt32>(g_configs.size());
			s_session.everyConfiguredProfileVerified =
				s_session.everyConfiguredJobCompleted
				&& s_session.verifiedCodePageFonts.size()
					== s_session.queuedFonts
				&& s_session.verifiedProfileKeys.size()
					== s_session.configuredProfileKeys.size();
		}

		void FinishIncrementalSession()
		{
			s_session.success =
				s_session.everyConfiguredJobCompleted
				&& s_session.everyConfiguredProfileVerified;
			gLog.FormattedMessage(
				"tnvse_freetype_font: incremental streamed prewarm end fonts=%u complete=%u streamFailed=%u cancelled=%u batches=%u peakBatchGlyphs=%u elapsedMs=%llu maxStepMs=%llu scanMs=%llu rasterMs=%llu streamMs=%llu memoryRetries=%u transactionRestarts=%u atlasOnlyTransaction=%s runtimeFallback=0",
				s_session.queuedFonts,
				s_session.completedFonts,
				s_session.streamFailedFonts,
				s_session.cancelledFonts,
				s_session.batches,
				s_session.peakBatchGlyphs,
				static_cast<unsigned long long>(
					GetTickCount64() - s_session.started),
				static_cast<unsigned long long>(
					s_session.maximumStepMs),
				static_cast<unsigned long long>(
					s_session.scanMs),
				static_cast<unsigned long long>(
					s_session.rasterMs),
				static_cast<unsigned long long>(
					s_session.streamMs),
				s_session.memoryRetries,
				s_session.transactionRestarts,
				!s_session.atlasOnlyTransactionStarted
					? "not-started"
					: s_session.success ? "complete" : "incomplete");
			if (!s_session.success)
			{
				ResetPrewarmTransactionForRetry(
					"incomplete-profile-validation");
				return;
			}
			TransitionPrewarmPhase(PrewarmPhase::Complete);
		}
	}


	FontPrewarmPumpStatus PumpFontPrewarmStep()
	{
		if (!g_bEnableFreeTypeFontRendering)
		{
			ShutdownFontPrewarm();
			return FontPrewarmPumpStatus::Idle;
		}

		QueueConfiguredFontPrewarms();
		if (s_session.phase == PrewarmPhase::Idle)
		{
			if (s_jobs.empty())
			{
				if (s_configuredFontsPrewarmed
					&& !s_transactionRestartPending)
				{
					return FontPrewarmPumpStatus::Idle;
				}
				if (g_configs.empty())
				{
					s_transactionRestartPending = false;
					return FontPrewarmPumpStatus::Idle;
				}
				ResetPrewarmTransactionForRetry(
					"configured-runtime-or-job-unavailable");
				return FontPrewarmPumpStatus::Active;
			}
			s_session = {};
			TransitionPrewarmPhase(PrewarmPhase::Prepare);
		}

		switch (s_session.phase)
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
			CollectPrewarmProfileResults();
			if (s_session.everyConfiguredProfileVerified)
			{
				const bool groupsReady =
					ConsolidatePhysicalFontAtlasGroups(
						s_session.rasterScale);
				const bool poolsReady =
					ConsolidatePhysicalFontAtlasPools(
						s_session.rasterScale);
				gLog.FormattedMessage(
					"tnvse_freetype_font: physical atlas consolidation groupV2=%s poolV3=%s",
					groupsReady ? "complete" : "partial-fallback",
					poolsReady ? "complete" : "partial-fallback");
				RefreshNativePrewarmOverlayTextGeometry();
			}
			if (!s_session.everyConfiguredProfileVerified)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: complete codepage mask retention required complete=%u verifiedAtlas=%u queued=%u verifiedProfiles=%u configuredProfiles=%u readyRuntimes=%u configuredFonts=%u",
					s_session.completedFonts,
					static_cast<UInt32>(
						s_session.verifiedCodePageFonts.size()),
					s_session.queuedFonts,
					static_cast<UInt32>(
						s_session.verifiedProfileKeys.size()),
					static_cast<UInt32>(
						s_session.configuredProfileKeys.size()),
					s_session.readyConfiguredRuntimes,
					static_cast<UInt32>(g_configs.size()));
			}
			RecordPrewarmStep(started);
			TransitionPrewarmPhase(PrewarmPhase::CleanupMasks);
			break;
		}
		case PrewarmPhase::CleanupMasks:
		{
			const ULONGLONG started = GetTickCount64();
			if (s_session.everyConfiguredProfileVerified
				&& GetPersistentFontCacheRoute()
					!= FontAtlasRoute::ArgbFallback
				&& !DeleteCompleteCodePageGlyphBitmapDiskCaches(
					s_session.atlasOnlyFontIds))
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
			if (s_session.everyConfiguredProfileVerified)
			{
				SetBitmapCacheReducedAfterPrewarm(true);
				EnforceCpuMemoryBudget("post-prewarm");
			}
			else
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: bitmap cache post-prewarm shrink skipped because streamed prewarm did not complete successfully complete=%u queued=%u",
					s_session.completedFonts,
					s_session.queuedFonts);
			}
			RecordPrewarmStep(started);
			TransitionPrewarmPhase(PrewarmPhase::CleanupFiles);
			break;
		}
		case PrewarmPhase::CleanupFiles:
		{
			const ULONGLONG started = GetTickCount64();
			if (g_bDeleteUnusedFreeTypeFontCache)
			{
				DeleteUnusedFreeTypeFontCacheFiles(
					s_session.everyConfiguredProfileVerified);
				if (!s_session.everyConfiguredProfileVerified)
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
			HideNativePrewarmOverlay();
			ReleaseFontPrewarmEmergencyAddressSpace();
			s_configuredFontsPrewarmed = true;
			s_session = {};
			s_transactionRestartPending = false;
			s_transactionRestartCount = 0;
			s_totalMemoryRetryCount = 0;
			return FontPrewarmPumpStatus::Completed;
		case PrewarmPhase::Idle:
		default:
			return s_transactionRestartPending
				? FontPrewarmPumpStatus::Active
				: FontPrewarmPumpStatus::Idle;
		}

		return s_transactionRestartPending
			|| s_session.phase != PrewarmPhase::Idle
			? FontPrewarmPumpStatus::Active
			: FontPrewarmPumpStatus::Idle;
	}

	FontPrewarmPumpStatus PumpFontPrewarm()
	{
		try
		{
			return PumpFontPrewarmStep();
		}
		catch (const std::bad_alloc&)
		{
			if (s_session.activeFont)
				RecordPrewarmMemoryPressure(s_session.activeFont->job);
			else
			{
				IncrementSaturating(s_session.memoryRetries);
				IncrementSaturating(s_totalMemoryRetryCount);
			}
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm pump allocation exception intercepted; restarting inside the same loading barrier");
			RetryPrewarmAfterPumpException(
				"unhandled-allocation-exception");
			return FontPrewarmPumpStatus::Active;
		}
		catch (const std::exception& error)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm pump exception intercepted type=std reason=%s; restarting inside the same loading barrier",
				error.what());
			RetryPrewarmAfterPumpException(
				"unhandled-standard-exception");
			return FontPrewarmPumpStatus::Active;
		}
		catch (...)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm pump unknown C++ exception intercepted; restarting inside the same loading barrier");
			RetryPrewarmAfterPumpException(
				"unhandled-cpp-exception");
			return FontPrewarmPumpStatus::Active;
		}
	}

	bool IsFontPrewarmActive()
	{
		return s_transactionRestartPending
			|| s_session.phase != PrewarmPhase::Idle;
	}

	void ShutdownFontPrewarm()
	{
		const bool cancelledTransaction =
			s_session.atlasOnlyTransactionStarted;
		if (s_session.activeFont)
		{
			if (RuntimeFont* runtime =
				FindRuntimeFont(s_session.activeFont->job.fontId))
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
		s_session = {};
		s_jobs.clear();
		s_scheduledProfiles.clear();
		s_configuredFontsQueued = false;
		s_configuredFontsPrewarmed = false;
		s_transactionRestartPending = false;
		s_transactionRestartCount = 0;
		s_totalMemoryRetryCount = 0;
		HideNativePrewarmOverlay();
	}
}

namespace fonthook
{
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
		vectorfont::ShutdownFontPrewarm();
	}
}

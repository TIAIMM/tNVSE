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
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace fonthook::vectorfont
{
	namespace
	{
		constexpr UInt32 kMaximumCandidatesPerBatch = 32768;
		constexpr UInt32 kMaximumGlyphsPerBatch = 4096;
		constexpr UInt32 kMaximumIncrementalGlyphsPerBatch = 512;
		constexpr UInt32 kMinimumIncrementalGlyphsPerBatch = 32;
		constexpr ULONGLONG kTargetPrewarmStepMs = 8;
		constexpr size_t kMaximumPrewarmBatchBytes = 24u * 1024u * 1024u;
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
		};

		std::deque<PrewarmJob> s_jobs;
		std::unordered_set<UInt64> s_scheduledProfiles;
		bool s_configuredFontsQueued = false;
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
			UInt32 batchGlyphLimit = kMinimumIncrementalGlyphsPerBatch;
			UInt32 maximumBatchGlyphLimit = kMaximumIncrementalGlyphsPerBatch;
			UInt32 allocationRetries = 0;
			bool exhausted = false;
			bool failed = false;
			bool cancelled = false;
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
			UInt32 completionFramesRemaining = 0;
			UInt32 readyConfiguredRuntimes = 0;
			ULONGLONG started = 0;
			ULONGLONG maximumStepMs = 0;
			bool everyConfiguredJobCompleted = false;
			bool everyConfiguredProfileVerified = false;
			bool atlasOnlyTransactionStarted = false;
			bool success = false;
		};

		PrewarmSession s_session;

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

		UInt32 ResolvePrewarmGlyphBatchLimit(const FontConfig& config,
			float rasterScale, bool shaderSdf, UInt32 sdfSpread)
		{
			size_t worstBytes = 1;
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
				size_t masks = 1;
				if (!shaderSdf)
					masks += (config.shadow.enabled ? 1u : 0u)
						+ (config.glow.enabled ? 1u : 0u)
						+ (config.outline.enabled ? 1u : 0u);
				size_t bytes = SaturatingMultiply(
					SaturatingMultiply(width, height), masks);
				if (!shaderSdf && masks > 1)
				{
					// Distance-aware CPU effects use one transient float
					// chamfer field while retaining the result masks.
					bytes = SaturatingAdd(bytes, SaturatingMultiply(
						SaturatingMultiply(width, height), 4u));
				}
				if (shaderSdf)
					bytes = SaturatingMultiply(bytes,
						DistanceFieldBytesPerPixel(
							GetConfiguredDistanceFieldMethod()));
				worstBytes = std::max(worstBytes, bytes);
			}
			const size_t configuredBudget = GetCpuMemoryBudget();
			const size_t targetBytes = std::max<size_t>(4u * 1024u * 1024u,
				std::min(kMaximumPrewarmBatchBytes, configuredBudget / 8u));
			const size_t resolved = worstBytes ? targetBytes / worstBytes : 1;
			return static_cast<UInt32>(std::clamp<size_t>(resolved, 1,
				kMaximumGlyphsPerBatch));
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
			float minimumJobProgress = 0.0f)
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
			UpdatePrewarmProgress(detail, stage ? stage : L"Preparing glyphs...", overall);
		}

		void FinishJob(const PrewarmJob& job, const char* status)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm font=%u prewarmEncoding=%s scale=%.3f glyphs=%u doubleByte=%u distanceField=%s distanceFieldGlyphs=%u status=%s",
				job.fontId,
				GetFontPrewarmRangeName(job.prewarmRange, job.codePage),
				job.rasterScaleMilli ? job.rasterScaleMilli / 1000.0f : 0.0f,
				job.rasterizedGlyphCount, job.validDoubleByteCount,
				GetConfiguredDistanceFieldMethodName(),
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
		if (!s_scheduledProfiles.insert(key).second)
		{
			const auto shared = std::find_if(s_jobs.begin(), s_jobs.end(),
				[config](const PrewarmJob& job)
				{
					return MatchesPrewarmProfile(job, *config);
				});
			if (shared != s_jobs.end())
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm profile alias font=%u owner=%u",
					fontId, shared->fontId);
			}
			return;
		}

		PrewarmJob job;
		job.fontId = fontId;
		job.profileKey = key;
		job.layoutHash = config->layoutHash;
		job.maskGenerationHash = config->maskGenerationHash;
		job.shaderEffectHash = config->shaderEffectHash;
		job.codePage = GetFreeTypeTextCodePage();
		job.route = route;
		job.prewarmRange = ResolveFontPrewarmRange(*config);
		const FontPrewarmRange prewarmRange = job.prewarmRange;
		const UInt32 codePage = job.codePage;
		ResetPrewarmScan(job, 0);
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

	namespace
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

		void RecordPrewarmStep(ULONGLONG started)
		{
			const ULONGLONG elapsed = GetTickCount64() - started;
			s_session.maximumStepMs =
				std::max(s_session.maximumStepMs, elapsed);
		}

		void PrepareIncrementalSession()
		{
			const FontAtlasRoute finalRoute = GetPersistentFontCacheRoute();
			std::unordered_set<UInt64> reboundProfiles;
			while (!s_jobs.empty())
			{
				PrewarmJob job = std::move(s_jobs.front());
				s_jobs.pop_front();
				const FontConfig* config = FindConfig(job.fontId);
				if (!config)
					continue;
				job.route = finalRoute;
				job.profileKey = BuildProfileKey(*config, finalRoute);
				if (!reboundProfiles.insert(job.profileKey).second)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: final-route prewarm alias font=%u route=%u",
						job.fontId, static_cast<UInt32>(finalRoute));
					continue;
				}
				s_session.restoreJobs.push_back(std::move(job));
			}
			s_scheduledProfiles = reboundProfiles;
			if (s_session.restoreJobs.empty())
			{
				EndAtlasOnlyPrewarmPolicy();
				TransitionPrewarmPhase(PrewarmPhase::Idle);
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
				"tnvse_freetype_font: incremental streamed prewarm begin fonts=%u scale=%.3f batchTargetMiB=%.2f targetStepMs=%llu",
				s_session.queuedFonts, s_session.rasterScale,
				kMaximumPrewarmBatchBytes / (1024.0 * 1024.0),
				static_cast<unsigned long long>(kTargetPrewarmStepMs));
			ShowNativePrewarmOverlay();
			UpdatePrewarmProgress(
				L"Validating persistent font cache...",
				L"Checking streamed atlas snapshots...",
				0.0f);
			TransitionPrewarmPhase(PrewarmPhase::RestoreSnapshots);
		}

		void RestoreOnePrewarmSnapshot()
		{
			if (s_session.restoreJobs.empty())
			{
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

			wchar_t detail[160] = {};
			_snwprintf_s(detail, _countof(detail), _TRUNCATE,
				L"Font %u of %u  |  ID %u",
				std::min(s_session.queuedFonts, s_session.finishedFonts + 1),
				s_session.queuedFonts, job.fontId);
			UpdatePrewarmProgress(detail, L"Restoring validated atlas snapshot...",
				s_session.queuedFonts
					? static_cast<float>(s_session.finishedFonts)
						/ s_session.queuedFonts
					: 0.0f);

			bool snapshotReady = false;
			if (g_bEnableFreeTypeDefaultPoolAtlas)
			{
				try
				{
					snapshotReady =
						TryLoadGloballyRepackedGlyphAtlasSnapshot(
							*runtime, s_session.rasterScale);
				}
				catch (const std::bad_alloc&)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: snapshot restore allocation failed font=%u scale=%.3f; regenerating with bounded batches",
						job.fontId, s_session.rasterScale);
				}
				catch (...)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: snapshot restore raised an unexpected exception font=%u; regenerating",
						job.fontId);
				}
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
				if (BuildDirectGlyphAtlasTables(
						*runtime, s_session.rasterScale)
					&& PublishSealedProfileAliases(
						job.fontId, s_session.rasterScale))
				{
					FinishJob(job, "snapshot");
					s_session.verifiedCodePageFonts.push_back(job.fontId);
					++s_session.completedFonts;
				}
				else
				{
					FinishJob(job, "snapshot-direct-failed");
					++s_session.streamFailedFonts;
				}
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
			if (active.sharedDoubleAlias
				&& !TryLoadGloballyRepackedGlyphAtlasSnapshotRole(
					*runtime, VectorFontByteClass::DoubleByte,
					s_session.rasterScale))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: shared distance-field double-byte role unavailable font=%u owner=%u; deferring alias atlas",
					active.job.fontId,
					config->mtsdfDoubleByteOwnerFontId);
				FinishJob(active.job, "shared-role-unavailable");
				++s_session.streamFailedFonts;
				++s_session.finishedFonts;
				return;
			}

			const UInt32 memoryLimit = ResolvePrewarmGlyphBatchLimit(
				*config, s_session.rasterScale, active.shaderSdf,
				active.sdfSpread);
			active.maximumBatchGlyphLimit = std::max<UInt32>(
				1, std::min(memoryLimit,
					kMaximumIncrementalGlyphsPerBatch));
			active.batchGlyphLimit = std::min(
				active.maximumBatchGlyphLimit,
				kMinimumIncrementalGlyphsPerBatch);
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
			ReportPrewarmProgress(
				s_session.activeFont->job,
				std::min(s_session.queuedFonts,
					s_session.finishedFonts + 1),
				s_session.queuedFonts,
				s_session.finishedFonts,
				L"Preparing streamed glyph batches...");
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
			const UInt32 candidateLimit = std::min(
				kMaximumCandidatesPerBatch,
				std::max<UInt32>(
					256, active.batchGlyphLimit * 8u));

			s_session.requestedGlyphs.clear();
			s_session.bitmapRequests.clear();
			s_session.bitmapResults.clear();
			try
			{
				s_session.requestedGlyphs.reserve(
					active.batchGlyphLimit);
				s_session.bitmapRequests.reserve(
					static_cast<size_t>(
						active.batchGlyphLimit) * 4u);
			}
			catch (const std::bad_alloc&)
			{
				if (active.batchGlyphLimit <= 1
					|| active.allocationRetries >= 6)
				{
					active.failed = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: prewarm request-buffer allocation failed font=%u limit=%u retries=%u",
						active.job.fontId,
						active.batchGlyphLimit,
						active.allocationRetries);
					TransitionPrewarmPhase(
						PrewarmPhase::FinalizeFont);
				}
				else
				{
					const UInt32 previous =
						active.batchGlyphLimit;
					active.batchGlyphLimit =
						std::max<UInt32>(
							1, active.batchGlyphLimit / 2);
					++active.allocationRetries;
					ReleasePrewarmBatchReferences(
						"prewarm-request-allocation-retry");
					gLog.FormattedMessage(
						"tnvse_freetype_font: prewarm request-buffer retry font=%u limit=%u->%u retry=%u",
						active.job.fontId, previous,
						active.batchGlyphLimit,
						active.allocationRetries);
				}
				RecordPrewarmStep(stepStarted);
				return;
			}

			const bool needsGrayFill =
				!active.shaderSdf
				&& !active.aggressiveComposite;
			UInt32 candidates = 0;
			UInt32 glyphCount = 0;
			while (candidates < candidateLimit
				&& glyphCount < active.batchGlyphLimit)
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

			bool retrySmallerBatch = false;
			if (!s_session.bitmapRequests.empty())
			{
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
					if (active.batchGlyphLimit > 1
						&& active.allocationRetries < 6)
					{
						const UInt32 previous =
							active.batchGlyphLimit;
						active.batchGlyphLimit =
							std::max<UInt32>(
								1,
								active.batchGlyphLimit / 2);
						++active.allocationRetries;
						active.job.encodedUnitIndex =
							encodedUnitStart;
						active.job.validDoubleByteCount =
							doubleByteStart;
						active.job.rasterizedGlyphCount =
							rasterizedStart;
						active.job.sdfGlyphCount = sdfStart;
						active.exhausted = false;
						retrySmallerBatch = true;
						gLog.FormattedMessage(
							"tnvse_freetype_font: prewarm allocation retry font=%u scale=%.3f batchGlyphs=%u limit=%u->%u retry=%u",
							active.job.fontId,
							s_session.rasterScale,
							glyphCount, previous,
							active.batchGlyphLimit,
							active.allocationRetries);
					}
					else
					{
						active.failed = true;
						gLog.FormattedMessage(
							"tnvse_freetype_font: streamed prewarm allocation failed font=%u scale=%.3f batchGlyphs=%u retries=%u",
							active.job.fontId,
							s_session.rasterScale,
							glyphCount,
							active.allocationRetries);
					}
				}
				catch (...)
				{
					active.failed = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: streamed prewarm batch raised an unexpected exception font=%u",
						active.job.fontId);
				}

				if (!active.failed && !retrySmallerBatch
					&& s_session.bitmapResults.size()
						!= s_session.bitmapRequests.size())
				{
					active.failed = true;
				}
				if (!active.failed && !retrySmallerBatch)
				{
					active.failed = !AppendStreamingPrewarmAtlas(
						*runtime,
						s_session.bitmapRequests,
						s_session.bitmapResults,
						s_session.rasterScale);
				}
			}

			ReleasePrewarmBatchReferences(
				retrySmallerBatch
					? "prewarm-allocation-retry"
					: "prewarm-stream-batch");
			++s_session.batches;
			const ULONGLONG elapsed =
				GetTickCount64() - stepStarted;
			s_session.maximumStepMs =
				std::max(s_session.maximumStepMs, elapsed);
			if (!retrySmallerBatch && !active.failed)
			{
				if (elapsed > kTargetPrewarmStepMs + 4
					&& active.batchGlyphLimit > 1)
				{
					active.batchGlyphLimit =
						std::max<UInt32>(
							1,
							active.batchGlyphLimit / 2);
				}
				else if (elapsed < kTargetPrewarmStepMs / 2
					&& active.batchGlyphLimit
						< active.maximumBatchGlyphLimit)
				{
					active.batchGlyphLimit = std::min(
						active.maximumBatchGlyphLimit,
						active.batchGlyphLimit
							+ std::max<UInt32>(
								8,
								active.batchGlyphLimit / 4));
				}
			}

			ReportPrewarmProgress(
				active.job,
				std::min(s_session.queuedFonts,
					s_session.finishedFonts + 1),
				s_session.queuedFonts,
				s_session.finishedFonts,
				retrySmallerBatch
					? L"Reducing batch size after memory pressure..."
					: active.shaderSdf
						? (UsesMtsdfDistanceField()
							? L"Streaming MTSDF glyphs to disk..."
							: L"Streaming true-SDF glyphs to disk...")
						: active.aggressiveComposite
							? L"Streaming aggressive BGRA composite glyphs to disk..."
							: L"Generating bounded fallback masks...");

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

			if (active.cancelled || !runtime)
			{
				if (runtime)
					CancelStreamingPrewarmAtlas(*runtime);
				FinishJob(active.job, "cancelled");
				++s_session.cancelledFonts;
			}
			else if (active.failed)
			{
				CancelStreamingPrewarmAtlas(*runtime);
				DiscardGlyphAtlasSnapshot(
					*runtime, s_session.rasterScale);
				FinishJob(active.job, "stream-failed");
				++s_session.streamFailedFonts;
			}
			else
			{
				ReportPrewarmProgress(
					active.job,
					std::min(s_session.queuedFonts,
						s_session.finishedFonts + 1),
					s_session.queuedFonts,
					s_session.finishedFonts,
					L"Publishing and globally repacking atlas pages...",
					0.95f);
				bool finalized = false;
				try
				{
					finalized = FinalizeStreamingPrewarmAtlas(
						*runtime, s_session.rasterScale);
				}
				catch (const std::bad_alloc&)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: streamed prewarm finalization allocation failed font=%u scale=%.3f",
						active.job.fontId,
						s_session.rasterScale);
				}
				catch (...)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: streamed prewarm finalization raised an unexpected exception font=%u",
						active.job.fontId);
				}
				if (finalized
					&& active.job.route
						== FontAtlasRoute::ArgbFallback
					&& !MarkCurrentFallbackBitmapProfilesUsed(
						*runtime, s_session.rasterScale))
				{
					finalized = false;
					gLog.FormattedMessage(
						"tnvse_freetype_font: fallback prewarm validation failed font=%u reason=incomplete-persistent-mask-set",
						active.job.fontId);
				}
				if (!finalized)
				{
					CancelStreamingPrewarmAtlas(*runtime);
					DiscardGlyphAtlasSnapshot(
						*runtime, s_session.rasterScale);
					FinishJob(active.job,
						"stream-finalize-failed");
					++s_session.streamFailedFonts;
				}
				else if (!BuildDirectGlyphAtlasTables(
							*runtime, s_session.rasterScale)
					|| !PublishSealedProfileAliases(
							active.job.fontId,
							s_session.rasterScale))
				{
					FinishJob(active.job,
						"complete-direct-failed");
					++s_session.streamFailedFonts;
				}
				else
				{
					if (g_bEnableFreeTypeDefaultPoolAtlas)
					{
						s_session.verifiedCodePageFonts.push_back(
							active.job.fontId);
					}
					FinishJob(active.job, "complete");
					++s_session.completedFonts;
				}
			}
			++s_session.finishedFonts;
			UpdatePrewarmProgress(
				L"Streamed font atlas is ready.",
				L"Preparing the next font...",
				s_session.queuedFonts
					? static_cast<float>(
						s_session.finishedFonts)
						/ s_session.queuedFonts
					: 1.0f);
			RefreshNativePrewarmOverlayTextGeometry();
			s_session.activeFont.reset();
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
				s_session.everyConfiguredJobCompleted;
			gLog.FormattedMessage(
				"tnvse_freetype_font: incremental streamed prewarm end fonts=%u complete=%u streamFailed=%u cancelled=%u batches=%u elapsedMs=%llu maxStepMs=%llu atlasOnlyTransaction=%s",
				s_session.queuedFonts,
				s_session.completedFonts,
				s_session.streamFailedFonts,
				s_session.cancelledFonts,
				s_session.batches,
				static_cast<unsigned long long>(
					GetTickCount64() - s_session.started),
				static_cast<unsigned long long>(
					s_session.maximumStepMs),
				!s_session.atlasOnlyTransactionStarted
					? "not-started"
					: s_session.success ? "complete" : "incomplete");
			UpdatePrewarmProgress(
				s_session.success
					? L"Font cache is ready."
					: L"Font cache generation was incomplete.",
				s_session.success
					? L"Starting the game..."
					: L"Starting with runtime fallback...",
				1.0f);
			s_session.completionFramesRemaining = 1;
			TransitionPrewarmPhase(PrewarmPhase::Complete);
		}
	}


	FontPrewarmPumpStatus PumpFontPrewarm()
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
				return FontPrewarmPumpStatus::Idle;
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
			if (s_session.everyConfiguredJobCompleted)
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
					s_session.everyConfiguredJobCompleted);
				if (!s_session.everyConfiguredJobCompleted)
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
			if (s_session.completionFramesRemaining)
			{
				--s_session.completionFramesRemaining;
				return FontPrewarmPumpStatus::Active;
			}
			HideNativePrewarmOverlay();
			s_session = {};
			return FontPrewarmPumpStatus::Completed;
		case PrewarmPhase::Idle:
		default:
			return FontPrewarmPumpStatus::Idle;
		}

		return s_session.phase == PrewarmPhase::Idle
			? FontPrewarmPumpStatus::Idle
			: FontPrewarmPumpStatus::Active;
	}

	bool IsFontPrewarmActive()
	{
		return s_session.phase != PrewarmPhase::Idle;
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
		if (cancelledTransaction)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: incremental streamed prewarm atlas-only transaction result=cancelled");
		}
		s_session = {};
		s_jobs.clear();
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

#include "font_vector_internal.h"

#include "load_config.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>

namespace fonthook::vectorfont
{
	namespace implementation::font_performance {}
	using namespace implementation::font_performance;

	namespace implementation::font_performance
	{
		constexpr size_t kCounterCount = static_cast<size_t>(FreeTypePerfCounter::Count);
		constexpr size_t kPhaseCount =
			static_cast<size_t>(FreeTypePerfPhase::Count);
		constexpr size_t kDurationExponentBuckets = 64;
		constexpr size_t kDurationSubBuckets = 4;
		constexpr size_t kDurationBuckets =
			kDurationExponentBuckets * kDurationSubBuckets;
		constexpr size_t kInstancingSizeBuckets = 32;
		struct PerformanceState
		{
			std::array<std::atomic<UInt64>, kCounterCount> counters = {};
			std::array<std::array<std::atomic<UInt64>,
				kDurationBuckets>, kPhaseCount> durations = {};
			std::array<std::atomic<UInt64>, kPhaseCount>
				durationNanoseconds = {};
			std::array<std::atomic<UInt64>, kInstancingSizeBuckets>
				instancingTextSizes = {};
			std::array<std::atomic<UInt64>, kInstancingSizeBuckets>
				instancingInstanceSizes = {};
			std::atomic<UInt64> instancingTextMax = 0;
			std::atomic<UInt64> instancingInstanceMax = 0;
			ULONGLONG lastReport = 0;
		};

		PerformanceState& GetPerformanceState()
		{
			static PerformanceState* state =
				new PerformanceState();
			return *state;
		}

		SInt64 QueryPerfFrequency()
		{
			static const SInt64 frequency = []()
			{
				LARGE_INTEGER value = {};
				return QueryPerformanceFrequency(&value)
					? value.QuadPart : 0;
			}();
			return frequency;
		}

		void RecordDuration(FreeTypePerfPhase phase, SInt64 ticks)
		{
			const SInt64 frequency = QueryPerfFrequency();
			if (ticks <= 0 || frequency <= 0)
				return;
			const long double scaled =
				static_cast<long double>(ticks)
				* 1000000000.0L
				/ static_cast<long double>(frequency);
			const UInt64 nanoseconds = static_cast<UInt64>(
				std::max<long double>(1.0L, scaled));
			size_t exponent = 0;
			UInt64 upper = 1;
			while (upper < nanoseconds
				&& exponent + 1 < kDurationExponentBuckets)
			{
				upper = upper
					<= std::numeric_limits<UInt64>::max() / 2
						? upper * 2
						: std::numeric_limits<UInt64>::max();
				++exponent;
			}
			size_t subBucket = kDurationSubBuckets - 1u;
			if (exponent >= 3 && upper != std::numeric_limits<UInt64>::max())
			{
				const UInt64 lower = upper / 2u;
				const UInt64 step = (upper - lower) / kDurationSubBuckets;
				subBucket = static_cast<size_t>(std::min<UInt64>(
					kDurationSubBuckets - 1u,
					(nanoseconds - lower - 1u) / step));
			}
			const size_t bucket = exponent * kDurationSubBuckets + subBucket;
			GetPerformanceState().durations[
				static_cast<size_t>(phase)][bucket]
				.fetch_add(1, std::memory_order_relaxed);
			GetPerformanceState().durationNanoseconds[
				static_cast<size_t>(phase)].fetch_add(
					nanoseconds, std::memory_order_relaxed);
		}

		struct DurationSummary
		{
			UInt64 count = 0;
			double medianMicroseconds = 0.0;
			double p95Microseconds = 0.0;
			double meanMicroseconds = 0.0;
		};

		DurationSummary ConsumeDurationSummary(
			FreeTypePerfPhase phase)
		{
			std::array<UInt64, kDurationBuckets> values = {};
			DurationSummary result;
			for (size_t bucket = 0;
				bucket < values.size(); ++bucket)
			{
				values[bucket] = GetPerformanceState().durations[
					static_cast<size_t>(phase)][bucket]
						.exchange(0, std::memory_order_relaxed);
				result.count += values[bucket];
			}
			const UInt64 totalNanoseconds =
				GetPerformanceState().durationNanoseconds[
					static_cast<size_t>(phase)].exchange(
						0, std::memory_order_relaxed);
			if (!result.count)
				return result;
			result.meanMicroseconds = static_cast<double>(totalNanoseconds)
				/ static_cast<double>(result.count) / 1000.0;
			auto quantile = [&](UInt64 numerator,
				UInt64 denominator)
			{
				const UInt64 target = std::max<UInt64>(1,
					(result.count * numerator
						+ denominator - 1) / denominator);
				UInt64 cumulative = 0;
				for (size_t bucket = 0;
					bucket < values.size(); ++bucket)
				{
					cumulative += values[bucket];
					if (cumulative < target)
						continue;
					const size_t exponent = bucket / kDurationSubBuckets;
					const size_t subBucket = bucket % kDurationSubBuckets;
					long double upperNanoseconds = std::ldexp(
						1.0L, static_cast<int>(exponent));
					if (exponent >= 3)
					{
						const long double lowerNanoseconds =
							upperNanoseconds / 2.0L;
						const long double step =
							(upperNanoseconds - lowerNanoseconds)
							/ static_cast<long double>(kDurationSubBuckets);
						upperNanoseconds = lowerNanoseconds
							+ step * static_cast<long double>(subBucket + 1u);
					}
					return static_cast<double>(
						upperNanoseconds / 1000.0L);
				}
				return static_cast<double>(
					std::numeric_limits<UInt64>::max())
					/ 1000.0;
			};
			result.medianMicroseconds = quantile(50, 100);
			result.p95Microseconds = quantile(95, 100);
			return result;
		}

		struct SizeSummary
		{
			UInt64 count = 0;
			UInt64 median = 0;
			UInt64 p95 = 0;
			UInt64 maximum = 0;
		};

		size_t SizeBucket(UInt32 value)
		{
			size_t bucket = 0;
			UInt64 upper = 1;
			while (upper < value
				&& bucket + 1 < kInstancingSizeBuckets)
			{
				upper <<= 1;
				++bucket;
			}
			return bucket;
		}

		SizeSummary ConsumeSizeSummary(
			std::array<std::atomic<UInt64>, kInstancingSizeBuckets>& source,
			std::atomic<UInt64>& maximum)
		{
			std::array<UInt64, kInstancingSizeBuckets> values = {};
			SizeSummary result;
			result.maximum = maximum.exchange(0, std::memory_order_relaxed);
			for (size_t bucket = 0; bucket < values.size(); ++bucket)
			{
				values[bucket] = source[bucket].exchange(
					0, std::memory_order_relaxed);
				result.count += values[bucket];
			}
			if (!result.count)
				return result;
			auto quantile = [&](UInt64 numerator)
			{
				const UInt64 target = std::max<UInt64>(1,
					(result.count * numerator + 99u) / 100u);
				UInt64 cumulative = 0;
				for (size_t bucket = 0; bucket < values.size(); ++bucket)
				{
					cumulative += values[bucket];
					if (cumulative >= target)
						return UInt64{ 1 } << bucket;
				}
				return UInt64{ 1 }
					<< (kInstancingSizeBuckets - 1u);
			};
			result.median = quantile(50);
			result.p95 = quantile(95);
			// The histogram stores power-of-two upper bounds while maximum is
			// exact. Clamp the displayed quantiles so a six-text sample cannot be
			// reported as median=8/max=6.
			result.median = std::min(result.median, result.maximum);
			result.p95 = std::min(result.p95, result.maximum);
			return result;
		}
	}

	FreeTypePerfScope::FreeTypePerfScope(
		FreeTypePerfPhase phase, bool enabled)
		: m_phase(phase),
		m_active(enabled && g_bEnableFreeTypeFontRenderingLog)
	{
		if (!m_active)
			return;
		LARGE_INTEGER now = {};
		if (!QueryPerformanceCounter(&now))
		{
			m_active = false;
			return;
		}
		m_start = now.QuadPart;
	}

	FreeTypePerfScope::~FreeTypePerfScope()
	{
		if (!m_active)
			return;
		LARGE_INTEGER now = {};
		if (QueryPerformanceCounter(&now))
			RecordDuration(m_phase, now.QuadPart - m_start);
	}

	void RecordFreeTypePerf(FreeTypePerfCounter counter, UInt64 amount)
	{
		if (g_bEnableFreeTypeFontRenderingLog)
		{
			GetPerformanceState().counters[
				static_cast<size_t>(counter)].fetch_add(
					amount, std::memory_order_relaxed);
		}
	}

	SInt64 BeginFreeTypePerfSample()
	{
		if (!g_bEnableFreeTypeFontRenderingLog)
			return 0;
		LARGE_INTEGER now = {};
		return QueryPerformanceCounter(&now) ? now.QuadPart : 0;
	}

	void EndFreeTypePerfSample(FreeTypePerfPhase phase, SInt64 start)
	{
		if (!start || !g_bEnableFreeTypeFontRenderingLog)
			return;
		LARGE_INTEGER now = {};
		if (QueryPerformanceCounter(&now))
			RecordDuration(phase, now.QuadPart - start);
	}

	void RecordFreeTypeGlyphInstancingBatchSize(
		UInt32 textCount, UInt32 instanceCount)
	{
		if (!g_bEnableFreeTypeFontRenderingLog
			|| !textCount || !instanceCount)
		{
			return;
		}
		PerformanceState& state = GetPerformanceState();
		state.instancingTextSizes[SizeBucket(textCount)].fetch_add(
			1, std::memory_order_relaxed);
		state.instancingInstanceSizes[SizeBucket(instanceCount)].fetch_add(
			1, std::memory_order_relaxed);
		auto updateMaximum = [](std::atomic<UInt64>& target, UInt64 value)
		{
			UInt64 current = target.load(std::memory_order_relaxed);
			while (current < value
				&& !target.compare_exchange_weak(current, value,
					std::memory_order_relaxed,
					std::memory_order_relaxed))
			{
			}
		};
		updateMaximum(state.instancingTextMax, textCount);
		updateMaximum(state.instancingInstanceMax, instanceCount);
	}

	void ReportFreeTypePerf(bool force)
	{
		if (!g_bEnableFreeTypeFontRenderingLog)
			return;
		const ULONGLONG now = GetTickCount64();
		PerformanceState& state = GetPerformanceState();
		if (!force && state.lastReport && now - state.lastReport < 10000)
			return;
		state.lastReport = now;
		std::array<UInt64, kCounterCount> values = {};
		for (size_t i = 0; i < values.size(); ++i)
			values[i] = state.counters[i].exchange(
				0, std::memory_order_relaxed);
		const auto counterValue = [&values](FreeTypePerfCounter counter)
		{
			return values[static_cast<size_t>(counter)];
		};
		const SizeSummary instancingTextSizes = ConsumeSizeSummary(
			state.instancingTextSizes, state.instancingTextMax);
		const SizeSummary instancingInstanceSizes = ConsumeSizeSummary(
			state.instancingInstanceSizes, state.instancingInstanceMax);
		const UInt64 stockConstantUpdates =
			values[static_cast<size_t>(
				FreeTypePerfCounter::StockConstantUpdate)];
		const UInt64 stockC0CompatibilityRepublishes =
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					StockPixelConstantCompatibilityRepublish)];
		const UInt64 stockC0RepublishElided =
			stockConstantUpdates >= stockC0CompatibilityRepublishes
				? stockConstantUpdates
					- stockC0CompatibilityRepublishes
				: 0;
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf: bitmap_mem=%llu cross_font=%llu disk_hit=%llu miss=%llu write=%llu read_bytes=%llu write_bytes=%llu raster=%llu bitmap_batch_requests=%llu deduped=%llu prepared_text_hit=%llu miss=%llu promotion_bypass=%llu probe=%llu probe_miss=%llu admitted=%llu evicted=%llu admission_rejected=%llu backoff_bypass=%llu atlas_hit=%llu create=%llu grow=%llu uploads=%llu bytes=%llu upload_rects=%llu text_artifact_hit=%llu miss=%llu hot=%llu bypass=%llu admitted=%llu evicted=%llu shader_batches=%llu cpu_effect_masks_avoided=%llu gpu_resident_glyph_hit=%llu miss=%llu atlas_snapshot_profile_reuse=%llu dynamic_vb_uploads=%llu bytes=%llu reuse=%llu discards=%llu static_vb_uploads=%llu bytes=%llu hits=%llu promotion_failed=%llu sorted_static_batches=%llu payloads=%llu bytes=%llu merged_packet_ranges=%llu metadata_hot=%llu locked=%llu sorted_facades=%llu unique_payloads=%llu frame_lookup_hits=%llu preflight_fast=%llu full=%llu direct_static=%llu direct_dynamic=%llu sorted_dynamic_batches=%llu payloads=%llu bytes=%llu lockless_packets=%llu visibility_checks=%llu culled=%llu app=%llu alpha=%llu clip=%llu scissor=%llu scissor_pre31=%llu scissor_post31=%llu preflight_skipped=%llu packets_saved=%llu vertices_saved=%llu direct_shape_candidates=%llu direct_shape_draws=%llu direct_shape_vertices=%llu direct_shape_fallback=%llu constant_ownership_segments=%llu reuses=%llu releases=%llu snapshot_gets_elided=%llu restore_sets_elided=%llu stock_constant_updates=%llu reuses=%llu composite_constant_full=%llu private_reuses=%llu partial=%llu stock_c0_republish_elided=%llu compat_republishes=%llu private_registers_uploaded=%llu full_tail_elided=%llu stock_tile_private_preserves=%llu sampler_sets=%llu reuses=%llu composite_onequad_single_page=%llu onequad_paged=%llu onequad_build_fallback=%llu legacy_multipage_fallback=%llu shader_fallback=%llu composite_draws=%llu tile_passes=%llu cache_hit=%llu miss=%llu state_changes=%llu generated=%llu evicted=%llu cache_bytes=%llu budget_reject=%llu rtt_fail=%llu restore_fail=%llu visual_validated=%llu rejected=%llu inconclusive=%llu",
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapMemoryHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapCrossFontHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapDiskHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapDiskMiss)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapDiskWrite)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapDiskReadBytes)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapDiskWriteBytes)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapRasterized)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapBatchRequest)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapBatchDedupe)],
			values[static_cast<size_t>(FreeTypePerfCounter::PreparedTextHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::PreparedTextMiss)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::PreparedTextPromotionBypass)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::PreparedTextGlobalProbe)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::PreparedTextGlobalProbeMiss)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::PreparedTextAdmission)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::PreparedTextEviction)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::PreparedTextAdmissionRejected)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::PreparedTextRejectionBypass)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasCreated)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasGrown)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasUpload)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasUploadBytes)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasUploadRect)],
			values[static_cast<size_t>(FreeTypePerfCounter::TextArtifactHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::TextArtifactMiss)],
			values[static_cast<size_t>(FreeTypePerfCounter::TextArtifactHotHit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::TextArtifactAdmissionBypass)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::TextArtifactAdmission)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::TextArtifactEviction)],
			values[static_cast<size_t>(FreeTypePerfCounter::ShaderEffectBatch)],
			values[static_cast<size_t>(FreeTypePerfCounter::CpuEffectMasksAvoided)],
			values[static_cast<size_t>(FreeTypePerfCounter::GpuResidentGlyphHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::GpuResidentGlyphMiss)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasSnapshotProfileReuse)],
			values[static_cast<size_t>(FreeTypePerfCounter::DynamicVertexUpload)],
			values[static_cast<size_t>(FreeTypePerfCounter::DynamicVertexUploadBytes)],
			values[static_cast<size_t>(FreeTypePerfCounter::DynamicVertexReuse)],
			values[static_cast<size_t>(FreeTypePerfCounter::DynamicVertexDiscard)],
			values[static_cast<size_t>(FreeTypePerfCounter::StaticVertexUpload)],
			values[static_cast<size_t>(FreeTypePerfCounter::StaticVertexUploadBytes)],
			values[static_cast<size_t>(FreeTypePerfCounter::StaticVertexHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::StaticVertexPromotionFailed)],
			values[static_cast<size_t>(FreeTypePerfCounter::SortedStaticBatch)],
			values[static_cast<size_t>(FreeTypePerfCounter::SortedStaticPayload)],
			values[static_cast<size_t>(FreeTypePerfCounter::SortedStaticBytes)],
			values[static_cast<size_t>(FreeTypePerfCounter::MergedPacketRange)],
			values[static_cast<size_t>(FreeTypePerfCounter::MetadataHotHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::MetadataLockedLookup)],
			values[static_cast<size_t>(FreeTypePerfCounter::SortedFrameFacade)],
			values[static_cast<size_t>(FreeTypePerfCounter::SortedFramePayload)],
			values[static_cast<size_t>(FreeTypePerfCounter::SortedFrameLookupHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::PreflightFastHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::PreflightFullValidation)],
			values[static_cast<size_t>(FreeTypePerfCounter::DirectStaticResidencyHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::DirectDynamicResidencyHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::SortedDynamicBatch)],
			values[static_cast<size_t>(FreeTypePerfCounter::SortedDynamicPayload)],
			values[static_cast<size_t>(FreeTypePerfCounter::SortedDynamicBytes)],
			values[static_cast<size_t>(FreeTypePerfCounter::LocklessPacketPrepare)],
			values[static_cast<size_t>(FreeTypePerfCounter::VisibilityCheck)],
			values[static_cast<size_t>(FreeTypePerfCounter::VisibilityCulled)],
			values[static_cast<size_t>(FreeTypePerfCounter::VisibilityAppCulled)],
			values[static_cast<size_t>(FreeTypePerfCounter::VisibilityZeroAlpha)],
			values[static_cast<size_t>(FreeTypePerfCounter::VisibilityClip)],
			values[static_cast<size_t>(FreeTypePerfCounter::VisibilityScissor)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VisibilityScissorPreConstants)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VisibilityScissorPostConstants)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VisibilityPreflightSkipped)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VisibilityPacketsSaved)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VisibilityVerticesSaved)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectCandidate)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectDraw)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectVertex)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallback)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ConstantOwnershipSegment)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ConstantOwnershipReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ConstantOwnershipRelease)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ConstantSnapshotGetElided)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ConstantRestoreSetElided)],
			stockConstantUpdates,
			static_cast<UInt64>(0),
			values[static_cast<size_t>(
				FreeTypePerfCounter::CompositeConstantFullUpload)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::NativePacketConstantReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CompositeConstantPartialUpload)],
			stockC0RepublishElided,
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					StockPixelConstantCompatibilityRepublish)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::NativePacketConstantRegisterUpload)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::NativePacketConstantFullTailElided)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::NativePrivateStateStockTilePreserve)],
			values[static_cast<size_t>(FreeTypePerfCounter::SamplerStateSet)],
			values[static_cast<size_t>(FreeTypePerfCounter::SamplerStateReuse)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeFusedEligible)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeOrderedEligible)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeOverlapFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeMultiPageFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeShaderFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeDraw)],
			values[static_cast<size_t>(FreeTypePerfCounter::TilePass)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeCacheHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeCacheMiss)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeCacheStateChange)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeCacheGenerated)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeCacheEvicted)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeCacheBytes)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeCacheBudgetReject)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeCacheRttFailure)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeCacheRestoreFailure)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeVisualValidated)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeVisualRejected)],
			values[static_cast<size_t>(FreeTypePerfCounter::CompositeVisualInconclusive)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_pre_accumulator_cull: checks=%llu eligible=%llu culled=%llu alpha=%llu scissor_deferred=%llu fail_open=%llu",
			values[static_cast<size_t>(FreeTypePerfCounter::
				VisibilityPreAccumulatorCheck)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				VisibilityPreAccumulatorEligible)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				VisibilityPreAccumulatorCulled)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				VisibilityPreAccumulatorZeroAlpha)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				VisibilityPreAccumulatorScissorDeferred)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				VisibilityPreAccumulatorFailOpen)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_static_promotion: deferred_lifecycle=%llu deferred_upload_history=%llu deferred_budget=%llu deferred_retry=%llu cold_evictions=%llu cold_evicted_bytes=%llu",
			values[static_cast<size_t>(FreeTypePerfCounter::
				StaticPromotionDeferredLifecycle)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				StaticPromotionDeferredUploadHistory)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				StaticPromotionDeferredBudget)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				StaticPromotionDeferredRetry)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				StaticResidentColdEviction)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				StaticResidentColdEvictionBytes)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_native_registration: artifact_sealed=%llu artifact_fallback=%llu hook_fast=%llu hook_slow=%llu proxy_fast=%llu proxy_slow=%llu",
			counterValue(
				FreeTypePerfCounter::NativeRegistrationArtifactSealed),
			counterValue(
				FreeTypePerfCounter::NativeRegistrationArtifactFallback),
			counterValue(FreeTypePerfCounter::NativeRegistrationHookFast),
			counterValue(FreeTypePerfCounter::NativeRegistrationHookSlow),
			counterValue(FreeTypePerfCounter::NativeRegistrationProxyFast),
			counterValue(FreeTypePerfCounter::NativeRegistrationProxySlow));
		FreeTypeFontDebugLog(
			"tnvse_freetype_structural_fastpaths: readiness_raw_hit=%llu full_audit=%llu hook_mismatch=%llu renderer_mismatch=%llu virtualquery_avoided=%llu tile_callback_mismatch=%llu render_alpha_mismatch=%llu sort_call_mismatch=%llu sort_tail_mismatch=%llu immediate_mismatch=%llu atlas_mismatch=%llu singleton_inline=%llu singleton_heap=%llu child_allocations_avoided=%llu metadata_reserve=%llu metadata_rehash=%llu sequence_hits=%llu sequence_fallbacks=%llu sequence_rescanned_items=%llu prefix_shrinks=%llu prefix_texts=%llu prefix_replayed=%llu texture_alias=%llu viewport_descriptor_hit=%llu viewport_descriptor_rebuild=%llu viewport_descriptor_fail=%llu prepared_reject_hit=%llu prepared_reject_stored=%llu prepared_profile_invalidations=%llu",
			counterValue(FreeTypePerfCounter::StructuralReadinessRawHit),
			counterValue(FreeTypePerfCounter::StructuralReadinessFullAudit),
			counterValue(FreeTypePerfCounter::StructuralReadinessHookMismatch),
			counterValue(FreeTypePerfCounter::StructuralReadinessRendererMismatch),
			counterValue(FreeTypePerfCounter::StructuralReadinessVirtualQueryAvoided),
			counterValue(FreeTypePerfCounter::
				StructuralReadinessTileCallbackMismatch),
			counterValue(FreeTypePerfCounter::
				StructuralReadinessRenderAlphaMismatch),
			counterValue(FreeTypePerfCounter::
				StructuralReadinessSortCallMismatch),
			counterValue(FreeTypePerfCounter::
				StructuralReadinessSortTailMismatch),
			counterValue(FreeTypePerfCounter::
				StructuralReadinessImmediateMismatch),
			counterValue(FreeTypePerfCounter::
				StructuralReadinessAtlasMismatch),
			counterValue(FreeTypePerfCounter::VirtualSingletonInlinePayload),
			counterValue(FreeTypePerfCounter::VirtualSingletonHeapPayload),
			counterValue(FreeTypePerfCounter::VirtualSingletonChildAllocationAvoided),
			counterValue(FreeTypePerfCounter::MetadataMapReserve),
			counterValue(FreeTypePerfCounter::MetadataMapRehash),
			counterValue(FreeTypePerfCounter::CommandSequenceSkeletonHit),
			counterValue(FreeTypePerfCounter::CommandSequenceSkeletonFallback),
			counterValue(FreeTypePerfCounter::CommandSequenceSkeletonItem),
			counterValue(FreeTypePerfCounter::GlyphInstancingPrefixShrink),
			counterValue(FreeTypePerfCounter::GlyphInstancingPrefixRetainedText),
			counterValue(FreeTypePerfCounter::GlyphInstancingPrefixReplayedText),
			counterValue(FreeTypePerfCounter::GlyphInstancingSourceTextureAlias),
			counterValue(FreeTypePerfCounter::ViewportDescriptorHit),
			counterValue(FreeTypePerfCounter::ViewportDescriptorRebuild),
			counterValue(FreeTypePerfCounter::ViewportDescriptorFail),
			counterValue(FreeTypePerfCounter::PreparedTextRejectCacheHit),
			counterValue(FreeTypePerfCounter::PreparedTextRejectCacheStored),
			counterValue(FreeTypePerfCounter::PreparedTextProfileEpochInvalidation));
		FreeTypeFontDebugLog(
			"tnvse_freetype_sort: original_anchor_sorts=%llu anchor_items=%llu anchor_mixed_runs=%llu anchor_fallbacks=%llu anchor_predecessor_fallbacks=%llu anchor_proof_fallbacks=%llu mixed_equal_depth_runs_restored=%llu items_restored=%llu restore_rejected=%llu",
			values[static_cast<size_t>(
				FreeTypePerfCounter::SortedOriginalOrderAnchorSort)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SortedOriginalOrderAnchorItem)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SortedOriginalOrderAnchorMixedRun)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SortedOriginalOrderAnchorFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				SortedOriginalOrderAnchorPredecessorFallback)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SortedOriginalOrderAnchorProofFallback)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SortedMixedEqualDepthRunRestored)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SortedMixedEqualDepthItemRestored)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SortedMixedEqualDepthRestoreRejected)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_sort_proof: stock_equivalent_sorts=%llu items=%llu ordinal_sidecar_recovered=%llu sidecar_mixed_runs=%llu sidecar_legacy=%llu fail_gate=%llu fail_count=%llu fail_storage=%llu fail_source=%llu fail_depth=%llu fail_metadata=%llu fail_registration=%llu fail_group=%llu fail_singleton=%llu fail_coverage=%llu fail_apply=%llu",
			values[static_cast<size_t>(FreeTypePerfCounter::
				SortedOriginalOrderStockEquivalentSort)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				SortedOriginalOrderStockEquivalentItem)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				SortedOriginalOrderSidecarRecovered)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				SortedOriginalOrderSidecarMixedRun)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				SortedOriginalOrderSidecarLegacy)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				SortedOriginalOrderAnchorFailGate)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				SortedOriginalOrderAnchorFailCount)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				SortedOriginalOrderAnchorFailStorage)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				SortedOriginalOrderAnchorFailSource)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				SortedOriginalOrderAnchorFailDepth)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				SortedOriginalOrderAnchorFailMetadata)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				SortedOriginalOrderAnchorFailRegistration)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				SortedOriginalOrderAnchorFailGroup)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				SortedOriginalOrderAnchorFailSingleton)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				SortedOriginalOrderAnchorFailCoverage)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				SortedOriginalOrderAnchorFailApply)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_viewport_cull: nodes=%llu install_failed=%llu checks=%llu fast_visible=%llu deep_checks=%llu deep_tiles=%llu culled=%llu app_culled=%llu fail_open=%llu fail_listindex=%llu fail_clips=%llu fail_clipwindow=%llu fail_root_bounds=%llu fail_transform=%llu fail_node_identity=%llu fail_subtree_topology=%llu fail_subtree_bounds=%llu deep_overlap=%llu",
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportNodeInstalled)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportNodeInstallFailed)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportCullCheck)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportCullFastVisible)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportCullDeepCheck)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportCullDeepTile)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportCulled)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportAppCulled)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportFailOpen)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportFailListIndex)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportFailClips)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportFailClipWindow)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportFailRootBounds)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportFailTransform)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportFailNodeIdentity)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportFailSubtreeTopology)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportFailSubtreeBounds)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportDeepOverlap)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf: constant_capture_mirror=%llu driver=%llu state_shadow_driver_gets=%llu isolation_bypass=%llu vertex_aa_sets=%llu reuses=%llu vertex_aa_stock_preserved=%llu command_program_setups=%llu binds_elided=%llu texture_sets=%llu reuses=%llu command_packet_constant_full=%llu partial=%llu reuses=%llu registers_uploaded=%llu full_tail_elided=%llu",
			static_cast<UInt64>(0),
			static_cast<UInt64>(0),
			static_cast<UInt64>(0),
			static_cast<UInt64>(0),
			values[static_cast<size_t>(
				FreeTypePerfCounter::VertexAaConstantSet)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VertexAaConstantReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VertexAaConstantStockPreserved)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandProgramSetup)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandProgramBindElided)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandTextureBindSet)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandTextureBindReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandPacketConstantFullUpload)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandPacketConstantPartialUpload)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandPacketConstantReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandPacketConstantRegisterUpload)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandPacketConstantFullTailElided)]);
		const UInt64 directFacadeClassified =
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeModelData)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeAlphaProperty)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeBufferData)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeTileProperty)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeStreamCount)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeVertexStride)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeVertexChipArray)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeVertexChip)];
		const UInt64 directFallbackClassified =
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackCommand)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackSubmission)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingInput)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingTopology)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingAtlas)]
			+ directFacadeClassified
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingProperty)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingTexture)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingShader)]
			+ values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackRuntime)];
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf: direct_fallback_total=%llu classified=%llu command=%llu submission=%llu binding_input=%llu binding_topology=%llu binding_atlas=%llu binding_facade=%llu facade_classified=%llu facade_model_data=%llu facade_alpha_property=%llu facade_buffer_data=%llu facade_tile_property=%llu facade_stream_count=%llu facade_vertex_stride=%llu facade_vertex_chip_array=%llu facade_vertex_chip=%llu binding_property=%llu binding_texture=%llu binding_shader=%llu runtime=%llu synthetic_buffers=%llu",
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallback)],
			directFallbackClassified,
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackCommand)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackSubmission)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingInput)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingTopology)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingAtlas)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingFacade)],
			directFacadeClassified,
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeModelData)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeAlphaProperty)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeBufferData)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeTileProperty)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeStreamCount)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeVertexStride)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeVertexChipArray)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackFacadeVertexChip)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingProperty)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingTexture)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackBindingShader)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectFallbackRuntime)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SinglePacketDirectSyntheticBuffer)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf: virtual_stock_candidates=%llu virtual_stock_singletons=%llu virtual_stock_groups=%llu virtual_stock_shapes=%llu virtual_stock_draws=%llu virtual_stock_static_hits=%llu virtual_stock_dynamic_hits=%llu virtual_stock_rebinds=%llu virtual_stock_revokes=%llu facade_fallbacks=%llu followers_skipped=%llu sorted_preflight_saved=%llu proxy_packets_saved=%llu fallback_no_parent=%llu packet_limit=%llu cpu_budget=%llu static_not_ready=%llu topology=%llu shader=%llu generation=%llu atlas=%llu resource=%llu noncontiguous=%llu registration_resolved=%llu register_rejected=%llu registration_missing=%llu registration_duplicate=%llu registration_order_mismatch=%llu",
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockCandidate)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockSingleton)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockGroup)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockShape)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockDraw)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockStaticHit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockDynamicHit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockRebind)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockRevoke)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockFacadeFallback)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockFollowerSkipped)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockSortedPreflightSaved)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockProxyPacketSaved)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockFallbackNoParent)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockFallbackPacketLimit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockFallbackCpuBudget)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockFallbackStaticNotReady)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockFallbackTopology)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockFallbackShader)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockFallbackGeneration)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockFallbackAtlas)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockFallbackResource)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockFallbackNoncontiguous)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockRegistrationResolved)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockRegistrationRejected)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockRegistrationMissing)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockRegistrationDuplicate)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockRegistrationOrderMismatch)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf: command_recorded=%llu single_packet_commands=%llu single_packet_build_fallbacks=%llu single_packet_hits=%llu single_packet_misses=%llu single_packet_replays=%llu single_packet_fallbacks=%llu spans=%llu packets=%llu span_hits=%llu span_misses=%llu retained_bridge_draws=%llu native_replays=%llu stock_bootstraps_saved=%llu virtual_spans_fused=%llu followers_consumed=%llu direct_range_replays=%llu span_full_validations=%llu light_validations=%llu packet_epoch_guards=%llu packet_state_elisions=%llu packet_range_validations=%llu packet_range_packets=%llu render_target_validations=%llu execution_segments=%llu segment_full_validations=%llu segment_validation_reuses=%llu segment_invalidations=%llu stock_tile_bridges=%llu instancing_bridges=%llu bridge_rejected=%llu retained_program_hits=%llu retained_program_misses=%llu fallback_token=%llu generation=%llu atlas=%llu resource=%llu topology=%llu hook=%llu nested=%llu render_target=%llu state=%llu",
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandRecorded)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSinglePacketRecorded)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSinglePacketBuildFallback)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSinglePacketHit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSinglePacketMiss)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSinglePacketReplay)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSinglePacketFallback)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSpanRecorded)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandPacketRecorded)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSpanHit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSpanMiss)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandRetainedBridgeDraw)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandNativeReplay)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandStockBootstrapSaved)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandVirtualSpanFused)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandVirtualFollowerConsumed)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandDirectRangeReplay)],
			static_cast<UInt64>(0),
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandPacketLightValidation)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandPacketEpochGuard)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					CommandPacketStateValidationElided)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandPacketRangeValidation)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandPacketRangeValidated)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandRenderTargetValidation)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandExecutionSegment)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSegmentFullValidation)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSegmentValidationReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSegmentInvalidation)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSegmentStockTileBridge)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSegmentInstancingBridge)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSegmentBridgeRejected)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandRetainedProgramHit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandRetainedProgramMiss)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandFallbackToken)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandFallbackGeneration)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandFallbackAtlas)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandFallbackResource)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandFallbackTopology)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandFallbackHook)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandFallbackNested)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandFallbackRenderTarget)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandFallbackState)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf: command_virtual_single_recorded=%llu hits=%llu misses=%llu replays=%llu fallbacks=%llu build_view_hits=%llu build_view_misses=%llu build_binding_reuses=%llu build_vector_growth=%llu deferred_render_target_captures=%llu tile_retained_builds=%llu refreshes=%llu hits=%llu misses=%llu packet_reuses=%llu",
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					CommandVirtualSinglePacketRecorded)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					CommandVirtualSinglePacketHit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					CommandVirtualSinglePacketMiss)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					CommandVirtualSinglePacketReplay)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					CommandVirtualSinglePacketFallback)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandBuildViewHit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandBuildViewMiss)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandBuildBindingReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandBuildVectorGrowth)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					CommandDeferredRenderTargetCapture)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandTileRetainedBuild)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandTileRetainedRefresh)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandTileRetainedHit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandTileRetainedMiss)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					CommandTileRetainedPacketReuse)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf: standard_pass_lite_candidates=%llu retained_builds=%llu retained_reuses=%llu retained_hits=%llu retained_misses=%llu stage1_eligible=%llu stage2_resident=%llu stage3_replays=%llu standard_v2_replays=%llu standard_v2_compat=%llu stock_fallbacks=%llu fallback_envelope=%llu program=%llu renderer=%llu geometry=%llu binding=%llu prelude=%llu",
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteCandidate)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteRetainedBuild)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteRetainedReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteRetainedHit)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteRetainedMiss)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteStage1Eligible)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteStage2Resident)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteStage3Replay)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassV2Replay)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::
					StandardPassV2CompatibilityReplay)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteStockFallback)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteFallbackEnvelope)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteFallbackProgram)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteFallbackRenderer)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteFallbackGeometry)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteFallbackBinding)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StandardPassLiteFallbackPrelude)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_direct_draw_lite: candidates=%llu replays=%llu fallbacks=%llu program=%llu renderer=%llu geometry=%llu binding=%llu declaration=%llu binding_sets=%llu binding_reuses=%llu binding_device_failures=%llu draw_device_failures=%llu",
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteCandidate)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteReplay)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteFallbackProgram)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteFallbackRenderer)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteFallbackGeometry)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteFallbackBinding)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteFallbackDeclaration)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteBindingSet)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteBindingReuse)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteBindingDeviceFailure)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeDirectDrawLiteDrawDeviceFailure)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_glyph_instancing: candidates=%llu accepted_batches=%llu texts=%llu instances=%llu compat_precomputed=%llu compat_live_suffix=%llu compat_immutable_words_avoided=%llu draws=%llu draws_saved=%llu upload_bytes=%llu buffer_growth=%llu discards=%llu fallback_depth=%llu state=%llu scissor=%llu topology=%llu budget=%llu frequency_sets=%llu frequency_resets=%llu binding_failures=%llu constant_failures=%llu draw_failures=%llu device_failures=%llu followers_consumed=%llu state_proofs=%llu proof_failures=%llu restore_failures=%llu begin_fallbacks=%llu begin_contract=%llu begin_pass=%llu begin_callback=%llu begin_resource=%llu begin_immutable=%llu begin_transient=%llu begin_preflight=%llu begin_snapshot=%llu snapshot_avoided_texts=%llu begin_upload=%llu begin_follower=%llu arm_fallbacks=%llu validation_fallbacks=%llu direct_draw_fallbacks=%llu text_median=%llu text_p95=%llu text_max=%llu instance_median=%llu instance_p95=%llu instance_max=%llu",
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingCandidate)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingAcceptedBatch)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingAcceptedText)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingInstance)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingAcceptedBatch)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				GlyphInstancingCompatibilityLiveSuffixCheck)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				GlyphInstancingCompatibilityLiveSuffixCheck)]
				* kGlyphInstancingImmutableCompatibilityWordCount,
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingDraw)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingDrawSaved)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingUploadByte)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingBufferGrowth)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingDiscard)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingDepthFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingStateFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingScissorFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingTopologyFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingBudgetFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingStreamFrequencySet)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingStreamFrequencyReset)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingBindingFailure)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingConstantFailure)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingDrawFailure)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingDeviceFailure)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingFollowerConsumed)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingStateProof)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingStateProofFailure)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingRestoreFailure)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingBeginFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingBeginContractFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingBeginPassFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingBeginCallbackFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingBeginResourceFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingBeginImmutableFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingBeginTransientFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingBeginPreflightFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingBeginSnapshotFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingLiveSnapshotAvoidedText)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingBeginUploadFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingBeginFollowerFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingArmFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingValidationFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::GlyphInstancingDirectDrawFallback)],
			instancingTextSizes.median,
			instancingTextSizes.p95,
			instancingTextSizes.maximum,
			instancingInstanceSizes.median,
			instancingInstanceSizes.p95,
			instancingInstanceSizes.maximum);
		FreeTypeFontDebugLog(
			"tnvse_freetype_glyph_instancing_compat_mismatch: total=%llu admission=%llu live=%llu program=%llu declaration=%llu source_texture=%llu alpha_texture=%llu atlas_texture=%llu constants=%llu texture_transform=%llu clamp_mode=%llu shader_class=%llu sampling=%llu quality=%llu distance_field_method=%llu layer=%llu atlas_page=%llu alpha_flags=%llu alpha_test_ref=%llu texture_mode_flags=%llu shader_flags=%llu shader_alpha=%llu shader_fade_alpha=%llu",
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchTotal),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchAdmission),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchLive),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchProgram),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchDeclaration),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchSourceTexture),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchAlphaTexture),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchAtlasTexture),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchConstants),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchTextureTransform),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchClampMode),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchShaderClass),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchSampling),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchQuality),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchDistanceFieldMethod),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchLayer),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchAtlasPage),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchAlphaFlags),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchAlphaTestRef),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchTextureModeFlags),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchShaderFlags),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchShaderAlpha),
			counterValue(FreeTypePerfCounter::
				GlyphInstancingCompatibilityMismatchShaderFadeAlpha));
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf: segment_device_state_starts=%llu segment_device_state_reuses=%llu stock_tile_bridges=%llu stock_tile_resets=%llu instancing_narrow_invalidates=%llu pass_sets=%llu pass_reuses=%llu constants_sets=%llu constants_reuses=%llu constants_lite_replays=%llu constants_lite_fallbacks=%llu constants_lite_scaled_fallbacks=%llu blend_sets=%llu blend_reuses=%llu alpha_test_sets=%llu alpha_test_reuses=%llu drawmode_sets=%llu drawmode_reuses=%llu post_calls=%llu post_elisions=%llu",
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceStateStart)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceStateReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceStockTileBridge)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceStockTileReset)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				SegmentDeviceInstancingNarrowInvalidate)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDevicePassSet)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDevicePassReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceConstantsSet)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceConstantsReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::NativeTileConstantsLiteReplay)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::NativeTileConstantsLiteFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeTileConstantsLiteScaledScissorFallback)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceBlendSet)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceBlendReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceAlphaTestSet)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceAlphaTestReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceDrawmodeSet)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDeviceDrawmodeReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDevicePostSet)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::SegmentDevicePostElision)]);
		FreeTypeFontDebugLog(
			"tnvse_freetype_constants_translation_lite: replays=%llu transient_replays=%llu fallbacks=%llu not_applicable=%llu scaled_scissor=%llu nonfinite=%llu device_failure=%llu",
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeTileConstantsTranslationLiteReplay)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeTileConstantsTranslationLiteTransientReplay)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeTileConstantsTranslationLiteFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeTileConstantsTranslationLiteNotApplicableFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeTileConstantsTranslationLiteScaledScissorFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeTileConstantsTranslationLiteNonFiniteFallback)],
			values[static_cast<size_t>(FreeTypePerfCounter::
				NativeTileConstantsTranslationLiteDeviceFailure)]);
		const UInt64 worldRotationOnly = counterValue(
			FreeTypePerfCounter::
				SegmentDeviceConstantsWorldMismatchRotationOnly);
		const UInt64 worldTranslationOnly = counterValue(
			FreeTypePerfCounter::
				SegmentDeviceConstantsWorldMismatchTranslationOnly);
		const UInt64 worldScaleOnly = counterValue(
			FreeTypePerfCounter::
				SegmentDeviceConstantsWorldMismatchScaleOnly);
		const UInt64 worldRotationTranslation = counterValue(
			FreeTypePerfCounter::
				SegmentDeviceConstantsWorldMismatchRotationTranslation);
		const UInt64 worldRotationScale = counterValue(
			FreeTypePerfCounter::
				SegmentDeviceConstantsWorldMismatchRotationScale);
		const UInt64 worldTranslationScale = counterValue(
			FreeTypePerfCounter::
				SegmentDeviceConstantsWorldMismatchTranslationScale);
		const UInt64 worldRotationTranslationScale = counterValue(
			FreeTypePerfCounter::
				SegmentDeviceConstantsWorldMismatchRotationTranslationScale);
		const UInt64 worldUnclassified = counterValue(
			FreeTypePerfCounter::SegmentDeviceConstantsFirstMismatchWorld);
		const UInt64 worldMismatchTotal = worldRotationOnly
			+ worldTranslationOnly + worldScaleOnly
			+ worldRotationTranslation + worldRotationScale
			+ worldTranslationScale + worldRotationTranslationScale
			+ worldUnclassified;
		const UInt64 worldRotation = worldRotationOnly
			+ worldRotationTranslation + worldRotationScale
			+ worldRotationTranslationScale;
		const UInt64 worldTranslation = worldTranslationOnly
			+ worldRotationTranslation + worldTranslationScale
			+ worldRotationTranslationScale;
		const UInt64 worldScale = worldScaleOnly + worldRotationScale
			+ worldTranslationScale + worldRotationTranslationScale;
		const UInt64 constantsFirstMismatchTotal =
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchProgram)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchRotates)
			+ worldMismatchTotal
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchView)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchProjection)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchViewProjection)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchCameraRight)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchCameraUp)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchOverlayColor)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchTextureTransform)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchTileAlpha)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchMaterialAlpha)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchNearDepth)
			+ counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchDepthRange);
		FreeTypeFontDebugLog(
			"tnvse_freetype_constants_mismatch: total=%llu program=%llu rotates=%llu world=%llu view=%llu projection=%llu view_projection=%llu camera_right=%llu camera_up=%llu overlay_color=%llu texture_transform=%llu tile_alpha=%llu material_alpha=%llu near_depth=%llu depth_range=%llu",
			constantsFirstMismatchTotal,
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchProgram),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchRotates),
			worldMismatchTotal,
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchView),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchProjection),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchViewProjection),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchCameraRight),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchCameraUp),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchOverlayColor),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchTextureTransform),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchTileAlpha),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchMaterialAlpha),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchNearDepth),
			counterValue(FreeTypePerfCounter::
				SegmentDeviceConstantsFirstMismatchDepthRange));
		FreeTypeFontDebugLog(
			"tnvse_freetype_constants_world_mismatch: total=%llu rotation=%llu translation=%llu scale=%llu rotation_only=%llu translation_only=%llu scale_only=%llu rotation_translation=%llu rotation_scale=%llu translation_scale=%llu rotation_translation_scale=%llu unclassified=%llu",
			worldMismatchTotal, worldRotation, worldTranslation, worldScale,
			worldRotationOnly, worldTranslationOnly, worldScaleOnly,
			worldRotationTranslation, worldRotationScale,
			worldTranslationScale, worldRotationTranslationScale,
			worldUnclassified);
		const DurationSummary layout =
			ConsumeDurationSummary(FreeTypePerfPhase::Layout);
		const DurationSummary sidecar =
			ConsumeDurationSummary(FreeTypePerfPhase::Sidecar);
		const DurationSummary directCompile =
			ConsumeDurationSummary(FreeTypePerfPhase::DirectCompile);
		const DurationSummary nativeRegistration =
			ConsumeDurationSummary(
				FreeTypePerfPhase::NativeRegistration);
		const DurationSummary registrationReadiness =
			ConsumeDurationSummary(
				FreeTypePerfPhase::NativeRegistrationReadiness);
		const DurationSummary registrationShape =
			ConsumeDurationSummary(
				FreeTypePerfPhase::NativeRegistrationShape);
		const DurationSummary registrationBudget =
			ConsumeDurationSummary(
				FreeTypePerfPhase::NativeRegistrationBudget);
		const DurationSummary registrationAllocation =
			ConsumeDurationSummary(
				FreeTypePerfPhase::NativeRegistrationAllocation);
		const DurationSummary registrationPayload =
			ConsumeDurationSummary(
				FreeTypePerfPhase::NativeRegistrationPayload);
		const DurationSummary registrationAccounting =
			ConsumeDurationSummary(
				FreeTypePerfPhase::NativeRegistrationAccounting);
		const DurationSummary registrationPublish =
			ConsumeDurationSummary(
				FreeTypePerfPhase::NativeRegistrationPublish);
		const DurationSummary preflight =
			ConsumeDurationSummary(FreeTypePerfPhase::Preflight);
		const DurationSummary submit =
			ConsumeDurationSummary(FreeTypePerfPhase::Submit);
		const DurationSummary commandBuild =
			ConsumeDurationSummary(FreeTypePerfPhase::CommandBuild);
		const DurationSummary commandBuildStamp =
			ConsumeDurationSummary(
				FreeTypePerfPhase::CommandBuildStamp);
		const DurationSummary commandBuildVirtual =
			ConsumeDurationSummary(
				FreeTypePerfPhase::CommandBuildVirtual);
		const DurationSummary commandBuildOrdinary =
			ConsumeDurationSummary(
				FreeTypePerfPhase::CommandBuildOrdinary);
		const DurationSummary commandBuildFinalize =
			ConsumeDurationSummary(
				FreeTypePerfPhase::CommandBuildFinalize);
		const DurationSummary commandSubmit =
			ConsumeDurationSummary(FreeTypePerfPhase::CommandSubmit);
		const DurationSummary extendedFnt =
			ConsumeDurationSummary(
				FreeTypePerfPhase::ExtendedFntGeometry);
		const DurationSummary instancingAdmission =
			ConsumeDurationSummary(
				FreeTypePerfPhase::GlyphInstancingAdmission);
		const DurationSummary instancingLeader =
			ConsumeDurationSummary(
				FreeTypePerfPhase::GlyphInstancingLeader);
		const DurationSummary instancingLivePreflight =
			ConsumeDurationSummary(
				FreeTypePerfPhase::GlyphInstancingLivePreflight);
		const DurationSummary instancingSnapshot =
			ConsumeDurationSummary(
				FreeTypePerfPhase::GlyphInstancingSnapshot);
		const DurationSummary instancingUpload =
			ConsumeDurationSummary(
				FreeTypePerfPhase::GlyphInstancingUpload);
		const DurationSummary instancingFollowerReserve =
			ConsumeDurationSummary(
				FreeTypePerfPhase::GlyphInstancingFollowerReserve);
		const DurationSummary instancingBind =
			ConsumeDurationSummary(
				FreeTypePerfPhase::GlyphInstancingBind);
		const DurationSummary instancingDraw =
			ConsumeDurationSummary(
				FreeTypePerfPhase::GlyphInstancingDraw);
		const DurationSummary instancingRestore =
			ConsumeDurationSummary(
				FreeTypePerfPhase::GlyphInstancingRestore);
		const DurationSummary viewportCull =
			ConsumeDurationSummary(FreeTypePerfPhase::ViewportCull);
		const DurationSummary viewportFastVisible =
			ConsumeDurationSummary(FreeTypePerfPhase::ViewportCullFastVisible);
		const DurationSummary viewportDeepSuccess =
			ConsumeDurationSummary(FreeTypePerfPhase::ViewportCullDeepSuccess);
		const DurationSummary viewportDeepFailTransform =
			ConsumeDurationSummary(
				FreeTypePerfPhase::ViewportCullDeepFailTransform);
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf_timing: layout_n=%llu median_us=%.3f p95_us=%.3f sidecar_n=%llu median_us=%.3f p95_us=%.3f direct_compile_n=%llu median_us=%.3f p95_us=%.3f native_registration_n=%llu median_us=%.3f p95_us=%.3f preflight_n=%llu median_us=%.3f p95_us=%.3f submit_n=%llu median_us=%.3f p95_us=%.3f command_build_n=%llu median_us=%.3f p95_us=%.3f command_submit_n=%llu median_us=%.3f p95_us=%.3f extended_fnt_geometry_n=%llu median_us=%.3f p95_us=%.3f",
			layout.count, layout.medianMicroseconds,
			layout.p95Microseconds,
			sidecar.count, sidecar.medianMicroseconds,
			sidecar.p95Microseconds,
			directCompile.count,
			directCompile.medianMicroseconds,
			directCompile.p95Microseconds,
			nativeRegistration.count,
			nativeRegistration.medianMicroseconds,
			nativeRegistration.p95Microseconds,
			preflight.count, preflight.medianMicroseconds,
			preflight.p95Microseconds,
			submit.count, submit.medianMicroseconds,
			submit.p95Microseconds,
			commandBuild.count, commandBuild.medianMicroseconds,
			commandBuild.p95Microseconds,
			commandSubmit.count, commandSubmit.medianMicroseconds,
			commandSubmit.p95Microseconds,
			extendedFnt.count, extendedFnt.medianMicroseconds,
			extendedFnt.p95Microseconds);
		FreeTypeFontDebugLog(
			"tnvse_freetype_command_build_timing: stamp_n=%llu median_us=%.3f p95_us=%.3f virtual_n=%llu median_us=%.3f p95_us=%.3f ordinary_n=%llu median_us=%.3f p95_us=%.3f finalize_n=%llu median_us=%.3f p95_us=%.3f",
			commandBuildStamp.count,
			commandBuildStamp.medianMicroseconds,
			commandBuildStamp.p95Microseconds,
			commandBuildVirtual.count,
			commandBuildVirtual.medianMicroseconds,
			commandBuildVirtual.p95Microseconds,
			commandBuildOrdinary.count,
			commandBuildOrdinary.medianMicroseconds,
			commandBuildOrdinary.p95Microseconds,
			commandBuildFinalize.count,
			commandBuildFinalize.medianMicroseconds,
			commandBuildFinalize.p95Microseconds);
		FreeTypeFontDebugLog(
			"tnvse_freetype_glyph_instancing_timing: admission_n=%llu median_us=%.3f p95_us=%.3f leader_n=%llu median_us=%.3f p95_us=%.3f live_preflight_n=%llu median_us=%.3f p95_us=%.3f snapshot_n=%llu median_us=%.3f p95_us=%.3f upload_n=%llu median_us=%.3f p95_us=%.3f follower_reserve_n=%llu median_us=%.3f p95_us=%.3f bind_n=%llu median_us=%.3f p95_us=%.3f draw_n=%llu median_us=%.3f p95_us=%.3f restore_n=%llu median_us=%.3f p95_us=%.3f",
			instancingAdmission.count,
			instancingAdmission.medianMicroseconds,
			instancingAdmission.p95Microseconds,
			instancingLeader.count,
			instancingLeader.medianMicroseconds,
			instancingLeader.p95Microseconds,
			instancingLivePreflight.count,
			instancingLivePreflight.medianMicroseconds,
			instancingLivePreflight.p95Microseconds,
			instancingSnapshot.count,
			instancingSnapshot.medianMicroseconds,
			instancingSnapshot.p95Microseconds,
			instancingUpload.count,
			instancingUpload.medianMicroseconds,
			instancingUpload.p95Microseconds,
			instancingFollowerReserve.count,
			instancingFollowerReserve.medianMicroseconds,
			instancingFollowerReserve.p95Microseconds,
			instancingBind.count,
			instancingBind.medianMicroseconds,
			instancingBind.p95Microseconds,
			instancingDraw.count,
			instancingDraw.medianMicroseconds,
			instancingDraw.p95Microseconds,
			instancingRestore.count,
			instancingRestore.medianMicroseconds,
			instancingRestore.p95Microseconds);
		FreeTypeFontDebugLog(
			"tnvse_freetype_native_registration_timing: readiness_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f shape_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f budget_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f allocation_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f payload_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f accounting_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f publish_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f total_mean_us=%.3f",
			registrationReadiness.count, registrationReadiness.meanMicroseconds,
			registrationReadiness.medianMicroseconds,
			registrationReadiness.p95Microseconds,
			registrationShape.count, registrationShape.meanMicroseconds,
			registrationShape.medianMicroseconds,
			registrationShape.p95Microseconds,
			registrationBudget.count, registrationBudget.meanMicroseconds,
			registrationBudget.medianMicroseconds,
			registrationBudget.p95Microseconds,
			registrationAllocation.count, registrationAllocation.meanMicroseconds,
			registrationAllocation.medianMicroseconds,
			registrationAllocation.p95Microseconds,
			registrationPayload.count, registrationPayload.meanMicroseconds,
			registrationPayload.medianMicroseconds,
			registrationPayload.p95Microseconds,
			registrationAccounting.count, registrationAccounting.meanMicroseconds,
			registrationAccounting.medianMicroseconds,
			registrationAccounting.p95Microseconds,
			registrationPublish.count, registrationPublish.meanMicroseconds,
			registrationPublish.medianMicroseconds,
			registrationPublish.p95Microseconds,
			nativeRegistration.meanMicroseconds);
		FreeTypeFontDebugLog(
			"tnvse_freetype_viewport_cull_timing: total_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f fast_visible_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f deep_success_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f deep_fail_transform_n=%llu mean_us=%.3f median_us=%.3f p95_us=%.3f",
			viewportCull.count, viewportCull.meanMicroseconds,
			viewportCull.medianMicroseconds, viewportCull.p95Microseconds,
			viewportFastVisible.count, viewportFastVisible.meanMicroseconds,
			viewportFastVisible.medianMicroseconds,
			viewportFastVisible.p95Microseconds,
			viewportDeepSuccess.count, viewportDeepSuccess.meanMicroseconds,
			viewportDeepSuccess.medianMicroseconds,
			viewportDeepSuccess.p95Microseconds,
			viewportDeepFailTransform.count,
			viewportDeepFailTransform.meanMicroseconds,
			viewportDeepFailTransform.medianMicroseconds,
			viewportDeepFailTransform.p95Microseconds);
	}
}

namespace fonthook
{
	void RecordFreeTypePreparedTextCacheResult(bool hit)
	{
		vectorfont::RecordFreeTypePerf(hit
			? vectorfont::FreeTypePerfCounter::PreparedTextHit
			: vectorfont::FreeTypePerfCounter::PreparedTextMiss);
	}

	void RecordFreeTypeViewportNodeInstallResult(bool installed)
	{
		vectorfont::RecordFreeTypePerf(installed
			? vectorfont::FreeTypePerfCounter::ViewportNodeInstalled
			: vectorfont::FreeTypePerfCounter::ViewportNodeInstallFailed);
	}

	void RecordFreeTypeViewportCullResult(
		bool culled, bool failOpen, bool fastVisible,
		bool deepCheck, UInt32 visitedTiles,
		FreeTypeViewportCullFailReason failReason,
		bool deepOverlap, bool appCulled)
	{
		using vectorfont::FreeTypePerfCounter;
		vectorfont::RecordFreeTypePerf(FreeTypePerfCounter::ViewportCullCheck);
		if (fastVisible)
		{
			vectorfont::RecordFreeTypePerf(
				FreeTypePerfCounter::ViewportCullFastVisible);
		}
		if (deepCheck)
		{
			vectorfont::RecordFreeTypePerf(
				FreeTypePerfCounter::ViewportCullDeepCheck);
			vectorfont::RecordFreeTypePerf(
				FreeTypePerfCounter::ViewportCullDeepTile, visitedTiles);
		}
		if (culled)
			vectorfont::RecordFreeTypePerf(FreeTypePerfCounter::ViewportCulled);
		if (failOpen)
			vectorfont::RecordFreeTypePerf(FreeTypePerfCounter::ViewportFailOpen);
		switch (failReason)
		{
		case FreeTypeViewportCullFailReason::ListIndex:
			vectorfont::RecordFreeTypePerf(
				FreeTypePerfCounter::ViewportFailListIndex);
			break;
		case FreeTypeViewportCullFailReason::Clips:
			vectorfont::RecordFreeTypePerf(
				FreeTypePerfCounter::ViewportFailClips);
			break;
		case FreeTypeViewportCullFailReason::ClipWindow:
			vectorfont::RecordFreeTypePerf(
				FreeTypePerfCounter::ViewportFailClipWindow);
			break;
		case FreeTypeViewportCullFailReason::RootBounds:
			vectorfont::RecordFreeTypePerf(
				FreeTypePerfCounter::ViewportFailRootBounds);
			break;
		case FreeTypeViewportCullFailReason::Transform:
			vectorfont::RecordFreeTypePerf(
				FreeTypePerfCounter::ViewportFailTransform);
			break;
		case FreeTypeViewportCullFailReason::NodeIdentity:
			vectorfont::RecordFreeTypePerf(
				FreeTypePerfCounter::ViewportFailNodeIdentity);
			break;
		case FreeTypeViewportCullFailReason::SubtreeTopology:
			vectorfont::RecordFreeTypePerf(
				FreeTypePerfCounter::ViewportFailSubtreeTopology);
			break;
		case FreeTypeViewportCullFailReason::SubtreeBounds:
			vectorfont::RecordFreeTypePerf(
				FreeTypePerfCounter::ViewportFailSubtreeBounds);
			break;
		default:
			break;
		}
		if (deepOverlap)
		{
			vectorfont::RecordFreeTypePerf(
				FreeTypePerfCounter::ViewportDeepOverlap);
		}
		if (appCulled)
		{
			vectorfont::RecordFreeTypePerf(
				FreeTypePerfCounter::ViewportAppCulled);
		}
	}

	void PumpFreeTypeFontPerformance()
	{
		vectorfont::ReportFreeTypePerf(false);
	}

	void ReportFreeTypeFontPerformanceNow()
	{
		vectorfont::ReportFreeTypePerf(true);
	}
}

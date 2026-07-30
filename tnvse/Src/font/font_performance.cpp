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
		constexpr size_t kDurationBuckets = 64;
		struct PerformanceState
		{
			std::array<std::atomic<UInt64>, kCounterCount> counters = {};
			std::array<std::array<std::atomic<UInt64>,
				kDurationBuckets>, kPhaseCount> durations = {};
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
			size_t bucket = 0;
			UInt64 upper = 1;
			while (upper < nanoseconds
				&& bucket + 1 < kDurationBuckets)
			{
				upper = upper
					<= std::numeric_limits<UInt64>::max() / 2
						? upper * 2
						: std::numeric_limits<UInt64>::max();
				++bucket;
			}
			GetPerformanceState().durations[
				static_cast<size_t>(phase)][bucket]
				.fetch_add(1, std::memory_order_relaxed);
		}

		struct DurationSummary
		{
			UInt64 count = 0;
			double medianMicroseconds = 0.0;
			double p95Microseconds = 0.0;
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
			if (!result.count)
				return result;
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
					const long double upperNanoseconds =
						std::ldexp(1.0L,
							static_cast<int>(bucket));
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

	void ReportFreeTypePerf()
	{
		if (!g_bEnableFreeTypeFontRenderingLog)
			return;
		const ULONGLONG now = GetTickCount64();
		PerformanceState& state = GetPerformanceState();
		if (state.lastReport && now - state.lastReport < 10000)
			return;
		state.lastReport = now;
		std::array<UInt64, kCounterCount> values = {};
		for (size_t i = 0; i < values.size(); ++i)
			values[i] = state.counters[i].exchange(
				0, std::memory_order_relaxed);
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf: bitmap_mem=%llu cross_font=%llu disk_hit=%llu miss=%llu write=%llu read_bytes=%llu write_bytes=%llu raster=%llu bitmap_batch_requests=%llu deduped=%llu prepared_text_hit=%llu miss=%llu promotion_bypass=%llu probe=%llu probe_miss=%llu admitted=%llu evicted=%llu admission_rejected=%llu backoff_bypass=%llu atlas_hit=%llu create=%llu grow=%llu uploads=%llu bytes=%llu upload_rects=%llu text_artifact_hit=%llu miss=%llu hot=%llu bypass=%llu admitted=%llu evicted=%llu shader_batches=%llu cpu_effect_masks_avoided=%llu gpu_resident_glyph_hit=%llu miss=%llu atlas_snapshot_profile_reuse=%llu dynamic_vb_uploads=%llu bytes=%llu reuse=%llu discards=%llu static_vb_uploads=%llu bytes=%llu hits=%llu promotion_failed=%llu sorted_static_batches=%llu payloads=%llu bytes=%llu merged_packet_ranges=%llu metadata_hot=%llu locked=%llu sorted_facades=%llu unique_payloads=%llu frame_lookup_hits=%llu preflight_fast=%llu full=%llu direct_static=%llu direct_dynamic=%llu sorted_dynamic_batches=%llu payloads=%llu bytes=%llu lockless_packets=%llu visibility_checks=%llu culled=%llu app=%llu alpha=%llu clip=%llu scissor=%llu preflight_skipped=%llu packets_saved=%llu vertices_saved=%llu viewport_nodes=%llu install_failed=%llu viewport_checks=%llu viewport_culled=%llu fail_open=%llu direct_shape_candidates=%llu direct_shape_draws=%llu direct_shape_vertices=%llu direct_shape_fallback=%llu constant_ownership_segments=%llu reuses=%llu releases=%llu snapshot_gets_elided=%llu restore_sets_elided=%llu stock_constant_updates=%llu reuses=%llu composite_constant_full=%llu c0_only=%llu partial=%llu sampler_sets=%llu reuses=%llu composite_onequad_single_page=%llu onequad_paged=%llu onequad_build_fallback=%llu legacy_multipage_fallback=%llu shader_fallback=%llu composite_draws=%llu tile_passes=%llu cache_hit=%llu miss=%llu state_changes=%llu generated=%llu evicted=%llu cache_bytes=%llu budget_reject=%llu rtt_fail=%llu restore_fail=%llu visual_validated=%llu rejected=%llu inconclusive=%llu",
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
				FreeTypePerfCounter::VisibilityPreflightSkipped)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VisibilityPacketsSaved)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::VisibilityVerticesSaved)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportNodeInstalled)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportNodeInstallFailed)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportCullCheck)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportCulled)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ViewportFailOpen)],
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
			values[static_cast<size_t>(FreeTypePerfCounter::StockConstantUpdate)],
			values[static_cast<size_t>(FreeTypePerfCounter::StockConstantReuse)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CompositeConstantFullUpload)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CompositeConstantC0Only)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::CompositeConstantPartialUpload)],
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
			"tnvse_freetype_perf: constant_capture_mirror=%llu driver=%llu state_shadow_driver_gets=%llu isolation_bypass=%llu vertex_aa_sets=%llu reuses=%llu vertex_aa_stock_preserved=%llu command_program_setups=%llu binds_elided=%llu texture_sets=%llu reuses=%llu command_packet_constant_full=%llu partial=%llu reuses=%llu",
			values[static_cast<size_t>(
				FreeTypePerfCounter::ConstantCaptureMirror)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ConstantCaptureDriver)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::StateShadowDriverGet)],
			values[static_cast<size_t>(
				FreeTypePerfCounter::ConstantIsolationBypass)],
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
				FreeTypePerfCounter::CommandPacketConstantReuse)]);
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
			"tnvse_freetype_perf: virtual_stock_candidates=%llu virtual_stock_groups=%llu virtual_stock_shapes=%llu virtual_stock_draws=%llu virtual_stock_static_hits=%llu virtual_stock_dynamic_hits=%llu virtual_stock_rebinds=%llu virtual_stock_revokes=%llu facade_fallbacks=%llu followers_skipped=%llu sorted_preflight_saved=%llu proxy_packets_saved=%llu fallback_no_parent=%llu packet_limit=%llu cpu_budget=%llu static_not_ready=%llu topology=%llu shader=%llu generation=%llu atlas=%llu resource=%llu noncontiguous=%llu registration_resolved=%llu register_rejected=%llu registration_missing=%llu registration_duplicate=%llu registration_order_mismatch=%llu",
			values[static_cast<size_t>(
				FreeTypePerfCounter::VirtualStockCandidate)],
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
			"tnvse_freetype_perf: command_recorded=%llu single_packet_commands=%llu single_packet_build_fallbacks=%llu single_packet_hits=%llu single_packet_misses=%llu single_packet_replays=%llu single_packet_fallbacks=%llu spans=%llu packets=%llu span_hits=%llu span_misses=%llu retained_bridge_draws=%llu native_replays=%llu stock_bootstraps_saved=%llu virtual_spans_fused=%llu followers_consumed=%llu direct_range_replays=%llu span_full_validations=%llu light_validations=%llu packet_epoch_guards=%llu packet_state_elisions=%llu packet_range_validations=%llu packet_range_packets=%llu render_target_validations=%llu execution_segments=%llu segment_full_validations=%llu segment_validation_reuses=%llu segment_invalidations=%llu retained_program_hits=%llu retained_program_misses=%llu fallback_token=%llu generation=%llu atlas=%llu resource=%llu topology=%llu hook=%llu nested=%llu render_target=%llu state=%llu",
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
			values[static_cast<size_t>(
				FreeTypePerfCounter::CommandSpanFullValidation)],
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
			"tnvse_freetype_perf: standard_pass_lite_candidates=%llu retained_builds=%llu retained_reuses=%llu retained_hits=%llu retained_misses=%llu stage1_eligible=%llu stage2_resident=%llu stage3_replays=%llu stock_fallbacks=%llu fallback_envelope=%llu program=%llu renderer=%llu geometry=%llu binding=%llu prelude=%llu",
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
		const DurationSummary layout =
			ConsumeDurationSummary(FreeTypePerfPhase::Layout);
		const DurationSummary sidecar =
			ConsumeDurationSummary(FreeTypePerfPhase::Sidecar);
		const DurationSummary directCompile =
			ConsumeDurationSummary(FreeTypePerfPhase::DirectCompile);
		const DurationSummary nativeRegistration =
			ConsumeDurationSummary(
				FreeTypePerfPhase::NativeRegistration);
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

	void RecordFreeTypeViewportCullResult(bool culled, bool failOpen)
	{
		vectorfont::RecordFreeTypePerf(
			vectorfont::FreeTypePerfCounter::ViewportCullCheck);
		if (culled)
		{
			vectorfont::RecordFreeTypePerf(
				vectorfont::FreeTypePerfCounter::ViewportCulled);
		}
		if (failOpen)
		{
			vectorfont::RecordFreeTypePerf(
				vectorfont::FreeTypePerfCounter::ViewportFailOpen);
		}
	}

	void PumpFreeTypeFontPerformance()
	{
		vectorfont::ReportFreeTypePerf();
	}
}

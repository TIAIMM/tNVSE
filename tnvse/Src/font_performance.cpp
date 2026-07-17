#include "font_vector_internal.h"

#include "load_config.h"

#include <array>
#include <atomic>

namespace fonthook::vectorfont
{
	namespace
	{
		constexpr size_t kCounterCount = static_cast<size_t>(FreeTypePerfCounter::Count);
		std::array<std::atomic<UInt64>, kCounterCount> s_counters = {};
		ULONGLONG s_lastReport = 0;
	}

	void RecordFreeTypePerf(FreeTypePerfCounter counter, UInt64 amount)
	{
		if (g_bEnableFreeTypeFontRenderingLog)
			s_counters[static_cast<size_t>(counter)].fetch_add(amount, std::memory_order_relaxed);
	}

	void ReportFreeTypePerf()
	{
		if (!g_bEnableFreeTypeFontRenderingLog)
			return;
		const ULONGLONG now = GetTickCount64();
		if (s_lastReport && now - s_lastReport < 10000)
			return;
		s_lastReport = now;
		std::array<UInt64, kCounterCount> values = {};
		for (size_t i = 0; i < values.size(); ++i)
			values[i] = s_counters[i].exchange(0, std::memory_order_relaxed);
		FreeTypeFontDebugLog(
			"tnvse_freetype_perf: layout_hit=%llu miss=%llu hb=%llu bitmap_mem=%llu cross_font=%llu disk_hit=%llu miss=%llu write=%llu read_bytes=%llu write_bytes=%llu raster=%llu bitmap_batch_requests=%llu deduped=%llu prepared_text_hit=%llu miss=%llu atlas_hit=%llu create=%llu grow=%llu uploads=%llu bytes=%llu upload_rects=%llu batch_hit=%llu miss=%llu packet_template_hit=%llu miss=%llu shader_batches=%llu passes=%llu estimated_samples=%llu cpu_effect_masks_avoided=%llu gpu_resident_glyph_hit=%llu miss=%llu atlas_snapshot_profile_reuse=%llu",
			values[static_cast<size_t>(FreeTypePerfCounter::LayoutHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::LayoutMiss)],
			values[static_cast<size_t>(FreeTypePerfCounter::HarfBuzzShape)],
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
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasCreated)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasGrown)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasUpload)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasUploadBytes)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasUploadRect)],
			values[static_cast<size_t>(FreeTypePerfCounter::BatchHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::BatchMiss)],
			values[static_cast<size_t>(FreeTypePerfCounter::PacketTemplateHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::PacketTemplateMiss)],
			values[static_cast<size_t>(FreeTypePerfCounter::ShaderEffectBatch)],
			values[static_cast<size_t>(FreeTypePerfCounter::ShaderEffectPass)],
			values[static_cast<size_t>(FreeTypePerfCounter::ShaderEffectSamples)],
			values[static_cast<size_t>(FreeTypePerfCounter::CpuEffectMasksAvoided)],
			values[static_cast<size_t>(FreeTypePerfCounter::GpuResidentGlyphHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::GpuResidentGlyphMiss)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasSnapshotProfileReuse)]);
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

	void PumpFreeTypeFontPerformance()
	{
		vectorfont::ReportFreeTypePerf();
	}
}

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
			"tnvse_freetype_perf: layout_hit=%llu miss=%llu hb=%llu kerning_hit=%llu miss=%llu bitmap_mem=%llu raster=%llu atlas_hit=%llu create=%llu grow=%llu uploads=%llu bytes=%llu batch_hit=%llu miss=%llu",
			values[static_cast<size_t>(FreeTypePerfCounter::LayoutHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::LayoutMiss)],
			values[static_cast<size_t>(FreeTypePerfCounter::HarfBuzzShape)],
			values[static_cast<size_t>(FreeTypePerfCounter::KerningHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::KerningMiss)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapMemoryHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::BitmapRasterized)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasCreated)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasGrown)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasUpload)],
			values[static_cast<size_t>(FreeTypePerfCounter::AtlasUploadBytes)],
			values[static_cast<size_t>(FreeTypePerfCounter::BatchHit)],
			values[static_cast<size_t>(FreeTypePerfCounter::BatchMiss)]);
	}
}

namespace fonthook
{
	void PumpFreeTypeFontPerformance()
	{
		vectorfont::ReportFreeTypePerf();
	}
}

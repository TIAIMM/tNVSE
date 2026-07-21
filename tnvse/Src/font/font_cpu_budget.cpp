#include "font_cpu_budget.h"

#include "font_vector.h"
#include "load_config.h"

#include <array>
#include <atomic>
#include <limits>

namespace fonthook::vectorfont
{
	void TrimFreeTypeCpuCachesForTotalBudget();
	void TrimAtlasCpuCachesForTotalBudget();
	void TrimPreparedTextCpuCacheForTotalBudget();
	void TrimNativeA8CpuCachesForTotalBudget();

	namespace
	{
		struct CpuBudgetState
		{
			std::array<std::atomic<std::size_t>,
				static_cast<std::size_t>(CpuMemoryCategory::Count)> bytes = {};
			std::atomic<std::size_t> lastLoggedTotal = 0;
		};

		CpuBudgetState& BudgetState()
		{
			// Leases can be released during CRT/static teardown. Keep counters alive
			// until process termination so destructor order cannot access dead state.
			static CpuBudgetState* state = new CpuBudgetState();
			return *state;
		}

		std::size_t SaturatingAdd(std::size_t left, std::size_t right)
		{
			return left <= std::numeric_limits<std::size_t>::max() - right
				? left + right : std::numeric_limits<std::size_t>::max();
		}
	}

	void AddCpuMemoryUsage(CpuMemoryCategory category, std::size_t bytes)
	{
		if (!bytes || category >= CpuMemoryCategory::Count)
			return;
		auto& value = BudgetState().bytes[static_cast<std::size_t>(category)];
		std::size_t current = value.load(std::memory_order_relaxed);
		for (;;)
		{
			const std::size_t desired = SaturatingAdd(current, bytes);
			if (value.compare_exchange_weak(current, desired,
				std::memory_order_relaxed, std::memory_order_relaxed))
			{
				break;
			}
		}
	}

	void RemoveCpuMemoryUsage(CpuMemoryCategory category, std::size_t bytes)
	{
		if (!bytes || category >= CpuMemoryCategory::Count)
			return;
		auto& value = BudgetState().bytes[static_cast<std::size_t>(category)];
		std::size_t current = value.load(std::memory_order_relaxed);
		for (;;)
		{
			const std::size_t desired = bytes <= current ? current - bytes : 0;
			if (value.compare_exchange_weak(current, desired,
				std::memory_order_relaxed, std::memory_order_relaxed))
			{
				break;
			}
		}
	}

	std::size_t GetCpuMemoryUsage(CpuMemoryCategory category)
	{
		return category < CpuMemoryCategory::Count
			? BudgetState().bytes[static_cast<std::size_t>(category)].load(
				std::memory_order_relaxed) : 0;
	}

	std::size_t GetCpuMemoryUsage()
	{
		std::size_t total = 0;
		for (std::size_t index = 0;
			index < static_cast<std::size_t>(CpuMemoryCategory::Count); ++index)
		{
			total = SaturatingAdd(total,
				BudgetState().bytes[index].load(std::memory_order_relaxed));
		}
		return total;
	}

	std::size_t GetCpuMemoryBudget()
	{
		constexpr std::size_t bytesPerMiB = 1024u * 1024u;
		const std::size_t configured =
			static_cast<std::size_t>(g_uiFreeTypeFontMemoryCacheMB);
		return configured <= std::numeric_limits<std::size_t>::max() / bytesPerMiB
			? configured * bytesPerMiB
			: std::numeric_limits<std::size_t>::max();
	}

	std::size_t GetCpuMemoryCategoryHeadroom(CpuMemoryCategory category,
		std::size_t preferredLimit)
	{
		return ResolveCpuMemoryCategoryHeadroom(GetCpuMemoryBudget(),
			GetCpuMemoryUsage(), GetCpuMemoryUsage(category), preferredLimit);
	}

	bool IsCpuMemoryBudgetExceeded()
	{
		return GetCpuMemoryUsage() > GetCpuMemoryBudget();
	}

	void EnforceCpuMemoryBudget(const char* phase)
	{
		static std::atomic_flag enforcing = ATOMIC_FLAG_INIT;
		if (!IsCpuMemoryBudgetExceeded()
			|| enforcing.test_and_set(std::memory_order_acquire))
		{
			return;
		}

		// Cheapest/reconstructible data first. A live shared object remains
		// accounted after its cache entry is removed, so enforcement continues
		// until the real aggregate is below the configured limit or all reclaimable
		// entries are gone.
		TrimPreparedTextCpuCacheForTotalBudget();
		TrimNativeA8CpuCachesForTotalBudget();
		TrimAtlasCpuCachesForTotalBudget();
		TrimFreeTypeCpuCachesForTotalBudget();
		enforcing.clear(std::memory_order_release);
		ReportCpuMemoryBudget(phase, true);
	}

	void ReportCpuMemoryBudget(const char* phase, bool force)
	{
		const std::size_t total = GetCpuMemoryUsage();
		const std::size_t budget = GetCpuMemoryBudget();
		auto& last = BudgetState().lastLoggedTotal;
		const std::size_t previous = last.load(std::memory_order_relaxed);
		const std::size_t quantum = 4u * 1024u * 1024u;
		if (!force && total <= budget
			&& total / quantum == previous / quantum)
		{
			return;
		}
		last.store(total, std::memory_order_relaxed);
		FreeTypeFontDebugLog(
			"tnvse_freetype_font: CPU budget phase=%s totalMiB=%.2f limitMiB=%.2f result=%s bitmap=%.2f prepared=%.2f textArtifact=%.2f atlasMeta=%.2f mappings=%.2f runtime=%.2f",
			phase ? phase : "unknown", total / (1024.0 * 1024.0),
			budget / (1024.0 * 1024.0),
			total <= budget ? "within-budget" : "pinned-overcommit",
			GetCpuMemoryUsage(CpuMemoryCategory::GlyphBitmap) / (1024.0 * 1024.0),
			GetCpuMemoryUsage(CpuMemoryCategory::PreparedText) / (1024.0 * 1024.0),
			GetCpuMemoryUsage(CpuMemoryCategory::TextArtifact) / (1024.0 * 1024.0),
			GetCpuMemoryUsage(CpuMemoryCategory::AtlasMetadata) / (1024.0 * 1024.0),
			GetCpuMemoryUsage(CpuMemoryCategory::PersistentMapping) / (1024.0 * 1024.0),
			GetCpuMemoryUsage(CpuMemoryCategory::RuntimeMetadata) / (1024.0 * 1024.0));
	}
}

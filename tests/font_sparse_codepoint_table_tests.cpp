#include "font_sparse_codepoint_table.h"
#include "font_cpu_budget.h"
#include "font_render_route.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <utility>

namespace fonthook::vectorfont
{
	namespace
	{
		std::array<std::size_t,
			static_cast<std::size_t>(CpuMemoryCategory::Count)> leaseTestUsage = {};
	}

	void AddCpuMemoryUsage(CpuMemoryCategory category, std::size_t bytes)
	{
		leaseTestUsage[static_cast<std::size_t>(category)] += bytes;
	}

	void RemoveCpuMemoryUsage(CpuMemoryCategory category, std::size_t bytes)
	{
		auto& usage = leaseTestUsage[static_cast<std::size_t>(category)];
		usage = bytes <= usage ? usage - bytes : 0;
	}

	std::size_t GetLeaseTestUsage(CpuMemoryCategory category)
	{
		return leaseTestUsage[static_cast<std::size_t>(category)];
	}
}

namespace
{
	int failures = 0;

	void Check(bool condition, const char* message)
	{
		if (condition)
			return;
		std::cerr << "FAILED: " << message << '\n';
		++failures;
	}
}

int main()
{
	using Table = fonthook::vectorfont::SparseCodePointTable<std::uint32_t>;
	using fonthook::vectorfont::FontAtlasRoute;
	using fonthook::vectorfont::ResolveCpuMemoryCategoryHeadroom;
	using fonthook::vectorfont::ResolveFontAtlasRoute;

	Check(ResolveFontAtlasRoute(true) == FontAtlasRoute::ShaderMtsdf,
		"Shader Loader route always selects MTSDF");
	Check(ResolveFontAtlasRoute(false) == FontAtlasRoute::ArgbFallback,
		"missing Shader Loader route selects only the ARGB fallback");

	Check(ResolveCpuMemoryCategoryHeadroom(192, 160, 64, 48) == 48,
		"category keeps its preferred limit while aggregate headroom permits it");
	Check(ResolveCpuMemoryCategoryHeadroom(192, 220, 80, 64) == 52,
		"category limit shrinks to the aggregate budget headroom");
	Check(ResolveCpuMemoryCategoryHeadroom(192, 256, 32, 64) == 0,
		"other categories can consume all available headroom");
	Check(ResolveCpuMemoryCategoryHeadroom(192, 64, 96, 80) == 80,
		"defensive inconsistent counters do not underflow");

	using fonthook::vectorfont::CpuMemoryCategory;
	using fonthook::vectorfont::CpuMemoryLease;
	using fonthook::vectorfont::GetLeaseTestUsage;
	{
		CpuMemoryLease first(CpuMemoryCategory::GlyphBitmap, 64);
		Check(GetLeaseTestUsage(CpuMemoryCategory::GlyphBitmap) == 64,
			"lease construction accounts owned bytes");
		CpuMemoryLease moved(std::move(first));
		Check(first.GetBytes() == 0 && moved.GetBytes() == 64
			&& GetLeaseTestUsage(CpuMemoryCategory::GlyphBitmap) == 64,
			"lease move transfers ownership without double accounting");
		moved.Reset(CpuMemoryCategory::PreparedText, 32);
		Check(GetLeaseTestUsage(CpuMemoryCategory::GlyphBitmap) == 0
			&& GetLeaseTestUsage(CpuMemoryCategory::PreparedText) == 32,
			"lease reset releases the previous category before reacquiring");
	}
	Check(GetLeaseTestUsage(CpuMemoryCategory::PreparedText) == 0,
		"lease destruction releases the final owned bytes");

	Table table;
	Check(table.GetAllocatedPageCount() == 0, "table starts without allocated pages");
	Check(table.GetAllocatedBytes() == 0, "table starts without allocated bytes");

	auto* first = table.GetOrCreate(0x8140);
	Check(first != nullptr, "first page allocation succeeds");
	Check(first && *first == Table::kUnset,
		"new entries use the unset sentinel");
	Check(table.GetAllocatedPageCount() == 1, "first lead byte allocates one page");
	Check(table.GetAllocatedBytes() == 256u * sizeof(std::uint32_t),
		"one page accounts for 256 code points");

	if (first)
		*first = 0x4E00;
	Check(table.GetOrCreate(0x8140) == first && first && *first == 0x4E00,
		"cached values remain stable");
	Check(table.GetOrCreate(0x81FE) != nullptr
		&& table.GetAllocatedPageCount() == 1,
		"entries with the same lead byte share a page");
	Check(table.GetOrCreate(0x8240) != nullptr
		&& table.GetAllocatedPageCount() == 2,
		"a different lead byte allocates a second page");

	table.Clear();
	Check(table.GetAllocatedPageCount() == 0, "clear releases every page");
	Check(table.GetAllocatedBytes() == 0, "clear resets byte accounting");
	auto* afterClear = table.GetOrCreate(0x8140);
	Check(afterClear && *afterClear == Table::kUnset,
		"entries are reset after a code-page change");

	if (failures)
		return 1;
	std::cout << "font_sparse_codepoint_table_tests: passed\n";
	return 0;
}

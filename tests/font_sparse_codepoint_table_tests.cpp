#include "font_sparse_codepoint_table.h"

#include <cstdint>
#include <iostream>

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

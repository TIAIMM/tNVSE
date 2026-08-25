#pragma once

namespace fonthook::vectorfont::implementation::font_atlas_cache
{
	// The LRU is supplied oldest-first. A stale list node has no cache entry and
	// is returned so the caller can clean it without touching budget accounting.
	// Live sealed-profile entries are skipped entirely; they are fixed resident
	// cost, not members of the budget-managed cache.
	template <class Iterator, class ResolveEntry, class IsProtected>
	Iterator FindOldestAtlasBudgetVictim(Iterator oldest, Iterator end,
		ResolveEntry&& resolveEntry, IsProtected&& isProtected)
	{
		for (Iterator candidate = oldest; candidate != end; ++candidate)
		{
			auto* entry = resolveEntry(*candidate);
			if (!entry || !isProtected(*candidate, *entry))
				return candidate;
		}
		return end;
	}
}

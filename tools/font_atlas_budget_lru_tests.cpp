#include "font_atlas_budget_lru_policy.h"

#include <iostream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
	using fonthook::vectorfont::implementation::font_atlas_cache::
		FindOldestAtlasBudgetVictim;

	struct Entry
	{
		bool protectedBySealedProfile = false;
	};

	int s_failures = 0;

	void Expect(bool condition, std::string_view message)
	{
		if (condition)
			return;
		std::cerr << "FAIL " << message << '\n';
		++s_failures;
	}
}

int main()
{
	std::vector<int> oldestFirst{ 1, 2, 3, 4 };
	std::unordered_map<int, Entry> entries{
		{ 1, { true } },
		{ 2, { true } },
		{ 3, { false } },
		{ 4, { false } },
	};
	auto resolve = [&](int key) -> Entry*
	{
		const auto entry = entries.find(key);
		return entry != entries.end() ? &entry->second : nullptr;
	};
	auto isProtected = [](int, const Entry& entry)
	{
		return entry.protectedBySealedProfile;
	};

	auto victim = FindOldestAtlasBudgetVictim(oldestFirst.begin(),
		oldestFirst.end(), resolve, isProtected);
	Expect(victim != oldestFirst.end() && *victim == 3,
		"oldest live sealed entries must be skipped in favor of LRU cache data");

	entries[3].protectedBySealedProfile = true;
	entries[4].protectedBySealedProfile = true;
	victim = FindOldestAtlasBudgetVictim(oldestFirst.begin(),
		oldestFirst.end(), resolve, isProtected);
	Expect(victim == oldestFirst.end(),
		"an all-sealed cache must expose no budget eviction victim");

	entries.erase(1);
	victim = FindOldestAtlasBudgetVictim(oldestFirst.begin(),
		oldestFirst.end(), resolve, isProtected);
	Expect(victim != oldestFirst.end() && *victim == 1,
		"a stale LRU node must remain removable without touching sealed data");

	entries.emplace(1, Entry{ true });
	entries[2].protectedBySealedProfile = false;
	victim = FindOldestAtlasBudgetVictim(oldestFirst.begin(),
		oldestFirst.end(), resolve, isProtected);
	Expect(victim != oldestFirst.end() && *victim == 2,
		"resources that cease to be sealed must re-enter ordinary LRU order");

	if (s_failures)
	{
		std::cerr << s_failures << " font atlas budget LRU test(s) failed\n";
		return 1;
	}
	std::cout << "font atlas budget LRU tests passed\n";
	return 0;
}

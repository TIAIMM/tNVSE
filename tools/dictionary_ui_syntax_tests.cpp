#include "dictionary_ui_syntax.h"

#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>

namespace
{
	using fonthook::dictionary_ui_syntax::Kind;
	using fonthook::dictionary_ui_syntax::Match;

	int s_failures = 0;

	void Fail(std::string_view source, std::string_view reason)
	{
		std::cerr << "FAIL source=\"" << source << "\" " << reason << '\n';
		++s_failures;
	}

	void Expect(
		std::string_view source,
		Kind expectedKind,
		std::initializer_list<std::string_view> expectedSlots)
	{
		Match match;
		if (!fonthook::dictionary_ui_syntax::TryParse(source, match))
		{
			Fail(source, "did not parse");
			return;
		}
		if (match.kind != expectedKind)
		{
			Fail(source, "parsed with the wrong syntax kind");
			return;
		}
		if (match.slotCount != expectedSlots.size())
		{
			Fail(source, "returned the wrong slot count");
			return;
		}

		size_t index = 0;
		for (std::string_view expected : expectedSlots)
		{
			const auto& slot = match.slots[index++];
			if (slot.begin > slot.end || slot.end > source.size() ||
				source.substr(slot.begin, slot.end - slot.begin) != expected)
			{
				Fail(source, "returned the wrong slot boundaries");
				return;
			}
		}
	}

	void ExpectNoMatch(std::string_view source)
	{
		Match match;
		if (fonthook::dictionary_ui_syntax::TryParse(source, match))
			Fail(source, "unexpectedly parsed");
	}

	void ExpectNonRecursive(std::string_view source, Kind expectedKind)
	{
		Match match;
		if (!fonthook::dictionary_ui_syntax::TryParse(source, match) ||
			match.kind != expectedKind ||
			match.allowGeneralRecursiveSlotTranslation ||
			match.fallbackSlotCount != 0)
		{
			Fail(source, "did not retain the non-recursive syntax policy");
		}
	}

	void ExpectFallback(
		std::string_view source,
		Kind expectedKind,
		std::string_view expectedPrimary,
		std::initializer_list<std::string_view> expectedFallbackSlots)
	{
		Match match;
		if (!fonthook::dictionary_ui_syntax::TryParse(source, match))
		{
			Fail(source, "did not parse");
			return;
		}
		if (match.kind != expectedKind || match.slotCount != 1 ||
			source.substr(match.slots[0].begin,
				match.slots[0].end - match.slots[0].begin) != expectedPrimary)
		{
			Fail(source, "returned the wrong primary wrapper slot");
			return;
		}
		if (!match.allowGeneralRecursiveSlotTranslation ||
			match.fallbackSlotCount != expectedFallbackSlots.size())
		{
			Fail(source, "returned the wrong wrapper fallback policy");
			return;
		}

		size_t index = 0;
		for (std::string_view expected : expectedFallbackSlots)
		{
			const auto& slot = match.fallbackSlots[index++];
			if (source.substr(slot.begin, slot.end - slot.begin) != expected)
			{
				Fail(source, "returned the wrong wrapper fallback boundaries");
				return;
			}
		}
	}

	void ExpectImprovingFallback(
		std::string_view source,
		Kind expectedKind,
		std::initializer_list<std::string_view> expectedPrimarySlots,
		std::initializer_list<std::string_view> expectedFallbackSlots)
	{
		Match match;
		if (!fonthook::dictionary_ui_syntax::TryParse(source, match) ||
			match.kind != expectedKind ||
			match.slotCount != expectedPrimarySlots.size() ||
			match.fallbackSlotCount != expectedFallbackSlots.size() ||
			!match.preferFallbackWhenItTranslatesMoreSlots)
		{
			Fail(source, "returned the wrong improving-fallback policy");
			return;
		}

		size_t index = 0;
		for (std::string_view expected : expectedPrimarySlots)
		{
			const auto& slot = match.slots[index++];
			if (source.substr(slot.begin, slot.end - slot.begin) != expected)
			{
				Fail(source, "returned the wrong primary alternative");
				return;
			}
		}
		index = 0;
		for (std::string_view expected : expectedFallbackSlots)
		{
			const auto& slot = match.fallbackSlots[index++];
			if (source.substr(slot.begin, slot.end - slot.begin) != expected)
			{
				Fail(source, "returned the wrong fallback alternative");
				return;
			}
		}
	}
}

int main()
{
	const std::string cp1252Core =
		std::string("Fire Ant Fricass") + static_cast<char>(0xE9) + "e";
	const std::string cp1252Count = cp1252Core + " (2)";
	Expect(cp1252Count,
		Kind::SuffixParenthesizedCount, { cp1252Core });

	Expect("  Fire Ant Fricassee (12)  ",
		Kind::SuffixParenthesizedCount, { "Fire Ant Fricassee" });
	Expect("Fire Ant Fricassee (2/3)",
		Kind::SuffixParenthesizedRatio, { "Fire Ant Fricassee" });
	Expect("Laser Rifle+ (4)",
		Kind::SuffixPlusParenthesizedCount, { "Laser Rifle" });
	Expect("Recipe [75%]",
		Kind::SuffixBracketedPercent, { "Recipe" });
	Expect("(12) Buffout",
		Kind::PrefixParenthesizedCount, { "Buffout" });
	Expect("Sleep +2",
		Kind::SuffixSignedValue, { "Sleep" });
	Expect("Caps   123",
		Kind::SuffixSpacedCount, { "Caps" });
	Expect("Boone  143/210",
		Kind::SuffixSpacedRatio, { "Boone" });
	Expect("Caps   123+",
		Kind::SuffixSpacedCountPlus, { "Caps" });
	Expect("Speech 1/2",
		Kind::SuffixSpacedRatio, { "Speech" });
	Expect("Item, 7",
		Kind::SuffixCommaCount, { "Item" });
	Expect("Radio New Vegas, 12:05",
		Kind::SuffixClock, { "Radio New Vegas" });
	Expect("10:05:3 PM",
		Kind::PrefixClock, { "PM" });
	Expect("--:--:- AM",
		Kind::PrefixClock, { "AM" });
	Expect("00:00:0 PM",
		Kind::PrefixClock, { "PM" });
	Expect("Sunday, October, 12:05 PM",
		Kind::CommaClock, { "Sunday", "October", "PM" });
	Expect("1. Hair Style",
		Kind::NumberedPrefix, { "Hair Style" });
	Expect("1 Save",
		Kind::PrefixSpacedCount, { "Save" });
	Expect("Courier Earnings: ",
		Kind::SuffixEarningsLabel, { "Courier", "Earnings" });
	Expect("(Hidden Valley)",
		Kind::WholeParenthesized, { "Hidden Valley" });
	ExpectNonRecursive("(Hidden Valley)", Kind::WholeParenthesized);
	Expect("Race >",
		Kind::SuffixGreaterThanMarker, { "Race" });
	Expect("Head - Condition 75",
		Kind::StatusPairValue, { "Head", "Condition" });
	Expect("Skill 2/3 Remaining",
		Kind::InfixRatio, { "Skill", "Remaining" });
	Expect("Brightness: High",
		Kind::ColonPair, { "Brightness", "High" });
	Expect("Difficulty 3: Normal",
		Kind::ColonPair, { "Difficulty 3", "Normal" });
	ExpectImprovingFallback("Difficulty 3: Normal", Kind::ColonPair,
		{ "Difficulty 3", "Normal" }, { "Difficulty", "Normal" });
	ExpectFallback("[Barter - Speech]",
		Kind::WholeSquareWrapper, "Barter - Speech", { "Barter", "Speech" });
	ExpectFallback("<<Robot craft kit:Mister Boltac>>",
		Kind::WholeAngleWrapper, "Robot craft kit:Mister Boltac",
		{ "Robot craft kit", "Mister Boltac" });
	Expect("[Speech, 50% 1/2]  I'm ready.",
		Kind::BracketedPrompt, { "Speech", "I'm ready." });
	const std::string cp1252SmartBody =
		std::string("I") + static_cast<char>(0x92) + "m ready.";
	const std::string cp1252SmartPrompt =
		"[Speech, 50% 1/2]  " + cp1252SmartBody;
	Expect(cp1252SmartPrompt,
		Kind::BracketedPrompt, { "Speech", cp1252SmartBody });

	ExpectNoMatch("Fire Ant (a/b)");
	ExpectNoMatch("Fire Ant (12345678901)");
	ExpectNoMatch("Fire Ant [75]");
	ExpectNoMatch("Chapter 123");
	ExpectNoMatch("12345678901 Save");
	ExpectNoMatch("Plan A");
	ExpectNoMatch("http://example.com");
	ExpectNoMatch("Label:Value");
	ExpectNoMatch("[Speech, 50%] I'm ready.");
	ExpectNoMatch("Line one\nItem (2)");
	ExpectNoMatch("(123)");
	const std::string tooLong = "1 " + std::string(2047, 'A');
	ExpectNoMatch(tooLong);

	if (s_failures != 0)
	{
		std::cerr << s_failures << " dictionary UI syntax test(s) failed\n";
		return 1;
	}
	std::cout << "dictionary UI syntax tests passed\n";
	return 0;
}

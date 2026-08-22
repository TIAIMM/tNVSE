#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace fonthook::dictionary_ui_syntax
{
	// Byte-preserving grammar for retail UI composites observed after the
	// engine's formatting calls and before Tile::SetString.  A match exposes
	// only dictionary-backed text spans; punctuation, counts and spacing remain
	// in the original byte stream.
	enum class Kind
	{
		None,
		// Existing visible-wrapper compatibility forms plus retail "(%s)".
		WholeAngleWrapper,
		WholeSquareWrapper,
		WholeParenthesized,
		// Multiple independently translatable fields.
		BracketedPrompt,
		InfixRatio,
		StatusPairValue,
		// Suffix/prefix numeric decorations.
		SuffixParenthesizedCount,
		SuffixParenthesizedRatio,
		SuffixPlusParenthesizedCount,
		SuffixBracketedPercent,
		PrefixParenthesizedCount,
		SuffixSignedValue,
		SuffixSpacedCount,
		SuffixSpacedRatio,
		SuffixSpacedCountPlus,
		SuffixCommaCount,
		// Clock/date composites.
		SuffixClock,
		CommaClock,
		PrefixClock,
		// Numbered labels and fixed punctuation forms.
		NumberedPrefix,
		PrefixSpacedCount,
		SuffixEarningsLabel,
		SuffixGreaterThanMarker,
		ColonPair,
	};
	inline constexpr size_t kMaximumSlots = 16;

	struct Span
	{
		size_t begin = 0;
		size_t end = 0;
	};

	struct Match
	{
		Kind kind = Kind::None;
		std::array<Span, kMaximumSlots> slots = {};
		size_t slotCount = 0;
		std::array<Span, kMaximumSlots> fallbackSlots = {};
		size_t fallbackSlotCount = 0;
		bool allowGeneralRecursiveSlotTranslation = false;
		bool preferFallbackWhenItTranslatesMoreSlots = false;
	};

	const char* KindName(Kind kind) noexcept;
	bool TryParse(std::string_view source, Match& match) noexcept;
}

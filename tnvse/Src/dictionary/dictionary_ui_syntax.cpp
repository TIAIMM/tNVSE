#include "dictionary_ui_syntax.h"

#include <algorithm>

namespace fonthook::dictionary_ui_syntax
{
	namespace
	{
		constexpr size_t kMaximumSourceBytes = 2048;
		constexpr size_t kMaximumNumericDigits = 10;

		bool IsInlineWhitespace(char ch) noexcept
		{
			return ch == ' ' || ch == '\t';
		}

		bool IsAsciiDigit(char ch) noexcept
		{
			return ch >= '0' && ch <= '9';
		}

		bool IsAsciiAlpha(char ch) noexcept
		{
			return (ch >= 'a' && ch <= 'z') ||
				(ch >= 'A' && ch <= 'Z');
		}

		bool HasAsciiAlphabet(
			std::string_view source, size_t begin, size_t end) noexcept
		{
			if (begin >= end || end > source.size())
				return false;
			return std::any_of(
				source.begin() + begin, source.begin() + end,
				[](char ch) { return IsAsciiAlpha(ch); });
		}

		void TrimInline(
			std::string_view source, size_t& begin, size_t& end) noexcept
		{
			while (begin < end && IsInlineWhitespace(source[begin]))
				++begin;
			while (end > begin && IsInlineWhitespace(source[end - 1]))
				--end;
		}

		bool SetOne(
			std::string_view source, Match& match, Kind kind,
			size_t begin, size_t end) noexcept
		{
			TrimInline(source, begin, end);
			if (!HasAsciiAlphabet(source, begin, end))
				return false;
			match = Match{};
			match.kind = kind;
			match.slots[0] = { begin, end };
			match.slotCount = 1;
			return true;
		}

		bool SetTwo(
			std::string_view source, Match& match, Kind kind,
			size_t firstBegin, size_t firstEnd,
			size_t secondBegin, size_t secondEnd) noexcept
		{
			TrimInline(source, firstBegin, firstEnd);
			TrimInline(source, secondBegin, secondEnd);
			if (!HasAsciiAlphabet(source, firstBegin, firstEnd) ||
				!HasAsciiAlphabet(source, secondBegin, secondEnd) ||
				firstEnd > secondBegin)
			{
				return false;
			}
			match = Match{};
			match.kind = kind;
			match.slots[0] = { firstBegin, firstEnd };
			match.slots[1] = { secondBegin, secondEnd };
			match.slotCount = 2;
			return true;
		}

		bool SetThree(
			std::string_view source, Match& match, Kind kind,
			size_t firstBegin, size_t firstEnd,
			size_t secondBegin, size_t secondEnd,
			size_t thirdBegin, size_t thirdEnd) noexcept
		{
			TrimInline(source, firstBegin, firstEnd);
			TrimInline(source, secondBegin, secondEnd);
			TrimInline(source, thirdBegin, thirdEnd);
			if (!HasAsciiAlphabet(source, firstBegin, firstEnd) ||
				!HasAsciiAlphabet(source, secondBegin, secondEnd) ||
				!HasAsciiAlphabet(source, thirdBegin, thirdEnd) ||
				firstEnd > secondBegin || secondEnd > thirdBegin)
			{
				return false;
			}
			match = Match{};
			match.kind = kind;
			match.slots[0] = { firstBegin, firstEnd };
			match.slots[1] = { secondBegin, secondEnd };
			match.slots[2] = { thirdBegin, thirdEnd };
			match.slotCount = 3;
			return true;
		}

		bool IsWholeWrapper(
			std::string_view source, size_t begin, size_t end,
			size_t& coreBegin, size_t& coreEnd,
			Kind& kind) noexcept
		{
			if (end - begin < 3)
				return false;
			const char open = source[begin];
			char close = 0;
			switch (open)
			{
			case '<': close = '>'; kind = Kind::WholeAngleWrapper; break;
			case '[': close = ']'; kind = Kind::WholeSquareWrapper; break;
			case '(': close = ')'; kind = Kind::WholeParenthesized; break;
			default: return false;
			}
			if (source[end - 1] != close)
				return false;

			size_t nesting = 0;
			for (size_t pos = begin; pos < end; ++pos)
			{
				if (source[pos] == open)
				{
					++nesting;
				}
				else if (source[pos] == close)
				{
					if (nesting == 0)
						return false;
					--nesting;
					if (nesting == 0 && pos + 1 != end)
						return false;
				}
			}
			if (nesting != 0)
				return false;
			coreBegin = begin + 1;
			coreEnd = end - 1;
			return coreBegin < coreEnd;
		}

		void AddFallbackSlot(
			std::string_view source, Match& match,
			size_t begin, size_t end) noexcept
		{
			TrimInline(source, begin, end);
			if (match.fallbackSlotCount >= match.fallbackSlots.size() ||
				!HasAsciiAlphabet(source, begin, end))
			{
				return;
			}
			match.fallbackSlots[match.fallbackSlotCount++] = { begin, end };
		}

		void BuildWrapperFallbackSlots(
			std::string_view source, Match& match,
			size_t begin, size_t end) noexcept
		{
			const size_t firstColon = source.find(':', begin);
			if (firstColon != std::string_view::npos && firstColon < end)
			{
				size_t cursor = begin;
				while (cursor <= end && match.fallbackSlotCount <
					match.fallbackSlots.size())
				{
					const size_t separator = source.find(':', cursor);
					const size_t partEnd = separator == std::string_view::npos ||
						separator >= end ? end : separator;
					AddFallbackSlot(source, match, cursor, partEnd);
					if (partEnd == end)
						break;
					cursor = separator + 1;
				}
			}
			else
			{
				size_t cursor = begin;
				while (cursor < end && match.fallbackSlotCount <
					match.fallbackSlots.size())
				{
					const size_t separator = source.find(" - ", cursor);
					if (separator == std::string_view::npos || separator >= end)
					{
						AddFallbackSlot(source, match, cursor, end);
						break;
					}
					AddFallbackSlot(source, match, cursor, separator);
					cursor = separator + 3;
				}
			}

			if (match.fallbackSlotCount < 2)
				match.fallbackSlotCount = 0;
		}

		bool TryParseWholeWrapper(
			std::string_view source, size_t begin, size_t end,
			Match& match) noexcept
		{
			size_t coreBegin = 0;
			size_t coreEnd = 0;
			Kind outerKind = Kind::None;
			if (!IsWholeWrapper(
				source, begin, end, coreBegin, coreEnd, outerKind))
			{
				return false;
			}

			// Retail and mods use doubled wrappers such as <<name>>. Peel every
			// complete wrapper layer while retaining absolute source offsets, so
			// replacement changes only the visible innermost text.
			for (;;)
			{
				size_t nestedBegin = 0;
				size_t nestedEnd = 0;
				Kind nestedKind = Kind::None;
				if (!IsWholeWrapper(source, coreBegin, coreEnd,
					nestedBegin, nestedEnd, nestedKind))
				{
					break;
				}
				coreBegin = nestedBegin;
				coreEnd = nestedEnd;
			}

			if (!SetOne(source, match, outerKind, coreBegin, coreEnd))
				return false;
			// Angle/square wrappers are the two legacy visible-wrapper forms and
			// retain their recursive/component fallback behavior.  Retail's
			// "(%s)" map label is new coverage and stays non-recursive so arbitrary
			// parenthesized prose is not routed through fuzzy/semantic matching.
			if (outerKind == Kind::WholeAngleWrapper ||
				outerKind == Kind::WholeSquareWrapper)
			{
				match.allowGeneralRecursiveSlotTranslation = true;
				BuildWrapperFallbackSlots(source, match, coreBegin, coreEnd);
			}
			return true;
		}

		bool ConsumeUnsignedForward(
			std::string_view source, size_t& pos, size_t end) noexcept
		{
			const size_t begin = pos;
			while (pos < end && IsAsciiDigit(source[pos]))
				++pos;
			const size_t digits = pos - begin;
			return digits != 0 && digits <= kMaximumNumericDigits;
		}

		bool ConsumeUnsignedBackward(
			std::string_view source, size_t begin, size_t& pos) noexcept
		{
			const size_t numberEnd = pos;
			while (pos > begin && IsAsciiDigit(source[pos - 1]))
				--pos;
			const size_t digits = numberEnd - pos;
			return digits != 0 && digits <= kMaximumNumericDigits;
		}

		bool TryParseBracketedPrompt(
			std::string_view source, size_t begin, size_t end,
			Match& match) noexcept
		{
			if (source[begin] != '[')
				return false;
			const size_t close = source.find(']', begin + 1);
			if (close == std::string_view::npos || close + 1 >= end)
				return false;

			size_t bodyBegin = close + 1;
			const size_t whitespaceBegin = bodyBegin;
			while (bodyBegin < end && IsInlineWhitespace(source[bodyBegin]))
				++bodyBegin;
			// DialogMenu uses "%s]  %s". Requiring the two-space boundary keeps
			// ordinary bracketed prose out of this global parser.
			if (bodyBegin - whitespaceBegin < 2 || bodyBegin == end)
				return false;

			size_t metadataBegin = begin + 1;
			size_t metadataEnd = close;
			const size_t comma = source.find(',', metadataBegin);
			if (comma != std::string_view::npos && comma < metadataEnd)
				metadataEnd = comma;
			return SetTwo(source, match, Kind::BracketedPrompt,
				metadataBegin, metadataEnd, bodyBegin, end);
		}

		bool TryParseStatusPairValue(
			std::string_view source, size_t begin, size_t end,
			Match& match) noexcept
		{
			size_t pos = end;
			if (!ConsumeUnsignedBackward(source, begin, pos))
				return false;
			if (pos > begin && (source[pos - 1] == '+' || source[pos - 1] == '-'))
				--pos;
			const size_t whitespaceEnd = pos;
			while (pos > begin && IsInlineWhitespace(source[pos - 1]))
				--pos;
			if (pos == whitespaceEnd)
				return false;

			const size_t separator = source.rfind(" - ", pos);
			if (separator == std::string_view::npos || separator <= begin ||
				separator + 3 >= pos)
			{
				return false;
			}
			return SetTwo(source, match, Kind::StatusPairValue,
				begin, separator, separator + 3, pos);
		}

		bool TryParseInfixRatio(
			std::string_view source, size_t begin, size_t end,
			Match& match) noexcept
		{
			for (size_t separator = begin + 1; separator + 4 < end; ++separator)
			{
				if (!IsInlineWhitespace(source[separator]))
					continue;
				size_t pos = separator;
				while (pos < end && IsInlineWhitespace(source[pos]))
					++pos;
				if (!ConsumeUnsignedForward(source, pos, end) ||
					pos >= end || source[pos++] != '/')
				{
					continue;
				}
				if (!ConsumeUnsignedForward(source, pos, end) ||
					pos >= end || !IsInlineWhitespace(source[pos]))
				{
					continue;
				}
				size_t rightBegin = pos;
				while (rightBegin < end && IsInlineWhitespace(source[rightBegin]))
					++rightBegin;
				if (rightBegin == end)
					continue;
				if (SetTwo(source, match, Kind::InfixRatio,
					begin, separator, rightBegin, end))
				{
					return true;
				}
			}
			return false;
		}

		bool TryParseSuffixParenthesizedNumber(
			std::string_view source, size_t begin, size_t end,
			Match& match) noexcept
		{
			if (source[end - 1] != ')')
				return false;
			size_t pos = end - 1;
			if (!ConsumeUnsignedBackward(source, begin, pos))
				return false;
			bool ratio = false;
			if (pos > begin && source[pos - 1] == '/')
			{
				ratio = true;
				--pos;
				if (!ConsumeUnsignedBackward(source, begin, pos))
					return false;
			}
			if (pos <= begin || source[pos - 1] != '(')
				return false;
			const size_t open = pos - 1;
			size_t whitespaceBegin = open;
			while (whitespaceBegin > begin &&
				IsInlineWhitespace(source[whitespaceBegin - 1]))
			{
				--whitespaceBegin;
			}
			if (whitespaceBegin == open || whitespaceBegin == begin)
				return false;

			size_t coreEnd = whitespaceBegin;
			Kind kind = ratio ? Kind::SuffixParenthesizedRatio :
				Kind::SuffixParenthesizedCount;
			if (!ratio && coreEnd > begin && source[coreEnd - 1] == '+')
			{
				--coreEnd;
				kind = Kind::SuffixPlusParenthesizedCount;
			}
			return SetOne(source, match, kind, begin, coreEnd);
		}

		bool TryParseSuffixBracketedPercent(
			std::string_view source, size_t begin, size_t end,
			Match& match) noexcept
		{
			if (end - begin < 6 || source[end - 1] != ']' ||
				source[end - 2] != '%')
			{
				return false;
			}
			size_t pos = end - 2;
			if (!ConsumeUnsignedBackward(source, begin, pos) ||
				pos <= begin || source[pos - 1] != '[')
			{
				return false;
			}
			const size_t open = pos - 1;
			size_t whitespaceBegin = open;
			while (whitespaceBegin > begin &&
				IsInlineWhitespace(source[whitespaceBegin - 1]))
			{
				--whitespaceBegin;
			}
			if (whitespaceBegin == open || whitespaceBegin == begin)
				return false;
			return SetOne(source, match, Kind::SuffixBracketedPercent,
				begin, whitespaceBegin);
		}

		bool TryParsePrefixParenthesizedCount(
			std::string_view source, size_t begin, size_t end,
			Match& match) noexcept
		{
			if (source[begin] != '(')
				return false;
			size_t pos = begin + 1;
			if (!ConsumeUnsignedForward(source, pos, end) ||
				pos >= end || source[pos++] != ')')
			{
				return false;
			}
			const size_t whitespaceBegin = pos;
			while (pos < end && IsInlineWhitespace(source[pos]))
				++pos;
			if (pos == whitespaceBegin || pos == end)
				return false;
			return SetOne(source, match, Kind::PrefixParenthesizedCount,
				pos, end);
		}

		bool TryParseSuffixSignedValue(
			std::string_view source, size_t begin, size_t end,
			Match& match) noexcept
		{
			size_t pos = end;
			if (!ConsumeUnsignedBackward(source, begin, pos) ||
				pos <= begin || (source[pos - 1] != '+' && source[pos - 1] != '-'))
			{
				return false;
			}
			--pos;
			const size_t whitespaceEnd = pos;
			while (pos > begin && IsInlineWhitespace(source[pos - 1]))
				--pos;
			if (pos == whitespaceEnd || pos == begin)
				return false;
			return SetOne(source, match, Kind::SuffixSignedValue, begin, pos);
		}

		bool TryParseSuffixSpacedNumber(
			std::string_view source, size_t begin, size_t end,
			Match& match) noexcept
		{
			size_t pos = end;
			bool plus = false;
			if (pos > begin && source[pos - 1] == '+')
			{
				plus = true;
				--pos;
			}
			if (!ConsumeUnsignedBackward(source, begin, pos))
				return false;
			bool ratio = false;
			if (pos > begin && source[pos - 1] == '/')
			{
				if (plus)
					return false;
				ratio = true;
				--pos;
				if (!ConsumeUnsignedBackward(source, begin, pos))
					return false;
			}

			const size_t whitespaceEnd = pos;
			while (pos > begin && IsInlineWhitespace(source[pos - 1]))
				--pos;
			const size_t whitespaceCount = whitespaceEnd - pos;
			if (pos == begin || whitespaceCount == 0 ||
				(!ratio && whitespaceCount < 2))
			{
				return false;
			}
			const Kind kind = ratio ? Kind::SuffixSpacedRatio :
				(plus ? Kind::SuffixSpacedCountPlus : Kind::SuffixSpacedCount);
			return SetOne(source, match, kind, begin, pos);
		}

		bool TryParseSuffixClockOrCommaCount(
			std::string_view source, size_t begin, size_t end,
			Match& match) noexcept
		{
			size_t pos = end;
			if (!ConsumeUnsignedBackward(source, begin, pos))
				return false;

			bool clock = false;
			if (pos > begin && source[pos - 1] == ':')
			{
				clock = true;
				--pos;
				if (!ConsumeUnsignedBackward(source, begin, pos))
					return false;
			}
			const size_t whitespaceEnd = pos;
			while (pos > begin && IsInlineWhitespace(source[pos - 1]))
				--pos;
			if (pos == whitespaceEnd || pos <= begin || source[pos - 1] != ',')
				return false;
			return SetOne(source, match,
				clock ? Kind::SuffixClock : Kind::SuffixCommaCount,
				begin, pos - 1);
		}

		bool TryParsePrefixClock(
			std::string_view source, size_t begin, size_t end,
			Match& match) noexcept
		{
			size_t pos = begin;
			if (source.substr(begin, 7) == "--:--:-" ||
				source.substr(begin, 7) == "00:00:0")
			{
				pos += 7;
			}
			else
			{
				for (int part = 0; part < 3; ++part)
				{
					if (!ConsumeUnsignedForward(source, pos, end))
						return false;
					if (part != 2)
					{
						if (pos >= end || source[pos++] != ':')
							return false;
					}
				}
			}
			const size_t whitespaceBegin = pos;
			while (pos < end && IsInlineWhitespace(source[pos]))
				++pos;
			if (pos == whitespaceBegin || pos == end)
				return false;
			return SetOne(source, match, Kind::PrefixClock, pos, end);
		}

		bool TryParseCommaClock(
			std::string_view source, size_t begin, size_t end,
			Match& match) noexcept
		{
			const size_t firstSeparator = source.find(", ", begin);
			if (firstSeparator == std::string_view::npos ||
				firstSeparator + 2 >= end)
			{
				return false;
			}
			const size_t secondSeparator =
				source.find(", ", firstSeparator + 2);
			if (secondSeparator == std::string_view::npos ||
				secondSeparator + 2 >= end)
			{
				return false;
			}

			size_t pos = secondSeparator + 2;
			if (!ConsumeUnsignedForward(source, pos, end) ||
				pos >= end || source[pos++] != ':' ||
				!ConsumeUnsignedForward(source, pos, end))
			{
				return false;
			}
			const size_t whitespaceBegin = pos;
			while (pos < end && IsInlineWhitespace(source[pos]))
				++pos;
			if (pos == whitespaceBegin || pos == end)
				return false;

			return SetThree(source, match, Kind::CommaClock,
				begin, firstSeparator,
				firstSeparator + 2, secondSeparator,
				pos, end);
		}

		bool TryParseNumberedPrefix(
			std::string_view source, size_t begin, size_t end,
			Match& match) noexcept
		{
			size_t pos = begin;
			if (!ConsumeUnsignedForward(source, pos, end) ||
				pos >= end || source[pos++] != '.')
			{
				return false;
			}
			const size_t whitespaceBegin = pos;
			while (pos < end && IsInlineWhitespace(source[pos]))
				++pos;
			if (pos == whitespaceBegin || pos == end)
				return false;
			return SetOne(source, match, Kind::NumberedPrefix, pos, end);
		}

		bool TryParsePrefixSpacedCount(
			std::string_view source, size_t begin, size_t end,
			Match& match) noexcept
		{
			size_t pos = begin;
			if (!ConsumeUnsignedForward(source, pos, end))
				return false;
			const size_t whitespaceBegin = pos;
			while (pos < end && IsInlineWhitespace(source[pos]))
				++pos;
			if (pos == whitespaceBegin || pos == end)
				return false;
			return SetOne(source, match, Kind::PrefixSpacedCount, pos, end);
		}

		bool TryParseSuffixEarningsLabel(
			std::string_view source, size_t begin, size_t end,
			Match& match) noexcept
		{
			static constexpr std::string_view kSuffix = " Earnings:";
			if (end - begin <= kSuffix.size() ||
				source.substr(end - kSuffix.size(), kSuffix.size()) != kSuffix)
			{
				return false;
			}
			const size_t suffixBegin = end - kSuffix.size();
			const size_t wordBegin = suffixBegin + 1;
			return SetTwo(source, match, Kind::SuffixEarningsLabel,
				begin, suffixBegin, wordBegin, end - 1);
		}

		bool TryParseSuffixGreaterThanMarker(
			std::string_view source, size_t begin, size_t end,
			Match& match) noexcept
		{
			// Both retail xrefs to "%s %c" pass 0x3E.  Do not generalize the
			// otherwise ambiguous shape to arbitrary one-character suffixes.
			if (end - begin < 3 || source[end - 1] != '>')
				return false;
			size_t pos = end - 1;
			const size_t whitespaceEnd = pos;
			while (pos > begin && IsInlineWhitespace(source[pos - 1]))
				--pos;
			if (pos == whitespaceEnd || pos == begin)
				return false;
			return SetOne(source, match, Kind::SuffixGreaterThanMarker,
				begin, pos);
		}

		bool TryParseColonPair(
			std::string_view source, size_t begin, size_t end,
			Match& match) noexcept
		{
			const size_t separator = source.find(':', begin);
			if (separator == std::string_view::npos || separator == begin ||
				separator + 1 >= end ||
				!IsInlineWhitespace(source[separator + 1]))
			{
				return false;
			}
			size_t rightBegin = separator + 1;
			while (rightBegin < end && IsInlineWhitespace(source[rightBegin]))
				++rightBegin;
			if (rightBegin == end)
				return false;
			if (!SetTwo(source, match, Kind::ColonPair,
				begin, separator, rightBegin, end))
			{
				return false;
			}

			// SaveLoadMenu also emits "%s %d%s: %s".  Prefer an exact match
			// for the complete left field (for example "Vault 21"), but expose
			// the label before a trailing count as a fallback when the complete
			// field has no dictionary entry.
			size_t leftEnd = separator;
			while (leftEnd > begin && IsInlineWhitespace(source[leftEnd - 1]))
				--leftEnd;
			size_t countBegin = leftEnd;
			if (ConsumeUnsignedBackward(source, begin, countBegin))
			{
				size_t labelEnd = countBegin;
				while (labelEnd > begin &&
					IsInlineWhitespace(source[labelEnd - 1]))
				{
					--labelEnd;
				}
				if (labelEnd < countBegin &&
					HasAsciiAlphabet(source, begin, labelEnd))
				{
					match.fallbackSlots[0] = { begin, labelEnd };
					match.fallbackSlots[1] = { rightBegin, end };
					match.fallbackSlotCount = 2;
					match.preferFallbackWhenItTranslatesMoreSlots = true;
				}
			}
			return true;
		}
	}

	const char* KindName(Kind kind) noexcept
	{
		switch (kind)
		{
		case Kind::BracketedPrompt: return "bracketed-prompt";
		case Kind::WholeAngleWrapper: return "whole-angle-wrapper";
		case Kind::WholeSquareWrapper: return "whole-square-wrapper";
		case Kind::WholeParenthesized: return "whole-parenthesized";
		case Kind::InfixRatio: return "infix-ratio";
		case Kind::StatusPairValue: return "status-pair-value";
		case Kind::SuffixParenthesizedCount: return "suffix-parenthesized-count";
		case Kind::SuffixParenthesizedRatio: return "suffix-parenthesized-ratio";
		case Kind::SuffixPlusParenthesizedCount: return "suffix-plus-parenthesized-count";
		case Kind::SuffixBracketedPercent: return "suffix-bracketed-percent";
		case Kind::PrefixParenthesizedCount: return "prefix-parenthesized-count";
		case Kind::SuffixSignedValue: return "suffix-signed-value";
		case Kind::SuffixSpacedCount: return "suffix-spaced-count";
		case Kind::SuffixSpacedRatio: return "suffix-spaced-ratio";
		case Kind::SuffixSpacedCountPlus: return "suffix-spaced-count-plus";
		case Kind::SuffixCommaCount: return "suffix-comma-count";
		case Kind::SuffixClock: return "suffix-clock";
		case Kind::CommaClock: return "comma-clock";
		case Kind::PrefixClock: return "prefix-clock";
		case Kind::NumberedPrefix: return "numbered-prefix";
		case Kind::PrefixSpacedCount: return "prefix-spaced-count";
		case Kind::SuffixEarningsLabel: return "suffix-earnings-label";
		case Kind::SuffixGreaterThanMarker: return "suffix-greater-than-marker";
		case Kind::ColonPair: return "colon-pair";
		default: return "none";
		}
	}

	bool TryParse(std::string_view source, Match& match) noexcept
	{
		match = Match{};
		if (source.empty() || source.size() > kMaximumSourceBytes ||
			source.find_first_of("\r\n") != std::string_view::npos)
		{
			return false;
		}

		size_t begin = 0;
		size_t end = source.size();
		TrimInline(source, begin, end);
		if (begin == end)
			return false;

		return
			TryParseWholeWrapper(source, begin, end, match) ||
			TryParseBracketedPrompt(source, begin, end, match) ||
			TryParseStatusPairValue(source, begin, end, match) ||
			TryParseInfixRatio(source, begin, end, match) ||
			TryParseSuffixParenthesizedNumber(source, begin, end, match) ||
			TryParseSuffixBracketedPercent(source, begin, end, match) ||
			TryParsePrefixParenthesizedCount(source, begin, end, match) ||
			TryParseSuffixSignedValue(source, begin, end, match) ||
			TryParseSuffixSpacedNumber(source, begin, end, match) ||
			TryParseSuffixClockOrCommaCount(source, begin, end, match) ||
			TryParsePrefixClock(source, begin, end, match) ||
			TryParseCommaClock(source, begin, end, match) ||
			TryParseNumberedPrefix(source, begin, end, match) ||
			TryParsePrefixSpacedCount(source, begin, end, match) ||
			TryParseSuffixEarningsLabel(source, begin, end, match) ||
			TryParseSuffixGreaterThanMarker(source, begin, end, match) ||
			TryParseColonPair(source, begin, end, match);
	}
}

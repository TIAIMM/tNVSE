#include "dictionary_internal.h"
#include "load_config.h"

namespace fonthook
{
	namespace implementation::dictionary_effects {}
	using namespace implementation::dictionary_effects;

	namespace implementation::dictionary_effects
	{
		struct EffectSegment
		{
			std::string name;
			std::string suffix;
			bool numeric = false;
		};

		struct RawEffectSegment
		{
			std::string leadingWhitespace;
			std::string text;
			std::string trailingWhitespace;
			std::string delimiter;
		};

		struct EffectSegmentLog
		{
			std::string source;
			std::string name;
			std::string suffix;
			std::string method;
			std::string result;
		};

		struct MultiplierTextParts
		{
			std::string left;
			std::string number;
			std::string right;
		};

		bool IsAsciiDigit(char ch)
		{
			return ch >= '0' && ch <= '9';
		}

		char ToLowerAsciiChar(char ch)
		{
			if (ch >= 'A' && ch <= 'Z')
				return static_cast<char>(ch + ('a' - 'A'));
			return ch;
		}

		bool StartsWithIgnoreCase(const std::string& text, std::string_view pattern)
		{
			if (text.size() < pattern.size())
				return false;
			for (size_t i = 0; i < pattern.size(); ++i)
			{
				if (ToLowerAsciiChar(text[i]) != ToLowerAsciiChar(pattern[i]))
					return false;
			}
			return true;
		}

		bool IsInlineWhitespace(char ch)
		{
			return ch == ' ' || ch == '\t';
		}

		bool IsRegexWhitespace(char ch)
		{
			return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\v' || ch == '\f';
		}

		bool HasLineBreak(const std::string& source)
		{
			return source.find('\r') != std::string::npos || source.find('\n') != std::string::npos;
		}

		bool SplitOuterWhitespace(
			const std::string& source,
			std::string& leadingWhitespace,
			std::string& core,
			std::string& trailingWhitespace)
		{
			size_t coreBegin = 0;
			while (coreBegin < source.size())
			{
				const char ch = source[coreBegin];
				if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
					break;
				++coreBegin;
			}

			size_t coreEnd = source.size();
			while (coreEnd > coreBegin)
			{
				const char ch = source[coreEnd - 1];
				if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
					break;
				--coreEnd;
			}

			if (coreBegin == coreEnd)
				return false;

			leadingWhitespace = source.substr(0, coreBegin);
			core = source.substr(coreBegin, coreEnd - coreBegin);
			trailingWhitespace = source.substr(coreEnd);
			return true;
		}

		bool BuildRawEffectSegment(const std::string& rawSegment, std::string delimiter, RawEffectSegment& segment)
		{
			size_t textBegin = 0;
			while (textBegin < rawSegment.size() && IsInlineWhitespace(rawSegment[textBegin]))
				++textBegin;

			size_t textEnd = rawSegment.size();
			while (textEnd > textBegin && IsInlineWhitespace(rawSegment[textEnd - 1]))
				--textEnd;

			if (textBegin == textEnd)
				return false;

			segment.leadingWhitespace = rawSegment.substr(0, textBegin);
			segment.text = rawSegment.substr(textBegin, textEnd - textBegin);
			segment.trailingWhitespace = rawSegment.substr(textEnd);
			segment.delimiter = std::move(delimiter);
			return true;
		}

		std::vector<RawEffectSegment> SplitCommaEffectSegments(const std::string& source)
		{
			std::vector<RawEffectSegment> segments;
			size_t cursor = 0;
			while (cursor <= source.size())
			{
				const size_t comma = source.find(',', cursor);
				const bool hasComma = comma != std::string::npos;
				const std::string rawSegment = hasComma
					? source.substr(cursor, comma - cursor)
					: source.substr(cursor);

				RawEffectSegment segment;
				if (!BuildRawEffectSegment(rawSegment, hasComma ? "," : "", segment))
					return {};
				segments.push_back(std::move(segment));

				if (!hasComma)
					break;
				cursor = comma + 1;
			}
			return segments;
		}

		std::vector<RawEffectSegment> SplitLineEffectSegments(const std::string& source)
		{
			std::vector<RawEffectSegment> segments;
			size_t cursor = 0;
			while (cursor < source.size())
			{
				size_t lineEnd = cursor;
				while (lineEnd < source.size() && source[lineEnd] != '\r' && source[lineEnd] != '\n')
					++lineEnd;

				std::string delimiter;
				if (lineEnd < source.size())
				{
					if (source[lineEnd] == '\r' && lineEnd + 1 < source.size() && source[lineEnd + 1] == '\n')
					{
						delimiter = "\r\n";
					}
					else
					{
						delimiter.assign(1, source[lineEnd]);
					}
				}

				const std::string rawSegment = source.substr(cursor, lineEnd - cursor);
				RawEffectSegment segment;
				if (!BuildRawEffectSegment(rawSegment, delimiter, segment))
					return {};
				segments.push_back(std::move(segment));

				if (lineEnd == source.size())
					break;
				cursor = lineEnd + segments.back().delimiter.size();
			}
			return segments;
		}

		bool IsValidDurationBody(std::string_view body)
		{
			size_t pos = 0;
			while (pos < body.size() && IsAsciiDigit(body[pos]))
				++pos;
			if (pos == 0)
				return false;

			if (pos < body.size() && (body[pos] == 'x' || body[pos] == 'X'))
			{
				++pos;
				const size_t secondNumber = pos;
				while (pos < body.size() && IsAsciiDigit(body[pos]))
					++pos;
				if (pos == secondNumber)
					return false;
			}

			if (pos + 1 != body.size())
				return false;
			const char unit = ToLowerAsciiChar(body[pos]);
			return unit == 's' || unit == 'm';
		}

		bool TrySplitDuration(const std::string& segment, size_t& coreEnd, std::string& duration)
		{
			coreEnd = segment.size();
			duration.clear();
			if (segment.empty() || segment.back() != ')')
				return false;

			const size_t open = segment.rfind('(');
			if (open == std::string::npos || open + 2 >= segment.size())
				return false;
			if (!IsValidDurationBody(std::string_view(segment).substr(open + 1, segment.size() - open - 2)))
				return false;

			size_t durationBegin = open;
			while (durationBegin > 0)
			{
				const char ch = segment[durationBegin - 1];
				if (ch != ' ' && ch != '\t')
					break;
				--durationBegin;
			}

			coreEnd = durationBegin;
			duration = segment.substr(durationBegin);
			return true;
		}

		bool TrySplitNumericValue(const std::string& segment, size_t coreEnd, size_t& valueBegin)
		{
			valueBegin = coreEnd;
			if (valueBegin == 0)
				return false;

			if (segment[valueBegin - 1] == '%')
			{
				--valueBegin;
				if (valueBegin == 0)
					return false;
			}

			const size_t fractionalEnd = valueBegin;
			while (valueBegin > 0 && IsAsciiDigit(segment[valueBegin - 1]))
				--valueBegin;
			const bool hasFractionalDigits = valueBegin < fractionalEnd;

			if (valueBegin > 0 && segment[valueBegin - 1] == '.')
			{
				--valueBegin;
				const size_t integerEnd = valueBegin;
				while (valueBegin > 0 && IsAsciiDigit(segment[valueBegin - 1]))
					--valueBegin;
				const bool hasIntegerDigits = valueBegin < integerEnd;
				return hasIntegerDigits && hasFractionalDigits;
			}

			return hasFractionalDigits;
		}

		bool IsEffectOperator(char ch)
		{
			return ch == '+' || ch == '-' || ch == 'x' || ch == 'X' || ch == '|';
		}

		bool TryParseNumericEffectSegment(const std::string& segment, EffectSegment& parsed)
		{
			parsed = EffectSegment{};
			size_t coreEnd = segment.size();
			std::string duration;
			TrySplitDuration(segment, coreEnd, duration);

			while (coreEnd > 0 && (segment[coreEnd - 1] == ' ' || segment[coreEnd - 1] == '\t'))
				--coreEnd;
			if (coreEnd == 0)
				return false;

			size_t valueBegin = 0;
			if (!TrySplitNumericValue(segment, coreEnd, valueBegin))
				return false;

			size_t operatorEnd = valueBegin;
			while (operatorEnd > 0 && IsInlineWhitespace(segment[operatorEnd - 1]))
				--operatorEnd;
			if (operatorEnd == 0)
				return false;

			const char sign = segment[operatorEnd - 1];
			if (!IsEffectOperator(sign))
				return false;
			if (operatorEnd < 2)
				return false;
			const char beforeSign = segment[operatorEnd - 2];
			if (!IsInlineWhitespace(beforeSign))
				return false;

			size_t nameEnd = operatorEnd - 1;
			while (nameEnd > 0 && IsInlineWhitespace(segment[nameEnd - 1]))
				--nameEnd;
			if (nameEnd == 0)
				return false;

			parsed.name = segment.substr(0, nameEnd);
			parsed.suffix = segment.substr(nameEnd, coreEnd - nameEnd) + duration;
			parsed.numeric = true;
			return true;
		}

		bool TranslateEffectName(const std::string& name, std::string& translated, int depth)
		{
			translated.clear();
			if (name.empty() || depth >= 4)
				return false;
			return TranslateInternal(name.c_str(), translated, depth + 1);
		}

		bool TranslateEffectNameExact(const std::string& name, std::string& translated, int depth)
		{
			translated.clear();
			if (name.empty() || depth >= 4)
				return false;
			return TryTranslateExactText(name, translated, depth + 1);
		}

		bool TranslateCommaSeparatedEffectName(const std::string& name, std::string& translated, int depth)
		{
			translated.clear();
			if (name.find(',') == std::string::npos)
				return false;

			const std::vector<RawEffectSegment> parts = SplitCommaEffectSegments(name);
			if (parts.size() < 2)
				return false;

			std::string result;
			bool changed = false;
			for (const RawEffectSegment& part : parts)
			{
				std::string translatedPart;
				if (TranslateEffectName(part.text, translatedPart, depth) &&
					translatedPart != part.text)
				{
					changed = true;
				}
				else
				{
					translatedPart = part.text;
				}

				result += part.leadingWhitespace;
				result += translatedPart;
				result += part.trailingWhitespace;
				result += part.delimiter;
			}

			if (!changed)
				return false;

			translated = std::move(result);
			return true;
		}

		bool TranslateEffectSegmentText(const std::string& text, std::string& translated, int depth)
		{
			translated.clear();
			if (text.empty() || depth >= 4)
				return false;
			return TranslateInternal(text.c_str(), translated, depth + 1);
		}

		bool TrySplitMultiplierText(const std::string& source, MultiplierTextParts& parts)
		{
			parts = MultiplierTextParts{};
			if (source.size() < 7)
				return false;

			for (size_t xPos = source.size(); xPos > 0;)
			{
				--xPos;
				if (source[xPos] != 'x')
					continue;
				if (xPos < 2 || xPos + 2 >= source.size())
					continue;
				if (!IsRegexWhitespace(source[xPos - 1]) || !IsRegexWhitespace(source[xPos + 1]))
					continue;

				const size_t leftEnd = xPos - 1;
				if (leftEnd == 0 || IsRegexWhitespace(source[leftEnd - 1]))
					continue;

				size_t numberBegin = xPos + 2;
				if (numberBegin >= source.size() || !IsAsciiDigit(source[numberBegin]))
					continue;

				size_t numberEnd = numberBegin + 1;
				while (numberEnd < source.size() && IsAsciiDigit(source[numberEnd]))
					++numberEnd;

				if (numberEnd >= source.size() || !IsRegexWhitespace(source[numberEnd]))
					continue;

				const size_t rightBegin = numberEnd + 1;
				if (rightBegin >= source.size())
					continue;

				parts.left = source.substr(0, leftEnd);
				parts.number = source.substr(numberBegin, numberEnd - numberBegin);
				parts.right = source.substr(rightBegin);
				return true;
			}

			return false;
		}

		bool TranslateMultiplierPart(const std::string& source, std::string& translated, int depth)
		{
			translated.clear();
			if (source.empty() || depth >= 4)
				return false;
			return TranslateInternal(source.c_str(), translated, depth + 1);
		}
	}

	bool TryTranslateItemEffectList(const std::string& source, std::string& translated, int depth)
	{
		if (depth >= 4 || source.empty())
			return false;

		std::string leadingWhitespace;
		std::string coreSource;
		std::string trailingWhitespace;
		if (!SplitOuterWhitespace(source, leadingWhitespace, coreSource, trailingWhitespace) ||
			StartsWithIgnoreCase(coreSource, "Permanent"))
			return false;

		const bool lineSeparated = HasLineBreak(coreSource);
		const std::vector<RawEffectSegment> rawSegments = lineSeparated
			? SplitLineEffectSegments(coreSource)
			: SplitCommaEffectSegments(coreSource);
		if (rawSegments.empty())
			return false;

		std::vector<EffectSegment> segments;
		segments.reserve(rawSegments.size());
		bool hasNumericSegment = false;
		for (const RawEffectSegment& rawSegment : rawSegments)
		{
			EffectSegment segment;
			if (TryParseNumericEffectSegment(rawSegment.text, segment))
			{
				hasNumericSegment = true;
			}
			else
			{
				segment.name = rawSegment.text;
			}

			if (!HasAlphabet(segment.name))
				return false;
			segments.push_back(std::move(segment));
		}

		if (!hasNumericSegment)
			return false;

		std::string result = leadingWhitespace;
		bool changed = false;
		std::vector<EffectSegmentLog> segmentLogs;
		if (g_bEnableDictionaryTranslationLog)
			segmentLogs.reserve(segments.size());

		for (size_t i = 0; i < segments.size(); ++i)
		{
			const RawEffectSegment& rawSegment = rawSegments[i];
			const EffectSegment& segment = segments[i];
			std::string translatedSegmentText;
			const char* method = "none";

			std::string translatedName;
			bool translatedNameFound = false;
			if (segment.numeric)
			{
				if (TranslateEffectNameExact(segment.name, translatedName, depth) &&
					translatedName != segment.name)
				{
					method = "name-exact";
					translatedNameFound = true;
				}
				else if (TranslateCommaSeparatedEffectName(segment.name, translatedName, depth) &&
					translatedName != segment.name)
				{
					method = "name-parts";
					translatedNameFound = true;
				}
				else if (TranslateEffectName(segment.name, translatedName, depth) &&
					translatedName != segment.name)
				{
					method = "name";
					translatedNameFound = true;
				}
			}
			if (translatedNameFound)
			{
				translatedSegmentText = translatedName + segment.suffix;
				changed = true;
			}
			else if (TranslateEffectSegmentText(rawSegment.text, translatedSegmentText, depth))
			{
				method = "segment";
				if (translatedSegmentText != rawSegment.text)
					changed = true;
			}
			else
			{
				if (translatedName.empty())
					translatedName = segment.name;
				else
					method = "name";

				translatedSegmentText = translatedName + segment.suffix;
			}

			if (g_bEnableDictionaryTranslationLog)
			{
				EffectSegmentLog log;
				log.source = rawSegment.text;
				log.name = segment.name;
				log.suffix = segment.suffix;
				log.method = method;
				log.result = translatedSegmentText;
				segmentLogs.push_back(std::move(log));
			}

			result += rawSegment.leadingWhitespace;
			result += translatedSegmentText;
			result += rawSegment.trailingWhitespace;
			result += rawSegment.delimiter;
		}

		if (!changed)
			return false;

		result += trailingWhitespace;
		translated = std::move(result);
		if (g_bEnableDictionaryTranslationLog)
		{
			gLog.FormattedMessage("tnvse_dictionary: item effect %s match:",
				lineSeparated ? "line-list" : "comma-list");
			gLog.FormattedMessage("tnvse_dictionary:   source=\"%s\"", source.c_str());
			gLog.FormattedMessage("tnvse_dictionary:   result=\"%s\"", translated.c_str());
			for (size_t i = 0; i < segmentLogs.size(); ++i)
			{
				const EffectSegmentLog& log = segmentLogs[i];
				gLog.FormattedMessage("tnvse_dictionary:   %s %u source=\"%s\"",
					lineSeparated ? "line" : "segment",
					static_cast<UInt32>(i + 1),
					log.source.c_str());
				gLog.FormattedMessage("tnvse_dictionary:     name=\"%s\" suffix=\"%s\"",
					log.name.c_str(), log.suffix.c_str());
				gLog.FormattedMessage("tnvse_dictionary:     method=%s result=\"%s\"",
					log.method.c_str(), log.result.c_str());
			}
		}
		return true;
	}

	bool TryTranslateMultiplierText(const std::string& source, std::string& translated, int depth)
	{
		if (depth >= 4 || source.empty())
			return false;

		MultiplierTextParts parts;
		if (!TrySplitMultiplierText(source, parts))
			return false;

		std::string translatedLeft;
		std::string translatedRight;
		const bool leftFound = TranslateMultiplierPart(parts.left, translatedLeft, depth) &&
			translatedLeft != parts.left;
		const bool rightFound = TranslateMultiplierPart(parts.right, translatedRight, depth) &&
			translatedRight != parts.right;

		if (!leftFound)
			translatedLeft = parts.left;
		if (!rightFound)
			translatedRight = parts.right;
		if (!leftFound && !rightFound)
			return false;

		translated = translatedLeft + " x " + parts.number + " " + translatedRight;

		if (g_bEnableDictionaryTranslationLog)
		{
			gLog.FormattedMessage("tnvse_dictionary: multiplier text match:");
			gLog.FormattedMessage("tnvse_dictionary:   source=\"%s\"", source.c_str());
			gLog.FormattedMessage("tnvse_dictionary:   left=\"%s\" ->\"%s\"",
				parts.left.c_str(), translatedLeft.c_str());
			gLog.FormattedMessage("tnvse_dictionary:   right=\"%s\" ->\"%s\"",
				parts.right.c_str(), translatedRight.c_str());
			gLog.FormattedMessage("tnvse_dictionary:   result=\"%s\"", translated.c_str());
		}
		return true;
	}

} // namespace fonthook

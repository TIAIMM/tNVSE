#include "dictionary_internal.h"
#include "load_config.h"

namespace fonthook
{
	namespace
	{
		struct EffectSegment
		{
			std::string name;
			std::string suffix;
			bool numeric = false;
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

		void TrimAsciiWhitespace(std::string& text)
		{
			const auto isTrim = [](unsigned char ch)
				{
					return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
				};

			size_t first = 0;
			while (first < text.size() && isTrim((unsigned char)text[first]))
				++first;
			size_t last = text.size();
			while (last > first && isTrim((unsigned char)text[last - 1]))
				--last;
			text = text.substr(first, last - first);
		}

		std::vector<std::string> SplitEffectSegments(const std::string& source)
		{
			std::vector<std::string> segments;
			size_t cursor = 0;
			while (cursor <= source.size())
			{
				const size_t comma = source.find(',', cursor);
				std::string segment = comma == std::string::npos
					? source.substr(cursor)
					: source.substr(cursor, comma - cursor);
				TrimAsciiWhitespace(segment);
				if (segment.empty())
					return {};
				segments.push_back(std::move(segment));
				if (comma == std::string::npos)
					break;
				cursor = comma + 1;
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

			size_t pos = coreEnd;
			if (pos > 0 && segment[pos - 1] == '%')
				--pos;
			const size_t digitsEnd = pos;
			while (pos > 0 && IsAsciiDigit(segment[pos - 1]))
				--pos;
			if (pos == digitsEnd)
				return false;
			if (pos == 0)
				return false;

			const char sign = segment[pos - 1];
			if (sign != '+' && sign != '-' && sign != '|')
				return false;
			if (pos < 2)
				return false;
			const char beforeSign = segment[pos - 2];
			if (beforeSign != ' ' && beforeSign != '\t')
				return false;

			size_t nameEnd = pos - 1;
			while (nameEnd > 0 && (segment[nameEnd - 1] == ' ' || segment[nameEnd - 1] == '\t'))
				--nameEnd;
			if (nameEnd == 0)
				return false;

			parsed.name = segment.substr(0, nameEnd);
			parsed.suffix = std::string(" ") + segment.substr(pos - 1, coreEnd - (pos - 1)) + duration;
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
	}

	bool TryTranslateItemEffectList(const std::string& source, std::string& translated, int depth)
	{
		if (depth >= 4 || source.empty())
			return false;

		std::string trimmedSource = source;
		TrimAsciiWhitespace(trimmedSource);
		if (trimmedSource.empty() || StartsWithIgnoreCase(trimmedSource, "Permanent"))
			return false;

		const std::vector<std::string> rawSegments = SplitEffectSegments(trimmedSource);
		if (rawSegments.empty())
			return false;

		std::vector<EffectSegment> segments;
		segments.reserve(rawSegments.size());
		bool hasNumericSegment = false;
		for (const std::string& rawSegment : rawSegments)
		{
			EffectSegment segment;
			if (TryParseNumericEffectSegment(rawSegment, segment))
			{
				hasNumericSegment = true;
			}
			else
			{
				segment.name = rawSegment;
			}

			if (!HasAlphabet(segment.name))
				return false;
			segments.push_back(std::move(segment));
		}

		if (!hasNumericSegment)
			return false;

		std::string result;
		bool changed = false;
		for (const EffectSegment& segment : segments)
		{
			std::string translatedName;
			const bool translatedSegment = TranslateEffectName(segment.name, translatedName, depth);
			if (!translatedSegment)
				translatedName = segment.name;
			else if (translatedName != segment.name)
				changed = true;

			if (!result.empty())
				result += ", ";
			result += translatedName;
			result += segment.suffix;
		}

		if (!changed)
			return false;

		translated = std::move(result);
		if (g_bEnableDictionaryTranslationLog)
		{
			gLog.FormattedMessage("tnvse_dictionary: item effect list match:");
			gLog.FormattedMessage("tnvse_dictionary:   source=\"%s\"", source.c_str());
			gLog.FormattedMessage("tnvse_dictionary:   result=\"%s\"", translated.c_str());
		}
		return true;
	}

} // namespace fonthook

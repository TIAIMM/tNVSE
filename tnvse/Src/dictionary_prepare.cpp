#include "dictionary_internal.h"
#include "encoding.h"
#include "load_config.h"

#include <regex>

namespace fonthook
{
	namespace
	{
		bool IsSourceWhitespace(char ch)
		{
			return
				ch == ' ' ||
				ch == '\t' ||
				ch == '\n' ||
				ch == '\r' ||
				ch == '\v' ||
				ch == '\f';
		}

		bool IsAsciiAlpha(char ch)
		{
			return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
		}

		char ToLowerAsciiChar(char ch)
		{
			if (ch >= 'A' && ch <= 'Z')
				return static_cast<char>(ch + ('a' - 'A'));
			return ch;
		}

		bool IsSourceBreakTag(const std::string& text, size_t tagBegin, size_t tagEnd)
		{
			size_t pos = tagBegin + 1;
			while (pos < tagEnd && IsSourceWhitespace(text[pos]))
				++pos;
			if (pos < tagEnd && text[pos] == '/')
			{
				++pos;
				while (pos < tagEnd && IsSourceWhitespace(text[pos]))
					++pos;
			}

			const size_t nameBegin = pos;
			while (pos < tagEnd && IsAsciiAlpha(text[pos]))
				++pos;
			const size_t nameLength = pos - nameBegin;
			if (nameLength == 0)
				return false;

			const bool isParagraph =
				nameLength == 1 &&
				ToLowerAsciiChar(text[nameBegin]) == 'p';
			const bool isDiv =
				nameLength == 3 &&
				ToLowerAsciiChar(text[nameBegin]) == 'd' &&
				ToLowerAsciiChar(text[nameBegin + 1]) == 'i' &&
				ToLowerAsciiChar(text[nameBegin + 2]) == 'v';
			if (!isParagraph && !isDiv)
				return false;

			return pos == tagEnd || IsSourceWhitespace(text[pos]) || text[pos] == '/';
		}

		void NormalizeSourceMarkup(std::string& text)
		{
			if (text.find('<') == std::string::npos)
				return;

			std::string result;
			result.reserve(text.size());
			for (size_t i = 0; i < text.size();)
			{
				if (text[i] == '<')
				{
					const size_t end = text.find('>', i + 1);
					if (end != std::string::npos && IsSourceBreakTag(text, i, end))
					{
						result.push_back(' ');
						i = end + 1;
						continue;
					}
				}
				result.push_back(text[i]);
				++i;
			}
			text.swap(result);
		}

		void NormalizeSourceWhitespace(std::string& text)
		{
			std::string result;
			result.reserve(text.size());
			bool previousSpace = false;
			for (char ch : text)
			{
				if (IsSourceWhitespace(ch))
				{
					if (!previousSpace)
						result.push_back(' ');
					previousSpace = true;
				}
				else
				{
					result.push_back(ch);
					previousSpace = false;
				}
			}
			text.swap(result);
			Trim(text);
		}
	}

	// ---- bind-token helpers ----

	size_t CountTargetBindToken(const std::string& text)
	{
		size_t count = 0;
		UInt32 code = 0;
		for (size_t i = 0; i < text.size(); ++i)
		{
			if (TryDecodeDoubleByte(&text[i], code))
			{
				++i;
				continue;
			}
			if (i + 3 <= text.size() && text.compare(i, 3, kBindSymbol) == 0)
			{
				++count;
				i += 2;
			}
		}
		return count;
	}

	std::vector<std::string> SplitTargetByBindToken(const std::string& text)
	{
		std::vector<std::string> result;
		size_t cursor = 0;
		UInt32 code = 0;
		for (size_t i = 0; i < text.size(); ++i)
		{
			if (TryDecodeDoubleByte(&text[i], code))
			{
				++i;
				continue;
			}
			if (i + 3 <= text.size() && text.compare(i, 3, kBindSymbol) == 0)
			{
				result.push_back(text.substr(cursor, i - cursor));
				i += 2;
				cursor = i + 1;
			}
		}
		result.push_back(text.substr(cursor));
		return result;
	}

	size_t LengthWithoutBinds(std::string_view text)
	{
		return text.size() - CountToken(text, kBindSymbol) * 3;
	}

	// ---- format-specifier / game-variable conversion ----

	// Convert game-engine variable references to [%] bind symbols.
	// These get substituted by the engine at runtime and must be preserved as placeholders.
	// Covers:
	//   &PCName; &PCRace; &PCSex; &PCSexPronoun; &PCSexPossessive;
	//   &-sUActnForward;    (button icon)
	//   p&-sUActnForward;   ("Press" + button icon)
	//   &-sXBStart;         (Xbox controller icons)
	void ConvertGameVariablesToBind(std::string& str)
	{
		if (str.find('&') == std::string::npos)
			return;

		static const std::regex pattern(
			R"((&pc(name|race|sex|sexpronoun|sexpossessive);|p?&-?s[0-9a-z]+;))",
			std::regex_constants::ECMAScript | std::regex_constants::icase | std::regex_constants::optimize);

		const std::string before = str;
		str = std::regex_replace(str, pattern, "[%]");

		if (g_bEnableDictionaryTranslationLog && str != before)
		{
			const bool toMb = g_usingWinEncoding != 0 && IsValidUTF8With3ByteMin(before.c_str());
			const std::string logBefore = toMb ? UTF8ToMultiByteStr(before, g_usingWinEncoding) : before;
			const std::string logAfter  = toMb ? UTF8ToMultiByteStr(str, g_usingWinEncoding) : str;
			gLog.FormattedMessage("tnvse_dictionary: ");
			gLog.FormattedMessage("tnvse_dictionary:   convertGameVars:");
			gLog.FormattedMessage("tnvse_dictionary:     before: \"%s\"", logBefore.c_str());
			gLog.FormattedMessage("tnvse_dictionary:     after:  \"%s\"", logAfter.c_str());
			gLog.FormattedMessage("tnvse_dictionary: ");
		}
	}

	// Convert GECK format-specifiers (%d, %s, %f, etc.) to [%] bind symbols so that
	// dictionary entries can match runtime text containing dynamic values.
	// Reference: https://geckwiki.com/index.php?title=String_Formatting
	void ConvertFormatSpecifiersToBind(std::string& str)
	{
		if (str.find('%') == std::string::npos)
			return;

		// Matches GECK / NVSE format specifiers:
		//   %a %b %B %c %d %e %f %g %i %k %n %v %x %z %s   (simple types)
		//   %p[ops]                                           (pronoun / percent-sign)
		//   %[flags][width].[precision][ef]                   (floating-point,
		//     e.g. %+.2f, %10.4f, %.0f)
		//   %x[0-9]                                           (hex with min-width)
		static const std::regex formatting(
			R"((%[abBcdefgiknvxzs]|%x[0-9]|%p[ops]|%[\+\- 0]?[0-9]*\.[0-9]*[ef]))",
			std::regex_constants::ECMAScript | std::regex_constants::optimize);

		const std::string before = str;
		str = std::regex_replace(str, formatting, "[%]");

		// Special single-character conversions
		ReplaceAll(str, "%q", "\"");     // %q → literal double-quote
		ReplaceAll(str, "%r", "[CRLF]"); // %r → line break placeholder

		if (g_bEnableDictionaryTranslationLog && str != before)
		{
			const bool toMb = g_usingWinEncoding != 0 && IsValidUTF8With3ByteMin(before.c_str());
			const std::string logBefore = toMb ? UTF8ToMultiByteStr(before, g_usingWinEncoding) : before;
			const std::string logAfter  = toMb ? UTF8ToMultiByteStr(str, g_usingWinEncoding) : str;
			gLog.FormattedMessage("tnvse_dictionary: ");
			gLog.FormattedMessage("tnvse_dictionary:   convertFmtSpec:");
			gLog.FormattedMessage("tnvse_dictionary:     before: \"%s\"", logBefore.c_str());
			gLog.FormattedMessage("tnvse_dictionary:     after:  \"%s\"", logAfter.c_str());
			gLog.FormattedMessage("tnvse_dictionary: ");
		}
	}

	// ---- text preparation pipelines ----

	std::string PrepareSourceForRegistration(std::string text)
	{
		StripUtf8Bom(text);
		NormalizeSourceMarkup(text);
		NormalizeSourceWhitespace(text);
		RemoveControlChars(text);
		Correct1252ToAscii(text);
		ConvertGameVariablesToBind(text);
		ConvertFormatSpecifiersToBind(text);
		ReplaceAll(text, "%%", "%"); // escaped percent → literal % (after format conversion)
		RemoveAlignmentTag(text);
		ReplaceAll(text, "[QUOTE]", "\"");
		NormalizeSourceWhitespace(text);
		ToLowerAscii(text);
		return text;
	}

	std::string PrepareSourceForLookup(std::string text)
	{
		NormalizeSourceMarkup(text);
		NormalizeSourceWhitespace(text);
		RemoveControlChars(text);
		Correct1252ToAscii(text);
		NormalizeSourceWhitespace(text);
		ToLowerAscii(text);
		return text;
	}

	std::string PrepareSourceForLookupPreserveCase(std::string text)
	{
		NormalizeSourceMarkup(text);
		NormalizeSourceWhitespace(text);
		RemoveControlChars(text);
		Correct1252ToAscii(text);
		NormalizeSourceWhitespace(text);
		return text;
	}

	std::string PrepareTarget(std::string text)
	{
		StripUtf8Bom(text);
		RemoveControlChars(text);
		ConvertGameVariablesToBind(text);
		ConvertFormatSpecifiersToBind(text);
		ReplaceAll(text, "%%", "%"); // escaped percent → literal % (after format conversion)
		ReplaceAll(text, "[CRLF]", "\n");
		ReplaceAll(text, "[QUOTE]", "\"");
		ReplaceAll(text, "\\n", "\n");
		ReplaceAll(text, "\t", "    ");
		if (g_usingWinEncoding != 0 && IsValidUTF8With3ByteMin(text.c_str()))
			text = UTF8ToMultiByteStr(text, g_usingWinEncoding);
		return text;
	}

} // namespace fonthook

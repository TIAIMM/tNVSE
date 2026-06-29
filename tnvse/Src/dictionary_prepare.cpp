#include "dictionary_internal.h"
#include "encoding.h"
#include "load_config.h"

#include <regex>

namespace fonthook
{

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

	// ---- double-byte detection ----

	bool ContainsDoubleByteText(const char* text)
	{
		if (!text || g_usingWinEncoding == 0)
			return false;

		UInt32 code = 0;
		for (size_t i = 0; text[i]; ++i)
		{
			if (TryDecodeDoubleByte(&text[i], code))
				return true;
		}
		return false;
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
			gLog.FormattedMessage("tnvse_dictionary:   convertGameVars: \"%s\" ->\"%s\"",
				toMb ? UTF8ToMultiByteStr(before, g_usingWinEncoding).c_str() : before.c_str(),
				toMb ? UTF8ToMultiByteStr(str, g_usingWinEncoding).c_str() : str.c_str());
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
			gLog.FormattedMessage("tnvse_dictionary:   convertFmtSpec: \"%s\" ->\"%s\"",
				toMb ? UTF8ToMultiByteStr(before, g_usingWinEncoding).c_str() : before.c_str(),
				toMb ? UTF8ToMultiByteStr(str, g_usingWinEncoding).c_str() : str.c_str());
		}
	}

	// ---- text preparation pipelines ----

	std::string PrepareSourceForRegistration(std::string text)
	{
		StripUtf8Bom(text);
		RemoveControlChars(text);
		Correct1252ToAscii(text);
		ConvertGameVariablesToBind(text);
		ConvertFormatSpecifiersToBind(text);
		ReplaceAll(text, "%%", "%"); // escaped percent → literal % (after format conversion)
		RemoveAlignmentTag(text);
		ReplaceAll(text, "[QUOTE]", "\"");
		CollapseSpaces(text);
		Trim(text);
		ToLowerAscii(text);
		return text;
	}

	std::string PrepareSourceForLookup(std::string text)
	{
		RemoveControlChars(text);
		Correct1252ToAscii(text);
		CollapseSpaces(text);
		Trim(text);
		ToLowerAscii(text);
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

#include "dictionary_internal.h"
#include "encoding.h"
#include "load_config.h"

namespace fonthook
{
	namespace implementation::dictionary_prepare {}
	using namespace implementation::dictionary_prepare;

	namespace implementation::dictionary_prepare
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

		bool IsAsciiDigit(char ch)
		{
			return ch >= '0' && ch <= '9';
		}

		bool IsAsciiAlnum(char ch)
		{
			return IsAsciiAlpha(ch) || IsAsciiDigit(ch);
		}

		bool StartsWithIgnoreCase(const std::string& text, size_t pos, std::string_view pattern)
		{
			if (pos + pattern.size() > text.size())
				return false;
			for (size_t i = 0; i < pattern.size(); ++i)
			{
				if (ToLowerAsciiChar(text[pos + i]) != ToLowerAsciiChar(pattern[i]))
					return false;
			}
			return true;
		}

		bool TryMatchEscapedSourceWhitespace(const std::string& text, size_t pos, size_t& end)
		{
			if (StartsWithIgnoreCase(text, pos, "[CRLF]"))
			{
				end = pos + 6;
				return true;
			}

			if (pos + 1 >= text.size() || text[pos] != '\\')
				return false;

			const char code = text[pos + 1];
			if ((code == 'r' || code == 'R') &&
				pos + 3 < text.size() &&
				text[pos + 2] == '\\' &&
				(text[pos + 3] == 'n' || text[pos + 3] == 'N'))
			{
				end = pos + 4;
				return true;
			}

			if (code == 'n' || code == 'N' || code == 'r' || code == 'R' || code == 't' || code == 'T')
			{
				end = pos + 2;
				return true;
			}

			return false;
		}

		void NormalizeEscapedSourceWhitespace(std::string& text)
		{
			if (text.find('\\') == std::string::npos && text.find('[') == std::string::npos)
				return;

			std::string result;
			result.reserve(text.size());
			bool changed = false;

			for (size_t i = 0; i < text.size();)
			{
				size_t end = 0;
				if (TryMatchEscapedSourceWhitespace(text, i, end))
				{
					result.push_back(' ');
					i = end;
					changed = true;
					continue;
				}

				result.push_back(text[i++]);
			}

			if (changed)
				text.swap(result);
		}

		void NormalizeEscapedTargetWhitespace(std::string& text)
		{
			if (text.find('\\') == std::string::npos && text.find('[') == std::string::npos)
				return;

			std::string result;
			result.reserve(text.size());
			bool changed = false;

			for (size_t i = 0; i < text.size();)
			{
				if (StartsWithIgnoreCase(text, i, "[CRLF]"))
				{
					result.push_back('\n');
					i += 6;
					changed = true;
					continue;
				}

				if (i + 1 < text.size() && text[i] == '\\')
				{
					const char code = text[i + 1];
					if ((code == 'r' || code == 'R') &&
						i + 3 < text.size() &&
						text[i + 2] == '\\' &&
						(text[i + 3] == 'n' || text[i + 3] == 'N'))
					{
						result.push_back('\n');
						i += 4;
						changed = true;
						continue;
					}

					if (code == 'n' || code == 'N' || code == 'r' || code == 'R')
					{
						result.push_back('\n');
						i += 2;
						changed = true;
						continue;
					}

					if (code == 't' || code == 'T')
					{
						result += "    ";
						i += 2;
						changed = true;
						continue;
					}
				}

				result.push_back(text[i++]);
			}

			if (changed)
				text.swap(result);
		}

		bool TryMatchPcVariable(const std::string& text, size_t ampPos, size_t& end)
		{
			static constexpr std::string_view names[] =
			{
				"PCName",
				"PCRace",
				"PCSex",
				"PCSexPronoun",
				"PCSexPossessive",
			};

			for (std::string_view name : names)
			{
				const size_t semicolon = ampPos + 1 + name.size();
				if (StartsWithIgnoreCase(text, ampPos + 1, name) &&
					semicolon < text.size() && text[semicolon] == ';')
				{
					end = semicolon + 1;
					return true;
				}
			}

			return false;
		}

		bool TryMatchControlVariable(const std::string& text, size_t begin, size_t& end)
		{
			size_t ampPos = begin;
			if ((text[begin] == 'p' || text[begin] == 'P') &&
				begin + 1 < text.size() && text[begin + 1] == '&')
			{
				ampPos = begin + 1;
			}
			else if (text[begin] != '&')
			{
				return false;
			}

			size_t pos = ampPos + 1;
			if (pos < text.size() && text[pos] == '-')
				++pos;
			if (pos >= text.size() || ToLowerAsciiChar(text[pos]) != 's')
				return false;

			++pos;
			const size_t payloadBegin = pos;
			while (pos < text.size() && IsAsciiAlnum(text[pos]))
				++pos;
			if (pos == payloadBegin)
				return false;
			if (pos >= text.size() || text[pos] != ';')
				return false;

			end = pos + 1;
			return true;
		}

		bool TryMatchGameVariable(const std::string& text, size_t begin, size_t& end)
		{
			if (text[begin] == '&' && TryMatchPcVariable(text, begin, end))
				return true;
			return TryMatchControlVariable(text, begin, end);
		}

		bool IsSimpleFormatSpecifier(char ch)
		{
			switch (ch)
			{
			case 'a':
			case 'b':
			case 'B':
			case 'c':
			case 'd':
			case 'e':
			case 'f':
			case 'g':
			case 'i':
			case 'k':
			case 'n':
			case 'v':
			case 'x':
			case 'z':
			case 's':
				return true;
			default:
				return false;
			}
		}

		bool IsFloatFormatFlag(char ch)
		{
			return ch == '+' || ch == '-' || ch == ' ' || ch == '0';
		}

		bool TryMatchFloatFormatSpecifier(const std::string& text, size_t percentPos, size_t& end)
		{
			size_t pos = percentPos + 1;
			if (pos >= text.size())
				return false;

			if (IsFloatFormatFlag(text[pos]))
				++pos;
			while (pos < text.size() && IsAsciiDigit(text[pos]))
				++pos;
			if (pos >= text.size() || text[pos] != '.')
				return false;

			++pos;
			while (pos < text.size() && IsAsciiDigit(text[pos]))
				++pos;
			if (pos >= text.size() || (text[pos] != 'e' && text[pos] != 'f'))
				return false;

			end = pos + 1;
			return true;
		}

		bool TryMatchBindFormatSpecifier(const std::string& text, size_t percentPos, size_t& end)
		{
			if (percentPos + 1 >= text.size())
				return false;

			const char next = text[percentPos + 1];
			if (next == 'x' && percentPos + 2 < text.size() && IsAsciiDigit(text[percentPos + 2]))
			{
				end = percentPos + 3;
				return true;
			}

			if (IsSimpleFormatSpecifier(next))
			{
				end = percentPos + 2;
				return true;
			}

			if (next == 'p' && percentPos + 2 < text.size())
			{
				const char pronoun = text[percentPos + 2];
				if (pronoun == 'o' || pronoun == 'p' || pronoun == 's')
				{
					end = percentPos + 3;
					return true;
				}
			}

			return TryMatchFloatFormatSpecifier(text, percentPos, end);
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

		const std::string before = g_bEnableDictionaryTranslationLog ? str : std::string{};
		std::string result;
		result.reserve(str.size());
		bool changed = false;

		for (size_t i = 0; i < str.size();)
		{
			size_t end = 0;
			if ((str[i] == '&' || str[i] == 'p' || str[i] == 'P') &&
				TryMatchGameVariable(str, i, end))
			{
				result += kBindSymbol;
				i = end;
				changed = true;
				continue;
			}

			result.push_back(str[i++]);
		}

		if (!changed)
			return;

		str.swap(result);

		if (g_bEnableDictionaryTranslationLog && str != before)
		{
			const bool toMb = IsEastAsianUiMode()
				&& IsValidUTF8With3ByteMin(before.c_str());
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

		const std::string before = g_bEnableDictionaryTranslationLog ? str : std::string{};
		std::string result;
		result.reserve(str.size());
		bool changed = false;

		for (size_t i = 0; i < str.size();)
		{
			if (str[i] == '%' && i + 1 < str.size())
			{
				const char next = str[i + 1];
				if (next == 'q')
				{
					result.push_back('"');
					i += 2;
					changed = true;
					continue;
				}
				if (next == 'r')
				{
					result += "[CRLF]";
					i += 2;
					changed = true;
					continue;
				}

				size_t end = 0;
				if (TryMatchBindFormatSpecifier(str, i, end))
				{
					result += kBindSymbol;
					i = end;
					changed = true;
					continue;
				}
			}

			result.push_back(str[i++]);
		}

		if (!changed)
			return;

		str.swap(result);

		if (g_bEnableDictionaryTranslationLog && str != before)
		{
			const bool toMb = IsEastAsianUiMode()
				&& IsValidUTF8With3ByteMin(before.c_str());
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
		NormalizeEscapedSourceWhitespace(text);
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
		NormalizeEscapedSourceWhitespace(text);
		NormalizeSourceWhitespace(text);
		RemoveControlChars(text);
		Correct1252ToAscii(text);
		NormalizeSourceWhitespace(text);
		ToLowerAscii(text);
		return text;
	}

	std::string PrepareSourceForWindows1252ExactRegistration(std::string text)
	{
		NormalizeSourceMarkup(text);
		NormalizeSourceWhitespace(text);
		RemoveControlChars(text);
		ConvertGameVariablesToBind(text);
		ConvertFormatSpecifiersToBind(text);
		NormalizeEscapedSourceWhitespace(text);
		ReplaceAll(text, "%%", "%");
		RemoveAlignmentTag(text);
		ReplaceAll(text, "[QUOTE]", "\"");
		NormalizeSourceWhitespace(text);
		ToLowerAscii(text);
		return text;
	}

	std::string PrepareSourceForWindows1252ExactLookup(std::string text)
	{
		NormalizeSourceMarkup(text);
		NormalizeEscapedSourceWhitespace(text);
		NormalizeSourceWhitespace(text);
		RemoveControlChars(text);
		NormalizeSourceWhitespace(text);
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
		NormalizeEscapedTargetWhitespace(text);
		ReplaceAll(text, "[QUOTE]", "\"");
		ReplaceAll(text, "\t", "    ");
		if (IsEastAsianUiMode() && IsValidUTF8With3ByteMin(text.c_str()))
			text = UTF8ToMultiByteStr(text, g_usingWinEncoding);
		return text;
	}

} // namespace fonthook

#include "dictionary_internal.h"
#include "encoding.h"
#include "load_config.h"

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

	// ---- text preparation pipelines ----

	std::string PrepareSourceForRegistration(std::string text)
	{
		StripUtf8Bom(text);
		RemoveControlChars(text);
		Correct1252ToAscii(text);
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
		ReplaceAll(text, "[CRLF]", "\n");
		ReplaceAll(text, "[QUOTE]", "\"");
		ReplaceAll(text, "\\n", "\n");
		ReplaceAll(text, "\t", "    ");
		if (g_usingWinEncoding != 0 && IsValidUTF8With3ByteMin(text.c_str()))
			text = UTF8ToMultiByteStr(text, g_usingWinEncoding);
		return text;
	}

} // namespace fonthook

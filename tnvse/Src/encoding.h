#pragma once
#include "ITypes.h"
#include "load_config.h"
#include <string>
#include <string_view>
#include <Windows.h>

namespace fonthook
{
	// ---- Encoding identifier ----
	enum class CodePage : UInt32
	{
		Windows1252 = 1252,
		GBK = 936,
		Big5 = 950,
		SJIS = 932,
		UHC = 949,
	};

	constexpr UInt32 kWindows1252CodePage =
		static_cast<UInt32>(CodePage::Windows1252);

	bool IsDbcsCodePage(UInt32 codePage);
	bool IsEastAsianUiMode();
	bool UsesDbcsTextLayout();
	UInt32 GetFreeTypeTextCodePage();
	bool TryDecodeDoubleByteForCodePage(
		const char* p, UInt32 codePage, UInt32& outCode);
	bool TryDecodeFreeTypeDoubleByte(const char* p, UInt32& outCode);

	// ---- Unified encoding dispatch (replaces repeated if/else-if chains) ----
	bool IsLeadByte(UInt8 c);
	bool IsTrailByte(UInt8 c);
	bool TryDecodeDoubleByte(const char* p, UInt32& outCode);

	// ---- GBK (Code Page 936) ----
	bool IsGBKLeadByte(UInt8 c);
	bool IsGBKTrailByte(UInt8 c);
	bool TryDecodeGBK(const char* p, UInt32& outCode);

	// ---- Big5 (Code Page 950) ----
	bool IsBig5LeadByte(UInt8 c);
	bool IsBig5TrailByte(UInt8 c);
	bool TryDecodeBig5(const char* p, UInt32& outCode);

	// ---- Shift-JIS (Code Page 932) ----
	bool IsSJISLeadByte(UInt8 c);
	bool IsSJISTrailByte(UInt8 c);
	bool TryDecodeSJIS(const char* p, UInt32& outCode);

	// ---- Korean/UHC (Code Page 949) ----
	bool IsKoreanLeadByte(UInt8 c);
	bool IsKoreanTrailByte(UInt8 c);
	bool TryDecodeKorean(const char* p, UInt32& outCode);

	// ---- UTF-8 ----
	bool IsValidUTF8With3ByteMin(const char* s);
	inline bool IsUTF8Lead3(UInt8 c)
	{
		return (c & 0xF0) == 0xE0;
	}
	inline bool IsUTF8Trail(UInt8 c)
	{
		return (c & 0xC0) == 0x80;
	}

	// ---- Encoding Conversion ----
	std::string UTF8ToMultiByteStr(const std::string& utf8, UInt32 codePage);
	std::string WideToUTF8(std::wstring_view value);

	// ---- Double-byte character counting ----
	// Reduces charCount by the number of double-byte character pairs in str,
	// scanning up to maxLen bytes (0 means scan until null terminator).
	template<typename TExtraGlyphs>
	inline int AdjustCharCountForDB(const char* str, int charCount, TExtraGlyphs* extraGlyphs, UInt32 maxLen = 0)
	{
		if (!extraGlyphs || !str) return charCount;
		int count = charCount;
		UInt32 code;
		for (UInt32 i = 0; (maxLen ? i < maxLen : str[i] != 0); ++i)
		{
			if (TryDecodeDoubleByte(&str[i], code))
			{
				++i; // skip trail byte
				--count;
			}
		}
		return count;
	}

	// ---- Shared UTF8 detection helper ----
	inline bool ShouldConvertUTF8(bool hasExtraGlyphs)
	{
		return g_bEnableUTF8 && IsEastAsianUiMode() && hasExtraGlyphs;
	}

	// Converts src from UTF-8 to multi-byte, reassigning pSrc to point into outConverted.
	// Caller must keep outConverted alive while using pSrc.
	// Returns true if conversion was performed.
	inline bool ConvertToMultiByte(const char*& pSrc, std::string& outConverted, bool hasExtraGlyphs)
	{
		if (!ShouldConvertUTF8(hasExtraGlyphs)) return false;
		if (!IsValidUTF8With3ByteMin(pSrc)) return false;
		outConverted = UTF8ToMultiByteStr(pSrc, g_usingWinEncoding);
		pSrc = outConverted.c_str();
		return true;
	}

} // namespace fonthook

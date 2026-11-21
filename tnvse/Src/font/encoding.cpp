#include "encoding.h"
#include "font_multibyte_prewarm_policy.h"
#include "load_config.h"

#include <limits>

namespace fonthook
{
	bool IsDbcsCodePage(UInt32 codePage)
	{
		return multibyte_prewarm::IsDbcsCodePage(codePage);
	}

	bool IsEastAsianUiMode()
	{
		return g_uiEncoding >= 1 && g_uiEncoding <= 4;
	}

	bool UsesDbcsTextLayout()
	{
		return g_bEnableMultibyteFontHook && IsDbcsCodePage(g_usingWinEncoding);
	}

	UInt32 GetFreeTypeTextCodePage()
	{
		if (UsesDbcsTextLayout())
			return g_usingWinEncoding;

		if (g_usingWinEncoding == kWindows1251CodePage)
			return kWindows1251CodePage;

		return kWindows1252CodePage;
	}

	// ===================== GBK (Code Page 936) =====================
	bool IsGBKLeadByte(UInt8 c)
	{
		return multibyte_prewarm::IsLeadByte(
			multibyte_prewarm::kCodePageGbk, c);
	}

	bool IsGBKTrailByte(UInt8 c)
	{
		return multibyte_prewarm::IsTrailByte(
			multibyte_prewarm::kCodePageGbk, c);
	}

	bool TryDecodeGBK(const char* p, UInt32& outCode)
	{
		UInt8 lead = (UInt8)p[0];
		UInt8 trail = (UInt8)p[1];
		if (!IsGBKLeadByte(lead) || !IsGBKTrailByte(trail))
			return false;
		outCode = (UInt32(lead) << 8) | UInt32(trail);
		return true;
	}

	// ===================== Big5 (Code Page 950) =====================
	bool IsBig5LeadByte(UInt8 c)
	{
		return multibyte_prewarm::IsLeadByte(
			multibyte_prewarm::kCodePageBig5, c);
	}

	bool IsBig5TrailByte(UInt8 c)
	{
		return multibyte_prewarm::IsTrailByte(
			multibyte_prewarm::kCodePageBig5, c);
	}

	bool TryDecodeBig5(const char* p, UInt32& outCode)
	{
		UInt8 lead = (UInt8)p[0];
		UInt8 trail = (UInt8)p[1];
		if (!IsBig5LeadByte(lead) || !IsBig5TrailByte(trail))
			return false;
		outCode = (UInt32(lead) << 8) | UInt32(trail);
		return true;
	}

	// ===================== Shift-JIS (Code Page 932) =====================
	bool IsSJISLeadByte(UInt8 c)
	{
		return multibyte_prewarm::IsLeadByte(
			multibyte_prewarm::kCodePageShiftJis, c);
	}

	bool IsSJISTrailByte(UInt8 c)
	{
		return multibyte_prewarm::IsTrailByte(
			multibyte_prewarm::kCodePageShiftJis, c);
	}

	bool TryDecodeSJIS(const char* p, UInt32& outCode)
	{
		UInt8 lead = (UInt8)p[0];
		UInt8 trail = (UInt8)p[1];
		if (!IsSJISLeadByte(lead) || !IsSJISTrailByte(trail))
			return false;
		outCode = (UInt32(lead) << 8) | UInt32(trail);
		return true;
	}

	// ===================== Korean/UHC (Code Page 949) =====================
	bool IsKoreanLeadByte(UInt8 c)
	{
		return multibyte_prewarm::IsLeadByte(
			multibyte_prewarm::kCodePageUhc, c);
	}

	bool IsKoreanTrailByte(UInt8 c)
	{
		return multibyte_prewarm::IsTrailByte(
			multibyte_prewarm::kCodePageUhc, c);
	}

	bool TryDecodeKorean(const char* p, UInt32& outCode)
	{
		UInt8 lead = (UInt8)p[0];
		UInt8 trail = (UInt8)p[1];
		if (!IsKoreanLeadByte(lead) || !IsKoreanTrailByte(trail))
			return false;
		outCode = (UInt32(lead) << 8) | UInt32(trail);
		return true;
	}

	bool TryDecodeDoubleByteForCodePage(
		const char* p, UInt32 codePage, UInt32& outCode)
	{
		if (!p || static_cast<UInt8>(p[0]) < 0x80 || !p[1])
			return false;
		switch (codePage)
		{
		case 936: return TryDecodeGBK(p, outCode);
		case 950: return TryDecodeBig5(p, outCode);
		case 932: return TryDecodeSJIS(p, outCode);
		case 949: return TryDecodeKorean(p, outCode);
		default: return false;
		}
	}

	bool TryDecodeFreeTypeDoubleByte(const char* p, UInt32& outCode)
	{
		return TryDecodeDoubleByteForCodePage(
			p, GetFreeTypeTextCodePage(), outCode);
	}

	// ===================== UTF-8 =====================
	bool IsValidUTF8With3ByteMin(const char* s)
	{
		if (!s)
			return false;

		const UInt8* p = (const UInt8*)s;
		bool has3ByteOrMore = false;

		while (*p)
		{
			if (*p < 0x80)
			{
				p++;
			}
			else if (*p < 0xC2)
			{
				return false;
			}
			else if (*p < 0xE0)
			{
				if ((p[1] & 0xC0) != 0x80)
					return false;
				p += 2;
			}
			else if (*p < 0xF0)
			{
				const UInt8 c2 = p[1];
				if (!IsUTF8Trail(c2) || (*p == 0xE0 && c2 < 0xA0)
					|| (*p == 0xED && c2 >= 0xA0) || !IsUTF8Trail(p[2]))
					return false;
				has3ByteOrMore = true;
				p += 3;
			}
			else if (*p < 0xF5)
			{
				const UInt8 c2 = p[1];
				if (!IsUTF8Trail(c2) || (*p == 0xF0 && c2 < 0x90)
					|| (*p == 0xF4 && c2 > 0x8F)
					|| !IsUTF8Trail(p[2]) || !IsUTF8Trail(p[3]))
					return false;
				has3ByteOrMore = true;
				p += 4;
			}
			else
			{
				return false;
			}
		}

		return has3ByteOrMore;
	}

	// ===================== Encoding Conversion =====================
	bool TryUTF8ToMultiByteStr(std::string_view utf8, UInt32 codePage,
		std::string& converted)
	{
		if (utf8.empty() || utf8.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
			return false;

		const int byteCount = static_cast<int>(utf8.size());
		const int wideCount = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			utf8.data(), byteCount, nullptr, 0);
		if (!wideCount)
			return false;
		std::wstring wide(wideCount, L'\0');
		if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			utf8.data(), byteCount, wide.data(), wideCount))
			return false;

		if (codePage == CP_UTF8)
		{
			converted.assign(utf8);
			return true;
		}

		BOOL usedDefault = FALSE;
		const int encodedCount = WideCharToMultiByte(codePage, WC_NO_BEST_FIT_CHARS,
			wide.data(), wideCount, nullptr, 0, nullptr, &usedDefault);
		if (!encodedCount || usedDefault)
			return false;
		std::string encoded(encodedCount, '\0');
		if (!WideCharToMultiByte(codePage, WC_NO_BEST_FIT_CHARS,
			wide.data(), wideCount, encoded.data(), encodedCount, nullptr, &usedDefault) || usedDefault)
			return false;

		std::wstring roundTrip(wideCount, L'\0');
		if (MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
			encoded.data(), encodedCount, roundTrip.data(), wideCount) != wideCount || roundTrip != wide)
			return false;
		converted.swap(encoded);
		return true;
	}

	std::string UTF8ToMultiByteStr(const std::string& utf8, UInt32 codePage)
	{
		const int wideCharacterCount = MultiByteToWideChar(CP_UTF8, 0,
			utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
		if (wideCharacterCount == 0)
			return "";
		std::wstring wideText(static_cast<size_t>(wideCharacterCount), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
			static_cast<int>(utf8.size()), wideText.data(), wideCharacterCount);

		const int encodedByteCount = WideCharToMultiByte(codePage, 0,
			wideText.data(), static_cast<int>(wideText.size()), nullptr, 0,
			nullptr, nullptr);
		if (encodedByteCount == 0)
			return "";
		std::string encodedText(static_cast<size_t>(encodedByteCount), '\0');
		WideCharToMultiByte(codePage, 0, wideText.data(),
			static_cast<int>(wideText.size()), encodedText.data(), encodedByteCount,
			nullptr, nullptr);
		return encodedText;
	}

	std::string WideToUTF8(std::wstring_view value)
	{
		if (value.empty())
			return {};

		const int encodedByteCount = WideCharToMultiByte(
			CP_UTF8,
			0,
			value.data(),
			static_cast<int>(value.size()),
			nullptr,
			0,
			nullptr,
			nullptr);

		if (encodedByteCount <= 0)
			return {};

		std::string utf8(static_cast<size_t>(encodedByteCount), '\0');

		const int written = WideCharToMultiByte(
			CP_UTF8,
			0,
			value.data(),
			static_cast<int>(value.size()),
			utf8.data(),
			encodedByteCount,
			nullptr,
			nullptr);

		if (written <= 0)
			return {};

		return utf8;
	}

} // namespace fonthook

// ---- Unified encoding dispatch ----
// These functions use the configured UI code page. FreeType-only code uses
// TryDecodeFreeTypeDoubleByte so its resource identity remains Windows-1252.
// They replace the repeated if/else-if chains scattered across the codebase.

namespace fonthook
{

	bool IsLeadByte(UInt8 c)
	{
		if (c < 0x80) return false;  // ASCII fast-path
		switch (g_usingWinEncoding)
		{
		case 936:  return IsGBKLeadByte(c);
		case 950:  return IsBig5LeadByte(c);
		case 932:  return IsSJISLeadByte(c);
		case 949:  return IsKoreanLeadByte(c);
		default:   return false;
		}
	}

	bool IsTrailByte(UInt8 c)
	{
		// DBCS trail bytes may be in the ASCII range, e.g. GBK/Big5/SJIS 0x40-0x7E.
		switch (g_usingWinEncoding)
		{
		case 936:  return IsGBKTrailByte(c);
		case 950:  return IsBig5TrailByte(c);
		case 932:  return IsSJISTrailByte(c);
		case 949:  return IsKoreanTrailByte(c);
		default:   return false;
		}
	}

	bool TryDecodeDoubleByte(const char* p, UInt32& outCode)
	{
		return TryDecodeDoubleByteForCodePage(p, g_usingWinEncoding, outCode);
	}

} // namespace fonthook

#include "encoding.h"
#include "font_multibyte_prewarm_policy.h"
#include "load_config.h"

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
		return UsesDbcsTextLayout() ? g_usingWinEncoding : kWindows1252CodePage;
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
				UInt8 c2 = p[1], c3 = p[2];
				if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80)
					return false;
				has3ByteOrMore = true;
				p += 3;
			}
			else if (*p < 0xF5)
			{
				UInt8 c2 = p[1], c3 = p[2], c4 = p[3];
				if ((c2 & 0xC0) != 0x80 ||
					(c3 & 0xC0) != 0x80 ||
					(c4 & 0xC0) != 0x80)
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

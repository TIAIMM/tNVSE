#include "encoding.h"
#include "load_config.h"

namespace fonthook
{
	// ===================== GBK (Code Page 936) =====================
	bool IsGBKLeadByte(unsigned char c)
	{
		return (c >= 0x81 && c <= 0xFE);
	}

	bool IsGBKTrailByte(unsigned char c)
	{
		return (c >= 0x40 && c <= 0xFE && c != 0x7F);
	}

	bool TryDecodeGBK(const char* p, UInt32& outCode)
	{
		unsigned char lead = (unsigned char)p[0];
		unsigned char trail = (unsigned char)p[1];
		if (!IsGBKLeadByte(lead) || !IsGBKTrailByte(trail))
			return false;
		outCode = (UInt32(lead) << 8) | UInt32(trail);
		return true;
	}

	// ===================== Big5 (Code Page 950) =====================
	bool IsBig5LeadByte(unsigned char c)
	{
		return (c >= 0x81 && c <= 0xFE);
	}

	bool IsBig5TrailByte(unsigned char c)
	{
		return ((c >= 0x40 && c <= 0x7E) || (c >= 0xA1 && c <= 0xFE));
	}

	bool TryDecodeBig5(const char* p, UInt32& outCode)
	{
		unsigned char lead = (unsigned char)p[0];
		unsigned char trail = (unsigned char)p[1];
		if (!IsBig5LeadByte(lead) || !IsBig5TrailByte(trail))
			return false;
		outCode = (UInt32(lead) << 8) | UInt32(trail);
		return true;
	}

	// ===================== Shift-JIS (Code Page 932) =====================
	bool IsSJISLeadByte(unsigned char c)
	{
		if (c >= 0x81 && c <= 0x9F) return true;
		if (c >= 0xE0 && c <= 0xEA) return true;
		if (c == 0xED || c == 0xEE) return true;
		if (c >= 0xFA && c <= 0xFC) return true;
		return false;
	}

	bool IsSJISTrailByte(unsigned char c)
	{
		if (c >= 0x40 && c <= 0x7E) return true;
		if (c >= 0x80 && c <= 0xFC) return true;
		return false;
	}

	bool TryDecodeSJIS(const char* p, UInt32& outCode)
	{
		unsigned char lead = (unsigned char)p[0];
		unsigned char trail = (unsigned char)p[1];
		if (!IsSJISLeadByte(lead) || !IsSJISTrailByte(trail))
			return false;
		outCode = (UInt32(lead) << 8) | UInt32(trail);
		return true;
	}

	// ===================== Korean/UHC (Code Page 949) =====================
	bool IsKoreanLeadByte(unsigned char c)
	{
		return (c >= 0x81 && c <= 0xC8);
	}

	bool IsKoreanTrailByte(unsigned char c)
	{
		if (c >= 0x41 && c <= 0x5A) return true;
		if (c >= 0x61 && c <= 0x7A) return true;
		if (c >= 0x81 && c <= 0xFE) return true;
		return false;
	}

	bool TryDecodeKorean(const char* p, UInt32& outCode)
	{
		unsigned char lead = (unsigned char)p[0];
		unsigned char trail = (unsigned char)p[1];
		if (!IsKoreanLeadByte(lead) || !IsKoreanTrailByte(trail))
			return false;
		outCode = (UInt32(lead) << 8) | UInt32(trail);
		return true;
	}

	// ===================== UTF-8 =====================
	bool IsValidUTF8With3ByteMin(const char* s)
	{
		if (!s)
			return false;

		const unsigned char* p = (const unsigned char*)s;
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
				unsigned char c2 = p[1], c3 = p[2];
				if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80)
					return false;
				has3ByteOrMore = true;
				p += 3;
			}
			else if (*p < 0xF5)
			{
				unsigned char c2 = p[1], c3 = p[2], c4 = p[3];
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
	std::string MultiByteToUTF8(const std::string& src, UINT32 codePage)
	{
		int len = MultiByteToWideChar(codePage, 0, src.c_str(), -1, nullptr, 0);
		if (len == 0) return "";
		std::wstring wstr(len, L'\0');
		MultiByteToWideChar(codePage, 0, src.c_str(), -1, &wstr[0], len);

		len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
		std::string utf8(len, '\0');
		WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &utf8[0], len, nullptr, nullptr);
		return utf8;
	}

	std::string UTF8ToMultiByteStr(const std::string& utf8, UINT32 codePage)
	{
		int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
		if (len == 0) return "";
		std::wstring wstr(len, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wstr[0], len);

		len = WideCharToMultiByte(codePage, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
		std::string mb(len, '\0');
		WideCharToMultiByte(codePage, 0, wstr.c_str(), -1, &mb[0], len, nullptr, nullptr);
		return mb;
	}

} // namespace fonthook

// ---- Unified encoding dispatch ----
// These functions use the global g_usingWinEncoding to dispatch to the correct codec.
// They replace the repeated if/else-if chains scattered across the codebase.

namespace fonthook
{

	bool IsLeadByte(unsigned char c)
	{
		switch (g_usingWinEncoding)
		{
		case 936:  return IsGBKLeadByte(c);
		case 950:  return IsBig5LeadByte(c);
		case 932:  return IsSJISLeadByte(c);
		case 949:  return IsKoreanLeadByte(c);
		default:   return false;
		}
	}

	bool IsTrailByte(unsigned char c)
	{
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
		switch (g_usingWinEncoding)
		{
		case 936:  return TryDecodeGBK(p, outCode);
		case 950:  return TryDecodeBig5(p, outCode);
		case 932:  return TryDecodeSJIS(p, outCode);
		case 949:  return TryDecodeKorean(p, outCode);
		default:   return false;
		}
	}

} // namespace fonthook

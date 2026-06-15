#pragma once
#include "ITypes.h"
#include <string>
#include <Windows.h>

namespace fonthook {

// ---- Encoding identifier ----
enum class CodePage : UInt32 {
	English = 0,
	GBK = 936,
	Big5 = 950,
	SJIS = 932,
	UHC = 949,
};

// ---- Unified encoding dispatch (replaces repeated if/else-if chains) ----
bool IsLeadByte(unsigned char c);
bool IsTrailByte(unsigned char c);
bool TryDecodeDoubleByte(const char* p, UInt32& outCode);

// ---- GBK (Code Page 936) ----
bool IsGBKLeadByte(unsigned char c);
bool IsGBKTrailByte(unsigned char c);
bool TryDecodeGBK(const char* p, UInt32& outCode);

// ---- Big5 (Code Page 950) ----
bool IsBig5LeadByte(unsigned char c);
bool IsBig5TrailByte(unsigned char c);
bool TryDecodeBig5(const char* p, UInt32& outCode);

// ---- Shift-JIS (Code Page 932) ----
bool IsSJISLeadByte(unsigned char c);
bool IsSJISTrailByte(unsigned char c);
bool TryDecodeSJIS(const char* p, UInt32& outCode);

// ---- Korean/UHC (Code Page 949) ----
bool IsKoreanLeadByte(unsigned char c);
bool IsKoreanTrailByte(unsigned char c);
bool TryDecodeKorean(const char* p, UInt32& outCode);

// ---- UTF-8 ----
bool IsValidUTF8With3ByteMin(const char* s);
inline bool IsUTF8Lead3(unsigned char c) { return (c & 0xF0) == 0xE0; }
inline bool IsUTF8Trail(unsigned char c) { return (c & 0xC0) == 0x80; }

// ---- Encoding Conversion ----
std::string MultiByteToUTF8(const std::string& src, UINT32 codePage);
std::string UTF8ToMultiByteStr(const std::string& utf8, UINT32 codePage);

} // namespace fonthook

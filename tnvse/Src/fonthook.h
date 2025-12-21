#pragma once
#include <icu.h>
#include "uidecode.h"
#include "loadconfig.h"
#include <unordered_map>
#include "MemoryManager.hpp"
#include "BSFile.hpp"
#include "tnvse.h"

namespace fonthook {
    static float __stdcall VertSpacingAdjust(UInt32 aiFont) {
        return 0;
    }

    static void __cdecl ConvertToAsciiQuotes(UInt8* currentChar) {
        CdeclCall(0xA122B0, currentChar);
    }

    static bool __cdecl ReplaceVariableInString(const char* varName, char* outBuffer, UInt32 bufferSize, bool isPositiveEscape) {
        return CdeclCall<bool>(0x7070C0, varName, outBuffer, bufferSize, isPositiveEscape);
    }

    static bool __cdecl ParseAndFormatVariableInString(const char* p_varNameBuffer, void* p_parsedTextBuffer) {
        return CdeclCall<bool>(0x7073D0, p_varNameBuffer, p_parsedTextBuffer);
    }

    static SInt32 __cdecl AlignLineWidthToTab(double a1, double a2) {
        return CdeclCall<SInt32>(0xEC9130, a1, a2);
    }

    static void* __cdecl AppendToListTail(void* ListNode, void* ListNode2) {
        return ThisStdCall<void*>(0xAF25B0, ListNode, ListNode2);
    }

    // 0xEC62F6
    static UINT32 SafeDoubleToUInt32(double a1)
    {
        uint64_t bits;
        std::memcpy(&bits, &a1, sizeof(bits));

        UINT32 low = static_cast<UINT32>(bits);
        UINT32 high = static_cast<UINT32>(bits >> 32);

        if (low == 0 && (high & 0x7FFFFFFF) == 0)
            return 0;

        bool sign = (high & 0x80000000u) != 0;

        if (!sign)
        {
            return low;
        }
        else
        {
            uint64_t pair64 = (static_cast<uint64_t>(0) << 32) | low;
            return static_cast<UINT32>((pair64 + 0x7FFFFFFF) >> 32);
        }
    }

    // 0xEC62C0
    static UINT32 ConditionalFloatToUInt(double a1)
    {
        if (*(volatile UINT32*)0x01270A6C)
            return static_cast<UINT32>(a1);
        else
            return SafeDoubleToUInt32(a1);
    }

    static Float32 __stdcall FontManagerGetLinePadding(UInt32 fontID) {
        return StdCall<Float32>(0xA1B3A0, fontID);
    }

    static bool IsGBKLeadByte(unsigned char c) {
        return (c >= 0x81 && c <= 0xFE);
    }

    static bool IsGBKTrailByte(unsigned char c) {
        return (c >= 0x40 && c <= 0xFE && c != 0x7F);
    }

    static bool TryDecodeGBK(const char* p, UInt32& outCode) {
        unsigned char lead = (unsigned char)p[0];
        unsigned char trail = (unsigned char)p[1];
        if (!IsGBKLeadByte(lead) || !IsGBKTrailByte(trail))
            return false;
        outCode = (UInt32(lead) << 8) | UInt32(trail);
        return true;
    }

    static bool IsBig5LeadByte(unsigned char c) {
        return (c >= 0x81 && c <= 0xFE);
    }

    static bool IsBig5TrailByte(unsigned char c) {
        return ((c >= 0x40 && c <= 0x7E) || (c >= 0xA1 && c <= 0xFE));
    }

    static bool TryDecodeBig5(const char* p, UInt32& outCode) {
        unsigned char lead = (unsigned char)p[0];
        unsigned char trail = (unsigned char)p[1];

        if (!IsBig5LeadByte(lead) || !IsBig5TrailByte(trail))
            return false;

        outCode = (UInt32(lead) << 8) | UInt32(trail);
        return true;
    }

    static bool IsSJISLeadByte(unsigned char c) {
        if (c >= 0x81 && c <= 0x9F) return true;
        if (c >= 0xE0 && c <= 0xEA) return true;
        if (c == 0xED || c == 0xEE) return true;
        if (c >= 0xFA && c <= 0xFC) return true;
        return false;
    }

    static bool IsSJISTrailByte(unsigned char c) {
        if (c >= 0x40 && c <= 0x7E) return true;
        if (c >= 0x80 && c <= 0xFC) return true;
        return false;
    }

    static bool TryDecodeSJIS(const char* p, UInt32& outCode) {
        unsigned char lead = (unsigned char)p[0];
        unsigned char trail = (unsigned char)p[1];

        if (!IsSJISLeadByte(lead) || !IsSJISTrailByte(trail))
            return false;

        outCode = (UInt32(lead) << 8) | UInt32(trail);
        return true;
    }

    static bool IsKoreanLeadByte(unsigned char c) {
        return (c >= 0x81 && c <= 0xC8);
    }

    static bool IsKoreanTrailByte(unsigned char c) {
        if (c >= 0x41 && c <= 0x5A) return true;
        if (c >= 0x61 && c <= 0x7A) return true;
        if (c >= 0x81 && c <= 0xFE) return true;
        return false;
    }

    static bool TryDecodeKorean(const char* p, UInt32& outCode) {
        unsigned char lead = (unsigned char)p[0];
        unsigned char trail = (unsigned char)p[1];

        if (!IsKoreanLeadByte(lead) || !IsKoreanTrailByte(trail))
            return false;

        outCode = (UInt32(lead) << 8) | UInt32(trail);
        return true;
    }

    static std::string MultiByteToUTF8(const std::string& src, UINT32 codePage) {
        int len = MultiByteToWideChar(codePage, 0, src.c_str(), -1, nullptr, 0);
        if (len == 0) return "";
        std::wstring wstr(len, L'\0');
        MultiByteToWideChar(codePage, 0, src.c_str(), -1, &wstr[0], len);

        len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &utf8[0], len, nullptr, nullptr);
        return utf8;
    }

    static std::string UTF8ToMultiByteStr(const std::string& utf8, UINT32 codePage) {
        int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        if (len == 0) return "";
        std::wstring wstr(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wstr[0], len);

        len = WideCharToMultiByte(codePage, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string mb(len, '\0');
        WideCharToMultiByte(codePage, 0, wstr.c_str(), -1, &mb[0], len, nullptr, nullptr);
        return mb;
    }

    static bool IsValidUTF8With3ByteMin(const char* s)
    {
        if (!s)
            return false;

        const unsigned char* p = (const unsigned char*)s;
        bool has3ByteOrMore = false;

        while (*p)
        {
            if (*p < 0x80) {
                p++;
            }
            else if (*p < 0xC2) {
                return false;
            }
            else if (*p < 0xE0) {
                if ((p[1] & 0xC0) != 0x80)
                    return false;
                p += 2;
            }
            else if (*p < 0xF0) {
                unsigned char c2 = p[1], c3 = p[2];
                if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80)
                    return false;
                has3ByteOrMore = true;
                p += 3;
            }
            else if (*p < 0xF5) {
                unsigned char c2 = p[1], c3 = p[2], c4 = p[3];
                if ((c2 & 0xC0) != 0x80 ||
                    (c3 & 0xC0) != 0x80 ||
                    (c4 & 0xC0) != 0x80)
                    return false;
                has3ByteOrMore = true;
                p += 4;
            }
            else {
                return false;
            }
        }

        return has3ByteOrMore;
    }


    inline bool IsUTF8Lead3(unsigned char c) {
        return (c & 0xF0) == 0xE0;
    }

    inline bool IsUTF8Trail(unsigned char c) {
        return (c & 0xC0) == 0x80;
    }

    typedef UINT32(__thiscall* GetFileSizeFunc)(void* pThis);

    static UINT32 GetFileSize(void* fntFileHandle) {
        void** vtable = *(void***)fntFileHandle;
        GetFileSizeFunc func = (GetFileSizeFunc)vtable[10];
        return func(fntFileHandle);
    }

    static BSFile* __cdecl LoadFile(const char* filePath, SInt32 loadMode, UInt32 allocFlags, SInt32 openMode) {
        return CdeclCall<BSFile*>(0xAFDF20, filePath, loadMode, allocFlags, openMode);
    }

    static BSFile* FileFinder_GetFile(
        const char* apName,
        NiFile::OpenMode aeMode,
        unsigned int aiSize,
        unsigned int aiArchiveType
    ) {
        //gLog.FormattedMessage("\nCall FileFinder::GetFile");
        //gLog.FormattedMessage("apName: %s", apName);
        //gLog.FormattedMessage("aiSize: %u", aiSize);
        BSFile* ret = CdeclCall<BSFile*>(0xAFDF20, apName, aeMode, aiSize, aiArchiveType);
        //gLog.FormattedMessage("FileFinder::GetFile End\n");
        return ret;
    }

    static NiPoint3& StringDefaultDimensions = *reinterpret_cast<NiPoint3*>(0x11F426C);

    static MemoryManager* MemoryManager_s_Instance = reinterpret_cast<MemoryManager*>(0x11F6238);

    static std::string fontNameKey;

    static std::unordered_map<std::string, std::unordered_map<UInt32, FontLetter>> gExtraFontLetters;

    static std::unordered_map<UInt32, std::unordered_map<UInt32, FontLetter>> gNumberedExtraLetters;

    static unsigned char pFirstChar;
    static bool bIsQuestTextMSBDBCharacter;
    static bool bIsQuestTextLSBDBCharacter;
    static char szDBChar[3];
    static bool bProcessingUTF8QueneText;
    static bool bIsQuestTextUTF8FirstChar;
    static bool bIsQuestTextUTF8SecondChar;
    static bool bIsQuestTextUTF8ThirdChar;
    static char szUTF8Char[4];

    static bool bHasLeadByteInLast;
    static unsigned char lastHanziByte;

    void InitBigGunsDescHooks();
    void InitDoorPromptHooks();
    void InitPluralHooks();
	void InitVertSpacingHook();
	void InitFontHook();
}
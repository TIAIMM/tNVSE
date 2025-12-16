#pragma once
#include "uidecode.h"
#include <unordered_map>
#include "MemoryManager.hpp"
#include "BSFile.hpp"

namespace fonthook {
	class ExtraFontData {
	public:

	};

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
    static uint32_t SafeDoubleToUInt32(double a1)
    {
        uint64_t bits;
        std::memcpy(&bits, &a1, sizeof(bits));

        uint32_t low = static_cast<uint32_t>(bits);
        uint32_t high = static_cast<uint32_t>(bits >> 32);

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
            return static_cast<uint32_t>((pair64 + 0x7FFFFFFF) >> 32);
        }
    }

    // 0xEC62C0
    static uint32_t ConditionalFloatToUInt(double a1)
    {
        if (*(volatile uint32_t*)0x01270A6C)
            return static_cast<uint32_t>(a1);
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

    typedef uint32_t(__thiscall* GetFileSizeFunc)(void* pThis);

    static uint32_t GetFileSize(void* fntFileHandle) {
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

    static HMODULE hJIP = 0;

    static size_t __fastcall GetJIPAddress(size_t aiAddress) {
        return reinterpret_cast<size_t>(hJIP) + aiAddress - 0x10000000;
    }

    static std::string fontNameKey;

    static std::unordered_map<std::string, std::unordered_map<UInt32, FontLetter>> gExtraFontLetters;

    static std::unordered_map<UInt32, std::unordered_map<UInt32, FontLetter>> gNumberedExtraLetters;

    static unsigned char pFirstChar;
    static bool bIsQuestTextMSBHanzi;
    static bool bIsQuestTextLSBHanzi;
    static char szGBKChar[3];

    static bool bHasLeadByteInLast;
    static unsigned char lastHanziByte;

	void InitVertSpacingHook();
	void InitFontHook();
	void InitJIPHooks();
}
#include "native_calls.h"
#include "MemoryManager.hpp"

namespace fonthook {

void __cdecl ConvertToAsciiQuotes(UInt8* currentChar) {
    CdeclCall(0xA122B0, currentChar);
}

bool __cdecl ReplaceVariableInString(const char* varName, char* outBuffer, UInt32 bufferSize, bool isPositiveEscape) {
    return CdeclCall<bool>(0x7070C0, varName, outBuffer, bufferSize, isPositiveEscape);
}

bool __cdecl ParseAndFormatVariableInString(const char* p_varNameBuffer, void* p_parsedTextBuffer) {
    return CdeclCall<bool>(0x7073D0, p_varNameBuffer, p_parsedTextBuffer);
}

SInt32 __cdecl AlignLineWidthToTab(double a1, double a2) {
    return CdeclCall<SInt32>(0xEC9130, a1, a2);
}

void* __cdecl AppendToListTail(void* ListNode, void* ListNode2) {
    return ThisStdCall<void*>(0xAF25B0, ListNode, ListNode2);
}

UINT32 SafeDoubleToUInt32(double a1)
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

UINT32 ConditionalFloatToUInt(double a1)
{
    if (*(volatile UINT32*)0x01270A6C)
        return static_cast<UINT32>(a1);
    else
        return SafeDoubleToUInt32(a1);
}

Float32 __stdcall FontManagerGetLinePadding(UInt32 fontID) {
    return StdCall<Float32>(0xA1B3A0, fontID);
}

UINT32 GetFileSize(void* fntFileHandle) {
    void** vtable = *(void***)fntFileHandle;
    typedef UINT32(__thiscall* GetFileSizeFunc)(void* pThis);
    GetFileSizeFunc func = (GetFileSizeFunc)vtable[10];
    return func(fntFileHandle);
}

BSFile* __cdecl LoadFile(const char* filePath, SInt32 loadMode, UInt32 allocFlags, SInt32 openMode) {
    return CdeclCall<BSFile*>(0xAFDF20, filePath, loadMode, allocFlags, openMode);
}

BSFile* FileFinder_GetFile(
    const char* apName,
    NiFile::OpenMode aeMode,
    unsigned int aiSize,
    unsigned int aiArchiveType
) {
    return CdeclCall<BSFile*>(0xAFDF20, apName, aeMode, aiSize, aiArchiveType);
}

// ---- Global singletons ----
MemoryManager* MemoryManager_s_Instance = reinterpret_cast<MemoryManager*>(0x11F6238);
NiPoint3& StringDefaultDimensions = *reinterpret_cast<NiPoint3*>(0x11F426C);

} // namespace fonthook

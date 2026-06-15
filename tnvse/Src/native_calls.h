#pragma once
#include "ui_decode.h"
#include "BSFile.hpp"
#include "MemoryManager.hpp"

namespace fonthook {

// ---- Native game function wrappers ----
void __cdecl ConvertToAsciiQuotes(UInt8* currentChar);
bool __cdecl ReplaceVariableInString(const char* varName, char* outBuffer, UInt32 bufferSize, bool isPositiveEscape);
bool __cdecl ParseAndFormatVariableInString(const char* p_varNameBuffer, void* p_parsedTextBuffer);
SInt32 __cdecl AlignLineWidthToTab(double a1, double a2);
void* __cdecl AppendToListTail(void* ListNode, void* ListNode2);

// ---- Float/int conversion helpers ----
UINT32 SafeDoubleToUInt32(double a1);
UINT32 ConditionalFloatToUInt(double a1);

// ---- Font/FontManager helpers ----
Float32 __stdcall FontManagerGetLinePadding(UInt32 fontID);
UINT32 GetFileSize(void* fntFileHandle);
BSFile* __cdecl LoadFile(const char* filePath, SInt32 loadMode, UInt32 allocFlags, SInt32 openMode);
BSFile* FileFinder_GetFile(const char* apName, NiFile::OpenMode aeMode, unsigned int aiSize, unsigned int aiArchiveType);

// ---- Global singletons ----
extern MemoryManager* MemoryManager_s_Instance;
extern NiPoint3& StringDefaultDimensions;

} // namespace fonthook

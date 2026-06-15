#pragma once
#include "BSFile.hpp"
#include "MemoryManager.hpp"
#include "ui_decode.h"

namespace fonthook {

// ---- Native game function wrappers ----
void __cdecl ConvertToAsciiQuotes(UInt8* currentChar);
bool __cdecl ReplaceVariableInString(const char* varName, char* outBuffer, UInt32 bufferSize, bool isPositiveEscape);
bool __cdecl ParseAndFormatVariableInString(const char* p_varNameBuffer, void* p_parsedTextBuffer);
SInt32 __cdecl AlignLineWidthToTab(double currentWidth, double tabInterval);
void* __cdecl AppendToListTail(void* listNode, void* itemData);

// ---- Float/int conversion helpers ----
UINT32 SafeDoubleToUInt32(double value);
UINT32 ConditionalFloatToUInt(double value);

// ---- Font/FontManager helpers ----
Float32 __stdcall FontManagerGetLinePadding(UInt32 fontID);
BSFile* FileFinder_GetFile(const char* apName, NiFile::OpenMode aeMode, unsigned int aiSize, unsigned int aiArchiveType);

// ---- Global singletons ----
extern MemoryManager* MemoryManager_s_Instance;
extern NiPoint3& StringDefaultDimensions;

// ---- Inline helpers ----
inline int MaxInt(int a, int b) { return a >= b ? a : b; }
inline float MaxFloat(float a, float b) { return a >= b ? a : b; }
inline float MinFloat(float a, float b) { return a <= b ? a : b; }
inline int MinInt(int a, int b) { return a <= b ? a : b; }

} // namespace fonthook

#pragma once
#include <unordered_map>
#include "ui_decode.h"
#include "encoding.h"
#include "load_config.h"
#include "MemoryManager.hpp"
#include "BSFile.hpp"

namespace fonthook {

// ---- Forward-declared globals (defined in fonthook.cpp) ----
extern MemoryManager* MemoryManager_s_Instance;
extern NiPoint3& StringDefaultDimensions;
extern std::string fontNameKey;
extern std::unordered_map<std::string, std::unordered_map<UInt32, FontLetter>> gExtraFontLetters;
extern std::unordered_map<UInt32, std::unordered_map<UInt32, FontLetter>> gNumberedExtraLetters;

// ---- Native function helpers (defined in nativecalls.cpp) ----
void __cdecl ConvertToAsciiQuotes(UInt8* currentChar);
bool __cdecl ReplaceVariableInString(const char* varName, char* outBuffer, UInt32 bufferSize, bool isPositiveEscape);
bool __cdecl ParseAndFormatVariableInString(const char* p_varNameBuffer, void* p_parsedTextBuffer);
SInt32 __cdecl AlignLineWidthToTab(double a1, double a2);
void* __cdecl AppendToListTail(void* ListNode, void* ListNode2);
UINT32 SafeDoubleToUInt32(double a1);
UINT32 ConditionalFloatToUInt(double a1);
Float32 __stdcall FontManagerGetLinePadding(UInt32 fontID);
UINT32 GetFileSize(void* fntFileHandle);
BSFile* __cdecl LoadFile(const char* filePath, SInt32 loadMode, UInt32 allocFlags, SInt32 openMode);
BSFile* FileFinder_GetFile(const char* apName, NiFile::OpenMode aeMode, unsigned int aiSize, unsigned int aiArchiveType);

// ---- FontEx - Extended font class with multi-byte support ----
class FontEx : public Font {
public:
    Font* FontInit(int iFontNum, char* apFilename, bool abLoad);
    void Load();
    void __thiscall PrepTextForTerminal(const char* apOrigString, Font::TextData* axData);
    void __thiscall PrepText(const char* apOrigString, Font::TextData* axData);
    UInt32 CreateText(
        BSStringT<char>* axTextString, int* aiWidth, int* aiHeight,
        int aiLineStart, int aiLineEnd, int aiFlags, char aiLineBreakChar,
        const NiColorA* axFontColor, UINT32** apTextShape, UINT32** apIconShape);
    UInt32* MakeString(
        float afStartX, float afStartY, float afZ,
        BSStringT<char>* apTextString, int* aiWidth, bool abPrepareObject,
        const NiColorA* arg1C, bool abUpperLeftCorner, bool abPrepareObject_1);
};

} // namespace fonthook

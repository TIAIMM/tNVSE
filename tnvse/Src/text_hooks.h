#pragma once
#include <unordered_map>
#include "ui_decode.h"
#include "encoding.h"
#include "load_config.h"

namespace fonthook {

// Forward-declared globals (defined in fonthook.cpp)
extern std::unordered_map<UInt32, std::unordered_map<UInt32, FontLetter>> gNumberedExtraLetters;

// Quest text state globals
extern unsigned char pFirstChar;
extern bool bIsQuestTextMSBDBCharacter;
extern bool bIsQuestTextLSBDBCharacter;
extern char szDBChar[3];

// ---- Quest/Location Text Hooks ----
void* __fastcall TileSetStringHookForQueueText(void* pThis, void*, int a2, char* a3, bool a4);

// ---- UTF-8 Conversion Hooks ----
char* __fastcall BSString_c_strHook(BSStringT<char>* pthis, void*);
char* __fastcall BSString_GetCStringOrEmptyHook(BSStringT<char>* pthis, void*);
int __cdecl strcpy_sHook(char* dest, int dest_size, const char* src);

// ---- Door Prompt Hooks ----
int BSsprintfHookCHS(
    char* buffer, size_t sizeOfBuffer,
    const char* sformat, const char* sDst,
    const char* sTo, const char* sCellName);

int BSsprintfHookKOR(
    char* buffer, size_t sizeOfBuffer,
    const char* sformat, const char* sDst,
    const char* sTo, const char* sCellName);

} // namespace fonthook

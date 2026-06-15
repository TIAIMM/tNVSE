#pragma once
#include <icu.h>
#include <unordered_map>
#include "ui_decode.h"
#include "load_config.h"
#include "MemoryManager.hpp"
#include "BSFile.hpp"
#include "tnvse.h"

// ---- Aggregated sub-module includes ----
#include "encoding.h"
#include "native_calls.h"
#include "font_engine.h"
#include "font_manager.h"
#include "text_hooks.h"
#include "game_hooks.h"

namespace fonthook {

// ---- Vertical spacing adjust (currently unused) ----
static float __stdcall VertSpacingAdjust(UInt32 aiFont) {
    return 0;
}

// ---- Global state: Font letter caches ----
extern std::string fontNameKey;
extern std::unordered_map<std::string, std::unordered_map<UInt32, FontLetter>> gExtraFontLetters;
extern std::unordered_map<UInt32, std::unordered_map<UInt32, FontLetter>> gNumberedExtraLetters;

// ---- Global state: Quest text double-byte character state machine ----
extern unsigned char pFirstChar;
extern bool bIsQuestTextMSBDBCharacter;
extern bool bIsQuestTextLSBDBCharacter;
extern char szDBChar[3];

// ---- Global state: Quest text UTF-8 state machine ----
extern bool bProcessingUTF8QueneText;
extern bool bIsQuestTextUTF8FirstChar;
extern bool bIsQuestTextUTF8SecondChar;
extern bool bIsQuestTextUTF8ThirdChar;
extern char szUTF8Char[4];

// ---- Global state: Line break helpers ----
extern bool bHasLeadByteInLast;
extern unsigned char lastHanziByte;

} // namespace fonthook

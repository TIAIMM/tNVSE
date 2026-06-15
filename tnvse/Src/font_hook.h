#pragma once
#include "BSFile.hpp"
#include "load_config.h"
#include "MemoryManager.hpp"
#include "tnvse.h"
#include "ui_decode.h"
#include <icu.h>
#include <unordered_map>

// ---- Aggregated sub-module includes ----
#include "encoding.h"
#include "font_engine.h"
#include "font_manager.h"
#include "game_hooks.h"
#include "native_calls.h"
#include "text_hooks.h"

namespace fonthook
{
	// ---- Vertical spacing adjust (currently unused) ----
	static float __stdcall VertSpacingAdjust(UInt32 aiFont)
	{
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

} // namespace fonthook

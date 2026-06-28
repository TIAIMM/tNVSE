#pragma once
#include "MemoryManager.hpp"
#include "NiPoint3.hpp"
#include "ui_decode.h"
#include <string>
#include <unordered_map>

namespace fonthook
{
	// ---- Font letter caches (defined in font_hook.cpp) ----
	extern std::string fontNameKey;
	extern std::unordered_map<std::string, std::unordered_map<UInt32, FontLetter>> gExtraFontLetters;
	extern std::unordered_map<UInt32, std::unordered_map<UInt32, FontLetter>> gNumberedExtraLetters;

	// ---- Quest text double-byte character state machine (defined in font_hook.cpp) ----
	extern UInt8 pFirstChar;
	extern bool bIsQuestTextMSBDBCharacter;
	extern bool bIsQuestTextLSBDBCharacter;
	extern bool bMeasureQuestTextMSBAsEmpty;
	extern char szDBChar[3];

	// ---- Memory / rendering singletons (defined in native_calls.cpp) ----
	extern MemoryManager* MemoryManager_s_Instance;
	extern NiPoint3& StringDefaultDimensions;

} // namespace fonthook

#pragma once

#include "internal/text.h"

//From JIP, JGNVSE, Stewie, Wall & Confused


// From JG
//0x11F33F8
// From Modern Minimap
//0x5BD5B0
__forceinline static FontManager* GetSingleton() { return *(FontManager**)0x11F33F8; }

//From Stewie Tweaks
void FontTextReplaced::GetVariableEscapedText(const char* input) { Font__CheckForVariablesInText(FontManager::GetSingleton()->fontInfos[0], input, this); }
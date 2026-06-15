#pragma once
#include "BSFile.hpp"
#include "encoding.h"
#include "load_config.h"
#include "MemoryManager.hpp"
#include "ui_decode.h"
#include <unordered_map>

namespace fonthook
{
	// ---- Forward-declared globals (defined in font_hook.cpp) ----
	extern MemoryManager* MemoryManager_s_Instance;
	extern NiPoint3& StringDefaultDimensions;
	extern std::string fontNameKey;
	extern std::unordered_map<std::string, std::unordered_map<UInt32, FontLetter>> gExtraFontLetters;
	extern std::unordered_map<UInt32, std::unordered_map<UInt32, FontLetter>> gNumberedExtraLetters;

	// ---- FontEx - Extended font class with multi-byte support ----
	class FontEx : public Font
	{
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

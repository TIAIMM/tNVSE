#pragma once
#include "BSFile.hpp"
#include "encoding.h"
#include "globals.h"
#include "load_config.h"
#include "ui_decode.h"
#include <unordered_map>

namespace fonthook
{
	// ---- Constants ----
	static constexpr UInt32 kFontDataSize = 0x3928;
	static constexpr UInt32 kMaxGlyphCount = 256;
	static constexpr UInt32 kSentinelMax = 0x7FFFFFFF;
	static constexpr int kTabWidth = 75;
	static constexpr UInt8 kSpaceChar = ' ';
	static constexpr UInt8 kNBSPChar = 160;
	static constexpr UInt8 kDelChar = 127;
	static constexpr UInt8 kPipeChar = '|';

	// ---- FontEx - Extended font class with multi-byte support ----
	class FontEx : public Font
	{
	public:
		Font* FontConstructor(int iFontNum, char* apFilename, bool abLoad);
		void Load();
		void __thiscall PrepTextForTerminal(const char* apOrigString, Font::TextData* axData);
		void __thiscall PrepText(const char* apOrigString, Font::TextData* axData);
		void __thiscall CreateText(
			BSStringT<char>* axTextString, int* aiWidth, int* aiHeight,
			int aiLineStart, int aiLineEnd, int aiFlags, char aiLineBreakChar,
			const NiColorA* axFontColor, NiTriShape** apTextShape, NiTriShape** apIconShape);
		// Low nibble is justification (1 left, 2 center, 4 right), not a boolean.
		NiAVObject* MakeString(
			float afStartX, float afStartY, float afZ,
			BSStringT<char>* apTextString, int* aiWidth, UInt32 aiFlags,
			const NiColorA* arg1C, bool abUpperLeftCorner, bool abPrepareObject_1);
		void __thiscall TextDocRenderAddChar(
			FontLetter* apLetter, int aiVert, NiTriShape* apShape,
			NiPoint3* apPosition, const NiColorA* apColor);

	private:
		void LoadExtraGlyphs(BSFile* fontFile, UInt32* textureMarkers);
	};

	// Runs the same direct encoded-unit layout used by FreeType CreateText
	// without calling the game's FontLetter measurement routine. It supports both
	// Windows-1252 and complete DBCS units. startCharIndex remains a byte index.
	bool MeasureFreeTypeSingleByteText(
		FontEx* font,
		const char* text,
		float maxWrapWidth,
		UInt32 startCharIndex,
		NiPoint3& dimensions);

} // namespace fonthook

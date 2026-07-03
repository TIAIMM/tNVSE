#pragma once

#include "globals.h"
#include "native_calls.h"

namespace fonthook
{
	using ExtraGlyphMap = std::unordered_map<UInt32, FontLetter>;

	inline bool HasAnyExtraGlyphs()
	{
		return !gNumberedExtraLetters.empty();
	}

	inline ExtraGlyphMap* GetExtraGlyphs(int fontNum)
	{
		auto it = gNumberedExtraLetters.find(fontNum);
		return it != gNumberedExtraLetters.end() ? &it->second : nullptr;
	}

	inline bool HasExtraGlyphsForFont(int fontNum)
	{
		return GetExtraGlyphs(fontNum) != nullptr;
	}

	inline FontLetter* LookupDBGlyph(ExtraGlyphMap* extraGlyphs, UInt32 code)
	{
		if (!extraGlyphs)
			return nullptr;

		auto it = extraGlyphs->find(code);
		return it != extraGlyphs->end() ? &it->second : nullptr;
	}

	inline UInt32 GetGlyphLayoutWidth(const FontLetter* glyph)
	{
		return glyph ? ConditionalFloatToUInt(glyph->fWidth + glyph->fSpacing) : 0;
	}
}

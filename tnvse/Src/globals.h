#pragma once
#include "MemoryManager.hpp"
#include "NiPoint3.hpp"
#include "ui_decode.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace fonthook
{
	using ExtraGlyphMap = std::unordered_map<UInt32, FontLetter>;

	// Serialized .fnt files store one dense FontLetter row for every lead byte
	// from 0x81 through 0xFE. Keep that immutable fallback representation dense;
	// generated FreeType metrics remain sparse and are cached separately.
	struct SerializedExtraGlyphTable
	{
		static constexpr UInt32 kFirstLeadByte = 0x81;
		static constexpr UInt32 kLastLeadByte = 0xFE;
		static constexpr UInt32 kFirstTrailByte = 0x40;
		static constexpr UInt32 kLastTrailByte = 0xFE;
		static constexpr UInt32 kGlyphsPerRow =
			kLastTrailByte - kFirstTrailByte + 1;
		static constexpr UInt32 kGlyphCount =
			(kLastLeadByte - kFirstLeadByte + 1) * kGlyphsPerRow;

		std::unique_ptr<FontLetter[]> glyphs;
		UInt32 validGlyphs = 0;

		bool empty() const
		{
			return !glyphs || !validGlyphs;
		}

		void clear()
		{
			glyphs.reset();
			validGlyphs = 0;
		}

		FontLetter* find(UInt32 code) const
		{
			if (!glyphs || (code & 0xFFFF0000u))
				return nullptr;
			const UInt32 lead = (code >> 8) & 0xFFu;
			const UInt32 trail = code & 0xFFu;
			if (lead < kFirstLeadByte || lead > kLastLeadByte
				|| trail < kFirstTrailByte || trail > kLastTrailByte)
			{
				return nullptr;
			}
			const UInt32 index = (lead - kFirstLeadByte) * kGlyphsPerRow
				+ (trail - kFirstTrailByte);
			return index < validGlyphs ? &glyphs[index] : nullptr;
		}
	};

	struct ExtraGlyphStore
	{
		SerializedExtraGlyphTable serialized;
		ExtraGlyphMap generated;

		bool empty() const
		{
			return serialized.empty() && generated.empty();
		}
	};

	// ---- Font letter caches (defined in font_hook.cpp) ----
	extern std::string fontNameKey;
	extern std::unordered_map<std::string, ExtraGlyphStore> gExtraFontLetters;
	extern std::unordered_map<UInt32, ExtraGlyphStore> gNumberedExtraLetters;

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

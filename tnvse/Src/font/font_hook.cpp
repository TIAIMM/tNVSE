#include "font_hook.h"

namespace fonthook
{
	// ---- Global state variables ----
	std::unordered_map<std::string, ExtraGlyphStore> gExtraFontLetters;
	std::unordered_map<UInt32, ExtraGlyphStore> gNumberedExtraLetters;

	// Quest text state machine variables
	UInt8 pFirstChar;
	bool bIsQuestTextMSBDBCharacter;
	bool bIsQuestTextLSBDBCharacter;
	bool bMeasureQuestTextMSBAsEmpty;
	char szDBChar[3];

} // namespace fonthook

#include "font_hook.h"

namespace fonthook
{
	// ---- Global state variables ----
	std::string fontNameKey;

	std::unordered_map<std::string, std::unordered_map<UInt32, FontLetter>> gExtraFontLetters;
	std::unordered_map<UInt32, std::unordered_map<UInt32, FontLetter>> gNumberedExtraLetters;

	// Quest text state machine variables
	unsigned char pFirstChar;
	bool bIsQuestTextMSBDBCharacter;
	bool bIsQuestTextLSBDBCharacter;
	char szDBChar[3];

} // namespace fonthook

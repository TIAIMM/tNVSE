#pragma once
#include <unordered_map>
#include "ui_decode.h"
#include "encoding.h"
#include "load_config.h"

namespace fonthook {

// Forward-declared globals (defined in fonthook.cpp)
extern std::unordered_map<UInt32, std::unordered_map<UInt32, FontLetter>> gNumberedExtraLetters;

// Quest text state globals (defined in fonthook.cpp)
extern unsigned char pFirstChar;
extern bool bIsQuestTextMSBDBCharacter;
extern bool bIsQuestTextLSBDBCharacter;
extern char szDBChar[3];
extern bool bProcessingUTF8QueneText;
extern bool bIsQuestTextUTF8FirstChar;
extern bool bIsQuestTextUTF8SecondChar;
extern bool bIsQuestTextUTF8ThirdChar;
extern char szUTF8Char[4];
extern bool bHasLeadByteInLast;
extern unsigned char lastHanziByte;

class FontManagerEx : public FontManager {
public:
    // outDims.x := width (pxl); outDims.y := height (pxl); outDims.z := numLines
    NiPoint3* __thiscall CalculateStringDimensions(NiPoint3* outDimensions, const char* srcString, UInt32 fontID, float maxWrapWidth, UInt32 startCharIndex);
    UINT32* PrepText(BSStringT<char>* a7, int a3);
};

} // namespace fonthook

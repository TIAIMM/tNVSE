#include "font_manager.h"
#include "native_calls.h"

namespace fonthook {

NiPoint3* __thiscall FontManagerEx::CalculateStringDimensions(NiPoint3* outDimensions, const char* srcString, UInt32 fontID, float maxWrapWidth, UInt32 startCharIndex) {
    double tabStopWidth;
    float finalMaxLineWidth;
    float adjustedWrapWidth;
    float previousLineWidthAtTabStop;
    unsigned __int8 currentChar;
    float currentCharTotalWidth;
    signed int currentCharIndex;
    float previousCharTotalWidth;
    int totalLines;
    char hasHyphenationPoint;
    NiPoint3 StringDimensions;
    float lastValidWrapPosition;
    float currentLineWidth;
    int sourceStringLength;
    FontLetter* fontCharMetrics;
    float fontVerticalSpacingAdjust;

    bool bIsDBCharacter;
    UInt32 uiDoubleByteCode;
    auto extraGlyphEntry = gNumberedExtraLetters.find(fontID);
    auto* extraGlyphs = extraGlyphEntry != gNumberedExtraLetters.end() ? &extraGlyphEntry->second : nullptr;

    std::string sCurrentStr, sConvertedStr;

    if (g_bEnableUTF8 && g_uiEncoding != 0 && extraGlyphs) {
        if (IsValidUTF8With3ByteMin(srcString)) {
            sCurrentStr = srcString;
            sConvertedStr = UTF8ToMultiByteStr(sCurrentStr, g_usingWinEncoding);
            srcString = sConvertedStr.c_str();
        }
    }

    if (fontID >= 1 && srcString)
    {
        StringDimensions = StringDefaultDimensions;
        sourceStringLength = strlen(srcString);
        fontCharMetrics = this->pFont[fontID - 1]->pFontData->pFontLetters;
        lastValidWrapPosition = 0.0;
        currentLineWidth = 0.0;
        fontVerticalSpacingAdjust = FontManagerGetLinePadding(fontID);
        previousCharTotalWidth = 0.0;
        hasHyphenationPoint = 0;
        totalLines = 1;
        StringDimensions.y = fontCharMetrics[' '].fHeight;

        for (currentCharIndex = startCharIndex; currentCharIndex < sourceStringLength; ++currentCharIndex)
        {
            bIsDBCharacter = false;

            currentChar = srcString[currentCharIndex];
            currentCharTotalWidth = 0.0;

            if (extraGlyphs) {
                if (bIsQuestTextMSBDBCharacter) {
                    if (szDBChar) {
                        srcString = szDBChar;
                    }
                }

                if ((currentCharIndex + 1) <= sourceStringLength) {
                    bIsDBCharacter = TryDecodeDoubleByte(&srcString[currentCharIndex], uiDoubleByteCode);

                    if (bIsQuestTextMSBDBCharacter) {
                        srcString = "";
                    }
                }
            }

            if (bIsDBCharacter) {
                auto glyphIt = extraGlyphs->find(uiDoubleByteCode);
                if (glyphIt != extraGlyphs->end()) {
                    currentCharTotalWidth = glyphIt->second.fLeadingEdge
                        + glyphIt->second.fWidth
                        + glyphIt->second.fSpacing;
                }
                ++currentCharIndex;
            }
            else {
                ConvertToAsciiQuotes(&currentChar);
                currentCharTotalWidth = fontCharMetrics[currentChar].fLeadingEdge
                    + fontCharMetrics[currentChar].fWidth
                    + fontCharMetrics[currentChar].fSpacing;
                switch (currentChar)
                {
                case '\t':
                    tabStopWidth = currentLineWidth;
                    AlignLineWidthToTab(currentLineWidth, 75.0);
                    previousLineWidthAtTabStop = tabStopWidth;
                    currentCharTotalWidth = 75.0 - previousLineWidthAtTabStop;
                    break;
                case '\n':
                    lastValidWrapPosition = currentLineWidth;
                    hasHyphenationPoint = 0;
                    break;
                case ' ':
                    break;
                case '~':
                    lastValidWrapPosition = currentLineWidth;
                    hasHyphenationPoint = 1;
                    break;
                default:
                    break;
                }
            }

            if (currentChar != '~')
                currentLineWidth = currentLineWidth + currentCharTotalWidth;
            if (maxWrapWidth < currentLineWidth || currentChar == '\n')
            {
                if (lastValidWrapPosition <= 0.0)
                {
                    lastValidWrapPosition = currentLineWidth
                        - currentCharTotalWidth
                        - previousCharTotalWidth;
                    currentLineWidth = currentCharTotalWidth + previousCharTotalWidth;
                }
                else
                {
                    currentLineWidth = currentLineWidth - lastValidWrapPosition;
                    if (!hasHyphenationPoint)
                    {
                        if (currentChar == '\n')
                            currentLineWidth = 0.0;
                    }
                }
                if (lastValidWrapPosition >= StringDimensions.x)
                    adjustedWrapWidth = lastValidWrapPosition;
                else
                    adjustedWrapWidth = StringDimensions.x;
                StringDimensions.x = adjustedWrapWidth;
                StringDimensions.y = fontVerticalSpacingAdjust
                    + this->pFont[fontID - 1]->pFontData->fBaseLine
                    + StringDimensions.y;
                lastValidWrapPosition = 0.0;
                ++totalLines;
            }
            previousCharTotalWidth = currentCharTotalWidth;
        }
        if (currentLineWidth >= StringDimensions.x)
            finalMaxLineWidth = currentLineWidth;
        else
            finalMaxLineWidth = StringDimensions.x;
        StringDimensions.z = totalLines;
        outDimensions->x = finalMaxLineWidth;
        outDimensions->y = StringDimensions.y;
        outDimensions->z = StringDimensions.z;
        return outDimensions;
    }
    else
    {
        *outDimensions = StringDefaultDimensions;
        return outDimensions;
    }
}

UINT32* FontManagerEx::PrepText(BSStringT<char>* a7, int a3) {
    return ThisStdCall<UINT32*>(0xA18A30, this, a7, a3);
}

} // namespace fonthook

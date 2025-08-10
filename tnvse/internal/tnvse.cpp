#pragma once
#include <cstring>
#include "SafeWrite.h"
#include "Game/uidecode.h"
#include "tnvse.h"

namespace tNVSE {

	static Float32 __stdcall VertSpacingAdjust(UInt32 fontID) {
		return 0;
	}

    static UInt8* __cdecl ConvertToAsciiQuotes(UInt8* currentChar) {
        return CdeclCall<UInt8*>(0xA122B0);
    }

    static UInt32 __cdecl AlignLineWidthToTab(double a1, double a2) {
        return CdeclCall<UInt32>(0xEC9130);
    }

    static inline bool IsGB2312LeadByte(unsigned char c) {
        return (c >= 0xA1 && c <= 0xF7);
    }

    inline NiVector3& StringDefaulDimensions = *reinterpret_cast<NiVector3*>(0x11F426C);

    class FontInfoEx : public FontInfo {
    public:
        void __thiscall ProcessTextLayoutEx(char* a2, FontTextReplaced* textParams) {
        }
    };

	class FontManagerEx : public FontManager {
	public:
        inline float GetHanziWidth(uint16_t gbChar, FontInfo::CharDimensions* fontCharMetrics) {
			// Check if the character is a valid GB2312 character
            if (fontCharMetrics[gbChar].width > 0) {
                return fontCharMetrics[gbChar].flt2C
                    + fontCharMetrics[gbChar].width
                    + fontCharMetrics[gbChar].widthMod;
            }

			// default space character width if the character is not found
            return (fontCharMetrics[' '].flt2C
                + fontCharMetrics[' '].width
                + fontCharMetrics[' '].widthMod);
        }

        //	outDims.x := width (pxl); outDims.y := height (pxl); outDims.z := numLines
		NiVector3* __thiscall GetStringDimensionsEx(NiVector3* outDimensions, const char* srcString, uint32_t fontID, float maxWrapWidth, uint32_t startCharIndex) {
            double tabStopWidth; // st7
            float finalMaxLineWidth; // [esp+10h] [ebp-4Ch]
            float adjustedWrapWidth; // [esp+14h] [ebp-48h]
            float previousLineWidthAtTabStop; // [esp+18h] [ebp-44h]
            unsigned __int8 currentChar; // [esp+27h] [ebp-35h] BYREF
            float currentCharTotalWidth; // [esp+28h] [ebp-34h]
            signed int currentCharIndex; // [esp+2Ch] [ebp-30h]
            float previousCharTotalWidth; // [esp+30h] [ebp-2Ch]
            int totalLines; // [esp+34h] [ebp-28h]
            char hasHyphenationPoint; // [esp+3Bh] [ebp-21h]
            NiVector3 StringDimensions; // [esp+3Ch] [ebp-20h]
            float lastValidWrapPosition; // [esp+48h] [ebp-14h]
            float currentLineWidth; // [esp+4Ch] [ebp-10h]
            int sourceStringLength; // [esp+50h] [ebp-Ch]
            FontInfo::CharDimensions* fontCharMetrics; // [esp+54h] [ebp-8h]
            float fontVerticalSpacingAdjust; // [esp+58h] [ebp-4h]

            if (fontID >= 1 && fontID <= 8 && srcString)
            {
                StringDimensions = StringDefaulDimensions;
                sourceStringLength = strlen(srcString);
                fontCharMetrics = this->fontInfos[fontID - 1]->bufferData->charDimensions;
                lastValidWrapPosition = 0.0;
                currentLineWidth = 0.0;
                fontVerticalSpacingAdjust = VertSpacingAdjust(fontID);
                previousCharTotalWidth = 0.0;
                hasHyphenationPoint = 0;
                totalLines = 1;
                StringDimensions.y = fontCharMetrics[' '].height;
                for (currentCharIndex = startCharIndex; currentCharIndex < sourceStringLength; ++currentCharIndex)
                {
                    currentChar = srcString[currentCharIndex];
                    currentCharTotalWidth = 0.0;
                    ConvertToAsciiQuotes(&currentChar);
                    currentCharTotalWidth = fontCharMetrics[currentChar].flt2C
                        + fontCharMetrics[currentChar].width
                        + fontCharMetrics[currentChar].widthMod;
                    switch (currentChar)
                    {
                    case '\t':
                        tabStopWidth = currentLineWidth;
                        AlignLineWidthToTab(currentLineWidth, 75.0);
                        previousLineWidthAtTabStop = tabStopWidth;
                        currentCharTotalWidth = 75.0 - previousLineWidthAtTabStop;
                        break;
                    case '\n':
                    case ' ':
                        lastValidWrapPosition = currentLineWidth;
                        hasHyphenationPoint = 0;
                        break;
                    case '~':
                        lastValidWrapPosition = currentLineWidth
                            + fontCharMetrics['-'].flt2C
                            + fontCharMetrics['-'].width
                            + fontCharMetrics['-'].widthMod;
                        hasHyphenationPoint = 1;
                        break;
                    default:
                        break;
                    }
                    if (currentChar != '~')
                        currentLineWidth = currentLineWidth + currentCharTotalWidth;
                    if (maxWrapWidth < currentLineWidth || currentChar == '\n')
                    {
                        if (lastValidWrapPosition <= 0.0)
                        {
                            lastValidWrapPosition = currentLineWidth
                                - currentCharTotalWidth
                                - previousCharTotalWidth
                                + fontCharMetrics['-'].flt2C
                                + fontCharMetrics['-'].width
                                + fontCharMetrics['-'].widthMod;
                            currentLineWidth = currentCharTotalWidth + previousCharTotalWidth;
                        }
                        else
                        {
                            currentLineWidth = currentLineWidth - lastValidWrapPosition;
                            if (!hasHyphenationPoint)
                            {
                                if (currentChar == '\n')
                                    currentLineWidth = 0.0;
                                else
                                    currentLineWidth = currentLineWidth
                                    - (fontCharMetrics[' '].flt2C
                                        + fontCharMetrics[' '].width
                                        + fontCharMetrics[' '].widthMod);
                            }
                        }
                        if (lastValidWrapPosition >= StringDimensions.x)
                            adjustedWrapWidth = lastValidWrapPosition;
                        else
                            adjustedWrapWidth = StringDimensions.x;
                        StringDimensions.x = adjustedWrapWidth;
                        StringDimensions.y = fontVerticalSpacingAdjust
                            + this->fontInfos[fontID - 1]->bufferData->lineHeight
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
                // width (pxl)
                outDimensions->x = finalMaxLineWidth;
                // height (pxl)
                outDimensions->y = StringDimensions.y;
                // numLines
                outDimensions->z = StringDimensions.z;
                return outDimensions;
            }
            else
            {
                *outDimensions = StringDefaulDimensions;
                return outDimensions;
            }
		}
	};

	void InitVertSpacingHook() {
        WriteRelJump(0xA1B3A0, &VertSpacingAdjust);
	}

    void InitFontHook() {
        WriteRelJumpEx(0xA1B020, &FontManagerEx::GetStringDimensionsEx);
    }
}
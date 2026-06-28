#include "font_manager.h"
#include "native_calls.h"

namespace fonthook
{
	NiPoint3* __thiscall FontManagerEx::CalculateStringDimensions(NiPoint3* outDimensions, const char* srcString, UInt32 fontID, float maxWrapWidth, UInt32 startCharIndex)
	{
		auto extraGlyphEntry = gNumberedExtraLetters.find(fontID);
		auto* extraGlyphs = extraGlyphEntry != gNumberedExtraLetters.end() ? &extraGlyphEntry->second : nullptr;

		if (fontID < 1 || !srcString)
		{
			*outDimensions = StringDefaultDimensions;
			return outDimensions;
		}

		std::string sConvertedStr;
		ConvertToMultiByte(srcString, sConvertedStr, extraGlyphs != nullptr);
		if (extraGlyphs && bIsQuestTextLSBDBCharacter && szDBChar[0] && szDBChar[1])
		{
			srcString = szDBChar;
			bIsQuestTextLSBDBCharacter = false;
		}
		else if (extraGlyphs && bMeasureQuestTextMSBAsEmpty)
		{
			srcString = "";
			bMeasureQuestTextMSBAsEmpty = false;
		}

		NiPoint3 StringDimensions = StringDefaultDimensions;
		int sourceStringLength = strlen(srcString);
		FontLetter* fontCharMetrics = this->pFont[fontID - 1]->pFontData->pFontLetters;
		float fontBaseLine = this->pFont[fontID - 1]->pFontData->fBaseLine;
		float lastValidWrapPosition = 0.0;
		float currentLineWidth = 0.0;
		float fontVerticalSpacingAdjust = FontManagerGetLinePadding(fontID);
		float previousCharTotalWidth = 0.0;
		char hasHyphenationPoint = 0;
		int totalLines = 1;
		StringDimensions.y = fontCharMetrics[' '].fHeight;

		UInt32 uiDoubleByteCode;
		for (int currentCharIndex = startCharIndex; currentCharIndex < sourceStringLength; ++currentCharIndex)
		{
			bool bIsDBCharacter = false;
			UInt8 currentChar = srcString[currentCharIndex];
			float currentCharTotalWidth = 0.0;

			if (extraGlyphs)
			{
				if (bIsQuestTextMSBDBCharacter && szDBChar)
				{
					srcString = szDBChar;
				}

				if ((currentCharIndex + 1) <= sourceStringLength)
				{
					bIsDBCharacter = TryDecodeDoubleByte(&srcString[currentCharIndex], uiDoubleByteCode);

					if (bIsQuestTextMSBDBCharacter)
					{
						srcString = "";
					}
				}
			}

			if (bIsDBCharacter)
			{
				auto glyphIt = extraGlyphs->find(uiDoubleByteCode);
				if (glyphIt != extraGlyphs->end())
				{
					currentCharTotalWidth = glyphIt->second.fLeadingEdge
						+ glyphIt->second.fWidth + glyphIt->second.fSpacing;
				}
				++currentCharIndex;
			}
			else
			{
				ConvertToAsciiQuotes(&currentChar);
				currentCharTotalWidth = fontCharMetrics[currentChar].fLeadingEdge
					+ fontCharMetrics[currentChar].fWidth + fontCharMetrics[currentChar].fSpacing;
				switch (currentChar)
				{
				case '\t':
				{
					double tabStopWidth = currentLineWidth;
					AlignLineWidthToTab(currentLineWidth, 75.0);
					currentCharTotalWidth = (float)(75.0 - tabStopWidth);
					break;
				}
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
						- currentCharTotalWidth - previousCharTotalWidth;
					currentLineWidth = currentCharTotalWidth + previousCharTotalWidth;
				}
				else
				{
					currentLineWidth = currentLineWidth - lastValidWrapPosition;
					if (!hasHyphenationPoint && currentChar == '\n')
						currentLineWidth = 0.0;
				}

				StringDimensions.x = (lastValidWrapPosition >= StringDimensions.x)
					? lastValidWrapPosition : StringDimensions.x;
				StringDimensions.y = fontVerticalSpacingAdjust
					+ fontBaseLine + StringDimensions.y;
				lastValidWrapPosition = 0.0;
				++totalLines;
			}
			previousCharTotalWidth = currentCharTotalWidth;
		}

		float finalMaxLineWidth = (currentLineWidth >= StringDimensions.x)
			? currentLineWidth : StringDimensions.x;
		StringDimensions.z = (float)totalLines;
		outDimensions->x = finalMaxLineWidth;
		outDimensions->y = StringDimensions.y;
		outDimensions->z = StringDimensions.z;
		return outDimensions;
	}

	UInt32* FontManagerEx::PrepText(BSStringT<char>* a7, int a3)
	{
		return ThisStdCall<UInt32*>(0xA18A30, this, a7, a3);
	}

} // namespace fonthook

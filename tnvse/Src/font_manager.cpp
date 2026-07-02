#include "font_manager.h"
#include "dictionary.h"
#include "native_calls.h"
#include <cmath>
#include <vector>

namespace fonthook
{
	namespace
	{
		static constexpr UInt32 kInitialAddCharLogCount = 8;
		std::unordered_map<const FontManager::CharData*, RichTextCharExtra> sRichTextCharExtras;
		std::vector<FontManager::CharData*> sRichTextRenderAddChars;
		FontManager::TextDoc* sRichTextRenderDoc = nullptr;
		UInt32 sRichTextRenderAddCharIndex = 0;

		void LogRichTextHookEnter(const char* hookName, BSStringT<char>& arTextString, FontManager::TextData& arData)
		{
			const char* text = arTextString.pString ? arTextString.pString : "";
			UInt32 textLength = arTextString.pString ? arTextString.GetLength() : 0;
			gLog.FormattedMessage(
				"tnvse_rich_text:\n"
				"  hook=%s\n"
				"  phase=enter\n"
				"  textLen=%u\n"
				"  data: font=%d justify=%d width=%d height=%d page=%d hyper=%d\n"
				"  textBegin:\n"
				"%s\n"
				"  textEnd",
				hookName,
				textLength,
				arData.iDefaultFont,
				arData.iJustification,
				arData.iWidth,
				arData.iHeight,
				arData.iPageNum,
				arData.bIsHypertext ? 1 : 0,
				text);
		}

		bool ShouldLogTextDocAddChar(const FontManager::CharData* apChar, UInt32 callCount)
		{
			if (callCount < kInitialAddCharLogCount || !apChar)
				return true;

			UInt32 dbcsCode = 0;
			if (TryGetRichTextCharDbcs(apChar, dbcsCode) || apChar->cChar >= 0x80)
				return true;

			if (apChar->cChar == '\n' || apChar->cChar == '\r' || apChar->cChar == '\t')
				return true;

			return apChar->xFilename.pString && apChar->xFilename.pString[0];
		}

		void LogTextDocAddChar(
			const char* callsite,
			FontManager::TextDoc* apDoc,
			FontManager::CharData* apChar,
			int aiNewLines,
			bool abNewPage,
			UInt32& arCallCount)
		{
			UInt32 callCount = arCallCount++;
			if (!ShouldLogTextDocAddChar(apChar, callCount))
				return;

			if (!apChar)
			{
				gLog.FormattedMessage(
					"tnvse_rich_text_addchar:\n"
					"  callsite=%s\n"
					"  callIndex=%u\n"
					"  textDoc=0x%08X\n"
					"  charData=0x00000000\n"
					"  args: newLines=%d newPage=%d",
					callsite,
					callCount,
					(UInt32)apDoc,
					aiNewLines,
					abNewPage ? 1 : 0);
				return;
			}

			UInt8 ch = apChar->cChar;
			char printable = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
			UInt32 dbcsCode = 0;
			bool isDbcs = TryGetRichTextCharDbcs(apChar, dbcsCode);
			const char* filename = apChar->xFilename.pString ? apChar->xFilename.pString : "";
			gLog.FormattedMessage(
				"tnvse_rich_text_addchar:\n"
				"  callsite=%s\n"
				"  callIndex=%u\n"
				"  textDoc=0x%08X charData=0x%08X\n"
				"  args: newLines=%d newPage=%d\n"
				"  char: byte=0x%02X ascii='%c' richDbcs=%d dbcsCode=0x%04X\n"
				"  data: font=%d justify=%d width=%d rise=%d drop=%d leading=%d x=%d\n"
				"  filename=\"%s\"",
				callsite,
				callCount,
				(UInt32)apDoc,
				(UInt32)apChar,
				aiNewLines,
				abNewPage ? 1 : 0,
				ch,
				printable,
				isDbcs ? 1 : 0,
				isDbcs ? dbcsCode : 0,
				apChar->iFontIndex,
				apChar->iJustification,
				apChar->iWidth,
				apChar->iRise,
				apChar->iDrop,
				apChar->iLeadingEdge,
				apChar->iX,
				filename);
		}

		void CallTextDocAddChar(FontManager::TextDoc* apDoc, FontManager::CharData* apChar, int aiNewLines, bool abNewPage)
		{
			ThisStdCall(0xA19A10, apDoc, apChar, aiNewLines, abNewPage);
		}

		void LogTextDocRenderEnter(FontManager::TextDoc* apDoc, NiNode* apNode, FontManager::TextData* apData)
		{
			UInt32 pageCount = 0;
			UInt32 lineCount = 0;
			UInt32 charCount = 0;
			UInt32 imageCount = 0;
			UInt32 richDbcsCount = 0;
			UInt32 highByteCount = 0;
			int maxLineWidth = 0;
			int firstPageMaxLineWidth = 0;
			int lastPageMaxLineWidth = 0;

			FontManager::TextPage* firstPage = nullptr;
			FontManager::TextPage* lastPage = nullptr;

			if (apDoc)
			{
				pageCount = apDoc->xPages.GetSize();
				if (!apDoc->xPages.IsEmpty())
				{
					firstPage = apDoc->xPages.GetHead();
					lastPage = apDoc->xPages.GetTail();
				}

				for (auto* pageNode = apDoc->xPages.m_pkHead; pageNode; pageNode = pageNode->m_pkNext)
				{
					FontManager::TextPage* page = pageNode->m_element;
					if (!page)
						continue;

					int pageMaxLineWidth = 0;
					lineCount += page->xLines.GetSize();
					for (auto* lineNode = page->xLines.m_pkHead; lineNode; lineNode = lineNode->m_pkNext)
					{
						FontManager::TextLine* line = lineNode->m_element;
						if (!line)
							continue;

						if (pageMaxLineWidth < line->iWidth)
							pageMaxLineWidth = line->iWidth;
						if (maxLineWidth < line->iWidth)
							maxLineWidth = line->iWidth;

						charCount += line->xChars.GetSize();
						for (auto* charNode = line->xChars.m_pkHead; charNode; charNode = charNode->m_pkNext)
						{
							FontManager::CharData* ch = charNode->m_element;
							if (!ch)
								continue;

							if (ch->xFilename.pString && ch->xFilename.pString[0])
								++imageCount;
							if (ch->cChar >= 0x80)
								++highByteCount;

							UInt32 dbcsCode = 0;
							if (TryGetRichTextCharDbcs(ch, dbcsCode))
								++richDbcsCount;
						}
					}

					if (page == firstPage)
						firstPageMaxLineWidth = pageMaxLineWidth;
					if (page == lastPage)
						lastPageMaxLineWidth = pageMaxLineWidth;
				}
			}

			gLog.FormattedMessage(
				"tnvse_rich_text_render:\n"
				"  hook=TextDoc::Render\n"
				"  phase=enter\n"
				"  textDoc=0x%08X node=0x%08X textData=0x%08X\n"
				"  data: font=%d justify=%d width=%d height=%d page=%d hyper=%d lines=%d pages=%d\n"
				"  doc: pageWidth=%d pageHeight=%d pageNum=%d pageCount=%u lineCount=%u charCount=%u images=%u highBytes=%u richDbcs=%u maxLineWidth=%d\n"
				"  firstPage=0x%08X firstPageLineWidth=%d firstPageHeight=%d firstPageLines=%u\n"
				"  lastPage=0x%08X lastPageLineWidth=%d lastPageHeight=%d lastPageLines=%u",
				(UInt32)apDoc,
				(UInt32)apNode,
				(UInt32)apData,
				apData ? apData->iDefaultFont : -1,
				apData ? apData->iJustification : -1,
				apData ? apData->iWidth : -1,
				apData ? apData->iHeight : -1,
				apData ? apData->iPageNum : -1,
				apData && apData->bIsHypertext ? 1 : 0,
				apData ? apData->iNumLines : -1,
				apData ? apData->iNumPages : -1,
				apDoc ? apDoc->iPageWidth : -1,
				apDoc ? apDoc->iPageHeight : -1,
				apDoc ? apDoc->iPageNum : -1,
				pageCount,
				lineCount,
				charCount,
				imageCount,
				highByteCount,
				richDbcsCount,
				maxLineWidth,
				(UInt32)firstPage,
				firstPage ? firstPageMaxLineWidth : -1,
				firstPage ? firstPage->iHeight : -1,
				firstPage ? firstPage->xLines.GetSize() : 0,
				(UInt32)lastPage,
				lastPage ? lastPageMaxLineWidth : -1,
				lastPage ? lastPage->iHeight : -1,
				lastPage ? lastPage->xLines.GetSize() : 0);
		}

		void CallTextDocRender(FontManager::TextDoc* apDoc, NiNode* apNode, FontManager::TextData* apData)
		{
			ThisStdCall(0xA19060, apDoc, apNode, apData);
		}

		FontManager::TextPage* GetRenderPage(FontManager::TextDoc* apDoc)
		{
			if (!apDoc)
				return nullptr;

			int pageIndex = apDoc->iPageNum;
			for (auto* pageNode = apDoc->xPages.m_pkHead; pageNode; pageNode = pageNode->m_pkNext)
			{
				if (pageIndex <= 0)
					return pageNode->m_element;
				--pageIndex;
			}

			return nullptr;
		}

		bool IsRenderedAsImage(const FontManager::CharData* apChar)
		{
			return apChar && apChar->xFilename.pString && apChar->xFilename.pString[0];
		}

		bool IsRenderedAsIcon(const FontManager::CharData* apChar)
		{
			return apChar && apChar->cChar == 1;
		}

		void LogRichTextRenderContextLeave(FontManager::TextDoc* apDoc)
		{
			UInt32 expectedAddChars = (UInt32)sRichTextRenderAddChars.size();
			UInt32 emittedAddChars = sRichTextRenderAddCharIndex;
			gLog.FormattedMessage(
				"tnvse_rich_text_render_context:\n"
				"  hook=TextDoc::Render\n"
				"  phase=leave\n"
				"  textDoc=0x%08X contextDoc=0x%08X\n"
				"  expectedAddChars=%u emittedAddChars=%u remaining=%d",
				(UInt32)apDoc,
				(UInt32)sRichTextRenderDoc,
				expectedAddChars,
				emittedAddChars,
				(int)expectedAddChars - (int)emittedAddChars);
		}

		UInt32 ClearRichTextCharExtrasForDoc(FontManager::TextDoc* apDoc)
		{
			UInt32 clearedCount = 0;
			if (!apDoc)
				return clearedCount;

			for (auto* pageNode = apDoc->xPages.m_pkHead; pageNode; pageNode = pageNode->m_pkNext)
			{
				FontManager::TextPage* page = pageNode->m_element;
				if (!page)
					continue;

				for (auto* lineNode = page->xLines.m_pkHead; lineNode; lineNode = lineNode->m_pkNext)
				{
					FontManager::TextLine* line = lineNode->m_element;
					if (!line)
						continue;

					for (auto* charNode = line->xChars.m_pkHead; charNode; charNode = charNode->m_pkNext)
					{
						FontManager::CharData* ch = charNode->m_element;
						if (!ch)
							continue;

						clearedCount += sRichTextCharExtras.erase(ch);
					}
				}
			}

			return clearedCount;
		}

		void LogTextDocDestroy(FontManager::TextDoc* apDoc, UInt32 clearedExtraCount)
		{
			gLog.FormattedMessage(
				"tnvse_rich_text_destroy:\n"
				"  hook=TextDoc::Destroy\n"
				"  phase=enter\n"
				"  textDoc=0x%08X\n"
				"  doc: pageWidth=%d pageHeight=%d pageNum=%d pageCount=%u\n"
				"  richTextExtrasCleared=%u richTextExtrasRemaining=%u",
				(UInt32)apDoc,
				apDoc ? apDoc->iPageWidth : -1,
				apDoc ? apDoc->iPageHeight : -1,
				apDoc ? apDoc->iPageNum : -1,
				apDoc ? apDoc->xPages.GetSize() : 0,
				clearedExtraCount,
				(UInt32)sRichTextCharExtras.size());
		}

		void CallTextDocDestroy(FontManager::TextDoc* apDoc)
		{
			ThisStdCall(0xA1B990, apDoc);
		}
	}

	void SetRichTextCharDbcs(const FontManager::CharData* apChar, UInt32 auiDbcsCode)
	{
		if (!apChar)
			return;

		sRichTextCharExtras[apChar] = { auiDbcsCode };
	}

	bool TryGetRichTextCharDbcs(const FontManager::CharData* apChar, UInt32& arDbcsCode)
	{
		if (!apChar)
			return false;

		auto it = sRichTextCharExtras.find(apChar);
		if (it == sRichTextCharExtras.end())
			return false;

		arDbcsCode = it->second.dbcsCode;
		return true;
	}

	void ClearRichTextCharExtra(const FontManager::CharData* apChar)
	{
		if (!apChar)
			return;

		sRichTextCharExtras.erase(apChar);
	}

	void ClearRichTextCharExtras()
	{
		sRichTextCharExtras.clear();
	}

	void BeginRichTextRenderContext(FontManager::TextDoc* apDoc, FontManager::TextData* apData)
	{
		sRichTextRenderAddChars.clear();
		sRichTextRenderDoc = apDoc;
		sRichTextRenderAddCharIndex = 0;

		FontManager::TextPage* page = GetRenderPage(apDoc);
		if (!page || !apData || apData->iLines <= 0)
			return;

		UInt32 lineLimit = (UInt32)apData->iLines;
		UInt32 lineIndex = 0;
		for (auto* lineNode = page->xLines.m_pkHead; lineNode && lineIndex < lineLimit; lineNode = lineNode->m_pkNext, ++lineIndex)
		{
			FontManager::TextLine* line = lineNode->m_element;
			if (!line)
				continue;

			for (auto* charNode = line->xChars.m_pkHead; charNode; charNode = charNode->m_pkNext)
			{
				FontManager::CharData* ch = charNode->m_element;
				if (!ch || IsRenderedAsImage(ch) || IsRenderedAsIcon(ch))
					continue;

				sRichTextRenderAddChars.push_back(ch);
			}
		}
	}

	void EndRichTextRenderContext(FontManager::TextDoc* apDoc)
	{
		LogRichTextRenderContextLeave(apDoc);
		sRichTextRenderAddChars.clear();
		sRichTextRenderDoc = nullptr;
		sRichTextRenderAddCharIndex = 0;
	}

	bool TryConsumeRichTextRenderAddChar(RichTextRenderAddCharInfo& arInfo)
	{
		arInfo = {};
		if (!sRichTextRenderDoc)
			return false;

		arInfo.textDoc = sRichTextRenderDoc;
		arInfo.callIndex = sRichTextRenderAddCharIndex;
		arInfo.expectedAddCharCount = (UInt32)sRichTextRenderAddChars.size();
		if (sRichTextRenderAddCharIndex < sRichTextRenderAddChars.size())
			arInfo.charData = sRichTextRenderAddChars[sRichTextRenderAddCharIndex];

		++sRichTextRenderAddCharIndex;
		return true;
	}

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
		bool bSkipDictionaryTranslation = false;
		if (extraGlyphs && bIsQuestTextLSBDBCharacter && szDBChar[0] && szDBChar[1])
		{
			srcString = szDBChar;
			bIsQuestTextLSBDBCharacter = false;
			bSkipDictionaryTranslation = true;
		}
		else if (extraGlyphs && bMeasureQuestTextMSBAsEmpty)
		{
			srcString = "";
			bMeasureQuestTextMSBAsEmpty = false;
			bSkipDictionaryTranslation = true;
		}
		std::string sTranslatedStr;
		if (!bSkipDictionaryTranslation && TranslateText(srcString, sTranslatedStr))
			srcString = sTranslatedStr.c_str();

		NiPoint3 StringDimensions = StringDefaultDimensions;
		int sourceStringLength = strlen(srcString);
		FontLetter* fontCharMetrics = this->pFont[fontID - 1]->pFontData->pFontLetters;
		float fontBaseLine = this->pFont[fontID - 1]->pFontData->fBaseLine;
		float lastValidWrapPosition = 0.0;
		float currentLineWidth = 0.0;
		float fontVerticalSpacingAdjust = FontManager::GetLinePadding(fontID);
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
					// 0xEC9130
					double tabRemainder = fmod(currentLineWidth, 75.0);
					currentCharTotalWidth = (float)(75.0 - tabRemainder);
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

	FontManager::TextDoc* __thiscall FontManagerEx::PrepHypertext(BSStringT<char>& arTextString, FontManager::TextData& arData)
	{
		LogRichTextHookEnter("FontManager::PrepHypertext", arTextString, arData);

		FontManager::TextDoc* textDoc = FontManager::PrepHypertext(arTextString, arData);
		gLog.FormattedMessage(
			"tnvse_rich_text:\n"
			"  hook=FontManager::PrepHypertext\n"
			"  phase=leave\n"
			"  textDoc=0x%08X",
			(UInt32)textDoc);
		return textDoc;
	}

	FontManager::TextDoc* __thiscall FontManagerEx::PrepText(BSStringT<char>& arTextString, FontManager::TextData& arData)
	{
		LogRichTextHookEnter("FontManager::PrepText", arTextString, arData);
		return FontManager::PrepText(arTextString, arData);
	}

	void __thiscall FontManagerEx::TextDocRender(NiNode* apNode, FontManager::TextData* apData)
	{
		FontManager::TextDoc* doc = reinterpret_cast<FontManager::TextDoc*>(this);
		LogTextDocRenderEnter(doc, apNode, apData);
		BeginRichTextRenderContext(doc, apData);
		CallTextDocRender(doc, apNode, apData);
		EndRichTextRenderContext(doc);
	}

	void __thiscall FontManagerEx::TextDocDestroy()
	{
		FontManager::TextDoc* doc = reinterpret_cast<FontManager::TextDoc*>(this);
		UInt32 clearedExtraCount = ClearRichTextCharExtrasForDoc(doc);
		LogTextDocDestroy(doc, clearedExtraCount);
		CallTextDocDestroy(doc);
	}

	void __thiscall FontManagerEx::TextDocAddChar_A178A4(FontManager::CharData* apChar, int aiNewLines, bool abNewPage)
	{
		static UInt32 sCallCount = 0;
		FontManager::TextDoc* doc = reinterpret_cast<FontManager::TextDoc*>(this);
		LogTextDocAddChar("0xA178A4 PrepHypertext", doc, apChar, aiNewLines, abNewPage, sCallCount);
		CallTextDocAddChar(doc, apChar, aiNewLines, abNewPage);
	}

	void __thiscall FontManagerEx::TextDocAddChar_A179D9(FontManager::CharData* apChar, int aiNewLines, bool abNewPage)
	{
		static UInt32 sCallCount = 0;
		FontManager::TextDoc* doc = reinterpret_cast<FontManager::TextDoc*>(this);
		LogTextDocAddChar("0xA179D9 PrepHypertext", doc, apChar, aiNewLines, abNewPage, sCallCount);
		CallTextDocAddChar(doc, apChar, aiNewLines, abNewPage);
	}

	void __thiscall FontManagerEx::TextDocAddChar_A17FC2(FontManager::CharData* apChar, int aiNewLines, bool abNewPage)
	{
		static UInt32 sCallCount = 0;
		FontManager::TextDoc* doc = reinterpret_cast<FontManager::TextDoc*>(this);
		LogTextDocAddChar("0xA17FC2 PrepHypertext", doc, apChar, aiNewLines, abNewPage, sCallCount);
		CallTextDocAddChar(doc, apChar, aiNewLines, abNewPage);
	}

	void __thiscall FontManagerEx::TextDocAddChar_A18D7C(FontManager::CharData* apChar, int aiNewLines, bool abNewPage)
	{
		static UInt32 sCallCount = 0;
		FontManager::TextDoc* doc = reinterpret_cast<FontManager::TextDoc*>(this);
		LogTextDocAddChar("0xA18D7C PrepText", doc, apChar, aiNewLines, abNewPage, sCallCount);
		CallTextDocAddChar(doc, apChar, aiNewLines, abNewPage);
	}

} // namespace fonthook

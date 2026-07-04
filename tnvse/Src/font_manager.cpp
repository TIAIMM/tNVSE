#include "font_manager.h"
#include "dictionary.h"
#include "font_glyphs.h"
#include "native_calls.h"
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace fonthook
{
	namespace
	{
		static constexpr UInt32 kRichTextDbcsFailureLogLimit = 64;
		static constexpr UInt32 kRichTextCharTypeOpen = 0x01;
		static constexpr UInt32 kRichTextCharTypeClose = 0x02;
		static constexpr UInt32 kRichTextCharTypeSpace = 0x04;
		static constexpr UInt32 kRichTextCharTypeQuote = 0x08;
		static constexpr UInt32 kRichTextCharTypeEquals = 0x10;
		static constexpr UInt32 kRichTextCharTypeEnd = 0x20;
		std::unordered_map<const FontManager::CharData*, RichTextCharExtra> sRichTextCharExtras;
		std::vector<FontManager::CharData*> sRichTextRenderAddChars;
		FontManager::TextDoc* sRichTextRenderDoc = nullptr;
		UInt32 sRichTextRenderAddCharIndex = 0;
		UInt32 sRichTextDbcsFailureLogCount = 0;

		struct PendingRichTextLead
		{
			FontManager::CharData* charData = nullptr;
			int newLines = 0;
			bool newPage = false;
			const char* callsite = nullptr;
		};

		std::unordered_map<FontManager::TextDoc*, PendingRichTextLead> sPendingRichTextLeads;
		thread_local UInt32 sRichTextConvertedInputDepth = 0;
		thread_local UInt32 sRichTextPrepTextParseDepth = 0;

		struct ScopedRichTextConvertedInput
		{
			bool active = false;

			explicit ScopedRichTextConvertedInput(bool abActive) : active(abActive)
			{
				if (active)
					++sRichTextConvertedInputDepth;
			}

			~ScopedRichTextConvertedInput()
			{
				if (active)
					--sRichTextConvertedInputDepth;
			}
		};

		struct ScopedRichTextPrepTextParse
		{
			bool active = false;

			explicit ScopedRichTextPrepTextParse(bool abActive) : active(abActive)
			{
				if (active)
					++sRichTextPrepTextParseDepth;
			}

			~ScopedRichTextPrepTextParse()
			{
				if (active)
					--sRichTextPrepTextParseDepth;
			}
		};

		bool HasRichTextExtraGlyphs()
		{
			return HasAnyExtraGlyphs();
		}

		UInt32 GetRichTextCharType(UInt8 c)
		{
			switch (c)
			{
			case 0:
				return kRichTextCharTypeEnd;
			case '"':
			case '\'':
				return kRichTextCharTypeQuote;
			case '<':
			case '{':
				return kRichTextCharTypeOpen;
			case '=':
				return kRichTextCharTypeEquals;
			case '>':
			case '}':
				return kRichTextCharTypeClose;
			default:
				return c > ' ' ? 0 : kRichTextCharTypeSpace;
			}
		}

		bool TryConvertRichTextInput(
			BSStringT<char>& arTextString,
			const char*& arParserText,
			std::string& arConvertedTextStorage)
		{
			const char* text = arTextString.pString ? arTextString.pString : "";
			if (sRichTextConvertedInputDepth != 0)
				return false;

			const char* parserText = text;
			if (!ConvertToMultiByte(parserText, arConvertedTextStorage, HasRichTextExtraGlyphs()))
				return false;

			arParserText = parserText;
			return true;
		}

		BSStringT<char>* CallOriginalCollectTo(
			FontManager* apManager,
			BSStringT<char>* apOutString,
			const char* apSource,
			UInt32* apIndex,
			UInt32 aiStopMask,
			UInt32 aiRequiredMask,
			UInt32* apOutType,
			char* apOutChar,
			bool abUseReplacements)
		{
			FontManager* fontManager = apManager ? apManager : FontManager::GetSingleton();
			return ThisStdCall<BSStringT<char>*>(0xA16EA0, fontManager,
				apOutString, apSource, apIndex, aiStopMask, aiRequiredMask,
				apOutType, apOutChar, abUseReplacements);
		}

		bool TryAppendRichTextReplacement(FontManager* apManager, const char* apSource, UInt32& arIndex, std::string& arOut)
		{
			if (!apSource || apSource[arIndex] != '&')
				return false;

			UInt32 length = 1;
			while (apSource[arIndex + length] &&
				!GetRichTextCharType(static_cast<UInt8>(apSource[arIndex + length])) &&
				apSource[arIndex + length - 1] != ';')
			{
				++length;
			}

			if (apSource[arIndex + length - 1] != ';')
			{
				++arIndex;
				return true;
			}

			std::string entity(apSource + arIndex, apSource + arIndex + length);
			char replacement[260] = {};
			Interface_FindTextReplacementString(entity.c_str(), replacement, sizeof(replacement), false);
			if (replacement[0] == '\\')
			{
				FontManager* fontManager = apManager ? apManager : FontManager::GetSingleton();
				if (fontManager && fontManager->pFont[0])
				{
					fontManager->pFont[0]->AddTextIcon(replacement + 1);
					arOut.push_back(1);
				}
			}
			else if (replacement[0])
			{
				arOut += replacement;
			}
			else
			{
				arOut += entity;
			}

			arIndex += length;
			return true;
		}

		bool IsRichTextTextSegmentCollectTo(
			UInt32 aiStopMask,
			UInt32 aiRequiredMask,
			bool abUseReplacements)
		{
			return aiStopMask == kRichTextCharTypeOpen &&
				aiRequiredMask == 0 &&
				abUseReplacements;
		}

		bool IsRichTextAttributeValueCollectTo(
			UInt32 aiStopMask,
			UInt32 aiRequiredMask,
			bool abUseReplacements)
		{
			return aiRequiredMask == 0 &&
				abUseReplacements &&
				(aiStopMask == (kRichTextCharTypeClose | kRichTextCharTypeQuote) ||
					aiStopMask == (kRichTextCharTypeClose | kRichTextCharTypeSpace));
		}

		bool ShouldUseRichTextCollectTo(
			const char* apSource,
			const UInt32* apIndex,
			UInt32 aiStopMask,
			UInt32 aiRequiredMask,
			bool abUseReplacements,
			bool abAllowAttributeValue)
		{
			if (!apSource || !apIndex || !HasRichTextExtraGlyphs())
				return false;

			if (IsRichTextTextSegmentCollectTo(aiStopMask, aiRequiredMask, abUseReplacements))
				return true;

			return abAllowAttributeValue &&
				IsRichTextAttributeValueCollectTo(aiStopMask, aiRequiredMask, abUseReplacements);
		}

		void InitRichTextCollectToOutput(BSStringT<char>* apOutString)
		{
			apOutString->pString = nullptr;
			apOutString->sLen = 0;
			apOutString->sMaxLen = 0;
		}

		ExtraGlyphMap* GetExtraGlyphsForChar(const FontManager::CharData* apChar, Font** apOutFont = nullptr)
		{
			if (apOutFont)
				*apOutFont = nullptr;
			if (!apChar)
				return nullptr;

			FontManager* fontManager = FontManager::GetSingleton();
			if (!fontManager || apChar->iFontIndex < 0 || apChar->iFontIndex >= 8)
				return nullptr;

			Font* font = fontManager->pFont[apChar->iFontIndex];
			if (!font)
				return nullptr;

			if (apOutFont)
				*apOutFont = font;

			return GetExtraGlyphs(font->iFontNum);
		}

		bool HasRichTextFilename(const FontManager::CharData* apChar)
		{
			return apChar && apChar->xFilename.pString && apChar->xFilename.pString[0];
		}

		bool CanHoldRichTextLead(const FontManager::CharData* apChar)
		{
			if (!apChar || HasRichTextFilename(apChar) || !IsLeadByte(apChar->cChar))
				return false;

			return GetExtraGlyphsForChar(apChar) != nullptr;
		}

		FontLetter* LookupRichTextDbcsGlyph(const FontManager::CharData* apChar, UInt32 auiDbcsCode, Font** apOutFont = nullptr)
		{
			ExtraGlyphMap* extraGlyphs = GetExtraGlyphsForChar(apChar, apOutFont);
			return LookupDBGlyph(extraGlyphs, auiDbcsCode);
		}

		void ApplyRichTextGlyphMetrics(FontManager::CharData* apChar, Font* apFont, const FontLetter* apGlyph)
		{
			if (!apChar || !apFont || !apFont->pFontData || !apGlyph)
				return;

			apChar->iWidth = GetGlyphLayoutWidth(apGlyph);
			apChar->iRise = ConditionalFloatToUInt(apFont->pFontData->fBaseLine);
			apChar->iDrop = ConditionalFloatToUInt(-apFont->fMaxDrop);
			apChar->iLeadingEdge = 0;
			apChar->iX = 0;
		}

		void LogRichTextDbcsFailure(
			const char* callsite,
			const char* action,
			const char* reason,
			FontManager::TextDoc* apDoc,
			const FontManager::CharData* apLead,
			const FontManager::CharData* apTrail,
			int aiTrailNewLines = 0,
			bool abTrailNewPage = false)
		{
			if (sRichTextDbcsFailureLogCount >= kRichTextDbcsFailureLogLimit)
				return;
			++sRichTextDbcsFailureLogCount;

			gLog.FormattedMessage(
				"tnvse_rich_text_dbcs:\n"
				"  callsite=%s\n"
				"  action=%s\n"
				"  reason=%s\n"
				"  textDoc=0x%08X lead=0x%08X trail=0x%08X\n"
				"  bytes=0x%02X 0x%02X\n"
				"  trailArgs: newLines=%d newPage=%d",
				callsite ? callsite : "",
				action,
				reason,
				(UInt32)apDoc,
				(UInt32)apLead,
				(UInt32)apTrail,
				apLead ? apLead->cChar : 0,
				apTrail ? apTrail->cChar : 0,
				aiTrailNewLines,
				abTrailNewPage ? 1 : 0);
		}

		void FreeRichTextCharData(FontManager::CharData* apChar)
		{
			if (!apChar)
				return;

			ClearRichTextCharExtra(apChar);
			MemoryManager_s_Instance->Deallocate(apChar);
		}

		bool TryGetRichTextCharExtra(const FontManager::CharData* apChar, RichTextCharExtra& arExtra)
		{
			if (!apChar)
				return false;

			auto it = sRichTextCharExtras.find(apChar);
			if (it == sRichTextCharExtras.end())
				return false;

			arExtra = it->second;
			return true;
		}

		void CallTextDocAddChar(FontManager::TextDoc* apDoc, FontManager::CharData* apChar, int aiNewLines, bool abNewPage)
		{
			ThisStdCall(0xA19A10, apDoc, apChar, aiNewLines, abNewPage);
		}

		void FlushPendingRichTextLead(FontManager::TextDoc* apDoc, const char* reason)
		{
			auto it = sPendingRichTextLeads.find(apDoc);
			if (it == sPendingRichTextLeads.end())
				return;

			PendingRichTextLead pending = it->second;
			sPendingRichTextLeads.erase(it);
			if (pending.charData)
			{
				LogRichTextDbcsFailure(pending.callsite, "flush-pending-lead",
					reason, apDoc, pending.charData, nullptr);
				CallTextDocAddChar(apDoc, pending.charData, pending.newLines, pending.newPage);
			}
		}

		void DiscardPendingRichTextLead(FontManager::TextDoc* apDoc)
		{
			auto it = sPendingRichTextLeads.find(apDoc);
			if (it == sPendingRichTextLeads.end())
				return;

			FontManager::CharData* pendingChar = it->second.charData;
			sPendingRichTextLeads.erase(it);
			FreeRichTextCharData(pendingChar);
		}

		bool TryMergePendingRichTextLead(
			FontManager::TextDoc* apDoc,
			const PendingRichTextLead& arPending,
			FontManager::CharData* apTrail,
			int aiTrailNewLines,
			bool abTrailNewPage,
			const char*& arRejectReason)
		{
			arRejectReason = "unknown";
			FontManager::CharData* lead = arPending.charData;
			if (!lead || !apTrail || aiTrailNewLines != 0 || abTrailNewPage)
			{
				arRejectReason = "missing-trail-or-newline-page-boundary";
				return false;
			}
			if (HasRichTextFilename(lead) || HasRichTextFilename(apTrail))
			{
				arRejectReason = "image-entry";
				return false;
			}
			if (lead->iFontIndex != apTrail->iFontIndex)
			{
				arRejectReason = "font-changed-between-bytes";
				return false;
			}

			char bytes[2] = { (char)lead->cChar, (char)apTrail->cChar };
			UInt32 dbcsCode = 0;
			if (!TryDecodeDoubleByte(bytes, dbcsCode))
			{
				arRejectReason = "invalid-dbcs-pair";
				return false;
			}

			Font* font = nullptr;
			FontLetter* glyph = LookupRichTextDbcsGlyph(lead, dbcsCode, &font);
			if (!glyph)
			{
				arRejectReason = "glyph-missing";
				return false;
			}

			SetRichTextCharDbcs(lead, dbcsCode, apDoc);
			ApplyRichTextGlyphMetrics(lead, font, glyph);
			CallTextDocAddChar(apDoc, lead, arPending.newLines, arPending.newPage);
			FreeRichTextCharData(apTrail);
			return true;
		}

		bool ShouldStartNewLineForRichTextDbcs(FontManager::TextLine* apLine, FontManager::CharData* apChar, bool abAddHead)
		{
			if (!apLine || !apChar || abAddHead || HasRichTextFilename(apChar))
				return false;
			if (!apLine->pPage || apLine->iWidth <= 0 || apLine->iPageWidth <= 0)
				return false;
			if (apLine->iWidth + apChar->iWidth <= apLine->iPageWidth)
				return false;

			UInt32 dbcsCode = 0;
			return TryGetRichTextCharDbcs(apChar, dbcsCode);
		}

		void HandleTextDocAddChar(
			const char* callsite,
			FontManager::TextDoc* apDoc,
			FontManager::CharData* apChar,
			int aiNewLines,
			bool abNewPage)
		{
			if (!apDoc || !apChar)
			{
				CallTextDocAddChar(apDoc, apChar, aiNewLines, abNewPage);
				return;
			}

			auto pendingIt = sPendingRichTextLeads.find(apDoc);
			if (pendingIt != sPendingRichTextLeads.end())
			{
				PendingRichTextLead pending = pendingIt->second;
				const char* rejectReason = nullptr;
				if (TryMergePendingRichTextLead(apDoc, pending, apChar, aiNewLines, abNewPage, rejectReason))
				{
					sPendingRichTextLeads.erase(pendingIt);
					return;
				}

				LogRichTextDbcsFailure(pending.callsite, "merge-rejected",
					rejectReason ? rejectReason : "unknown", apDoc, pending.charData,
					apChar, aiNewLines, abNewPage);
				FlushPendingRichTextLead(apDoc, "merge-rejected");
			}

			if (CanHoldRichTextLead(apChar))
			{
				sPendingRichTextLeads[apDoc] = { apChar, aiNewLines, abNewPage, callsite };
				return;
			}

			CallTextDocAddChar(apDoc, apChar, aiNewLines, abNewPage);
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
			if (expectedAddChars == emittedAddChars)
				return;

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

			for (auto it = sRichTextCharExtras.begin(); it != sRichTextCharExtras.end();)
			{
				if (it->second.textDoc == apDoc)
				{
					it = sRichTextCharExtras.erase(it);
					++clearedCount;
				}
				else
				{
					++it;
				}
			}

			return clearedCount;
		}

		UInt32 CountRichTextCharExtrasForDoc(FontManager::TextDoc* apDoc)
		{
			if (!apDoc)
				return 0;

			UInt32 count = 0;
			for (const auto& entry : sRichTextCharExtras)
			{
				if (entry.second.textDoc == apDoc)
					++count;
			}
			return count;
		}

		void LogTextDocDestroy(FontManager::TextDoc* apDoc, UInt32 clearedExtraCount, UInt32 remainingDocExtraCount)
		{
			if (remainingDocExtraCount == 0)
				return;

			gLog.FormattedMessage(
				"tnvse_rich_text_destroy:\n"
				"  hook=TextDoc::Destroy\n"
				"  phase=enter\n"
				"  textDoc=0x%08X\n"
				"  doc: pageWidth=%d pageHeight=%d pageNum=%d pageCount=%u\n"
				"  richTextExtrasCleared=%u richTextExtrasRemainingForDoc=%u richTextExtrasTotal=%u",
				(UInt32)apDoc,
				apDoc ? apDoc->iPageWidth : -1,
				apDoc ? apDoc->iPageHeight : -1,
				apDoc ? apDoc->iPageNum : -1,
				apDoc ? apDoc->xPages.GetSize() : 0,
				clearedExtraCount,
				remainingDocExtraCount,
				(UInt32)sRichTextCharExtras.size());
		}

		void CallTextDocDestroy(FontManager::TextDoc* apDoc)
		{
			ThisStdCall(0xA1B990, apDoc);
		}

		struct ScopedRichTextRenderContext
		{
			std::vector<FontManager::CharData*> savedAddChars;
			FontManager::TextDoc* savedDoc = nullptr;
			UInt32 savedAddCharIndex = 0;
			FontManager::TextDoc* currentDoc = nullptr;

			ScopedRichTextRenderContext(FontManager::TextDoc* apDoc, FontManager::TextData* apData)
				: savedDoc(sRichTextRenderDoc),
				savedAddCharIndex(sRichTextRenderAddCharIndex),
				currentDoc(apDoc)
			{
				savedAddChars.swap(sRichTextRenderAddChars);
				BeginRichTextRenderContext(apDoc, apData);
			}

			~ScopedRichTextRenderContext()
			{
				EndRichTextRenderContext(currentDoc);
				sRichTextRenderAddChars.swap(savedAddChars);
				sRichTextRenderDoc = savedDoc;
				sRichTextRenderAddCharIndex = savedAddCharIndex;
			}
		};

	}

	void SetRichTextCharDbcs(const FontManager::CharData* apChar, UInt32 auiDbcsCode, const FontManager::TextDoc* apDoc)
	{
		if (!apChar)
			return;

		sRichTextCharExtras[apChar] = { auiDbcsCode, apDoc };
	}

	bool TryGetRichTextCharDbcs(const FontManager::CharData* apChar, UInt32& arDbcsCode)
	{
		RichTextCharExtra extra = {};
		if (!TryGetRichTextCharExtra(apChar, extra))
			return false;

		arDbcsCode = extra.dbcsCode;
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
		for (auto& entry : sPendingRichTextLeads)
			FreeRichTextCharData(entry.second.charData);
		sPendingRichTextLeads.clear();
		sRichTextRenderAddChars.clear();
		sRichTextRenderDoc = nullptr;
		sRichTextRenderAddCharIndex = 0;
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
		auto* extraGlyphs = GetExtraGlyphs(fontID);

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

	BSStringT<char>* CollectToImpl(
		FontManager* apManager,
		BSStringT<char>* apOutString,
		const char* apSource,
		UInt32* apIndex,
		UInt32 aiStopMask,
		UInt32 aiRequiredMask,
		UInt32* apOutType,
		char* apOutChar,
		bool abUseReplacements,
		bool abAllowAttributeValue)
	{
		if (!apOutString || !apSource || !apIndex || !apOutType || !apOutChar ||
			!ShouldUseRichTextCollectTo(apSource, apIndex, aiStopMask, aiRequiredMask,
				abUseReplacements, abAllowAttributeValue))
		{
			return CallOriginalCollectTo(apManager, apOutString, apSource, apIndex, aiStopMask,
				aiRequiredMask, apOutType, apOutChar, abUseReplacements);
		}

		std::string collected;
		while (true)
		{
			UInt8 current = static_cast<UInt8>(apSource[*apIndex]);
			*apOutChar = static_cast<char>(current);
			UInt32 charType = GetRichTextCharType(current);
			if (charType & kRichTextCharTypeEnd)
			{
				*apOutType = kRichTextCharTypeEnd;
				break;
			}

			if (aiStopMask & charType)
			{
				*apOutType = charType;
				++*apIndex;
				break;
			}

			if (aiRequiredMask && (aiRequiredMask & charType) == 0)
			{
				*apOutType = charType;
				break;
			}

			if (current == '\n' || current == '\r')
			{
				++*apIndex;
				continue;
			}

			UInt32 dbcsCode = 0;
			if (TryDecodeDoubleByte(&apSource[*apIndex], dbcsCode))
			{
				if (abUseReplacements)
				{
					collected.push_back(apSource[*apIndex]);
					collected.push_back(apSource[*apIndex + 1]);
				}
				*apIndex += 2;
				continue;
			}

			if (abUseReplacements)
			{
				if (current == '&' &&
					TryAppendRichTextReplacement(apManager, apSource, *apIndex, collected))
				{
					continue;
				}

				collected.push_back(static_cast<char>(current));
			}
			++*apIndex;
		}

		InitRichTextCollectToOutput(apOutString);
		apOutString->Set(collected.c_str());
		return apOutString;
	}

	BSStringT<char>* __fastcall FontManagerEx::CollectTo(
		FontManager* apManager,
		void*,
		BSStringT<char>* apOutString,
		const char* apSource,
		UInt32* apIndex,
		UInt32 aiStopMask,
		UInt32 aiRequiredMask,
		UInt32* apOutType,
		char* apOutChar,
		bool abUseReplacements)
	{
		return CollectToImpl(apManager, apOutString, apSource, apIndex, aiStopMask,
			aiRequiredMask, apOutType, apOutChar, abUseReplacements, false);
	}

	BSStringT<char>* __fastcall FontManagerEx::CollectToAttributeValue(
		FontManager* apManager,
		void*,
		BSStringT<char>* apOutString,
		const char* apSource,
		UInt32* apIndex,
		UInt32 aiStopMask,
		UInt32 aiRequiredMask,
		UInt32* apOutType,
		char* apOutChar,
		bool abUseReplacements)
	{
		return CollectToImpl(apManager, apOutString, apSource, apIndex, aiStopMask,
			aiRequiredMask, apOutType, apOutChar, abUseReplacements, true);
	}

	FontManager::TextDoc* __thiscall FontManagerEx::PrepHypertext(BSStringT<char>& arTextString, FontManager::TextData& arData)
	{
		FontManager::TextDoc* textDoc = nullptr;
		const char* text = arTextString.pString ? arTextString.pString : "";
		const char* parserText = text;
		std::string convertedTextStorage;
		bool usedConvertedText = false;
		if (TryConvertRichTextInput(arTextString, parserText, convertedTextStorage))
		{
			BSStringT<char> convertedText;
			if (convertedText.Set(parserText))
			{
				usedConvertedText = true;
				ScopedRichTextConvertedInput convertedScope(true);
				textDoc = FontManager::PrepHypertext(convertedText, arData);
			}
		}

		if (!usedConvertedText)
			textDoc = FontManager::PrepHypertext(arTextString, arData);

		FlushPendingRichTextLead(textDoc, "prep-hypertext-leave");
		return textDoc;
	}

	FontManager::TextDoc* __thiscall FontManagerEx::PrepText(BSStringT<char>& arTextString, FontManager::TextData& arData)
	{
		const char* text = arTextString.pString ? arTextString.pString : "";
		const char* parserText = text;
		std::string convertedTextStorage;
		if (TryConvertRichTextInput(arTextString, parserText, convertedTextStorage))
		{
			BSStringT<char> convertedText;
			if (convertedText.Set(parserText))
			{
				ScopedRichTextConvertedInput convertedScope(true);
				ScopedRichTextPrepTextParse prepTextParseScope(true);
				FontManager::TextDoc* textDoc = FontManager::PrepText(convertedText, arData);
				FlushPendingRichTextLead(textDoc, "prep-text-leave");
				return textDoc;
			}
		}

		ScopedRichTextPrepTextParse prepTextParseScope(true);
		FontManager::TextDoc* textDoc = FontManager::PrepText(arTextString, arData);
		FlushPendingRichTextLead(textDoc, "prep-text-leave");
		return textDoc;
	}

	void __thiscall FontManagerEx::TextDocRender(NiNode* apNode, FontManager::TextData* apData)
	{
		FontManager::TextDoc* doc = reinterpret_cast<FontManager::TextDoc*>(this);
		ScopedRichTextRenderContext renderContext(doc, apData);
		CallTextDocRender(doc, apNode, apData);
	}

	void __thiscall FontManagerEx::TextDocDestroy()
	{
		FontManager::TextDoc* doc = reinterpret_cast<FontManager::TextDoc*>(this);
		DiscardPendingRichTextLead(doc);
		UInt32 clearedExtraCount = ClearRichTextCharExtrasForDoc(doc);
		UInt32 remainingExtraCount = CountRichTextCharExtrasForDoc(doc);
		LogTextDocDestroy(doc, clearedExtraCount, remainingExtraCount);
		CallTextDocDestroy(doc);
	}

	void __thiscall FontManagerEx::TextDocAddChar(FontManager::CharData* apChar, int aiNewLines, bool abNewPage)
	{
		FontManager::TextDoc* doc = reinterpret_cast<FontManager::TextDoc*>(this);
		HandleTextDocAddChar("TextDoc::AddChar", doc, apChar, aiNewLines, abNewPage);
	}

	FontManager::TextPage* __thiscall FontManagerEx::TextPageAddChar(FontManager::CharData* apChar, int aiNewLines)
	{
		FontManager::TextPage* page = reinterpret_cast<FontManager::TextPage*>(this);
		FontManager::TextPage* addedPage = ThisStdCall<FontManager::TextPage*>(0xA19C00, page, apChar, aiNewLines);

		UInt32 dbcsCode = 0;
		if (!apChar || HasRichTextFilename(apChar) || !TryGetRichTextCharDbcs(apChar, dbcsCode))
			return addedPage;

		Font* font = nullptr;
		FontLetter* glyph = LookupRichTextDbcsGlyph(apChar, dbcsCode, &font);
		if (!font || !font->pFontData || !glyph)
			return addedPage;

		FontManager::TextPage* targetPage = addedPage ? addedPage : page;
		if (targetPage)
			targetPage->iLastFontHeight = GetGlyphLayoutLineHeight(font->pFontData, glyph);

		return addedPage;
	}

	FontManager::TextLine* __thiscall FontManagerEx::TextLineAddChar(FontManager::CharData* apChar, bool abAddHead)
	{
		FontManager::TextLine* line = reinterpret_cast<FontManager::TextLine*>(this);
		if (!ShouldStartNewLineForRichTextDbcs(line, apChar, abAddHead))
			return ThisStdCall<FontManager::TextLine*>(0xA19F70, line, apChar, abAddHead);

		void* lineMem = MemoryManager_s_Instance->Allocate(sizeof(FontManager::TextLine));
		if (!lineMem)
			return ThisStdCall<FontManager::TextLine*>(0xA19F70, line, apChar, abAddHead);

		return ThisStdCall<FontManager::TextLine*>(0xA1BD40,
			lineMem, line->pPage, apChar, 0, line->iPageWidth);
	}

	FontManager::CharData* __fastcall FontManagerEx::CharDataCopy(FontManager::CharData* apChar, void*)
	{
		FontManager::CharData* copiedChar = ThisStdCall<FontManager::CharData*>(0xA1B660, apChar);
		if (!copiedChar)
			return nullptr;

		RichTextCharExtra extra = {};
		if (TryGetRichTextCharExtra(apChar, extra))
			SetRichTextCharDbcs(copiedChar, extra.dbcsCode, extra.textDoc);
		else
			ClearRichTextCharExtra(copiedChar);

		return copiedChar;
	}

} // namespace fonthook

#include "font_manager.h"
#include "dictionary.h"
#include "native_calls.h"
#include <cmath>
#include <string>
#include <vector>

namespace fonthook
{
	namespace
	{
		static constexpr UInt32 kInitialAddCharLogCount = 8;
		static constexpr UInt32 kRichTextHookEnterLogLimit = 64;
		static constexpr UInt32 kRichTextHookEnterTextPreviewLimit = 1024;
		static constexpr UInt32 kRichTextUtf8ProbeLogLimit = 32;
		static constexpr UInt32 kRichTextDbcsMergeLogLimit = 32;
		static constexpr UInt32 kRichTextDbcsFailureLogLimit = 64;
		static constexpr UInt32 kRichTextCollectToScanLogLimit = 32;
		static constexpr UInt32 kRichTextCollectToRiskLogLimit = 64;
		std::unordered_map<const FontManager::CharData*, RichTextCharExtra> sRichTextCharExtras;
		std::vector<FontManager::CharData*> sRichTextRenderAddChars;
		FontManager::TextDoc* sRichTextRenderDoc = nullptr;
		UInt32 sRichTextRenderAddCharIndex = 0;
		UInt32 sRichTextHookEnterLogCount = 0;
		UInt32 sRichTextUtf8ProbeLogCount = 0;
		UInt32 sRichTextDbcsMergeLogCount = 0;
		UInt32 sRichTextDbcsFailureLogCount = 0;
		UInt32 sRichTextCollectToScanLogCount = 0;
		UInt32 sRichTextCollectToRiskLogCount = 0;

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

		using ExtraGlyphMap = std::unordered_map<UInt32, FontLetter>;

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

		void LogRichTextHookEnter(const char* hookName, BSStringT<char>& arTextString, FontManager::TextData& arData)
		{
			if (sRichTextHookEnterLogCount >= kRichTextHookEnterLogLimit)
				return;
			++sRichTextHookEnterLogCount;

			const char* text = arTextString.pString ? arTextString.pString : "";
			UInt32 textLength = arTextString.pString ? arTextString.GetLength() : 0;
			UInt32 previewLength = textLength;
			bool truncated = false;
			if (previewLength > kRichTextHookEnterTextPreviewLimit)
			{
				previewLength = kRichTextHookEnterTextPreviewLimit;
				truncated = true;
			}
			std::string textPreview(text, text + previewLength);
			if (truncated)
				textPreview += "\n...<truncated>";

			gLog.FormattedMessage(
				"tnvse_rich_text:\n"
				"  hook=%s\n"
				"  phase=enter\n"
				"  textLen=%u\n"
				"  textPreviewLen=%u truncated=%d\n"
				"  data: font=%d justify=%d width=%d height=%d page=%d hyper=%d\n"
				"  textBegin:\n"
				"%s\n"
				"  textEnd",
				hookName,
				textLength,
				previewLength,
				truncated ? 1 : 0,
				arData.iDefaultFont,
				arData.iJustification,
				arData.iWidth,
				arData.iHeight,
				arData.iPageNum,
				arData.bIsHypertext ? 1 : 0,
				textPreview.c_str());
		}

		bool HasRichTextExtraGlyphs()
		{
			return !gNumberedExtraLetters.empty();
		}

		bool ShouldConvertRichTextInput()
		{
			return sRichTextConvertedInputDepth == 0 && HasRichTextExtraGlyphs();
		}

		void LogRichTextUtf8Conversion(const char* hookName, UInt32 originalLength, UInt32 convertedLength)
		{
			gLog.FormattedMessage(
				"tnvse_rich_text_convert:\n"
				"  hook=%s\n"
				"  source=UTF-8\n"
				"  targetCodePage=%u\n"
				"  originalLen=%u convertedLen=%u",
				hookName,
				g_usingWinEncoding,
				originalLength,
				convertedLength);
		}

		bool HasHighBytes(const char* apText)
		{
			if (!apText)
				return false;

			for (const UInt8* p = reinterpret_cast<const UInt8*>(apText); *p; ++p)
			{
				if (*p >= 0x80)
					return true;
			}

			return false;
		}

		void LogRichTextUtf8Probe(const char* hookName, const char* reason, UInt32 textLength)
		{
			if (sRichTextUtf8ProbeLogCount >= kRichTextUtf8ProbeLogLimit)
				return;
			++sRichTextUtf8ProbeLogCount;

			gLog.FormattedMessage(
				"tnvse_rich_text_convert_probe:\n"
				"  hook=%s\n"
				"  result=%s\n"
				"  targetCodePage=%u\n"
				"  textLen=%u",
				hookName,
				reason,
				g_usingWinEncoding,
				textLength);
		}

		bool TryConvertRichTextInput(
			const char* hookName,
			BSStringT<char>& arTextString,
			const char*& arParserText,
			std::string& arConvertedTextStorage)
		{
			const char* text = arTextString.pString ? arTextString.pString : "";
			UInt32 textLength = arTextString.pString ? arTextString.GetLength() : 0;
			if (!ShouldConvertRichTextInput())
			{
				if (HasHighBytes(text))
					LogRichTextUtf8Probe(hookName, "skip-disabled-or-no-extra-glyphs", textLength);
				return false;
			}

			if (!HasHighBytes(text))
				return false;

			if (!IsValidUTF8With3ByteMin(text))
			{
				LogRichTextUtf8Probe(hookName, "skip-not-valid-utf8", textLength);
				return false;
			}

			arConvertedTextStorage = UTF8ToMultiByteStr(text, g_usingWinEncoding);
			if (arConvertedTextStorage.empty())
			{
				LogRichTextUtf8Probe(hookName, "fail-empty-conversion", textLength);
				return false;
			}

			arParserText = arConvertedTextStorage.c_str();
			LogRichTextUtf8Conversion(hookName, textLength, (UInt32)arConvertedTextStorage.size());
			return true;
		}

		char PrintableAscii(UInt8 c)
		{
			return (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
		}

		bool IsRichTextCollectToRiskTrail(UInt8 c)
		{
			switch (c)
			{
			case '\t':
			case '\n':
			case '\r':
			case ' ':
			case '"':
			case '\'':
			case '&':
			case '/':
			case ';':
			case '<':
			case '=':
			case '>':
			case '{':
			case '|':
			case '}':
				return true;
			default:
				return false;
			}
		}

		void LogRichTextCollectToRisk(
			const char* hookName,
			UInt32 pos,
			UInt32 dbcsCode,
			UInt8 lead,
			UInt8 trail)
		{
			if (sRichTextCollectToRiskLogCount >= kRichTextCollectToRiskLogLimit)
				return;
			++sRichTextCollectToRiskLogCount;

			gLog.FormattedMessage(
				"tnvse_rich_text_collectto_risk:\n"
				"  hook=%s\n"
				"  pos=%u\n"
				"  dbcsCode=0x%04X\n"
				"  bytes=0x%02X 0x%02X trailAscii='%c'",
				hookName,
				pos,
				dbcsCode,
				lead,
				trail,
				PrintableAscii(trail));
		}

		void LogRichTextCollectToScan(
			const char* hookName,
			BSStringT<char>& arTextString)
		{
			if (!HasRichTextExtraGlyphs())
				return;

			const char* text = arTextString.pString ? arTextString.pString : "";
			UInt32 textLength = arTextString.pString ? arTextString.GetLength() : 0;
			if (!HasHighBytes(text))
				return;

			const UInt8* bytes = reinterpret_cast<const UInt8*>(text);
			UInt32 dbcsPairs = 0;
			UInt32 delimiterTrailPairs = 0;
			UInt32 unmatchedHighBytes = 0;
			for (UInt32 i = 0; i < textLength; ++i)
			{
				if (bytes[i] < 0x80)
					continue;

				UInt32 dbcsCode = 0;
				if (i + 1 < textLength && TryDecodeDoubleByte(reinterpret_cast<const char*>(&bytes[i]), dbcsCode))
				{
					++dbcsPairs;
					UInt8 trail = bytes[i + 1];
					if (IsRichTextCollectToRiskTrail(trail))
					{
						++delimiterTrailPairs;
						LogRichTextCollectToRisk(hookName, i, dbcsCode, bytes[i], trail);
					}
					++i;
					continue;
				}

				++unmatchedHighBytes;
			}

			if (!dbcsPairs && !unmatchedHighBytes)
				return;
			if (sRichTextCollectToScanLogCount >= kRichTextCollectToScanLogLimit)
				return;
			++sRichTextCollectToScanLogCount;

			gLog.FormattedMessage(
				"tnvse_rich_text_collectto_scan:\n"
				"  hook=%s\n"
				"  targetCodePage=%u\n"
				"  textLen=%u\n"
				"  dbcsPairs=%u delimiterTrailPairs=%u unmatchedHighBytes=%u",
				hookName,
				g_usingWinEncoding,
				textLength,
				dbcsPairs,
				delimiterTrailPairs,
				unmatchedHighBytes);
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

			auto it = gNumberedExtraLetters.find(font->iFontNum);
			return it != gNumberedExtraLetters.end() ? &it->second : nullptr;
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
			if (!extraGlyphs)
				return nullptr;

			auto it = extraGlyphs->find(auiDbcsCode);
			return it != extraGlyphs->end() ? &it->second : nullptr;
		}

		void ApplyRichTextGlyphMetrics(FontManager::CharData* apChar, Font* apFont, const FontLetter* apGlyph)
		{
			if (!apChar || !apFont || !apFont->pFontData || !apGlyph)
				return;

			apChar->iWidth = ConditionalFloatToUInt(apGlyph->fWidth + apGlyph->fSpacing);
			apChar->iRise = ConditionalFloatToUInt(apFont->pFontData->fBaseLine);
			apChar->iDrop = ConditionalFloatToUInt(-apFont->fMaxDrop);
			apChar->iLeadingEdge = 0;
			apChar->iX = 0;
		}

		void InitRichTextLineBreakMarker(FontManager::CharData* apChar)
		{
			if (!apChar)
				return;

			ClearRichTextCharExtra(apChar);
			apChar->cChar = ' ';
			apChar->iWidth = 0;
			apChar->iRise = 0;
			apChar->iDrop = 0;
			apChar->iLeadingEdge = 0;
			apChar->iX = 0;
		}

		void LogRichTextDbcsMerge(
			const char* callsite,
			FontManager::TextDoc* apDoc,
			const FontManager::CharData* apLead,
			const FontManager::CharData* apTrail,
			UInt32 auiDbcsCode,
			const FontLetter* apGlyph)
		{
			if (sRichTextDbcsMergeLogCount >= kRichTextDbcsMergeLogLimit)
				return;
			++sRichTextDbcsMergeLogCount;

			gLog.FormattedMessage(
				"tnvse_rich_text_dbcs:\n"
				"  callsite=%s\n"
				"  action=merge-lead-trail\n"
				"  textDoc=0x%08X lead=0x%08X trail=0x%08X\n"
				"  bytes=0x%02X 0x%02X dbcsCode=0x%04X\n"
				"  glyph=0x%08X texture=%d width=%.3f height=%.3f spacing=%.3f",
				callsite,
				(UInt32)apDoc,
				(UInt32)apLead,
				(UInt32)apTrail,
				apLead ? apLead->cChar : 0,
				apTrail ? apTrail->cChar : 0,
				auiDbcsCode,
				(UInt32)apGlyph,
				apGlyph ? apGlyph->iTextureIndex : -1,
				apGlyph ? apGlyph->fWidth : 0.0f,
				apGlyph ? apGlyph->fHeight : 0.0f,
				apGlyph ? apGlyph->fSpacing : 0.0f);
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

		bool ShouldLogTextDocAddChar(const FontManager::CharData* apChar, UInt32 callCount)
		{
			if (callCount < kInitialAddCharLogCount || !apChar)
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

			SetRichTextCharDbcs(lead, dbcsCode);
			ApplyRichTextGlyphMetrics(lead, font, glyph);
			LogRichTextDbcsMerge(arPending.callsite, apDoc, lead, apTrail, dbcsCode, glyph);
			CallTextDocAddChar(apDoc, lead, arPending.newLines, arPending.newPage);
			InitRichTextLineBreakMarker(apTrail);
			CallTextDocAddChar(apDoc, apTrail, 0, false);
			return true;
		}

		void HandleTextDocAddChar(
			const char* callsite,
			FontManager::TextDoc* apDoc,
			FontManager::CharData* apChar,
			int aiNewLines,
			bool abNewPage,
			UInt32& arCallCount)
		{
			LogTextDocAddChar(callsite, apDoc, apChar, aiNewLines, abNewPage, arCallCount);

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

		FontManager::TextDoc* textDoc = nullptr;
		const char* text = arTextString.pString ? arTextString.pString : "";
		const char* parserText = text;
		std::string convertedTextStorage;
		bool usedConvertedText = false;
		if (TryConvertRichTextInput("FontManager::PrepHypertext",
			arTextString, parserText, convertedTextStorage))
		{
			BSStringT<char> convertedText;
			if (convertedText.Set(parserText))
			{
				usedConvertedText = true;
				if (sRichTextPrepTextParseDepth == 0)
					LogRichTextCollectToScan("FontManager::PrepHypertext", convertedText);
				ScopedRichTextConvertedInput convertedScope(true);
				textDoc = FontManager::PrepHypertext(convertedText, arData);
			}
		}

		if (!usedConvertedText)
		{
			if (sRichTextPrepTextParseDepth == 0)
				LogRichTextCollectToScan("FontManager::PrepHypertext", arTextString);
			textDoc = FontManager::PrepHypertext(arTextString, arData);
		}

		FlushPendingRichTextLead(textDoc, "prep-hypertext-leave");
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

		const char* text = arTextString.pString ? arTextString.pString : "";
		const char* parserText = text;
		std::string convertedTextStorage;
		if (TryConvertRichTextInput("FontManager::PrepText",
			arTextString, parserText, convertedTextStorage))
		{
			BSStringT<char> convertedText;
			if (convertedText.Set(parserText))
			{
				LogRichTextCollectToScan("FontManager::PrepText", convertedText);
				ScopedRichTextConvertedInput convertedScope(true);
				ScopedRichTextPrepTextParse prepTextParseScope(true);
				FontManager::TextDoc* textDoc = FontManager::PrepText(convertedText, arData);
				FlushPendingRichTextLead(textDoc, "prep-text-leave");
				return textDoc;
			}
		}

		LogRichTextCollectToScan("FontManager::PrepText", arTextString);
		ScopedRichTextPrepTextParse prepTextParseScope(true);
		FontManager::TextDoc* textDoc = FontManager::PrepText(arTextString, arData);
		FlushPendingRichTextLead(textDoc, "prep-text-leave");
		return textDoc;
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
		DiscardPendingRichTextLead(doc);
		UInt32 clearedExtraCount = ClearRichTextCharExtrasForDoc(doc);
		LogTextDocDestroy(doc, clearedExtraCount);
		CallTextDocDestroy(doc);
	}

	void __thiscall FontManagerEx::TextDocAddChar_A178A4(FontManager::CharData* apChar, int aiNewLines, bool abNewPage)
	{
		static UInt32 sCallCount = 0;
		FontManager::TextDoc* doc = reinterpret_cast<FontManager::TextDoc*>(this);
		HandleTextDocAddChar("0xA178A4 PrepHypertext", doc, apChar, aiNewLines, abNewPage, sCallCount);
	}

	void __thiscall FontManagerEx::TextDocAddChar_A179D9(FontManager::CharData* apChar, int aiNewLines, bool abNewPage)
	{
		static UInt32 sCallCount = 0;
		FontManager::TextDoc* doc = reinterpret_cast<FontManager::TextDoc*>(this);
		HandleTextDocAddChar("0xA179D9 PrepHypertext", doc, apChar, aiNewLines, abNewPage, sCallCount);
	}

	void __thiscall FontManagerEx::TextDocAddChar_A17FC2(FontManager::CharData* apChar, int aiNewLines, bool abNewPage)
	{
		static UInt32 sCallCount = 0;
		FontManager::TextDoc* doc = reinterpret_cast<FontManager::TextDoc*>(this);
		HandleTextDocAddChar("0xA17FC2 PrepHypertext", doc, apChar, aiNewLines, abNewPage, sCallCount);
	}

	void __thiscall FontManagerEx::TextDocAddChar_A18D7C(FontManager::CharData* apChar, int aiNewLines, bool abNewPage)
	{
		static UInt32 sCallCount = 0;
		FontManager::TextDoc* doc = reinterpret_cast<FontManager::TextDoc*>(this);
		HandleTextDocAddChar("0xA18D7C PrepText", doc, apChar, aiNewLines, abNewPage, sCallCount);
	}

} // namespace fonthook

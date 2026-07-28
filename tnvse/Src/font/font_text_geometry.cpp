#include "font_engine.h"
#include "dictionary.h"
#include "font_glyphs.h"
#include "game_hooks.h"
#include "font_manager.h"
#include "font_vector.h"
#include "font_vector_internal.h"
#include "native_calls.h"
#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <vector>

namespace fonthook
{
	static constexpr UInt32 kInitialRenderAddCharLogCount = 0;

	static float GetFreeTypeLineOffset(FontEx* font, const char* text,
		float requestedWrapWidth, UInt32 flags, UInt32 startCharIndex)
	{
		const UInt32 justification = flags & 0x0F;
		if (justification != 2 && justification != 4)
			return 0.0f;
		const float wrapWidth = requestedWrapWidth > 0.0f
			? requestedWrapWidth : std::numeric_limits<float>::max();
		NiPoint3 dimensions = {};
		if (!MeasureFreeTypeSingleByteText(font, text, wrapWidth,
			startCharIndex, dimensions))
		{
			return 0.0f;
		}
		return justification == 2 ? dimensions.x * -0.5f : -dimensions.x;
	}

	static int GetBaseFontGlyphIndex(const Font* apFont, const FontLetter* apLetter)
	{
		if (!apFont || !apFont->pFontData || !apLetter)
			return -1;

		const UInt32 glyphAddr = reinterpret_cast<UInt32>(apLetter);
		const UInt32 glyphBegin = reinterpret_cast<UInt32>(apFont->pFontData->pFontLetters);
		const UInt32 glyphEnd = glyphBegin + (sizeof(FontLetter) * kMaxGlyphCount);
		if (glyphAddr < glyphBegin || glyphAddr >= glyphEnd)
			return -1;

		const UInt32 glyphOffset = glyphAddr - glyphBegin;
		if (glyphOffset % sizeof(FontLetter))
			return -1;

		return glyphOffset / sizeof(FontLetter);
	}

	static FontLetter* ResolveRichTextRenderGlyph(
		Font* apFont,
		const RichTextRenderAddCharInfo& arRenderInfo,
		bool abHasRenderInfo,
		FontLetter* apOriginalLetter)
	{
		if (!abHasRenderInfo || !apFont || !arRenderInfo.charData)
			return apOriginalLetter;

		UInt32 dbcsCode = 0;
		if (!TryGetRichTextCharDbcs(arRenderInfo.charData, dbcsCode))
			return apOriginalLetter;

		FontLetter* glyph = LookupDBGlyph(GetExtraGlyphs(apFont->iFontNum), dbcsCode);
		return glyph ? glyph : apOriginalLetter;
	}

	static bool ShouldLogTextDocRenderAddChar(
		const RichTextRenderAddCharInfo& arRenderInfo,
		bool abHasRenderInfo,
		const Font* apFont,
		const FontLetter* apLetter,
		UInt32 callCount)
	{
		if (callCount < kInitialRenderAddCharLogCount)
			return true;

		const FontManager::CharData* ch = arRenderInfo.charData;

		int glyphIndex = GetBaseFontGlyphIndex(apFont, apLetter);
		if (glyphIndex < 0)
			return true;

		if (abHasRenderInfo && !ch)
			return true;

		return ch && glyphIndex != ch->cChar;
	}

	static void LogTextDocRenderAddChar(
		Font* apFont,
		const RichTextRenderAddCharInfo& arRenderInfo,
		bool abHasRenderInfo,
		FontLetter* apLetter,
		FontLetter* apRenderLetter,
		int aiVert,
		NiTriShape* apShape,
		NiPoint3* apPosition,
		const NiColorA* apColor,
		UInt32& arCallCount)
	{
		if (!g_bEnableFreeTypeFontRenderingLog)
			return;
		UInt32 callCount = abHasRenderInfo ? arRenderInfo.callIndex : arCallCount++;
		if (!ShouldLogTextDocRenderAddChar(arRenderInfo, abHasRenderInfo, apFont, apLetter, callCount))
			return;

		const FontManager::CharData* ch = arRenderInfo.charData;
		UInt32 dbcsCode = 0;
		bool isDbcs = TryGetRichTextCharDbcs(ch, dbcsCode);
		UInt8 charByte = ch ? ch->cChar : 0;
		char printable = (charByte >= 0x20 && charByte < 0x7F) ? (char)charByte : '.';
		int glyphIndex = GetBaseFontGlyphIndex(apFont, apLetter);
		const char* fontFile = apFont && apFont->pFontFile ? apFont->pFontFile : "";

		gLog.FormattedMessage(
			"tnvse_rich_text_render_addchar:\n"
			"  callsite=0xA19622 TextDoc::Render -> Font::AddChar\n"
			"  callIndex=%u\n"
			"  renderContext: active=%d textDoc=0x%08X expectedAddChars=%u charData=0x%08X\n"
			"  font=0x%08X fontNum=%d fontFile=\"%s\"\n"
			"  char: byte=0x%02X ascii='%c' font=%d width=%d rise=%d drop=%d leading=%d x=%d richDbcs=%d dbcsCode=0x%04X\n"
			"  args: letter=0x%08X vert=%d shape=0x%08X pos=0x%08X color=0x%08X\n"
			"  glyph: baseGlyphIndex=%d renderLetter=0x%08X replaced=%d texture=%d width=%.3f height=%.3f leading=%.3f spacing=%.3f top=%.3f\n"
			"  pos: x=%.3f y=%.3f z=%.3f\n"
			"  color: r=%.3f g=%.3f b=%.3f a=%.3f",
			callCount,
			abHasRenderInfo ? 1 : 0,
			reinterpret_cast<UInt32>(arRenderInfo.textDoc),
			arRenderInfo.expectedAddCharCount,
			reinterpret_cast<UInt32>(ch),
			reinterpret_cast<UInt32>(apFont),
			apFont ? apFont->iFontNum : -1,
			fontFile,
			charByte,
			printable,
			ch ? ch->iFontIndex : -1,
			ch ? ch->iWidth : -1,
			ch ? ch->iRise : -1,
			ch ? ch->iDrop : -1,
			ch ? ch->iLeadingEdge : -1,
			ch ? ch->iX : -1,
			isDbcs ? 1 : 0,
			isDbcs ? dbcsCode : 0,
			reinterpret_cast<UInt32>(apLetter),
			aiVert,
			reinterpret_cast<UInt32>(apShape),
			reinterpret_cast<UInt32>(apPosition),
			reinterpret_cast<UInt32>(apColor),
			glyphIndex,
			reinterpret_cast<UInt32>(apRenderLetter),
			apRenderLetter != apLetter ? 1 : 0,
			apRenderLetter ? apRenderLetter->iTextureIndex : -1,
			apRenderLetter ? apRenderLetter->fWidth : 0.0f,
			apRenderLetter ? apRenderLetter->fHeight : 0.0f,
			apRenderLetter ? apRenderLetter->fLeadingEdge : 0.0f,
			apRenderLetter ? apRenderLetter->fSpacing : 0.0f,
			apRenderLetter ? apRenderLetter->fTopEdge : 0.0f,
			apPosition ? apPosition->x : 0.0f,
			apPosition ? apPosition->y : 0.0f,
			apPosition ? apPosition->z : 0.0f,
			apColor ? apColor->r : 0.0f,
			apColor ? apColor->g : 0.0f,
			apColor ? apColor->b : 0.0f,
			apColor ? apColor->a : 0.0f);
	}

	static std::string EscapeFreeTypeLayoutText(const char* text)
	{
		static constexpr size_t kMaxLoggedBytes = 160;
		std::string escaped;
		if (!text)
			return escaped;
		escaped.reserve(kMaxLoggedBytes + 8);
		const char hex[] = "0123456789ABCDEF";
		size_t byteCount = 0;
		for (; text[byteCount] && byteCount < kMaxLoggedBytes; ++byteCount)
		{
			const UInt8 value = static_cast<UInt8>(text[byteCount]);
			switch (value)
			{
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default:
				if (value < 0x20 || value == 0x7F)
				{
					escaped += "\\x";
					escaped.push_back(hex[value >> 4]);
					escaped.push_back(hex[value & 0x0F]);
				}
				else
				{
					escaped.push_back(static_cast<char>(value));
				}
				break;
			}
		}
		if (text[byteCount])
			escaped += "...";
		return escaped;
	}

	static void LogFreeTypeOrdinaryAlignmentOnce(
		const FontEx* font,
		int flags,
		int preparedWidth,
		float alignmentOffset,
		const char* preparedText)
	{
		static std::unordered_set<UInt64> logged;
		if (!g_bEnableFreeTypeFontRenderingLog
			|| !font || font->iFontNum < 0)
			return;
		bool hasVisibleText = false;
		for (const UInt8* cursor = reinterpret_cast<const UInt8*>(preparedText);
			cursor && *cursor; ++cursor)
		{
			if (*cursor > 0x20 && *cursor != kDelChar)
			{
				hasVisibleText = true;
				break;
			}
		}
		if (!hasVisibleText)
			return;

		const int alignmentIndex = flags == 2 ? 1 : flags == 4 ? 2 : 0;
		const UInt64 logKey = (static_cast<UInt64>(
			static_cast<UInt32>(font->iFontNum)) << 2)
			| static_cast<UInt32>(alignmentIndex);
		if (!logged.insert(logKey).second)
			return;

		const char* alignmentName = flags == 2 ? "center" : flags == 4 ? "right" : "left";
		const std::string escaped = EscapeFreeTypeLayoutText(preparedText);
		FreeTypeFontDebugLog(
			"tnvse_freetype_font: ordinary layout font=%d flags=%d alignment=%s preparedWidth=%d offset=%.3f text=\"%s\"",
			font->iFontNum,
			flags,
			alignmentName,
			preparedWidth,
			alignmentOffset,
			escaped.c_str());
	}

	static void LogFreeTypeLineDrift(
		const FontEx* font,
		int flags,
		int lineIndex,
		int preparedWidth,
		float renderedAdvance,
		int trailingWhitespaceCount,
		float trailingWhitespaceWidth,
		const char* preparedText)
	{
		static UInt32 driftLogCount = 0;
		static constexpr UInt32 kDriftLogLimit = 64;
		if (!g_bEnableFreeTypeFontRenderingLog)
			return;
		const float drift = renderedAdvance - static_cast<float>(preparedWidth);
		if (std::fabs(drift) < 0.001f || driftLogCount >= kDriftLogLimit)
			return;
		++driftLogCount;
		const std::string escaped = EscapeFreeTypeLayoutText(preparedText);
		FreeTypeFontDebugLog(
			"tnvse_freetype_font: layout drift font=%d flags=%d line=%d prepared=%d rendered=%.3f drift=%.3f trailingWhitespace=%d trailingWidth=%.3f text=\"%s\"",
			font ? font->iFontNum : -1,
			flags,
			lineIndex,
			preparedWidth,
			renderedAdvance,
			drift,
			trailingWhitespaceCount,
			trailingWhitespaceWidth,
			escaped.c_str());
	}

	static UInt32 CreateFreeTypePreparedText(
		FontEx* font,
		Font::TextData& textData,
		int* outputWidth,
		int aiFlags,
		char aiLineBreakChar,
		const NiColorA* fontColor,
		NiTriShape** textShape,
		NiTriShape** iconShape,
		float rasterScale)
	{
		*textShape = nullptr;
		*iconShape = nullptr;
		VectorTextBuilder builder(font, true, rasterScale, fontColor);
		if (!builder.IsAvailable())
		{
			*textShape = CreateEmptyFreeTypeTextShape(font, true);
			font->ButtonIcons.Clear(1);
			return ThisStdCall<UInt32>(0x7593E0, reinterpret_cast<char*>(&textData));
		}
		builder.ReserveGlyphs(static_cast<size_t>(std::max(0, textData.iCharCount)));
		if (outputWidth)
			*outputWidth = textData.iWidth;
		const char* preparedText = textData.xNewText.c_str();

		BSSimpleList<int>* lineWidthCursor = &textData.xLineWidths;
		int lineWidth = lineWidthCursor ? lineWidthCursor->m_item : 0;
		float lineOrigin = aiFlags == 4 ? -static_cast<float>(lineWidth)
			: aiFlags == 2 ? static_cast<float>(lineWidth) * -0.5f : 0.0f;
		LogFreeTypeOrdinaryAlignmentOnce(
			font, aiFlags, lineWidth, lineOrigin, preparedText);

		NiPoint3 position;
		position.x = lineOrigin;
		const double lineBaseOffset = font->pFontData->fBaseLine - font->fFontHeight;
		position.z = static_cast<float>(lineBaseOffset + lineBaseOffset);
		position.y = 0.0f;

		NiTriShape* icons = nullptr;
		if (font->ButtonIcons.uiSize)
		{
			icons = font->MakeIconsTriShape();
			*iconShape = icons;
			if (icons)
			{
				icons->m_kLocal.m_Translate = NiPoint3(0.0f, position.y, position.z);
				ThisStdCall(0xA67050, icons->GetModelData(), 0x4000);
			}
		}

		const float linePadding = FontManager::GetLinePadding(font->iFontNum);
		int lineIndex = 0;
		float lineStartX = lineOrigin;
		int trailingWhitespaceCount = 0;
		float trailingWhitespaceWidth = 0.0f;
		int iconIndex = 0;
		auto advanceLine = [&]()
		{
			LogFreeTypeLineDrift(font, aiFlags, lineIndex, lineWidth,
				position.x - lineStartX, trailingWhitespaceCount,
				trailingWhitespaceWidth, preparedText);
			++lineIndex;
			if (lineWidthCursor && lineWidthCursor->m_pkNext)
				lineWidthCursor = lineWidthCursor->m_pkNext;
			lineWidth = lineWidthCursor ? lineWidthCursor->m_item : 0;
			lineOrigin = aiFlags == 4 ? -static_cast<float>(lineWidth)
				: aiFlags == 2 ? static_cast<float>(lineWidth) * -0.5f : 0.0f;
			position.x = lineOrigin;
			lineStartX = lineOrigin;
			trailingWhitespaceCount = 0;
			trailingWhitespaceWidth = 0.0f;
			position.z -= font->pFontData->fBaseLine + linePadding;
		};
		auto advanceTab = [&]()
		{
			const float beforeTab = position.x;
			position.x += static_cast<float>(kTabWidth)
				- fmodf(position.x, static_cast<float>(kTabWidth));
			++trailingWhitespaceCount;
			trailingWhitespaceWidth += position.x - beforeTab;
		};
		auto addIcon = [&]()
		{
			if (icons && font->ButtonIcons.pBuffer)
				font->AddIcon(iconIndex++, icons, &position);
			else
				++iconIndex;
			trailingWhitespaceCount = 0;
			trailingWhitespaceWidth = 0.0f;
		};
		auto addGlyph = [&](const VectorEncodedGlyph& glyph,
			float glyphAdvance)
		{
			builder.AddGlyph(glyph, position, fontColor);
			position.x += glyphAdvance;
			if (glyph.encodedCode == kSpaceChar || glyph.encodedCode == kNBSPChar)
			{
				++trailingWhitespaceCount;
				trailingWhitespaceWidth += glyphAdvance;
			}
			else
			{
				trailingWhitespaceCount = 0;
				trailingWhitespaceWidth = 0.0f;
			}
		};

		const std::shared_ptr<const PreparedDirectTextSidecar>
			preparedSidecar = ConsumeFreeTypePreparedTextSidecar(
				&textData, font, preparedText);
		if (preparedSidecar && preparedSidecar->rejectBatch)
		{
			*textShape = CreateEmptyFreeTypeTextShape(font, true);
			font->ButtonIcons.Clear(1);
			return ThisStdCall<UInt32>(0x7593E0,
				reinterpret_cast<char*>(&textData));
		}
		const std::shared_ptr<const PreparedDirectTextSidecar>
			directSidecar = builder.UsesSealedDirectProfile()
				? preparedSidecar
				: std::shared_ptr<const PreparedDirectTextSidecar>();
		if (directSidecar)
		{
			for (const DirectTextUnit& unit :
				directSidecar->units)
			{
				switch (unit.kind)
				{
				case DirectTextUnitKind::LineBreak:
					advanceLine();
					break;
				case DirectTextUnitKind::Tab:
					advanceTab();
					break;
				case DirectTextUnitKind::Icon:
					addIcon();
					break;
				case DirectTextUnitKind::Glyph:
				{
					VectorEncodedGlyph glyph;
					glyph.encodedCode = unit.encodedCode;
					glyph.directSlot = unit.directSlot;
					glyph.hasDirectMetrics = true;
					glyph.byteLength = unit.byteLength;
					glyph.byteClass =
						static_cast<VectorFontByteClass>(
							unit.byteClass);
					addGlyph(glyph, unit.advance);
					break;
				}
				default:
					break;
				}
			}
		}
		else
		{
			for (int byteIndex = 0;
				preparedText[byteIndex]; ++byteIndex)
			{
				const UInt8 current =
					static_cast<UInt8>(preparedText[byteIndex]);
				if (current
					== static_cast<UInt8>(aiLineBreakChar))
				{
					advanceLine();
					continue;
				}
				if (current == '\t')
				{
					advanceTab();
					continue;
				}
				if (current == 1)
				{
					addIcon();
					continue;
				}
				if (current < 0x20 || current == kDelChar)
					continue;

				VectorEncodedGlyph glyph;
				if (!builder.AddEncodedGlyph(
					&preparedText[byteIndex],
					position, fontColor, &glyph))
				{
					continue;
				}
				const float glyphAdvance =
					GetVectorGlyphRenderAdvance(glyph);
				position.x += glyphAdvance;
				if (glyph.encodedCode == kSpaceChar
					|| glyph.encodedCode == kNBSPChar)
				{
					++trailingWhitespaceCount;
					trailingWhitespaceWidth += glyphAdvance;
				}
				else
				{
					trailingWhitespaceCount = 0;
					trailingWhitespaceWidth = 0.0f;
				}
				byteIndex += glyph.byteLength - 1;
			}
		}
		LogFreeTypeLineDrift(font, aiFlags, lineIndex, lineWidth,
			position.x - lineStartX, trailingWhitespaceCount,
			trailingWhitespaceWidth, preparedText);

		NiTriShape* textObject = builder.Finish();
		if (!textObject)
			textObject = CreateEmptyFreeTypeTextShape(font, true);
		if (textObject)
		{
			const float rootZ = static_cast<float>(lineBaseOffset + lineBaseOffset);
			textObject->m_kLocal.m_Translate = NiPoint3(0.0f, 0.0f,
				std::round(rootZ * rasterScale) / rasterScale);
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				static bool loggedRootTransform = false;
				if (!loggedRootTransform)
				{
					loggedRootTransform = true;
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: ordinary object type=%s local=(%.3f,%.3f,%.3f)",
						"shape",
						textObject->m_kLocal.m_Translate.x,
						textObject->m_kLocal.m_Translate.y,
						textObject->m_kLocal.m_Translate.z);
				}
			}
			*textShape = textObject;
		}
		font->ButtonIcons.Clear(1);
		return ThisStdCall<UInt32>(0x7593E0, reinterpret_cast<char*>(&textData));
	}

	// ==================== FontEx::CreateText ====================
	UInt32 FontEx::CreateText(
		BSStringT<char>* axTextString, int* aiWidth, int* aiHeight,
		int aiLineStart, int aiLineEnd, int aiFlags, char aiLineBreakChar,
		const NiColorA* axFontColor, NiTriShape** apTextShape, NiTriShape** apIconShape)
	{
		const float rasterScale = ConsumeFreeTypeCreateTextScale();
		const bool freeTypeActive = IsFreeTypeFontActive(this);
		if (!g_bEnableMultibyteFontHook && !freeTypeActive)
		{
			return CallOriginalFontCreateText(this, axTextString, aiWidth,
				aiHeight, aiLineStart, aiLineEnd, aiFlags, aiLineBreakChar,
				axFontColor, apTextShape, apIconShape);
		}

		auto* extraGlyphs = GetExtraGlyphs(this->iFontNum);
		Font::TextData textData;

		if (!*aiHeight)
			*aiHeight = kSentinelMax;
		if (!aiLineEnd)
			aiLineEnd = kSentinelMax;

		float linePadding = FontManager::GetLinePadding(this->iFontNum);
		ThisStdCall(0x759330, &textData, *aiWidth, *aiHeight, aiLineStart, aiLineEnd, aiLineBreakChar);

		if (g_bEnableMultibyteFontHook)
		{
			const char* pStr = axTextString->pString;
			std::string sConvertedStr;
			if (ConvertToMultiByte(pStr, sConvertedStr, extraGlyphs != nullptr))
				axTextString->Set(pStr);
			std::string sTranslatedStr;
			if (TranslateText(axTextString->pString, sTranslatedStr))
				axTextString->Set(sTranslatedStr.c_str());
		}

		if (g_bEnableMultibyteFontHook)
			ThisStdCall(0xA12FB0, this, axTextString->pString, &textData);
		else
			PrepText(axTextString->pString, &textData);

		*aiWidth = textData.iWidth;
		*aiHeight = textData.iHeight;

		if (freeTypeActive && IsFreeTypeVuiProxyMeasureOnlyActive())
		{
			NiTriShape* emptyProxy = CreateEmptyFreeTypeTextShape(this, true);
			if (emptyProxy)
			{
				*apTextShape = emptyProxy;
				*apIconShape = nullptr;
				ConsumeFreeTypePreparedTextSidecar(
					&textData, this, textData.xNewText.c_str());
				this->ButtonIcons.Clear(1);
				if (g_bEnableFreeTypeFontRenderingLog)
				{
					static bool loggedVuiProxyMeasureOnly = false;
					if (!loggedVuiProxyMeasureOnly)
					{
						loggedVuiProxyMeasureOnly = true;
						FreeTypeFontDebugLog(
							"tnvse_freetype_font: VUI+ ordinary proxy retained layout and bypassed glyph geometry font=%d",
							this->iFontNum);
					}
				}
				return ThisStdCall<UInt32>(0x7593E0,
					reinterpret_cast<char*>(&textData));
			}
		}

		if (freeTypeActive)
		{
			return CreateFreeTypePreparedText(this, textData, aiWidth,
				aiFlags, aiLineBreakChar, axFontColor, apTextShape, apIconShape,
				rasterScale);
		}
		vectorfont::FreeTypePerfScope extendedFntPerf(
			vectorfont::FreeTypePerfPhase::ExtendedFntGeometry);

		int alignmentOffset = 0;
		if (aiFlags == 4)
			alignmentOffset = -textData.xLineWidths.m_item;
		else if (aiFlags == 2)
			alignmentOffset = textData.xLineWidths.m_item / -2;

		NiPoint3 textPosition;
		textPosition.x = (float)alignmentOffset;
		double lineBaseOffset = this->pFontData->fBaseLine - this->fFontHeight;
		textPosition.z = lineBaseOffset + lineBaseOffset;
		textPosition.y = 0.0;

		int iActualCharCount = AdjustCharCountForDB(
			textData.xNewText.pString, textData.iCharCount, extraGlyphs);

		auto* pTextShape = Font::MakeTriShape(iActualCharCount, axFontColor, 1);
		*apTextShape = pTextShape;
		pTextShape->m_kLocal.m_Translate = NiPoint3(0.0f, textPosition.y, textPosition.z);

		NiTriShape* pIconShape = nullptr;
		if (this->ButtonIcons.uiSize)
		{
			pIconShape = Font::MakeIconsTriShape();
			*apIconShape = pIconShape;
			pIconShape->m_kLocal.m_Translate = NiPoint3(0.0f, textPosition.y, textPosition.z);
			ThisStdCall(0xA67050, pIconShape->GetModelData(), 0x4000);
		}

		float yOffsetStart = textPosition.x;
		int lineCounter = 0;
		int vertexIdx = 0;
		int iconIdx = 0;

		BSSimpleList<int>* pLineWidthCursor = &textData.xLineWidths;

		UInt32 uiDoubleByteCode;
		for (int charIdx = 0; textData.xNewText.pString[charIdx]; ++charIdx)
		{
			if (textData.xNewText.pString[charIdx] == aiLineBreakChar)
			{
				++lineCounter;
				textPosition.x = 0.0;
				if (aiFlags == 4 || aiFlags == 2)
				{
					if (pLineWidthCursor && pLineWidthCursor->m_pkNext)
						pLineWidthCursor = pLineWidthCursor->m_pkNext;
					textPosition.x = (aiFlags == 4)
						? (float)(pLineWidthCursor ? -pLineWidthCursor->m_item : 1)
						: (float)(pLineWidthCursor ? pLineWidthCursor->m_item / -2 : 0);
				}
				textPosition.z = textPosition.z - (this->pFontData->fBaseLine + linePadding);
			}
			else if (textData.xNewText.pString[charIdx] == '\t')
			{
				double tabRemainder = fmod(textPosition.x, 75.0);
				textPosition.x = (float)(textPosition.x + 75.0 - tabRemainder);
			}

			UInt8 currentChar = textData.xNewText.pString[charIdx];

			bool bIsDBCharacter = false;
			if (extraGlyphs)
			{
				UInt8 cNextByte = (UInt8)textData.xNewText.pString[charIdx + 1];
				if (cNextByte != 0)
				{
					bIsDBCharacter = TryDecodeDoubleByte(
						(const char*)&textData.xNewText.pString[charIdx], uiDoubleByteCode);
				}
			}

			if (!bIsDBCharacter)
				ConvertToAsciiQuotes(&currentChar);

			bool rendered = false;

			if (currentChar == 1)
			{
				if (this->ButtonIcons.uiSize && this->ButtonIcons.pBuffer)
					Font::AddIcon(iconIdx++, pIconShape, &textPosition);
			}
			else
			{
				FontLetter* glyph = LookupDBGlyph(extraGlyphs, uiDoubleByteCode);
				if (extraGlyphs && bIsDBCharacter && glyph)
				{
					StdCall<FontLetter*>(0xA142D0, glyph, vertexIdx++,
						pTextShape, &textPosition.x, axFontColor);
					++charIdx;
					rendered = true;
				}
				if (!rendered)
				{
					StdCall<FontLetter*>(0xA142D0, &this->pFontData->pFontLetters[currentChar],
						vertexIdx++, pTextShape, &textPosition.x, axFontColor);
				}
			}
			int maxRenderedWidth = *aiWidth;
			int renderedWidth = ConditionalFloatToUInt(textPosition.x - yOffsetStart);
			*aiWidth = MaxInt(renderedWidth, maxRenderedWidth);
		}

		auto* pTextGeomData = pTextShape->GetModelData();
		ThisStdCall(0xA7EE30, &pTextGeomData->m_kBound,
			pTextGeomData->m_usVertices, pTextGeomData->m_pkVertex);
		if (pIconShape)
		{
			auto* pIconGeomData = pIconShape->GetModelData();
			ThisStdCall(0xA7EE30, &pIconGeomData->m_kBound,
				pIconGeomData->m_usVertices, pIconGeomData->m_pkVertex);
		}
		this->ButtonIcons.Clear(1);
		return ThisStdCall(0x7593E0, (char*)&textData);
	}

	// ==================== FontEx::MakeString ====================
	NiTriShape* FontEx::MakeString(
		float afStartX, float afStartY, float afZ,
		BSStringT<char>* apTextString, int* aiWidth, bool abPrepareObject,
		const NiColorA* arg1C, bool abUpperLeftCorner, bool abPrepareObject_1)
	{
		const bool freeTypeActive = IsFreeTypeFontActive(this);
		if (!g_bEnableMultibyteFontHook && !freeTypeActive)
		{
			return CallOriginalFontMakeString(this, afStartX, afStartY, afZ,
				apTextString, aiWidth, abPrepareObject, arg1C,
				abUpperLeftCorner, abPrepareObject_1);
		}

		auto* extraGlyphs = GetExtraGlyphs(this->iFontNum);

		if (g_bEnableMultibyteFontHook)
		{
			const char* pStr = apTextString->pString;
			std::string sConvertedStr;
			if (ConvertToMultiByte(pStr, sConvertedStr, extraGlyphs != nullptr))
				apTextString->Set(pStr);
			std::string sTranslatedStr;
			if (TranslateText(apTextString->pString, sTranslatedStr))
				apTextString->Set(sTranslatedStr.c_str());
		}

		if (!apTextString->pString || !this->pFontData)
			return 0;

		UInt32 textLen = (apTextString->sLen == 0xFFFF)
			? strlen(apTextString->pString) : apTextString->sLen;
		if (!textLen)
			return 0;

		char newlineBuffer[4];
		float textXOffset = (float)*aiWidth;
		if (freeTypeActive)
		{
			textXOffset = GetFreeTypeLineOffset(
				this,
				apTextString->pString,
				textXOffset,
				static_cast<UInt32>(abPrepareObject),
				0);
		}
		else
		{
			ThisStdCall(0xA12370, this, apTextString->pString, &textXOffset,
				newlineBuffer, abPrepareObject, 0);
		}

		float currentX = afStartX + textXOffset;
		float currentY = afStartY;
		float currentZ = afZ;

		if (abUpperLeftCorner)
		{
			double lineBaseOffset = this->pFontData->pFontLetters[32].fHeight - this->pFontData->fBaseLine;
			currentY = currentY - (lineBaseOffset + lineBaseOffset);
		}

		UInt32 charIdx;
		if (apTextString->sLen == 0xFFFF)
			charIdx = textLen;
		else {
			charIdx = 0;
			for (; charIdx < textLen && apTextString->pString[charIdx]; ++charIdx)
				;
		}

		if (!charIdx)
			return 0;

		if (freeTypeActive)
		{
			const float rasterScale = GetCanonicalFreeTypeRasterScale();
			VectorTextBuilder builder(this, abPrepareObject_1, rasterScale, arg1C);
			if (!builder.IsAvailable())
			{
				NiTriShape* empty = CreateEmptyFreeTypeTextShape(this, abPrepareObject_1);
				if (empty)
					empty->m_kLocal.m_Translate = NiPoint3(afStartX, currentZ, currentY);
				return empty;
			}
			builder.ReserveGlyphs(charIdx);

			const float startY = currentY;
			float lineStartX = currentX;
			NiColorA* activeColor = nullptr;
			NiColorA defaultColor = { 0.0f, 0.0f, 1.0f, 1.0f };
			*aiWidth = 0;
			for (int byteIndex = 0; apTextString->pString[byteIndex]; ++byteIndex)
			{
				const UInt8 current = static_cast<UInt8>(apTextString->pString[byteIndex]);
				if (current == 3)
				{
					activeColor = nullptr;
					continue;
				}
				if (current == 2)
				{
					activeColor = &defaultColor;
					continue;
				}
				if (current == '\t')
				{
					currentX += static_cast<float>(kTabWidth)
						- static_cast<float>(fmod(currentX,
							static_cast<double>(kTabWidth)));
					*aiWidth = MaxInt(*aiWidth, static_cast<int>(std::ceil(
						std::max(0.0f, currentX - lineStartX))));
					continue;
				}
				if (current == '\n')
				{
					float nextLineX = static_cast<float>(*aiWidth);
					if (freeTypeActive)
					{
						nextLineX = GetFreeTypeLineOffset(this,
							apTextString->pString, nextLineX,
							static_cast<UInt32>(abPrepareObject), byteIndex + 1);
					}
					else
					{
						char escapeBuffer[4];
						ThisStdCall(0xA12370, this, apTextString->pString,
							&nextLineX, escapeBuffer, abPrepareObject,
							byteIndex + 1);
					}
					*aiWidth = MaxInt(*aiWidth, static_cast<int>(std::ceil(
						std::max(0.0f, currentX - lineStartX))));
					currentX = nextLineX;
					lineStartX = currentX;
					currentY -= this->pFontData->fBaseLine;
					continue;
				}
				if (current < 0x20 || current == kDelChar)
					continue;

				VectorEncodedGlyph glyph;
				const NiPoint3 pen(currentX, currentZ, currentY);
				if (!builder.AddEncodedGlyph(
					&apTextString->pString[byteIndex],
					pen, activeColor ? activeColor : arg1C, &glyph))
				{
					continue;
				}
				currentX += GetVectorGlyphRenderAdvance(glyph);
				byteIndex += glyph.byteLength - 1;
				*aiWidth = MaxInt(*aiWidth,
					static_cast<int>(std::ceil(std::max(0.0f, currentX - lineStartX))));
			}

			NiTriShape* textObject = builder.Finish();
			if (!textObject)
				textObject = CreateEmptyFreeTypeTextShape(this, abPrepareObject_1);
			if (textObject)
				textObject->m_kLocal.m_Translate = NiPoint3(afStartX, currentZ, startY);
			return textObject;
		}
		vectorfont::FreeTypePerfScope extendedFntPerf(
			vectorfont::FreeTypePerfPhase::ExtendedFntGeometry);

		int iActualCharCount = AdjustCharCountForDB(
			apTextString->pString, charIdx, extraGlyphs, textLen);

		auto* pTriShape = Font::MakeTriShape(iActualCharCount, arg1C, abPrepareObject_1);
		float startY = currentY;
		pTriShape->m_kLocal.m_Translate = NiPoint3(afStartX, currentZ, startY);

		NiColorA* pColor = 0;
		float defaultColor[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
		*aiWidth = 0;
		double lineBaseOffset = currentX;
		int vertexIdx = 0;
		UInt32 uiDoubleByteCode;

		for (int lineIdx = 0; apTextString->pString[lineIdx]; ++lineIdx)
		{
			if (apTextString->pString[lineIdx] == 3)
				pColor = 0;

			char currentCharValue = apTextString->pString[lineIdx];
			if (currentCharValue == '\t')
			{
				double tabRemainder = fmod(currentX, 75.0);
				currentX += 75.0 - tabRemainder;
			}
			else if (currentCharValue == '\n')
			{
				char escapeBuffer[4];
				float tabPrevX = (float)*aiWidth;
				ThisStdCall(0xA12370, this, apTextString->pString, &tabPrevX,
					escapeBuffer, abPrepareObject, lineIdx + 1);
				currentX = tabPrevX;
				currentY = currentY - this->pFontData->fBaseLine;
			}

			UInt8 currentChar = apTextString->pString[lineIdx];

			bool bIsDBCharacter = false;
			if (extraGlyphs)
			{
				UInt8 cNextByte = (UInt8)apTextString->pString[lineIdx + 1];
				if (cNextByte != 0)
				{
					bIsDBCharacter = TryDecodeDoubleByte(
						(const char*)&apTextString->pString[lineIdx], uiDoubleByteCode);
				}
			}

			if (!bIsDBCharacter)
				ConvertToAsciiQuotes(&currentChar);

			bool rendered = false;
			if (extraGlyphs && bIsDBCharacter)
			{
				FontLetter* glyph = LookupDBGlyph(extraGlyphs, uiDoubleByteCode);
				if (glyph)
				{
					StdCall<FontLetter*>(0xA142D0, glyph, vertexIdx++,
						pTriShape, &currentX, pColor);
					lineIdx += 1;
					rendered = true;
				}
			}
			if (!rendered)
			{
				StdCall<FontLetter*>(0xA142D0, &this->pFontData->pFontLetters[currentChar],
					vertexIdx++, pTriShape, &currentX, pColor);
			}

			int prevWidth = *aiWidth;
			int renderedWidth = ConditionalFloatToUInt(currentX - lineBaseOffset);
			*aiWidth = MaxInt(renderedWidth, prevWidth);

			if (apTextString->pString[lineIdx] == 2)
				pColor = (NiColorA*)defaultColor;
		}

		auto* pGeomData = pTriShape->GetModelData();
		ThisStdCall(0xA7EE30, &pGeomData->m_kBound,
			pGeomData->m_usVertices, pGeomData->m_pkVertex);
		return pTriShape;
	}

	void __thiscall FontEx::TextDocRenderAddChar(
		FontLetter* apLetter,
		int aiVert,
		NiTriShape* apShape,
		NiPoint3* apPosition,
		const NiColorA* apColor)
	{
		static UInt32 sCallCount = 0;
		RichTextRenderAddCharInfo renderInfo = {};
		bool hasRenderInfo = TryConsumeRichTextRenderAddChar(renderInfo);
		FontLetter* renderLetter = ResolveRichTextRenderGlyph(this, renderInfo, hasRenderInfo, apLetter);
		LogTextDocRenderAddChar(this, renderInfo, hasRenderInfo, apLetter,
			renderLetter, aiVert, apShape, apPosition, apColor, sCallCount);
		if (hasRenderInfo && apPosition
			&& AddFreeTypeRichTextGlyph(this, renderInfo.charData, *apPosition, apColor))
		{
			static FontLetter emptyLetter = {};
			Font::AddChar(&emptyLetter, aiVert, apShape, apPosition, apColor);
			return;
		}
		Font::AddChar(renderLetter, aiVert, apShape, apPosition, apColor);
	}

} // namespace fonthook

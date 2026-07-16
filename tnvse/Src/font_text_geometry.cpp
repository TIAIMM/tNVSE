#include "font_engine.h"
#include "dictionary.h"
#include "font_glyphs.h"
#include "font_manager.h"
#include "font_vector.h"
#include "native_calls.h"
#include <array>
#include <cmath>
#include <vector>

namespace fonthook
{
	static constexpr UInt32 kInitialRenderAddCharLogCount = 0;

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

	static bool DecodeRenderableVectorGlyph(FontEx* font, const char* text, VectorEncodedGlyph& glyph)
	{
		UInt32 dbcsCode = 0;
		if (text && text[1] && TryDecodeDoubleByte(text, dbcsCode))
			return DecodeFreeTypeGlyph(font, text, glyph);

		char converted[2] = { text ? text[0] : 0, 0 };
		ConvertToAsciiQuotes(reinterpret_cast<UInt8*>(converted));
		return DecodeFreeTypeGlyph(font, converted, glyph);
	}

	static float GetLineAlignmentOffset(int flags, int lineWidth)
	{
		if (flags == 4)
			return static_cast<float>(-lineWidth);
		if (flags == 2)
			return static_cast<float>(lineWidth / -2);
		return 0.0f;
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
		static bool logged[32][3] = {};
		if (!g_bEnableFreeTypeFontRenderingLog
			|| !font || font->iFontNum < 0 || font->iFontNum >= 32)
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
		if (logged[font->iFontNum][alignmentIndex])
			return;
		logged[font->iFontNum][alignmentIndex] = true;

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

	struct PreparedTextMeasuredRun
	{
		size_t byteOffset = 0;
		size_t byteLength = 0;
		FreeTypeLayoutRun layout;
		bool available = false;
	};

	struct PreparedTextLayoutScratch
	{
		std::vector<float> lineWidths;
		std::vector<PreparedTextMeasuredRun> measuredRuns;

		void Reset()
		{
			lineWidths.clear();
			measuredRuns.clear();
		}

		void ReleaseLayoutsAndTrim()
		{
			constexpr size_t kMaximumRetainedLines = 1024;
			constexpr size_t kMaximumRetainedRuns = 8192;
			Reset();
			if (lineWidths.capacity() > kMaximumRetainedLines)
				std::vector<float>().swap(lineWidths);
			if (measuredRuns.capacity() > kMaximumRetainedRuns)
				std::vector<PreparedTextMeasuredRun>().swap(measuredRuns);
		}

		const FreeTypeLayoutRun* FindMeasuredRun(size_t byteOffset,
			size_t byteLength, size_t& cursor) const
		{
			while (cursor < measuredRuns.size()
				&& measuredRuns[cursor].byteOffset < byteOffset)
			{
				++cursor;
			}
			if (cursor >= measuredRuns.size())
				return nullptr;
			const PreparedTextMeasuredRun& measured = measuredRuns[cursor];
			if (measured.byteOffset != byteOffset || measured.byteLength != byteLength)
				return nullptr;
			++cursor;
			return measured.available ? &measured.layout : nullptr;
		}
	};

	struct PreparedTextLayoutScratchPool
	{
		std::array<PreparedTextLayoutScratch, 4> slots;
		size_t depth = 0;
	};

	class PreparedTextLayoutScratchLease
	{
	public:
		explicit PreparedTextLayoutScratchLease(PreparedTextLayoutScratchPool& pool)
			: m_pool(pool)
		{
			m_scratch = m_pool.depth < m_pool.slots.size()
				? &m_pool.slots[m_pool.depth] : &m_fallback;
			++m_pool.depth;
			m_scratch->Reset();
		}

		~PreparedTextLayoutScratchLease()
		{
			m_scratch->ReleaseLayoutsAndTrim();
			--m_pool.depth;
		}

		PreparedTextLayoutScratch& Get() { return *m_scratch; }

	private:
		PreparedTextLayoutScratchPool& m_pool;
		PreparedTextLayoutScratch m_fallback;
		PreparedTextLayoutScratch* m_scratch = nullptr;
	};

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

		thread_local PreparedTextLayoutScratchPool layoutScratchPool;
		PreparedTextLayoutScratchLease layoutScratchLease(layoutScratchPool);
		PreparedTextLayoutScratch& layoutScratch = layoutScratchLease.Get();
		std::vector<float>& exactLineWidths = layoutScratch.lineWidths;
		std::vector<PreparedTextMeasuredRun>& measuredRuns = layoutScratch.measuredRuns;
		const char* const preparedText = textData.xNewText.pString;
		int measureIconIndex = 0;
		const char* measureCursor = preparedText;
		while (measureCursor && *measureCursor)
		{
			const char* lineEnd = std::strchr(measureCursor, aiLineBreakChar);
			const char* end = lineEnd ? lineEnd : measureCursor + std::strlen(measureCursor);
			float width = 0.0f;
			for (const char* cursor = measureCursor; cursor < end;)
			{
				const UInt8 current = static_cast<UInt8>(*cursor);
				if (current == 1)
				{
					if (font->ButtonIcons.pBuffer
						&& measureIconIndex < static_cast<int>(font->ButtonIcons.uiSize))
					{
						const Font::ButtonIcon& icon = font->ButtonIcons.pBuffer[measureIconIndex];
						width += icon.fWidth + icon.fSpacing;
					}
					++measureIconIndex;
					++cursor;
					continue;
				}
				if (current == '\t')
				{
					const float remainder = std::fmod(width, static_cast<float>(kTabWidth));
					width += static_cast<float>(kTabWidth) - remainder;
					++cursor;
					continue;
				}
				if (current < 0x20 || current == kDelChar)
				{
					++cursor;
					continue;
				}
				const char* runEnd = cursor;
				while (runEnd < end)
				{
					const UInt8 value = static_cast<UInt8>(*runEnd);
					if (value < 0x20 || value == kDelChar)
						break;
					UInt32 dbcsCode = 0;
					runEnd += TryDecodeDoubleByte(runEnd, dbcsCode) ? 2 : 1;
				}
				if (runEnd > cursor)
				{
					PreparedTextMeasuredRun measured;
					measured.byteOffset = static_cast<size_t>(cursor - preparedText);
					measured.byteLength = static_cast<size_t>(runEnd - cursor);
					measured.available = LayoutFreeTypeRun(font, cursor,
						measured.byteLength, measured.layout, true);
					if (measured.available)
						width += measured.layout.advance;
					measuredRuns.push_back(std::move(measured));
				}
				cursor = runEnd > cursor ? runEnd : cursor + 1;
			}
			exactLineWidths.push_back(width);
			if (!lineEnd)
				break;
			measureCursor = lineEnd + 1;
		}
		if (exactLineWidths.empty())
			exactLineWidths.push_back(0.0f);
		int maxExactWidth = 0;
		BSSimpleList<int>* updateWidthCursor = &textData.xLineWidths;
		for (float width : exactLineWidths)
		{
			const int gameWidth = static_cast<int>(std::ceil(std::max(0.0f, width)));
			maxExactWidth = MaxInt(maxExactWidth, gameWidth);
			if (updateWidthCursor)
			{
				updateWidthCursor->m_item = gameWidth;
				updateWidthCursor = updateWidthCursor->m_pkNext;
			}
		}
		textData.iWidth = maxExactWidth;
		if (outputWidth)
			*outputWidth = maxExactWidth;

		BSSimpleList<int>* lineWidthCursor = &textData.xLineWidths;
		int lineWidth = lineWidthCursor ? lineWidthCursor->m_item : 0;
		float exactLineWidth = exactLineWidths.front();
		float lineOrigin = aiFlags == 4 ? -exactLineWidth
			: aiFlags == 2 ? exactLineWidth * -0.5f : 0.0f;
		LogFreeTypeOrdinaryAlignmentOnce(
			font, aiFlags, lineWidth, lineOrigin, textData.xNewText.pString);

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
		size_t measuredRunCursor = 0;
		for (int byteIndex = 0; textData.xNewText.pString[byteIndex]; ++byteIndex)
		{
			const UInt8 current = static_cast<UInt8>(textData.xNewText.pString[byteIndex]);
			if (current == static_cast<UInt8>(aiLineBreakChar))
			{
				LogFreeTypeLineDrift(font, aiFlags, lineIndex, lineWidth,
					position.x - lineStartX, trailingWhitespaceCount,
					trailingWhitespaceWidth, textData.xNewText.pString);
				++lineIndex;
				if (lineWidthCursor && lineWidthCursor->m_pkNext)
					lineWidthCursor = lineWidthCursor->m_pkNext;
				lineWidth = lineWidthCursor ? lineWidthCursor->m_item : 0;
				exactLineWidth = lineIndex < static_cast<int>(exactLineWidths.size())
					? exactLineWidths[lineIndex] : static_cast<float>(lineWidth);
				lineOrigin = aiFlags == 4 ? -exactLineWidth
					: aiFlags == 2 ? exactLineWidth * -0.5f : 0.0f;
				position.x = lineOrigin;
				lineStartX = lineOrigin;
				trailingWhitespaceCount = 0;
				trailingWhitespaceWidth = 0.0f;
				position.z -= font->pFontData->fBaseLine + linePadding;
				continue;
			}
			if (current == '\t')
			{
				const float beforeTab = position.x;
				const float remainder = fmodf(position.x, static_cast<float>(kTabWidth));
				position.x += static_cast<float>(kTabWidth) - remainder;
				++trailingWhitespaceCount;
				trailingWhitespaceWidth += position.x - beforeTab;
				continue;
			}
			if (current == 1)
			{
				if (icons && font->ButtonIcons.pBuffer)
					font->AddIcon(iconIndex++, icons, &position);
				else
					++iconIndex;
				trailingWhitespaceCount = 0;
				trailingWhitespaceWidth = 0.0f;
				continue;
			}
			if (current < 0x20 || current == kDelChar)
				continue;

			int runEnd = byteIndex;
			while (textData.xNewText.pString[runEnd])
			{
				const UInt8 value = static_cast<UInt8>(textData.xNewText.pString[runEnd]);
				if (value == static_cast<UInt8>(aiLineBreakChar)
					|| value < 0x20 || value == kDelChar)
					break;
				UInt32 dbcsCode = 0;
				runEnd += TryDecodeDoubleByte(&textData.xNewText.pString[runEnd], dbcsCode) ? 2 : 1;
			}
			FreeTypeLayoutRun fallbackRun;
			const size_t runLength = runEnd > byteIndex
				? static_cast<size_t>(runEnd - byteIndex) : 0;
			const FreeTypeLayoutRun* run = runLength
				? layoutScratch.FindMeasuredRun(static_cast<size_t>(byteIndex),
					runLength, measuredRunCursor) : nullptr;
			if (!run && runLength && LayoutFreeTypeRun(font,
				&textData.xNewText.pString[byteIndex], runLength, fallbackRun, true))
			{
				run = &fallbackRun;
			}
			if (run)
			{
				float runPen = position.x;
				for (const FreeTypeLayoutGlyph& item : *run->glyphs)
				{
					NiPoint3 glyphPen(runPen + item.xOffset, position.y,
						position.z + item.yOffset);
					VectorEncodedGlyph glyph = item.glyph;
					if (!glyph.metrics)
					{
						glyph.metrics = glyph.byteClass == VectorFontByteClass::DoubleByte
							? EnsureFreeTypeDoubleByteMetrics(font, glyph.encodedCode)
							: &font->pFontData->pFontLetters[glyph.encodedCode & 0xFF];
					}
					builder.AddGlyph(glyph, glyphPen, fontColor);
					runPen += item.xAdvance;
				}
				position.x += run->advance;
			}
			trailingWhitespaceCount = 0;
			trailingWhitespaceWidth = 0.0f;
			byteIndex = runEnd > byteIndex ? runEnd - 1 : byteIndex;
		}
		LogFreeTypeLineDrift(font, aiFlags, lineIndex, lineWidth,
			position.x - lineStartX, trailingWhitespaceCount,
			trailingWhitespaceWidth, textData.xNewText.pString);

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

		auto* extraGlyphs = GetExtraGlyphs(this->iFontNum);
		Font::TextData textData;

		if (!*aiHeight)
			*aiHeight = kSentinelMax;
		if (!aiLineEnd)
			aiLineEnd = kSentinelMax;

		float linePadding = FontManager::GetLinePadding(this->iFontNum);
		ThisStdCall(0x759330, &textData, *aiWidth, *aiHeight, aiLineStart, aiLineEnd, aiLineBreakChar);

		const char* pStr = axTextString->pString;
		std::string sConvertedStr;
		if (ConvertToMultiByte(pStr, sConvertedStr, extraGlyphs != nullptr))
			axTextString->Set(pStr);
		std::string sTranslatedStr;
		if (TranslateText(axTextString->pString, sTranslatedStr))
			axTextString->Set(sTranslatedStr.c_str());

		ThisStdCall(0xA12FB0, this, axTextString->pString, &textData);

		*aiWidth = textData.iWidth;
		*aiHeight = textData.iHeight;

		if (IsFreeTypeFontActive(this))
		{
			return CreateFreeTypePreparedText(this, textData, aiWidth,
				aiFlags, aiLineBreakChar, axFontColor, apTextShape, apIconShape,
				rasterScale);
		}

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

		// Cursor for O(1) linked list traversal per line break (was O(n) from head each time)
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
					// Advance cursor by 1 instead of traversing from head (O(n^2) to O(n))
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
				// 0xEC9130
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
			{
				ConvertToAsciiQuotes(&currentChar);
			}

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

		// Recompute bounding volumes for text/icon geometry (NiBound::ComputeFromData)
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
		auto* extraGlyphs = GetExtraGlyphs(this->iFontNum);

		const char* pStr = apTextString->pString;
		std::string sConvertedStr;
		if (ConvertToMultiByte(pStr, sConvertedStr, extraGlyphs != nullptr))
			apTextString->Set(pStr);
		std::string sTranslatedStr;
		if (TranslateText(apTextString->pString, sTranslatedStr))
			apTextString->Set(sTranslatedStr.c_str());

		if (!apTextString->pString || !this->pFontData)
			return 0;

		UInt32 textLen = (apTextString->sLen == 0xFFFF)
			? strlen(apTextString->pString) : apTextString->sLen;
		if (!textLen)
			return 0;

		char newlineBuffer[4];
		float textXOffset = (float)*aiWidth;
		ThisStdCall(0xA12370, this, apTextString->pString, &textXOffset,
			newlineBuffer, abPrepareObject, 0);

		float currentX = afStartX + textXOffset;
		float currentY = afStartY;
		float currentZ = afZ;

		if (abUpperLeftCorner)
		{
			double lineBaseOffset = this->pFontData->pFontLetters[32].fHeight - this->pFontData->fBaseLine;
			currentY = currentY - (lineBaseOffset + lineBaseOffset);
		}

		// For sLen==0xFFFF, textLen==strlen so charIdx==textLen; otherwise scan for embedded nulls
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

		if (IsFreeTypeFontActive(this))
		{
			const float rasterScale = ResolveFreeTypeRasterScale();
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
			const float startX = currentX;
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
					const double remainder = fmod(currentX, static_cast<double>(kTabWidth));
					currentX = static_cast<float>(currentX + kTabWidth - remainder);
					continue;
				}
				if (current == '\n')
				{
					char escapeBuffer[4];
					float nextLineX = static_cast<float>(*aiWidth);
					ThisStdCall(0xA12370, this, apTextString->pString, &nextLineX,
						escapeBuffer, abPrepareObject, byteIndex + 1);
					currentX = nextLineX;
					currentY -= this->pFontData->fBaseLine;
					continue;
				}
				if (current < 0x20 || current == kDelChar)
					continue;

				int runEnd = byteIndex;
				while (apTextString->pString[runEnd])
				{
					const UInt8 value = static_cast<UInt8>(apTextString->pString[runEnd]);
					if (value < 0x20 || value == kDelChar)
						break;
					UInt32 dbcsCode = 0;
					runEnd += TryDecodeDoubleByte(&apTextString->pString[runEnd], dbcsCode) ? 2 : 1;
				}
				FreeTypeLayoutRun run;
				if (runEnd > byteIndex && LayoutFreeTypeRun(this,
					&apTextString->pString[byteIndex], runEnd - byteIndex, run, true))
				{
					float runPen = currentX;
					for (const FreeTypeLayoutGlyph& item : *run.glyphs)
					{
						const NiPoint3 pen(runPen + item.xOffset, currentZ,
							currentY + item.yOffset);
						VectorEncodedGlyph glyph = item.glyph;
						if (!glyph.metrics)
						{
							glyph.metrics = glyph.byteClass == VectorFontByteClass::DoubleByte
								? EnsureFreeTypeDoubleByteMetrics(this, glyph.encodedCode)
								: &pFontData->pFontLetters[glyph.encodedCode & 0xFF];
						}
						builder.AddGlyph(glyph, pen, activeColor ? activeColor : arg1C);
						runPen += item.xAdvance;
					}
					currentX += run.advance;
				}
				byteIndex = runEnd > byteIndex ? runEnd - 1 : byteIndex;
				*aiWidth = MaxInt(*aiWidth,
					static_cast<int>(std::ceil(std::max(0.0f, currentX - startX))));
			}

			NiTriShape* textObject = builder.Finish();
			if (!textObject)
				textObject = CreateEmptyFreeTypeTextShape(this, abPrepareObject_1);
			if (textObject)
				textObject->m_kLocal.m_Translate = NiPoint3(afStartX, currentZ, startY);
			return textObject;
		}

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
				// 0xEC9130
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
			{
				ConvertToAsciiQuotes(&currentChar);
			}

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

		// Recompute bounding volume for text geometry (NiBound::ComputeFromData)
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

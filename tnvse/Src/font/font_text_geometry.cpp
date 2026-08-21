#include "font_engine.h"
#include "dictionary.h"
#include "font_glyphs.h"
#include "game_hooks.h"
#include "font_manager.h"
#include "font_vector.h"
#include "font_vector_internal.h"
#include "native_calls.h"
#include <atomic>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <unordered_set>
#include <vector>

namespace fonthook
{
	static constexpr UInt32 kInitialRenderAddCharLogCount = 0;
	static constexpr size_t kLongTextTraceMinimumBytes = 4096;
	static std::atomic<UInt32> s_longTextTraceSequence = 0;

	struct LongTextGeometryTiming
	{
		SInt64 scanTicks = 0;
		SInt64 finishTicks = 0;
	};

	static SInt64 ReadLongTextQpc()
	{
		return vectorfont::BeginFreeTypePerfSample();
	}

	static SInt64 StopLongTextQpc(SInt64 start)
	{
		const SInt64 end = ReadLongTextQpc();
		return start > 0 && end >= start ? end - start : 0;
	}

	static double LongTextTicksToMicroseconds(SInt64 ticks)
	{
		static const SInt64 frequency = []()
		{
			LARGE_INTEGER value = {};
			return QueryPerformanceFrequency(&value)
				? value.QuadPart : 0;
		}();
		return ticks > 0 && frequency > 0
			? static_cast<double>(ticks) * 1000000.0
				/ static_cast<double>(frequency)
			: 0.0;
	}

	static const char* LongTextOffsetDomainName(
		vectorfont::PreparedTextSidecarOffsetDomain domain)
	{
		switch (domain)
		{
		case vectorfont::PreparedTextSidecarOffsetDomain::LayoutSource:
			return "layout-source";
		case vectorfont::PreparedTextSidecarOffsetDomain::PreparedText:
			return "prepared-text";
		default:
			return "none";
		}
	}

	static const char* LongTextByteClassName(UInt8 byteLength, UInt8 byteClass)
	{
		if (!byteLength)
			return "none";
		return byteClass == static_cast<UInt8>(
			VectorFontByteClass::DoubleByte) ? "doubleByte" : "singleByte";
	}

	static void LogLongTextPerformance(
		const vectorfont::FreeTypeLongTextTrace& trace,
		SInt64 preprocessTicks, SInt64 layoutTicks,
		const LongTextGeometryTiming& geometryTiming,
		SInt64 geometryTicks, SInt64 totalTicks)
	{
		const SInt64 measuredWorkTicks = preprocessTicks + layoutTicks
			+ geometryTicks;
		gLog.FormattedMessage(
			"tnvse_freetype_long_text_perf: id=%u font=%d sourceBytes=%u preparedBytes=%u chars=%u lines=%u route=%s routeInitial=%s routeReason=%s sidecar=%u sidecarReason=%s sidecarRequested=%u directProfile=%u sidecarPublished=%u sidecarCaptured=%u sidecarRejected=%u sidecarConsumed=%u sidecarUnits=%u failureOffset=%u failureOffsetDomain=%s failureEncoded=0x%04X failureBytes=%u failureRole=%s builderFailureEncoded=0x%04X builderFailureBytes=%u builderFailureRole=%s directGlyphs=%u genericGlyphs=%u atlasOutcome=%s atlasOutcomeCode=%u shapeCreated=%u preprocessUs=%.3f layoutUs=%.3f geometryScanUs=%.3f finishUs=%.3f geometryUs=%.3f workUs=%.3f wallUs=%.3f",
			trace.traceId,
			trace.fontId,
			trace.sourceByteCount,
			trace.preparedByteCount,
			trace.preparedCharCount,
			trace.preparedLineCount,
			vectorfont::VectorTextBuildRouteName(trace.builderFinalRoute),
			vectorfont::VectorTextBuildRouteName(trace.builderInitialRoute),
			vectorfont::VectorTextBuildReasonName(trace.builderReason),
			trace.sidecarCaptured && !trace.sidecarRejected ? 1u : 0u,
			vectorfont::PreparedTextSidecarReasonName(trace.sidecarReason),
			trace.sidecarRequested ? 1u : 0u,
			trace.sidecarDirectProfileAcquired ? 1u : 0u,
			trace.sidecarPublished ? 1u : 0u,
			trace.sidecarCaptured ? 1u : 0u,
			trace.sidecarRejected ? 1u : 0u,
			trace.sidecarConsumed ? 1u : 0u,
			trace.sidecarUnitCount,
			trace.sidecarFailureByteOffset,
			LongTextOffsetDomainName(trace.sidecarFailureOffsetDomain),
			trace.sidecarFailureEncodedCode,
			static_cast<UInt32>(trace.sidecarFailureByteLength),
			LongTextByteClassName(trace.sidecarFailureByteLength,
				trace.sidecarFailureByteClass),
			trace.builderFailureEncodedCode,
			static_cast<UInt32>(trace.builderFailureByteLength),
			LongTextByteClassName(trace.builderFailureByteLength,
				trace.builderFailureByteClass),
			trace.builderDirectGlyphCount,
			trace.builderGenericGlyphCount,
			vectorfont::VectorTextBuildOutcomeName(
				trace.builderAtlasOutcome),
			static_cast<UInt32>(trace.builderAtlasOutcome),
			trace.builderShapeCreated ? 1u : 0u,
			LongTextTicksToMicroseconds(preprocessTicks),
			LongTextTicksToMicroseconds(layoutTicks),
			LongTextTicksToMicroseconds(geometryTiming.scanTicks),
			LongTextTicksToMicroseconds(geometryTiming.finishTicks),
			LongTextTicksToMicroseconds(geometryTicks),
			LongTextTicksToMicroseconds(measuredWorkTicks),
			LongTextTicksToMicroseconds(totalTicks));
	}

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

	static void CreateFreeTypePreparedText(
		FontEx* font,
		Font::TextData& textData,
		std::shared_ptr<const PreparedDirectTextSidecar> preparedSidecar,
		int* outputWidth,
		int aiFlags,
		char aiLineBreakChar,
		const NiColorA* fontColor,
		NiTriShape** textShape,
		NiTriShape** iconShape,
		float rasterScale,
		LongTextGeometryTiming* longTextTiming)
	{
		const SInt64 geometryScanStart = longTextTiming
			? ReadLongTextQpc() : 0;
		*textShape = nullptr;
		*iconShape = nullptr;
		VectorTextBuilder builder(font, true, rasterScale, fontColor);
		if (!builder.IsAvailable())
		{
			if (longTextTiming)
				longTextTiming->scanTicks = StopLongTextQpc(geometryScanStart);
			*textShape = CreateEmptyFreeTypeTextShape(font, true);
			font->ButtonIcons.Clear(1);
			ThisStdCall<void>(0x7593E0, reinterpret_cast<char*>(&textData));
			return;
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
				ThisStdCall<void>(0xA67050, icons->GetModelData(), 0x4000);
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

		if (preparedSidecar && preparedSidecar->rejectBatch)
		{
			if (vectorfont::FreeTypeLongTextTrace* trace =
				vectorfont::GetActiveFreeTypeLongTextTrace())
			{
				trace->sidecarRejected = true;
				trace->builderReason = vectorfont::
					VectorTextBuildReason::PreparedSidecarRejected;
			}
			vectorfont::RecordFreeTypePerf(
				vectorfont::FreeTypePerfCounter::
					PreparedSidecarRejectedFallback);
			preparedSidecar.reset();
		}
		const std::shared_ptr<const PreparedDirectTextSidecar>
			directSidecar = builder.UsesSealedDirectProfile()
				? preparedSidecar
				: std::shared_ptr<const PreparedDirectTextSidecar>();
		if (vectorfont::FreeTypeLongTextTrace* trace =
			vectorfont::GetActiveFreeTypeLongTextTrace())
		{
			trace->sidecarConsumed = directSidecar != nullptr;
			if (preparedSidecar && !directSidecar
				&& trace->builderReason
					!= vectorfont::VectorTextBuildReason::PreparedSidecarRejected)
			{
				trace->builderReason = vectorfont::VectorTextBuildReason::
					PreparedSidecarIgnoredNoProfile;
			}
		}
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

		if (longTextTiming)
			longTextTiming->scanTicks = StopLongTextQpc(geometryScanStart);
		const SInt64 finishStart = longTextTiming ? ReadLongTextQpc() : 0;
		NiTriShape* textObject = builder.Finish();
		if (longTextTiming)
			longTextTiming->finishTicks = StopLongTextQpc(finishStart);
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
		ThisStdCall<void>(0x7593E0, reinterpret_cast<char*>(&textData));
	}

	// ==================== FontEx::CreateText ====================
	void __thiscall FontEx::CreateText(
		BSStringT<char>* axTextString, int* aiWidth, int* aiHeight,
		int aiLineStart, int aiLineEnd, int aiFlags, char aiLineBreakChar,
		const NiColorA* axFontColor, NiTriShape** apTextShape, NiTriShape** apIconShape)
	{
		size_t sourceBytes = 0;
		if (g_bEnableFreeTypeFontRenderingLog && axTextString
			&& axTextString->pString)
		{
			sourceBytes = std::strlen(axTextString->pString);
		}
		const bool traceLongText =
			sourceBytes >= kLongTextTraceMinimumBytes;
		const UInt32 traceId = traceLongText
			? s_longTextTraceSequence.fetch_add(
				1, std::memory_order_relaxed) + 1
			: 0;
		std::optional<vectorfont::FreeTypeLongTextTrace> longTextTraceStorage;
		vectorfont::FreeTypeLongTextTrace* longTextTrace = nullptr;
		if (traceLongText)
		{
			longTextTrace = &longTextTraceStorage.emplace();
			longTextTrace->traceId = traceId;
			longTextTrace->fontId = this ? this->iFontNum : -1;
			longTextTrace->sourceByteCount = static_cast<UInt32>(
				std::min<size_t>(sourceBytes,
					std::numeric_limits<UInt32>::max()));
		}
		vectorfont::ScopedFreeTypeLongTextTrace longTextTraceScope(
			longTextTrace);
		SInt64 traceStartQpc = 0;
		SInt64 preprocessTicks = 0;
		SInt64 layoutTicks = 0;
		LongTextGeometryTiming geometryTiming;
		const DWORD traceStartTick = traceLongText ? GetTickCount() : 0;
		if (traceLongText)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_long_text: id=%u stage=enter font=%d bytes=%u width=%d height=%d lineStart=%d lineEnd=%d flags=%d lineSep=0x%02X uiEncoding=%u codePage=%u dictionary=%u multibyteInput=%u",
				traceId, this ? this->iFontNum : -1,
				static_cast<UInt32>(sourceBytes),
				aiWidth ? *aiWidth : 0, aiHeight ? *aiHeight : 0,
				aiLineStart, aiLineEnd, aiFlags,
				static_cast<UInt8>(aiLineBreakChar),
				g_uiEncoding, g_usingWinEncoding,
				g_bEnableDictionaryTranslation ? 1u : 0u,
				g_bMultibyteInput ? 1u : 0u);
			traceStartQpc = ReadLongTextQpc();
		}
		const float rasterScale = GetCanonicalFreeTypeRasterScale();
		const bool freeTypeActive = IsFreeTypeFontActive(this);
		if (!g_bEnableMultibyteFontHook && !freeTypeActive)
		{
			const SInt64 vanillaStart = traceLongText ? ReadLongTextQpc() : 0;
			CallOriginalFontCreateText(this, axTextString, aiWidth,
				aiHeight, aiLineStart, aiLineEnd, aiFlags, aiLineBreakChar,
				axFontColor, apTextShape, apIconShape);
			if (traceLongText)
			{
				longTextTrace->builderInitialRoute =
					vectorfont::VectorTextBuildRoute::Vanilla;
				longTextTrace->builderFinalRoute =
					vectorfont::VectorTextBuildRoute::Vanilla;
				const SInt64 vanillaTicks = StopLongTextQpc(vanillaStart);
				LogLongTextPerformance(*longTextTrace, 0, 0, geometryTiming,
					vanillaTicks, StopLongTextQpc(traceStartQpc));
			}
			return;
		}

		auto* extraGlyphs = GetExtraGlyphs(this->iFontNum);
		Font::TextData textData;

		if (!*aiHeight)
			*aiHeight = kSentinelMax;
		if (!aiLineEnd)
			aiLineEnd = kSentinelMax;

		float linePadding = FontManager::GetLinePadding(this->iFontNum);
		ThisStdCall<void>(
			0x759330, &textData, *aiWidth, *aiHeight,
			aiLineStart, aiLineEnd, aiLineBreakChar);

		if (g_bEnableMultibyteFontHook)
		{
			if (traceLongText)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_long_text: id=%u stage=preprocess-begin elapsedMs=%u",
					traceId, GetTickCount() - traceStartTick);
			}
			const SInt64 preprocessStart = traceLongText
				? ReadLongTextQpc() : 0;
			const char* pStr = axTextString->pString;
			std::string sConvertedStr;
			if (ConvertToMultiByte(pStr, sConvertedStr, extraGlyphs != nullptr))
				axTextString->Set(pStr);
			std::string sTranslatedStr;
			if (TranslateText(axTextString->pString, sTranslatedStr))
				axTextString->Set(sTranslatedStr.c_str());
			if (traceLongText)
				preprocessTicks = StopLongTextQpc(preprocessStart);
			if (traceLongText)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_long_text: id=%u stage=preprocess-end bytes=%u elapsedMs=%u",
					traceId,
					static_cast<UInt32>(std::strlen(
						axTextString->pString ? axTextString->pString : "")),
					GetTickCount() - traceStartTick);
			}
		}

		SInt64 layoutStart = 0;
		std::shared_ptr<const PreparedDirectTextSidecar> preparedSidecar;
		{
			if (traceLongText)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_long_text: id=%u stage=layout-begin elapsedMs=%u",
					traceId, GetTickCount() - traceStartTick);
			}
			layoutStart = traceLongText ? ReadLongTextQpc() : 0;
			PreparedTextSidecarCapture capture(&textData, this);
			if (g_bEnableMultibyteFontHook)
			{
				ThisStdCall<void>(0xA12FB0, this,
					axTextString->pString, &textData);
			}
			else
			{
				PrepText(axTextString->pString, &textData);
			}
			preparedSidecar = capture.Take();
		}
		if (traceLongText)
			layoutTicks = StopLongTextQpc(layoutStart);
		if (traceLongText)
		{
			longTextTrace->preparedByteCount = static_cast<UInt32>(
				std::min<size_t>(std::strlen(textData.xNewText.pString
					? textData.xNewText.pString : ""),
					std::numeric_limits<UInt32>::max()));
			longTextTrace->preparedCharCount = static_cast<UInt32>(
				std::max(0, textData.iCharCount));
			longTextTrace->preparedLineCount = static_cast<UInt32>(
				std::max(0, textData.iLineEnd));
			gLog.FormattedMessage(
				"tnvse_freetype_long_text: id=%u stage=layout-end preparedBytes=%u chars=%d lines=%d width=%d height=%d sidecar=%u elapsedMs=%u",
				traceId, longTextTrace->preparedByteCount,
				textData.iCharCount, textData.iLineEnd,
				textData.iWidth, textData.iHeight,
				preparedSidecar ? 1u : 0u,
				GetTickCount() - traceStartTick);
		}

		*aiWidth = textData.iWidth;
		*aiHeight = textData.iHeight;

		if (freeTypeActive && IsFreeTypeVuiProxyMeasureOnlyActive())
		{
			NiTriShape* emptyProxy = CreateEmptyFreeTypeTextShape(this, true);
			if (emptyProxy)
			{
				*apTextShape = emptyProxy;
				*apIconShape = nullptr;
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
				ThisStdCall<void>(0x7593E0,
					reinterpret_cast<char*>(&textData));
				if (traceLongText)
				{
					const SInt64 totalTicks =
						StopLongTextQpc(traceStartQpc);
					gLog.FormattedMessage(
						"tnvse_freetype_long_text: id=%u stage=measure-only-end elapsedMs=%u",
						traceId, GetTickCount() - traceStartTick);
					longTextTrace->builderInitialRoute =
						vectorfont::VectorTextBuildRoute::MeasureOnly;
					longTextTrace->builderFinalRoute =
						vectorfont::VectorTextBuildRoute::MeasureOnly;
					LogLongTextPerformance(*longTextTrace, preprocessTicks,
						layoutTicks, geometryTiming, 0,
						totalTicks);
				}
				return;
			}
		}

		if (freeTypeActive)
		{
			if (traceLongText)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_long_text: id=%u stage=geometry-begin chars=%d elapsedMs=%u",
					traceId, textData.iCharCount,
					GetTickCount() - traceStartTick);
			}
			const SInt64 geometryStart = traceLongText
				? ReadLongTextQpc() : 0;
			CreateFreeTypePreparedText(this, textData,
				std::move(preparedSidecar), aiWidth,
				aiFlags, aiLineBreakChar, axFontColor, apTextShape, apIconShape,
				rasterScale, traceLongText ? &geometryTiming : nullptr);
			const SInt64 geometryTicks = traceLongText
				? StopLongTextQpc(geometryStart) : 0;
			const SInt64 totalTicks = traceLongText
				? StopLongTextQpc(traceStartQpc) : 0;
			if (traceLongText)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_long_text: id=%u stage=geometry-end textShape=0x%08X iconShape=0x%08X elapsedMs=%u",
					traceId,
					reinterpret_cast<UInt32>(
						apTextShape ? *apTextShape : nullptr),
					reinterpret_cast<UInt32>(
						apIconShape ? *apIconShape : nullptr),
					GetTickCount() - traceStartTick);
				LogLongTextPerformance(*longTextTrace, preprocessTicks,
					layoutTicks, geometryTiming, geometryTicks,
					totalTicks);
			}
			return;
		}
		const SInt64 vanillaGeometryStart = traceLongText
			? ReadLongTextQpc() : 0;
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
			pIconShape->m_kLocal.m_Translate = NiPoint3(
				0.0f, textPosition.y, textPosition.z);
			ThisStdCall<void>(
				0xA67050, pIconShape->GetModelData(), 0x4000);
		}

		float yOffsetStart = textPosition.x;
		int lineCounter = 0;
		int vertexIdx = 0;
		int iconIdx = 0;

		BSSimpleList<int>* pLineWidthCursor = &textData.xLineWidths;

		UInt32 uiDoubleByteCode = 0;
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
				if (extraGlyphs && bIsDBCharacter)
				{
					if (FontLetter* glyph = LookupDBGlyph(
						extraGlyphs, uiDoubleByteCode))
					{
						Font::AddChar(glyph, vertexIdx++,
							pTextShape, &textPosition, axFontColor);
						++charIdx;
						rendered = true;
					}
				}
				if (!rendered)
				{
					Font::AddChar(&this->pFontData->pFontLetters[currentChar],
						vertexIdx++, pTextShape, &textPosition, axFontColor);
				}
			}
			int maxRenderedWidth = *aiWidth;
			int renderedWidth = ConditionalFloatToUInt(textPosition.x - yOffsetStart);
			*aiWidth = MaxInt(renderedWidth, maxRenderedWidth);
		}

		auto* pTextGeomData = pTextShape->GetModelData();
		ThisStdCall<void>(0xA7EE30, &pTextGeomData->m_kBound,
			pTextGeomData->m_usVertices, pTextGeomData->m_pkVertex);
		if (pIconShape)
		{
			auto* pIconGeomData = pIconShape->GetModelData();
			ThisStdCall<void>(0xA7EE30, &pIconGeomData->m_kBound,
				pIconGeomData->m_usVertices, pIconGeomData->m_pkVertex);
		}
		this->ButtonIcons.Clear(1);
		ThisStdCall<void>(0x7593E0, (char*)&textData);
		if (traceLongText)
		{
			longTextTrace->builderInitialRoute =
				vectorfont::VectorTextBuildRoute::Vanilla;
			longTextTrace->builderFinalRoute =
				vectorfont::VectorTextBuildRoute::Vanilla;
			LogLongTextPerformance(*longTextTrace, preprocessTicks,
				layoutTicks, geometryTiming,
				StopLongTextQpc(vanillaGeometryStart),
				StopLongTextQpc(traceStartQpc));
		}
	}

	// ==================== FontEx::MakeString ====================
	NiAVObject* FontEx::MakeString(
		float afStartX, float afStartY, float afZ,
		BSStringT<char>* apTextString, int* aiWidth, UInt32 aiFlags,
		const NiColorA* arg1C, bool abUpperLeftCorner, bool abPrepareObject_1)
	{
		const bool freeTypeActive = IsFreeTypeFontActive(this);
		if (!g_bEnableMultibyteFontHook && !freeTypeActive)
		{
			return CallOriginalFontMakeString(this, afStartX, afStartY, afZ,
				apTextString, aiWidth, aiFlags, arg1C,
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

		float textYOffset = 0.0f;
		float textXOffset = (float)*aiWidth;
		if (freeTypeActive)
		{
			textXOffset = GetFreeTypeLineOffset(
				this,
				apTextString->pString,
				textXOffset,
				aiFlags,
				0);
		}
		else
		{
			ThisStdCall<void>(0xA12370, this, apTextString->pString, &textXOffset,
				&textYOffset, aiFlags, 0);
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
							aiFlags, byteIndex + 1);
					}
					else
					{
						float nextLineYOffset = 0.0f;
						ThisStdCall<void>(0xA12370, this, apTextString->pString,
							&nextLineX, &nextLineYOffset, aiFlags,
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
		int iActualCharCount = AdjustCharCountForDB(
			apTextString->pString, charIdx, extraGlyphs, textLen);

		auto* pTriShape = Font::MakeTriShape(iActualCharCount, arg1C, abPrepareObject_1);
		// Retail Font::MakeString lays the AddChar position tuple out as
		// [horizontal X, depth Z, vertical Y]. AddChar reads element 2 for
		// glyph top/bottom, so model that ABI explicitly instead of relying on
		// three unrelated locals being adjacent in the compiler's stack frame.
		NiPoint3 position(currentX, currentZ, currentY);
		float startY = position.z;
		pTriShape->m_kLocal.m_Translate = NiPoint3(afStartX, position.y, startY);

		NiColorA* pColor = 0;
		float defaultColor[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
		*aiWidth = 0;
		double lineBaseOffset = position.x;
		int vertexIdx = 0;
		UInt32 uiDoubleByteCode = 0;

		for (int lineIdx = 0; apTextString->pString[lineIdx]; ++lineIdx)
		{
			if (apTextString->pString[lineIdx] == 3)
				pColor = 0;

			char currentCharValue = apTextString->pString[lineIdx];
			if (currentCharValue == '\t')
			{
				double tabRemainder = fmod(position.x, 75.0);
				position.x += static_cast<float>(75.0 - tabRemainder);
			}
			else if (currentCharValue == '\n')
			{
				float nextLineYOffset = 0.0f;
				float tabPrevX = (float)*aiWidth;
				ThisStdCall<void>(0xA12370, this, apTextString->pString, &tabPrevX,
					&nextLineYOffset, aiFlags, lineIdx + 1);
				position.x = tabPrevX;
				position.z -= this->pFontData->fBaseLine;
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
					Font::AddChar(glyph, vertexIdx++,
						pTriShape, &position, pColor);
					lineIdx += 1;
					rendered = true;
				}
			}
			if (!rendered)
			{
				Font::AddChar(&this->pFontData->pFontLetters[currentChar],
					vertexIdx++, pTriShape, &position, pColor);
			}

			int prevWidth = *aiWidth;
			int renderedWidth = ConditionalFloatToUInt(position.x - lineBaseOffset);
			*aiWidth = MaxInt(renderedWidth, prevWidth);

			if (apTextString->pString[lineIdx] == 2)
				pColor = (NiColorA*)defaultColor;
		}

		auto* pGeomData = pTriShape->GetModelData();
		ThisStdCall<void>(0xA7EE30, &pGeomData->m_kBound,
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

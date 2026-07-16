#include "font_freetype_internal.h"

#include "encoding.h"
#include "globals.h"

namespace fonthook
{
	void FlushFreeTypePersistentFontCache()
	{
		vectorfont::FlushGlyphBitmapDiskCache();
	}

	bool LayoutFreeTypeRun(Font* apFont, const char* apText, size_t auiLength,
		FreeTypeLayoutRun& arLayout, bool abAllowShaping)
	{
		arLayout = {};
		if (!apFont || !apText || !auiLength)
			return false;
		vectorfont::RuntimeFont* runtime = vectorfont::FindActiveRuntime(apFont);
		return runtime && vectorfont::LayoutRuntimeRun(
			*runtime, apText, auiLength, abAllowShaping, arLayout);
	}

	bool GetFreeTypePairKerning(Font* apFont,
		const char* apLeft, size_t auiLeftLength,
		const char* apRight, size_t auiRightLength, float& arKerning)
	{
		arKerning = 0.0f;
		if (!apFont || !apLeft || !apRight || !auiLeftLength || !auiRightLength
			|| auiLeftLength > 2 || auiRightLength > 2)
		{
			return false;
		}
		vectorfont::RuntimeFont* runtime = vectorfont::FindActiveRuntime(apFont);
		if (!runtime)
			return false;
		std::lock_guard<std::recursive_mutex> lock(vectorfont::State().mutex);
		auto packCode = [](const char* bytes, size_t length)
		{
			UInt16 code = static_cast<UInt8>(bytes[0]);
			if (length == 2)
				code = static_cast<UInt16>((code << 8) | static_cast<UInt8>(bytes[1]));
			return code;
		};
		const vectorfont::KerningCacheKey cacheKey = {
			runtime->config->layoutHash,
			g_usingWinEncoding, packCode(apLeft, auiLeftLength),
			packCode(apRight, auiRightLength),
			static_cast<UInt8>(auiLeftLength), static_cast<UInt8>(auiRightLength)
		};
		auto cached = vectorfont::State().kerningCache.find(cacheKey);
		if (cached != vectorfont::State().kerningCache.end())
		{
			arKerning = cached->second;
			vectorfont::RecordFreeTypePerf(vectorfont::FreeTypePerfCounter::KerningHit);
			return true;
		}
		auto storeResult = [&](float value)
		{
			constexpr size_t kKerningCacheLimit = 16384;
			while (vectorfont::State().kerningCache.size() >= kKerningCacheLimit
				&& !vectorfont::State().kerningCacheOrder.empty())
			{
				vectorfont::State().kerningCache.erase(
					vectorfont::State().kerningCacheOrder.front());
				vectorfont::State().kerningCacheOrder.pop_front();
			}
			vectorfont::State().kerningCache.emplace(cacheKey, value);
			vectorfont::State().kerningCacheOrder.push_back(cacheKey);
		};
		VectorEncodedGlyph left;
		VectorEncodedGlyph right;
		if (!vectorfont::DecodeEncodedGlyphIdentity(*runtime, apLeft, left)
			|| !vectorfont::DecodeEncodedGlyphIdentity(*runtime, apRight, right)
			|| left.byteLength != auiLeftLength || right.byteLength != auiRightLength
			|| left.byteClass != right.byteClass || left.faceIndex != right.faceIndex
			|| !left.glyphIndex || !right.glyphIndex)
		{
			return false;
		}
		vectorfont::RuntimeRole& role = runtime->roles[static_cast<size_t>(left.byteClass)];
		if (role.style->fixedWidth > 0.0f)
		{
			vectorfont::RecordFreeTypePerf(vectorfont::FreeTypePerfCounter::KerningMiss);
			storeResult(0.0f);
			return true;
		}
		if (left.faceIndex >= role.faces.size())
			return false;
		vectorfont::RuntimeFace& face = role.faces[left.faceIndex];
		vectorfont::RecordFreeTypePerf(vectorfont::FreeTypePerfCounter::KerningMiss);
		if (!vectorfont::ConfigureRuntimeFace(face, *role.style, 1.0f, false)
			|| !FT_HAS_KERNING(face.face))
		{
			storeResult(0.0f);
			return true;
		}
		FT_Vector delta = {};
		if (!FT_Get_Kerning(face.face, left.glyphIndex, right.glyphIndex,
			FT_KERNING_DEFAULT, &delta))
		{
			arKerning = static_cast<float>(delta.x) / 64.0f;
		}
		storeResult(arKerning);
		return true;
	}

	bool IsHarfBuzzShapingEnabled(const Font* apFont)
	{
		const vectorfont::RuntimeFont* runtime = vectorfont::FindActiveRuntime(apFont);
		return runtime && runtime->config && runtime->config->shaping;
	}

	bool IsFreeTypeFontConfigured(UInt32 auiFontId)
	{
		return vectorfont::FindConfig(auiFontId) != nullptr;
	}

	bool ActivateFreeTypeFont(Font* apFont, bool abForce)
	{
		if (!apFont || !apFont->pFontData || !g_bEnableFreeTypeFontRendering)
			return false;
		if (!vectorfont::FindConfig(apFont->iFontNum))
		{
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				std::lock_guard<std::recursive_mutex> lock(vectorfont::State().mutex);
				if (vectorfont::State().loggedUnconfiguredFontIds.insert(apFont->iFontNum).second)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: font id=%d file=%s is not configured; keeping original renderer",
						apFont->iFontNum, apFont->pFontFile ? apFont->pFontFile : "");
				}
			}
			return false;
		}
		{
			std::lock_guard<std::recursive_mutex> lock(vectorfont::State().mutex);
			const auto active = vectorfont::State().activeFonts.find(apFont);
			if (!abForce && active != vectorfont::State().activeFonts.end()
				&& active->second.data == apFont->pFontData
				&& active->second.fontId == static_cast<UInt32>(apFont->iFontNum))
				return true;
		}
		if (!InitializeFreeTypeVectorRenderer())
		{
			gLog.FormattedMessage("tnvse_freetype_font: vector renderer unavailable for font id=%d",
				apFont->iFontNum);
			return false;
		}
		vectorfont::RuntimeFont* runtime = vectorfont::EnsureRuntimeFont(apFont->iFontNum);
		if (!runtime)
		{
			gLog.FormattedMessage("tnvse_freetype_font: runtime initialization failed for font id=%d",
				apFont->iFontNum);
			return false;
		}
		if (!vectorfont::ApplyRuntimeMetrics(*runtime, *apFont))
		{
			gLog.FormattedMessage("tnvse_freetype_font: metric replacement failed for font id=%d",
				apFont->iFontNum);
			return false;
		}
		gLog.FormattedMessage("tnvse_freetype_font: activated font id=%d file=%s",
			apFont->iFontNum, apFont->pFontFile ? apFont->pFontFile : "");
		vectorfont::QueueFontPrewarm(apFont->iFontNum);
		return true;
	}

	bool IsFreeTypeFontActive(const Font* apFont)
	{
		return vectorfont::FindActiveRuntime(apFont) != nullptr;
	}

	FontLetter* EnsureFreeTypeDoubleByteMetrics(Font* apFont, UInt32 encodedCode)
	{
		vectorfont::RuntimeFont* runtime = vectorfont::FindActiveRuntime(apFont);
		return runtime ? vectorfont::EnsureDoubleByteMetrics(*runtime, *apFont, encodedCode) : nullptr;
	}

	bool DecodeFreeTypeGlyph(Font* apFont, const char* text, VectorEncodedGlyph& glyph)
	{
		vectorfont::RuntimeFont* runtime = vectorfont::FindActiveRuntime(apFont);
		return runtime && vectorfont::DecodeEncodedGlyph(*runtime, *apFont, text, glyph);
	}
}

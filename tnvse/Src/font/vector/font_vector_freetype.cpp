#include "font_freetype_internal.h"

#include "encoding.h"
#include "globals.h"
#include "load_config.h"

#include <linebreak.h>

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

	bool GetFreeTypeLayoutIdentity(const Font* apFont, UInt64& arIdentity)
	{
		arIdentity = 0;
		const vectorfont::RuntimeFont* runtime = vectorfont::FindActiveRuntime(apFont);
		if (!runtime || !runtime->config)
			return false;
		arIdentity = runtime->config->layoutHash;
		return true;
	}

	bool BuildFreeTypeUnicodeLineBreakMap(const Font* apFont, const char* apText,
		size_t auiLength, std::vector<UInt8>& arBreakAfter)
	{
		arBreakAfter.clear();
		if (!apText || !auiLength)
			return false;

		std::vector<utf32_t> codePoints;
		std::vector<size_t> codePointEndBytes;
		UInt32 codePage = kWindows1252CodePage;
		codePoints.reserve(auiLength);
		codePointEndBytes.reserve(auiLength);
		{
			vectorfont::FreeTypeState& state = vectorfont::State();
			std::lock_guard<std::recursive_mutex> lock(state.mutex);
			const vectorfont::RuntimeFont* runtime = vectorfont::FindActiveRuntime(apFont);
			if (!runtime || !runtime->config || !runtime->config->unicodeLineBreaking)
				return false;
			codePage = GetFreeTypeTextCodePage();
			arBreakAfter.assign(auiLength, 0);

			for (size_t offset = 0; offset < auiLength && apText[offset];)
			{
				int byteLength = 1;
				UInt32 encodedCode = 0;
				if (offset + 1 < auiLength
					&& TryDecodeDoubleByteForCodePage(
						apText + offset, codePage, encodedCode))
				{
					byteLength = 2;
				}

				UInt32 codePoint = 0xFFFD;
				if (static_cast<UInt8>(apText[offset]) == 1)
					codePoint = 0xFFFC;
				else
					vectorfont::DecodeCodePoint(apText + offset, byteLength, codePoint);
				codePoints.push_back(static_cast<utf32_t>(codePoint));
				offset += byteLength;
				codePointEndBytes.push_back(offset - 1);
			}
		}

		if (codePoints.empty())
			return true;
		std::vector<char> breaks(codePoints.size(), LINEBREAK_NOBREAK);
		const char* language = "en";
		switch (codePage)
		{
		case 936: language = "zh"; break;
		case 950: language = "zh"; break;
		case 932: language = "ja"; break;
		case 949: language = "ko"; break;
		default: break;
		}
		set_linebreaks_utf32(codePoints.data(), codePoints.size(), language, breaks.data());
		for (size_t index = 0; index < breaks.size(); ++index)
		{
			// Gamebryo's configured line separator remains the sole mandatory
			// control.  libunibreak contributes discretionary UAX #14 points.
			if (breaks[index] == LINEBREAK_ALLOWBREAK)
			{
				arBreakAfter[codePointEndBytes[index]] = 1;
			}
		}
		return true;
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

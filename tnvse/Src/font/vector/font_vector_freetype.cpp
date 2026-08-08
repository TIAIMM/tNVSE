#include "font_freetype_internal.h"

#include "globals.h"
#include "load_config.h"

namespace fonthook
{
	namespace
	{
		thread_local UInt32 s_legacyFntRenderRouteDepth = 0;
	}

	ScopedLegacyFntRenderRoute::ScopedLegacyFntRenderRoute() noexcept
	{
		++s_legacyFntRenderRouteDepth;
	}

	ScopedLegacyFntRenderRoute::~ScopedLegacyFntRenderRoute() noexcept
	{
		if (s_legacyFntRenderRouteDepth)
			--s_legacyFntRenderRouteDepth;
	}

	bool IsLegacyFntRenderRouteActive() noexcept
	{
		return s_legacyFntRenderRouteDepth != 0;
	}

	void FlushFreeTypePersistentFontCache()
	{
		vectorfont::FlushGlyphBitmapDiskCache();
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
						"tnvse_freetype_font: font id=%d file=%s is not configured; keeping vanilla renderer",
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
		if (IsLegacyFntRenderRouteActive())
			return false;
		return vectorfont::FindActiveRuntime(apFont) != nullptr;
	}

	bool HasEnabledFreeTypeFontEffects(const Font* apFont)
	{
		const vectorfont::RuntimeFont* runtime = vectorfont::FindActiveRuntime(apFont);
		if (!runtime || !runtime->config)
			return false;
		const vectorfont::FontConfig& config = *runtime->config;
		return config.shadow.enabled || config.glow.enabled || config.outline.enabled;
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
} // namespace fonthook

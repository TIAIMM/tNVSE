#include "font_engine.h"
#include "font_glyphs.h"
#include "game_hooks.h"
#include "font_vector.h"
#include "native_calls.h"
#include <cstdio>
#include <new>

namespace fonthook::implementation::font_engine {}
using namespace fonthook::implementation::font_engine;

namespace fonthook::implementation::font_engine
{
	thread_local UInt32 s_fontConstructorLoadDepth = 0;

	class FontConstructorLoadScope
	{
	public:
		FontConstructorLoadScope() { ++s_fontConstructorLoadDepth; }
		~FontConstructorLoadScope() { --s_fontConstructorLoadDepth; }
	};

	bool PublishExtraGlyphs(Font& font, const std::string& fontKey)
	{
		if (!font.pFontData || !font.iRefCount || fontKey.empty())
			return false;

		// A numeric slot denotes the just-completed load. Never leave metrics from
		// a previous file occupying that slot when the replacement has no extension.
		gNumberedExtraLetters.erase(font.iFontNum);
		auto extra = gExtraFontLetters.find(fontKey);
		if (extra == gExtraFontLetters.end() || extra->second.empty())
		{
			if (extra != gExtraFontLetters.end())
				gExtraFontLetters.erase(extra);
			return false;
		}

		gNumberedExtraLetters[font.iFontNum] = std::move(extra->second);
		gExtraFontLetters.erase(extra);
		return !gNumberedExtraLetters[font.iFontNum].empty();
	}
}

namespace fonthook
{
	// ==================== FontEx::FontConstructor ====================
	Font* FontEx::FontConstructor(int iFontNum, char* apFilename, bool abLoad)
	{
		Font* result = nullptr;
		{
			FontConstructorLoadScope loadScope;
			result = CallOriginalFontConstructor(
				this, iFontNum, apFilename, abLoad);
		}
		if (result && g_bEnableFreeTypeFontRendering)
			ActivateFreeTypeFont(result, true);
		return result;
	}

	// ==================== FontEx::Load ====================
	void FontEx::Load()
	{
		const bool firstLoad = !this->iRefCount && this->pFontFile;
		const bool loadExtraGlyphs = g_bEnableMultibyteFontHook && firstLoad;
		const std::string fontKey = loadExtraGlyphs ? this->pFontFile : "";
		if (loadExtraGlyphs)
		{
			gExtraFontLetters.erase(fontKey);
			gNumberedExtraLetters.erase(static_cast<UInt32>(this->iFontNum));
		}

		// Preserve the retail texture loader. The dedicated Font::Load call-site
		// hook bounds retail's initial .FNT read to the 0x3928-byte FontData block;
		// the appended tNVSE glyph payload can therefore be read safely here after
		// the original loader has completed. Do not validate each
		// TextureFile::pFilename as an isolated 32-byte C string: stock fonts 4-7
		// contain legal 32-38 byte names that continue into unused texture records,
		// exactly as retail's BSsprintf("%s") path expects.
		CallOriginalFontLoad(this);

		bool publishedExtraGlyphs = false;
		if (loadExtraGlyphs && this->pFontData && this->iRefCount)
		{
			BSFile* fontFile = FileFinder_GetFile(
				this->pFontFile, NiFile::READ_ONLY, 0x4000u, 2u);
			if (fontFile && fontFile->m_pFile)
			{
				fontFile->Seek(static_cast<SInt32>(kFontDataSize), SEEK_SET);
				UInt32 componentSizes[2] = { 1u, 0u };
				LoadExtraGlyphs(fontFile, componentSizes);
			}
			if (fontFile)
				delete fontFile;
			publishedExtraGlyphs = PublishExtraGlyphs(*this, fontKey);
		}
		else if (loadExtraGlyphs)
		{
			gExtraFontLetters.erase(fontKey);
		}

		if (g_bEnableFreeTypeFontRendering && this->pFontData
			&& !s_fontConstructorLoadDepth)
		{
			ActivateFreeTypeFont(this, publishedExtraGlyphs);
		}
	}

	// ---- FontEx::Load helpers ----

	void FontEx::LoadExtraGlyphs(BSFile* fontFile, UInt32* textureMarkers)
	{
		if (!UsesDbcsTextLayout()) return;

		UInt32 uiActualSize = fontFile->GetSize();
		if (uiActualSize <= kFontDataSize) return;

		const std::string fontKey = this->pFontFile ? this->pFontFile : "";
		if (fontKey.empty()) return;

		auto& extraStore = gExtraFontLetters[fontKey];
		if (!extraStore.empty()) return;

		// Batch-read all FontLetters for each high-byte row (191 entries at once)
		// instead of 24,066 individual m_pfnRead calls: 126 calls total.
		static constexpr UInt32 kRowGlyphCount =
			SerializedExtraGlyphTable::kGlyphsPerRow;
		static constexpr UInt32 kRowBytes = kRowGlyphCount * sizeof(FontLetter);
		extraStore.serialized.glyphs.reset(new (std::nothrow)
			FontLetter[SerializedExtraGlyphTable::kGlyphCount]);
		if (!extraStore.serialized.glyphs)
			return;
		extraStore.serialized.validGlyphs = 0;

		for (UInt32 highByte = SerializedExtraGlyphTable::kFirstLeadByte;
			highByte <= SerializedExtraGlyphTable::kLastLeadByte; ++highByte)
		{
			FontLetter* row = extraStore.serialized.glyphs.get()
				+ extraStore.serialized.validGlyphs;
			UInt32 bytesRead = fontFile->m_pfnRead(
				fontFile, row, kRowBytes, textureMarkers, 1u);
			fontFile->m_uiAbsoluteCurrentPos += bytesRead;
			if (bytesRead != kRowBytes)
			{
				// The old map published only complete rows. Avoid retaining the
				// allocation when even the first serialized row was incomplete.
				if (!extraStore.serialized.validGlyphs)
					extraStore.serialized.clear();
				return;
			}
			extraStore.serialized.validGlyphs += kRowGlyphCount;
		}
	}

} // namespace fonthook

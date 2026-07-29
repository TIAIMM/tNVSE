#include "font_engine.h"
#include "dictionary.h"
#include "font_glyphs.h"
#include "game_hooks.h"
#include "font_manager.h"
#include "font_vector.h"
#include "native_calls.h"
#include <array>
#include <cmath>
#include <vector>

namespace fonthook::implementation::font_engine {}
using namespace fonthook::implementation::font_engine;

namespace fonthook::implementation::font_engine
{
	thread_local UInt32 s_fontInitLoadDepth = 0;

	class FontInitLoadScope
	{
	public:
		FontInitLoadScope() { ++s_fontInitLoadDepth; }
		~FontInitLoadScope() { --s_fontInitLoadDepth; }
	};
}

namespace fonthook
{
	// ==================== FontEx::FontInit ====================
	Font* FontEx::FontInit(int iFontNum, char* apFilename, bool abLoad)
	{
		if (!g_bEnableMultibyteFontHook)
		{
			FontInitLoadScope loadScope;
			Font* result = CallOriginalFontInit(
				this, iFontNum, apFilename, abLoad);
			if (result && g_bEnableFreeTypeFontRendering)
				ActivateFreeTypeFont(result);
			return result;
		}

		TlsSlotGuard tlsGuard;
		fontNameKey.clear();
		bool movedExtraGlyphs = false;

		StdCall(0xEC782F, this->pTextureData, 4, 8, 0xA1B410, 0x45CEC0);
		this->IconAtlasTextureName.pString = 0;
		this->IconAtlasTextureName.sLen = 0;
		this->IconAtlasTextureName.sMaxLen = 0;
		ThisStdCall(0xA1BEF0, &this->ButtonIcons);

		this->pFontFile = 0;
		this->iRefCount = 0;
		this->pFontData = 0;

		if (apFilename)
		{
			UInt32 filenameLen = strlen(apFilename);
			if (filenameLen)
			{
				this->pFontFile = (char*)MemoryManager_s_Instance->Allocate(filenameLen + 1);
				strcpy_s(this->pFontFile, filenameLen + 1, apFilename);
			}
			this->iFontNum = iFontNum;
			if (abLoad)
			{
				FontInitLoadScope loadScope;
				ThisStdCall(0xA15320, this);
			}
		}

		if (!fontNameKey.empty())
		{
			auto it = gExtraFontLetters.find(fontNameKey);
			if (it != gExtraFontLetters.end() && !it->second.empty())
			{
				gNumberedExtraLetters[iFontNum] = std::move(it->second);
				gExtraFontLetters.erase(it);
				movedExtraGlyphs = true;
				if (!gNumberedExtraLetters[iFontNum].empty())
				{
					fontNameKey.clear();
				}
			}
			else
			{
				fontNameKey.clear();
			}
		}

		ActivateFreeTypeFont(this, movedExtraGlyphs);

		return this;
	}

	// ==================== FontEx::Load ====================
	void FontEx::Load()
	{
		if (!g_bEnableMultibyteFontHook)
		{
			CallOriginalFontLoad(this);
			if (g_bEnableFreeTypeFontRendering && this->pFontData
				&& !s_fontInitLoadDepth)
			{
				ActivateFreeTypeFont(this);
			}
			return;
		}

		TlsSlotGuard tlsGuard;

		UInt16 refCount = this->iRefCount;
		if (refCount || !this->pFontFile)
		{
			++this->iRefCount;
			if (this->pFontData && !s_fontInitLoadDepth)
				ActivateFreeTypeFont(this);
			return;
		}

		// ---- Open and validate font file ----
		BSFile* fontFile = FileFinder_GetFile(this->pFontFile, (NiFile::OpenMode)0, 0x4000u, 2u);
		if (!fontFile || !fontFile->m_pFile)
		{
			if (fontFile) delete fontFile;
			return;
		}

		// ---- Read font data header ----
		UInt32 textureMarkers[2] = {};
		textureMarkers[0] = 1;
		this->pFontData = (FontData*)MemoryManager_s_Instance->Allocate(kFontDataSize);
		fontFile->m_uiAbsoluteCurrentPos +=
			fontFile->m_pfnRead(fontFile, this->pFontData, kFontDataSize, textureMarkers, 1u);

		// ---- Load extended glyphs then close the file ----
		LoadExtraGlyphs(fontFile, textureMarkers);
		if (fontFile) delete fontFile;

		// ---- Calculate metrics & copy special character properties ----
		ComputeGlyphMetrics();

		if (this->pFontData->iTextureCount > 8)
			return;

		// ---- Load font textures ----
		int stringRefFlag = 0;
		if (!LoadFontTextures(textureMarkers, stringRefFlag))
			return;

		++this->iRefCount;
		// FontInit binds path-keyed extended metrics to the numeric font slot and
		// activates once after Load returns. Standalone Load calls still activate here.
		if (!s_fontInitLoadDepth)
			ActivateFreeTypeFont(this);
	}

	// ---- FontEx::Load helpers ----

	void FontEx::LoadExtraGlyphs(BSFile* fontFile, UInt32* textureMarkers)
	{
		if (!UsesDbcsTextLayout()) return;

		UInt32 uiActualSize = fontFile->GetSize();
		if (uiActualSize <= kFontDataSize) return;

		fontNameKey = this->pFontFile ? this->pFontFile : "";
		if (fontNameKey.empty()) return;

		auto& extraStore = gExtraFontLetters[fontNameKey];
		if (!extraStore.empty()) return;

		// Batch-read all FontLetters for each high-byte row (191 entries at once)
		// instead of 24,066 individual m_pfnRead calls: 126 calls total.
		static constexpr UInt32 kRowGlyphCount =
			SerializedExtraGlyphTable::kGlyphsPerRow;
		static constexpr UInt32 kRowBytes = kRowGlyphCount * sizeof(FontLetter);
		extraStore.serialized.glyphs.reset(
			new FontLetter[SerializedExtraGlyphTable::kGlyphCount]);
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

	float FontEx::ComputeGlyphMetrics()
	{
		this->fFontHeight = 0.0f;
		float maxHeight = 0.0f;
		this->fMaxDrop = 0.0f;

		auto* pLetters = this->pFontData->pFontLetters;

		for (int i = 0; i < kMaxGlyphCount; ++i)
		{
			auto& letter = pLetters[i];
			float glyphHeight = GetGlyphLineHeight(this->pFontData, &letter);
			this->fFontHeight = MaxFloat(glyphHeight, this->fFontHeight);
			maxHeight = MaxFloat(maxHeight, letter.fHeight);
			this->fMaxDrop = MinFloat(letter.fTopEdge - letter.fHeight, this->fMaxDrop);
		}

		// Space character (swap width & spacing)
		float savedWidth = pLetters[kSpaceChar].fWidth;
		pLetters[kSpaceChar].fWidth = pLetters[kSpaceChar].fSpacing;
		pLetters[kSpaceChar].fSpacing = savedWidth;
		pLetters[kSpaceChar].fHeight = maxHeight;
		pLetters[kSpaceChar].fTopEdge = this->fMaxDrop + maxHeight;

		// NBSP copies space
		pLetters[kNBSPChar].fWidth = pLetters[kSpaceChar].fWidth;
		pLetters[kNBSPChar].fSpacing = pLetters[kSpaceChar].fSpacing;
		pLetters[kNBSPChar].fHeight = pLetters[kSpaceChar].fHeight;
		pLetters[kNBSPChar].fTopEdge = pLetters[kSpaceChar].fTopEdge;

		// Delete char copies pipe
		pLetters[kDelChar].fWidth = pLetters[kPipeChar].fWidth;
		pLetters[kDelChar].fLeadingEdge = pLetters[kPipeChar].fLeadingEdge;
		pLetters[kDelChar].fSpacing = pLetters[kPipeChar].fSpacing;
		pLetters[kDelChar].fHeight = pLetters[kPipeChar].fHeight;
		pLetters[kDelChar].fTopEdge = pLetters[kPipeChar].fTopEdge;

		// Null glyph
		auto& nullGlyph = pLetters[0];
		nullGlyph.fWidth = 0.0f;
		nullGlyph.fSpacing = 0.0f;
		nullGlyph.fHeight = maxHeight;
		nullGlyph.fTopEdge = this->fMaxDrop + maxHeight;
		memset(nullGlyph.pMapping, 0, sizeof(UVMap) * 4);

		return maxHeight;
	}

	bool FontEx::LoadFontTextures(UInt32* textureMarkers, int& stringRefFlag)
	{
		for (int textureCount = 0; textureCount < this->pFontData->iTextureCount; ++textureCount)
		{
			char texNameBuffer[MAX_PATH];
			_snprintf_s(texNameBuffer, 0x100u, _TRUNCATE,
				"TEXTURES\\FONTS\\%s.TEX",
				this->pFontData->pTextureFiles[textureCount].pFilename);

			BSFile* textureReadStream = FileFinder_GetFile(
				(const char*)texNameBuffer, (NiFile::OpenMode)0, 0x4000u, 2u);
			if (!textureReadStream || !textureReadStream->m_pFile)
			{
				if (textureReadStream) delete textureReadStream;
				return false;
			}

			UInt32 texWidth, texHeight;
			textureMarkers[0] = 1;
			UInt32 readFlagValue = textureReadStream->m_pfnRead(textureReadStream, &texWidth, 4u, textureMarkers, 1u);
			readFlagValue += textureReadStream->m_pfnRead(textureReadStream, &texHeight, 4u, textureMarkers, 1u);
			textureReadStream->m_uiAbsoluteCurrentPos += readFlagValue;

			NiTexture::FormatPrefs formatPrefs;
			formatPrefs.m_ePixelLayout = static_cast<NiTexture::FormatPrefs::PixelLayout>(0x6);
			formatPrefs.m_eAlphaFmt = static_cast<NiTexture::FormatPrefs::AlphaFormat>(0x3);
			formatPrefs.m_eMipMapped = static_cast<NiTexture::FormatPrefs::MipFlag>(0x2);

			NiPixelData* finalPixelData;
			NiPixelData* createdPixelData = (NiPixelData*)NiMemObject::operator new(sizeof(NiPixelData));
			if (createdPixelData)
			{
				finalPixelData = ThisStdCall<NiPixelData*>(
					0xA7C190, createdPixelData, texWidth, texHeight,
					reinterpret_cast<const NiPixelFormat*>(0x11AA2A0), 1, 1);
			}
			else
			{
				finalPixelData = 0;
			}

			void* readBuffer = &finalPixelData->m_pucPixels[*finalPixelData->m_puiOffsetInBytes];
			int readFlag = 1;
			UInt32 bytesRead = textureReadStream->m_pfnRead(
				textureReadStream, readBuffer, 4 * texHeight * texWidth, (UInt32*)&readFlag, 1u);
			textureReadStream->m_uiAbsoluteCurrentPos += bytesRead;
			finalPixelData->bNoConvert = 1;

			NiTexturingProperty* textureProperty;
			NiFixedString textureName;
			NiTexturingProperty* createdProperty = (NiTexturingProperty*)NiMemObject::operator new(sizeof(NiTexturingProperty));
			if (createdProperty)
			{
				char* pFontFilePath = this->pFontFile;
				textureName.m_kHandle = pFontFilePath
					? (char*)NiGlobalStringTable::AddString(pFontFilePath) : 0;
				stringRefFlag |= 1u;
				textureProperty = ThisStdCall<NiTexturingProperty*>(
					0xA6ABB0, createdProperty, finalPixelData, &textureName, &formatPrefs);
			}
			else
			{
				textureProperty = 0;
			}

			if ((stringRefFlag & 1) != 0)
			{
				stringRefFlag &= ~1u;
				if (textureName.m_kHandle)
					InterlockedDecrement((volatile LONG*)textureName.m_kHandle - 2);
			}
			if (textureReadStream) delete textureReadStream;

			ThisStdCall(0x60AEB0, textureProperty, 1);
			ThisStdCall(0x66B0D0, &this->pTextureData[textureCount].m_pObject, (int)textureProperty);
		}
		return true;
	}

} // namespace fonthook

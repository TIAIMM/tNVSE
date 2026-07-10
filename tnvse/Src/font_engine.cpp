#include "font_engine.h"
#include "dictionary.h"
#include "font_glyphs.h"
#include "font_manager.h"
#include "font_vector.h"
#include "native_calls.h"
#include <cmath>
#include <vector>

namespace fonthook
{
	// ---- Helper: look up last character glyph (for wrap handling) ----
	static FontLetter* LookupLastCharGlyph(
		ExtraGlyphMap* extraGlyphs,
		const char* buffer, UInt32 processedLen, FontData* fontData,
		UInt32& outDBCode, bool& outIsDB)
	{
		outIsDB = false;
		if (extraGlyphs && processedLen >= 2)
		{
			UInt8 lastByte = (UInt8)buffer[processedLen - 1];
			if (IsTrailByte(lastByte))
			{
				if (TryDecodeDoubleByte(&buffer[processedLen - 2], outDBCode))
				{
					FontLetter* glyph = LookupDBGlyph(extraGlyphs, outDBCode);
					if (glyph)
					{
						outIsDB = true;
						return glyph;
					}
				}
			}
		}
		return &fontData->pFontLetters[(UInt8)buffer[processedLen - 1]];
	}

	// ---- Helper: adjust wrap position to avoid splitting a double-byte character ----
	static bool TryGetDoubleByteAt(const char* buffer, UInt32 byteIndex, UInt32 bufferLen)
	{
		if (byteIndex + 1 >= bufferLen) return false;

		UInt32 dbCode = 0;
		return TryDecodeDoubleByte(&buffer[byteIndex], dbCode);
	}

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

	static UInt32 AdjustWrapPositionForDB(UInt32 insertPos, const char* buffer, UInt32 bufferLen)
	{
		if (insertPos > 0 && TryGetDoubleByteAt(buffer, insertPos - 1, bufferLen))
		{
			return insertPos - 1;
		}
		return insertPos;
	}

	// ==================== FontEx::FontInit ====================
	Font* FontEx::FontInit(int iFontNum, char* apFilename, bool abLoad)
	{
		TlsSlotGuard tlsGuard;
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
				ThisStdCall(0xA15320, this);
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
		TlsSlotGuard tlsGuard;

		UInt16 refCount = this->iRefCount;
		if (refCount || !this->pFontFile)
		{
			++this->iRefCount;
			if (this->pFontData)
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
		ActivateFreeTypeFont(this);
	}

	// ---- FontEx::Load helpers ----

	void FontEx::LoadExtraGlyphs(BSFile* fontFile, UInt32* textureMarkers)
	{
		if (!g_uiEncoding) return;

		UInt32 uiActualSize = fontFile->GetSize();
		if (uiActualSize <= kFontDataSize) return;

		fontNameKey = this->pFontFile ? this->pFontFile : "";
		if (fontNameKey.empty()) return;

		auto& extraMap = gExtraFontLetters[fontNameKey];
		if (!extraMap.empty()) return;

		extraMap.reserve(kExtraGlyphReserve);

		// Batch-read all FontLetters for each high-byte row (195 entries at once)
		// instead of 24,960 individual m_pfnRead calls: 128 calls total
		static constexpr UInt32 kRowGlyphCount = 0xFE - 0x40 + 1; // 195
		static constexpr UInt32 kRowBytes = kRowGlyphCount * sizeof(FontLetter);
		FontLetter batchRow[kRowGlyphCount];

		for (UInt32 highByte = 0x81; highByte <= 0xFE; ++highByte)
		{
			UInt32 bytesRead = fontFile->m_pfnRead(
				fontFile, batchRow, kRowBytes, textureMarkers, 1u);
			fontFile->m_uiAbsoluteCurrentPos += bytesRead;
			if (bytesRead != kRowBytes) return;

			UInt32 keyBase = highByte << 8;
			for (UInt32 i = 0; i < kRowGlyphCount; ++i)
				extraMap[keyBase | (0x40 + i)] = batchRow[i];
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

	// ==================== Shared PrepText implementation ====================
	// PrepTextForTerminal and PrepText differ only in how iCharCount is set at the end:
	//   PrepTextForTerminal: iCharCount = origConsumed
	//   PrepText:            iCharCount = processedTextLen

	// Pass 1: process escape sequences (&variable;) in-place
	static bool ProcessEscapeSequences(
		char*& processedOriginalText, char*& dynamicTextBuffer,
		UInt32& textBufferSize, UInt32& processedTextLen,
		UInt32& origConsumed, UInt32& sourceTextLen,
		FontEx* font, Font::TextData* axData)
	{
		char parsedTextBuffer[1028] = {};
		bool hasEscapeSequence = false;

		for (UInt32 srcTextIndex = 0; srcTextIndex < sourceTextLen; ++srcTextIndex)
		{
			if (processedOriginalText[srcTextIndex] != '&')
			{
				dynamicTextBuffer[processedTextLen++] = processedOriginalText[srcTextIndex];
				continue;
			}

			int varNameLen = 0;
			int escapeSeqPrefixLen = 1;
			bool isPositiveEscape = true;
			if (processedOriginalText[srcTextIndex + 1] == '-')
			{
				isPositiveEscape = false;
				escapeSeqPrefixLen = 2;
			}
			char varNameBuffer[128];
			int rawLen = 0;
			while (processedOriginalText[escapeSeqPrefixLen + rawLen + srcTextIndex]
				&& rawLen < 127
				&& processedOriginalText[escapeSeqPrefixLen + rawLen + srcTextIndex] != ';'
				&& processedOriginalText[escapeSeqPrefixLen + rawLen + srcTextIndex] != '\n'
				&& processedOriginalText[escapeSeqPrefixLen + rawLen + srcTextIndex] != axData->cLineSep)
			{
				varNameBuffer[rawLen] = processedOriginalText[escapeSeqPrefixLen + rawLen + srcTextIndex];
				++rawLen;
			}
			int effectiveLen = rawLen;
			varNameBuffer[effectiveLen] = 0;
			UInt32 totalEscapeSeqLen = rawLen + escapeSeqPrefixLen; // prefix + variable name
			if (processedOriginalText[escapeSeqPrefixLen + rawLen + srcTextIndex] == ';')
				totalEscapeSeqLen += 1;

			if (Interface_FindTextReplacementString(varNameBuffer, parsedTextBuffer, 0x400u, isPositiveEscape)
				|| Interface_TestConstantForGameSettings(varNameBuffer, parsedTextBuffer))
			{
				UInt32 postEscapeTextLen = strlen(parsedTextBuffer);
				int escapeSeqSizeDiff = postEscapeTextLen - totalEscapeSeqLen;

				if (postEscapeTextLen > 0 && parsedTextBuffer[postEscapeTextLen - 1] == '\\')
				{
					int charScanIndex = 0;
					while (parsedTextBuffer[charScanIndex] != '\\') ++charScanIndex;

					char iconPath[264] = {};
					char textureNameBuffer[268];
					strcpy_s(iconPath, sizeof(iconPath), &parsedTextBuffer[charScanIndex + 1]);
					if (font->iFontNum == 7)
					{
						strcpy_s(textureNameBuffer, MAX_PATH, iconPath);
						sprintf_s(iconPath, MAX_PATH, "glow_%s", textureNameBuffer);
					}
					font->AddTextIcon(iconPath);
					parsedTextBuffer[charScanIndex] = 1;
					parsedTextBuffer[charScanIndex + 1] = 0;
					postEscapeTextLen = charScanIndex + 2;
					escapeSeqSizeDiff = postEscapeTextLen - totalEscapeSeqLen;
				}
				if (escapeSeqSizeDiff > 0)
				{
					textBufferSize += escapeSeqSizeDiff;
					dynamicTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Reallocate(dynamicTextBuffer, textBufferSize + 1));
				}
				memcpy(&dynamicTextBuffer[processedTextLen], parsedTextBuffer, postEscapeTextLen);
				processedTextLen += postEscapeTextLen;
				origConsumed += totalEscapeSeqLen;
				srcTextIndex = srcTextIndex + totalEscapeSeqLen - 1;
			}
			else
			{
				dynamicTextBuffer[processedTextLen++] = processedOriginalText[srcTextIndex];
			}
			hasEscapeSequence = true;
		}

		dynamicTextBuffer[processedTextLen] = 0;
		if (hasEscapeSequence)
		{
			sourceTextLen = processedTextLen;
			processedOriginalText = static_cast<char*>(MemoryManager_s_Instance->Reallocate(processedOriginalText, processedTextLen + 4));
			strcpy_s(processedOriginalText, processedTextLen + 4, dynamicTextBuffer);
		}
		*dynamicTextBuffer = 0;
		processedTextLen = 0;
		return hasEscapeSequence;
	}

	static void PrepTextImpl(FontEx* font, const char* apOrigString, Font::TextData* axData, bool isTerminal)
	{
		if (!apOrigString)
			return;

		if (axData->iWidth <= 0)
			axData->iWidth = kSentinelMax;
		if (axData->iHeight <= 0)
			axData->iHeight = kSentinelMax;
		if (axData->iLineEnd <= 0)
			axData->iLineEnd = kSentinelMax;

		auto* extraGlyphs = GetExtraGlyphs(font->iFontNum);
		UInt32 origConsumed = 0;

		// Cache frequently accessed font data to avoid repeated pointer chasing
		auto* pFontLetters = font->pFontData->pFontLetters;
		float fBaseLine = font->pFontData->fBaseLine;
		float lineSpacingAdjust = FontManager::GetLinePadding(font->iFontNum);
		float lineHeight = fBaseLine + lineSpacingAdjust;
		UInt32 lastWrapPosition = 0;
		int preSpaceWidth = 0;
		int postSpaceWidth = 0;
		int currentLineWidth = 0;
		int maxLineWidth = 0;
		float totalTextHeight = pFontLetters[kSpaceChar].fHeight;
		int currentLineCount = 1;
		UInt32 sourceTextLen = strlen(apOrigString);
		int maxAllowedLines = axData->iLineEnd;

		char* originalTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Allocate(sourceTextLen + 4));
		if (!originalTextBuffer) return;
		memset(originalTextBuffer, 0, sourceTextLen + 4);
		char* processedOriginalText = originalTextBuffer;

		char* processedTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Allocate(sourceTextLen + 4));
		if (!processedTextBuffer)
		{
			MemoryManager_s_Instance->Deallocate(originalTextBuffer);
			return;
		}
		memset(processedTextBuffer, 0, sourceTextLen + 4);
		char* dynamicTextBuffer = processedTextBuffer;
		snprintf(originalTextBuffer, sourceTextLen + 1, "%s", apOrigString);

		UInt32 processedTextLen = 0;
		UInt32 textBufferSize = sourceTextLen + 4;

		// ---- Pass 1: Process escape sequences (&variable;) ----
		ProcessEscapeSequences(processedOriginalText, dynamicTextBuffer,
			textBufferSize, processedTextLen, origConsumed, sourceTextLen, font, axData);

		bool isTildeChar = false;
		UInt32 buttonIconIndex = 0;

		// ---- Pass 2: Text layout with wrapping ----
		bool bIsDBCharacter, bLastIsDBCharacter;
		UInt32 uiDoubleByteCode, uiTempDoubleByteCode;
		for (UInt32 charIndex = 0; charIndex < sourceTextLen && processedOriginalText[charIndex]; ++charIndex)
		{
			if (processedOriginalText[charIndex] == axData->cLineSep)
			{
				dynamicTextBuffer[processedTextLen] = axData->cLineSep;
				origConsumed += 1;
				if (++processedTextLen >= textBufferSize)
				{
					dynamicTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Reallocate(dynamicTextBuffer, processedTextLen + 4));
					textBufferSize = processedTextLen + 4;
				}
				totalTextHeight = lineHeight + totalTextHeight;
				axData->xLineWidths.AddTail(currentLineWidth);
				maxLineWidth = MaxInt(maxLineWidth, currentLineWidth);
				currentLineWidth = 0;
				lastWrapPosition = 0;
				++currentLineCount;
			}
			else
			{
				bIsDBCharacter = false;
				if (extraGlyphs && (charIndex + 1) <= sourceTextLen)
				{
					bIsDBCharacter = TryDecodeDoubleByte((const char*)&processedOriginalText[charIndex], uiDoubleByteCode);
				}

				if (processedOriginalText[charIndex] == '\t')
				{
					currentLineWidth += 75 - currentLineWidth % 75;
					origConsumed += 1;
					continue;
				}

				UInt8 currentChar;
				FontLetter* pCurrentGlyph;
				UInt32 charWidthWithKerning;

				if (!bIsDBCharacter)
				{
					currentChar = processedOriginalText[charIndex];
					origConsumed += 1;
					ConvertToAsciiQuotes(&currentChar);
					pCurrentGlyph = &pFontLetters[currentChar];
					if (currentChar == 1)
					{
						if (buttonIconIndex < font->ButtonIcons.uiSize)
						{
							pCurrentGlyph->fWidth = font->ButtonIcons.pBuffer[buttonIconIndex].fWidth;
							pCurrentGlyph->fSpacing = font->ButtonIcons.pBuffer[buttonIconIndex].fSpacing;
						}
						++buttonIconIndex;
					}
					charWidthWithKerning = GetGlyphLayoutWidth(pCurrentGlyph);
					currentLineWidth += charWidthWithKerning;
					if (currentChar == '~')
					{
						lastWrapPosition = processedTextLen;
						isTildeChar = true;
						UInt32 tildeCharWidth = GetGlyphLayoutWidth(pCurrentGlyph);
						currentLineWidth -= tildeCharWidth;
						preSpaceWidth = currentLineWidth;
						postSpaceWidth = currentLineWidth;
					}
				}
				else
				{
					origConsumed += 2;
					EnsureFreeTypeDoubleByteMetrics(font, uiDoubleByteCode);
					FontLetter* glyph = LookupDBGlyph(extraGlyphs, uiDoubleByteCode);
					if (glyph)
					{
						pCurrentGlyph = glyph;
						charWidthWithKerning = GetGlyphLayoutWidth(pCurrentGlyph);
						currentLineWidth += charWidthWithKerning;
					}
				}

				if (currentLineWidth > axData->iWidth)
				{
					if (lastWrapPosition)
					{
						if (isTildeChar)
						{
							isTildeChar = false;
							textBufferSize += 4;
							dynamicTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Reallocate(dynamicTextBuffer, textBufferSize + 1));

							UInt32 insertPos = AdjustWrapPositionForDB(lastWrapPosition, dynamicTextBuffer, processedTextLen);
							memmove(&dynamicTextBuffer[insertPos + 1], &dynamicTextBuffer[insertPos],
								(processedTextLen - insertPos) + 1);
							dynamicTextBuffer[insertPos] = axData->cLineSep;
							processedTextLen += 1;

							axData->xLineWidths.AddTail(currentLineWidth);
							maxLineWidth = MaxInt(maxLineWidth, currentLineWidth);
							lastWrapPosition = 0;
							++currentLineCount;

							pCurrentGlyph = LookupLastCharGlyph(extraGlyphs, dynamicTextBuffer, processedTextLen, font->pFontData, uiTempDoubleByteCode, bLastIsDBCharacter);
							currentLineWidth = GetGlyphLayoutWidth(pCurrentGlyph);

							if (bIsDBCharacter)
							{
								FontLetter* glyph = LookupDBGlyph(extraGlyphs, uiDoubleByteCode);
								if (glyph) pCurrentGlyph = glyph;
							}
							else
							{
								pCurrentGlyph = &pFontLetters[currentChar];
							}

							UInt32 nextCharWidth = GetGlyphLayoutWidth(pCurrentGlyph);
							currentLineWidth += nextCharWidth;
						}
						else
						{
							if (lastWrapPosition == processedTextLen)
								currentChar = axData->cLineSep;
							else
								dynamicTextBuffer[lastWrapPosition] = axData->cLineSep;
							totalTextHeight = lineHeight + totalTextHeight;
							axData->xLineWidths.AddTail(preSpaceWidth);
							maxLineWidth = MaxInt(maxLineWidth, preSpaceWidth);
							lastWrapPosition = 0;
							++currentLineCount;
							currentLineWidth -= postSpaceWidth;
						}
					}
					else
					{
						if (processedTextLen + 4 >= textBufferSize)
						{
							dynamicTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Reallocate(dynamicTextBuffer, processedTextLen + 8));
							textBufferSize = processedTextLen + 8;
						}

						UInt32 tailStart = processedTextLen - 1;
						UInt32 tailBytes = processedTextLen - tailStart;
						if (processedTextLen >= 2)
						{
							UInt32 dbStart = processedTextLen - 2;
							if (extraGlyphs && TryGetDoubleByteAt(dynamicTextBuffer, dbStart, processedTextLen))
							{
								tailStart = dbStart;
								tailBytes = processedTextLen - tailStart;
							}
						}

						memmove(&dynamicTextBuffer[tailStart + 1], &dynamicTextBuffer[tailStart], tailBytes);
						dynamicTextBuffer[tailStart] = axData->cLineSep;

						processedTextLen += 1;
						totalTextHeight += lineHeight;

						axData->xLineWidths.AddTail(currentLineWidth);
						maxLineWidth = MaxInt(maxLineWidth, currentLineWidth);
						lastWrapPosition = 0;
						++currentLineCount;

						pCurrentGlyph = LookupLastCharGlyph(extraGlyphs, dynamicTextBuffer, processedTextLen, font->pFontData, uiTempDoubleByteCode, bLastIsDBCharacter);
						currentLineWidth = GetGlyphLayoutWidth(pCurrentGlyph);

						if (bIsDBCharacter)
						{
							FontLetter* glyph = LookupDBGlyph(extraGlyphs, uiDoubleByteCode);
							if (glyph) pCurrentGlyph = glyph;
						}
						else
						{
							pCurrentGlyph = &pFontLetters[currentChar];
						}

						UInt32 combinedCharWidth = GetGlyphLayoutWidth(pCurrentGlyph);
						currentLineWidth += combinedCharWidth;
					}
				}

				if (bIsDBCharacter)
				{
					if (processedTextLen + 4 >= textBufferSize)
					{
						dynamicTextBuffer = (char*)MemoryManager_s_Instance->Reallocate(dynamicTextBuffer, processedTextLen + 8);
						textBufferSize = processedTextLen + 8;
					}
					dynamicTextBuffer[processedTextLen++] = processedOriginalText[charIndex];
					dynamicTextBuffer[processedTextLen++] = processedOriginalText[charIndex + 1];
					dynamicTextBuffer[processedTextLen] = 0;
					++charIndex;
				}
				else
				{
					if (currentChar != '~')
					{
						if (processedTextLen + 1 >= textBufferSize)
						{
							dynamicTextBuffer = (char*)MemoryManager_s_Instance->Reallocate(dynamicTextBuffer, processedTextLen + 8);
							textBufferSize = processedTextLen + 8;
						}
						dynamicTextBuffer[processedTextLen++] = (char)currentChar;
						dynamicTextBuffer[processedTextLen] = 0;
					}
				}

				if (processedTextLen >= textBufferSize)
				{
					dynamicTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Reallocate(dynamicTextBuffer, processedTextLen + 4));
					textBufferSize = processedTextLen + 4;
				}
			}

			if (maxAllowedLines > 0 && currentLineCount > maxAllowedLines && processedTextLen)
			{
				while (processedTextLen > 0 && dynamicTextBuffer[processedTextLen - 1] != axData->cLineSep)
				{
					--processedTextLen;
					--origConsumed;
				}
				dynamicTextBuffer[processedTextLen] = 0;
				currentLineCount = maxAllowedLines;
				currentLineWidth = 0;
				totalTextHeight = totalTextHeight - lineHeight;
				break;
			}
		}

		if (*dynamicTextBuffer && axData->iLineStart)
		{
			UInt32 truncatedTextLen = 0;
			SInt32 lineCounter = 0;
			for (UInt32 truncateCharCounter = 0; truncateCharCounter < processedTextLen; ++truncateCharCounter)
			{
				if (lineCounter >= axData->iLineStart && lineCounter < axData->iLineEnd)
					dynamicTextBuffer[truncatedTextLen++] = dynamicTextBuffer[truncateCharCounter];
				if (dynamicTextBuffer[truncateCharCounter] == axData->cLineSep)
					++lineCounter;
			}
			dynamicTextBuffer[truncatedTextLen] = 0;
			processedTextLen = truncatedTextLen;
			origConsumed = truncatedTextLen;
		}

		if (!*dynamicTextBuffer)
		{
			strcpy_s(dynamicTextBuffer, textBufferSize, " ");
			origConsumed = 1;
			processedTextLen = 1;
			currentLineCount = 1;
			totalTextHeight = pFontLetters[kSpaceChar].fHeight;
			currentLineWidth = ConditionalFloatToUInt(pFontLetters[kSpaceChar].fWidth);
		}
		axData->xLineWidths.AddTail(currentLineWidth);
		maxLineWidth = MaxInt(maxLineWidth, currentLineWidth);
		dynamicTextBuffer[processedTextLen] = 0;
		axData->xNewText.Set(dynamicTextBuffer, 0);
		axData->iWidth = maxLineWidth;
		axData->iHeight = totalTextHeight;
		axData->iLineStart = 0;
		axData->iLineEnd = currentLineCount;
		axData->iCharCount = isTerminal ? origConsumed : processedTextLen;
		MemoryManager_s_Instance->Deallocate(processedOriginalText);
		MemoryManager_s_Instance->Deallocate(dynamicTextBuffer);
	}

	// ==================== FontEx::PrepTextForTerminal ====================
	void __thiscall FontEx::PrepTextForTerminal(const char* apOrigString, Font::TextData* axData)
	{
		PrepTextImpl(this, apOrigString, axData, true);
	}

	// ==================== FontEx::PrepText ====================
	void __thiscall FontEx::PrepText(const char* apOrigString, Font::TextData* axData)
	{
		PrepTextImpl(this, apOrigString, axData, false);
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

	static float GetPreparedIconAdvance(FontEx* font, int iconIndex)
	{
		if (!font || !font->pFontData)
			return 0.0f;

		const FontLetter* iconGlyph = &font->pFontData->pFontLetters[1];
		if (!font->ButtonIcons.pBuffer
			|| iconIndex < 0
			|| iconIndex >= static_cast<int>(font->ButtonIcons.uiSize))
		{
			return GetGlyphRenderAdvance(iconGlyph);
		}

		const ButtonIcon& icon = font->ButtonIcons.pBuffer[iconIndex];
		return iconGlyph->fLeadingEdge + icon.fWidth
			+ (icon.fWidth > 0.0f ? icon.fSpacing : 0.0f);
	}

	static std::vector<float> MeasureFreeTypePreparedLineWidths(
		FontEx* font,
		const char* text,
		char lineBreakChar)
	{
		std::vector<float> widths;
		float lineAdvance = 0.0f;
		int iconIndex = 0;

		for (int byteIndex = 0; text && text[byteIndex]; ++byteIndex)
		{
			const UInt8 current = static_cast<UInt8>(text[byteIndex]);
			if (current == static_cast<UInt8>(lineBreakChar))
			{
				widths.push_back(lineAdvance);
				lineAdvance = 0.0f;
				continue;
			}
			if (current == '\t')
			{
				const float remainder = fmodf(lineAdvance, static_cast<float>(kTabWidth));
				lineAdvance += static_cast<float>(kTabWidth) - remainder;
				continue;
			}
			if (current == 1)
			{
				lineAdvance += GetPreparedIconAdvance(font, iconIndex++);
				continue;
			}
			if (current < 0x20 || current == kDelChar)
				continue;

			VectorEncodedGlyph glyph;
			if (!DecodeRenderableVectorGlyph(font, &text[byteIndex], glyph))
				continue;
			lineAdvance += GetGlyphRenderAdvance(glyph.metrics);
			byteIndex += glyph.byteLength - 1;
		}

		widths.push_back(lineAdvance);
		return widths;
	}

	static float GetLineAlignmentOffset(int flags, float lineWidth)
	{
		if (flags == 4)
			return -lineWidth;
		if (flags == 2)
			return lineWidth * -0.5f;
		return 0.0f;
	}

	static void LogFreeTypeOrdinaryAlignmentOnce(
		const FontEx* font,
		int flags,
		int preparedWidth,
		float vectorWidth,
		float alignmentOffset)
	{
		static bool logged[32][3] = {};
		if (!font || font->iFontNum < 0 || font->iFontNum >= 32)
			return;

		const int alignmentIndex = flags == 2 ? 1 : flags == 4 ? 2 : 0;
		if (logged[font->iFontNum][alignmentIndex])
			return;
		logged[font->iFontNum][alignmentIndex] = true;

		const char* alignmentName = flags == 2 ? "center" : flags == 4 ? "right" : "left";
		FreeTypeFontDebugLog(
			"tnvse_freetype_font: ordinary layout font=%d flags=%d alignment=%s preparedWidth=%d vectorWidth=%.3f offset=%.3f",
			font->iFontNum,
			flags,
			alignmentName,
			preparedWidth,
			vectorWidth,
			alignmentOffset);
	}

	static UInt32 CreateFreeTypePreparedText(
		FontEx* font,
		Font::TextData& textData,
		int* aiWidth,
		int* aiHeight,
		int aiFlags,
		char aiLineBreakChar,
		const NiColorA* fontColor,
		NiTriShape** textShape,
		NiTriShape** iconShape)
	{
		*textShape = nullptr;
		*iconShape = nullptr;
		VectorTextBuilder builder(font, true);
		if (!builder.IsAvailable())
		{
			if (NiNode* empty = NiNode::Create())
				*textShape = reinterpret_cast<NiTriShape*>(empty);
			font->ButtonIcons.Clear(1);
			return ThisStdCall<UInt32>(0x7593E0, reinterpret_cast<char*>(&textData));
		}

		const std::vector<float> lineWidths = MeasureFreeTypePreparedLineWidths(
			font, textData.xNewText.pString, aiLineBreakChar);
		float maxLineWidth = 0.0f;
		for (float lineWidth : lineWidths)
		{
			if (lineWidth > maxLineWidth)
				maxLineWidth = lineWidth;
		}
		*aiWidth = static_cast<int>(ceilf(maxLineWidth));

		size_t lineIndex = 0;
		float lineWidth = lineWidths.empty() ? 0.0f : lineWidths[0];
		float lineOrigin = GetLineAlignmentOffset(aiFlags, lineWidth);
		LogFreeTypeOrdinaryAlignmentOnce(
			font, aiFlags, textData.xLineWidths.m_item, lineWidth, lineOrigin);

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
		float lineAdvance = 0.0f;
		int iconIndex = 0;
		for (int byteIndex = 0; textData.xNewText.pString[byteIndex]; ++byteIndex)
		{
			const UInt8 current = static_cast<UInt8>(textData.xNewText.pString[byteIndex]);
			if (current == static_cast<UInt8>(aiLineBreakChar))
			{
				++lineIndex;
				lineWidth = lineIndex < lineWidths.size() ? lineWidths[lineIndex] : 0.0f;
				lineOrigin = GetLineAlignmentOffset(aiFlags, lineWidth);
				lineAdvance = 0.0f;
				position.x = lineOrigin;
				position.z -= font->pFontData->fBaseLine + linePadding;
				continue;
			}
			if (current == '\t')
			{
				const float remainder = fmodf(lineAdvance, static_cast<float>(kTabWidth));
				lineAdvance += static_cast<float>(kTabWidth) - remainder;
				position.x = lineOrigin + lineAdvance;
				continue;
			}
			if (current == 1)
			{
				if (icons && font->ButtonIcons.pBuffer)
				{
					font->AddIcon(iconIndex++, icons, &position);
					lineAdvance = position.x - lineOrigin;
				}
				else
				{
					lineAdvance += GetPreparedIconAdvance(font, iconIndex++);
					position.x = lineOrigin + lineAdvance;
				}
				continue;
			}
			if (current < 0x20 || current == kDelChar)
				continue;

			VectorEncodedGlyph glyph;
			if (!DecodeRenderableVectorGlyph(font, &textData.xNewText.pString[byteIndex], glyph))
				continue;
			builder.AddGlyph(glyph, position, fontColor);
			lineAdvance += GetGlyphRenderAdvance(glyph.metrics);
			position.x = lineOrigin + lineAdvance;
			byteIndex += glyph.byteLength - 1;
		}

		NiNode* root = builder.Finish();
		if (!root)
			root = NiNode::Create();
		if (root)
		{
			root->m_kLocal.m_Translate = NiPoint3(0.0f, 0.0f,
				static_cast<float>(lineBaseOffset + lineBaseOffset));
			*textShape = reinterpret_cast<NiTriShape*>(root);
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
			return CreateFreeTypePreparedText(this, textData, aiWidth, aiHeight,
				aiFlags, aiLineBreakChar, axFontColor, apTextShape, apIconShape);
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
			VectorTextBuilder builder(this, abPrepareObject_1);
			if (!builder.IsAvailable())
			{
				NiNode* empty = NiNode::Create();
				if (empty)
				empty->m_kLocal.m_Translate = NiPoint3(afStartX, currentZ, currentY);
				return reinterpret_cast<NiTriShape*>(empty);
			}

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

				VectorEncodedGlyph glyph;
				if (!DecodeRenderableVectorGlyph(this, &apTextString->pString[byteIndex], glyph))
					continue;
				const NiPoint3 pen(currentX, currentY, currentZ);
				builder.AddGlyph(glyph, pen, activeColor ? activeColor : arg1C);
				currentX += GetGlyphRenderAdvance(glyph.metrics);
				byteIndex += glyph.byteLength - 1;
				*aiWidth = MaxInt(*aiWidth, ConditionalFloatToUInt(currentX - startX));
			}

			NiNode* root = builder.Finish();
			if (!root)
				root = NiNode::Create();
			if (root)
				root->m_kLocal.m_Translate = NiPoint3(afStartX, currentZ, startY);
			return reinterpret_cast<NiTriShape*>(root);
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

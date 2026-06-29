#include "font_engine.h"
#include "dictionary.h"
#include "native_calls.h"

namespace fonthook
{
	// ---- Helper: find extra glyphs for a given font number ----
	static auto GetExtraGlyphs(int fontNum)
	{
		auto it = gNumberedExtraLetters.find(fontNum);
		return it != gNumberedExtraLetters.end() ? &it->second : nullptr;
	}

	// ---- Helper: look up a double-byte glyph, returns nullptr if not found ----
	static FontLetter* LookupDBGlyph(std::unordered_map<UInt32, FontLetter>* extraGlyphs, UInt32 code)
	{
		if (!extraGlyphs) return nullptr;
		auto it = extraGlyphs->find(code);
		return it != extraGlyphs->end() ? &it->second : nullptr;
	}

	// ---- Helper: look up last character glyph (for wrap handling) ----
	static FontLetter* LookupLastCharGlyph(
		std::unordered_map<UInt32, FontLetter>* extraGlyphs,
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
	static UInt32 AdjustWrapPositionForDB(UInt32 insertPos, const char* buffer)
	{
		if (insertPos > 0 && IsLeadByte((UInt8)buffer[insertPos - 1]))
		{
			return insertPos - 1;
		}
		return insertPos;
	}

	// ==================== FontEx::FontInit ====================
	Font* FontEx::FontInit(int iFontNum, char* apFilename, bool abLoad)
	{
		TlsSlotGuard tlsGuard;

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
			return;
		}

		// ---- Open and validate font file ----
		BSFile* fontFile = FileFinder_GetFile(this->pFontFile, (NiFile::OpenMode)0, 0x150000u, 2u);
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
		// instead of 24,960 individual m_pfnRead calls → 128 calls total
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
		float baseLine = this->pFontData->fBaseLine;

		for (int i = 0; i < kMaxGlyphCount; ++i)
		{
			auto& letter = pLetters[i];
			float glyphHeight = baseLine - letter.fTopEdge + letter.fHeight;
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
				(const char*)texNameBuffer, (NiFile::OpenMode)0, 0x5000000u, 2u);
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

			if (ReplaceVariableInString(varNameBuffer, parsedTextBuffer, 0x400u, isPositiveEscape)
				|| ParseAndFormatVariableInString(varNameBuffer, parsedTextBuffer))
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
		float lineSpacingAdjust = FontManagerGetLinePadding(font->iFontNum);
		float lineHeight = fBaseLine + lineSpacingAdjust;
		UInt32 lastWrapPosition = 0;
		SInt32 preSpaceWidth = 0;
		SInt32 postSpaceWidth = 0;
		SInt32 currentLineWidth = 0;
		SInt32 maxLineWidth = 0;
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
				AppendToListTail(&axData->xLineWidths, &currentLineWidth);
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
					charWidthWithKerning = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
					currentLineWidth += charWidthWithKerning;
					if (currentChar == '~')
					{
						lastWrapPosition = processedTextLen;
						isTildeChar = true;
						UInt32 tildeCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
						currentLineWidth -= tildeCharWidth;
						preSpaceWidth = currentLineWidth;
						postSpaceWidth = currentLineWidth;
					}
				}
				else
				{
					origConsumed += 2;
					FontLetter* glyph = LookupDBGlyph(extraGlyphs, uiDoubleByteCode);
					if (glyph)
					{
						pCurrentGlyph = glyph;
						charWidthWithKerning = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
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

							UInt32 insertPos = AdjustWrapPositionForDB(lastWrapPosition, dynamicTextBuffer);
							memmove(&dynamicTextBuffer[insertPos + 1], &dynamicTextBuffer[insertPos],
								(processedTextLen - insertPos) + 1);
							dynamicTextBuffer[insertPos] = axData->cLineSep;
							processedTextLen += 1;

							AppendToListTail(&axData->xLineWidths, &currentLineWidth);
							maxLineWidth = MaxInt(maxLineWidth, currentLineWidth);
							lastWrapPosition = 0;
							++currentLineCount;

							pCurrentGlyph = LookupLastCharGlyph(extraGlyphs, dynamicTextBuffer, processedTextLen, font->pFontData, uiTempDoubleByteCode, bLastIsDBCharacter);
							currentLineWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);

							if (bIsDBCharacter)
							{
								FontLetter* glyph = LookupDBGlyph(extraGlyphs, uiDoubleByteCode);
								if (glyph) pCurrentGlyph = glyph;
							}
							else
							{
								pCurrentGlyph = &pFontLetters[currentChar];
							}

							UInt32 nextCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
							currentLineWidth += nextCharWidth;
						}
						else
						{
							if (lastWrapPosition == processedTextLen)
								currentChar = axData->cLineSep;
							else
								dynamicTextBuffer[lastWrapPosition] = axData->cLineSep;
							totalTextHeight = lineHeight + totalTextHeight;
							AppendToListTail(&axData->xLineWidths, &preSpaceWidth);
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
							UInt8 lastChar = (UInt8)dynamicTextBuffer[processedTextLen - 1];
							if (extraGlyphs && IsLeadByte(lastChar))
							{
								tailStart = processedTextLen - 2;
								tailBytes = processedTextLen - tailStart;
							}
						}

						memmove(&dynamicTextBuffer[tailStart + 1], &dynamicTextBuffer[tailStart], tailBytes);
						dynamicTextBuffer[tailStart] = axData->cLineSep;

						processedTextLen += 1;
						totalTextHeight += lineHeight;

						AppendToListTail(&axData->xLineWidths, &currentLineWidth);
						maxLineWidth = MaxInt(maxLineWidth, currentLineWidth);
						lastWrapPosition = 0;
						++currentLineCount;

						pCurrentGlyph = LookupLastCharGlyph(extraGlyphs, dynamicTextBuffer, processedTextLen, font->pFontData, uiTempDoubleByteCode, bLastIsDBCharacter);
						currentLineWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);

						if (bIsDBCharacter)
						{
							FontLetter* glyph = LookupDBGlyph(extraGlyphs, uiDoubleByteCode);
							if (glyph) pCurrentGlyph = glyph;
						}
						else
						{
							pCurrentGlyph = &pFontLetters[currentChar];
						}

						UInt32 combinedCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
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
		AppendToListTail(&axData->xLineWidths, &currentLineWidth);
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

		float linePadding = FontManagerGetLinePadding(this->iFontNum);
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
			ThisStdCall(0xA67050, *reinterpret_cast<NiTriShapeData**>(reinterpret_cast<char*>(pIconShape) + 0xB8), 0x4000);
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
					// Advance cursor by 1 instead of traversing from head (O(n²) → O(n))
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
				float prevTabX = textPosition.x;
				AlignLineWidthToTab(textPosition.x, 75.0);
				textPosition.x = 75.0 - prevTabX + textPosition.x;
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
		// Use direct offset (0xB8) to access m_spModelData, bypassing unimplemented GetModelData()
		auto* pTextGeomData = *reinterpret_cast<NiTriShapeData**>(reinterpret_cast<char*>(pTextShape) + 0xB8);
		ThisStdCall(0xA7EE30, &pTextGeomData->m_kBound,
			pTextGeomData->m_usVertices, pTextGeomData->m_pkVertex);
		if (pIconShape)
		{
			auto* pIconGeomData = *reinterpret_cast<NiTriShapeData**>(reinterpret_cast<char*>(pIconShape) + 0xB8);
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
				double prevXPos = currentX;
				AlignLineWidthToTab(currentX, 75.0);
				currentX = 75.0 - (float)prevXPos + currentX;
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
		// Use direct offset (0xB8) to access m_spModelData, bypassing unimplemented GetModelData()
		auto* pGeomData = *reinterpret_cast<NiTriShapeData**>(reinterpret_cast<char*>(pTriShape) + 0xB8);
		ThisStdCall(0xA7EE30, &pGeomData->m_kBound,
			pGeomData->m_usVertices, pGeomData->m_pkVertex);
		return pTriShape;
	}

} // namespace fonthook

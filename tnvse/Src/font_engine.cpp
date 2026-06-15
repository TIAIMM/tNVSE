#include "font_engine.h"
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
			unsigned char lastByte = (unsigned char)buffer[processedLen - 1];
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
		return &fontData->pFontLetters[(unsigned char)buffer[processedLen - 1]];
	}

	// ---- Helper: adjust wrap position to avoid splitting a double-byte character ----
	static UInt32 AdjustWrapPositionForDB(UInt32 insertPos, const char* buffer)
	{
		if (insertPos > 0 && IsLeadByte((unsigned char)buffer[insertPos - 1]))
		{
			return insertPos - 1;
		}
		return insertPos;
	}

	// ==================== FontEx::FontInit ====================
	Font* FontEx::FontInit(int iFontNum, char* apFilename, bool abLoad)
	{
		DWORD tebAddress;
		DWORD tlsPointer;
		DWORD tlsSlotAddress;
		DWORD targetAddress;
		DWORD* pTlsIndex = (DWORD*)0x126FD98;

		StdCall(0xEC782F, this->pTextureData, 4, 8, 0xA1B410, 0x45CEC0);
		this->IconAtlasTextureName.pString = 0;
		this->IconAtlasTextureName.sLen = 0;
		this->IconAtlasTextureName.sMaxLen = 0;
		ThisStdCall(0xA1BEF0, &this->ButtonIcons);

		__asm {
			mov eax, fs: [0x18]
			mov tebAddress, eax
		}
		tlsPointer = *(DWORD*)(tebAddress + 0x2C);
		tlsSlotAddress = *(DWORD*)(tlsPointer + (*pTlsIndex) * 4);
		targetAddress = tlsSlotAddress + 692;

		int savedTlsValue = *(DWORD*)targetAddress;
		*(DWORD*)targetAddress = 12;

		this->pFontFile = 0;
		this->iRefCount = 0;
		this->pFontData = 0;

		if (apFilename)
		{
			UINT32 filenameLen = strlen(apFilename);
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

		*(DWORD*)targetAddress = savedTlsValue;
		return this;
	}

	// ==================== FontEx::Load ====================
	void FontEx::Load()
	{
		// ---- TLS setup ----
		DWORD* pTlsIndex = (DWORD*)0x126FD98;
		DWORD tebAddress;
		__asm {
			mov eax, fs: [0x18]
			mov tebAddress, eax
		}
		DWORD tlsPointer = *(DWORD*)(tebAddress + 0x2C);
		DWORD tlsSlotAddress = *(DWORD*)(tlsPointer + (*pTlsIndex) * 4);
		DWORD targetAddress = tlsSlotAddress + 692;

		int stringRefFlag = 0;
		int savedTlsValue = *(DWORD*)targetAddress;
		*(DWORD*)targetAddress = 12;

		unsigned __int16 refCount = this->iRefCount;
		if (refCount || !this->pFontFile)
		{
			++this->iRefCount;
			*(DWORD*)targetAddress = savedTlsValue;
			return;
		}

		// ---- Open and validate font file ----
		BSFile* fontFile = FileFinder_GetFile(this->pFontFile, (NiFile::OpenMode)0, 0x150000u, 2u);
		if (!fontFile || !fontFile->m_pFile)
		{
			if (fontFile) delete fontFile;
			*(DWORD*)targetAddress = savedTlsValue;
			return;
		}

		// ---- Read font data header ----
		DWORD textureMarkers[2] = {};
		textureMarkers[0] = 1;
		FontData* pFontData = (FontData*)MemoryManager_s_Instance->Allocate(0x3928u);
		this->pFontData = pFontData;

		unsigned int textureReadBytes = fontFile->m_pfnRead(fontFile, pFontData, 0x3928u, textureMarkers, 1u);
		fontFile->m_uiAbsoluteCurrentPos += textureReadBytes;

		// ---- Load extra double-byte glyphs from extended font file ----
		if (g_uiEncoding != 0)
		{
			unsigned int uiActualSize = fontFile->GetSize();
			if (uiActualSize > 0x3928)
			{
				fontNameKey = this->pFontFile ? this->pFontFile : "";
				if (!fontNameKey.empty())
				{
					auto& extraMap = gExtraFontLetters[fontNameKey];
					if (extraMap.empty())
					{
						extraMap.reserve(24066);
						for (unsigned int highByte = 0x81; highByte <= 0xFE; ++highByte)
						{
							for (unsigned int lowByte = 0x40; lowByte <= 0xFE; ++lowByte)
							{
								unsigned int charCode = (highByte << 8) | lowByte;
								FontLetter letter{};
								UInt32 letterRead = fontFile->m_pfnRead(fontFile, &letter, sizeof(letter), textureMarkers, 1u);
								fontFile->m_uiAbsoluteCurrentPos += letterRead;
								if (letterRead != sizeof(letter)) break;
								extraMap[charCode] = letter;
							}
						}
					}
				}
			}
		}

		// Close the font file
		if (fontFile) delete fontFile;

		// ---- Calculate glyph metrics ----
		this->fFontHeight = 0.0;
		float maxHeight = 0.0;
		this->fMaxDrop = 0.0;
		for (int glyphIndex = 0; glyphIndex < 256; ++glyphIndex)
		{
			float glyphFontHeight = this->pFontData->fBaseLine - this->pFontData->pFontLetters[glyphIndex].fTopEdge;
			glyphFontHeight += this->pFontData->pFontLetters[glyphIndex].fHeight;
			this->fFontHeight = MaxFloat(glyphFontHeight, this->fFontHeight);
			maxHeight = MaxFloat(this->pFontData->pFontLetters[glyphIndex].fHeight, glyphFontHeight);
			this->fMaxDrop = MinFloat(
				this->pFontData->pFontLetters[glyphIndex].fTopEdge - this->pFontData->pFontLetters[glyphIndex].fHeight,
				this->fMaxDrop);
		}

		// ---- Copy space/nbsp/delete letter properties ----
		float savedWidth = this->pFontData->pFontLetters[32].fWidth;
		this->pFontData->pFontLetters[32].fWidth = this->pFontData->pFontLetters[32].fSpacing;
		this->pFontData->pFontLetters[32].fSpacing = savedWidth;
		this->pFontData->pFontLetters[32].fHeight = maxHeight;
		this->pFontData->pFontLetters[32].fTopEdge = this->fMaxDrop + maxHeight;
		this->pFontData->pFontLetters[160].fWidth = this->pFontData->pFontLetters[32].fWidth;
		this->pFontData->pFontLetters[160].fSpacing = this->pFontData->pFontLetters[32].fSpacing;
		this->pFontData->pFontLetters[160].fHeight = this->pFontData->pFontLetters[32].fHeight;
		this->pFontData->pFontLetters[160].fTopEdge = this->pFontData->pFontLetters[32].fTopEdge;
		this->pFontData->pFontLetters[127].fWidth = this->pFontData->pFontLetters[124].fWidth;
		this->pFontData->pFontLetters[127].fLeadingEdge = this->pFontData->pFontLetters[124].fLeadingEdge;
		this->pFontData->pFontLetters[127].fSpacing = this->pFontData->pFontLetters[124].fSpacing;
		this->pFontData->pFontLetters[127].fHeight = this->pFontData->pFontLetters[124].fHeight;
		this->pFontData->pFontLetters[127].fTopEdge = this->pFontData->pFontLetters[124].fTopEdge;
		this->pFontData->pFontLetters[0].fWidth = 0.0;
		this->pFontData->pFontLetters[0].fSpacing = 0.0;
		this->pFontData->pFontLetters[0].fHeight = maxHeight;
		this->pFontData->pFontLetters[0].fTopEdge = this->fMaxDrop + maxHeight;
		memset(&this->pFontData->pFontLetters[0].pMapping[0], 0, sizeof(UVMap) * 4);

		if (this->pFontData->iTextureCount > 8)
		{
			*(DWORD*)targetAddress = savedTlsValue;
			return;
		}

		// ---- Load font textures ----
		for (int textureCount = 0; textureCount < this->pFontData->iTextureCount; ++textureCount)
		{
			char texNameBuffer[260];
			_snprintf_s(
				texNameBuffer, 0x100u, _TRUNCATE,
				"TEXTURES\\FONTS\\%s.TEX",
				this->pFontData->pTextureFiles[textureCount].pFilename);

			BSFile* textureReadStream = FileFinder_GetFile(
				(const char*)texNameBuffer, (NiFile::OpenMode)0, 0x5000000u, 2u);

			if (!textureReadStream || !textureReadStream->m_pFile)
			{
				if (textureReadStream) delete textureReadStream;
				*(DWORD*)targetAddress = savedTlsValue;
				return;
			}

			unsigned int texWidth, texHeight;
			textureMarkers[0] = 1;
			unsigned int readFlagValue = textureReadStream->m_pfnRead(textureReadStream, &texWidth, 4u, textureMarkers, 1u);
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
			unsigned int bytesRead = textureReadStream->m_pfnRead(
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

		++this->iRefCount;
		*(DWORD*)targetAddress = savedTlsValue;
	}

	// ==================== Shared PrepText implementation ====================
	// PrepTextForTerminal and PrepText differ only in how iCharCount is set at the end:
	//   PrepTextForTerminal: iCharCount = origConsumed
	//   PrepText:            iCharCount = processedTextLen
	static void PrepTextImpl(FontEx* font, const char* apOrigString, Font::TextData* axData, bool isTerminal)
	{
		if (!apOrigString)
			return;

		if (axData->iWidth <= 0)
			axData->iWidth = 0x7FFFFFFF;
		if (axData->iHeight <= 0)
			axData->iHeight = 0x7FFFFFFF;
		if (axData->iLineEnd <= 0)
			axData->iLineEnd = 0x7FFFFFFF;

		auto* extraGlyphs = GetExtraGlyphs(font->iFontNum);
		UInt32 origConsumed = 0;

		float lineSpacingAdjust = FontManagerGetLinePadding(font->iFontNum);
		unsigned int lastWrapPosition = 0;
		SInt32 preSpaceWidth = 0;
		SInt32 postSpaceWidth = 0;
		SInt32 currentLineWidth = 0;
		SInt32 maxLineWidth = 0;
		float totalTextHeight = font->pFontData->pFontLetters[' '].fHeight;
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

		unsigned int processedTextLen = 0;
		UInt32 textBufferSize = sourceTextLen + 4;
		bool isTildeChar = false;
		char parsedTextBuffer[1028] = {};
		bool hasEscapeSequence = false;

		// ---- Pass 1: Process escape sequences (&variable;) ----
		for (unsigned int srcTextIndex = 0; srcTextIndex < sourceTextLen; ++srcTextIndex)
		{
			if (processedOriginalText[srcTextIndex] == '&')
			{
				int varNameLen = 0;
				int escapeSeqPrefixLen = 1;
				bool isPositiveEscape = true;
				if (processedOriginalText[srcTextIndex + 1] == '-')
				{
					isPositiveEscape = false;
					escapeSeqPrefixLen = 2;
				}
				char varNameBuffer[128];
				while (processedOriginalText[escapeSeqPrefixLen + varNameLen + srcTextIndex]
					&& varNameLen < 127
					&& processedOriginalText[varNameLen + srcTextIndex] != ';'
					&& processedOriginalText[varNameLen + srcTextIndex] != '\n'
					&& processedOriginalText[varNameLen + srcTextIndex] != axData->cLineSep)
				{
					varNameBuffer[varNameLen] = processedOriginalText[escapeSeqPrefixLen + varNameLen + srcTextIndex];
					++varNameLen;
				}
				int escapeSeqEffectiveLen = varNameLen ? varNameLen - escapeSeqPrefixLen : 0;
				varNameBuffer[escapeSeqEffectiveLen] = 0;
				UInt32 totalEscapeSeqLen = strlen(varNameBuffer) + 1;
				if (processedOriginalText[varNameLen + srcTextIndex] == ';')
					totalEscapeSeqLen += escapeSeqPrefixLen;

				if (ReplaceVariableInString(varNameBuffer, parsedTextBuffer, 0x400u, isPositiveEscape)
					|| ParseAndFormatVariableInString(varNameBuffer, parsedTextBuffer))
				{
					UInt32 postEscapeTextLen = strlen(parsedTextBuffer);
					int escapeSeqSizeDiff = postEscapeTextLen - totalEscapeSeqLen;

					if (postEscapeTextLen > 0 && parsedTextBuffer[postEscapeTextLen - 1] == '\\')
					{
						float unkarray[4] = {};
						int charScanIndex = 0;
						while (parsedTextBuffer[charScanIndex] != '\\') ++charScanIndex;

						char substrBuffer[264] = {};
						char textureNameBuffer[268];
						strcpy_s(&parsedTextBuffer[charScanIndex + 1],
							sizeof(parsedTextBuffer) - (charScanIndex + 1), substrBuffer);
						UInt32 strLen = strlen(substrBuffer);
						*((char*)unkarray + strLen + 15) = 0;
						if (font->iFontNum == 7)
						{
							strcpy_s(textureNameBuffer, 0x104u, substrBuffer);
							sprintf_s(substrBuffer, 0x104u, "glow_%s", textureNameBuffer);
						}
						font->AddTextIcon(substrBuffer);
						parsedTextBuffer[charScanIndex] = 1;
						parsedTextBuffer[charScanIndex + 1] = 0;
						postEscapeTextLen = strlen(parsedTextBuffer);
						escapeSeqSizeDiff = postEscapeTextLen - totalEscapeSeqLen;
					}
					if (escapeSeqSizeDiff > 0)
					{
						textBufferSize += escapeSeqSizeDiff;
						dynamicTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Reallocate(dynamicTextBuffer, textBufferSize + 1));
					}
					for (UInt32 bufferCopyIndex = 0; bufferCopyIndex < postEscapeTextLen; ++bufferCopyIndex)
						dynamicTextBuffer[processedTextLen++] = parsedTextBuffer[bufferCopyIndex];
					origConsumed += totalEscapeSeqLen;
					srcTextIndex = srcTextIndex + totalEscapeSeqLen - 1;
				}
				else
				{
					dynamicTextBuffer[processedTextLen++] = processedOriginalText[srcTextIndex];
				}
				hasEscapeSequence = true;
			}
			else
			{
				dynamicTextBuffer[processedTextLen++] = processedOriginalText[srcTextIndex];
			}
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
				totalTextHeight = font->pFontData->fBaseLine + lineSpacingAdjust + totalTextHeight;
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

				unsigned __int8 currentChar;
				FontLetter* pCurrentGlyph;
				unsigned int charWidthWithKerning;

				if (!bIsDBCharacter)
				{
					currentChar = processedOriginalText[charIndex];
					origConsumed += 1;
					ConvertToAsciiQuotes(&currentChar);
					pCurrentGlyph = &font->pFontData->pFontLetters[currentChar];
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
						unsigned int tildeCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
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
								pCurrentGlyph = &font->pFontData->pFontLetters[currentChar];
							}

							unsigned int nextCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
							currentLineWidth += nextCharWidth;
						}
						else
						{
							if (lastWrapPosition == processedTextLen)
								currentChar = axData->cLineSep;
							else
								dynamicTextBuffer[lastWrapPosition] = axData->cLineSep;
							totalTextHeight = font->pFontData->fBaseLine + lineSpacingAdjust + totalTextHeight;
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
							unsigned char lastChar = (unsigned char)dynamicTextBuffer[processedTextLen - 1];
							if (extraGlyphs && IsLeadByte(lastChar))
							{
								tailStart = processedTextLen - 2;
								tailBytes = processedTextLen - tailStart;
							}
						}

						memmove(&dynamicTextBuffer[tailStart + 1], &dynamicTextBuffer[tailStart], tailBytes);
						dynamicTextBuffer[tailStart] = axData->cLineSep;

						processedTextLen += 1;
						totalTextHeight += (font->pFontData->fBaseLine + lineSpacingAdjust);

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
							pCurrentGlyph = &font->pFontData->pFontLetters[currentChar];
						}

						unsigned int combinedCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
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
				totalTextHeight = totalTextHeight - (font->pFontData->fBaseLine + lineSpacingAdjust);
				break;
			}
		}

		if (*dynamicTextBuffer && axData->iLineStart)
		{
			unsigned int truncatedTextLen = 0;
			SInt32 lineCounter = 0;
			for (unsigned int truncateCharCounter = 0; truncateCharCounter < processedTextLen; ++truncateCharCounter)
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
			totalTextHeight = font->pFontData->pFontLetters[' '].fHeight;
			currentLineWidth = ConditionalFloatToUInt(font->pFontData->pFontLetters[' '].fWidth);
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
		const NiColorA* axFontColor, UINT32** apTextShape, UINT32** apIconShape)
	{

		auto* extraGlyphs = GetExtraGlyphs(this->iFontNum);
		Font::TextData textData;

		if (!*aiHeight)
			*aiHeight = 0x7FFFFFFF;
		if (!aiLineEnd)
			aiLineEnd = 0x7FFFFFFF;

		float linePadding = FontManagerGetLinePadding(this->iFontNum);
		ThisStdCall(0x759330, &textData, *aiWidth, *aiHeight, aiLineStart, aiLineEnd, aiLineBreakChar);

		if (g_bEnableUTF8 && g_uiEncoding != 0 && extraGlyphs)
		{
			if (IsValidUTF8With3ByteMin(axTextString->pString))
			{
				std::string sCurrentStr = axTextString->pString;
				std::string sConvertedStr = UTF8ToMultiByteStr(sCurrentStr, g_usingWinEncoding);
				axTextString->Set(sConvertedStr.c_str());
			}
		}

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

		int iActualCharCount = textData.iCharCount;
		if (extraGlyphs)
		{
			UInt32 uiDoubleByteCode;
			for (int charIdx = 0;
				textData.xNewText.pString[textData.xNewText.pString ? charIdx : 0];
				++charIdx)
			{
				bool bIsDBCharacter = false;
				unsigned char cNextByte = (unsigned char)textData.xNewText.pString[charIdx + 1];
				if (cNextByte != 0)
				{
					bIsDBCharacter = TryDecodeDoubleByte(
						(const char*)&textData.xNewText.pString[charIdx], uiDoubleByteCode);
				}
				if (bIsDBCharacter)
				{
					++charIdx;
					--iActualCharCount;
				}
			}
		}

		*apTextShape = (UINT32*)Font::MakeTriShape(iActualCharCount, axFontColor, 1);

		// Initialize text shape position data
		UINT32* pTextShapeData = *apTextShape + 22;
		*(float*)pTextShapeData = 0.0f;
		pTextShapeData[1] = 0;       // textPosition.y (initially 0)
		pTextShapeData[2] = *(UINT32*)&textPosition.z;

		if (this->ButtonIcons.uiSize)
		{
			*apIconShape = (UINT32*)Font::MakeIconsTriShape();
			UINT32* pIconShapeData = *apIconShape + 22;
			pIconShapeData[0] = 0;
			pIconShapeData[1] = 0;
			pIconShapeData[2] = *(UINT32*)&textPosition.z;
			ThisStdCall(0xA67050, (NiGeometryData*)(*apIconShape)[46], 0x4000);
		}

		float yOffsetStart = textPosition.x;
		int lineCounter = 0;
		int vertexIdx = 0;
		int iconIdx = 0;

		UInt32 uiDoubleByteCode;
		for (int charIdx = 0;
			textData.xNewText.pString[textData.xNewText.pString ? charIdx : 0];
			++charIdx)
		{

			if (textData.xNewText.pString[textData.xNewText.pString ? charIdx : 0] == aiLineBreakChar)
			{
				++lineCounter;
				textPosition.x = 0.0;
				if (aiFlags == 4)
				{
					BSSimpleList<int>* pNode = &textData.xLineWidths;
					for (int lineIdx = 0; lineIdx < lineCounter && pNode; ++lineIdx)
						pNode = pNode->m_pkNext;
					textPosition.x = (float)(pNode ? -pNode->m_item : 1);
				}
				else if (aiFlags == 2)
				{
					BSSimpleList<int>* pNode = &textData.xLineWidths;
					for (int lineIdx = 0; lineIdx < lineCounter && pNode; ++lineIdx)
						pNode = pNode->m_pkNext;
					textPosition.x = (float)(pNode ? pNode->m_item / -2 : 0);
				}
				textPosition.z = textPosition.z - (this->pFontData->fBaseLine + linePadding);
			}
			else if (textData.xNewText.pString[textData.xNewText.pString ? charIdx : 0] == '\t')
			{
				float prevTabX = textPosition.x;
				AlignLineWidthToTab(textPosition.x, 75.0);
				textPosition.x = 75.0 - prevTabX + textPosition.x;
			}

			unsigned __int8 currentChar = textData.xNewText.pString[textData.xNewText.pString ? charIdx : 0];

			bool bIsDBCharacter = false;
			if (extraGlyphs)
			{
				unsigned char cNextByte = (unsigned char)textData.xNewText.pString[charIdx + 1];
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
				if (this->ButtonIcons.uiSize)
					Font::AddIcon(iconIdx++, (NiTriShape*)*apIconShape, &textPosition);
			}
			else
			{
				FontLetter* glyph = LookupDBGlyph(extraGlyphs, uiDoubleByteCode);
				if (extraGlyphs && bIsDBCharacter && glyph)
				{
					StdCall<FontLetter*>(0xA142D0, glyph, vertexIdx++,
						(NiTriShape*)*apTextShape, &textPosition.x, axFontColor);
					++charIdx;
					rendered = true;
				}
				if (!rendered)
				{
					StdCall<FontLetter*>(0xA142D0, &this->pFontData->pFontLetters[currentChar],
						vertexIdx++, (NiTriShape*)*apTextShape, &textPosition.x, axFontColor);
				}
			}
			int maxRenderedWidth = *aiWidth;
			int renderedWidth = ConditionalFloatToUInt(textPosition.x - yOffsetStart);
			*aiWidth = MaxInt(renderedWidth, maxRenderedWidth);
		}

		*(DWORD*)&textData.cLineSep = *(DWORD*)((*apTextShape)[46] + 32);
		ThisStdCall(0xA7EE30, (float*)((*apTextShape)[46] + 16),
			*(unsigned __int16*)((*apTextShape)[46] + 8), *(float**)&textData.cLineSep);
		if (*apIconShape)
			ThisStdCall(0xA7EE30, (float*)((*apIconShape)[46] + 16),
				*(unsigned __int16*)((*apIconShape)[46] + 8),
				*(float**)((*apIconShape)[46] + 32));
		this->ButtonIcons.Clear(1);
		return ThisStdCall(0x7593E0, (char*)&textData);
	}

	// ==================== FontEx::MakeString ====================
	UInt32* FontEx::MakeString(
		float afStartX, float afStartY, float afZ,
		BSStringT<char>* apTextString, int* aiWidth, bool abPrepareObject,
		const NiColorA* arg1C, bool abUpperLeftCorner, bool abPrepareObject_1)
	{

		auto* extraGlyphs = GetExtraGlyphs(this->iFontNum);

		if (g_bEnableUTF8 && g_uiEncoding != 0 && extraGlyphs)
		{
			if (IsValidUTF8With3ByteMin(apTextString->pString))
			{
				std::string sCurrentStr = apTextString->pString;
				std::string sConvertedStr = UTF8ToMultiByteStr(sCurrentStr, g_usingWinEncoding);
				apTextString->Set(sConvertedStr.c_str());
			}
		}

		UINT32 textLen = (apTextString->sLen == 0xFFFF)
			? strlen(apTextString->pString) : apTextString->sLen;
		if (!textLen || !this->pFontData)
			return 0;

		char newlineBuffer[4];
		float textXOffset = (float)*aiWidth;
		ThisStdCall(0xA12370, (int*)this, apTextString->pString, &textXOffset,
			(int)newlineBuffer, abPrepareObject, 0);

		float currentX = afStartX + textXOffset;
		float currentY = afStartY;
		float currentZ = afZ;

		if (abUpperLeftCorner)
		{
			double lineBaseOffset = this->pFontData->pFontLetters[32].fHeight - this->pFontData->fBaseLine;
			currentY = currentY - (lineBaseOffset + lineBaseOffset);
		}

		// Count printable characters
		UINT32 charIdx = 0;
		for (; ; ++charIdx)
		{
			UINT32 stringLength = (apTextString->sLen == 0xFFFF)
				? strlen(apTextString->pString) : apTextString->sLen;
			if (charIdx >= stringLength || !apTextString->pString[apTextString->pString ? charIdx : 0])
				break;
		}

		if (!charIdx)
			return 0;

		int iActualCharCount = charIdx;

		// Adjust count for double-byte characters
		if (extraGlyphs)
		{
			UINT32 stringLength = (apTextString->sLen == 0xFFFF)
				? strlen(apTextString->pString) : apTextString->sLen;
			UInt32 uiDoubleByteCode;
			for (UINT32 scanIdx = 0;
				apTextString->pString[apTextString->pString ? scanIdx : 0];
				++scanIdx)
			{
				if (scanIdx >= stringLength) break;

				unsigned char cNextByte = (unsigned char)apTextString->pString[scanIdx + 1];
				if (cNextByte != 0)
				{
					bool bIsDBCharacter = TryDecodeDoubleByte(
						(const char*)&apTextString->pString[scanIdx], uiDoubleByteCode);
					if (bIsDBCharacter)
					{
						++scanIdx;
						--iActualCharCount;
					}
				}
			}
		}

		UINT32* pTriShape = (UINT32*)Font::MakeTriShape(iActualCharCount, arg1C, abPrepareObject_1);
		float startY = currentY;
		*((float*)pTriShape + 22) = afStartX;
		*((float*)pTriShape + 23) = currentZ;
		*((float*)pTriShape + 24) = startY;

		NiColorA* pColor = 0;
		float defaultColor[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
		*aiWidth = 0;
		double lineBaseOffset = currentX;
		int vertexIdx = 0;
		UInt32 uiDoubleByteCode;

		for (int lineIdx = 0; apTextString->pString[apTextString->pString ? lineIdx : 0]; ++lineIdx)
		{
			if (apTextString->pString[apTextString->pString ? lineIdx : 0] == 3)
				pColor = 0;

			char currentCharValue = apTextString->pString[apTextString->pString ? lineIdx : 0];
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
				ThisStdCall(0xA12370, (int*)this, apTextString->pString, &tabPrevX,
					(int)escapeBuffer, abPrepareObject, lineIdx + 1);
				currentX = tabPrevX;
				currentY = currentY - this->pFontData->fBaseLine;
			}

			unsigned __int8 currentChar = apTextString->pString[apTextString->pString ? lineIdx : 0];

			bool bIsDBCharacter = false;
			if (extraGlyphs)
			{
				unsigned char cNextByte = (unsigned char)apTextString->pString[lineIdx + 1];
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
						(NiTriShape*)pTriShape, &currentX, pColor);
					lineIdx += 1;
					rendered = true;
				}
			}
			if (!rendered)
			{
				StdCall<FontLetter*>(0xA142D0, &this->pFontData->pFontLetters[currentChar],
					vertexIdx++, (NiTriShape*)pTriShape, &currentX, pColor);
			}

			int prevWidth = *aiWidth;
			int renderedWidth = ConditionalFloatToUInt(currentX - lineBaseOffset);
			*aiWidth = MaxInt(renderedWidth, prevWidth);

			if (apTextString->pString[apTextString->pString ? lineIdx : 0] == 2)
				pColor = (NiColorA*)defaultColor;
		}

		float* pVertexData = *(float**)(pTriShape[46] + 32);
		ThisStdCall(0xA7EE30, (float*)(pTriShape[46] + 16),
			*(unsigned __int16*)(pTriShape[46] + 8), pVertexData);
		return (UInt32*)pTriShape;
	}

} // namespace fonthook

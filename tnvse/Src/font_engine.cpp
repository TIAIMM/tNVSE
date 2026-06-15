#include "font_engine.h"
#include "native_calls.h"

namespace fonthook {

// ---- Helper: find extra glyphs for a given font number ----
static auto GetExtraGlyphs(int fontNum) {
    auto it = gNumberedExtraLetters.find(fontNum);
    return it != gNumberedExtraLetters.end() ? &it->second : nullptr;
}

// ---- Helper: look up a double-byte glyph, returns nullptr if not found ----
static FontLetter* LookupDBGlyph(std::unordered_map<UInt32, FontLetter>* extraGlyphs, UInt32 code) {
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
    if (extraGlyphs && processedLen >= 2) {
        unsigned char lastByte = (unsigned char)buffer[processedLen - 1];
        if (IsTrailByte(lastByte)) {
            if (TryDecodeDoubleByte(&buffer[processedLen - 2], outDBCode)) {
                FontLetter* glyph = LookupDBGlyph(extraGlyphs, outDBCode);
                if (glyph) {
                    outIsDB = true;
                    return glyph;
                }
            }
        }
    }
    return &fontData->pFontLetters[(unsigned char)buffer[processedLen - 1]];
}

// ---- Helper: adjust wrap position to avoid splitting a double-byte character ----
static UInt32 AdjustWrapPositionForDB(UInt32 insertPos, const char* buffer) {
    if (insertPos > 0 && IsLeadByte((unsigned char)buffer[insertPos - 1])) {
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

    if (!fontNameKey.empty()) {
        auto it = gExtraFontLetters.find(fontNameKey);
        if (it != gExtraFontLetters.end() && !it->second.empty()) {
            gNumberedExtraLetters[iFontNum] = std::move(it->second);
            gExtraFontLetters.erase(it);
            if (!gNumberedExtraLetters[iFontNum].empty()) {
                fontNameKey.clear();
            }
        }
        else {
            fontNameKey.clear();
        }
    }

    *(DWORD*)targetAddress = savedTlsValue;
    return this;
}

// ==================== FontEx::Load ====================
void FontEx::Load()
{
    float maxDropCandidate;
    float fontHeightCandidate;
    void* readBuffer;
    unsigned int bytesRead;
    int readFlag;
    unsigned int readFlagValue;
    unsigned int totalBytesRead;
    bool fileOpenSuccess;
    int savedTlsValueTemp;
    char* pFontFilePath;
    float currentMaxDrop;
    float currentMaxDropCandidate;
    float currentHeight;
    float currentHeightCandidate;
    float currentFontHeight;
    unsigned int tempValue;
    FontData* loadedFontData;
    unsigned int textureReadBytes;
    unsigned int readResult;
    DWORD textureMarkers[2];
    char* pFontFilePath2;
    unsigned __int16 refCount;
    int savedTlsValue;
    int stringRefFlag;
    NiFixedString textureName;
    NiTexturingProperty* textureProperty;
    NiTexturingProperty* createdProperty;
    NiPixelData* createdPixelData;
    FontData* pFontData;
    unsigned int texWidth;
    unsigned int texHeight;
    NiTexture::FormatPrefs formatPrefs;
    BSFile* textureReadStream;
    NiTexturingProperty* finalProperty;
    NiPixelData* finalPixelData;
    int textureCount;
    float glyphFontHeight;
    int glyphIndex;
    char texNameBuffer[260];
    float savedWidth;
    BSFile* fontFile;
    BSFile* texFile;
    float maxHeight;

    DWORD tebAddress;
    DWORD tlsPointer;
    DWORD tlsSlotAddress;
    DWORD targetAddress;
    DWORD* pTlsIndex = (DWORD*)0x126FD98;

    __asm {
        mov eax, fs: [0x18]
        mov tebAddress, eax
    }
    tlsPointer = *(DWORD*)(tebAddress + 0x2C);
    tlsSlotAddress = *(DWORD*)(tlsPointer + (*pTlsIndex) * 4);
    targetAddress = tlsSlotAddress + 692;

    stringRefFlag = 0;
    savedTlsValue = *(DWORD*)targetAddress;
    *(DWORD*)targetAddress = 12;

    refCount = this->iRefCount;
    if (refCount || !this->pFontFile)
    {
    LABEL_46:
        ++this->iRefCount;
        *(DWORD*)targetAddress = savedTlsValue;
        return;
    }
    fontFile = FileFinder_GetFile(this->pFontFile, (NiFile::OpenMode)0, 0x150000u, 2u);
    if (fontFile)
    {
        if (LOBYTE(fontFile->m_pFile))
        {
            pFontData = (FontData*)MemoryManager_s_Instance->Allocate(0x3928u);
            this->pFontData = pFontData;
            tempValue = 0x3928u;
            loadedFontData = this->pFontData;
            textureMarkers[0] = 1;
            textureReadBytes = fontFile->m_pfnRead(fontFile, loadedFontData, tempValue, textureMarkers, 1u);
            fontFile->m_uiAbsoluteCurrentPos += textureReadBytes;
            readResult = textureReadBytes;

            if (g_uiEncoding != 0) {
                unsigned int uiActualSize = fontFile->GetSize();
                if (uiActualSize > 0x3928) {
                    fontNameKey = this->pFontFile ? this->pFontFile : "";
                    if (!fontNameKey.empty()) {
                        auto& extraMap = gExtraFontLetters[fontNameKey];
                        if (extraMap.empty()) {
                            extraMap.reserve(24066);
                            unsigned int highByte, lowByte, charCode;
                            for (highByte = 0x81; highByte <= 0xFE; ++highByte) {
                                for (lowByte = 0x40; lowByte <= 0xFE; ++lowByte) {
                                    charCode = (highByte << 8) | lowByte;
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

            texFile = fontFile;
            if (fontFile) {
                delete(fontFile);
            }
            fontFile = 0;
            this->fFontHeight = 0.0;
            maxHeight = 0.0;
            this->fMaxDrop = 0.0;
            for (glyphIndex = 0; glyphIndex < 256; ++glyphIndex)
            {
                glyphFontHeight = this->pFontData->fBaseLine - this->pFontData->pFontLetters[glyphIndex].fTopEdge;
                glyphFontHeight = glyphFontHeight + this->pFontData->pFontLetters[glyphIndex].fHeight;
                currentFontHeight = this->fFontHeight;
                fontHeightCandidate = MaxFloat(glyphFontHeight, currentFontHeight);
                this->fFontHeight = fontHeightCandidate;
                currentHeight = this->pFontData->pFontLetters[glyphIndex].fHeight;
                currentHeightCandidate = MaxFloat(currentHeight, glyphFontHeight);
                maxHeight = currentHeightCandidate;
                currentMaxDrop = this->fMaxDrop;
                currentMaxDropCandidate = this->pFontData->pFontLetters[glyphIndex].fTopEdge - this->pFontData->pFontLetters[glyphIndex].fHeight;
                maxDropCandidate = MinFloat(currentMaxDropCandidate, currentMaxDrop);
                this->fMaxDrop = maxDropCandidate;
            }
            savedWidth = this->pFontData->pFontLetters[32].fWidth;
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
                pFontFilePath = this->pFontFile;
                savedTlsValueTemp = savedTlsValue;
                *(DWORD*)targetAddress = savedTlsValue;
                return;
            }
            for (textureCount = 0; textureCount < this->pFontData->iTextureCount; ++textureCount)
            {
                _snprintf_s(
                    texNameBuffer,
                    0x100u,
                    _TRUNCATE,
                    "TEXTURES\\FONTS\\%s.TEX",
                    this->pFontData->pTextureFiles[textureCount].pFilename
                );

                textureReadStream = FileFinder_GetFile((const char*)texNameBuffer, (NiFile::OpenMode)0, 0x5000000u, 2u);

                if (!textureReadStream || !(fileOpenSuccess = (bool)textureReadStream->m_pFile))
                {
                    if (textureReadStream)
                    {
                        delete(textureReadStream);
                    }
                    textureMarkers[1] = savedTlsValue;
                    *(DWORD*)targetAddress = savedTlsValue;
                    return;
                }

                textureMarkers[0] = 1;
                readFlagValue = textureReadStream->m_pfnRead(textureReadStream, &texWidth, 4u, textureMarkers, 1u);
                readFlagValue += textureReadStream->m_pfnRead(textureReadStream, &texHeight, 4u, textureMarkers, 1u);
                textureReadStream->m_uiAbsoluteCurrentPos += readFlagValue;
                totalBytesRead = readFlagValue;
                formatPrefs.m_ePixelLayout = static_cast<NiTexture::FormatPrefs::PixelLayout>(0x6);
                formatPrefs.m_eAlphaFmt = static_cast<NiTexture::FormatPrefs::AlphaFormat>(0x3);
                formatPrefs.m_eMipMapped = static_cast<NiTexture::FormatPrefs::MipFlag>(0x2);
                createdPixelData = (NiPixelData*)NiMemObject::operator new(sizeof(NiPixelData));
                if (createdPixelData) {
                    createdPixelData = ThisStdCall<NiPixelData*>(
                        0xA7C190,
                        createdPixelData,
                        texWidth,
                        texHeight,
                        reinterpret_cast<const NiPixelFormat*>(0x11AA2A0),
                        1,
                        1);
                }
                else
                    createdPixelData = 0;
                finalPixelData = createdPixelData;
                readBuffer = &finalPixelData->m_pucPixels[*finalPixelData->m_puiOffsetInBytes];
                readFlag = 1;
                bytesRead = textureReadStream->m_pfnRead(textureReadStream, readBuffer, 4 * texHeight * texWidth, (UInt32*)&readFlag, 1u);
                textureReadStream->m_uiAbsoluteCurrentPos += bytesRead;
                finalPixelData->bNoConvert = 1;
                createdProperty = (NiTexturingProperty*)NiMemObject::operator new(sizeof(NiTexturingProperty));
                if (createdProperty)
                {
                    pFontFilePath2 = this->pFontFile;
                    if (pFontFilePath2)
                        textureName.m_kHandle = (char*)NiGlobalStringTable::AddString(pFontFilePath2);
                    else
                        textureName.m_kHandle = 0;
                    stringRefFlag |= 1u;
                    textureProperty = ThisStdCall<NiTexturingProperty*>(
                        0xA6ABB0,
                        createdProperty,
                        finalPixelData,
                        &textureName,
                        &formatPrefs);
                }
                else
                {
                    textureProperty = 0;
                }
                finalProperty = textureProperty;
                if ((stringRefFlag & 1) != 0)
                {
                    stringRefFlag &= ~1u;
                    if (textureName.m_kHandle)
                        InterlockedDecrement((volatile LONG*)textureName.m_kHandle - 2);
                }
                if (textureReadStream) {
                    delete(textureReadStream);
                }
                textureReadStream = 0;
                ThisStdCall(0x60AEB0, finalProperty, 1);
                ThisStdCall(0x66B0D0, &this->pTextureData[textureCount].m_pObject, (int)finalProperty);
            }
            goto LABEL_46;
        }
    }
    pFontFilePath = this->pFontFile;
    if (fontFile)
    {
        delete(fontFile);
    }
    *(DWORD*)targetAddress = savedTlsValue;
}

// ==================== Shared PrepText implementation ====================
// PrepTextForTerminal and PrepText differ only in how iCharCount is set at the end:
//   PrepTextForTerminal: iCharCount = origConsumed
//   PrepText:            iCharCount = processedTextLen
static void PrepTextImpl(FontEx* font, const char* apOrigString, Font::TextData* axData, bool isTerminal) {
    unsigned int charWidthWithKerning;
    unsigned int tildeCharWidth;
    unsigned int nextCharWidth;
    unsigned int combinedCharWidth;
    int escapeSeqEffectiveLen;
    char* processedTextBuffer;
    char* originalTextBuffer;
    unsigned int truncateCharCounter;
    SInt32 lineCounter;
    unsigned int truncatedTextLen;
    unsigned __int8 currentChar;
    FontLetter* pCurrentGlyph;
    UInt32 charIndex;
    UInt32 bufferCopyIndex;
    char textureNameBuffer[268];
    int charScanIndex;
    float unkarray[4];
    char substrBuffer[264];
    signed int escapeSeqSizeDiff;
    UInt32 postEscapeTextLen;
    bool isPositiveEscape;
    int escapeSeqPrefixLen;
    UInt32 totalEscapeSeqLen;
    int varNameLen;
    char varNameBuffer[128];
    unsigned int srcTextIndex;
    SInt32 currentLineWidth;
    unsigned int processedTextLen;
    unsigned int lastWrapPosition;
    SInt32 maxLineWidth;
    char* dynamicTextBuffer;
    UInt32 buttonIconIndex;
    SInt32 preSpaceWidth;
    bool hasEscapeSequence;
    signed int postSpaceWidth;
    bool isTildeChar;
    char* processedOriginalText;
    int currentLineCount;
    UInt32 textBufferSize;
    int hyphenInsertCount;
    char parsedTextBuffer[1028];
    float lineSpacingAdjust;
    float totalTextHeight;
    UInt32 sourceTextLen;
    int maxAllowedLines;

    UInt32 origConsumed = 0;

    bool bLastIsDBCharacter, bIsDBCharacter;
    UInt32 uiDoubleByteCode, uiTempDoubleByteCode;
    auto* extraGlyphs = GetExtraGlyphs(font->iFontNum);

    if (!apOrigString)
        return;

    if (axData->iWidth <= 0)
        axData->iWidth = 0x7FFFFFFF;
    if (axData->iHeight <= 0)
        axData->iHeight = 0x7FFFFFFF;
    if (axData->iLineEnd <= 0)
        axData->iLineEnd = 0x7FFFFFFF;

    lineSpacingAdjust = FontManagerGetLinePadding(font->iFontNum);
    lastWrapPosition = 0;
    preSpaceWidth = 0;
    postSpaceWidth = 0;
    currentLineWidth = 0;
    maxLineWidth = 0;
    totalTextHeight = font->pFontData->pFontLetters[' '].fHeight;
    currentLineCount = 1;
    sourceTextLen = strlen(apOrigString);
    maxAllowedLines = axData->iLineEnd;

    originalTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Allocate(sourceTextLen + 4));
    if (!originalTextBuffer) {
        return;
    }
    memset(originalTextBuffer, 0, sourceTextLen + 4);
    processedOriginalText = originalTextBuffer;

    processedTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Allocate(sourceTextLen + 4));
    if (!processedTextBuffer) {
        MemoryManager_s_Instance->Deallocate(originalTextBuffer);
        return;
    }
    memset(processedTextBuffer, 0, sourceTextLen + 4);
    dynamicTextBuffer = processedTextBuffer;
    snprintf(originalTextBuffer, sourceTextLen + 1, "%s", apOrigString);

    processedTextLen = 0;
    textBufferSize = sourceTextLen + 4;
    hyphenInsertCount = 0;
    isTildeChar = 0;
    parsedTextBuffer[0] = 0;
    hasEscapeSequence = 0;

    // ---- Pass 1: Process escape sequences (&variable;) ----
    for (srcTextIndex = 0; srcTextIndex < sourceTextLen; ++srcTextIndex)
    {
        if (processedOriginalText[srcTextIndex] == '&')
        {
            varNameLen = 0;
            escapeSeqPrefixLen = 1;
            isPositiveEscape = 1;
            if (processedOriginalText[srcTextIndex + 1] == '-')
            {
                isPositiveEscape = 0;
                escapeSeqPrefixLen = 2;
            }
            while (processedOriginalText[escapeSeqPrefixLen + varNameLen + srcTextIndex]
                && varNameLen < 127
                && processedOriginalText[varNameLen + srcTextIndex] != ';'
                && processedOriginalText[varNameLen + srcTextIndex] != '\n'
                && processedOriginalText[varNameLen + srcTextIndex] != axData->cLineSep)
            {
                varNameBuffer[varNameLen] = processedOriginalText[escapeSeqPrefixLen + varNameLen + srcTextIndex];
                ++varNameLen;
            }
            if (varNameLen)
                escapeSeqEffectiveLen = varNameLen - escapeSeqPrefixLen;
            else
                escapeSeqEffectiveLen = 0;
            varNameBuffer[escapeSeqEffectiveLen] = 0;
            totalEscapeSeqLen = (strlen(varNameBuffer) + 1);
            if (processedOriginalText[varNameLen + srcTextIndex] == ';')
                totalEscapeSeqLen += escapeSeqPrefixLen;
            if (ReplaceVariableInString(varNameBuffer, parsedTextBuffer, 0x400u, isPositiveEscape)
                || ParseAndFormatVariableInString(varNameBuffer, parsedTextBuffer))
            {
                postEscapeTextLen = strlen(parsedTextBuffer);
                escapeSeqSizeDiff = postEscapeTextLen - totalEscapeSeqLen;
                if (postEscapeTextLen > 0 && parsedTextBuffer[postEscapeTextLen - 1] == '\\')
                {
                    unkarray[0] = 0.0;
                    unkarray[1] = 0.0;
                    unkarray[2] = 0.0;
                    unkarray[3] = 0.0;
                    for (charScanIndex = 0; parsedTextBuffer[charScanIndex] != '\\'; ++charScanIndex)
                        ;
                    substrBuffer[0] = 0;
                    strcpy_s(&parsedTextBuffer[charScanIndex + 1],
                        sizeof(parsedTextBuffer) - (charScanIndex + 1),
                        substrBuffer);
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
                for (bufferCopyIndex = 0; bufferCopyIndex < postEscapeTextLen; ++bufferCopyIndex)
                    dynamicTextBuffer[processedTextLen++] = parsedTextBuffer[bufferCopyIndex];
                origConsumed += totalEscapeSeqLen;
                srcTextIndex = srcTextIndex + totalEscapeSeqLen - 1;
            }
            else
            {
                dynamicTextBuffer[processedTextLen++] = processedOriginalText[srcTextIndex];
            }
            hasEscapeSequence = 1;
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
    buttonIconIndex = 0;

    // ---- Pass 2: Text layout with wrapping ----
    for (charIndex = 0; charIndex < sourceTextLen && processedOriginalText[charIndex]; ++charIndex)
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
            if (extraGlyphs && (charIndex + 1) <= sourceTextLen) {
                bIsDBCharacter = TryDecodeDoubleByte((const char*)&processedOriginalText[charIndex], uiDoubleByteCode);
            }

            if (processedOriginalText[charIndex] == '\t')
            {
                currentLineWidth += 75 - currentLineWidth % 75;
                origConsumed += 1;
                continue;
            }

            if (!bIsDBCharacter) {
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
                    isTildeChar = 1;
                    tildeCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
                    currentLineWidth -= tildeCharWidth;
                    preSpaceWidth = currentLineWidth;
                    postSpaceWidth = currentLineWidth;
                }
            }
            else {
                origConsumed += 2;
                FontLetter* glyph = LookupDBGlyph(extraGlyphs, uiDoubleByteCode);
                if (glyph) {
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
                        isTildeChar = 0;
                        textBufferSize += 4;
                        dynamicTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Reallocate(dynamicTextBuffer, textBufferSize + 1));

                        UInt32 insertPos = AdjustWrapPositionForDB(lastWrapPosition, dynamicTextBuffer);

                        memmove(
                            &dynamicTextBuffer[insertPos + 1],
                            &dynamicTextBuffer[insertPos],
                            (processedTextLen - insertPos) + 1
                        );

                        dynamicTextBuffer[insertPos] = axData->cLineSep;
                        processedTextLen += 1;

                        AppendToListTail(&axData->xLineWidths, &currentLineWidth);
                        maxLineWidth = MaxInt(maxLineWidth, currentLineWidth);
                        lastWrapPosition = 0;
                        ++currentLineCount;

                        pCurrentGlyph = LookupLastCharGlyph(extraGlyphs, dynamicTextBuffer, processedTextLen, font->pFontData, uiTempDoubleByteCode, bLastIsDBCharacter);
                        currentLineWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);

                        if (bIsDBCharacter) {
                            FontLetter* glyph = LookupDBGlyph(extraGlyphs, uiDoubleByteCode);
                            if (glyph) pCurrentGlyph = glyph;
                        }
                        else {
                            pCurrentGlyph = &font->pFontData->pFontLetters[currentChar];
                        }

                        nextCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
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

                    if (processedTextLen >= 2) {
                        unsigned char lastChar = (unsigned char)dynamicTextBuffer[processedTextLen - 1];
                        if (extraGlyphs && IsLeadByte(lastChar)) {
                            tailStart = processedTextLen - 2;
                            tailBytes = processedTextLen - tailStart;
                        }
                    }

                    memmove(&dynamicTextBuffer[tailStart + 1],
                        &dynamicTextBuffer[tailStart],
                        tailBytes);

                    dynamicTextBuffer[tailStart] = axData->cLineSep;

                    processedTextLen += 1;
                    totalTextHeight += (font->pFontData->fBaseLine + lineSpacingAdjust);

                    AppendToListTail(&axData->xLineWidths, &currentLineWidth);
                    maxLineWidth = MaxInt(maxLineWidth, currentLineWidth);
                    lastWrapPosition = 0;
                    ++currentLineCount;

                    pCurrentGlyph = LookupLastCharGlyph(extraGlyphs, dynamicTextBuffer, processedTextLen, font->pFontData, uiTempDoubleByteCode, bLastIsDBCharacter);
                    currentLineWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);

                    if (bIsDBCharacter) {
                        FontLetter* glyph = LookupDBGlyph(extraGlyphs, uiDoubleByteCode);
                        if (glyph) pCurrentGlyph = glyph;
                    }
                    else {
                        pCurrentGlyph = &font->pFontData->pFontLetters[currentChar];
                    }

                    combinedCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
                    currentLineWidth += combinedCharWidth;
                }
            }

            if (bIsDBCharacter)
            {
                if (processedTextLen + 4 >= textBufferSize) {
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
                    if (processedTextLen + 1 >= textBufferSize) {
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
            while (processedTextLen > 0 && dynamicTextBuffer[processedTextLen - 1] != axData->cLineSep) {
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
        truncatedTextLen = 0;
        lineCounter = 0;
        for (truncateCharCounter = 0; truncateCharCounter < processedTextLen; ++truncateCharCounter)
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
void __thiscall FontEx::PrepTextForTerminal(const char* apOrigString, Font::TextData* axData) {
    PrepTextImpl(this, apOrigString, axData, true);
}

// ==================== FontEx::PrepText ====================
void __thiscall FontEx::PrepText(const char* apOrigString, Font::TextData* axData) {
    PrepTextImpl(this, apOrigString, axData, false);
}

// ==================== FontEx::CreateText ====================
UInt32 FontEx::CreateText(
    BSStringT<char>* axTextString, int* aiWidth, int* aiHeight,
    int aiLineStart, int aiLineEnd, int aiFlags, char aiLineBreakChar,
    const NiColorA* axFontColor, UINT32** apTextShape, UINT32** apIconShape) {
    double lineBaseOffset;
    UINT32* pIconShapeData;
    int lineWidthAtIdx;
    BSSimpleList<int>* pLineWidthNode2;
    int lineStartIdx;
    int lineStartIdx2;
    int lineWidthAtIdx2;
    BSSimpleList<int>* pLineWidthNode3;
    unsigned __int8 currentChar;
    int alignmentOffset;
    NiPoint3 textPosition;
    Font::TextData textData;
    int lineCounter;
    Font::TextData iconData;
    bool bIsDBCharacter, rendered;
    unsigned char cNextByte;
    int iActualCharCount;
    UInt32 uiDoubleByteCode;
    auto* extraGlyphs = GetExtraGlyphs(this->iFontNum);

    std::string sCurrentStr, sConvertedStr;

    if (!*aiHeight)
        *aiHeight = 0x7FFFFFFF;
    if (!aiLineEnd)
        aiLineEnd = 0x7FFFFFFF;
    *(float*)&iconData.iLineStart = FontManagerGetLinePadding(this->iFontNum);
    ThisStdCall(0x759330, &textData, *aiWidth, *aiHeight, aiLineStart, aiLineEnd, aiLineBreakChar);
    int iconVertIndex = 0;

    if (g_bEnableUTF8 && g_uiEncoding != 0 && extraGlyphs) {
        if (IsValidUTF8With3ByteMin(axTextString->pString)) {
            sCurrentStr = axTextString->pString;
            sConvertedStr = UTF8ToMultiByteStr(sCurrentStr, g_usingWinEncoding);
            axTextString->Set(sConvertedStr.c_str());
        }
    }

    ThisStdCall(0xA12FB0, this, axTextString->pString, &textData);

    *aiWidth = textData.iWidth;
    *aiHeight = textData.iHeight;
    lineCounter = 0;
    alignmentOffset = 0;
    if (aiFlags == 4)
    {
        alignmentOffset = -textData.xLineWidths.m_item;
    }
    else if (aiFlags == 2)
    {
        alignmentOffset = textData.xLineWidths.m_item / -2;
    }
    textPosition.x = (float)alignmentOffset;
    lineBaseOffset = this->pFontData->fBaseLine - this->fFontHeight;
    textPosition.z = lineBaseOffset + lineBaseOffset;
    textPosition.y = 0.0;

    iActualCharCount = textData.iCharCount;
    if (extraGlyphs) {
        for (int charIdx = 0;
            textData.xNewText.pString[textData.xNewText.pString != 0 ? charIdx : 0];
            ++charIdx) {
            bIsDBCharacter = false;

            cNextByte = (unsigned char)textData.xNewText.pString[charIdx + 1];
            if (cNextByte != 0) {
                bIsDBCharacter = TryDecodeDoubleByte((const char*)&textData.xNewText.pString[charIdx], uiDoubleByteCode);
            }

            if (bIsDBCharacter) {
                ++charIdx;
                iActualCharCount = iActualCharCount - 1;
            }
        }
    }

    *apTextShape = (UINT32*)Font::MakeTriShape(iActualCharCount, axFontColor, 1);
    *(float*)&iconData.xNewText.sLen = 0.0;
    *(float*)&iconData.iWidth = textPosition.y;
    *(float*)&iconData.iHeight = textPosition.z;
    UINT32* pTextShapeData = *apTextShape + 22;
    *(float*)pTextShapeData = 0.0;
    pTextShapeData[1] = iconData.iWidth;
    pTextShapeData[2] = iconData.iHeight;
    if (this->ButtonIcons.uiSize)
    {
        *apIconShape = (UINT32*)Font::MakeIconsTriShape();
        pIconShapeData = *apIconShape + 22;
        *pIconShapeData = *(DWORD*)&iconData.xNewText.sLen;
        pIconShapeData[1] = iconData.iWidth;
        pIconShapeData[2] = iconData.iHeight;
        ThisStdCall(0xA67050, (NiGeometryData*)(*apIconShape)[46], 0x4000);
    }
    float yOffsetStart = textPosition.x;
    iconData.iLineEnd = 0;
    iconData.xNewText.pString = 0;
    for (iconData.iCharCount = 0;
        textData.xNewText.pString[textData.xNewText.pString != 0 ? iconData.iCharCount : 0];
        ++iconData.iCharCount)
    {
        if (textData.xNewText.pString[textData.xNewText.pString != 0 ? iconData.iCharCount : 0] == aiLineBreakChar)
        {
            ++lineCounter;
            textPosition.x = 0.0;
            if (aiFlags == 4)
            {
                pLineWidthNode3 = &textData.xLineWidths;
                for (lineStartIdx2 = 0; lineStartIdx2 < lineCounter && pLineWidthNode3; ++lineStartIdx2)
                    pLineWidthNode3 = pLineWidthNode3->m_pkNext;
                if (pLineWidthNode3)
                    lineWidthAtIdx2 = pLineWidthNode3->m_item;
                else
                    lineWidthAtIdx2 = -1;
                textPosition.x = (float)-lineWidthAtIdx2;
            }
            else if (aiFlags == 2)
            {
                pLineWidthNode2 = &textData.xLineWidths;
                for (lineStartIdx = 0; lineStartIdx < lineCounter && pLineWidthNode2; ++lineStartIdx)
                    pLineWidthNode2 = pLineWidthNode2->m_pkNext;
                if (pLineWidthNode2)
                    lineWidthAtIdx = pLineWidthNode2->m_item;
                else
                    lineWidthAtIdx = -1;
                textPosition.x = (float)(lineWidthAtIdx / -2);
            }
            textPosition.z = textPosition.z - (this->pFontData->fBaseLine + *(float*)&iconData.iLineStart);
        }
        else if (textData.xNewText.pString[textData.xNewText.pString != 0 ? iconData.iCharCount : 0] == '\t')
        {
            float prevTabX = textPosition.x;
            AlignLineWidthToTab(textPosition.x, 75.0);
            textPosition.x = 75.0 - prevTabX + textPosition.x;
        }
        currentChar = textData.xNewText.pString[textData.xNewText.pString != 0 ? iconData.iCharCount : 0];

        bIsDBCharacter = false;

        if (extraGlyphs) {
            cNextByte = (unsigned char)textData.xNewText.pString[iconData.iCharCount + 1];
            if (cNextByte != 0) {
                bIsDBCharacter = TryDecodeDoubleByte((const char*)&textData.xNewText.pString[iconData.iCharCount], uiDoubleByteCode);
            }
            else {
                bIsDBCharacter = false;
            }
        }

        if (!bIsDBCharacter) {
            ConvertToAsciiQuotes(&currentChar);
        }

        rendered = false;

        if (currentChar == 1)
        {
            if (this->ButtonIcons.uiSize)
                Font::AddIcon((int)iconData.xNewText.pString++, (NiTriShape*)*apIconShape, &textPosition);
        }
        else
        {
            FontLetter* glyph = LookupDBGlyph(extraGlyphs, uiDoubleByteCode);
            if (extraGlyphs && bIsDBCharacter && glyph) {
                StdCall<FontLetter*>(
                    0xA142D0,
                    glyph,
                    iconData.iLineEnd++,
                    (NiTriShape*)*apTextShape,
                    &textPosition.x,
                    axFontColor);
                iconData.iCharCount += 1;
                rendered = true;
            }
            if (!rendered) {
                StdCall<FontLetter*>(
                    0xA142D0,
                    &this->pFontData->pFontLetters[currentChar],
                    iconData.iLineEnd++,
                    (NiTriShape*)*apTextShape,
                    &textPosition.x,
                    axFontColor);
            }
        }
        int maxRenderedWidth = *aiWidth;
        signed int renderedWidth = ConditionalFloatToUInt(textPosition.x - yOffsetStart);
        *aiWidth = MaxInt(renderedWidth, maxRenderedWidth);
    }
    *(DWORD*)&iconData.cLineSep = *(DWORD*)((*apTextShape)[46] + 32);
    ThisStdCall(
        0xA7EE30,
        (float*)((*apTextShape)[46] + 16),
        *(unsigned __int16*)((*apTextShape)[46] + 8),
        *(float**)&iconData.cLineSep);
    if (*apIconShape)
        ThisStdCall(
            0xA7EE30,
            (float*)((*apIconShape)[46] + 16),
            *(unsigned __int16*)((*apIconShape)[46] + 8),
            *(float**)((*apIconShape)[46] + 32));
    this->ButtonIcons.Clear(1);
    return ThisStdCall(0x7593E0, (char*)&textData);
}

// ==================== FontEx::MakeString ====================
UInt32* FontEx::MakeString(
    float afStartX, float afStartY, float afZ,
    BSStringT<char>* apTextString, int* aiWidth, bool abPrepareObject,
    const NiColorA* arg1C, bool abUpperLeftCorner, bool abPrepareObject_1) {

    double prevXPos;
    float prevXOffset;
    char currentCharValue;
    UINT32 stringLength;
    UINT32 textLen;
    int prevWidth;
    signed int renderedWidth;
    char escapeBuffer[4];
    float tabPrevX;
    unsigned __int8 currentChar;
    int charValue;
    UINT32 charIdx;
    float currentX;
    float currentZ;
    float currentY;
    UINT32* pTriShape;
    float lineBaseOffset;
    NiColorA* pColor;
    float defaultColor[4];
    char newlineBuffer[4];
    float startX;
    float startZ;
    float startY;
    float textXOffset;
    int vertexIdx;
    int lineIdx;
    float* pVertexData;

    bool bIsDBCharacter, rendered;
    unsigned char cNextByte;
    int iActualCharCount;
    UInt32 uiDoubleByteCode;
    auto* extraGlyphs = GetExtraGlyphs(this->iFontNum);

    std::string sCurrentStr, sConvertedStr;

    if (g_bEnableUTF8 && g_uiEncoding != 0 && extraGlyphs) {
        if (IsValidUTF8With3ByteMin(apTextString->pString)) {
            sCurrentStr = apTextString->pString;
            sConvertedStr = UTF8ToMultiByteStr(sCurrentStr, g_usingWinEncoding);
            apTextString->Set(sConvertedStr.c_str());
        }
    }

    if (apTextString->sLen == 0xFFFF)
        textLen = strlen(apTextString->pString);
    else
        textLen = apTextString->sLen;
    if (!textLen || !this->pFontData)
        return 0;
    textXOffset = (float)*aiWidth;
    ThisStdCall(0xA12370, (int*)this, apTextString->pString, &textXOffset, (int)newlineBuffer, abPrepareObject, 0);
    currentX = afStartX + textXOffset;
    currentY = afStartY;
    currentZ = afZ;

    if (abUpperLeftCorner)
    {
        lineBaseOffset = this->pFontData->pFontLetters[32].fHeight - this->pFontData->fBaseLine;
        currentY = currentY - (lineBaseOffset + lineBaseOffset);
    }

    for (charIdx = 0; ; ++charIdx)
    {
        stringLength = apTextString->sLen == 0xFFFF ? strlen(apTextString->pString) : apTextString->sLen;
        if (charIdx >= stringLength || !apTextString->pString[apTextString->pString != 0 ? charIdx : 0])
            break;
    }

    if (!charIdx)
        return 0;

    iActualCharCount = charIdx;

    if (extraGlyphs) {
        for (UINT32 scanIdx = 0;
            apTextString->pString[apTextString->pString != 0 ? scanIdx : 0];
            ++scanIdx) {
            bIsDBCharacter = false;

            if (scanIdx >= stringLength) {
                break;
            }

            cNextByte = (unsigned char)apTextString->pString[scanIdx + 1];
            if (cNextByte != 0) {
                bIsDBCharacter = TryDecodeDoubleByte((const char*)&apTextString->pString[scanIdx], uiDoubleByteCode);
            }
            if (bIsDBCharacter) {
                ++scanIdx;
                iActualCharCount = iActualCharCount - 1;
            }
        }
    }

    pTriShape = (UINT32*)Font::MakeTriShape(iActualCharCount, arg1C, abPrepareObject_1);
    startX = afStartX;
    startZ = currentZ;
    startY = currentY;
    *((float*)pTriShape + 22) = afStartX;
    *((float*)pTriShape + 23) = startZ;
    *((float*)pTriShape + 24) = startY;
    pColor = 0;
    defaultColor[0] = 0.0;
    defaultColor[1] = 0.0;
    defaultColor[2] = 1.0;
    defaultColor[3] = 1.0;
    *aiWidth = 0;
    lineBaseOffset = currentX;
    vertexIdx = 0;
    for (lineIdx = 0; apTextString->pString[apTextString->pString != 0 ? lineIdx : 0]; ++lineIdx)
    {
        if (apTextString->pString[apTextString->pString != 0 ? lineIdx : 0] == 3)
            pColor = 0;
        currentCharValue = apTextString->pString[apTextString->pString != 0 ? lineIdx : 0];
        if (currentCharValue == '\t')
        {
            prevXPos = currentX;
            AlignLineWidthToTab(currentX, 75.0);
            prevXOffset = prevXPos;
            currentX = 75.0 - prevXOffset + currentX;
        }
        else if (currentCharValue == '\n')
        {
            tabPrevX = (float)*aiWidth;
            ThisStdCall(0xA12370, (int*)this, apTextString->pString, &tabPrevX, (int)escapeBuffer, abPrepareObject, lineIdx + 1);
            currentX = tabPrevX;
            currentY = currentY - this->pFontData->fBaseLine;
        }

        currentChar = apTextString->pString[apTextString->pString != 0 ? lineIdx : 0];

        bIsDBCharacter = false;

        if (extraGlyphs) {
            cNextByte = (unsigned char)apTextString->pString[lineIdx + 1];
            if (cNextByte != 0) {
                bIsDBCharacter = TryDecodeDoubleByte((const char*)&apTextString->pString[lineIdx], uiDoubleByteCode);
            }
            else {
                bIsDBCharacter = false;
            }
        }

        if (!bIsDBCharacter) {
            ConvertToAsciiQuotes(&currentChar);
        }

        rendered = false;

        charValue = currentChar;

        if (extraGlyphs && bIsDBCharacter) {
            FontLetter* glyph = LookupDBGlyph(extraGlyphs, uiDoubleByteCode);
            if (glyph) {
                StdCall<FontLetter*>(
                    0xA142D0,
                    glyph,
                    vertexIdx++,
                    (NiTriShape*)pTriShape,
                    &currentX,
                    pColor);
                lineIdx += 1;
                rendered = true;
            }
        }
        if (!rendered) {
            StdCall<FontLetter*>(
                0xA142D0,
                &this->pFontData->pFontLetters[currentChar],
                vertexIdx++,
                (NiTriShape*)pTriShape,
                &currentX,
                pColor);
        }

        prevWidth = *aiWidth;
        renderedWidth = ConditionalFloatToUInt(currentX - lineBaseOffset);
        *aiWidth = MaxInt(renderedWidth, prevWidth);
        if (apTextString->pString[apTextString->pString != 0 ? lineIdx : 0] == 2)
            pColor = (NiColorA*)defaultColor;
    }
    pVertexData = *(float**)(pTriShape[46] + 32);
    ThisStdCall(
        0xA7EE30,
        (float*)(pTriShape[46] + 16),
        *(unsigned __int16*)(pTriShape[46] + 8), pVertexData);
    return (UInt32*)pTriShape;
}

} // namespace fonthook

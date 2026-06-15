#include "font_engine.h"
#include "native_calls.h"

namespace fonthook {

// ==================== FontEx::FontInit ====================
Font* FontEx::FontInit(int iFontNum, char* apFilename, bool abLoad)
{
    UINT32 v6; // [esp+28h] [ebp-14h]
    int v7; // [esp+2Ch] [ebp-10h]

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

    v7 = *(DWORD*)targetAddress;
    *(DWORD*)targetAddress = 12;

    this->pFontFile = 0;
    this->iRefCount = 0;
    this->pFontData = 0;

    if (apFilename)
    {
        v6 = strlen(apFilename);
        if (v6)
        {
            this->pFontFile = (char*)MemoryManager_s_Instance->Allocate(v6 + 1);
            strcpy_s(this->pFontFile, v6 + 1, apFilename);
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

    *(DWORD*)targetAddress = v7;
    return this;
}

// ==================== FontEx::Load ====================
void FontEx::Load()
{
    NiTexturingProperty* NiTexturingProperty_4; // [esp+10h] [ebp-23Ch]
    NiPixelData* NiPixelData_1; // [esp+18h] [ebp-234h]
    float fMaxDrop_1; // [esp+20h] [ebp-22Ch]
    float fHeight_1; // [esp+24h] [ebp-228h]
    float fFontHeight_1; // [esp+28h] [ebp-224h]
    char* pFontFile_1; // [esp+58h] [ebp-1F4h]
    void* v8; // [esp+60h] [ebp-1ECh]
    unsigned int v9; // [esp+64h] [ebp-1E8h]
    int v10; // [esp+6Ch] [ebp-1E0h] BYREF
    unsigned int v11; // [esp+70h] [ebp-1DCh]
    unsigned int v12; // [esp+74h] [ebp-1D8h]
    DWORD v13[2]; // [esp+78h] [ebp-1D4h] BYREF
    bool m_pfnWrite; // [esp+83h] [ebp-1C9h]
    int savedTlsValueTemp; // [esp+84h] [ebp-1C8h]
    char* pFontFile; // [esp+88h] [ebp-1C4h]
    float fMaxDrop; // [esp+8Ch] [ebp-1C0h]
    float fMaxDrop_2; // [esp+90h] [ebp-1BCh]
    float fHeight; // [esp+94h] [ebp-1B8h]
    float fFontHeight; // [esp+98h] [ebp-1B4h]
    unsigned int v21; // [esp+9Ch] [ebp-1B0h]
    FontData* pFontData_1; // [esp+A0h] [ebp-1ACh]
    unsigned int v23; // [esp+A4h] [ebp-1A8h]
    unsigned int v24; // [esp+A8h] [ebp-1A4h]
    DWORD v25[2]; // [esp+ACh] [ebp-1A0h] BYREF
    char* pFontFile_2; // [esp+B4h] [ebp-198h]
    unsigned __int16 iRefCount; // [esp+BAh] [ebp-192h]
    int oldTlsValue; // [esp+BCh] [ebp-190h]
    int stringRefFlag; // [esp+C0h] [ebp-18Ch]
    BSFile* NiBinaryStream_3; // [esp+C4h] [ebp-188h]
    BSFile* NiBinaryStream_4; // [esp+C8h] [ebp-184h]
    NiFixedString kName_; // [esp+CCh] [ebp-180h] BYREF
    NiTexturingProperty* NiTexturingProperty_3; // [esp+D0h] [ebp-17Ch]
    NiTexturingProperty* NiTexturingProperty_1; // [esp+D4h] [ebp-178h]
    NiPixelData* v36; // [esp+D8h] [ebp-174h]
    NiPixelData* NiPixelData_2; // [esp+DCh] [ebp-170h]
    BSFile* NiBinaryStream_1; // [esp+E0h] [ebp-16Ch]
    BSFile* NiBinaryStream_2; // [esp+E4h] [ebp-168h]
    BSFile* NiFile_1; // [esp+E8h] [ebp-164h]
    BSFile* BSFile_2; // [esp+ECh] [ebp-160h]
    FontData* pFontData; // [esp+F0h] [ebp-15Ch]
    BSFile* BSFile_3; // [esp+F4h] [ebp-158h]
    BSFile* BSFile_4; // [esp+F8h] [ebp-154h]
    unsigned int a2; // [esp+FCh] [ebp-150h] BYREF
    unsigned int a3; // [esp+100h] [ebp-14Ch]
    NiTexture::FormatPrefs arPrefs_; // [esp+104h] [ebp-148h] BYREF
    BSFile* NiBinaryStream_0; // [esp+110h] [ebp-13Ch]
    NiTexturingProperty* NiTexturingProperty_2; // [esp+114h] [ebp-138h]
    NiPixelData* NiPixelData_3; // [esp+118h] [ebp-134h]
    int iTextureCount; // [esp+11Ch] [ebp-130h]
    float fFontHeight_2; // [esp+120h] [ebp-12Ch]
    int n256; // [esp+124h] [ebp-128h]
    char apName_[260]; // [esp+128h] [ebp-124h] BYREF
    float fWidth; // [esp+230h] [ebp-1Ch]
    BSFile* BSFile_1; // [esp+234h] [ebp-18h]
    float fHeight_2; // [esp+238h] [ebp-14h]
    int savedTlsValue; // [esp+23Ch] [ebp-10h]
    int stackCookie; // [esp+248h] [ebp-4h]

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

    oldTlsValue = *(DWORD*)targetAddress;
    savedTlsValue = oldTlsValue;
    *(DWORD*)targetAddress = 12;

    stackCookie = 0;
    iRefCount = this->iRefCount;
    if (iRefCount || !this->pFontFile)
    {
    LABEL_46:
        ++this->iRefCount;
        stackCookie = -1;
        *(DWORD*)targetAddress = savedTlsValue;
        return;
    }
    BSFile_1 = FileFinder_GetFile(this->pFontFile, (NiFile::OpenMode)0, 0x150000u, 2u);
    if (BSFile_1)
    {
        if (LOBYTE(BSFile_1->m_pFile))
        {
            pFontData = (FontData*)MemoryManager_s_Instance->Allocate(0x3928u);
            this->pFontData = pFontData;
            v21 = 0x3928u;
            pFontData_1 = this->pFontData;
            v25[0] = 1;
            v23 = BSFile_1->m_pfnRead(BSFile_1, pFontData_1, v21, v25, 1u);
            BSFile_1->m_uiAbsoluteCurrentPos += v23;
            v24 = v23;

            if (g_uiEncoding != 0) {
                unsigned int uiActualSize = BSFile_1->GetSize();
                if (uiActualSize > 0x3928) {
                    fontNameKey = this->pFontFile ? this->pFontFile : 0;
                    if (!fontNameKey.empty()) {
                        auto& extraMap = gExtraFontLetters[fontNameKey];
                        if (extraMap.empty()) {
                            extraMap.reserve(24066);
                            unsigned int hh, ll, code;
                            for (hh = 0x81; hh <= 0xFE; ++hh) {
                                for (ll = 0x40; ll <= 0xFE; ++ll) {
                                    code = (hh << 8) | ll;
                                    FontLetter letter{};
                                    UInt32 r2 = BSFile_1->m_pfnRead(BSFile_1, &letter, sizeof(letter), v25, 1u);
                                    BSFile_1->m_uiAbsoluteCurrentPos += r2;
                                    if (r2 != sizeof(letter)) break;
                                    extraMap[code] = letter;
                                }
                            }
                        }
                    }
                }
            }

            NiFile_1 = BSFile_1;
            BSFile_2 = BSFile_1;
            if (BSFile_1) {
                delete(BSFile_2);
            }
            BSFile_1 = 0;
            this->fFontHeight = 0.0;
            fHeight_2 = 0.0;
            this->fMaxDrop = 0.0;
            for (n256 = 0; n256 < 256; ++n256)
            {
                fFontHeight_2 = this->pFontData->fBaseLine - this->pFontData->pFontLetters[n256].fTopEdge;
                fFontHeight_2 = fFontHeight_2 + this->pFontData->pFontLetters[n256].fHeight;
                fFontHeight = this->fFontHeight;
                if (fFontHeight_2 >= (double)fFontHeight)
                    fFontHeight_1 = fFontHeight_2;
                else
                    fFontHeight_1 = fFontHeight;
                this->fFontHeight = fFontHeight_1;
                fHeight = this->pFontData->pFontLetters[n256].fHeight;
                if (fHeight >= (double)fHeight_2)
                    fHeight_1 = fHeight;
                else
                    fHeight_1 = fHeight_2;
                fHeight_2 = fHeight_1;
                fMaxDrop = this->fMaxDrop;
                fMaxDrop_2 = this->pFontData->pFontLetters[n256].fTopEdge - this->pFontData->pFontLetters[n256].fHeight;
                if (fMaxDrop_2 <= (double)fMaxDrop)
                    fMaxDrop_1 = fMaxDrop_2;
                else
                    fMaxDrop_1 = fMaxDrop;
                this->fMaxDrop = fMaxDrop_1;
            }
            fWidth = this->pFontData->pFontLetters[32].fWidth;
            this->pFontData->pFontLetters[32].fWidth = this->pFontData->pFontLetters[32].fSpacing;
            this->pFontData->pFontLetters[32].fSpacing = fWidth;
            this->pFontData->pFontLetters[32].fHeight = fHeight_2;
            this->pFontData->pFontLetters[32].fTopEdge = this->fMaxDrop + fHeight_2;
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
            this->pFontData->pFontLetters[0].fHeight = fHeight_2;
            this->pFontData->pFontLetters[0].fTopEdge = this->fMaxDrop + fHeight_2;
            this->pFontData->pFontLetters[0].pMapping[0].fU = 0.0;
            this->pFontData->pFontLetters[0].pMapping[1].fU = 0.0;
            this->pFontData->pFontLetters[0].pMapping[2].fU = 0.0;
            this->pFontData->pFontLetters[0].pMapping[3].fU = 0.0;
            this->pFontData->pFontLetters[0].pMapping[0].fV = 0.0;
            this->pFontData->pFontLetters[0].pMapping[1].fV = 0.0;
            this->pFontData->pFontLetters[0].pMapping[2].fV = 0.0;
            this->pFontData->pFontLetters[0].pMapping[3].fV = 0.0;
            if (this->pFontData->iTextureCount > 8)
            {
                pFontFile = this->pFontFile;
                stackCookie = -1;
                savedTlsValueTemp = savedTlsValue;
                *(DWORD*)targetAddress = savedTlsValue;
                return;
            }
            for (iTextureCount = 0; iTextureCount < this->pFontData->iTextureCount; ++iTextureCount)
            {
                _snprintf_s(
                    apName_,
                    0x100u,
                    _TRUNCATE,
                    "TEXTURES\\FONTS\\%s.TEX",
                    this->pFontData->pTextureFiles[iTextureCount].pFilename
                );

                NiBinaryStream_0 = FileFinder_GetFile((const char*)apName_, (NiFile::OpenMode)0, 0x5000000u, 2u);

                if (!NiBinaryStream_0 || !(m_pfnWrite = (bool)NiBinaryStream_0->m_pFile))
                {
                    if (NiBinaryStream_0)
                    {
                        NiBinaryStream_1 = NiBinaryStream_0;
                        NiBinaryStream_2 = NiBinaryStream_0;
                        delete(NiBinaryStream_2);
                    }
                    stackCookie = -1;
                    v13[1] = savedTlsValue;
                    *(DWORD*)targetAddress = savedTlsValue;
                    return;
                }

                v13[0] = 1;
                v11 = NiBinaryStream_0->m_pfnRead(NiBinaryStream_0, &a2, 4u, v13, 1u);
                v11 += NiBinaryStream_0->m_pfnRead(NiBinaryStream_0, &a3, 4u, v13, 1u);
                NiBinaryStream_0->m_uiAbsoluteCurrentPos += v11;
                v12 = v11;
                arPrefs_.m_ePixelLayout = static_cast<NiTexture::FormatPrefs::PixelLayout>(0x6);
                arPrefs_.m_eAlphaFmt = static_cast<NiTexture::FormatPrefs::AlphaFormat>(0x3);
                arPrefs_.m_eMipMapped = static_cast<NiTexture::FormatPrefs::MipFlag>(0x2);
                v36 = (NiPixelData*)NiMemObject::operator new(sizeof(NiPixelData));
                stackCookie = (stackCookie & 0xFFFFFF00) | 1;
                if (v36) {
                    NiPixelData_1 = ThisStdCall<NiPixelData*>(
                        0xA7C190,
                        v36,
                        a2,
                        a3,
                        reinterpret_cast<const NiPixelFormat*>(0x11AA2A0),
                        1,
                        1);
                }
                else
                    NiPixelData_1 = 0;
                NiPixelData_2 = NiPixelData_1;
                stackCookie = (stackCookie & 0xFFFFFF00) | 0;
                NiPixelData_3 = NiPixelData_1;
                v8 = &NiPixelData_1->m_pucPixels[*NiPixelData_1->m_puiOffsetInBytes];
                v10 = 1;
                v9 = NiBinaryStream_0->m_pfnRead(NiBinaryStream_0, v8, 4 * a3 * a2, (UInt32*)&v10, 1u);
                NiBinaryStream_0->m_uiAbsoluteCurrentPos += v9;
                NiPixelData_3->bNoConvert = 1;
                NiTexturingProperty_3 = (NiTexturingProperty*)NiMemObject::operator new(sizeof(NiTexturingProperty));
                stackCookie = (stackCookie & 0xFFFFFF00) | 2;
                if (NiTexturingProperty_3)
                {
                    pFontFile_1 = this->pFontFile;
                    if (pFontFile_1)
                        kName_.m_kHandle = (char*)NiGlobalStringTable::AddString(pFontFile_1);
                    else
                        kName_.m_kHandle = 0;
                    stackCookie = (stackCookie & 0xFFFFFF00) | 3;
                    stringRefFlag |= 1u;
                    NiTexturingProperty_4 = ThisStdCall<NiTexturingProperty*>(
                        0xA6ABB0,
                        NiTexturingProperty_3,
                        NiPixelData_3,
                        &kName_,
                        &arPrefs_);
                }
                else
                {
                    NiTexturingProperty_4 = 0;
                }
                NiTexturingProperty_1 = NiTexturingProperty_4;
                NiTexturingProperty_2 = NiTexturingProperty_4;
                stackCookie = 0;
                if ((stringRefFlag & 1) != 0)
                {
                    stringRefFlag &= ~1u;
                    if (kName_.m_kHandle)
                        InterlockedDecrement((volatile LONG*)kName_.m_kHandle - 2);
                }
                NiBinaryStream_3 = NiBinaryStream_0;
                NiBinaryStream_4 = NiBinaryStream_0;
                if (NiBinaryStream_0) {
                    delete(NiBinaryStream_4);
                }
                NiBinaryStream_0 = 0;
                ThisStdCall(0x60AEB0, NiTexturingProperty_2, 1);
                ThisStdCall(0x66B0D0, &this->pTextureData[iTextureCount].m_pObject, (int)NiTexturingProperty_2);
            }
            goto LABEL_46;
        }
    }
    pFontFile_2 = this->pFontFile;
    if (BSFile_1)
    {
        BSFile_3 = BSFile_1;
        BSFile_4 = BSFile_1;
        delete(BSFile_4);
    }
    stackCookie = -1;
    v25[1] = savedTlsValue;
    *(DWORD*)targetAddress = savedTlsValue;
}

// ==================== FontEx::PrepTextForTerminal ====================
void __thiscall FontEx::PrepTextForTerminal(const char* apOrigString, Font::TextData* axData) {
    unsigned int charWidthWithKerning;
    unsigned int tildeCharWidth;
    unsigned int nextCharWidth;
    unsigned int combinedCharWidth;
    int finalMaxLineWidth;
    int tempLineWidthComp1;
    int tempLineWidthComp2;
    int tempLineWidthComp3;
    int tempLineWidthComp4;
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
    auto extraGlyphEntry = gNumberedExtraLetters.find(this->iFontNum);
    auto* extraGlyphs = extraGlyphEntry != gNumberedExtraLetters.end() ? &extraGlyphEntry->second : nullptr;

    if (!apOrigString)
        return;

    if (axData->iWidth <= 0)
        axData->iWidth = 0x7FFFFFFF;
    if (axData->iHeight <= 0)
        axData->iHeight = 0x7FFFFFFF;
    if (axData->iLineEnd <= 0)
        axData->iLineEnd = 0x7FFFFFFF;

    lineSpacingAdjust = FontManagerGetLinePadding(this->iFontNum);
    lastWrapPosition = 0;
    preSpaceWidth = 0;
    postSpaceWidth = 0;
    currentLineWidth = 0;
    maxLineWidth = 0;
    totalTextHeight = this->pFontData->pFontLetters[' '].fHeight;
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
                    if (this->iFontNum == 7)
                    {
                        strcpy_s(textureNameBuffer, 0x104u, substrBuffer);
                        sprintf_s(substrBuffer, 0x104u, "glow_%s", textureNameBuffer);
                    }
                    this->AddTextIcon(substrBuffer);
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
            totalTextHeight = this->pFontData->fBaseLine + lineSpacingAdjust + totalTextHeight;
            AppendToListTail(&axData->xLineWidths, &currentLineWidth);
            if (maxLineWidth <= currentLineWidth)
                tempLineWidthComp4 = currentLineWidth;
            else
                tempLineWidthComp4 = maxLineWidth;
            maxLineWidth = tempLineWidthComp4;
            currentLineWidth = 0;
            lastWrapPosition = 0;
            ++currentLineCount;
        }
        else
        {
            bIsDBCharacter = false;
            if (extraGlyphs) {
                if ((charIndex + 1) <= sourceTextLen) {
                    if (g_usingWinEncoding == 936) {
                        bIsDBCharacter = TryDecodeGBK((const char*)&processedOriginalText[charIndex], uiDoubleByteCode);
                    }
                    else if (g_usingWinEncoding == 950) {
                        bIsDBCharacter = TryDecodeBig5((const char*)&processedOriginalText[charIndex], uiDoubleByteCode);
                    }
                    else if (g_usingWinEncoding == 932) {
                        bIsDBCharacter = TryDecodeSJIS((const char*)&processedOriginalText[charIndex], uiDoubleByteCode);
                    }
                    else if (g_usingWinEncoding == 949) {
                        bIsDBCharacter = TryDecodeKorean((const char*)&processedOriginalText[charIndex], uiDoubleByteCode);
                    }
                }
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
                pCurrentGlyph = &this->pFontData->pFontLetters[currentChar];
                if (currentChar == 1)
                {
                    if (buttonIconIndex < this->ButtonIcons.uiSize)
                    {
                        pCurrentGlyph->fWidth = this->ButtonIcons.pBuffer[buttonIconIndex].fWidth;
                        pCurrentGlyph->fSpacing = this->ButtonIcons.pBuffer[buttonIconIndex].fSpacing;
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
                auto glyphIt = extraGlyphs->find(uiDoubleByteCode);
                if (glyphIt != extraGlyphs->end()) {
                    pCurrentGlyph = &glyphIt->second;
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

                        UInt32 insertPos = lastWrapPosition;

                        if (insertPos > 0) {
                            unsigned char prevByte = (unsigned char)dynamicTextBuffer[insertPos - 1];
                            if (g_usingWinEncoding == 936) {
                                if (IsGBKLeadByte(prevByte)) { insertPos -= 1; }
                            }
                            else if (g_usingWinEncoding == 950) {
                                if (IsBig5LeadByte(prevByte)) { insertPos -= 1; }
                            }
                            else if (g_usingWinEncoding == 932) {
                                if (IsSJISLeadByte(prevByte)) { insertPos -= 1; }
                            }
                            else if (g_usingWinEncoding == 949) {
                                if (IsKoreanLeadByte(prevByte)) { insertPos -= 1; }
                            }
                        }

                        memmove(
                            &dynamicTextBuffer[insertPos + 1],
                            &dynamicTextBuffer[insertPos],
                            (processedTextLen - insertPos) + 1
                        );

                        dynamicTextBuffer[insertPos] = axData->cLineSep;
                        processedTextLen += 1;

                        AppendToListTail(&axData->xLineWidths, &currentLineWidth);
                        if (maxLineWidth <= currentLineWidth)
                            tempLineWidthComp3 = currentLineWidth;
                        else
                            tempLineWidthComp3 = maxLineWidth;
                        maxLineWidth = tempLineWidthComp3;
                        lastWrapPosition = 0;
                        ++currentLineCount;

                        bLastIsDBCharacter = false;
                        if (extraGlyphs) {
                            if (g_usingWinEncoding == 936) {
                                if (IsGBKTrailByte((dynamicTextBuffer[processedTextLen - 1]))) {
                                    if (TryDecodeGBK(&dynamicTextBuffer[processedTextLen - 2], uiTempDoubleByteCode)) {
                                        auto glyphIt2 = extraGlyphs->find(uiTempDoubleByteCode);
                                        if (glyphIt2 != extraGlyphs->end()) {
                                            pCurrentGlyph = &glyphIt2->second;
                                            bLastIsDBCharacter = true;
                                        }
                                    }
                                }
                            }
                            else if (g_usingWinEncoding == 950) {
                                if (IsBig5TrailByte((dynamicTextBuffer[processedTextLen - 1]))) {
                                    if (TryDecodeBig5(&dynamicTextBuffer[processedTextLen - 2], uiTempDoubleByteCode)) {
                                        auto glyphIt2 = extraGlyphs->find(uiTempDoubleByteCode);
                                        if (glyphIt2 != extraGlyphs->end()) {
                                            pCurrentGlyph = &glyphIt2->second;
                                            bLastIsDBCharacter = true;
                                        }
                                    }
                                }
                            }
                            else if (g_usingWinEncoding == 932) {
                                if (IsSJISTrailByte((dynamicTextBuffer[processedTextLen - 1]))) {
                                    if (TryDecodeSJIS(&dynamicTextBuffer[processedTextLen - 2], uiTempDoubleByteCode)) {
                                        auto glyphIt2 = extraGlyphs->find(uiTempDoubleByteCode);
                                        if (glyphIt2 != extraGlyphs->end()) {
                                            pCurrentGlyph = &glyphIt2->second;
                                            bLastIsDBCharacter = true;
                                        }
                                    }
                                }
                            }
                            else if (g_usingWinEncoding == 949) {
                                if (IsKoreanTrailByte((dynamicTextBuffer[processedTextLen - 1]))) {
                                    if (TryDecodeKorean(&dynamicTextBuffer[processedTextLen - 2], uiTempDoubleByteCode)) {
                                        auto glyphIt2 = extraGlyphs->find(uiTempDoubleByteCode);
                                        if (glyphIt2 != extraGlyphs->end()) {
                                            pCurrentGlyph = &glyphIt2->second;
                                            bLastIsDBCharacter = true;
                                        }
                                    }
                                }
                            }
                        }

                        if (!bLastIsDBCharacter) {
                            pCurrentGlyph = &this->pFontData->pFontLetters[dynamicTextBuffer[processedTextLen - 1]];
                        }
                        currentLineWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);

                        if (bIsDBCharacter) {
                            auto glyphIt3 = extraGlyphs->find(uiDoubleByteCode);
                            if (glyphIt3 != extraGlyphs->end()) {
                                pCurrentGlyph = &glyphIt3->second;
                            }
                        }
                        else {
                            pCurrentGlyph = &this->pFontData->pFontLetters[currentChar];
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
                        totalTextHeight = this->pFontData->fBaseLine + lineSpacingAdjust + totalTextHeight;
                        AppendToListTail(&axData->xLineWidths, &preSpaceWidth);
                        if (maxLineWidth <= preSpaceWidth)
                            tempLineWidthComp2 = preSpaceWidth;
                        else
                            tempLineWidthComp2 = maxLineWidth;
                        maxLineWidth = tempLineWidthComp2;
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
                        unsigned char cLastChar = (unsigned char)dynamicTextBuffer[processedTextLen - 1];

                        if (extraGlyphs) {
                            if (g_usingWinEncoding == 936) {
                                if (IsGBKLeadByte(cLastChar)) { tailStart = processedTextLen - 2; tailBytes = processedTextLen - tailStart; }
                            }
                            else if (g_usingWinEncoding == 950) {
                                if (IsBig5LeadByte(cLastChar)) { tailStart = processedTextLen - 2; tailBytes = processedTextLen - tailStart; }
                            }
                            else if (g_usingWinEncoding == 932) {
                                if (IsSJISLeadByte(cLastChar)) { tailStart = processedTextLen - 2; tailBytes = processedTextLen - tailStart; }
                            }
                            else if (g_usingWinEncoding == 949) {
                                if (IsKoreanLeadByte(cLastChar)) { tailStart = processedTextLen - 2; tailBytes = processedTextLen - tailStart; }
                            }
                        }
                    }

                    memmove(&dynamicTextBuffer[tailStart + 1],
                        &dynamicTextBuffer[tailStart],
                        tailBytes);

                    dynamicTextBuffer[tailStart] = axData->cLineSep;

                    processedTextLen += 1;
                    totalTextHeight += (this->pFontData->fBaseLine + lineSpacingAdjust);

                    AppendToListTail(&axData->xLineWidths, &currentLineWidth);
                    if (maxLineWidth <= currentLineWidth)
                        tempLineWidthComp1 = currentLineWidth;
                    else
                        tempLineWidthComp1 = maxLineWidth;
                    maxLineWidth = tempLineWidthComp1;
                    lastWrapPosition = 0;
                    ++currentLineCount;

                    bLastIsDBCharacter = false;
                    if (extraGlyphs) {
                        if (g_usingWinEncoding == 936) {
                            if (IsGBKTrailByte(dynamicTextBuffer[processedTextLen - 1])) {
                                if (TryDecodeGBK((const char*)&dynamicTextBuffer[processedTextLen - 2], uiTempDoubleByteCode)) {
                                    auto glyphIt2 = extraGlyphs->find(uiTempDoubleByteCode);
                                    if (glyphIt2 != extraGlyphs->end()) { pCurrentGlyph = &glyphIt2->second; bLastIsDBCharacter = true; }
                                }
                            }
                        }
                        else if (g_usingWinEncoding == 950) {
                            if (IsBig5TrailByte(dynamicTextBuffer[processedTextLen - 1])) {
                                if (TryDecodeBig5((const char*)&dynamicTextBuffer[processedTextLen - 2], uiTempDoubleByteCode)) {
                                    auto glyphIt2 = extraGlyphs->find(uiTempDoubleByteCode);
                                    if (glyphIt2 != extraGlyphs->end()) { pCurrentGlyph = &glyphIt2->second; bLastIsDBCharacter = true; }
                                }
                            }
                        }
                        else if (g_usingWinEncoding == 932) {
                            if (IsSJISTrailByte(dynamicTextBuffer[processedTextLen - 1])) {
                                if (TryDecodeSJIS((const char*)&dynamicTextBuffer[processedTextLen - 2], uiTempDoubleByteCode)) {
                                    auto glyphIt2 = extraGlyphs->find(uiTempDoubleByteCode);
                                    if (glyphIt2 != extraGlyphs->end()) { pCurrentGlyph = &glyphIt2->second; bLastIsDBCharacter = true; }
                                }
                            }
                        }
                        else if (g_usingWinEncoding == 949) {
                            if (IsKoreanTrailByte(dynamicTextBuffer[processedTextLen - 1])) {
                                if (TryDecodeKorean((const char*)&dynamicTextBuffer[processedTextLen - 2], uiTempDoubleByteCode)) {
                                    auto glyphIt2 = extraGlyphs->find(uiTempDoubleByteCode);
                                    if (glyphIt2 != extraGlyphs->end()) { pCurrentGlyph = &glyphIt2->second; bLastIsDBCharacter = true; }
                                }
                            }
                        }
                    }

                    if (!bLastIsDBCharacter) {
                        pCurrentGlyph = &this->pFontData->pFontLetters[dynamicTextBuffer[processedTextLen - 1]];
                    }

                    currentLineWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);

                    if (bIsDBCharacter) {
                        auto glyphIt3 = extraGlyphs->find(uiDoubleByteCode);
                        if (glyphIt3 != extraGlyphs->end()) { pCurrentGlyph = &glyphIt3->second; }
                    }
                    else {
                        pCurrentGlyph = &this->pFontData->pFontLetters[currentChar];
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
            totalTextHeight = totalTextHeight - (this->pFontData->fBaseLine + lineSpacingAdjust);
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
        totalTextHeight = this->pFontData->pFontLetters[' '].fHeight;
        currentLineWidth = ConditionalFloatToUInt(this->pFontData->pFontLetters[' '].fWidth);
    }
    AppendToListTail(&axData->xLineWidths, &currentLineWidth);
    if (maxLineWidth <= currentLineWidth)
        finalMaxLineWidth = currentLineWidth;
    else
        finalMaxLineWidth = maxLineWidth;
    maxLineWidth = finalMaxLineWidth;
    dynamicTextBuffer[processedTextLen] = 0;
    axData->xNewText.Set(dynamicTextBuffer, 0);
    axData->iWidth = maxLineWidth;
    axData->iHeight = totalTextHeight;
    axData->iLineStart = 0;
    axData->iLineEnd = currentLineCount;
    axData->iCharCount = origConsumed;
    MemoryManager_s_Instance->Deallocate(processedOriginalText);
    MemoryManager_s_Instance->Deallocate(dynamicTextBuffer);
}

// ==================== FontEx::PrepText ====================
void __thiscall FontEx::PrepText(const char* apOrigString, Font::TextData* axData) {
    unsigned int charWidthWithKerning;
    unsigned int tildeCharWidth;
    unsigned int nextCharWidth;
    unsigned int combinedCharWidth;
    int finalMaxLineWidth;
    int tempLineWidthComp1;
    int tempLineWidthComp2;
    int tempLineWidthComp3;
    int tempLineWidthComp4;
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
    auto extraGlyphEntry = gNumberedExtraLetters.find(this->iFontNum);
    auto* extraGlyphs = extraGlyphEntry != gNumberedExtraLetters.end() ? &extraGlyphEntry->second : nullptr;

    if (!apOrigString)
        return;

    if (axData->iWidth <= 0)
        axData->iWidth = 0x7FFFFFFF;
    if (axData->iHeight <= 0)
        axData->iHeight = 0x7FFFFFFF;
    if (axData->iLineEnd <= 0)
        axData->iLineEnd = 0x7FFFFFFF;

    lineSpacingAdjust = FontManagerGetLinePadding(this->iFontNum);
    lastWrapPosition = 0;
    preSpaceWidth = 0;
    postSpaceWidth = 0;
    currentLineWidth = 0;
    maxLineWidth = 0;
    totalTextHeight = this->pFontData->pFontLetters[' '].fHeight;
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
                    if (this->iFontNum == 7)
                    {
                        strcpy_s(textureNameBuffer, 0x104u, substrBuffer);
                        sprintf_s(substrBuffer, 0x104u, "glow_%s", textureNameBuffer);
                    }
                    this->AddTextIcon(substrBuffer);
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
            totalTextHeight = this->pFontData->fBaseLine + lineSpacingAdjust + totalTextHeight;
            AppendToListTail(&axData->xLineWidths, &currentLineWidth);
            if (maxLineWidth <= currentLineWidth)
                tempLineWidthComp4 = currentLineWidth;
            else
                tempLineWidthComp4 = maxLineWidth;
            maxLineWidth = tempLineWidthComp4;
            currentLineWidth = 0;
            lastWrapPosition = 0;
            ++currentLineCount;
        }
        else
        {
            bIsDBCharacter = false;
            if (extraGlyphs) {
                if ((charIndex + 1) <= sourceTextLen) {
                    if (g_usingWinEncoding == 936) {
                        bIsDBCharacter = TryDecodeGBK((const char*)&processedOriginalText[charIndex], uiDoubleByteCode);
                    }
                    else if (g_usingWinEncoding == 950) {
                        bIsDBCharacter = TryDecodeBig5((const char*)&processedOriginalText[charIndex], uiDoubleByteCode);
                    }
                    else if (g_usingWinEncoding == 932) {
                        bIsDBCharacter = TryDecodeSJIS((const char*)&processedOriginalText[charIndex], uiDoubleByteCode);
                    }
                    else if (g_usingWinEncoding == 949) {
                        bIsDBCharacter = TryDecodeKorean((const char*)&processedOriginalText[charIndex], uiDoubleByteCode);
                    }
                }
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
                pCurrentGlyph = &this->pFontData->pFontLetters[currentChar];
                if (currentChar == 1)
                {
                    if (buttonIconIndex < this->ButtonIcons.uiSize)
                    {
                        pCurrentGlyph->fWidth = this->ButtonIcons.pBuffer[buttonIconIndex].fWidth;
                        pCurrentGlyph->fSpacing = this->ButtonIcons.pBuffer[buttonIconIndex].fSpacing;
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
                auto glyphIt = extraGlyphs->find(uiDoubleByteCode);
                if (glyphIt != extraGlyphs->end()) {
                    pCurrentGlyph = &glyphIt->second;
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

                        UInt32 insertPos = lastWrapPosition;

                        if (insertPos > 0) {
                            unsigned char prevByte = (unsigned char)dynamicTextBuffer[insertPos - 1];
                            if (g_usingWinEncoding == 936) {
                                if (IsGBKLeadByte(prevByte)) { insertPos -= 1; }
                            }
                            else if (g_usingWinEncoding == 950) {
                                if (IsBig5LeadByte(prevByte)) { insertPos -= 1; }
                            }
                            else if (g_usingWinEncoding == 932) {
                                if (IsSJISLeadByte(prevByte)) { insertPos -= 1; }
                            }
                            else if (g_usingWinEncoding == 949) {
                                if (IsKoreanLeadByte(prevByte)) { insertPos -= 1; }
                            }
                        }

                        memmove(
                            &dynamicTextBuffer[insertPos + 1],
                            &dynamicTextBuffer[insertPos],
                            (processedTextLen - insertPos) + 1
                        );

                        dynamicTextBuffer[insertPos] = axData->cLineSep;
                        processedTextLen += 1;

                        AppendToListTail(&axData->xLineWidths, &currentLineWidth);
                        if (maxLineWidth <= currentLineWidth)
                            tempLineWidthComp3 = currentLineWidth;
                        else
                            tempLineWidthComp3 = maxLineWidth;
                        maxLineWidth = tempLineWidthComp3;
                        lastWrapPosition = 0;
                        ++currentLineCount;

                        bLastIsDBCharacter = false;
                        if (extraGlyphs) {
                            if (g_usingWinEncoding == 936) {
                                if (IsGBKTrailByte((dynamicTextBuffer[processedTextLen - 1]))) {
                                    if (TryDecodeGBK(&dynamicTextBuffer[processedTextLen - 2], uiTempDoubleByteCode)) {
                                        auto glyphIt2 = extraGlyphs->find(uiTempDoubleByteCode);
                                        if (glyphIt2 != extraGlyphs->end()) {
                                            pCurrentGlyph = &glyphIt2->second; bLastIsDBCharacter = true;
                                        }
                                    }
                                }
                            }
                            else if (g_usingWinEncoding == 950) {
                                if (IsBig5TrailByte((dynamicTextBuffer[processedTextLen - 1]))) {
                                    if (TryDecodeBig5(&dynamicTextBuffer[processedTextLen - 2], uiTempDoubleByteCode)) {
                                        auto glyphIt2 = extraGlyphs->find(uiTempDoubleByteCode);
                                        if (glyphIt2 != extraGlyphs->end()) {
                                            pCurrentGlyph = &glyphIt2->second; bLastIsDBCharacter = true;
                                        }
                                    }
                                }
                            }
                            else if (g_usingWinEncoding == 932) {
                                if (IsSJISTrailByte((dynamicTextBuffer[processedTextLen - 1]))) {
                                    if (TryDecodeSJIS(&dynamicTextBuffer[processedTextLen - 2], uiTempDoubleByteCode)) {
                                        auto glyphIt2 = extraGlyphs->find(uiTempDoubleByteCode);
                                        if (glyphIt2 != extraGlyphs->end()) {
                                            pCurrentGlyph = &glyphIt2->second; bLastIsDBCharacter = true;
                                        }
                                    }
                                }
                            }
                            else if (g_usingWinEncoding == 949) {
                                if (IsKoreanTrailByte((dynamicTextBuffer[processedTextLen - 1]))) {
                                    if (TryDecodeKorean(&dynamicTextBuffer[processedTextLen - 2], uiTempDoubleByteCode)) {
                                        auto glyphIt2 = extraGlyphs->find(uiTempDoubleByteCode);
                                        if (glyphIt2 != extraGlyphs->end()) {
                                            pCurrentGlyph = &glyphIt2->second; bLastIsDBCharacter = true;
                                        }
                                    }
                                }
                            }
                        }

                        if (!bLastIsDBCharacter) {
                            pCurrentGlyph = &this->pFontData->pFontLetters[dynamicTextBuffer[processedTextLen - 1]];
                        }
                        currentLineWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);

                        if (bIsDBCharacter) {
                            auto glyphIt3 = extraGlyphs->find(uiDoubleByteCode);
                            if (glyphIt3 != extraGlyphs->end()) { pCurrentGlyph = &glyphIt3->second; }
                        }
                        else { pCurrentGlyph = &this->pFontData->pFontLetters[currentChar]; }

                        nextCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
                        currentLineWidth += nextCharWidth;
                    }
                    else
                    {
                        if (lastWrapPosition == processedTextLen)
                            currentChar = axData->cLineSep;
                        else
                            dynamicTextBuffer[lastWrapPosition] = axData->cLineSep;
                        totalTextHeight = this->pFontData->fBaseLine + lineSpacingAdjust + totalTextHeight;
                        AppendToListTail(&axData->xLineWidths, &preSpaceWidth);
                        if (maxLineWidth <= preSpaceWidth)
                            tempLineWidthComp2 = preSpaceWidth;
                        else
                            tempLineWidthComp2 = maxLineWidth;
                        maxLineWidth = tempLineWidthComp2;
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
                        unsigned char cLastChar = (unsigned char)dynamicTextBuffer[processedTextLen - 1];

                        if (extraGlyphs) {
                            if (g_usingWinEncoding == 936) {
                                if (IsGBKLeadByte(cLastChar)) { tailStart = processedTextLen - 2; tailBytes = processedTextLen - tailStart; }
                            }
                            else if (g_usingWinEncoding == 950) {
                                if (IsBig5LeadByte(cLastChar)) { tailStart = processedTextLen - 2; tailBytes = processedTextLen - tailStart; }
                            }
                            else if (g_usingWinEncoding == 932) {
                                if (IsSJISLeadByte(cLastChar)) { tailStart = processedTextLen - 2; tailBytes = processedTextLen - tailStart; }
                            }
                            else if (g_usingWinEncoding == 949) {
                                if (IsKoreanLeadByte(cLastChar)) { tailStart = processedTextLen - 2; tailBytes = processedTextLen - tailStart; }
                            }
                        }
                    }

                    memmove(&dynamicTextBuffer[tailStart + 1],
                        &dynamicTextBuffer[tailStart],
                        tailBytes);

                    dynamicTextBuffer[tailStart] = axData->cLineSep;

                    processedTextLen += 1;
                    totalTextHeight += (this->pFontData->fBaseLine + lineSpacingAdjust);

                    AppendToListTail(&axData->xLineWidths, &currentLineWidth);
                    if (maxLineWidth <= currentLineWidth)
                        tempLineWidthComp1 = currentLineWidth;
                    else
                        tempLineWidthComp1 = maxLineWidth;
                    maxLineWidth = tempLineWidthComp1;
                    lastWrapPosition = 0;
                    ++currentLineCount;

                    bLastIsDBCharacter = false;
                    if (extraGlyphs) {
                        if (g_usingWinEncoding == 936) {
                            if (IsGBKTrailByte(dynamicTextBuffer[processedTextLen - 1])) {
                                if (TryDecodeGBK((const char*)&dynamicTextBuffer[processedTextLen - 2], uiTempDoubleByteCode)) {
                                    auto glyphIt2 = extraGlyphs->find(uiTempDoubleByteCode);
                                    if (glyphIt2 != extraGlyphs->end()) { pCurrentGlyph = &glyphIt2->second; bLastIsDBCharacter = true; }
                                }
                            }
                        }
                        else if (g_usingWinEncoding == 950) {
                            if (IsBig5TrailByte(dynamicTextBuffer[processedTextLen - 1])) {
                                if (TryDecodeBig5((const char*)&dynamicTextBuffer[processedTextLen - 2], uiTempDoubleByteCode)) {
                                    auto glyphIt2 = extraGlyphs->find(uiTempDoubleByteCode);
                                    if (glyphIt2 != extraGlyphs->end()) { pCurrentGlyph = &glyphIt2->second; bLastIsDBCharacter = true; }
                                }
                            }
                        }
                        else if (g_usingWinEncoding == 932) {
                            if (IsSJISTrailByte(dynamicTextBuffer[processedTextLen - 1])) {
                                if (TryDecodeSJIS((const char*)&dynamicTextBuffer[processedTextLen - 2], uiTempDoubleByteCode)) {
                                    auto glyphIt2 = extraGlyphs->find(uiTempDoubleByteCode);
                                    if (glyphIt2 != extraGlyphs->end()) { pCurrentGlyph = &glyphIt2->second; bLastIsDBCharacter = true; }
                                }
                            }
                        }
                        else if (g_usingWinEncoding == 949) {
                            if (IsKoreanTrailByte(dynamicTextBuffer[processedTextLen - 1])) {
                                if (TryDecodeKorean((const char*)&dynamicTextBuffer[processedTextLen - 2], uiTempDoubleByteCode)) {
                                    auto glyphIt2 = extraGlyphs->find(uiTempDoubleByteCode);
                                    if (glyphIt2 != extraGlyphs->end()) { pCurrentGlyph = &glyphIt2->second; bLastIsDBCharacter = true; }
                                }
                            }
                        }
                    }

                    if (!bLastIsDBCharacter) {
                        pCurrentGlyph = &this->pFontData->pFontLetters[dynamicTextBuffer[processedTextLen - 1]];
                    }

                    currentLineWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);

                    if (bIsDBCharacter) {
                        auto glyphIt3 = extraGlyphs->find(uiDoubleByteCode);
                        if (glyphIt3 != extraGlyphs->end()) { pCurrentGlyph = &glyphIt3->second; }
                    }
                    else { pCurrentGlyph = &this->pFontData->pFontLetters[currentChar]; }

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
            totalTextHeight = totalTextHeight - (this->pFontData->fBaseLine + lineSpacingAdjust);
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
        totalTextHeight = this->pFontData->pFontLetters[' '].fHeight;
        currentLineWidth = ConditionalFloatToUInt(this->pFontData->pFontLetters[' '].fWidth);
    }
    AppendToListTail(&axData->xLineWidths, &currentLineWidth);
    if (maxLineWidth <= currentLineWidth)
        finalMaxLineWidth = currentLineWidth;
    else
        finalMaxLineWidth = maxLineWidth;
    maxLineWidth = finalMaxLineWidth;
    dynamicTextBuffer[processedTextLen] = 0;
    axData->xNewText.Set(dynamicTextBuffer, 0);
    axData->iWidth = maxLineWidth;
    axData->iHeight = totalTextHeight;
    axData->iLineStart = 0;
    axData->iLineEnd = currentLineCount;
    axData->iCharCount = processedTextLen;
    MemoryManager_s_Instance->Deallocate(processedOriginalText);
    MemoryManager_s_Instance->Deallocate(dynamicTextBuffer);
}

// ==================== FontEx::CreateText ====================
UInt32 FontEx::CreateText(
    BSStringT<char>* axTextString, int* aiWidth, int* aiHeight,
    int aiLineStart, int aiLineEnd, int aiFlags, char aiLineBreakChar,
    const NiColorA* axFontColor, UINT32** apTextShape, UINT32** apIconShape) {
    double v11;
    UINT32* v12;
    UINT32* v13;
    double axPos__3;
    int v16;
    float axPos__4;
    int v19;
    signed int v20;
    int m_item_2;
    BSSimpleList<int>* m_pkNext_0;
    int _1_1;
    int m_item_3;
    BSSimpleList<int>* m_pkNext_1;
    int _1;
    int m_item_1;
    int m_item;
    unsigned __int8 cCurrentChar;
    int cCurrentChar_1;
    int axPos__1;
    NiPoint3 axPos_;
    float axPos__2;
    Font::TextData axData;
    int j_1;
    Font::TextData axData2;
    int v37;
    bool bIsDBCharacter, rendered;
    unsigned char cLSB;
    int iActualCharCount;
    UInt32 uiDoubleByteCode;
    auto extraGlyphEntry = gNumberedExtraLetters.find(this->iFontNum);
    auto* extraGlyphs = extraGlyphEntry != gNumberedExtraLetters.end() ? &extraGlyphEntry->second : nullptr;

    std::string sCurrentStr, sConvertedStr;

    if (!*aiHeight)
        *aiHeight = 0x7FFFFFFF;
    if (!aiLineEnd)
        aiLineEnd = 0x7FFFFFFF;
    *(float*)&axData2.iLineStart = FontManagerGetLinePadding(this->iFontNum);
    ThisStdCall(0x759330, &axData, *aiWidth, *aiHeight, aiLineStart, aiLineEnd, aiLineBreakChar);
    v37 = 0;

    if (g_bEnableUTF8 && g_uiEncoding != 0 && extraGlyphs) {
        if (IsValidUTF8With3ByteMin(axTextString->pString)) {
            sCurrentStr = axTextString->pString;
            sConvertedStr = UTF8ToMultiByteStr(sCurrentStr, g_usingWinEncoding);
            axTextString->Set(sConvertedStr.c_str());
        }
    }

    ThisStdCall(0xA12FB0, this, axTextString->pString, &axData);

    *aiWidth = axData.iWidth;
    *aiHeight = axData.iHeight;
    j_1 = 0;
    axPos__1 = 0;
    if (aiFlags == 4)
    {
        if (&axData == (Font::TextData*)-32)
            m_item = -1;
        else
            m_item = axData.xLineWidths.m_item;
        axPos__1 = -m_item;
    }
    else if (aiFlags == 2)
    {
        if (&axData == (Font::TextData*)-32)
            m_item_1 = -1;
        else
            m_item_1 = axData.xLineWidths.m_item;
        axPos__1 = m_item_1 / -2;
    }
    axPos_.x = (float)axPos__1;
    v11 = this->pFontData->fBaseLine - this->fFontHeight;
    axPos_.z = v11 + v11;
    axPos_.y = 0.0;

    iActualCharCount = axData.iCharCount;
    if (extraGlyphs) {
        for (int Charcount = 0;
            axData.xNewText.pString[axData.xNewText.pString != 0 ? Charcount : 0];
            ++Charcount) {
            bIsDBCharacter = false;

            cLSB = (unsigned char)axData.xNewText.pString[Charcount + 1];
            if (cLSB != 0) {
                if (g_usingWinEncoding == 936) {
                    bIsDBCharacter = TryDecodeGBK((const char*)&axData.xNewText.pString[Charcount], uiDoubleByteCode);
                }
                else if (g_usingWinEncoding == 950) {
                    bIsDBCharacter = TryDecodeBig5((const char*)&axData.xNewText.pString[Charcount], uiDoubleByteCode);
                }
                else if (g_usingWinEncoding == 932) {
                    bIsDBCharacter = TryDecodeSJIS((const char*)&axData.xNewText.pString[Charcount], uiDoubleByteCode);
                }
                else if (g_usingWinEncoding == 949) {
                    bIsDBCharacter = TryDecodeKorean((const char*)&axData.xNewText.pString[Charcount], uiDoubleByteCode);
                }
            }

            if (bIsDBCharacter) {
                ++Charcount;
                iActualCharCount = iActualCharCount - 1;
            }
        }
    }

    *apTextShape = (UINT32*)Font::MakeTriShape(iActualCharCount, axFontColor, 1);
    *(float*)&axData2.xNewText.sLen = 0.0;
    *(float*)&axData2.iWidth = axPos_.y;
    *(float*)&axData2.iHeight = axPos_.z;
    v12 = *apTextShape + 22;
    *(float*)v12 = 0.0;
    v12[1] = axData2.iWidth;
    v12[2] = axData2.iHeight;
    if (this->ButtonIcons.uiSize)
    {
        *apIconShape = (UINT32*)Font::MakeIconsTriShape();
        v13 = *apIconShape + 22;
        *v13 = *(DWORD*)&axData2.xNewText.sLen;
        v13[1] = axData2.iWidth;
        v13[2] = axData2.iHeight;
        ThisStdCall(0xA67050, (NiGeometryData*)(*apIconShape)[46], 0x4000);
    }
    axPos__2 = axPos_.x;
    axData2.iLineEnd = 0;
    axData2.xNewText.pString = 0;
    for (axData2.iCharCount = 0;
        axData.xNewText.pString[axData.xNewText.pString != 0 ? axData2.iCharCount : 0];
        ++axData2.iCharCount)
    {
        if (axData.xNewText.pString[axData.xNewText.pString != 0 ? axData2.iCharCount : 0] == aiLineBreakChar)
        {
            ++j_1;
            axPos_.x = 0.0;
            if (aiFlags == 4)
            {
                m_pkNext_1 = &axData.xLineWidths;
                for (_1 = 0; _1 < j_1 && m_pkNext_1; ++_1)
                    m_pkNext_1 = m_pkNext_1->m_pkNext;
                if (m_pkNext_1)
                    m_item_3 = m_pkNext_1->m_item;
                else
                    m_item_3 = -1;
                axPos_.x = (float)-m_item_3;
            }
            else if (aiFlags == 2)
            {
                m_pkNext_0 = &axData.xLineWidths;
                for (_1_1 = 0; _1_1 < j_1 && m_pkNext_0; ++_1_1)
                    m_pkNext_0 = m_pkNext_0->m_pkNext;
                if (m_pkNext_0)
                    m_item_2 = m_pkNext_0->m_item;
                else
                    m_item_2 = -1;
                axPos_.x = (float)(m_item_2 / -2);
            }
            axPos_.z = axPos_.z - (this->pFontData->fBaseLine + *(float*)&axData2.iLineStart);
        }
        else if (axData.xNewText.pString[axData.xNewText.pString != 0 ? axData2.iCharCount : 0] == '\t')
        {
            axPos__3 = axPos_.x;
            AlignLineWidthToTab(axPos_.x, 75.0);
            axPos__4 = axPos__3;
            axPos_.x = 75.0 - axPos__4 + axPos_.x;
        }
        cCurrentChar = axData.xNewText.pString[axData.xNewText.pString != 0 ? axData2.iCharCount : 0];

        bIsDBCharacter = false;

        if (extraGlyphs) {
            cLSB = (unsigned char)axData.xNewText.pString[axData2.iCharCount + 1];
            if (cLSB != 0) {
                if (g_usingWinEncoding == 936) {
                    bIsDBCharacter = TryDecodeGBK((const char*)&axData.xNewText.pString[axData2.iCharCount], uiDoubleByteCode);
                }
                else if (g_usingWinEncoding == 950) {
                    bIsDBCharacter = TryDecodeBig5((const char*)&axData.xNewText.pString[axData2.iCharCount], uiDoubleByteCode);
                }
                else if (g_usingWinEncoding == 932) {
                    bIsDBCharacter = TryDecodeSJIS((const char*)&axData.xNewText.pString[axData2.iCharCount], uiDoubleByteCode);
                }
                else if (g_usingWinEncoding == 949) {
                    bIsDBCharacter = TryDecodeKorean((const char*)&axData.xNewText.pString[axData2.iCharCount], uiDoubleByteCode);
                }
            }
            else {
                bIsDBCharacter = false;
            }
        }

        if (!bIsDBCharacter) {
            ConvertToAsciiQuotes(&cCurrentChar);
        }

        rendered = false;

        cCurrentChar_1 = cCurrentChar;
        if (cCurrentChar == 1)
        {
            if (this->ButtonIcons.uiSize)
                Font::AddIcon((int)axData2.xNewText.pString++, (NiTriShape*)*apIconShape, &axPos_);
        }
        else
        {
            if (extraGlyphs) {
                if (bIsDBCharacter) {
                    auto glyphIt = extraGlyphs->find(uiDoubleByteCode);
                    if (glyphIt != extraGlyphs->end()) {
                        StdCall<FontLetter*>(
                            0xA142D0,
                            &glyphIt->second,
                            axData2.iLineEnd++,
                            (NiTriShape*)*apTextShape,
                            &axPos_.x,
                            axFontColor);
                        axData2.iCharCount += 1;
                        rendered = true;
                    }
                }
            }
            if (!rendered) {
                StdCall<FontLetter*>(
                    0xA142D0,
                    &this->pFontData->pFontLetters[cCurrentChar],
                    axData2.iLineEnd++,
                    (NiTriShape*)*apTextShape,
                    &axPos_.x,
                    axFontColor);
            }
        }
        v19 = *aiWidth;
        v20 = ConditionalFloatToUInt(axPos_.x - axPos__2);
        if (v20 <= v19)
            v16 = v19;
        else
            v16 = v20;
        *aiWidth = v16;
    }
    *(DWORD*)&axData2.cLineSep = *(DWORD*)((*apTextShape)[46] + 32);
    ThisStdCall(
        0xA7EE30,
        (float*)((*apTextShape)[46] + 16),
        *(unsigned __int16*)((*apTextShape)[46] + 8),
        *(float**)&axData2.cLineSep);
    if (*apIconShape)
        ThisStdCall(
            0xA7EE30,
            (float*)((*apIconShape)[46] + 16),
            *(unsigned __int16*)((*apIconShape)[46] + 8),
            *(float**)((*apIconShape)[46] + 32));
    this->ButtonIcons.Clear(1);
    v37 = -1;
    return ThisStdCall(0x7593E0, (char*)&axData);
}

// ==================== FontEx::MakeString ====================
UInt32* FontEx::MakeString(
    float afStartX, float afStartY, float afZ,
    BSStringT<char>* apTextString, int* aiWidth, bool abPrepareObject,
    const NiColorA* arg1C, bool abUpperLeftCorner, bool abPrepareObject_1) {

    double v11;
    UINT32* NiTriShape_1;
    double arg4__4;
    int pString_1;
    float arg4__5;
    char n10;
    UINT32 i_1;
    UINT32 sLen;
    int pString;
    signed int pString_2;
    char _54__1[4];
    float arg4__1;
    unsigned __int8 cCurrentChar;
    int v25;
    UINT32 i;
    float arg4__2;
    float afZ_1;
    float afStartY_1;
    UINT32* NiTriShape_0;
    float arg4__3;
    NiColorA* apColor;
    float v34[4];
    char _54_[4];
    float afStartX_1;
    float afZ_2;
    float afStartY_2;
    float arg4;
    int aiVert;
    int j;
    float* v42;

    bool bIsDBCharacter, rendered;
    unsigned char cLSB;
    int iActualCharCount;
    UInt32 uiDoubleByteCode;
    auto extraGlyphEntry = gNumberedExtraLetters.find(this->iFontNum);
    auto* extraGlyphs = extraGlyphEntry != gNumberedExtraLetters.end() ? &extraGlyphEntry->second : nullptr;

    std::string sCurrentStr, sConvertedStr;

    if (g_bEnableUTF8 && g_uiEncoding != 0 && extraGlyphs) {
        if (IsValidUTF8With3ByteMin(apTextString->pString)) {
            sCurrentStr = apTextString->pString;
            sConvertedStr = UTF8ToMultiByteStr(sCurrentStr, g_usingWinEncoding);
            apTextString->Set(sConvertedStr.c_str());
        }
    }

    if (apTextString->sLen == 0xFFFF)
        sLen = strlen(apTextString->pString);
    else
        sLen = apTextString->sLen;
    if (!sLen || !this->pFontData)
        return 0;
    arg4 = (float)*aiWidth;
    ThisStdCall(0xA12370, (int*)this, apTextString->pString, &arg4, (int)_54_, abPrepareObject, 0);
    arg4__2 = afStartX + arg4;
    afStartY_1 = afStartY;
    afZ_1 = afZ;

    if (abUpperLeftCorner)
    {
        v11 = this->pFontData->pFontLetters[32].fHeight - this->pFontData->fBaseLine;
        afStartY_1 = afStartY_1 - (v11 + v11);
    }

    for (i = 0; ; ++i)
    {
        i_1 = apTextString->sLen == 0xFFFF ? strlen(apTextString->pString) : apTextString->sLen;
        if (i >= i_1 || !apTextString->pString[apTextString->pString != 0 ? i : 0])
            break;
    }

    if (!i)
        return 0;

    iActualCharCount = i;

    if (extraGlyphs) {
        for (UINT32 Charcount = 0;
            apTextString->pString[apTextString->pString != 0 ? Charcount : 0];
            ++Charcount) {
            bIsDBCharacter = false;

            if (Charcount >= i_1) {
                break;
            }

            cLSB = (unsigned char)apTextString->pString[Charcount + 1];
            if (cLSB != 0) {
                if (g_usingWinEncoding == 936) {
                    bIsDBCharacter = TryDecodeGBK((const char*)&apTextString->pString[Charcount], uiDoubleByteCode);
                }
                else if (g_usingWinEncoding == 950) {
                    bIsDBCharacter = TryDecodeBig5((const char*)&apTextString->pString[Charcount], uiDoubleByteCode);
                }
                else if (g_usingWinEncoding == 932) {
                    bIsDBCharacter = TryDecodeSJIS((const char*)&apTextString->pString[Charcount], uiDoubleByteCode);
                }
                else if (g_usingWinEncoding == 949) {
                    bIsDBCharacter = TryDecodeKorean((const char*)&apTextString->pString[Charcount], uiDoubleByteCode);
                }
            }
            if (bIsDBCharacter) {
                ++Charcount;
                iActualCharCount = iActualCharCount - 1;
            }
        }
    }

    NiTriShape_1 = (UINT32*)Font::MakeTriShape(iActualCharCount, arg1C, abPrepareObject_1);
    NiTriShape_0 = NiTriShape_1;
    afStartX_1 = afStartX;
    afZ_2 = afZ_1;
    afStartY_2 = afStartY_1;
    *((float*)NiTriShape_1 + 22) = afStartX;
    *((float*)NiTriShape_1 + 23) = afZ_2;
    *((float*)NiTriShape_1 + 24) = afStartY_2;
    apColor = 0;
    v34[0] = 0.0;
    v34[1] = 0.0;
    v34[2] = 1.0;
    v34[3] = 1.0;
    *aiWidth = 0;
    arg4__3 = arg4__2;
    aiVert = 0;
    for (j = 0; apTextString->pString[apTextString->pString != 0 ? j : 0]; ++j)
    {
        if (apTextString->pString[apTextString->pString != 0 ? j : 0] == 3)
            apColor = 0;
        n10 = apTextString->pString[apTextString->pString != 0 ? j : 0];
        if (n10 == '\t')
        {
            arg4__4 = arg4__2;
            AlignLineWidthToTab(arg4__2, 75.0);
            arg4__5 = arg4__4;
            arg4__2 = 75.0 - arg4__5 + arg4__2;
        }
        else if (n10 == '\n')
        {
            arg4__1 = (float)*aiWidth;
            ThisStdCall(0xA12370, (int*)this, apTextString->pString, &arg4__1, (int)_54__1, abPrepareObject, j + 1);
            arg4__2 = arg4__1;
            afStartY_1 = afStartY_1 - this->pFontData->fBaseLine;
        }

        cCurrentChar = apTextString->pString[apTextString->pString != 0 ? j : 0];

        bIsDBCharacter = false;

        if (extraGlyphs) {
            cLSB = (unsigned char)apTextString->pString[j + 1];
            if (cLSB != 0) {
                if (g_usingWinEncoding == 936) {
                    bIsDBCharacter = TryDecodeGBK((const char*)&apTextString->pString[j], uiDoubleByteCode);
                }
                else if (g_usingWinEncoding == 950) {
                    bIsDBCharacter = TryDecodeBig5((const char*)&apTextString->pString[j], uiDoubleByteCode);
                }
                else if (g_usingWinEncoding == 932) {
                    bIsDBCharacter = TryDecodeSJIS((const char*)&apTextString->pString[j], uiDoubleByteCode);
                }
                else if (g_usingWinEncoding == 949) {
                    bIsDBCharacter = TryDecodeKorean((const char*)&apTextString->pString[j], uiDoubleByteCode);
                }
            }
            else {
                bIsDBCharacter = false;
            }
        }

        if (!bIsDBCharacter) {
            ConvertToAsciiQuotes(&cCurrentChar);
        }

        rendered = false;

        v25 = cCurrentChar;

        if (extraGlyphs) {
            if (bIsDBCharacter) {
                auto glyphIt = extraGlyphs->find(uiDoubleByteCode);
                if (glyphIt != extraGlyphs->end()) {
                    StdCall<FontLetter*>(
                        0xA142D0,
                        &glyphIt->second,
                        aiVert++,
                        (NiTriShape*)NiTriShape_0,
                        &arg4__2,
                        apColor);
                    j += 1;
                    rendered = true;
                }
            }
        }
        if (!rendered) {
            StdCall<FontLetter*>(
                0xA142D0,
                &this->pFontData->pFontLetters[cCurrentChar],
                aiVert++,
                (NiTriShape*)NiTriShape_0,
                &arg4__2,
                apColor);
        }

        pString = *aiWidth;
        pString_2 = ConditionalFloatToUInt(arg4__2 - arg4__3);
        if (pString_2 <= pString)
            pString_1 = pString;
        else
            pString_1 = pString_2;
        *aiWidth = pString_1;
        if (apTextString->pString[apTextString->pString != 0 ? j : 0] == 2)
            apColor = (NiColorA*)v34;
    }
    v42 = *(float**)(NiTriShape_0[46] + 32);
    ThisStdCall(
        0xA7EE30,
        (float*)(NiTriShape_0[46] + 16),
        *(unsigned __int16*)(NiTriShape_0[46] + 8), v42);
    return (UInt32*)NiTriShape_0;
}

} // namespace fonthook

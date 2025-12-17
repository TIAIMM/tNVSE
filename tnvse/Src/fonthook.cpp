#pragma once
#include "fonthook.h"

namespace fonthook {
    static Font* __fastcall FontCreateForJIP(Font* apThis, void*, int iFontNum, char* apFilename, bool abLoad) {
        //gLog.FormattedMessage("\nCall Font::Font");
        //gLog.FormattedMessage("iFontNum: %u", iFontNum);
        //gLog.FormattedMessage("apFilename: %s", (const char*)apFilename);

        Font* ret = ThisStdCall<Font*>(
            0xA12020,
            apThis,
            iFontNum,
            apFilename,
            abLoad);

        if (!fontNameKey.empty()) {
            auto it = gExtraFontLetters.find(fontNameKey);
            if (it != gExtraFontLetters.end() && !it->second.empty()) {
                //gLog.FormattedMessage("From gExtraFontLetters to gNumberedExtraLetters");
                gNumberedExtraLetters[iFontNum] = std::move(it->second);
                gExtraFontLetters.erase(it);
                if (!gNumberedExtraLetters[iFontNum].empty()) {
                    //gLog.FormattedMessage("gNumberedExtraLetters[%d] is filled", iFontNum);
                    fontNameKey.clear();
                }
            }
            else {
                //gLog.FormattedMessage("gExtraFontLetters for %s is empty", fontNameKey);
                fontNameKey.clear();
            }
        }

        return ret;
    }

    class FontEx : public Font {
    public:
        Font* FontCreate(int iFontNum, char* apFilename, bool abLoad) {
            //gLog.FormattedMessage("\nCall Font::Font");
            //gLog.FormattedMessage("iFontNum: %u", iFontNum);
            //gLog.FormattedMessage("apFilename: %s", (const char*)apFilename);

            Font* ret = ThisStdCall<Font*>(
                0xA12020,
                this,
                iFontNum,
                apFilename,
                abLoad);

            if (!fontNameKey.empty()) {
                auto it = gExtraFontLetters.find(fontNameKey);
                if (it != gExtraFontLetters.end() && !it->second.empty()) {
                    //gLog.FormattedMessage("From gExtraFontLetters to gNumberedExtraLetters");
                    gNumberedExtraLetters[iFontNum] = std::move(it->second);
                    gExtraFontLetters.erase(it);
                    if (!gNumberedExtraLetters[iFontNum].empty()) {
                        //gLog.FormattedMessage("gNumberedExtraLetters[%d] is filled", iFontNum);
                        fontNameKey.clear();
                    }
                }
                else {
                    //gLog.FormattedMessage("gExtraFontLetters for %s is empty", fontNameKey);
                    fontNameKey.clear();
                }
            }

            return ret;
        }

        void Load()
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

            //gLog.FormattedMessage("Call Font::Load");

            __asm {
                mov eax, fs: [0x18]
                mov tebAddress, eax
            }
            tlsPointer = *(DWORD*)(tebAddress + 0x2C);
            tlsSlotAddress = *(DWORD*)(tlsPointer + (*pTlsIndex) * 4);
            targetAddress = tlsSlotAddress + 692;

            //gLog.FormattedMessage("Set stringRefFlag");

            stringRefFlag = 0;

            oldTlsValue = *(DWORD*)targetAddress;
            savedTlsValue = oldTlsValue;
            *(DWORD*)targetAddress = 12;

            //gLog.FormattedMessage("Initialize stackCookie");

            stackCookie = 0;
            iRefCount = this->iRefCount;
            if (iRefCount || !this->pFontFile)
            {
            LABEL_46:
                //gLog.FormattedMessage("LABEL_46");
                ++this->iRefCount;
                stackCookie = -1;
                *(DWORD*)targetAddress = savedTlsValue;
                //gLog.FormattedMessage("Font::Load End\n");
                return;
            }
            //gLog.FormattedMessage("Load %s", (const char*)this->pFontFile);
            BSFile_1 = FileFinder_GetFile(this->pFontFile, (NiFile::OpenMode)0, 0x150000u, 2u);
            //gLog.FormattedMessage("Load %s End", (const char*)this->pFontFile);
            if (BSFile_1)
            {
                if (LOBYTE(BSFile_1->m_pFile))
                {
                    //gLog.FormattedMessage("Allocate pFontData");
                    pFontData = (FontData*)MemoryManager_s_Instance->Allocate(0x3928u);
                    //gLog.FormattedMessage("Allocate pFontData End");
                    this->pFontData = pFontData;
                    //gLog.FormattedMessage("Moved Font Data");
                    //v21 = BSFile_1->GetSize();
                    v21 = 0x3928u;
                    pFontData_1 = this->pFontData;
                    v25[0] = 1;
                    v23 = BSFile_1->m_pfnRead(BSFile_1, pFontData_1, v21, v25, 1u);
                    BSFile_1->m_uiAbsoluteCurrentPos += v23;
                    v24 = v23;

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

                            if (!extraMap.empty()) {
                                //gLog.FormattedMessage("Loaded %u extra FontLetter records for %s",(unsigned)extraMap.size(),fontNameKey);
                            }
                        }
                    }

                    NiFile_1 = BSFile_1;
                    BSFile_2 = BSFile_1;
                    //gLog.FormattedMessage("Delete BSFile_2");
                    if (BSFile_1) {
                        delete(BSFile_2);
                    }
                    //gLog.FormattedMessage("Delete BSFile_2 End");
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

                        //gLog.FormattedMessage("Load %s", (const char*)apName_);

                        NiBinaryStream_0 = FileFinder_GetFile((const char*)apName_, (NiFile::OpenMode)0, 0x6000000u, 2u);

                        //gLog.FormattedMessage("Load %s End", (const char*)apName_);

                        if (!NiBinaryStream_0 || !(m_pfnWrite = (bool)NiBinaryStream_0->m_pFile))
                        {
                            if (NiBinaryStream_0)
                            {
                                NiBinaryStream_1 = NiBinaryStream_0;
                                NiBinaryStream_2 = NiBinaryStream_0;
                                //gLog.FormattedMessage("Delete NiBinaryStream_2");
                                delete(NiBinaryStream_2);
                                //gLog.FormattedMessage("Delete NiBinaryStream_2 End");
                            }
                            stackCookie = -1;
                            v13[1] = savedTlsValue;
                            *(DWORD*)targetAddress = savedTlsValue;
                            return;
                        }

                        v13[0] = 1;
                        //gLog.FormattedMessage("Read Texture Size");
                        v11 = NiBinaryStream_0->m_pfnRead(NiBinaryStream_0, &a2, 4u, v13, 1u);
                        //gLog.FormattedMessage("Length: %u", a2);
                        v11 += NiBinaryStream_0->m_pfnRead(NiBinaryStream_0, &a3, 4u, v13, 1u);
                        //gLog.FormattedMessage("Width: %u", a3);
                        NiBinaryStream_0->m_uiAbsoluteCurrentPos += v11;
                        v12 = v11;
                        arPrefs_.m_ePixelLayout = static_cast<NiTexture::FormatPrefs::PixelLayout>(0x6);
                        arPrefs_.m_eAlphaFmt = static_cast<NiTexture::FormatPrefs::AlphaFormat>(0x3);
                        arPrefs_.m_eMipMapped = static_cast<NiTexture::FormatPrefs::MipFlag>(0x2);
                        v36 = (NiPixelData*)NiMemObject::operator new(sizeof(NiPixelData));
                        stackCookie = (stackCookie & 0xFFFFFF00) | 1;
                        if (v36) {
                            //gLog.FormattedMessage("Instancing NiPixelData");
                            NiPixelData_1 = ThisStdCall<NiPixelData*>(
                                0xA7C190,
                                v36,
                                a2,
                                a3,
                                reinterpret_cast<const NiPixelFormat*>(0x11AA2A0),
                                1,
                                1);
                            //gLog.FormattedMessage("Instancing NiPixelData End");
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
                            //gLog.FormattedMessage("Instancing NiTexturingProperty");
                            /*NiTexturingProperty_4 = static_cast<NiTexturingPropertyEx*>(NiTexturingProperty_3)->NiTexturingPropertyBuild(
                                NiPixelData_3,
                                &kName_,
                                &arPrefs_);*/
                            NiTexturingProperty_4 = ThisStdCall<NiTexturingProperty*>(
                                0xA6ABB0,
                                NiTexturingProperty_3,
                                NiPixelData_3,
                                &kName_,
                                &arPrefs_);
                            //gLog.FormattedMessage("Instancing NiTexturingProperty End");
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
                            //gLog.FormattedMessage("Delete NiBinaryStream_4");
                            delete(NiBinaryStream_4);
                            //gLog.FormattedMessage("Delete NiBinaryStream_4 End");
                        }
                        NiBinaryStream_0 = 0;
                        ThisStdCall(
                            0x60AEB0,
                            NiTexturingProperty_2,
                            1);
                        ThisStdCall(
                            0x66B0D0,
                            &this->pTextureData[iTextureCount].m_pObject,
                            (int)NiTexturingProperty_2);
                        //gLog.FormattedMessage("finish load");
                    }
                    goto LABEL_46;
                }
            }
            pFontFile_2 = this->pFontFile;
            if (BSFile_1)
            {
                BSFile_3 = BSFile_1;
                BSFile_4 = BSFile_1;
                //gLog.FormattedMessage("Delete BSFile_4");
                delete(BSFile_4);
                //gLog.FormattedMessage("Delete BSFile_4 End");
            }
            stackCookie = -1;
            v25[1] = savedTlsValue;
            *(DWORD*)targetAddress = savedTlsValue;
        }

        void __thiscall PrepTextForTerminal(const char* apOrigString, Font::TextData* axData) {
            unsigned int charWidthWithKerning; // eax
            //unsigned int spaceCharWidth; // eax
            unsigned int tildeCharWidth; // eax
            //unsigned int hyphenCharWidth; // eax
            unsigned int nextCharWidth; // eax
            //unsigned int hyphenInsertWidth; // eax
            unsigned int combinedCharWidth; // eax
            int finalMaxLineWidth; // [esp+0h] [ebp-7E0h]
            int tempLineWidthComp1; // [esp+4h] [ebp-7DCh]
            int tempLineWidthComp2; // [esp+8h] [ebp-7D8h]
            int tempLineWidthComp3; // [esp+Ch] [ebp-7D4h]
            int tempLineWidthComp4; // [esp+10h] [ebp-7D0h]
            int escapeSeqEffectiveLen; // [esp+14h] [ebp-7CCh]
            char* processedTextBuffer; // [esp+8Ch] [ebp-754h]
            char* originalTextBuffer; // [esp+94h] [ebp-74Ch]
            unsigned int truncateCharCounter; // [esp+9Ch] [ebp-744h]
            SInt32 lineCounter; // [esp+A0h] [ebp-740h]
            unsigned int truncatedTextLen; // [esp+A4h] [ebp-73Ch]
            unsigned __int8 currentChar; // [esp+ABh] [ebp-735h] BYREF
            FontLetter* pCurrentGlyph; // [esp+ACh] [ebp-734h]
            UInt32 charIndex; // [esp+B0h] [ebp-730h]
            UInt32 bufferCopyIndex; // [esp+B4h] [ebp-72Ch]
            char textureNameBuffer[268]; // [esp+B8h] [ebp-728h] BYREF
            int charScanIndex; // [esp+1C4h] [ebp-61Ch]
            float unkarray[4]; // [esp+1D8h] [ebp-608h]
            char substrBuffer[264]; // [esp+1E8h] [ebp-5F8h] BYREF
            signed int escapeSeqSizeDiff; // [esp+2F0h] [ebp-4F0h]
            UInt32 postEscapeTextLen; // [esp+2F4h] [ebp-4ECh]
            bool isPositiveEscape; // [esp+2FBh] [ebp-4E5h]
            int escapeSeqPrefixLen; // [esp+2FCh] [ebp-4E4h]
            UInt32 totalEscapeSeqLen; // [esp+300h] [ebp-4E0h]
            int varNameLen; // [esp+304h] [ebp-4DCh]
            char varNameBuffer[128]; // [esp+308h] [ebp-4D8h] BYREF
            unsigned int srcTextIndex; // [esp+38Ch] [ebp-454h]
            SInt32 currentLineWidth; // [esp+390h] [ebp-450h] BYREF
            unsigned int processedTextLen; // [esp+394h] [ebp-44Ch]
            unsigned int lastWrapPosition; // [esp+398h] [ebp-448h]
            SInt32 maxLineWidth; // [esp+39Ch] [ebp-444h]
            char* dynamicTextBuffer; // [esp+3A0h] [ebp-440h]
            UInt32 buttonIconIndex; // [esp+3A4h] [ebp-43Ch]
            SInt32 preSpaceWidth; // [esp+3A8h] [ebp-438h] BYREF
            bool hasEscapeSequence; // [esp+3AFh] [ebp-431h]
            signed int postSpaceWidth; // [esp+3B0h] [ebp-430h]
            bool isTildeChar; // [esp+3B7h] [ebp-429h]
            char* processedOriginalText; // [esp+3B8h] [ebp-428h]
            int currentLineCount; // [esp+3BCh] [ebp-424h]
            UInt32 textBufferSize; // [esp+3C0h] [ebp-420h]
            int hyphenInsertCount; // [esp+3C4h] [ebp-41Ch]
            char parsedTextBuffer[1028]; // [esp+3C8h] [ebp-418h] BYREF
            float lineSpacingAdjust; // [esp+7D0h] [ebp-10h]
            float totalTextHeight; // [esp+7D4h] [ebp-Ch]
            UInt32 sourceTextLen; // [esp+7D8h] [ebp-8h]
            int maxAllowedLines; // [esp+7DCh] [ebp-4h]

            UInt32 origConsumed = 0;

            bool bLastIsHanzi,bHanzi;
            UInt32 uiGBKcode, uiTempGBKcode;
            auto extraGlyphEntry = gNumberedExtraLetters.find(this->iFontNum);
            auto* extraGlyphs = extraGlyphEntry != gNumberedExtraLetters.end() ? &extraGlyphEntry->second : nullptr;

            //gLog.FormattedMessage("\nCall PrepText");

            if (!apOrigString)
                return;

            //gLog.FormattedMessage("apOrigString = %s", apOrigString);

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
            // 0xEC6130
            sourceTextLen = strlen(apOrigString);
            //gLog.FormattedMessage("sourceTextLen = %u", sourceTextLen);
            maxAllowedLines = axData->iLineEnd;

            //gLog.FormattedMessage("Init originalTextBuffer");
            //gLog.FormattedMessage("Allocating originalTextBuffer: size=%d", sourceTextLen + 4);
            originalTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Allocate(sourceTextLen + 4));
            if (!originalTextBuffer) {
                //gLog.FormattedMessage("Memory allocation failed for originalTextBuffer");
                return;
            }

            //gLog.FormattedMessage("MemSet originalTextBuffer");
            // 0xEC61C0
            memset(originalTextBuffer, 0, sourceTextLen + 4);

            //gLog.FormattedMessage("Init processedOriginalText");
            processedOriginalText = originalTextBuffer;


            //gLog.FormattedMessage("Init processedTextBuffer");
            //gLog.FormattedMessage("Allocating processedTextBuffer: size=%u", sourceTextLen + 4);
            processedTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Allocate(sourceTextLen + 4));
            if (!processedTextBuffer) {
                //gLog.FormattedMessage("Memory allocation failed for processedTextBuffer");
                MemoryManager_s_Instance->Deallocate(originalTextBuffer);
                return;
            }

            //gLog.FormattedMessage("MemSet processedTextBuffer");
            // 0xEC61C0
            memset(processedTextBuffer, 0, sourceTextLen + 4);

            //gLog.FormattedMessage("Init dynamicTextBuffer");
            dynamicTextBuffer = processedTextBuffer;

            //gLog.FormattedMessage("SafeFormatString originalTextBuffer");
            // 0xEC623A
            snprintf(originalTextBuffer, sourceTextLen + 1, "%s", apOrigString);
            //gLog.FormattedMessage("Buffer Init Finish");

            processedTextLen = 0;
            textBufferSize = sourceTextLen + 4;
            hyphenInsertCount = 0;
            isTildeChar = 0;
            parsedTextBuffer[0] = 0;
            hasEscapeSequence = 0;
            //gLog.FormattedMessage("processedOriginalText Read Start");
            for (srcTextIndex = 0; srcTextIndex < sourceTextLen; ++srcTextIndex)
            {
                if (processedOriginalText[srcTextIndex] == '&')
                {
                    //gLog.FormattedMessage("Found replaceable text");
                    varNameLen = 0;
                    escapeSeqPrefixLen = 1;
                    isPositiveEscape = 1;
                    if (processedOriginalText[srcTextIndex + 1] == '-')
                    {
                        isPositiveEscape = 0;
                        escapeSeqPrefixLen = 2;
                    }
                    //gLog.FormattedMessage("Replace replaceable varname");
                    while (processedOriginalText[escapeSeqPrefixLen + varNameLen + srcTextIndex]
                        && varNameLen < 127
                        && processedOriginalText[varNameLen + srcTextIndex] != ';'
                        && processedOriginalText[varNameLen + srcTextIndex] != '\n'
                        && processedOriginalText[varNameLen + srcTextIndex] != axData->cLineSep)
                    {
                        varNameBuffer[varNameLen] = processedOriginalText[escapeSeqPrefixLen + varNameLen + srcTextIndex];
                        ++varNameLen;
                    }
                    //gLog.FormattedMessage("Replace replaceable varname end");
                    if (varNameLen)
                        escapeSeqEffectiveLen = varNameLen - escapeSeqPrefixLen;
                    else
                        escapeSeqEffectiveLen = 0;
                    //gLog.FormattedMessage("Finish varNameBuffer string");
                    varNameBuffer[escapeSeqEffectiveLen] = 0;
                    // 0xEC6130
                    //gLog.FormattedMessage("Get varNameBuffer Length");
                    totalEscapeSeqLen = (strlen(varNameBuffer) + 1);
                    //gLog.FormattedMessage("Process ; Length");
                    if (processedOriginalText[varNameLen + srcTextIndex] == ';')
                        totalEscapeSeqLen += escapeSeqPrefixLen;
                    //gLog.FormattedMessage("ReplaceVariableInString & ParseAndFormatVariableInString");
                    if (ReplaceVariableInString(varNameBuffer, parsedTextBuffer, 0x400u, isPositiveEscape)
                        || ParseAndFormatVariableInString(varNameBuffer, parsedTextBuffer))
                    {
                        //gLog.FormattedMessage("ReplaceVariableInString & ParseAndFormatVariableInString Process Start");
                        // 0xEC6130
                        postEscapeTextLen = strlen(parsedTextBuffer);
                        escapeSeqSizeDiff = postEscapeTextLen - totalEscapeSeqLen;
                        if (parsedTextBuffer[postEscapeTextLen - 1] == '\\')
                        {
                            unkarray[0] = 0.0;
                            unkarray[1] = 0.0;
                            unkarray[2] = 0.0;
                            unkarray[3] = 0.0;
                            for (charScanIndex = 0; parsedTextBuffer[charScanIndex] != '\\'; ++charScanIndex)
                                ;
                            substrBuffer[0] = 0;
                            // 0xEC6370
                            //gLog.FormattedMessage("FastStringCopyAligned 1");
                            //strcpy(&parsedTextBuffer[charScanIndex + 1], substrBuffer);
                            strcpy_s(&parsedTextBuffer[charScanIndex + 1],
                                sizeof(parsedTextBuffer) - (charScanIndex + 1),
                                substrBuffer);
                            //gLog.FormattedMessage("FastStringCopyAligned 1 End");
                            // 0xEC6130
                            UInt32 strLen = strlen(substrBuffer);
                            *((char*)unkarray + strLen + 15) = 0;
                            if (this->iFontNum == 7)
                            {
                                // 0xEC65A6
                                strncpy_s(textureNameBuffer, sizeof(textureNameBuffer), substrBuffer, _TRUNCATE);
                                // 0x406D00
                                vsnprintf(substrBuffer, sizeof(substrBuffer), "glow_%s", textureNameBuffer);
                            }
                            this->AddTextIcon(substrBuffer);
                            parsedTextBuffer[charScanIndex] = 1;
                            parsedTextBuffer[charScanIndex + 1] = 0;
                            // 0xEC6130
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
                        //gLog.FormattedMessage("ReplaceVariableInString & ParseAndFormatVariableInString Process End");
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
            //gLog.FormattedMessage("processedOriginalText Read Finish");
            dynamicTextBuffer[processedTextLen] = 0;
            if (hasEscapeSequence)
            {
                sourceTextLen = processedTextLen;
                //gLog.FormattedMessage("Relocating processedOriginalText: size=%u", processedTextLen + 4);
                processedOriginalText = static_cast<char*>(MemoryManager_s_Instance->Reallocate(processedOriginalText, processedTextLen + 4));
                // 0xEC6370
                //gLog.FormattedMessage("FastStringCopyAligned");
                //strcpy(processedOriginalText, dynamicTextBuffer);
                strcpy_s(processedOriginalText, processedTextLen + 4, dynamicTextBuffer);
            }
            *dynamicTextBuffer = 0;
            processedTextLen = 0;
            buttonIconIndex = 0;

            //gLog.FormattedMessage("processedOriginalText Read 2 Start");
            for (charIndex = 0; charIndex < sourceTextLen && processedOriginalText[charIndex]; ++charIndex)
            {
                if (processedOriginalText[charIndex] == axData->cLineSep)
                {
                    dynamicTextBuffer[processedTextLen] = axData->cLineSep;
                    origConsumed += 1;
                    if (++processedTextLen >= textBufferSize)
                    {
                        //gLog.FormattedMessage("Relocating dynamicTextBuffer: size=%u", processedTextLen + 4);
                        dynamicTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Reallocate(dynamicTextBuffer, processedTextLen + 4));
                        textBufferSize = processedTextLen + 4;
                    }
                    totalTextHeight = this->pFontData->fBaseLine + lineSpacingAdjust + totalTextHeight;
                    //gLog.FormattedMessage("AppendToListTail: %d", currentLineWidth);
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
                    bHanzi = false;
                    if (extraGlyphs) {
                        if ((charIndex + 1) <= sourceTextLen) {
                            if ((unsigned char)processedOriginalText[charIndex] >= 0x81) {
                                bHanzi = TryDecodeGBK((const char*)&processedOriginalText[charIndex], uiGBKcode);
                            }
                        }
                    }

                    if (processedOriginalText[charIndex] == '\t')
                    {
                        currentLineWidth += 75 - currentLineWidth % 75;
                        origConsumed += 1;
                        continue;
                    }

                    if (!bHanzi) {
                        currentChar = processedOriginalText[charIndex];
                        origConsumed += 1;
                        //gLog.FormattedMessage("ConvertToAsciiQuotes: '%c'", currentChar);
                        ConvertToAsciiQuotes(&currentChar);
                        //gLog.FormattedMessage("ConvertToAsciiQuotes: '%c' end", currentChar);
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
                        //gLog.FormattedMessage("charWidthWithKerning: %d", charWidthWithKerning);
                        currentLineWidth += charWidthWithKerning;
                        /*if (currentChar == ' ')
                        {
                            lastWrapPosition = processedTextLen;
                            spaceCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
                            preSpaceWidth = currentLineWidth - spaceCharWidth;
                            postSpaceWidth = currentLineWidth;
                            isTildeChar = 0;
                        }*/
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
                        auto glyphIt = extraGlyphs->find(uiGBKcode);
                        if (glyphIt != extraGlyphs->end()) {
                            pCurrentGlyph = &glyphIt->second;
                            charWidthWithKerning = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
                            //gLog.FormattedMessage("HanziWidthWithKerning: %d", charWidthWithKerning);
                            currentLineWidth += charWidthWithKerning;
                        }
                    }

                    //gLog.FormattedMessage("currentChar '%c' process end", currentChar);

                    if (currentLineWidth > axData->iWidth)
                    {
                        if (lastWrapPosition)
                        {
                            if (isTildeChar)
                            {
                                isTildeChar = 0;
                                textBufferSize += 4;
                                dynamicTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Reallocate(dynamicTextBuffer, textBufferSize + 1));

                                // 0xEC7230
                                /*memmove(
                                    &dynamicTextBuffer[lastWrapPosition + 2],
                                    &dynamicTextBuffer[lastWrapPosition],
                                    processedTextLen - lastWrapPosition);
                                dynamicTextBuffer[lastWrapPosition + 1] = '\n';
                                dynamicTextBuffer[lastWrapPosition] = '-';
                                processedTextLen += 2;
                                hyphenInsertCount += 2;
                                pCurrentGlyph = &this->pFontData->pFontLetters['-'];
                                totalTextHeight = this->pFontData->fBaseLine + lineSpacingAdjust + totalTextHeight;
                                hyphenCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
                                currentLineWidth -= hyphenCharWidth;*/

                                UInt32 insertPos = lastWrapPosition;

                                if (insertPos > 0) {
                                    unsigned char prevByte = (unsigned char)dynamicTextBuffer[insertPos - 1];
                                    if (IsGBKLeadByte(prevByte)) {
                                        insertPos -= 1;
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

                                bLastIsHanzi = false;
                                if (extraGlyphs) {
                                    if (IsGBKTrailByte((dynamicTextBuffer[processedTextLen - 1]))) {
                                        if (TryDecodeGBK(&dynamicTextBuffer[processedTextLen - 2], uiTempGBKcode)) {
                                            auto glyphIt2 = extraGlyphs->find(uiTempGBKcode);
                                            if (glyphIt2 != extraGlyphs->end()) {
                                                pCurrentGlyph = &glyphIt2->second;
                                                bLastIsHanzi = true;
                                            }
                                        }
                                    }
                                }

                                if (!bLastIsHanzi) {
                                    pCurrentGlyph = &this->pFontData->pFontLetters[dynamicTextBuffer[processedTextLen - 1]];
                                }
                                currentLineWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);

                                if (bHanzi) {
                                    auto glyphIt3 = extraGlyphs->find(uiGBKcode);
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

                            /*dynamicTextBuffer[processedTextLen + 1] = dynamicTextBuffer[processedTextLen];
                            dynamicTextBuffer[processedTextLen] = dynamicTextBuffer[processedTextLen - 1];*/

                            //qmemcpy
                            /*memcpy(&dynamicTextBuffer[processedTextLen - 1], "-\n", 2);
                            processedTextLen += 2;
                            hyphenInsertCount += 2;
                            pCurrentGlyph = &this->pFontData->pFontLetters['-'];
                            totalTextHeight = this->pFontData->fBaseLine + lineSpacingAdjust + totalTextHeight;
                            hyphenInsertWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
                            currentLineWidth -= hyphenInsertWidth;*/

                            UInt32 tailStart = processedTextLen - 1;
                            UInt32 tailBytes = 2;

                            if (processedTextLen >= 2) {
                                unsigned char cmsb = (unsigned char)dynamicTextBuffer[processedTextLen - 2];
                                unsigned char clsb = (unsigned char)dynamicTextBuffer[processedTextLen - 1];

                                if (extraGlyphs) {
                                    if (IsGBKLeadByte(cmsb) && IsGBKTrailByte(clsb)) {
                                        tailStart = processedTextLen - 2;
                                        tailBytes = 3;
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

                            bLastIsHanzi = false;
                            if (extraGlyphs) {
                                if (IsGBKTrailByte(dynamicTextBuffer[processedTextLen - 1])) {
                                    if (TryDecodeGBK((const char*)&dynamicTextBuffer[processedTextLen - 2], uiTempGBKcode)) {
                                        auto glyphIt2 = extraGlyphs->find(uiTempGBKcode);
                                        if (glyphIt2 != extraGlyphs->end()) {
                                            pCurrentGlyph = &glyphIt2->second;
                                            bLastIsHanzi = true;
                                        }
                                    }
                                }
                            }

                            if (!bLastIsHanzi) {
                                pCurrentGlyph = &this->pFontData->pFontLetters[dynamicTextBuffer[processedTextLen - 1]];
                            }

                            currentLineWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);

                            if (bHanzi) {
                                auto glyphIt3 = extraGlyphs->find(uiGBKcode);
                                if (glyphIt3 != extraGlyphs->end()) {
                                    pCurrentGlyph = &glyphIt3->second;
                                }
                            }
                            else {
                                pCurrentGlyph = &this->pFontData->pFontLetters[currentChar];
                            }

                            combinedCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);

                            currentLineWidth += combinedCharWidth;
                        }
                    }

                    if (bHanzi)
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
                        /*bHanzi = false;
                        if (extraGlyphs) {
                            if (processedTextLen >= 2) {
                                bHanzi = TryDecodeGBK((const char*)&dynamicTextBuffer[processedTextLen - 2], uiTempGBKcode);
                                if (bHanzi) {
                                        --processedTextLen;
                                        --processedTextLen;
                                        continue;
                                }
                            }
                        }

                        if (!bHanzi) {
                            --processedTextLen;
                        }*/
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

            //gLog.FormattedMessage("processedOriginalText Read 2 End");

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
                //strcpy
                strcpy_s(dynamicTextBuffer, textBufferSize, " ");
                origConsumed = 1;
                processedTextLen = 1;
                currentLineCount = 1;
                totalTextHeight = this->pFontData->pFontLetters[' '].fHeight;
                currentLineWidth = ConditionalFloatToUInt(this->pFontData->pFontLetters[' '].fWidth);
            }
            //gLog.FormattedMessage("AppendToListTail");
            AppendToListTail(&axData->xLineWidths, &currentLineWidth);
            //gLog.FormattedMessage("AppendToListTail End");
            if (maxLineWidth <= currentLineWidth)
                finalMaxLineWidth = currentLineWidth;
            else
                finalMaxLineWidth = maxLineWidth;
            maxLineWidth = finalMaxLineWidth;
            dynamicTextBuffer[processedTextLen] = 0;
            //gLog.FormattedMessage("Write axData");
            //gLog.FormattedMessage("dynamicTextBuffer: %s",(const char*)dynamicTextBuffer);
            axData->xNewText.Set(dynamicTextBuffer, 0);
            //gLog.FormattedMessage("Write Finish");
            axData->iWidth = maxLineWidth;
            axData->iHeight = totalTextHeight;
            axData->iLineStart = 0;
            axData->iLineEnd = currentLineCount;
            axData->iCharCount = origConsumed;
            //gLog.FormattedMessage("Deallocate");
            MemoryManager_s_Instance->Deallocate(processedOriginalText);
            MemoryManager_s_Instance->Deallocate(dynamicTextBuffer);
            //gLog.FormattedMessage("PrepText End");
        }

        void __thiscall PrepText(const char* apOrigString, Font::TextData* axData) {
            unsigned int charWidthWithKerning; // eax
            //unsigned int spaceCharWidth; // eax
            unsigned int tildeCharWidth; // eax
            //unsigned int hyphenCharWidth; // eax
            unsigned int nextCharWidth; // eax
            //unsigned int hyphenInsertWidth; // eax
            unsigned int combinedCharWidth; // eax
            int finalMaxLineWidth; // [esp+0h] [ebp-7E0h]
            int tempLineWidthComp1; // [esp+4h] [ebp-7DCh]
            int tempLineWidthComp2; // [esp+8h] [ebp-7D8h]
            int tempLineWidthComp3; // [esp+Ch] [ebp-7D4h]
            int tempLineWidthComp4; // [esp+10h] [ebp-7D0h]
            int escapeSeqEffectiveLen; // [esp+14h] [ebp-7CCh]
            char* processedTextBuffer; // [esp+8Ch] [ebp-754h]
            char* originalTextBuffer; // [esp+94h] [ebp-74Ch]
            unsigned int truncateCharCounter; // [esp+9Ch] [ebp-744h]
            SInt32 lineCounter; // [esp+A0h] [ebp-740h]
            unsigned int truncatedTextLen; // [esp+A4h] [ebp-73Ch]
            unsigned __int8 currentChar; // [esp+ABh] [ebp-735h] BYREF
            FontLetter* pCurrentGlyph; // [esp+ACh] [ebp-734h]
            UInt32 charIndex; // [esp+B0h] [ebp-730h]
            UInt32 bufferCopyIndex; // [esp+B4h] [ebp-72Ch]
            char textureNameBuffer[268]; // [esp+B8h] [ebp-728h] BYREF
            int charScanIndex; // [esp+1C4h] [ebp-61Ch]
            float unkarray[4]; // [esp+1D8h] [ebp-608h]
            char substrBuffer[264]; // [esp+1E8h] [ebp-5F8h] BYREF
            signed int escapeSeqSizeDiff; // [esp+2F0h] [ebp-4F0h]
            UInt32 postEscapeTextLen; // [esp+2F4h] [ebp-4ECh]
            bool isPositiveEscape; // [esp+2FBh] [ebp-4E5h]
            int escapeSeqPrefixLen; // [esp+2FCh] [ebp-4E4h]
            UInt32 totalEscapeSeqLen; // [esp+300h] [ebp-4E0h]
            int varNameLen; // [esp+304h] [ebp-4DCh]
            char varNameBuffer[128]; // [esp+308h] [ebp-4D8h] BYREF
            unsigned int srcTextIndex; // [esp+38Ch] [ebp-454h]
            SInt32 currentLineWidth; // [esp+390h] [ebp-450h] BYREF
            unsigned int processedTextLen; // [esp+394h] [ebp-44Ch]
            unsigned int lastWrapPosition; // [esp+398h] [ebp-448h]
            SInt32 maxLineWidth; // [esp+39Ch] [ebp-444h]
            char* dynamicTextBuffer; // [esp+3A0h] [ebp-440h]
            UInt32 buttonIconIndex; // [esp+3A4h] [ebp-43Ch]
            SInt32 preSpaceWidth; // [esp+3A8h] [ebp-438h] BYREF
            bool hasEscapeSequence; // [esp+3AFh] [ebp-431h]
            signed int postSpaceWidth; // [esp+3B0h] [ebp-430h]
            bool isTildeChar; // [esp+3B7h] [ebp-429h]
            char* processedOriginalText; // [esp+3B8h] [ebp-428h]
            int currentLineCount; // [esp+3BCh] [ebp-424h]
            UInt32 textBufferSize; // [esp+3C0h] [ebp-420h]
            int hyphenInsertCount; // [esp+3C4h] [ebp-41Ch]
            char parsedTextBuffer[1028]; // [esp+3C8h] [ebp-418h] BYREF
            float lineSpacingAdjust; // [esp+7D0h] [ebp-10h]
            float totalTextHeight; // [esp+7D4h] [ebp-Ch]
            UInt32 sourceTextLen; // [esp+7D8h] [ebp-8h]
            int maxAllowedLines; // [esp+7DCh] [ebp-4h]

            UInt32 origConsumed = 0;

            bool bLastIsHanzi, bHanzi;
            UInt32 uiGBKcode, uiTempGBKcode;
            auto extraGlyphEntry = gNumberedExtraLetters.find(this->iFontNum);
            auto* extraGlyphs = extraGlyphEntry != gNumberedExtraLetters.end() ? &extraGlyphEntry->second : nullptr;

            //gLog.FormattedMessage("\nCall PrepText");

            if (!apOrigString)
                return;

            //gLog.FormattedMessage("apOrigString = %s", apOrigString);

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
            // 0xEC6130
            sourceTextLen = strlen(apOrigString);
            //gLog.FormattedMessage("sourceTextLen = %u", sourceTextLen);
            maxAllowedLines = axData->iLineEnd;

            //gLog.FormattedMessage("Init originalTextBuffer");
            //gLog.FormattedMessage("Allocating originalTextBuffer: size=%d", sourceTextLen + 4);
            originalTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Allocate(sourceTextLen + 4));
            if (!originalTextBuffer) {
                //gLog.FormattedMessage("Memory allocation failed for originalTextBuffer");
                return;
            }

            //gLog.FormattedMessage("MemSet originalTextBuffer");
            // 0xEC61C0
            memset(originalTextBuffer, 0, sourceTextLen + 4);

            //gLog.FormattedMessage("Init processedOriginalText");
            processedOriginalText = originalTextBuffer;


            //gLog.FormattedMessage("Init processedTextBuffer");
            //gLog.FormattedMessage("Allocating processedTextBuffer: size=%u", sourceTextLen + 4);
            processedTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Allocate(sourceTextLen + 4));
            if (!processedTextBuffer) {
                //gLog.FormattedMessage("Memory allocation failed for processedTextBuffer");
                MemoryManager_s_Instance->Deallocate(originalTextBuffer);
                return;
            }

            //gLog.FormattedMessage("MemSet processedTextBuffer");
            // 0xEC61C0
            memset(processedTextBuffer, 0, sourceTextLen + 4);

            //gLog.FormattedMessage("Init dynamicTextBuffer");
            dynamicTextBuffer = processedTextBuffer;

            //gLog.FormattedMessage("SafeFormatString originalTextBuffer");
            // 0xEC623A
            snprintf(originalTextBuffer, sourceTextLen + 1, "%s", apOrigString);
            //gLog.FormattedMessage("Buffer Init Finish");

            processedTextLen = 0;
            textBufferSize = sourceTextLen + 4;
            hyphenInsertCount = 0;
            isTildeChar = 0;
            parsedTextBuffer[0] = 0;
            hasEscapeSequence = 0;
            //gLog.FormattedMessage("processedOriginalText Read Start");
            for (srcTextIndex = 0; srcTextIndex < sourceTextLen; ++srcTextIndex)
            {
                if (processedOriginalText[srcTextIndex] == '&')
                {
                    //gLog.FormattedMessage("Found replaceable text");
                    varNameLen = 0;
                    escapeSeqPrefixLen = 1;
                    isPositiveEscape = 1;
                    if (processedOriginalText[srcTextIndex + 1] == '-')
                    {
                        isPositiveEscape = 0;
                        escapeSeqPrefixLen = 2;
                    }
                    //gLog.FormattedMessage("Replace replaceable varname");
                    while (processedOriginalText[escapeSeqPrefixLen + varNameLen + srcTextIndex]
                        && varNameLen < 127
                        && processedOriginalText[varNameLen + srcTextIndex] != ';'
                        && processedOriginalText[varNameLen + srcTextIndex] != '\n'
                        && processedOriginalText[varNameLen + srcTextIndex] != axData->cLineSep)
                    {
                        varNameBuffer[varNameLen] = processedOriginalText[escapeSeqPrefixLen + varNameLen + srcTextIndex];
                        ++varNameLen;
                    }
                    //gLog.FormattedMessage("Replace replaceable varname end");
                    if (varNameLen)
                        escapeSeqEffectiveLen = varNameLen - escapeSeqPrefixLen;
                    else
                        escapeSeqEffectiveLen = 0;
                    //gLog.FormattedMessage("Finish varNameBuffer string");
                    varNameBuffer[escapeSeqEffectiveLen] = 0;
                    // 0xEC6130
                    //gLog.FormattedMessage("Get varNameBuffer Length");
                    totalEscapeSeqLen = (strlen(varNameBuffer) + 1);
                    //gLog.FormattedMessage("Process ; Length");
                    if (processedOriginalText[varNameLen + srcTextIndex] == ';')
                        totalEscapeSeqLen += escapeSeqPrefixLen;
                    //gLog.FormattedMessage("ReplaceVariableInString & ParseAndFormatVariableInString");
                    if (ReplaceVariableInString(varNameBuffer, parsedTextBuffer, 0x400u, isPositiveEscape)
                        || ParseAndFormatVariableInString(varNameBuffer, parsedTextBuffer))
                    {
                        //gLog.FormattedMessage("ReplaceVariableInString & ParseAndFormatVariableInString Process Start");
                        // 0xEC6130
                        postEscapeTextLen = strlen(parsedTextBuffer);
                        escapeSeqSizeDiff = postEscapeTextLen - totalEscapeSeqLen;
                        if (parsedTextBuffer[postEscapeTextLen - 1] == '\\')
                        {
                            unkarray[0] = 0.0;
                            unkarray[1] = 0.0;
                            unkarray[2] = 0.0;
                            unkarray[3] = 0.0;
                            for (charScanIndex = 0; parsedTextBuffer[charScanIndex] != '\\'; ++charScanIndex)
                                ;
                            substrBuffer[0] = 0;
                            // 0xEC6370
                            //gLog.FormattedMessage("FastStringCopyAligned 1");
                            //strcpy(&parsedTextBuffer[charScanIndex + 1], substrBuffer);
                            strcpy_s(&parsedTextBuffer[charScanIndex + 1],
                                sizeof(parsedTextBuffer) - (charScanIndex + 1),
                                substrBuffer);
                            //gLog.FormattedMessage("FastStringCopyAligned 1 End");
                            // 0xEC6130
                            UInt32 strLen = strlen(substrBuffer);
                            *((char*)unkarray + strLen + 15) = 0;
                            if (this->iFontNum == 7)
                            {
                                // 0xEC65A6
                                strncpy_s(textureNameBuffer, sizeof(textureNameBuffer), substrBuffer, _TRUNCATE);
                                // 0x406D00
                                vsnprintf(substrBuffer, sizeof(substrBuffer), "glow_%s", textureNameBuffer);
                            }
                            this->AddTextIcon(substrBuffer);
                            parsedTextBuffer[charScanIndex] = 1;
                            parsedTextBuffer[charScanIndex + 1] = 0;
                            // 0xEC6130
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
                        //gLog.FormattedMessage("ReplaceVariableInString & ParseAndFormatVariableInString Process End");
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
            //gLog.FormattedMessage("processedOriginalText Read Finish");
            dynamicTextBuffer[processedTextLen] = 0;
            if (hasEscapeSequence)
            {
                sourceTextLen = processedTextLen;
                //gLog.FormattedMessage("Relocating processedOriginalText: size=%u", processedTextLen + 4);
                processedOriginalText = static_cast<char*>(MemoryManager_s_Instance->Reallocate(processedOriginalText, processedTextLen + 4));
                // 0xEC6370
                //gLog.FormattedMessage("FastStringCopyAligned");
                //strcpy(processedOriginalText, dynamicTextBuffer);
                strcpy_s(processedOriginalText, processedTextLen + 4, dynamicTextBuffer);
            }
            *dynamicTextBuffer = 0;
            processedTextLen = 0;
            buttonIconIndex = 0;

            //gLog.FormattedMessage("processedOriginalText Read 2 Start");
            for (charIndex = 0; charIndex < sourceTextLen && processedOriginalText[charIndex]; ++charIndex)
            {
                if (processedOriginalText[charIndex] == axData->cLineSep)
                {
                    dynamicTextBuffer[processedTextLen] = axData->cLineSep;
                    origConsumed += 1;
                    if (++processedTextLen >= textBufferSize)
                    {
                        //gLog.FormattedMessage("Relocating dynamicTextBuffer: size=%u", processedTextLen + 4);
                        dynamicTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Reallocate(dynamicTextBuffer, processedTextLen + 4));
                        textBufferSize = processedTextLen + 4;
                    }
                    totalTextHeight = this->pFontData->fBaseLine + lineSpacingAdjust + totalTextHeight;
                    //gLog.FormattedMessage("AppendToListTail: %d", currentLineWidth);
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
                    bHanzi = false;
                    if (extraGlyphs) {
                        if ((charIndex + 1) <= sourceTextLen) {
                            if ((unsigned char)processedOriginalText[charIndex] >= 0x81) {
                                bHanzi = TryDecodeGBK((const char*)&processedOriginalText[charIndex], uiGBKcode);
                            }
                        }
                    }

                    if (processedOriginalText[charIndex] == '\t')
                    {
                        currentLineWidth += 75 - currentLineWidth % 75;
                        origConsumed += 1;
                        continue;
                    }

                    if (!bHanzi) {
                        currentChar = processedOriginalText[charIndex];
                        origConsumed += 1;
                        //gLog.FormattedMessage("ConvertToAsciiQuotes: '%c'", currentChar);
                        ConvertToAsciiQuotes(&currentChar);
                        //gLog.FormattedMessage("ConvertToAsciiQuotes: '%c' end", currentChar);
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
                        //gLog.FormattedMessage("charWidthWithKerning: %d", charWidthWithKerning);
                        currentLineWidth += charWidthWithKerning;
                        /*if (currentChar == ' ')
                        {
                            lastWrapPosition = processedTextLen;
                            spaceCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
                            preSpaceWidth = currentLineWidth - spaceCharWidth;
                            postSpaceWidth = currentLineWidth;
                            isTildeChar = 0;
                        }*/
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
                        auto glyphIt = extraGlyphs->find(uiGBKcode);
                        if (glyphIt != extraGlyphs->end()) {
                            pCurrentGlyph = &glyphIt->second;
                            charWidthWithKerning = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
                            //gLog.FormattedMessage("HanziWidthWithKerning: %d", charWidthWithKerning);
                            currentLineWidth += charWidthWithKerning;
                        }
                    }

                    //gLog.FormattedMessage("currentChar '%c' process end", currentChar);

                    if (currentLineWidth > axData->iWidth)
                    {
                        if (lastWrapPosition)
                        {
                            if (isTildeChar)
                            {
                                isTildeChar = 0;
                                textBufferSize += 4;
                                dynamicTextBuffer = static_cast<char*>(MemoryManager_s_Instance->Reallocate(dynamicTextBuffer, textBufferSize + 1));

                                // 0xEC7230
                                /*memmove(
                                    &dynamicTextBuffer[lastWrapPosition + 2],
                                    &dynamicTextBuffer[lastWrapPosition],
                                    processedTextLen - lastWrapPosition);
                                dynamicTextBuffer[lastWrapPosition + 1] = '\n';
                                dynamicTextBuffer[lastWrapPosition] = '-';
                                processedTextLen += 2;
                                hyphenInsertCount += 2;
                                pCurrentGlyph = &this->pFontData->pFontLetters['-'];
                                totalTextHeight = this->pFontData->fBaseLine + lineSpacingAdjust + totalTextHeight;
                                hyphenCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
                                currentLineWidth -= hyphenCharWidth;*/

                                UInt32 insertPos = lastWrapPosition;

                                if (insertPos > 0) {
                                    unsigned char prevByte = (unsigned char)dynamicTextBuffer[insertPos - 1];
                                    if (IsGBKLeadByte(prevByte)) {
                                        insertPos -= 1;
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

                                bLastIsHanzi = false;
                                if (extraGlyphs) {
                                    if (IsGBKTrailByte((dynamicTextBuffer[processedTextLen - 1]))) {
                                        if (TryDecodeGBK(&dynamicTextBuffer[processedTextLen - 2], uiTempGBKcode)) {
                                            auto glyphIt2 = extraGlyphs->find(uiTempGBKcode);
                                            if (glyphIt2 != extraGlyphs->end()) {
                                                pCurrentGlyph = &glyphIt2->second;
                                                bLastIsHanzi = true;
                                            }
                                        }
                                    }
                                }

                                if (!bLastIsHanzi) {
                                    pCurrentGlyph = &this->pFontData->pFontLetters[dynamicTextBuffer[processedTextLen - 1]];
                                }
                                currentLineWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);

                                if (bHanzi) {
                                    auto glyphIt3 = extraGlyphs->find(uiGBKcode);
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

                            /*dynamicTextBuffer[processedTextLen + 1] = dynamicTextBuffer[processedTextLen];
                            dynamicTextBuffer[processedTextLen] = dynamicTextBuffer[processedTextLen - 1];*/

                            //qmemcpy
                            /*memcpy(&dynamicTextBuffer[processedTextLen - 1], "-\n", 2);
                            processedTextLen += 2;
                            hyphenInsertCount += 2;
                            pCurrentGlyph = &this->pFontData->pFontLetters['-'];
                            totalTextHeight = this->pFontData->fBaseLine + lineSpacingAdjust + totalTextHeight;
                            hyphenInsertWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
                            currentLineWidth -= hyphenInsertWidth;*/

                            UInt32 tailStart = processedTextLen - 1;
                            UInt32 tailBytes = 2;

                            if (processedTextLen >= 2) {
                                unsigned char cmsb = (unsigned char)dynamicTextBuffer[processedTextLen - 2];
                                unsigned char clsb = (unsigned char)dynamicTextBuffer[processedTextLen - 1];

                                if (extraGlyphs) {
                                    if (IsGBKLeadByte(cmsb) && IsGBKTrailByte(clsb)) {
                                        tailStart = processedTextLen - 2;
                                        tailBytes = 3;
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

                            bLastIsHanzi = false;
                            if (extraGlyphs) {
                                if (IsGBKTrailByte(dynamicTextBuffer[processedTextLen - 1])) {
                                    if (TryDecodeGBK((const char*)&dynamicTextBuffer[processedTextLen - 2], uiTempGBKcode)) {
                                        auto glyphIt2 = extraGlyphs->find(uiTempGBKcode);
                                        if (glyphIt2 != extraGlyphs->end()) {
                                            pCurrentGlyph = &glyphIt2->second;
                                            bLastIsHanzi = true;
                                        }
                                    }
                                }
                            }

                            if (!bLastIsHanzi) {
                                pCurrentGlyph = &this->pFontData->pFontLetters[dynamicTextBuffer[processedTextLen - 1]];
                            }

                            currentLineWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);

                            if (bHanzi) {
                                auto glyphIt3 = extraGlyphs->find(uiGBKcode);
                                if (glyphIt3 != extraGlyphs->end()) {
                                    pCurrentGlyph = &glyphIt3->second;
                                }
                            }
                            else {
                                pCurrentGlyph = &this->pFontData->pFontLetters[currentChar];
                            }

                            combinedCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);

                            currentLineWidth += combinedCharWidth;
                        }
                    }

                    if (bHanzi)
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
                        /*bHanzi = false;
                        if (extraGlyphs) {
                            if (processedTextLen >= 2) {
                                bHanzi = TryDecodeGBK((const char*)&dynamicTextBuffer[processedTextLen - 2], uiTempGBKcode);
                                if (bHanzi) {
                                        --processedTextLen;
                                        --processedTextLen;
                                        continue;
                                }
                            }
                        }

                        if (!bHanzi) {
                            --processedTextLen;
                        }*/
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

            //gLog.FormattedMessage("processedOriginalText Read 2 End");

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
                //strcpy
                strcpy_s(dynamicTextBuffer, textBufferSize, " ");
                origConsumed = 1;
                processedTextLen = 1;
                currentLineCount = 1;
                totalTextHeight = this->pFontData->pFontLetters[' '].fHeight;
                currentLineWidth = ConditionalFloatToUInt(this->pFontData->pFontLetters[' '].fWidth);
            }
            //gLog.FormattedMessage("AppendToListTail");
            AppendToListTail(&axData->xLineWidths, &currentLineWidth);
            //gLog.FormattedMessage("AppendToListTail End");
            if (maxLineWidth <= currentLineWidth)
                finalMaxLineWidth = currentLineWidth;
            else
                finalMaxLineWidth = maxLineWidth;
            maxLineWidth = finalMaxLineWidth;
            dynamicTextBuffer[processedTextLen] = 0;
            //gLog.FormattedMessage("Write axData");
            //gLog.FormattedMessage("dynamicTextBuffer: %s",(const char*)dynamicTextBuffer);
            axData->xNewText.Set(dynamicTextBuffer, 0);
            //gLog.FormattedMessage("Write Finish");
            axData->iWidth = maxLineWidth;
            axData->iHeight = totalTextHeight;
            axData->iLineStart = 0;
            axData->iLineEnd = currentLineCount;
            axData->iCharCount = processedTextLen;
            //gLog.FormattedMessage("Deallocate");
            MemoryManager_s_Instance->Deallocate(processedOriginalText);
            MemoryManager_s_Instance->Deallocate(dynamicTextBuffer);
            //gLog.FormattedMessage("PrepText End");
        }

        UInt32 CreateText(
            BSStringT<char>* axTextString,
            int* aiWidth,
            int* aiHeight,
            int aiLineStart,
            int aiLineEnd,
            int aiFlags,
            char aiLineBreakChar,
            const NiColorA* axFontColor,
            UINT32** apTextShape,
            UINT32** apIconShape
        ) {
            double v11; // st7
            UINT32* v12; // ecx
            UINT32* v13; // ecx
            double axPos__3; // st7
            int v16; // [esp+14h] [ebp-164h]
            float axPos__4; // [esp+1Ch] [ebp-15Ch]
            int v19; // [esp+90h] [ebp-E8h]
            signed int v20; // [esp+94h] [ebp-E4h]
            int m_item_2; // [esp+A4h] [ebp-D4h]
            BSSimpleList<int>* m_pkNext_0; // [esp+A8h] [ebp-D0h]
            int _1_1; // [esp+ACh] [ebp-CCh]
            int m_item_3; // [esp+B0h] [ebp-C8h]
            BSSimpleList<int>* m_pkNext_1; // [esp+B4h] [ebp-C4h]
            int _1; // [esp+B8h] [ebp-C0h]
            int m_item_1; // [esp+D0h] [ebp-A8h]
            int m_item; // [esp+DCh] [ebp-9Ch]
            unsigned __int8 cCurrentChar; // [esp+107h] [ebp-71h] BYREF
            int cCurrentChar_1; // [esp+108h] [ebp-70h]
            int axPos__1; // [esp+10Ch] [ebp-6Ch]
            NiPoint3 axPos_; // [esp+110h] [ebp-68h] BYREF
            float axPos__2; // [esp+11Ch] [ebp-5Ch]
            Font::TextData axData; // [esp+120h] [ebp-58h] BYREF
            int j_1; // [esp+148h] [ebp-30h]
            Font::TextData axData2; // [esp+14Ch] [ebp-2Ch]
            int v37; // [esp+174h] [ebp-4h]
            bool bHanzi, rendered;
            unsigned char cLSB;
            int iActualCharCount;
            UInt32 uiGBKcode;
            auto extraGlyphEntry = gNumberedExtraLetters.find(this->iFontNum);
            auto* extraGlyphs = extraGlyphEntry != gNumberedExtraLetters.end() ? &extraGlyphEntry->second : nullptr;

            //gLog.FormattedMessage("\nCall Font::CreateText");
            if (!*aiHeight)
                *aiHeight = 0x7FFFFFFF;
            if (!aiLineEnd)
                aiLineEnd = 0x7FFFFFFF;
            *(float*)&axData2.iLineStart = FontManagerGetLinePadding(this->iFontNum);
            ThisStdCall(0x759330, &axData, *aiWidth, *aiHeight, aiLineStart, aiLineEnd, aiLineBreakChar);
            v37 = 0;

            //gLog.FormattedMessage("axTextString->pString: '%s'", (const char*)axTextString->pString);
            //gLog.FormattedMessage("Call Font::PrepText");
            ThisStdCall(0xA12FB0, this, axTextString->pString, &axData);
            //gLog.FormattedMessage("Font::PrepText End");

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
                    bHanzi = false;

                    cLSB = (unsigned char)axData.xNewText.pString[Charcount + 1];
                    if (cLSB != 0) {
                        bHanzi = TryDecodeGBK((const char*)&axData.xNewText.pString[Charcount], uiGBKcode);
                    }
                    if (bHanzi) {
                        ++Charcount;
                        iActualCharCount = iActualCharCount - 1;
                    }
                }
            }
            //axData.iCharCount = iActualCharCount;

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

                bHanzi = false;

                if (extraGlyphs) {
                    cLSB = (unsigned char)axData.xNewText.pString[axData2.iCharCount + 1];
                    if (cLSB != 0) {
                        bHanzi = TryDecodeGBK((const char*)&axData.xNewText.pString[axData2.iCharCount], uiGBKcode);
                    }
                    else {
                        bHanzi = false;
                    }
                }

                if (!bHanzi) {
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
                        //gLog.FormattedMessage("Found extraGlyphs");
                        if (bHanzi) {
                            //gLog.FormattedMessage("Find GBKByte");
                            auto glyphIt = extraGlyphs->find(uiGBKcode);
                            if (glyphIt != extraGlyphs->end()) {
                                //Call Font::AddChar
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
                            /*FontAddChar(
                                &this->pFontData->pFontLetters[cCurrentChar],
                                axData2.iLineEnd++,
                                (NiTriShape*)*apTextShape,
                                &axPos_.x,
                                axFontColor);*/
                        }
                    }
                    else {
                        //gLog.FormattedMessage("extraGlyphs Not Found");
                    }
                    if (!rendered) {
                        //Call Font::AddChar
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
            //gLog.FormattedMessage("Font::CreateText End\n");
            return ThisStdCall(0x7593E0, (char*)&axData);
        }

        UINT32* MakeString(
            float afStartX,
            float afStartY,
            float afZ,
            BSStringT<char>* apTextString,
            int* aiWidth,
            bool abPrepareObject,
            const NiColorA* arg1C,
            bool abUpperLeftCorner,
            bool abPrepareObject_1) {

            //gLog.FormattedMessage("Call Font::MakeString");
            //gLog.FormattedMessage("apTextString: %s", (const char*)apTextString->pString);

            double v11; // st7
            UINT32* NiTriShape_1; // eax
            double arg4__4; // st7
            int pString_1; // [esp+10h] [ebp-ACh]
            float arg4__5; // [esp+14h] [ebp-A8h]
            char n10; // [esp+18h] [ebp-A4h]
            UINT32 i_1; // [esp+1Ch] [ebp-A0h]
            UINT32 sLen; // [esp+20h] [ebp-9Ch]
            int pString; // [esp+3Ch] [ebp-80h]
            signed int pString_2; // [esp+40h] [ebp-7Ch]
            char _54__1[4]; // [esp+5Ch] [ebp-60h] BYREF
            float arg4__1; // [esp+60h] [ebp-5Ch] BYREF
            unsigned __int8 cCurrentChar; // [esp+67h] [ebp-55h] BYREF
            int v25; // [esp+68h] [ebp-54h]
            //FontLetter* apLetter; // [esp+6Ch] [ebp-50h]
            UINT32 i; // [esp+70h] [ebp-4Ch]
            float arg4__2; // [esp+74h] [ebp-48h] BYREF
            float afZ_1; // [esp+78h] [ebp-44h]
            float afStartY_1; // [esp+7Ch] [ebp-40h]
            UINT32* NiTriShape_0; // [esp+80h] [ebp-3Ch]
            float arg4__3; // [esp+84h] [ebp-38h]
            NiColorA* apColor; // [esp+88h] [ebp-34h]
            float v34[4]; // [esp+8Ch] [ebp-30h] BYREF
            char _54_[4]; // [esp+9Ch] [ebp-20h] BYREF
            float afStartX_1; // [esp+A0h] [ebp-1Ch]
            float afZ_2; // [esp+A4h] [ebp-18h]
            float afStartY_2; // [esp+A8h] [ebp-14h]
            float arg4; // [esp+ACh] [ebp-10h] BYREF
            int aiVert; // [esp+B0h] [ebp-Ch]
            int j; // [esp+B4h] [ebp-8h]
            float* v42; // [esp+B8h] [ebp-4h]

            bool bHanzi, rendered;
            unsigned char cLSB;
            int iActualCharCount;
            UInt32 uiGBKcode;
            auto extraGlyphEntry = gNumberedExtraLetters.find(this->iFontNum);
            auto* extraGlyphs = extraGlyphEntry != gNumberedExtraLetters.end() ? &extraGlyphEntry->second : nullptr;

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
                    bHanzi = false;

                    if (Charcount >= i_1) {
                        break;
                    }

                    cLSB = (unsigned char)apTextString->pString[Charcount + 1];
                    if (cLSB != 0) {
                        bHanzi = TryDecodeGBK((const char*)&apTextString->pString[Charcount], uiGBKcode);
                    }
                    if (bHanzi) {
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

                bHanzi = false;

                if (extraGlyphs) {
                    cLSB = (unsigned char)apTextString->pString[j + 1];
                    if (cLSB != 0) {
                        bHanzi = TryDecodeGBK((const char*)&apTextString->pString[j], uiGBKcode);
                    }
                    else {
                        bHanzi = false;
                    }
                }

                if (!bHanzi) {
                    ConvertToAsciiQuotes(&cCurrentChar);
                }

                rendered = false;

                v25 = cCurrentChar;

                //apLetter = &this->pFontData->pFontLetters[cCurrentChar];
                //Font::AddChar
                //StdCall<FontLetter*>(0xA142D0, apLetter, aiVert++, (NiTriShape*)NiTriShape_0, &arg4__2, apColor);

                if (extraGlyphs) {
                    //gLog.FormattedMessage("Found extraGlyphs");
                    if (bHanzi) {
                        //gLog.FormattedMessage("Find GBKByte");
                        auto glyphIt = extraGlyphs->find(uiGBKcode);
                        if (glyphIt != extraGlyphs->end()) {
                            //Call Font::AddChar
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
                        /*FontAddChar(
                            &this->pFontData->pFontLetters[cCurrentChar],
                            axData2.iLineEnd++,
                            (NiTriShape*)*apTextShape,
                            &axPos_.x,
                            axFontColor);*/
                    }
                }
                else {
                    //gLog.FormattedMessage("extraGlyphs Not Found");
                }
                if (!rendered) {
                    //Call Font::AddChar
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
            //NiBound::ComputeFromData
            ThisStdCall(
                0xA7EE30,
                (float*)(NiTriShape_0[46] + 16),
                *(unsigned __int16*)(NiTriShape_0[46] + 8), v42);
            return NiTriShape_0;
        }
    };

    class FontManagerEx : public FontManager {
    public:
        //	outDims.x := width (pxl); outDims.y := height (pxl); outDims.z := numLines
        NiPoint3* __thiscall CalculateStringDimensions(NiPoint3* outDimensions, const char* srcString, UInt32 fontID, float maxWrapWidth, UInt32 startCharIndex) {
            double tabStopWidth; // st7
            float finalMaxLineWidth; // [esp+10h] [ebp-4Ch]
            float adjustedWrapWidth; // [esp+14h] [ebp-48h]
            float previousLineWidthAtTabStop; // [esp+18h] [ebp-44h]
            unsigned __int8 currentChar; // [esp+27h] [ebp-35h] BYREF
            float currentCharTotalWidth; // [esp+28h] [ebp-34h]
            signed int currentCharIndex; // [esp+2Ch] [ebp-30h]
            float previousCharTotalWidth; // [esp+30h] [ebp-2Ch]
            int totalLines; // [esp+34h] [ebp-28h]
            char hasHyphenationPoint; // [esp+3Bh] [ebp-21h]
            NiPoint3 StringDimensions; // [esp+3Ch] [ebp-20h]
            float lastValidWrapPosition; // [esp+48h] [ebp-14h]
            float currentLineWidth; // [esp+4Ch] [ebp-10h]
            int sourceStringLength; // [esp+50h] [ebp-Ch]
            FontLetter* fontCharMetrics; // [esp+54h] [ebp-8h]
            float fontVerticalSpacingAdjust; // [esp+58h] [ebp-4h]

            bool bHanzi;
            UInt32 uiGBKcode;
            auto extraGlyphEntry = gNumberedExtraLetters.find(fontID);
            auto* extraGlyphs = extraGlyphEntry != gNumberedExtraLetters.end() ? &extraGlyphEntry->second : nullptr;

            //gLog.FormattedMessage("Call GetStringDimensionsEx");
            //gLog.FormattedMessage("fontID: %u", fontID);

            if (fontID >= 1 /*&& fontID <= 8*/ && srcString)
            {
                StringDimensions = StringDefaultDimensions;
                // 0xEC6130
                //gLog.FormattedMessage("Call strlen");
                sourceStringLength = strlen(srcString);
                //gLog.FormattedMessage("sourceStringLength: %u", sourceStringLength);
                fontCharMetrics = this->pFont[fontID - 1]->pFontData->pFontLetters;
                lastValidWrapPosition = 0.0;
                currentLineWidth = 0.0;
                //gLog.FormattedMessage("Call VertSpacingAdjust");
                fontVerticalSpacingAdjust = FontManagerGetLinePadding(fontID);
                //gLog.FormattedMessage("fontVerticalSpacingAdjust: %f", fontVerticalSpacingAdjust);
                previousCharTotalWidth = 0.0;
                hasHyphenationPoint = 0;
                totalLines = 1;
                StringDimensions.y = fontCharMetrics[' '].fHeight;

                //gLog.FormattedMessage("GetStringDimensionsEx srcString: '%s'", srcString);

                for (currentCharIndex = startCharIndex; currentCharIndex < sourceStringLength; ++currentCharIndex)
                {
                    bHanzi = false;

                    currentChar = srcString[currentCharIndex];
                    currentCharTotalWidth = 0.0;

                    if (extraGlyphs) {
                        if (bIsQuestTextMSBHanzi) {
                            if (szGBKChar) {
                                srcString = szGBKChar;
                            }
                        }

                        if ((currentCharIndex + 1) <= sourceStringLength) {
                            if ((unsigned char)srcString[currentCharIndex] >= 0x81) {
                                bHanzi = TryDecodeGBK(&srcString[currentCharIndex], uiGBKcode);
                                if (bIsQuestTextMSBHanzi) {
                                    srcString = "";
                                }
                            }
                        }
                    }

                    if (bHanzi) {
                        //gLog.FormattedMessage("CalculateStringDimensions found Hanzi");
                        auto glyphIt = extraGlyphs->find(uiGBKcode);
                        if (glyphIt != extraGlyphs->end()) {
                            currentCharTotalWidth = glyphIt->second.fLeadingEdge
                                + glyphIt->second.fWidth
                                + glyphIt->second.fSpacing;
                        }
                        ++currentCharIndex;
                    }
                    else {
                        //gLog.FormattedMessage("Call ConvertToAsciiQuotes");
                        ConvertToAsciiQuotes(&currentChar);
                        //gLog.FormattedMessage("Call ConvertToAsciiQuotes end");
                        currentCharTotalWidth = fontCharMetrics[currentChar].fLeadingEdge
                            + fontCharMetrics[currentChar].fWidth
                            + fontCharMetrics[currentChar].fSpacing;
                        switch (currentChar)
                        {
                        case '\t':
                            tabStopWidth = currentLineWidth;
                            AlignLineWidthToTab(currentLineWidth, 75.0);
                            previousLineWidthAtTabStop = tabStopWidth;
                            currentCharTotalWidth = 75.0 - previousLineWidthAtTabStop;
                            break;
                        case '\n':
                        case ' ':
                            /*lastValidWrapPosition = currentLineWidth;
                            hasHyphenationPoint = 0;*/
                            break;
                        case '~':
                            lastValidWrapPosition = currentLineWidth
                                /*+ fontCharMetrics['-'].fLeadingEdge
                                + fontCharMetrics['-'].fWidth
                                + fontCharMetrics['-'].fSpacing*/;
                            hasHyphenationPoint = 1;
                            break;
                        default:
                            break;
                        }
                    }
                    
                    if (currentChar != '~')
                        currentLineWidth = currentLineWidth + currentCharTotalWidth;
                    if (maxWrapWidth < currentLineWidth || currentChar == '\n')
                    {
                        if (lastValidWrapPosition <= 0.0)
                        {
                            lastValidWrapPosition = currentLineWidth
                                - currentCharTotalWidth
                                - previousCharTotalWidth
                                /*+ fontCharMetrics['-'].fLeadingEdge
                                + fontCharMetrics['-'].fWidth
                                + fontCharMetrics['-'].fSpacing*/;
                            currentLineWidth = currentCharTotalWidth + previousCharTotalWidth;
                        }
                        else
                        {
                            currentLineWidth = currentLineWidth - lastValidWrapPosition;
                            if (!hasHyphenationPoint)
                            {
                                if (currentChar == '\n')
                                    currentLineWidth = 0.0;
                                /*else
                                    currentLineWidth = currentLineWidth
                                    - (fontCharMetrics[' '].fLeadingEdge
                                        + fontCharMetrics[' '].fWidth
                                        + fontCharMetrics[' '].fSpacing);*/
                            }
                        }
                        if (lastValidWrapPosition >= StringDimensions.x)
                            adjustedWrapWidth = lastValidWrapPosition;
                        else
                            adjustedWrapWidth = StringDimensions.x;
                        StringDimensions.x = adjustedWrapWidth;
                        StringDimensions.y = fontVerticalSpacingAdjust
                            + this->pFont[fontID - 1]->pFontData->fBaseLine
                            + StringDimensions.y;
                        lastValidWrapPosition = 0.0;
                        ++totalLines;
                    }
                    previousCharTotalWidth = currentCharTotalWidth;
                }
                if (currentLineWidth >= StringDimensions.x)
                    finalMaxLineWidth = currentLineWidth;
                else
                    finalMaxLineWidth = StringDimensions.x;
                StringDimensions.z = totalLines;
                outDimensions->x = finalMaxLineWidth;
                outDimensions->y = StringDimensions.y;
                outDimensions->z = StringDimensions.z;
                return outDimensions;
            }
            else
            {
                *outDimensions = StringDefaultDimensions;
                return outDimensions;
            }
        }
    };

    static void* __fastcall TileSetString(void* pThis, void*, int a2, char* a3, bool a4) {
        //gLog.FormattedMessage("Call QuestText TileSetString");
        //gLog.FormattedMessage("a2: %d", a2);
        //gLog.FormattedMessage("a3: %x", a3[0]);

        bIsQuestTextLSBHanzi = false;
        if (bIsQuestTextMSBHanzi) {
            bIsQuestTextLSBHanzi = IsGBKTrailByte(a3[0]);
            if (bIsQuestTextLSBHanzi) {
                szGBKChar[0] = pFirstChar;
                szGBKChar[1] = a3[0];
                szGBKChar[2] = 0;
                a3 = (char*)szGBKChar;
            }
        }

        if (gNumberedExtraLetters.find(8) != gNumberedExtraLetters.end() && !bIsQuestTextMSBHanzi) {
            bIsQuestTextMSBHanzi = false;
            bIsQuestTextMSBHanzi = IsGBKLeadByte((unsigned char)a3[0]);
            if (bIsQuestTextMSBHanzi) {
                pFirstChar = (unsigned char)a3[0];
                //gLog.FormattedMessage("QuestText FirstByte: 0x%x", pFirstChar);
                a3 = (char*)"";
            }
        }
        else {
            bIsQuestTextMSBHanzi = false;
        }

        return ThisStdCall<void*>(0xA01350, pThis, a2, a3, a4);
    }

    void InitVertSpacingHook() {
        //FontManager::GetLinePadding
        //WriteRelJump(0xA1B3A0, &VertSpacingAdjust);
    }

    void InitFontHook() {
        //Font::Create hook in InitJIPHooks

        // Font::Load
        WriteRelJumpEx(0xA15320, &FontEx::Load);
        // 
        // Font::PrepText
        WriteRelJumpEx(0xA12FB0, &FontEx::PrepText);
        WriteRelCallEx(0x759281, &FontEx::PrepTextForTerminal);
        // 
        // Font::CreateText
        WriteRelJumpEx(0xA12880, &FontEx::CreateText);
        // 
        // Font::MakeString
        WriteRelJumpEx(0xA12460, &FontEx::MakeString);
        // 
        // FontManager::CalculateStringDimensions
        WriteRelJumpEx(0xA1B020, &FontManagerEx::CalculateStringDimensions);

        //QuestText and LocationText
        //Tile::Value::SetString
        WriteRelCall(0x77AF4B, &TileSetString);
        WriteRelCall(0x772B5E, &TileSetString);
    }

    void InitJIPHooks() {
        hJIP = GetModuleHandle("jip_nvse.dll");
        if (!hJIP) {
            //gLog.FormattedMessage("JIP not find");
            //1
            WriteRelCallEx(0xA1695A, &FontEx::FontCreate);
            //2
            WriteRelCallEx(0xA169C7, &FontEx::FontCreate);
            //3
            WriteRelCallEx(0xA16A35, &FontEx::FontCreate);
            //4
            WriteRelCallEx(0xA16AA3, &FontEx::FontCreate);
            //5
            WriteRelCallEx(0xA16B11, &FontEx::FontCreate);
            //6
            WriteRelCallEx(0xA16B7F, &FontEx::FontCreate);
            //7
            WriteRelCallEx(0xA16BED, &FontEx::FontCreate);
            //8
            WriteRelCallEx(0xA16C5B, &FontEx::FontCreate);
            return;
        }
        //gLog.FormattedMessage("hook JIP Font::Font");
        SafeWrite32(GetJIPAddress(0x10011A3E + 1), uint32_t(FontCreateForJIP));
        SafeWrite32(GetJIPAddress(0x10011AA9 + 1), uint32_t(FontCreateForJIP));
        SafeWrite32(GetJIPAddress(0x1003943F + 1), uint32_t(FontCreateForJIP));
    }
}
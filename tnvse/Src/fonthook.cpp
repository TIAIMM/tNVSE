#pragma once
#include <cstring>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include "BSFile.hpp"
#include "BSSimpleList.hpp"
#include "NiPoint3.hpp"
#include "NiTriShape.hpp"
#include "MemoryManager.hpp"
#include "uidecode.h"
#include "fonthook.h"

namespace fonthook {
    static void __cdecl ConvertToAsciiQuotes(UInt8* currentChar) {
        CdeclCall(0xA122B0, currentChar);
    }

    static bool __cdecl ReplaceVariableInString(const char* varName, char* outBuffer, UInt32 bufferSize, bool isPositiveEscape) {
        return CdeclCall<bool>(0x7070C0, varName, outBuffer, bufferSize, isPositiveEscape);
    }

    static bool __cdecl ParseAndFormatVariableInString(const char* p_varNameBuffer, void* p_parsedTextBuffer) {
        return CdeclCall<bool>(0x7073D0, p_varNameBuffer, p_parsedTextBuffer);
    }

    static SInt32 __cdecl AlignLineWidthToTab(double a1, double a2) {
        return CdeclCall<SInt32>(0xEC9130, a1, a2);
    }

    static void* __cdecl AppendToListTail(void* ListNode, void* ListNode2) {
        return ThisStdCall<void*>(0xAF25B0, ListNode, ListNode2);
    }

    // 0xEC62F6
    static uint32_t SafeDoubleToUInt32(double a1)
    {
        uint64_t bits;
        std::memcpy(&bits, &a1, sizeof(bits));

        uint32_t low = static_cast<uint32_t>(bits);
        uint32_t high = static_cast<uint32_t>(bits >> 32);

        if (low == 0 && (high & 0x7FFFFFFF) == 0)
            return 0;

        bool sign = (high & 0x80000000u) != 0;

        if (!sign)
        {
            return low;
        }
        else
        {
            uint64_t pair64 = (static_cast<uint64_t>(0) << 32) | low;
            return static_cast<uint32_t>((pair64 + 0x7FFFFFFF) >> 32);
        }
    }

    // 0xEC62C0
    static uint32_t ConditionalFloatToUInt(double a1)
    {
        if (*(volatile uint32_t*)0x01270A6C)
            return static_cast<uint32_t>(a1);
        else
            return SafeDoubleToUInt32(a1);
    }

    static Float32 __stdcall FontManagerGetLinePadding(UInt32 fontID) {
        return StdCall<Float32>(0xA1B3A0, fontID);
    }

    static Float32 __stdcall VertSpacingAdjust(UInt32 fontID) {
        return 0;
    }

    static bool IsGB2312LeadByte(unsigned char c) {
        return (c >= 0xA1 && c <= 0xF7);
    }

    typedef uint32_t(__thiscall* GetFileSizeFunc)(void* pThis);

    static uint32_t GetFileSize(void* fntFileHandle) {
        void** vtable = *(void***)fntFileHandle;
        GetFileSizeFunc func = (GetFileSizeFunc)vtable[10];
        return func(fntFileHandle);
    }

    static NiSourceTexture* NiSourceTextureCreate(
        NiPixelData* pkPixelData,
        const char* apName,
        NiTexture::FormatPrefs* kPrefs
    ) {
        gLog.FormattedMessage("\nCall NiSourceTexture::Create");
        gLog.FormattedMessage("apName: %s", apName);
        NiSourceTexture* ret = CdeclCall<NiSourceTexture*>(
            0xA5FF10,
            pkPixelData,
            apName,
            kPrefs);
        gLog.FormattedMessage("NiSourceTexture::Create End\n");
        return ret;
    }

    static char* NiGlobalStringTableAddString(const char* pcString) {
        gLog.FormattedMessage("\nCall NiGlobalStringTable::AddString");
        gLog.FormattedMessage("pcString: %s", pcString);
        char* ret = CdeclCall<char*>(
            0xA5B690,
            pcString);
        gLog.FormattedMessage("NiGlobalStringTable::AddString End\n");
        return ret;
    }

    static BSFile* __cdecl LoadFile(const char* filePath, SInt32 loadMode, UInt32 allocFlags, SInt32 openMode) {
        return CdeclCall<BSFile*>(0xAFDF20, filePath, loadMode, allocFlags, openMode);
    }

    static BSFile* FileFinder_GetFile(
        const char* apName,
        NiFile::OpenMode aeMode,
        unsigned int aiSize,
        unsigned int aiArchiveType
    ) {
        gLog.FormattedMessage("\nCall FileFinder::GetFile");
        gLog.FormattedMessage("apName: %s", apName);
        gLog.FormattedMessage("aiSize: %u", aiSize);
        BSFile* ret = CdeclCall<BSFile*>(0xAFDF20, apName, aeMode, aiSize, aiArchiveType);
        gLog.FormattedMessage("FileFinder::GetFile End\n");
        return ret;
    }

    static NiPoint3& StringDefaulDimensions = *reinterpret_cast<NiPoint3*>(0x11F426C);

    static MemoryManager* MemoryManagerSingleton = reinterpret_cast<MemoryManager*>(0x11F6238);

    static MemoryManager* MemoryManager_s_Instance = reinterpret_cast<MemoryManager*>(0x11F6238);

    static std::unordered_map<const char*, std::unordered_map<UInt32, FontLetter>> gExtraFontLetters;

    static std::unordered_map<UInt32, std::unordered_map<UInt32, FontLetter>> gNumberedExtraLetters;

    static const char* fontNameKey;

    static HMODULE hJIP = 0;

    static size_t __fastcall GetJIPAddress(size_t aiAddress) {
        return reinterpret_cast<size_t>(hJIP) + aiAddress - 0x10000000;
    }

    class NiTexturingPropertyEx : public NiTexturingProperty {
    public:
        NiTexturingProperty* NiTexturingPropertyBuild(
            NiPixelData* pkPixelData,
            const NiFixedString* kName,
            NiTexture::FormatPrefs* arPrefs) {
            gLog.FormattedMessage("\nCall NiTexturingPropertyBuild");
            const char* sName = kName->m_kHandle ? kName->m_kHandle : "<null>";

            gLog.FormattedMessage("source: %p", this);
            gLog.FormattedMessage("pkPixelData: %p", pkPixelData);

            gLog.FormattedMessage("kName: %s", sName);
            gLog.FormattedMessage("m_ePixelLayout: %x", arPrefs->m_ePixelLayout);
            gLog.FormattedMessage("m_eAlphaFmt: %x", arPrefs->m_eAlphaFmt);
            gLog.FormattedMessage("m_eMipMapped: %x", arPrefs->m_eMipMapped);

            NiTexturingProperty* ret = ThisStdCall<NiTexturingProperty*>(
                0xA6ABB0,
                this,
                pkPixelData,
                kName,
                arPrefs);
            gLog.FormattedMessage("NiTexturingPropertyBuild End\n");
            return ret;
        }
    };

    NiTexturingProperty* NiTexturingPropertyBuild2(
        NiTexturingProperty* source,
        NiPixelData* pkPixelData,
        const NiFixedString* kName,
        NiTexture::FormatPrefs* arPrefs) {
        gLog.FormattedMessage("\nCall NiTexturingPropertyBuild");
        const char* sName = kName->m_kHandle ? kName->m_kHandle : "<null>";

        gLog.FormattedMessage("source: %p", source);
        gLog.FormattedMessage("pkPixelData: %p", pkPixelData);

        gLog.FormattedMessage("kName: %s", sName);
        gLog.FormattedMessage("m_ePixelLayout: %x", arPrefs->m_ePixelLayout);
        gLog.FormattedMessage("m_eAlphaFmt: %x", arPrefs->m_eAlphaFmt);
        gLog.FormattedMessage("m_eMipMapped: %x", arPrefs->m_eMipMapped);

        NiTexturingProperty* ret = ThisStdCall<NiTexturingProperty*>(
            0xA6ABB0,
            source,
            pkPixelData,
            kName,
            arPrefs);
        gLog.FormattedMessage("NiTexturingPropertyBuild End\n");
        return ret;
    }

    class NiPixelDataEx : public NiPixelData {
    public:
        NiPixelData* NiPixelDataBuild(
            unsigned int uiWidth,
            unsigned int uiHeight,
            const NiPixelFormat* kFormat,
            unsigned int uiMipmapLevels,
            unsigned int uiFaces) {
            gLog.FormattedMessage("\nCall NiPixelDataBuild");
            gLog.FormattedMessage("uiWidth: %u", uiWidth);
            gLog.FormattedMessage("uiHeight: %u", uiHeight);
            gLog.FormattedMessage("kFormat: %p", kFormat);
            gLog.FormattedMessage("uiMipmapLevels: %u", uiMipmapLevels);
            gLog.FormattedMessage("uiFaces: %u", uiFaces);
            NiPixelData* ret = ThisStdCall<NiPixelData*>(
                0xA7C190,
                this,
                uiWidth,
                uiHeight,
                kFormat,
                uiMipmapLevels,
                uiFaces);
            gLog.FormattedMessage("NiPixelDataBuild End\n");
            return ret;
        }
    };

    NiPixelData* NiPixelDataBuild2(
        NiPixelData* source,
        unsigned int uiWidth,
        unsigned int uiHeight,
        const NiPixelFormat* kFormat,
        unsigned int uiMipmapLevels,
        unsigned int uiFaces) {
        gLog.FormattedMessage("\nCall NiPixelDataBuild");
        gLog.FormattedMessage("uiWidth: %u", uiWidth);
        gLog.FormattedMessage("uiHeight: %u", uiHeight);
        gLog.FormattedMessage("kFormat: %p", kFormat);
        gLog.FormattedMessage("uiMipmapLevels: %u", uiMipmapLevels);
        gLog.FormattedMessage("uiFaces: %u", uiFaces);
        NiPixelData* ret = ThisStdCall<NiPixelData*>(
            0xA7C190,
            source,
            uiWidth,
            uiHeight,
            kFormat,
            uiMipmapLevels,
            uiFaces);
        gLog.FormattedMessage("NiPixelDataBuild End\n");
        return ret;
    }

    class NiSourceTextureEx : public NiSourceTexture {
    public:
        char* SetFilename(const NiFixedString* kFilename) {
            gLog.FormattedMessage("\nCall NiSourceTexture::SetFilename");
            gLog.FormattedMessage("kFilename: %p", kFilename);
            char* ret = ThisStdCall<char*>(
                0xA5FBA0,
                this,
                kFilename);
            gLog.FormattedMessage("NiSourceTexture::SetFilename End\n");
            //gLog.FormattedMessage("ret: %s", ret);
            return ret;
        }
    };

    FontLetter* __stdcall FontAddChar(
        FontLetter* apLetter,
        UInt32 aiVert,
        NiTriShape* apShape,
        float* axPos,
        const NiColorA* apColor
    ) {
        //gLog.FormattedMessage("Call AddChar");

        FontLetter* ret = StdCall<FontLetter*>(
            0xA142D0,
            apLetter,
            aiVert,
            apShape,
            axPos,
            apColor);

        return ret;
    }

    class FontEx : public Font {
    public:
        Font* FontCreate(int iFontNum, char* apFilename, bool abLoad) {
            gLog.FormattedMessage("\nCall Font::Font");
            gLog.FormattedMessage("iFontNum: %u", iFontNum);
            gLog.FormattedMessage("apFilename: %s", (const char*)apFilename);
            Font* ret = ThisStdCall<Font*>(
                0xA12020,
                this,
                iFontNum,
                apFilename,
                abLoad);
            gLog.FormattedMessage("Font::Font End\n");
            //gLog.FormattedMessage("ret: %s", ret);
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
            //BSFile* NiBinaryStream_4; // [esp+C8h] [ebp-184h]
            NiFixedString kName_; // [esp+CCh] [ebp-180h] BYREF
            NiTexturingProperty* NiTexturingProperty_3; // [esp+D0h] [ebp-17Ch]
            NiTexturingProperty* NiTexturingProperty_1; // [esp+D4h] [ebp-178h]
            NiPixelData* v36; // [esp+D8h] [ebp-174h]
            NiPixelData* NiPixelData_2; // [esp+DCh] [ebp-170h]
            BSFile* NiBinaryStream_1; // [esp+E0h] [ebp-16Ch]
            //BSFile* NiBinaryStream_2; // [esp+E4h] [ebp-168h]
            BSFile* NiFile_1; // [esp+E8h] [ebp-164h]
            //BSFile* BSFile_2; // [esp+ECh] [ebp-160h]
            FontData* pFontData; // [esp+F0h] [ebp-15Ch]
            BSFile* BSFile_3; // [esp+F4h] [ebp-158h]
            //BSFile* BSFile_4; // [esp+F8h] [ebp-154h]
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

            gLog.FormattedMessage("Call Font::Load");

            __asm {
                mov eax, fs: [0x18]
                mov tebAddress, eax
            }
            tlsPointer = *(DWORD*)(tebAddress + 0x2C);
            tlsSlotAddress = *(DWORD*)(tlsPointer + (*pTlsIndex) * 4);
            targetAddress = tlsSlotAddress + 692;

            gLog.FormattedMessage("Set stringRefFlag");

            stringRefFlag = 0;

            oldTlsValue = *(DWORD*)targetAddress;
            savedTlsValue = oldTlsValue;
            *(DWORD*)targetAddress = 12;

            gLog.FormattedMessage("Initialize stackCookie");

            stackCookie = 0;
            iRefCount = this->iRefCount;
            if (iRefCount || !this->pFontFile)
            {
            LABEL_46:
                gLog.FormattedMessage("LABEL_46");
                ++this->iRefCount;
                stackCookie = -1;
                *(DWORD*)targetAddress = savedTlsValue;
                gLog.FormattedMessage("Font::Load End\n");
                return;
            }
            //gLog.FormattedMessage("Load %s", (const char*)this->pFontFile);
            BSFile_1 = FileFinder_GetFile(this->pFontFile, (NiFile::OpenMode)0, 0x4000000u, 2u);
            //gLog.FormattedMessage("Load %s End", (const char*)this->pFontFile);
            if (BSFile_1)
            {
                if (LOBYTE(BSFile_1->m_pFile))
                {
                    gLog.FormattedMessage("Allocate pFontData");
                    pFontData = (FontData*)MemoryManager_s_Instance->Allocate(0x3928u);
                    gLog.FormattedMessage("Allocate pFontData End");
                    this->pFontData = pFontData;
                    gLog.FormattedMessage("Moved Font Data");
                    //v21 = BSFile_1->GetSize();
                    v21 = 0x3928u;
                    pFontData_1 = this->pFontData;
                    v25[0] = 1;
                    v23 = BSFile_1->m_pfnRead(BSFile_1, pFontData_1, v21, v25, 1u);
                    BSFile_1->m_uiAbsoluteCurrentPos += v23;
                    v24 = v23;

                    unsigned int uiActualSize = BSFile_1->GetSize();
                    if (uiActualSize > 0x3928) {
                        fontNameKey = this->pFontFile ? this->pFontFile : "";
                        auto& extraMap = gExtraFontLetters[fontNameKey];
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

                        if (!extraMap.empty()) {
                            gLog.FormattedMessage("Loaded %u extra FontLetter records for %s",
                                (unsigned)extraMap.size(),
                                fontNameKey);
                        }
                    }

                    NiFile_1 = BSFile_1;
                    //BSFile_2 = BSFile_1;
                    gLog.FormattedMessage("Delete BSFile_2");
                    if (BSFile_1) {
                        //delete(BSFile_2);
                    }
                    gLog.FormattedMessage("Delete BSFile_2 End");
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

                        NiBinaryStream_0 = FileFinder_GetFile((const char*)apName_, (NiFile::OpenMode)0, 0x4000000u, 2u);

                        //gLog.FormattedMessage("Load %s End", (const char*)apName_);

                        if (!NiBinaryStream_0 || !(m_pfnWrite = (bool)NiBinaryStream_0->m_pFile))
                        {
                            if (NiBinaryStream_0)
                            {
                                NiBinaryStream_1 = NiBinaryStream_0;
                                //NiBinaryStream_2 = NiBinaryStream_0;
                                gLog.FormattedMessage("Delete NiBinaryStream_2");
                                //delete(NiBinaryStream_2);
                                gLog.FormattedMessage("Delete NiBinaryStream_2 End");
                            }
                            stackCookie = -1;
                            v13[1] = savedTlsValue;
                            *(DWORD*)targetAddress = savedTlsValue;
                            return;
                        }

                        v13[0] = 1;
                        gLog.FormattedMessage("Read Texture Size");
                        v11 = NiBinaryStream_0->m_pfnRead(NiBinaryStream_0, &a2, 4u, v13, 1u);
                        gLog.FormattedMessage("Length: %u", a2);
                        v11 += NiBinaryStream_0->m_pfnRead(NiBinaryStream_0, &a3, 4u, v13, 1u);
                        gLog.FormattedMessage("Width: %u", a3);
                        NiBinaryStream_0->m_uiAbsoluteCurrentPos += v11;
                        v12 = v11;
                        arPrefs_.m_ePixelLayout = static_cast<NiTexture::FormatPrefs::PixelLayout>(0x6);
                        arPrefs_.m_eAlphaFmt = static_cast<NiTexture::FormatPrefs::AlphaFormat>(0x3);
                        arPrefs_.m_eMipMapped = static_cast<NiTexture::FormatPrefs::MipFlag>(0x2);
                        v36 = (NiPixelData*)NiMemObject::operator new(0x74);
                        stackCookie = (stackCookie & 0xFFFFFF00) | 1;
                        if (v36) {
                            gLog.FormattedMessage("Instancing NiPixelData");
                            NiPixelData_1 = NiPixelDataBuild2(
                                v36,
                                a2,
                                a3,
                                reinterpret_cast<const NiPixelFormat*>(0x11AA2A0),
                                1,
                                1);
                            gLog.FormattedMessage("Instancing NiPixelData End");
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
                        NiTexturingProperty_3 = (NiTexturingProperty*)NiMemObject::operator new(0x30);
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
                            gLog.FormattedMessage("Instancing NiTexturingProperty");
                            /*NiTexturingProperty_4 = static_cast<NiTexturingPropertyEx*>(NiTexturingProperty_3)->NiTexturingPropertyBuild(
                                NiPixelData_3,
                                &kName_,
                                &arPrefs_);*/
                            NiTexturingProperty_4 = NiTexturingPropertyBuild2(
                                NiTexturingProperty_3,
                                NiPixelData_3,
                                &kName_,
                                &arPrefs_);
                            gLog.FormattedMessage("Instancing NiTexturingProperty End");
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
                        //NiBinaryStream_4 = NiBinaryStream_0;
                        if (NiBinaryStream_0) {
                            gLog.FormattedMessage("Delete NiBinaryStream_4");
                            //delete(NiBinaryStream_4);
                            gLog.FormattedMessage("Delete NiBinaryStream_4 End");
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
                        gLog.FormattedMessage("finish load");
                    }
                    goto LABEL_46;
                }
            }
            pFontFile_2 = this->pFontFile;
            if (BSFile_1)
            {
                BSFile_3 = BSFile_1;
                //BSFile_4 = BSFile_1;
                gLog.FormattedMessage("Delete BSFile_4");
                //delete(BSFile_4);
                gLog.FormattedMessage("Delete BSFile_4 End");
            }
            stackCookie = -1;
            v25[1] = savedTlsValue;
            *(DWORD*)targetAddress = savedTlsValue;
        }

        void __thiscall PrepText(const char* apOrigString, Font::TextData* axData) {
            unsigned int charWidthWithKerning; // eax
            unsigned int spaceCharWidth; // eax
            unsigned int tildeCharWidth; // eax
            unsigned int hyphenCharWidth; // eax
            unsigned int nextCharWidth; // eax
            unsigned int hyphenInsertWidth; // eax
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
            char varNameBuffer[33]; // [esp+308h] [ebp-4D8h] BYREF
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

            //gLog.Message("Call CalculateTextLayoutEx");

            if (!apOrigString)
                return;

            //gLog.FormattedMessage("textSrc = '%s'", textSrc);

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

            //gLog.Message("Init originalTextBuffer");
            //gLog.FormattedMessage("Allocating originalTextBuffer: size=%d", sourceTextLen + 4);
            originalTextBuffer = static_cast<char*>(MemoryManagerSingleton->Allocate(sourceTextLen + 4));
            if (!originalTextBuffer) {
                //gLog.Message("Memory allocation failed for originalTextBuffer");
                return;
            }

            //gLog.Message("MemSet originalTextBuffer");
            // 0xEC61C0
            memset(originalTextBuffer, 0, sourceTextLen + 4);

            //gLog.Message("Init processedOriginalText");
            processedOriginalText = originalTextBuffer;


            //gLog.Message("Init processedTextBuffer");
            //gLog.FormattedMessage("Allocating processedTextBuffer: size=%u", sourceTextLen + 4);
            processedTextBuffer = static_cast<char*>(MemoryManagerSingleton->Allocate(sourceTextLen + 4));
            if (!processedTextBuffer) {
                //gLog.Message("Memory allocation failed for processedTextBuffer");
                MemoryManagerSingleton->Deallocate(originalTextBuffer);
                return;
            }

            //gLog.Message("MemSet processedTextBuffer");
            // 0xEC61C0
            memset(processedTextBuffer, 0, sourceTextLen + 4);

            //gLog.Message("Init dynamicTextBuffer");
            dynamicTextBuffer = processedTextBuffer;

            //gLog.Message("SafeFormatString originalTextBuffer");
            // 0xEC623A
            snprintf(originalTextBuffer, sourceTextLen + 1, "%s", apOrigString);
            //gLog.Message("Buffer Init Finish");

            processedTextLen = 0;
            textBufferSize = sourceTextLen + 4;
            hyphenInsertCount = 0;
            isTildeChar = 0;
            parsedTextBuffer[0] = 0;
            hasEscapeSequence = 0;
            //gLog.Message("processedOriginalText Read Start");
            for (srcTextIndex = 0; srcTextIndex < sourceTextLen; ++srcTextIndex)
            {
                //gLog.FormattedMessage("process index %u", srcTextIndex);
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
                    // 0xEC6130
                    totalEscapeSeqLen = (strlen(varNameBuffer) + 1);
                    if (processedOriginalText[varNameLen + srcTextIndex] == ';')
                        totalEscapeSeqLen += escapeSeqPrefixLen;
                    //gLog.Message("ReplaceVariableInString & ParseAndFormatVariableInString");
                    if (ReplaceVariableInString(varNameBuffer, parsedTextBuffer, 0x400u, isPositiveEscape)
                        || ParseAndFormatVariableInString(varNameBuffer, parsedTextBuffer))
                    {
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
                            //gLog.Message("FastStringCopyAligned 1");
                            //strcpy(&parsedTextBuffer[charScanIndex + 1], substrBuffer);
                            strcpy_s(&parsedTextBuffer[charScanIndex + 1],
                                sizeof(parsedTextBuffer) - (charScanIndex + 1),
                                substrBuffer);
                            //gLog.Message("FastStringCopyAligned 1 End");
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
                            dynamicTextBuffer = static_cast<char*>(MemoryManagerSingleton->Reallocate(dynamicTextBuffer, textBufferSize));
                        }
                        for (bufferCopyIndex = 0; bufferCopyIndex < postEscapeTextLen; ++bufferCopyIndex)
                            dynamicTextBuffer[processedTextLen++] = parsedTextBuffer[bufferCopyIndex];
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
                    //gLog.FormattedMessage("dynamicTextBuffer [%u] = processedOriginalText[%u]", processedTextLen + 1 , srcTextIndex);
                    dynamicTextBuffer[processedTextLen++] = processedOriginalText[srcTextIndex];
                    //gLog.Message("Copy Finished");
                }
            }
            //gLog.Message("processedOriginalText Read Finish");
            dynamicTextBuffer[processedTextLen] = 0;
            if (hasEscapeSequence)
            {
                sourceTextLen = processedTextLen;
                //gLog.FormattedMessage("Relocating processedOriginalText: size=%u", processedTextLen + 4);
                processedOriginalText = static_cast<char*>(MemoryManagerSingleton->Reallocate(processedOriginalText, processedTextLen + 4));
                // 0xEC6370
                //gLog.Message("FastStringCopyAligned");
                //strcpy(processedOriginalText, dynamicTextBuffer);
                strcpy_s(processedOriginalText, processedTextLen + 4, dynamicTextBuffer);
            }
            *dynamicTextBuffer = 0;
            processedTextLen = 0;
            buttonIconIndex = 0;

            //gLog.Message("processedOriginalText Read 2 Start");
            for (charIndex = 0; charIndex < sourceTextLen && processedOriginalText[charIndex]; ++charIndex)
            {
                if (processedOriginalText[charIndex] == axData->cLineSep)
                {
                    dynamicTextBuffer[processedTextLen] = axData->cLineSep;
                    if (++processedTextLen >= textBufferSize)
                    {
                        //gLog.FormattedMessage("Relocating dynamicTextBuffer: size=%u", processedTextLen + 4);
                        dynamicTextBuffer = static_cast<char*>(MemoryManagerSingleton->Reallocate(dynamicTextBuffer, processedTextLen + 4));
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
                    if (processedOriginalText[charIndex] == '\t')
                    {
                        currentLineWidth += 75 - currentLineWidth % 75;
                        continue;
                    }
                    currentChar = processedOriginalText[charIndex];
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
                    //gLog.FormattedMessage("ConditionalFloatToUInt: %d", pCurrentGlyph->width + pCurrentGlyph->kerningRight);
                    charWidthWithKerning = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
                    //gLog.FormattedMessage("charWidthWithKerning: %d", charWidthWithKerning);
                    currentLineWidth += charWidthWithKerning;
                    if (currentChar == ' ')
                    {
                        lastWrapPosition = processedTextLen;
                        spaceCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
                        preSpaceWidth = currentLineWidth - spaceCharWidth;
                        postSpaceWidth = currentLineWidth;
                        isTildeChar = 0;
                    }
                    else if (currentChar == '~')
                    {
                        lastWrapPosition = processedTextLen;
                        preSpaceWidth = currentLineWidth;
                        postSpaceWidth = currentLineWidth;
                        isTildeChar = 1;
                        tildeCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
                        currentLineWidth -= tildeCharWidth;
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
                                dynamicTextBuffer = static_cast<char*>(MemoryManagerSingleton->Reallocate(dynamicTextBuffer, textBufferSize));
                                // 0xEC7230
                                memmove(
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
                                currentLineWidth -= hyphenCharWidth;
                                AppendToListTail(&axData->xLineWidths, &currentLineWidth);
                                if (maxLineWidth <= currentLineWidth)
                                    tempLineWidthComp3 = currentLineWidth;
                                else
                                    tempLineWidthComp3 = maxLineWidth;
                                maxLineWidth = tempLineWidthComp3;
                                lastWrapPosition = 0;
                                ++currentLineCount;
                                pCurrentGlyph = &this->pFontData->pFontLetters[*(dynamicTextBuffer - 1)];
                                currentLineWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
                                pCurrentGlyph = &this->pFontData->pFontLetters[currentChar];
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
                            if (processedTextLen + 3 >= textBufferSize)
                            {
                                dynamicTextBuffer = static_cast<char*>(MemoryManagerSingleton->Reallocate(dynamicTextBuffer, processedTextLen + 6));
                                textBufferSize = processedTextLen + 6;
                            }
                            dynamicTextBuffer[processedTextLen + 2] = dynamicTextBuffer[processedTextLen];
                            dynamicTextBuffer[processedTextLen + 1] = dynamicTextBuffer[processedTextLen - 1];
                            //qmemcpy
                            memcpy(&dynamicTextBuffer[processedTextLen - 1], "-\n", 2);
                            processedTextLen += 2;
                            hyphenInsertCount += 2;
                            pCurrentGlyph = &this->pFontData->pFontLetters['-'];
                            totalTextHeight = this->pFontData->fBaseLine + lineSpacingAdjust + totalTextHeight;
                            hyphenInsertWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
                            currentLineWidth -= hyphenInsertWidth;
                            AppendToListTail(&axData->xLineWidths, &currentLineWidth);
                            if (maxLineWidth <= currentLineWidth)
                                tempLineWidthComp1 = currentLineWidth;
                            else
                                tempLineWidthComp1 = maxLineWidth;
                            maxLineWidth = tempLineWidthComp1;
                            lastWrapPosition = 0;
                            ++currentLineCount;
                            pCurrentGlyph = &this->pFontData->pFontLetters[dynamicTextBuffer[processedTextLen - 1]];
                            currentLineWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
                            pCurrentGlyph = &this->pFontData->pFontLetters[currentChar];
                            combinedCharWidth = ConditionalFloatToUInt(pCurrentGlyph->fWidth + pCurrentGlyph->fSpacing);
                            currentLineWidth += combinedCharWidth;
                        }
                    }
                    if (currentChar != '~')
                        dynamicTextBuffer[processedTextLen++] = currentChar;
                    if (processedTextLen >= textBufferSize)
                    {
                        dynamicTextBuffer = static_cast<char*>(MemoryManagerSingleton->Reallocate(dynamicTextBuffer, processedTextLen + 4));
                        textBufferSize = processedTextLen + 4;
                    }
                }
                if (maxAllowedLines > 0 && currentLineCount > maxAllowedLines && processedTextLen)
                {
                    while (dynamicTextBuffer[processedTextLen] != axData->cLineSep)
                        --processedTextLen;
                    dynamicTextBuffer[processedTextLen] = 0;
                    totalTextHeight = totalTextHeight - (this->pFontData->fBaseLine + lineSpacingAdjust);
                    break;
                }
            }
            //gLog.Message("processedOriginalText Read 2 End");
            if (*dynamicTextBuffer && axData->iLineStart)
            {
                truncatedTextLen = 0;
                lineCounter = 0;
                for (truncateCharCounter = 0; truncateCharCounter < processedTextLen; ++truncateCharCounter)
                {
                    if (lineCounter >= axData->iLineStart && lineCounter < axData->iLineEnd)
                        dynamicTextBuffer[truncatedTextLen++] = dynamicTextBuffer[truncateCharCounter];
                    if (dynamicTextBuffer[truncateCharCounter] == 10)
                        ++lineCounter;
                }
                dynamicTextBuffer[truncatedTextLen] = 0;
                processedTextLen = truncatedTextLen;
            }
            if (!*dynamicTextBuffer)
            {
                //strcpy
                strcpy_s(dynamicTextBuffer, textBufferSize, " ");
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
            MemoryManagerSingleton->Deallocate(processedOriginalText);
            MemoryManagerSingleton->Deallocate(dynamicTextBuffer);
            //gLog.Message("CalculateTextLayoutEx End");
        }

        UInt32 CreateText() {

        }
    };

    class FontManagerEx : public FontManager {
    public:
        inline float GetHanziWidth(uint16_t gbChar, FontLetter* fontCharMetrics) {
            // Check if the character is a valid GB2312 character
            if (fontCharMetrics[gbChar].fWidth > 0) {
                return fontCharMetrics[gbChar].fLeadingEdge
                    + fontCharMetrics[gbChar].fWidth
                    + fontCharMetrics[gbChar].fSpacing;
            }

            // default space character width if the character is not found
            return (fontCharMetrics[' '].fLeadingEdge
                + fontCharMetrics[' '].fWidth
                + fontCharMetrics[' '].fSpacing);
        }

        //	outDims.x := width (pxl); outDims.y := height (pxl); outDims.z := numLines
        NiPoint3* __thiscall CalculateStringDimensions(NiPoint3* outDimensions, const char* srcString, uint32_t fontID, float maxWrapWidth, uint32_t startCharIndex) {
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

            //gLog.Message("Call GetStringDimensionsEx");

            if (fontID >= 1 && fontID <= 8 && srcString)
            {
                StringDimensions = StringDefaulDimensions;
                // 0xEC6130
                //gLog.Message("Call strlen");
                sourceStringLength = strlen(srcString);
                //gLog.FormattedMessage("sourceStringLength: %u", sourceStringLength);
                fontCharMetrics = this->pFont[fontID - 1]->pFontData->pFontLetters;
                lastValidWrapPosition = 0.0;
                currentLineWidth = 0.0;
                //gLog.Message("Call VertSpacingAdjust");
                fontVerticalSpacingAdjust = FontManagerGetLinePadding(fontID);
                //gLog.FormattedMessage("fontVerticalSpacingAdjust: %u", fontVerticalSpacingAdjust);
                previousCharTotalWidth = 0.0;
                hasHyphenationPoint = 0;
                totalLines = 1;
                StringDimensions.y = fontCharMetrics[' '].fHeight;
                for (currentCharIndex = startCharIndex; currentCharIndex < sourceStringLength; ++currentCharIndex)
                {
                    currentChar = srcString[currentCharIndex];
                    currentCharTotalWidth = 0.0;
                    //gLog.Message("Call ConvertToAsciiQuotes");
                    ConvertToAsciiQuotes(&currentChar);
                    //gLog.Message("Call ConvertToAsciiQuotes end");
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
                        lastValidWrapPosition = currentLineWidth;
                        hasHyphenationPoint = 0;
                        break;
                    case '~':
                        lastValidWrapPosition = currentLineWidth
                            + fontCharMetrics['-'].fLeadingEdge
                            + fontCharMetrics['-'].fWidth
                            + fontCharMetrics['-'].fSpacing;
                        hasHyphenationPoint = 1;
                        break;
                    default:
                        break;
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
                                + fontCharMetrics['-'].fLeadingEdge
                                + fontCharMetrics['-'].fWidth
                                + fontCharMetrics['-'].fSpacing;
                            currentLineWidth = currentCharTotalWidth + previousCharTotalWidth;
                        }
                        else
                        {
                            currentLineWidth = currentLineWidth - lastValidWrapPosition;
                            if (!hasHyphenationPoint)
                            {
                                if (currentChar == '\n')
                                    currentLineWidth = 0.0;
                                else
                                    currentLineWidth = currentLineWidth
                                    - (fontCharMetrics[' '].fLeadingEdge
                                        + fontCharMetrics[' '].fWidth
                                        + fontCharMetrics[' '].fSpacing);
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
                *outDimensions = StringDefaulDimensions;
                return outDimensions;
            }
        }
    };

    void InitVertSpacingHook() {
        WriteRelJump(0xA1B3A0, &VertSpacingAdjust);
    }

    void InitFontHook() {
        // Font::Load
        WriteRelCallEx(0xA1219D, &FontEx::Load);
        // 
        // Font::PrepText
        WriteRelJumpEx(0xA12FB0, &FontEx::PrepText);
        // 
        // Font::AddChar
        //WriteRelCall(0xA1278B, &FontAddChar);
        //WriteRelCall(0xA12E1B, &FontAddChar);
        //WriteRelCall(0xA19622, &FontAddChar);
        // 
        // Font::CreateText
        //WriteRelCallEx(0xA22211, &FontEx::CreateText);
        // 
        // FontManager::CalculateStringDimensions
        WriteRelJumpEx(0xA1B020, &FontManagerEx::CalculateStringDimensions);
        // 
        // FileFinder::GetFile
        //WriteRelCall(0xA15A86, &FileFinder_GetFile);
        // 
        // NiSourceTexture::Create
        //WriteRelCall(0xA6AC12, &NiSourceTextureCreate);
        // 
        // NiGlobalStringTable::AddString
        //WriteRelCall(0xA5FF86, &NiGlobalStringTableAddString);
        // 
        // NiSourceTexture::SetFilename
        //WriteRelCallEx(0xA5FF9B, &NiSourceTextureEx::SetFilename);
        //
        //NiTexturingProperty::NiTexturingProperty
        //WriteRelCallEx(0xA15D86, &NiTexturingPropertyEx::NiTexturingPropertyBuild);

        //NiPixelData::NiPixelData
        //WriteRelCallEx(0xA15C03, &NiPixelDataEx::NiPixelDataBuild);
    }

    Font* __fastcall FontCreateForJIP(Font* apThis, void*, int iFontNum, char* apFilename, bool abLoad) {
        gLog.FormattedMessage("\nCall Font::Font");
        gLog.FormattedMessage("iFontNum: %u", iFontNum);
        gLog.FormattedMessage("apFilename: %s", (const char*)apFilename);

        Font* ret = ThisStdCall<Font*>(
            0xA12020,
            apThis,
            iFontNum,
            apFilename,
            abLoad);

        if (!gExtraFontLetters[fontNameKey].empty()) {
            gLog.FormattedMessage("From gExtraFontLetters to gNumberedExtraLetters");
			gNumberedExtraLetters[iFontNum] = gExtraFontLetters[fontNameKey];
            UInt32 numRemoved = gExtraFontLetters.erase(fontNameKey);
            if (!gNumberedExtraLetters[iFontNum].empty())
                gLog.FormattedMessage("gNumberedExtraLetters[%d] is filled", iFontNum);
            fontNameKey = "";
        }
        else {
			gLog.FormattedMessage("gExtraFontLetters for %s is empty", fontNameKey);
            fontNameKey = "";
        }
        return ret;
    }

    void InitJIPHooks() {
        hJIP = GetModuleHandle("jip_nvse.dll");
        if (!hJIP) {
            gLog.FormattedMessage("JIP not find");
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
        gLog.FormattedMessage("hook JIP Font::Font");
        SafeWrite32(GetJIPAddress(0x10011A3E + 1), uint32_t(FontCreateForJIP));
        SafeWrite32(GetJIPAddress(0x10011AA9 + 1), uint32_t(FontCreateForJIP));
        SafeWrite32(GetJIPAddress(0x1003943F + 1), uint32_t(FontCreateForJIP));
    }
}
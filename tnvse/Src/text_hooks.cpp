#include "text_hooks.h"

namespace fonthook {

// ==================== Quest/Location Text Hook ====================
void* __fastcall TileSetStringHookForQueueText(void* pThis, void*, int a2, char* a3, bool a4) {
    bIsQuestTextLSBDBCharacter = false;
    if (bIsQuestTextMSBDBCharacter) {
        bIsQuestTextLSBDBCharacter = IsTrailByte(a3[0]);

        if (bIsQuestTextLSBDBCharacter) {
            szDBChar[0] = pFirstChar;
            szDBChar[1] = a3[0];
            szDBChar[2] = 0;
            a3 = (char*)szDBChar;
        }
    }

    if (gNumberedExtraLetters.find(8) != gNumberedExtraLetters.end() && !bIsQuestTextMSBDBCharacter) {
        bIsQuestTextMSBDBCharacter = false;

        bIsQuestTextMSBDBCharacter = IsLeadByte((unsigned char)a3[0]);

        if (bIsQuestTextMSBDBCharacter) {
            pFirstChar = (unsigned char)a3[0];
            a3 = (char*)"";
        }
    }
    else {
        bIsQuestTextMSBDBCharacter = false;
    }

    return ThisStdCall<void*>(0xA01350, pThis, a2, a3, a4);
}

// ==================== UTF-8 Conversion Hooks ====================
char* __fastcall BSString_c_strHook(BSStringT<char>* pthis, void*) {
    auto extraGlyphEntry = gNumberedExtraLetters.find(5);
    auto* extraGlyphs = extraGlyphEntry != gNumberedExtraLetters.end() ? &extraGlyphEntry->second : nullptr;
    std::string sCurrentStr, sConvertedStr;

    if (g_bEnableUTF8 && g_uiEncoding != 0 && extraGlyphs) {
        if (IsValidUTF8With3ByteMin(pthis->pString)) {
            sCurrentStr = pthis->pString;
            sConvertedStr = UTF8ToMultiByteStr(sCurrentStr, g_usingWinEncoding);
            pthis->Set(sConvertedStr.c_str());
        }
    }
    return pthis->pString;
}

char* __fastcall BSString_GetCStringOrEmptyHook(BSStringT<char>* pthis, void*) {
    auto extraGlyphEntry = gNumberedExtraLetters.find(8);
    auto* extraGlyphs = extraGlyphEntry != gNumberedExtraLetters.end() ? &extraGlyphEntry->second : nullptr;
    std::string sCurrentStr, sConvertedStr;

    if (g_bEnableUTF8 && g_uiEncoding != 0 && extraGlyphs) {
        if (IsValidUTF8With3ByteMin(pthis->pString)) {
            sCurrentStr = pthis->pString;
            sConvertedStr = UTF8ToMultiByteStr(sCurrentStr, g_usingWinEncoding);
            pthis->Set(sConvertedStr.c_str());
        }
    }

    return ThisStdCall<char*>(0x408DA0, pthis);
}

int __cdecl strcpy_sHook(char* dest, int dest_size, const char* src) {
    auto extraGlyphEntry = gNumberedExtraLetters.find(8);
    auto* extraGlyphs = extraGlyphEntry != gNumberedExtraLetters.end() ? &extraGlyphEntry->second : nullptr;
    std::string sCurrentStr, sConvertedStr;

    if (g_bEnableUTF8 && g_uiEncoding != 0 && extraGlyphs) {
        if (IsValidUTF8With3ByteMin(src)) {
            sCurrentStr = src;
            sConvertedStr = UTF8ToMultiByteStr(sCurrentStr, g_usingWinEncoding);
            src = sConvertedStr.c_str();
        }
    }

    return strcpy_s(dest, dest_size, src);
}

// ==================== Door Prompt Hooks ====================
int BSsprintfHookCHS(
    char* buffer, size_t sizeOfBuffer,
    const char* sformat, const char* sDst,
    const char* sTo, const char* sCellName) {

    static std::string sConvertedStructuralParticle = UTF8ToMultiByteStr(g_sOptionalStructuralParticle, g_usingWinEncoding);

    return sprintf_s(buffer, sizeOfBuffer, "%s%s%s%s", sTo, sCellName, sConvertedStructuralParticle.c_str(), sDst);
}

int BSsprintfHookKOR(
    char* buffer, size_t sizeOfBuffer,
    const char* sformat, const char* sDst,
    const char* sTo, const char* sCellName) {

    static std::string sConvertedStructuralParticle = UTF8ToMultiByteStr(g_sOptionalStructuralParticle, g_usingWinEncoding);

    return sprintf_s(buffer, sizeOfBuffer, "%s%s%s%s", sCellName, sTo, sConvertedStructuralParticle.c_str(), sDst);
}

} // namespace fonthook

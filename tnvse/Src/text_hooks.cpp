#include "text_hooks.h"
#include "dictionary.h"

namespace fonthook
{
	// ==================== Quest/Location Text Hook ====================
	void* __fastcall TileSetStringHookForQueueText(void* pThis, void*, int a2, char* a3, bool a4)
	{
		if (!a3)
			return ThisStdCall<void*>(0xA01350, pThis, a2, a3, a4);

		bool bHasFont8 = gNumberedExtraLetters.find(8) != gNumberedExtraLetters.end();
		if (bHasFont8)
		{
			bIsQuestTextLSBDBCharacter = false;
			if (bIsQuestTextMSBDBCharacter)
			{
				bIsQuestTextMSBDBCharacter = false;
				bMeasureQuestTextMSBAsEmpty = false;
				bIsQuestTextLSBDBCharacter = IsTrailByte((UInt8)a3[0]);

				if (bIsQuestTextLSBDBCharacter)
				{
					szDBChar[0] = pFirstChar;
					szDBChar[1] = a3[0];
					szDBChar[2] = 0;
					a3 = (char*)szDBChar;
				}
			}

			if (!bIsQuestTextLSBDBCharacter && IsLeadByte((UInt8)a3[0]))
			{
				pFirstChar = (UInt8)a3[0];
				bIsQuestTextMSBDBCharacter = true;
				bMeasureQuestTextMSBAsEmpty = true;
				a3 = (char*)"";
			}
		}
		else
		{
			bIsQuestTextLSBDBCharacter = false;
			bIsQuestTextMSBDBCharacter = false;
			bMeasureQuestTextMSBAsEmpty = false;
		}

		return ThisStdCall<void*>(0xA01350, pThis, a2, a3, a4);
	}

	// ==================== UTF-8 Conversion Hooks ====================
	char* __fastcall BSString_c_strHook(BSStringT<char>* pthis, void*)
	{
		const char* pStr = pthis->pString;
		std::string sConvertedStr;
		if (ConvertToMultiByte(pStr, sConvertedStr, gNumberedExtraLetters.find(5) != gNumberedExtraLetters.end()))
			pthis->Set(pStr);
		std::string sTranslatedStr;
		if (TranslateText(pthis->pString, sTranslatedStr))
			pthis->Set(sTranslatedStr.c_str());
		return pthis->pString;
	}

	char* __fastcall BSString_GetCStringOrEmptyHook(BSStringT<char>* pthis, void*)
	{
		const char* pStr = pthis->pString;
		std::string sConvertedStr;
		if (ConvertToMultiByte(pStr, sConvertedStr, gNumberedExtraLetters.find(8) != gNumberedExtraLetters.end()))
			pthis->Set(pStr);
		std::string sTranslatedStr;
		if (TranslateText(pthis->pString, sTranslatedStr))
			pthis->Set(sTranslatedStr.c_str());

		return ThisStdCall<char*>(0x408DA0, pthis);
	}

	int __cdecl strcpy_sHook(char* dest, int dest_size, const char* src)
	{
		std::string sConvertedStr;
		ConvertToMultiByte(src, sConvertedStr, gNumberedExtraLetters.find(8) != gNumberedExtraLetters.end());
		std::string sTranslatedStr;
		if (TranslateText(src, sTranslatedStr))
			return strcpy_s(dest, dest_size, sTranslatedStr.c_str());
		return strcpy_s(dest, dest_size, src);
	}

	// ==================== Door Prompt Hooks ====================
	int BSsprintfHookCHS(
		char* buffer, size_t sizeOfBuffer,
		const char* sformat, const char* sDst,
		const char* sTo, const char* sCellName)
	{

		static std::string sConvertedStructuralParticle = UTF8ToMultiByteStr(g_sOptionalStructuralParticle, g_usingWinEncoding);

		return sprintf_s(buffer, sizeOfBuffer, "%s%s%s%s", sTo, sCellName, sConvertedStructuralParticle.c_str(), sDst);
	}

	int BSsprintfHookKOR(
		char* buffer, size_t sizeOfBuffer,
		const char* sformat, const char* sDst,
		const char* sTo, const char* sCellName)
	{

		static std::string sConvertedStructuralParticle = UTF8ToMultiByteStr(g_sOptionalStructuralParticle, g_usingWinEncoding);

		return sprintf_s(buffer, sizeOfBuffer, "%s%s%s%s", sCellName, sTo, sConvertedStructuralParticle.c_str(), sDst);
	}

} // namespace fonthook

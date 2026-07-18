#include "text_hooks.h"
#include "dictionary.h"
#include "font_glyphs.h"

namespace fonthook
{
	namespace
	{
		std::string GetDoorStructuralParticle()
		{
			if (g_sOptionalStructuralParticle.empty())
				return {};
			if (IsEastAsianUiMode()
				&& IsValidUTF8With3ByteMin(g_sOptionalStructuralParticle.c_str()))
				return UTF8ToMultiByteStr(g_sOptionalStructuralParticle, g_usingWinEncoding);
			return g_sOptionalStructuralParticle;
		}

		const char* SafeCString(const char* value)
		{
			return value ? value : "";
		}

		const char* TranslateDoorPiece(const char* source, std::string& translated)
		{
			const char* safeSource = SafeCString(source);
			translated.clear();
			if (*safeSource && TranslateText(safeSource, translated))
				return translated.c_str();
			return safeSource;
		}

		const char* TranslateDoorToPiece(
			const char* source,
			const std::string& structuralParticle,
			bool& usedStructuralParticleFallback)
		{
			const char* safeSource = SafeCString(source);
			if (!structuralParticle.empty())
			{
				usedStructuralParticleFallback = true;
				return structuralParticle.c_str();
			}
			usedStructuralParticleFallback = false;
			return safeSource;
		}

		int FormatDoorPromptCHS(
			char* buffer, size_t sizeOfBuffer,
			const char* sDst, const char* sTo, const char* sCellName,
			bool translatePieces)
		{
			static std::string sConvertedStructuralParticle = GetDoorStructuralParticle();

			std::string translatedDst;
			std::string translatedCellName;
			bool usedStructuralParticleFallback = false;
			const char* dst = translatePieces ? TranslateDoorPiece(sDst, translatedDst) : SafeCString(sDst);
			const char* to = translatePieces ? TranslateDoorToPiece(sTo, sConvertedStructuralParticle, usedStructuralParticleFallback) : SafeCString(sTo);
			const char* cellName = translatePieces ? TranslateDoorPiece(sCellName, translatedCellName) : SafeCString(sCellName);

			if (usedStructuralParticleFallback)
				return sprintf_s(buffer, sizeOfBuffer, "%s%s%s", cellName, to, dst);

			return sprintf_s(buffer, sizeOfBuffer, "%s%s%s%s", to, cellName, sConvertedStructuralParticle.c_str(), dst);
		}

		int FormatDoorPromptKOR(
			char* buffer, size_t sizeOfBuffer,
			const char* sDst, const char* sTo, const char* sCellName,
			bool translatePieces)
		{
			static std::string sConvertedStructuralParticle = GetDoorStructuralParticle();

			std::string translatedDst;
			std::string translatedCellName;
			bool usedStructuralParticleFallback = false;
			const char* dst = translatePieces ? TranslateDoorPiece(sDst, translatedDst) : SafeCString(sDst);
			const char* to = translatePieces ? TranslateDoorToPiece(sTo, sConvertedStructuralParticle, usedStructuralParticleFallback) : SafeCString(sTo);
			const char* cellName = translatePieces ? TranslateDoorPiece(sCellName, translatedCellName) : SafeCString(sCellName);

			if (usedStructuralParticleFallback)
				return sprintf_s(buffer, sizeOfBuffer, "%s%s%s", cellName, to, dst);

			return sprintf_s(buffer, sizeOfBuffer, "%s%s%s%s", cellName, to, sConvertedStructuralParticle.c_str(), dst);
		}
	}

	// ==================== Quest/Location Text Hook ====================
	void* __fastcall TileSetStringHookForQueueText(void* pThis, void*, int a2, char* a3, bool a4)
	{
		if (!a3)
			return ThisStdCall<void*>(0xA01350, pThis, a2, a3, a4);

		bool bHasFont8 = HasExtraGlyphsForFont(8);
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
		if (ConvertToMultiByte(pStr, sConvertedStr, HasExtraGlyphsForFont(5)))
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
		if (ConvertToMultiByte(pStr, sConvertedStr, HasExtraGlyphsForFont(8)))
			pthis->Set(pStr);
		std::string sTranslatedStr;
		if (TranslateText(pthis->pString, sTranslatedStr))
			pthis->Set(sTranslatedStr.c_str());

		return ThisStdCall<char*>(0x408DA0, pthis);
	}

	int __cdecl strcpy_sHook(char* dest, int dest_size, const char* src)
	{
		std::string sConvertedStr;
		ConvertToMultiByte(src, sConvertedStr, HasExtraGlyphsForFont(8));
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
		if (!g_bEnableDictionaryTranslation)
			return FormatDoorPromptCHS(buffer, sizeOfBuffer, sDst, sTo, sCellName, false);

		char sourceBuffer[1024] = {};
		sprintf_s(sourceBuffer, sizeof(sourceBuffer), "%s%s%s", SafeCString(sTo), SafeCString(sCellName), SafeCString(sDst));

		std::string translated;
		if (TranslateText(sourceBuffer, translated))
			return sprintf_s(buffer, sizeOfBuffer, "%s", translated.c_str());

		return FormatDoorPromptCHS(buffer, sizeOfBuffer, sDst, sTo, sCellName, true);
	}

	int BSsprintfHookKOR(
		char* buffer, size_t sizeOfBuffer,
		const char* sformat, const char* sDst,
		const char* sTo, const char* sCellName)
	{
		if (!g_bEnableDictionaryTranslation)
			return FormatDoorPromptKOR(buffer, sizeOfBuffer, sDst, sTo, sCellName, false);

		char sourceBuffer[1024] = {};
		sprintf_s(sourceBuffer, sizeof(sourceBuffer), "%s%s%s", SafeCString(sCellName), SafeCString(sTo), SafeCString(sDst));

		std::string translated;
		if (TranslateText(sourceBuffer, translated))
			return sprintf_s(buffer, sizeOfBuffer, "%s", translated.c_str());

		return FormatDoorPromptKOR(buffer, sizeOfBuffer, sDst, sTo, sCellName, true);
	}

} // namespace fonthook

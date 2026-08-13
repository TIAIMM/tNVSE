#include "text_hooks.h"
#include "dictionary.h"
#include "font_glyphs.h"

#include <cstddef>

namespace fonthook
{
	namespace implementation::text_hooks {}
	using namespace implementation::text_hooks;

	namespace implementation::text_hooks
	{
		inline constexpr SIZE_T kRetailQuestTextToUpper = 0xECA7F4;

		// HUDMainMenu::UpdateQuestText uppercases its copied quest/location body
		// one byte at a time. DBCS trail bytes can overlap ASCII a-z, so perform
		// the intended case conversion here while the complete byte stream is
		// available. The retail byte loop is replaced by the identity hook below.
		void UppercaseQuestTextPreservingDbcs(char* text, size_t capacity)
		{
			if (!text || capacity == 0)
				return;

			for (size_t index = 0; index < capacity && text[index];)
			{
				UInt32 doubleByteCode = 0;
				if (index + 1 < capacity
					&& TryDecodeDoubleByte(text + index, doubleByteCode))
				{
					index += 2;
					continue;
				}

				// Match retail's MOVSX argument exactly for every standalone byte.
				const int input = static_cast<int>(
					static_cast<signed char>(text[index]));
				text[index] = static_cast<char>(
					CdeclCall<int>(kRetailQuestTextToUpper, input));
				++index;
			}
		}

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
	void __fastcall TileSetStringHookForQuestAndLocationText(
		void* pThis, void*, int a2, char* a3, bool a4)
	{
		if (!a3)
		{
			ThisStdCall<void>(0xA01350, pThis, a2, a3, a4);
			return;
		}

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

		ThisStdCall<void>(0xA01350, pThis, a2, a3, a4);
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
		const char* copySource = TranslateText(src, sTranslatedStr)
			? sTranslatedStr.c_str() : src;
		const int result = strcpy_s(dest, dest_size, copySource);
		if (result == 0 && dest_size > 0)
		{
			UppercaseQuestTextPreservingDbcs(
				dest, static_cast<size_t>(dest_size));
		}
		return result;
	}

	int __cdecl QuestTextCaseIdentityHook(int value)
	{
		return value;
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

#include "text_hooks.h"
#include "dictionary.h"
#include "font_glyphs.h"
#include "text_safety.h"

#include <cerrno>
#include <cstddef>
#include <limits>
#include <string_view>

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

		int WriteFixedUiText(
			char* buffer, size_t sizeOfBuffer, std::string_view text)
		{
			if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max())
				|| text_safety::CopyTextIfFits(buffer, sizeOfBuffer, text)
					!= text_safety::CopyStatus::Copied)
			{
				return -1;
			}
			return static_cast<int>(text.size());
		}

		std::string BuildDoorPromptCHS(
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

			std::string prompt;
			if (usedStructuralParticleFallback)
			{
				prompt = cellName;
				prompt += to;
				prompt += dst;
				return prompt;
			}

			prompt = to;
			prompt += cellName;
			prompt += sConvertedStructuralParticle;
			prompt += dst;
			return prompt;
		}

		std::string BuildDoorPromptKOR(
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

			std::string prompt = cellName;
			if (usedStructuralParticleFallback)
			{
				prompt += to;
				prompt += dst;
				return prompt;
			}

			prompt += to;
			prompt += sConvertedStructuralParticle;
			prompt += dst;
			return prompt;
		}

		std::string BuildDoorDictionarySourceCHS(
			const char* sDst, const char* sTo, const char* sCellName)
		{
			std::string source = SafeCString(sTo);
			source += SafeCString(sCellName);
			source += SafeCString(sDst);
			return source;
		}

		std::string BuildDoorDictionarySourceKOR(
			const char* sDst, const char* sTo, const char* sCellName)
		{
			std::string source = SafeCString(sCellName);
			source += SafeCString(sTo);
			source += SafeCString(sDst);
			return source;
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
		if (!dest || dest_size <= 0 || !src)
		{
			text_safety::ClearBuffer(
				dest, dest_size > 0 ? static_cast<size_t>(dest_size) : 0u);
			return EINVAL;
		}

		std::string sConvertedStr;
		ConvertToMultiByte(src, sConvertedStr, HasExtraGlyphsForFont(8));
		std::string sTranslatedStr;
		const std::string_view source(src);
		const bool translated = TranslateText(src, sTranslatedStr);
		const bool copied = translated
			? text_safety::CopyPreferredTextWithFallback(
				dest, static_cast<size_t>(dest_size), sTranslatedStr, source)
				!= text_safety::CopyChoice::None
			: text_safety::CopyTextIfFits(
				dest, static_cast<size_t>(dest_size), source)
				== text_safety::CopyStatus::Copied;
		const int result = copied ? 0 : ERANGE;
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
		(void)sformat;
		if (!buffer || sizeOfBuffer == 0)
			return -1;

		const std::string original = BuildDoorPromptCHS(
			sDst, sTo, sCellName, false);
		if (!g_bEnableDictionaryTranslation)
			return WriteFixedUiText(buffer, sizeOfBuffer, original);

		const std::string source = BuildDoorDictionarySourceCHS(
			sDst, sTo, sCellName);
		std::string translated;
		if (TranslateText(source.c_str(), translated))
		{
			const int result = WriteFixedUiText(
				buffer, sizeOfBuffer, translated);
			if (result >= 0)
				return result;
		}

		const std::string pieceTranslated = BuildDoorPromptCHS(
			sDst, sTo, sCellName, true);
		const int pieceResult = WriteFixedUiText(
			buffer, sizeOfBuffer, pieceTranslated);
		return pieceResult >= 0
			? pieceResult : WriteFixedUiText(buffer, sizeOfBuffer, original);
	}

	int BSsprintfHookKOR(
		char* buffer, size_t sizeOfBuffer,
		const char* sformat, const char* sDst,
		const char* sTo, const char* sCellName)
	{
		(void)sformat;
		if (!buffer || sizeOfBuffer == 0)
			return -1;

		const std::string original = BuildDoorPromptKOR(
			sDst, sTo, sCellName, false);
		if (!g_bEnableDictionaryTranslation)
			return WriteFixedUiText(buffer, sizeOfBuffer, original);

		const std::string source = BuildDoorDictionarySourceKOR(
			sDst, sTo, sCellName);
		std::string translated;
		if (TranslateText(source.c_str(), translated))
		{
			const int result = WriteFixedUiText(
				buffer, sizeOfBuffer, translated);
			if (result >= 0)
				return result;
		}

		const std::string pieceTranslated = BuildDoorPromptKOR(
			sDst, sTo, sCellName, true);
		const int pieceResult = WriteFixedUiText(
			buffer, sizeOfBuffer, pieceTranslated);
		return pieceResult >= 0
			? pieceResult : WriteFixedUiText(buffer, sizeOfBuffer, original);
	}

} // namespace fonthook

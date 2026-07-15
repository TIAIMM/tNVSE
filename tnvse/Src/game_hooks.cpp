#include "font_engine.h"
#include "font_manager.h"
#include "font_vector.h"
#include "game_hooks.h"
#include "load_config.h"
#include "SafeWrite.h"
#include "text_hooks.h"
#include "tnvse.h"

#include "TileText.hpp"

#include <cstring>

namespace fonthook
{
	namespace
	{
		constexpr SIZE_T kTileTextMakeNodeVTableEntry = 0x1094880;
		constexpr SIZE_T kVanillaTileTextMakeNode = 0xA21AF0;
		using TileTextMakeNodeFn = NiNode* (__thiscall*)(TileText*);

		TileTextMakeNodeFn s_tileTextMakeNode = nullptr;
		thread_local UInt32 s_effectSuppressionDepth = 0;
		bool s_loggedVuiShadowSuppression = false;
		bool s_loggedVuiOutlineSuppression = false;

		bool IsVuiEffectProxy(const TileText* tile, bool& isOutline)
		{
			// VUI+'s Prefabs/VUI+/outline.xml implements its original-style dark
			// shadow/outline by cloning the source text into these two named tiles.
			// They must keep their fill and Tile color, but must not recursively gain
			// the configured tNVSE font effects of their own.
			isOutline = false;
			if (!tile)
				return false;
			const char* name = tile->strName.c_str();
			if (!name)
				return false;
			if (_stricmp(name, "VUI+Shadow") == 0)
				return true;
			if (_stricmp(name, "VUI+Outline") == 0)
			{
				isOutline = true;
				return true;
			}
			return false;
		}

		class ScopedEffectSuppression
		{
		public:
			explicit ScopedEffectSuppression(bool suppress) : m_suppress(suppress)
			{
				if (m_suppress)
					++s_effectSuppressionDepth;
			}

			~ScopedEffectSuppression()
			{
				if (m_suppress)
					--s_effectSuppressionDepth;
			}

		private:
			bool m_suppress;
		};

		NiNode* __fastcall TileTextMakeNodeHook(TileText* tile, void*)
		{
			bool isOutline = false;
			const bool suppress = IsVuiEffectProxy(tile, isOutline);
			if (suppress)
			{
				bool& logged = isOutline
					? s_loggedVuiOutlineSuppression : s_loggedVuiShadowSuppression;
				if (!logged)
				{
					logged = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: suppressing configured effects for VUI+ proxy tile=%s",
						tile->strName.c_str());
				}
			}
			ScopedEffectSuppression scope(suppress);
			return s_tileTextMakeNode ? s_tileTextMakeNode(tile) : nullptr;
		}

		void InstallVuiEffectProxyCompatibility()
		{
			const SIZE_T current = *reinterpret_cast<const SIZE_T*>(
				kTileTextMakeNodeVTableEntry);
			const SIZE_T hook = reinterpret_cast<SIZE_T>(&TileTextMakeNodeHook);
			if (current == hook)
				return;
			if (!current)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: VUI+ effect proxy compatibility skipped; TileText::MakeNode is null");
				return;
			}
			s_tileTextMakeNode = reinterpret_cast<TileTextMakeNodeFn>(current);
			SafeWrite32(kTileTextMakeNodeVTableEntry, hook);
			gLog.FormattedMessage(
				"tnvse_freetype_font: VUI+ effect proxy compatibility installed entry=%08X target=%08X chained=%d",
				static_cast<UInt32>(kTileTextMakeNodeVTableEntry),
				static_cast<UInt32>(current), current != kVanillaTileTextMakeNode ? 1 : 0);
		}
	}

	bool IsFreeTypeEffectSuppressionActive()
	{
		return s_effectSuppressionDepth != 0;
	}

	void InitBigGunsDescHooks()
	{
		static std::string sConvertedBigGunsDesc = UTF8ToMultiByteStr(g_sNewBigGunsDesc, g_usingWinEncoding);
		SafeWrite32(GetJIPAddress(0x100113BD + 1), (UInt32)sConvertedBigGunsDesc.c_str());
		gLog.FormattedMessage("g_sNewBigGunsDesc: %s", sConvertedBigGunsDesc.c_str());
	}

	void InitDoorPromptHooksCHS()
	{
		WriteRelCall(0x777006, &BSsprintfHookCHS);
	}

	void InitDoorPromptHooksKOR()
	{
		WriteRelCall(0x777006, &BSsprintfHookKOR);
	}

	void InitPluralHooks()
	{
		SafeWrite8(0x753E39, 0xEB);
	}

	void InitVertSpacingHook()
	{
		// FontManager::GetLinePadding
		//WriteRelJump(0xA1B3A0, &VertSpacingAdjust);
	}

	void InitFontHook()
	{
		InstallVuiEffectProxyCompatibility();

		WriteRelJumpEx(0xA12020, &FontEx::FontInit);
		WriteRelJumpEx(0xA15320, &FontEx::Load);
		WriteRelJumpEx(0xA12FB0, &FontEx::PrepText);
		// Terminal -> Font::PrepTextForTerminal
		WriteRelCallEx(0x759281, &FontEx::PrepTextForTerminal);

		WriteRelJump(0xA12880, reinterpret_cast<UInt32>(&FreeTypeCreateTextEntryHook));
		WriteRelJumpEx(0xA12460, &FontEx::MakeString);
		WriteRelJumpEx(0xA1B020, &FontManagerEx::CalculateStringDimensions);

		// FontManager::CreateText -> FontManager::PrepText
		WriteRelCallEx(0xA18F4A, &FontManagerEx::PrepText);

		// FontManager::PrepText -> FontManager::PrepHypertext
		WriteRelCallEx(0xA18ACC, &FontManagerEx::PrepHypertext);

		// FontManager::PrepHypertext -> CollectTo
		WriteRelCall(0xA1772D, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17835, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17A1E, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17B65, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17BB1, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17CFE, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17D5D, &FontManagerEx::CollectToAttributeValue);
		WriteRelCall(0xA17DE9, &FontManagerEx::CollectToAttributeValue);

		// FontManager::CreateText -> TextDoc::Render
		WriteRelCallEx(0xA18F63, &FontManagerEx::TextDocRender);

		// TextDoc::Render -> Font::AddChar
		WriteRelCallEx(0xA19622, &FontEx::TextDocRenderAddChar);

		// FontManager::CreateText -> TextDoc::Destroy
		WriteRelCallEx(0xA18F7D, &FontManagerEx::TextDocDestroy);

		// FontManager::PrepHypertext -> TextDoc::AddChar
		WriteRelCallEx(0xA178A4, &FontManagerEx::TextDocAddChar);
		WriteRelCallEx(0xA179D9, &FontManagerEx::TextDocAddChar);
		WriteRelCallEx(0xA17FC2, &FontManagerEx::TextDocAddChar);
		// FontManager::PrepText -> TextDoc::AddChar
		WriteRelCallEx(0xA18D7C, &FontManagerEx::TextDocAddChar);

		// TextDoc::AddChar -> TextPage::AddChar
		WriteRelCallEx(0xA19A6F, &FontManagerEx::TextPageAddChar);
		// TextPage::TextPage -> TextPage::AddChar
		WriteRelCallEx(0xA1BD1C, &FontManagerEx::TextPageAddChar);

		// TextPage::AddChar -> TextLine::AddChar
		WriteRelCallEx(0xA19C80, &FontManagerEx::TextLineAddChar);

		// FontManager::PrepHypertext -> CharData::Copy
		WriteRelCall(0xA17898, &FontManagerEx::CharDataCopy);
		WriteRelCall(0xA179CD, &FontManagerEx::CharDataCopy);
		WriteRelCall(0xA17FB6, &FontManagerEx::CharDataCopy);
		// FontManager::PrepText -> CharData::Copy
		WriteRelCall(0xA18D73, &FontManagerEx::CharDataCopy);

		// Tile::SetString - Quest Text
		WriteRelCall(0x77AF4B, &TileSetStringHookForQueueText);
		// Tile::SetString - Location Text
		WriteRelCall(0x772B5E, &TileSetStringHookForQueueText);

		// BSStringT<char>::c_str - Terminal UTF8 conversion
		WriteRelCall(0x7591AC, &BSString_c_strHook);

		// BSStringT<char>::GetCStringOrEmpty - Location Text UTF8 conversion
		WriteRelCall(0x772B4B, &BSString_GetCStringOrEmptyHook);

		// strcpy_s - Quest Text UTF8 conversion
		WriteRelCall(0x77ACCC, &strcpy_sHook);
		WriteRelCall(0x77ACF8, &strcpy_sHook);
	}

} // namespace fonthook

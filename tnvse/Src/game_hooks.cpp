#include "font_engine.h"
#include "font_manager.h"
#include "game_hooks.h"
#include "load_config.h"
#include "SafeWrite.h"
#include "text_hooks.h"
#include "tnvse.h"

namespace fonthook
{
	void InitBigGunsDescHooks()
	{
		static std::string sConvertedBigGunsDesc = UTF8ToMultiByteStr(g_sNewBigGunsDesc, g_usingWinEncoding);
		SafeWrite32(GetJIPAddress(0x100113BD + 1), (UInt32)sConvertedBigGunsDesc.c_str());
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
		//FontManager::GetLinePadding
		//WriteRelJump(0xA1B3A0, &VertSpacingAdjust);
	}

	void InitFontHook()
	{
		// Font::Font
		WriteRelJumpEx(0xA12020, &FontEx::FontInit);

		// Font::Load
		WriteRelJumpEx(0xA15320, &FontEx::Load);

		// Font::PrepText
		WriteRelJumpEx(0xA12FB0, &FontEx::PrepText);
		WriteRelCallEx(0x759281, &FontEx::PrepTextForTerminal);

		// Font::CreateText
		WriteRelJumpEx(0xA12880, &FontEx::CreateText);

		// Font::MakeString
		WriteRelJumpEx(0xA12460, &FontEx::MakeString);

		// FontManager::CalculateStringDimensions
		WriteRelJumpEx(0xA1B020, &FontManagerEx::CalculateStringDimensions);

		// Tile::SetString - Quest Text
		WriteRelCall(0x77AF4B, &TileSetStringHookForQueueText);
		// Location Text
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

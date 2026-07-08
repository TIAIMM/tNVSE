#include "multibyte_input_internal.h"

#include "InterfaceManager.hpp"
#include "load_config.h"
#include "SafeWrite.h"
#include "tnvse.h"
#include "Tile.hpp"
#include "ui_decode.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <Windows.h>

namespace fonthook
{
	// Forward-declared here so the anonymous-namespace helpers below can call it;
	// the definition appears later in this translation unit.
	bool IsStewieTweaksAvailable();

	namespace
	{
		using StewieKeyboardHandler = bool(__thiscall*)(Menu*, UInt32);

		UInt32 s_tileTraitIsActive = 0;
		UInt32 s_tileTraitIsSearchActive = 0;
		UInt32 s_tileTraitCaretIndex = 0;
		StewieShadowState s_stewieShadow;
		bool s_stewieChecked = false;
		bool s_stewieAvailable = false;
		bool s_stewieHooksInstalled = false;
		SIZE_T s_stewMenuOriginalInputHandler = 0;
		SIZE_T s_stewMenuHookedEntry = 0;

		bool IsCtrlKeyDown()
		{
			return (GetKeyState(VK_CONTROL) & 0x8000) != 0
				|| (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
		}

		UInt32 TileID(Tile* tile)
		{
			if (!tile)
				return 0;

			return static_cast<UInt32>(tile->GetValueFloat(Tile::kTileValue_id));
		}

		float TileTraitFloat(Tile* tile, UInt32 trait)
		{
			return tile && trait ? tile->GetValueFloat(trait) : 0.0f;
		}

		Tile* MenuRoot(Menu* menu)
		{
			return menu ? reinterpret_cast<Tile*>(menu->pRootTile) : nullptr;
		}

		UInt32 MenuID(Menu* menu)
		{
			return menu ? menu->GetID() : 0;
		}

		Menu* GetOpenMenu(UInt32 menuID)
		{
			Tile* root = InterfaceManager::GetMenuByType(menuID);
			if (!root)
				return nullptr;

			Menu* menu = root->GetMenu();
			if (!menu || MenuID(menu) != menuID)
				return nullptr;

			return menu;
		}

		Tile* FindTileByID(Tile* tile, UInt32 id)
		{
			if (!tile)
				return nullptr;

			if (TileID(tile) == id)
				return tile;

			for (Tile* child : tile->GetChildren())
			{
				if (Tile* result = FindTileByID(child, id))
					return result;
			}

			return nullptr;
		}

		Tile* FindStewieActiveInputTile(Tile* tile, UInt32 id)
		{
			if (!tile)
				return nullptr;

			if (TileID(tile) == id && TileTraitFloat(tile, s_tileTraitIsActive) > 0.5f)
				return tile;

			for (Tile* child : tile->GetChildren())
			{
				if (Tile* result = FindStewieActiveInputTile(child, id))
					return result;
			}

			return nullptr;
		}

		std::string TileStringWithoutCaret(Tile* tile, size_t& caret)
		{
			caret = 0;
			std::string text = tile ? tile->GetValueString(Tile::kTileValue_string) : "";
			const size_t caretMarker = text.find('|');
			if (caretMarker != std::string::npos)
			{
				text.erase(caretMarker, 1);
				caret = ClampToPrevBoundary(text, caretMarker);
				return text;
			}

			if (s_tileTraitCaretIndex && TileTraitFloat(tile, s_tileTraitIsActive) > 0.5f)
			{
				const size_t caretIndex = static_cast<size_t>(TileTraitFloat(tile, s_tileTraitCaretIndex));
				if (caretIndex < text.size())
				{
					text.erase(caretIndex, 1);
					caret = ClampToPrevBoundary(text, caretIndex);
					return text;
				}
			}

			caret = text.size();
			return text;
		}

		bool SameStewieTarget(const StewieInputTarget& lhs, const StewieInputTarget& rhs)
		{
			return lhs.valid
				&& rhs.valid
				&& lhs.kind == rhs.kind
				&& lhs.menu == rhs.menu
				&& lhs.tile == rhs.tile
				&& lhs.inputField == rhs.inputField;
		}

		bool LooksLikeActiveMenuSearchTile(Tile* tile)
		{
			if (!tile)
				return false;

			if (s_tileTraitIsActive && TileTraitFloat(tile, s_tileTraitIsActive) > 0.5f)
				return true;

			const float visible = tile->GetValueFloat(Tile::kTileValue_visible);
			const float alpha = tile->GetValueFloat(Tile::kTileValue_alpha);
			return visible > 0.5f && alpha > 200.0f;
		}

		StewieInputTarget MakeStewieTarget(StewieInputKind kind, Menu* menu, Tile* tile, bool inputField)
		{
			StewieInputTarget target;
			target.kind = kind;
			target.menu = menu;
			target.tile = tile;
			target.inputField = inputField;
			target.valid = menu && tile;
			return target;
		}

		bool TryGetStewieInputFieldType(Menu* menu, Tile* tile, UInt8& inputType)
		{
			if (!menu || !tile)
				return false;

			static constexpr UInt32 kInputFieldStringLengthOffset = 0x08;
			static constexpr UInt32 kInputFieldStringCapacityOffset = 0x0A;
			static constexpr UInt32 kInputFieldActiveOffset = 0x0C;
			static constexpr UInt32 kInputFieldCaretOffset = 0x0E;
			static constexpr UInt32 kInputFieldTypeOffset = 0x14;

			__try
			{
				auto* base = reinterpret_cast<UInt8*>(menu);
				for (UInt32 offset = 0; offset < 0x2000; offset += sizeof(void*))
				{
					if (*reinterpret_cast<Tile**>(base + offset) != tile)
						continue;

					const bool isActive = *reinterpret_cast<bool*>(base + offset + kInputFieldActiveOffset);
					const UInt16 length = *reinterpret_cast<UInt16*>(base + offset + kInputFieldStringLengthOffset);
					const UInt16 capacity = *reinterpret_cast<UInt16*>(base + offset + kInputFieldStringCapacityOffset);
					const SInt16 caret = *reinterpret_cast<SInt16*>(base + offset + kInputFieldCaretOffset);
					const UInt8 candidateType = *reinterpret_cast<UInt8*>(base + offset + kInputFieldTypeOffset);
					if (isActive
						&& candidateType <= 3
						&& capacity < 0x10000
						&& length <= capacity
						&& caret >= 0
						&& static_cast<UInt16>(caret) <= length)
					{
						inputType = candidateType;
						return true;
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}

			return false;
		}

		Tile* FindStewieInputFieldTile(Menu* menu, Tile* tile, UInt32 id, UInt8& inputType)
		{
			if (!tile)
				return nullptr;

			if (TileID(tile) == id && TryGetStewieInputFieldType(menu, tile, inputType))
				return tile;

			for (Tile* child : tile->GetChildren())
			{
				if (Tile* result = FindStewieInputFieldTile(menu, child, id, inputType))
					return result;
			}

			return nullptr;
		}

		StewieInputTarget FindStewMenuTarget(Menu* menu)
		{
			if (!menu || MenuID(menu) != kMenuType_StewMenu)
				return {};

			Tile* root = MenuRoot(menu);
			if (!root)
				return {};

			UInt8 searchInputType = 0xFF;
			if (Tile* searchInputTile = FindStewieInputFieldTile(menu, root, kStewMenu_SearchBar, searchInputType);
				searchInputTile && searchInputType == 0)
				return MakeStewieTarget(StewieInputKind::StewMenuSearch, menu, searchInputTile, true);

			Tile* searchTile = FindTileByID(root, kStewMenu_SearchBar);
			const float searchRootActive = TileTraitFloat(root, s_tileTraitIsSearchActive);
			const float searchTileActive = TileTraitFloat(searchTile, s_tileTraitIsActive);
			if (searchTile
				&& (searchTileActive > 0.5f
					|| (searchRootActive > 0.5f && searchRootActive < 1.5f)))
				return MakeStewieTarget(StewieInputKind::StewMenuSearch, menu, searchTile, true);

			UInt8 inputType = 0xFF;
			Tile* subsettingTile = FindStewieInputFieldTile(menu, root, kStewMenu_SubsettingInputFieldText, inputType);
			if (subsettingTile && inputType == 0)
				return MakeStewieTarget(StewieInputKind::StewMenuStringSubsetting, menu, subsettingTile, true);

			return {};
		}

		StewieInputTarget FindStewieMenuSearchTarget(Menu* menu)
		{
			if (!menu)
				return {};

			Tile* searchTile = FindTileByID(MenuRoot(menu), kStewieMenuSearch_TextTile);
			if (!LooksLikeActiveMenuSearchTile(searchTile))
				return {};

			const bool inputField = s_tileTraitIsActive && TileTraitFloat(searchTile, s_tileTraitIsActive) > 0.5f;
			return MakeStewieTarget(StewieInputKind::MenuSearch, menu, searchTile, inputField);
		}

		StewieInputTarget FindStewieTargetForMenu(Menu* menu)
		{
			if (!IsStewieTweaksAvailable() || !menu)
				return {};

			if (MenuID(menu) == kMenuType_StewMenu)
				return FindStewMenuTarget(menu);

			return FindStewieMenuSearchTarget(menu);
		}

		SIZE_T OriginalStewieHandlerForMenu(Menu* menu)
		{
			if (!menu)
				return 0;

			const UInt32 menuID = MenuID(menu);
			if (menuID == kMenuType_StewMenu)
				return s_stewMenuOriginalInputHandler;

			for (const StewieMenuHook& hook : g_stewieMenuHooks)
			{
				if (hook.menuID == menuID)
					return hook.original;
			}

			return 0;
		}

		bool CallStewieOriginalInput(Menu* menu, UInt32 input)
		{
			const SIZE_T original = OriginalStewieHandlerForMenu(menu);
			if (!original)
				return false;

			return reinterpret_cast<StewieKeyboardHandler>(original)(menu, input);
		}

		void EnsureStewieShadow(const StewieInputTarget& target)
		{
			if (SameStewieTarget(s_stewieShadow.target, target))
				return;

			s_stewieShadow.target = target;
			s_stewieShadow.text = TileStringWithoutCaret(target.tile, s_stewieShadow.caret);
			if (!target.inputField && s_stewieShadow.text == "_")
				s_stewieShadow.text.clear();
			s_stewieShadow.caret = ClampToPrevBoundary(s_stewieShadow.text, s_stewieShadow.caret);
			s_stewieShadow.appliedBytes = s_stewieShadow.text.size();
			s_stewieShadow.initialized = target.valid;
		}

		bool ReplayStewieShadow(const StewieInputTarget& target)
		{
			if (!target.valid || !s_stewieShadow.initialized)
				return false;

			const size_t clearCount = std::min<size_t>(s_stewieShadow.appliedBytes, kStewieMaxShadowBytes);

			s_stewieReplay = true;
			CallStewieOriginalInput(target.menu, kInputCode_End);
			for (size_t i = 0; i < clearCount; ++i)
				CallStewieOriginalInput(target.menu, kInputCode_Backspace);

			for (unsigned char ch : s_stewieShadow.text)
				CallStewieOriginalInput(target.menu, ch);

			const size_t caret = ClampToPrevBoundary(s_stewieShadow.text, s_stewieShadow.caret);
			for (size_t i = caret; i < s_stewieShadow.text.size(); ++i)
				CallStewieOriginalInput(target.menu, kInputCode_ArrowLeft);
			s_stewieReplay = false;

			s_stewieShadow.appliedBytes = s_stewieShadow.text.size();
			return true;
		}

		bool FitsStewieShadow(const std::string& candidate)
		{
			return candidate.size() <= kStewieMaxShadowBytes;
		}

		bool CommitStewieShadow(const StewieInputTarget& target, std::string candidate, size_t caret)
		{
			if (!FitsStewieShadow(candidate))
				return false;

			s_stewieShadow.target = target;
			s_stewieShadow.text = std::move(candidate);
			s_stewieShadow.caret = ClampToPrevBoundary(s_stewieShadow.text, caret);
			s_stewieShadow.initialized = true;
			return ReplayStewieShadow(target);
		}

		bool DeletePreviousStewieChar(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			size_t caret = ClampToPrevBoundary(s_stewieShadow.text, s_stewieShadow.caret);
			if (!caret)
				return true;

			const size_t previous = PrevCharBoundary(s_stewieShadow.text, caret);
			std::string candidate = s_stewieShadow.text;
			candidate.erase(previous, caret - previous);
			return CommitStewieShadow(target, std::move(candidate), previous);
		}

		bool DeleteNextStewieChar(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			size_t caret = ClampToPrevBoundary(s_stewieShadow.text, s_stewieShadow.caret);
			if (caret >= s_stewieShadow.text.size())
				return true;

			const size_t next = NextCharBoundary(s_stewieShadow.text, caret);
			std::string candidate = s_stewieShadow.text;
			candidate.erase(caret, next - caret);
			return CommitStewieShadow(target, std::move(candidate), caret);
		}

		bool MoveStewieCaretPrevious(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			return CommitStewieShadow(target, s_stewieShadow.text, PrevCharBoundary(s_stewieShadow.text, s_stewieShadow.caret));
		}

		bool MoveStewieCaretNext(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			return CommitStewieShadow(target, s_stewieShadow.text, NextCharBoundary(s_stewieShadow.text, s_stewieShadow.caret));
		}

		bool MoveStewieCaretHome(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			return CommitStewieShadow(target, s_stewieShadow.text, 0);
		}

		bool MoveStewieCaretEnd(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			return CommitStewieShadow(target, s_stewieShadow.text, s_stewieShadow.text.size());
		}

		bool InstallStewieHook(StewieMenuHook& hook)
		{
			if (hook.installed)
				return true;

			SIZE_T current = *reinterpret_cast<SIZE_T*>(hook.entry);
			if (current == hook.hook)
			{
				hook.installed = true;
				return true;
			}

			hook.original = current;
			SafeWrite32(hook.entry, hook.hook);
			hook.installed = true;
			DebugLog(
				"tnvse_multibyte_input: chained Stewie %s handler=0x%08X",
				hook.name,
				static_cast<UInt32>(current));
			return true;
		}
	}

	// Shared hook-table needs the adapter member addresses; the class lives in
	// the named fonthook namespace below and its members are referenced here.
	class StewieTweaksInputTargetEx
	{
	public:
		static bool __fastcall StewMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
		static bool __fastcall InventoryMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
		static bool __fastcall StatsMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
		static bool __fastcall MapMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
		static bool __fastcall ContainerMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
		static bool __fastcall BarterMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
		static bool __fastcall LevelUpMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
		static bool __fastcall RecipeMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
		static bool __fastcall StartMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
	};

	static StewieMenuHook g_stewieMenuHooks[] =
	{
		{ "InventoryMenu", Inventory, kInventoryMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieTweaksInputTargetEx::InventoryMenuKeyboardInput), false },
		{ "StatsMenu", Stats, kStatsMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieTweaksInputTargetEx::StatsMenuKeyboardInput), false },
		{ "MapMenu", PipboyData, kMapMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieTweaksInputTargetEx::MapMenuKeyboardInput), false },
		{ "ContainerMenu", Container, kContainerMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieTweaksInputTargetEx::ContainerMenuKeyboardInput), false },
		{ "BarterMenu", Barter, kBarterMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieTweaksInputTargetEx::BarterMenuKeyboardInput), false },
		{ "LevelUpMenu", LevelUp, kLevelUpMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieTweaksInputTargetEx::LevelUpMenuKeyboardInput), false },
		{ "RecipeMenu", Recipe, kRecipeMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieTweaksInputTargetEx::RecipeMenuKeyboardInput), false },
		{ "StartMenu", Pause, kStartMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieTweaksInputTargetEx::StartMenuKeyboardInput), false },
	};

	bool IsStewieTweaksAvailable()
	{
		if (!g_bMultibyteInputStewieTweaks || !g_cmdTableInterface)
			return false;

		if (s_stewieChecked)
			return s_stewieAvailable;

		const PluginInfo* info = g_cmdTableInterface->GetPluginInfoByName(kStewieTweaksPluginName);
		if (!info)
			return false;

		s_stewieChecked = true;
		s_stewieAvailable = info
			&& info->version >= kStewieTweaksMinVersion;
		if (info && !s_stewieAvailable)
		{
			gLog.FormattedMessage(
				"tnvse_multibyte_input: Stewie Tweaks version %u is older than supported minimum %u; Stewie input adapter disabled",
				info->version,
				kStewieTweaksMinVersion);
		}
		else if (s_stewieAvailable)
		{
			gLog.FormattedMessage(
				"tnvse_multibyte_input: Stewie Tweaks version %u detected",
				info->version);
		}

		return s_stewieAvailable;
	}

	StewieInputTarget GetActiveStewieInputTarget()
	{
		if (!IsStewieTweaksAvailable())
			return {};

		if (Menu* menu = GetOpenMenu(kMenuType_StewMenu))
		{
			if (StewieInputTarget target = FindStewMenuTarget(menu); target.valid)
				return target;
		}

		for (const StewieMenuHook& hook : g_stewieMenuHooks)
		{
			if (Menu* menu = GetOpenMenu(hook.menuID))
			{
				if (StewieInputTarget target = FindStewieMenuSearchTarget(menu); target.valid)
					return target;
			}
		}

		return {};
	}

	StewieInputTarget GetOverlayStewieInputTarget()
	{
		if (StewieInputTarget target = GetActiveStewieInputTarget(); target.valid)
			return target;

		if (s_stewieShadow.initialized)
		{
			StewieInputTarget target = FindStewieTargetForMenu(s_stewieShadow.target.menu);
			if (target.valid)
				return target;
		}

		return {};
	}

	bool InsertTextAtCaretStewie(const StewieInputTarget& target, std::string_view text)
	{
		if (!target.valid || text.empty())
			return false;

		EnsureStewieShadow(target);
		std::string candidate = s_stewieShadow.text;
		size_t caret = ClampToPrevBoundary(candidate, s_stewieShadow.caret);
		candidate.insert(caret, text.data(), text.size());
		return CommitStewieShadow(target, std::move(candidate), caret + text.size());
	}

	bool InsertWideTextStewie(const StewieInputTarget& target, std::wstring_view text)
	{
		std::string converted = WideToCurrentCodePage(text);
		if (converted.empty())
			return false;

		return InsertTextAtCaretStewie(target, converted);
	}

	bool RemovePreviousStewieAsciiCompositionEcho(wchar_t compositionLead)
	{
		StewieInputTarget target = GetOverlayStewieInputTarget();
		if (!target.valid)
			return false;

		EnsureStewieShadow(target);
		size_t caret = ClampToPrevBoundary(s_stewieShadow.text, s_stewieShadow.caret);
		if (!caret)
			return false;

		const size_t previous = PrevCharBoundary(s_stewieShadow.text, caret);
		if (caret - previous != 1)
			return false;

		if (!AsciiEqualsIgnoreCase(static_cast<UInt8>(s_stewieShadow.text[previous]), compositionLead))
			return false;

		std::string candidate = s_stewieShadow.text;
		candidate.erase(previous, 1);
		return CommitStewieShadow(target, std::move(candidate), previous);
	}

	void ClearStewieInputState()
	{
		s_stewieShadow = StewieShadowState();
	}

	namespace
	{
		bool HandleStewieInput(Menu* menu, UInt32 input)
		{
			if (s_stewieReplay)
				return CallStewieOriginalInput(menu, input);

			StewieInputTarget target = FindStewieTargetForMenu(menu);
			if (!target.valid)
			{
				DebugLog(
					"tnvse_multibyte_input_event: source=StewieTweaksInputTarget action=target_miss menu=%u input=0x%08X",
					MenuID(menu),
					input);
				if (MenuID(menu) == kMenuType_StewMenu)
				{
					Tile* root = MenuRoot(menu);
					Tile* searchTile = FindTileByID(root, kStewMenu_SearchBar);
					UInt8 searchInputType = 0xFF;
					Tile* searchInputTile = FindStewieInputFieldTile(menu, root, kStewMenu_SearchBar, searchInputType);
					UInt8 subsettingInputType = 0xFF;
					Tile* subsettingTile = FindStewieInputFieldTile(menu, root, kStewMenu_SubsettingInputFieldText, subsettingInputType);
					DebugLog(
						"tnvse_multibyte_input_event: source=StewieTweaksInputTarget action=target_miss_stewmenu root=0x%08X search=0x%08X searchInput=0x%08X searchType=%u searchRootActive=%.1f searchTileActive=%.1f subsetting=0x%08X subsettingType=%u",
						reinterpret_cast<UInt32>(root),
						reinterpret_cast<UInt32>(searchTile),
						reinterpret_cast<UInt32>(searchInputTile),
						searchInputType,
						TileTraitFloat(root, s_tileTraitIsSearchActive),
						TileTraitFloat(searchTile, s_tileTraitIsActive),
						reinterpret_cast<UInt32>(subsettingTile),
						subsettingInputType);
				}
				return CallStewieOriginalInput(menu, input);
			}

			EnsureStewieShadow(target);

			if (IsCtrlKeyDown())
			{
				const bool handled = CallStewieOriginalInput(menu, input);
				ClearStewieInputState();
				return handled;
			}

			if (IsImeCompositionActive())
			{
				DebugLog(
					"tnvse_multibyte_input_event: source=StewieTweaksInputTarget action=suppress_composition_input menu=%u input=0x%08X",
					MenuID(menu),
					input);
				return true;
			}

			if (input >= 0x20 && input <= 0x7E)
			{
				if (IsImeConsumingAscii())
					return true;

				if (s_lastWndProcAsciiChar == static_cast<UInt8>(input)
					&& GetTickCount() - s_lastWndProcAsciiTick <= kDuplicateAsciiSuppressMs)
				{
					s_lastWndProcAsciiChar = 0;
					return true;
				}

				const char ch = static_cast<char>(input);
				return InsertTextAtCaretStewie(target, std::string_view(&ch, 1));
			}

			if (input > 0x7F && input <= 0xFF)
				return true;

			switch (input)
			{
			case kInputCode_Backspace:
				return DeletePreviousStewieChar(target);
			case kInputCode_Delete:
				return DeleteNextStewieChar(target);
			case kInputCode_ArrowLeft:
				return MoveStewieCaretPrevious(target);
			case kInputCode_ArrowRight:
				return MoveStewieCaretNext(target);
			case kInputCode_Home:
				return MoveStewieCaretHome(target);
			case kInputCode_End:
				return MoveStewieCaretEnd(target);
			case kInputCode_Enter:
				ClearStewieInputState();
				return CallStewieOriginalInput(menu, input);
			default:
				return CallStewieOriginalInput(menu, input);
			}
		}
	}

	void TryInstallStewieTweaksInputHooks()
	{
		if (!IsStewieTweaksAvailable())
			return;

		if (!s_tileTraitIsActive)
			s_tileTraitIsActive = Tile::TraitNameToID("_IsActive");
		if (!s_tileTraitIsSearchActive)
			s_tileTraitIsSearchActive = Tile::TraitNameToID("_IsSearchActive");
		if (!s_tileTraitCaretIndex)
			s_tileTraitCaretIndex = Tile::TraitNameToID("_CaretIndex");

		if (!s_stewieHooksInstalled)
		{
			for (StewieMenuHook& hook : g_stewieMenuHooks)
				InstallStewieHook(hook);

			s_stewieHooksInstalled = true;
			gLog.FormattedMessage("tnvse_multibyte_input: Stewie Tweaks menu search input adapter installed");
		}

		if (Menu* stewMenu = GetOpenMenu(kMenuType_StewMenu))
		{
			SIZE_T entry = *reinterpret_cast<SIZE_T*>(stewMenu) + kMenuHandleKeyboardInputVTableOffset;
			SIZE_T current = *reinterpret_cast<SIZE_T*>(entry);
			const SIZE_T hook = reinterpret_cast<SIZE_T>(&StewieTweaksInputTargetEx::StewMenuKeyboardInput);
			if (current != hook)
			{
				s_stewMenuOriginalInputHandler = current;
				s_stewMenuHookedEntry = entry;
				SafeWrite32(entry, hook);
				DebugLog(
					"tnvse_multibyte_input: chained StewMenu handler=0x%08X",
					static_cast<UInt32>(current));
				gLog.FormattedMessage("tnvse_multibyte_input: Stewie Tweaks StewMenu input adapter installed");
			}
		}
	}

	bool __fastcall StewieTweaksInputTargetEx::StewMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
	{
		return HandleStewieInput(apMenu, aiInput);
	}

	bool __fastcall StewieTweaksInputTargetEx::InventoryMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
	{
		return HandleStewieInput(apMenu, aiInput);
	}

	bool __fastcall StewieTweaksInputTargetEx::StatsMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
	{
		return HandleStewieInput(apMenu, aiInput);
	}

	bool __fastcall StewieTweaksInputTargetEx::MapMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
	{
		return HandleStewieInput(apMenu, aiInput);
	}

	bool __fastcall StewieTweaksInputTargetEx::ContainerMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
	{
		return HandleStewieInput(apMenu, aiInput);
	}

	bool __fastcall StewieTweaksInputTargetEx::BarterMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
	{
		return HandleStewieInput(apMenu, aiInput);
	}

	bool __fastcall StewieTweaksInputTargetEx::LevelUpMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
	{
		return HandleStewieInput(apMenu, aiInput);
	}

	bool __fastcall StewieTweaksInputTargetEx::RecipeMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
	{
		return HandleStewieInput(apMenu, aiInput);
	}

	bool __fastcall StewieTweaksInputTargetEx::StartMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
	{
		return HandleStewieInput(apMenu, aiInput);
	}
}
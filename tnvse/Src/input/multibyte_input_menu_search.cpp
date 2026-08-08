#include "multibyte_input_internal.h"

// Stewie Tweaks MenuSearch integration for vanilla game menus.

namespace fonthook
{
	namespace multibyte_input
	{
		// Unlike GetMenuByType(), Menu::pMenusVisible changes when a persistent
		// menu is hidden or closed. This base is deliberately biased so an
		// absolute menu ID indexes the real table at 0x11F3478.
		constexpr SIZE_T kMenuVisibilityTableBiasedBase = 0x011F308F;
		constexpr UInt32 kMenuType_StewMenu = 1069;
		constexpr UInt32 kStewieMenuSearch_TextTile = 87698483;
		constexpr DWORD kStewieMenuSearchStateSyncDelayMs = 150;
		constexpr DWORD kStewieMenuSearchStateSyncRetryMs = 50;
		constexpr DWORD kStewieMenuSearchStateSyncTimeoutMs = 1000;
		constexpr DWORD kStewieMenuHandlerStableDelayMs = 1000;
		constexpr DWORD kStewieMenuSearchDiscoveryIntervalMs = 50;
		constexpr UInt8 kStewieMenuSearchSync_None = 0;
		constexpr UInt8 kStewieMenuSearchSync_Toggle = 1;
		constexpr UInt8 kStewieMenuSearchSync_Deactivate = 2;

		constexpr SIZE_T kInventoryMenuHandleKeyboardInputVTableEntry = 0x10739E4;
		constexpr SIZE_T kStatsMenuHandleKeyboardInputVTableEntry = 0x1070004;
		constexpr SIZE_T kMapMenuHandleKeyboardInputVTableEntry = 0x1074D74;
		constexpr SIZE_T kContainerMenuHandleKeyboardInputVTableEntry = 0x10721DC;
		constexpr SIZE_T kBarterMenuHandleKeyboardInputVTableEntry = 0x107071C;
		constexpr SIZE_T kLevelUpMenuHandleKeyboardInputVTableEntry = 0x1073D0C;
		constexpr SIZE_T kRecipeMenuHandleKeyboardInputVTableEntry = 0x10704BC;
		constexpr SIZE_T kStartMenuHandleKeyboardInputVTableEntry = 0x1076D4C;
		// StartMenu::savesList is at +0x174; ListBox::parentTile is +0x0C.
		// Stewie's SaveLoad search handler gates all input on
		// savesList.IsEnabled(), which reads this Tile's _enabled trait.
		constexpr UInt32 kStartMenuSavesListParentTileOffset = 0x180;

		struct StewieMenuSearchHook
		{
			const char* name = "";
			UInt32 menuID = 0;
			SIZE_T entry = 0;
			SIZE_T original = 0;
			SIZE_T hook = 0;
			bool installed = false;
			SIZE_T observedHandler = 0;

			Tile* root = nullptr;
			Tile* tile = nullptr;
			DWORD seenTick = 0;
			bool keyboardActive = false;
			bool stateSyncPending = false;
			UInt8 stateSyncAction = kStewieMenuSearchSync_None;
			bool stateSyncWasActive = false;
			DWORD stateSyncStartTick = 0;
			DWORD stateSyncDueTick = 0;
			bool targetReported = false;
		};

		class StewieMenuSearchInputTargetEx
		{
		public:
			static bool __fastcall InventoryMenuKeyboardInput(Menu* menu, void*, UInt32 input);
			static bool __fastcall StatsMenuKeyboardInput(Menu* menu, void*, UInt32 input);
			static bool __fastcall MapMenuKeyboardInput(Menu* menu, void*, UInt32 input);
			static bool __fastcall ContainerMenuKeyboardInput(Menu* menu, void*, UInt32 input);
			static bool __fastcall BarterMenuKeyboardInput(Menu* menu, void*, UInt32 input);
			static bool __fastcall LevelUpMenuKeyboardInput(Menu* menu, void*, UInt32 input);
			static bool __fastcall RecipeMenuKeyboardInput(Menu* menu, void*, UInt32 input);
			static bool __fastcall StartMenuKeyboardInput(Menu* menu, void*, UInt32 input);
		};

		StewieMenuSearchHook s_menuSearchHooks[] =
		{
			{ "InventoryMenu", Inventory, kInventoryMenuHandleKeyboardInputVTableEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputTargetEx::InventoryMenuKeyboardInput) },
			{ "StatsMenu", Stats, kStatsMenuHandleKeyboardInputVTableEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputTargetEx::StatsMenuKeyboardInput) },
			{ "MapMenu", PipboyData, kMapMenuHandleKeyboardInputVTableEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputTargetEx::MapMenuKeyboardInput) },
			{ "ContainerMenu", Container, kContainerMenuHandleKeyboardInputVTableEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputTargetEx::ContainerMenuKeyboardInput) },
			{ "BarterMenu", Barter, kBarterMenuHandleKeyboardInputVTableEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputTargetEx::BarterMenuKeyboardInput) },
			{ "LevelUpMenu", LevelUp, kLevelUpMenuHandleKeyboardInputVTableEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputTargetEx::LevelUpMenuKeyboardInput) },
			{ "RecipeMenu", Recipe, kRecipeMenuHandleKeyboardInputVTableEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputTargetEx::RecipeMenuKeyboardInput) },
			{ "StartMenu", Pause, kStartMenuHandleKeyboardInputVTableEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputTargetEx::StartMenuKeyboardInput) },
		};

		bool s_menuSearchHooksInstalled = false;
		DWORD s_menuHandlersStableSince = 0;
		DWORD s_lastMenuSearchDiscoveryTick = 0;

		bool IsGameMenuVisible(UInt32 menuID)
		{
			return menuID
				&& reinterpret_cast<volatile UInt8*>(
					kMenuVisibilityTableBiasedBase)[menuID] != 0;
		}

		bool IsPipboySearchMenu(UInt32 menuID)
		{
			return menuID == Inventory || menuID == Stats || menuID == PipboyData;
		}

		bool MenuSearchUsesInputField(UInt32 menuID)
		{
			return IsPipboySearchMenu(menuID);
		}

		UInt32 MenuSearchInputActiveTrait()
		{
			static const UInt32 trait = Tile::TraitNameToID("_IsActive");
			return trait;
		}

		UInt32 MenuSearchEnabledTrait()
		{
			static const UInt32 trait = Tile::TraitNameToID("_enabled");
			return trait;
		}

		bool TileTreeContains(Tile* root, Tile* target, UInt32 depth = 0);

		bool IsStartMenuSaveListEnabled(Menu* menu)
		{
			if (!menu || MenuID(menu) != Pause)
				return false;

			__try
			{
				Tile* parent = *reinterpret_cast<Tile**>(
					reinterpret_cast<UInt8*>(menu)
					+ kStartMenuSavesListParentTileOffset);
				Tile* root = MenuRoot(menu);
				const UInt32 enabledTrait = MenuSearchEnabledTrait();
				return parent
					&& root
					&& enabledTrait
					&& TileTreeContains(root, parent)
					&& parent->GetValueFloat(enabledTrait) > 0.5f;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool MenuSearchOwnerReportsActive(
			const StewieMenuSearchHook& hook,
			Menu* menu,
			Tile* tile)
		{
			if (!tile)
				return false;

			if (MenuSearchUsesInputField(hook.menuID))
			{
				const UInt32 activeTrait = MenuSearchInputActiveTrait();
				return activeTrait
					&& tile->GetValueFloat(activeTrait) > 0.5f;
			}

			if (hook.menuID == Pause)
				return IsStartMenuSaveListEnabled(menu);

			// Plain SearchBar has no stable active trait. Its Ctrl+F/Ctrl+R
			// handler plus the exact owning-menu lifetime remain authoritative.
			return true;
		}

		StewieMenuSearchHook* FindMenuSearchHookByMenuID(UInt32 menuID)
		{
			for (StewieMenuSearchHook& hook : s_menuSearchHooks)
			{
				if (hook.menuID == menuID)
					return &hook;
			}

			return nullptr;
		}

		StewieMenuSearchHook* FindMenuSearchHookByMenu(Menu* menu)
		{
			return menu ? FindMenuSearchHookByMenuID(MenuID(menu)) : nullptr;
		}

		void ResetMenuSearchStateSync(StewieMenuSearchHook& hook)
		{
			hook.stateSyncPending = false;
			hook.stateSyncAction = kStewieMenuSearchSync_None;
			hook.stateSyncWasActive = false;
			hook.stateSyncStartTick = 0;
			hook.stateSyncDueTick = 0;
		}

		bool TileTreeContains(Tile* root, Tile* target, UInt32 depth)
		{
			if (!root || !target || depth > 64)
				return false;
			if (root == target)
				return true;

			for (Tile* child : root->GetChildren())
			{
				if (TileTreeContains(child, target, depth + 1))
					return true;
			}

			return false;
		}

		Tile* GetTrackedMenuSearchTile(Menu* menu)
		{
			StewieMenuSearchHook* hook = FindMenuSearchHookByMenu(menu);
			if (!hook || !hook->tile)
				return nullptr;

			Tile* root = MenuRoot(menu);
			if (!root)
				return nullptr;
			if (TileTreeContains(root, hook->tile))
				return hook->tile;

			DebugLog(
				"tnvse_multibyte_input_debug: menusearch_track_stale name=%s menu=%u oldRoot=0x%08X newRoot=0x%08X oldTile=0x%08X",
				hook->name,
				hook->menuID,
				reinterpret_cast<UInt32>(hook->root),
				reinterpret_cast<UInt32>(root),
				reinterpret_cast<UInt32>(hook->tile));

			hook->root = nullptr;
			hook->tile = nullptr;
			hook->seenTick = 0;
			hook->targetReported = false;
			return nullptr;
		}

		void DebugLogMenuSearchState(const char* stage, StewieMenuSearchHook& hook, Menu* menu)
		{
			if (!g_bMultibyteInputLog)
				return;
			if (!menu)
				menu = GetOpenMenu(hook.menuID);

			Tile* root = MenuRoot(menu);
			Tile* trackedTile = nullptr;
			if (root && hook.tile && TileTreeContains(root, hook.tile))
				trackedTile = hook.tile;

			Tile* fallbackTile = root ? FindTileByID(root, kStewieMenuSearch_TextTile) : nullptr;
			Tile* resolvedTile = trackedTile ? trackedTile : fallbackTile;
			InterfaceManager* manager = InterfaceManager::GetSingleton();
			Menu* activeMenu = manager ? manager->pActiveMenu : nullptr;
			const SInt32 dueInMs = hook.stateSyncPending
				? static_cast<SInt32>(hook.stateSyncDueTick - GetTickCount())
				: 0;
			const bool ownerActive = MenuSearchOwnerReportsActive(
				hook, menu, resolvedTile);
			const SIZE_T currentHandler = hook.entry
				? *reinterpret_cast<SIZE_T*>(hook.entry)
				: 0;

			gLog.FormattedMessage(
				"tnvse_multibyte_input_menusearch: stage=%s name=%s menuID=%u inputField=%u "
				"gameVisible=%u pipVisible=%u/%u/%u menu=0x%08X activeMenu=0x%08X activeMenuID=%u "
				"root=0x%08X trackedRaw=0x%08X tracked=0x%08X fallback=0x%08X resolved=0x%08X "
				"tileID=%u tileVisible=%.1f tileAlpha=%.1f keyboardActive=%u pending=%u action=%u "
				"wasActive=%u dueInMs=%d ownerActive=%u installed=%u currentHandler=0x%08X expectedHandler=0x%08X "
				"originalHandler=0x%08X string=\"%s\"",
				stage ? stage : "unknown",
				hook.name,
				hook.menuID,
				MenuSearchUsesInputField(hook.menuID) ? 1 : 0,
				IsGameMenuVisible(hook.menuID) ? 1 : 0,
				IsGameMenuVisible(Inventory) ? 1 : 0,
				IsGameMenuVisible(Stats) ? 1 : 0,
				IsGameMenuVisible(PipboyData) ? 1 : 0,
				reinterpret_cast<UInt32>(menu),
				reinterpret_cast<UInt32>(activeMenu),
				MenuID(activeMenu),
				reinterpret_cast<UInt32>(root),
				reinterpret_cast<UInt32>(hook.tile),
				reinterpret_cast<UInt32>(trackedTile),
				reinterpret_cast<UInt32>(fallbackTile),
				reinterpret_cast<UInt32>(resolvedTile),
				TileID(resolvedTile),
				resolvedTile ? resolvedTile->GetValueFloat(Tile::kTileValue_visible) : 0.0f,
				resolvedTile ? resolvedTile->GetValueFloat(Tile::kTileValue_alpha) : 0.0f,
				hook.keyboardActive ? 1 : 0,
				hook.stateSyncPending ? 1 : 0,
				static_cast<UInt32>(hook.stateSyncAction),
				hook.stateSyncWasActive ? 1 : 0,
				dueInMs,
				ownerActive ? 1 : 0,
				hook.installed ? 1 : 0,
				static_cast<UInt32>(currentHandler),
				static_cast<UInt32>(hook.hook),
				static_cast<UInt32>(hook.original),
				resolvedTile ? resolvedTile->GetValueString(Tile::kTileValue_string) : "");
		}

		void DeactivateMenuSearch(StewieMenuSearchHook& hook, const char* reason, Tile* tile = nullptr)
		{
			if (!hook.keyboardActive && !hook.stateSyncPending)
				return;

			DebugLogMenuSearchState(reason, hook, GetOpenMenu(hook.menuID));
			hook.keyboardActive = false;
			hook.targetReported = false;
			ResetMenuSearchStateSync(hook);
			ClearStewieInputState();
			EndStewieTextInputSession(reason);

			DebugLog(
				"tnvse_multibyte_input_event: source=StatePoll action=menusearch_deactivate reason=%s menu=%u tile=0x%08X visible=%.1f alpha=%.1f",
				reason ? reason : "unknown",
				hook.menuID,
				reinterpret_cast<UInt32>(tile),
				tile ? tile->GetValueFloat(Tile::kTileValue_visible) : 0.0f,
				tile ? tile->GetValueFloat(Tile::kTileValue_alpha) : 0.0f);
		}

		void TrackMenuSearchTile(StewieMenuSearchHook& hook, Tile* root, Tile* tile)
		{
			if (!tile)
				return;

			if ((hook.keyboardActive || hook.stateSyncPending)
				&& (hook.root != root || hook.tile != tile))
			{
				DeactivateMenuSearch(hook, "tile_replaced", hook.tile);
			}

			hook.root = root;
			hook.tile = tile;
			hook.seenTick = GetTickCount();
			hook.keyboardActive = false;
			hook.targetReported = false;
			ResetMenuSearchStateSync(hook);

			DebugLog(
				"tnvse_multibyte_input_debug: menusearch_track name=%s menu=%u root=0x%08X tile=0x%08X id=%u source=main_loop string='%s'",
				hook.name,
				hook.menuID,
				reinterpret_cast<UInt32>(root),
				reinterpret_cast<UInt32>(tile),
				TileID(tile),
				tile->GetValueString(Tile::kTileValue_string));
			DebugLogMenuSearchState("tile_tracked", hook, GetOpenMenu(hook.menuID));
		}

		void DiscoverMenuSearchTiles()
		{
			const DWORD now = GetTickCount();
			if (s_lastMenuSearchDiscoveryTick
				&& now - s_lastMenuSearchDiscoveryTick
					< kStewieMenuSearchDiscoveryIntervalMs)
			{
				return;
			}
			s_lastMenuSearchDiscoveryTick = now;

			for (StewieMenuSearchHook& hook : s_menuSearchHooks)
			{
				if (!IsGameMenuVisible(hook.menuID))
					continue;

				Menu* menu = GetOpenMenu(hook.menuID);
				Tile* root = MenuRoot(menu);
				if (root && hook.root == root && hook.tile)
					continue;

				Tile* tile = root ? FindTileByID(root, kStewieMenuSearch_TextTile) : nullptr;
				if (!tile || (hook.root == root && hook.tile == tile))
					continue;

				TrackMenuSearchTile(hook, root, tile);
			}
		}

		StewieInputTarget FindStewieMenuSearchTarget(Menu* menu)
		{
			if (!menu)
				return {};

			StewieMenuSearchHook* hook = FindMenuSearchHookByMenu(menu);
			if (!hook || !hook->installed || !hook->keyboardActive)
				return {};

			Tile* root = MenuRoot(menu);
			if (!root)
				return {};

			Tile* searchTile = GetTrackedMenuSearchTile(menu);
			if (!searchTile)
			{
				// Fallback for the short interval before main-loop discovery refreshes
				// its cached tile. Tile presence identifies the edit target;
				// keyboardActive remains the input-session source of truth.
				searchTile = FindTileByID(root, kStewieMenuSearch_TextTile);
			}

			if (!searchTile)
			{
				DeactivateMenuSearch(*hook, "no_tracked_searchbar");
				DebugLog(
					"tnvse_multibyte_input_debug: menusearch_target_miss reason=no_tracked_searchbar menu=%u root=0x%08X legacyID=%u",
					MenuID(menu),
					reinterpret_cast<UInt32>(root),
					kStewieMenuSearch_TextTile);
				return {};
			}

			if (!hook->targetReported)
			{
				DebugLogMenuSearchState("target_found", *hook, menu);
				hook->targetReported = true;
			}

			return MakeStewieTarget(
				StewieInputKind::MenuSearch,
				menu,
				searchTile,
				MenuSearchUsesInputField(MenuID(menu)));
		}

		StewieInputTarget GetActiveStewieMenuSearchTarget()
		{
			if (!IsStewieTweaksAvailable())
				return {};

			for (const StewieMenuSearchHook& hook : s_menuSearchHooks)
			{
				if (!IsGameMenuVisible(hook.menuID))
					continue;

				if (Menu* menu = GetOpenMenu(hook.menuID))
				{
					if (StewieInputTarget target = FindStewieMenuSearchTarget(menu); target.valid)
						return target;
				}
			}

			return {};
		}

		SIZE_T GetStewieMenuSearchOriginalInputHandler(Menu* menu)
		{
			StewieMenuSearchHook* hook = FindMenuSearchHookByMenu(menu);
			return hook ? hook->original : 0;
		}

		bool HasMenuSearchTileForHotkey(Menu* menu)
		{
			if (!menu)
				return false;

			Tile* root = MenuRoot(menu);
			if (!root)
				return false;
			if (GetTrackedMenuSearchTile(menu))
				return true;

			return FindTileByID(root, kStewieMenuSearch_TextTile) != nullptr;
		}

		void ScheduleMenuSearchStateSync(Menu* menu, UInt32 input, const char* source, bool originalHandled)
		{
			if (!menu || MenuID(menu) == kMenuType_StewMenu)
				return;

			StewieMenuSearchHook* hook = FindMenuSearchHookByMenu(menu);
			if (!hook)
				return;

			const UInt32 key = input | 0x20;
			if (key != 'f' && key != 'r')
				return;

			const DWORD now = GetTickCount();
			const bool wasActive = hook->keyboardActive;
			hook->targetReported = false;
			hook->stateSyncPending = true;
			hook->stateSyncAction = key == 'r'
				? kStewieMenuSearchSync_Deactivate
				: kStewieMenuSearchSync_Toggle;
			hook->stateSyncWasActive = wasActive;
			hook->stateSyncStartTick = now;
			hook->stateSyncDueTick = now + kStewieMenuSearchStateSyncDelayMs;

			if (key == 'r')
			{
				hook->keyboardActive = false;
				ClearStewieInputState();
				EndStewieTextInputSession("menusearch_ctrl_r");
			}

			DebugLog(
				"tnvse_multibyte_input_event: source=%s action=menusearch_sync_schedule menu=%u key=0x%08X handled=%u activeBefore=%u dueInMs=%u",
				source ? source : "unknown",
				MenuID(menu),
				input,
				originalHandled ? 1 : 0,
				wasActive ? 1 : 0,
				static_cast<UInt32>(kStewieMenuSearchStateSyncDelayMs));
			DebugLogMenuSearchState("sync_scheduled", *hook, menu);
		}

		Menu* GetMenuSearchHotkeyMenu()
		{
			if (!s_menuSearchHooksInstalled)
				return nullptr;

			if (InterfaceManager* manager = InterfaceManager::GetSingleton())
			{
				if (Menu* activeMenu = manager->pActiveMenu)
				{
					if (FindMenuSearchHookByMenu(activeMenu) && IsGameMenuVisible(MenuID(activeMenu)))
						return activeMenu;
				}
			}

			for (StewieMenuSearchHook& hook : s_menuSearchHooks)
			{
				if (!IsGameMenuVisible(hook.menuID))
					continue;
				if (Menu* menu = GetOpenMenu(hook.menuID))
				{
					if (HasMenuSearchTileForHotkey(menu))
						return menu;
				}
			}

			return nullptr;
		}

		bool TranslateMenuSearchHotkeyMessage(
			UINT msg,
			WPARAM wParam,
			LPARAM lParam,
			bool controlDown,
			UInt32& key,
			const char*& source)
		{
			key = 0;
			source = nullptr;

			if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
			{
				if (lParam & (1 << 30))
					return false;
				if (!controlDown || (wParam != 'F' && wParam != 'R'))
					return false;

				key = static_cast<UInt32>(wParam);
				source = msg == WM_SYSKEYDOWN ? "WndProc.WM_SYSKEYDOWN" : "WndProc.WM_KEYDOWN";
				return true;
			}

			if (msg != WM_CHAR)
				return false;
			if (wParam == 0x06)
			{
				key = 'F';
				source = "WndProc.WM_CHAR_CTRL_F";
				return true;
			}
			if (wParam == 0x12)
			{
				key = 'R';
				source = "WndProc.WM_CHAR_CTRL_R";
				return true;
			}
			if (!controlDown)
				return false;

			const UInt32 lowered = static_cast<UInt32>(wParam) | 0x20;
			if (lowered != 'f' && lowered != 'r')
				return false;

			key = lowered;
			source = "WndProc.WM_CHAR";
			return true;
		}

		bool ObserveStewieMenuSearchHotkeyMessage(
			UINT msg,
			WPARAM wParam,
			LPARAM lParam,
			bool controlDown)
		{
			UInt32 key = 0;
			const char* source = nullptr;
			if (!TranslateMenuSearchHotkeyMessage(
					msg, wParam, lParam, controlDown, key, source))
				return false;

			Menu* menu = GetMenuSearchHotkeyMenu();
			if (!menu)
			{
				DebugLog(
					"tnvse_multibyte_input_event: source=%s action=menusearch_hotkey_no_menu key=0x%08X raw=0x%08X",
					source ? source : "WndProc",
					key,
					static_cast<UInt32>(wParam));

				for (StewieMenuSearchHook& hook : s_menuSearchHooks)
				{
					if (IsPipboySearchMenu(hook.menuID))
						DebugLogMenuSearchState("hotkey_no_menu", hook, GetOpenMenu(hook.menuID));
				}
				return false;
			}

			// The chained menu handler observes the result after Stewie has actually
			// processed Ctrl+F/Ctrl+R. Avoid scheduling the same physical key again
			// from WM_KEYDOWN and WM_CHAR; keep this path only as a fallback if another
			// plugin has replaced our vtable entry.
			if (StewieMenuSearchHook* hook = FindMenuSearchHookByMenu(menu))
			{
				if (hook->installed
					&& hook->entry
					&& *reinterpret_cast<SIZE_T*>(hook->entry) == hook->hook)
				{
					return false;
				}
			}

			ScheduleMenuSearchStateSync(menu, key, source, false);
			return true;
		}

		void ProcessStewieMenuSearchPendingStateSync()
		{
			// The three Pip-Boy searches use Stewie's InputField. _IsActive is a
			// presentation trait written by InputField::SetActive(), not the
			// IsSearchMode flag that the menu keyboard handler actually tests.
			// Keep it only as a recovery hint when our explicit Ctrl+F state was
			// unavailable (for example, after a tab becomes visible again).
			for (StewieMenuSearchHook& hook : s_menuSearchHooks)
			{
				if (!hook.installed
					|| hook.stateSyncPending
					|| hook.keyboardActive
					|| !MenuSearchUsesInputField(hook.menuID)
					|| !IsGameMenuVisible(hook.menuID))
				{
					continue;
				}

				Menu* menu = GetOpenMenu(hook.menuID);
				Tile* searchTile = menu ? GetTrackedMenuSearchTile(menu) : nullptr;
				if (!searchTile && menu)
					searchTile = FindTileByID(MenuRoot(menu), kStewieMenuSearch_TextTile);
				if (!MenuSearchOwnerReportsActive(hook, menu, searchTile))
					continue;

				hook.keyboardActive = true;
				hook.targetReported = false;
				ClearStewieInputState();
				RefreshTextInputSessionForActiveTarget(
					"menusearch_input_field_reactivate");
				DebugLog(
					"tnvse_multibyte_input_event: source=MainLoop action=menusearch_input_field_reactivate menu=%u tile=0x%08X",
					hook.menuID,
					reinterpret_cast<UInt32>(searchTile));
			}

			const bool needsMaintenance = std::any_of(std::begin(s_menuSearchHooks),
				std::end(s_menuSearchHooks), [](const StewieMenuSearchHook& hook)
				{
					return hook.stateSyncPending || hook.keyboardActive;
				});
			if (!needsMaintenance)
				return;
			const DWORD now = GetTickCount();
			for (StewieMenuSearchHook& hook : s_menuSearchHooks)
			{
				if (!hook.stateSyncPending
					|| static_cast<SInt32>(now - hook.stateSyncDueTick) < 0)
				{
					continue;
				}

				Menu* menu = GetOpenMenu(hook.menuID);
				DebugLogMenuSearchState("sync_due", hook, menu);
				if (!menu)
				{
					hook.keyboardActive = false;
					ResetMenuSearchStateSync(hook);
					ClearStewieInputState();
					EndStewieTextInputSession("menusearch_sync_no_menu");
					DebugLog(
						"tnvse_multibyte_input_event: source=MainLoop action=menusearch_sync_cancel_no_menu menu=%u",
						hook.menuID);
					continue;
				}

				if (hook.stateSyncAction == kStewieMenuSearchSync_Deactivate)
				{
					DebugLogMenuSearchState("sync_deactivate", hook, menu);
					hook.keyboardActive = false;
					ResetMenuSearchStateSync(hook);
					ClearStewieInputState();
					EndStewieTextInputSession("menusearch_sync_deactivate");
					DebugLog(
						"tnvse_multibyte_input_event: source=MainLoop action=menusearch_sync_deactivate menu=%u",
						hook.menuID);
					continue;
				}

				if (hook.stateSyncAction != kStewieMenuSearchSync_Toggle)
				{
					ResetMenuSearchStateSync(hook);
					continue;
				}

				Tile* searchTile = GetTrackedMenuSearchTile(menu);
				if (!searchTile)
					searchTile = FindTileByID(MenuRoot(menu), kStewieMenuSearch_TextTile);
				const bool hasSearchTile = searchTile != nullptr;
				const bool ownerReportsActive =
					MenuSearchOwnerReportsActive(hook, menu, searchTile);
				if (!hasSearchTile
					&& static_cast<SInt32>(now - hook.stateSyncStartTick)
						< static_cast<SInt32>(kStewieMenuSearchStateSyncTimeoutMs))
				{
					hook.stateSyncDueTick = now + kStewieMenuSearchStateSyncRetryMs;
					DebugLog(
						"tnvse_multibyte_input_event: source=MainLoop action=menusearch_sync_retry menu=%u hasTile=%u ownerActive=%u retryInMs=%u",
						hook.menuID,
						hasSearchTile ? 1 : 0,
						ownerReportsActive ? 1 : 0,
						static_cast<UInt32>(kStewieMenuSearchStateSyncRetryMs));
					continue;
				}

				hook.keyboardActive = !hook.stateSyncWasActive
					&& hasSearchTile;
				DebugLog(
					"tnvse_multibyte_input_event: source=MainLoop action=menusearch_sync_apply menu=%u hasTile=%u ownerActive=%u activeAfter=%u",
					hook.menuID,
					hasSearchTile ? 1 : 0,
					ownerReportsActive ? 1 : 0,
					hook.keyboardActive ? 1 : 0);
				DebugLogMenuSearchState("sync_applied", hook, menu);
				ResetMenuSearchStateSync(hook);
				ClearStewieInputState();
				if (!hook.keyboardActive)
					EndStewieTextInputSession("menusearch_sync_closed");
			}

			for (StewieMenuSearchHook& hook : s_menuSearchHooks)
			{
				if (!hook.keyboardActive || hook.stateSyncPending)
					continue;

				if (!IsGameMenuVisible(hook.menuID))
				{
					DeactivateMenuSearch(hook, "menu_hidden");
					continue;
				}

				Menu* menu = GetOpenMenu(hook.menuID);
				if (!menu)
				{
					DeactivateMenuSearch(hook, "menu_missing");
					continue;
				}

				Tile* searchTile = GetTrackedMenuSearchTile(menu);
				if (!searchTile)
					searchTile = FindTileByID(MenuRoot(menu), kStewieMenuSearch_TextTile);
				if (!searchTile)
				{
					DeactivateMenuSearch(hook, "tile_missing");
					continue;
				}

				if (MenuSearchOwnerReportsActive(hook, menu, searchTile))
				{
					continue;
				}

				if (MenuSearchUsesInputField(hook.menuID))
				{
					// The chained Stewie handler and the matching Ctrl+F/Ctrl+R
					// transition are authoritative. The _IsActive Tile mirror can
					// read false while InputField::isActive and IsSearchMode still
					// accept input (the caret remains visible in that state).
					// Closing the session here detaches HIMC, so the next printable
					// key is consumed by Stewie before the IME can see it. Menu
					// visibility, tile lifetime and explicit hotkeys still end
					// ownership.
					continue;
				}

				if (hook.menuID == Pause)
				{
					// StartMenu remains visible after leaving Save/Load. The
					// saves-list _enabled trait is the same source Stewie checks
					// before accepting search input, so close the IME as soon as
					// that owning subpage is no longer active.
					DeactivateMenuSearch(
						hook, "startmenu_save_list_inactive", searchTile);
					continue;
				}
			}
		}

		bool HandleMenuSearchControlInput(Menu* menu, UInt32 input, bool& handled)
		{
			handled = false;
			if (!menu || !IsCtrlKeyDown())
				return false;

			const UInt32 key = input | 0x20;
			if (key != 'f' && key != 'r')
				return false;

			StewieMenuSearchHook* hook = FindMenuSearchHookByMenu(menu);
			const bool wasActive = hook && hook->keyboardActive;
			handled = CallStewieOriginalInput(menu, input);
			ClearStewieInputState();

			if (!handled)
			{
				if (hook)
					ResetMenuSearchStateSync(*hook);
				return true;
			}

			if (!hook)
				return true;

			const bool active = key == 'f' ? !wasActive : false;
			hook->keyboardActive = active;
			hook->targetReported = false;
			// We just called the exact chained Stewie handler and know that it
			// accepted this control key. Do not let the delayed _IsActive probe
			// overwrite that result; the trait is not Stewie's IsSearchMode.
			ResetMenuSearchStateSync(*hook);
			if (active)
			{
				RefreshTextInputSessionForActiveTarget("menusearch_ctrl_f_open");
				DebugLog(
					"tnvse_multibyte_input_event: source=StewieTweaksInputTarget action=menusearch_activate_immediate menu=%u",
					MenuID(menu));
			}
			else
			{
				EndStewieTextInputSession("menusearch_ctrl_f_close");
				DebugLog(
					"tnvse_multibyte_input_event: source=StewieTweaksInputTarget action=menusearch_deactivate_immediate menu=%u",
					MenuID(menu));
			}
			return true;
		}

		bool HandleMenuSearchInput(Menu* menu, UInt32 input)
		{
			if (MenuID(menu) == Pause && HandleDialogueHistoryMenuInput(menu, input))
				return true;

			// MCM Extender registers its own JIP key handlers on StartMenu.  When
			// tNVSE owns that search field, consume the matching menu-keyboard
			// event before Stewie's StartMenu search adapter or the vanilla menu
			// can interpret the same physical key a second time.
			if (MenuID(menu) == Pause && HandleMcmExtenderMenuInput(menu, input))
				return true;
			if (!IsStewieTweaksAvailable())
				return CallStewieOriginalInput(menu, input);

			bool controlHandled = false;
			if (HandleMenuSearchControlInput(menu, input, controlHandled))
				return controlHandled;

			const StewieInputTarget target = FindStewieMenuSearchTarget(menu);
			if (target.valid
				&& FilterGameInput(
						input,
						ImeCommitInputChannel::Stewie,
						GameInputFilterClass::None)
					== GameInputFilterResult::SuppressImeCommit)
			{
				DebugLog(
					"tnvse_multibyte_input_event: source=MenuSearch action=suppress_ime_commit_key menu=%u input=0x%08X",
					MenuID(menu),
					input);
				return true;
			}
			if (input == kInputCode_Enter
				&& target.valid
				&& HandleStewieImeEnter(target))
			{
				return true;
			}
			if (input == kInputCode_Enter
				&& target.valid
				&& target.inputField
				&& MenuID(menu) != PipboyData)
			{
				// Inventory and Stats close Stewie's InputField on Enter while the
				// visible search tile remains open. Consume empty Enter for ordinary
				// English layouts as well, keeping keyboardActive and Ctrl+F in sync.
				DebugLog(
					"tnvse_multibyte_input_event: source=MenuSearch.Enter action=suppress_empty_inputfield_enter menu=%u",
					MenuID(menu));
				return true;
			}

			const bool handled = HandleStewieInput(menu, input);

			return handled;
		}

		bool InstallMenuSearchHook(StewieMenuSearchHook& hook)
		{
			if (hook.installed)
				return true;

			const SIZE_T current = *reinterpret_cast<SIZE_T*>(hook.entry);
			if (current == hook.hook)
			{
				hook.installed = true;
				return true;
			}

			hook.original = current;
			SafeWrite32(hook.entry, hook.hook);
			hook.installed = true;
			hook.observedHandler = hook.hook;
			DebugLog(
				"tnvse_multibyte_input: chained Stewie %s handler=0x%08X",
				hook.name,
				static_cast<UInt32>(current));
			return true;
		}

		bool AreMenuSearchHandlersStable()
		{
			const DWORD now = GetTickCount();
			DWORD latestSearchTileSeenTick = 0;
			for (const StewieMenuSearchHook& hook : s_menuSearchHooks)
			{
				if (hook.seenTick
					&& static_cast<SInt32>(hook.seenTick - latestSearchTileSeenTick) > 0)
				{
					latestSearchTileSeenTick = hook.seenTick;
				}
			}

			// A discovered MenuSearch tile proves that Stewie's deferred menu
			// initialization has started. Do not publish the keyboard slots until
			// their handlers have then remained stable for the full delay.
			if (!latestSearchTileSeenTick)
				return false;

			bool changed = false;
			for (StewieMenuSearchHook& hook : s_menuSearchHooks)
			{
				const SIZE_T current = *reinterpret_cast<SIZE_T*>(hook.entry);
				if (hook.observedHandler == current)
					continue;

				DebugLog(
					"tnvse_multibyte_input: observed Stewie %s handler change old=0x%08X new=0x%08X",
					hook.name,
					static_cast<UInt32>(hook.observedHandler),
					static_cast<UInt32>(current));
				hook.observedHandler = current;
				changed = true;
			}

			if (changed
				|| !s_menuHandlersStableSince
				|| static_cast<SInt32>(latestSearchTileSeenTick - s_menuHandlersStableSince) > 0)
			{
				s_menuHandlersStableSince = now;
				return false;
			}

			return now - s_menuHandlersStableSince >= kStewieMenuHandlerStableDelayMs;
		}

		void TryInstallStewieMenuSearchHooks()
		{
			DiscoverMenuSearchTiles();
			if (s_menuSearchHooksInstalled || !AreMenuSearchHandlersStable())
				return;

			for (StewieMenuSearchHook& hook : s_menuSearchHooks)
				InstallMenuSearchHook(hook);

			s_menuSearchHooksInstalled = true;
			gLog.FormattedMessage(
				"tnvse_multibyte_input: Stewie Tweaks menu search input adapter installed after %u ms handler stability",
				static_cast<UInt32>(kStewieMenuHandlerStableDelayMs));

			for (StewieMenuSearchHook& hook : s_menuSearchHooks)
				DebugLogMenuSearchState("adapter_installed", hook, GetOpenMenu(hook.menuID));
		}

		void ResetStewieMenuSearchState()
		{
			for (StewieMenuSearchHook& hook : s_menuSearchHooks)
			{
				hook.root = nullptr;
				hook.tile = nullptr;
				hook.seenTick = 0;
				hook.keyboardActive = false;
				hook.targetReported = false;
				ResetMenuSearchStateSync(hook);
			}

			if (!s_menuSearchHooksInstalled)
				s_menuHandlersStableSince = 0;
			s_lastMenuSearchDiscoveryTick = 0;
		}

		bool __fastcall StewieMenuSearchInputTargetEx::InventoryMenuKeyboardInput(Menu* menu, void*, UInt32 input)
		{
			return HandleMenuSearchInput(menu, input);
		}

		bool __fastcall StewieMenuSearchInputTargetEx::StatsMenuKeyboardInput(Menu* menu, void*, UInt32 input)
		{
			return HandleMenuSearchInput(menu, input);
		}

		bool __fastcall StewieMenuSearchInputTargetEx::MapMenuKeyboardInput(Menu* menu, void*, UInt32 input)
		{
			return HandleMenuSearchInput(menu, input);
		}

		bool __fastcall StewieMenuSearchInputTargetEx::ContainerMenuKeyboardInput(Menu* menu, void*, UInt32 input)
		{
			return HandleMenuSearchInput(menu, input);
		}

		bool __fastcall StewieMenuSearchInputTargetEx::BarterMenuKeyboardInput(Menu* menu, void*, UInt32 input)
		{
			return HandleMenuSearchInput(menu, input);
		}

		bool __fastcall StewieMenuSearchInputTargetEx::LevelUpMenuKeyboardInput(Menu* menu, void*, UInt32 input)
		{
			return HandleMenuSearchInput(menu, input);
		}

		bool __fastcall StewieMenuSearchInputTargetEx::RecipeMenuKeyboardInput(Menu* menu, void*, UInt32 input)
		{
			return HandleMenuSearchInput(menu, input);
		}

		bool __fastcall StewieMenuSearchInputTargetEx::StartMenuKeyboardInput(Menu* menu, void*, UInt32 input)
		{
			return HandleMenuSearchInput(menu, input);
		}
	}
}

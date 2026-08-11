#include "multibyte_input_internal.h"
#include "hook_identity.h"
#include "Utils/SafeWrite.h"

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

		struct StewieMenuSearchAdapterSite
		{
			const char* menuName = "";
			UInt32 menuID = 0;
			SIZE_T vtableEntry = 0;
			SIZE_T predecessorHandler = 0;
			SIZE_T adapterHandler = 0;
			bool adapterInstalled = false;
			bool adapterPublicationUncertain = false;
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

		class StewieMenuSearchInputAdapter
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

		StewieMenuSearchAdapterSite s_menuSearchSites[] =
		{
			{ "InventoryMenu", Inventory, kInventoryMenuHandleKeyboardInputVTableEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputAdapter::InventoryMenuKeyboardInput) },
			{ "StatsMenu", Stats, kStatsMenuHandleKeyboardInputVTableEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputAdapter::StatsMenuKeyboardInput) },
			{ "MapMenu", PipboyData, kMapMenuHandleKeyboardInputVTableEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputAdapter::MapMenuKeyboardInput) },
			{ "ContainerMenu", Container, kContainerMenuHandleKeyboardInputVTableEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputAdapter::ContainerMenuKeyboardInput) },
			{ "BarterMenu", Barter, kBarterMenuHandleKeyboardInputVTableEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputAdapter::BarterMenuKeyboardInput) },
			{ "LevelUpMenu", LevelUp, kLevelUpMenuHandleKeyboardInputVTableEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputAdapter::LevelUpMenuKeyboardInput) },
			{ "RecipeMenu", Recipe, kRecipeMenuHandleKeyboardInputVTableEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputAdapter::RecipeMenuKeyboardInput) },
			{ "StartMenu", Pause, kStartMenuHandleKeyboardInputVTableEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputAdapter::StartMenuKeyboardInput) },
		};

		bool s_menuSearchAdaptersInstalled = false;
		DWORD s_menuHandlersStableSince = 0;
		DWORD s_lastMenuSearchDiscoveryTick = 0;

		bool ReadMenuSearchHandler(
			const StewieMenuSearchAdapterSite& site, SIZE_T& handler)
		{
			handler = 0;
			if (!hook_identity::IsAccessibleRegion(
					site.vtableEntry, sizeof(SIZE_T), false))
			{
				return false;
			}
			handler = *reinterpret_cast<const SIZE_T*>(site.vtableEntry);
			return true;
		}

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
			const StewieMenuSearchAdapterSite& site,
			Menu* menu,
			Tile* tile)
		{
			if (!tile)
				return false;

			if (MenuSearchUsesInputField(site.menuID))
			{
				const UInt32 activeTrait = MenuSearchInputActiveTrait();
				return activeTrait
					&& tile->GetValueFloat(activeTrait) > 0.5f;
			}

			if (site.menuID == Pause)
				return IsStartMenuSaveListEnabled(menu);

			// Plain SearchBar has no stable active trait. Its Ctrl+F/Ctrl+R
			// handler plus the exact owning-menu lifetime remain authoritative.
			return true;
		}

		StewieMenuSearchAdapterSite* FindMenuSearchSiteByMenuID(UInt32 menuID)
		{
			for (StewieMenuSearchAdapterSite& site : s_menuSearchSites)
			{
				if (site.menuID == menuID)
					return &site;
			}

			return nullptr;
		}

		StewieMenuSearchAdapterSite* FindMenuSearchSiteByMenu(Menu* menu)
		{
			return menu ? FindMenuSearchSiteByMenuID(MenuID(menu)) : nullptr;
		}

		void ResetMenuSearchStateSync(StewieMenuSearchAdapterSite& site)
		{
			site.stateSyncPending = false;
			site.stateSyncAction = kStewieMenuSearchSync_None;
			site.stateSyncWasActive = false;
			site.stateSyncStartTick = 0;
			site.stateSyncDueTick = 0;
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
			StewieMenuSearchAdapterSite* site = FindMenuSearchSiteByMenu(menu);
			if (!site || !site->tile)
				return nullptr;

			Tile* root = MenuRoot(menu);
			if (!root)
				return nullptr;
			if (TileTreeContains(root, site->tile))
				return site->tile;

			DebugLog(
				"tnvse_multibyte_input_debug: menusearch_track_stale name=%s menu=%u oldRoot=0x%08X newRoot=0x%08X oldTile=0x%08X",
				site->menuName,
				site->menuID,
				reinterpret_cast<UInt32>(site->root),
				reinterpret_cast<UInt32>(root),
				reinterpret_cast<UInt32>(site->tile));

			site->root = nullptr;
			site->tile = nullptr;
			site->seenTick = 0;
			site->targetReported = false;
			return nullptr;
		}

		void DebugLogMenuSearchState(const char* stage, StewieMenuSearchAdapterSite& site, Menu* menu)
		{
			if (!g_bMultibyteInputLog)
				return;
			if (!menu)
				menu = GetOpenMenu(site.menuID);

			Tile* root = MenuRoot(menu);
			Tile* trackedTile = nullptr;
			if (root && site.tile && TileTreeContains(root, site.tile))
				trackedTile = site.tile;

			Tile* fallbackTile = root ? FindTileByID(root, kStewieMenuSearch_TextTile) : nullptr;
			Tile* resolvedTile = trackedTile ? trackedTile : fallbackTile;
			InterfaceManager* manager = InterfaceManager::GetSingleton();
			Menu* activeMenu = manager ? manager->pActiveMenu : nullptr;
			const SInt32 dueInMs = site.stateSyncPending
				? static_cast<SInt32>(site.stateSyncDueTick - GetTickCount())
				: 0;
			const bool ownerActive = MenuSearchOwnerReportsActive(
				site, menu, resolvedTile);
			SIZE_T currentHandler = 0;
			ReadMenuSearchHandler(site, currentHandler);

			gLog.FormattedMessage(
				"tnvse_multibyte_input_menusearch: stage=%s name=%s menuID=%u inputField=%u "
				"gameVisible=%u pipVisible=%u/%u/%u menu=0x%08X activeMenu=0x%08X activeMenuID=%u "
				"root=0x%08X trackedRaw=0x%08X tracked=0x%08X fallback=0x%08X resolved=0x%08X "
				"tileID=%u tileVisible=%.1f tileAlpha=%.1f keyboardActive=%u pending=%u action=%u "
				"wasActive=%u dueInMs=%d ownerActive=%u installed=%u currentHandler=0x%08X expectedHandler=0x%08X "
				"predecessorHandler=0x%08X string=\"%s\"",
				stage ? stage : "unknown",
				site.menuName,
				site.menuID,
				MenuSearchUsesInputField(site.menuID) ? 1 : 0,
				IsGameMenuVisible(site.menuID) ? 1 : 0,
				IsGameMenuVisible(Inventory) ? 1 : 0,
				IsGameMenuVisible(Stats) ? 1 : 0,
				IsGameMenuVisible(PipboyData) ? 1 : 0,
				reinterpret_cast<UInt32>(menu),
				reinterpret_cast<UInt32>(activeMenu),
				MenuID(activeMenu),
				reinterpret_cast<UInt32>(root),
				reinterpret_cast<UInt32>(site.tile),
				reinterpret_cast<UInt32>(trackedTile),
				reinterpret_cast<UInt32>(fallbackTile),
				reinterpret_cast<UInt32>(resolvedTile),
				TileID(resolvedTile),
				resolvedTile ? resolvedTile->GetValueFloat(Tile::kTileValue_visible) : 0.0f,
				resolvedTile ? resolvedTile->GetValueFloat(Tile::kTileValue_alpha) : 0.0f,
				site.keyboardActive ? 1 : 0,
				site.stateSyncPending ? 1 : 0,
				static_cast<UInt32>(site.stateSyncAction),
				site.stateSyncWasActive ? 1 : 0,
				dueInMs,
				ownerActive ? 1 : 0,
				site.adapterInstalled ? 1 : 0,
				static_cast<UInt32>(currentHandler),
				static_cast<UInt32>(site.adapterHandler),
				static_cast<UInt32>(site.predecessorHandler),
				resolvedTile ? resolvedTile->GetValueString(Tile::kTileValue_string) : "");
		}

		void DeactivateMenuSearch(StewieMenuSearchAdapterSite& site, const char* reason, Tile* tile = nullptr)
		{
			if (!site.keyboardActive && !site.stateSyncPending)
				return;

			DebugLogMenuSearchState(reason, site, GetOpenMenu(site.menuID));
			site.keyboardActive = false;
			site.targetReported = false;
			ResetMenuSearchStateSync(site);
			ClearStewieInputState();
			EndStewieTextInputSession(reason);

			DebugLog(
				"tnvse_multibyte_input_event: source=StatePoll action=menusearch_deactivate reason=%s menu=%u tile=0x%08X visible=%.1f alpha=%.1f",
				reason ? reason : "unknown",
				site.menuID,
				reinterpret_cast<UInt32>(tile),
				tile ? tile->GetValueFloat(Tile::kTileValue_visible) : 0.0f,
				tile ? tile->GetValueFloat(Tile::kTileValue_alpha) : 0.0f);
		}

		void TrackMenuSearchTile(StewieMenuSearchAdapterSite& site, Tile* root, Tile* tile)
		{
			if (!tile)
				return;

			if ((site.keyboardActive || site.stateSyncPending)
				&& (site.root != root || site.tile != tile))
			{
				DeactivateMenuSearch(site, "tile_replaced", site.tile);
			}

			site.root = root;
			site.tile = tile;
			site.seenTick = GetTickCount();
			site.keyboardActive = false;
			site.targetReported = false;
			ResetMenuSearchStateSync(site);

			DebugLog(
				"tnvse_multibyte_input_debug: menusearch_track name=%s menu=%u root=0x%08X tile=0x%08X id=%u source=main_loop string='%s'",
				site.menuName,
				site.menuID,
				reinterpret_cast<UInt32>(root),
				reinterpret_cast<UInt32>(tile),
				TileID(tile),
				tile->GetValueString(Tile::kTileValue_string));
			DebugLogMenuSearchState("tile_tracked", site, GetOpenMenu(site.menuID));
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

			for (StewieMenuSearchAdapterSite& site : s_menuSearchSites)
			{
				if (!IsGameMenuVisible(site.menuID))
					continue;

				Menu* menu = GetOpenMenu(site.menuID);
				Tile* root = MenuRoot(menu);
				if (root && site.root == root && site.tile)
					continue;

				Tile* tile = root ? FindTileByID(root, kStewieMenuSearch_TextTile) : nullptr;
				if (!tile || (site.root == root && site.tile == tile))
					continue;

				TrackMenuSearchTile(site, root, tile);
			}
		}

		StewieInputTarget FindStewieMenuSearchTarget(Menu* menu)
		{
			if (!menu)
				return {};

			StewieMenuSearchAdapterSite* site = FindMenuSearchSiteByMenu(menu);
			if (!site || !site->adapterInstalled || !site->keyboardActive)
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
				DeactivateMenuSearch(*site, "no_tracked_searchbar");
				DebugLog(
					"tnvse_multibyte_input_debug: menusearch_target_miss reason=no_tracked_searchbar menu=%u root=0x%08X legacyID=%u",
					MenuID(menu),
					reinterpret_cast<UInt32>(root),
					kStewieMenuSearch_TextTile);
				return {};
			}

			if (!site->targetReported)
			{
				DebugLogMenuSearchState("target_found", *site, menu);
				site->targetReported = true;
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

			for (const StewieMenuSearchAdapterSite& site : s_menuSearchSites)
			{
				if (!IsGameMenuVisible(site.menuID))
					continue;

				if (Menu* menu = GetOpenMenu(site.menuID))
				{
					if (StewieInputTarget target = FindStewieMenuSearchTarget(menu); target.valid)
						return target;
				}
			}

			return {};
		}

		SIZE_T GetStewieMenuSearchPredecessorInputHandler(Menu* menu)
		{
			StewieMenuSearchAdapterSite* site = FindMenuSearchSiteByMenu(menu);
			return site && site->predecessorHandler != site->adapterHandler
				&& hook_identity::IsExecutableTarget(site->predecessorHandler)
				? site->predecessorHandler : 0;
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

		void ScheduleMenuSearchStateSync(Menu* menu, UInt32 input, const char* source, bool predecessorHandled)
		{
			if (!menu || MenuID(menu) == kMenuType_StewMenu)
				return;

			StewieMenuSearchAdapterSite* site = FindMenuSearchSiteByMenu(menu);
			if (!site)
				return;

			const UInt32 key = input | 0x20;
			if (key != 'f' && key != 'r')
				return;

			const DWORD now = GetTickCount();
			const bool wasActive = site->keyboardActive;
			site->targetReported = false;
			site->stateSyncPending = true;
			site->stateSyncAction = key == 'r'
				? kStewieMenuSearchSync_Deactivate
				: kStewieMenuSearchSync_Toggle;
			site->stateSyncWasActive = wasActive;
			site->stateSyncStartTick = now;
			site->stateSyncDueTick = now + kStewieMenuSearchStateSyncDelayMs;

			if (key == 'r')
			{
				site->keyboardActive = false;
				ClearStewieInputState();
				EndStewieTextInputSession("menusearch_ctrl_r");
			}

			DebugLog(
				"tnvse_multibyte_input_event: source=%s action=menusearch_sync_schedule menu=%u key=0x%08X handled=%u activeBefore=%u dueInMs=%u",
				source ? source : "unknown",
				MenuID(menu),
				input,
				predecessorHandled ? 1 : 0,
				wasActive ? 1 : 0,
				static_cast<UInt32>(kStewieMenuSearchStateSyncDelayMs));
			DebugLogMenuSearchState("sync_scheduled", *site, menu);
		}

		Menu* GetMenuSearchHotkeyMenu()
		{
			if (!s_menuSearchAdaptersInstalled)
				return nullptr;

			if (InterfaceManager* manager = InterfaceManager::GetSingleton())
			{
				if (Menu* activeMenu = manager->pActiveMenu)
				{
					if (FindMenuSearchSiteByMenu(activeMenu) && IsGameMenuVisible(MenuID(activeMenu)))
						return activeMenu;
				}
			}

			for (StewieMenuSearchAdapterSite& site : s_menuSearchSites)
			{
				if (!IsGameMenuVisible(site.menuID))
					continue;
				if (Menu* menu = GetOpenMenu(site.menuID))
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

				for (StewieMenuSearchAdapterSite& site : s_menuSearchSites)
				{
					if (IsPipboySearchMenu(site.menuID))
						DebugLogMenuSearchState("hotkey_no_menu", site, GetOpenMenu(site.menuID));
				}
				return false;
			}

			// The chained menu handler observes the result after Stewie has actually
			// processed Ctrl+F/Ctrl+R. Avoid scheduling the same physical key again
			// from WM_KEYDOWN and WM_CHAR; keep this path only as a fallback if another
			// plugin has replaced our vtable entry.
			if (StewieMenuSearchAdapterSite* site = FindMenuSearchSiteByMenu(menu))
			{
				if (site->adapterInstalled
					&& site->vtableEntry
					&& *reinterpret_cast<SIZE_T*>(site->vtableEntry) == site->adapterHandler)
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
			for (StewieMenuSearchAdapterSite& site : s_menuSearchSites)
			{
				if (!site.adapterInstalled
					|| site.stateSyncPending
					|| site.keyboardActive
					|| !MenuSearchUsesInputField(site.menuID)
					|| !IsGameMenuVisible(site.menuID))
				{
					continue;
				}

				Menu* menu = GetOpenMenu(site.menuID);
				Tile* searchTile = menu ? GetTrackedMenuSearchTile(menu) : nullptr;
				if (!searchTile && menu)
					searchTile = FindTileByID(MenuRoot(menu), kStewieMenuSearch_TextTile);
				if (!MenuSearchOwnerReportsActive(site, menu, searchTile))
					continue;

				site.keyboardActive = true;
				site.targetReported = false;
				ClearStewieInputState();
				RefreshTextInputSessionForActiveTarget(
					"menusearch_input_field_reactivate");
				DebugLog(
					"tnvse_multibyte_input_event: source=MainLoop action=menusearch_input_field_reactivate menu=%u tile=0x%08X",
					site.menuID,
					reinterpret_cast<UInt32>(searchTile));
			}

			const bool needsMaintenance = std::any_of(std::begin(s_menuSearchSites),
				std::end(s_menuSearchSites), [](const StewieMenuSearchAdapterSite& site)
				{
					return site.stateSyncPending || site.keyboardActive;
				});
			if (!needsMaintenance)
				return;
			const DWORD now = GetTickCount();
			for (StewieMenuSearchAdapterSite& site : s_menuSearchSites)
			{
				if (!site.stateSyncPending
					|| static_cast<SInt32>(now - site.stateSyncDueTick) < 0)
				{
					continue;
				}

				Menu* menu = GetOpenMenu(site.menuID);
				DebugLogMenuSearchState("sync_due", site, menu);
				if (!menu)
				{
					site.keyboardActive = false;
					ResetMenuSearchStateSync(site);
					ClearStewieInputState();
					EndStewieTextInputSession("menusearch_sync_no_menu");
					DebugLog(
						"tnvse_multibyte_input_event: source=MainLoop action=menusearch_sync_cancel_no_menu menu=%u",
						site.menuID);
					continue;
				}

				if (site.stateSyncAction == kStewieMenuSearchSync_Deactivate)
				{
					DebugLogMenuSearchState("sync_deactivate", site, menu);
					site.keyboardActive = false;
					ResetMenuSearchStateSync(site);
					ClearStewieInputState();
					EndStewieTextInputSession("menusearch_sync_deactivate");
					DebugLog(
						"tnvse_multibyte_input_event: source=MainLoop action=menusearch_sync_deactivate menu=%u",
						site.menuID);
					continue;
				}

				if (site.stateSyncAction != kStewieMenuSearchSync_Toggle)
				{
					ResetMenuSearchStateSync(site);
					continue;
				}

				Tile* searchTile = GetTrackedMenuSearchTile(menu);
				if (!searchTile)
					searchTile = FindTileByID(MenuRoot(menu), kStewieMenuSearch_TextTile);
				const bool hasSearchTile = searchTile != nullptr;
				const bool ownerReportsActive =
					MenuSearchOwnerReportsActive(site, menu, searchTile);
				if (!hasSearchTile
					&& static_cast<SInt32>(now - site.stateSyncStartTick)
						< static_cast<SInt32>(kStewieMenuSearchStateSyncTimeoutMs))
				{
					site.stateSyncDueTick = now + kStewieMenuSearchStateSyncRetryMs;
					DebugLog(
						"tnvse_multibyte_input_event: source=MainLoop action=menusearch_sync_retry menu=%u hasTile=%u ownerActive=%u retryInMs=%u",
						site.menuID,
						hasSearchTile ? 1 : 0,
						ownerReportsActive ? 1 : 0,
						static_cast<UInt32>(kStewieMenuSearchStateSyncRetryMs));
					continue;
				}

				site.keyboardActive = !site.stateSyncWasActive
					&& hasSearchTile;
				DebugLog(
					"tnvse_multibyte_input_event: source=MainLoop action=menusearch_sync_apply menu=%u hasTile=%u ownerActive=%u activeAfter=%u",
					site.menuID,
					hasSearchTile ? 1 : 0,
					ownerReportsActive ? 1 : 0,
					site.keyboardActive ? 1 : 0);
				DebugLogMenuSearchState("sync_applied", site, menu);
				ResetMenuSearchStateSync(site);
				ClearStewieInputState();
				if (!site.keyboardActive)
					EndStewieTextInputSession("menusearch_sync_closed");
			}

			for (StewieMenuSearchAdapterSite& site : s_menuSearchSites)
			{
				if (!site.keyboardActive || site.stateSyncPending)
					continue;

				if (!IsGameMenuVisible(site.menuID))
				{
					DeactivateMenuSearch(site, "menu_hidden");
					continue;
				}

				Menu* menu = GetOpenMenu(site.menuID);
				if (!menu)
				{
					DeactivateMenuSearch(site, "menu_missing");
					continue;
				}

				Tile* searchTile = GetTrackedMenuSearchTile(menu);
				if (!searchTile)
					searchTile = FindTileByID(MenuRoot(menu), kStewieMenuSearch_TextTile);
				if (!searchTile)
				{
					DeactivateMenuSearch(site, "tile_missing");
					continue;
				}

				if (MenuSearchOwnerReportsActive(site, menu, searchTile))
				{
					continue;
				}

				if (MenuSearchUsesInputField(site.menuID))
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

				if (site.menuID == Pause)
				{
					// StartMenu remains visible after leaving Save/Load. The
					// saves-list _enabled trait is the same source Stewie checks
					// before accepting search input, so close the IME as soon as
					// that owning subpage is no longer active.
					DeactivateMenuSearch(
						site, "startmenu_save_list_inactive", searchTile);
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

			StewieMenuSearchAdapterSite* site = FindMenuSearchSiteByMenu(menu);
			const bool wasActive = site && site->keyboardActive;
			handled = CallStewiePredecessorInput(menu, input);
			ClearStewieInputState();

			if (!handled)
			{
				if (site)
					ResetMenuSearchStateSync(*site);
				return true;
			}

			if (!site)
				return true;

			const bool active = key == 'f' ? !wasActive : false;
			site->keyboardActive = active;
			site->targetReported = false;
			// We just called the exact chained Stewie handler and know that it
			// accepted this control key. Do not let the delayed _IsActive probe
			// overwrite that result; the trait is not Stewie's IsSearchMode.
			ResetMenuSearchStateSync(*site);
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
			if (MenuID(menu) == Pause
				&& HandleModernHelpMenuMenuInput(menu, input))
			{
				return true;
			}

			if (MenuID(menu) == Pause && HandleDialogueHistoryMenuInput(menu, input))
				return true;

			// MCM Extender registers its own JIP key handlers on StartMenu.  When
			// tNVSE owns that search field, consume the matching menu-keyboard
			// event before Stewie's StartMenu search adapter or the vanilla menu
			// can interpret the same physical key a second time.
			if (MenuID(menu) == Pause && HandleMcmExtenderMenuInput(menu, input))
				return true;
			if (!IsStewieTweaksAvailable())
				return CallStewiePredecessorInput(menu, input);

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

		bool InstallMenuSearchAdapter(StewieMenuSearchAdapterSite& site)
		{
			SIZE_T currentHandler = 0;
			if (!ReadMenuSearchHandler(site, currentHandler)
				|| !hook_identity::IsExecutableTarget(site.adapterHandler))
			{
				gLog.FormattedMessage(
					"tnvse_multibyte_input: cannot install Stewie %s adapter; unreadable entry=0x%08X or non-executable adapter=0x%08X",
					site.menuName,
					static_cast<UInt32>(site.vtableEntry),
					static_cast<UInt32>(site.adapterHandler));
				return false;
			}
			if (site.adapterPublicationUncertain)
			{
				if (currentHandler == site.adapterHandler
					&& site.predecessorHandler != site.adapterHandler
					&& hook_identity::IsExecutableTarget(site.predecessorHandler))
				{
					site.adapterPublicationUncertain = false;
					site.adapterInstalled = true;
					site.observedHandler = currentHandler;
					return true;
				}
				if (currentHandler != site.predecessorHandler)
				{
					// The saved predecessor may still be reachable below this owner.
					// Do not republish above it and risk H->S->H recursion.
					site.observedHandler = currentHandler;
					return false;
				}

				// The slot has returned to the predecessor, proving that the
				// uncertain chain is gone. A normal installation may be retried.
				site.adapterPublicationUncertain = false;
				site.predecessorHandler = 0;
			}
			if (currentHandler == site.adapterHandler)
			{
				site.adapterInstalled = site.predecessorHandler != site.adapterHandler
					&& hook_identity::IsExecutableTarget(site.predecessorHandler);
				if (!site.adapterInstalled)
				{
					gLog.FormattedMessage(
						"tnvse_multibyte_input: Stewie %s adapter is present but its predecessor is unavailable predecessor=0x%08X",
						site.menuName,
						static_cast<UInt32>(site.predecessorHandler));
				}
				return site.adapterInstalled;
			}
			if (!hook_identity::IsExecutableTarget(currentHandler))
			{
				gLog.FormattedMessage(
					"tnvse_multibyte_input: cannot chain Stewie %s adapter; non-executable predecessor=0x%08X entry=0x%08X",
					site.menuName,
					static_cast<UInt32>(currentHandler),
					static_cast<UInt32>(site.vtableEntry));
				return false;
			}

			const SIZE_T previousPredecessor = site.predecessorHandler;
			site.predecessorHandler = currentHandler;
			// Stewie menu handler vtable slot
			// (__thiscall target via __fastcall adapter).
			const SafeWrite32IfEqualResult publication =
				SafeWrite32IfEqualDetailed(site.vtableEntry,
					site.adapterHandler, currentHandler);
			const bool published = publication.WasPublished();
			if (!published)
			{
				SIZE_T observedTarget = publication.comparisonPerformed
					? publication.observed : 0;
				if (!publication.comparisonPerformed)
					ReadMenuSearchHandler(site, observedTarget);
				site.predecessorHandler = previousPredecessor;
				site.adapterInstalled = false;
				site.adapterPublicationUncertain = false;
				site.observedHandler = observedTarget;
				gLog.FormattedMessage(
					"tnvse_multibyte_input: Stewie %s adapter CAS did not publish entry=0x%08X predecessor=0x%08X observed=0x%08X compared=%u protectionError=%lu",
					site.menuName,
					static_cast<UInt32>(site.vtableEntry),
					static_cast<UInt32>(currentHandler),
					static_cast<UInt32>(observedTarget),
					publication.comparisonPerformed ? 1u : 0u,
					publication.protectionError);
				return false;
			}
			if (!publication.PostconditionsComplete())
			{
				gLog.FormattedMessage(
					"tnvse_multibyte_input: Stewie %s adapter published with incomplete write postconditions protectionRestored=%u protectionError=%lu cacheFlushed=%u cacheError=%lu",
					site.menuName,
					publication.protectionRestored ? 1u : 0u,
					publication.protectionError,
					publication.instructionCacheFlushed ? 1u : 0u,
					publication.cacheFlushError);
			}
			SIZE_T installedTarget = 0;
			const bool installedTargetReadable =
				ReadMenuSearchHandler(site, installedTarget);
			if (installedTargetReadable && installedTarget == site.adapterHandler)
			{
				site.adapterInstalled = true;
				site.observedHandler = site.adapterHandler;
				DebugLog(
					"tnvse_multibyte_input: chained Stewie %s handler=0x%08X",
					site.menuName,
					static_cast<UInt32>(currentHandler));
				return true;
			}

			if (installedTargetReadable && installedTarget == currentHandler)
			{
				site.predecessorHandler = previousPredecessor;
				site.observedHandler = currentHandler;
				gLog.FormattedMessage(
					"tnvse_multibyte_input: Stewie %s adapter was published but the slot returned to its predecessor entry=0x%08X predecessor=0x%08X",
					site.menuName,
					static_cast<UInt32>(site.vtableEntry),
					static_cast<UInt32>(currentHandler));
				return false;
			}

			if (installedTargetReadable
				&& hook_identity::IsExecutableTarget(installedTarget))
			{
				// A successor could have captured this adapter between publication
				// and verification. Preserve the predecessor and block republishing,
				// but do not report the adapter as installed: executable does not
				// prove that the successor actually chains through tNVSE.
				site.adapterInstalled = false;
				site.adapterPublicationUncertain = true;
				site.observedHandler = installedTarget;
				gLog.FormattedMessage(
					"tnvse_multibyte_input: Stewie %s adapter may be retained below successor=0x%08X predecessor=0x%08X; reachability unverified",
					site.menuName,
					static_cast<UInt32>(installedTarget),
					static_cast<UInt32>(currentHandler));
				return false;
			}

			// Do not overwrite an unrecognized value with the predecessor: a
			// later owner may already have captured this adapter. Retain the
			// predecessor so a still-reachable adapter can continue to chain.
			site.adapterInstalled = false;
			site.adapterPublicationUncertain = true;
			site.observedHandler = installedTarget;
			gLog.FormattedMessage(
				"tnvse_multibyte_input: Stewie %s adapter publication state is unreadable or invalid entry=0x%08X observed=0x%08X predecessor_retained=0x%08X",
				site.menuName,
				static_cast<UInt32>(site.vtableEntry),
				static_cast<UInt32>(installedTarget),
				static_cast<UInt32>(currentHandler));
			return false;
		}

		bool AreMenuSearchHandlersStable()
		{
			const DWORD now = GetTickCount();
			DWORD latestSearchTileSeenTick = 0;
			for (const StewieMenuSearchAdapterSite& site : s_menuSearchSites)
			{
				if (site.seenTick
					&& static_cast<SInt32>(site.seenTick - latestSearchTileSeenTick) > 0)
				{
					latestSearchTileSeenTick = site.seenTick;
				}
			}

			// A discovered MenuSearch tile proves that Stewie's deferred menu
			// initialization has started. Do not publish the keyboard slots until
			// their handlers have then remained stable for the full delay.
			if (!latestSearchTileSeenTick)
				return false;

			bool changed = false;
			for (StewieMenuSearchAdapterSite& site : s_menuSearchSites)
			{
				SIZE_T currentHandler = 0;
				if (!ReadMenuSearchHandler(site, currentHandler))
				{
					s_menuHandlersStableSince = now;
					return false;
				}
				if (site.observedHandler == currentHandler)
					continue;

				DebugLog(
					"tnvse_multibyte_input: observed Stewie %s handler change old=0x%08X new=0x%08X",
					site.menuName,
					static_cast<UInt32>(site.observedHandler),
					static_cast<UInt32>(currentHandler));
				site.observedHandler = currentHandler;
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

		void TryInstallStewieMenuSearchAdapterSites()
		{
			DiscoverMenuSearchTiles();
			if (s_menuSearchAdaptersInstalled)
			{
				bool allCurrent = true;
				for (StewieMenuSearchAdapterSite& site : s_menuSearchSites)
				{
					SIZE_T currentHandler = 0;
					const bool readable = ReadMenuSearchHandler(site, currentHandler);
					const bool predecessorValid = site.predecessorHandler != site.adapterHandler
						&& hook_identity::IsExecutableTarget(site.predecessorHandler);
					if (readable && currentHandler == site.adapterHandler
						&& predecessorValid)
						continue;

					allCurrent = false;
					site.adapterInstalled = false;
					site.observedHandler = currentHandler;
					if (readable && currentHandler == site.predecessorHandler)
					{
						// The whole adapter chain was removed. Forget the stale
						// predecessor so a later stable publication can start cleanly.
						site.adapterPublicationUncertain = false;
						site.predecessorHandler = 0;
					}
					else
					{
						// An unreadable slot or a different owner may still retain this
						// adapter below it. Preserve the predecessor and never reassert.
						site.adapterPublicationUncertain = true;
					}
					gLog.FormattedMessage(
						"tnvse_multibyte_input: Stewie %s adapter lost verified top-level ownership current=0x%08X readable=%u; capability revoked",
						site.menuName,
						static_cast<UInt32>(currentHandler),
						readable ? 1u : 0u);
				}
				if (allCurrent)
					return;
				s_menuSearchAdaptersInstalled = false;
				s_menuHandlersStableSince = GetTickCount();
				return;
			}

			if (!AreMenuSearchHandlersStable())
				return;

			bool allInstalled = true;
			for (StewieMenuSearchAdapterSite& site : s_menuSearchSites)
				allInstalled = InstallMenuSearchAdapter(site) && allInstalled;

			if (!allInstalled)
			{
				// Rate-limit retries while still allowing a late Stewie handler
				// publication or a transient write failure to recover.
				s_menuHandlersStableSince = GetTickCount();
				return;
			}

			s_menuSearchAdaptersInstalled = true;
			gLog.FormattedMessage(
				"tnvse_multibyte_input: Stewie Tweaks menu search input adapter installed after %u ms handler stability",
				static_cast<UInt32>(kStewieMenuHandlerStableDelayMs));

			for (StewieMenuSearchAdapterSite& site : s_menuSearchSites)
				DebugLogMenuSearchState("adapter_installed", site, GetOpenMenu(site.menuID));
		}

		void ResetStewieMenuSearchState()
		{
			for (StewieMenuSearchAdapterSite& site : s_menuSearchSites)
			{
				site.root = nullptr;
				site.tile = nullptr;
				site.seenTick = 0;
				site.keyboardActive = false;
				site.targetReported = false;
				ResetMenuSearchStateSync(site);
			}

			if (!s_menuSearchAdaptersInstalled)
				s_menuHandlersStableSince = 0;
			s_lastMenuSearchDiscoveryTick = 0;
		}

		bool __fastcall StewieMenuSearchInputAdapter::InventoryMenuKeyboardInput(Menu* menu, void*, UInt32 input)
		{
			return HandleMenuSearchInput(menu, input);
		}

		bool __fastcall StewieMenuSearchInputAdapter::StatsMenuKeyboardInput(Menu* menu, void*, UInt32 input)
		{
			return HandleMenuSearchInput(menu, input);
		}

		bool __fastcall StewieMenuSearchInputAdapter::MapMenuKeyboardInput(Menu* menu, void*, UInt32 input)
		{
			return HandleMenuSearchInput(menu, input);
		}

		bool __fastcall StewieMenuSearchInputAdapter::ContainerMenuKeyboardInput(Menu* menu, void*, UInt32 input)
		{
			return HandleMenuSearchInput(menu, input);
		}

		bool __fastcall StewieMenuSearchInputAdapter::BarterMenuKeyboardInput(Menu* menu, void*, UInt32 input)
		{
			return HandleMenuSearchInput(menu, input);
		}

		bool __fastcall StewieMenuSearchInputAdapter::LevelUpMenuKeyboardInput(Menu* menu, void*, UInt32 input)
		{
			return HandleMenuSearchInput(menu, input);
		}

		bool __fastcall StewieMenuSearchInputAdapter::RecipeMenuKeyboardInput(Menu* menu, void*, UInt32 input)
		{
			return HandleMenuSearchInput(menu, input);
		}

		bool __fastcall StewieMenuSearchInputAdapter::StartMenuKeyboardInput(Menu* menu, void*, UInt32 input)
		{
			return HandleMenuSearchInput(menu, input);
		}
	}
}

#include "multibyte_input_internal.h"

// Stewie Tweaks MenuSearch integration for vanilla game menus.

namespace fonthook
{
	namespace multibyte_input
	{
		constexpr SIZE_T kAddr_ReadXML = 0x00A01B00;
		constexpr SIZE_T kReadXMLPatchLen = 5;
		// Unlike GetMenuByType(), the vanilla visibility table changes when a
		// persistent menu is hidden or closed.
		constexpr SIZE_T kAddr_MenuVisibility = 0x011F308F;
		constexpr UInt32 kMenuType_StewMenu = 1069;
		constexpr UInt32 kStewieMenuSearch_TextTile = 87698483;
		constexpr DWORD kStewieMenuSearchStateSyncDelayMs = 150;
		constexpr DWORD kStewieMenuSearchStateSyncRetryMs = 50;
		constexpr DWORD kStewieMenuSearchStateSyncTimeoutMs = 1000;
		constexpr DWORD kStewieMenuHandlerStableDelayMs = 1000;
		constexpr UInt8 kStewieMenuSearchSync_None = 0;
		constexpr UInt8 kStewieMenuSearchSync_Toggle = 1;
		constexpr UInt8 kStewieMenuSearchSync_Deactivate = 2;

		constexpr SIZE_T kInventoryMenuHandleKeyboardInputEntry = 0x10739E4;
		constexpr SIZE_T kStatsMenuHandleKeyboardInputEntry = 0x1070004;
		constexpr SIZE_T kMapMenuHandleKeyboardInputEntry = 0x1074D74;
		constexpr SIZE_T kContainerMenuHandleKeyboardInputEntry = 0x10721DC;
		constexpr SIZE_T kBarterMenuHandleKeyboardInputEntry = 0x107071C;
		constexpr SIZE_T kLevelUpMenuHandleKeyboardInputEntry = 0x1073D0C;
		constexpr SIZE_T kRecipeMenuHandleKeyboardInputEntry = 0x10704BC;
		constexpr SIZE_T kStartMenuHandleKeyboardInputEntry = 0x1076D4C;

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
			{ "InventoryMenu", Inventory, kInventoryMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputTargetEx::InventoryMenuKeyboardInput) },
			{ "StatsMenu", Stats, kStatsMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputTargetEx::StatsMenuKeyboardInput) },
			{ "MapMenu", PipboyData, kMapMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputTargetEx::MapMenuKeyboardInput) },
			{ "ContainerMenu", Container, kContainerMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputTargetEx::ContainerMenuKeyboardInput) },
			{ "BarterMenu", Barter, kBarterMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputTargetEx::BarterMenuKeyboardInput) },
			{ "LevelUpMenu", LevelUp, kLevelUpMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputTargetEx::LevelUpMenuKeyboardInput) },
			{ "RecipeMenu", Recipe, kRecipeMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputTargetEx::RecipeMenuKeyboardInput) },
			{ "StartMenu", Pause, kStartMenuHandleKeyboardInputEntry, 0, reinterpret_cast<SIZE_T>(&StewieMenuSearchInputTargetEx::StartMenuKeyboardInput) },
		};

		using TileReadXMLFn = Tile * (__thiscall*)(Tile*, const char*);

		TileReadXMLFn s_originalTileReadXML = nullptr;
		void* s_tileReadXMLTrampoline = nullptr;
		bool s_tileReadXMLHookInstalled = false;
		bool s_menuSearchHooksInstalled = false;
		DWORD s_menuHandlersStableSince = 0;

		bool IsGameMenuVisible(UInt32 menuID)
		{
			return menuID && reinterpret_cast<volatile UInt8*>(kAddr_MenuVisibility)[menuID] != 0;
		}

		bool IsPipboySearchMenu(UInt32 menuID)
		{
			return menuID == Inventory || menuID == Stats || menuID == PipboyData;
		}

		bool IsAnyPipboySearchMenuVisible()
		{
			return IsGameMenuVisible(Inventory)
				|| IsGameMenuVisible(Stats)
				|| IsGameMenuVisible(PipboyData);
		}

		bool MenuSearchUsesInputField(UInt32 menuID)
		{
			return IsPipboySearchMenu(menuID);
		}

		bool ContainsNoCase(const char* haystack, const char* needle)
		{
			if (!haystack || !needle || !*needle)
				return false;

			const size_t needleLen = std::strlen(needle);
			for (const char* p = haystack; *p; ++p)
			{
				size_t i = 0;
				while (i < needleLen
					&& p[i]
					&& std::tolower(static_cast<unsigned char>(p[i])) ==
					std::tolower(static_cast<unsigned char>(needle[i])))
				{
					++i;
				}

				if (i == needleLen)
					return true;
			}

			return false;
		}

		bool IsStewieMenuSearchXmlPath(const char* path)
		{
			return path
				&& ContainsNoCase(path, "lStewieAl")
				&& ContainsNoCase(path, "MenuSearch")
				&& ContainsNoCase(path, ".xml");
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

		StewieMenuSearchHook* FindMenuSearchHookByRoot(Tile* root)
		{
			if (!root)
				return nullptr;

			for (StewieMenuSearchHook& hook : s_menuSearchHooks)
			{
				if (Menu* menu = GetOpenMenu(hook.menuID))
				{
					if (MenuRoot(menu) == root)
						return &hook;
				}
			}

			return nullptr;
		}

		StewieMenuSearchHook* FindMenuSearchHookByXmlPath(const char* path)
		{
			if (!path)
				return nullptr;

			if (ContainsNoCase(path, "Inventory.xml"))
				return FindMenuSearchHookByMenuID(Inventory);
			if (ContainsNoCase(path, "Stats.xml"))
				return FindMenuSearchHookByMenuID(Stats);
			if (ContainsNoCase(path, "Map.xml"))
				return FindMenuSearchHookByMenuID(PipboyData);
			if (ContainsNoCase(path, "Container.xml"))
				return FindMenuSearchHookByMenuID(Container);
			if (ContainsNoCase(path, "Barter.xml"))
				return FindMenuSearchHookByMenuID(Barter);
			if (ContainsNoCase(path, "LevelUp.xml"))
				return FindMenuSearchHookByMenuID(LevelUp);
			if (ContainsNoCase(path, "Recipe.xml"))
				return FindMenuSearchHookByMenuID(Recipe);
			if (ContainsNoCase(path, "SaveLoad.xml"))
				return FindMenuSearchHookByMenuID(Pause);

			return nullptr;
		}

		void ResetMenuSearchStateSync(StewieMenuSearchHook& hook)
		{
			hook.stateSyncPending = false;
			hook.stateSyncAction = kStewieMenuSearchSync_None;
			hook.stateSyncWasActive = false;
			hook.stateSyncStartTick = 0;
			hook.stateSyncDueTick = 0;
		}

		bool TileTreeContains(Tile* root, Tile* target, UInt32 depth = 0)
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
			hook->keyboardActive = false;
			hook->targetReported = false;
			ResetMenuSearchStateSync(*hook);
			return nullptr;
		}

		void DebugLogMenuSearchState(const char* stage, StewieMenuSearchHook& hook, Menu* menu)
		{
			if (!g_bMultibyteInputDebug)
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
			const SIZE_T currentHandler = hook.entry
				? *reinterpret_cast<SIZE_T*>(hook.entry)
				: 0;

			gLog.FormattedMessage(
				"tnvse_multibyte_input_menusearch: stage=%s name=%s menuID=%u inputField=%u "
				"gameVisible=%u pipVisible=%u/%u/%u menu=0x%08X activeMenu=0x%08X activeMenuID=%u "
				"root=0x%08X trackedRaw=0x%08X tracked=0x%08X fallback=0x%08X resolved=0x%08X "
				"tileID=%u tileVisible=%.1f tileAlpha=%.1f keyboardActive=%u pending=%u action=%u "
				"wasActive=%u dueInMs=%d installed=%u currentHandler=0x%08X expectedHandler=0x%08X "
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
			HideCandidateOverlay();

			DebugLog(
				"tnvse_multibyte_input_event: source=StatePoll action=menusearch_deactivate reason=%s menu=%u tile=0x%08X visible=%.1f alpha=%.1f",
				reason ? reason : "unknown",
				hook.menuID,
				reinterpret_cast<UInt32>(tile),
				tile ? tile->GetValueFloat(Tile::kTileValue_visible) : 0.0f,
				tile ? tile->GetValueFloat(Tile::kTileValue_alpha) : 0.0f);
		}

		bool IsMenuSearchTileActive(const StewieMenuSearchHook& hook, Tile* tile)
		{
			if (!tile)
				return false;

			// These three XML files use a namespace-level InputField and do not
			// declare _IsActive on the tracked MenuSearch tile.
			if (MenuSearchUsesInputField(hook.menuID))
				return true;

			return tile->GetValueFloat(Tile::kTileValue_visible) > 0.5f
				&& tile->GetValueFloat(Tile::kTileValue_alpha) > 200.0f;
		}

		void TrackMenuSearchTile(StewieMenuSearchHook& hook, Tile* root, Tile* tile, const char* path)
		{
			if (!tile)
				return;

			hook.root = root;
			hook.tile = tile;
			hook.seenTick = GetTickCount();
			hook.keyboardActive = false;
			hook.targetReported = false;
			ResetMenuSearchStateSync(hook);

			DebugLog(
				"tnvse_multibyte_input_debug: menusearch_track name=%s menu=%u root=0x%08X tile=0x%08X id=%u path='%s' string='%s'",
				hook.name,
				hook.menuID,
				reinterpret_cast<UInt32>(root),
				reinterpret_cast<UInt32>(tile),
				TileID(tile),
				path ? path : "",
				tile->GetValueString(Tile::kTileValue_string));
			DebugLogMenuSearchState("tile_tracked", hook, GetOpenMenu(hook.menuID));
		}

		Tile* __fastcall TileReadXMLHook(Tile* root, void*, const char* xmlPath)
		{
			Tile* result = s_originalTileReadXML(root, xmlPath);
			if (result && IsStewieMenuSearchXmlPath(xmlPath))
			{
				StewieMenuSearchHook* hook = FindMenuSearchHookByRoot(root);
				if (!hook)
					hook = FindMenuSearchHookByXmlPath(xmlPath);

				if (hook)
				{
					TrackMenuSearchTile(*hook, root, result, xmlPath);
				}
				else
				{
					DebugLog(
						"tnvse_multibyte_input_debug: menusearch_track_unmapped root=0x%08X tile=0x%08X id=%u path='%s'",
						reinterpret_cast<UInt32>(root),
						reinterpret_cast<UInt32>(result),
						TileID(result),
						xmlPath ? xmlPath : "");
				}
			}

			return result;
		}

		void TryInstallTileReadXMLHook()
		{
			if (s_tileReadXMLHookInstalled)
				return;

			UInt8* trampoline = static_cast<UInt8*>(
				VirtualAlloc(nullptr, kReadXMLPatchLen + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
			if (!trampoline)
			{
				gLog.FormattedMessage("tnvse_multibyte_input: failed to allocate Tile::ReadXML trampoline");
				return;
			}

			std::memcpy(trampoline, reinterpret_cast<void*>(kAddr_ReadXML), kReadXMLPatchLen);
			WriteRelJump(
				reinterpret_cast<SIZE_T>(trampoline + kReadXMLPatchLen),
				kAddr_ReadXML + kReadXMLPatchLen);

			s_tileReadXMLTrampoline = trampoline;
			s_originalTileReadXML = reinterpret_cast<TileReadXMLFn>(trampoline);
			WriteRelJump(kAddr_ReadXML, reinterpret_cast<SIZE_T>(&TileReadXMLHook));
			s_tileReadXMLHookInstalled = true;

			gLog.FormattedMessage(
				"tnvse_multibyte_input: Tile::ReadXML hook installed addr=0x%08X patchLen=%u",
				static_cast<UInt32>(kAddr_ReadXML),
				static_cast<UInt32>(kReadXMLPatchLen));
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
				// Legacy fallback for older MenuSearch XML tracking. Tile traits are
				// only used to reject a tracked target, never to activate one.
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

			if (!IsMenuSearchTileActive(*hook, searchTile))
			{
				DeactivateMenuSearch(*hook, "tile_inactive", searchTile);
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

			TryInstallTileReadXMLHook();
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
				HideCandidateOverlay();
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

		bool TranslateMenuSearchHotkeyMessage(UINT msg, WPARAM wParam, LPARAM lParam, UInt32& key, const char*& source)
		{
			key = 0;
			source = nullptr;

			if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
			{
				if (lParam & (1 << 30))
					return false;
				if (!IsCtrlKeyDown() || (wParam != 'F' && wParam != 'R'))
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
			if (!IsCtrlKeyDown())
				return false;

			const UInt32 lowered = static_cast<UInt32>(wParam) | 0x20;
			if (lowered != 'f' && lowered != 'r')
				return false;

			key = lowered;
			source = "WndProc.WM_CHAR";
			return true;
		}

		bool ObserveStewieMenuSearchHotkeyMessage(UINT msg, WPARAM wParam, LPARAM lParam)
		{
			UInt32 key = 0;
			const char* source = nullptr;
			if (!TranslateMenuSearchHotkeyMessage(msg, wParam, lParam, key, source))
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

			ScheduleMenuSearchStateSync(menu, key, source, false);
			return true;
		}

		void ProcessStewieMenuSearchPendingStateSync()
		{
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
					HideCandidateOverlay();
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
					HideCandidateOverlay();
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

				const bool hasSearchTile = HasMenuSearchTileForHotkey(menu);
				if (!hasSearchTile
					&& static_cast<SInt32>(now - hook.stateSyncStartTick)
						< static_cast<SInt32>(kStewieMenuSearchStateSyncTimeoutMs))
				{
					hook.stateSyncDueTick = now + kStewieMenuSearchStateSyncRetryMs;
					DebugLog(
						"tnvse_multibyte_input_event: source=MainLoop action=menusearch_sync_retry_no_tile menu=%u retryInMs=%u",
						hook.menuID,
						static_cast<UInt32>(kStewieMenuSearchStateSyncRetryMs));
					continue;
				}

				hook.keyboardActive = !hook.stateSyncWasActive && hasSearchTile;
				DebugLog(
					"tnvse_multibyte_input_event: source=MainLoop action=menusearch_sync_apply menu=%u hasTile=%u activeAfter=%u",
					hook.menuID,
					hasSearchTile ? 1 : 0,
					hook.keyboardActive ? 1 : 0);
				DebugLogMenuSearchState("sync_applied", hook, menu);
				ResetMenuSearchStateSync(hook);
				ClearStewieInputState();
			}

			for (StewieMenuSearchHook& hook : s_menuSearchHooks)
			{
				if (!hook.keyboardActive || hook.stateSyncPending)
					continue;

				if (!IsGameMenuVisible(hook.menuID))
				{
					// Pip-Boy menu objects remain allocated while tabs change.
					if (IsPipboySearchMenu(hook.menuID) && IsAnyPipboySearchMenuVisible())
						continue;

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
				if (!IsMenuSearchTileActive(hook, searchTile))
					DeactivateMenuSearch(hook, "tile_inactive", searchTile);
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

			handled = CallStewieOriginalInput(menu, input);
			ClearStewieInputState();
			ScheduleMenuSearchStateSync(menu, input, "StewieTweaksInputTarget", handled);
			return true;
		}

		bool HandleMenuSearchInput(Menu* menu, UInt32 input)
		{
			bool controlHandled = false;
			if (HandleMenuSearchControlInput(menu, input, controlHandled))
				return controlHandled;

			const StewieInputTarget target = FindStewieMenuSearchTarget(menu);
			const bool handled = HandleStewieInput(menu, input);

			// Stewie's Inventory and Stats InputField deactivates on Enter. Map
			// reserves Enter for recentering and keeps search active.
			if (input == kInputCode_Enter
				&& handled
				&& target.valid
				&& target.inputField
				&& MenuID(menu) != PipboyData)
			{
				if (StewieMenuSearchHook* hook = FindMenuSearchHookByMenu(menu))
					DeactivateMenuSearch(*hook, "inputfield_enter", target.tile);
			}

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

			// MenuSearch is installed from Stewie's DeferredInit. Its XML load is
			// the first reliable indication that deferred initialization started.
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
			TryInstallTileReadXMLHook();
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

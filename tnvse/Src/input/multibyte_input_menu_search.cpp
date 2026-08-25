#include "multibyte_input_internal.h"
#include "hook_identity.h"
#include "menu_search_owner_state.h"
#include "Utils/SafeWrite.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

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
		constexpr size_t kMaximumMenuTileTraversalNodes = 16384;
		using MenuSearchOwnerState = menu_search_owner_state::State;

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
			MenuSearchOwnerState ownerState = MenuSearchOwnerState::Unknown;
			bool ownerReconcilePending = false;
			DWORD ownerReconcileStartTick = 0;
			DWORD ownerReconcileDueTick = 0;
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

		bool TileTreeContains(Tile* root, Tile* target);

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

		MenuSearchOwnerState ObserveMenuSearchOwnerState(
			const StewieMenuSearchAdapterSite& site,
			Menu* menu,
			Tile* tile)
		{
			menu_search_owner_state::Observation observation;
			observation.menuVisible = menu && IsGameMenuVisible(site.menuID);
			observation.tileAvailable = tile != nullptr;
			observation.ownerSurfaceEnabled = site.menuID != Pause
				|| IsStartMenuSaveListEnabled(menu);
			observation.usesInputField = MenuSearchUsesInputField(site.menuID);
			if (!observation.menuVisible || !observation.ownerSurfaceEnabled
				|| !observation.tileAvailable)
			{
				return menu_search_owner_state::Resolve(observation);
			}

			__try
			{
				if (observation.usesInputField)
				{
					const UInt32 activeTrait = MenuSearchInputActiveTrait();
					const Tile::Value* activeValue = activeTrait
						? tile->GetValue(activeTrait) : nullptr;
					observation.activeTraitPresent = activeValue != nullptr;
					observation.activeTraitValue = activeValue
						? activeValue->fNum : 0.0f;
				}
				else
				{
					const Tile::Value* visibleValue =
						tile->GetValue(Tile::kTileValue_visible);
					const Tile::Value* alphaValue =
						tile->GetValue(Tile::kTileValue_alpha);
					const Tile::Value* stringValue =
						tile->GetValue(Tile::kTileValue_string);
					observation.visibleTraitPresent = visibleValue != nullptr;
					observation.visibleTraitValue = visibleValue
						? visibleValue->fNum : 0.0f;
					observation.alphaTraitPresent = alphaValue != nullptr;
					observation.alphaTraitValue = alphaValue
						? alphaValue->fNum : 0.0f;
					observation.stringTraitPresent = stringValue != nullptr;
					observation.stringNonEmpty = stringValue
						&& stringValue->pcText && stringValue->pcText[0];
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				// A menu/root replacement can invalidate a non-owning Tile pointer
				// between validation and observation. Unknown fails closed and lets
				// the main-loop discovery path acquire the replacement.
				observation.tileAvailable = false;
			}

			return menu_search_owner_state::Resolve(observation);
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

		void ResetMenuSearchOwnerReconcile(StewieMenuSearchAdapterSite& site)
		{
			site.ownerReconcilePending = false;
			site.ownerReconcileStartTick = 0;
			site.ownerReconcileDueTick = 0;
		}

		bool TileTreeContains(Tile* root, Tile* target)
		{
			if (!root || !target)
				return false;

			std::vector<Tile*> pending;
			std::unordered_set<Tile*> visited;
			pending.reserve(128);
			visited.reserve(256);
			pending.push_back(root);
			while (!pending.empty()
				&& visited.size() < kMaximumMenuTileTraversalNodes)
			{
				Tile* current = pending.back();
				pending.pop_back();
				if (!current || !visited.insert(current).second)
					continue;
				if (current == target)
					return true;

				const size_t queuedAndVisited = std::min(
					kMaximumMenuTileTraversalNodes,
					visited.size() + pending.size());
				UInt32 remaining = static_cast<UInt32>(
					kMaximumMenuTileTraversalNodes - queuedAndVisited);
				remaining = std::min(remaining,
					current->kChildren.GetSize());
				NiTListIterator childPosition =
					current->kChildren.GetHeadPos();
				while (childPosition && remaining--)
				{
					Tile* child = current->kChildren.GetNext(
						childPosition);
					if (child)
						pending.push_back(child);
				}
			}

			if (!pending.empty())
			{
				DebugLog(
					"tnvse_multibyte_input_debug: bounded Tile containment traversal aborted root=0x%08X target=0x%08X visited=%u",
					reinterpret_cast<UInt32>(root),
					reinterpret_cast<UInt32>(target),
					static_cast<UInt32>(visited.size()));
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
			// Preserve seenTick as replacement history. The old non-owning Tile
			// pointer is forgotten immediately, while the next Track call can still
			// distinguish a replacement from the first attachment and refresh an
			// already-active target identity.
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
			const SInt32 dueInMs = site.ownerReconcilePending
				? static_cast<SInt32>(site.ownerReconcileDueTick - GetTickCount())
				: 0;
			const MenuSearchOwnerState observedState = ObserveMenuSearchOwnerState(
				site, menu, resolvedTile);
			SIZE_T currentHandler = 0;
			ReadMenuSearchHandler(site, currentHandler);

			gLog.FormattedMessage(
				"tnvse_multibyte_input_menusearch: stage=%s name=%s menuID=%u inputField=%u "
				"gameVisible=%u pipVisible=%u/%u/%u menu=0x%08X activeMenu=0x%08X activeMenuID=%u "
				"root=0x%08X trackedRaw=0x%08X tracked=0x%08X fallback=0x%08X resolved=0x%08X "
				"tileID=%u tileVisible=%.1f tileAlpha=%.1f ownerState=%s pending=%u "
				"dueInMs=%d observedState=%s installed=%u currentHandler=0x%08X expectedHandler=0x%08X "
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
				menu_search_owner_state::Name(site.ownerState),
				site.ownerReconcilePending ? 1 : 0,
				dueInMs,
				menu_search_owner_state::Name(observedState),
				site.adapterInstalled ? 1 : 0,
				static_cast<UInt32>(currentHandler),
				static_cast<UInt32>(site.adapterHandler),
				static_cast<UInt32>(site.predecessorHandler),
				resolvedTile ? resolvedTile->GetValueString(Tile::kTileValue_string) : "");
		}

		void ApplyObservedMenuSearchOwnerState(
			StewieMenuSearchAdapterSite& site,
			MenuSearchOwnerState observedState,
			const char* reason,
			bool forceSessionRefresh = false,
			bool consumePendingReconcile = true)
		{
			const MenuSearchOwnerState previousState = site.ownerState;
			const bool isActive = menu_search_owner_state::IsActive(observedState);
			const bool stateChanged = previousState != observedState;
			const menu_search_owner_state::ReconcileDecision decision =
				menu_search_owner_state::DecideReconcile(
					previousState, observedState, site.adapterInstalled,
					forceSessionRefresh);

			site.ownerState = observedState;
			if (consumePendingReconcile)
				ResetMenuSearchOwnerReconcile(site);
			if (!stateChanged && !(forceSessionRefresh && isActive))
				return;

			site.targetReported = false;
			if (decision.clearCachedTarget)
				ClearStewieInputState();

			if (decision.activateSession)
			{
				RefreshTextInputSessionForActiveTarget(
					reason ? reason : "menusearch_owner_activate");
			}
			else if (decision.endSession)
			{
				EndStewieTextInputSession(
					reason ? reason : "menusearch_owner_deactivate");
			}

			DebugLog(
				"tnvse_multibyte_input_event: source=OwnerState action=menusearch_reconcile reason=%s menu=%u previous=%s observed=%s forceRefresh=%u",
				reason ? reason : "unknown",
				site.menuID,
				menu_search_owner_state::Name(previousState),
				menu_search_owner_state::Name(observedState),
				forceSessionRefresh ? 1 : 0);
			DebugLogMenuSearchState(
				reason ? reason : "owner_reconciled", site,
				GetOpenMenu(site.menuID));
		}

		void TrackMenuSearchTile(
			StewieMenuSearchAdapterSite& site,
			Menu* menu,
			Tile* root,
			Tile* tile,
			bool refreshOnReplacement = true)
		{
			if (!tile)
				return;

			const bool replaced =
				menu_search_owner_state::IsTrackedTileReplacement(
					site.seenTick != 0, site.root == root, site.tile == tile);
			site.root = root;
			site.tile = tile;
			site.seenTick = GetTickCount();
			site.targetReported = false;
			const MenuSearchOwnerState observedState =
				ObserveMenuSearchOwnerState(site, menu, tile);
			ApplyObservedMenuSearchOwnerState(
				site,
				observedState,
				replaced ? "menusearch_tile_replaced" : "menusearch_tile_tracked",
				replaced && refreshOnReplacement
					&& menu_search_owner_state::IsActive(observedState),
				!site.ownerReconcilePending);

			DebugLog(
				"tnvse_multibyte_input_debug: menusearch_track name=%s menu=%u root=0x%08X tile=0x%08X id=%u source=main_loop replaced=%u observed=%s",
				site.menuName,
				site.menuID,
				reinterpret_cast<UInt32>(root),
				reinterpret_cast<UInt32>(tile),
				TileID(tile),
				replaced ? 1 : 0,
				menu_search_owner_state::Name(observedState));
		}

		void DiscoverMenuSearchTiles()
		{
			const bool needsActiveMaintenance = std::any_of(
				std::begin(s_menuSearchSites), std::end(s_menuSearchSites),
				[](const StewieMenuSearchAdapterSite& site)
				{
					return menu_search_owner_state::IsActive(site.ownerState)
						|| site.ownerReconcilePending;
				});
			// Before publication, discovery is used only to prove that Stewie has
			// finished injecting its search tiles. Once the adapters are installed,
			// an inactive menu has no tNVSE text-input target and must stay entirely
			// on the predecessor path. In particular, do not walk MapMenu while it is
			// rebuilding the Data/Notes subtree.
			if (s_menuSearchAdaptersInstalled && !needsActiveMaintenance)
				return;

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
				if (s_menuSearchAdaptersInstalled
					&& !menu_search_owner_state::IsActive(site.ownerState)
					&& !site.ownerReconcilePending)
				{
					continue;
				}
				if (!IsGameMenuVisible(site.menuID))
					continue;

				Menu* menu = GetOpenMenu(site.menuID);
				Tile* root = MenuRoot(menu);
				if (root && site.root == root && site.tile)
					continue;

				Tile* tile = root ? FindTileByID(root, kStewieMenuSearch_TextTile) : nullptr;
				if (!tile || (site.root == root && site.tile == tile))
					continue;

				TrackMenuSearchTile(site, menu, root, tile);
			}
		}

		StewieInputTarget FindStewieMenuSearchTarget(Menu* menu)
		{
			if (!menu)
				return {};

			StewieMenuSearchAdapterSite* site = FindMenuSearchSiteByMenu(menu);
			if (!site || !site->adapterInstalled
				|| !menu_search_owner_state::IsActive(site->ownerState))
				return {};

			Tile* root = MenuRoot(menu);
			if (!root)
				return {};

			Tile* searchTile = GetTrackedMenuSearchTile(menu);
			if (!searchTile)
			{
				searchTile = FindTileByID(root, kStewieMenuSearch_TextTile);
				if (searchTile)
				{
					// Synchronize the non-owning cache, but do not recursively refresh
					// the IME session while this function may itself be resolving the
					// active target for a refresh.
					TrackMenuSearchTile(*site, menu, root, searchTile, false);
				}
			}

			if (!searchTile)
			{
				ApplyObservedMenuSearchOwnerState(
					*site, MenuSearchOwnerState::Unknown,
					"menusearch_no_tracked_searchbar");
				DebugLog(
					"tnvse_multibyte_input_debug: menusearch_target_miss reason=no_tracked_searchbar menu=%u root=0x%08X legacyID=%u",
					MenuID(menu),
					reinterpret_cast<UInt32>(root),
					kStewieMenuSearch_TextTile);
				return {};
			}
			if (!menu_search_owner_state::IsActive(site->ownerState))
				return {};

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

		void ScheduleMenuSearchOwnerReconcile(
			Menu* menu,
			UInt32 input,
			const char* source,
			bool predecessorHandled)
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
			site->targetReported = false;
			if (!site->ownerReconcilePending)
				site->ownerReconcileStartTick = now;
			site->ownerReconcilePending = true;
			site->ownerReconcileDueTick =
				now + kStewieMenuSearchStateSyncDelayMs;

			DebugLog(
				"tnvse_multibyte_input_event: source=%s action=menusearch_owner_reconcile_schedule menu=%u key=0x%08X handled=%u cached=%s dueInMs=%u",
				source ? source : "unknown",
				MenuID(menu),
				input,
				predecessorHandled ? 1 : 0,
				menu_search_owner_state::Name(site->ownerState),
				static_cast<UInt32>(kStewieMenuSearchStateSyncDelayMs));
			DebugLogMenuSearchState("owner_reconcile_scheduled", *site, menu);
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

			ScheduleMenuSearchOwnerReconcile(menu, key, source, false);
			return true;
		}

		void ProcessStewieMenuSearchPendingStateSync()
		{
			const bool needsMaintenance = std::any_of(std::begin(s_menuSearchSites),
				std::end(s_menuSearchSites), [](const StewieMenuSearchAdapterSite& site)
				{
					return site.ownerReconcilePending
						|| menu_search_owner_state::IsActive(site.ownerState);
				});
			if (!needsMaintenance)
				return;
			// Only an active or explicitly pending site is observed. Inactive menus
			// retain the zero-traversal fast path, especially while MapMenu rebuilds
			// its Data/Notes subtree.
			const DWORD now = GetTickCount();
			for (StewieMenuSearchAdapterSite& site : s_menuSearchSites)
			{
				if (!site.ownerReconcilePending
					|| static_cast<SInt32>(now - site.ownerReconcileDueTick) < 0)
				{
					continue;
				}

				Menu* menu = GetOpenMenu(site.menuID);
				DebugLogMenuSearchState("owner_reconcile_due", site, menu);
				if (!menu)
				{
					ApplyObservedMenuSearchOwnerState(
						site, MenuSearchOwnerState::Inactive,
						"menusearch_reconcile_no_menu");
					continue;
				}

				Tile* root = MenuRoot(menu);
				Tile* searchTile = GetTrackedMenuSearchTile(menu);
				if (!searchTile)
					searchTile = FindTileByID(root, kStewieMenuSearch_TextTile);
				if (searchTile && (site.root != root || site.tile != searchTile))
					TrackMenuSearchTile(site, menu, root, searchTile);

				const MenuSearchOwnerState observedState =
					ObserveMenuSearchOwnerState(site, menu, searchTile);
				if (observedState == MenuSearchOwnerState::Unknown
					&& static_cast<SInt32>(now - site.ownerReconcileStartTick)
						< static_cast<SInt32>(kStewieMenuSearchStateSyncTimeoutMs))
				{
					site.ownerReconcileDueTick =
						now + kStewieMenuSearchStateSyncRetryMs;
					DebugLog(
						"tnvse_multibyte_input_event: source=MainLoop action=menusearch_owner_reconcile_retry menu=%u hasTile=%u observed=%s retryInMs=%u",
						site.menuID,
						searchTile ? 1 : 0,
						menu_search_owner_state::Name(observedState),
						static_cast<UInt32>(kStewieMenuSearchStateSyncRetryMs));
					continue;
				}

				ApplyObservedMenuSearchOwnerState(
					site, observedState, "menusearch_owner_reconcile");
			}

			for (StewieMenuSearchAdapterSite& site : s_menuSearchSites)
			{
				if (!menu_search_owner_state::IsActive(site.ownerState)
					|| site.ownerReconcilePending)
					continue;

				if (!IsGameMenuVisible(site.menuID))
				{
					ApplyObservedMenuSearchOwnerState(
						site, MenuSearchOwnerState::Inactive,
						"menusearch_menu_hidden");
					continue;
				}

				Menu* menu = GetOpenMenu(site.menuID);
				if (!menu)
				{
					ApplyObservedMenuSearchOwnerState(
						site, MenuSearchOwnerState::Inactive,
						"menusearch_menu_missing");
					continue;
				}

				Tile* root = MenuRoot(menu);
				Tile* searchTile = GetTrackedMenuSearchTile(menu);
				if (!searchTile)
					searchTile = FindTileByID(root, kStewieMenuSearch_TextTile);
				if (!searchTile)
				{
					ApplyObservedMenuSearchOwnerState(
						site, MenuSearchOwnerState::Unknown,
						"menusearch_tile_missing");
					continue;
				}
				if (site.root != root || site.tile != searchTile)
					TrackMenuSearchTile(site, menu, root, searchTile);

				const MenuSearchOwnerState observedState =
					ObserveMenuSearchOwnerState(site, menu, searchTile);
				if (observedState != MenuSearchOwnerState::Active)
					ApplyObservedMenuSearchOwnerState(
						site, observedState, "menusearch_active_maintenance");
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
			handled = CallStewiePredecessorInput(menu, input);
			if (!site)
				return true;

			Tile* root = MenuRoot(menu);
			Tile* searchTile = GetTrackedMenuSearchTile(menu);
			if (!searchTile)
				searchTile = FindTileByID(root, kStewieMenuSearch_TextTile);
			if (searchTile && (site->root != root || site->tile != searchTile))
				TrackMenuSearchTile(*site, menu, root, searchTile);

			const MenuSearchOwnerState observedState =
				ObserveMenuSearchOwnerState(*site, menu, searchTile);
			if (observedState == MenuSearchOwnerState::Unknown)
			{
				ScheduleMenuSearchOwnerReconcile(
					menu, input, "StewieTweaksInputTarget", handled);
			}
			else
			{
				ApplyObservedMenuSearchOwnerState(
					*site, observedState,
					key == 'r' ? "menusearch_ctrl_r_owner"
						: "menusearch_ctrl_f_owner");
			}
			DebugLog(
				"tnvse_multibyte_input_event: source=StewieTweaksInputTarget action=menusearch_control_observed menu=%u key=0x%08X handled=%u observed=%s",
				MenuID(menu),
				input,
				handled ? 1 : 0,
				menu_search_owner_state::Name(observedState));
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

			StewieMenuSearchAdapterSite* site =
				FindMenuSearchSiteByMenu(menu);
			const UInt32 loweredInput = input | 0x20;
			const bool searchControlInput = IsCtrlKeyDown()
				&& (loweredInput == 'f' || loweredInput == 'r');
			if (!site
				|| (!menu_search_owner_state::IsActive(site->ownerState)
					&& !site->ownerReconcilePending
					&& !searchControlInput))
			{
				return CallStewiePredecessorInput(menu, input);
			}

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
				// English layouts as well, keeping the owner state and Ctrl+F in sync.
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
					ApplyObservedMenuSearchOwnerState(
						site, MenuSearchOwnerState::Unknown,
						"menusearch_adapter_ownership_lost");
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
			{
				Menu* menu = IsGameMenuVisible(site.menuID)
					? GetOpenMenu(site.menuID) : nullptr;
				Tile* root = MenuRoot(menu);
				Tile* tile = GetTrackedMenuSearchTile(menu);
				if (!tile && root)
					tile = FindTileByID(root, kStewieMenuSearch_TextTile);

				const bool wasActive =
					menu_search_owner_state::IsActive(site.ownerState);
				if (tile && (site.root != root || site.tile != tile))
					TrackMenuSearchTile(site, menu, root, tile, false);
				const MenuSearchOwnerState observedState =
					ObserveMenuSearchOwnerState(site, menu, tile);
				ApplyObservedMenuSearchOwnerState(
					site, observedState, "menusearch_adapter_installed",
					wasActive
						&& observedState == MenuSearchOwnerState::Active);
				DebugLogMenuSearchState("adapter_installed", site, menu);
			}
		}

		void ResetStewieMenuSearchState()
		{
			for (StewieMenuSearchAdapterSite& site : s_menuSearchSites)
			{
				site.root = nullptr;
				site.tile = nullptr;
				site.seenTick = 0;
				site.ownerState = MenuSearchOwnerState::Unknown;
				site.targetReported = false;
				ResetMenuSearchOwnerReconcile(site);
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

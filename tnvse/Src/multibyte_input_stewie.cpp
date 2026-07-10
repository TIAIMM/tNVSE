#include "multibyte_input_internal.h"

// Stewie Tweaks search and string-subsetting input integration.

namespace fonthook
{
	namespace multibyte_input
	{
		constexpr SIZE_T kAddr_ReadXML = 0x00A01B00;
		constexpr SIZE_T kReadXMLPatchLen = 5;
		constexpr UInt32 kStewieTweaksMinVersion = 990;
		constexpr const char* kStewieTweaksPluginName = "lStewieAl's Tweaks";
		constexpr UInt32 kMenuType_StewMenu = 1069;
		constexpr UInt32 kStewMenu_SearchBar = 5;
		constexpr UInt32 kStewMenu_SubsettingInputFieldText = 103;
		constexpr UInt32 kStewieMenuSearch_TextTile = 87698483;
		constexpr UInt32 kStewieMaxShadowBytes = 1023;
		constexpr DWORD kStewieMenuSearchStateSyncDelayMs = 150;
		constexpr DWORD kStewieMenuSearchStateSyncRetryMs = 50;
		constexpr DWORD kStewieMenuSearchStateSyncTimeoutMs = 1000;
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
		constexpr UInt32 kMenuHandleKeyboardInputVTableOffset = 0x30;

		struct StewieShadowState
		{
			StewieInputTarget target;
			std::string text;
			size_t caret = 0;
			size_t appliedBytes = 0;
			bool initialized = false;
		};

		using StewieKeyboardHandler = bool(__thiscall*)(Menu*, UInt32);

		struct StewieMenuHook
		{
			const char* name = "";
			UInt32 menuID = 0;
			SIZE_T entry = 0;
			SIZE_T original = 0;
			SIZE_T hook = 0;
			bool installed = false;

			Tile* menuSearchRoot = nullptr;
			Tile* menuSearchTile = nullptr;
			DWORD menuSearchSeenTick = 0;
			bool menuSearchKeyboardActive = false;
			bool menuSearchStateSyncPending = false;
			UInt8 menuSearchStateSyncAction = kStewieMenuSearchSync_None;
			bool menuSearchStateSyncWasActive = false;
			DWORD menuSearchStateSyncStartTick = 0;
			DWORD menuSearchStateSyncDueTick = 0;
		};

		bool s_stewieChecked = false;
		bool s_stewieAvailable = false;
		bool s_stewieHooksInstalled = false;
		bool s_stewieReplay = false;
		SIZE_T s_stewMenuOriginalInputHandler = 0;
		SIZE_T s_stewMenuHookedEntry = 0;
		UInt32 s_tileTraitIsActive = 0;
		UInt32 s_tileTraitIsSearchActive = 0;
		UInt32 s_tileTraitCaretIndex = 0;
		StewieShadowState s_stewieShadow;

		using TileReadXMLFn = Tile * (__thiscall*)(Tile*, const char*);

		TileReadXMLFn s_originalTileReadXML = nullptr;
		void* s_tileReadXMLTrampoline = nullptr;
		bool s_tileReadXMLHookInstalled = false;


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

		StewieMenuHook s_stewieMenuHooks[] =
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

		StewieMenuHook* FindStewieHookByMenuID(UInt32 menuID)
		{
			for (StewieMenuHook& hook : s_stewieMenuHooks)
			{
				if (hook.menuID == menuID)
					return &hook;
			}

			return nullptr;
		}

		StewieMenuHook* FindStewieHookByMenu(Menu* menu)
		{
			return menu ? FindStewieHookByMenuID(MenuID(menu)) : nullptr;
		}

		StewieMenuHook* FindStewieHookByRoot(Tile* root)
		{
			if (!root)
				return nullptr;

			for (StewieMenuHook& hook : s_stewieMenuHooks)
			{
				if (Menu* menu = GetOpenMenu(hook.menuID))
				{
					if (MenuRoot(menu) == root)
						return &hook;
				}
			}

			return nullptr;
		}

		StewieMenuHook* FindStewieHookByMenuSearchXmlPath(const char* path)
		{
			if (!path)
				return nullptr;

			if (ContainsNoCase(path, "Inventory.xml"))
				return FindStewieHookByMenuID(Inventory);

			if (ContainsNoCase(path, "Stats.xml"))
				return FindStewieHookByMenuID(Stats);

			if (ContainsNoCase(path, "Map.xml"))
				return FindStewieHookByMenuID(PipboyData);

			if (ContainsNoCase(path, "Container.xml"))
				return FindStewieHookByMenuID(Container);

			if (ContainsNoCase(path, "Barter.xml"))
				return FindStewieHookByMenuID(Barter);

			if (ContainsNoCase(path, "LevelUp.xml"))
				return FindStewieHookByMenuID(LevelUp);

			if (ContainsNoCase(path, "Recipe.xml"))
				return FindStewieHookByMenuID(Recipe);

			if (ContainsNoCase(path, "SaveLoad.xml"))
				return FindStewieHookByMenuID(Pause);

			return nullptr;
		}

		void ResetStewieMenuSearchStateSync(StewieMenuHook& hook)
		{
			hook.menuSearchStateSyncPending = false;
			hook.menuSearchStateSyncAction = kStewieMenuSearchSync_None;
			hook.menuSearchStateSyncWasActive = false;
			hook.menuSearchStateSyncStartTick = 0;
			hook.menuSearchStateSyncDueTick = 0;
		}


		void TrackStewieMenuSearchTile(
			StewieMenuHook& hook,
			Tile* root,
			Tile* tile,
			const char* path)
		{
			if (!tile)
				return;

			hook.menuSearchRoot = root;
			hook.menuSearchTile = tile;
			hook.menuSearchSeenTick = GetTickCount();
			hook.menuSearchKeyboardActive = false;
			ResetStewieMenuSearchStateSync(hook);

			DebugLog(
				"tnvse_multibyte_input_debug: menusearch_track name=%s menu=%u root=0x%08X tile=0x%08X id=%u path='%s' string='%s'",
				hook.name,
				hook.menuID,
				reinterpret_cast<UInt32>(root),
				reinterpret_cast<UInt32>(tile),
				TileID(tile),
				path ? path : "",
				tile->GetValueString(Tile::kTileValue_string));
		}

		Tile* __fastcall TileReadXMLHook(Tile* root, void*, const char* xmlPath)
		{
			Tile* result = s_originalTileReadXML(root, xmlPath);

			if (result && IsStewieMenuSearchXmlPath(xmlPath))
			{
				StewieMenuHook* hook = FindStewieHookByRoot(root);
				if (!hook)
					hook = FindStewieHookByMenuSearchXmlPath(xmlPath);

				if (hook)
				{
					TrackStewieMenuSearchTile(*hook, root, result, xmlPath);
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
				VirtualAlloc(
					nullptr,
					kReadXMLPatchLen + 5,
					MEM_COMMIT | MEM_RESERVE,
					PAGE_EXECUTE_READWRITE));

			if (!trampoline)
			{
				gLog.FormattedMessage("tnvse_multibyte_input: failed to allocate Tile::ReadXML trampoline");
				return;
			}

			std::memcpy(
				trampoline,
				reinterpret_cast<void*>(kAddr_ReadXML),
				kReadXMLPatchLen);

			WriteRelJump(
				reinterpret_cast<SIZE_T>(trampoline + kReadXMLPatchLen),
				kAddr_ReadXML + kReadXMLPatchLen);

			s_tileReadXMLTrampoline = trampoline;
			s_originalTileReadXML = reinterpret_cast<TileReadXMLFn>(trampoline);

			WriteRelJump(
				kAddr_ReadXML,
				reinterpret_cast<SIZE_T>(&TileReadXMLHook));

			s_tileReadXMLHookInstalled = true;

			gLog.FormattedMessage(
				"tnvse_multibyte_input: Tile::ReadXML hook installed addr=0x%08X patchLen=%u",
				static_cast<UInt32>(kAddr_ReadXML),
				static_cast<UInt32>(kReadXMLPatchLen));
		}

		bool TileTreeContains(Tile* root, Tile* target, UInt32 depth = 0)
		{
			if (!root || !target || depth > 64)
				return false;

			if (root == target)
				return true;

			const std::vector<Tile*> children = root->GetChildren();
			for (Tile* child : children)
			{
				if (TileTreeContains(child, target, depth + 1))
					return true;
			}

			return false;
		}

		Tile* GetTrackedMenuSearchTile(Menu* menu)
		{
			StewieMenuHook* hook = FindStewieHookByMenu(menu);
			if (!hook || !hook->menuSearchTile)
				return nullptr;

			Tile* root = MenuRoot(menu);
			if (!root)
				return nullptr;

			if (TileTreeContains(root, hook->menuSearchTile))
				return hook->menuSearchTile;

			DebugLog(
				"tnvse_multibyte_input_debug: menusearch_track_stale name=%s menu=%u oldRoot=0x%08X newRoot=0x%08X oldTile=0x%08X",
				hook->name,
				hook->menuID,
				reinterpret_cast<UInt32>(hook->menuSearchRoot),
				reinterpret_cast<UInt32>(root),
				reinterpret_cast<UInt32>(hook->menuSearchTile));

			hook->menuSearchRoot = nullptr;
			hook->menuSearchTile = nullptr;
			hook->menuSearchSeenTick = 0;
			hook->menuSearchKeyboardActive = false;
			ResetStewieMenuSearchStateSync(*hook);
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

		std::string TileStringWithoutCaret(Tile* tile, bool inputField, size_t& caret)
		{
			caret = 0;
			std::string text = tile ? tile->GetValueString(Tile::kTileValue_string) : "";
			const size_t caretMarker = text.find('|');
			if (caretMarker != std::string::npos)
			{
				text.erase(caretMarker, 1);
				caret = ClampToPrevUTF8Boundary(text, caretMarker);
				return text;
			}

			if (inputField
				&& s_tileTraitCaretIndex
				&& TileTraitFloat(tile, s_tileTraitIsActive) > 0.5f)
			{
				const size_t caretIndex = static_cast<size_t>(TileTraitFloat(tile, s_tileTraitCaretIndex));
				if (caretIndex < text.size())
				{
					text.erase(caretIndex, 1);
					caret = ClampToPrevUTF8Boundary(text, caretIndex);
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

			StewieMenuHook* hook = FindStewieHookByMenu(menu);
			if (!hook || !hook->menuSearchKeyboardActive)
				return {};

			Tile* root = MenuRoot(menu);
			if (!root)
				return {};

			Tile* searchTile = GetTrackedMenuSearchTile(menu);

			if (!searchTile)
			{
				// Legacy fallback for older MenuSearch XML tracking. Do not use tile visibility or _IsActive as state.
				searchTile = FindTileByID(root, kStewieMenuSearch_TextTile);
			}

			if (!searchTile)
			{
				hook->menuSearchKeyboardActive = false;
				DebugLog(
					"tnvse_multibyte_input_debug: menusearch_target_miss reason=no_tracked_searchbar menu=%u root=0x%08X legacyID=%u",
					MenuID(menu),
					reinterpret_cast<UInt32>(root),
					kStewieMenuSearch_TextTile);
				return {};
			}

			DebugLog(
				"tnvse_multibyte_input_debug: menusearch_target_found menu=%u tile=0x%08X id=%u string='%s' keyboardActive=%u",
				MenuID(menu),
				reinterpret_cast<UInt32>(searchTile),
				TileID(searchTile),
				searchTile->GetValueString(Tile::kTileValue_string),
				hook->menuSearchKeyboardActive ? 1 : 0);

			return MakeStewieTarget(
				StewieInputKind::MenuSearch,
				menu,
				searchTile,
				false);
		}

		StewieInputTarget FindStewieTargetForMenu(Menu* menu)
		{
			if (!IsStewieTweaksAvailable() || !menu)
				return {};

			if (MenuID(menu) == kMenuType_StewMenu)
				return FindStewMenuTarget(menu);

			return FindStewieMenuSearchTarget(menu);
		}

		StewieInputTarget GetActiveStewieInputTarget()
		{
			if (!IsStewieTweaksAvailable())
				return {};

			TryInstallTileReadXMLHook();

			if (Menu* menu = GetOpenMenu(kMenuType_StewMenu))
			{
				if (StewieInputTarget target = FindStewMenuTarget(menu); target.valid)
					return target;
			}

			for (const StewieMenuHook& hook : s_stewieMenuHooks)
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

			if (!s_stewieShadow.initialized)
				return {};

			switch (s_stewieShadow.target.kind)
			{
			case StewieInputKind::StewMenuSearch:
			case StewieInputKind::StewMenuStringSubsetting:
			{
				if (Menu* menu = GetOpenMenu(kMenuType_StewMenu))
				{
					if (StewieInputTarget target = FindStewMenuTarget(menu); target.valid)
						return target;
				}
				break;
			}

			case StewieInputKind::MenuSearch:
			{
				for (const StewieMenuHook& hook : s_stewieMenuHooks)
				{
					if (Menu* menu = GetOpenMenu(hook.menuID))
					{
						if (StewieInputTarget target = FindStewieMenuSearchTarget(menu); target.valid)
							return target;
					}
				}
				break;
			}

			default:
				break;
			}

			ClearStewieInputState();
			HideCandidateOverlay();
			return {};
		}


		SIZE_T OriginalStewieHandlerForMenu(Menu* menu)
		{
			if (!menu)
				return 0;

			const UInt32 menuID = MenuID(menu);
			if (menuID == kMenuType_StewMenu)
				return s_stewMenuOriginalInputHandler;

			for (const StewieMenuHook& hook : s_stewieMenuHooks)
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
			s_stewieShadow.text = TileStringWithoutCaret(target.tile, target.inputField, s_stewieShadow.caret);
			if (!target.inputField && s_stewieShadow.text == "_")
				s_stewieShadow.text.clear();
			s_stewieShadow.caret = ClampToPrevUTF8Boundary(s_stewieShadow.text, s_stewieShadow.caret);
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

			const size_t caret = ClampToPrevUTF8Boundary(s_stewieShadow.text, s_stewieShadow.caret);
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
			s_stewieShadow.caret = ClampToPrevUTF8Boundary(s_stewieShadow.text, caret);
			s_stewieShadow.initialized = true;
			return ReplayStewieShadow(target);
		}

		bool InsertTextAtCaretStewie(const StewieInputTarget& target, std::string_view text)
		{
			if (!target.valid || text.empty())
				return false;

			EnsureStewieShadow(target);
			std::string candidate = s_stewieShadow.text;
			size_t caret = ClampToPrevUTF8Boundary(candidate, s_stewieShadow.caret);
			candidate.insert(caret, text.data(), text.size());
			return CommitStewieShadow(target, std::move(candidate), caret + text.size());
		}

		bool InsertWideTextStewie(const StewieInputTarget& target, std::wstring_view text)
		{
			std::string converted = WideToUTF8(text);
			if (converted.empty())
				return false;

			return InsertTextAtCaretStewie(target, converted);
		}

		bool DeletePreviousStewieChar(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			size_t caret = ClampToPrevUTF8Boundary(s_stewieShadow.text, s_stewieShadow.caret);
			if (!caret)
				return true;

			const size_t previous = PrevUTF8CharBoundary(s_stewieShadow.text, caret);
			std::string candidate = s_stewieShadow.text;
			candidate.erase(previous, caret - previous);
			return CommitStewieShadow(target, std::move(candidate), previous);
		}

		bool DeleteNextStewieChar(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			size_t caret = ClampToPrevUTF8Boundary(s_stewieShadow.text, s_stewieShadow.caret);
			if (caret >= s_stewieShadow.text.size())
				return true;

			const size_t next = NextUTF8CharBoundary(s_stewieShadow.text, caret);
			std::string candidate = s_stewieShadow.text;
			candidate.erase(caret, next - caret);
			return CommitStewieShadow(target, std::move(candidate), caret);
		}

		bool MoveStewieCaretPrevious(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			return CommitStewieShadow(
				target,
				s_stewieShadow.text,
				PrevUTF8CharBoundary(s_stewieShadow.text, s_stewieShadow.caret));
		}

		bool MoveStewieCaretNext(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			return CommitStewieShadow(
				target,
				s_stewieShadow.text,
				NextUTF8CharBoundary(s_stewieShadow.text, s_stewieShadow.caret));
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

		bool RemovePreviousStewieAsciiCompositionEcho(wchar_t compositionLead)
		{
			StewieInputTarget target = GetOverlayStewieInputTarget();
			if (!target.valid)
				return false;

			EnsureStewieShadow(target);
			size_t caret = ClampToPrevUTF8Boundary(s_stewieShadow.text, s_stewieShadow.caret);
			if (!caret)
				return false;

			const size_t previous = PrevUTF8CharBoundary(s_stewieShadow.text, caret);
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

		void ClearStewieMenuSearchTracking()
		{
			for (StewieMenuHook& hook : s_stewieMenuHooks)
			{
				hook.menuSearchRoot = nullptr;
				hook.menuSearchTile = nullptr;
				hook.menuSearchSeenTick = 0;
				hook.menuSearchKeyboardActive = false;
				ResetStewieMenuSearchStateSync(hook);
			}
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

		void ScheduleStewieMenuSearchStateSync(Menu* menu, UInt32 input, const char* source, bool originalHandled)
		{
			if (!menu || MenuID(menu) == kMenuType_StewMenu)
				return;

			StewieMenuHook* hook = FindStewieHookByMenu(menu);
			if (!hook)
				return;

			const UInt32 key = input | 0x20;
			if (key != 'f' && key != 'r')
				return;

			const DWORD now = GetTickCount();
			const bool wasActive = hook->menuSearchKeyboardActive;

			hook->menuSearchStateSyncPending = true;
			hook->menuSearchStateSyncAction = key == 'r'
				? kStewieMenuSearchSync_Deactivate
				: kStewieMenuSearchSync_Toggle;
			hook->menuSearchStateSyncWasActive = wasActive;
			hook->menuSearchStateSyncStartTick = now;
			hook->menuSearchStateSyncDueTick = now + kStewieMenuSearchStateSyncDelayMs;

			if (key == 'r')
			{
				hook->menuSearchKeyboardActive = false;
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
		}

		Menu* GetStewieMenuSearchHotkeyMenu()
		{
			if (InterfaceManager* manager = InterfaceManager::GetSingleton())
			{
				if (Menu* activeMenu = manager->pActiveMenu)
				{
					if (FindStewieHookByMenu(activeMenu))
						return activeMenu;
				}
			}

			for (StewieMenuHook& hook : s_stewieMenuHooks)
			{
				if (Menu* menu = GetOpenMenu(hook.menuID))
				{
					if (HasMenuSearchTileForHotkey(menu))
						return menu;
				}
			}

			return nullptr;
		}

		bool TryTranslateMenuSearchHotkeyMessage(UINT msg, WPARAM wParam, LPARAM lParam, UInt32& key, const char*& source)
		{
			key = 0;
			source = nullptr;

			if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
			{
				if (lParam & (1 << 30))
					return false;

				if (!IsCtrlKeyDown())
					return false;

				if (wParam != 'F' && wParam != 'R')
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
			if (!TryTranslateMenuSearchHotkeyMessage(msg, wParam, lParam, key, source))
				return false;

			Menu* menu = GetStewieMenuSearchHotkeyMenu();
			if (!menu)
			{
				DebugLog(
					"tnvse_multibyte_input_event: source=%s action=menusearch_hotkey_no_menu key=0x%08X raw=0x%08X",
					source ? source : "WndProc",
					key,
					static_cast<UInt32>(wParam));
				return false;
			}

			ScheduleStewieMenuSearchStateSync(
				menu,
				key,
				source,
				false);
			return true;
		}

		void ProcessStewieMenuSearchPendingStateSync()
		{
			const DWORD now = GetTickCount();

			for (StewieMenuHook& hook : s_stewieMenuHooks)
			{
				if (!hook.menuSearchStateSyncPending)
					continue;

				if (static_cast<SInt32>(now - hook.menuSearchStateSyncDueTick) < 0)
					continue;

				Menu* menu = GetOpenMenu(hook.menuID);
				if (!menu)
				{
					hook.menuSearchKeyboardActive = false;
					ResetStewieMenuSearchStateSync(hook);
					ClearStewieInputState();
					HideCandidateOverlay();
					DebugLog(
						"tnvse_multibyte_input_event: source=MainLoop action=menusearch_sync_cancel_no_menu menu=%u",
						hook.menuID);
					continue;
				}

				if (hook.menuSearchStateSyncAction == kStewieMenuSearchSync_Deactivate)
				{
					hook.menuSearchKeyboardActive = false;
					ResetStewieMenuSearchStateSync(hook);
					ClearStewieInputState();
					HideCandidateOverlay();
					DebugLog(
						"tnvse_multibyte_input_event: source=MainLoop action=menusearch_sync_deactivate menu=%u",
						hook.menuID);
					continue;
				}

				if (hook.menuSearchStateSyncAction != kStewieMenuSearchSync_Toggle)
				{
					ResetStewieMenuSearchStateSync(hook);
					continue;
				}

				const bool hasSearchTile = HasMenuSearchTileForHotkey(menu);
				if (!hasSearchTile
					&& static_cast<SInt32>(now - hook.menuSearchStateSyncStartTick) < static_cast<SInt32>(kStewieMenuSearchStateSyncTimeoutMs))
				{
					hook.menuSearchStateSyncDueTick = now + kStewieMenuSearchStateSyncRetryMs;
					DebugLog(
						"tnvse_multibyte_input_event: source=MainLoop action=menusearch_sync_retry_no_tile menu=%u retryInMs=%u",
						hook.menuID,
						static_cast<UInt32>(kStewieMenuSearchStateSyncRetryMs));
					continue;
				}

				hook.menuSearchKeyboardActive = !hook.menuSearchStateSyncWasActive && hasSearchTile;
				ResetStewieMenuSearchStateSync(hook);
				ClearStewieInputState();

				DebugLog(
					"tnvse_multibyte_input_event: source=MainLoop action=menusearch_sync_apply menu=%u hasTile=%u activeAfter=%u",
					hook.menuID,
					hasSearchTile ? 1 : 0,
					hook.menuSearchKeyboardActive ? 1 : 0);
			}
		}

		bool HandleStewieMenuSearchControlInput(Menu* menu, UInt32 input, bool& handled)
		{
			handled = false;

			if (!menu || MenuID(menu) == kMenuType_StewMenu || !IsCtrlKeyDown())
				return false;

			const UInt32 key = input | 0x20;
			if (key != 'f' && key != 'r')
				return false;

			handled = CallStewieOriginalInput(menu, input);
			ClearStewieInputState();
			ScheduleStewieMenuSearchStateSync(
				menu,
				input,
				"StewieTweaksInputTarget",
				handled);

			return true;
		}

		bool HandleStewieInput(Menu* menu, UInt32 input)
		{
			if (s_stewieReplay)
				return CallStewieOriginalInput(menu, input);

			bool menuSearchControlHandled = false;
			if (HandleStewieMenuSearchControlInput(menu, input, menuSearchControlHandled))
				return menuSearchControlHandled;

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

		void TryInstallStewieTweaksInputHooks()
		{
			if (!IsStewieTweaksAvailable())
				return;

			TryInstallTileReadXMLHook();

			if (!s_tileTraitIsActive)
				s_tileTraitIsActive = Tile::TraitNameToID("_IsActive");
			if (!s_tileTraitIsSearchActive)
				s_tileTraitIsSearchActive = Tile::TraitNameToID("_IsSearchActive");
			if (!s_tileTraitCaretIndex)
				s_tileTraitCaretIndex = Tile::TraitNameToID("_CaretIndex");

			if (!s_stewieHooksInstalled)
			{
				for (StewieMenuHook& hook : s_stewieMenuHooks)
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


		void ResetStewieInputState()
		{
			ClearStewieInputState();
			ClearStewieMenuSearchTracking();
			s_stewieReplay = false;
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
}

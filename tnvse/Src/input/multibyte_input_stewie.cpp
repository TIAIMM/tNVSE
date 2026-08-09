#include "multibyte_input_internal.h"
#include "hook_identity.h"
#include "plugin_dependencies.h"
#include "Utils/SafeWrite.h"

// Shared Stewie Tweaks UTF-8 input engine and StewMenu integration.

namespace fonthook
{
	namespace multibyte_input
	{
		constexpr UInt32 kMenuType_StewMenu = 1069;
		constexpr UInt32 kStewMenu_SearchBar = 5;
		constexpr UInt32 kStewMenu_SubsettingInputFieldText = 103;
		constexpr UInt32 kStewieMaxShadowBytes = 1023;
		constexpr UInt32 kMenuHandleKeyboardInputVTableOffset = 0x30;
		// Stewie Tweaks 9.90-9.95 keeps the two editable InputField members at
		// these offsets.  The 9.95 private symbols describe a 0x190-byte
		// StewMenu with subSettingInput at 0xE8 and searchBar at 0x118.
		// Accessing the two validated locations is both faster and safer than
		// probing arbitrary pointer-aligned words beyond the live object.
		constexpr UInt32 kStewMenuSubsettingInputOffset = 0xE8;
		constexpr UInt32 kStewMenuSearchInputOffset = 0x118;

		struct StewieShadowState
		{
			StewieInputTarget target;
			std::string text;
			size_t caret = 0;
			std::string appliedText;
			size_t appliedCaret = 0;
			bool initialized = false;
		};

		struct MenuSearchReplayStats
		{
			UInt32 commits = 0;
			UInt32 predecessorCalls = 0;
			UInt32 legacyEquivalentCalls = 0;
		};

		struct PendingStewieAscii
		{
			StewieInputTarget target;
			UInt8 input = 0;
			DWORD tick = 0;
		};

		enum class StewieAsciiSource : UInt8
		{
			Adapter,
			WndProc,
		};

		struct DeferredStewieAscii
		{
			UInt32 token = 0;
			StewieInputTarget target;
			UInt8 input = 0;
			bool adapterSeen = false;
			bool wndProcSeen = false;
		};

		struct StewieSpaceCommitState
		{
			DWORD tick = 0;
			StewieInputTarget target;
			UInt32 menuID = 0;
			std::string text;
			size_t caret = 0;
		};

		using StewieKeyboardHandler = bool(__thiscall*)(Menu*, UInt32);

		bool s_stewieChecked = false;
		bool s_stewieAvailable = false;
		bool s_stewieReplay = false;
		SIZE_T s_stewMenuPredecessorInputHandler = 0;
		SIZE_T s_stewMenuInputVtableEntry = 0;
		SIZE_T s_stewMenuObservedSuccessorHandler = 0;
		thread_local UInt32 s_stewiePredecessorCallDepth = 0;
		UInt32 s_tileTraitIsActive = 0;
		UInt32 s_tileTraitIsSearchActive = 0;
		UInt32 s_tileTraitCaretIndex = 0;
		StewieShadowState s_stewieShadow;
		std::vector<PendingStewieAscii> s_pendingStewieAdapterAscii;
		std::vector<PendingStewieAscii> s_pendingStewieWndProcAscii;
		std::vector<DeferredStewieAscii> s_deferredStewieAscii;
		UInt32 s_nextDeferredStewieAsciiToken = 1;
		DWORD s_lastDeferredStewieAsciiCommitTick = 0;
		UInt8 s_lastDeferredStewieAsciiCommitChar = 0;
		StewieSpaceCommitState s_lastStewieSpaceCommit;
		MenuSearchReplayStats s_menuSearchReplayStats;
		DWORD s_lastStewTargetPollTick = 0;
		StewieInputTarget s_observedStewTarget;

		class StewieTweaksInputAdapter
		{
		public:
			static bool __fastcall StewMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput);
		};

		UInt32 TileID(Tile* tile)
		{
			if (!tile)
				return 0;

			return static_cast<UInt32>(tile->GetValueFloat(Tile::kTileValue_id));
		}

		constexpr UInt32 CanonicalTileTraitID(UInt32 id)
		{
			// Gamebryo stores numeric Tile traits as IEEE-754 single-precision
			// floats. XML integer IDs above 2^24 therefore have to be rounded to
			// the same float value before comparing them with a live Tile.
			return static_cast<UInt32>(static_cast<float>(id));
		}
		static_assert(
			CanonicalTileTraitID(87698483) == 87698480,
			"MenuSearch XML id must match the live float-backed Tile trait");

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

		Tile* FindStewMenuSearchTile(Tile* root)
		{
			if (!root)
				return nullptr;

			// The search field has a stable named path in Stewie Tweaks 9.90+.
			// Following four direct child links avoids recursively walking the
			// live Tile tree while StewMenu is performing mouse hit-testing.
			return root->GetTileByPath(
				std::string("STW_MainRect/SearchParent/InputClipper/STW_SearchBar"));
		}

		Tile* FindTileByCanonicalID(Tile* tile, UInt32 canonicalID)
		{
			if (!tile)
				return nullptr;

			if (TileID(tile) == canonicalID)
				return tile;

			for (Tile* child : tile->GetChildren())
			{
				if (Tile* result = FindTileByCanonicalID(child, canonicalID))
					return result;
			}

			return nullptr;
		}

		Tile* FindTileByID(Tile* tile, UInt32 id)
		{
			return FindTileByCanonicalID(tile, CanonicalTileTraitID(id));
		}

		bool UsesUTF8StewieEncoding(const StewieInputTarget& target)
		{
			return target.kind != StewieInputKind::MenuSearch;
		}

		size_t ClampStewieBoundary(const StewieInputTarget& target, const std::string& text, size_t offset)
		{
			return UsesUTF8StewieEncoding(target)
				? ClampToPrevUTF8Boundary(text, offset)
				: ClampToPrevBoundary(text, offset);
		}

		size_t PreviousStewieBoundary(const StewieInputTarget& target, const std::string& text, size_t offset)
		{
			return UsesUTF8StewieEncoding(target)
				? PrevUTF8CharBoundary(text, offset)
				: PrevCharBoundary(text, offset);
		}

		size_t NextStewieBoundary(const StewieInputTarget& target, const std::string& text, size_t offset)
		{
			return UsesUTF8StewieEncoding(target)
				? NextUTF8CharBoundary(text, offset)
				: NextCharBoundary(text, offset);
		}

		std::string TileStringWithoutCaret(const StewieInputTarget& target, size_t& caret)
		{
			caret = 0;
			Tile* tile = target.tile;
			std::string text = tile ? tile->GetValueString(Tile::kTileValue_string) : "";
			const size_t caretMarker = text.find('|');
			if (caretMarker != std::string::npos)
			{
				text.erase(caretMarker, 1);
				caret = ClampStewieBoundary(target, text, caretMarker);
				return text;
			}

			if (target.inputField
				&& s_tileTraitCaretIndex
				&& TileTraitFloat(tile, s_tileTraitIsActive) > 0.5f)
			{
				const size_t caretIndex = static_cast<size_t>(TileTraitFloat(tile, s_tileTraitCaretIndex));
				if (caretIndex < text.size())
				{
					text.erase(caretIndex, 1);
					caret = ClampStewieBoundary(target, text, caretIndex);
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

		bool IsStewMenuTextTarget(const StewieInputTarget& target)
		{
			return target.valid
				&& (target.kind == StewieInputKind::StewMenuSearch
					|| target.kind == StewieInputKind::StewMenuStringSubsetting);
		}

		void DeactivateObservedStewMenuTarget(const char* reason)
		{
			const StewieInputTarget observedTarget = s_observedStewTarget;
			const StewieInputTarget oldTarget = IsStewMenuTextTarget(observedTarget)
				? observedTarget
				: s_stewieShadow.target;
			const bool hadActiveTarget = IsStewMenuTextTarget(observedTarget)
				|| (s_stewieShadow.initialized
					&& IsStewMenuTextTarget(s_stewieShadow.target));

			s_lastStewTargetPollTick = 0;
			s_observedStewTarget = {};
			if (!hadActiveTarget)
				return;

			ClearStewieInputState();
			EndStewieTextInputSession(reason);
			DebugLog(
				"tnvse_multibyte_input_event: source=MainLoop action=stewmenu_deactivate reason=%s kind=%u menu=0x%08X tile=0x%08X",
				reason ? reason : "unknown",
				static_cast<UInt32>(oldTarget.kind),
				reinterpret_cast<UInt32>(oldTarget.menu),
				reinterpret_cast<UInt32>(oldTarget.tile));
		}

		bool IsStewieTweaksAvailable()
		{
			if (!g_bMultibyteInputStewieTweaks || !g_cmdTableInterface
				|| !g_cmdTableInterface->GetPluginInfoByName)
				return false;

			if (s_stewieChecked)
				return s_stewieAvailable;

			const PluginInfo* info = g_cmdTableInterface->GetPluginInfoByName(
				dependencies::kStewieTweaksPluginName);
			if (!info)
				return false;

			s_stewieChecked = true;
			s_stewieAvailable = dependencies::IsPluginInfoValid(info)
				&& info->version >= dependencies::kStewieTweaksMinVersion;
			if (info && !s_stewieAvailable)
			{
				gLog.FormattedMessage(
					"tnvse_multibyte_input: Stewie Tweaks version %u is older than supported minimum %u; Stewie input adapter disabled",
					info->version,
					dependencies::kStewieTweaksMinVersion);
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

		bool TryGetActiveStewieInputFieldAtOffset(
			Menu* menu,
			UInt32 offset,
			UInt32 id,
			Tile*& tile,
			UInt8& inputType)
		{
			if (!menu)
				return false;

			static constexpr UInt32 kInputFieldStringLengthOffset = 0x08;
			static constexpr UInt32 kInputFieldStringCapacityOffset = 0x0A;
			static constexpr UInt32 kInputFieldActiveOffset = 0x0C;
			static constexpr UInt32 kInputFieldCaretOffset = 0x0E;
			static constexpr UInt32 kInputFieldTypeOffset = 0x14;

			__try
			{
				auto* base = reinterpret_cast<UInt8*>(menu);
				Tile* candidate = *reinterpret_cast<Tile**>(base + offset);
				const bool isActive = *reinterpret_cast<bool*>(
					base + offset + kInputFieldActiveOffset);
				const UInt16 length = *reinterpret_cast<UInt16*>(
					base + offset + kInputFieldStringLengthOffset);
				const UInt16 capacity = *reinterpret_cast<UInt16*>(
					base + offset + kInputFieldStringCapacityOffset);
				const SInt16 caret = *reinterpret_cast<SInt16*>(
					base + offset + kInputFieldCaretOffset);
				const UInt8 candidateType = *reinterpret_cast<UInt8*>(
					base + offset + kInputFieldTypeOffset);
				if (candidate
					&& isActive
					&& candidateType <= 3
					&& capacity < 0x10000
					&& length <= capacity
					&& caret >= 0
					&& static_cast<UInt16>(caret) <= length
					&& TileID(candidate) == id)
				{
					tile = candidate;
					inputType = candidateType;
					return true;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}

			return false;
		}

		Tile* FindActiveStewieInputFieldTile(Menu* menu, UInt32 id, UInt8& inputType)
		{
			if (!menu)
				return nullptr;

			UInt32 offset = 0;
			switch (id)
			{
			case kStewMenu_SearchBar:
				offset = kStewMenuSearchInputOffset;
				break;
			case kStewMenu_SubsettingInputFieldText:
				offset = kStewMenuSubsettingInputOffset;
				break;
			default:
				return nullptr;
			}

			Tile* tile = nullptr;
			return TryGetActiveStewieInputFieldAtOffset(
				menu, offset, id, tile, inputType)
				? tile : nullptr;
		}

		StewieInputTarget FindStewMenuTarget(Menu* menu)
		{
			if (!menu || MenuID(menu) != kMenuType_StewMenu)
				return {};

			Tile* root = MenuRoot(menu);
			if (!root)
				return {};

			UInt8 searchInputType = 0xFF;
			if (Tile* searchInputTile = FindActiveStewieInputFieldTile(
				menu, kStewMenu_SearchBar, searchInputType);
				searchInputTile && searchInputType == 0)
				return MakeStewieTarget(StewieInputKind::StewMenuSearch, menu, searchInputTile, true);

			Tile* searchTile = FindStewMenuSearchTile(root);
			const float searchRootActive = TileTraitFloat(root, s_tileTraitIsSearchActive);
			const float searchTileActive = TileTraitFloat(searchTile, s_tileTraitIsActive);
			if (searchTile
				&& (searchTileActive > 0.5f
					|| (searchRootActive > 0.5f && searchRootActive < 1.5f)))
				return MakeStewieTarget(StewieInputKind::StewMenuSearch, menu, searchTile, true);

			UInt8 inputType = 0xFF;
			Tile* subsettingTile = FindActiveStewieInputFieldTile(
				menu, kStewMenu_SubsettingInputFieldText, inputType);
			if (subsettingTile && inputType == 0)
				return MakeStewieTarget(StewieInputKind::StewMenuStringSubsetting, menu, subsettingTile, true);

			return {};
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

			return GetActiveStewieMenuSearchTarget();
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
				break;

			default:
				break;
			}

			// MenuSearch can briefly stop reporting its tracked tile as active while
			// the shell owns the Win+Space language picker. Keep the last exact target
			// for that chord instead of clearing the shadow needed to reject/roll back
			// Stewie's independently polled Space input.
			if (s_stewieShadow.target.kind == StewieInputKind::MenuSearch
				&& ShouldSuppressInputLanguageSwitchAscii(' ')
				&& s_stewieShadow.target.menu
				&& GetOpenMenu(MenuID(s_stewieShadow.target.menu)) == s_stewieShadow.target.menu)
			{
				return s_stewieShadow.target;
			}

			ClearStewieInputState();
			HideCandidateOverlay();
			return {};
		}

		SIZE_T PredecessorStewieHandlerForMenu(Menu* menu)
		{
			if (!menu)
				return 0;

			if (MenuID(menu) == kMenuType_StewMenu)
				return s_stewMenuPredecessorInputHandler;

			return GetStewieMenuSearchPredecessorInputHandler(menu);
		}

		bool CallStewiePredecessorInput(Menu* menu, UInt32 input)
		{
			const SIZE_T predecessorHandler =
				PredecessorStewieHandlerForMenu(menu);
			const SIZE_T stewMenuAdapterHandler = reinterpret_cast<SIZE_T>(
				&StewieTweaksInputAdapter::StewMenuKeyboardInput);
			if (!predecessorHandler
				|| predecessorHandler == stewMenuAdapterHandler
				|| s_stewiePredecessorCallDepth
				|| !hook_identity::IsExecutableTarget(predecessorHandler))
				return false;

			struct PredecessorCallGuard
			{
				PredecessorCallGuard()
				{
					++s_stewiePredecessorCallDepth;
				}
				~PredecessorCallGuard()
				{
					--s_stewiePredecessorCallDepth;
				}
			} guard;
			return reinterpret_cast<StewieKeyboardHandler>(
				predecessorHandler)(menu, input);
		}

		void EnsureStewieShadow(const StewieInputTarget& target)
		{
			if (SameStewieTarget(s_stewieShadow.target, target))
				return;

			s_stewieShadow.target = target;
			s_stewieShadow.text = TileStringWithoutCaret(target, s_stewieShadow.caret);
			if (!target.inputField && s_stewieShadow.text == "_")
				s_stewieShadow.text.clear();
			s_stewieShadow.caret = ClampStewieBoundary(target, s_stewieShadow.text, s_stewieShadow.caret);
			s_stewieShadow.appliedText = s_stewieShadow.text;
			s_stewieShadow.appliedCaret = s_stewieShadow.caret;
			s_stewieShadow.initialized = target.valid;
		}

		bool ReplayStewieShadow(const StewieInputTarget& target)
		{
			if (!target.valid || !s_stewieShadow.initialized)
				return false;

			const size_t clearCount = std::min<size_t>(
				s_stewieShadow.appliedText.size(),
				kStewieMaxShadowBytes);

			s_stewieReplay = true;
			CallStewiePredecessorInput(target.menu, kInputCode_End);
			for (size_t i = 0; i < clearCount; ++i)
				CallStewiePredecessorInput(target.menu, kInputCode_Backspace);

			for (unsigned char ch : s_stewieShadow.text)
				CallStewiePredecessorInput(target.menu, ch);

			const size_t caret = ClampStewieBoundary(target, s_stewieShadow.text, s_stewieShadow.caret);
			for (size_t i = caret; i < s_stewieShadow.text.size(); ++i)
				CallStewiePredecessorInput(target.menu, kInputCode_ArrowLeft);
			s_stewieReplay = false;

			s_stewieShadow.appliedText = s_stewieShadow.text;
			s_stewieShadow.appliedCaret = caret;
			return true;
		}

		bool IsInsertedAtCaret(
			std::string_view before,
			size_t beforeCaret,
			std::string_view after,
			size_t afterCaret,
			size_t& insertedBytes)
		{
			insertedBytes = 0;
			if (beforeCaret > before.size()
				|| afterCaret < beforeCaret
				|| afterCaret > after.size())
			{
				return false;
			}

			insertedBytes = afterCaret - beforeCaret;
			return after.size() == before.size() + insertedBytes
				&& before.compare(0, beforeCaret, after, 0, beforeCaret) == 0
				&& before.compare(
					beforeCaret,
					std::string_view::npos,
					after,
					afterCaret,
					std::string_view::npos) == 0;
		}

		bool IsRemovedBeforeCaret(
			std::string_view before,
			size_t beforeCaret,
			std::string_view after,
			size_t afterCaret,
			size_t& removedBytes)
		{
			removedBytes = 0;
			if (afterCaret > beforeCaret
				|| beforeCaret > before.size()
				|| afterCaret > after.size())
			{
				return false;
			}

			removedBytes = beforeCaret - afterCaret;
			return before.size() == after.size() + removedBytes
				&& before.compare(0, afterCaret, after, 0, afterCaret) == 0
				&& before.compare(
					beforeCaret,
					std::string_view::npos,
					after,
					afterCaret,
					std::string_view::npos) == 0;
		}

		bool IsRemovedAfterCaret(
			std::string_view before,
			size_t beforeCaret,
			std::string_view after,
			size_t afterCaret,
			size_t& removedBytes)
		{
			removedBytes = 0;
			if (afterCaret != beforeCaret
				|| beforeCaret > before.size()
				|| afterCaret > after.size()
				|| before.size() < after.size())
			{
				return false;
			}

			removedBytes = before.size() - after.size();
			return before.compare(0, beforeCaret, after, 0, afterCaret) == 0
				&& before.compare(
					beforeCaret + removedBytes,
					std::string_view::npos,
					after,
					afterCaret,
					std::string_view::npos) == 0;
		}

		size_t CommonStewiePrefix(
			const StewieInputTarget& target,
			std::string_view lhs,
			std::string_view rhs)
		{
			size_t prefix = 0;
			const size_t limit = std::min(lhs.size(), rhs.size());
			while (prefix < limit && lhs[prefix] == rhs[prefix])
				++prefix;

			const std::string lhsCopy(lhs);
			const std::string rhsCopy(rhs);
			return std::min(
				ClampStewieBoundary(target, lhsCopy, prefix),
				ClampStewieBoundary(target, rhsCopy, prefix));
		}

		bool ApplyMenuSearchShadowDelta(const StewieInputTarget& target)
		{
			const std::string& before = s_stewieShadow.appliedText;
			const std::string& after = s_stewieShadow.text;
			const size_t beforeCaret = ClampStewieBoundary(
				target,
				before,
				s_stewieShadow.appliedCaret);
			size_t afterCaret = ClampStewieBoundary(
				target,
				after,
				s_stewieShadow.caret);

			// Stewie's lightweight SearchBar variants only support append and
			// Backspace. Their caret is therefore always the byte-string end.
			if (!target.inputField)
				afterCaret = after.size();

			const UInt32 legacyCalls = g_bMultibyteInputLog
				? static_cast<UInt32>(
					1
						+ before.size()
						+ after.size()
						+ (after.size() - afterCaret))
				: 0;
			UInt32 issuedCalls = 0;
			auto issue = [&](UInt32 input)
			{
				if (g_bMultibyteInputLog)
					++issuedCalls;
				CallStewiePredecessorInput(target.menu, input);
			};

			s_stewieReplay = true;
			size_t changedBytes = 0;
			if (before == after)
			{
				if (beforeCaret != afterCaret)
				{
					if (afterCaret == 0)
						issue(kInputCode_Home);
					else if (afterCaret == after.size())
						issue(kInputCode_End);
					else if (afterCaret < beforeCaret)
					{
						for (size_t i = afterCaret; i < beforeCaret; ++i)
							issue(kInputCode_ArrowLeft);
					}
					else
					{
						for (size_t i = beforeCaret; i < afterCaret; ++i)
							issue(kInputCode_ArrowRight);
					}
				}
			}
			else if (IsInsertedAtCaret(
				before,
				beforeCaret,
				after,
				afterCaret,
				changedBytes))
			{
				for (size_t i = beforeCaret; i < afterCaret; ++i)
					issue(static_cast<UInt8>(after[i]));
			}
			else if (IsRemovedBeforeCaret(
				before,
				beforeCaret,
				after,
				afterCaret,
				changedBytes))
			{
				for (size_t i = 0; i < changedBytes; ++i)
					issue(kInputCode_Backspace);
			}
			else if (IsRemovedAfterCaret(
				before,
				beforeCaret,
				after,
				afterCaret,
				changedBytes))
			{
				for (size_t i = 0; i < changedBytes; ++i)
					issue(kInputCode_Delete);
			}
			else
			{
				if (beforeCaret != before.size())
					issue(kInputCode_End);

				const size_t prefix = CommonStewiePrefix(target, before, after);
				for (size_t i = prefix; i < before.size(); ++i)
					issue(kInputCode_Backspace);
				for (size_t i = prefix; i < after.size(); ++i)
					issue(static_cast<UInt8>(after[i]));
				for (size_t i = afterCaret; i < after.size(); ++i)
					issue(kInputCode_ArrowLeft);
			}
			s_stewieReplay = false;

			s_stewieShadow.caret = afterCaret;
			s_stewieShadow.appliedText = after;
			s_stewieShadow.appliedCaret = afterCaret;
			if (g_bMultibyteInputLog)
			{
				++s_menuSearchReplayStats.commits;
				s_menuSearchReplayStats.predecessorCalls += issuedCalls;
				s_menuSearchReplayStats.legacyEquivalentCalls += legacyCalls;
			}
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
			s_stewieShadow.caret = ClampStewieBoundary(target, s_stewieShadow.text, caret);
			s_stewieShadow.initialized = true;
			return target.kind == StewieInputKind::MenuSearch
				? ApplyMenuSearchShadowDelta(target)
				: ReplayStewieShadow(target);
		}

		bool InsertTextAtCaretStewie(const StewieInputTarget& target, std::string_view text)
		{
			if (!target.valid || text.empty())
				return false;

			EnsureStewieShadow(target);
			std::string candidate = s_stewieShadow.text;
			size_t caret = ClampStewieBoundary(target, candidate, s_stewieShadow.caret);
			candidate.insert(caret, text.data(), text.size());
			return CommitStewieShadow(target, std::move(candidate), caret + text.size());
		}

		bool InsertWideTextStewie(const StewieInputTarget& target, std::wstring_view text)
		{
			std::string converted = UsesUTF8StewieEncoding(target)
				? WideToUTF8(text)
				: WideToCurrentCodePage(text);
			if (converted.empty())
				return false;

			return InsertTextAtCaretStewie(target, converted);
		}

		bool ConsumePendingStewieAscii(
			std::vector<PendingStewieAscii>& pending,
			const StewieInputTarget& target,
			UInt8 input,
			DWORD now,
			UInt8& matchedInput)
		{
			pending.erase(
				std::remove_if(
					pending.begin(),
					pending.end(),
					[now](const PendingStewieAscii& value)
					{
						return now - value.tick > kDuplicateAsciiSuppressMs;
					}),
				pending.end());

			const auto match = std::find_if(
				pending.begin(),
				pending.end(),
				[&target, input](const PendingStewieAscii& value)
				{
					return SameStewieTarget(value.target, target)
						&& AsciiEqualsIgnoreCase(value.input, input);
				});
			if (match == pending.end())
				return false;

			matchedInput = match->input;
			pending.erase(match);
			return true;
		}

		void RecordPendingStewieAscii(
			std::vector<PendingStewieAscii>& pending,
			const StewieInputTarget& target,
			UInt8 input,
			DWORD now)
		{
			constexpr size_t kMaxPendingAscii = 16;
			if (pending.size() >= kMaxPendingAscii)
				pending.erase(pending.begin());
			pending.push_back({ target, input, now });
		}

		void RecordStewieSpaceCommit(const StewieInputTarget& target, UInt8 input, DWORD now)
		{
			if (input != ' ')
				return;

			s_lastStewieSpaceCommit = {};
			if (!target.valid
				|| !s_stewieShadow.initialized
				|| !SameStewieTarget(target, s_stewieShadow.target))
			{
				return;
			}

			s_lastStewieSpaceCommit.tick = now;
			s_lastStewieSpaceCommit.target = target;
			s_lastStewieSpaceCommit.menuID = MenuID(target.menu);
			s_lastStewieSpaceCommit.text = s_stewieShadow.text;
			s_lastStewieSpaceCommit.caret = s_stewieShadow.caret;
		}

		bool DeferredSourceSeen(const DeferredStewieAscii& pending, StewieAsciiSource source)
		{
			return source == StewieAsciiSource::Adapter
				? pending.adapterSeen
				: pending.wndProcSeen;
		}

		void MarkDeferredSourceSeen(DeferredStewieAscii& pending, StewieAsciiSource source)
		{
			if (source == StewieAsciiSource::Adapter)
				pending.adapterSeen = true;
			else
				pending.wndProcSeen = true;
		}

		bool ShouldDeferStewieAscii(const StewieInputTarget& target)
		{
			// StewMenu delivers printable input through its menu adapter before some
			// TSF IMEs publish WM_IME_STARTCOMPOSITION, just like the external
			// MenuSearch hooks. Defer one window-message turn for every writable
			// Stewie string target so the composition-start message can cancel the
			// pending ASCII instead of leaving its first phonetic letter in the field.
			return target.valid
				&& target.kind != StewieInputKind::None
				&& s_window
				&& IsConfiguredImeLayout(s_window)
				&& !IsImeCompositionActive();
		}

		bool QueueDeferredStewieAscii(
			const StewieInputTarget& target,
			UInt8 input,
			StewieAsciiSource source)
		{
			for (DeferredStewieAscii& pending : s_deferredStewieAscii)
			{
				if (!AsciiEqualsIgnoreCase(pending.input, input)
					|| !SameStewieTarget(pending.target, target)
					|| DeferredSourceSeen(pending, source))
				{
					continue;
				}

				// WM_CHAR is the authoritative character-producing path for Caps Lock
				// and Shift. The game adapter can report the same letter normalized to
				// lowercase, so keep the WndProc spelling when both paths rendezvous.
				if (source == StewieAsciiSource::WndProc)
					pending.input = input;
				MarkDeferredSourceSeen(pending, source);
				DebugLog(
					"tnvse_multibyte_input_event: source=StewieAscii action=merge_deferred_ascii input=0x%02X resolved=0x%02X token=%u source=%s",
					static_cast<UInt32>(input),
					static_cast<UInt32>(pending.input),
					pending.token,
					source == StewieAsciiSource::Adapter ? "adapter" : "wndproc");
				return true;
			}

			DeferredStewieAscii pending;
			pending.token = s_nextDeferredStewieAsciiToken++;
			if (!pending.token)
				pending.token = s_nextDeferredStewieAsciiToken++;
			pending.target = target;
			pending.input = input;
			MarkDeferredSourceSeen(pending, source);
			s_deferredStewieAscii.push_back(pending);

			if (PostMessageA(s_window, kMessage_FlushDeferredStewieAscii, pending.token, 0))
			{
				DebugLog(
					"tnvse_multibyte_input_event: source=StewieAscii action=defer_ascii input=0x%08X token=%u source=%s",
					static_cast<UInt32>(input),
					pending.token,
					source == StewieAsciiSource::Adapter ? "adapter" : "wndproc");
				return true;
			}

			s_deferredStewieAscii.pop_back();
			const char ch = static_cast<char>(input);
			const bool inserted = InsertTextAtCaretStewie(target, std::string_view(&ch, 1));
			if (inserted)
			{
				std::vector<PendingStewieAscii>& own = source == StewieAsciiSource::Adapter
					? s_pendingStewieAdapterAscii
					: s_pendingStewieWndProcAscii;
				RecordPendingStewieAscii(own, target, input, GetTickCount());
				s_lastDeferredStewieAsciiCommitTick = GetTickCount();
				s_lastDeferredStewieAsciiCommitChar = input;
				RecordStewieSpaceCommit(target, input, s_lastDeferredStewieAsciiCommitTick);
			}
			return inserted;
		}

		bool HandleStewieAscii(
			const StewieInputTarget& target,
			UInt8 input,
			StewieAsciiSource source)
		{
			if (!target.valid || input < 0x20 || input > 0x7E)
				return false;
			if (ShouldSuppressInputLanguageSwitchAscii(input))
				return true;
			const UInt8 resolvedInput = source == StewieAsciiSource::Adapter
				? ResolveAsciiLetterCaseFromKeyboard(input)
				: input;

			const DWORD now = GetTickCount();
			std::vector<PendingStewieAscii>& opposite = source == StewieAsciiSource::Adapter
				? s_pendingStewieWndProcAscii
				: s_pendingStewieAdapterAscii;
			UInt8 matchedInput = 0;
			if (ConsumePendingStewieAscii(opposite, target, resolvedInput, now, matchedInput))
			{
				DebugLog(
					"tnvse_multibyte_input_event: source=StewieAscii action=suppress_cross_channel_duplicate input=0x%02X matched=0x%02X source=%s",
					static_cast<UInt32>(resolvedInput),
					static_cast<UInt32>(matchedInput),
					source == StewieAsciiSource::Adapter ? "adapter" : "wndproc");
				return true;
			}

			if (ShouldDeferStewieAscii(target))
				return QueueDeferredStewieAscii(target, resolvedInput, source);

			const char ch = static_cast<char>(resolvedInput);
			if (!InsertTextAtCaretStewie(target, std::string_view(&ch, 1)))
				return false;

			std::vector<PendingStewieAscii>& own = source == StewieAsciiSource::Adapter
				? s_pendingStewieAdapterAscii
				: s_pendingStewieWndProcAscii;
			RecordPendingStewieAscii(own, target, resolvedInput, now);
			RecordStewieSpaceCommit(target, resolvedInput, now);
			return true;
		}

		bool HandleStewieWndProcAscii(const StewieInputTarget& target, UInt8 input)
		{
			return HandleStewieAscii(target, input, StewieAsciiSource::WndProc);
		}

		bool FlushDeferredStewieAscii(UInt32 token)
		{
			const auto match = std::find_if(
				s_deferredStewieAscii.begin(),
				s_deferredStewieAscii.end(),
				[token](const DeferredStewieAscii& pending)
				{
					return pending.token == token;
				});
			if (match == s_deferredStewieAscii.end())
				return false;

			const DeferredStewieAscii pending = *match;
			s_deferredStewieAscii.erase(match);
			if ((pending.adapterSeen && ShouldSuppressImeCommitInput(
					pending.input,
					ImeCommitInputChannel::Stewie))
				|| (pending.wndProcSeen && ShouldSuppressImeCommitInput(
					pending.input,
					ImeCommitInputChannel::WndProcChar)))
			{
				return true;
			}
			if (IsImeCompositionActive())
				return true;

			const StewieInputTarget target = GetOverlayStewieInputTarget();
			if (!target.valid || !SameStewieTarget(target, pending.target))
				return true;

			const char ch = static_cast<char>(pending.input);
			if (!InsertTextAtCaretStewie(target, std::string_view(&ch, 1)))
				return true;

			s_lastDeferredStewieAsciiCommitTick = GetTickCount();
			s_lastDeferredStewieAsciiCommitChar = pending.input;
			RecordStewieSpaceCommit(target, pending.input, s_lastDeferredStewieAsciiCommitTick);
			if (pending.adapterSeen != pending.wndProcSeen)
			{
				std::vector<PendingStewieAscii>& own = pending.adapterSeen
					? s_pendingStewieAdapterAscii
					: s_pendingStewieWndProcAscii;
				RecordPendingStewieAscii(own, target, pending.input, s_lastDeferredStewieAsciiCommitTick);
			}
			DebugLog(
				"tnvse_multibyte_input_event: source=WndProc action=flush_deferred_stewie_ascii input=0x%08X token=%u",
				static_cast<UInt32>(pending.input),
				token);
			return true;
		}

		void CancelDeferredStewieAscii()
		{
			if (s_deferredStewieAscii.empty())
				return;

			DebugLog(
				"tnvse_multibyte_input_event: source=IMEComposition action=cancel_deferred_stewie_ascii count=%u",
				static_cast<UInt32>(s_deferredStewieAscii.size()));
			s_deferredStewieAscii.clear();
		}

		void SuppressStewieInputLanguageSwitchSpace()
		{
			CancelDeferredStewieAscii();
			s_pendingStewieAdapterAscii.clear();
			s_pendingStewieWndProcAscii.clear();

			// Match the WndProc guard that is restarted when Win+Space is released.
			// Stewie polls keyboard input and can observe the hotkey Space after the
			// shell's fullscreen language-switch UI has finished.
			constexpr DWORD kLanguageSwitchRollbackMs = 500;
			const StewieSpaceCommitState commit = s_lastStewieSpaceCommit;
			s_lastStewieSpaceCommit = {};
			const StewieInputTarget target = commit.target;
			if (!commit.tick
				|| GetTickCount() - commit.tick > kLanguageSwitchRollbackMs
				|| !target.valid
				|| !commit.menuID
				|| GetOpenMenu(commit.menuID) != target.menu)
			{
				return;
			}

			// A valid active target that differs from the recorded one means focus
			// really moved; never remove content from the previous field. An empty
			// active probe is allowed because MenuSearch traits can be transient during
			// the shell language picker, while the saved menu/tile pair remains exact.
			const StewieInputTarget activeTarget = GetActiveStewieInputTarget();
			if (activeTarget.valid && !SameStewieTarget(activeTarget, target))
				return;

			// MenuSearch can clear the shared shadow when its visibility traits flicker
			// under the shell language picker. Re-read the exact tile and only restore
			// the saved shadow when its text and caret still match the committed Space.
			size_t currentCaret = 0;
			std::string currentText = TileStringWithoutCaret(target, currentCaret);
			if (!target.inputField && currentText == "_")
				currentText.clear();
			currentCaret = ClampStewieBoundary(target, currentText, currentCaret);
			if (currentText != commit.text || currentCaret != commit.caret)
				return;

			s_stewieShadow.target = target;
			s_stewieShadow.text = currentText;
			s_stewieShadow.caret = currentCaret;
			s_stewieShadow.appliedText = currentText;
			s_stewieShadow.appliedCaret = currentCaret;
			s_stewieShadow.initialized = true;

			const size_t caret = ClampStewieBoundary(target, s_stewieShadow.text, currentCaret);
			if (caret)
			{
				const size_t previous = PreviousStewieBoundary(target, s_stewieShadow.text, caret);
				if (caret - previous == 1 && s_stewieShadow.text[previous] == ' ')
				{
					std::string candidate = s_stewieShadow.text;
					candidate.erase(previous, 1);
					CommitStewieShadow(target, std::move(candidate), previous);
					DebugLog("tnvse_multibyte_input_event: source=WinSpace action=rollback_search_space");
				}
			}
		}

		bool DeletePreviousStewieChar(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			size_t caret = ClampStewieBoundary(target, s_stewieShadow.text, s_stewieShadow.caret);
			if (!caret)
				return true;

			const size_t previous = PreviousStewieBoundary(target, s_stewieShadow.text, caret);
			std::string candidate = s_stewieShadow.text;
			candidate.erase(previous, caret - previous);
			return CommitStewieShadow(target, std::move(candidate), previous);
		}

		bool DeleteNextStewieChar(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			size_t caret = ClampStewieBoundary(target, s_stewieShadow.text, s_stewieShadow.caret);
			if (caret >= s_stewieShadow.text.size())
				return true;

			const size_t next = NextStewieBoundary(target, s_stewieShadow.text, caret);
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
				PreviousStewieBoundary(target, s_stewieShadow.text, s_stewieShadow.caret));
		}

		bool MoveStewieCaretNext(const StewieInputTarget& target)
		{
			EnsureStewieShadow(target);
			return CommitStewieShadow(
				target,
				s_stewieShadow.text,
				NextStewieBoundary(target, s_stewieShadow.text, s_stewieShadow.caret));
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
			if (!s_lastDeferredStewieAsciiCommitChar
				|| GetTickCount() - s_lastDeferredStewieAsciiCommitTick > kDuplicateAsciiSuppressMs
				|| !AsciiEqualsIgnoreCase(s_lastDeferredStewieAsciiCommitChar, compositionLead))
			{
				return false;
			}
			s_lastDeferredStewieAsciiCommitChar = 0;

			StewieInputTarget target = GetOverlayStewieInputTarget();
			if (!target.valid)
				return false;

			EnsureStewieShadow(target);
			size_t caret = ClampStewieBoundary(target, s_stewieShadow.text, s_stewieShadow.caret);
			if (!caret)
				return false;

			const size_t previous = PreviousStewieBoundary(target, s_stewieShadow.text, caret);
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
			// MenuSearch state can briefly deactivate while Windows owns the language
			// picker. Keep its self-validating Space snapshot so the switch path can
			// still remove a Space already replayed into the search tile.
			const bool preserveMenuSearchSpaceCommit =
				s_lastStewieSpaceCommit.target.kind == StewieInputKind::MenuSearch;
			if (s_menuSearchReplayStats.commits)
			{
				const UInt32 savedCalls =
					s_menuSearchReplayStats.legacyEquivalentCalls
						> s_menuSearchReplayStats.predecessorCalls
					? s_menuSearchReplayStats.legacyEquivalentCalls
						- s_menuSearchReplayStats.predecessorCalls
					: 0;
				DebugLog(
					"tnvse_multibyte_input_performance: target=MenuSearch strategy=incremental commits=%u predecessor_calls=%u legacy_equivalent_calls=%u saved_calls=%u",
					s_menuSearchReplayStats.commits,
					s_menuSearchReplayStats.predecessorCalls,
					s_menuSearchReplayStats.legacyEquivalentCalls,
					savedCalls);
				s_menuSearchReplayStats = {};
			}
			s_stewieShadow = StewieShadowState();
			s_pendingStewieAdapterAscii.clear();
			s_pendingStewieWndProcAscii.clear();
			s_deferredStewieAscii.clear();
			s_lastDeferredStewieAsciiCommitTick = 0;
			s_lastDeferredStewieAsciiCommitChar = 0;
			if (!preserveMenuSearchSpaceCommit)
				s_lastStewieSpaceCommit = {};
		}

		bool HandleStewieInput(Menu* menu, UInt32 input)
		{
			if (s_stewieReplay)
				return CallStewiePredecessorInput(menu, input);

			// MenuSearch target traits can flicker while Win+Space is owned by the
			// shell. Reject the hotkey Space before target discovery so a transient
			// miss cannot fall through to Stewie's predecessor ASCII handler.
			if (input == ' ' && ShouldSuppressInputLanguageSwitchAscii(' '))
			{
				DebugLog(
					"tnvse_multibyte_input_event: source=StewieTweaksInputTarget action=suppress_language_switch_space_before_target menu=%u",
					MenuID(menu));
				return true;
			}

			StewieInputTarget target = MenuID(menu) == kMenuType_StewMenu
				? FindStewMenuTarget(menu)
				: FindStewieMenuSearchTarget(menu);
			if (!target.valid)
			{
				DebugLog(
					"tnvse_multibyte_input_event: source=StewieTweaksInputTarget action=target_miss menu=%u input=0x%08X",
					MenuID(menu),
					input);
				if (MenuID(menu) == kMenuType_StewMenu)
				{
					Tile* root = MenuRoot(menu);
					Tile* searchTile = FindStewMenuSearchTile(root);
					UInt8 searchInputType = 0xFF;
					Tile* searchInputTile = FindActiveStewieInputFieldTile(
						menu, kStewMenu_SearchBar, searchInputType);
					UInt8 subsettingInputType = 0xFF;
					Tile* subsettingTile = FindActiveStewieInputFieldTile(
						menu, kStewMenu_SubsettingInputFieldText, subsettingInputType);
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
				const bool handled = CallStewiePredecessorInput(menu, input);
				if (MenuID(menu) == kMenuType_StewMenu)
				{
					// StewMenu activates its search/InputField inside the predecessor
					// handler. The pre-call probe above therefore cannot see a target
					// for the key/controller action that opens it. Probe once after the
					// state change and start the IME session in the same input event.
					if (StewieInputTarget activatedTarget = FindStewMenuTarget(menu);
						activatedTarget.valid)
					{
						s_observedStewTarget = activatedTarget;
						EnsureStewieShadow(activatedTarget);
						RefreshTextInputSessionForActiveTarget("stewmenu_target_activated");
						DebugLog(
							"tnvse_multibyte_input_event: source=StewieTweaksInputTarget action=stewmenu_activate_immediate kind=%u",
							static_cast<UInt32>(activatedTarget.kind));
					}
				}
				return handled;
			}

			if (MenuID(menu) == kMenuType_StewMenu)
				s_observedStewTarget = target;
			EnsureStewieShadow(target);
			if (FilterGameInput(
					input,
					ImeCommitInputChannel::Stewie,
					GameInputFilterClass::None)
				== GameInputFilterResult::SuppressImeCommit)
			{
				DebugLog(
					"tnvse_multibyte_input_event: source=StewieTweaksInputTarget action=suppress_ime_commit_key menu=%u input=0x%08X",
					MenuID(menu),
					input);
				return true;
			}

			if (IsCtrlKeyDown())
			{
				const bool handled = CallStewiePredecessorInput(menu, input);
				ClearStewieInputState();
				return handled;
			}

			const GameInputFilterClass inputClass =
				input >= 0x20 && input <= 0x7E
					? GameInputFilterClass::AllDuringComposition
						| GameInputFilterClass::PrintableAscii
					: GameInputFilterClass::AllDuringComposition;
			const GameInputFilterResult filterResult = FilterGameInput(
				input,
				ImeCommitInputChannel::Stewie,
				inputClass);
			if (filterResult != GameInputFilterResult::Pass)
			{
				DebugLog(
					"tnvse_multibyte_input_event: source=StewieTweaksInputTarget action=%s menu=%u input=0x%08X",
					filterResult == GameInputFilterResult::SuppressCompositionAscii
						? "suppress_composition_ascii"
						: "suppress_composition_input",
					MenuID(menu),
					input);
				return true;
			}

			if (input >= 0x20 && input <= 0x7E)
			{
				return HandleStewieAscii(
					target,
					static_cast<UInt8>(input),
					StewieAsciiSource::Adapter);
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
				return CallStewiePredecessorInput(menu, input);
			default:
				return CallStewiePredecessorInput(menu, input);
			}
		}

		void TryInstallStewieTweaksInputHooks()
		{
			if (!IsStewieTweaksAvailable())
				return;

			TryInstallStewieMenuSearchAdapterSites();

			if (!s_tileTraitIsActive)
				s_tileTraitIsActive = Tile::TraitNameToID("_IsActive");
			if (!s_tileTraitIsSearchActive)
				s_tileTraitIsSearchActive = Tile::TraitNameToID("_IsSearchActive");
			if (!s_tileTraitCaretIndex)
				s_tileTraitCaretIndex = Tile::TraitNameToID("_CaretIndex");

			if (Menu* stewMenu = GetOpenMenu(kMenuType_StewMenu))
			{
				const SIZE_T menuAddress = reinterpret_cast<SIZE_T>(stewMenu);
				if (!hook_identity::IsAccessibleRegion(
						menuAddress, sizeof(SIZE_T), false))
				{
					return;
				}
				const SIZE_T menuVtable =
					*reinterpret_cast<const SIZE_T*>(menuAddress);
				if (menuVtable > std::numeric_limits<SIZE_T>::max()
						- kMenuHandleKeyboardInputVTableOffset)
				{
					return;
				}
				const SIZE_T keyboardInputVtableEntry =
					menuVtable + kMenuHandleKeyboardInputVTableOffset;
				if (!hook_identity::IsAccessibleRegion(
						keyboardInputVtableEntry, sizeof(SIZE_T), false))
				{
					return;
				}
				const SIZE_T currentHandler =
					*reinterpret_cast<const SIZE_T*>(keyboardInputVtableEntry);
				const SIZE_T adapterHandler = reinterpret_cast<SIZE_T>(
					&StewieTweaksInputAdapter::StewMenuKeyboardInput);
				if (!s_stewMenuInputVtableEntry)
				{
					if (currentHandler == adapterHandler)
					{
						gLog.FormattedMessage(
							"tnvse_multibyte_input: StewMenu adapter is present but its predecessor is unavailable");
						return;
					}
					if (!hook_identity::IsExecutableTarget(currentHandler)
						|| !hook_identity::IsExecutableTarget(adapterHandler))
					{
						return;
					}
					s_stewMenuPredecessorInputHandler = currentHandler;
					s_stewMenuInputVtableEntry = keyboardInputVtableEntry;
					// Stewie Tweaks StewMenu::HandleKeyboardInput vtable slot
					// (__thiscall target via __fastcall shim).
					SafeWrite32(keyboardInputVtableEntry, adapterHandler);
					const SIZE_T observedHandler =
						*reinterpret_cast<const SIZE_T*>(keyboardInputVtableEntry);
					if (observedHandler == adapterHandler)
					{
						DebugLog(
							"tnvse_multibyte_input: chained StewMenu handler=0x%08X",
							static_cast<UInt32>(currentHandler));
						gLog.FormattedMessage("tnvse_multibyte_input: Stewie Tweaks StewMenu input adapter installed");
						return;
					}

					if (observedHandler == currentHandler)
					{
						s_stewMenuPredecessorInputHandler = 0;
						s_stewMenuInputVtableEntry = 0;
						gLog.FormattedMessage(
							"tnvse_multibyte_input: StewMenu adapter write did not publish predecessor=0x%08X",
							static_cast<UInt32>(currentHandler));
						return;
					}

					// A later owner may already chain through this adapter. Retain
					// its predecessor instead of overwriting the new top-level slot.
					s_stewMenuObservedSuccessorHandler = observedHandler;
					gLog.FormattedMessage(
						"tnvse_multibyte_input: StewMenu adapter retained below observed handler=0x%08X predecessor=0x%08X executable=%u",
						static_cast<UInt32>(observedHandler),
						static_cast<UInt32>(currentHandler),
						hook_identity::IsExecutableTarget(observedHandler) ? 1u : 0u);
					return;
				}

				if (keyboardInputVtableEntry != s_stewMenuInputVtableEntry
					|| currentHandler == adapterHandler)
					return;
				if (currentHandler == s_stewMenuPredecessorInputHandler)
				{
					// The complete adapter chain was removed. Forget the stale
					// predecessor so a later active StewMenu can be hooked anew.
					s_stewMenuPredecessorInputHandler = 0;
					s_stewMenuInputVtableEntry = 0;
					s_stewMenuObservedSuccessorHandler = 0;
					return;
				}

				// A later plugin now owns the vtable slot.  It may already keep
				// this adapter as its predecessor, so writing ourselves back on
				// top can form tNVSE -> successor -> tNVSE and recurse forever.
				// Leave the later hook in place and retain our predecessor
				// predecessor for calls that still reach this adapter.
				if (currentHandler != s_stewMenuObservedSuccessorHandler)
				{
					s_stewMenuObservedSuccessorHandler = currentHandler;
					gLog.FormattedMessage(
						"tnvse_multibyte_input: StewMenu adapter left below later handler=0x%08X",
						static_cast<UInt32>(currentHandler));
				}
			}
		}

		void ProcessStewieTweaksInputTargetState()
		{
			if (!g_bMultibyteInputStewieTweaks)
			{
				DeactivateObservedStewMenuTarget("stewmenu_adapter_disabled");
				return;
			}

			// StewMenu can activate an InputField entirely through its game-menu
			// update path, without producing another Win32 input message. Install and
			// observe it from the main loop, but only while that menu actually exists.
			Menu* stewMenu = GetOpenMenu(kMenuType_StewMenu);
			if (!stewMenu)
			{
				DeactivateObservedStewMenuTarget("stewmenu_closed");
				return;
			}

			TryInstallStewieTweaksInputHooks();

			// A 50 ms edge detector is visually immediate and avoids walking the
			// Tweak XML tree every rendered frame while the settings list is open.
			constexpr DWORD kStewTargetPollIntervalMs = 50;
			const DWORD now = GetTickCount();
			if (s_lastStewTargetPollTick
				&& now - s_lastStewTargetPollTick < kStewTargetPollIntervalMs)
			{
				return;
			}
			s_lastStewTargetPollTick = now;

			const StewieInputTarget target = FindStewMenuTarget(stewMenu);
			if (!target.valid)
			{
				DeactivateObservedStewMenuTarget("stewmenu_target_inactive");
				return;
			}

			if (SameStewieTarget(target, s_observedStewTarget))
				return;

			s_observedStewTarget = target;
			EnsureStewieShadow(target);
			RefreshTextInputSessionForActiveTarget("stewmenu_target_poll_activate");
			DebugLog(
				"tnvse_multibyte_input_event: source=MainLoop action=stewmenu_activate_from_state kind=%u menu=0x%08X tile=0x%08X",
				static_cast<UInt32>(target.kind),
				reinterpret_cast<UInt32>(target.menu),
				reinterpret_cast<UInt32>(target.tile));
		}


		void ResetStewieInputState()
		{
			ClearStewieInputState();
			s_lastStewieSpaceCommit = {};
			ResetStewieMenuSearchState();
			s_stewieReplay = false;
			s_lastStewTargetPollTick = 0;
			s_observedStewTarget = {};
			s_stewMenuObservedSuccessorHandler = 0;
		}

		bool __fastcall StewieTweaksInputAdapter::StewMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
		{
			return HandleStewieInput(apMenu, aiInput);
		}

	}
}

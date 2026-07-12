#include "multibyte_input_internal.h"

// Shared Stewie Tweaks UTF-8 input engine and StewMenu integration.

namespace fonthook
{
	namespace multibyte_input
	{
		constexpr UInt32 kStewieTweaksMinVersion = 990;
		constexpr const char* kStewieTweaksPluginName = "lStewieAl's Tweaks";
		constexpr UInt32 kMenuType_StewMenu = 1069;
		constexpr UInt32 kStewMenu_SearchBar = 5;
		constexpr UInt32 kStewMenu_SubsettingInputFieldText = 103;
		constexpr UInt32 kStewieMaxShadowBytes = 1023;
		constexpr UInt32 kMenuHandleKeyboardInputVTableOffset = 0x30;

		struct StewieShadowState
		{
			StewieInputTarget target;
			std::string text;
			size_t caret = 0;
			size_t appliedBytes = 0;
			bool initialized = false;
		};

		struct PendingStewieAscii
		{
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

		using StewieKeyboardHandler = bool(__thiscall*)(Menu*, UInt32);

		bool s_stewieChecked = false;
		bool s_stewieAvailable = false;
		bool s_stewieReplay = false;
		SIZE_T s_stewMenuOriginalInputHandler = 0;
		SIZE_T s_stewMenuHookedEntry = 0;
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
		DWORD s_lastStewieSpaceCommitTick = 0;
		StewieInputTarget s_lastStewieSpaceCommitTarget;

		class StewieTweaksInputTargetEx
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
				if (StewieInputTarget target = GetActiveStewieMenuSearchTarget(); target.valid)
					return target;
				break;

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

			if (MenuID(menu) == kMenuType_StewMenu)
				return s_stewMenuOriginalInputHandler;

			return GetStewieMenuSearchOriginalInputHandler(menu);
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
			s_stewieShadow.text = TileStringWithoutCaret(target, s_stewieShadow.caret);
			if (!target.inputField && s_stewieShadow.text == "_")
				s_stewieShadow.text.clear();
			s_stewieShadow.caret = ClampStewieBoundary(target, s_stewieShadow.text, s_stewieShadow.caret);
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

			const size_t caret = ClampStewieBoundary(target, s_stewieShadow.text, s_stewieShadow.caret);
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
			s_stewieShadow.caret = ClampStewieBoundary(target, s_stewieShadow.text, caret);
			s_stewieShadow.initialized = true;
			return ReplayStewieShadow(target);
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

		bool ConsumePendingStewieAscii(std::vector<PendingStewieAscii>& pending, UInt8 input, DWORD now)
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
				[input](const PendingStewieAscii& value)
				{
					return value.input == input;
				});
			if (match == pending.end())
				return false;

			pending.erase(match);
			return true;
		}

		void RecordPendingStewieAscii(std::vector<PendingStewieAscii>& pending, UInt8 input, DWORD now)
		{
			constexpr size_t kMaxPendingAscii = 16;
			if (pending.size() >= kMaxPendingAscii)
				pending.erase(pending.begin());
			pending.push_back({ input, now });
		}

		void RecordStewieSpaceCommit(const StewieInputTarget& target, UInt8 input, DWORD now)
		{
			if (input != ' ')
				return;

			s_lastStewieSpaceCommitTick = now;
			s_lastStewieSpaceCommitTarget = target;
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
			return target.kind == StewieInputKind::MenuSearch
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
				if (pending.input != input
					|| !SameStewieTarget(pending.target, target)
					|| DeferredSourceSeen(pending, source))
				{
					continue;
				}

				MarkDeferredSourceSeen(pending, source);
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
				RecordPendingStewieAscii(own, input, GetTickCount());
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

			const DWORD now = GetTickCount();
			std::vector<PendingStewieAscii>& opposite = source == StewieAsciiSource::Adapter
				? s_pendingStewieWndProcAscii
				: s_pendingStewieAdapterAscii;
			if (ConsumePendingStewieAscii(opposite, input, now))
				return true;

			if (ShouldDeferStewieAscii(target))
				return QueueDeferredStewieAscii(target, input, source);

			const char ch = static_cast<char>(input);
			if (!InsertTextAtCaretStewie(target, std::string_view(&ch, 1)))
				return false;

			std::vector<PendingStewieAscii>& own = source == StewieAsciiSource::Adapter
				? s_pendingStewieAdapterAscii
				: s_pendingStewieWndProcAscii;
			RecordPendingStewieAscii(own, input, now);
			RecordStewieSpaceCommit(target, input, now);
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
				RecordPendingStewieAscii(own, pending.input, s_lastDeferredStewieAsciiCommitTick);
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

			constexpr DWORD kLanguageSwitchRollbackMs = 250;
			const StewieInputTarget target = GetOverlayStewieInputTarget();
			if (!s_lastStewieSpaceCommitTick
				|| GetTickCount() - s_lastStewieSpaceCommitTick > kLanguageSwitchRollbackMs
				|| !target.valid
				|| !SameStewieTarget(target, s_lastStewieSpaceCommitTarget))
			{
				s_lastStewieSpaceCommitTick = 0;
				s_lastStewieSpaceCommitTarget = {};
				return;
			}

			EnsureStewieShadow(target);
			const size_t caret = ClampStewieBoundary(target, s_stewieShadow.text, s_stewieShadow.caret);
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

			s_lastStewieSpaceCommitTick = 0;
			s_lastStewieSpaceCommitTarget = {};
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
			s_stewieShadow = StewieShadowState();
			s_pendingStewieAdapterAscii.clear();
			s_pendingStewieWndProcAscii.clear();
			s_deferredStewieAscii.clear();
			s_lastDeferredStewieAsciiCommitTick = 0;
			s_lastDeferredStewieAsciiCommitChar = 0;
			s_lastStewieSpaceCommitTick = 0;
			s_lastStewieSpaceCommitTarget = {};
		}

		bool HandleStewieInput(Menu* menu, UInt32 input)
		{
			if (s_stewieReplay)
				return CallStewieOriginalInput(menu, input);

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
				return CallStewieOriginalInput(menu, input);
			default:
				return CallStewieOriginalInput(menu, input);
			}
		}

		void TryInstallStewieTweaksInputHooks()
		{
			if (!IsStewieTweaksAvailable())
				return;

			TryInstallStewieMenuSearchHooks();

			if (!s_tileTraitIsActive)
				s_tileTraitIsActive = Tile::TraitNameToID("_IsActive");
			if (!s_tileTraitIsSearchActive)
				s_tileTraitIsSearchActive = Tile::TraitNameToID("_IsSearchActive");
			if (!s_tileTraitCaretIndex)
				s_tileTraitCaretIndex = Tile::TraitNameToID("_CaretIndex");

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
			ResetStewieMenuSearchState();
			s_stewieReplay = false;
		}

		bool __fastcall StewieTweaksInputTargetEx::StewMenuKeyboardInput(Menu* apMenu, void*, UInt32 aiInput)
		{
			return HandleStewieInput(apMenu, aiInput);
		}

	}
}

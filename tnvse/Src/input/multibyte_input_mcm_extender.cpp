#include "multibyte_input_ime_internal.h"

// MCM Extender search integration.  The mod files remain untouched: tNVSE
// owns editing while the search box is active, then dispatches the mod's
// existing UDFs through private runtime events.

namespace fonthook::multibyte_input
{
	namespace
	{
		constexpr char kSearchChangedEvent[] = "tNVSE:MCMExtenderSearchChanged";
		constexpr char kKeyDownEvent[] = "tNVSE:MCMExtenderKeyDown";
		constexpr char kSearchInputEvent[] = "tNVSE:MCMExtenderSearchInput";
		constexpr UInt32 kMaxSearchBytes = 4095;
		enum class PendingMcmAction : UInt8
		{
			SearchInputEnter,
			KeyDownEscape,
			KeyDownTab,
			KeyDownCtrlF,
		};

		struct McmExtenderShadowState
		{
			McmExtenderInputTarget target;
			std::string text;
			size_t caret = 0;
			bool initialized = false;
			bool uiDirty = false;
			bool searchDirty = false;
		};

		bool s_bridgeInitializationAttempted = false;
		bool s_bridgeReady = false;
		bool s_targetObserved = false;
		DWORD s_lastAsciiTick = 0;
		UInt8 s_lastAscii = 0;
		DWORD s_suppressedControlCharTick = 0;
		WPARAM s_suppressedControlChar = 0;
		McmExtenderShadowState s_shadow;
		std::vector<PendingMcmAction> s_pendingActions;

		UInt32 s_traitSearchState = 0;
		UInt32 s_traitSearchText = 0;
		UInt32 s_traitCursor1 = 0;
		UInt32 s_traitCursor2 = 0;
		NVSEEventManagerInterface::ParamType s_stringEventParam[] = {
			NVSEEventManagerInterface::eParamType_String,
		};
		NVSEEventManagerInterface::ParamType s_intEventParam[] = {
			NVSEEventManagerInterface::eParamType_Int,
		};

		bool FileExists(const char* path)
		{
			const DWORD attributes = GetFileAttributesA(path);
			return attributes != INVALID_FILE_ATTRIBUTES
				&& !(attributes & FILE_ATTRIBUTE_DIRECTORY);
		}

		bool HasRequiredMcmExtenderScripts()
		{
			return FileExists("Data\\NVSE\\user_defined_functions\\MCMExtender\\MenuSearch.gek")
				&& FileExists("Data\\NVSE\\user_defined_functions\\MCMExtender\\MenuSearchInput.gek")
				&& FileExists("Data\\NVSE\\user_defined_functions\\MCMExtender\\OnKeyDown.gek");
		}

		void InitializeTraitIds()
		{
			if (!s_traitSearchState)
				s_traitSearchState = Tile::TraitNameToID("_MCMExt+Search");
			if (!s_traitSearchText)
				s_traitSearchText = Tile::TraitNameToID("_search_text_1");
			if (!s_traitCursor1)
				s_traitCursor1 = Tile::TraitNameToID("_search_cursor_1");
			if (!s_traitCursor2)
				s_traitCursor2 = Tile::TraitNameToID("_search_cursor_2");
		}

		Tile* FindDirectChild(Tile* parent, const char* name)
		{
			if (!parent || !name)
				return nullptr;

			for (Tile* child : parent->GetChildren())
			{
				if (child && !_stricmp(child->strName.c_str(), name))
					return child;
			}
			return nullptr;
		}

		bool HasTrait(Tile* tile, UInt32 trait)
		{
			return tile && trait && tile->GetValue(trait);
		}

		bool SameTarget(
			const McmExtenderInputTarget& lhs,
			const McmExtenderInputTarget& rhs)
		{
			return lhs.valid && rhs.valid
				&& lhs.menu == rhs.menu
				&& lhs.root == rhs.root
				&& lhs.mcm == rhs.mcm
				&& lhs.search == rhs.search;
		}

		size_t ReadCaretFromTiles(const McmExtenderInputTarget& target, const std::string& text)
		{
			if (!target.search || !s_traitCursor1 || !s_traitCursor2)
				return text.size();

			const std::string cursor1 = target.search->GetValueString(s_traitCursor1);
			const std::string cursor2 = target.search->GetValueString(s_traitCursor2);
			if (cursor1.size() != text.size() + 1 || cursor2.size() != cursor1.size())
				return text.size();

			for (size_t index = 0; index < cursor1.size(); ++index)
			{
				if (cursor1[index] != '|' || cursor2[index] != ' ')
					continue;

				bool sameAroundCaret = true;
				for (size_t other = 0; other < cursor1.size(); ++other)
				{
					if (other != index && cursor1[other] != cursor2[other])
					{
						sameAroundCaret = false;
						break;
					}
				}
				if (sameAroundCaret)
					return ClampToPrevBoundary(text, index);
			}

			return text.size();
		}

		void LoadShadow(const McmExtenderInputTarget& target)
		{
			s_shadow.target = target;
			s_shadow.text = target.search && s_traitSearchText
				? target.search->GetValueString(s_traitSearchText)
				: "";
			s_shadow.caret = ReadCaretFromTiles(target, s_shadow.text);
			s_shadow.caret = ClampToPrevBoundary(s_shadow.text, s_shadow.caret);
			s_shadow.initialized = target.valid;
			s_shadow.uiDirty = false;
			s_shadow.searchDirty = false;
		}

		void EnsureShadow(const McmExtenderInputTarget& target)
		{
			if (!s_shadow.initialized || !SameTarget(s_shadow.target, target))
			{
				LoadShadow(target);
				return;
			}

			const std::string visible = target.search->GetValueString(s_traitSearchText);
			if (!s_shadow.uiDirty && visible != s_shadow.text)
				LoadShadow(target);
		}

		bool DispatchStringEvent(const char* eventName, const std::string& value)
		{
			return g_eventInterface && g_eventInterface->DispatchEvent
				&& g_eventInterface->DispatchEvent(eventName, nullptr, value.c_str());
		}

		bool DispatchIntEvent(const char* eventName, int value)
		{
			return g_eventInterface && g_eventInterface->DispatchEvent
				&& g_eventInterface->DispatchEvent(eventName, nullptr, value);
		}

		bool ApplyShadow(
			const McmExtenderInputTarget& target,
			std::string text,
			size_t caret,
			bool updateSearch)
		{
			McmExtenderInputTarget active = GetActiveMcmExtenderInputTarget();
			if (!SameTarget(target, active))
				return false;

			caret = ClampToPrevBoundary(text, std::min(caret, text.size()));
			s_shadow.target = active;
			s_shadow.text = std::move(text);
			s_shadow.caret = caret;
			s_shadow.initialized = true;
			s_shadow.uiDirty = true;
			s_shadow.searchDirty = s_shadow.searchDirty || updateSearch;
			return true;
		}

		UInt32 GetJipCompatibleTopVisibleMenuID(InterfaceManager* manager)
		{
			if (!manager || manager->uiCurrentMode < 2)
				return 0;
			if (manager->pActiveMenu)
				return MenuID(manager->pActiveMenu);

			// JIP 57.30 starts at menuStack[1], walks to the first zero, then
			// returns the preceding entry.  The HUD-only special case is not a
			// StartMenu target, so returning that stack value is sufficient here.
			size_t index = 1;
			while (index < _countof(manager->menuStack) && manager->menuStack[index])
				++index;
			return manager->menuStack[index - 1];
		}

		void QueueAction(PendingMcmAction action)
		{
			if (s_pendingActions.size() < 16)
				s_pendingActions.push_back(action);
		}

		void FlushPendingWork(const McmExtenderInputTarget& target)
		{
			if (!SameTarget(target, s_shadow.target) || !s_shadow.initialized)
				return;

			if (s_shadow.uiDirty)
			{
				const size_t caret = ClampToPrevBoundary(
					s_shadow.text, std::min(s_shadow.caret, s_shadow.text.size()));
				std::string cursor1 = s_shadow.text;
				cursor1.insert(caret, 1, '|');
				std::string cursor2 = cursor1;
				cursor2[caret] = ' ';

				target.search->SetValueString(s_traitSearchText, s_shadow.text.c_str(), true);
				target.search->SetValueString(s_traitCursor1, cursor1.c_str(), true);
				target.search->SetValueString(s_traitCursor2, cursor2.c_str(), true);
				s_shadow.uiDirty = false;
			}

			if (s_shadow.searchDirty)
			{
				s_shadow.searchDirty = false;
				if (!DispatchStringEvent(kSearchChangedEvent, s_shadow.text))
				{
					gLog.FormattedMessage(
						"tnvse_multibyte_input: MCM Extender search event dispatch failed");
				}
			}

			std::vector<PendingMcmAction> actions;
			actions.swap(s_pendingActions);
			for (PendingMcmAction action : actions)
			{
				switch (action)
				{
				case PendingMcmAction::SearchInputEnter:
					DispatchIntEvent(kSearchInputEvent, 28);
					break;
				case PendingMcmAction::KeyDownEscape:
					DispatchIntEvent(kKeyDownEvent, 1);
					break;
				case PendingMcmAction::KeyDownTab:
					DispatchIntEvent(kKeyDownEvent, 15);
					break;
				case PendingMcmAction::KeyDownCtrlF:
					DispatchIntEvent(kKeyDownEvent, 33);
					break;
				}
			}
		}

		bool InsertBytes(
			const McmExtenderInputTarget& target,
			std::string_view bytes)
		{
			if (!target.valid || bytes.empty())
				return false;

			EnsureShadow(target);
			if (!s_shadow.initialized
				|| bytes.size() > kMaxSearchBytes - std::min<size_t>(s_shadow.text.size(), kMaxSearchBytes))
			{
				return false;
			}

			const size_t caret = ClampToPrevBoundary(s_shadow.text, s_shadow.caret);
			std::string candidate = s_shadow.text;
			candidate.insert(caret, bytes.data(), bytes.size());
			return ApplyShadow(target, std::move(candidate), caret + bytes.size(), true);
		}

		bool DeletePrevious(const McmExtenderInputTarget& target)
		{
			EnsureShadow(target);
			const size_t caret = ClampToPrevBoundary(s_shadow.text, s_shadow.caret);
			if (!caret)
				return true;

			const size_t previous = PrevCharBoundary(s_shadow.text, caret);
			std::string candidate = s_shadow.text;
			candidate.erase(previous, caret - previous);
			return ApplyShadow(target, std::move(candidate), previous, true);
		}

		bool DeleteNext(const McmExtenderInputTarget& target)
		{
			EnsureShadow(target);
			const size_t caret = ClampToPrevBoundary(s_shadow.text, s_shadow.caret);
			if (caret >= s_shadow.text.size())
				return true;

			const size_t next = NextCharBoundary(s_shadow.text, caret);
			std::string candidate = s_shadow.text;
			candidate.erase(caret, next - caret);
			return ApplyShadow(target, std::move(candidate), caret, true);
		}

		bool MoveCaret(const McmExtenderInputTarget& target, size_t caret)
		{
			EnsureShadow(target);
			return ApplyShadow(target, s_shadow.text, caret, false);
		}

		bool RegisterMcmEvent(
			const char* eventName,
			NVSEEventManagerInterface::ParamType paramType,
			const char* handlerScript)
		{
			NVSEEventManagerInterface::ParamType* params =
				paramType == NVSEEventManagerInterface::eParamType_String
				? s_stringEventParam
				: s_intEventParam;
			if (!g_eventInterface->RegisterEvent(
				eventName, 1, params, NVSEEventManagerInterface::kFlags_None))
			{
				gLog.FormattedMessage(
					"tnvse_multibyte_input: failed to register MCM Extender bridge event %s",
					eventName);
				return false;
			}

			char command[384] = {};
			_snprintf_s(
				command,
				_countof(command),
				_TRUNCATE,
				"SetEventHandler \"%s\" (CompileScript \"%s\")",
				eventName,
				handlerScript);
			if (!g_consoleInterface->RunScriptLine2(command, nullptr, true))
			{
				gLog.FormattedMessage(
					"tnvse_multibyte_input: failed to attach MCM Extender UDF %s to event %s",
					handlerScript,
					eventName);
				return false;
			}
			return true;
		}
	}

	bool InitializeMcmExtenderInputBridge()
	{
		if (s_bridgeReady || s_bridgeInitializationAttempted)
			return s_bridgeReady;
		s_bridgeInitializationAttempted = true;

		if (!g_bMultibyteInputMCMExtender)
			return false;
		if (!g_bSuppressJIPKeyEventsDuringMultibyteInput
			|| !IsJipKeyEventSuppressionHookInstalled())
		{
			gLog.FormattedMessage(
				"tnvse_multibyte_input: MCM Extender adapter disabled because the JIP 57.30 key-event suppression hook is unavailable");
			return false;
		}
		if (!g_eventInterface || !g_eventInterface->RegisterEvent
			|| !g_eventInterface->DispatchEvent
			|| !g_consoleInterface
			|| g_consoleInterface->version < NVSEConsoleInterface::kVersion
			|| !g_consoleInterface->RunScriptLine2)
		{
			gLog.FormattedMessage(
				"tnvse_multibyte_input: MCM Extender adapter disabled because the xNVSE event/console interfaces are unavailable");
			return false;
		}
		if (!HasRequiredMcmExtenderScripts())
		{
			gLog.FormattedMessage(
				"tnvse_multibyte_input: MCM Extender scripts not found; adapter not installed");
			return false;
		}

		const bool searchRegistered = RegisterMcmEvent(
			kSearchChangedEvent,
			NVSEEventManagerInterface::eParamType_String,
			"MCMExtender\\MenuSearch.gek");
		const bool keyRegistered = RegisterMcmEvent(
			kKeyDownEvent,
			NVSEEventManagerInterface::eParamType_Int,
			"MCMExtender\\OnKeyDown.gek");
		const bool searchInputRegistered = RegisterMcmEvent(
			kSearchInputEvent,
			NVSEEventManagerInterface::eParamType_Int,
			"MCMExtender\\MenuSearchInput.gek");
		s_bridgeReady = searchRegistered && keyRegistered && searchInputRegistered;
		if (s_bridgeReady)
		{
			gLog.FormattedMessage(
				"tnvse_multibyte_input: MCM Extender runtime input bridge installed");
		}
		return s_bridgeReady;
	}

	McmExtenderInputTarget GetActiveMcmExtenderInputTarget()
	{
		McmExtenderInputTarget target;
		if (!s_bridgeReady || !IsJipKeyEventSuppressionHookInstalled())
			return target;

		Menu* menu = GetOpenMenu(Pause);
		InterfaceManager* manager = InterfaceManager::GetSingleton();
		if (!menu || !manager
			|| InterfaceManager::IsMenuActive(Console)
			|| GetJipCompatibleTopVisibleMenuID(manager) != Pause)
		{
			return target;
		}

		InitializeTraitIds();
		Tile* root = MenuRoot(menu);
		Tile* mcm = FindDirectChild(root, "MCM");
		Tile* search = FindDirectChild(mcm, "MCM_Search");
		if (!root || !mcm || !search
			|| !HasTrait(root, s_traitSearchState)
			|| !HasTrait(search, s_traitSearchText)
			|| !HasTrait(search, s_traitCursor1)
			|| !HasTrait(search, s_traitCursor2)
			|| root->GetValueFloat(s_traitSearchState) != 1.0f)
		{
			return target;
		}

		target.menu = menu;
		target.root = root;
		target.mcm = mcm;
		target.search = search;
		target.valid = true;
		return target;
	}

	McmExtenderInputTarget GetOverlayMcmExtenderInputTarget()
	{
		return GetActiveMcmExtenderInputTarget();
	}

	void ProcessMcmExtenderInputTargetState()
	{
		if (!s_bridgeReady)
			return;

		const McmExtenderInputTarget target = GetActiveMcmExtenderInputTarget();
		if (target.valid)
		{
			EnsureShadow(target);
			if (!s_targetObserved)
			{
				s_targetObserved = true;
				// WndProc may already have observed this exact target and started
				// composition before the 50 ms state poll.  Do not cancel that live
				// composition merely to publish the target-observed edge.
				if (!State().textInputSessionActive)
					RefreshTextInputSessionForActiveTarget("mcm_extender_search_activate");
				DebugLog(
					"tnvse_multibyte_input_event: source=MainLoop action=mcm_extender_activate menu=0x%08X search=0x%08X",
					reinterpret_cast<UInt32>(target.menu),
					reinterpret_cast<UInt32>(target.search));
			}
			// Tile mutation and MCM's list-rebuilding UDFs run only here, from
			// kMessage_MainGameLoop.  Win32 callbacks merely update the shadow and
			// enqueue work, avoiding UI/script re-entry from WndProc.
			FlushPendingWork(target);
			return;
		}

		if (s_targetObserved)
		{
			s_targetObserved = false;
			s_shadow = {};
			s_lastAscii = 0;
			s_lastAsciiTick = 0;
			s_pendingActions.clear();
			if (!GetCurrentTextEditMenuObject()
				&& !GetOverlayStewieInputTarget().valid
				&& !GetOverlayDialogueHistoryInputTarget().valid)
			{
				EndStewieTextInputSession("mcm_extender_search_deactivate");
			}
			DebugLog(
				"tnvse_multibyte_input_event: source=MainLoop action=mcm_extender_deactivate");
		}
	}

	void ResetMcmExtenderInputState()
	{
		s_targetObserved = false;
		s_lastAsciiTick = 0;
		s_lastAscii = 0;
		s_suppressedControlCharTick = 0;
		s_suppressedControlChar = 0;
		s_pendingActions.clear();
		s_shadow = {};
	}

	bool InsertWideTextMcmExtender(
		const McmExtenderInputTarget& target,
		std::wstring_view text)
	{
		const std::string bytes = WideToCurrentCodePage(text);
		return !bytes.empty() && InsertBytes(target, bytes);
	}

	bool HandleMcmExtenderWndProcChar(
		const McmExtenderInputTarget& target,
		WPARAM input,
		bool controlDown)
	{
		if (!target.valid)
			return false;
		if (input < 0x20 || input == 0x7F || controlDown)
			return true;
		if (input > 0xFFFF)
			return false;

		if (input <= 0x7E)
		{
			if (FilterGameInput(
					static_cast<UInt32>(input),
					ImeCommitInputChannel::WndProcChar,
					GameInputFilterClass::PrintableAscii)
				!= GameInputFilterResult::Pass)
			{
				return true;
			}
			if (ShouldSuppressInputLanguageSwitchAscii(static_cast<UInt8>(input)))
				return true;

			const char value = static_cast<char>(input);
			if (!InsertBytes(target, std::string_view(&value, 1)))
				return true;
			s_lastAscii = static_cast<UInt8>(input);
			s_lastAsciiTick = GetTickCount();
			DebugLog(
				"tnvse_multibyte_input_event: source=MCMExtender.WM_CHAR action=insert_ascii input=0x%02X",
				static_cast<UInt32>(input));
			return true;
		}

		const wchar_t value = static_cast<wchar_t>(input);
		const bool inserted = InsertWideTextMcmExtender(
			target, std::wstring_view(&value, 1));
		DebugLog(
			"tnvse_multibyte_input_event: source=MCMExtender.WM_CHAR action=insert_nonascii input=0x%04X inserted=%u",
			static_cast<UInt32>(input),
			inserted ? 1 : 0);
		return true;
	}

	bool HandleMcmExtenderKeyDown(
		const McmExtenderInputTarget& target,
		WPARAM virtualKey,
		bool controlDown)
	{
		if (!target.valid)
			return false;
		const bool compositionControl =
			virtualKey == VK_BACK
			|| virtualKey == VK_DELETE
			|| virtualKey == VK_LEFT
			|| virtualKey == VK_RIGHT
			|| virtualKey == VK_HOME
			|| virtualKey == VK_END
			|| virtualKey == VK_RETURN
			|| virtualKey == VK_ESCAPE
			|| virtualKey == VK_TAB;
		if (compositionControl
			&& FilterGameInput(
				static_cast<UInt32>(virtualKey),
				ImeCommitInputChannel::McmExtender,
				GameInputFilterClass::CompositionControl)
			!= GameInputFilterResult::Pass)
		{
			return true;
		}

		EnsureShadow(target);
		bool handled = true;
		switch (virtualKey)
		{
		case VK_BACK:
			handled = DeletePrevious(target);
			break;
		case VK_DELETE:
			handled = DeleteNext(target);
			break;
		case VK_LEFT:
			handled = MoveCaret(target, PrevCharBoundary(s_shadow.text, s_shadow.caret));
			break;
		case VK_RIGHT:
			handled = MoveCaret(target, NextCharBoundary(s_shadow.text, s_shadow.caret));
			break;
		case VK_HOME:
			handled = MoveCaret(target, 0);
			break;
		case VK_END:
			handled = MoveCaret(target, s_shadow.text.size());
			break;
		case VK_RETURN:
			s_suppressedControlChar = '\r';
			s_suppressedControlCharTick = GetTickCount();
			QueueAction(PendingMcmAction::SearchInputEnter);
			break;
		case VK_ESCAPE:
			s_suppressedControlChar = 0x1B;
			s_suppressedControlCharTick = GetTickCount();
			QueueAction(PendingMcmAction::KeyDownEscape);
			break;
		case VK_TAB:
			s_suppressedControlChar = '\t';
			s_suppressedControlCharTick = GetTickCount();
			QueueAction(PendingMcmAction::KeyDownTab);
			break;
		case 'F':
			if (controlDown)
			{
				s_suppressedControlChar = 0x06;
				s_suppressedControlCharTick = GetTickCount();
				QueueAction(PendingMcmAction::KeyDownCtrlF);
				break;
			}
			handled = false;
			break;
		default:
			handled = false;
			break;
		}
		return handled;
	}

	bool HandleMcmExtenderMenuInput(Menu* menu, UInt32 input)
	{
		const McmExtenderInputTarget target = GetActiveMcmExtenderInputTarget();
		if (!target.valid || target.menu != menu)
			return false;

		// Printable input is committed from WM_CHAR/IME so keyboard layout,
		// Shift and Caps Lock remain authoritative.  Consuming it here prevents
		// Stewie/vanilla StartMenu handling from seeing a second copy.
		if ((input >= 0x20 && input <= 0xFF)
			|| input == '\b' || input == '\t' || input == '\r' || input == 0x1B)
		{
			return true;
		}

		WPARAM virtualKey = 0;
		switch (input)
		{
		case kInputCode_Backspace: virtualKey = VK_BACK; break;
		case kInputCode_Delete: virtualKey = VK_DELETE; break;
		case kInputCode_ArrowLeft: virtualKey = VK_LEFT; break;
		case kInputCode_ArrowRight: virtualKey = VK_RIGHT; break;
		case kInputCode_Home: virtualKey = VK_HOME; break;
		case kInputCode_End: virtualKey = VK_END; break;
		case kInputCode_Enter: virtualKey = VK_RETURN; break;
		default:
			// Preserve controller navigation and unrelated StartMenu actions.
			return false;
		}

		if (!(GetAsyncKeyState(static_cast<int>(virtualKey)) & 0x8000))
			return false;

		// WM_KEYDOWN is the sole owner of keyboard editing, including Windows
		// key-repeat.  StartMenu receives the same physical key again through
		// DirectInput; consume that copy unconditionally instead of guessing from
		// timing, which could turn one Backspace into two DBCS deletions.
		DebugLog(
			"tnvse_multibyte_input_event: source=MCMExtender.MenuInput action=consume_keyboard_duplicate input=0x%08X",
			input);
		return true;
	}

	bool ShouldSuppressMcmExtenderControlChar(WPARAM input)
	{
		if (!s_suppressedControlChar
			|| input != s_suppressedControlChar
			|| GetTickCount() - s_suppressedControlCharTick > kDuplicateAsciiSuppressMs)
		{
			return false;
		}

		s_suppressedControlChar = 0;
		s_suppressedControlCharTick = 0;
		return true;
	}

	bool RemovePreviousMcmExtenderAsciiCompositionEcho(wchar_t compositionLead)
	{
		if (!s_lastAscii
			|| GetTickCount() - s_lastAsciiTick > kDuplicateAsciiSuppressMs
			|| !AsciiEqualsIgnoreCase(s_lastAscii, compositionLead))
		{
			return false;
		}

		const McmExtenderInputTarget target = GetOverlayMcmExtenderInputTarget();
		if (!target.valid)
			return false;
		EnsureShadow(target);
		const size_t caret = ClampToPrevBoundary(s_shadow.text, s_shadow.caret);
		if (!caret)
			return false;

		const size_t previous = PrevCharBoundary(s_shadow.text, caret);
		if (caret - previous != 1
			|| !AsciiEqualsIgnoreCase(
				static_cast<UInt8>(s_shadow.text[previous]), compositionLead))
		{
			return false;
		}

		s_lastAscii = 0;
		s_lastAsciiTick = 0;
		std::string candidate = s_shadow.text;
		candidate.erase(previous, 1);
		return ApplyShadow(target, std::move(candidate), previous, true);
	}
}

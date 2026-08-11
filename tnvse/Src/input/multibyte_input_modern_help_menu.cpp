#include "multibyte_input_ime_internal.h"

// Modern Help Menu search integration. The mod files remain untouched: tNVSE
// owns encoding-aware editing while the scripted search field is active, then
// dispatches the mod's existing UDFs through private runtime events.

namespace fonthook::multibyte_input
{
	namespace implementation::multibyte_input_modern_help_menu {}
	using namespace implementation::multibyte_input_modern_help_menu;

	namespace implementation::multibyte_input_modern_help_menu
	{
		constexpr char kSearchChangedEvent[] = "tNVSE:ModernHelpMenuSearchChanged";
		constexpr char kKeyDownEvent[] = "tNVSE:ModernHelpMenuKeyDown";
		constexpr char kSearchInputEvent[] = "tNVSE:ModernHelpMenuSearchInput";
		constexpr UInt32 kMaxSearchBytes = 4095;

		enum class PendingAction : UInt8
		{
			SearchInputEnter,
			KeyDownTab,
			KeyDownCtrlF,
		};

		struct ShadowState
		{
			ModernHelpMenuInputTarget target;
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
		ShadowState s_shadow;
		std::vector<PendingAction> s_pendingActions;

		UInt32 s_traitVisible = 0;
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

		bool HasRequiredScripts()
		{
			return FileExists(
				"Data\\NVSE\\user_defined_functions\\ModernHelpMenu\\Search.gek")
				&& FileExists(
					"Data\\NVSE\\user_defined_functions\\ModernHelpMenu\\SearchInput.gek")
				&& FileExists(
					"Data\\NVSE\\user_defined_functions\\ModernHelpMenu\\OnKeyDown.gek");
		}

		void InitializeTraitIds()
		{
			if (!s_traitVisible)
				s_traitVisible = Tile::TraitNameToID("_HelpMenu+Visible");
			if (!s_traitSearchState)
				s_traitSearchState = Tile::TraitNameToID("_HelpMenu+Search");
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
			const ModernHelpMenuInputTarget& lhs,
			const ModernHelpMenuInputTarget& rhs)
		{
			return lhs.valid && rhs.valid
				&& lhs.menu == rhs.menu
				&& lhs.root == rhs.root
				&& lhs.modernHelpMenu == rhs.modernHelpMenu
				&& lhs.search == rhs.search;
		}

		size_t ReadCaretFromTiles(
			const ModernHelpMenuInputTarget& target,
			const std::string& text)
		{
			if (!target.search || !s_traitCursor1 || !s_traitCursor2)
				return text.size();

			const std::string cursor1 =
				target.search->GetValueString(s_traitCursor1);
			const std::string cursor2 =
				target.search->GetValueString(s_traitCursor2);
			if (cursor1.size() != text.size() + 1
				|| cursor2.size() != cursor1.size())
			{
				return text.size();
			}

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

		void LoadShadow(const ModernHelpMenuInputTarget& target)
		{
			s_shadow.target = target;
			s_shadow.text = target.search && s_traitSearchText
				? target.search->GetValueString(s_traitSearchText)
				: "";
			s_shadow.caret = ReadCaretFromTiles(target, s_shadow.text);
			s_shadow.caret =
				ClampToPrevBoundary(s_shadow.text, s_shadow.caret);
			s_shadow.initialized = target.valid;
			s_shadow.uiDirty = false;
			s_shadow.searchDirty = false;
		}

		void EnsureShadow(const ModernHelpMenuInputTarget& target)
		{
			if (!s_shadow.initialized || !SameTarget(s_shadow.target, target))
			{
				LoadShadow(target);
				return;
			}

			const std::string visible =
				target.search->GetValueString(s_traitSearchText);
			if (!s_shadow.uiDirty && visible != s_shadow.text)
				LoadShadow(target);
		}

		bool DispatchStringEvent(const char* eventName, const std::string& value)
		{
			return g_eventInterface && g_eventInterface->DispatchEvent
				&& g_eventInterface->DispatchEvent(
					eventName, nullptr, value.c_str());
		}

		bool DispatchIntEvent(const char* eventName, int value)
		{
			return g_eventInterface && g_eventInterface->DispatchEvent
				&& g_eventInterface->DispatchEvent(eventName, nullptr, value);
		}

		bool ApplyShadow(
			const ModernHelpMenuInputTarget& target,
			std::string text,
			size_t caret,
			bool updateSearch)
		{
			ModernHelpMenuInputTarget active =
				GetActiveModernHelpMenuInputTarget();
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

		void QueueAction(PendingAction action)
		{
			if (s_pendingActions.size() < 16)
				s_pendingActions.push_back(action);
		}

		void FlushPendingWork(const ModernHelpMenuInputTarget& target)
		{
			if (!SameTarget(target, s_shadow.target) || !s_shadow.initialized)
				return;

			if (s_shadow.uiDirty)
			{
				const size_t caret = ClampToPrevBoundary(
					s_shadow.text,
					std::min(s_shadow.caret, s_shadow.text.size()));
				std::string cursor1 = s_shadow.text;
				cursor1.insert(caret, 1, '|');
				std::string cursor2 = cursor1;
				cursor2[caret] = ' ';

				target.search->SetValueString(
					s_traitSearchText, s_shadow.text.c_str(), true);
				target.search->SetValueString(
					s_traitCursor1, cursor1.c_str(), true);
				target.search->SetValueString(
					s_traitCursor2, cursor2.c_str(), true);
				s_shadow.uiDirty = false;
			}

			if (s_shadow.searchDirty)
			{
				s_shadow.searchDirty = false;
				if (!DispatchStringEvent(kSearchChangedEvent, s_shadow.text))
				{
					gLog.FormattedMessage(
						"tnvse_multibyte_input: Modern Help Menu search event dispatch failed");
				}
			}

			std::vector<PendingAction> actions;
			actions.swap(s_pendingActions);
			for (PendingAction action : actions)
			{
				switch (action)
				{
				case PendingAction::SearchInputEnter:
					DispatchIntEvent(kSearchInputEvent, 28);
					break;
				case PendingAction::KeyDownTab:
					DispatchIntEvent(kKeyDownEvent, 15);
					break;
				case PendingAction::KeyDownCtrlF:
					DispatchIntEvent(kKeyDownEvent, 33);
					break;
				}
			}
		}

		bool InsertBytes(
			const ModernHelpMenuInputTarget& target,
			std::string_view bytes)
		{
			if (!target.valid || bytes.empty())
				return false;

			EnsureShadow(target);
			if (!s_shadow.initialized
				|| bytes.size() > kMaxSearchBytes
					- std::min<size_t>(s_shadow.text.size(), kMaxSearchBytes))
			{
				return false;
			}

			const size_t caret =
				ClampToPrevBoundary(s_shadow.text, s_shadow.caret);
			std::string candidate = s_shadow.text;
			candidate.insert(caret, bytes.data(), bytes.size());
			return ApplyShadow(
				target, std::move(candidate), caret + bytes.size(), true);
		}

		bool DeletePrevious(const ModernHelpMenuInputTarget& target)
		{
			EnsureShadow(target);
			const size_t caret =
				ClampToPrevBoundary(s_shadow.text, s_shadow.caret);
			if (!caret)
				return true;

			const size_t previous = PrevCharBoundary(s_shadow.text, caret);
			std::string candidate = s_shadow.text;
			candidate.erase(previous, caret - previous);
			return ApplyShadow(target, std::move(candidate), previous, true);
		}

		bool DeleteNext(const ModernHelpMenuInputTarget& target)
		{
			EnsureShadow(target);
			const size_t caret =
				ClampToPrevBoundary(s_shadow.text, s_shadow.caret);
			if (caret >= s_shadow.text.size())
				return true;

			const size_t next = NextCharBoundary(s_shadow.text, caret);
			std::string candidate = s_shadow.text;
			candidate.erase(caret, next - caret);
			return ApplyShadow(target, std::move(candidate), caret, true);
		}

		bool MoveCaret(
			const ModernHelpMenuInputTarget& target,
			size_t caret)
		{
			EnsureShadow(target);
			return ApplyShadow(target, s_shadow.text, caret, false);
		}

		bool RegisterEvent(
			const char* eventName,
			NVSEEventManagerInterface::ParamType paramType,
			const char* handlerScript)
		{
			NVSEEventManagerInterface::ParamType* params =
				paramType == NVSEEventManagerInterface::eParamType_String
				? s_stringEventParam
				: s_intEventParam;
			if (!g_eventInterface->RegisterEvent(
					eventName, 1, params,
					NVSEEventManagerInterface::kFlags_None))
			{
				gLog.FormattedMessage(
					"tnvse_multibyte_input: failed to register Modern Help Menu bridge event %s",
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
					"tnvse_multibyte_input: failed to attach Modern Help Menu UDF %s to event %s",
					handlerScript,
					eventName);
				return false;
			}
			return true;
		}
	}

	bool InitializeModernHelpMenuInputBridge()
	{
		if (s_bridgeReady || s_bridgeInitializationAttempted)
			return s_bridgeReady;
		s_bridgeInitializationAttempted = true;

		if (!g_bMultibyteInputModernHelpMenu)
			return false;
		if (!g_bSuppressJIPKeyEventsDuringMultibyteInput
			|| !IsJipKeyEventSuppressionHookInstalled())
		{
			gLog.FormattedMessage(
				"tnvse_multibyte_input: Modern Help Menu adapter disabled because the JIP 57.30 key-event suppression hook is unavailable");
			return false;
		}
		if (!g_eventInterface || !g_eventInterface->RegisterEvent
			|| !g_eventInterface->DispatchEvent
			|| !g_consoleInterface
			|| g_consoleInterface->version < NVSEConsoleInterface::kVersion
			|| !g_consoleInterface->RunScriptLine2)
		{
			gLog.FormattedMessage(
				"tnvse_multibyte_input: Modern Help Menu adapter disabled because the xNVSE event/console interfaces are unavailable");
			return false;
		}
		if (!HasRequiredScripts())
		{
			gLog.FormattedMessage(
				"tnvse_multibyte_input: Modern Help Menu scripts not found; adapter not installed");
			return false;
		}

		const bool searchRegistered = RegisterEvent(
			kSearchChangedEvent,
			NVSEEventManagerInterface::eParamType_String,
			"ModernHelpMenu\\Search.gek");
		const bool keyRegistered = RegisterEvent(
			kKeyDownEvent,
			NVSEEventManagerInterface::eParamType_Int,
			"ModernHelpMenu\\OnKeyDown.gek");
		const bool inputRegistered = RegisterEvent(
			kSearchInputEvent,
			NVSEEventManagerInterface::eParamType_Int,
			"ModernHelpMenu\\SearchInput.gek");
		s_bridgeReady =
			searchRegistered && keyRegistered && inputRegistered;
		if (s_bridgeReady)
		{
			gLog.FormattedMessage(
				"tnvse_multibyte_input: Modern Help Menu runtime input bridge installed");
		}
		return s_bridgeReady;
	}

	ModernHelpMenuInputTarget GetActiveModernHelpMenuInputTarget()
	{
		ModernHelpMenuInputTarget target;
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
		Tile* modernHelpMenu = FindDirectChild(root, "ModernHelpMenu");
		Tile* search = FindDirectChild(modernHelpMenu, "Search");
		if (!root || !modernHelpMenu || !search
			|| !HasTrait(root, s_traitVisible)
			|| !HasTrait(root, s_traitSearchState)
			|| !HasTrait(search, s_traitSearchText)
			|| !HasTrait(search, s_traitCursor1)
			|| !HasTrait(search, s_traitCursor2)
			|| root->GetValueFloat(s_traitVisible) != 1.0f
			|| root->GetValueFloat(s_traitSearchState) != 1.0f)
		{
			return target;
		}

		target.menu = menu;
		target.root = root;
		target.modernHelpMenu = modernHelpMenu;
		target.search = search;
		target.valid = true;
		return target;
	}

	ModernHelpMenuInputTarget GetOverlayModernHelpMenuInputTarget()
	{
		return GetActiveModernHelpMenuInputTarget();
	}

	void ProcessModernHelpMenuInputTargetState()
	{
		if (!s_bridgeReady)
			return;

		const ModernHelpMenuInputTarget target =
			GetActiveModernHelpMenuInputTarget();
		if (target.valid)
		{
			EnsureShadow(target);
			if (!s_targetObserved)
			{
				s_targetObserved = true;
				if (!State().textInputSessionActive)
				{
					RefreshTextInputSessionForActiveTarget(
						"modern_help_menu_search_activate");
				}
				DebugLog(
					"tnvse_multibyte_input_event: source=MainLoop action=modern_help_menu_activate menu=0x%08X search=0x%08X",
					reinterpret_cast<UInt32>(target.menu),
					reinterpret_cast<UInt32>(target.search));
			}
			// Tile mutation and UDF dispatch stay on the game thread. WndProc
			// processing only updates the shadow and queues work.
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
				&& !GetOverlayDialogueHistoryInputTarget().valid
				&& !GetOverlayMcmExtenderInputTarget().valid)
			{
				EndStewieTextInputSession(
					"modern_help_menu_search_deactivate");
			}
			DebugLog(
				"tnvse_multibyte_input_event: source=MainLoop action=modern_help_menu_deactivate");
		}
	}

	void ResetModernHelpMenuInputState()
	{
		s_targetObserved = false;
		s_lastAsciiTick = 0;
		s_lastAscii = 0;
		s_suppressedControlCharTick = 0;
		s_suppressedControlChar = 0;
		s_pendingActions.clear();
		s_shadow = {};
	}

	bool InsertWideTextModernHelpMenu(
		const ModernHelpMenuInputTarget& target,
		std::wstring_view text)
	{
		const std::string bytes = WideToCurrentCodePage(text);
		return !bytes.empty() && InsertBytes(target, bytes);
	}

	bool HandleModernHelpMenuWndProcChar(
		const ModernHelpMenuInputTarget& target,
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
			if (ShouldSuppressInputLanguageSwitchAscii(
					static_cast<UInt8>(input)))
			{
				return true;
			}

			const char value = static_cast<char>(input);
			if (!InsertBytes(target, std::string_view(&value, 1)))
				return true;
			s_lastAscii = static_cast<UInt8>(input);
			s_lastAsciiTick = GetTickCount();
			DebugLog(
				"tnvse_multibyte_input_event: source=ModernHelpMenu.WM_CHAR action=insert_ascii input=0x%02X",
				static_cast<UInt32>(input));
			return true;
		}

		const wchar_t value = static_cast<wchar_t>(input);
		const bool inserted = InsertWideTextModernHelpMenu(
			target, std::wstring_view(&value, 1));
		DebugLog(
			"tnvse_multibyte_input_event: source=ModernHelpMenu.WM_CHAR action=insert_nonascii input=0x%04X inserted=%u",
			static_cast<UInt32>(input),
			inserted ? 1 : 0);
		return true;
	}

	bool HandleModernHelpMenuKeyDown(
		const ModernHelpMenuInputTarget& target,
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
			|| virtualKey == VK_TAB
			|| (controlDown && virtualKey == 'F');
		if (compositionControl
			&& FilterGameInput(
					static_cast<UInt32>(virtualKey),
					ImeCommitInputChannel::ModernHelpMenu,
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
			handled = MoveCaret(
				target,
				PrevCharBoundary(s_shadow.text, s_shadow.caret));
			break;
		case VK_RIGHT:
			handled = MoveCaret(
				target,
				NextCharBoundary(s_shadow.text, s_shadow.caret));
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
			QueueAction(PendingAction::SearchInputEnter);
			break;
		case VK_TAB:
			s_suppressedControlChar = '\t';
			s_suppressedControlCharTick = GetTickCount();
			QueueAction(PendingAction::KeyDownTab);
			break;
		case 'F':
			if (controlDown)
			{
				s_suppressedControlChar = 0x06;
				s_suppressedControlCharTick = GetTickCount();
				QueueAction(PendingAction::KeyDownCtrlF);
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

	bool HandleModernHelpMenuMenuInput(Menu* menu, UInt32 input)
	{
		const ModernHelpMenuInputTarget target =
			GetActiveModernHelpMenuInputTarget();
		if (!target.valid || target.menu != menu)
			return false;

		// Printable input is committed from WM_CHAR/IME. Consume the matching
		// menu/DirectInput copy before the original per-key UDF sees it again.
		if ((input >= 0x20 && input <= 0xFF)
			|| input == '\b' || input == '\t' || input == '\r')
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
			return false;
		}

		if (!(GetAsyncKeyState(static_cast<int>(virtualKey)) & 0x8000))
			return false;

		DebugLog(
			"tnvse_multibyte_input_event: source=ModernHelpMenu.MenuInput action=consume_keyboard_duplicate input=0x%08X",
			input);
		return true;
	}

	bool ShouldSuppressModernHelpMenuControlChar(WPARAM input)
	{
		if (!s_suppressedControlChar
			|| input != s_suppressedControlChar
			|| GetTickCount() - s_suppressedControlCharTick
				> kDuplicateAsciiSuppressMs)
		{
			return false;
		}

		s_suppressedControlChar = 0;
		s_suppressedControlCharTick = 0;
		return true;
	}

	bool RemovePreviousModernHelpMenuAsciiCompositionEcho(
		wchar_t compositionLead)
	{
		if (!s_lastAscii
			|| GetTickCount() - s_lastAsciiTick > kDuplicateAsciiSuppressMs
			|| !AsciiEqualsIgnoreCase(s_lastAscii, compositionLead))
		{
			return false;
		}

		const ModernHelpMenuInputTarget target =
			GetOverlayModernHelpMenuInputTarget();
		if (!target.valid)
			return false;
		EnsureShadow(target);
		const size_t caret =
			ClampToPrevBoundary(s_shadow.text, s_shadow.caret);
		if (!caret)
			return false;

		const size_t previous = PrevCharBoundary(s_shadow.text, caret);
		if (caret - previous != 1
			|| !AsciiEqualsIgnoreCase(
				static_cast<UInt8>(s_shadow.text[previous]),
				compositionLead))
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

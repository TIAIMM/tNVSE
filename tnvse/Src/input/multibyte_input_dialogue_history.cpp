#include "multibyte_input_ime_internal.h"

// Dialogue History search integration. The mod remains untouched: tNVSE owns
// code-page editing while its search field is active and calls the original
// UDFs from the main loop through private runtime events.

namespace fonthook::multibyte_input
{
	namespace
	{
		constexpr char kSearchChangedEvent[] = "tNVSE:DialogueHistorySearchChanged";
		constexpr char kKeyDownEvent[] = "tNVSE:DialogueHistoryKeyDown";
		constexpr char kSearchInputEvent[] = "tNVSE:DialogueHistorySearchInput";
		constexpr UInt32 kMaxSearchBytes = 4095;
		constexpr DWORD kSearchDebounceMs = 500;

		enum class PendingAction : UInt8
		{
			SearchInputEnter,
			KeyDownTab,
			KeyDownCtrlF,
		};

		struct ShadowState
		{
			DialogueHistoryInputTarget target;
			std::string text;
			size_t caret = 0;
			bool initialized = false;
			bool uiDirty = false;
			bool searchDirty = false;
			DWORD searchDueTick = 0;
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
			return FileExists("Data\\NVSE\\user_defined_functions\\DialogueHistory\\Search.gek")
				&& FileExists("Data\\NVSE\\user_defined_functions\\DialogueHistory\\SearchInput.gek")
				&& FileExists("Data\\NVSE\\user_defined_functions\\DialogueHistory\\OnKeyDown.gek");
		}

		void InitializeTraitIds()
		{
			if (!s_traitVisible)
				s_traitVisible = Tile::TraitNameToID("_DiaHist+Visible");
			if (!s_traitSearchState)
				s_traitSearchState = Tile::TraitNameToID("_DiaHist+Search");
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
			const DialogueHistoryInputTarget& lhs,
			const DialogueHistoryInputTarget& rhs)
		{
			return lhs.valid && rhs.valid
				&& lhs.menu == rhs.menu
				&& lhs.root == rhs.root
				&& lhs.dialogueHistory == rhs.dialogueHistory
				&& lhs.search == rhs.search;
		}

		UInt32 GetJipCompatibleTopVisibleMenuID(InterfaceManager* manager)
		{
			if (!manager || manager->uiCurrentMode < 2)
				return 0;
			if (manager->pActiveMenu)
				return MenuID(manager->pActiveMenu);
			size_t index = 1;
			while (index < _countof(manager->menuStack) && manager->menuStack[index])
				++index;
			return manager->menuStack[index - 1];
		}

		size_t ReadCaretFromTiles(
			const DialogueHistoryInputTarget& target,
			const std::string& text)
		{
			const std::string cursor1 = target.search->GetValueString(s_traitCursor1);
			const std::string cursor2 = target.search->GetValueString(s_traitCursor2);
			if (cursor1.size() != text.size() + 1 || cursor2.size() != cursor1.size())
				return text.size();

			for (size_t index = 0; index < cursor1.size(); ++index)
			{
				if (cursor1[index] != '|' || cursor2[index] != ' ')
					continue;
				bool matches = true;
				for (size_t other = 0; other < cursor1.size(); ++other)
				{
					if (other != index && cursor1[other] != cursor2[other])
					{
						matches = false;
						break;
					}
				}
				if (matches)
					return ClampToPrevBoundary(text, index);
			}
			return text.size();
		}

		void LoadShadow(const DialogueHistoryInputTarget& target)
		{
			s_shadow.target = target;
			s_shadow.text = target.search->GetValueString(s_traitSearchText);
			s_shadow.caret = ClampToPrevBoundary(
				s_shadow.text, ReadCaretFromTiles(target, s_shadow.text));
			s_shadow.initialized = target.valid;
			s_shadow.uiDirty = false;
			s_shadow.searchDirty = false;
			s_shadow.searchDueTick = 0;
		}

		void EnsureShadow(const DialogueHistoryInputTarget& target)
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
			const DialogueHistoryInputTarget& target,
			std::string text,
			size_t caret,
			bool updateSearch)
		{
			DialogueHistoryInputTarget active = GetActiveDialogueHistoryInputTarget();
			if (!SameTarget(target, active))
				return false;
			caret = ClampToPrevBoundary(text, std::min(caret, text.size()));
			s_shadow.target = active;
			s_shadow.text = std::move(text);
			s_shadow.caret = caret;
			s_shadow.initialized = true;
			s_shadow.uiDirty = true;
			if (updateSearch)
			{
				s_shadow.searchDirty = true;
				s_shadow.searchDueTick = GetTickCount() + kSearchDebounceMs;
			}
			return true;
		}

		void QueueAction(PendingAction action)
		{
			if (s_pendingActions.size() < 16)
				s_pendingActions.push_back(action);
		}

		void FlushPendingWork(const DialogueHistoryInputTarget& target)
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

			const DWORD now = GetTickCount();
			const bool forceSearch = !s_pendingActions.empty();
			if (s_shadow.searchDirty
				&& (forceSearch
					|| static_cast<SInt32>(now - s_shadow.searchDueTick) >= 0))
			{
				s_shadow.searchDirty = false;
				s_shadow.searchDueTick = 0;
				if (!DispatchStringEvent(kSearchChangedEvent, s_shadow.text))
					gLog.FormattedMessage("tnvse_multibyte_input: Dialogue History search event dispatch failed");
			}

			std::vector<PendingAction> actions;
			actions.swap(s_pendingActions);
			for (PendingAction action : actions)
			{
				bool dispatched = false;
				switch (action)
				{
				case PendingAction::SearchInputEnter:
					dispatched = DispatchIntEvent(kSearchInputEvent, 28);
					break;
				case PendingAction::KeyDownTab:
					dispatched = DispatchIntEvent(kKeyDownEvent, 15);
					break;
				case PendingAction::KeyDownCtrlF:
					dispatched = DispatchIntEvent(kKeyDownEvent, 33);
					break;
				}
				if (!dispatched)
					gLog.FormattedMessage("tnvse_multibyte_input: Dialogue History control event dispatch failed");
			}
		}

		bool InsertBytes(
			const DialogueHistoryInputTarget& target,
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

		bool DeletePrevious(const DialogueHistoryInputTarget& target)
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

		bool DeleteNext(const DialogueHistoryInputTarget& target)
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

		bool MoveCaret(const DialogueHistoryInputTarget& target, size_t caret)
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
				eventName, 1, params, NVSEEventManagerInterface::kFlags_None))
			{
				gLog.FormattedMessage(
					"tnvse_multibyte_input: failed to register Dialogue History bridge event %s",
					eventName);
				return false;
			}

			char command[384] = {};
			_snprintf_s(
				command, _countof(command), _TRUNCATE,
				"SetEventHandler \"%s\" (CompileScript \"%s\")",
				eventName, handlerScript);
			if (!g_consoleInterface->RunScriptLine2(command, nullptr, true))
			{
				gLog.FormattedMessage(
					"tnvse_multibyte_input: failed to attach Dialogue History UDF %s to event %s",
					handlerScript,
					eventName);
				return false;
			}
			return true;
		}
	}

	bool InitializeDialogueHistoryInputBridge()
	{
		if (s_bridgeReady || s_bridgeInitializationAttempted)
			return s_bridgeReady;
		s_bridgeInitializationAttempted = true;
		if (!g_bMultibyteInputDialogueHistory)
			return false;
		if (!g_bSuppressJIPKeyEventsDuringMultibyteInput
			|| !IsJipKeyEventSuppressionHookInstalled())
		{
			gLog.FormattedMessage("tnvse_multibyte_input: Dialogue History adapter disabled because the JIP 57.30 key-event suppression hook is unavailable");
			return false;
		}
		if (!g_eventInterface || !g_eventInterface->RegisterEvent
			|| !g_eventInterface->DispatchEvent
			|| !g_consoleInterface
			|| g_consoleInterface->version < NVSEConsoleInterface::kVersion
			|| !g_consoleInterface->RunScriptLine2)
		{
			gLog.FormattedMessage("tnvse_multibyte_input: Dialogue History adapter disabled because the xNVSE event/console interfaces are unavailable");
			return false;
		}
		if (!HasRequiredScripts())
		{
			gLog.FormattedMessage("tnvse_multibyte_input: Dialogue History scripts not found; adapter not installed");
			return false;
		}

		const bool searchRegistered = RegisterEvent(
			kSearchChangedEvent,
			NVSEEventManagerInterface::eParamType_String,
			"DialogueHistory\\Search.gek");
		const bool keyRegistered = RegisterEvent(
			kKeyDownEvent,
			NVSEEventManagerInterface::eParamType_Int,
			"DialogueHistory\\OnKeyDown.gek");
		const bool inputRegistered = RegisterEvent(
			kSearchInputEvent,
			NVSEEventManagerInterface::eParamType_Int,
			"DialogueHistory\\SearchInput.gek");
		s_bridgeReady = searchRegistered && keyRegistered && inputRegistered;
		if (s_bridgeReady)
			gLog.FormattedMessage("tnvse_multibyte_input: Dialogue History runtime input bridge installed");
		return s_bridgeReady;
	}

	DialogueHistoryInputTarget GetActiveDialogueHistoryInputTarget()
	{
		DialogueHistoryInputTarget target;
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
		Tile* dialogueHistory = FindDirectChild(root, "DialogueHistory");
		Tile* search = FindDirectChild(dialogueHistory, "Search");
		if (!root || !dialogueHistory || !search
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
		target.dialogueHistory = dialogueHistory;
		target.search = search;
		target.valid = true;
		return target;
	}

	DialogueHistoryInputTarget GetOverlayDialogueHistoryInputTarget()
	{
		return GetActiveDialogueHistoryInputTarget();
	}

	void ProcessDialogueHistoryInputTargetState()
	{
		if (!s_bridgeReady)
			return;
		const DialogueHistoryInputTarget target = GetActiveDialogueHistoryInputTarget();
		if (target.valid)
		{
			EnsureShadow(target);
			if (!s_targetObserved)
			{
				s_targetObserved = true;
				if (!State().textInputSessionActive)
					RefreshTextInputSessionForActiveTarget("dialogue_history_search_activate");
				DebugLog(
					"tnvse_multibyte_input_event: source=MainLoop action=dialogue_history_activate menu=0x%08X search=0x%08X",
					reinterpret_cast<UInt32>(target.menu),
					reinterpret_cast<UInt32>(target.search));
			}
			FlushPendingWork(target);
			return;
		}

		if (s_targetObserved)
		{
			s_targetObserved = false;
			s_shadow = {};
			s_pendingActions.clear();
			s_lastAscii = 0;
			s_lastAsciiTick = 0;
			if (!GetCurrentTextEditMenuObject()
				&& !GetOverlayStewieInputTarget().valid
				&& !GetOverlayMcmExtenderInputTarget().valid)
			{
				EndStewieTextInputSession("dialogue_history_search_deactivate");
			}
			DebugLog("tnvse_multibyte_input_event: source=MainLoop action=dialogue_history_deactivate");
		}
	}

	void ResetDialogueHistoryInputState()
	{
		s_targetObserved = false;
		s_lastAsciiTick = 0;
		s_lastAscii = 0;
		s_suppressedControlCharTick = 0;
		s_suppressedControlChar = 0;
		s_pendingActions.clear();
		s_shadow = {};
	}

	bool InsertWideTextDialogueHistory(
		const DialogueHistoryInputTarget& target,
		std::wstring_view text)
	{
		const std::string bytes = WideToCurrentCodePage(text);
		return !bytes.empty() && InsertBytes(target, bytes);
	}

	bool HandleDialogueHistoryWndProcChar(
		const DialogueHistoryInputTarget& target,
		WPARAM input)
	{
		if (!target.valid)
			return false;
		if (input < 0x20 || input == 0x7F || IsCtrlKeyDown())
			return true;
		if (input > 0xFFFF)
			return false;

		if (input <= 0x7E)
		{
			if (IsImeConsumingAscii())
			{
				ObserveImeCommitInput(static_cast<UInt32>(input));
				return true;
			}
			if (ShouldSuppressInputLanguageSwitchAscii(static_cast<UInt8>(input)))
				return true;
			const char value = static_cast<char>(input);
			InsertBytes(target, std::string_view(&value, 1));
			s_lastAscii = static_cast<UInt8>(input);
			s_lastAsciiTick = GetTickCount();
			DebugLog(
				"tnvse_multibyte_input_event: source=DialogueHistory.WM_CHAR action=insert_ascii input=0x%02X",
				static_cast<UInt32>(input));
			return true;
		}

		const wchar_t value = static_cast<wchar_t>(input);
		const bool inserted = InsertWideTextDialogueHistory(
			target, std::wstring_view(&value, 1));
		DebugLog(
			"tnvse_multibyte_input_event: source=DialogueHistory.WM_CHAR action=insert_nonascii input=0x%04X inserted=%u",
			static_cast<UInt32>(input), inserted ? 1 : 0);
		return true;
	}

	bool HandleDialogueHistoryKeyDown(
		const DialogueHistoryInputTarget& target,
		WPARAM virtualKey)
	{
		if (!target.valid)
			return false;
		if (IsImeCompositionActive())
			return false;
		EnsureShadow(target);
		switch (virtualKey)
		{
		case VK_BACK:
			return DeletePrevious(target);
		case VK_DELETE:
			return DeleteNext(target);
		case VK_LEFT:
			return MoveCaret(target, PrevCharBoundary(s_shadow.text, s_shadow.caret));
		case VK_RIGHT:
			return MoveCaret(target, NextCharBoundary(s_shadow.text, s_shadow.caret));
		case VK_HOME:
			return MoveCaret(target, 0);
		case VK_END:
			return MoveCaret(target, s_shadow.text.size());
		case VK_RETURN:
			s_suppressedControlChar = '\r';
			s_suppressedControlCharTick = GetTickCount();
			QueueAction(PendingAction::SearchInputEnter);
			return true;
		case VK_TAB:
			s_suppressedControlChar = '\t';
			s_suppressedControlCharTick = GetTickCount();
			QueueAction(PendingAction::KeyDownTab);
			return true;
		case 'F':
			if (IsCtrlKeyDown())
			{
				s_suppressedControlChar = 0x06;
				s_suppressedControlCharTick = GetTickCount();
				QueueAction(PendingAction::KeyDownCtrlF);
				return true;
			}
			break;
		default:
			break;
		}
		return false;
	}

	bool HandleDialogueHistoryMenuInput(Menu* menu, UInt32 input)
	{
		const DialogueHistoryInputTarget target = GetActiveDialogueHistoryInputTarget();
		if (!target.valid || target.menu != menu)
			return false;
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
			"tnvse_multibyte_input_event: source=DialogueHistory.MenuInput action=consume_keyboard_duplicate input=0x%08X",
			input);
		return true;
	}

	bool ShouldSuppressDialogueHistoryControlChar(WPARAM input)
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

	bool RemovePreviousDialogueHistoryAsciiCompositionEcho(wchar_t compositionLead)
	{
		if (!s_lastAscii
			|| GetTickCount() - s_lastAsciiTick > kDuplicateAsciiSuppressMs
			|| !AsciiEqualsIgnoreCase(s_lastAscii, compositionLead))
		{
			return false;
		}

		const DialogueHistoryInputTarget target = GetOverlayDialogueHistoryInputTarget();
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

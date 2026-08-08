#include "multibyte_input_internal.h"
#include "hook_identity.h"
#include "native_calls.h"

// Vanilla TextEditMenu editing plus the JIP LN text-input adapter.

namespace fonthook
{
	namespace multibyte_input
	{
		constexpr SIZE_T kPlayerNameEntryMenuTextEditMenuOpenCallSite = 0x7AB740;
		constexpr SIZE_T kVanillaTextEditMenuOpen = 0x7E6320;
		constexpr SIZE_T kPlayerNameEntryMenuIsValidName = 0x7AB820;
		constexpr SIZE_T kTextEditMenuVTable = 0x1070034;
		constexpr SIZE_T kTextEditMenuHandleKeyboardInput = 0x7E6620;
		constexpr SIZE_T kTextEditMenuHandleKeyboardInputVTableEntry = 0x1070064;
		constexpr SIZE_T kTextEditStateInputCallSiteInHandleKeyboardInput = 0x7E6685;
		constexpr SIZE_T kVanillaTextEditStateInput = 0x716B00;
		constexpr UInt32 kMaxTextEditRawBytes = 1023;
		constexpr UInt32 kJipNumericOnlyFlag = 1;
		constexpr UInt32 kJipEnterAcceptsOkFlag = 2;

		SIZE_T s_jipOriginalInputHandler = 0;
		SIZE_T s_jipObservedSuccessor = 0;
		bool s_jipAdapterPublished = false;
		thread_local UInt32 s_jipOriginalCallDepth = 0;

		class JipTextInputAdapterEx
		{
		public:
			static bool __fastcall Input(TextEditMenu* apMenu, void*, UInt32 aiInput);
		};

		void SetCaret(TextEditState& state, size_t offset)
		{
			state.iCaretByteOffset = static_cast<UInt32>(std::min<size_t>(
				offset,
				std::numeric_limits<UInt32>::max()));
			state.bCaretVisible = true;
		}

		bool FitsTextEditConstraints(TextEditState& state, const std::string& candidate)
		{
			if (candidate.size() > kMaxTextEditRawBytes)
				return false;

			if (state.iMaxPixelWidth != -1 && !state.FitsMaxPixelWidth(candidate.c_str()))
				return false;

			return true;
		}

		TextEditMenu* GetCurrentTextEditMenuObject()
		{
			TextEditMenu* current = TextEditMenu::GetCurrent();
			if (!current || *reinterpret_cast<SIZE_T*>(current) != kTextEditMenuVTable)
				return nullptr;

			return current;
		}

		TextEditMenu* GetActiveTextEditMenu()
		{
			TextEditMenu* current = GetCurrentTextEditMenuObject();
			if (!current)
				return nullptr;

			if (*reinterpret_cast<SIZE_T*>(
					kTextEditMenuHandleKeyboardInputVTableEntry)
				!= kTextEditMenuHandleKeyboardInput)
				return nullptr;

			if (!current->xEditState.IsActive())
				return nullptr;

			return current;
		}

		SIZE_T CurrentTextEditInputHandler()
		{
			return *reinterpret_cast<SIZE_T*>(
				kTextEditMenuHandleKeyboardInputVTableEntry);
		}

		SIZE_T JipTextInputHandlerAddress()
		{
			return reinterpret_cast<SIZE_T>(&JipTextInputAdapterEx::Input);
		}

		BSStringT<char>& JipCurrentText(TextEditMenu* menu)
		{
			return *reinterpret_cast<BSStringT<char>*>(reinterpret_cast<UInt8*>(menu) + 0x34);
		}

		BSStringT<char>& JipDisplayedText(TextEditMenu* menu)
		{
			return *reinterpret_cast<BSStringT<char>*>(reinterpret_cast<UInt8*>(menu) + 0x3C);
		}

		UInt32& JipCursorIndex(TextEditMenu* menu)
		{
			return *reinterpret_cast<UInt32*>(reinterpret_cast<UInt8*>(menu) + 0x44);
		}

		UInt16 JipMinLength(TextEditMenu* menu)
		{
			return *reinterpret_cast<UInt16*>(reinterpret_cast<UInt8*>(menu) + 0x48);
		}

		UInt16 JipMaxLength(TextEditMenu* menu)
		{
			return *reinterpret_cast<UInt16*>(reinterpret_cast<UInt8*>(menu) + 0x4A);
		}

		Tile* JipInputRect(TextEditMenu* menu)
		{
			return *reinterpret_cast<Tile**>(reinterpret_cast<UInt8*>(menu) + 0x4C);
		}

		UInt8& JipCursorVisible(TextEditMenu* menu)
		{
			return *reinterpret_cast<UInt8*>(reinterpret_cast<UInt8*>(menu) + 0x54);
		}

		UInt8 JipIsActiveFlag(TextEditMenu* menu)
		{
			return *reinterpret_cast<UInt8*>(reinterpret_cast<UInt8*>(menu) + 0x55);
		}

		UInt8 JipMiscFlags(TextEditMenu* menu)
		{
			return *reinterpret_cast<UInt8*>(reinterpret_cast<UInt8*>(menu) + 0x57);
		}

		bool LooksLikeJipTextInputStorage(TextEditMenu* menu)
		{
			if (!menu || *reinterpret_cast<SIZE_T*>(menu) != kTextEditMenuVTable)
				return false;

			const SIZE_T handler = CurrentTextEditInputHandler();
			if (handler == kTextEditMenuHandleKeyboardInput)
				return false;

			if (!JipCurrentText(menu).GetMaxLength() || !JipDisplayedText(menu).GetMaxLength())
				return false;

			const UInt16 maxLength = JipMaxLength(menu);
			if (!maxLength || maxLength > 0x7FFF)
				return false;

			const auto inputRect = reinterpret_cast<SIZE_T>(JipInputRect(menu));
			return inputRect > 0x10000;
		}

		bool LooksLikeJipTextInput(TextEditMenu* menu)
		{
			return LooksLikeJipTextInputStorage(menu) && JipIsActiveFlag(menu);
		}

		TextEditMenu* GetCurrentJipTextInputMenu()
		{
			TextEditMenu* current = TextEditMenu::GetCurrent();
			return LooksLikeJipTextInputStorage(current) ? current : nullptr;
		}

		TextEditMenu* GetActiveJipTextInputMenu()
		{
			TextEditMenu* current = TextEditMenu::GetCurrent();
			return LooksLikeJipTextInput(current) ? current : nullptr;
		}

		TextEditMenu* GetAnyActiveTextInputMenu()
		{
			if (TextEditMenu* menu = GetActiveTextEditMenu())
				return menu;

			return GetActiveJipTextInputMenu();
		}

		TextEditMenu* GetOverlayTextInputMenu()
		{
			if (TextEditMenu* menu = GetAnyActiveTextInputMenu())
				return menu;

			TextEditMenu* current = GetCurrentTextEditMenuObject();
			if (!current)
				return nullptr;

			return current;
		}

		void ClearJipTextInputHookState()
		{
			s_jipOriginalInputHandler = 0;
			s_jipObservedSuccessor = 0;
			s_jipAdapterPublished = false;
		}

		bool CallJipOriginalInput(TextEditMenu* menu, UInt32 input)
		{
			if (!menu || !s_jipOriginalInputHandler
				|| s_jipOriginalInputHandler == JipTextInputHandlerAddress()
				|| s_jipOriginalCallDepth)
				return false;

			struct OriginalCallGuard
			{
				OriginalCallGuard()
				{
					++s_jipOriginalCallDepth;
				}
				~OriginalCallGuard()
				{
					--s_jipOriginalCallDepth;
				}
			} guard;
			using InputHandler = bool(__thiscall*)(TextEditMenu*, UInt32);
			return reinterpret_cast<InputHandler>(s_jipOriginalInputHandler)(menu, input);
		}

		void TryInstallJipTextInputHook()
		{
			const SIZE_T currentHandler = CurrentTextEditInputHandler();
			const SIZE_T hookHandler = JipTextInputHandlerAddress();

			if (currentHandler == kTextEditMenuHandleKeyboardInput)
			{
				ClearJipTextInputHookState();
				return;
			}

			if (currentHandler == hookHandler)
			{
				s_jipAdapterPublished = true;
				return;
			}

			if (s_jipAdapterPublished)
			{
				// Another adapter was published after ours.  Replacing it while
				// it still chains to tNVSE would create a two-node recursive
				// vtable loop.  Keep our saved predecessor and stay below the
				// later owner until the slot returns to the vanilla handler.
				if (currentHandler != s_jipObservedSuccessor)
				{
					s_jipObservedSuccessor = currentHandler;
					gLog.FormattedMessage(
						"tnvse_multibyte_input: JIP TextInput adapter left below later handler=0x%08X",
						static_cast<UInt32>(currentHandler));
				}
				return;
			}

			TextEditMenu* current = TextEditMenu::GetCurrent();
			if (!LooksLikeJipTextInput(current))
				return;

			s_jipOriginalInputHandler = currentHandler;
			SafeWrite32(kTextEditMenuHandleKeyboardInputVTableEntry, hookHandler);
			if (CurrentTextEditInputHandler() != hookHandler)
			{
				ClearJipTextInputHookState();
				return;
			}
			s_jipAdapterPublished = true;
			DebugLog(
				"tnvse_multibyte_input: chained JIP TextInput handler=0x%08X menu=0x%08X",
				static_cast<UInt32>(currentHandler),
				reinterpret_cast<UInt32>(current));
		}

		std::string GetJipText(TextEditMenu* menu)
		{
			BSStringT<char>& text = JipCurrentText(menu);
			const UInt32 length = text.GetLength();
			if (!length)
				return {};

			return std::string(text.c_str(), length);
		}

		void DebugLogJipState(const char* source, const char* action, TextEditMenu* menu, UInt32 input)
		{
			if (!g_bMultibyteInputLog)
				return;

			const UInt32 textLen = menu ? JipCurrentText(menu).GetLength() : 0;
			const UInt32 caret = menu ? JipCursorIndex(menu) : 0;
			gLog.FormattedMessage(
				"tnvse_multibyte_input_event: source=%s action=%s input=0x%08X ascii='%c' composing=%u jip=1 active=0x%08X current=0x%08X caret=%u textLen=%u min=%u max=%u flags=0x%02X handler=0x%08X",
				source,
				action,
				input,
				PrintableAscii(input),
				s_imeComposing ? 1 : 0,
				reinterpret_cast<UInt32>(menu),
				reinterpret_cast<UInt32>(TextEditMenu::GetCurrent()),
				caret,
				textLen,
				menu ? JipMinLength(menu) : 0,
				menu ? JipMaxLength(menu) : 0,
				menu ? JipMiscFlags(menu) : 0,
				static_cast<UInt32>(CurrentTextEditInputHandler()));
		}

		bool JipNumericInsertIsValid(TextEditMenu* menu, std::string_view text, const std::string& current, size_t caret)
		{
			if (!(JipMiscFlags(menu) & kJipNumericOnlyFlag))
				return true;

			for (char raw : text)
			{
				const UInt8 ch = static_cast<UInt8>(raw);
				if (ch >= 0x80)
					return false;

				if (ch == '-')
				{
					if (caret != 0 || current.find('-') != std::string::npos)
						return false;
					continue;
				}

				if (ch == '.')
				{
					if (current.find('.') != std::string::npos)
						return false;
					continue;
				}

				if (!std::isdigit(ch))
					return false;
			}

			return true;
		}

		void RefreshJipTextInput(TextEditMenu* menu)
		{
			if (!menu)
				return;

			std::string current = GetJipText(menu);
			size_t caret = ClampToPrevBoundary(current, JipCursorIndex(menu));
			JipCursorIndex(menu) = static_cast<UInt32>(caret);

			std::string displayText;
			displayText.reserve(current.size() + 1);
			displayText.append(current, 0, caret);
			displayText.push_back(JipCursorVisible(menu) ? '|' : '\x7F');
			displayText.append(current, caret, std::string::npos);
			JipDisplayedText(menu).Set(displayText.c_str());

			if (menu->pEditText)
				menu->pEditText->SetValueString(Tile::kTileValue_string, JipDisplayedText(menu).c_str(), true);

			if (menu->pOkButton)
			{
				const bool enabled = JipCurrentText(menu).GetLength() >= JipMinLength(menu);
				menu->pOkButton->SetValueFloat(Tile::kTileValue_target, enabled ? 1.0f : 0.0f, true);
			}

			if (Tile* inputRect = JipInputRect(menu))
			{
				const float user1 = inputRect->GetValueFloat(Tile::kTileValue_user1);
				inputRect->SetValueFloat(Tile::kTileValue_user2, user1, true);
			}
		}

		bool CommitJipCandidate(TextEditMenu* menu, const std::string& candidate, size_t caret)
		{
			if (!menu)
				return false;

			const UInt32 maxLength = std::min<UInt32>(JipMaxLength(menu), kMaxTextEditRawBytes);
			if (candidate.size() > maxLength)
				return false;

			JipCurrentText(menu).Set(candidate.c_str());
			JipCursorIndex(menu) = static_cast<UInt32>(ClampToPrevBoundary(candidate, caret));
			RefreshJipTextInput(menu);
			return true;
		}

		bool InsertJipTextAtCaret(TextEditMenu* menu, std::string_view text)
		{
			if (!menu || text.empty())
				return false;

			std::string current = GetJipText(menu);
			size_t caret = ClampToPrevBoundary(current, JipCursorIndex(menu));
			if (!JipNumericInsertIsValid(menu, text, current, caret))
				return false;

			std::string candidate;
			candidate.reserve(current.size() + text.size());
			candidate.append(current, 0, caret);
			candidate.append(text.data(), text.size());
			candidate.append(current, caret, std::string::npos);
			return CommitJipCandidate(menu, candidate, caret + text.size());
		}

		bool DeletePreviousJipChar(TextEditMenu* menu)
		{
			std::string current = GetJipText(menu);
			size_t caret = ClampToPrevBoundary(current, JipCursorIndex(menu));
			if (!caret)
				return true;

			const size_t previous = PrevCharBoundary(current, caret);
			current.erase(previous, caret - previous);
			return CommitJipCandidate(menu, current, previous);
		}

		bool DeleteNextJipChar(TextEditMenu* menu)
		{
			std::string current = GetJipText(menu);
			size_t caret = ClampToPrevBoundary(current, JipCursorIndex(menu));
			if (caret >= current.size())
				return true;

			const size_t next = NextCharBoundary(current, caret);
			current.erase(caret, next - caret);
			return CommitJipCandidate(menu, current, caret);
		}

		bool MoveJipCaret(TextEditMenu* menu, size_t caret)
		{
			const std::string current = GetJipText(menu);
			JipCursorIndex(menu) = static_cast<UInt32>(ClampToPrevBoundary(current, caret));
			RefreshJipTextInput(menu);
			return true;
		}

		bool MoveJipCaretPrevious(TextEditMenu* menu)
		{
			const std::string current = GetJipText(menu);
			const size_t caret = ClampToPrevBoundary(current, JipCursorIndex(menu));
			return MoveJipCaret(menu, PrevCharBoundary(current, caret));
		}

		bool MoveJipCaretNext(TextEditMenu* menu)
		{
			const std::string current = GetJipText(menu);
			return MoveJipCaret(menu, NextCharBoundary(current, JipCursorIndex(menu)));
		}

		bool MoveJipCaretLineStart(TextEditMenu* menu)
		{
			const std::string current = GetJipText(menu);
			size_t caret = ClampToPrevBoundary(current, JipCursorIndex(menu));
			if (!caret)
				return MoveJipCaret(menu, 0);

			const size_t lineStart = current.rfind('\n', caret ? caret - 1 : 0);
			return MoveJipCaret(menu, lineStart == std::string::npos ? 0 : lineStart + 1);
		}

		bool MoveJipCaretLineEnd(TextEditMenu* menu)
		{
			const std::string current = GetJipText(menu);
			const size_t caret = ClampToPrevBoundary(current, JipCursorIndex(menu));
			const size_t lineEnd = current.find('\n', caret);
			return MoveJipCaret(menu, lineEnd == std::string::npos ? current.size() : lineEnd);
		}

		bool MoveJipCaretByChars(TextEditMenu* menu, int count)
		{
			const std::string current = GetJipText(menu);
			size_t caret = ClampToPrevBoundary(current, JipCursorIndex(menu));
			if (count < 0)
			{
				for (int i = 0; i < -count; ++i)
					caret = PrevCharBoundary(current, caret);
			}
			else
			{
				for (int i = 0; i < count; ++i)
					caret = NextCharBoundary(current, caret);
			}

			return MoveJipCaret(menu, caret);
		}

		bool InsertWideTextJip(TextEditMenu* menu, std::wstring_view value)
		{
			std::string converted = WideToCurrentCodePage(value);
			if (converted.empty())
				return false;

			return InsertJipTextAtCaret(menu, converted);
		}

		bool IsJipCompositionControl(UInt32 input)
		{
			switch (input)
			{
			case kInputCode_Backspace:
			case kInputCode_Delete:
			case kInputCode_ArrowLeft:
			case kInputCode_ArrowRight:
			case kInputCode_Home:
			case kInputCode_End:
			case kInputCode_PageUp:
			case kInputCode_PageDown:
			case kInputCode_Enter:
				return true;
			default:
				return false;
			}
		}

		bool __fastcall JipTextInputAdapterEx::Input(TextEditMenu* apMenu, void*, UInt32 aiInput)
		{
			if (!LooksLikeJipTextInputStorage(apMenu))
				return CallJipOriginalInput(apMenu, aiInput);

			GameInputFilterClass inputClass = GameInputFilterClass::None;
			if (aiInput >= 0x20 && aiInput <= 0x7E)
				inputClass = GameInputFilterClass::PrintableAscii;
			else if (IsJipCompositionControl(aiInput))
				inputClass = GameInputFilterClass::CompositionControl;
			const GameInputFilterResult filterResult = FilterGameInput(
				aiInput,
				ImeCommitInputChannel::JipTextInput,
				inputClass);
			if (filterResult != GameInputFilterResult::Pass)
			{
				const char* action =
					filterResult == GameInputFilterResult::SuppressImeCommit
						? "suppress_ime_commit_key"
						: (filterResult == GameInputFilterResult::SuppressCompositionAscii
							? "suppress_composition_ascii"
							: "suppress_composition_control");
				DebugLogJipState(
					"JipTextInputAdapter::Input",
					action,
					apMenu,
					aiInput);
				return true;
			}

			const bool editActive = JipIsActiveFlag(apMenu) != 0;
			if (aiInput >= 0x20 && aiInput <= 0x7E)
			{
				if (!editActive)
					return CallJipOriginalInput(apMenu, aiInput);

				const UInt8 asciiInput = ResolveAsciiLetterCaseFromKeyboard(static_cast<UInt8>(aiInput));
				if (AsciiEqualsIgnoreCase(s_lastWndProcAsciiChar, asciiInput)
					&& GetTickCount() - s_lastWndProcAsciiTick <= kDuplicateAsciiSuppressMs)
				{
					s_lastWndProcAsciiChar = 0;
					DebugLogJipState("JipTextInputAdapter::Input", "suppress_duplicate_wndproc_ascii", apMenu, aiInput);
					return true;
				}

				const char ch = static_cast<char>(asciiInput);
				if (!InsertJipTextAtCaret(apMenu, std::string_view(&ch, 1)))
				{
					DebugLogJipState("JipTextInputAdapter::Input", "reject_ascii_insert", apMenu, aiInput);
					return false;
				}

				DebugLogJipState("JipTextInputAdapter::Input", "insert_ascii", apMenu, asciiInput);
				return true;
			}

			if (aiInput > 0x7F && aiInput < kInputCode_Backspace)
			{
				DebugLogJipState("JipTextInputAdapter::Input", "swallow_high_byte", apMenu, aiInput);
				return true;
			}

			if (!editActive)
				return CallJipOriginalInput(apMenu, aiInput);

			switch (aiInput)
			{
			case kInputCode_Backspace:
				DebugLogJipState("JipTextInputAdapter::Input", "delete_previous", apMenu, aiInput);
				return DeletePreviousJipChar(apMenu);
			case kInputCode_Delete:
				DebugLogJipState("JipTextInputAdapter::Input", "delete_next", apMenu, aiInput);
				return DeleteNextJipChar(apMenu);
			case kInputCode_ArrowLeft:
				DebugLogJipState("JipTextInputAdapter::Input", "move_left", apMenu, aiInput);
				return MoveJipCaretPrevious(apMenu);
			case kInputCode_ArrowRight:
				DebugLogJipState("JipTextInputAdapter::Input", "move_right", apMenu, aiInput);
				return MoveJipCaretNext(apMenu);
			case kInputCode_Home:
				DebugLogJipState("JipTextInputAdapter::Input", "move_home", apMenu, aiInput);
				return MoveJipCaretLineStart(apMenu);
			case kInputCode_End:
				DebugLogJipState("JipTextInputAdapter::Input", "move_end", apMenu, aiInput);
				return MoveJipCaretLineEnd(apMenu);
			case kInputCode_Enter:
				if (JipMiscFlags(apMenu) & kJipEnterAcceptsOkFlag)
				{
					DebugLogJipState("JipTextInputAdapter::Input", "pass_enter_to_jip", apMenu, aiInput);
					return CallJipOriginalInput(apMenu, aiInput);
				}
				DebugLogJipState("JipTextInputAdapter::Input", "insert_newline", apMenu, aiInput);
				return InsertJipTextAtCaret(apMenu, std::string_view("\n", 1));
			case kInputCode_PageUp:
				DebugLogJipState("JipTextInputAdapter::Input", "page_up", apMenu, aiInput);
				return MoveJipCaretByChars(apMenu, -5);
			case kInputCode_PageDown:
				DebugLogJipState("JipTextInputAdapter::Input", "page_down", apMenu, aiInput);
				return MoveJipCaretByChars(apMenu, 5);
			case kInputCode_ArrowUp:
			case kInputCode_ArrowDown:
				DebugLogJipState("JipTextInputAdapter::Input", "pass_vertical_arrow_to_jip", apMenu, aiInput);
				return CallJipOriginalInput(apMenu, aiInput);
			default:
				DebugLogJipState("JipTextInputAdapter::Input", "pass_original", apMenu, aiInput);
				return CallJipOriginalInput(apMenu, aiInput);
			}
		}

		bool CommitCandidate(TextEditState& state, const std::string& candidate, size_t caret)
		{
			if (!FitsTextEditConstraints(state, candidate))
				return false;

			state.SetText(candidate.c_str());
			SetCaret(state, ClampToPrevBoundary(candidate, caret));
			state.bClearOnNextType = false;
			return true;
		}

		bool CommitCandidate(TextEditMenu* menu, const std::string& candidate, size_t caret)
		{
			if (!menu || !CommitCandidate(menu->xEditState, candidate, caret))
				return false;

			menu->Refresh();
			return true;
		}

		bool InsertTextAtCaret(TextEditState& state, std::string_view text)
		{
			if (text.empty())
				return false;

			std::string current = GetText(state);
			size_t caret = ClampToPrevBoundary(current, state.iCaretByteOffset);

			if (state.bClearOnNextType)
			{
				current.clear();
				caret = 0;
			}

			std::string candidate;
			candidate.reserve(current.size() + text.size());
			candidate.append(current, 0, caret);
			candidate.append(text.data(), text.size());
			candidate.append(current, caret, std::string::npos);

			return CommitCandidate(state, candidate, caret + text.size());
		}

		bool InsertTextAtCaret(TextEditMenu* menu, std::string_view text)
		{
			if (!menu)
				return false;

			if (!InsertTextAtCaret(menu->xEditState, text))
				return false;

			menu->Refresh();
			return true;
		}

		bool DeletePreviousChar(TextEditState& state)
		{
			std::string current = GetText(state);
			size_t caret = ClampToPrevBoundary(current, state.iCaretByteOffset);
			if (!caret)
				return true;

			const size_t previous = PrevCharBoundary(current, caret);
			current.erase(previous, caret - previous);
			return CommitCandidate(state, current, previous);
		}

		bool DeleteNextChar(TextEditState& state)
		{
			std::string current = GetText(state);
			size_t caret = ClampToPrevBoundary(current, state.iCaretByteOffset);
			if (caret >= current.size())
				return true;

			const size_t next = NextCharBoundary(current, caret);
			current.erase(caret, next - caret);
			return CommitCandidate(state, current, caret);
		}

		bool MoveCaretPrevious(TextEditState& state)
		{
			const std::string current = GetText(state);
			SetCaret(state, PrevCharBoundary(current, ClampToPrevBoundary(current, state.iCaretByteOffset)));
			return true;
		}

		bool MoveCaretNext(TextEditState& state)
		{
			const std::string current = GetText(state);
			SetCaret(state, NextCharBoundary(current, state.iCaretByteOffset));
			return true;
		}

		bool MoveCaretHome(TextEditState& state)
		{
			SetCaret(state, 0);
			return true;
		}

		bool MoveCaretEnd(TextEditState& state)
		{
			const std::string current = GetText(state);
			SetCaret(state, current.size());
			return true;
		}

		bool RemovePreviousAsciiCompositionEcho(TextEditState& state, wchar_t compositionLead)
		{
			std::string current = GetText(state);
			size_t caret = ClampToPrevBoundary(current, state.iCaretByteOffset);
			if (!caret)
				return false;

			const size_t previous = PrevCharBoundary(current, caret);
			if (caret - previous != 1)
				return false;

			if (!AsciiEqualsIgnoreCase(static_cast<UInt8>(current[previous]), compositionLead))
				return false;

			current.erase(previous, 1);
			return CommitCandidate(state, current, previous);
		}

		bool RemovePreviousJipAsciiCompositionEcho(TextEditMenu* menu, wchar_t compositionLead)
		{
			if (!LooksLikeJipTextInputStorage(menu))
				return false;

			std::string current = GetJipText(menu);
			size_t caret = ClampToPrevBoundary(current, JipCursorIndex(menu));
			if (!caret)
				return false;

			const size_t previous = PrevCharBoundary(current, caret);
			if (caret - previous != 1)
				return false;

			if (!AsciiEqualsIgnoreCase(static_cast<UInt8>(current[previous]), compositionLead))
				return false;

			current.erase(previous, 1);
			return CommitJipCandidate(menu, current, previous);
		}


		bool InsertWideText(TextEditMenu* menu, std::wstring_view value)
		{
			std::string converted = WideToCurrentCodePage(value);
			if (converted.empty())
				return false;

			return InsertTextAtCaret(menu, converted);
		}

		bool ValidatePlayerName(const char* text)
		{
			if (!text || !*text)
				return false;

			bool hasVisibleCharacter = false;
			const size_t length = std::strlen(text);

			for (size_t i = 0; i < length;)
			{
				const UInt8 ch = static_cast<UInt8>(text[i]);
				if (ch >= 0x80)
				{
					UInt32 code = 0;
					if (i + 1 >= length || !TryDecodeDoubleByte(&text[i], code))
						return false;

					hasVisibleCharacter = true;
					i += 2;
					continue;
				}

				UInt8 converted = ch;
				ConvertToAsciiQuotes(&converted);
				if (converted == '\\' || converted == '~')
					return false;

				if (converted != ' ')
					hasVisibleCharacter = true;

				++i;
			}

			return hasVisibleCharacter;
		}


		class TextEditMenuEx : public TextEditMenu
		{
		public:
			static bool __cdecl Open(const char* apTitle, const char* apInitialText, ValidateTextCallback apValidateText)
			{
				ValidateTextCallback validateText = apValidateText;
				if (reinterpret_cast<SIZE_T>(apValidateText)
					== kPlayerNameEntryMenuIsValidName)
					validateText = &ValidatePlayerName;

				const bool opened = TextEditMenu::Open(apTitle, apInitialText, validateText);
				if (opened && s_window)
					SetTextInputSessionActive(true);
				DebugLog(
					"tnvse_multibyte_input: TextEditMenu::Open opened=%u title=\"%s\" initialLen=%u menu=0x%08X",
					opened ? 1 : 0,
					apTitle ? apTitle : "",
					apInitialText ? static_cast<UInt32>(std::strlen(apInitialText)) : 0,
					reinterpret_cast<UInt32>(opened ? TextEditMenu::GetCurrent() : nullptr));
				return opened;
			}
		};

		bool IsTextEditCompositionControl(SInt32 input)
		{
			switch (input)
			{
			case kTextEditInput_Backspace:
			case kTextEditInput_Delete:
			case kTextEditInput_Left:
			case kTextEditInput_Right:
			case kTextEditInput_Home:
			case kTextEditInput_End:
			case kTextEditInput_Confirm:
				return true;
			default:
				return false;
			}
		}

		class TextEditStateEx : public TextEditState
		{
		public:
			static void __fastcall Input(TextEditState* apState, void*, SInt32 aiInput, SInt32 aiChar)
			{
				if (!apState || !apState->IsActive())
					return;

				TextEditMenu* menu = GetActiveTextEditMenu();
				if (menu && &menu->xEditState != apState)
					menu = nullptr;

				GameInputFilterClass inputClass = GameInputFilterClass::None;
				if (aiInput >= 0x20 && aiInput <= 0x7E)
					inputClass = GameInputFilterClass::PrintableAscii;
				else if (IsTextEditCompositionControl(aiInput))
					inputClass = GameInputFilterClass::CompositionControl;
				const GameInputFilterResult filterResult = FilterGameInput(
					static_cast<UInt32>(aiInput),
					ImeCommitInputChannel::TextEdit,
					inputClass);
				if (filterResult != GameInputFilterResult::Pass)
				{
					const char* action =
						filterResult == GameInputFilterResult::SuppressImeCommit
							? "suppress_ime_commit_key"
							: (filterResult == GameInputFilterResult::SuppressCompositionAscii
								? "suppress_composition_ascii"
								: "suppress_composition_control");
					DebugLogState(
						"TextEditState::Input",
						action,
						menu,
						aiInput);
					return;
				}

				if (aiInput >= 0x20 && aiInput <= 0x7E)
				{
					const UInt8 asciiInput = ResolveAsciiLetterCaseFromKeyboard(static_cast<UInt8>(aiInput));
					if (AsciiEqualsIgnoreCase(s_lastWndProcAsciiChar, asciiInput)
						&& GetTickCount() - s_lastWndProcAsciiTick <= kDuplicateAsciiSuppressMs)
					{
						s_lastWndProcAsciiChar = 0;
						DebugLogState("TextEditState::Input", "suppress_duplicate_wndproc_ascii", menu, aiInput);
						return;
					}

					const char ch = static_cast<char>(asciiInput);
					if (!InsertTextAtCaret(*apState, std::string_view(&ch, 1)))
					{
						DebugLogState("TextEditState::Input", "reject_ascii_insert", menu, aiInput);
						return;
					}

					DebugLogState("TextEditState::Input", "insert_ascii", menu, asciiInput);
					return;
				}

				if (aiInput > 0x7F && aiInput <= 0xFF)
				{
					DebugLogState("TextEditState::Input", "swallow_high_byte", menu, aiInput);
					return;
				}

				switch (aiInput)
				{
				case kTextEditInput_Backspace:
					DebugLogState("TextEditState::Input", "delete_previous", menu, aiInput);
					DeletePreviousChar(*apState);
					return;
				case kTextEditInput_Delete:
					DebugLogState("TextEditState::Input", "delete_next", menu, aiInput);
					DeleteNextChar(*apState);
					return;
				case kTextEditInput_Left:
					DebugLogState("TextEditState::Input", "move_left", menu, aiInput);
					MoveCaretPrevious(*apState);
					return;
				case kTextEditInput_Right:
					DebugLogState("TextEditState::Input", "move_right", menu, aiInput);
					MoveCaretNext(*apState);
					return;
				case kTextEditInput_Home:
					DebugLogState("TextEditState::Input", "move_home", menu, aiInput);
					MoveCaretHome(*apState);
					return;
				case kTextEditInput_End:
					DebugLogState("TextEditState::Input", "move_end", menu, aiInput);
					MoveCaretEnd(*apState);
					return;
				case kTextEditInput_Confirm:
					DebugLogState("TextEditState::Input", "pass_original", menu, aiInput);
					apState->Input(aiInput, aiChar);
					return;
				default:
					DebugLogState("TextEditState::Input", "pass_original", menu, aiInput);
					apState->Input(aiInput, aiChar);
					return;
				}
			}
		};

		void InstallTextEditHooks()
		{
			using hook_identity::Rel32Opcode;
			SIZE_T openTarget = 0;
			SIZE_T inputTarget = 0;
			if (!hook_identity::ReadRel32Target(
					kPlayerNameEntryMenuTextEditMenuOpenCallSite,
					Rel32Opcode::Call,
					openTarget)
				|| openTarget != kVanillaTextEditMenuOpen
				|| !hook_identity::ReadRel32Target(
					kTextEditStateInputCallSiteInHandleKeyboardInput,
					Rel32Opcode::Call,
					inputTarget)
				|| inputTarget != kVanillaTextEditStateInput)
			{
				gLog.FormattedMessage(
					"tnvse_multibyte_input: TextEdit hook identity mismatch open=%08X input=%08X; disabled",
					static_cast<UInt32>(openTarget),
					static_cast<UInt32>(inputTarget));
				return;
			}

			WriteRelCall(kPlayerNameEntryMenuTextEditMenuOpenCallSite,
				&TextEditMenuEx::Open);
			WriteRelCall(kTextEditStateInputCallSiteInHandleKeyboardInput,
				&TextEditStateEx::Input);
			const bool installed = hook_identity::MatchesRel32Target(
				kPlayerNameEntryMenuTextEditMenuOpenCallSite,
				Rel32Opcode::Call,
				reinterpret_cast<SIZE_T>(&TextEditMenuEx::Open))
				&& hook_identity::MatchesRel32Target(
					kTextEditStateInputCallSiteInHandleKeyboardInput,
					Rel32Opcode::Call,
					reinterpret_cast<SIZE_T>(&TextEditStateEx::Input));
			if (!installed)
			{
				WriteRelCall(kPlayerNameEntryMenuTextEditMenuOpenCallSite,
					kVanillaTextEditMenuOpen);
				WriteRelCall(kTextEditStateInputCallSiteInHandleKeyboardInput,
					kVanillaTextEditStateInput);
				gLog.FormattedMessage(
					"tnvse_multibyte_input: TextEdit hook write verification failed; restored vanilla targets");
			}
		}

		void RestoreTextEditInputHook()
		{
			if (CurrentTextEditInputHandler() == JipTextInputHandlerAddress())
			{
				const SIZE_T predecessor = s_jipOriginalInputHandler
					&& s_jipOriginalInputHandler != JipTextInputHandlerAddress()
					? s_jipOriginalInputHandler
					: kTextEditMenuHandleKeyboardInput;
				SafeWrite32(
					kTextEditMenuHandleKeyboardInputVTableEntry, predecessor);
			}

			ClearJipTextInputHookState();
		}
	}
}

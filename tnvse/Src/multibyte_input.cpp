#include "multibyte_input.h"

#include "encoding.h"
#include "load_config.h"
#include "SafeWrite.h"
#include "tnvse.h"
#include "Tile.hpp"
#include "ui_decode.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <Windows.h>
#include <Imm.h>

namespace fonthook
{
	namespace
	{
		constexpr SIZE_T kPlayerNameTextEditOpenCall = 0x7AB740;
		constexpr SIZE_T kPlayerNameIsValidName = 0x7AB820;
		constexpr SIZE_T kTextEditMenuVTable = 0x1070034;
		constexpr SIZE_T kTextEditMenuHandleKeyboardInput = 0x7E6620;
		constexpr SIZE_T kTextEditMenuInputVTableEntry = 0x1070064;
		constexpr SIZE_T kTextEditStateInputCallInHandleKeyboardInput = 0x7E6685;
		constexpr UInt32 kMaxTextEditRawBytes = 1023;
		constexpr UInt32 kJipNumericOnlyFlag = 1;
		constexpr UInt32 kJipEnterAcceptsOkFlag = 2;
		constexpr DWORD kDuplicateImeCharSuppressMs = 250;
		constexpr DWORD kDuplicateAsciiSuppressMs = 100;

		constexpr UInt32 kInputCode_Backspace = 0x80000000;
		constexpr UInt32 kInputCode_ArrowLeft = 0x80000001;
		constexpr UInt32 kInputCode_ArrowRight = 0x80000002;
		constexpr UInt32 kInputCode_ArrowUp = 0x80000003;
		constexpr UInt32 kInputCode_ArrowDown = 0x80000004;
		constexpr UInt32 kInputCode_Home = 0x80000005;
		constexpr UInt32 kInputCode_End = 0x80000006;
		constexpr UInt32 kInputCode_Delete = 0x80000007;
		constexpr UInt32 kInputCode_Enter = 0x80000008;
		constexpr UInt32 kInputCode_PageUp = 0x80000009;
		constexpr UInt32 kInputCode_PageDown = 0x8000000A;

		HWND s_window = nullptr;
		WNDPROC s_originalWndProc = nullptr;
		bool s_initialized = false;
		bool s_hooksInstalled = false;
		bool s_imeComposing = false;
		TextEditMenu* s_activeTextEditMenu = nullptr;
		DWORD s_lastImeCommitTick = 0;
		DWORD s_lastWndProcAsciiTick = 0;
		UInt8 s_lastWndProcAsciiChar = 0;
		UInt32 s_suppressedImeCharCount = 0;
		SIZE_T s_jipOriginalInputHandler = 0;

		class JipTextInputAdapterEx
		{
		public:
			static bool __fastcall Input(TextEditMenu* apMenu, void*, UInt32 aiInput);
		};

		bool IsImeCompositionActive();
		bool IsImeConsumingAscii();
		std::string WideToCurrentCodePage(std::wstring_view value);

		void DebugLog(const char* fmt, ...)
		{
			if (!g_bMultibyteInputDebug)
				return;

			va_list args;
			va_start(args, fmt);
			gLog.FormattedMessage(fmt, args);
			va_end(args);
		}

		char PrintableAscii(UInt32 value)
		{
			return (value >= 0x20 && value <= 0x7E) ? static_cast<char>(value) : '.';
		}

		void DebugLogState(const char* source, const char* action, TextEditMenu* menu, SInt32 input)
		{
			if (!g_bMultibyteInputDebug)
				return;

			const UInt32 textLen = menu ? menu->xEditState.xText.GetLength() : 0;
			const UInt32 caret = menu ? menu->xEditState.iCaretByteOffset : 0;
			gLog.FormattedMessage(
				"tnvse_multibyte_input_event: source=%s action=%s input=0x%08X ascii='%c' composing=%u active=0x%08X current=0x%08X caret=%u textLen=%u",
				source,
				action,
				static_cast<UInt32>(input),
				PrintableAscii(static_cast<UInt32>(input)),
				s_imeComposing ? 1 : 0,
				reinterpret_cast<UInt32>(menu),
				reinterpret_cast<UInt32>(TextEditMenu::GetCurrent()),
				caret,
				textLen);
		}

		std::string GetText(const TextEditState& state)
		{
			const UInt32 length = state.xText.GetLength();
			if (!length)
				return {};

			return std::string(state.xText.c_str(), length);
		}

		size_t NextOffset(const std::string& text, size_t offset)
		{
			if (offset >= text.size())
				return text.size();

			UInt32 code = 0;
			if (offset + 1 < text.size() && TryDecodeDoubleByte(&text[offset], code))
				return offset + 2;

			return offset + 1;
		}

		bool IsCharBoundary(const std::string& text, size_t offset)
		{
			if (offset > text.size())
				return false;

			size_t current = 0;
			while (current < text.size())
			{
				if (current == offset)
					return true;

				current = NextOffset(text, current);
			}

			return offset == text.size();
		}

		size_t PrevCharBoundary(const std::string& text, size_t offset)
		{
			offset = std::min(offset, text.size());
			size_t previous = 0;
			size_t current = 0;
			while (current < offset)
			{
				previous = current;
				current = NextOffset(text, current);
			}

			return previous;
		}

		size_t ClampToPrevBoundary(const std::string& text, size_t offset)
		{
			offset = std::min(offset, text.size());
			if (IsCharBoundary(text, offset))
				return offset;

			return PrevCharBoundary(text, offset);
		}

		size_t NextCharBoundary(const std::string& text, size_t offset)
		{
			offset = ClampToPrevBoundary(text, offset);
			return NextOffset(text, offset);
		}

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

		TextEditMenu* GetActiveTextEditMenu()
		{
			TextEditMenu* current = TextEditMenu::GetCurrent();
			if (!current || *reinterpret_cast<SIZE_T*>(current) != kTextEditMenuVTable)
			{
				s_activeTextEditMenu = nullptr;
				return nullptr;
			}

			if (*reinterpret_cast<SIZE_T*>(kTextEditMenuInputVTableEntry) != kTextEditMenuHandleKeyboardInput)
			{
				s_activeTextEditMenu = nullptr;
				return nullptr;
			}

			if (!current->xEditState.IsActive())
				return nullptr;

			s_activeTextEditMenu = current;
			return current;
		}

		SIZE_T CurrentTextEditInputHandler()
		{
			return *reinterpret_cast<SIZE_T*>(kTextEditMenuInputVTableEntry);
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

		UInt32& JipCursorBlink(TextEditMenu* menu)
		{
			return *reinterpret_cast<UInt32*>(reinterpret_cast<UInt8*>(menu) + 0x50);
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

		bool LooksLikeJipTextInput(TextEditMenu* menu)
		{
			if (!menu || *reinterpret_cast<SIZE_T*>(menu) != kTextEditMenuVTable)
				return false;

			const SIZE_T handler = CurrentTextEditInputHandler();
			if (handler == kTextEditMenuHandleKeyboardInput)
				return false;

			if (!JipIsActiveFlag(menu))
				return false;

			if (JipCurrentText(menu).GetMaxLength() < 0x400 || JipDisplayedText(menu).GetMaxLength() < 0x400)
				return false;

			const UInt16 maxLength = JipMaxLength(menu);
			if (!maxLength || maxLength > 0x7FFF)
				return false;

			const auto inputRect = reinterpret_cast<SIZE_T>(JipInputRect(menu));
			return inputRect > 0x10000;
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

		void ClearJipTextInputHookState()
		{
			s_jipOriginalInputHandler = 0;
		}

		bool CallJipOriginalInput(TextEditMenu* menu, UInt32 input)
		{
			if (!menu || !s_jipOriginalInputHandler || s_jipOriginalInputHandler == JipTextInputHandlerAddress())
				return false;

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
				return;

			TextEditMenu* current = TextEditMenu::GetCurrent();
			if (!LooksLikeJipTextInput(current))
				return;

			s_jipOriginalInputHandler = currentHandler;
			SafeWrite32(kTextEditMenuInputVTableEntry, hookHandler);
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
			if (!g_bMultibyteInputDebug)
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

		bool JipInputCompositionControlShouldSuppress(UInt32 input)
		{
			if (!IsImeCompositionActive())
				return false;

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
				return true;
			default:
				return false;
			}
		}

		bool __fastcall JipTextInputAdapterEx::Input(TextEditMenu* apMenu, void*, UInt32 aiInput)
		{
			if (!LooksLikeJipTextInput(apMenu))
				return CallJipOriginalInput(apMenu, aiInput);

			if (aiInput >= 0x20 && aiInput <= 0x7E)
			{
				if (IsImeConsumingAscii())
				{
					DebugLogJipState("JipTextInputAdapter::Input", "suppress_composition_ascii", apMenu, aiInput);
					return true;
				}

				if (s_lastWndProcAsciiChar == static_cast<UInt8>(aiInput)
					&& GetTickCount() - s_lastWndProcAsciiTick <= kDuplicateAsciiSuppressMs)
				{
					s_lastWndProcAsciiChar = 0;
					DebugLogJipState("JipTextInputAdapter::Input", "suppress_duplicate_wndproc_ascii", apMenu, aiInput);
					return true;
				}

				const char ch = static_cast<char>(aiInput);
				if (!InsertJipTextAtCaret(apMenu, std::string_view(&ch, 1)))
				{
					DebugLogJipState("JipTextInputAdapter::Input", "reject_ascii_insert", apMenu, aiInput);
					return false;
				}

				DebugLogJipState("JipTextInputAdapter::Input", "insert_ascii", apMenu, aiInput);
				return true;
			}

			if (aiInput > 0x7F && aiInput < kInputCode_Backspace)
			{
				DebugLogJipState("JipTextInputAdapter::Input", "swallow_high_byte", apMenu, aiInput);
				return true;
			}

			if (JipInputCompositionControlShouldSuppress(aiInput))
			{
				DebugLogJipState("JipTextInputAdapter::Input", "suppress_composition_control", apMenu, aiInput);
				return true;
			}

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

		bool DeletePreviousChar(TextEditMenu* menu)
		{
			if (!menu)
				return false;

			if (!DeletePreviousChar(menu->xEditState))
				return false;

			menu->Refresh();
			return true;
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

		bool DeleteNextChar(TextEditMenu* menu)
		{
			if (!menu)
				return false;

			if (!DeleteNextChar(menu->xEditState))
				return false;

			menu->Refresh();
			return true;
		}

		bool MoveCaretPrevious(TextEditState& state)
		{
			const std::string current = GetText(state);
			SetCaret(state, PrevCharBoundary(current, ClampToPrevBoundary(current, state.iCaretByteOffset)));
			return true;
		}

		bool MoveCaretPrevious(TextEditMenu* menu)
		{
			if (!menu)
				return false;

			MoveCaretPrevious(menu->xEditState);
			menu->Refresh();
			return true;
		}

		bool MoveCaretNext(TextEditState& state)
		{
			const std::string current = GetText(state);
			SetCaret(state, NextCharBoundary(current, state.iCaretByteOffset));
			return true;
		}

		bool MoveCaretNext(TextEditMenu* menu)
		{
			if (!menu)
				return false;

			MoveCaretNext(menu->xEditState);
			menu->Refresh();
			return true;
		}

		bool MoveCaretHome(TextEditState& state)
		{
			SetCaret(state, 0);
			return true;
		}

		bool MoveCaretHome(TextEditMenu* menu)
		{
			if (!menu)
				return false;

			MoveCaretHome(menu->xEditState);
			menu->Refresh();
			return true;
		}

		bool MoveCaretEnd(TextEditState& state)
		{
			const std::string current = GetText(state);
			SetCaret(state, current.size());
			return true;
		}

		bool MoveCaretEnd(TextEditMenu* menu)
		{
			if (!menu)
				return false;

			MoveCaretEnd(menu->xEditState);
			menu->Refresh();
			return true;
		}

		std::wstring GetImeCompositionString(HWND hwnd, DWORD index)
		{
			HIMC context = ImmGetContext(hwnd);
			if (!context)
				return {};

			const LONG bytes = ImmGetCompositionStringW(context, index, nullptr, 0);
			if (bytes <= 0)
			{
				ImmReleaseContext(hwnd, context);
				return {};
			}

			std::wstring value(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
			ImmGetCompositionStringW(context, index, value.data(), bytes);
			ImmReleaseContext(hwnd, context);
			return value;
		}

		bool HasImeCompositionString(HWND hwnd)
		{
			if (!hwnd)
				return false;

			HIMC context = ImmGetContext(hwnd);
			if (!context)
				return false;

			const LONG compositionBytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
			ImmReleaseContext(hwnd, context);
			return compositionBytes > 0;
		}

		bool IsImeCompositionActive()
		{
			if (s_imeComposing)
				return true;

			if (!s_window)
				return false;

			return HasImeCompositionString(s_window);
		}

		bool IsImeConsumingAscii()
		{
			if (IsImeCompositionActive())
				return true;

			if (!s_window)
				return false;

			HIMC context = ImmGetContext(s_window);
			if (!context)
				return false;

			DWORD conversionMode = 0;
			DWORD sentenceMode = 0;
			const bool isOpen = ImmGetOpenStatus(context) != FALSE;
			const bool hasConversionStatus = ImmGetConversionStatus(
				context,
				&conversionMode,
				&sentenceMode) != FALSE;
			ImmReleaseContext(s_window, context);

			return isOpen && hasConversionStatus && (conversionMode & IME_CMODE_NATIVE);
		}

		std::string WideToCurrentCodePage(std::wstring_view value)
		{
			if (value.empty() || !g_usingWinEncoding)
				return {};

			BOOL usedDefaultChar = FALSE;
			const int length = WideCharToMultiByte(
				g_usingWinEncoding,
				WC_NO_BEST_FIT_CHARS,
				value.data(),
				static_cast<int>(value.size()),
				nullptr,
				0,
				nullptr,
				&usedDefaultChar);
			if (length <= 0 || usedDefaultChar)
				return {};

			std::string converted(static_cast<size_t>(length), '\0');
			usedDefaultChar = FALSE;
			WideCharToMultiByte(
				g_usingWinEncoding,
				WC_NO_BEST_FIT_CHARS,
				value.data(),
				static_cast<int>(value.size()),
				converted.data(),
				length,
				nullptr,
				&usedDefaultChar);
			if (usedDefaultChar)
				return {};

			return converted;
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
				Font::ConvertCharacter(converted);
				if (converted == '\\' || converted == '~')
					return false;

				if (converted != ' ')
					hasVisibleCharacter = true;

				++i;
			}

			return hasVisibleCharacter;
		}

		bool HandleImeResult(HWND hwnd, LPARAM lParam)
		{
			if (!(lParam & GCS_RESULTSTR))
				return false;

			TextEditMenu* menu = GetActiveTextEditMenu();
			TextEditMenu* jipMenu = menu ? nullptr : GetActiveJipTextInputMenu();
			if (!menu && !jipMenu)
			{
				DebugLogState("WndProc.WM_IME_COMPOSITION", "result_no_active_target", nullptr, static_cast<SInt32>(lParam));
				return false;
			}

			std::wstring result = GetImeCompositionString(hwnd, GCS_RESULTSTR);
			if (result.empty())
			{
				if (jipMenu)
					DebugLogJipState("WndProc.WM_IME_COMPOSITION", "result_empty", jipMenu, static_cast<UInt32>(lParam));
				else
					DebugLogState("WndProc.WM_IME_COMPOSITION", "result_empty", menu, static_cast<SInt32>(lParam));
				return false;
			}

			const bool inserted = menu ? InsertWideText(menu, result) : InsertWideTextJip(jipMenu, result);
			if (!inserted)
			{
				if (jipMenu)
					DebugLogJipState("WndProc.WM_IME_COMPOSITION", "result_rejected", jipMenu, static_cast<UInt32>(lParam));
				else
					DebugLogState("WndProc.WM_IME_COMPOSITION", "result_rejected", menu, static_cast<SInt32>(lParam));
				DebugLog("tnvse_multibyte_input: rejected IME result length=%u", static_cast<UInt32>(result.size()));
				return false;
			}

			s_lastImeCommitTick = GetTickCount();
			s_suppressedImeCharCount = static_cast<UInt32>(result.size());
			if (jipMenu)
				DebugLogJipState("WndProc.WM_IME_COMPOSITION", "result_inserted", jipMenu, static_cast<UInt32>(lParam));
			else
				DebugLogState("WndProc.WM_IME_COMPOSITION", "result_inserted", menu, static_cast<SInt32>(lParam));
			DebugLog("tnvse_multibyte_input: committed IME result chars=%u", s_suppressedImeCharCount);
			return true;
		}

		bool ShouldSuppressDuplicateImeChar()
		{
			if (!s_suppressedImeCharCount)
				return false;

			if (GetTickCount() - s_lastImeCommitTick > kDuplicateImeCharSuppressMs)
			{
				s_suppressedImeCharCount = 0;
				return false;
			}

			--s_suppressedImeCharCount;
			return true;
		}

		bool HandleCharFallback(WPARAM wParam)
		{
			if (ShouldSuppressDuplicateImeChar())
			{
				if (TextEditMenu* jipMenu = GetActiveJipTextInputMenu())
					DebugLogJipState("WndProc.WM_CHAR", "suppress_duplicate_ime_char", jipMenu, static_cast<UInt32>(wParam));
				else
					DebugLogState("WndProc.WM_CHAR", "suppress_duplicate_ime_char", GetActiveTextEditMenu(), static_cast<SInt32>(wParam));
				return true;
			}

			if (wParam > 0xFFFF)
			{
				DebugLogState("WndProc.WM_CHAR", "pass_out_of_range", GetAnyActiveTextInputMenu(), static_cast<SInt32>(wParam));
				return false;
			}

			TextEditMenu* menu = GetActiveTextEditMenu();
			TextEditMenu* jipMenu = menu ? nullptr : GetActiveJipTextInputMenu();
			if (!menu && !jipMenu)
			{
				DebugLogState("WndProc.WM_CHAR", "pass_no_active_target", nullptr, static_cast<SInt32>(wParam));
				return false;
			}

			if (jipMenu)
			{
				if (wParam >= 0x20 && wParam <= 0x7E)
				{
					if (IsImeConsumingAscii())
					{
						DebugLogJipState("WndProc.WM_CHAR", "suppress_composition_ascii", jipMenu, static_cast<UInt32>(wParam));
						return true;
					}

					DebugLogJipState("WndProc.WM_CHAR", "pass_ascii_to_jip_adapter", jipMenu, static_cast<UInt32>(wParam));
					return false;
				}

				if (wParam < 0x80)
				{
					DebugLogJipState("WndProc.WM_CHAR", "pass_control_char", jipMenu, static_cast<UInt32>(wParam));
					return false;
				}

				const wchar_t ch = static_cast<wchar_t>(wParam);
				if (!InsertWideTextJip(jipMenu, std::wstring_view(&ch, 1)))
				{
					DebugLogJipState("WndProc.WM_CHAR", "reject_nonascii_insert", jipMenu, static_cast<UInt32>(wParam));
					return false;
				}

				DebugLogJipState("WndProc.WM_CHAR", "insert_nonascii", jipMenu, static_cast<UInt32>(wParam));
				return true;
			}

			if (wParam >= 0x20 && wParam <= 0x7E && IsImeConsumingAscii())
			{
				DebugLogState("WndProc.WM_CHAR", "suppress_composition_ascii", menu, static_cast<SInt32>(wParam));
				return true;
			}

			if (wParam >= 0x20 && wParam <= 0x7E)
			{
				const char ch = static_cast<char>(wParam);
				if (!InsertTextAtCaret(menu, std::string_view(&ch, 1)))
				{
					DebugLogState("WndProc.WM_CHAR", "reject_ascii_insert", menu, static_cast<SInt32>(wParam));
					return false;
				}

				s_lastWndProcAsciiTick = GetTickCount();
				s_lastWndProcAsciiChar = static_cast<UInt8>(wParam);
				DebugLogState("WndProc.WM_CHAR", "insert_ascii", menu, static_cast<SInt32>(wParam));
				return true;
			}

			if (wParam < 0x80)
			{
				DebugLogState("WndProc.WM_CHAR", "pass_control_char", menu, static_cast<SInt32>(wParam));
				return false;
			}

			const wchar_t ch = static_cast<wchar_t>(wParam);
			if (!InsertWideText(menu, std::wstring_view(&ch, 1)))
			{
				DebugLogState("WndProc.WM_CHAR", "reject_nonascii_insert", menu, static_cast<SInt32>(wParam));
				return false;
			}

			DebugLogState("WndProc.WM_CHAR", "insert_nonascii", menu, static_cast<SInt32>(wParam));
			return true;
		}

		LRESULT CALLBACK MultibyteInputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
		{
			if (s_hooksInstalled)
			{
				TryInstallJipTextInputHook();

				if (msg == WM_IME_STARTCOMPOSITION && GetAnyActiveTextInputMenu())
				{
					s_imeComposing = true;
					DebugLogState("WndProc.WM_IME_STARTCOMPOSITION", "composition_start", GetAnyActiveTextInputMenu(), 0);
				}

				if (msg == WM_IME_COMPOSITION)
				{
					TextEditMenu* activeTarget = GetAnyActiveTextInputMenu();
					DebugLog(
						"tnvse_multibyte_input_event: source=WndProc.WM_IME_COMPOSITION lParam=0x%08X hasResult=%u hasComp=%u composingBefore=%u active=0x%08X",
						static_cast<UInt32>(lParam),
						(lParam & GCS_RESULTSTR) ? 1 : 0,
						(lParam & GCS_COMPSTR) ? 1 : 0,
						s_imeComposing ? 1 : 0,
						reinterpret_cast<UInt32>(activeTarget));

					if (HandleImeResult(hwnd, lParam))
					{
						s_imeComposing = false;
						DebugLogState("WndProc.WM_IME_COMPOSITION", "composition_result_consumed", GetAnyActiveTextInputMenu(), static_cast<SInt32>(lParam));
						return 0;
					}

					if (GetAnyActiveTextInputMenu())
					{
						s_imeComposing = true;
						DebugLogState("WndProc.WM_IME_COMPOSITION", "composition_continue", GetAnyActiveTextInputMenu(), static_cast<SInt32>(lParam));
					}
				}

				if (msg == WM_IME_ENDCOMPOSITION)
				{
					s_imeComposing = false;
					DebugLogState("WndProc.WM_IME_ENDCOMPOSITION", "composition_end", GetAnyActiveTextInputMenu(), 0);
				}

				if (msg == WM_CHAR && HandleCharFallback(wParam))
					return 0;
			}

			if (msg == WM_NCDESTROY && hwnd == s_window && s_originalWndProc)
			{
				WNDPROC original = s_originalWndProc;
				SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
				s_originalWndProc = nullptr;
				s_window = nullptr;
				return CallWindowProcA(original, hwnd, msg, wParam, lParam);
			}

			return CallWindowProcA(s_originalWndProc, hwnd, msg, wParam, lParam);
		}

		struct WindowSearch
		{
			DWORD processId = 0;
			HWND window = nullptr;
		};

		BOOL CALLBACK EnumProcessWindows(HWND hwnd, LPARAM lParam)
		{
			auto* search = reinterpret_cast<WindowSearch*>(lParam);
			DWORD processId = 0;
			GetWindowThreadProcessId(hwnd, &processId);
			if (processId != search->processId)
				return TRUE;

			if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
				return TRUE;

			char className[64] = {};
			GetClassNameA(hwnd, className, sizeof(className));
			if (!_stricmp(className, "ConsoleWindowClass"))
				return TRUE;

			search->window = hwnd;
			return FALSE;
		}

		HWND FindGameWindow()
		{
			const DWORD currentProcessId = GetCurrentProcessId();
			HWND foreground = GetForegroundWindow();
			if (foreground)
			{
				DWORD foregroundProcessId = 0;
				GetWindowThreadProcessId(foreground, &foregroundProcessId);
				if (foregroundProcessId == currentProcessId && IsWindowVisible(foreground))
					return foreground;
			}

			WindowSearch search;
			search.processId = currentProcessId;
			EnumWindows(EnumProcessWindows, reinterpret_cast<LPARAM>(&search));
			return search.window;
		}

		bool TryInstallWindowProc()
		{
			if (s_originalWndProc)
				return true;

			HWND hwnd = FindGameWindow();
			if (!hwnd)
				return false;

			LONG_PTR original = SetWindowLongPtrA(
				hwnd,
				GWLP_WNDPROC,
				reinterpret_cast<LONG_PTR>(&MultibyteInputWndProc));
			if (!original)
				return false;

			s_window = hwnd;
			s_originalWndProc = reinterpret_cast<WNDPROC>(original);
			DebugLog("tnvse_multibyte_input: subclassed hwnd=0x%08X", reinterpret_cast<UInt32>(hwnd));
			return true;
		}

		void ClearInputState()
		{
			s_activeTextEditMenu = nullptr;
			s_imeComposing = false;
			s_suppressedImeCharCount = 0;
			s_lastImeCommitTick = 0;
			s_lastWndProcAsciiTick = 0;
			s_lastWndProcAsciiChar = 0;
			ClearJipTextInputHookState();
		}

		void RestoreWindowProc()
		{
			if (CurrentTextEditInputHandler() == JipTextInputHandlerAddress())
				SafeWrite32(kTextEditMenuInputVTableEntry, kTextEditMenuHandleKeyboardInput);

			if (s_window && s_originalWndProc)
			{
				SetWindowLongPtrA(s_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(s_originalWndProc));
			}

			s_window = nullptr;
			s_originalWndProc = nullptr;
			ClearInputState();
		}
	}

	class TextEditMenuEx : public TextEditMenu
	{
	public:
		static bool __cdecl Open(const char* apTitle, const char* apInitialText, ValidateTextCallback apValidateText)
		{
			ValidateTextCallback validateText = apValidateText;
			if (reinterpret_cast<SIZE_T>(apValidateText) == kPlayerNameIsValidName)
				validateText = &ValidatePlayerName;

			const bool opened = TextEditMenu::Open(apTitle, apInitialText, validateText);
			s_activeTextEditMenu = opened ? TextEditMenu::GetCurrent() : nullptr;
			DebugLog(
				"tnvse_multibyte_input: TextEditMenu::Open opened=%u title=\"%s\" initialLen=%u menu=0x%08X",
				opened ? 1 : 0,
				apTitle ? apTitle : "",
				apInitialText ? static_cast<UInt32>(std::strlen(apInitialText)) : 0,
				reinterpret_cast<UInt32>(s_activeTextEditMenu));
			return opened;
		}
	};

	class TextEditStateEx : public TextEditState
	{
	public:
		static void __fastcall Input(TextEditState* apState, SInt32 aiInput, SInt32 aiChar)
		{
			if (!apState || !apState->IsActive())
				return;

			TextEditMenu* menu = GetActiveTextEditMenu();
			if (menu && &menu->xEditState != apState)
				menu = nullptr;

			if (aiInput >= 0x20 && aiInput <= 0x7E)
			{
				if (IsImeConsumingAscii())
				{
					DebugLogState("TextEditState::Input", "suppress_composition_ascii", menu, aiInput);
					return;
				}

				if (s_lastWndProcAsciiChar == static_cast<UInt8>(aiInput)
					&& GetTickCount() - s_lastWndProcAsciiTick <= kDuplicateAsciiSuppressMs)
				{
					s_lastWndProcAsciiChar = 0;
					DebugLogState("TextEditState::Input", "suppress_duplicate_wndproc_ascii", menu, aiInput);
					return;
				}

				const char ch = static_cast<char>(aiInput);
				if (!InsertTextAtCaret(*apState, std::string_view(&ch, 1)))
				{
					DebugLogState("TextEditState::Input", "reject_ascii_insert", menu, aiInput);
					return;
				}

				DebugLogState("TextEditState::Input", "insert_ascii", menu, aiInput);
				return;
			}

			if (aiInput > 0x7F && aiInput <= 0xFF)
			{
				DebugLogState("TextEditState::Input", "swallow_high_byte", menu, aiInput);
				return;
			}

			const bool imeCompositionActive = IsImeCompositionActive();
			switch (aiInput)
			{
			case kTextEditInput_Backspace:
				if (imeCompositionActive)
				{
					DebugLogState("TextEditState::Input", "suppress_composition_control", menu, aiInput);
					return;
				}
				DebugLogState("TextEditState::Input", "delete_previous", menu, aiInput);
				DeletePreviousChar(*apState);
				return;
			case kTextEditInput_Delete:
				if (imeCompositionActive)
				{
					DebugLogState("TextEditState::Input", "suppress_composition_control", menu, aiInput);
					return;
				}
				DebugLogState("TextEditState::Input", "delete_next", menu, aiInput);
				DeleteNextChar(*apState);
				return;
			case kTextEditInput_Left:
				if (imeCompositionActive)
				{
					DebugLogState("TextEditState::Input", "suppress_composition_control", menu, aiInput);
					return;
				}
				DebugLogState("TextEditState::Input", "move_left", menu, aiInput);
				MoveCaretPrevious(*apState);
				return;
			case kTextEditInput_Right:
				if (imeCompositionActive)
				{
					DebugLogState("TextEditState::Input", "suppress_composition_control", menu, aiInput);
					return;
				}
				DebugLogState("TextEditState::Input", "move_right", menu, aiInput);
				MoveCaretNext(*apState);
				return;
			case kTextEditInput_Home:
				if (imeCompositionActive)
				{
					DebugLogState("TextEditState::Input", "suppress_composition_control", menu, aiInput);
					return;
				}
				DebugLogState("TextEditState::Input", "move_home", menu, aiInput);
				MoveCaretHome(*apState);
				return;
			case kTextEditInput_End:
				if (imeCompositionActive)
				{
					DebugLogState("TextEditState::Input", "suppress_composition_control", menu, aiInput);
					return;
				}
				DebugLogState("TextEditState::Input", "move_end", menu, aiInput);
				MoveCaretEnd(*apState);
				return;
			default:
				DebugLogState("TextEditState::Input", "pass_original", menu, aiInput);
				apState->InputUnk01(aiInput, aiChar);
				return;
			}
		}
	};

	void InitMultibyteInputHook()
	{
		if (s_initialized)
			return;

		s_initialized = true;

		if (!g_bMultibyteInput)
			return;

		if (!g_bEnableMultibyteFontHook || !g_usingWinEncoding)
		{
			gLog.FormattedMessage("tnvse_multibyte_input: disabled because font hooks or uiEncoding are disabled");
			return;
		}

		WriteRelCall(kPlayerNameTextEditOpenCall, &TextEditMenuEx::Open);
		WriteRelCall(kTextEditStateInputCallInHandleKeyboardInput, &TextEditStateEx::Input);
		s_hooksInstalled = true;
		TryInstallWindowProc();
		if (g_bMultibyteInputCompositionPreview)
			gLog.FormattedMessage("tnvse_multibyte_input: composition preview is reserved for a later stage");
		gLog.FormattedMessage("tnvse_multibyte_input: hooks installed");
	}

	void HandleMultibyteInputMessage(NVSEMessagingInterface::Message* apMessage)
	{
		if (!s_initialized || !g_bMultibyteInput || !apMessage)
			return;

		if (apMessage->type == NVSEMessagingInterface::kMessage_MainGameLoop)
		{
			if (s_hooksInstalled && !s_originalWndProc)
				TryInstallWindowProc();

			if (s_hooksInstalled)
				TryInstallJipTextInputHook();
		}
		else if (apMessage->type == NVSEMessagingInterface::kMessage_ExitGame
			|| apMessage->type == NVSEMessagingInterface::kMessage_ExitToMainMenu)
		{
			RestoreWindowProc();
		}
	}
}

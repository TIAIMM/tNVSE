#include "multibyte_input_internal.h"

#include "encoding.h"
#include "SafeWrite.h"
#include "tnvse.h"
#include "Tile.hpp"
#include "ui_decode.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <Windows.h>

namespace fonthook
{
	namespace
	{
		class JipTextInputAdapterEx
		{
		public:
			static bool __fastcall Input(TextEditMenu* apMenu, void*, UInt32 aiInput);
		};

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

		bool CallJipOriginalInput(TextEditMenu* menu, UInt32 input)
		{
			if (!menu || !s_jipOriginalInputHandler || s_jipOriginalInputHandler == JipTextInputHandlerAddress())
				return false;

			using InputHandler = bool(__thiscall*)(TextEditMenu*, UInt32);
			return reinterpret_cast<InputHandler>(s_jipOriginalInputHandler)(menu, input);
		}

		std::string GetJipText(TextEditMenu* menu)
		{
			BSStringT<char>& text = JipCurrentText(menu);
			const UInt32 length = text.GetLength();
			if (!length)
				return {};

			return std::string(text.c_str(), length);
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
			if (!LooksLikeJipTextInputStorage(apMenu))
				return CallJipOriginalInput(apMenu, aiInput);

			const bool editActive = JipIsActiveFlag(apMenu) != 0;
			if (aiInput >= 0x20 && aiInput <= 0x7E)
			{
				if (IsImeConsumingAscii())
				{
					DebugLogJipState("JipTextInputAdapter::Input", "suppress_composition_ascii", apMenu, aiInput);
					return true;
				}

				if (!editActive)
					return CallJipOriginalInput(apMenu, aiInput);

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
	}

	SIZE_T JipTextInputHandlerAddress()
	{
		return reinterpret_cast<SIZE_T>(&JipTextInputAdapterEx::Input);
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

	void ClearJipTextInputHookState()
	{
		s_jipOriginalInputHandler = 0;
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

	bool InsertWideTextJip(TextEditMenu* menu, std::wstring_view value)
	{
		std::string converted = WideToCurrentCodePage(value);
		if (converted.empty())
			return false;

		return InsertJipTextAtCaret(menu, converted);
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

}

#include "multibyte_input_internal.h"

#include "encoding.h"
#include "InterfaceManager.hpp"
#include "SafeWrite.h"
#include "tnvse.h"
#include "Tile.hpp"
#include "ui_decode.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <Windows.h>

namespace fonthook
{
	namespace
	{
		std::string GetText(const TextEditState& state)
		{
			const UInt32 length = state.xText.GetLength();
			if (!length)
				return {};

			return std::string(state.xText.c_str(), length);
		}
	}

	char PrintableAscii(UInt32 value)
	{
		return (value >= 0x20 && value <= 0x7E) ? static_cast<char>(value) : '.';
	}

	bool AsciiEqualsIgnoreCase(UInt8 lhs, wchar_t rhs)
	{
		if (lhs > 0x7F || rhs > 0x7F)
			return false;

		return std::tolower(lhs) == std::tolower(static_cast<unsigned char>(rhs));
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

	bool InsertWideText(TextEditMenu* menu, std::wstring_view value)
	{
		std::string converted = WideToCurrentCodePage(value);
		if (converted.empty())
			return false;

		return InsertTextAtCaret(menu, converted);
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

		if (*reinterpret_cast<SIZE_T*>(kTextEditMenuInputVTableEntry) != kTextEditMenuHandleKeyboardInput)
			return nullptr;

		if (!current->xEditState.IsActive())
			return nullptr;

		return current;
	}

	SIZE_T CurrentTextEditInputHandler()
	{
		return *reinterpret_cast<SIZE_T*>(kTextEditMenuInputVTableEntry);
	}

	bool HasOverlayInputTarget()
	{
		return GetOverlayTextInputMenu() || GetOverlayStewieInputTarget().valid;
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

	void TryRemoveCompositionEcho()
	{
		if (s_compositionEchoChecked || s_imeCandidateState.composition.empty())
			return;

		s_compositionEchoChecked = true;
		if (!IsConfiguredImeLayout(s_window))
			return;

		const wchar_t compositionLead = s_imeCandidateState.composition.front();
		if (TextEditMenu* menu = GetActiveTextEditMenu())
		{
			if (RemovePreviousAsciiCompositionEcho(menu->xEditState, compositionLead))
			{
				menu->Refresh();
				DebugLogState("IMECompositionEcho", "remove_ascii_echo", menu, static_cast<SInt32>(compositionLead));
			}
			return;
		}

		if (TextEditMenu* jipMenu = GetCurrentJipTextInputMenu())
		{
			if (RemovePreviousJipAsciiCompositionEcho(jipMenu, compositionLead))
				DebugLogJipState("IMECompositionEcho", "remove_ascii_echo", jipMenu, static_cast<UInt32>(compositionLead));
			return;
		}

		if (RemovePreviousStewieAsciiCompositionEcho(compositionLead))
			DebugLog("tnvse_multibyte_input_event: source=IMECompositionEcho action=remove_ascii_echo_stewie input=0x%08X", static_cast<UInt32>(compositionLead));
	}
}
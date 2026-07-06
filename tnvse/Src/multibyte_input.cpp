#include "multibyte_input.h"

#include "encoding.h"
#include "load_config.h"
#include "SafeWrite.h"
#include "tnvse.h"
#include "ui_decode.h"

#include <algorithm>
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
		constexpr SIZE_T kTextEditMenuInputVTableEntry = 0x1070064;
		constexpr UInt32 kMaxTextEditRawBytes = 1023;
		constexpr DWORD kDuplicateImeCharSuppressMs = 250;
		constexpr DWORD kDuplicateAsciiSuppressMs = 100;

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
			if (!s_activeTextEditMenu || s_activeTextEditMenu != current)
			{
				s_activeTextEditMenu = nullptr;
				return nullptr;
			}

			if (!current || !current->xEditState.IsActive())
				return nullptr;

			return current;
		}

		bool CommitCandidate(TextEditMenu* menu, std::string& candidate, size_t caret)
		{
			if (!menu)
				return false;

			TextEditState& state = menu->xEditState;
			if (!FitsTextEditConstraints(state, candidate))
				return false;

			state.SetText(candidate.c_str());
			SetCaret(state, ClampToPrevBoundary(candidate, caret));
			state.bClearOnNextType = false;
			menu->Refresh();
			return true;
		}

		bool InsertTextAtCaret(TextEditMenu* menu, std::string_view text)
		{
			if (!menu || text.empty())
				return false;

			TextEditState& state = menu->xEditState;
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

			return CommitCandidate(menu, candidate, caret + text.size());
		}

		bool DeletePreviousChar(TextEditMenu* menu)
		{
			if (!menu)
				return false;

			TextEditState& state = menu->xEditState;
			std::string current = GetText(state);
			size_t caret = ClampToPrevBoundary(current, state.iCaretByteOffset);
			if (!caret)
				return true;

			const size_t previous = PrevCharBoundary(current, caret);
			current.erase(previous, caret - previous);
			return CommitCandidate(menu, current, previous);
		}

		bool DeleteNextChar(TextEditMenu* menu)
		{
			if (!menu)
				return false;

			TextEditState& state = menu->xEditState;
			std::string current = GetText(state);
			size_t caret = ClampToPrevBoundary(current, state.iCaretByteOffset);
			if (caret >= current.size())
				return true;

			const size_t next = NextCharBoundary(current, caret);
			current.erase(caret, next - caret);
			return CommitCandidate(menu, current, caret);
		}

		bool MoveCaretPrevious(TextEditMenu* menu)
		{
			if (!menu)
				return false;

			TextEditState& state = menu->xEditState;
			const std::string current = GetText(state);
			SetCaret(state, PrevCharBoundary(current, ClampToPrevBoundary(current, state.iCaretByteOffset)));
			menu->Refresh();
			return true;
		}

		bool MoveCaretNext(TextEditMenu* menu)
		{
			if (!menu)
				return false;

			TextEditState& state = menu->xEditState;
			const std::string current = GetText(state);
			SetCaret(state, NextCharBoundary(current, state.iCaretByteOffset));
			menu->Refresh();
			return true;
		}

		bool MoveCaretHome(TextEditMenu* menu)
		{
			if (!menu)
				return false;

			SetCaret(menu->xEditState, 0);
			menu->Refresh();
			return true;
		}

		bool MoveCaretEnd(TextEditMenu* menu)
		{
			if (!menu)
				return false;

			const std::string current = GetText(menu->xEditState);
			SetCaret(menu->xEditState, current.size());
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
			if (!menu)
			{
				DebugLogState("WndProc.WM_IME_COMPOSITION", "result_no_active_target", nullptr, static_cast<SInt32>(lParam));
				return false;
			}

			std::wstring result = GetImeCompositionString(hwnd, GCS_RESULTSTR);
			if (result.empty())
			{
				DebugLogState("WndProc.WM_IME_COMPOSITION", "result_empty", menu, static_cast<SInt32>(lParam));
				return false;
			}

			if (!InsertWideText(menu, result))
			{
				DebugLogState("WndProc.WM_IME_COMPOSITION", "result_rejected", menu, static_cast<SInt32>(lParam));
				DebugLog("tnvse_multibyte_input: rejected IME result length=%u", static_cast<UInt32>(result.size()));
				return false;
			}

			s_lastImeCommitTick = GetTickCount();
			s_suppressedImeCharCount = static_cast<UInt32>(result.size());
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
				DebugLogState("WndProc.WM_CHAR", "suppress_duplicate_ime_char", GetActiveTextEditMenu(), static_cast<SInt32>(wParam));
				return true;
			}

			if (wParam > 0xFFFF)
			{
				DebugLogState("WndProc.WM_CHAR", "pass_out_of_range", GetActiveTextEditMenu(), static_cast<SInt32>(wParam));
				return false;
			}

			TextEditMenu* menu = GetActiveTextEditMenu();
			if (!menu)
			{
				DebugLogState("WndProc.WM_CHAR", "pass_no_active_target", nullptr, static_cast<SInt32>(wParam));
				return false;
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
				if (msg == WM_IME_STARTCOMPOSITION && GetActiveTextEditMenu())
				{
					s_imeComposing = true;
					DebugLogState("WndProc.WM_IME_STARTCOMPOSITION", "composition_start", GetActiveTextEditMenu(), 0);
				}

				if (msg == WM_IME_COMPOSITION)
				{
					DebugLog(
						"tnvse_multibyte_input_event: source=WndProc.WM_IME_COMPOSITION lParam=0x%08X hasResult=%u hasComp=%u composingBefore=%u active=0x%08X",
						static_cast<UInt32>(lParam),
						(lParam & GCS_RESULTSTR) ? 1 : 0,
						(lParam & GCS_COMPSTR) ? 1 : 0,
						s_imeComposing ? 1 : 0,
						reinterpret_cast<UInt32>(GetActiveTextEditMenu()));

					if (HandleImeResult(hwnd, lParam))
					{
						s_imeComposing = false;
						DebugLogState("WndProc.WM_IME_COMPOSITION", "composition_result_consumed", GetActiveTextEditMenu(), static_cast<SInt32>(lParam));
						return 0;
					}

					if (GetActiveTextEditMenu())
					{
						s_imeComposing = true;
						DebugLogState("WndProc.WM_IME_COMPOSITION", "composition_continue", GetActiveTextEditMenu(), static_cast<SInt32>(lParam));
					}
				}

				if (msg == WM_IME_ENDCOMPOSITION)
				{
					s_imeComposing = false;
					DebugLogState("WndProc.WM_IME_ENDCOMPOSITION", "composition_end", GetActiveTextEditMenu(), 0);
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
		}

		void RestoreWindowProc()
		{
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

		bool __thiscall HandleKeyboardInput(SInt32 aiInput)
		{
			TextEditMenu* menu = (s_activeTextEditMenu == this
				&& this == TextEditMenu::GetCurrent()
				&& xEditState.IsActive()) ? this : nullptr;
			if (menu)
			{
				if (aiInput >= 0x20 && aiInput <= 0x7E)
				{
					if (IsImeConsumingAscii())
					{
						DebugLogState("TextEditMenu::HandleKeyboardInput", "suppress_composition_ascii", menu, aiInput);
						return true;
					}

					if (s_lastWndProcAsciiChar == static_cast<UInt8>(aiInput)
						&& GetTickCount() - s_lastWndProcAsciiTick <= kDuplicateAsciiSuppressMs)
					{
						s_lastWndProcAsciiChar = 0;
						DebugLogState("TextEditMenu::HandleKeyboardInput", "suppress_duplicate_wndproc_ascii", menu, aiInput);
						return true;
					}

					const char ch = static_cast<char>(aiInput);
					if (!InsertTextAtCaret(menu, std::string_view(&ch, 1)))
					{
						DebugLogState("TextEditMenu::HandleKeyboardInput", "reject_ascii_insert", menu, aiInput);
						return false;
					}

					DebugLogState("TextEditMenu::HandleKeyboardInput", "insert_ascii", menu, aiInput);
					return true;
				}

				if (aiInput > 0x7F && aiInput <= 0xFF)
				{
					DebugLogState("TextEditMenu::HandleKeyboardInput", "swallow_high_byte", menu, aiInput);
					return true;
				}

				const bool imeCompositionActive = IsImeCompositionActive();
				switch (aiInput)
				{
				case kTextEditInput_Backspace:
					if (imeCompositionActive)
					{
						DebugLogState("TextEditMenu::HandleKeyboardInput", "suppress_composition_control", menu, aiInput);
						return true;
					}
					DebugLogState("TextEditMenu::HandleKeyboardInput", "delete_previous", menu, aiInput);
					return DeletePreviousChar(menu);
				case kTextEditInput_Delete:
					if (imeCompositionActive)
					{
						DebugLogState("TextEditMenu::HandleKeyboardInput", "suppress_composition_control", menu, aiInput);
						return true;
					}
					DebugLogState("TextEditMenu::HandleKeyboardInput", "delete_next", menu, aiInput);
					return DeleteNextChar(menu);
				case kTextEditInput_Left:
					if (imeCompositionActive)
					{
						DebugLogState("TextEditMenu::HandleKeyboardInput", "suppress_composition_control", menu, aiInput);
						return true;
					}
					DebugLogState("TextEditMenu::HandleKeyboardInput", "move_left", menu, aiInput);
					return MoveCaretPrevious(menu);
				case kTextEditInput_Right:
					if (imeCompositionActive)
					{
						DebugLogState("TextEditMenu::HandleKeyboardInput", "suppress_composition_control", menu, aiInput);
						return true;
					}
					DebugLogState("TextEditMenu::HandleKeyboardInput", "move_right", menu, aiInput);
					return MoveCaretNext(menu);
				case kTextEditInput_Home:
					if (imeCompositionActive)
					{
						DebugLogState("TextEditMenu::HandleKeyboardInput", "suppress_composition_control", menu, aiInput);
						return true;
					}
					DebugLogState("TextEditMenu::HandleKeyboardInput", "move_home", menu, aiInput);
					return MoveCaretHome(menu);
				case kTextEditInput_End:
					if (imeCompositionActive)
					{
						DebugLogState("TextEditMenu::HandleKeyboardInput", "suppress_composition_control", menu, aiInput);
						return true;
					}
					DebugLogState("TextEditMenu::HandleKeyboardInput", "move_end", menu, aiInput);
					return MoveCaretEnd(menu);
				case kTextEditInput_Confirm:
				{
					if (imeCompositionActive)
					{
						DebugLogState("TextEditMenu::HandleKeyboardInput", "suppress_composition_control", menu, aiInput);
						return true;
					}
					DebugLogState("TextEditMenu::HandleKeyboardInput", "confirm_original_enter", menu, aiInput);
					const bool handled = TextEditMenu::HandleKeyboardInput(aiInput);
					if (handled)
						ClearInputState();
					DebugLogState("TextEditMenu::HandleKeyboardInput", handled ? "confirm_original_handled" : "confirm_original_unhandled", nullptr, aiInput);
					return handled;
				}
				default:
					DebugLogState("TextEditMenu::HandleKeyboardInput", "pass_original", menu, aiInput);
					break;
				}
			}

			return TextEditMenu::HandleKeyboardInput(aiInput);
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
		ReplaceVirtualFuncEx(kTextEditMenuInputVTableEntry, &TextEditMenuEx::HandleKeyboardInput);
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
		}
		else if (apMessage->type == NVSEMessagingInterface::kMessage_ExitGame
			|| apMessage->type == NVSEMessagingInterface::kMessage_ExitToMainMenu)
		{
			RestoreWindowProc();
		}
	}
}

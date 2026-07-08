#include "multibyte_input.h"

#include "multibyte_input_internal.h"

#include "load_config.h"
#include "SafeWrite.h"
#include "tnvse.h"

#include <cstring>
#include <memory>
#include <utility>

namespace fonthook
{
	namespace
	{
		bool s_initialized = false;
	}

	// ---- Shared global state definitions ----
	HWND s_window = nullptr;
	WNDPROC s_originalWndProc = nullptr;
	bool s_hooksInstalled = false;
	bool s_imeComposing = false;
	bool s_compositionEchoChecked = false;
	DWORD s_lastImeCommitTick = 0;
	DWORD s_lastWndProcAsciiTick = 0;
	UInt8 s_lastWndProcAsciiChar = 0;
	UInt32 s_suppressedImeCharCount = 0;
	SIZE_T s_jipOriginalInputHandler = 0;

	ImeCandidateState s_imeCandidateState;
	bool s_tsfCandidateActive = false;
	bool s_gameImeContextDetached = false;
	bool s_textInputSessionActive = false;
	bool s_stewieReplay = false;

	void DebugLog(const char* fmt, ...)
	{
		if (!g_bMultibyteInputDebug)
			return;

		va_list args;
		va_start(args, fmt);
		gLog.FormattedMessage(fmt, args);
		va_end(args);
	}

	namespace
	{
		void ClearInputState()
		{
			s_textInputSessionActive = false;
			s_imeComposing = false;
			ClearImePreviewState();
			HideCandidateOverlay();
			ReleaseCandidateOverlayTexture();
			s_suppressedImeCharCount = 0;
			s_lastImeCommitTick = 0;
			s_lastWndProcAsciiTick = 0;
			s_lastWndProcAsciiChar = 0;
			ClearJipTextInputHookState();
			ClearStewieInputState();
			s_stewieReplay = false;
		}

		void RestoreWindowProc()
		{
			if (CurrentTextEditInputHandler() == JipTextInputHandlerAddress())
				SafeWrite32(kTextEditMenuInputVTableEntry, kTextEditMenuHandleKeyboardInput);

			if (s_window && s_originalWndProc)
			{
				SetGameImeEnabled(s_window, true);
				SetWindowLongPtrA(s_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(s_originalWndProc));
			}

			s_window = nullptr;
			s_originalWndProc = nullptr;
			if (s_tsfCandidateSink)
			{
				ShutdownTsfCandidateSink();
			}
			ClearInputState();
		}
	}

	bool __cdecl TextEditMenuEx::Open(const char* apTitle, const char* apInitialText, ValidateTextCallback apValidateText)
	{
		ValidateTextCallback validateText = apValidateText;
		if (reinterpret_cast<SIZE_T>(apValidateText) == kPlayerNameIsValidName)
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

	void __fastcall TextEditStateEx::Input(TextEditState* apState, void*, SInt32 aiInput, SInt32 aiChar)
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
		{
			if (g_bMultibyteInputUseTSFCandidates)
			{
				if (!InitializeTsfCandidateSink())
					gLog.FormattedMessage("tnvse_multibyte_input: TSF candidate sink unavailable; using IMM32 fallback");
				else
					gLog.FormattedMessage("tnvse_multibyte_input: TSF candidate sink enabled");
			}
			gLog.FormattedMessage("tnvse_multibyte_input: composition preview overlay enabled");
		}
		gLog.FormattedMessage("tnvse_multibyte_input: hooks installed");
	}

	void HandleMultibyteInputMessage(NVSEMessagingInterface::Message* apMessage)
	{
		if (!s_initialized || !g_bMultibyteInput || !apMessage)
			return;

		if (apMessage->type == kMessage_OnFramePresent)
		{
			if (s_hooksInstalled && s_window && g_bMultibyteInputCompositionPreview)
				DrawCandidateOverlay();
		}
		else if (apMessage->type == NVSEMessagingInterface::kMessage_MainGameLoop)
		{
			if (s_hooksInstalled && !s_originalWndProc)
				TryInstallWindowProc();

			if (s_hooksInstalled)
				TryInstallJipTextInputHook();

			if (s_hooksInstalled)
				TryInstallStewieTweaksInputHooks();

			if (s_hooksInstalled && s_window)
				UpdateGameImeAssociation();

			if (s_hooksInstalled && s_window && g_bMultibyteInputCompositionPreview)
			{
				RefreshImeStatus(s_window);
				UpdateCandidateOverlay();
			}
		}
		else if (apMessage->type == NVSEMessagingInterface::kMessage_ExitGame
			|| apMessage->type == NVSEMessagingInterface::kMessage_ExitToMainMenu)
		{
			RestoreWindowProc();
		}
	}
}
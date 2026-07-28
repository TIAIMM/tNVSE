#include "multibyte_input_internal.h"
#include "game_hooks.h"
#include "native_tile_overlay.h"

// Public lifecycle and frame-message integration for multibyte input.

namespace fonthook
{
	namespace
	{
		bool s_initialized = false;
	}

	namespace multibyte_input
	{
		HWND s_window = nullptr;
		WNDPROC s_originalWndProc = nullptr;
		bool s_hooksInstalled = false;
		bool s_imeComposing = false;
		DWORD s_lastWndProcAsciiTick = 0;
		UInt8 s_lastWndProcAsciiChar = 0;
	}

	void InitMultibyteInputHook()
	{
		using namespace multibyte_input;

		if (s_initialized)
			return;

		s_initialized = true;

		if (!g_bMultibyteInput)
			return;

		if (!AreMultibyteFontHooksInstalled() || !IsEastAsianUiMode())
		{
			gLog.FormattedMessage(
				"tnvse_multibyte_input: disabled because multibyte font hooks are unavailable or uiEncoding is not 1-4");
			return;
		}

		InstallTextEditHooks();
		s_hooksInstalled = true;
		TryInstallWindowProc();

		if (g_bMultibyteInputCompositionPreview)
		{
			if (g_bMultibyteInputUseTSFCandidates)
			{
				if (InitializeTsfCandidateSupport())
					gLog.FormattedMessage("tnvse_multibyte_input: TSF candidate sink enabled");
				else
					gLog.FormattedMessage("tnvse_multibyte_input: TSF candidate sink unavailable; using IMM32 fallback");
			}
			gLog.FormattedMessage(
				"tnvse_multibyte_input: native Tile composition preview enabled; host will be resolved on the main loop");
		}
		gLog.FormattedMessage("tnvse_multibyte_input: hooks installed");
	}

	void HandleMultibyteInputMessage(NVSEMessagingInterface::Message* apMessage)
	{
		using namespace multibyte_input;

		if (!s_initialized || !g_bMultibyteInput || !apMessage)
			return;

		if (apMessage->type == NVSEMessagingInterface::kMessage_DeferredInit)
		{
			if (s_hooksInstalled)
			{
				TryInstallJipKeyEventSuppressionHook();
				InitializeMcmExtenderInputBridge();
				InitializeDialogueHistoryInputBridge();
			}
		}
		else if (apMessage->type == NVSEMessagingInterface::kMessage_MainGameLoop)
		{
			if (s_hooksInstalled && !s_originalWndProc)
				TryInstallWindowProc();

			if (s_hooksInstalled)
			{
				// All menu discovery and handler publication is owned by the game
				// loop. The window/TSF callbacks only enqueue captured input data.
				TryInstallJipTextInputHook();
				TryInstallStewieTweaksInputHooks();
				ProcessStewieTweaksInputTargetState();
				ProcessStewieMenuSearchPendingStateSync();
				ProcessMcmExtenderInputTargetState();
				ProcessDialogueHistoryInputTargetState();
				SynchronizeTextInputTarget("main_game_loop");
				PumpTsfInputUpdates();
				PumpCapturedInputEvents();
				PumpCandidateOverlay();
			}

			if (s_hooksInstalled && s_window)
				PumpImeStatusWatchdog();
		}
		else if (apMessage->type == NVSEMessagingInterface::kMessage_ExitGame
			|| apMessage->type
				== NVSEMessagingInterface::kMessage_ExitGame_Console)
		{
			ResetMcmExtenderInputState();
			ResetDialogueHistoryInputState();
			ShutdownNativeTileOverlayHost();
			RestoreWindowProc();
		}
		else if (apMessage->type == NVSEMessagingInterface::kMessage_ExitToMainMenu)
		{
			ResetMcmExtenderInputState();
			ResetDialogueHistoryInputState();
			HideCandidateOverlay();
			ShutdownNativeTileOverlayHost();
			RestoreWindowProc();
		}
	}
}

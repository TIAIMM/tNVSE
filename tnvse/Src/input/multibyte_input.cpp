#include "multibyte_input_internal.h"
#include "game_hooks.h"
#include "native_tile_overlay.h"

// Public lifecycle and frame-message integration for multibyte input.

namespace fonthook
{
	namespace implementation::multibyte_input {}
	using namespace implementation::multibyte_input;

	namespace implementation::multibyte_input
	{
		bool s_initialized = false;
	}

	namespace multibyte_input
	{
		HWND s_window = nullptr;
		WNDPROC s_predecessorWndProc = nullptr;
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

		if (!InstallTextEditHooks())
		{
			gLog.FormattedMessage(
				"tnvse_multibyte_input: disabled because the required TextEdit hooks were not installed as one complete unit");
			return;
		}
		s_hooksInstalled = true;
		TryInstallWindowProc();

		if (InitializeTsfCandidateSupport())
		{
			gLog.FormattedMessage(
				"tnvse_multibyte_input: TSF system candidate UI suppression enabled");
		}
		else
		{
			gLog.FormattedMessage(
				"tnvse_multibyte_input: TSF UI element sink unavailable; WM_IME_SETCONTEXT and IMM32 offscreen suppression remain active");
		}

		if (g_bMultibyteInputCompositionPreview)
		{
			if (g_bMultibyteInputUseTSFCandidates)
				gLog.FormattedMessage("tnvse_multibyte_input: TSF candidate data capture enabled with IMM32 fallback");
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
				InitializeModernHelpMenuInputBridge();
				InitializeMcmExtenderInputBridge();
				InitializeDialogueHistoryInputBridge();
			}
		}
		else if (apMessage->type == NVSEMessagingInterface::kMessage_MainGameLoop)
		{
			bool windowProcPublished = false;
			if (s_hooksInstalled
				&& TryInstallWindowProc(&windowProcPublished)
				&& windowProcPublished)
			{
				// A new game window can follow ExitToMainMenu. Recreate/verify the
				// process-local TSF sink only when the adapter is actually published;
				// an externally replaced live chain is deliberately not overwritten.
				InitializeTsfCandidateSupport();
			}

			if (s_hooksInstalled)
			{
				// All menu discovery and handler publication is owned by the game
				// loop. The window/TSF callbacks only enqueue captured input data.
				TryInstallJipTextInputHook();
				TryInstallStewieTweaksInputHooks();
				ProcessStewieTweaksInputTargetState();
				ProcessStewieMenuSearchPendingStateSync();
				ProcessModernHelpMenuInputTargetState();
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
			ResetModernHelpMenuInputState();
			ResetMcmExtenderInputState();
			ResetDialogueHistoryInputState();
			ShutdownNativeTileOverlayHost();
			RestoreWindowProc();
		}
		else if (apMessage->type == NVSEMessagingInterface::kMessage_ExitToMainMenu)
		{
			ResetModernHelpMenuInputState();
			ResetMcmExtenderInputState();
			ResetDialogueHistoryInputState();
			HideCandidateOverlay();
			ShutdownNativeTileOverlayHost();
			RestoreWindowProc();
		}
	}
}

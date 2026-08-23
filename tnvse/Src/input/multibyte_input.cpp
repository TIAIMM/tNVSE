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

		void TryInitializeRuntime(const char* source)
		{
			using namespace fonthook::multibyte_input;

			if (!s_hooksInstalled)
				return;

			bool windowProcPublished = false;
			if (!TryInstallWindowProc(&windowProcPublished)
				|| !windowProcPublished)
			{
				return;
			}

			// COM/TSF and window subclassing must not run from NVSEPlugin_Load:
			// xNVSE invokes plugin Load callbacks while its DLL is still under the
			// loader lock. DeferredInit/MainGameLoop run after that load chain and
			// on the game-window thread enforced by TryInstallWindowProc().
			if (InitializeTsfCandidateSupport())
			{
				gLog.FormattedMessage(
					"tnvse_multibyte_input: runtime initialized source=%s; TSF system candidate UI suppression enabled",
					source ? source : "unknown");
			}
			else
			{
				gLog.FormattedMessage(
					"tnvse_multibyte_input: runtime initialized source=%s; TSF UI element sink unavailable, WM_IME_SETCONTEXT and IMM32 offscreen suppression remain active",
					source ? source : "unknown");
			}
		}
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

		if (g_bMultibyteInputCompositionPreview)
		{
			if (g_bMultibyteInputUseTSFCandidates)
				gLog.FormattedMessage("tnvse_multibyte_input: TSF candidate data capture enabled with IMM32 fallback");
			gLog.FormattedMessage(
				"tnvse_multibyte_input: native Tile composition preview enabled; host will be resolved on the main loop");
		}
		gLog.FormattedMessage(
			"tnvse_multibyte_input: TextEdit hooks installed; window, IMM32, and TSF runtime initialization deferred");
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
				TryInitializeRuntime("deferred_init");
			}
		}
		else if (apMessage->type == NVSEMessagingInterface::kMessage_MainGameLoop)
		{
			// DeferredInit can precede creation of the game HWND. Retry window
			// publication here; TSF is initialized only for a newly published
			// adapter, including a replacement window after ExitToMainMenu.
			TryInitializeRuntime("main_game_loop");

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
			// COM teardown must not depend on whether a later plugin currently
			// owns the top of the WndProc subclass chain.
			ShutdownTsfCandidateSupport();
			RestoreWindowProc();
		}
		else if (apMessage->type == NVSEMessagingInterface::kMessage_ExitToMainMenu)
		{
			ResetModernHelpMenuInputState();
			ResetMcmExtenderInputState();
			ResetDialogueHistoryInputState();
			HideCandidateOverlay();
			RestoreWindowProc();
		}
	}
}

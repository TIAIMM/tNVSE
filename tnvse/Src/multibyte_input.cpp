#include "multibyte_input_internal.h"

// Public lifecycle and frame-message integration for multibyte input.

namespace fonthook
{
	namespace
	{
		constexpr UInt32 kMessage_OnFramePresent = NVSEMessagingInterface::kMessage_PostQueryPlugins + 1;
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

		if (!g_bEnableMultibyteFontHook || !g_usingWinEncoding)
		{
			gLog.FormattedMessage("tnvse_multibyte_input: disabled because font hooks or uiEncoding are disabled");
			return;
		}

		InstallTextEditHooks();
		s_hooksInstalled = true;
		TryInstallWindowProc();

		if (g_bMultibyteInputStewieTweaks)
			TryInstallTileReadXMLHook();

		if (g_bMultibyteInputCompositionPreview)
		{
			if (InitializeCandidateOverlayRenderer())
			{
				if (g_bMultibyteInputUseTSFCandidates)
				{
					if (InitializeTsfCandidateSupport())
						gLog.FormattedMessage("tnvse_multibyte_input: TSF candidate sink enabled");
					else
						gLog.FormattedMessage("tnvse_multibyte_input: TSF candidate sink unavailable; using IMM32 fallback");
				}
				gLog.FormattedMessage("tnvse_multibyte_input: composition preview overlay enabled");
			}
		}
		gLog.FormattedMessage("tnvse_multibyte_input: hooks installed");
	}

	void HandleMultibyteInputMessage(NVSEMessagingInterface::Message* apMessage)
	{
		using namespace multibyte_input;

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
				ProcessStewieMenuSearchPendingStateSync();

			if (s_hooksInstalled && s_window)
				PumpImeStatusWatchdog();
		}
		else if (apMessage->type == NVSEMessagingInterface::kMessage_ExitGame)
		{
			RestoreWindowProc();
			ShutdownCandidateOverlayRenderer();
		}
		else if (apMessage->type == NVSEMessagingInterface::kMessage_ExitToMainMenu)
		{
			RestoreWindowProc();
		}
	}
}

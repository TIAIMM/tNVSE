#include "multibyte_input_ime_internal.h"

namespace fonthook
{
	namespace multibyte_input
	{
		namespace
		{
			ImeState s_imeState;
		}

		ImeState& State()
		{
			return s_imeState;
		}

		bool HasOverlayInputTarget()
		{
			return GetOverlayTextInputMenu()
				|| GetOverlayStewieInputTarget().valid
				|| GetOverlayDialogueHistoryInputTarget().valid
				|| GetOverlayMcmExtenderInputTarget().valid;
		}

		void TryRemoveCompositionEcho()
		{
			if (State().compositionEchoChecked || State().candidate.composition.empty())
				return;

			State().compositionEchoChecked = true;
			if (!IsConfiguredImeLayout(s_window))
				return;

			const wchar_t compositionLead = State().candidate.composition.front();
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
			{
				DebugLog("tnvse_multibyte_input_event: source=IMECompositionEcho action=remove_ascii_echo_stewie input=0x%08X", static_cast<UInt32>(compositionLead));
				return;
			}

			if (RemovePreviousDialogueHistoryAsciiCompositionEcho(compositionLead))
			{
				DebugLog("tnvse_multibyte_input_event: source=IMECompositionEcho action=remove_ascii_echo_dialogue_history input=0x%08X", static_cast<UInt32>(compositionLead));
				return;
			}

			if (RemovePreviousMcmExtenderAsciiCompositionEcho(compositionLead))
				DebugLog("tnvse_multibyte_input_event: source=IMECompositionEcho action=remove_ascii_echo_mcm_extender input=0x%08X", static_cast<UInt32>(compositionLead));
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
			if (!s_window || !IsConfiguredImeLayout(s_window))
				return false;

			if (IsImeCompositionActive())
				return true;

			if (IsNativeImeAsciiGuardActive())
				return true;

			if (State().textInputSessionActive && HasOverlayInputTarget())
			{
				if (!State().candidate.composition.empty() || !State().candidate.candidates.empty())
					return true;
			}

			// Open/native is only the requested IMM compatibility state. Modern TSF
			// IMEs can report it before they actually start a composition. Treating it
			// as permanent consumption loses every ASCII key until the input profile is
			// switched. The short activation guard above covers the first-key race;
			// after that, only real composition or candidate state consumes ASCII.
			return false;
		}

		std::string WideToCurrentCodePage(std::wstring_view value)
		{
			if (value.empty())
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

		std::wstring GetCurrentImeName(HWND hwnd)
		{
			if (g_bMultibyteInputUseTSFCandidates)
			{
				std::wstring tsfName = GetCurrentTsfInputMethodName();
				if (!tsfName.empty())
					return tsfName;
			}

			DWORD threadId = hwnd ? GetWindowThreadProcessId(hwnd, nullptr) : GetCurrentThreadId();
			HKL layout = GetKeyboardLayout(threadId);
			if (layout)
			{
				wchar_t description[128] = {};
				if (ImmGetDescriptionW(layout, description, ARRAYSIZE(description)) > 0 && description[0])
					return description;
			}

			wchar_t layoutName[KL_NAMELENGTH] = {};
			if (GetKeyboardLayoutNameW(layoutName) && layoutName[0])
				return layoutName;

			return L"IME";
		}

		void RefreshImeStatus(HWND hwnd, HKL expectedLayout)
		{
			State().candidate.imeName = GetCurrentImeName(hwnd);
			State().candidate.imeOpen = false;
			State().candidate.conversionMode = 0;
			State().candidate.sentenceMode = 0;

			if (!IsConfiguredImeLayout(hwnd, expectedLayout))
				return;

			HIMC context = hwnd ? ImmGetContext(hwnd) : nullptr;
			if (!context)
				return;

			State().candidate.imeOpen = ImmGetOpenStatus(context) != FALSE;
			ImmGetConversionStatus(
				context,
				&State().candidate.conversionMode,
				&State().candidate.sentenceMode);
			ImmReleaseContext(hwnd, context);
		}

		void RefreshImeComposition(HWND hwnd)
		{
			State().candidate.composition = GetImeCompositionString(hwnd, GCS_COMPSTR);
			if (!State().candidate.composition.empty())
				State().tsfCompositionFallbackActive = false;
		}

		void ClearImeCandidates()
		{
			State().candidate.candidates.clear();
			State().candidate.selection = 0;
			State().candidate.pageStart = 0;
			State().candidate.pageSize = 0;
			State().candidate.candidatesFromTsf = false;
			State().tsfCandidateActive = false;
		}

		void RefreshImeCandidatesFromImm(HWND hwnd)
		{
			ClearImeCandidates();

			HIMC context = hwnd ? ImmGetContext(hwnd) : nullptr;
			if (!context)
				return;

			const DWORD bytes = ImmGetCandidateListW(context, 0, nullptr, 0);
			if (!bytes)
			{
				ImmReleaseContext(hwnd, context);
				return;
			}

			std::unique_ptr<char[]> buffer(new char[bytes]);
			auto* list = reinterpret_cast<LPCANDIDATELIST>(buffer.get());
			if (ImmGetCandidateListW(context, 0, list, bytes) != bytes)
			{
				ImmReleaseContext(hwnd, context);
				return;
			}

			if (list->dwStyle != IME_CAND_CODE)
			{
				State().candidate.selection = list->dwSelection;
				State().candidate.pageStart = list->dwPageStart;
				State().candidate.pageSize = list->dwPageSize;

				const DWORD pageEnd = std::min<DWORD>(
					list->dwCount,
					list->dwPageStart + std::min<DWORD>(list->dwPageSize, kMaxImeCandidatesToDisplay));
				for (DWORD index = list->dwPageStart; index < pageEnd; ++index)
				{
					const DWORD offset = list->dwOffset[index];
					if (!offset || offset >= bytes)
						continue;

					const wchar_t* candidate = reinterpret_cast<const wchar_t*>(buffer.get() + offset);
					if (candidate && *candidate)
						State().candidate.candidates.emplace_back(candidate);
				}
			}

			ImmReleaseContext(hwnd, context);
		}

		void RefreshImeCandidates(HWND hwnd)
		{
			if (!IsConfiguredImeLayout(hwnd))
			{
				ClearImeCandidates();
				return;
			}

			if (g_bMultibyteInputUseTSFCandidates
				&& State().tsfCandidateActive
				&& State().candidate.candidatesFromTsf
				&& !State().candidate.candidates.empty())
				return;

			RefreshImeCandidatesFromImm(hwnd);
		}

		void ClearImePreviewState()
		{
			State().candidate.composing = false;
			State().candidate.composition.clear();
			State().tsfCompositionFallbackActive = false;
			State().compositionEchoChecked = false;
			ClearImeCandidates();
		}

		void HideSystemImeWindows(HWND hwnd)
		{
			if (!g_bMultibyteInputHideSystemCandidateWindow
				|| !IsCandidateOverlayRendererAvailable()
				|| !hwnd)
				return;

			if (State().hidingSystemImeWindows)
				return;

			HIMC context = ImmGetContext(hwnd);
			if (!context)
				return;

			State().hidingSystemImeWindows = true;

			COMPOSITIONFORM compositionForm = {};
			compositionForm.dwStyle = CFS_FORCE_POSITION;
			compositionForm.ptCurrentPos.x = -32000;
			compositionForm.ptCurrentPos.y = -32000;
			ImmSetCompositionWindow(context, &compositionForm);

			for (DWORD i = 0; i < 4; ++i)
			{
				CANDIDATEFORM candidateForm = {};
				candidateForm.dwIndex = i;
				candidateForm.dwStyle = CFS_CANDIDATEPOS;
				candidateForm.ptCurrentPos.x = -32000;
				candidateForm.ptCurrentPos.y = -32000;
				ImmSetCandidateWindow(context, &candidateForm);
			}

			ImmReleaseContext(hwnd, context);
			State().hidingSystemImeWindows = false;
		}

		void CancelGameImeComposition(HWND hwnd)
		{
			if (!hwnd)
				return;

			HIMC context = ImmGetContext(hwnd);
			if (!context)
				return;

			ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
			ImmReleaseContext(hwnd, context);
		}

		void EnsureConfiguredImeOpen(HWND hwnd, const char* reason, HKL expectedLayout)
		{
			if (!hwnd)
				return;

			if (!IsConfiguredImeLayout(hwnd, expectedLayout))
			{
				State().nativeImeAsciiGuardUntilTick = 0;
				return;
			}

			HIMC context = ImmGetContext(hwnd);
			if (!context)
				return;

			DWORD conversionMode = 0;
			DWORD sentenceMode = 0;
			const bool wasOpen = ImmGetOpenStatus(context) != FALSE;
			const bool hasConversionStatus = ImmGetConversionStatus(
				context,
				&conversionMode,
				&sentenceMode) != FALSE;

			bool changed = false;
			if (!wasOpen)
			{
				ImmSetOpenStatus(context, TRUE);
				changed = true;
			}

			if (hasConversionStatus && !(conversionMode & IME_CMODE_NATIVE))
			{
				ImmSetConversionStatus(context, conversionMode | IME_CMODE_NATIVE, sentenceMode);
				conversionMode |= IME_CMODE_NATIVE;
				changed = true;
			}

			ImmReleaseContext(hwnd, context);

			const bool guardNativeAscii = hasConversionStatus && (conversionMode & IME_CMODE_NATIVE);
			if (guardNativeAscii)
				State().nativeImeAsciiGuardUntilTick = GetTickCount() + kNativeImeAsciiGuardMs;

			if (changed || guardNativeAscii)
			{
				RefreshImeStatus(hwnd, expectedLayout);
				DebugLog(
					"tnvse_multibyte_input: prepared configured IME reason=%s changed=%u guard=%u open=%u native=%u",
					reason ? reason : "unknown",
					changed ? 1 : 0,
					guardNativeAscii ? 1 : 0,
					State().candidate.imeOpen ? 1 : 0,
					(State().candidate.conversionMode & IME_CMODE_NATIVE) ? 1 : 0);
			}
		}

		void RestoreDefaultGameImeContext(HWND hwnd, const char* reason, HKL expectedLayout)
		{
			if (!hwnd)
				return;

			if (!IsConfiguredImeLayout(hwnd, expectedLayout))
			{
				State().nativeImeAsciiGuardUntilTick = 0;
				s_imeComposing = false;
				ClearImePreviewState();
			}
			EnsureConfiguredImeOpen(hwnd, reason, expectedLayout);
			RefreshImeStatus(hwnd, expectedLayout);
			DebugLog(
				"tnvse_multibyte_input: game IME default context enabled reason=%s open=%u native=%u",
				reason ? reason : "unknown",
				State().candidate.imeOpen ? 1 : 0,
				(State().candidate.conversionMode & IME_CMODE_NATIVE) ? 1 : 0);
		}

		void SetGameImeEnabled(HWND hwnd, bool enable)
		{
			if (!hwnd)
				return;
			if (State().gameImeEnabled == enable)
				return;

			// Change the state before calling IMM. Cancel can synchronously deliver
			// IME messages, and those messages must observe the disabled state.
			State().gameImeEnabled = enable;

			if (enable)
			{
				RestoreDefaultGameImeContext(hwnd, "enable");
				DebugLog("tnvse_multibyte_input: game IME context enabled");
				return;
			}

			CancelGameImeComposition(hwnd);
			s_imeComposing = false;
			ClearImePreviewState();
			HideCandidateOverlay();
			DebugLog("tnvse_multibyte_input: game IME input disabled; context retained");
		}

		void SetTextInputSessionActive(bool active)
		{
			if (State().textInputSessionActive == active)
				return;

			SynchronizeTextInputTarget(
				active ? "session_activate_sync" : "session_deactivate_sync");
			State().textInputSessionActive = active;
			AdvanceTextInputSessionGeneration(
				active ? "text_input_session_start" : "text_input_session_end");
			SetJipKeyEventSuppressionCaptureActive(active);
			if (!active)
				ResetImeCommitKeyState("text_input_session_end");
			if (s_window)
			{
				SetGameImeEnabled(s_window, active);
				if (active)
				{
					// Enabling the retained IMM context does not guarantee an
					// IMN_SETOPENSTATUS notification. Read it explicitly so a newly
					// focused Stewie input shows the current IME status before typing.
					RefreshImeStatus(s_window);
					UpdateCandidateOverlay();
				}
			}

			DebugLog(
				"tnvse_multibyte_input: text input session %s",
				active ? "started" : "ended");
		}

		void RefreshTextInputSessionForActiveTarget(const char* reason)
		{
			const bool wasActive = State().textInputSessionActive;
			SynchronizeTextInputTarget(
				reason ? reason : "target_refresh_sync");
			AdvanceTextInputSessionGeneration(
				reason ? reason : "text_input_target_refresh");
			CancelDeferredStewieAscii();
			ResetImeCommitKeyState("text_input_target_refresh");
			s_imeComposing = false;
			AdvanceTsfCandidateSession();
			ClearImePreviewState();
			HideCandidateOverlay();
			State().textInputSessionActive = true;
			SetJipKeyEventSuppressionCaptureActive(true);
			if (s_window)
			{
				if (State().gameImeEnabled)
					RestoreDefaultGameImeContext(s_window, reason ? reason : "target_refresh");
				else
					SetGameImeEnabled(s_window, true);

				// Target activation is an input event in its own right. Do not wait
				// for the first composition/key message to publish the status line.
				RefreshImeStatus(s_window);
				UpdateCandidateOverlay();
			}

			DebugLog(
				"tnvse_multibyte_input: text input session %s reason=%s",
				wasActive ? "refreshed" : "started",
				reason ? reason : "target_refresh");
		}

		void UpdateGameImeAssociation()
		{
			if (!s_window)
				return;

			SynchronizeTextInputTarget("update_game_ime_association");
			SetTextInputSessionActive(HasCurrentTextInputTarget());
		}

		void PumpImeStatusWatchdog()
		{
			if (!s_window)
				return;

			// Input, focus, language, composition and TSF candidate events refresh
			// immediately in the window/TSF callbacks. Keep only a low-frequency
			// safety net while there is live state that can become stale.
			if (!State().textInputSessionActive && !State().overlay.visible)
			{
				State().lastImeWatchdogTick = 0;
				return;
			}

			constexpr DWORD kImeWatchdogIntervalMs = 250;
			const DWORD now = GetTickCount();
			if (State().lastImeWatchdogTick && now - State().lastImeWatchdogTick < kImeWatchdogIntervalMs)
				return;

			State().lastImeWatchdogTick = now;
			UpdateGameImeAssociation();
			if (!State().textInputSessionActive && !State().overlay.visible)
				return;

			if (g_bMultibyteInputCompositionPreview)
			{
				RefreshImeStatus(s_window);
				UpdateCandidateOverlay();
			}
		}

		void EndStewieTextInputSession(const char* reason)
		{
			CancelDeferredStewieAscii();
			s_imeComposing = false;
			State().nativeImeAsciiGuardUntilTick = 0;
			State().lastStewieImeCommitTick = 0;
			State().lastStewieImeEnterKeyTick = 0;
			AdvanceTsfCandidateSession();
			ClearImePreviewState();
			HideCandidateOverlay();
			SetTextInputSessionActive(false);
			UpdateGameImeAssociation();

			DebugLog(
				"tnvse_multibyte_input: Stewie text input session reset reason=%s",
				reason ? reason : "unknown");
		}

		bool IsImeWindowMessage(UINT msg)
		{
			switch (msg)
			{
			case WM_IME_STARTCOMPOSITION:
			case WM_IME_COMPOSITION:
			case WM_IME_ENDCOMPOSITION:
			case WM_IME_NOTIFY:
			case WM_IME_SETCONTEXT:
			case WM_IME_CHAR:
				return true;
			default:
				return false;
			}
		}

		bool IsVirtualKeyDown(int vk)
		{
			return (GetKeyState(vk) & 0x8000) != 0 || (GetAsyncKeyState(vk) & 0x8000) != 0;
		}

		namespace
		{
			bool MapImeCommitVirtualKey(UINT virtualKey, UInt32& input)
			{
				if (virtualKey == VK_SPACE)
				{
					input = ' ';
					return true;
				}

				if (virtualKey >= '0' && virtualKey <= '9')
				{
					input = virtualKey;
					return true;
				}

				if (virtualKey >= VK_NUMPAD0 && virtualKey <= VK_NUMPAD9)
				{
					input = '0' + virtualKey - VK_NUMPAD0;
					return true;
				}

				if (virtualKey == VK_RETURN)
				{
					input = kInputCode_Enter;
					return true;
				}

				return false;
			}

			UInt32 NormalizeImeCommitInput(UInt32 input)
			{
				if (input == ' ' || (input >= '0' && input <= '9'))
					return input;
				if (input == '\r' || input == kInputCode_Enter)
					return kInputCode_Enter;
				return 0;
			}

			const char* ImeCommitChannelName(ImeCommitInputChannel channel)
			{
				switch (channel)
				{
				case ImeCommitInputChannel::WndProcChar:
					return "wndproc";
				case ImeCommitInputChannel::TextEdit:
					return "textedit";
				case ImeCommitInputChannel::JipTextInput:
					return "jip";
				case ImeCommitInputChannel::Stewie:
					return "stewie";
				case ImeCommitInputChannel::DialogueHistory:
					return "dialogue_history";
				case ImeCommitInputChannel::McmExtender:
					return "mcm_extender";
				default:
					return "unknown";
				}
			}

			bool ImeCommitModifiersDown()
			{
				return IsVirtualKeyDown(VK_SHIFT)
					|| IsVirtualKeyDown(VK_CONTROL)
					|| IsVirtualKeyDown(VK_MENU)
					|| IsVirtualKeyDown(VK_LWIN)
					|| IsVirtualKeyDown(VK_RWIN);
			}

			bool CommitKeyStateExpired(const ImeCommitKeyState& key, DWORD now)
			{
				if (!key.pending && !key.confirmed)
					return false;

				if (key.confirmed)
					return false;

				if (!key.released && key.virtualKey && IsVirtualKeyDown(key.virtualKey))
					return false;
				return now - key.observedTick > kImeCommitKeyPendingLifetimeMs;
			}

			void ExpireImeCommitKeyState()
			{
				if (CommitKeyStateExpired(State().commitKey, GetTickCount()))
					ResetImeCommitKeyState("expired");
			}

			bool FindPressedImeCommitKey(UINT& virtualKey, UInt32& input, UInt32 requiredInput = 0)
			{
				if (ImeCommitModifiersDown())
					return false;

				constexpr UINT keys[] = {
					VK_SPACE,
					'1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
					VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4, VK_NUMPAD5,
					VK_NUMPAD6, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9, VK_NUMPAD0,
					VK_RETURN,
				};

				for (UINT key : keys)
				{
					if (!IsVirtualKeyDown(key))
						continue;
					if (!MapImeCommitVirtualKey(key, input))
						continue;
					if (requiredInput && input != requiredInput)
						continue;

					virtualKey = key;
					return true;
				}

				return false;
			}
		}

		void ResetImeCommitKeyState(const char* reason)
		{
			const ImeCommitKeyState previous = State().commitKey;
			State().commitKey = {};
			if (!previous.pending && !previous.confirmed)
				return;

			DebugLog(
				"tnvse_multibyte_input_event: source=ImeCommitKey action=reset reason=%s vk=0x%02X input=0x%08X pending=%u confirmed=%u released=%u consumed=0x%02X",
				reason ? reason : "unknown",
				previous.virtualKey,
				previous.input,
				previous.pending ? 1 : 0,
				previous.confirmed ? 1 : 0,
				previous.released ? 1 : 0,
				previous.consumedChannels);
		}

		void ObserveImeCommitKeyMessage(UINT msg, WPARAM wParam, LPARAM lParam, bool hasInputTarget)
		{
			const bool keyDown = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
			const bool keyUp = msg == WM_KEYUP || msg == WM_SYSKEYUP;
			if (!keyDown && !keyUp)
				return;

			UInt32 input = 0;
			const UINT virtualKey = static_cast<UINT>(wParam);
			if (!MapImeCommitVirtualKey(virtualKey, input))
				return;

			ExpireImeCommitKeyState();
			ImeCommitKeyState& key = State().commitKey;
			if (keyUp)
			{
				if ((key.pending || key.confirmed) && key.virtualKey == virtualKey)
				{
					key.released = true;
					DebugLog(
						"tnvse_multibyte_input_event: source=ImeCommitKey action=release vk=0x%02X input=0x%08X confirmed=%u consumed=0x%02X",
						virtualKey,
						key.input,
						key.confirmed ? 1 : 0,
						key.consumedChannels);
				}
				return;
			}

			const bool repeat = (static_cast<ULONG_PTR>(lParam) & (1UL << 30)) != 0;
			if ((key.pending || key.confirmed) && key.virtualKey == virtualKey && repeat)
				return;

			// A non-repeat key-down is a new physical press. It must never inherit a
			// confirmed latch from the preceding press of the same logical key.
			if (key.pending || key.confirmed)
				ResetImeCommitKeyState("new_commit_keydown");

			if (!hasInputTarget || ImeCommitModifiersDown() || !IsImeConsumingAscii())
				return;

			ImeCommitKeyState& armed = State().commitKey;
			armed.virtualKey = virtualKey;
			armed.input = input;
			armed.observedTick = GetTickCount();
			armed.pending = true;
			armed.released = false;
			DebugLog(
				"tnvse_multibyte_input_event: source=ImeCommitKey action=arm vk=0x%02X input=0x%08X",
				virtualKey,
				input);
		}

		void ObserveImeCommitInput(UInt32 input)
		{
			const UInt32 normalized = NormalizeImeCommitInput(input);
			if (!normalized)
				return;

			ExpireImeCommitKeyState();
			ImeCommitKeyState& key = State().commitKey;
			if (key.confirmed)
				return;
			if (key.pending && key.input == normalized)
			{
				UINT refreshedVirtualKey = 0;
				UInt32 refreshedInput = 0;
				if (FindPressedImeCommitKey(
						refreshedVirtualKey,
						refreshedInput,
						normalized))
				{
					key.virtualKey = refreshedVirtualKey;
				}
				key.observedTick = GetTickCount();
				key.released = false;
				return;
			}
			if (key.pending)
				ResetImeCommitKeyState("different_composition_commit_input");

			UINT virtualKey = 0;
			UInt32 mappedInput = 0;
			const bool foundPressed = FindPressedImeCommitKey(
				virtualKey,
				mappedInput,
				normalized);
			if (!foundPressed)
			{
				virtualKey = normalized == kInputCode_Enter
					? VK_RETURN
					: static_cast<UINT>(normalized);
			}

			ImeCommitKeyState& armed = State().commitKey;
			armed.virtualKey = virtualKey;
			armed.input = normalized;
			armed.observedTick = GetTickCount();
			armed.pending = true;
			armed.released = false;
			DebugLog(
				"tnvse_multibyte_input_event: source=ImeCommitKey action=arm_from_input vk=0x%02X input=0x%08X pressed=%u",
				virtualKey,
				normalized,
				foundPressed ? 1 : 0);
		}

		void ConfirmImeCommitKey(ImeCommitInputChannel expectedChannel)
		{
			ExpireImeCommitKeyState();
			ImeCommitKeyState& key = State().commitKey;
			if (!key.pending)
			{
				UINT virtualKey = 0;
				UInt32 input = 0;
				if (!FindPressedImeCommitKey(virtualKey, input))
					return;

				key.virtualKey = virtualKey;
				key.input = input;
				key.observedTick = GetTickCount();
				key.pending = true;
				key.released = false;
				DebugLog(
					"tnvse_multibyte_input_event: source=ImeCommitKey action=infer_pressed vk=0x%02X input=0x%08X",
					virtualKey,
					input);
			}

			key.confirmed = true;
			key.expectedChannel = static_cast<UInt8>(expectedChannel);
			DebugLog(
				"tnvse_multibyte_input_event: source=ImeCommitKey action=confirm vk=0x%02X input=0x%08X released=%u channel=%s",
				key.virtualKey,
				key.input,
				key.released ? 1 : 0,
				ImeCommitChannelName(expectedChannel));
		}

		bool ShouldSuppressImeCommitInput(UInt32 input, ImeCommitInputChannel channel)
		{
			const UInt32 normalized = NormalizeImeCommitInput(input);
			if (!normalized)
				return false;

			ExpireImeCommitKeyState();
			ImeCommitKeyState& key = State().commitKey;
			if (!key.confirmed || key.input != normalized)
				return false;
			if (key.released && key.virtualKey && IsVirtualKeyDown(key.virtualKey))
			{
				// The release was observed and the same physical key is down again.
				// Treat this as a fresh literal press even if its WM_KEYDOWN was delayed
				// behind the game's polling callback.
				ResetImeCommitKeyState("new_press_detected_by_key_state");
				return false;
			}

			const UInt8 channelBit = static_cast<UInt8>(channel);
			key.consumedChannels |= channelBit;
			if (normalized == kInputCode_Enter
				&& key.expectedChannel == static_cast<UInt8>(ImeCommitInputChannel::Stewie))
			{
				// The lifecycle latch supersedes MenuSearch's older time-based Enter
				// pairing once either delivery channel has observed the committed key.
				State().lastStewieImeCommitTick = 0;
				State().lastStewieImeEnterKeyTick = 0;
			}
			DebugLog(
				"tnvse_multibyte_input_event: source=ImeCommitKey action=suppress input=0x%08X channel=%s expected=0x%02X released=%u consumed=0x%02X",
				normalized,
				ImeCommitChannelName(channel),
				key.expectedChannel,
				key.released ? 1 : 0,
				key.consumedChannels);
			return true;
		}

		bool IsPendingWinSpaceRelease(UINT msg, WPARAM wParam)
		{
			return State().winSpaceSwitchPending
				&& (msg == WM_KEYUP || msg == WM_SYSKEYUP)
				&& wParam == VK_SPACE;
		}

		bool IsWindowsKeyMessage(UINT msg, WPARAM wParam)
		{
			return (msg == WM_KEYDOWN
				|| msg == WM_SYSKEYDOWN
				|| msg == WM_KEYUP
				|| msg == WM_SYSKEYUP)
				&& (wParam == VK_LWIN || wParam == VK_RWIN);
		}

		bool ShouldSuppressInputLanguageSwitchAscii(UInt8 input)
		{
			if (input != ' ')
				return false;

			return IsVirtualKeyDown(VK_LWIN)
				|| IsVirtualKeyDown(VK_RWIN)
				|| State().winSpaceChordArmed
				|| State().winSpaceSwitchPending
				|| static_cast<SInt32>(State().inputLanguageSwitchGuardUntilTick - GetTickCount()) > 0;
		}

		HKL GetGameKeyboardLayout(HWND hwnd)
		{
			DWORD threadId = hwnd ? GetWindowThreadProcessId(hwnd, nullptr) : GetCurrentThreadId();
			return GetKeyboardLayout(threadId);
		}

		bool LayoutMatchesCurrentEncoding(HKL layout)
		{
			if (!layout || !IsEastAsianUiMode())
				return false;

			const LANGID language = LOWORD(reinterpret_cast<ULONG_PTR>(layout));
			switch (g_uiEncoding)
			{
			case 1:
			case 2:
				return PRIMARYLANGID(language) == LANG_CHINESE;
			case 3:
				return PRIMARYLANGID(language) == LANG_JAPANESE;
			case 4:
				return PRIMARYLANGID(language) == LANG_KOREAN;
			default:
				return false;
			}
		}

		bool IsConfiguredImeLayout(HWND hwnd, HKL expectedLayout)
		{
			HKL layout = expectedLayout ? expectedLayout : GetGameKeyboardLayout(hwnd);
			return LayoutMatchesCurrentEncoding(layout);
		}

		bool IsNativeImeAsciiGuardActive()
		{
			return s_window
				&& State().textInputSessionActive
				&& IsConfiguredImeLayout(s_window)
				&& static_cast<SInt32>(State().nativeImeAsciiGuardUntilTick - GetTickCount()) > 0;
		}

		bool IsFocusRestoreMessage(UINT msg, WPARAM wParam)
		{
			switch (msg)
			{
			case WM_SETFOCUS:
				return true;
			case WM_ACTIVATEAPP:
				return wParam != FALSE;
			case WM_ACTIVATE:
				return LOWORD(wParam) != WA_INACTIVE;
			default:
				return false;
			}
		}
	}
}

#include "multibyte_input_ime_internal.h"

namespace fonthook
{
	namespace multibyte_input
	{
		bool ApplyCapturedImeResult(
			const std::wstring& result,
			LPARAM lParam)
		{
			if (!(lParam & GCS_RESULTSTR))
				return false;

			TextEditMenu* menu = GetActiveTextEditMenu();
			TextEditMenu* jipMenu = menu ? nullptr : GetActiveJipTextInputMenu();
			if (!menu && !jipMenu)
				jipMenu = GetCurrentJipTextInputMenu();
			StewieInputTarget stewieTarget = (!menu && !jipMenu) ? GetOverlayStewieInputTarget() : StewieInputTarget();
			DialogueHistoryInputTarget dialogueHistoryTarget =
				(!menu && !jipMenu && !stewieTarget.valid)
					? GetOverlayDialogueHistoryInputTarget()
					: DialogueHistoryInputTarget();
			McmExtenderInputTarget mcmTarget =
				(!menu && !jipMenu && !stewieTarget.valid && !dialogueHistoryTarget.valid)
				? GetOverlayMcmExtenderInputTarget()
				: McmExtenderInputTarget();
			if (!menu && !jipMenu && !stewieTarget.valid
				&& !dialogueHistoryTarget.valid && !mcmTarget.valid)
			{
				DebugLogState("WndProc.WM_IME_COMPOSITION", "result_no_active_target", nullptr, static_cast<SInt32>(lParam));
				return false;
			}

			if (result.empty())
			{
				if (jipMenu)
					DebugLogJipState("WndProc.WM_IME_COMPOSITION", "result_empty", jipMenu, static_cast<UInt32>(lParam));
				else if (stewieTarget.valid)
					DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_COMPOSITION action=result_empty_stewie input=0x%08X", static_cast<UInt32>(lParam));
				else if (dialogueHistoryTarget.valid)
					DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_COMPOSITION action=result_empty_dialogue_history input=0x%08X", static_cast<UInt32>(lParam));
				else if (mcmTarget.valid)
					DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_COMPOSITION action=result_empty_mcm_extender input=0x%08X", static_cast<UInt32>(lParam));
				else
					DebugLogState("WndProc.WM_IME_COMPOSITION", "result_empty", menu, static_cast<SInt32>(lParam));
				return false;
			}

			const bool inserted = menu
				? InsertWideText(menu, result)
				: (jipMenu
					? InsertWideTextJip(jipMenu, result)
					: (stewieTarget.valid
						? InsertWideTextStewie(stewieTarget, result)
						: (dialogueHistoryTarget.valid
							? InsertWideTextDialogueHistory(dialogueHistoryTarget, result)
							: InsertWideTextMcmExtender(mcmTarget, result))));
			if (!inserted)
			{
				if (jipMenu)
					DebugLogJipState("WndProc.WM_IME_COMPOSITION", "result_rejected", jipMenu, static_cast<UInt32>(lParam));
				else if (stewieTarget.valid)
					DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_COMPOSITION action=result_rejected_stewie input=0x%08X", static_cast<UInt32>(lParam));
				else if (dialogueHistoryTarget.valid)
					DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_COMPOSITION action=result_rejected_dialogue_history input=0x%08X", static_cast<UInt32>(lParam));
				else if (mcmTarget.valid)
					DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_COMPOSITION action=result_rejected_mcm_extender input=0x%08X", static_cast<UInt32>(lParam));
				else
					DebugLogState("WndProc.WM_IME_COMPOSITION", "result_rejected", menu, static_cast<SInt32>(lParam));
				DebugLog("tnvse_multibyte_input: rejected IME result length=%u", static_cast<UInt32>(result.size()));
				return false;
			}

			State().lastImeCommitTick = GetTickCount();
			ConfirmImeCommitKey(menu
				? ImeCommitInputChannel::TextEdit
				: (jipMenu
					? ImeCommitInputChannel::JipTextInput
					: (stewieTarget.valid
						? ImeCommitInputChannel::Stewie
						: (dialogueHistoryTarget.valid
							? ImeCommitInputChannel::DialogueHistory
							: ImeCommitInputChannel::McmExtender))));
			constexpr DWORD kImeEnterPairMs = 250;
			if (stewieTarget.valid
				&& State().lastStewieImeEnterKeyTick
				&& State().lastImeCommitTick - State().lastStewieImeEnterKeyTick <= kImeEnterPairMs)
			{
				State().lastStewieImeCommitTick = State().lastImeCommitTick;
				State().lastStewieImeEnterKeyTick = 0;
			}
			State().suppressedImeCharCount = static_cast<UInt32>(result.size());
			ClearImePreviewState();
			RefreshImeStatus(s_window);
			UpdateCandidateOverlay();
			if (jipMenu)
				DebugLogJipState("WndProc.WM_IME_COMPOSITION", "result_inserted", jipMenu, static_cast<UInt32>(lParam));
			else if (stewieTarget.valid)
				DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_COMPOSITION action=result_inserted_stewie input=0x%08X", static_cast<UInt32>(lParam));
			else if (dialogueHistoryTarget.valid)
				DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_COMPOSITION action=result_inserted_dialogue_history input=0x%08X", static_cast<UInt32>(lParam));
			else if (mcmTarget.valid)
				DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_COMPOSITION action=result_inserted_mcm_extender input=0x%08X", static_cast<UInt32>(lParam));
			else
				DebugLogState("WndProc.WM_IME_COMPOSITION", "result_inserted", menu, static_cast<SInt32>(lParam));
			DebugLog("tnvse_multibyte_input: committed IME result chars=%u", State().suppressedImeCharCount);
			return true;
		}

		std::wstring GetStewieImeEnterLiteral(HWND hwnd)
		{
			std::wstring literal = GetImeCompositionString(hwnd, GCS_COMPREADSTR);
			if (literal.empty())
				literal = GetImeCompositionString(hwnd, GCS_COMPSTR);
			if (literal.empty())
				literal = State().candidate.composition;

			if (g_uiEncoding == 1)
			{
				literal.erase(
					std::remove_if(
						literal.begin(),
						literal.end(),
						[](wchar_t value)
						{
							return !((value >= L'A' && value <= L'Z')
								|| (value >= L'a' && value <= L'z'));
						}),
					literal.end());
			}

			return literal;
		}

		bool HandleStewieImeEnter(const StewieInputTarget& target)
		{
			if (!target.valid || !s_window || !IsConfiguredImeLayout(s_window))
				return false;

			RefreshImeStatus(s_window);
			const bool imeWasOpen = State().candidate.imeOpen;
			const bool imeWasNative =
				(State().candidate.conversionMode & IME_CMODE_NATIVE) != 0;

			std::wstring composition = GetStewieImeEnterLiteral(s_window);

			if (!composition.empty())
			{
				CancelDeferredStewieAscii();
				State().lastStewieImeEnterKeyTick = 0;
				CancelGameImeComposition(s_window);
				s_imeComposing = false;
				ClearImePreviewState();

				const bool inserted = InsertWideTextStewie(target, composition);
				EnsureConfiguredImeOpen(s_window, "menusearch_enter_literal");
				RefreshImeStatus(s_window);
				UpdateCandidateOverlay();
				DebugLog(
					"tnvse_multibyte_input_event: source=MenuSearch.Enter action=commit_ime_literal chars=%u inserted=%u",
					static_cast<UInt32>(composition.size()),
					inserted ? 1 : 0);
				return true;
			}

			if (IsImeCompositionActive() || !State().candidate.candidates.empty())
			{
				State().lastStewieImeEnterKeyTick = 0;
				return true;
			}

			constexpr DWORD kImeEnterPairMs = 250;
			if (State().lastStewieImeCommitTick
				&& GetTickCount() - State().lastStewieImeCommitTick <= kImeEnterPairMs)
			{
				State().lastStewieImeCommitTick = 0;
				DebugLog("tnvse_multibyte_input_event: source=MenuSearch.Enter action=suppress_paired_ime_enter");
				return true;
			}

			// An empty Enter belongs to the active menu-search session whenever the
			// configured IME layout is selected, including its English sub-mode.
			// Stewie's InputField otherwise deactivates while the visible search tile
			// remains open, leaving both the IME session and the next Ctrl+F out of sync.
			State().lastStewieImeEnterKeyTick = 0;
			UpdateCandidateOverlay();
			DebugLog(
				"tnvse_multibyte_input_event: source=MenuSearch.Enter action=suppress_empty_configured_ime_enter open=%u native=%u",
				imeWasOpen ? 1 : 0,
				imeWasNative ? 1 : 0);
			return true;
		}

		bool ShouldSuppressDuplicateImeChar()
		{
			if (!State().suppressedImeCharCount)
				return false;

			if (GetTickCount() - State().lastImeCommitTick > kDuplicateImeCharSuppressMs)
			{
				State().suppressedImeCharCount = 0;
				return false;
			}

			--State().suppressedImeCharCount;
			return true;
		}

		bool HandleCharFallback(WPARAM wParam, bool controlDown)
		{
			if (ShouldSuppressDialogueHistoryControlChar(wParam))
				return true;
			if (ShouldSuppressMcmExtenderControlChar(wParam))
				return true;

			if (ShouldSuppressImeCommitInput(
				static_cast<UInt32>(wParam),
				ImeCommitInputChannel::WndProcChar))
			{
				return true;
			}

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
			StewieInputTarget stewieTarget = (!menu && !jipMenu) ? GetActiveStewieInputTarget() : StewieInputTarget();
			DialogueHistoryInputTarget dialogueHistoryTarget =
				(!menu && !jipMenu && !stewieTarget.valid)
					? GetActiveDialogueHistoryInputTarget()
					: DialogueHistoryInputTarget();
			McmExtenderInputTarget mcmTarget =
				(!menu && !jipMenu && !stewieTarget.valid && !dialogueHistoryTarget.valid)
				? GetActiveMcmExtenderInputTarget()
				: McmExtenderInputTarget();
			if (!menu && !jipMenu)
			{
				if (dialogueHistoryTarget.valid)
					return HandleDialogueHistoryWndProcChar(
						dialogueHistoryTarget, wParam, controlDown);

				if (mcmTarget.valid)
					return HandleMcmExtenderWndProcChar(
						mcmTarget, wParam, controlDown);

				if (stewieTarget.valid)
				{
					if (wParam >= 0x20 && wParam <= 0x7E)
					{
						if (IsImeConsumingAscii())
						{
							ObserveImeCommitInput(static_cast<UInt32>(wParam));
							DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_CHAR action=suppress_composition_ascii_stewie input=0x%08X", static_cast<UInt32>(wParam));
							return true;
						}

						return HandleStewieWndProcAscii(stewieTarget, static_cast<UInt8>(wParam));
					}

					if (wParam < 0x80)
						return false;

					const wchar_t ch = static_cast<wchar_t>(wParam);
					if (!InsertWideTextStewie(stewieTarget, std::wstring_view(&ch, 1)))
						return false;

					DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_CHAR action=insert_nonascii_stewie input=0x%08X", static_cast<UInt32>(wParam));
					return true;
				}

				if (wParam >= 0x20 && wParam <= 0x7E && HasOverlayInputTarget() && IsImeConsumingAscii())
				{
					ObserveImeCommitInput(static_cast<UInt32>(wParam));
					DebugLogState("WndProc.WM_CHAR", "suppress_overlay_composition_ascii", GetOverlayTextInputMenu(), static_cast<SInt32>(wParam));
					return true;
				}

				DebugLogState("WndProc.WM_CHAR", "pass_no_active_target", nullptr, static_cast<SInt32>(wParam));
				return false;
			}

			if (jipMenu)
			{
				if (wParam >= 0x20 && wParam <= 0x7E)
				{
					if (IsImeConsumingAscii())
					{
						ObserveImeCommitInput(static_cast<UInt32>(wParam));
						DebugLogJipState("WndProc.WM_CHAR", "suppress_composition_ascii", jipMenu, static_cast<UInt32>(wParam));
						return true;
					}

					DebugLogJipState("WndProc.WM_CHAR", "consume_ascii_handled_by_jip_adapter", jipMenu, static_cast<UInt32>(wParam));
					return true;
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
				ObserveImeCommitInput(static_cast<UInt32>(wParam));
				DebugLogState("WndProc.WM_CHAR", "suppress_composition_ascii", menu, static_cast<SInt32>(wParam));
				return true;
			}

			if (wParam >= 0x20 && wParam <= 0x7E)
			{
				DebugLogState("WndProc.WM_CHAR", "consume_ascii_handled_by_textedit", menu, static_cast<SInt32>(wParam));
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

		namespace
		{
			constexpr size_t kCapturedInputEventCapacity = 128;

			struct CapturedInputEvent
			{
				UINT message = 0;
				WPARAM wParam = 0;
				LPARAM lParam = 0;
				bool controlDown = false;
				bool winDown = false;
				std::wstring result;
				std::wstring composition;
			};

			std::array<CapturedInputEvent, kCapturedInputEventCapacity>
				s_capturedInputEvents;
			size_t s_capturedInputRead = 0;
			size_t s_capturedInputWrite = 0;
			size_t s_capturedInputCount = 0;
			UInt32 s_droppedCapturedInputEvents = 0;
			bool s_pumpingCapturedInputEvents = false;

			bool EnqueueCapturedInputEvent(CapturedInputEvent event)
			{
				if (s_capturedInputCount == s_capturedInputEvents.size())
				{
					++s_droppedCapturedInputEvents;
					return false;
				}

				s_capturedInputEvents[s_capturedInputWrite] = std::move(event);
				s_capturedInputWrite =
					(s_capturedInputWrite + 1) % s_capturedInputEvents.size();
				++s_capturedInputCount;
				return true;
			}

			bool DequeueCapturedInputEvent(CapturedInputEvent& event)
			{
				if (!s_capturedInputCount)
					return false;

				event = std::move(s_capturedInputEvents[s_capturedInputRead]);
				s_capturedInputEvents[s_capturedInputRead] = {};
				s_capturedInputRead =
					(s_capturedInputRead + 1) % s_capturedInputEvents.size();
				--s_capturedInputCount;
				return true;
			}

			bool IsMouseRoutingMessage(UINT message)
			{
				if ((message >= WM_NCMOUSEMOVE
						&& message <= WM_NCXBUTTONDBLCLK)
					|| (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST))
				{
					return true;
				}

				switch (message)
				{
				case WM_NCHITTEST:
				case WM_SETCURSOR:
				case WM_CAPTURECHANGED:
				case WM_NCMOUSEHOVER:
				case WM_MOUSEHOVER:
				case WM_NCMOUSELEAVE:
				case WM_MOUSELEAVE:
					return true;
				default:
					return false;
				}
			}

			bool IsKeyboardMessage(UINT message)
			{
				return message == WM_KEYDOWN
					|| message == WM_SYSKEYDOWN
					|| message == WM_KEYUP
					|| message == WM_SYSKEYUP;
			}

			bool IsFocusCaptureMessage(UINT message)
			{
				return message == WM_SETFOCUS
					|| message == WM_ACTIVATEAPP
					|| message == WM_ACTIVATE;
			}

			bool IsDeferredEditorKey(WPARAM key, bool controlDown)
			{
				switch (key)
				{
				case VK_BACK:
				case VK_DELETE:
				case VK_LEFT:
				case VK_RIGHT:
				case VK_HOME:
				case VK_END:
				case VK_RETURN:
				case VK_ESCAPE:
				case VK_TAB:
					return true;
				default:
					return key == 'F' && controlDown;
				}
			}

			bool ShouldCaptureWindowMessage(
				UINT message,
				WPARAM wParam,
				bool sessionActive,
				bool controlDown)
			{
				if (message == kMessage_FlushDeferredStewieAscii
					|| IsImeWindowMessage(message)
					|| message == WM_INPUTLANGCHANGE
					|| IsFocusCaptureMessage(message))
				{
					return true;
				}

				if (message == WM_CHAR)
				{
					return sessionActive
						|| wParam == 0x06
						|| wParam == 0x12;
				}

				if (!IsKeyboardMessage(message))
					return false;
				if (sessionActive
					|| wParam == VK_LWIN
					|| wParam == VK_RWIN)
				{
					return true;
				}
				return (wParam == 'F' || wParam == 'R') && controlDown;
			}

			bool IsPotentialInputCaptureMessage(UINT message)
			{
				return message == kMessage_FlushDeferredStewieAscii
					|| IsImeWindowMessage(message)
					|| message == WM_INPUTLANGCHANGE
					|| message == WM_CHAR
					|| IsKeyboardMessage(message)
					|| IsFocusCaptureMessage(message);
			}

			LRESULT ForwardWindowMessage(
				HWND hwnd,
				UINT message,
				WPARAM wParam,
				LPARAM lParam)
			{
				WNDPROC original = s_originalWndProc;
				return original
					? CallWindowProcA(original, hwnd, message, wParam, lParam)
					: DefWindowProcA(hwnd, message, wParam, lParam);
			}
		}

		LRESULT ProcessCapturedInputMessage(
			HWND hwnd,
			const CapturedInputEvent& event)
		{
			const UINT msg = event.message;
			const WPARAM wParam = event.wParam;
			const LPARAM lParam = event.lParam;
			ImeState& state = State();
			if (s_hooksInstalled)
			{
				if (msg == kMessage_FlushDeferredStewieAscii)
				{
					FlushDeferredStewieAscii(static_cast<UInt32>(wParam));
					return 0;
				}

				ObserveStewieMenuSearchHotkeyMessage(
					msg, wParam, lParam, event.controlDown);
				TextEditMenu* inputTarget = GetOverlayTextInputMenu();
				StewieInputTarget stewieOverlayTarget = GetOverlayStewieInputTarget();
				DialogueHistoryInputTarget dialogueHistoryOverlayTarget =
					(!inputTarget && !stewieOverlayTarget.valid)
						? GetOverlayDialogueHistoryInputTarget()
						: DialogueHistoryInputTarget();
				McmExtenderInputTarget mcmOverlayTarget =
					(!inputTarget && !stewieOverlayTarget.valid
						&& !dialogueHistoryOverlayTarget.valid)
					? GetOverlayMcmExtenderInputTarget()
					: McmExtenderInputTarget();
				const bool hasInputTarget = inputTarget
					|| stewieOverlayTarget.valid
					|| dialogueHistoryOverlayTarget.valid
					|| mcmOverlayTarget.valid;
				if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
					&& wParam == VK_RETURN
					&& stewieOverlayTarget.valid
					&& IsConfiguredImeLayout(hwnd))
				{
					state.lastStewieImeEnterKeyTick = GetTickCount();
				}
				if (hasInputTarget)
				{
					SetTextInputSessionActive(true);
				}
				else if (state.textInputSessionActive && GetCurrentTextEditMenuObject() == nullptr)
				{
					SetTextInputSessionActive(false);
				}

				ObserveImeCommitKeyMessage(msg, wParam, lParam, hasInputTarget);

				if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
					&& dialogueHistoryOverlayTarget.valid
					&& HandleDialogueHistoryKeyDown(
						dialogueHistoryOverlayTarget, wParam, event.controlDown))
				{
					return 0;
				}

				if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
					&& mcmOverlayTarget.valid
					&& HandleMcmExtenderKeyDown(
						mcmOverlayTarget, wParam, event.controlDown))
				{
					return 0;
				}

				if (IsWindowsKeyMessage(msg, wParam))
				{
					const bool keyDown = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
					if (keyDown && state.textInputSessionActive && !state.winSpaceSwitchPending)
					{
						state.winSpaceLayoutBefore = GetGameKeyboardLayout(hwnd);
						state.winSpaceChordArmed = true;
						state.winSpaceLanguageChangeObserved = false;
					}
					else if (!keyDown && !state.winSpaceSwitchPending)
					{
						state.winSpaceLayoutBefore = nullptr;
						state.winSpaceChordArmed = false;
						state.winSpaceLanguageChangeObserved = false;
					}
				}

				if (msg == WM_INPUTLANGCHANGEREQUEST && state.textInputSessionActive)
				{
					DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_INPUTLANGCHANGEREQUEST action=def_window_proc");
					return DefWindowProcA(hwnd, msg, wParam, lParam);
				}

				if (msg == WM_INPUTLANGCHANGE && state.textInputSessionActive)
				{
					ResetImeCommitKeyState("input_language_change");
					HKL newLayout = reinterpret_cast<HKL>(lParam);
					if (state.winSpaceChordArmed || state.winSpaceSwitchPending)
					{
						state.winSpaceLanguageChangeObserved = true;
						// The shell may consume the Space key messages entirely. Treat
						// the observed layout change as confirmation of the armed chord,
						// and cover MenuSearch's delayed keyboard polling from here.
						state.inputLanguageSwitchGuardUntilTick = GetTickCount()
							+ kInputLanguageSwitchAsciiGuardMs;
						SuppressStewieInputLanguageSwitchSpace();
					}
					RestoreDefaultGameImeContext(hwnd, "inputlangchange", newLayout);
					ClearImeCandidates();
					UpdateCandidateOverlay();
					DebugLog(
						"tnvse_multibyte_input_event: source=WndProc.WM_INPUTLANGCHANGE action=refresh_ime_name layout=0x%08X configured=%u",
						static_cast<UInt32>(reinterpret_cast<ULONG_PTR>(newLayout)),
						LayoutMatchesCurrentEncoding(newLayout) ? 1 : 0);
					return 0;
				}

				if (IsPendingWinSpaceRelease(msg, wParam))
				{
					// Fullscreen language-switch UI can delay Stewie's polled Space
					// until after the key-up message. Restart the guard from key-up
					// and remove any Space that arrived while the shell was switching.
					state.inputLanguageSwitchGuardUntilTick = GetTickCount()
						+ kInputLanguageSwitchAsciiGuardMs;
					SuppressStewieInputLanguageSwitchSpace();
					const HKL layoutBefore = state.winSpaceLayoutBefore;
					state.winSpaceSwitchPending = false;

					HKL currentLayout = GetGameKeyboardLayout(hwnd);
					const bool systemChangedLayout = state.winSpaceLanguageChangeObserved
						|| currentLayout != layoutBefore;
					HKL previousLayout = currentLayout;
					if (!systemChangedLayout)
					{
						// Win+Space is a shell hotkey. Normally its
						// WM_INPUTLANGCHANGEREQUEST has already switched the focused
						// thread by key release. Only compensate when the shell did
						// not change anything; switching on key-down races the shell
						// and can advance twice back to the original layout.
						previousLayout = ActivateKeyboardLayout(
							reinterpret_cast<HKL>(HKL_NEXT),
							KLF_SETFORPROCESS);
						currentLayout = GetGameKeyboardLayout(hwnd);
					}

					RestoreDefaultGameImeContext(hwnd, "winspace_complete", currentLayout);
					ClearImeCandidates();
					UpdateCandidateOverlay();
					if (event.winDown)
					{
						// Keep the original layout for the next Space press while Win
						// remains held, matching the shell's multi-layout cycling UI.
						state.winSpaceLayoutBefore = currentLayout;
						state.winSpaceChordArmed = true;
						state.winSpaceLanguageChangeObserved = false;
					}
					else
					{
						state.winSpaceLayoutBefore = nullptr;
						state.winSpaceChordArmed = false;
						state.winSpaceLanguageChangeObserved = false;
					}
					DebugLog(
						"tnvse_multibyte_input_event: source=WndProc action=winspace_complete systemChanged=%u previous=0x%08X current=0x%08X",
						systemChangedLayout ? 1 : 0,
						static_cast<UInt32>(reinterpret_cast<ULONG_PTR>(previousLayout)),
						static_cast<UInt32>(reinterpret_cast<ULONG_PTR>(currentLayout)));
					return 0;
				}

				if (state.textInputSessionActive
					&& (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
					&& wParam == VK_SPACE
					&& event.winDown)
				{
					state.inputLanguageSwitchGuardUntilTick = GetTickCount()
						+ kInputLanguageSwitchAsciiGuardMs;
					SuppressStewieInputLanguageSwitchSpace();
					if (!state.winSpaceChordArmed)
					{
						state.winSpaceLayoutBefore = GetGameKeyboardLayout(hwnd);
						state.winSpaceChordArmed = true;
						state.winSpaceLanguageChangeObserved = false;
					}
					state.winSpaceSwitchPending = true;
					DebugLog(
						"tnvse_multibyte_input_event: source=WndProc action=winspace_wait_for_system layout=0x%08X",
						static_cast<UInt32>(reinterpret_cast<ULONG_PTR>(state.winSpaceLayoutBefore)));
					return 0;
				}

				if (state.textInputSessionActive && IsFocusRestoreMessage(msg, wParam))
				{
					RestoreDefaultGameImeContext(hwnd, "focus_restore");
					ClearImeCandidates();
					UpdateCandidateOverlay();
					DebugLog("tnvse_multibyte_input_event: source=WndProc action=focus_restore_ime msg=0x%04X", static_cast<UInt32>(msg));
				}

				if (hasInputTarget)
				{
					SetGameImeEnabled(hwnd, true);
				}
				else if (IsImeWindowMessage(msg))
				{
					SetGameImeEnabled(hwnd, false);
					DebugLog("tnvse_multibyte_input_event: source=WndProc action=suppress_ime_without_target msg=0x%04X", static_cast<UInt32>(msg));
					if (msg == WM_IME_SETCONTEXT)
						return DefWindowProcA(hwnd, WM_IME_SETCONTEXT, wParam, 0);
					return 0;
				}

				if (msg == WM_IME_STARTCOMPOSITION && hasInputTarget)
				{
					// A few IMEs can open the next composition before the game has
					// drained the just-committed physical key. Preserve a confirmed
					// latch, but discard an unconfirmed key from the old composition.
					if (!state.commitKey.confirmed)
						ResetImeCommitKeyState("composition_start");
					CancelDeferredStewieAscii();
					if (static_cast<SInt32>(state.inputLanguageSwitchGuardUntilTick
						- GetTickCount()) > 0)
					{
						// If the first phonetic key starts composition immediately after
						// Win+Space, remove a late polled hotkey Space before committing
						// any multibyte result. This only runs inside the switch guard.
						SuppressStewieInputLanguageSwitchSpace();
						state.inputLanguageSwitchGuardUntilTick = 0;
					}
					HideSystemImeWindows(hwnd);
					s_imeComposing = true;
					state.compositionEchoChecked = false;
					state.candidate.composing = true;
					RefreshImeStatus(hwnd);
					state.candidate.composition = event.composition;
					TryRemoveCompositionEcho();
					// Candidate data is not valid until TSF or IMN_OPENCANDIDATE /
					// IMN_CHANGECANDIDATE publishes it for this composition. Reading
					// IMM here can return the previous composition's cached list.
					ClearImeCandidates();
					UpdateCandidateOverlay();
					DebugLogState("WndProc.WM_IME_STARTCOMPOSITION", "composition_start", GetAnyActiveTextInputMenu(), 0);
				}

				if (msg == WM_IME_NOTIFY && hasInputTarget)
				{
					RefreshImeStatus(hwnd);
					switch (wParam)
					{
					case IMN_OPENCANDIDATE:
					case IMN_SETCANDIDATEPOS:
					case IMN_CHANGECANDIDATE:
						// Candidate list updates are driven by WM_IME_NOTIFY and the TSF
						// UI element sink. Do not poll IMM during composition text updates;
						// some IMEs retain the previous list until the open/change notify.
						DebugLog(
							"tnvse_multibyte_input_event: source=WndProc.WM_IME_NOTIFY action=refresh_candidates notify=0x%08X count=%u selection=%u pageStart=%u pageSize=%u",
							static_cast<UInt32>(wParam),
							static_cast<UInt32>(state.candidate.candidates.size()),
							static_cast<UInt32>(state.candidate.selection),
							static_cast<UInt32>(state.candidate.pageStart),
							static_cast<UInt32>(state.candidate.pageSize));
						return 0;
					case IMN_CLOSECANDIDATE:
						ClearImeCandidates();
						DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_NOTIFY action=close_candidates");
						return 0;
					case IMN_SETOPENSTATUS:
					case IMN_SETCONVERSIONMODE:
					case IMN_SETSENTENCEMODE:
						DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_NOTIFY action=refresh_ime_status");
						break;
					default:
						break;
					}
				}

				if (msg == WM_IME_COMPOSITION)
				{
					if (lParam & GCS_COMPSTR)
						CancelDeferredStewieAscii();

					TextEditMenu* activeTarget = GetAnyActiveTextInputMenu();
					TextEditMenu* overlayTarget = inputTarget;
					if (hasInputTarget)
						HideSystemImeWindows(hwnd);
					DebugLog(
						"tnvse_multibyte_input_event: source=WndProc.WM_IME_COMPOSITION lParam=0x%08X hasResult=%u hasComp=%u composingBefore=%u active=0x%08X overlay=0x%08X stewie=%u dialogueHistory=%u mcm=%u",
						static_cast<UInt32>(lParam),
						(lParam & GCS_RESULTSTR) ? 1 : 0,
						(lParam & GCS_COMPSTR) ? 1 : 0,
						s_imeComposing ? 1 : 0,
						reinterpret_cast<UInt32>(activeTarget),
						reinterpret_cast<UInt32>(overlayTarget),
						stewieOverlayTarget.valid ? 1 : 0,
						dialogueHistoryOverlayTarget.valid ? 1 : 0,
						mcmOverlayTarget.valid ? 1 : 0);

					if (ApplyCapturedImeResult(event.result, lParam))
					{
						s_imeComposing = false;
						DebugLogState("WndProc.WM_IME_COMPOSITION", "composition_result_consumed", GetAnyActiveTextInputMenu(), static_cast<SInt32>(lParam));
						return 0;
					}

					if (hasInputTarget)
					{
						if (lParam & GCS_RESULTSTR)
						{
							s_imeComposing = false;
							ClearImePreviewState();
						}
						else
						{
							s_imeComposing = true;
							state.candidate.composing = true;
						}
						RefreshImeStatus(hwnd);
						if (lParam & GCS_COMPSTR)
						{
							state.candidate.composition = event.composition;
							TryRemoveCompositionEcho();
						}
						RefreshImeCandidates(hwnd);
						UpdateCandidateOverlay();
						DebugLogState("WndProc.WM_IME_COMPOSITION", "composition_continue", GetAnyActiveTextInputMenu(), static_cast<SInt32>(lParam));
						return 0;
					}
				}

				if (msg == WM_IME_ENDCOMPOSITION)
				{
					s_imeComposing = false;
					ClearImePreviewState();
					RefreshImeStatus(hwnd);
					UpdateCandidateOverlay();
					DebugLogState("WndProc.WM_IME_ENDCOMPOSITION", "composition_end", GetAnyActiveTextInputMenu(), 0);
					if (hasInputTarget)
						return 0;
				}

				if (msg == WM_IME_SETCONTEXT
					&& g_bMultibyteInputHideSystemCandidateWindow
					&& IsCandidateOverlayRendererAvailable()
					&& hasInputTarget)
				{
					HideSystemImeWindows(hwnd);
					return DefWindowProcA(hwnd, WM_IME_SETCONTEXT, wParam, 0);
				}

				if (msg == WM_IME_CHAR && hasInputTarget)
				{
					DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_CHAR action=suppress_ime_char input=0x%08X", static_cast<UInt32>(wParam));
					return 0;
				}

				if (msg == WM_CHAR
					&& HandleCharFallback(wParam, event.controlDown))
					return 0;
			}

			return 0;
		}

		LRESULT CALLBACK MultibyteInputWndProc(
			HWND hwnd,
			UINT msg,
			WPARAM wParam,
			LPARAM lParam)
		{
			// Mouse routing is deliberately the first branch. No tNVSE state,
			// menu object, Tile, IME context, hook slot, or log is touched before
			// forwarding mouse/hit-test/capture traffic to the existing chain.
			if (IsMouseRoutingMessage(msg))
				return ForwardWindowMessage(hwnd, msg, wParam, lParam);

			if (msg == WM_NCDESTROY && hwnd == s_window)
			{
				const LRESULT result =
					ForwardWindowMessage(hwnd, msg, wParam, lParam);
				ClearCapturedInputEvents();
				State().overlayRefreshPending = false;
				s_originalWndProc = nullptr;
				s_window = nullptr;
				return result;
			}

			if (!s_hooksInstalled)
				return ForwardWindowMessage(hwnd, msg, wParam, lParam);

			// The allowlist makes raw-input, pointer, touch, gesture and any future
			// mouse-related messages unconditional pass-through even if they are
			// outside the legacy WM_MOUSE/WM_NCMOUSE numeric ranges above.
			if (!IsPotentialInputCaptureMessage(msg))
				return ForwardWindowMessage(hwnd, msg, wParam, lParam);

			ImeState& state = State();
			const bool sessionActive = state.textInputSessionActive;
			const bool keyboardStateRelevant =
				IsKeyboardMessage(msg) || msg == WM_CHAR;
			const bool controlDown =
				keyboardStateRelevant && IsCtrlKeyDown();
			const bool winDown =
				keyboardStateRelevant
				&& (IsVirtualKeyDown(VK_LWIN)
					|| IsVirtualKeyDown(VK_RWIN));

			// Suppressing the default IME windows requires changing only the
			// parameters passed to DefWindowProc. No game UI is queried here.
			if (msg == WM_IME_SETCONTEXT)
			{
				if (sessionActive
					&& g_bMultibyteInputHideSystemCandidateWindow
					&& IsCandidateOverlayRendererAvailable())
				{
					return DefWindowProcA(
						hwnd, WM_IME_SETCONTEXT, wParam, 0);
				}
				return ForwardWindowMessage(hwnd, msg, wParam, lParam);
			}

			if (!ShouldCaptureWindowMessage(
					msg, wParam, sessionActive, controlDown))
				return ForwardWindowMessage(hwnd, msg, wParam, lParam);

			CapturedInputEvent event;
			event.message = msg;
			event.wParam = wParam;
			event.lParam = lParam;
			event.controlDown = controlDown;
			event.winDown = winDown;

			if (msg == WM_IME_STARTCOMPOSITION && sessionActive)
			{
				s_imeComposing = true;
				state.candidate.composing = true;
				event.composition =
					GetImeCompositionString(hwnd, GCS_COMPSTR);
			}
			else if (msg == WM_IME_COMPOSITION && sessionActive)
			{
				if (lParam & GCS_RESULTSTR)
					event.result =
						GetImeCompositionString(hwnd, GCS_RESULTSTR);
				if (lParam & GCS_COMPSTR)
				{
					event.composition =
						GetImeCompositionString(hwnd, GCS_COMPSTR);
					s_imeComposing = true;
					state.candidate.composing = true;
				}
				else if (lParam & GCS_RESULTSTR)
				{
					s_imeComposing = false;
				}
			}
			else if (msg == WM_IME_ENDCOMPOSITION && sessionActive)
			{
				s_imeComposing = false;
			}
			else if (msg == WM_IME_NOTIFY && sessionActive)
			{
				// Candidate lists are transient IME data, so copy them while the
				// notification is live. This function touches IMM state only.
				switch (wParam)
				{
				case IMN_OPENCANDIDATE:
				case IMN_SETCANDIDATEPOS:
				case IMN_CHANGECANDIDATE:
					RefreshImeCandidates(hwnd);
					break;
				case IMN_CLOSECANDIDATE:
					ClearImeCandidates();
					break;
				default:
					break;
				}
				state.overlayRefreshPending = true;
			}

			// Preserve the normal consume/forward contract even if the bounded
			// queue is saturated. Forwarding an uncaptured IME commit into the
			// byte-oriented game path would be more damaging than dropping it;
			// the main loop reports the overflow on its next pass.
			EnqueueCapturedInputEvent(std::move(event));

			if (msg == kMessage_FlushDeferredStewieAscii)
				return 0;
			if (msg == WM_CHAR && sessionActive)
				return 0;
			if (msg == WM_IME_COMPOSITION
				|| msg == WM_IME_ENDCOMPOSITION
				|| msg == WM_IME_CHAR)
			{
				return sessionActive
					? 0
					: ForwardWindowMessage(hwnd, msg, wParam, lParam);
			}
			if (msg == WM_IME_NOTIFY
				&& sessionActive
				&& (wParam == IMN_OPENCANDIDATE
					|| wParam == IMN_SETCANDIDATEPOS
					|| wParam == IMN_CHANGECANDIDATE
					|| wParam == IMN_CLOSECANDIDATE))
			{
				return 0;
			}
			if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
				&& sessionActive
				&& (IsDeferredEditorKey(wParam, event.controlDown)
					|| (wParam == VK_SPACE
						&& event.winDown)))
			{
				return 0;
			}

			return ForwardWindowMessage(hwnd, msg, wParam, lParam);
		}

		void PumpCapturedInputEvents()
		{
			if (s_pumpingCapturedInputEvents)
				return;
			struct PumpGuard
			{
				PumpGuard()
				{
					s_pumpingCapturedInputEvents = true;
				}
				~PumpGuard()
				{
					s_pumpingCapturedInputEvents = false;
				}
			} guard;

			if (s_droppedCapturedInputEvents)
			{
				gLog.FormattedMessage(
					"tnvse_multibyte_input: captured input queue overflow dropped=%u",
					s_droppedCapturedInputEvents);
				s_droppedCapturedInputEvents = 0;
			}

			CapturedInputEvent event;
			// Process only the snapshot that existed at frame entry.  Menu/IME
			// work can synchronously dispatch more window messages; draining those
			// recursively in the same pass can otherwise starve the game loop
			// indefinitely.  Newly captured messages remain ordered for the next
			// frame, and the fixed queue still bounds memory.
			const size_t frameEventCount = std::min(
				s_capturedInputCount, kCapturedInputEventCapacity);
			for (size_t index = 0; index < frameEventCount
				&& DequeueCapturedInputEvent(event); ++index)
			{
				ProcessCapturedInputMessage(s_window, event);
			}

			if (State().overlayRefreshPending)
			{
				State().overlayRefreshPending = false;
				UpdateCandidateOverlay();
			}
		}

		void ClearCapturedInputEvents()
		{
			for (CapturedInputEvent& event : s_capturedInputEvents)
				event = {};
			s_capturedInputRead = 0;
			s_capturedInputWrite = 0;
			s_capturedInputCount = 0;
			s_droppedCapturedInputEvents = 0;
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
			SetGameImeEnabled(hwnd, false);
			DebugLog("tnvse_multibyte_input: subclassed hwnd=0x%08X", reinterpret_cast<UInt32>(hwnd));
			return true;
		}

		void ClearInputState()
		{
			ClearCapturedInputEvents();
			SetJipKeyEventSuppressionCaptureActive(false);
			State().textInputSessionActive = false;
			State().overlayRefreshPending = false;
			s_imeComposing = false;
			State().tsfSessionGeneration = 1;
			State().tsfUiElementSessions.clear();
			ClearImePreviewState();
			HideCandidateOverlay();
			ReleaseCandidateOverlayTexture();
			State().suppressedImeCharCount = 0;
			State().lastImeCommitTick = 0;
			State().lastStewieImeCommitTick = 0;
			State().lastStewieImeEnterKeyTick = 0;
			State().inputLanguageSwitchGuardUntilTick = 0;
			State().winSpaceLayoutBefore = nullptr;
			State().winSpaceChordArmed = false;
			State().winSpaceSwitchPending = false;
			State().winSpaceLanguageChangeObserved = false;
			ResetImeCommitKeyState("clear_input_state");
			s_lastWndProcAsciiTick = 0;
			s_lastWndProcAsciiChar = 0;
			ClearJipTextInputHookState();
			ResetStewieInputState();
		}

		void RestoreWindowProc()
		{
			RestoreTextEditInputHook();

			bool detached = true;
			if (s_window && s_originalWndProc)
			{
				SetGameImeEnabled(s_window, true);
				const WNDPROC current = reinterpret_cast<WNDPROC>(
					GetWindowLongPtrA(s_window, GWLP_WNDPROC));
				if (current == &MultibyteInputWndProc)
				{
					detached = SetWindowLongPtrA(
						s_window,
						GWLP_WNDPROC,
						reinterpret_cast<LONG_PTR>(s_originalWndProc)) != 0;
				}
				else if (current != s_originalWndProc)
				{
					// Another plugin installed above tNVSE. Replacing the top of
					// that chain would strand its saved predecessor and can freeze
					// input dispatch. Leave this node connected until window
					// destruction instead.
					detached = false;
					gLog.FormattedMessage(
						"tnvse_multibyte_input: deferred WndProc detach because a later subclass is active current=0x%08X",
						reinterpret_cast<UInt32>(current));
				}
			}

			ClearInputState();
			if (detached)
			{
				s_window = nullptr;
				s_originalWndProc = nullptr;
				ShutdownTsfCandidateSupport();
			}
		}
	}
}

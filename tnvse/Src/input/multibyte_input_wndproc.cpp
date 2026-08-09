#include "multibyte_input_ime_internal.h"
#include "hook_site.h"
#include "Utils/SafeWrite.h"

namespace fonthook
{
	namespace multibyte_input
	{
		LRESULT CALLBACK MultibyteInputWndProc(
			HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
		hook_site::WindowProcSite s_gameWindowProcSite{
			"FalloutNV main window WndProc (CALLBACK/__stdcall)",
			&MultibyteInputWndProc
		};
		WNDPROC s_lastUnknownTopWindowProc = nullptr;
		bool s_loggedWindowProcReadFailure = false;

		bool ApplyCapturedImeResult(
			const std::wstring& result,
			LPARAM lParam)
		{
			if (!(lParam & GCS_RESULTSTR))
				return false;

			const TextInputTarget target = GetCachedTextInputTarget();
			if (target.token.kind == TextInputTargetKind::None)
			{
				DebugLogState("WndProc.WM_IME_COMPOSITION", "result_no_active_target", nullptr, static_cast<SInt32>(lParam));
				return false;
			}

			if (result.empty())
			{
				DebugLog(
					"tnvse_multibyte_input_event: source=WndProc.WM_IME_COMPOSITION action=result_empty target=%s input=0x%08X",
					TextInputTargetKindName(target.token.kind),
					static_cast<UInt32>(lParam));
				return false;
			}

			const bool inserted = InsertWideTextIntoTarget(target, result);
			if (!inserted)
			{
				DebugLog(
					"tnvse_multibyte_input: rejected IME result target=%s length=%u",
					TextInputTargetKindName(target.token.kind),
					static_cast<UInt32>(result.size()));
				return false;
			}

			State().lastImeCommitTick = GetTickCount();
			ConfirmImeCommitKey(TextInputTargetCommitChannel(target));
			constexpr DWORD kImeEnterPairMs = 250;
			if (target.token.kind == TextInputTargetKind::Stewie
				&& State().lastStewieImeEnterKeyTick
				&& State().lastImeCommitTick - State().lastStewieImeEnterKeyTick <= kImeEnterPairMs)
			{
				State().lastStewieImeCommitTick = State().lastImeCommitTick;
				State().lastStewieImeEnterKeyTick = 0;
			}
			State().suppressedImeCharCount = static_cast<UInt32>(result.size());
			DebugLog(
				"tnvse_multibyte_input: committed IME result target=%s chars=%u",
				TextInputTargetKindName(target.token.kind),
				State().suppressedImeCharCount);
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

			if (FilterGameInput(
					static_cast<UInt32>(wParam),
					ImeCommitInputChannel::WndProcChar,
					GameInputFilterClass::None)
				== GameInputFilterResult::SuppressImeCommit)
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

			const TextInputTarget target = GetCachedTextInputTarget();
			if (target.token.kind == TextInputTargetKind::DialogueHistory)
				return HandleDialogueHistoryWndProcChar(
					target.dialogueHistory, wParam, controlDown);
			if (target.token.kind == TextInputTargetKind::McmExtender)
				return HandleMcmExtenderWndProcChar(
					target.mcmExtender, wParam, controlDown);

			if (target.token.kind == TextInputTargetKind::Stewie)
			{
				if (wParam >= 0x20 && wParam <= 0x7E)
				{
					if (IsImeConsumingAscii())
					{
						ObserveImeCommitInput(static_cast<UInt32>(wParam));
						DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_CHAR action=suppress_composition_ascii_stewie input=0x%08X", static_cast<UInt32>(wParam));
						return true;
					}

					return HandleStewieWndProcAscii(
						target.stewie, static_cast<UInt8>(wParam));
				}

				if (wParam < 0x80)
					return false;

				const wchar_t ch = static_cast<wchar_t>(wParam);
				if (!InsertWideTextIntoTarget(
						target, std::wstring_view(&ch, 1)))
					return false;

				DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_CHAR action=insert_nonascii_stewie input=0x%08X", static_cast<UInt32>(wParam));
				return true;
			}

			if ((target.token.kind == TextInputTargetKind::TextEdit
					|| target.token.kind
						== TextInputTargetKind::JipTextInput)
				&& !target.token.active)
			{
				if (wParam >= 0x20 && wParam <= 0x7E
					&& IsImeConsumingAscii())
				{
					ObserveImeCommitInput(static_cast<UInt32>(wParam));
					DebugLog(
						"tnvse_multibyte_input_event: source=WndProc.WM_CHAR action=suppress_inactive_target_composition_ascii target=%s input=0x%08X",
						TextInputTargetKindName(target.token.kind),
						static_cast<UInt32>(wParam));
					return true;
				}
				return false;
			}

			if (target.token.kind == TextInputTargetKind::JipTextInput)
			{
				if (wParam >= 0x20 && wParam <= 0x7E)
				{
					if (IsImeConsumingAscii())
					{
						ObserveImeCommitInput(static_cast<UInt32>(wParam));
						DebugLogJipState("WndProc.WM_CHAR", "suppress_composition_ascii", target.textEdit, static_cast<UInt32>(wParam));
						return true;
					}

					DebugLogJipState("WndProc.WM_CHAR", "consume_ascii_handled_by_jip_adapter", target.textEdit, static_cast<UInt32>(wParam));
					return true;
				}

				if (wParam < 0x80)
				{
					DebugLogJipState("WndProc.WM_CHAR", "pass_control_char", target.textEdit, static_cast<UInt32>(wParam));
					return false;
				}

				const wchar_t ch = static_cast<wchar_t>(wParam);
				if (!InsertWideTextIntoTarget(
						target, std::wstring_view(&ch, 1)))
				{
					DebugLogJipState("WndProc.WM_CHAR", "reject_nonascii_insert", target.textEdit, static_cast<UInt32>(wParam));
					return false;
				}

				DebugLogJipState("WndProc.WM_CHAR", "insert_nonascii", target.textEdit, static_cast<UInt32>(wParam));
				return true;
			}

			if (target.token.kind == TextInputTargetKind::TextEdit
				&& wParam >= 0x20 && wParam <= 0x7E
				&& IsImeConsumingAscii())
			{
				ObserveImeCommitInput(static_cast<UInt32>(wParam));
				DebugLogState("WndProc.WM_CHAR", "suppress_composition_ascii", target.textEdit, static_cast<SInt32>(wParam));
				return true;
			}

			if (target.token.kind == TextInputTargetKind::TextEdit
				&& wParam >= 0x20 && wParam <= 0x7E)
			{
				DebugLogState("WndProc.WM_CHAR", "consume_ascii_handled_by_textedit", target.textEdit, static_cast<SInt32>(wParam));
				return true;
			}

			if (target.token.kind == TextInputTargetKind::TextEdit
				&& wParam < 0x80)
			{
				DebugLogState("WndProc.WM_CHAR", "pass_control_char", target.textEdit, static_cast<SInt32>(wParam));
				return false;
			}

			if (target.token.kind == TextInputTargetKind::TextEdit)
			{
				const wchar_t ch = static_cast<wchar_t>(wParam);
				if (!InsertWideTextIntoTarget(
						target, std::wstring_view(&ch, 1)))
				{
					DebugLogState("WndProc.WM_CHAR", "reject_nonascii_insert", target.textEdit, static_cast<SInt32>(wParam));
					return false;
				}

				DebugLogState("WndProc.WM_CHAR", "insert_nonascii", target.textEdit, static_cast<SInt32>(wParam));
				return true;
			}

			if (wParam >= 0x20 && wParam <= 0x7E
				&& HasOverlayInputTarget()
				&& IsImeConsumingAscii())
			{
				ObserveImeCommitInput(static_cast<UInt32>(wParam));
				DebugLogState("WndProc.WM_CHAR", "suppress_overlay_composition_ascii", GetOverlayTextInputMenu(), static_cast<SInt32>(wParam));
				return true;
			}

			DebugLogState("WndProc.WM_CHAR", "pass_no_active_target", nullptr, static_cast<SInt32>(wParam));
			return false;
		}

		namespace implementation::multibyte_input_wndproc {}
		using namespace implementation::multibyte_input_wndproc;

		namespace implementation::multibyte_input_wndproc
		{
			constexpr size_t kCapturedInputEventCapacity = 128;

			struct CapturedInputEvent
			{
				UINT message = 0;
				WPARAM wParam = 0;
				LPARAM lParam = 0;
				DWORD captureTick = 0;
				bool controlDown = false;
				bool winDown = false;
				bool targetBound = false;
				TextInputTargetToken targetToken;
				std::wstring result;
				std::wstring composition;
				bool candidateSnapshotCaptured = false;
				bool candidateSnapshotFromTsf = false;
				bool candidateSnapshotTsfActive = false;
				DWORD candidateSelection = 0;
				DWORD candidatePageStart = 0;
				DWORD candidatePageSize = 0;
				std::vector<std::wstring> candidates;
			};

			std::array<CapturedInputEvent, kCapturedInputEventCapacity>
				s_capturedInputEvents;
			size_t s_capturedInputRead = 0;
			size_t s_capturedInputWrite = 0;
			size_t s_capturedInputCount = 0;
			UInt32 s_droppedCapturedInputEvents = 0;
			bool s_pumpingCapturedInputEvents = false;

			struct WindowMessageForwardFrame
			{
				WindowMessageForwardFrame* previous = nullptr;
				WNDPROC predecessor = nullptr;
				HWND hwnd = nullptr;
				UINT message = 0;
				WPARAM wParam = 0;
				LPARAM lParam = 0;
			};

			thread_local WindowMessageForwardFrame*
				s_activeWindowMessageForward = nullptr;
			volatile LONG s_loggedWindowMessageForwardCycle = 0;

			class ScopedWindowMessageForward
			{
			public:
				ScopedWindowMessageForward(WNDPROC predecessor, HWND hwnd,
					UINT message, WPARAM wParam, LPARAM lParam)
					: m_frame{ s_activeWindowMessageForward, predecessor, hwnd,
						message, wParam, lParam }
				{
					s_activeWindowMessageForward = &m_frame;
				}

				~ScopedWindowMessageForward()
				{
					s_activeWindowMessageForward = m_frame.previous;
				}

				ScopedWindowMessageForward(
					const ScopedWindowMessageForward&) = delete;
				ScopedWindowMessageForward& operator=(
					const ScopedWindowMessageForward&) = delete;

			private:
				WindowMessageForwardFrame m_frame;
			};

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

			void CaptureImeCandidateSnapshot(
				CapturedInputEvent& event,
				const ImeState& state)
			{
				event.candidateSnapshotCaptured = true;
				event.candidateSnapshotFromTsf =
					state.candidate.candidatesFromTsf;
				event.candidateSnapshotTsfActive = state.tsfCandidateActive;
				event.candidateSelection = state.candidate.selection;
				event.candidatePageStart = state.candidate.pageStart;
				event.candidatePageSize = state.candidate.pageSize;
				event.candidates = state.candidate.candidates;
			}

			void PublishImeCandidateSnapshot(
				const CapturedInputEvent& event,
				ImeState& state)
			{
				if (!event.candidateSnapshotCaptured)
					return;

				ClearImeCandidates();
				state.candidate.candidatesFromTsf =
					event.candidateSnapshotFromTsf;
				state.tsfCandidateActive = event.candidateSnapshotTsfActive;
				state.candidate.selection = event.candidateSelection;
				state.candidate.pageStart = event.candidatePageStart;
				state.candidate.pageSize = event.candidatePageSize;
				state.candidate.candidates = event.candidates;
				MarkImeResultHandoffCandidates();
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

			bool IsTargetBoundCapturedMessage(
				UINT message,
				WPARAM wParam,
				bool sessionActive,
				bool controlDown)
			{
				if (!sessionActive)
					return false;
				if (message == WM_CHAR
					|| IsImeWindowMessage(message))
				{
					return true;
				}
				return (message == WM_KEYDOWN
						|| message == WM_SYSKEYDOWN)
					&& IsDeferredEditorKey(wParam, controlDown);
			}

			LRESULT ForwardWindowMessage(
				HWND hwnd,
				UINT message,
				WPARAM wParam,
				LPARAM lParam)
			{
				WNDPROC original = s_predecessorWndProc;
				if (!original)
					return DefWindowProcA(hwnd, message, wParam, lParam);

				for (const WindowMessageForwardFrame* frame =
						s_activeWindowMessageForward;
					frame; frame = frame->previous)
				{
					if (frame->predecessor == original
						&& frame->hwnd == hwnd
						&& frame->message == message
						&& frame->wParam == wParam
						&& frame->lParam == lParam)
					{
						if (InterlockedCompareExchange(
								&s_loggedWindowMessageForwardCycle, 1, 0) == 0)
						{
							gLog.FormattedMessage(
								"tnvse_multibyte_input: recursive WndProc predecessor frame detected predecessor=0x%08X hwnd=0x%08X message=0x%04X; forwarding to DefWindowProcA",
								reinterpret_cast<UInt32>(original),
								reinterpret_cast<UInt32>(hwnd),
								static_cast<UInt32>(message));
						}
						return DefWindowProcA(hwnd, message, wParam, lParam);
					}
				}

				ScopedWindowMessageForward activeForward(
					original, hwnd, message, wParam, lParam);
				return CallWindowProcA(original, hwnd, message, wParam, lParam);
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
			if (event.targetBound
				&& !IsCurrentTextInputTargetToken(event.targetToken))
			{
				DebugLog(
					"tnvse_multibyte_input: discarded stale captured input msg=0x%04X target=%s generation=%u currentGeneration=%u",
					msg,
					TextInputTargetKindName(event.targetToken.kind),
					event.targetToken.generation,
					state.textInputSessionGeneration);
				return 0;
			}
			if (s_hooksInstalled)
			{
				if (msg == kMessage_FlushDeferredStewieAscii)
				{
					FlushDeferredStewieAscii(static_cast<UInt32>(wParam));
					return 0;
				}

				ObserveStewieMenuSearchHotkeyMessage(
					msg, wParam, lParam, event.controlDown);
				const TextInputTarget brokerTarget =
					GetCachedTextInputTarget();
				TextEditMenu* inputTarget =
					(brokerTarget.token.kind == TextInputTargetKind::TextEdit
						|| brokerTarget.token.kind == TextInputTargetKind::JipTextInput)
						? brokerTarget.textEdit
						: nullptr;
				const StewieInputTarget stewieOverlayTarget =
					brokerTarget.token.kind == TextInputTargetKind::Stewie
						? brokerTarget.stewie
						: StewieInputTarget();
				const DialogueHistoryInputTarget dialogueHistoryOverlayTarget =
					brokerTarget.token.kind == TextInputTargetKind::DialogueHistory
						? brokerTarget.dialogueHistory
						: DialogueHistoryInputTarget();
				const McmExtenderInputTarget mcmOverlayTarget =
					brokerTarget.token.kind == TextInputTargetKind::McmExtender
						? brokerTarget.mcmExtender
						: McmExtenderInputTarget();
				const bool hasInputTarget =
					brokerTarget.token.kind != TextInputTargetKind::None;
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

				if (msg == WM_INPUTLANGCHANGE)
				{
					if (!state.textInputSessionActive)
						return 0;
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
					state.associatedGameImeLayout = nullptr;
					state.associatedGameImeGeneration = 0;
					state.hiddenSystemImeUiEpoch = 0;
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

					state.associatedGameImeLayout = nullptr;
					state.associatedGameImeGeneration = 0;
					state.hiddenSystemImeUiEpoch = 0;
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
					state.hiddenSystemImeUiEpoch = 0;
					if (s_imeComposing)
						HideSystemImeWindows(hwnd);
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
					++state.imeCompositionUiEpoch;
					if (!state.imeCompositionUiEpoch)
						++state.imeCompositionUiEpoch;
					state.hiddenSystemImeUiEpoch = 0;
					HideSystemImeWindows(hwnd);
					const bool resultHandoff =
						IsImeResultHandoffActive(event.captureTick);
					const bool preservedCandidates = resultHandoff
						&& state.resultHandoff.candidatesReady
						&& !state.candidate.candidates.empty();
					const bool preservedComposition = resultHandoff
						&& state.resultHandoff.compositionReady
						&& !state.candidate.composition.empty();
					ClearImePreviewState(resultHandoff, event.captureTick);
					s_imeComposing = true;
					state.compositionEchoChecked = false;
					state.candidate.composing = true;
					RefreshImeStatus(hwnd);
					if (!event.composition.empty())
					{
						state.candidate.composition = event.composition;
						state.tsfCompositionFallbackActive = false;
					}
					TryRemoveCompositionEcho();
					ResetImeResultHandoff();
					UpdateCandidateOverlay();
					if (resultHandoff)
					{
						DebugLog(
							"tnvse_multibyte_input_event: source=ImeResultHandoff action=start preserveCandidates=%u preserveComposition=%u",
							preservedCandidates ? 1 : 0,
							preservedComposition ? 1 : 0);
					}
					DebugLogState("WndProc.WM_IME_STARTCOMPOSITION", "composition_start", GetAnyActiveTextInputMenu(), 0);
				}

				if (msg == WM_IME_NOTIFY && hasInputTarget)
				{
					switch (wParam)
					{
					case IMN_OPENCANDIDATE:
						// Some IMM services replace their candidate form when the
						// list is first opened. Reapply the offscreen form once for
						// that edge, not on every composition/position update.
						state.hiddenSystemImeUiEpoch = 0;
						HideSystemImeWindows(hwnd);
						[[fallthrough]];
					case IMN_CHANGECANDIDATE:
						PublishImeCandidateSnapshot(event, state);
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
					case IMN_SETCANDIDATEPOS:
						// Hide/consume the system candidate position notification, but
						// do not reread candidate content or rebuild the native overlay.
						return 0;
					case IMN_CLOSECANDIDATE:
						ClearImeCandidates();
						DebugLog("tnvse_multibyte_input_event: source=WndProc.WM_IME_NOTIFY action=close_candidates");
						return 0;
					case IMN_SETOPENSTATUS:
					case IMN_SETCONVERSIONMODE:
					case IMN_SETSENTENCEMODE:
						RefreshImeStatus(hwnd);
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
						const bool resultHandoff =
							IsImeResultHandoffActive(event.captureTick);
						ClearImePreviewState(resultHandoff, event.captureTick);
						const bool compositionContinues =
							(lParam & GCS_COMPSTR) != 0;
						s_imeComposing = compositionContinues;
						if (compositionContinues)
						{
							state.candidate.composing = true;
							if (!event.composition.empty())
							{
								state.candidate.composition = event.composition;
								state.tsfCompositionFallbackActive = false;
								MarkImeResultHandoffComposition();
							}
							TryRemoveCompositionEcho();
							ResetImeResultHandoff();
						}
						RefreshImeStatus(hwnd);
						UpdateCandidateOverlay();
						DebugLogState("WndProc.WM_IME_COMPOSITION", "composition_result_consumed", GetAnyActiveTextInputMenu(), static_cast<SInt32>(lParam));
						return 0;
					}

					if (hasInputTarget)
					{
						bool overlayChanged = false;
						if (lParam & GCS_RESULTSTR)
						{
							s_imeComposing = false;
							overlayChanged =
								state.candidate.composing
								|| !state.candidate.composition.empty()
								|| !state.candidate.candidates.empty();
							const bool resultHandoff =
								IsImeResultHandoffActive(event.captureTick);
							ClearImePreviewState(
								resultHandoff, event.captureTick);
						}
						else
						{
							s_imeComposing = true;
							overlayChanged = !state.candidate.composing;
							state.candidate.composing = true;
						}
						if (lParam & GCS_COMPSTR)
						{
							if (state.candidate.composition
								!= event.composition)
							{
								state.candidate.composition =
									event.composition;
								overlayChanged = true;
							}
							if (!event.composition.empty())
								state.tsfCompositionFallbackActive = false;
							TryRemoveCompositionEcho();
						}
						if (overlayChanged)
							UpdateCandidateOverlay();
						DebugLogState("WndProc.WM_IME_COMPOSITION", "composition_continue", GetAnyActiveTextInputMenu(), static_cast<SInt32>(lParam));
						return 0;
					}
				}

				if (msg == WM_IME_ENDCOMPOSITION)
				{
					s_imeComposing = false;
					const bool resultHandoff =
						IsImeResultHandoffActive(event.captureTick);
					ClearImePreviewState(resultHandoff, event.captureTick);
					RefreshImeStatus(hwnd);
					UpdateCandidateOverlay();
					if (resultHandoff)
					{
						DebugLog(
							"tnvse_multibyte_input_event: source=ImeResultHandoff action=end preserveCandidates=%u preserveComposition=%u",
							state.resultHandoff.candidatesReady ? 1 : 0,
							state.resultHandoff.compositionReady ? 1 : 0);
					}
					DebugLogState("WndProc.WM_IME_ENDCOMPOSITION", "composition_end", GetAnyActiveTextInputMenu(), 0);
					if (hasInputTarget)
						return 0;
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
				State().tsfInputWindow.store(
					0, std::memory_order_release);
				State().textInputSessionActive.store(
					false, std::memory_order_release);
				State().tsfCandidateActive = false;
				s_imeComposing = false;
				AdvanceTsfCandidateSession();
				State().gameImeEnabled = false;
				State().gameImeContextDetached = false;
				State().gameImeDetachFailureLogged = false;
				State().detachedGameImeWindow = nullptr;
				State().associatedGameImeWindow = nullptr;
				State().associatedGameImeLayout = nullptr;
				State().associatedGameImeGeneration = 0;
				s_window = nullptr;
				const LRESULT result =
					ForwardWindowMessage(hwnd, msg, wParam, lParam);
				ClearCapturedInputEvents();
				State().overlayRefreshPending = false;
				s_predecessorWndProc = nullptr;
				s_gameWindowProcSite.windowHandle = nullptr;
				s_gameWindowProcSite.predecessorProc = nullptr;
				return result;
			}

			if (!s_hooksInstalled)
				return ForwardWindowMessage(hwnd, msg, wParam, lParam);

			if (msg == WM_INPUTLANGCHANGEREQUEST)
			{
				DebugLog(
					"tnvse_multibyte_input_event: source=WndProc.WM_INPUTLANGCHANGEREQUEST action=forward_without_system_ui_restore");
				return ForwardWindowMessage(
					hwnd, msg, wParam, lParam);
			}

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

			// Strip the system composition/candidate UI flags, then preserve the
			// existing subclass chain. This policy is independent of the native
			// Tile host, the current target and composition-preview availability.
			if (msg == WM_IME_SETCONTEXT)
			{
				if (wParam && !sessionActive)
				{
					// A focus transition or another subclass may have rebound the
					// default HIMC. Mark the detached state stale; the main-loop
					// watchdog retries outside this synchronous window callback.
					state.gameImeContextDetached = false;
					state.detachedGameImeWindow = nullptr;
				}
				return ForwardWindowMessage(
					hwnd, WM_IME_SETCONTEXT, wParam, 0);
			}

			if (!ShouldCaptureWindowMessage(
					msg, wParam, sessionActive, controlDown))
				return ForwardWindowMessage(hwnd, msg, wParam, lParam);

			CapturedInputEvent event;
			event.message = msg;
			event.wParam = wParam;
			event.lParam = lParam;
			event.captureTick = GetTickCount();
			event.controlDown = controlDown;
			event.winDown = winDown;
			event.targetBound = IsTargetBoundCapturedMessage(
				msg, wParam, sessionActive, controlDown);
			event.targetToken = CaptureTextInputTargetToken();

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
				{
					ArmImeResultHandoff(event.captureTick);
					event.result =
						GetImeCompositionString(hwnd, GCS_RESULTSTR);
				}
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
				case IMN_CHANGECANDIDATE:
					RefreshImeCandidates(hwnd);
					CaptureImeCandidateSnapshot(event, state);
					MarkImeResultHandoffCandidates();
					state.overlayRefreshPending = true;
					break;
				case IMN_SETCANDIDATEPOS:
					if (g_bMultibyteInputLog)
						++state.ignoredCandidatePositionNotifications;
					break;
				case IMN_CLOSECANDIDATE:
					ClearImeCandidates();
					state.overlayRefreshPending = true;
					break;
				case IMN_SETOPENSTATUS:
				case IMN_SETCONVERSIONMODE:
				case IMN_SETSENTENCEMODE:
					state.overlayRefreshPending = true;
					break;
				default:
					break;
				}
			}

			// Preserve the normal consume/forward contract even if the bounded
			// queue is saturated. Forwarding an uncaptured IME commit into the
			// byte-oriented game path would be more damaging than dropping it;
			// the main loop reports the overflow on its next pass.
			const bool eventQueued = EnqueueCapturedInputEvent(std::move(event));
			if (!eventQueued
				&& msg == WM_IME_COMPOSITION
				&& (lParam & GCS_RESULTSTR))
			{
				ResetImeResultHandoff();
			}

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
				&& (IsDeferredEditorKey(wParam, controlDown)
					|| (wParam == VK_SPACE
						&& winDown)))
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

		bool ReadCurrentWindowProc(HWND hwnd, WNDPROC& current)
		{
			current = nullptr;
			if (!hwnd)
				return false;
			SetLastError(ERROR_SUCCESS);
			const LONG_PTR value = GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
			if (!value && GetLastError() != ERROR_SUCCESS)
				return false;
			current = reinterpret_cast<WNDPROC>(value);
			return current != nullptr;
		}

		bool TryInstallWindowProc(bool* publishedNow)
		{
			if (publishedNow)
				*publishedNow = false;

			const WNDPROC adapter = &MultibyteInputWndProc;
			if (s_window && s_predecessorWndProc)
			{
				WNDPROC current = nullptr;
				if (!ReadCurrentWindowProc(s_window, current))
				{
					if (!s_loggedWindowProcReadFailure)
					{
						s_loggedWindowProcReadFailure = true;
						gLog.FormattedMessage(
							"tnvse_multibyte_input: WndProc maintenance read failed hwnd=0x%08X site=GWLP_WNDPROC adapter=0x%08X predecessor=0x%08X; fail-closed",
							reinterpret_cast<UInt32>(s_window),
							reinterpret_cast<UInt32>(adapter),
							reinterpret_cast<UInt32>(s_predecessorWndProc));
					}
					return false;
				}
				s_loggedWindowProcReadFailure = false;

				if (current == adapter)
				{
					s_lastUnknownTopWindowProc = nullptr;
					State().tsfInputWindow.store(
						reinterpret_cast<ULONG_PTR>(s_window),
						std::memory_order_release);
					return true;
				}

				// Pointer equality with the saved predecessor is not proof that the
				// old chain was merely restored: the same subclass function may have
				// been installed again and may now call tNVSE as its predecessor.
				// GWLP_WNDPROC has no compare/exchange operation, so any automatic
				// re-publication can either form a recursion loop or overwrite a
				// concurrent successor. Preserve the current owner and fail closed.
				if (current != s_lastUnknownTopWindowProc)
				{
					s_lastUnknownTopWindowProc = current;
					gLog.FormattedMessage(
						"tnvse_multibyte_input: WndProc adapter no longer owns top-level dispatch hwnd=0x%08X site=GWLP_WNDPROC current=0x%08X adapter=0x%08X savedPredecessor=0x%08X sameAsSaved=%u; not republished",
						reinterpret_cast<UInt32>(s_window),
						reinterpret_cast<UInt32>(current),
						reinterpret_cast<UInt32>(adapter),
						reinterpret_cast<UInt32>(s_predecessorWndProc),
						current == s_predecessorWndProc ? 1u : 0u);
				}
				return false;
			}

			HWND hwnd = FindGameWindow();
			if (!hwnd)
				return false;

			LONG_PTR previousWndProc = 0;
			if (!SafeSetWindowLongPtrA(hwnd, GWLP_WNDPROC,
					reinterpret_cast<LONG_PTR>(adapter),
					&previousWndProc)
				|| !previousWndProc)
			{
				return false;
			}

			s_gameWindowProcSite.windowHandle = hwnd;
			s_gameWindowProcSite.predecessorProc =
				reinterpret_cast<WNDPROC>(previousWndProc);

			s_window = hwnd;
			s_predecessorWndProc = s_gameWindowProcSite.predecessorProc;
			s_lastUnknownTopWindowProc = nullptr;
			s_loggedWindowProcReadFailure = false;
			State().gameImeEnabled = false;
			State().associatedGameImeWindow = nullptr;
			State().associatedGameImeLayout = nullptr;
			State().associatedGameImeGeneration = 0;
			if (State().detachedGameImeWindow != hwnd)
			{
				State().gameImeContextDetached = false;
				State().gameImeDetachFailureLogged = false;
				State().detachedGameImeWindow = nullptr;
			}
			State().tsfInputWindow.store(
				reinterpret_cast<ULONG_PTR>(hwnd),
				std::memory_order_release);
			SetGameImeEnabled(hwnd, false);
			if (publishedNow)
				*publishedNow = true;
			gLog.FormattedMessage(
				"tnvse_multibyte_input: WndProc adapter installed hwnd=0x%08X site=GWLP_WNDPROC adapter=0x%08X predecessor=0x%08X abi=CALLBACK/__stdcall",
				reinterpret_cast<UInt32>(hwnd),
				reinterpret_cast<UInt32>(adapter),
				reinterpret_cast<UInt32>(s_predecessorWndProc));
			return true;
		}

		void ClearInputState()
		{
			ClearCapturedInputEvents();
			SetJipKeyEventSuppressionCaptureActive(false);
			State().textInputSessionActive = false;
			ResetTextInputBroker();
			State().overlayRefreshPending = false;
			s_imeComposing = false;
			State().tsfSessionGeneration = 1;
			State().tsfUiElementSessions.clear();
			ClearImePreviewState();
			HideCandidateOverlay();
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
			ResetStewieInputState();
		}

		void RestoreWindowProc()
		{
			RestoreTextEditInputHook();
			State().tsfInputWindow.store(
				0, std::memory_order_release);

			bool detached = true;
			if (s_window && s_predecessorWndProc)
			{
				const WNDPROC current = reinterpret_cast<WNDPROC>(
					GetWindowLongPtrA(s_window, GWLP_WNDPROC));
				WNDPROC observed = current;
				detached = s_gameWindowProcSite.RollbackOwned(&observed);
				if (!detached && current != s_predecessorWndProc)
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
				s_predecessorWndProc = nullptr;
				s_lastUnknownTopWindowProc = nullptr;
				s_loggedWindowProcReadFailure = false;
				s_gameWindowProcSite.windowHandle = nullptr;
				s_gameWindowProcSite.predecessorProc = nullptr;
				ShutdownTsfCandidateSupport();
			}
		}
	}
}

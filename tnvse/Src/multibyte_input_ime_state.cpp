#include "multibyte_input_internal.h"

#include "load_config.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <Windows.h>
#include <Imm.h>

namespace fonthook
{
	namespace
	{
		bool s_hidingSystemImeWindows = false;
		DWORD s_nativeImeAsciiGuardUntilTick = 0;

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

		bool IsNativeImeAsciiGuardActive()
		{
			return s_window
				&& s_textInputSessionActive
				&& IsConfiguredImeLayout(s_window)
				&& static_cast<SInt32>(s_nativeImeAsciiGuardUntilTick - GetTickCount()) > 0;
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
				s_imeCandidateState.selection = list->dwSelection;
				s_imeCandidateState.pageStart = list->dwPageStart;
				s_imeCandidateState.pageSize = list->dwPageSize;

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
						s_imeCandidateState.candidates.emplace_back(candidate);
				}
			}

			ImmReleaseContext(hwnd, context);
		}

		std::wstring GetCurrentImeName(HWND hwnd)
		{
			if (g_bMultibyteInputUseTSFCandidates && s_tsfCandidateSink)
			{
				std::wstring tsfName = TsfGetCurrentInputMethodName();
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

		if (s_textInputSessionActive && HasOverlayInputTarget())
		{
			if (!s_imeCandidateState.composition.empty() || !s_imeCandidateState.candidates.empty())
				return true;

			if (s_imeCandidateState.imeOpen
				&& (s_imeCandidateState.conversionMode & IME_CMODE_NATIVE))
				return true;
		}

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

	void RefreshImeStatus(HWND hwnd, HKL expectedLayout)
	{
		s_imeCandidateState.imeName = GetCurrentImeName(hwnd);
		s_imeCandidateState.imeOpen = false;
		s_imeCandidateState.conversionMode = 0;
		s_imeCandidateState.sentenceMode = 0;

		if (!IsConfiguredImeLayout(hwnd, expectedLayout))
			return;

		HIMC context = hwnd ? ImmGetContext(hwnd) : nullptr;
		if (!context)
			return;

		s_imeCandidateState.imeOpen = ImmGetOpenStatus(context) != FALSE;
		ImmGetConversionStatus(
			context,
			&s_imeCandidateState.conversionMode,
			&s_imeCandidateState.sentenceMode);
		ImmReleaseContext(hwnd, context);
	}

	void RefreshImeComposition(HWND hwnd)
	{
		s_imeCandidateState.composition = GetImeCompositionString(hwnd, GCS_COMPSTR);
	}

	void ClearImeCandidates()
	{
		s_imeCandidateState.candidates.clear();
		s_imeCandidateState.selection = 0;
		s_imeCandidateState.pageStart = 0;
		s_imeCandidateState.pageSize = 0;
		s_imeCandidateState.candidatesFromTsf = false;
		s_tsfCandidateActive = false;
	}

	void RefreshImeCandidates(HWND hwnd)
	{
		if (!IsConfiguredImeLayout(hwnd))
		{
			ClearImeCandidates();
			return;
		}

		if (g_bMultibyteInputUseTSFCandidates
			&& s_tsfCandidateActive
			&& s_imeCandidateState.candidatesFromTsf
			&& !s_imeCandidateState.candidates.empty())
			return;

		RefreshImeCandidatesFromImm(hwnd);
	}

	void ClearImePreviewState()
	{
		s_imeCandidateState.composing = false;
		s_imeCandidateState.composition.clear();
		s_compositionEchoChecked = false;
		ClearImeCandidates();
	}

	void HideSystemImeWindows(HWND hwnd)
	{
		if (!g_bMultibyteInputHideSystemCandidateWindow || !hwnd)
			return;

		if (s_hidingSystemImeWindows)
			return;

		HIMC context = ImmGetContext(hwnd);
		if (!context)
			return;

		s_hidingSystemImeWindows = true;

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
		s_hidingSystemImeWindows = false;
	}

	void EnsureConfiguredImeOpen(HWND hwnd, const char* reason, HKL expectedLayout)
	{
		if (!hwnd)
			return;

		if (!IsConfiguredImeLayout(hwnd, expectedLayout))
		{
			s_nativeImeAsciiGuardUntilTick = 0;
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
			s_nativeImeAsciiGuardUntilTick = GetTickCount() + kNativeImeAsciiGuardMs;

		if (changed || guardNativeAscii)
		{
			RefreshImeStatus(hwnd, expectedLayout);
			DebugLog(
				"tnvse_multibyte_input: prepared configured IME reason=%s changed=%u guard=%u open=%u native=%u",
				reason ? reason : "unknown",
				changed ? 1 : 0,
				guardNativeAscii ? 1 : 0,
				s_imeCandidateState.imeOpen ? 1 : 0,
				(s_imeCandidateState.conversionMode & IME_CMODE_NATIVE) ? 1 : 0);
		}
	}

	void RestoreDefaultGameImeContext(HWND hwnd, const char* reason, HKL expectedLayout)
	{
		if (!hwnd)
			return;

		ImmAssociateContextEx(hwnd, nullptr, IACE_DEFAULT);
		s_gameImeContextDetached = false;
		if (!IsConfiguredImeLayout(hwnd, expectedLayout))
		{
			s_nativeImeAsciiGuardUntilTick = 0;
			s_imeComposing = false;
			ClearImePreviewState();
		}
		EnsureConfiguredImeOpen(hwnd, reason, expectedLayout);
		RefreshImeStatus(hwnd, expectedLayout);
		DebugLog(
			"tnvse_multibyte_input: game IME default context enabled reason=%s open=%u native=%u",
			reason ? reason : "unknown",
			s_imeCandidateState.imeOpen ? 1 : 0,
			(s_imeCandidateState.conversionMode & IME_CMODE_NATIVE) ? 1 : 0);
	}

	void SetGameImeEnabled(HWND hwnd, bool enable)
	{
		if (!hwnd)
			return;

		if (enable)
		{
			if (!s_gameImeContextDetached)
				return;

			RestoreDefaultGameImeContext(hwnd, "enable");
			DebugLog("tnvse_multibyte_input: game IME context enabled");
			return;
		}

		if (s_gameImeContextDetached)
			return;

		CancelGameImeComposition(hwnd);
		s_imeComposing = false;
		ClearImePreviewState();
		HideCandidateOverlay();

		if (!s_gameImeContextDetached)
		{
			ImmAssociateContext(hwnd, nullptr);
			s_gameImeContextDetached = true;
		}

		DebugLog("tnvse_multibyte_input: game IME context disabled");
	}

	void SetTextInputSessionActive(bool active)
	{
		if (s_textInputSessionActive == active)
			return;

		s_textInputSessionActive = active;
		if (s_window)
		{
			if (active)
				RestoreDefaultGameImeContext(s_window, "session_start");
			else
				SetGameImeEnabled(s_window, false);
		}

		DebugLog(
			"tnvse_multibyte_input: text input session %s",
			active ? "started" : "ended");
	}

	void UpdateGameImeAssociation()
	{
		if (!s_window)
			return;

		SetTextInputSessionActive(GetCurrentTextEditMenuObject() != nullptr || GetOverlayStewieInputTarget().valid);
	}

	bool LayoutMatchesCurrentEncoding(HKL layout)
	{
		if (!layout || !g_uiEncoding)
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

	HKL GetGameKeyboardLayout(HWND hwnd)
	{
		DWORD threadId = hwnd ? GetWindowThreadProcessId(hwnd, nullptr) : GetCurrentThreadId();
		return GetKeyboardLayout(threadId);
	}

	bool IsConfiguredImeLayout(HWND hwnd, HKL expectedLayout)
	{
		HKL layout = expectedLayout ? expectedLayout : GetGameKeyboardLayout(hwnd);
		return LayoutMatchesCurrentEncoding(layout);
	}
}
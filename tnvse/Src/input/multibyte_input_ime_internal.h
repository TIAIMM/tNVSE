#pragma once

// Private IME implementation contract. Public input services remain declared in
// multibyte_input_internal.h.

#include "multibyte_input_internal.h"

namespace fonthook
{
	namespace multibyte_input
	{
		constexpr DWORD kDuplicateImeCharSuppressMs = 250;
		constexpr DWORD kNativeImeAsciiGuardMs = 250;
		constexpr DWORD kInputLanguageSwitchAsciiGuardMs = 500;
		constexpr DWORD kImeCommitKeyPendingLifetimeMs = 1500;
		constexpr UInt32 kMaxImeCandidatesToDisplay = 9;

		struct ImeCandidateState
		{
			bool composing = false;
			bool imeOpen = false;
			DWORD conversionMode = 0;
			DWORD sentenceMode = 0;
			DWORD selection = 0;
			DWORD pageStart = 0;
			DWORD pageSize = 0;
			bool candidatesFromTsf = false;
			std::wstring imeName;
			std::wstring composition;
			std::vector<std::wstring> candidates;
		};

		struct CandidateOverlayState
		{
			bool visible = false;
		};

		struct TsfUiElementSession
		{
			DWORD id = 0;
			UInt32 generation = 0;
		};

		class TsfCandidateSink;

		struct TsfCandidateSinkDeleter
		{
			void operator()(TsfCandidateSink* sink) const;
		};

		using TsfCandidateSinkPtr =
			std::unique_ptr<TsfCandidateSink, TsfCandidateSinkDeleter>;

		struct ImeCommitKeyState
		{
			// A key is only suppressible after a successful IME result confirms a
			// pending composition-time Space/digit/Enter observation. Keep it across
			// key-up so delayed game polling and WM_CHAR can both be drained.
			UINT virtualKey = 0;
			UInt32 input = 0;
			DWORD observedTick = 0;
			UInt8 expectedChannel = 0;
			UInt8 consumedChannels = 0;
			bool pending = false;
			bool confirmed = false;
			bool released = false;
		};

		struct ImeState
		{
			bool compositionEchoChecked = false;
			DWORD lastImeCommitTick = 0;
			UInt32 suppressedImeCharCount = 0;
			DWORD lastStewieImeCommitTick = 0;
			DWORD lastStewieImeEnterKeyTick = 0;
			DWORD inputLanguageSwitchGuardUntilTick = 0;
			HKL winSpaceLayoutBefore = nullptr;
			bool winSpaceChordArmed = false;
			bool winSpaceSwitchPending = false;
			bool winSpaceLanguageChangeObserved = false;
			ImeCommitKeyState commitKey;

			ImeCandidateState candidate;
			CandidateOverlayState overlay;
			bool tsfCandidateActive = false;
			bool hidingSystemImeWindows = false;
			UInt32 imeCompositionUiEpoch = 1;
			UInt32 hiddenSystemImeUiEpoch = 0;
			std::atomic<ULONG_PTR> tsfInputWindow = 0;
			bool gameImeEnabled = false;
			bool gameImeContextDetached = false;
			HWND detachedGameImeWindow = nullptr;
			HWND associatedGameImeWindow = nullptr;
			HKL associatedGameImeLayout = nullptr;
			UInt32 associatedGameImeGeneration = 0;
			std::atomic_bool textInputSessionActive = false;
			UInt32 textInputSessionGeneration = 1;
			TextInputTarget currentTextInputTarget;
			bool overlayRefreshPending = false;
			UInt32 nativeOverlayHostGeneration = 0;
			DWORD lastNativeOverlayHostCheckTick = 0;
			bool tsfCompositionFallbackActive = false;
			DWORD nativeImeAsciiGuardUntilTick = 0;
			DWORD lastImeWatchdogTick = 0;
			UInt32 gameImeAssociationFastPathHits = 0;
			UInt32 ignoredCandidatePositionNotifications = 0;
			UInt32 candidateContentRefreshes = 0;
			std::vector<char> immCandidateBuffer;

			UInt32 tsfSessionGeneration = 1;
			std::vector<TsfUiElementSession> tsfUiElementSessions;
			TsfCandidateSinkPtr tsfCandidateSink;
		};

		ImeState& State();

		void AdvanceTsfCandidateSession();
		std::wstring GetCurrentTsfInputMethodName();
		void ShutdownTsfCandidateSupport();

		void TryRemoveCompositionEcho();
		std::wstring GetImeCompositionString(HWND hwnd, DWORD index);
		void RefreshImeComposition(HWND hwnd);
		void RefreshImeCandidates(HWND hwnd);
		void ClearImePreviewState();
		void CancelGameImeComposition(HWND hwnd);
		bool IsImeWindowMessage(UINT msg);
		bool IsVirtualKeyDown(int vk);
		bool IsPendingWinSpaceRelease(UINT msg, WPARAM wParam);
		bool IsWindowsKeyMessage(UINT msg, WPARAM wParam);
		HKL GetGameKeyboardLayout(HWND hwnd);
		bool LayoutMatchesCurrentEncoding(HKL layout);
		bool IsFocusRestoreMessage(UINT msg, WPARAM wParam);
	}
}

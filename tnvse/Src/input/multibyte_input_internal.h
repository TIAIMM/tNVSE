#pragma once

// Shared implementation contract; not part of the public multibyte-input API.

#include "multibyte_input.h"

#include "encoding.h"
#include "InterfaceManager.hpp"
#include "load_config.h"
#include "SafeWrite.h"
#include "tnvse.h"
#include "Tile.hpp"
#include "ui_decode.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <Windows.h>
#include <Imm.h>
#include <msctf.h>
#include <OleAuto.h>

namespace fonthook
{
	namespace multibyte_input
	{
		constexpr DWORD kDuplicateAsciiSuppressMs = 100;
		constexpr UINT kMessage_FlushDeferredStewieAscii = WM_APP + 0x5E1;

		constexpr UInt32 kInputCode_Backspace = 0x80000000;
		constexpr UInt32 kInputCode_ArrowLeft = 0x80000001;
		constexpr UInt32 kInputCode_ArrowRight = 0x80000002;
		constexpr UInt32 kInputCode_ArrowUp = 0x80000003;
		constexpr UInt32 kInputCode_ArrowDown = 0x80000004;
		constexpr UInt32 kInputCode_Home = 0x80000005;
		constexpr UInt32 kInputCode_End = 0x80000006;
		constexpr UInt32 kInputCode_Delete = 0x80000007;
		constexpr UInt32 kInputCode_Enter = 0x80000008;
		constexpr UInt32 kInputCode_PageUp = 0x80000009;
		constexpr UInt32 kInputCode_PageDown = 0x8000000A;

		enum class ImeCommitInputChannel : UInt8
		{
			WndProcChar = 1,
			TextEdit = 2,
			JipTextInput = 4,
			Stewie = 8,
			McmExtender = 16,
			DialogueHistory = 32,
		};

		extern HWND s_window;
		extern WNDPROC s_predecessorWndProc;
		extern bool s_hooksInstalled;
		extern bool s_imeComposing;
		extern DWORD s_lastWndProcAsciiTick;
		extern UInt8 s_lastWndProcAsciiChar;

		enum class StewieInputKind : UInt8
		{
			None,
			StewMenuSearch,
			StewMenuStringSubsetting,
			MenuSearch,
		};

		struct StewieInputTarget
		{
			StewieInputKind kind = StewieInputKind::None;
			Menu* menu = nullptr;
			Tile* tile = nullptr;
			bool inputField = false;
			bool valid = false;
		};

		struct McmExtenderInputTarget
		{
			Menu* menu = nullptr;
			Tile* root = nullptr;
			Tile* mcm = nullptr;
			Tile* search = nullptr;
			bool valid = false;
		};

		struct DialogueHistoryInputTarget
		{
			Menu* menu = nullptr;
			Tile* root = nullptr;
			Tile* dialogueHistory = nullptr;
			Tile* search = nullptr;
			bool valid = false;
		};

		enum class TextInputTargetKind : UInt8
		{
			None,
			TextEdit,
			JipTextInput,
			Stewie,
			DialogueHistory,
			McmExtender,
		};

		struct TextInputTargetToken
		{
			TextInputTargetKind kind = TextInputTargetKind::None;
			const void* identity = nullptr;
			const void* secondaryIdentity = nullptr;
			UInt32 generation = 0;
			bool writable = false;
			bool active = false;
		};

		struct TextInputTarget
		{
			TextInputTargetToken token;
			TextEditMenu* textEdit = nullptr;
			StewieInputTarget stewie;
			DialogueHistoryInputTarget dialogueHistory;
			McmExtenderInputTarget mcmExtender;
		};

		enum class GameInputFilterClass : UInt8
		{
			None = 0,
			PrintableAscii = 1,
			CompositionControl = 2,
			AllDuringComposition = 4,
		};

		enum class GameInputFilterResult : UInt8
		{
			Pass,
			SuppressImeCommit,
			SuppressCompositionAscii,
			SuppressCompositionControl,
		};

		constexpr GameInputFilterClass operator|(
			GameInputFilterClass lhs,
			GameInputFilterClass rhs)
		{
			return static_cast<GameInputFilterClass>(
				static_cast<UInt8>(lhs) | static_cast<UInt8>(rhs));
		}

		struct CandidateOverlayLine
		{
			std::wstring text;
			bool highlighted = false;
		};

		UInt32 TileID(Tile* tile);
		float TileTraitFloat(Tile* tile, UInt32 trait);
		Tile* MenuRoot(Menu* menu);
		UInt32 MenuID(Menu* menu);
		Menu* GetOpenMenu(UInt32 menuID);
		Tile* FindTileByID(Tile* tile, UInt32 id);
		bool IsStewieTweaksAvailable();
		StewieInputTarget MakeStewieTarget(StewieInputKind kind, Menu* menu, Tile* tile, bool inputField);
		bool CallStewiePredecessorInput(Menu* menu, UInt32 input);
		bool HandleStewieInput(Menu* menu, UInt32 input);

		StewieInputTarget FindStewieMenuSearchTarget(Menu* menu);
		StewieInputTarget GetActiveStewieMenuSearchTarget();
		SIZE_T GetStewieMenuSearchPredecessorInputHandler(Menu* menu);
		void TryInstallStewieMenuSearchAdapterSites();
		void ResetStewieMenuSearchState();

		void DebugLog(const char* fmt, ...);
		void DebugLogState(const char* source, const char* action, TextEditMenu* menu, SInt32 input);
		char PrintableAscii(UInt32 value);
		bool AsciiEqualsIgnoreCase(UInt8 lhs, UInt8 rhs);
		bool AsciiEqualsIgnoreCase(UInt8 lhs, wchar_t rhs);
		UInt8 ResolveAsciiLetterCaseFromKeyboard(UInt8 input);

		std::string GetText(const TextEditState& state);
		size_t NextOffset(const std::string& text, size_t offset);
		bool IsCharBoundary(const std::string& text, size_t offset);
		size_t PrevCharBoundary(const std::string& text, size_t offset);
		size_t ClampToPrevBoundary(const std::string& text, size_t offset);
		size_t NextCharBoundary(const std::string& text, size_t offset);
		size_t NextUTF8Offset(const std::string& text, size_t offset);
		bool IsUTF8CharBoundary(const std::string& text, size_t offset);
		size_t PrevUTF8CharBoundary(const std::string& text, size_t offset);
		size_t ClampToPrevUTF8Boundary(const std::string& text, size_t offset);
		size_t NextUTF8CharBoundary(const std::string& text, size_t offset);
		bool IsCtrlKeyDown();

		bool InstallTextEditHooks();
		void RestoreTextEditInputHook();
		void TryInstallJipTextInputHook();
		void ClearJipTextInputHookState();
		TextEditMenu* GetCurrentTextEditMenuObject();
		TextEditMenu* GetActiveTextEditMenu();
		TextEditMenu* GetCurrentJipTextInputMenu();
		TextEditMenu* GetActiveJipTextInputMenu();
		TextEditMenu* GetAnyActiveTextInputMenu();
		TextEditMenu* GetOverlayTextInputMenu();
		void DebugLogJipState(const char* source, const char* action, TextEditMenu* menu, UInt32 input);
		bool InsertWideText(TextEditMenu* menu, std::wstring_view value);
		bool InsertWideTextJip(TextEditMenu* menu, std::wstring_view value);
		bool RemovePreviousAsciiCompositionEcho(TextEditState& state, wchar_t compositionLead);
		bool RemovePreviousJipAsciiCompositionEcho(TextEditMenu* menu, wchar_t compositionLead);

		StewieInputTarget GetActiveStewieInputTarget();
		StewieInputTarget GetOverlayStewieInputTarget();
		void TryInstallStewieTweaksInputHooks();
		void ProcessStewieTweaksInputTargetState();
		void ClearStewieInputState();
		void ResetStewieInputState();
		void ProcessStewieMenuSearchPendingStateSync();
		bool ObserveStewieMenuSearchHotkeyMessage(
			UINT msg,
			WPARAM wParam,
			LPARAM lParam,
			bool controlDown);
		bool InsertWideTextStewie(const StewieInputTarget& target, std::wstring_view text);
		bool HandleStewieWndProcAscii(const StewieInputTarget& target, UInt8 input);
		bool HandleStewieImeEnter(const StewieInputTarget& target);
		bool ShouldSuppressInputLanguageSwitchAscii(UInt8 input);
		void SuppressStewieInputLanguageSwitchSpace();
		bool FlushDeferredStewieAscii(UInt32 token);
		void CancelDeferredStewieAscii();
		bool RemovePreviousStewieAsciiCompositionEcho(wchar_t compositionLead);

		bool InitializeMcmExtenderInputBridge();
		McmExtenderInputTarget GetActiveMcmExtenderInputTarget();
		McmExtenderInputTarget GetOverlayMcmExtenderInputTarget();
		void ProcessMcmExtenderInputTargetState();
		void ResetMcmExtenderInputState();
		bool InsertWideTextMcmExtender(
			const McmExtenderInputTarget& target,
			std::wstring_view text);
		bool HandleMcmExtenderWndProcChar(
			const McmExtenderInputTarget& target,
			WPARAM input,
			bool controlDown);
		bool HandleMcmExtenderKeyDown(
			const McmExtenderInputTarget& target,
			WPARAM virtualKey,
			bool controlDown);
		bool HandleMcmExtenderMenuInput(Menu* menu, UInt32 input);
		bool ShouldSuppressMcmExtenderControlChar(WPARAM input);
		bool RemovePreviousMcmExtenderAsciiCompositionEcho(wchar_t compositionLead);

		bool InitializeDialogueHistoryInputBridge();
		DialogueHistoryInputTarget GetActiveDialogueHistoryInputTarget();
		DialogueHistoryInputTarget GetOverlayDialogueHistoryInputTarget();
		void ProcessDialogueHistoryInputTargetState();
		void ResetDialogueHistoryInputState();
		bool InsertWideTextDialogueHistory(
			const DialogueHistoryInputTarget& target,
			std::wstring_view text);
		bool HandleDialogueHistoryWndProcChar(
			const DialogueHistoryInputTarget& target,
			WPARAM input,
			bool controlDown);
		bool HandleDialogueHistoryKeyDown(
			const DialogueHistoryInputTarget& target,
			WPARAM virtualKey,
			bool controlDown);
		bool HandleDialogueHistoryMenuInput(Menu* menu, UInt32 input);
		bool ShouldSuppressDialogueHistoryControlChar(WPARAM input);
		bool RemovePreviousDialogueHistoryAsciiCompositionEcho(wchar_t compositionLead);

		TextInputTarget ResolveCurrentTextInputTarget();
		void SynchronizeTextInputTarget(const char* reason);
		void AdvanceTextInputSessionGeneration(const char* reason);
		void ResetTextInputBroker();
		TextInputTarget GetCachedTextInputTarget();
		TextInputTargetToken CaptureTextInputTargetToken();
		bool IsCurrentTextInputTargetToken(const TextInputTargetToken& token);
		bool HasCurrentTextInputTarget();
		const char* TextInputTargetKindName(TextInputTargetKind kind);
		ImeCommitInputChannel TextInputTargetCommitChannel(
			const TextInputTarget& target);
		bool InsertWideTextIntoTarget(
			const TextInputTarget& target,
			std::wstring_view text);
		GameInputFilterResult FilterGameInput(
			UInt32 input,
			ImeCommitInputChannel channel,
			GameInputFilterClass inputClass);

		bool HasOverlayInputTarget();
		bool IsCandidateOverlayRendererAvailable();
		bool InitializeTsfCandidateSupport();
		void PumpTsfInputUpdates();
		void UpdateCandidateOverlay();
		void PumpCandidateOverlay();
		void HideCandidateOverlay();
		void ClearImeCandidates();
		void TryInstallJipKeyEventSuppressionHook();
		bool IsJipKeyEventSuppressionHookInstalled();
		void SetJipKeyEventSuppressionCaptureActive(bool active);
		void HideSystemImeWindows(HWND hwnd);
		void SetTextInputSessionActive(bool active);
		void RefreshTextInputSessionForActiveTarget(const char* reason);
		void EndStewieTextInputSession(const char* reason);
		void SetGameImeEnabled(HWND hwnd, bool enable);
		void RestoreDefaultGameImeContext(HWND hwnd, const char* reason, HKL expectedLayout = nullptr);
		void EnsureConfiguredImeOpen(HWND hwnd, const char* reason, HKL expectedLayout = nullptr);
		void UpdateGameImeAssociation();
		void PumpImeStatusWatchdog();
		void RefreshImeStatus(HWND hwnd, HKL expectedLayout = nullptr);
		bool IsConfiguredImeLayout(HWND hwnd, HKL expectedLayout = nullptr);
		bool IsNativeImeAsciiGuardActive();
		bool IsImeCompositionActive();
		bool IsImeConsumingAscii();
		void ObserveImeCommitKeyMessage(UINT msg, WPARAM wParam, LPARAM lParam, bool hasInputTarget);
		void ObserveImeCommitInput(UInt32 input);
		void ConfirmImeCommitKey(ImeCommitInputChannel expectedChannel);
		bool ShouldSuppressImeCommitInput(UInt32 input, ImeCommitInputChannel channel);
		void ResetImeCommitKeyState(const char* reason);
		std::string WideToCurrentCodePage(std::wstring_view value);
		void PumpCapturedInputEvents();
		void ClearCapturedInputEvents();
		bool TryInstallWindowProc(bool* publishedNow = nullptr);
		void RestoreWindowProc();
	}
}

#pragma once

// Shared implementation contract; not part of the public multibyte-input API.

#include "multibyte_input.h"

#include "encoding.h"
#include "InterfaceManager.hpp"
#include "load_config.h"
#include "NiDX9Renderer.hpp"
#include "SafeWrite.h"
#include "tnvse.h"
#include "Tile.hpp"
#include "ui_decode.h"

#include <algorithm>
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
#include <d3d9.h>
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

		extern HWND s_window;
		extern WNDPROC s_originalWndProc;
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
		bool CallStewieOriginalInput(Menu* menu, UInt32 input);
		bool HandleStewieInput(Menu* menu, UInt32 input);

		StewieInputTarget FindStewieMenuSearchTarget(Menu* menu);
		StewieInputTarget GetActiveStewieMenuSearchTarget();
		SIZE_T GetStewieMenuSearchOriginalInputHandler(Menu* menu);
		void TryInstallStewieMenuSearchHooks();
		void ResetStewieMenuSearchState();

		void DebugLog(const char* fmt, ...);
		void DebugLogState(const char* source, const char* action, TextEditMenu* menu, SInt32 input);
		char PrintableAscii(UInt32 value);
		bool AsciiEqualsIgnoreCase(UInt8 lhs, wchar_t rhs);

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

		void InstallTextEditHooks();
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
		void TryInstallTileReadXMLHook();
		void ClearStewieInputState();
		void ResetStewieInputState();
		void ProcessStewieMenuSearchPendingStateSync();
		bool ObserveStewieMenuSearchHotkeyMessage(UINT msg, WPARAM wParam, LPARAM lParam);
		bool InsertWideTextStewie(const StewieInputTarget& target, std::wstring_view text);
		bool HandleStewieWndProcAscii(const StewieInputTarget& target, UInt8 input);
		bool HandleStewieImeEnter(const StewieInputTarget& target);
		bool ShouldSuppressInputLanguageSwitchAscii(UInt8 input);
		void SuppressStewieInputLanguageSwitchSpace();
		bool FlushDeferredStewieAscii(UInt32 token);
		void CancelDeferredStewieAscii();
		bool RemovePreviousStewieAsciiCompositionEcho(wchar_t compositionLead);

		bool HasOverlayInputTarget();
		bool InitializeCandidateOverlayRenderer();
		bool IsCandidateOverlayRendererAvailable();
		bool RasterizeCandidateOverlay(
			const std::vector<CandidateOverlayLine>& lines,
			std::vector<UInt32>& pixels,
			UInt32& width,
			UInt32& height);
		void ShutdownCandidateOverlayRenderer();
		bool InitializeTsfCandidateSupport();
		void UpdateCandidateOverlay();
		void DrawCandidateOverlay();
		void ReleaseCandidateOverlayTexture();
		void HideCandidateOverlay();
		void ClearImeCandidates();
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
		std::string WideToCurrentCodePage(std::wstring_view value);
		bool TryInstallWindowProc();
		void RestoreWindowProc();
	}
}

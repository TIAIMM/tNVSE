#pragma once

// Internal shared declarations for the multibyte input module.
//
// The original single translation unit (multibyte_input.cpp) placed every
// constant, struct, global and helper inside an anonymous namespace. Splitting
// the logic across several .cpp files requires the cross-module symbols to be
// promoted to the (named) fonthook namespace so they have external linkage and
// can be declared here. Symbols only referenced within a single .cpp remain in
// that file's anonymous namespace.

#include "nvse/PluginAPI.h"

#include "ui_decode.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <Windows.h>

namespace fonthook
{
	// ---- Shared constants (compile-time; each TU gets its own copy) ----
	constexpr SIZE_T kPlayerNameTextEditOpenCall = 0x7AB740;
	constexpr SIZE_T kPlayerNameIsValidName = 0x7AB820;
	constexpr SIZE_T kTextEditMenuVTable = 0x1070034;
	constexpr SIZE_T kTextEditMenuHandleKeyboardInput = 0x7E6620;
	constexpr SIZE_T kTextEditMenuInputVTableEntry = 0x1070064;
	constexpr SIZE_T kTextEditStateInputCallInHandleKeyboardInput = 0x7E6685;
	constexpr UInt32 kMaxTextEditRawBytes = 1023;
	constexpr UInt32 kJipNumericOnlyFlag = 1;
	constexpr UInt32 kJipEnterAcceptsOkFlag = 2;
	constexpr DWORD kDuplicateImeCharSuppressMs = 250;
	constexpr DWORD kDuplicateAsciiSuppressMs = 100;
	constexpr DWORD kNativeImeAsciiGuardMs = 1000;
	constexpr UInt32 kMaxImeCandidatesToDisplay = 9;
	constexpr UInt32 kMessage_OnFramePresent = NVSEMessagingInterface::kMessage_PostQueryPlugins + 1;
	constexpr UInt32 kOverlayPadding = 10;
	constexpr UInt32 kOverlayLineHeight = 24;
	constexpr UInt32 kOverlayMinWidth = 260;
	constexpr UInt32 kOverlayMaxWidth = 620;
	constexpr UInt32 kStewieTweaksMinVersion = 990;
	constexpr const char* kStewieTweaksPluginName = "lStewieAl's Tweaks";
	constexpr UInt32 kMenuType_StewMenu = 1069;
	constexpr UInt32 kStewMenu_SearchBar = 5;
	constexpr UInt32 kStewMenu_SubsettingInputFieldText = 103;
	constexpr UInt32 kStewieMenuSearch_TextTile = 87698483;
	constexpr UInt32 kStewieMaxShadowBytes = 1023;

	constexpr SIZE_T kInventoryMenuHandleKeyboardInputEntry = 0x10739E4;
	constexpr SIZE_T kStatsMenuHandleKeyboardInputEntry = 0x1070004;
	constexpr SIZE_T kMapMenuHandleKeyboardInputEntry = 0x1074D74;
	constexpr SIZE_T kContainerMenuHandleKeyboardInputEntry = 0x10721DC;
	constexpr SIZE_T kBarterMenuHandleKeyboardInputEntry = 0x107071C;
	constexpr SIZE_T kLevelUpMenuHandleKeyboardInputEntry = 0x1073D0C;
	constexpr SIZE_T kRecipeMenuHandleKeyboardInputEntry = 0x10704BC;
	constexpr SIZE_T kStartMenuHandleKeyboardInputEntry = 0x1076D4C;
	constexpr UInt32 kMenuHandleKeyboardInputVTableOffset = 0x30;

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

	// ---- Shared structs ----
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

	struct StewieShadowState
	{
		StewieInputTarget target;
		std::string text;
		size_t caret = 0;
		size_t appliedBytes = 0;
		bool initialized = false;
	};

	struct StewieMenuHook
	{
		const char* name = "";
		UInt32 menuID = 0;
		SIZE_T entry = 0;
		SIZE_T original = 0;
		SIZE_T hook = 0;
		bool installed = false;
	};

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

	struct CandidateOverlayLine
	{
		std::wstring text;
		bool highlighted = false;
	};

	// ---- Adapter classes patched into the game ----
	class TextEditMenuEx : public TextEditMenu
	{
	public:
		static bool __cdecl Open(const char* apTitle, const char* apInitialText, ValidateTextCallback apValidateText);
	};

	class TextEditStateEx : public TextEditState
	{
	public:
		static void __fastcall Input(TextEditState* apState, void*, SInt32 aiInput, SInt32 aiChar);
	};

	// TSF candidate sink is fully owned by ime_tsf.cpp; only a forward
	// declaration is needed here so wrappers can mention the type.
	class TsfCandidateSink;

	// ---- Shared global state (defined in multibyte_input.cpp) ----
	extern HWND s_window;
	extern WNDPROC s_originalWndProc;
	extern bool s_hooksInstalled;
	extern bool s_imeComposing;
	extern bool s_compositionEchoChecked;
	extern DWORD s_lastImeCommitTick;
	extern DWORD s_lastWndProcAsciiTick;
	extern UInt8 s_lastWndProcAsciiChar;
	extern UInt32 s_suppressedImeCharCount;
	extern SIZE_T s_jipOriginalInputHandler;

	extern ImeCandidateState s_imeCandidateState;
	extern bool s_tsfCandidateActive;
	extern bool s_gameImeContextDetached;
	extern bool s_textInputSessionActive;
	extern bool s_stewieReplay;
	extern std::unique_ptr<TsfCandidateSink> s_tsfCandidateSink;

	// ---- Cross-module functions ----
	void DebugLog(const char* fmt, ...);
	char PrintableAscii(UInt32 value);
	bool AsciiEqualsIgnoreCase(UInt8 lhs, wchar_t rhs);
	void DebugLogState(const char* source, const char* action, TextEditMenu* menu, SInt32 input);

	// TextEditState helpers (multibyte_input_text.cpp)
	size_t NextOffset(const std::string& text, size_t offset);
	bool IsCharBoundary(const std::string& text, size_t offset);
	size_t PrevCharBoundary(const std::string& text, size_t offset);
	size_t ClampToPrevBoundary(const std::string& text, size_t offset);
	size_t NextCharBoundary(const std::string& text, size_t offset);
	bool InsertTextAtCaret(TextEditState& state, std::string_view text);
	bool InsertTextAtCaret(TextEditMenu* menu, std::string_view text);
	bool InsertWideText(TextEditMenu* menu, std::wstring_view value);
	std::string WideToCurrentCodePage(std::wstring_view value);
	bool ValidatePlayerName(const char* text);

	// Caret/edit helpers operating on a TextEditState directly (used by TextEditStateEx::Input)
	void SetCaret(TextEditState& state, size_t offset);
	bool FitsTextEditConstraints(TextEditState& state, const std::string& candidate);
	bool CommitCandidate(TextEditState& state, const std::string& candidate, size_t caret);
	bool CommitCandidate(TextEditMenu* menu, const std::string& candidate, size_t caret);
	bool DeletePreviousChar(TextEditState& state);
	bool DeleteNextChar(TextEditState& state);
	bool MoveCaretPrevious(TextEditState& state);
	bool MoveCaretNext(TextEditState& state);
	bool MoveCaretHome(TextEditState& state);
	bool MoveCaretEnd(TextEditState& state);
	bool RemovePreviousAsciiCompositionEcho(TextEditState& state, wchar_t compositionLead);

	// TextEditMenu probes (multibyte_input_text.cpp)
	TextEditMenu* GetCurrentTextEditMenuObject();
	TextEditMenu* GetActiveTextEditMenu();
	TextEditMenu* GetAnyActiveTextInputMenu();
	TextEditMenu* GetOverlayTextInputMenu();
	SIZE_T CurrentTextEditInputHandler();
	bool HasOverlayInputTarget();
	void TryRemoveCompositionEcho();

	// JIP text input adapter (input_jip.cpp)
	SIZE_T JipTextInputHandlerAddress();
	TextEditMenu* GetCurrentJipTextInputMenu();
	TextEditMenu* GetActiveJipTextInputMenu();
	void ClearJipTextInputHookState();
	void TryInstallJipTextInputHook();
	void DebugLogJipState(const char* source, const char* action, TextEditMenu* menu, UInt32 input);
	bool InsertWideTextJip(TextEditMenu* menu, std::wstring_view value);
	bool RemovePreviousJipAsciiCompositionEcho(TextEditMenu* menu, wchar_t compositionLead);

	// Stewie Tweaks adapter (input_stewie.cpp)
	StewieInputTarget GetActiveStewieInputTarget();
	StewieInputTarget GetOverlayStewieInputTarget();
	void TryInstallStewieTweaksInputHooks();
	void ClearStewieInputState();
	bool InsertWideTextStewie(const StewieInputTarget& target, std::wstring_view text);
	bool InsertTextAtCaretStewie(const StewieInputTarget& target, std::string_view text);
	bool RemovePreviousStewieAsciiCompositionEcho(wchar_t compositionLead);

	// IME / IMM32 state & layout (ime_state.cpp)
	std::wstring GetImeCompositionString(HWND hwnd, DWORD index);
	bool IsImeCompositionActive();
	bool IsImeConsumingAscii();
	void RefreshImeStatus(HWND hwnd, HKL expectedLayout = nullptr);
	void RefreshImeComposition(HWND hwnd);
	void ClearImeCandidates();
	void RefreshImeCandidates(HWND hwnd);
	void ClearImePreviewState();
	void HideSystemImeWindows(HWND hwnd);
	void RestoreDefaultGameImeContext(HWND hwnd, const char* reason, HKL expectedLayout = nullptr);
	void SetGameImeEnabled(HWND hwnd, bool enable);
	void SetTextInputSessionActive(bool active);
	void UpdateGameImeAssociation();
	bool LayoutMatchesCurrentEncoding(HKL layout);
	HKL GetGameKeyboardLayout(HWND hwnd);
	bool IsConfiguredImeLayout(HWND hwnd, HKL expectedLayout = nullptr);

	// TSF candidate sink wrappers (ime_tsf.cpp)
	bool InitializeTsfCandidateSink();
	void ShutdownTsfCandidateSink();
	std::wstring TsfGetCurrentInputMethodName();

	// Candidate overlay (candidate_overlay.cpp)
	void UpdateCandidateOverlay();
	void DrawCandidateOverlay();
	void ReleaseCandidateOverlayTexture();
	void HideCandidateOverlay();

	// Window subclassing (window_proc.cpp)
	bool TryInstallWindowProc();
}
#pragma once

#include "native_tile_overlay.h"
#include "prewarm_overlay_mailbox.h"

#include "BSMemory.hpp"
#include "BSRenderedTexture.hpp"
#include "font_glyphs.h"
#include "font_manager.h"
#include "font_vector.h"
#include "hook_identity.h"
#include "InterfaceManager.hpp"
#include "Menu.hpp"
#include "NiRenderer.hpp"
#include "SafeWrite.h"
#include "Tile.hpp"
#include "TileMenu.hpp"
#include "load_config.h"
#include "tnvse.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <string_view>
#include <vector>


namespace fonthook::implementation::native_tile_overlay
{
		constexpr const char* kImeOverlayXmlPath =
			"Data\\Menus\\prefabs\\tNVSE\\ImeOverlay.xml";
		constexpr const char* kPrewarmOverlayXmlPath =
			"Data\\Menus\\prefabs\\tNVSE\\FontPrewarmOverlay.xml";
		constexpr size_t kImeLineCount = 11;
		constexpr size_t kImeHighlightCount = 9;
		constexpr float kImeMinimumLineHeight = 24.0f;
		constexpr float kImeLineGap = 2.0f;
		constexpr float kImeVerticalPadding = 10.0f;
		constexpr float kImeMinimumWidth = 320.0f;
		constexpr float kImeMaximumWidth = 620.0f;
		constexpr float kImeHorizontalPadding = 20.0f;
		constexpr UInt32 kImeVisualBoundsLogLimit = 64;
		constexpr UInt32 kTileReadFile = 0xA01B00;
		constexpr UInt32 kTileRelease = 0x9FF690;
		constexpr UInt32 kMenuSetMenuTile = 0xA1DC70;
		// Retail TileMenu::PostParse uses this variadic registry setter after
		// Menu::SetMenuTile. Passing (menuCode, TileMenu*) publishes the root in
		// the same table read by InterfaceManager::GetMenuByType.
		constexpr UInt32 kMenuRegisterTile = 0xA1E130;
		constexpr SIZE_T kLoadingMenu_pMe = 0x11DA0C0;
		constexpr SIZE_T kLoadingMenuThread_pMe = 0x11DA0C4;
		constexpr SIZE_T kInterfaceManager_pMe = 0x11DEA10;
		constexpr size_t kLoadingMenuThreadPauseRequestedOffset = 72;
		constexpr size_t kLoadingMenuThreadShutdownOffset = 74;
		// TileMenu::PostParse and the vanilla visibility table only accept
		// Menu Codes 1001-1084. 1079 is the highest unused gap immediately
		// below SlotMachineMenu (1080), and keeps the overlay inside the
		// engine's native menu ownership without colliding with vanilla menus.
		constexpr UInt32 kImeMenuClass = 1079;
		static_assert(kImeMenuClass >= 1001 && kImeMenuClass <= 1084);
		constexpr SIZE_T kMenuConstructor = 0xA1C4A0;
		constexpr SIZE_T kMenuVTable = 0x1095484;
		constexpr size_t kMenuVTableEntryCount = 18;
		constexpr size_t kMenuGetIdVtableIndex = 13;
		constexpr SInt32 kImeMenuDepthContribution = -1000000;
		// FOPipboyManager vtable slot 3 is FORenderedMenu::Draw
		// (FalloutNV.exe 0x7FBA00; symbolized test build 0x82674410). It
		// captures the UI into the Pip-Boy's
		// 1280x960 render target before the ordinary screen-space UI pass.
		constexpr SIZE_T kFOPipboyManagerDrawVTableEntry = 0x10780B8;
		constexpr SIZE_T kFORenderedMenuDraw = 0x7FBA00;
		// LoadingMenuThread::ThreadProc calls LoadingMenu::Update immediately before
		// the thread presents Tile changes. Consuming queued progress at this
		// exact call boundary keeps all LoadingMenu Tile mutations on its owner.
		constexpr SIZE_T kLoadingMenuThreadUpdateCallSite = 0x78D552;
		constexpr SIZE_T kLoadingMenuUpdate = 0x789820;
		constexpr std::array<UInt8, 6>
			kExpectedLoadingMenuInstanceLoadInstruction = {
			0x8B, 0x0D, 0xC0, 0xA0, 0x1D, 0x01, // mov ecx, [011DA0C0h]
			                                         // LoadingMenu::pMe
		};

		using RenderedMenuDrawFn =
			void (__thiscall*)(void*, BSRenderedTexture*,
				NiRenderer::ClearFlags, BSRenderedTexture*);
		using LoadingMenuUpdateFn = void (__thiscall*)(void*);

		struct NativeTileOverlayState
		{
			Tile* imeParent = nullptr;

			Tile* imeRoot = nullptr;
			Tile* imeBackground = nullptr;
			std::array<Tile*, kImeLineCount> imeLines = {};
			std::array<Tile*, kImeHighlightCount> imeHighlights = {};
			std::wstring imeKey;
			size_t imeVisibleLineCount = 0;
			float imeLineHeight = 0.0f;
			float imeContentWidth = 0.0f;
			bool imeLoadFailed = false;
			bool imeVisible = false;
		};

		using PrewarmOverlayInstance = BasicPrewarmOverlayInstance<Tile*>;

		struct ImeTextVisualBounds
		{
			float top = 0.0f;
			float height = 0.0f;
			bool valid = false;
		};

	enum class LoadingMenuDiagnosticPhase : UInt32
	{
		Idle,
		LoadingTextMakeNode,
	};

	enum class LoadingTransitionKind : UInt32
	{
		None = 0,
		SaveLoad,
		FastTravel,
		NonSaveLoading,
	};

	enum class LoadingTransitionTerminalReason : UInt32
	{
		None = 0,
		SavePostLoad,
		LoadingMenuClosed,
		Superseded,
		Shutdown,
	};

	enum class LoadingMenuUpdateDiagnosticPhase : UInt32
	{
		Idle = 0,
		Predecessor,
		PrewarmCommand,
	};

	struct LoadingTransitionDiagnosticState
	{
		std::atomic_bool runtimeArmed{ false };
		std::atomic_bool inactiveBaselineObserved{ false };
		std::atomic_bool captureEnabled{ false };
		std::atomic<UInt64> nextTraceId{ 0 };
		std::atomic<UInt64> activeTraceId{ 0 };
		std::atomic<UInt64> eventSequence{ 0 };
		std::atomic<LoadingTransitionKind> kind{
			LoadingTransitionKind::None
		};
		std::atomic<ULONGLONG> startedAt{ 0 };
		std::atomic<ULONGLONG> terminalRequestedAt{ 0 };
		std::atomic<ULONGLONG> observationStartedAt{ 0 };
		std::atomic<LoadingTransitionTerminalReason> terminalReason{
			LoadingTransitionTerminalReason::None
		};
		std::atomic<UInt32> terminalSucceeded{ 0 };
		std::atomic<ULONGLONG> nextSnapshotAt{ 0 };
		std::atomic<ULONGLONG> lastLoadingThreadStaleLogAt{ 0 };

		std::atomic<UInt64> mainLoopSequence{ 0 };
		std::atomic<ULONGLONG> lastMainLoopAt{ 0 };
		std::atomic<UInt32> mainThreadId{ 0 };
		std::atomic<NativeLoadingMainThreadStage> mainThreadStage{
			NativeLoadingMainThreadStage::Idle
		};

		std::atomic<UInt64> loadingUpdateEnterSequence{ 0 };
		std::atomic<UInt64> loadingUpdateExitSequence{ 0 };
		std::atomic<ULONGLONG> lastLoadingUpdateEnterAt{ 0 };
		std::atomic<ULONGLONG> lastLoadingUpdateExitAt{ 0 };
		std::atomic<UInt32> lastLoadingUpdateDurationUs{ 0 };
		std::atomic<UInt32> loadingUpdateInFlight{ 0 };
		std::atomic<UInt32> loadingThreadId{ 0 };
		std::atomic<LoadingMenuUpdateDiagnosticPhase> loadingUpdatePhase{
			LoadingMenuUpdateDiagnosticPhase::Idle
		};

		std::atomic<UInt32> loadingMenuActive{ 0 };
		std::atomic<UInt32> loadingMenuObserved{ 0 };
		std::atomic<UInt32> saveReadObserved{ 0 };
		std::atomic<UInt32> savePostLoadObserved{ 0 };
		std::atomic<UInt32> savePathHash{ 0 };
		std::atomic<SIZE_T> fastTravelRef{ 0 };
		std::atomic<SIZE_T> destinationRef{ 0 };
		std::atomic<UInt32> fastTravelFormId{ 0 };
		std::atomic<UInt32> destinationFormId{ 0 };
		std::atomic<UInt32> movingIntoNewSpace{ 0 };
	};

	struct LoadingMenuDiagnosticState
	{
		std::atomic<UInt64> traceId{ 0 };
		std::atomic<SIZE_T> observedLoadingMenu{ 0 };
		std::atomic<SIZE_T> observedLoadingMenuRoot{ 0 };
		std::atomic<ULONGLONG> traceStartedAt{ 0 };
		std::atomic<ULONGLONG> lastActivityAt{ 0 };
		std::atomic<ULONGLONG> lastLoadingTextMakeNodeEnterAt{ 0 };
		std::atomic<ULONGLONG> lastLoadingTextMakeNodeExitAt{ 0 };
		std::atomic<ULONGLONG> phaseEnteredAt{ 0 };
		std::atomic<UInt64> loadingTextMakeNodeCalls{ 0 };
		std::atomic<UInt32> lastLoadingTextMakeNodeDurationUs{ 0 };
		std::atomic<UInt32> lastLoadingTextTileNameHash{ 0 };
		std::atomic<UInt32> lastLoadingTextFontTraitBits{ 0 };
		std::atomic<SIZE_T> lastLoadingTextTile{ 0 };
		std::atomic<UInt32> lastLoadingTextProducedNode{ 0 };
		std::atomic<UInt32> loadingTextMakeNodeInFlight{ 0 };
		std::atomic<LoadingMenuDiagnosticPhase> phase{
			LoadingMenuDiagnosticPhase::Idle
		};
	};

	struct NativeTileOverlayRuntimeState
	{
		NativeTileOverlayState state;
		LoadingMenuDiagnosticState loadingMenuDiagnostics;
		LoadingTransitionDiagnosticState loadingTransitionDiagnostics;
		std::atomic_bool imeReady{ false };
		std::atomic<UInt32> imeHostGeneration{ 1 };
		PrewarmOverlayMailbox prewarmMailbox;
		PrewarmOverlayInstance prewarmInstance;
		std::array<SIZE_T, kMenuVTableEntryCount + 1> imeMenuVtable = {};
		RenderedMenuDrawFn predecessorPipboyDraw = nullptr;
		LoadingMenuUpdateFn predecessorLoadingMenuUpdate = nullptr;
		bool imeMenuVtableInitialized = false;
		bool imeMenuVtableInitializationFailed = false;
		bool pipboyDrawHookInstalled = false;
		bool pipboyDrawHookInstallFailed = false;
		bool loadingMenuUpdateHookInstalled = false;
		bool loadingMenuUpdateHookInstallFailed = false;
		bool loggedPipboyRttExclusion = false;
		UInt32 imeVisualBoundsLogCount = 0;
	};

	NativeTileOverlayRuntimeState& OverlayRuntime();
	void AdvanceImeHostGeneration();
	void ConsumeNativePrewarmOverlayCommand(Menu* loadingMenu);
	bool EnsureLocalImeMenuSupport();
	Menu* CreateLocalImeMenu();

	bool IsDirectChild(const Tile* parent, const Tile* child);
	bool IsNamedDirectChild(const Tile* parent, const Tile* child,
		const char* name);
	std::string WideToUiText(std::wstring_view value);
	Tile* FindDirectMenuByClass(Tile* parent, UInt32 menuClass);
	Tile* FindDirectChild(Tile* parent, const char* name);
	ImeTextVisualBounds MeasureImeTextVisualBounds(
		std::string_view encoded, float measuredTextHeight);
	void SetVisible(Tile* tile, bool visible);
	void SetText(Tile* tile, std::wstring_view value);
	void PublishTextGeometry(Tile* tile, std::wstring_view value,
		bool forceRefresh);
	void RebuildTextGeometry(Tile* tile);
	bool TryGetReadOnlyMaximumMenuDepth(float& result);
	void SynchronizeOverlayDepth(Tile* root);
	bool HasExpectedImeLinePresentation(size_t visibleCount);
	void ReleaseAndDestroyAttachedRoot(Tile* parent, Tile* root,
		const char* expectedName);
	void ReleaseAndDestroyImeRoot(Tile* parent, Tile* root);

	void ClearImeResolvedTiles();
	void ResetImeForParent(Tile* parent);
	bool IsImeMenuRegistered(Tile* root);
	bool RegisterImeMenuRoot(Tile* root);
	bool ResolveImeTiles(Tile* root);
	bool IsResolvedImeTreeAttached(Tile* parent);
	void ResetImePresentationState();
	bool EnsureImeHost(Tile* parent);
	Tile* SynchronizeImeParent();
	std::wstring BuildImeKey(
		const std::vector<NativeTileOverlayLine>& lines);
}

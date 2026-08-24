#pragma once

#include "native_tile_overlay.h"
#include "owner_thread_shutdown_latch.h"

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
		constexpr float kPrewarmMinimumTextHeight = 24.0f;
		constexpr float kPrewarmMaximumPanelWidth = 620.0f;
		constexpr float kPrewarmMinimumPanelWidth = 320.0f;
		constexpr size_t kPrewarmProgressTextCapacity = 160;
		constexpr UInt32 kTileReadFile = 0xA01B00;
		constexpr UInt32 kTileRelease = 0x9FF690;
		constexpr UInt32 kMenuSetMenuTile = 0xA1DC70;
		constexpr SIZE_T kLoadingMenu_pMe = 0x11DA0C0;
		constexpr SIZE_T kLoadingMenuThread_pMe = 0x11DA0C4;
		constexpr SIZE_T kInterfaceManager_pMe = 0x11DEA10;
		constexpr SIZE_T kLoadingMenuStartupFlag = 0x11A0294;
		constexpr size_t kLoadingMenuThreadPauseRequestedOffset = 72;
		constexpr size_t kLoadingMenuThreadShutdownOffset = 74;
		// TileMenu::PostParse and the vanilla visibility table only accept
		// Menu Codes 1001-1084. 1079 is the highest unused gap immediately
		// below SlotMachineMenu (1080), and keeps the overlay inside the
		// engine's native menu ownership without colliding with vanilla menus.
		constexpr UInt32 kImeMenuClass = 1079;
		static_assert(kImeMenuClass >= 1001 && kImeMenuClass <= 1084);
		constexpr SIZE_T kCreateMenuByClassCallSite = 0x7079A3;
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
		constexpr SIZE_T kLoadingMenuThreadShowChangesCallSite = 0x78D557;
		constexpr SIZE_T kLoadingMenuShowChanges = 0x78D080;
		constexpr std::array<UInt8, 6>
			kExpectedLoadingMenuInstanceLoadInstruction = {
			0x8B, 0x0D, 0xC0, 0xA0, 0x1D, 0x01, // mov ecx, [011DA0C0h]
			                                         // LoadingMenu::pMe
		};

		using CreateMenuByClassFn =
			Menu* (__thiscall*)(void*, UInt32);
		using RenderedMenuDrawFn =
			void (__thiscall*)(void*, BSRenderedTexture*,
				NiRenderer::ClearFlags, BSRenderedTexture*);
		using LoadingMenuUpdateFn = void (__thiscall*)(void*);
		using LoadingMenuShowChangesFn = void (__cdecl*)();

		struct PrewarmOverlayCommand
		{
			std::array<wchar_t, kPrewarmProgressTextCapacity> detail = {};
			std::array<wchar_t, kPrewarmProgressTextCapacity> stage = {};
			float progress = 0.0f;
			UInt32 sequence = 0;
			UInt32 refreshSequence = 0;
			bool visible = false;
			bool ownerShutdown = false;
		};

		struct NativeTileOverlayState
		{
			Tile* imeParent = nullptr;
			Tile* prewarmParent = nullptr;

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

			Tile* prewarmRoot = nullptr;
			Tile* prewarmShade = nullptr;
			Tile* prewarmPanel = nullptr;
			Tile* prewarmTrack = nullptr;
			Tile* prewarmDetail = nullptr;
			Tile* prewarmStage = nullptr;
			Tile* prewarmFill = nullptr;
			Tile* prewarmPercent = nullptr;
			Tile* prewarmTitle = nullptr;
			std::array<float, 6> prewarmLayoutSignature = {};
			float prewarmProgressWidth = 520.0f;
			bool prewarmLoadFailed = false;
			bool prewarmParentUnavailableLogged = false;
			bool prewarmTileVisible = false;
		};

		struct ImeTextVisualBounds
		{
			float top = 0.0f;
			float height = 0.0f;
			bool valid = false;
		};

	enum class LoadingMenuDiagnosticPhase : UInt32
	{
		Idle,
		UpdateGuard,
		UpdatePredecessor,
		UpdateOverlayConsume,
		ShowChangesGuard,
		ShowChangesRendererLock,
		ShowChangesPredecessor,
		LoadingTextMakeNode,
	};

	enum class LoadingMenuUpdateDisposition : UInt32
	{
		Unknown,
		Running,
		SkippedPauseOrShutdown,
		SkippedStartupBarrier,
	};

	struct LoadingMenuDiagnosticState
	{
		std::atomic<UInt64> traceId{ 0 };
		std::atomic<SIZE_T> observedLoadingMenu{ 0 };
		std::atomic<SIZE_T> observedLoadingMenuRoot{ 0 };
		std::atomic<ULONGLONG> traceStartedAt{ 0 };
		std::atomic<ULONGLONG> lastActivityAt{ 0 };
		std::atomic<ULONGLONG> lastUpdateEnterAt{ 0 };
		std::atomic<ULONGLONG> lastUpdateExitAt{ 0 };
		std::atomic<ULONGLONG> lastShowChangesEnterAt{ 0 };
		std::atomic<ULONGLONG> lastShowChangesExitAt{ 0 };
		std::atomic<ULONGLONG> lastLoadingTextMakeNodeEnterAt{ 0 };
		std::atomic<ULONGLONG> lastLoadingTextMakeNodeExitAt{ 0 };
		std::atomic<ULONGLONG> phaseEnteredAt{ 0 };
		std::atomic<ULONGLONG> lastCommandPublishedAt{ 0 };
		std::atomic<ULONGLONG> lastCommandConsumeAttemptAt{ 0 };
		std::atomic<ULONGLONG> lastCommandConsumedAt{ 0 };
		std::atomic<ULONGLONG> lastHeartbeatLogAt{ 0 };
		std::atomic<ULONGLONG> lastStallLogAt{ 0 };
		std::atomic<ULONGLONG> lastSlowLogAt{ 0 };
		std::atomic<ULONGLONG> lastShowSkipLogAt{ 0 };
		std::atomic<UInt64> updateCalls{ 0 };
		std::atomic<UInt64> showChangesCalls{ 0 };
		std::atomic<UInt64> loadingTextMakeNodeCalls{ 0 };
		std::atomic<UInt64> updateSkippedPauseOrShutdown{ 0 };
		std::atomic<UInt64> updateSkippedStartupBarrier{ 0 };
		std::atomic<UInt64> showChangesSkippedPauseOrShutdown{ 0 };
		std::atomic<UInt64> showChangesSkippedRendererUnavailable{ 0 };
		std::atomic<UInt64> showChangesSkippedRendererLock{ 0 };
		std::atomic<UInt64> commandsPublished{ 0 };
		std::atomic<UInt64> commandConsumeAttempts{ 0 };
		std::atomic<UInt64> commandsConsumed{ 0 };
		std::atomic<UInt32> lastUpdateDurationUs{ 0 };
		std::atomic<UInt32> lastShowChangesDurationUs{ 0 };
		std::atomic<UInt32> lastLoadingTextMakeNodeDurationUs{ 0 };
		std::atomic<UInt32> lastLoadingTextTileNameHash{ 0 };
		std::atomic<UInt32> lastLoadingTextFontTraitBits{ 0 };
		std::atomic<SIZE_T> lastLoadingTextTile{ 0 };
		std::atomic<UInt32> lastLoadingTextProducedNode{ 0 };
		std::atomic<UInt32> lastCommandAttemptSequence{ 0 };
		std::atomic<UInt32> lastCommandConsumedSequence{ 0 };
		std::atomic<UInt32> updateInFlight{ 0 };
		std::atomic<UInt32> showChangesInFlight{ 0 };
		std::atomic<UInt32> loadingTextMakeNodeInFlight{ 0 };
		std::atomic<UInt32> lastShowSkipReason{ 0 };
		std::atomic<UInt64> lastShowSkipTrace{ 0 };
		std::atomic<UInt64> lastLoadingTextBeginLoggedTrace{ 0 };
		std::atomic<UInt64> lastLoadingTextEndLoggedTrace{ 0 };
		std::atomic_bool pendingLoadingTextFirstCompleteLog{ false };
		std::atomic_bool pendingLoadingTextSlowLog{ false };
		std::atomic<UInt32> pendingLoadingTextSlowDurationUs{ 0 };
		std::atomic<SIZE_T> pendingLoadingTextSlowTile{ 0 };
		std::atomic<UInt32> pendingLoadingTextSlowNameHash{ 0 };
		std::atomic<UInt32> pendingLoadingTextSlowFontBits{ 0 };
		std::atomic_bool loggedPrewarmConsumerDisabledConsumption{ false };
		std::atomic<LoadingMenuDiagnosticPhase> phase{
			LoadingMenuDiagnosticPhase::Idle
		};
		std::atomic<LoadingMenuUpdateDisposition> updateDisposition{
			LoadingMenuUpdateDisposition::Unknown
		};
	};

	struct NativeTileOverlayRuntimeState
	{
		NativeTileOverlayState state;
		LoadingMenuDiagnosticState loadingMenuDiagnostics;
		std::atomic_bool imeReady{ false };
		std::atomic<UInt32> imeHostGeneration{ 1 };
		std::atomic_bool prewarmReady{ false };
		std::atomic_bool prewarmActive{ false };
		SRWLOCK prewarmCommandLock = SRWLOCK_INIT;
		PrewarmOverlayCommand prewarmCommand;
		UInt32 nextPrewarmCommandSequence = 0;
		std::atomic<UInt32> prewarmPublishedSequence{ 0 };
		std::atomic<UInt32> prewarmConsumedSequence{ 0 };
		UInt32 prewarmAppliedRefreshSequence = 0;
		std::atomic<DWORD> prewarmConsumerThreadId{ 0 };
		std::atomic_bool prewarmConsumerDisabled{ false };
		OwnerThreadShutdownLatch prewarmOwnerShutdown;
		std::array<SIZE_T, kMenuVTableEntryCount + 1> imeMenuVtable = {};
		CreateMenuByClassFn predecessorCreateMenuByClass = nullptr;
		RenderedMenuDrawFn predecessorPipboyDraw = nullptr;
		LoadingMenuUpdateFn predecessorLoadingMenuUpdate = nullptr;
		LoadingMenuShowChangesFn predecessorLoadingMenuShowChanges = nullptr;
		bool imeMenuFactoryInstalled = false;
		bool imeMenuFactoryInstallFailed = false;
		bool pipboyDrawHookInstalled = false;
		bool pipboyDrawHookInstallFailed = false;
		bool loadingMenuUpdateHookInstalled = false;
		bool loadingMenuShowChangesHookInstalled = false;
		bool loadingMenuUpdateHookInstallFailed = false;
		bool loggedPipboyRttExclusion = false;
		bool creatingImeMenu = false;
		UInt32 imeVisualBoundsLogCount = 0;
	};

	NativeTileOverlayRuntimeState& OverlayRuntime();
	void AdvanceImeHostGeneration();
	void ConsumeNativePrewarmOverlayCommand();
	bool HasVerifiedLoadingMenuUpdateHook();
	bool EnsureImeMenuFactory();

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
	float ReadTextHeight(Tile* tile);

	void ClearPrewarmResolvedTiles();
	void ResetPrewarmForParent(Tile* parent);
	bool ResolvePrewarmTiles(Tile* root);
	bool IsResolvedPrewarmTreeAttached(Tile* parent);
	void ResetPrewarmPresentationState();
	bool EnsurePrewarmHost(Tile* parent);
	void LayoutPrewarmOverlay();
	UInt32 NextPrewarmCommandSequenceLocked();
	void CopyPrewarmCommandText(
		std::array<wchar_t, kPrewarmProgressTextCapacity>& target,
		std::wstring_view source);
	UInt32 PublishPrewarmOverlayUpdate(std::wstring_view detail,
		std::wstring_view stage, float progress);
	UInt32 PublishPrewarmOverlayVisibility(bool visible);
	UInt32 PublishPrewarmOverlayRefresh();
	UInt32 PublishPrewarmOverlayOwnerShutdown();
	bool IsPrewarmOverlayOwnerShutdownRequested();
	void ApplyPrewarmOverlayHidden();
	bool ApplyPrewarmOverlayVisible(const PrewarmOverlayCommand& command);
}

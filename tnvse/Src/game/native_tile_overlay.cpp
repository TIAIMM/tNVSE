#include "native_tile_overlay.h"

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

namespace fonthook
{
	namespace implementation::native_tile_overlay {}
	using namespace implementation::native_tile_overlay;

	namespace implementation::native_tile_overlay
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

		struct PrewarmOverlayCommand
		{
			std::array<wchar_t, kPrewarmProgressTextCapacity> detail = {};
			std::array<wchar_t, kPrewarmProgressTextCapacity> stage = {};
			float progress = 0.0f;
			UInt32 sequence = 0;
			UInt32 refreshSequence = 0;
			bool visible = false;
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

		NativeTileOverlayState s_state;
		std::atomic_bool s_imeReady = false;
		std::atomic<UInt32> s_imeHostGeneration = 1;
		std::atomic_bool s_prewarmReady = false;
		std::atomic_bool s_prewarmActive = false;
		SRWLOCK s_prewarmCommandLock = SRWLOCK_INIT;
		PrewarmOverlayCommand s_prewarmCommand;
		UInt32 s_nextPrewarmCommandSequence = 0;
		std::atomic<UInt32> s_prewarmPublishedSequence = 0;
		std::atomic<UInt32> s_prewarmConsumedSequence = 0;
		UInt32 s_prewarmAppliedRefreshSequence = 0;
		std::atomic<DWORD> s_prewarmConsumerThreadId = 0;
		std::atomic_bool s_prewarmConsumerDisabled = false;
		// Preserve the MSVC complete-object locator at vtable[-1] as well as
		// the 18 virtual entries used by Menu.
		std::array<SIZE_T, kMenuVTableEntryCount + 1>
			s_imeMenuVtable = {};
		CreateMenuByClassFn s_predecessorCreateMenuByClass = nullptr;
		RenderedMenuDrawFn s_predecessorPipboyDraw = nullptr;
		LoadingMenuUpdateFn s_predecessorLoadingMenuUpdate = nullptr;
		bool s_imeMenuFactoryInstalled = false;
		bool s_imeMenuFactoryInstallFailed = false;
		bool s_pipboyDrawHookInstalled = false;
		bool s_pipboyDrawHookInstallFailed = false;
		bool s_loadingMenuUpdateHookInstalled = false;
		bool s_loadingMenuUpdateHookInstallFailed = false;
		bool s_loggedPipboyRttExclusion = false;
		bool s_creatingImeMenu = false;

		void AdvanceImeHostGeneration()
		{
			const UInt32 previous = s_imeHostGeneration.fetch_add(
				1,
				std::memory_order_acq_rel);
			if (previous == std::numeric_limits<UInt32>::max())
				s_imeHostGeneration.store(1, std::memory_order_release);
		}
		UInt32 s_imeVisualBoundsLogCount = 0;

		void ConsumeNativePrewarmOverlayCommand();

		void __fastcall LoadingMenuUpdateHook(void* loadingMenu, void*)
		{
			if (s_predecessorLoadingMenuUpdate)
				s_predecessorLoadingMenuUpdate(loadingMenu);
			ConsumeNativePrewarmOverlayCommand();
		}

		bool HasVerifiedLoadingMenuUpdateHook()
		{
			if (!s_loadingMenuUpdateHookInstalled)
				return false;

			SIZE_T currentTarget = 0;
			const SIZE_T adapterTarget = reinterpret_cast<SIZE_T>(
				&LoadingMenuUpdateHook);
			const SIZE_T predecessorTarget = reinterpret_cast<SIZE_T>(
				s_predecessorLoadingMenuUpdate);
			return hook_identity::ReadRel32Target(
					kLoadingMenuThreadUpdateCallSite,
					hook_identity::Rel32Opcode::Call,
					currentTarget)
				&& currentTarget == adapterTarget
				&& predecessorTarget != adapterTarget
				&& hook_identity::IsExecutableTarget(predecessorTarget);
		}

		UInt32 __fastcall ImeMenuGetId(Menu*, void*)
		{
			return kImeMenuClass;
		}

		Menu* CreateImeMenu()
		{
			void* storage = BSNew(sizeof(Menu));
			if (!storage)
				return nullptr;

			// Menu::Menu is a void constructor; the allocated object is the result.
			ThisStdCall<void>(kMenuConstructor, storage);
			Menu* menu = static_cast<Menu*>(storage);

			*reinterpret_cast<SIZE_T**>(menu) =
				s_imeMenuVtable.data() + 1;
			// Menu::GetMaxDepth adds this signed field to the root Tile
			// depth. Keep the screen-space overlay above the current menus
			// without making it raise every menu created afterwards.
			menu->unk18 =
				static_cast<UInt32>(kImeMenuDepthContribution);
			return menu;
		}

		Menu* __fastcall CreateMenuByClassHook(
			void* factory,
			void*,
			UInt32 menuClass)
		{
			if (menuClass == kImeMenuClass && s_creatingImeMenu)
				return CreateImeMenu();
			return s_predecessorCreateMenuByClass
				? s_predecessorCreateMenuByClass(factory, menuClass)
				: nullptr;
		}

		void __fastcall PipboyRenderedMenuDrawHook(
			void* renderedMenu,
			void*,
			BSRenderedTexture* currentTexture,
			NiRenderer::ClearFlags clearMode,
			BSRenderedTexture* alternateTexture)
		{
			// The dedicated IME Menu is a normal pMenuRoot child, so the
			// Pip-Boy's rendered-menu pass would otherwise capture it and the
			// later screen-space UI pass would draw the same node a second
			// time. App-cull only for this RTT call, preserving its prior state
			// and restoring it before normal UI composition.
			NiNode* imeNode = nullptr;
			bool wasAppCulled = false;
			if (s_imeReady.load(std::memory_order_acquire)
				&& s_state.imeVisible
				&& s_state.imeRoot)
			{
				imeNode = s_state.imeRoot->spNiNode;
				if (imeNode)
				{
					wasAppCulled = imeNode->GetAppCulled();
					if (!wasAppCulled)
						imeNode->SetAppCulled(true);
				}
			}

			if (s_predecessorPipboyDraw)
			{
				s_predecessorPipboyDraw(renderedMenu, currentTexture,
					clearMode, alternateTexture);
			}

			if (imeNode && !wasAppCulled)
			{
				imeNode->SetAppCulled(false);
				if (!s_loggedPipboyRttExclusion)
				{
					s_loggedPipboyRttExclusion = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: excluded IME Menu node=%p from Pip-Boy rendered-menu RTT; screen-space pass remains enabled",
						imeNode);
				}
			}
		}

		bool EnsurePipboyDrawExclusionHook()
		{
			if (s_pipboyDrawHookInstalled)
			{
				if (!hook_identity::IsAccessibleRegion(
					kFOPipboyManagerDrawVTableEntry, sizeof(SIZE_T), false))
				{
					s_pipboyDrawHookInstalled = false;
					s_pipboyDrawHookInstallFailed = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: Pip-Boy RTT exclusion capability revoked; FORenderedMenu::Draw vtable entry became unreadable entry=0x%08X",
						static_cast<UInt32>(kFOPipboyManagerDrawVTableEntry));
					return false;
				}

				const SIZE_T currentTarget =
					*reinterpret_cast<const SIZE_T*>(
						kFOPipboyManagerDrawVTableEntry);
				const SIZE_T adapterTarget = reinterpret_cast<SIZE_T>(
					&PipboyRenderedMenuDrawHook);
				const SIZE_T predecessorTarget = reinterpret_cast<SIZE_T>(
					s_predecessorPipboyDraw);
				if (currentTarget == adapterTarget
					&& predecessorTarget != adapterTarget
					&& hook_identity::IsExecutableTarget(predecessorTarget))
				{
					return true;
				}
				if (currentTarget == predecessorTarget
					&& hook_identity::IsExecutableTarget(currentTarget))
				{
					// A clean restoration to our predecessor cannot retain a chain
					// through this hook. It is safe to publish it again below.
					s_pipboyDrawHookInstalled = false;
					s_predecessorPipboyDraw = nullptr;
				}
				else
				{
					// A different owner may have captured this hook. Keep the saved
					// predecessor callable, but do not claim verified reachability.
					s_pipboyDrawHookInstalled = false;
					s_pipboyDrawHookInstallFailed = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: Pip-Boy RTT exclusion capability revoked; observed handler=0x%08X adapter=0x%08X predecessor=0x%08X",
						static_cast<UInt32>(currentTarget),
						static_cast<UInt32>(adapterTarget),
						static_cast<UInt32>(predecessorTarget));
					return false;
				}
			}
			if (s_pipboyDrawHookInstallFailed)
				return false;

			if (!hook_identity::IsAccessibleRegion(
				kFOPipboyManagerDrawVTableEntry, sizeof(SIZE_T), false))
			{
				s_pipboyDrawHookInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: cannot install Pip-Boy RTT exclusion hook; FORenderedMenu::Draw vtable entry is unreadable entry=0x%08X",
					static_cast<UInt32>(kFOPipboyManagerDrawVTableEntry));
				return false;
			}

			const SIZE_T currentTarget =
				*reinterpret_cast<const SIZE_T*>(
					kFOPipboyManagerDrawVTableEntry);
			if (currentTarget == reinterpret_cast<SIZE_T>(
					&PipboyRenderedMenuDrawHook))
			{
				const SIZE_T predecessorTarget = reinterpret_cast<SIZE_T>(
					s_predecessorPipboyDraw);
				s_pipboyDrawHookInstalled =
					predecessorTarget != currentTarget
					&& hook_identity::IsExecutableTarget(predecessorTarget);
				if (!s_pipboyDrawHookInstalled)
				{
					s_pipboyDrawHookInstallFailed = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: Pip-Boy RTT exclusion hook is present but its predecessor is unavailable predecessor=0x%08X",
						static_cast<UInt32>(predecessorTarget));
				}
				return s_pipboyDrawHookInstalled;
			}
			if (!hook_identity::IsExecutableTarget(currentTarget))
			{
				s_pipboyDrawHookInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: cannot install Pip-Boy RTT exclusion hook; non-executable FORenderedMenu::Draw target=0x%08X entry=0x%08X",
					static_cast<UInt32>(currentTarget),
					static_cast<UInt32>(kFOPipboyManagerDrawVTableEntry));
				return false;
			}

			const RenderedMenuDrawFn previousPredecessor =
				s_predecessorPipboyDraw;
			s_predecessorPipboyDraw =
				reinterpret_cast<RenderedMenuDrawFn>(currentTarget);
			const SIZE_T adapterTarget = reinterpret_cast<SIZE_T>(
				&PipboyRenderedMenuDrawHook);
			// FOPipboyManager::Draw vtable slot
			// (__thiscall target via __fastcall shim).
			const SafeWrite32IfEqualResult publication =
				SafeWrite32IfEqualDetailed(kFOPipboyManagerDrawVTableEntry,
					adapterTarget, currentTarget);
			const bool published = publication.WasPublished();
			if (!published)
			{
				const SIZE_T observedTarget = publication.comparisonPerformed
					? publication.observed
					: *reinterpret_cast<const SIZE_T*>(
						kFOPipboyManagerDrawVTableEntry);
				s_predecessorPipboyDraw = previousPredecessor;
				s_pipboyDrawHookInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: Pip-Boy FORenderedMenu::Draw CAS did not publish entry=0x%08X predecessor=0x%08X observed=0x%08X compared=%u protectionError=%lu",
					static_cast<UInt32>(kFOPipboyManagerDrawVTableEntry),
					static_cast<UInt32>(currentTarget),
					static_cast<UInt32>(observedTarget),
					publication.comparisonPerformed ? 1u : 0u,
					publication.protectionError);
				return false;
			}
			if (!publication.PostconditionsComplete())
			{
				gLog.FormattedMessage(
					"tnvse_native_overlay: Pip-Boy draw hook published with incomplete write postconditions protectionRestored=%u protectionError=%lu cacheFlushed=%u cacheError=%lu",
					publication.protectionRestored ? 1u : 0u,
					publication.protectionError,
					publication.instructionCacheFlushed ? 1u : 0u,
					publication.cacheFlushError);
			}
			const SIZE_T observedTarget =
				*reinterpret_cast<const SIZE_T*>(
					kFOPipboyManagerDrawVTableEntry);
			if (observedTarget == adapterTarget)
			{
				s_pipboyDrawHookInstalled = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: installed Pip-Boy RTT exclusion hook chainedTarget=0x%08X vanilla=%d",
					static_cast<UInt32>(currentTarget),
					currentTarget == kFORenderedMenuDraw ? 1 : 0);
				return true;
			}

			if (observedTarget == currentTarget)
			{
				s_predecessorPipboyDraw = previousPredecessor;
				s_pipboyDrawHookInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: Pip-Boy FORenderedMenu::Draw hook was published but the slot returned to its predecessor entry=0x%08X predecessor=0x%08X",
					static_cast<UInt32>(kFOPipboyManagerDrawVTableEntry),
					static_cast<UInt32>(currentTarget));
				return false;
			}

			// Preserve a later vtable owner. It may already chain through this
			// hook, so replacing it with our predecessor could strand that chain.
			const bool successorExecutable =
				hook_identity::IsExecutableTarget(observedTarget);
			// Keeping the predecessor is required in case the observed owner
			// captured this hook, but an executable top-level target alone does not
			// prove that it did. Do not publish the RTT exclusion capability unless
			// the vtable slot itself was verified to contain our hook.
			s_pipboyDrawHookInstalled = false;
			s_pipboyDrawHookInstallFailed = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: Pip-Boy draw hook may be retained below observed handler=0x%08X predecessor=0x%08X executable=%u; reachability unverified, feature disabled",
				static_cast<UInt32>(observedTarget),
				static_cast<UInt32>(currentTarget),
				successorExecutable ? 1u : 0u);
			return false;
		}

		bool EnsureImeMenuFactory()
		{
			if (!EnsurePipboyDrawExclusionHook())
				return false;
			if (s_imeMenuFactoryInstalled)
			{
				SIZE_T currentTarget = 0;
				const bool targetReadable = hook_identity::ReadRel32Target(
					kCreateMenuByClassCallSite,
					hook_identity::Rel32Opcode::Call,
					currentTarget);
				const SIZE_T adapterTarget = reinterpret_cast<SIZE_T>(
					&CreateMenuByClassHook);
				const SIZE_T predecessorTarget = reinterpret_cast<SIZE_T>(
					s_predecessorCreateMenuByClass);
				if (targetReadable
					&& currentTarget == adapterTarget
					&& predecessorTarget != adapterTarget
					&& hook_identity::IsExecutableTarget(predecessorTarget))
				{
					return true;
				}
				if (targetReadable
					&& currentTarget == predecessorTarget
					&& hook_identity::IsExecutableTarget(currentTarget))
				{
					s_imeMenuFactoryInstalled = false;
					s_predecessorCreateMenuByClass = nullptr;
				}
				else
				{
					s_imeMenuFactoryInstalled = false;
					s_imeMenuFactoryInstallFailed = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: IME Menu factory capability revoked; observed target=0x%08X adapter=0x%08X predecessor=0x%08X readable=%u",
						static_cast<UInt32>(currentTarget),
						static_cast<UInt32>(adapterTarget),
						static_cast<UInt32>(predecessorTarget),
						targetReadable ? 1u : 0u);
					return false;
				}
			}
			if (s_imeMenuFactoryInstallFailed)
				return false;

			SIZE_T currentTarget = 0;
			if (!hook_identity::ReadRel32Target(
				kCreateMenuByClassCallSite,
				hook_identity::Rel32Opcode::Call,
				currentTarget))
			{
				s_imeMenuFactoryInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: cannot install IME Menu factory hook; expected CALL at 0x%08X",
					static_cast<UInt32>(kCreateMenuByClassCallSite));
				return false;
			}

			if (currentTarget == reinterpret_cast<SIZE_T>(
					&CreateMenuByClassHook))
			{
				const SIZE_T predecessorTarget = reinterpret_cast<SIZE_T>(
					s_predecessorCreateMenuByClass);
				s_imeMenuFactoryInstalled =
					predecessorTarget != currentTarget
					&& hook_identity::IsExecutableTarget(predecessorTarget);
				if (!s_imeMenuFactoryInstalled)
				{
					s_imeMenuFactoryInstallFailed = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: IME Menu factory hook is present but its predecessor is unavailable predecessor=0x%08X",
						static_cast<UInt32>(predecessorTarget));
				}
				return s_imeMenuFactoryInstalled;
			}
			if (!hook_identity::IsExecutableTarget(currentTarget)
				|| !hook_identity::IsAccessibleRegion(
					kMenuVTable - sizeof(SIZE_T),
					(kMenuVTableEntryCount + 1) * sizeof(SIZE_T),
					false))
			{
				s_imeMenuFactoryInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: cannot install IME Menu factory hook; invalid CreateMenuByClass target=0x%08X or Menu vtable",
					static_cast<UInt32>(currentTarget));
				return false;
			}

			const SIZE_T* vanillaVtable =
				reinterpret_cast<const SIZE_T*>(kMenuVTable);
			s_imeMenuVtable.front() = vanillaVtable[-1];
			std::copy_n(
				vanillaVtable,
				kMenuVTableEntryCount,
				s_imeMenuVtable.begin() + 1);
			s_imeMenuVtable[kMenuGetIdVtableIndex + 1] =
				reinterpret_cast<SIZE_T>(&ImeMenuGetId);
			s_predecessorCreateMenuByClass =
				reinterpret_cast<CreateMenuByClassFn>(currentTarget);
			// InterfaceManager menu factory -> CreateMenuByClass
			// (__thiscall target via __fastcall shim).
			WriteRelCall(kCreateMenuByClassCallSite, &CreateMenuByClassHook);
			const SIZE_T adapterTarget = reinterpret_cast<SIZE_T>(
				&CreateMenuByClassHook);
			SIZE_T observedTarget = 0;
			const bool observedReadable = hook_identity::ReadRel32Target(
				kCreateMenuByClassCallSite,
				hook_identity::Rel32Opcode::Call,
				observedTarget);
			if (observedReadable && observedTarget == adapterTarget)
			{
				s_imeMenuFactoryInstalled = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: installed dedicated IME Menu factory class=%u chainedTarget=0x%08X",
					kImeMenuClass,
					static_cast<UInt32>(currentTarget));
				return true;
			}

			if (observedReadable && observedTarget == currentTarget)
			{
				s_predecessorCreateMenuByClass = nullptr;
				s_imeMenuFactoryInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: IME Menu factory hook write did not publish call=0x%08X predecessor=0x%08X",
					static_cast<UInt32>(kCreateMenuByClassCallSite),
					static_cast<UInt32>(currentTarget));
				return false;
			}

			const bool successorExecutable = observedReadable
				&& hook_identity::IsExecutableTarget(observedTarget);
			// Preserve the saved predecessor for a possibly live chain, while
			// keeping the factory unavailable until our CALL target is observable.
			s_imeMenuFactoryInstalled = false;
			s_imeMenuFactoryInstallFailed = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: IME Menu factory hook may be retained below observed target=0x%08X predecessor=0x%08X readable=%u executable=%u; reachability unverified, feature disabled",
				static_cast<UInt32>(observedTarget),
				static_cast<UInt32>(currentTarget),
				observedReadable ? 1u : 0u,
				successorExecutable ? 1u : 0u);
			return false;
		}

		Tile* FindDirectChild(Tile* parent, const char* name)
		{
			if (!parent || !name)
				return nullptr;
			for (Tile* child : parent->kChildren)
			{
				if (child && !_stricmp(child->strName.c_str(), name))
					return child;
			}
			return nullptr;
		}

		bool IsDirectChild(const Tile* parent, const Tile* child)
		{
			if (!parent || !child)
				return false;
			for (const Tile* candidate : parent->kChildren)
			{
				if (candidate == child)
					return true;
			}
			return false;
		}

		bool IsNamedDirectChild(
			const Tile* parent,
			const Tile* child,
			const char* name)
		{
			return IsDirectChild(parent, child)
				&& name
				&& !_stricmp(child->strName.c_str(), name);
		}

		Tile* FindDirectMenuByClass(Tile* parent, UInt32 menuClass)
		{
			if (!parent)
				return nullptr;
			for (Tile* child : parent->kChildren)
			{
				Menu* menu = child ? child->GetMenu() : nullptr;
				if (menu && menu->GetID() == menuClass)
					return child;
			}
			return nullptr;
		}

		std::string WideToUiText(std::wstring_view value)
		{
			if (value.empty())
				return {};
			const int length = WideCharToMultiByte(
				g_usingWinEncoding,
				0,
				value.data(),
				static_cast<int>(value.size()),
				nullptr,
				0,
				nullptr,
				nullptr);
			if (length <= 0)
				return {};
			std::string result(static_cast<size_t>(length), '\0');
			const int written = WideCharToMultiByte(
				g_usingWinEncoding,
				0,
				value.data(),
				static_cast<int>(value.size()),
				result.data(),
				length,
				nullptr,
				nullptr);
			if (written <= 0)
				return {};
			return result;
		}

		struct ImeTextVisualBounds
		{
			float top = 0.0f;
			float height = 0.0f;
			bool valid = false;
		};

		ImeTextVisualBounds MeasureImeTextVisualBounds(
			std::string_view encoded,
			float measuredTextHeight)
		{
			ImeTextVisualBounds result;
			if (encoded.empty()
				|| !std::isfinite(measuredTextHeight)
				|| measuredTextHeight <= 0.0f)
			{
				return result;
			}

			Font* font = ResolveGameFont(FontManager::GetSingleton(), 1);
			if (!font || !font->pFontData)
				return result;

			const float sourceLineHeight =
				font->pFontData->pFontLetters[' '].fHeight;
			if (!std::isfinite(sourceLineHeight)
				|| sourceLineHeight <= 0.0f)
			{
				return result;
			}

			float visualTop = std::numeric_limits<float>::infinity();
			float visualBottom = -std::numeric_limits<float>::infinity();
			const float lineOriginZ = 2.0f
				* (font->pFontData->fBaseLine - font->fFontHeight);
			ExtraGlyphStore* extraGlyphs =
				GetExtraGlyphs(font->iFontNum);
			for (size_t offset = 0; offset < encoded.size();)
			{
				const UInt8 current =
					static_cast<UInt8>(encoded[offset]);
				FontLetter* glyph = nullptr;
				size_t unitLength = 1;
				UInt32 dbcsCode = 0;
				if (offset + 1 < encoded.size()
					&& TryDecodeDoubleByte(
						encoded.data() + offset,
						dbcsCode))
				{
					glyph = LookupDBGlyph(extraGlyphs, dbcsCode);
					unitLength = 2;
				}
				else if (current >= 0x20 && current != 0x7F
					&& current != static_cast<UInt8>(' '))
				{
					glyph =
						&font->pFontData->pFontLetters[current];
				}

				if (glyph
					&& std::isfinite(glyph->fTopEdge)
					&& std::isfinite(glyph->fHeight)
					&& glyph->fHeight > 0.0f)
				{
					// Font::CreateText starts the first line at
					// 2 * (baseline - fontHeight). FontLetter::fTopEdge
					// is then added in Z, while screen-space Y is -Z.
					// Convert that exact geometry convention to a
					// downward-positive offset from TileText::y.
					const float glyphTop =
						-(lineOriginZ + glyph->fTopEdge);
					const float glyphBottom =
						glyphTop + glyph->fHeight;
					if (std::isfinite(glyphTop)
						&& std::isfinite(glyphBottom)
						&& glyphBottom > glyphTop)
					{
						visualTop =
							std::min(visualTop, glyphTop);
						visualBottom =
							std::max(visualBottom, glyphBottom);
					}
				}
				offset += unitLength;
			}

			if (!std::isfinite(visualTop)
				|| !std::isfinite(visualBottom)
				|| visualBottom <= visualTop)
			{
				return result;
			}

			// TileText::height is the post-UIO height returned to the Tile.
			// Relate the raw FontLetter coordinates to that value instead of
			// assuming that the optional TileText zoom patch is installed.
			const float scale = measuredTextHeight / sourceLineHeight;
			if (!std::isfinite(scale) || scale <= 0.0f)
				return result;

			result.top = visualTop * scale;
			result.height = (visualBottom - visualTop) * scale;
			result.valid = std::isfinite(result.top)
				&& std::isfinite(result.height)
				&& result.height > 0.0f;
			return result;
		}

		void SetVisible(Tile* tile, bool visible)
		{
			if (tile)
				tile->SetValueFloat(
					Tile::kTileValue_visible, visible ? 1.0f : 0.0f, true);
		}

		void SetText(Tile* tile, std::wstring_view value)
		{
			if (!tile)
				return;
			const std::string encoded = WideToUiText(value);
			tile->SetValueString(Tile::kTileValue_string, encoded.c_str(), true);
		}

		void PublishTextGeometry(
			Tile* tile,
			std::wstring_view value,
			bool forceRefresh)
		{
			if (!tile)
				return;
			const std::string encoded = WideToUiText(value);
			if (forceRefresh)
			{
				// Tile::SetValueString may leave an equal string untouched. An
				// IME status row can therefore retain geometry that was created
				// while its Menu ancestor was hidden. Clear it once on logical
				// activation/repair so the replacement shape is built with the
				// now-visible ancestor and current TileShader alpha.
				tile->SetValueString(
					Tile::kTileValue_string, "", true);
			}
			tile->SetValueString(
				Tile::kTileValue_string, encoded.c_str(), true);
		}

		void RebuildTextGeometry(Tile* tile)
		{
			if (!tile)
				return;
			const std::string text =
				tile->GetValueString(Tile::kTileValue_string);
			tile->SetValueString(Tile::kTileValue_string, "", true);
			tile->SetValueString(
				Tile::kTileValue_string, text.c_str(), true);
		}

		bool TryGetReadOnlyMaximumMenuDepth(float& result)
		{
			InterfaceManager* manager = InterfaceManager::GetSingleton();
			Tile* menusRoot = manager ? manager->pMenuRoot : nullptr;
			if (!menusRoot)
				return false;

			// FalloutNV.exe 0xA1DFB0 is Menu::GetMaxDepth, despite the old
			// address-only name used here. Its return value is the maximum
			// top-level menu depth/thickness plus two, but the function also
			// rewrites the cursor Tile depth and cursor NiNode translation.
			// Overlay refreshes must reproduce only the read-only scan.
			float maximumDepth = 0.0f;
			for (Tile* child : menusRoot->kChildren)
			{
				if (!child || child == s_state.imeRoot)
					continue;
				Menu* menu = child->GetMenu();
				if (!menu)
					continue;

				const float menuDepth =
					child->GetValueFloat(Tile::kTileValue_depth);
				const float menuThickness = static_cast<float>(
					static_cast<SInt32>(menu->unk18));
				const float candidate = menuDepth + menuThickness;
				if (std::isfinite(candidate))
					maximumDepth = std::max(maximumDepth, candidate);
			}

			result = maximumDepth + 2.0f;
			return std::isfinite(result);
		}

		void SynchronizeOverlayDepth(Tile* root)
		{
			if (!root)
				return;

			float depth = 0.0f;
			if (!TryGetReadOnlyMaximumMenuDepth(depth))
				return;

			const float current =
				root->GetValueFloat(Tile::kTileValue_depth);
			if (std::isfinite(current)
				&& std::fabs(current - depth) <= 0.001f)
				return;

			// Avoid clearing/rebuilding the depth trait for every candidate
			// update. It changes only when the actual top-level menu depth does.
			root->SetValueFloat(Tile::kTileValue_depth, depth, true);
		}

		bool HasExpectedImeLinePresentation(size_t visibleCount)
		{
			if (!visibleCount || visibleCount > kImeLineCount)
				return false;
			for (size_t i = 0; i < kImeLineCount; ++i)
			{
				Tile* line = s_state.imeLines[i];
				if (!line)
					return false;
				const bool expectedVisible = i < visibleCount;
				const bool visible =
					line->GetValueFloat(
						Tile::kTileValue_visible) > 0.5f;
				if (visible != expectedVisible)
					return false;
				if (expectedVisible
					&& !line->GetValueString(
						Tile::kTileValue_string)[0])
				{
					return false;
				}
			}
			return true;
		}

		void ClearImeResolvedTiles()
		{
			const bool hadResolvedTiles = s_state.imeRoot
				|| s_state.imeBackground
				|| std::any_of(
					s_state.imeLines.begin(),
					s_state.imeLines.end(),
					[](Tile* tile) { return tile != nullptr; })
				|| std::any_of(
					s_state.imeHighlights.begin(),
					s_state.imeHighlights.end(),
					[](Tile* tile) { return tile != nullptr; });
			s_imeReady.store(false, std::memory_order_release);
			s_state.imeRoot = nullptr;
			s_state.imeBackground = nullptr;
			s_state.imeLines.fill(nullptr);
			s_state.imeHighlights.fill(nullptr);
			s_state.imeKey.clear();
			s_state.imeVisibleLineCount = 0;
			s_state.imeLineHeight = 0.0f;
			s_state.imeContentWidth = 0.0f;
			s_state.imeVisible = false;
			if (hadResolvedTiles)
				AdvanceImeHostGeneration();
		}

		void ClearPrewarmResolvedTiles()
		{
			s_prewarmReady.store(false, std::memory_order_release);
			s_state.prewarmRoot = nullptr;
			s_state.prewarmShade = nullptr;
			s_state.prewarmPanel = nullptr;
			s_state.prewarmTrack = nullptr;
			s_state.prewarmDetail = nullptr;
			s_state.prewarmStage = nullptr;
			s_state.prewarmFill = nullptr;
			s_state.prewarmPercent = nullptr;
			s_state.prewarmTitle = nullptr;
			s_state.prewarmLayoutSignature.fill(0.0f);
			s_state.prewarmProgressWidth = 520.0f;
			s_state.prewarmTileVisible = false;
		}

		void ResetImeForParent(Tile* parent)
		{
			// A changed pMenuRoot means every pointer under the old host
			// may already have been released. Never dereference or destroy it
			// here.
			ClearImeResolvedTiles();
			s_state.imeParent = parent;
			s_state.imeLoadFailed = false;
		}

		void ResetPrewarmForParent(Tile* parent)
		{
			// A changed LoadingMenu root means the engine may already have
			// released the old Tile tree. Never dereference the stale pointers.
			ClearPrewarmResolvedTiles();
			s_state.prewarmParent = parent;
			s_state.prewarmLoadFailed = false;
		}

		Tile* SynchronizeImeParent()
		{
			InterfaceManager* manager = InterfaceManager::GetSingleton();
			// The IME tree is a dedicated non-stacking Menu and a direct
			// pMenuRoot child. The Pip-Boy RTT hook temporarily culls its
			// NiNode while that root is captured, then restores screen drawing.
			Tile* parent = manager ? manager->pMenuRoot : nullptr;
			if (s_state.imeParent != parent)
				ResetImeForParent(parent);
			return parent;
		}

		Tile* GetLoadingMenuRoot()
		{
			Menu* loadingMenu =
				*reinterpret_cast<Menu**>(kLoadingMenu_pMe);
			return loadingMenu && loadingMenu->pRootTile
				? static_cast<Tile*>(loadingMenu->pRootTile)
				: nullptr;
		}

		Tile* SynchronizePrewarmParent()
		{
			// Match Cell Offset Generator: the prewarm component belongs to the
			// vanilla LoadingMenu tree, whose own thread continues Update and
			// ShowChanges while the game thread remains inside DeferredInit.
			Tile* parent = GetLoadingMenuRoot();
			if (s_state.prewarmParent != parent)
				ResetPrewarmForParent(parent);
			return parent;
		}

		void ReleaseAndDestroyAttachedRoot(
			Tile* parent,
			Tile* root,
			const char* expectedName)
		{
			if (IsNamedDirectChild(parent, root, expectedName))
			{
				SetVisible(root, false);
				ThisStdCall<void>(kTileRelease, root);
				delete root;
			}
		}

		bool IsImeMenuRegistered(Tile* root)
		{
			Menu* menu = root ? root->GetMenu() : nullptr;
			return menu
				&& menu->GetID() == kImeMenuClass
				&& menu->uiID == kImeMenuClass
				&& menu->pRootTile == static_cast<TileMenu*>(root)
				&& InterfaceManager::GetMenuByType(kImeMenuClass) == root;
		}

		bool RegisterImeMenuRoot(Tile* root)
		{
			Menu* menu = root ? root->GetMenu() : nullptr;
			if (!menu || menu->GetID() != kImeMenuClass)
				return false;

			// TileMenu::PostParse binds once when it encounters <class>,
			// before the remaining root traits have been parsed. Repeat the
			// vanilla Create() finalization after ReadXML completes, matching
			// built-in menus and Stewie Tweaks' injected menu lifecycle.
			menu->uiID = kImeMenuClass;
			ThisStdCall<void>(
				kMenuSetMenuTile,
				menu,
				static_cast<TileMenu*>(root),
				false);
			return IsImeMenuRegistered(root);
		}

		Tile* NormalizeOwnedImeRoots(Tile* parent)
		{
			if (!parent)
				return nullptr;

			std::vector<Tile*> ownedRoots;
			for (Tile* child : parent->kChildren)
			{
				if (child
					&& !_stricmp(
						child->strName.c_str(), "tNVSE_IME"))
				{
					ownedRoots.push_back(child);
				}
			}

			Tile* selected = nullptr;
			for (Tile* root : ownedRoots)
			{
				Menu* menu = root->GetMenu();
				if (!selected
					&& menu
					&& menu->GetID() == kImeMenuClass)
				{
					selected = root;
					continue;
				}
				ReleaseAndDestroyAttachedRoot(
					parent, root, "tNVSE_IME");
			}

			if (ownedRoots.size() > (selected ? 1u : 0u))
			{
				gLog.FormattedMessage(
					"tnvse_native_overlay: removed %u stale or duplicate IME Menu root(s) before singleton bind",
					static_cast<UInt32>(
						ownedRoots.size() - (selected ? 1u : 0u)));
			}
			return selected;
		}

		bool ResolveImeTiles(Tile* root)
		{
			if (!root || _stricmp(root->strName.c_str(), "tNVSE_IME"))
				return false;
			Menu* menu = root->GetMenu();
			if (!menu || menu->GetID() != kImeMenuClass)
				return false;

			Tile* background =
				FindDirectChild(root, "tNVSE_IME_Background");
			std::array<Tile*, kImeLineCount> lines = {};
			std::array<Tile*, kImeHighlightCount> highlights = {};
			for (size_t i = 0; i < kImeLineCount; ++i)
			{
				char lineName[32] = {};
				_snprintf_s(lineName, _countof(lineName), _TRUNCATE,
					"tNVSE_IME_Line%02u", static_cast<UInt32>(i));
				lines[i] = FindDirectChild(root, lineName);
				if (!lines[i])
					return false;
			}
			for (size_t i = 0; i < kImeHighlightCount; ++i)
			{
				char highlightName[40] = {};
				_snprintf_s(highlightName, _countof(highlightName), _TRUNCATE,
					"tNVSE_IME_Highlight%02u", static_cast<UInt32>(i));
				highlights[i] = FindDirectChild(root, highlightName);
				if (!highlights[i])
					return false;
			}
			if (!background)
				return false;

			const bool changed = s_state.imeRoot != root
				|| s_state.imeBackground != background
				|| s_state.imeLines != lines
				|| s_state.imeHighlights != highlights;
			s_state.imeRoot = root;
			s_state.imeBackground = background;
			s_state.imeLines = lines;
			s_state.imeHighlights = highlights;
			if (changed)
				AdvanceImeHostGeneration();
			return true;
		}

		bool ResolvePrewarmTiles(Tile* root)
		{
			if (!root || _stricmp(root->strName.c_str(), "tNVSE_Prewarm"))
				return false;

			Tile* shade =
				FindDirectChild(root, "tNVSE_Prewarm_Shade");
			Tile* panel =
				FindDirectChild(root, "tNVSE_Prewarm_Panel");
			Tile* track =
				FindDirectChild(root, "tNVSE_Prewarm_Track");
			Tile* detail =
				FindDirectChild(root, "tNVSE_Prewarm_Detail");
			Tile* stage =
				FindDirectChild(root, "tNVSE_Prewarm_Stage");
			Tile* fill =
				FindDirectChild(root, "tNVSE_Prewarm_Fill");
			Tile* percent =
				FindDirectChild(root, "tNVSE_Prewarm_Percent");
			Tile* title =
				FindDirectChild(root, "tNVSE_Prewarm_Title");
			if (!shade || !panel || !track || !detail || !stage || !fill
				|| !percent || !title)
				return false;

			s_state.prewarmRoot = root;
			s_state.prewarmShade = shade;
			s_state.prewarmPanel = panel;
			s_state.prewarmTrack = track;
			s_state.prewarmDetail = detail;
			s_state.prewarmStage = stage;
			s_state.prewarmFill = fill;
			s_state.prewarmPercent = percent;
			s_state.prewarmTitle = title;
			return true;
		}

		bool IsResolvedImeTreeAttached(Tile* parent)
		{
			if (!IsNamedDirectChild(
					parent, s_state.imeRoot, "tNVSE_IME")
				|| !IsNamedDirectChild(
					s_state.imeRoot,
					s_state.imeBackground,
					"tNVSE_IME_Background"))
			{
				return false;
			}
			for (size_t i = 0; i < s_state.imeLines.size(); ++i)
			{
				char name[32] = {};
				_snprintf_s(name, _countof(name), _TRUNCATE,
					"tNVSE_IME_Line%02u", static_cast<UInt32>(i));
				if (!IsNamedDirectChild(
						s_state.imeRoot, s_state.imeLines[i], name))
					return false;
			}
			for (size_t i = 0; i < s_state.imeHighlights.size(); ++i)
			{
				char name[40] = {};
				_snprintf_s(name, _countof(name), _TRUNCATE,
					"tNVSE_IME_Highlight%02u", static_cast<UInt32>(i));
				if (!IsNamedDirectChild(
						s_state.imeRoot, s_state.imeHighlights[i], name))
					return false;
			}
			return true;
		}

		bool IsResolvedPrewarmTreeAttached(Tile* parent)
		{
			if (!IsNamedDirectChild(
					parent, s_state.prewarmRoot, "tNVSE_Prewarm"))
			{
				return false;
			}
			const std::array<Tile*, 8> required = {
				s_state.prewarmShade,
				s_state.prewarmPanel,
				s_state.prewarmTrack,
				s_state.prewarmDetail,
				s_state.prewarmStage,
				s_state.prewarmFill,
				s_state.prewarmPercent,
				s_state.prewarmTitle,
			};
			const std::array<const char*, 8> names = {
				"tNVSE_Prewarm_Shade",
				"tNVSE_Prewarm_Panel",
				"tNVSE_Prewarm_Track",
				"tNVSE_Prewarm_Detail",
				"tNVSE_Prewarm_Stage",
				"tNVSE_Prewarm_Fill",
				"tNVSE_Prewarm_Percent",
				"tNVSE_Prewarm_Title",
			};
			for (size_t i = 0; i < required.size(); ++i)
			{
				if (!IsNamedDirectChild(
						s_state.prewarmRoot, required[i], names[i]))
					return false;
			}
			return true;
		}

		void ResetImePresentationState()
		{
			s_state.imeKey.clear();
			s_state.imeVisibleLineCount = 0;
			s_state.imeLineHeight = 0.0f;
			s_state.imeContentWidth = 0.0f;
			s_state.imeVisible = false;
		}

		void ResetPrewarmPresentationState()
		{
			s_state.prewarmLayoutSignature.fill(0.0f);
			s_state.prewarmProgressWidth = 520.0f;
			s_state.prewarmTileVisible = false;
		}

		bool EnsureImeHost(Tile* parent)
		{
			if (s_state.imeRoot)
			{
				Tile* root = s_state.imeRoot;
				if (IsResolvedImeTreeAttached(parent))
				{
					if (IsImeMenuRegistered(root)
						|| RegisterImeMenuRoot(root))
					{
						s_imeReady.store(true, std::memory_order_release);
						return true;
					}
				}
				if (IsDirectChild(parent, root)
					&& ResolveImeTiles(root)
					&& RegisterImeMenuRoot(root))
				{
					ResetImePresentationState();
					SetVisible(root, false);
					s_imeReady.store(true, std::memory_order_release);
					gLog.FormattedMessage(
						"tnvse_native_overlay: rebound IME Menu after child-tree replacement host=%p parent=%p",
						root, parent);
					return true;
				}
				ClearImeResolvedTiles();
				s_state.imeLoadFailed = false;
				ReleaseAndDestroyAttachedRoot(
					parent, root, "tNVSE_IME");
				gLog.FormattedMessage(
					"tnvse_native_overlay: IME Menu was detached, malformed, or lost its native registration; reloading");
			}
			if (s_state.imeLoadFailed)
				return false;

			if (Tile* existing = NormalizeOwnedImeRoots(parent))
			{
				if (ResolveImeTiles(existing)
					&& RegisterImeMenuRoot(existing))
				{
					ResetImePresentationState();
					SetVisible(existing, false);
					s_imeReady.store(true, std::memory_order_release);
					gLog.FormattedMessage(
						"tnvse_native_overlay: adopted existing IME Menu class=%u host=%p parent=%p",
						kImeMenuClass,
						existing,
						parent);
					return true;
				}

				ClearImeResolvedTiles();
				ReleaseAndDestroyAttachedRoot(
					parent, existing, "tNVSE_IME");
				gLog.FormattedMessage(
					"tnvse_native_overlay: discarded malformed owned IME Menu class=%u before reloading",
					kImeMenuClass);
			}

			Tile* foreign =
				FindDirectMenuByClass(parent, kImeMenuClass);
			if (!foreign)
				foreign =
					InterfaceManager::GetMenuByType(kImeMenuClass);
			if (foreign)
			{
				const bool attached = IsDirectChild(parent, foreign);
				s_state.imeLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: Menu Code %u is already owned by foreign Tile host=%p name='%s'; native IME overlay unavailable and system candidate UI remains suppressed",
					kImeMenuClass,
					foreign,
					attached
						? foreign->strName.c_str()
						: "<registered outside current pMenuRoot>");
				return false;
			}
			if (!EnsureImeMenuFactory())
			{
				s_state.imeLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: IME Menu factory unavailable; native IME overlay unavailable and system candidate UI remains suppressed");
				return false;
			}

			s_creatingImeMenu = true;
			Tile* root = ThisStdCall<Tile*>(
				kTileReadFile, parent, kImeOverlayXmlPath);
			s_creatingImeMenu = false;
			if (!root || !IsDirectChild(parent, root)
				|| !ResolveImeTiles(root)
				|| !RegisterImeMenuRoot(root))
			{
				ClearImeResolvedTiles();
				ReleaseAndDestroyAttachedRoot(
					parent, root, "tNVSE_IME");
				s_state.imeLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: failed to load, resolve, or register IME Menu path='%s'; native IME overlay unavailable and system candidate UI remains suppressed",
					kImeOverlayXmlPath);
				return false;
			}

			SetVisible(s_state.imeRoot, false);
			s_imeReady.store(true, std::memory_order_release);
			gLog.FormattedMessage(
				"tnvse_native_overlay: loaded IME Menu path='%s' host=%p parent=%p class=%u",
				kImeOverlayXmlPath,
				s_state.imeRoot,
				parent,
				kImeMenuClass);
			return true;
		}

		bool EnsurePrewarmHost(Tile* parent)
		{
			if (s_state.prewarmRoot)
			{
				Tile* root = s_state.prewarmRoot;
				if (IsResolvedPrewarmTreeAttached(parent))
				{
					const bool tileVisible =
						root->GetValueFloat(Tile::kTileValue_visible) > 0.5f;
					if (s_state.prewarmTileVisible && !tileVisible)
						ResetPrewarmPresentationState();
					else if (!s_state.prewarmTileVisible && tileVisible)
						SetVisible(root, false);
					s_prewarmReady.store(true, std::memory_order_release);
					return true;
				}
				if (IsDirectChild(parent, root) && ResolvePrewarmTiles(root))
				{
					ResetPrewarmPresentationState();
					SetVisible(root, false);
					s_prewarmReady.store(true, std::memory_order_release);
					gLog.FormattedMessage(
						"tnvse_native_overlay: rebound prewarm Tile component after child-tree replacement host=%p parent=%p",
						root, parent);
					return true;
				}
				ClearPrewarmResolvedTiles();
				s_state.prewarmLoadFailed = false;
				ReleaseAndDestroyAttachedRoot(
					parent, root, "tNVSE_Prewarm");
				gLog.FormattedMessage(
					"tnvse_native_overlay: prewarm Tile component was detached or malformed; reloading component");
			}
			if (s_state.prewarmLoadFailed)
				return false;

			Tile* root = ThisStdCall<Tile*>(
				kTileReadFile, parent, kPrewarmOverlayXmlPath);
			if (!root || !IsDirectChild(parent, root)
				|| !ResolvePrewarmTiles(root))
			{
				ClearPrewarmResolvedTiles();
				ReleaseAndDestroyAttachedRoot(
					parent, root, "tNVSE_Prewarm");
				s_state.prewarmLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: failed to load or resolve prewarm component path='%s'; font prewarm continues without Tile progress UI",
					kPrewarmOverlayXmlPath);
				return false;
			}

			SetVisible(s_state.prewarmRoot, false);
			s_prewarmReady.store(true, std::memory_order_release);
			gLog.FormattedMessage(
				"tnvse_native_overlay: loaded prewarm Tile component path='%s' host=%p parent=%p",
				kPrewarmOverlayXmlPath, s_state.prewarmRoot, parent);
			return true;
		}

		std::wstring BuildImeKey(
			const std::vector<NativeTileOverlayLine>& lines)
		{
			std::wstring result;
			for (const NativeTileOverlayLine& line : lines)
			{
				result.push_back(line.highlighted ? L'\1' : L'\0');
				result += line.text;
				result.push_back(L'\n');
			}
			return result;
		}

		float ReadTextHeight(Tile* tile)
		{
			const float height = tile
				? tile->GetValueFloat(Tile::kTileValue_height)
				: 0.0f;
			return std::isfinite(height) && height > 0.0f
				? std::max(kPrewarmMinimumTextHeight, height)
				: kPrewarmMinimumTextHeight;
		}

		void LayoutPrewarmOverlay()
		{
			if (!s_state.prewarmRoot || !s_state.prewarmPanel)
				return;
			const float rootWidth =
				s_state.prewarmRoot->GetValueFloat(
					Tile::kTileValue_width);
			const float rootHeight =
				s_state.prewarmRoot->GetValueFloat(
					Tile::kTileValue_height);
			if (!std::isfinite(rootWidth)
				|| !std::isfinite(rootHeight)
				|| rootWidth <= 0.0f
				|| rootHeight <= 0.0f)
			{
				return;
			}

			const float titleHeight = ReadTextHeight(
				s_state.prewarmTitle);
			const float detailHeight = ReadTextHeight(
				s_state.prewarmDetail);
			const float stageHeight = ReadTextHeight(
				s_state.prewarmStage);
			const float percentHeight = ReadTextHeight(
				s_state.prewarmPercent);
			const std::array<float, 6> signature = {
				rootWidth,
				rootHeight,
				titleHeight,
				detailHeight,
				stageHeight,
				percentHeight,
			};
			bool changed = false;
			for (size_t i = 0; i < signature.size(); ++i)
			{
				if (std::fabs(
						signature[i]
							- s_state.prewarmLayoutSignature[i])
					> 0.25f)
				{
					changed = true;
					break;
				}
			}
			if (!changed)
				return;
			s_state.prewarmLayoutSignature = signature;

			const float panelWidth = std::min(
				kPrewarmMaximumPanelWidth,
				std::max(
					kPrewarmMinimumPanelWidth,
					rootWidth - 24.0f));
			const float panelHeight = std::max(
				190.0f,
				20.0f + titleHeight
					+ 6.0f + detailHeight
					+ 18.0f + 16.0f
					+ 10.0f + std::max(stageHeight, percentHeight)
					+ 20.0f);
			const float panelX = std::max(
				12.0f, (rootWidth - panelWidth) * 0.5f);
			const float panelY = std::max(
				12.0f, (rootHeight - panelHeight) * 0.5f);
			const float titleY = panelY + 20.0f;
			const float detailY =
				titleY + titleHeight + 6.0f;
			const float trackX = panelX + 50.0f;
			const float trackY =
				detailY + detailHeight + 18.0f;
			const float progressWidth =
				std::max(220.0f, panelWidth - 100.0f);
			const float stageY = trackY + 16.0f + 10.0f;

			s_state.prewarmPanel->SetValueFloat(
				Tile::kTileValue_x, panelX, true);
			s_state.prewarmPanel->SetValueFloat(
				Tile::kTileValue_y, panelY, true);
			s_state.prewarmPanel->SetValueFloat(
				Tile::kTileValue_width, panelWidth, true);
			s_state.prewarmPanel->SetValueFloat(
				Tile::kTileValue_height, panelHeight, true);
			s_state.prewarmTitle->SetValueFloat(
				Tile::kTileValue_x, panelX + 30.0f, true);
			s_state.prewarmTitle->SetValueFloat(
				Tile::kTileValue_y, titleY, true);
			s_state.prewarmDetail->SetValueFloat(
				Tile::kTileValue_x, panelX + 30.0f, true);
			s_state.prewarmDetail->SetValueFloat(
				Tile::kTileValue_y, detailY, true);
			s_state.prewarmDetail->SetValueFloat(
				Tile::kTileValue_wrapwidth,
				std::max(1.0f, panelWidth - 60.0f),
				true);
			s_state.prewarmTrack->SetValueFloat(
				Tile::kTileValue_x, trackX, true);
			s_state.prewarmTrack->SetValueFloat(
				Tile::kTileValue_y, trackY, true);
			s_state.prewarmTrack->SetValueFloat(
				Tile::kTileValue_width, progressWidth, true);
			s_state.prewarmFill->SetValueFloat(
				Tile::kTileValue_x, trackX, true);
			s_state.prewarmFill->SetValueFloat(
				Tile::kTileValue_y, trackY, true);
			s_state.prewarmStage->SetValueFloat(
				Tile::kTileValue_x, trackX, true);
			s_state.prewarmStage->SetValueFloat(
				Tile::kTileValue_y, stageY, true);
			s_state.prewarmStage->SetValueFloat(
				Tile::kTileValue_wrapwidth,
				std::max(1.0f, panelWidth - 200.0f),
				true);
			s_state.prewarmPercent->SetValueFloat(
				Tile::kTileValue_x,
				panelX + panelWidth - 120.0f,
				true);
			s_state.prewarmPercent->SetValueFloat(
				Tile::kTileValue_y, stageY, true);
			s_state.prewarmProgressWidth = progressWidth;
		}

		UInt32 NextPrewarmCommandSequenceLocked()
		{
			++s_nextPrewarmCommandSequence;
			if (!s_nextPrewarmCommandSequence)
				++s_nextPrewarmCommandSequence;
			return s_nextPrewarmCommandSequence;
		}

		void CopyPrewarmCommandText(
			std::array<wchar_t, kPrewarmProgressTextCapacity>& target,
			std::wstring_view source)
		{
			target.fill(L'\0');
			const size_t length = std::min(
				source.size(), target.size() - 1u);
			if (length)
				std::copy_n(source.data(), length, target.data());
		}

		UInt32 PublishPrewarmOverlayUpdate(
			std::wstring_view detail,
			std::wstring_view stage,
			float progress)
		{
			AcquireSRWLockExclusive(&s_prewarmCommandLock);
			const UInt32 sequence = NextPrewarmCommandSequenceLocked();
			CopyPrewarmCommandText(s_prewarmCommand.detail, detail);
			CopyPrewarmCommandText(s_prewarmCommand.stage, stage);
			s_prewarmCommand.progress = std::clamp(progress, 0.0f, 1.0f);
			s_prewarmCommand.visible = true;
			s_prewarmCommand.sequence = sequence;
			ReleaseSRWLockExclusive(&s_prewarmCommandLock);
			s_prewarmActive.store(
				!s_prewarmConsumerDisabled.load(std::memory_order_acquire),
				std::memory_order_release);
			s_prewarmPublishedSequence.store(
				sequence, std::memory_order_release);
			return sequence;
		}

		UInt32 PublishPrewarmOverlayVisibility(bool visible)
		{
			AcquireSRWLockExclusive(&s_prewarmCommandLock);
			const UInt32 sequence = NextPrewarmCommandSequenceLocked();
			s_prewarmCommand.visible = visible;
			s_prewarmCommand.sequence = sequence;
			ReleaseSRWLockExclusive(&s_prewarmCommandLock);
			s_prewarmActive.store(
				visible
					&& !s_prewarmConsumerDisabled.load(
						std::memory_order_acquire),
				std::memory_order_release);
			s_prewarmPublishedSequence.store(
				sequence, std::memory_order_release);
			return sequence;
		}

		UInt32 PublishPrewarmOverlayRefresh()
		{
			AcquireSRWLockExclusive(&s_prewarmCommandLock);
			const UInt32 sequence = NextPrewarmCommandSequenceLocked();
			s_prewarmCommand.refreshSequence = sequence;
			s_prewarmCommand.sequence = sequence;
			ReleaseSRWLockExclusive(&s_prewarmCommandLock);
			s_prewarmPublishedSequence.store(
				sequence, std::memory_order_release);
			return sequence;
		}

		PrewarmOverlayCommand ReadLatestPrewarmOverlayCommand()
		{
			PrewarmOverlayCommand command;
			AcquireSRWLockShared(&s_prewarmCommandLock);
			command = s_prewarmCommand;
			ReleaseSRWLockShared(&s_prewarmCommandLock);
			return command;
		}

		void ApplyPrewarmOverlayHidden()
		{
			Tile* parent = SynchronizePrewarmParent();
			Tile* root = s_state.prewarmRoot;
			const bool attached = IsNamedDirectChild(
				parent, root, "tNVSE_Prewarm");
			if (s_state.prewarmTileVisible
				&& IsNativePrewarmOverlayHostReady()
				&& attached)
			{
				SetVisible(root, false);
			}
			if (root && attached)
			{
				ReleaseAndDestroyAttachedRoot(
					parent, root, "tNVSE_Prewarm");
			}
			ClearPrewarmResolvedTiles();
		}

		bool ApplyPrewarmOverlayVisible(
			const PrewarmOverlayCommand& command)
		{
			Tile* parent = SynchronizePrewarmParent();
			if (!parent)
			{
				if (!s_state.prewarmParentUnavailableLogged)
				{
					s_state.prewarmParentUnavailableLogged = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: LoadingMenu root not ready; retaining queued prewarm progress until owner thread can attach it");
				}
				return false;
			}
			s_state.prewarmParentUnavailableLogged = false;
			if (!EnsurePrewarmHost(parent))
			{
				if (s_state.prewarmLoadFailed)
				{
					s_prewarmConsumerDisabled.store(
						true, std::memory_order_release);
					s_prewarmActive.store(false, std::memory_order_release);
				}
				return true;
			}

			if (!s_state.prewarmTileVisible)
			{
				SetVisible(s_state.prewarmRoot, true);
				s_state.prewarmTileVisible = true;
			}

			SetText(s_state.prewarmDetail, command.detail.data());
			SetText(s_state.prewarmStage, command.stage.data());
			wchar_t percent[16] = {};
			_snwprintf_s(percent, _countof(percent), _TRUNCATE, L"%u%%",
				static_cast<UInt32>(std::lround(
					std::clamp(command.progress, 0.0f, 1.0f) * 100.0f)));
			SetText(s_state.prewarmPercent, percent);

			if (command.refreshSequence
				&& command.refreshSequence != s_prewarmAppliedRefreshSequence)
			{
				RebuildTextGeometry(s_state.prewarmTitle);
				RebuildTextGeometry(s_state.prewarmDetail);
				RebuildTextGeometry(s_state.prewarmStage);
				RebuildTextGeometry(s_state.prewarmPercent);
				s_state.prewarmLayoutSignature.fill(0.0f);
				s_prewarmAppliedRefreshSequence = command.refreshSequence;
			}

			LayoutPrewarmOverlay();
			s_state.prewarmFill->SetValueFloat(
				Tile::kTileValue_width,
				s_state.prewarmProgressWidth
					* std::clamp(command.progress, 0.0f, 1.0f),
				true);
			return true;
		}

		void ConsumeNativePrewarmOverlayCommand()
		{
			const UInt32 published = s_prewarmPublishedSequence.load(
				std::memory_order_acquire);
			if (!published || published == s_prewarmConsumedSequence.load(
					std::memory_order_acquire))
			{
				return;
			}

			const PrewarmOverlayCommand command =
				ReadLatestPrewarmOverlayCommand();
			if (!command.sequence)
				return;

			DWORD expectedThread = 0;
			const DWORD currentThread = GetCurrentThreadId();
			if (s_prewarmConsumerThreadId.compare_exchange_strong(
				expectedThread, currentThread, std::memory_order_acq_rel))
			{
				gLog.FormattedMessage(
					"tnvse_native_overlay: prewarm progress consumer active thread=%u owner=LoadingMenuThread policy=queued-snapshot legacyFontRoute=thread-local-fnt",
					currentThread);
			}
			if (s_prewarmConsumerDisabled.load(std::memory_order_acquire))
			{
				s_prewarmActive.store(false, std::memory_order_release);
				s_prewarmConsumedSequence.store(
					command.sequence, std::memory_order_release);
				return;
			}

			try
			{
				// Cell Offset Generator keeps generation on workers and limits its
				// foreground path to LoadingMenu progress presentation. This path
				// additionally confines every Tile mutation to LoadingMenuThread
				// and routes only that presentation through legacy FNT geometry.
				// Prefab import and text refresh therefore cannot recursively enter
				// the MTSDF transaction that produced this command.
				ScopedLegacyFntRenderRoute legacyFntRoute;
				bool applied = true;
				if (command.visible)
					applied = ApplyPrewarmOverlayVisible(command);
				else
					ApplyPrewarmOverlayHidden();
				if (!applied)
					return;
			}
			catch (const std::exception& error)
			{
				s_prewarmConsumerDisabled.store(true, std::memory_order_release);
				s_prewarmReady.store(false, std::memory_order_release);
				s_prewarmActive.store(false, std::memory_order_release);
				s_state.prewarmLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: prewarm progress consumer disabled reason=%s policy=font-prewarm-continues",
					error.what());
			}
			catch (...)
			{
				s_prewarmConsumerDisabled.store(true, std::memory_order_release);
				s_prewarmReady.store(false, std::memory_order_release);
				s_prewarmActive.store(false, std::memory_order_release);
				s_state.prewarmLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: prewarm progress consumer disabled reason=unknown policy=font-prewarm-continues");
			}
			s_prewarmConsumedSequence.store(
				command.sequence, std::memory_order_release);
		}
	}

	bool InstallNativePrewarmOverlayLoadingThreadHook()
	{
		if (s_loadingMenuUpdateHookInstalled)
		{
			SIZE_T currentTarget = 0;
			const bool targetReadable = hook_identity::ReadRel32Target(
				kLoadingMenuThreadUpdateCallSite,
				hook_identity::Rel32Opcode::Call,
				currentTarget);
			const SIZE_T adapterTarget = reinterpret_cast<SIZE_T>(
				&LoadingMenuUpdateHook);
			const SIZE_T predecessorTarget = reinterpret_cast<SIZE_T>(
				s_predecessorLoadingMenuUpdate);
			if (targetReadable
				&& currentTarget == adapterTarget
				&& predecessorTarget != adapterTarget
				&& hook_identity::IsExecutableTarget(predecessorTarget))
			{
				return true;
			}
			if (targetReadable
				&& currentTarget == predecessorTarget
				&& hook_identity::IsExecutableTarget(currentTarget))
			{
				s_loadingMenuUpdateHookInstalled = false;
				s_predecessorLoadingMenuUpdate = nullptr;
			}
			else
			{
				s_loadingMenuUpdateHookInstalled = false;
				s_loadingMenuUpdateHookInstallFailed = true;
				s_prewarmConsumerDisabled.store(true, std::memory_order_release);
				s_prewarmReady.store(false, std::memory_order_release);
				s_prewarmActive.store(false, std::memory_order_release);
				gLog.FormattedMessage(
					"tnvse_native_overlay: LoadingMenuThread prewarm capability revoked; observed target=0x%08X adapter=0x%08X predecessor=0x%08X readable=%u",
					static_cast<UInt32>(currentTarget),
					static_cast<UInt32>(adapterTarget),
					static_cast<UInt32>(predecessorTarget),
					targetReadable ? 1u : 0u);
				return false;
			}
		}
		if (s_loadingMenuUpdateHookInstallFailed)
			return false;

		SIZE_T currentUpdateTarget = 0;
		SIZE_T currentShowChangesTarget = 0;
		const SIZE_T loadingMenuPointerLoad =
			kLoadingMenuThreadUpdateCallSite
				- kExpectedLoadingMenuInstanceLoadInstruction.size();
		if (!hook_identity::IsAccessibleRegion(
				loadingMenuPointerLoad,
				kExpectedLoadingMenuInstanceLoadInstruction.size()
					+ 2u * sizeof(UInt8) + 2u * sizeof(SInt32),
				true)
			|| !hook_identity::MatchesBytesUnchecked(
				loadingMenuPointerLoad,
				kExpectedLoadingMenuInstanceLoadInstruction.data(),
				kExpectedLoadingMenuInstanceLoadInstruction.size())
			|| !hook_identity::ReadRel32Target(
				kLoadingMenuThreadUpdateCallSite,
				hook_identity::Rel32Opcode::Call,
				currentUpdateTarget)
			|| !hook_identity::ReadRel32Target(
				kLoadingMenuThreadShowChangesCallSite,
				hook_identity::Rel32Opcode::Call,
				currentShowChangesTarget)
			|| !hook_identity::IsExecutableTarget(currentUpdateTarget)
			|| !hook_identity::IsExecutableTarget(currentShowChangesTarget))
		{
			s_loadingMenuUpdateHookInstallFailed = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: cannot install LoadingMenuThread prewarm consumer; executable identity mismatch updateCall=0x%08X showChangesCall=0x%08X",
				static_cast<UInt32>(kLoadingMenuThreadUpdateCallSite),
				static_cast<UInt32>(kLoadingMenuThreadShowChangesCallSite));
			return false;
		}

		if (currentUpdateTarget == reinterpret_cast<SIZE_T>(
				&LoadingMenuUpdateHook))
		{
			const SIZE_T predecessorTarget = reinterpret_cast<SIZE_T>(
				s_predecessorLoadingMenuUpdate);
			s_loadingMenuUpdateHookInstalled =
				predecessorTarget != currentUpdateTarget
				&& hook_identity::IsExecutableTarget(predecessorTarget);
			if (!s_loadingMenuUpdateHookInstalled)
			{
				s_loadingMenuUpdateHookInstallFailed = true;
				s_prewarmConsumerDisabled.store(true, std::memory_order_release);
				gLog.FormattedMessage(
					"tnvse_native_overlay: LoadingMenuThread prewarm hook is present but its predecessor is unavailable predecessor=0x%08X",
					static_cast<UInt32>(predecessorTarget));
			}
			return s_loadingMenuUpdateHookInstalled;
		}

		s_predecessorLoadingMenuUpdate =
			reinterpret_cast<LoadingMenuUpdateFn>(currentUpdateTarget);
		// LoadingMenuThread -> LoadingMenu::Update
		// (__thiscall target via __fastcall shim).
		WriteRelCall(kLoadingMenuThreadUpdateCallSite, &LoadingMenuUpdateHook);
		const SIZE_T adapterTarget = reinterpret_cast<SIZE_T>(
			&LoadingMenuUpdateHook);
		SIZE_T observedTarget = 0;
		const bool observedReadable = hook_identity::ReadRel32Target(
			kLoadingMenuThreadUpdateCallSite,
			hook_identity::Rel32Opcode::Call,
			observedTarget);
		if (observedReadable && observedTarget == adapterTarget)
		{
			s_loadingMenuUpdateHookInstalled = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: installed LoadingMenuThread prewarm consumer call=0x%08X chainedTarget=0x%08X vanillaUpdate=%u showChangesTarget=0x%08X vanillaShowChanges=%u policy=queued-snapshot legacyFontRoute=thread-local-fnt",
				static_cast<UInt32>(kLoadingMenuThreadUpdateCallSite),
				static_cast<UInt32>(currentUpdateTarget),
				currentUpdateTarget == kLoadingMenuUpdate ? 1u : 0u,
				static_cast<UInt32>(currentShowChangesTarget),
				currentShowChangesTarget == kLoadingMenuShowChanges ? 1u : 0u);
			return true;
		}

		if (observedReadable && observedTarget == currentUpdateTarget)
		{
			s_predecessorLoadingMenuUpdate = nullptr;
			s_loadingMenuUpdateHookInstallFailed = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: LoadingMenuThread prewarm hook write did not publish call=0x%08X predecessor=0x%08X",
				static_cast<UInt32>(kLoadingMenuThreadUpdateCallSite),
				static_cast<UInt32>(currentUpdateTarget));
			return false;
		}

		const bool successorExecutable = observedReadable
			&& hook_identity::IsExecutableTarget(observedTarget);
		// A different executable CALL target is not evidence that the loading
		// thread still reaches this consumer. Retain the predecessor pointer for
		// safety, but keep all producer wait paths fail-closed.
		s_loadingMenuUpdateHookInstalled = false;
		s_loadingMenuUpdateHookInstallFailed = true;
		gLog.FormattedMessage(
			"tnvse_native_overlay: LoadingMenuThread prewarm hook may be retained below observed target=0x%08X predecessor=0x%08X readable=%u executable=%u; reachability unverified, consumer disabled",
			static_cast<UInt32>(observedTarget),
			static_cast<UInt32>(currentUpdateTarget),
			observedReadable ? 1u : 0u,
			successorExecutable ? 1u : 0u);
		return false;
	}

	bool EnsureNativeImeOverlayHost()
	{
		Tile* parent = SynchronizeImeParent();
		if (!parent)
			return false;
		return EnsureImeHost(parent);
	}

	bool IsNativeImeOverlayHostReady()
	{
		return s_imeReady.load(std::memory_order_acquire);
	}

	bool IsNativeImeOverlayVisible()
	{
		return IsNativeImeOverlayHostReady()
			&& s_state.imeVisible
			&& s_state.imeRoot
			&& s_state.imeRoot->GetValueFloat(
				Tile::kTileValue_visible) > 0.5f
			&& HasExpectedImeLinePresentation(
				s_state.imeVisibleLineCount);
	}

	UInt32 GetNativeImeOverlayHostGeneration()
	{
		return s_imeHostGeneration.load(std::memory_order_acquire);
	}

	bool IsNativePrewarmOverlayHostReady()
	{
		return s_prewarmReady.load(std::memory_order_acquire);
	}

	void UpdateNativeImeOverlay(
		const std::vector<NativeTileOverlayLine>& lines,
		bool forceTextGeometryRefresh)
	{
		if (!IsNativeImeOverlayHostReady())
			EnsureNativeImeOverlayHost();
		if (!IsNativeImeOverlayHostReady()
			|| s_prewarmActive.load(std::memory_order_acquire))
		{
			HideNativeImeOverlay();
			return;
		}
		if (lines.empty())
		{
			HideNativeImeOverlay();
			return;
		}

		const size_t visibleCount = std::min(lines.size(), kImeLineCount);
		const bool rootVisible =
			s_state.imeRoot->GetValueFloat(
				Tile::kTileValue_visible) > 0.5f;
		const bool linePresentationIntact =
			HasExpectedImeLinePresentation(
				s_state.imeVisibleLineCount);
		const bool presentationNeedsRepair =
			forceTextGeometryRefresh
			|| !s_state.imeVisible
			|| !rootVisible
			|| !linePresentationIntact;

		// Publish text only after its Menu ancestor is visible. Line 00 is the
		// status-only presentation created when a text target first activates;
		// constructing it below the XML-default-hidden root can seal a zero-alpha
		// FreeType shape while later composition/candidate rows are built after
		// the root is visible.
		SynchronizeOverlayDepth(s_state.imeRoot);
		if (!rootVisible)
			SetVisible(s_state.imeRoot, true);
		s_state.imeVisible = true;

		const std::wstring key = BuildImeKey(lines);
		const bool contentChanged = key != s_state.imeKey;
		const bool republish = contentChanged || presentationNeedsRepair;
		if (republish)
		{
			s_state.imeKey = key;
			s_state.imeVisibleLineCount = visibleCount;
			for (Tile* highlight : s_state.imeHighlights)
				SetVisible(highlight, false);
			for (size_t i = 0; i < kImeLineCount; ++i)
			{
				const bool visible = i < visibleCount;
				SetVisible(s_state.imeLines[i], visible);
				if (visible)
				{
					PublishTextGeometry(
						s_state.imeLines[i],
						lines[i].text,
						presentationNeedsRepair);
				}
			}
			if (presentationNeedsRepair && g_bMultibyteInputLog)
			{
				gLog.FormattedMessage(
					"tnvse_native_overlay: repaired IME text presentation host=%p lines=%u caller_force=%u root_visible=%u line_state_valid=%u",
					s_state.imeRoot,
					static_cast<UInt32>(visibleCount),
					forceTextGeometryRefresh ? 1u : 0u,
					rootVisible ? 1u : 0u,
					linePresentationIntact ? 1u : 0u);
			}
		}

		float lineHeight = kImeMinimumLineHeight;
		float contentWidth = kImeMinimumWidth;
		std::array<ImeTextVisualBounds, kImeLineCount> visualBounds = {};
		for (size_t i = 0; i < visibleCount; ++i)
		{
			const float measuredHeight =
				s_state.imeLines[i]->GetValueFloat(
					Tile::kTileValue_height);
			if (std::isfinite(measuredHeight) && measuredHeight > 0.0f)
				lineHeight = std::max(lineHeight, measuredHeight);

			const std::string encoded = WideToUiText(lines[i].text);
			visualBounds[i] =
				MeasureImeTextVisualBounds(encoded, measuredHeight);
			const float estimatedWidth =
				static_cast<float>(encoded.size()) * 12.0f
				+ kImeHorizontalPadding;
			contentWidth = std::max(contentWidth, estimatedWidth);
			const float measuredWidth =
				s_state.imeLines[i]->GetValueFloat(
					Tile::kTileValue_width);
			if (std::isfinite(measuredWidth) && measuredWidth > 0.0f)
			{
				contentWidth = std::max(
					contentWidth,
					measuredWidth + kImeHorizontalPadding);
			}
		}
		contentWidth = std::clamp(
			contentWidth, kImeMinimumWidth, kImeMaximumWidth);
		const bool metricsChanged =
			std::fabs(lineHeight - s_state.imeLineHeight) > 0.25f;
		const bool widthChanged =
			std::fabs(contentWidth - s_state.imeContentWidth) > 0.25f;
		if (republish || metricsChanged || widthChanged)
		{
			s_state.imeLineHeight = lineHeight;
			s_state.imeContentWidth = contentWidth;
			const float left =
				(kImeMaximumWidth - contentWidth) * 0.5f;
			const float lineStride = lineHeight + kImeLineGap;
			const float height = kImeVerticalPadding * 2.0f
				+ static_cast<float>(visibleCount) * lineHeight
				+ static_cast<float>(
					visibleCount > 0 ? visibleCount - 1 : 0)
					* kImeLineGap;
			s_state.imeRoot->SetValueFloat(
				Tile::kTileValue_height, height, true);
			s_state.imeBackground->SetValueFloat(
				Tile::kTileValue_x, left, true);
			s_state.imeBackground->SetValueFloat(
				Tile::kTileValue_height, height, true);
			s_state.imeBackground->SetValueFloat(
				Tile::kTileValue_width, contentWidth, true);
			for (size_t i = 0; i < visibleCount; ++i)
			{
				const float lineY = kImeVerticalPadding
					+ static_cast<float>(i) * lineStride;
				s_state.imeLines[i]->SetValueFloat(
					Tile::kTileValue_x, left + 10.0f, true);
				s_state.imeLines[i]->SetValueFloat(
					Tile::kTileValue_user0,
					lineY,
					true);
				s_state.imeLines[i]->SetValueFloat(
					Tile::kTileValue_user1,
					lineHeight,
					true);
				const float measuredHeight =
					s_state.imeLines[i]->GetValueFloat(
						Tile::kTileValue_height);
				s_state.imeLines[i]->SetValueFloat(
					Tile::kTileValue_user2,
					visualBounds[i].valid
						? visualBounds[i].top : 0.0f,
					true);
				s_state.imeLines[i]->SetValueFloat(
					Tile::kTileValue_user3,
					visualBounds[i].valid
						? visualBounds[i].height
						: std::max(0.0f, measuredHeight),
					true);
				if (g_bEnableFreeTypeFontRenderingLog
					&& s_imeVisualBoundsLogCount
						< kImeVisualBoundsLogLimit)
				{
					++s_imeVisualBoundsLogCount;
					gLog.FormattedMessage(
						"tnvse_native_overlay: IME visual center line=%u rowTop=%.2f rowHeight=%.2f tileHeight=%.2f glyphTop=%.2f glyphHeight=%.2f xmlOffset=%.2f valid=%d",
						static_cast<UInt32>(i),
						lineY,
						lineHeight,
						measuredHeight,
						visualBounds[i].valid
							? visualBounds[i].top : 0.0f,
						visualBounds[i].valid
							? visualBounds[i].height
							: std::max(0.0f, measuredHeight),
						visualBounds[i].valid
							? (lineHeight - visualBounds[i].height)
								* 0.5f - visualBounds[i].top
							: (lineHeight
								- std::max(0.0f, measuredHeight))
								* 0.5f,
						visualBounds[i].valid ? 1 : 0);
				}
				s_state.imeLines[i]->SetValueFloat(
					Tile::kTileValue_wrapwidth,
					std::max(1.0f,
						contentWidth - kImeHorizontalPadding),
					true);
			}
			size_t highlightIndex = 0;
			for (size_t i = 0; i < visibleCount; ++i)
			{
				if (!lines[i].highlighted
					|| highlightIndex >= kImeHighlightCount)
				{
					continue;
				}
				Tile* highlight =
					s_state.imeHighlights[highlightIndex++];
				highlight->SetValueFloat(
					Tile::kTileValue_y,
					kImeVerticalPadding
						+ static_cast<float>(i) * lineStride,
					true);
				highlight->SetValueFloat(
					Tile::kTileValue_height, lineHeight, true);
				SetVisible(highlight, true);
			}
			for (Tile* highlight : s_state.imeHighlights)
			{
				highlight->SetValueFloat(
					Tile::kTileValue_x, left + 8.0f, true);
				highlight->SetValueFloat(
					Tile::kTileValue_width,
					std::max(1.0f, contentWidth - 16.0f),
					true);
			}
		}

	}

	void HideNativeImeOverlay()
	{
		Tile* parent = SynchronizeImeParent();
		const bool attached =
			IsNamedDirectChild(parent, s_state.imeRoot, "tNVSE_IME");
		if (s_state.imeVisible
			&& IsNativeImeOverlayHostReady()
			&& attached)
			SetVisible(s_state.imeRoot, false);
		if (s_state.imeRoot && !attached)
			ClearImeResolvedTiles();
		s_state.imeVisible = false;
		s_state.imeKey.clear();
		s_state.imeVisibleLineCount = 0;
	}

	void ShowNativePrewarmOverlay()
	{
		PublishPrewarmOverlayVisibility(true);
	}

	void UpdateNativePrewarmOverlay(
		std::wstring_view detail,
		std::wstring_view stage,
		float progress)
	{
		PublishPrewarmOverlayUpdate(detail, stage, progress);
	}

	bool RefreshNativePrewarmOverlayTextGeometry(UInt32 timeoutMs)
	{
		const UInt32 sequence = PublishPrewarmOverlayRefresh();
		if (!timeoutMs
			|| !HasVerifiedLoadingMenuUpdateHook()
			|| (!s_prewarmConsumerThreadId.load(std::memory_order_acquire)
				&& !s_prewarmReady.load(std::memory_order_acquire)))
		{
			return true;
		}

		const ULONGLONG deadline = GetTickCount64() + timeoutMs;
		while (s_prewarmConsumedSequence.load(std::memory_order_acquire)
			!= sequence)
		{
			if (GetTickCount64() >= deadline)
			{
				gLog.FormattedMessage(
					"tnvse_native_overlay: timed out synchronizing prewarm text refresh sequence=%u consumed=%u timeoutMs=%u policy=retain-referenced-retired-atlas",
					sequence,
					s_prewarmConsumedSequence.load(
						std::memory_order_acquire),
					timeoutMs);
				return false;
			}
			Sleep(1);
		}
		return true;
	}

	void HideNativePrewarmOverlay()
	{
		PublishPrewarmOverlayVisibility(false);
	}

	bool QuiesceNativePrewarmOverlay(UInt32 timeoutMs)
	{
		const UInt32 sequence = PublishPrewarmOverlayVisibility(false);
		if (!HasVerifiedLoadingMenuUpdateHook()
			|| (!s_prewarmConsumerThreadId.load(std::memory_order_acquire)
				&& !s_prewarmReady.load(std::memory_order_acquire)))
		{
			return true;
		}

		const ULONGLONG deadline = GetTickCount64() + timeoutMs;
		while (s_prewarmConsumedSequence.load(std::memory_order_acquire)
			!= sequence)
		{
			if (GetTickCount64() >= deadline)
			{
				gLog.FormattedMessage(
					"tnvse_native_overlay: timed out quiescing prewarm progress sequence=%u consumed=%u timeoutMs=%u policy=skip-atlas-publication",
					sequence,
					s_prewarmConsumedSequence.load(
						std::memory_order_acquire),
					timeoutMs);
				return false;
			}
			Sleep(1);
		}
		return true;
	}

	bool IsNativePrewarmOverlayActive()
	{
		return s_prewarmActive.load(std::memory_order_acquire);
	}

	void ShutdownNativeTileOverlayHost()
	{
		// Stop publishing either Tile tree before destruction. The IME tree is
		// owned by the game thread and can be released here. The prewarm tree is
		// owned by LoadingMenuThread; leave its attached child to LoadingMenu's
		// normal teardown and only forget our non-owning pointers.
		s_imeReady.store(false, std::memory_order_release);
		s_prewarmReady.store(false, std::memory_order_release);
		s_prewarmActive.store(false, std::memory_order_release);
		InterfaceManager* manager = InterfaceManager::GetSingleton();
		Tile* currentImeParent =
			manager ? manager->pMenuRoot : nullptr;
		if (currentImeParent
			&& currentImeParent == s_state.imeParent)
		{
			ReleaseAndDestroyAttachedRoot(
				currentImeParent,
				s_state.imeRoot,
				"tNVSE_IME");
		}
		ResetImeForParent(nullptr);
		ResetPrewarmForParent(nullptr);
	}
}

#include "native_tile_overlay_detail.h"

namespace fonthook
{
	namespace implementation::native_tile_overlay {}
	using namespace implementation::native_tile_overlay;

	namespace implementation::native_tile_overlay
	{
		void AdvanceImeHostGeneration()
		{
			const UInt32 previous = OverlayRuntime().imeHostGeneration.fetch_add(
				1,
				std::memory_order_acq_rel);
			if (previous == std::numeric_limits<UInt32>::max())
				OverlayRuntime().imeHostGeneration.store(1, std::memory_order_release);
		}
		void ConsumeNativePrewarmOverlayCommand();

		bool IsLoadingMenuThreadPauseOrShutdownRequested() noexcept
		{
			const UInt8* loadingThread =
				*reinterpret_cast<UInt8**>(kLoadingMenuThread_pMe);
			if (!loadingThread)
				return false;
			return *reinterpret_cast<const volatile UInt8*>(loadingThread
					+ kLoadingMenuThreadPauseRequestedOffset) != 0
				|| *reinterpret_cast<const volatile UInt8*>(loadingThread
					+ kLoadingMenuThreadShutdownOffset) != 0;
		}

		bool IsLoadingMenuVanillaUpdateBlockedAtStartup() noexcept
		{
			return *reinterpret_cast<volatile UInt8*>(
				kLoadingMenuStartupFlag) != 0
				&& *reinterpret_cast<void* volatile*>(
					kInterfaceManager_pMe) == nullptr;
		}

		class ScopedRendererTryLock final
		{
		public:
			explicit ScopedRendererTryLock(NiRenderer* renderer) noexcept
				: m_renderer(renderer)
			{
				if (m_renderer)
				{
					m_locked = TryEnterCriticalSection(
						&m_renderer->m_kRendererLock.m_kCriticalSection) != FALSE;
				}
			}

			~ScopedRendererTryLock() noexcept
			{
				if (m_locked)
				{
					LeaveCriticalSection(
						&m_renderer->m_kRendererLock.m_kCriticalSection);
				}
			}

			explicit operator bool() const noexcept { return m_locked; }

		private:
			NiRenderer* m_renderer = nullptr;
			bool m_locked = false;
		};

		void __fastcall LoadingMenuUpdateHook(void* loadingMenu, void*)
		{
			// ThreadProc cannot acknowledge a pause request until this hook returns.
			// Never enter a known unbounded vanilla wait or optional overlay work once
			// the main thread has requested that acknowledgement.
			if (IsLoadingMenuThreadPauseOrShutdownRequested()
				|| IsLoadingMenuVanillaUpdateBlockedAtStartup())
			{
				if (IsPrewarmOverlayOwnerShutdownRequested())
					ConsumeNativePrewarmOverlayCommand();
				return;
			}
			if (OverlayRuntime().predecessorLoadingMenuUpdate)
				OverlayRuntime().predecessorLoadingMenuUpdate(loadingMenu);
			if (IsPrewarmOverlayOwnerShutdownRequested())
			{
				ConsumeNativePrewarmOverlayCommand();
				return;
			}
			if (IsLoadingMenuThreadPauseOrShutdownRequested())
				return;
			ConsumeNativePrewarmOverlayCommand();
		}

		void __cdecl LoadingMenuShowChangesHook()
		{
			if (IsLoadingMenuThreadPauseOrShutdownRequested())
				return;
			LoadingMenuShowChangesFn predecessor =
				OverlayRuntime().predecessorLoadingMenuShowChanges;
			if (!predecessor)
				return;

			NiRenderer* renderer = NiRenderer::GetSingleton();
			ScopedRendererTryLock rendererLock(renderer);
			if (!rendererLock || IsLoadingMenuThreadPauseOrShutdownRequested())
				return;

			// ShowChanges creates every dirty LoadingMenu TileText node while holding
			// renderer/UI locks. Keep all tNVSE geometry on the synchronous,
			// non-retained route for this complete traversal. The renderer lock is a
			// recursive CRITICAL_SECTION, so vanilla and NVTF's StopProcessing hook
			// retain their original nested lock/unlock and pause-event pairing.
			ScopedFreeTypeNoPrecacheRoute noPrecacheRoute;
			predecessor();
		}

		bool IsVerifiedLoadingMenuCallHook(SIZE_T callSite,
			SIZE_T adapterTarget, SIZE_T predecessorTarget)
		{
			SIZE_T currentTarget = 0;
			return hook_identity::ReadRel32Target(
					callSite, hook_identity::Rel32Opcode::Call, currentTarget)
				&& currentTarget == adapterTarget
				&& predecessorTarget != adapterTarget
				&& hook_identity::IsExecutableTarget(predecessorTarget);
		}

		bool HasVerifiedLoadingMenuUpdateHook()
		{
			if (!OverlayRuntime().loadingMenuUpdateHookInstalled
				|| !OverlayRuntime().loadingMenuShowChangesHookInstalled)
				return false;

			return IsVerifiedLoadingMenuCallHook(
					kLoadingMenuThreadUpdateCallSite,
					reinterpret_cast<SIZE_T>(&LoadingMenuUpdateHook),
					reinterpret_cast<SIZE_T>(
						OverlayRuntime().predecessorLoadingMenuUpdate))
				&& IsVerifiedLoadingMenuCallHook(
					kLoadingMenuThreadShowChangesCallSite,
					reinterpret_cast<SIZE_T>(&LoadingMenuShowChangesHook),
					reinterpret_cast<SIZE_T>(
						OverlayRuntime().predecessorLoadingMenuShowChanges));
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
				OverlayRuntime().imeMenuVtable.data() + 1;
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
			if (menuClass == kImeMenuClass && OverlayRuntime().creatingImeMenu)
				return CreateImeMenu();
			return OverlayRuntime().predecessorCreateMenuByClass
				? OverlayRuntime().predecessorCreateMenuByClass(factory, menuClass)
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
			if (OverlayRuntime().imeReady.load(std::memory_order_acquire)
				&& OverlayRuntime().state.imeVisible
				&& OverlayRuntime().state.imeRoot)
			{
				imeNode = OverlayRuntime().state.imeRoot->spNiNode;
				if (imeNode)
				{
					wasAppCulled = imeNode->GetAppCulled();
					if (!wasAppCulled)
						imeNode->SetAppCulled(true);
				}
			}

			if (OverlayRuntime().predecessorPipboyDraw)
			{
				OverlayRuntime().predecessorPipboyDraw(renderedMenu, currentTexture,
					clearMode, alternateTexture);
			}

			if (imeNode && !wasAppCulled)
			{
				imeNode->SetAppCulled(false);
				if (!OverlayRuntime().loggedPipboyRttExclusion)
				{
					OverlayRuntime().loggedPipboyRttExclusion = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: excluded IME Menu node=%p from Pip-Boy rendered-menu RTT; screen-space pass remains enabled",
						imeNode);
				}
			}
		}

		bool EnsurePipboyDrawExclusionHook()
		{
			if (OverlayRuntime().pipboyDrawHookInstalled)
			{
				if (!hook_identity::IsAccessibleRegion(
					kFOPipboyManagerDrawVTableEntry, sizeof(SIZE_T), false))
				{
					OverlayRuntime().pipboyDrawHookInstalled = false;
					OverlayRuntime().pipboyDrawHookInstallFailed = true;
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
					OverlayRuntime().predecessorPipboyDraw);
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
					OverlayRuntime().pipboyDrawHookInstalled = false;
					OverlayRuntime().predecessorPipboyDraw = nullptr;
				}
				else
				{
					// A different owner may have captured this hook. Keep the saved
					// predecessor callable, but do not claim verified reachability.
					OverlayRuntime().pipboyDrawHookInstalled = false;
					OverlayRuntime().pipboyDrawHookInstallFailed = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: Pip-Boy RTT exclusion capability revoked; observed handler=0x%08X adapter=0x%08X predecessor=0x%08X",
						static_cast<UInt32>(currentTarget),
						static_cast<UInt32>(adapterTarget),
						static_cast<UInt32>(predecessorTarget));
					return false;
				}
			}
			if (OverlayRuntime().pipboyDrawHookInstallFailed)
				return false;

			if (!hook_identity::IsAccessibleRegion(
				kFOPipboyManagerDrawVTableEntry, sizeof(SIZE_T), false))
			{
				OverlayRuntime().pipboyDrawHookInstallFailed = true;
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
					OverlayRuntime().predecessorPipboyDraw);
				OverlayRuntime().pipboyDrawHookInstalled =
					predecessorTarget != currentTarget
					&& hook_identity::IsExecutableTarget(predecessorTarget);
				if (!OverlayRuntime().pipboyDrawHookInstalled)
				{
					OverlayRuntime().pipboyDrawHookInstallFailed = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: Pip-Boy RTT exclusion hook is present but its predecessor is unavailable predecessor=0x%08X",
						static_cast<UInt32>(predecessorTarget));
				}
				return OverlayRuntime().pipboyDrawHookInstalled;
			}
			if (!hook_identity::IsExecutableTarget(currentTarget))
			{
				OverlayRuntime().pipboyDrawHookInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: cannot install Pip-Boy RTT exclusion hook; non-executable FORenderedMenu::Draw target=0x%08X entry=0x%08X",
					static_cast<UInt32>(currentTarget),
					static_cast<UInt32>(kFOPipboyManagerDrawVTableEntry));
				return false;
			}

			const RenderedMenuDrawFn previousPredecessor =
				OverlayRuntime().predecessorPipboyDraw;
			OverlayRuntime().predecessorPipboyDraw =
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
				OverlayRuntime().predecessorPipboyDraw = previousPredecessor;
				OverlayRuntime().pipboyDrawHookInstallFailed = true;
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
				OverlayRuntime().pipboyDrawHookInstalled = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: installed Pip-Boy RTT exclusion hook chainedTarget=0x%08X vanilla=%d",
					static_cast<UInt32>(currentTarget),
					currentTarget == kFORenderedMenuDraw ? 1 : 0);
				return true;
			}

			if (observedTarget == currentTarget)
			{
				OverlayRuntime().predecessorPipboyDraw = previousPredecessor;
				OverlayRuntime().pipboyDrawHookInstallFailed = true;
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
			OverlayRuntime().pipboyDrawHookInstalled = false;
			OverlayRuntime().pipboyDrawHookInstallFailed = true;
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
			if (OverlayRuntime().imeMenuFactoryInstalled)
			{
				SIZE_T currentTarget = 0;
				const bool targetReadable = hook_identity::ReadRel32Target(
					kCreateMenuByClassCallSite,
					hook_identity::Rel32Opcode::Call,
					currentTarget);
				const SIZE_T adapterTarget = reinterpret_cast<SIZE_T>(
					&CreateMenuByClassHook);
				const SIZE_T predecessorTarget = reinterpret_cast<SIZE_T>(
					OverlayRuntime().predecessorCreateMenuByClass);
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
					OverlayRuntime().imeMenuFactoryInstalled = false;
					OverlayRuntime().predecessorCreateMenuByClass = nullptr;
				}
				else
				{
					OverlayRuntime().imeMenuFactoryInstalled = false;
					OverlayRuntime().imeMenuFactoryInstallFailed = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: IME Menu factory capability revoked; observed target=0x%08X adapter=0x%08X predecessor=0x%08X readable=%u",
						static_cast<UInt32>(currentTarget),
						static_cast<UInt32>(adapterTarget),
						static_cast<UInt32>(predecessorTarget),
						targetReadable ? 1u : 0u);
					return false;
				}
			}
			if (OverlayRuntime().imeMenuFactoryInstallFailed)
				return false;

			SIZE_T currentTarget = 0;
			if (!hook_identity::ReadRel32Target(
				kCreateMenuByClassCallSite,
				hook_identity::Rel32Opcode::Call,
				currentTarget))
			{
				OverlayRuntime().imeMenuFactoryInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: cannot install IME Menu factory hook; expected CALL at 0x%08X",
					static_cast<UInt32>(kCreateMenuByClassCallSite));
				return false;
			}

			if (currentTarget == reinterpret_cast<SIZE_T>(
					&CreateMenuByClassHook))
			{
				const SIZE_T predecessorTarget = reinterpret_cast<SIZE_T>(
					OverlayRuntime().predecessorCreateMenuByClass);
				OverlayRuntime().imeMenuFactoryInstalled =
					predecessorTarget != currentTarget
					&& hook_identity::IsExecutableTarget(predecessorTarget);
				if (!OverlayRuntime().imeMenuFactoryInstalled)
				{
					OverlayRuntime().imeMenuFactoryInstallFailed = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: IME Menu factory hook is present but its predecessor is unavailable predecessor=0x%08X",
						static_cast<UInt32>(predecessorTarget));
				}
				return OverlayRuntime().imeMenuFactoryInstalled;
			}
			if (!hook_identity::IsExecutableTarget(currentTarget)
				|| !hook_identity::IsAccessibleRegion(
					kMenuVTable - sizeof(SIZE_T),
					(kMenuVTableEntryCount + 1) * sizeof(SIZE_T),
					false))
			{
				OverlayRuntime().imeMenuFactoryInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: cannot install IME Menu factory hook; invalid CreateMenuByClass target=0x%08X or Menu vtable",
					static_cast<UInt32>(currentTarget));
				return false;
			}

			const SIZE_T* vanillaVtable =
				reinterpret_cast<const SIZE_T*>(kMenuVTable);
			OverlayRuntime().imeMenuVtable.front() = vanillaVtable[-1];
			std::copy_n(
				vanillaVtable,
				kMenuVTableEntryCount,
				OverlayRuntime().imeMenuVtable.begin() + 1);
			OverlayRuntime().imeMenuVtable[kMenuGetIdVtableIndex + 1] =
				reinterpret_cast<SIZE_T>(&ImeMenuGetId);
			OverlayRuntime().predecessorCreateMenuByClass =
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
				OverlayRuntime().imeMenuFactoryInstalled = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: installed dedicated IME Menu factory class=%u chainedTarget=0x%08X",
					kImeMenuClass,
					static_cast<UInt32>(currentTarget));
				return true;
			}

			if (observedReadable && observedTarget == currentTarget)
			{
				OverlayRuntime().predecessorCreateMenuByClass = nullptr;
				OverlayRuntime().imeMenuFactoryInstallFailed = true;
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
			OverlayRuntime().imeMenuFactoryInstalled = false;
			OverlayRuntime().imeMenuFactoryInstallFailed = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: IME Menu factory hook may be retained below observed target=0x%08X predecessor=0x%08X readable=%u executable=%u; reachability unverified, feature disabled",
				static_cast<UInt32>(observedTarget),
				static_cast<UInt32>(currentTarget),
				observedReadable ? 1u : 0u,
				successorExecutable ? 1u : 0u);
			return false;
		}

	}

	bool InstallNativePrewarmOverlayLoadingThreadHook()
	{
		NativeTileOverlayRuntimeState& runtime = OverlayRuntime();
		if (runtime.loadingMenuUpdateHookInstalled
			|| runtime.loadingMenuShowChangesHookInstalled)
		{
			if (HasVerifiedLoadingMenuUpdateHook())
				return true;

			SIZE_T currentUpdateTarget = 0;
			SIZE_T currentShowChangesTarget = 0;
			const bool updateReadable = hook_identity::ReadRel32Target(
				kLoadingMenuThreadUpdateCallSite,
				hook_identity::Rel32Opcode::Call,
				currentUpdateTarget);
			const bool showChangesReadable = hook_identity::ReadRel32Target(
				kLoadingMenuThreadShowChangesCallSite,
				hook_identity::Rel32Opcode::Call,
				currentShowChangesTarget);
			const SIZE_T updateAdapterTarget = reinterpret_cast<SIZE_T>(
				&LoadingMenuUpdateHook);
			const SIZE_T showChangesAdapterTarget = reinterpret_cast<SIZE_T>(
				&LoadingMenuShowChangesHook);
			const SIZE_T updatePredecessorTarget = reinterpret_cast<SIZE_T>(
				runtime.predecessorLoadingMenuUpdate);
			const SIZE_T showChangesPredecessorTarget = reinterpret_cast<SIZE_T>(
				runtime.predecessorLoadingMenuShowChanges);
			const bool cleanlyRestored = updateReadable
				&& showChangesReadable
				&& currentUpdateTarget == updatePredecessorTarget
				&& currentShowChangesTarget == showChangesPredecessorTarget
				&& hook_identity::IsExecutableTarget(currentUpdateTarget)
				&& hook_identity::IsExecutableTarget(currentShowChangesTarget);
			if (cleanlyRestored)
			{
				runtime.loadingMenuUpdateHookInstalled = false;
				runtime.loadingMenuShowChangesHookInstalled = false;
				runtime.predecessorLoadingMenuUpdate = nullptr;
				runtime.predecessorLoadingMenuShowChanges = nullptr;
			}
			else
			{
				runtime.loadingMenuUpdateHookInstalled = false;
				runtime.loadingMenuShowChangesHookInstalled = false;
				runtime.loadingMenuUpdateHookInstallFailed = true;
				runtime.prewarmConsumerDisabled.store(true, std::memory_order_release);
				runtime.prewarmReady.store(false, std::memory_order_release);
				runtime.prewarmActive.store(false, std::memory_order_release);
				gLog.FormattedMessage(
					"tnvse_native_overlay: LoadingMenuThread safety capability revoked; updateObserved=0x%08X updateAdapter=0x%08X updatePredecessor=0x%08X updateReadable=%u showObserved=0x%08X showAdapter=0x%08X showPredecessor=0x%08X showReadable=%u",
					static_cast<UInt32>(currentUpdateTarget),
					static_cast<UInt32>(updateAdapterTarget),
					static_cast<UInt32>(updatePredecessorTarget),
					updateReadable ? 1u : 0u,
					static_cast<UInt32>(currentShowChangesTarget),
					static_cast<UInt32>(showChangesAdapterTarget),
					static_cast<UInt32>(showChangesPredecessorTarget),
					showChangesReadable ? 1u : 0u);
				return false;
			}
		}
		if (runtime.loadingMenuUpdateHookInstallFailed)
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
			runtime.loadingMenuUpdateHookInstallFailed = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: cannot install LoadingMenuThread safety hooks; executable identity mismatch updateCall=0x%08X showChangesCall=0x%08X",
				static_cast<UInt32>(kLoadingMenuThreadUpdateCallSite),
				static_cast<UInt32>(kLoadingMenuThreadShowChangesCallSite));
			return false;
		}

		const SIZE_T updateAdapterTarget = reinterpret_cast<SIZE_T>(
			&LoadingMenuUpdateHook);
		const SIZE_T showChangesAdapterTarget = reinterpret_cast<SIZE_T>(
			&LoadingMenuShowChangesHook);
		if (currentUpdateTarget == updateAdapterTarget
			|| currentShowChangesTarget == showChangesAdapterTarget)
		{
			runtime.loadingMenuUpdateHookInstallFailed = true;
			runtime.prewarmConsumerDisabled.store(true, std::memory_order_release);
			gLog.FormattedMessage(
				"tnvse_native_overlay: LoadingMenuThread safety adapter is already present without recoverable predecessor updateTarget=0x%08X showChangesTarget=0x%08X",
				static_cast<UInt32>(currentUpdateTarget),
				static_cast<UInt32>(currentShowChangesTarget));
			return false;
		}

		// Install ShowChanges first. The prewarm consumer is not reachable until
		// every LoadingMenu geometry rebuild is already inside the safe route.
		runtime.predecessorLoadingMenuShowChanges =
			reinterpret_cast<LoadingMenuShowChangesFn>(currentShowChangesTarget);
		WriteRelCall(kLoadingMenuThreadShowChangesCallSite,
			&LoadingMenuShowChangesHook);
		SIZE_T observedShowChangesTarget = 0;
		const bool observedShowChangesReadable = hook_identity::ReadRel32Target(
			kLoadingMenuThreadShowChangesCallSite,
			hook_identity::Rel32Opcode::Call,
			observedShowChangesTarget);
		if (!observedShowChangesReadable
			|| observedShowChangesTarget != showChangesAdapterTarget)
		{
			if (observedShowChangesReadable
				&& observedShowChangesTarget == currentShowChangesTarget)
			{
				runtime.predecessorLoadingMenuShowChanges = nullptr;
			}
			runtime.loadingMenuUpdateHookInstallFailed = true;
			runtime.prewarmConsumerDisabled.store(true, std::memory_order_release);
			gLog.FormattedMessage(
				"tnvse_native_overlay: LoadingMenuThread ShowChanges safety hook publication failed observed=0x%08X predecessor=0x%08X readable=%u",
				static_cast<UInt32>(observedShowChangesTarget),
				static_cast<UInt32>(currentShowChangesTarget),
				observedShowChangesReadable ? 1u : 0u);
			return false;
		}
		runtime.loadingMenuShowChangesHookInstalled = true;

		runtime.predecessorLoadingMenuUpdate =
			reinterpret_cast<LoadingMenuUpdateFn>(currentUpdateTarget);
		// LoadingMenuThread -> LoadingMenu::Update
		// (__thiscall target via __fastcall shim).
		WriteRelCall(kLoadingMenuThreadUpdateCallSite, &LoadingMenuUpdateHook);
		SIZE_T observedUpdateTarget = 0;
		const bool observedUpdateReadable = hook_identity::ReadRel32Target(
			kLoadingMenuThreadUpdateCallSite,
			hook_identity::Rel32Opcode::Call,
			observedUpdateTarget);
		if (observedUpdateReadable
			&& observedUpdateTarget == updateAdapterTarget)
		{
			runtime.loadingMenuUpdateHookInstalled = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: installed LoadingMenuThread safety hooks updateCall=0x%08X updateTarget=0x%08X vanillaUpdate=%u showChangesCall=0x%08X showChangesTarget=0x%08X vanillaShowChanges=%u policy=pause-aware-startup-bounded rendererLock=try-enter fontRoute=loading-menu-no-precache-no-retained-command",
				static_cast<UInt32>(kLoadingMenuThreadUpdateCallSite),
				static_cast<UInt32>(currentUpdateTarget),
				currentUpdateTarget == kLoadingMenuUpdate ? 1u : 0u,
				static_cast<UInt32>(kLoadingMenuThreadShowChangesCallSite),
				static_cast<UInt32>(currentShowChangesTarget),
				currentShowChangesTarget == kLoadingMenuShowChanges ? 1u : 0u);
			return true;
		}

		if (observedUpdateReadable
			&& observedUpdateTarget == currentUpdateTarget)
		{
			runtime.predecessorLoadingMenuUpdate = nullptr;
		}
		runtime.loadingMenuUpdateHookInstalled = false;
		runtime.loadingMenuUpdateHookInstallFailed = true;
		runtime.prewarmConsumerDisabled.store(true, std::memory_order_release);
		runtime.prewarmReady.store(false, std::memory_order_release);
		runtime.prewarmActive.store(false, std::memory_order_release);
		gLog.FormattedMessage(
			"tnvse_native_overlay: LoadingMenuThread Update safety hook publication failed observed=0x%08X predecessor=0x%08X readable=%u; ShowChanges safety hook remains installed, prewarm consumer disabled",
			static_cast<UInt32>(observedUpdateTarget),
			static_cast<UInt32>(currentUpdateTarget),
			observedUpdateReadable ? 1u : 0u);
		return false;
	}

}

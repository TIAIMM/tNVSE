#include "native_tile_overlay_detail.h"

#include <cstring>

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

		constexpr ULONGLONG kLoadingMenuVerboseSlowThresholdMs = 100;
		constexpr ULONGLONG kLoadingMenuForcedSlowThresholdMs = 1000;
		constexpr ULONGLONG kLoadingMenuSlowLogIntervalMs = 2000;

		struct LoadingMenuEngineSnapshot
		{
			void* loadingMenu = nullptr;
			UInt8* loadingThread = nullptr;
			void* interfaceManager = nullptr;
			UInt8 pauseRequested = 0;
			UInt8 shutdownRequested = 0;
		};

		ULONGLONG LoadingMenuAgeMs(
			ULONGLONG now, ULONGLONG timestamp) noexcept
		{
			return timestamp && now >= timestamp ? now - timestamp : 0;
		}

		UInt32 LoadingMenuDurationUs(
			ULONGLONG startedAt, ULONGLONG completedAt) noexcept
		{
			const ULONGLONG durationMs = LoadingMenuAgeMs(completedAt, startedAt);
			return static_cast<UInt32>(std::min<ULONGLONG>(
				durationMs * 1000ull,
				std::numeric_limits<UInt32>::max()));
		}

		bool ClaimLoadingMenuLogInterval(
			std::atomic<ULONGLONG>& timestamp,
			ULONGLONG now,
			ULONGLONG intervalMs) noexcept
		{
			ULONGLONG previous = timestamp.load(std::memory_order_acquire);
			for (;;)
			{
				if (previous && LoadingMenuAgeMs(now, previous) < intervalMs)
					return false;
				if (timestamp.compare_exchange_weak(
						previous, now, std::memory_order_acq_rel,
						std::memory_order_acquire))
				{
					return true;
				}
			}
		}

		LoadingMenuEngineSnapshot CaptureLoadingMenuEngineSnapshot() noexcept
		{
			LoadingMenuEngineSnapshot snapshot;
			snapshot.loadingMenu = *reinterpret_cast<void* volatile*>(
				kLoadingMenu_pMe);
			snapshot.loadingThread = *reinterpret_cast<UInt8* volatile*>(
				kLoadingMenuThread_pMe);
			snapshot.interfaceManager = *reinterpret_cast<void* volatile*>(
				kInterfaceManager_pMe);
			if (snapshot.loadingThread
				&& hook_identity::IsAccessibleRegion(
					reinterpret_cast<SIZE_T>(snapshot.loadingThread),
					kLoadingMenuThreadShutdownOffset + 1u,
					false))
			{
				snapshot.pauseRequested = *reinterpret_cast<volatile UInt8*>(
					snapshot.loadingThread
						+ kLoadingMenuThreadPauseRequestedOffset);
				snapshot.shutdownRequested = *reinterpret_cast<volatile UInt8*>(
					snapshot.loadingThread
						+ kLoadingMenuThreadShutdownOffset);
			}
			return snapshot;
		}

		const char* LoadingMenuDiagnosticPhaseName(
			LoadingMenuDiagnosticPhase phase) noexcept
		{
			return phase == LoadingMenuDiagnosticPhase::LoadingTextMakeNode
				? "loading-text-makenode" : "idle";
		}

		UInt32 HashLoadingMenuTileName(const char* value) noexcept
		{
			UInt32 hash = 2166136261u;
			if (!value)
				return hash;
			for (const unsigned char* current =
				reinterpret_cast<const unsigned char*>(value);
				*current;
				++current)
			{
				hash ^= *current;
				hash *= 16777619u;
			}
			return hash;
		}

		void SetLoadingMenuDiagnosticPhase(
			LoadingMenuDiagnosticPhase phase,
			ULONGLONG now = GetTickCount64()) noexcept
		{
			LoadingMenuDiagnosticState& diagnostics =
				OverlayRuntime().loadingMenuDiagnostics;
			diagnostics.phaseEnteredAt.store(now, std::memory_order_release);
			diagnostics.phase.store(phase, std::memory_order_release);
		}

		void LogLoadingMenuDiagnosticSnapshot(const char* event)
		{
			NativeTileOverlayRuntimeState& runtime = OverlayRuntime();
			LoadingMenuDiagnosticState& diagnostics =
				runtime.loadingMenuDiagnostics;
			const ULONGLONG now = GetTickCount64();
			const LoadingMenuEngineSnapshot engine =
				CaptureLoadingMenuEngineSnapshot();
			const PrewarmOverlayCommand command =
				runtime.prewarmMailbox.ReadLatest();
			const UInt32 published =
				runtime.prewarmMailbox.PublishedSequence();
			const UInt32 consumed =
				runtime.prewarmMailbox.ConsumedSequence();

			gLog.FormattedMessage(
				"tnvse_loading_menu_diag: event=%s trace=%llu thread=%u phase=%s phaseAgeMs=%llu menu=%p root=%p loadingThread=%p interface=%p pause=%u shutdown=%u text={calls:%llu,inFlight:%u,tile:%p,nameHash:%08X,fontBits:%08X,lastUs:%u,produced:%u} prewarm={published:%u,consumed:%u,pending:%u,run:%llu,command:%u,action:%u,presentation:graphical-only,progress:%.3f,requested:%u}",
				event ? event : "unspecified",
				static_cast<unsigned long long>(diagnostics.traceId.load(
					std::memory_order_acquire)),
				GetCurrentThreadId(),
				LoadingMenuDiagnosticPhaseName(diagnostics.phase.load(
					std::memory_order_acquire)),
				static_cast<unsigned long long>(LoadingMenuAgeMs(
					now, diagnostics.phaseEnteredAt.load(std::memory_order_acquire))),
				engine.loadingMenu,
				reinterpret_cast<void*>(diagnostics.observedLoadingMenuRoot.load(
					std::memory_order_acquire)),
				engine.loadingThread,
				engine.interfaceManager,
				static_cast<UInt32>(engine.pauseRequested),
				static_cast<UInt32>(engine.shutdownRequested),
				static_cast<unsigned long long>(
					diagnostics.loadingTextMakeNodeCalls.load(
						std::memory_order_acquire)),
				diagnostics.loadingTextMakeNodeInFlight.load(
					std::memory_order_acquire),
				reinterpret_cast<void*>(diagnostics.lastLoadingTextTile.load(
					std::memory_order_acquire)),
				diagnostics.lastLoadingTextTileNameHash.load(
					std::memory_order_acquire),
				diagnostics.lastLoadingTextFontTraitBits.load(
					std::memory_order_acquire),
				diagnostics.lastLoadingTextMakeNodeDurationUs.load(
					std::memory_order_acquire),
				diagnostics.lastLoadingTextProducedNode.load(
					std::memory_order_acquire),
				published, consumed, published != consumed ? 1u : 0u,
				static_cast<unsigned long long>(command.runToken),
				command.sequence, static_cast<UInt32>(command.action),
				command.progress,
				runtime.prewarmMailbox.IsPresentationRequested() ? 1u : 0u);
		}

		bool IsLoadingMenuThreadPauseOrShutdownRequested() noexcept
		{
			const UInt8* loadingThread =
				*reinterpret_cast<UInt8**>(kLoadingMenuThread_pMe);
			if (!loadingThread
				|| !hook_identity::IsAccessibleRegion(
					reinterpret_cast<SIZE_T>(loadingThread),
					kLoadingMenuThreadShutdownOffset + 1u,
					false))
			{
				return false;
			}
			return *reinterpret_cast<const volatile UInt8*>(loadingThread
					+ kLoadingMenuThreadPauseRequestedOffset) != 0
				|| *reinterpret_cast<const volatile UInt8*>(loadingThread
					+ kLoadingMenuThreadShutdownOffset) != 0;
		}

		void __fastcall LoadingMenuUpdateHook(void* loadingMenu, void*)
		{
			// Preserve the complete predecessor chain and vanilla behavior first.
			// ThreadProc invokes vanilla presentation immediately after this call.
			LoadingMenuUpdateFn predecessor =
				OverlayRuntime().predecessorLoadingMenuUpdate;
			if (predecessor)
				predecessor(loadingMenu);
			else
				return;

			PrewarmOverlayMailbox& mailbox = OverlayRuntime().prewarmMailbox;
			if (!mailbox.HasPending())
				return;
			if (IsLoadingMenuThreadPauseOrShutdownRequested())
				return;

			ConsumeNativePrewarmOverlayCommand(
				static_cast<Menu*>(loadingMenu));
		}

		bool IsVerifiedLoadingMenuUpdateHook()
		{
			SIZE_T currentTarget = 0;
			const SIZE_T predecessorTarget = reinterpret_cast<SIZE_T>(
				OverlayRuntime().predecessorLoadingMenuUpdate);
			return OverlayRuntime().loadingMenuUpdateHookInstalled
				&& hook_identity::ReadRel32Target(
					kLoadingMenuThreadUpdateCallSite,
					hook_identity::Rel32Opcode::Call,
					currentTarget)
				&& currentTarget == reinterpret_cast<SIZE_T>(
					&LoadingMenuUpdateHook)
				&& predecessorTarget != currentTarget
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

	void LogNativeLoadingMenuDiagnostic(const char* event)
	{
		LogLoadingMenuDiagnosticSnapshot(event);
	}

	void PumpNativeLoadingMenuDiagnostics()
	{
		// The prewarm Update hook has no heartbeat or per-frame diagnostics.
		// LoadingMenu text diagnostics are completed synchronously by MakeNode.
	}

	void BeginNativeLoadingMenuTextGeometryDiagnostic(
		const void* tile, const char* tileName, float fontTrait)
	{
		LoadingMenuDiagnosticState& diagnostics =
			OverlayRuntime().loadingMenuDiagnostics;
		const ULONGLONG now = GetTickCount64();
		UInt32 fontTraitBits = 0;
		static_assert(sizeof(fontTraitBits) == sizeof(fontTrait));
		std::memcpy(&fontTraitBits, &fontTrait, sizeof(fontTraitBits));
		const UInt32 nameHash = HashLoadingMenuTileName(tileName);
		Menu* loadingMenu = *reinterpret_cast<Menu**>(kLoadingMenu_pMe);
		diagnostics.observedLoadingMenu.store(
			reinterpret_cast<SIZE_T>(loadingMenu), std::memory_order_release);
		diagnostics.observedLoadingMenuRoot.store(
			loadingMenu ? reinterpret_cast<SIZE_T>(loadingMenu->pRootTile) : 0,
			std::memory_order_release);
		const UInt64 call = diagnostics.loadingTextMakeNodeCalls.fetch_add(
			1, std::memory_order_relaxed) + 1;
		UInt64 trace = diagnostics.traceId.load(std::memory_order_acquire);
		if (!trace)
		{
			diagnostics.traceId.compare_exchange_strong(
				trace, 1, std::memory_order_acq_rel);
			trace = diagnostics.traceId.load(std::memory_order_acquire);
			diagnostics.traceStartedAt.store(now, std::memory_order_release);
		}
		diagnostics.loadingTextMakeNodeInFlight.store(
			1, std::memory_order_release);
		diagnostics.lastLoadingTextMakeNodeEnterAt.store(
			now, std::memory_order_release);
		diagnostics.lastLoadingTextTile.store(
			reinterpret_cast<SIZE_T>(tile), std::memory_order_release);
		diagnostics.lastLoadingTextTileNameHash.store(
			nameHash, std::memory_order_release);
		diagnostics.lastLoadingTextFontTraitBits.store(
			fontTraitBits, std::memory_order_release);
		diagnostics.lastLoadingTextProducedNode.store(
			0, std::memory_order_release);
		diagnostics.lastActivityAt.store(now, std::memory_order_release);
		SetLoadingMenuDiagnosticPhase(
			LoadingMenuDiagnosticPhase::LoadingTextMakeNode, now);

		if (call == 1)
		{
			gLog.FormattedMessage(
				"tnvse_loading_menu_diag: event=loading-text-makenode-begin trace=%llu thread=%u call=%llu tile=%p name='%s' nameHash=%08X fontTrait=%.3f fontBits=%08X route=freetype-no-precache",
				static_cast<unsigned long long>(trace),
				GetCurrentThreadId(),
				static_cast<unsigned long long>(call),
				tile, tileName ? tileName : "", nameHash,
				fontTrait, fontTraitBits);
		}
	}

	void EndNativeLoadingMenuTextGeometryDiagnostic(
		const void* tile, bool producedNode)
	{
		LoadingMenuDiagnosticState& diagnostics =
			OverlayRuntime().loadingMenuDiagnostics;
		const ULONGLONG now = GetTickCount64();
		const ULONGLONG enteredAt =
			diagnostics.lastLoadingTextMakeNodeEnterAt.load(
				std::memory_order_acquire);
		const UInt32 durationUs = LoadingMenuDurationUs(enteredAt, now);
		diagnostics.lastLoadingTextMakeNodeDurationUs.store(
			durationUs, std::memory_order_release);
		diagnostics.lastLoadingTextMakeNodeExitAt.store(
			now, std::memory_order_release);
		diagnostics.lastLoadingTextProducedNode.store(
			producedNode ? 1u : 0u, std::memory_order_release);
		diagnostics.lastActivityAt.store(now, std::memory_order_release);
		diagnostics.loadingTextMakeNodeInFlight.store(
			0, std::memory_order_release);
		SetLoadingMenuDiagnosticPhase(
			LoadingMenuDiagnosticPhase::Idle, now);

		const UInt64 call = diagnostics.loadingTextMakeNodeCalls.load(
			std::memory_order_acquire);
		if (call == 1)
			LogLoadingMenuDiagnosticSnapshot(
				"loading-text-makenode-complete");

		const ULONGLONG durationMs = LoadingMenuAgeMs(now, enteredAt);
		static std::atomic<ULONGLONG> lastSlowLogAt{ 0 };
		if ((durationMs >= kLoadingMenuForcedSlowThresholdMs
				|| (g_bEnableFreeTypeFontRenderingLog
					&& durationMs >= kLoadingMenuVerboseSlowThresholdMs))
			&& (durationMs >= kLoadingMenuForcedSlowThresholdMs
				|| ClaimLoadingMenuLogInterval(
					lastSlowLogAt, now, kLoadingMenuSlowLogIntervalMs)))
		{
			gLog.FormattedMessage(
				"tnvse_loading_menu_diag: event=slow-call:loading-text-makenode trace=%llu thread=%u tile=%p nameHash=%08X fontBits=%08X durationUs=%u phase=complete",
				static_cast<unsigned long long>(diagnostics.traceId.load(
					std::memory_order_acquire)),
				GetCurrentThreadId(), tile,
				diagnostics.lastLoadingTextTileNameHash.load(
					std::memory_order_acquire),
				diagnostics.lastLoadingTextFontTraitBits.load(
					std::memory_order_acquire),
				durationUs);
		}
	}

	bool InstallNativePrewarmOverlayLoadingMenuUpdateHook()
	{
		NativeTileOverlayRuntimeState& runtime = OverlayRuntime();
		if (runtime.loadingMenuUpdateHookInstalled)
		{
			if (IsVerifiedLoadingMenuUpdateHook())
				return true;

			SIZE_T observedTarget = 0;
			const bool readable = hook_identity::ReadRel32Target(
				kLoadingMenuThreadUpdateCallSite,
				hook_identity::Rel32Opcode::Call,
				observedTarget);
			runtime.loadingMenuUpdateHookInstalled = false;
			runtime.loadingMenuUpdateHookInstallFailed = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: LoadingMenu Update hook successor changed after installation observed=0x%08X readable=%u policy=no-reassert-prewarm-ui-optional",
				static_cast<UInt32>(observedTarget), readable ? 1u : 0u);
			return false;
		}
		if (runtime.loadingMenuUpdateHookInstallFailed)
			return false;

		SIZE_T currentTarget = 0;
		const SIZE_T instanceLoad =
			kLoadingMenuThreadUpdateCallSite
				- kExpectedLoadingMenuInstanceLoadInstruction.size();
		const SIZE_T adapterTarget =
			reinterpret_cast<SIZE_T>(&LoadingMenuUpdateHook);
		if (!hook_identity::IsAccessibleRegion(
				instanceLoad,
				kExpectedLoadingMenuInstanceLoadInstruction.size() + 5u,
				true)
			|| !hook_identity::MatchesBytesUnchecked(
				instanceLoad,
				kExpectedLoadingMenuInstanceLoadInstruction.data(),
				kExpectedLoadingMenuInstanceLoadInstruction.size())
			|| !hook_identity::ReadRel32Target(
				kLoadingMenuThreadUpdateCallSite,
				hook_identity::Rel32Opcode::Call,
				currentTarget)
			|| !hook_identity::IsExecutableTarget(currentTarget)
			|| currentTarget == adapterTarget)
		{
			runtime.loadingMenuUpdateHookInstallFailed = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: cannot install optional LoadingMenu Update hook call=0x%08X target=0x%08X executable=%u adapterAlreadyPresent=%u policy=font-prewarm-continues-without-presentation",
				static_cast<UInt32>(kLoadingMenuThreadUpdateCallSite),
				static_cast<UInt32>(currentTarget),
				hook_identity::IsExecutableTarget(currentTarget) ? 1u : 0u,
				currentTarget == adapterTarget ? 1u : 0u);
			return false;
		}

		runtime.predecessorLoadingMenuUpdate =
			reinterpret_cast<LoadingMenuUpdateFn>(currentTarget);
		WriteRelCall(kLoadingMenuThreadUpdateCallSite, &LoadingMenuUpdateHook);

		SIZE_T observedTarget = 0;
		const bool observedReadable = hook_identity::ReadRel32Target(
			kLoadingMenuThreadUpdateCallSite,
			hook_identity::Rel32Opcode::Call,
			observedTarget);
		if (observedReadable && observedTarget == adapterTarget)
		{
			runtime.loadingMenuUpdateHookInstalled = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: installed single chained LoadingMenu Update hook call=0x%08X predecessor=0x%08X vanilla=%u presentation=graphical-only idlePath=atomic-sequence-compare originalPresentation=unmodified",
				static_cast<UInt32>(kLoadingMenuThreadUpdateCallSite),
				static_cast<UInt32>(currentTarget),
				currentTarget == kLoadingMenuUpdate ? 1u : 0u);
			return true;
		}

		if (observedReadable && observedTarget == currentTarget)
			runtime.predecessorLoadingMenuUpdate = nullptr;
		runtime.loadingMenuUpdateHookInstalled = false;
		runtime.loadingMenuUpdateHookInstallFailed = true;
		gLog.FormattedMessage(
			"tnvse_native_overlay: LoadingMenu Update hook publication failed observed=0x%08X predecessor=0x%08X readable=%u policy=no-retry-font-prewarm-continues",
			static_cast<UInt32>(observedTarget),
			static_cast<UInt32>(currentTarget),
			observedReadable ? 1u : 0u);
		return false;
	}

}

#include "native_tile_overlay_detail.h"

namespace fonthook
{
	namespace implementation::native_tile_overlay {}
	using namespace implementation::native_tile_overlay;

	namespace implementation::native_tile_overlay
	{
		void ClearPrewarmResolvedTiles()
		{
			OverlayRuntime().prewarmReady.store(false, std::memory_order_release);
			OverlayRuntime().state.prewarmRoot = nullptr;
			OverlayRuntime().state.prewarmShade = nullptr;
			OverlayRuntime().state.prewarmPanel = nullptr;
			OverlayRuntime().state.prewarmLabel = nullptr;
			OverlayRuntime().state.prewarmTrack = nullptr;
			OverlayRuntime().state.prewarmFill = nullptr;
			OverlayRuntime().state.prewarmLayoutSignature.fill(0.0f);
			OverlayRuntime().state.prewarmProgressWidth = 520.0f;
			OverlayRuntime().state.prewarmTileVisible = false;
		}


		void ResetPrewarmForParent(Tile* parent)
		{
			// A changed LoadingMenu root means the engine may already have
			// released the old Tile tree. Never dereference the stale pointers.
			ClearPrewarmResolvedTiles();
			OverlayRuntime().state.prewarmParent = parent;
			OverlayRuntime().state.prewarmLoadFailed = false;
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
			if (OverlayRuntime().state.prewarmParent != parent)
				ResetPrewarmForParent(parent);
			return parent;
		}


		bool ResolvePrewarmTiles(Tile* root)
		{
			if (!root || _stricmp(root->strName.c_str(), "tNVSE_Prewarm"))
				return false;

			Tile* shade =
				FindDirectChild(root, "tNVSE_Prewarm_Shade");
			Tile* panel =
				FindDirectChild(root, "tNVSE_Prewarm_Panel");
			Tile* label =
				FindDirectChild(root, "tNVSE_Prewarm_Label");
			Tile* track =
				FindDirectChild(root, "tNVSE_Prewarm_Track");
			Tile* fill =
				FindDirectChild(root, "tNVSE_Prewarm_Fill");
			if (!shade || !panel || !label || !track || !fill)
				return false;

			OverlayRuntime().state.prewarmRoot = root;
			OverlayRuntime().state.prewarmShade = shade;
			OverlayRuntime().state.prewarmPanel = panel;
			OverlayRuntime().state.prewarmLabel = label;
			OverlayRuntime().state.prewarmTrack = track;
			OverlayRuntime().state.prewarmFill = fill;
			return true;
		}


		bool IsResolvedPrewarmTreeAttached(Tile* parent)
		{
			if (!IsNamedDirectChild(
					parent, OverlayRuntime().state.prewarmRoot, "tNVSE_Prewarm"))
			{
				return false;
			}
			const std::array<Tile*, 5> required = {
				OverlayRuntime().state.prewarmShade,
				OverlayRuntime().state.prewarmPanel,
				OverlayRuntime().state.prewarmLabel,
				OverlayRuntime().state.prewarmTrack,
				OverlayRuntime().state.prewarmFill,
			};
			const std::array<const char*, 5> names = {
				"tNVSE_Prewarm_Shade",
				"tNVSE_Prewarm_Panel",
				"tNVSE_Prewarm_Label",
				"tNVSE_Prewarm_Track",
				"tNVSE_Prewarm_Fill",
			};
			for (size_t i = 0; i < required.size(); ++i)
			{
				if (!IsNamedDirectChild(
						OverlayRuntime().state.prewarmRoot, required[i], names[i]))
					return false;
			}
			return true;
		}


		void ResetPrewarmPresentationState()
		{
			OverlayRuntime().state.prewarmLayoutSignature.fill(0.0f);
			OverlayRuntime().state.prewarmProgressWidth = 520.0f;
			OverlayRuntime().state.prewarmTileVisible = false;
		}


		bool EnsurePrewarmHost(Tile* parent)
		{
			if (OverlayRuntime().state.prewarmRoot)
			{
				Tile* root = OverlayRuntime().state.prewarmRoot;
				if (IsResolvedPrewarmTreeAttached(parent))
				{
					const bool tileVisible =
						root->GetValueFloat(Tile::kTileValue_visible) > 0.5f;
					if (OverlayRuntime().state.prewarmTileVisible && !tileVisible)
						ResetPrewarmPresentationState();
					else if (!OverlayRuntime().state.prewarmTileVisible && tileVisible)
						SetVisible(root, false);
					OverlayRuntime().prewarmReady.store(true, std::memory_order_release);
					return true;
				}
				if (IsDirectChild(parent, root) && ResolvePrewarmTiles(root))
				{
					ResetPrewarmPresentationState();
					SetVisible(root, false);
					OverlayRuntime().prewarmReady.store(true, std::memory_order_release);
					gLog.FormattedMessage(
						"tnvse_native_overlay: rebound prewarm Tile component after child-tree replacement host=%p parent=%p",
						root, parent);
					return true;
				}
				ClearPrewarmResolvedTiles();
				OverlayRuntime().state.prewarmLoadFailed = false;
				ReleaseAndDestroyAttachedRoot(
					parent, root, "tNVSE_Prewarm");
				gLog.FormattedMessage(
					"tnvse_native_overlay: prewarm Tile component was detached or malformed; reloading component");
			}
			if (OverlayRuntime().state.prewarmLoadFailed)
				return false;

			Tile* root = ThisStdCall<Tile*>(
				kTileReadFile, parent, kPrewarmOverlayXmlPath);
			if (!root || !IsDirectChild(parent, root)
				|| !ResolvePrewarmTiles(root))
			{
				ClearPrewarmResolvedTiles();
				ReleaseAndDestroyAttachedRoot(
					parent, root, "tNVSE_Prewarm");
				OverlayRuntime().state.prewarmLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: failed to load or resolve prewarm component path='%s'; font prewarm continues without Tile progress UI",
					kPrewarmOverlayXmlPath);
				return false;
			}

			SetVisible(OverlayRuntime().state.prewarmRoot, false);
			OverlayRuntime().prewarmReady.store(true, std::memory_order_release);
			gLog.FormattedMessage(
				"tnvse_native_overlay: loaded prewarm Tile component path='%s' host=%p parent=%p",
				kPrewarmOverlayXmlPath, OverlayRuntime().state.prewarmRoot, parent);
			return true;
		}


		void LayoutPrewarmOverlay()
		{
			if (!OverlayRuntime().state.prewarmRoot
				|| !OverlayRuntime().state.prewarmPanel
				|| !OverlayRuntime().state.prewarmLabel
				|| !OverlayRuntime().state.prewarmTrack
				|| !OverlayRuntime().state.prewarmFill)
			{
				return;
			}
			const float rootWidth =
				OverlayRuntime().state.prewarmRoot->GetValueFloat(
					Tile::kTileValue_width);
			const float rootHeight =
				OverlayRuntime().state.prewarmRoot->GetValueFloat(
					Tile::kTileValue_height);
			if (!std::isfinite(rootWidth)
				|| !std::isfinite(rootHeight)
				|| rootWidth <= 0.0f
				|| rootHeight <= 0.0f)
			{
				return;
			}

			const std::array<float, 2> signature = {
				rootWidth,
				rootHeight,
			};
			bool changed = false;
			for (size_t i = 0; i < signature.size(); ++i)
			{
				if (std::fabs(
						signature[i]
							- OverlayRuntime().state.prewarmLayoutSignature[i])
					> 0.25f)
				{
					changed = true;
					break;
				}
			}
			if (!changed)
				return;
			OverlayRuntime().state.prewarmLayoutSignature = signature;

			const float panelWidth = std::min(
				kPrewarmMaximumPanelWidth,
				std::max(
					kPrewarmMinimumPanelWidth,
					rootWidth - 24.0f));
			const float panelX = std::max(
				12.0f, (rootWidth - panelWidth) * 0.5f);
			const float panelY = std::max(
				12.0f, (rootHeight - kPrewarmPanelHeight) * 0.5f);
			const float labelWidth = std::min(
				kPrewarmMaximumLabelWidth,
				std::max(1.0f, panelWidth - 60.0f));
			const float labelHeight = labelWidth / kPrewarmLabelAspectRatio;
			const float labelX = panelX + (panelWidth - labelWidth) * 0.5f;
			const float labelY = panelY + kPrewarmLabelTopPadding;
			const float trackX = panelX + kPrewarmHorizontalPadding;
			const float trackY = panelY + kPrewarmTrackOffsetY;
			const float progressWidth =
				std::max(220.0f,
					panelWidth - 2.0f * kPrewarmHorizontalPadding);

			OverlayRuntime().state.prewarmPanel->SetValueFloat(
				Tile::kTileValue_x, panelX, true);
			OverlayRuntime().state.prewarmPanel->SetValueFloat(
				Tile::kTileValue_y, panelY, true);
			OverlayRuntime().state.prewarmPanel->SetValueFloat(
				Tile::kTileValue_width, panelWidth, true);
			OverlayRuntime().state.prewarmPanel->SetValueFloat(
				Tile::kTileValue_height, kPrewarmPanelHeight, true);
			OverlayRuntime().state.prewarmLabel->SetValueFloat(
				Tile::kTileValue_x, labelX, true);
			OverlayRuntime().state.prewarmLabel->SetValueFloat(
				Tile::kTileValue_y, labelY, true);
			OverlayRuntime().state.prewarmLabel->SetValueFloat(
				Tile::kTileValue_width, labelWidth, true);
			OverlayRuntime().state.prewarmLabel->SetValueFloat(
				Tile::kTileValue_height, labelHeight, true);
			OverlayRuntime().state.prewarmTrack->SetValueFloat(
				Tile::kTileValue_x, trackX, true);
			OverlayRuntime().state.prewarmTrack->SetValueFloat(
				Tile::kTileValue_y, trackY, true);
			OverlayRuntime().state.prewarmTrack->SetValueFloat(
				Tile::kTileValue_width, progressWidth, true);
			OverlayRuntime().state.prewarmFill->SetValueFloat(
				Tile::kTileValue_x, trackX, true);
			OverlayRuntime().state.prewarmFill->SetValueFloat(
				Tile::kTileValue_y, trackY, true);
			OverlayRuntime().state.prewarmProgressWidth = progressWidth;
		}

		UInt32 NextPrewarmCommandSequenceLocked()
		{
			++OverlayRuntime().nextPrewarmCommandSequence;
			if (!OverlayRuntime().nextPrewarmCommandSequence)
				++OverlayRuntime().nextPrewarmCommandSequence;
			return OverlayRuntime().nextPrewarmCommandSequence;
		}

		void RecordPrewarmOverlayCommandPublished(UInt32 sequence)
		{
			if (!sequence)
				return;
			LoadingMenuDiagnosticState& diagnostics =
				OverlayRuntime().loadingMenuDiagnostics;
			diagnostics.commandsPublished.fetch_add(
				1, std::memory_order_relaxed);
			diagnostics.lastCommandPublishedAt.store(
				GetTickCount64(), std::memory_order_release);
		}

		void RecordPrewarmOverlayCommandConsumeAttempt(UInt32 sequence)
		{
			LoadingMenuDiagnosticState& diagnostics =
				OverlayRuntime().loadingMenuDiagnostics;
			diagnostics.commandConsumeAttempts.fetch_add(
				1, std::memory_order_relaxed);
			diagnostics.lastCommandAttemptSequence.store(
				sequence, std::memory_order_release);
			diagnostics.lastCommandConsumeAttemptAt.store(
				GetTickCount64(), std::memory_order_release);
		}

		void CompletePrewarmOverlayCommand(
			UInt32 sequence, const char* diagnosticEvent = nullptr)
		{
			OverlayRuntime().prewarmConsumedSequence.store(
				sequence, std::memory_order_release);
			LoadingMenuDiagnosticState& diagnostics =
				OverlayRuntime().loadingMenuDiagnostics;
			diagnostics.commandsConsumed.fetch_add(
				1, std::memory_order_relaxed);
			diagnostics.lastCommandConsumedSequence.store(
				sequence, std::memory_order_release);
			diagnostics.lastCommandConsumedAt.store(
				GetTickCount64(), std::memory_order_release);
			if (diagnosticEvent)
				LogNativeLoadingMenuDiagnostic(diagnosticEvent);
		}

		UInt32 PublishPrewarmOverlayUpdate(float progress)
		{
			AcquireSRWLockExclusive(&OverlayRuntime().prewarmCommandLock);
			const UInt32 shutdownSequence =
				OverlayRuntime().prewarmOwnerShutdown.RequestedSequence();
			if (shutdownSequence)
			{
				ReleaseSRWLockExclusive(&OverlayRuntime().prewarmCommandLock);
				return shutdownSequence;
			}
			const UInt32 sequence = NextPrewarmCommandSequenceLocked();
			OverlayRuntime().prewarmCommand.progress = std::clamp(progress, 0.0f, 1.0f);
			OverlayRuntime().prewarmCommand.visible = true;
			OverlayRuntime().prewarmCommand.ownerShutdown = false;
			OverlayRuntime().prewarmCommand.sequence = sequence;
			ReleaseSRWLockExclusive(&OverlayRuntime().prewarmCommandLock);
			OverlayRuntime().prewarmActive.store(
				!OverlayRuntime().prewarmConsumerDisabled.load(std::memory_order_acquire),
				std::memory_order_release);
			OverlayRuntime().prewarmPublishedSequence.store(
				sequence, std::memory_order_release);
			RecordPrewarmOverlayCommandPublished(sequence);
			return sequence;
		}

		UInt32 PublishPrewarmOverlayVisibility(bool visible)
		{
			AcquireSRWLockExclusive(&OverlayRuntime().prewarmCommandLock);
			const UInt32 shutdownSequence =
				OverlayRuntime().prewarmOwnerShutdown.RequestedSequence();
			if (shutdownSequence)
			{
				ReleaseSRWLockExclusive(&OverlayRuntime().prewarmCommandLock);
				return shutdownSequence;
			}
			const UInt32 sequence = NextPrewarmCommandSequenceLocked();
			OverlayRuntime().prewarmCommand.visible = visible;
			OverlayRuntime().prewarmCommand.ownerShutdown = false;
			OverlayRuntime().prewarmCommand.sequence = sequence;
			ReleaseSRWLockExclusive(&OverlayRuntime().prewarmCommandLock);
			OverlayRuntime().prewarmActive.store(
				visible
					&& !OverlayRuntime().prewarmConsumerDisabled.load(
						std::memory_order_acquire),
				std::memory_order_release);
			OverlayRuntime().prewarmPublishedSequence.store(
				sequence, std::memory_order_release);
			RecordPrewarmOverlayCommandPublished(sequence);
			LogNativeLoadingMenuDiagnostic(visible
				? "prewarm-command-published:show"
				: "prewarm-command-published:hide");
			return sequence;
		}

		UInt32 PublishPrewarmOverlayOwnerShutdown()
		{
			bool publishedNewCommand = false;
			AcquireSRWLockExclusive(&OverlayRuntime().prewarmCommandLock);
			UInt32 sequence =
				OverlayRuntime().prewarmOwnerShutdown.RequestedSequence();
			if (!sequence)
			{
				sequence = NextPrewarmCommandSequenceLocked();
				OverlayRuntime().prewarmCommand.visible = false;
				OverlayRuntime().prewarmCommand.ownerShutdown = true;
				OverlayRuntime().prewarmCommand.sequence = sequence;
				OverlayRuntime().prewarmOwnerShutdown.Request(sequence);
				OverlayRuntime().prewarmPublishedSequence.store(
					sequence, std::memory_order_release);
				publishedNewCommand = true;
			}
			ReleaseSRWLockExclusive(&OverlayRuntime().prewarmCommandLock);
			OverlayRuntime().prewarmActive.store(false, std::memory_order_release);
			OverlayRuntime().prewarmConsumerDisabled.store(
				true, std::memory_order_release);
			if (publishedNewCommand)
				RecordPrewarmOverlayCommandPublished(sequence);
			LogNativeLoadingMenuDiagnostic(
				"prewarm-command-published:owner-shutdown");
			return sequence;
		}

		bool IsPrewarmOverlayOwnerShutdownRequested()
		{
			return OverlayRuntime().prewarmOwnerShutdown.RequestedSequence() != 0;
		}

		PrewarmOverlayCommand ReadLatestPrewarmOverlayCommand()
		{
			PrewarmOverlayCommand command;
			AcquireSRWLockShared(&OverlayRuntime().prewarmCommandLock);
			command = OverlayRuntime().prewarmCommand;
			ReleaseSRWLockShared(&OverlayRuntime().prewarmCommandLock);
			return command;
		}

		void ApplyPrewarmOverlayHidden()
		{
			Tile* parent = SynchronizePrewarmParent();
			Tile* root = OverlayRuntime().state.prewarmRoot;
			const bool hadOverlay = root
				|| OverlayRuntime().state.prewarmTileVisible;
			const bool attached = IsNamedDirectChild(
				parent, root, "tNVSE_Prewarm");
			if (OverlayRuntime().state.prewarmTileVisible
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
			if (hadOverlay)
				LogNativeLoadingMenuDiagnostic("prewarm-overlay-hidden-detached");
		}

		bool ApplyPrewarmOverlayVisible(
			const PrewarmOverlayCommand& command)
		{
			Tile* parent = SynchronizePrewarmParent();
			if (!parent)
			{
				if (!OverlayRuntime().state.prewarmParentUnavailableLogged)
				{
					OverlayRuntime().state.prewarmParentUnavailableLogged = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: LoadingMenu root not ready; retaining queued prewarm progress until owner thread can attach it");
					LogNativeLoadingMenuDiagnostic(
						"prewarm-consume-wait:loadingmenu-root-unavailable");
				}
				return false;
			}
			OverlayRuntime().state.prewarmParentUnavailableLogged = false;
			if (!EnsurePrewarmHost(parent))
			{
				if (OverlayRuntime().state.prewarmLoadFailed)
				{
					OverlayRuntime().prewarmConsumerDisabled.store(
						true, std::memory_order_release);
					OverlayRuntime().prewarmActive.store(false, std::memory_order_release);
					LogNativeLoadingMenuDiagnostic(
						"prewarm-host-load-failed:consumer-disabled");
				}
				return true;
			}

			if (!OverlayRuntime().state.prewarmTileVisible)
			{
				SetVisible(OverlayRuntime().state.prewarmRoot, true);
				OverlayRuntime().state.prewarmTileVisible = true;
				LogNativeLoadingMenuDiagnostic("prewarm-overlay-visible");
			}

			LayoutPrewarmOverlay();
			OverlayRuntime().state.prewarmFill->SetValueFloat(
				Tile::kTileValue_width,
				OverlayRuntime().state.prewarmProgressWidth
					* std::clamp(command.progress, 0.0f, 1.0f),
				true);
			return true;
		}

		void ConsumeNativePrewarmOverlayCommand()
		{
			struct ScopedOwnerWork final
			{
				ScopedOwnerWork()
				{
					OverlayRuntime().prewarmOwnerShutdown.EnterOwnerWork();
				}
				~ScopedOwnerWork()
				{
					OverlayRuntime().prewarmOwnerShutdown.LeaveOwnerWork();
				}
			} ownerWork;

			DWORD expectedThread = 0;
			const DWORD currentThread = GetCurrentThreadId();
			if (OverlayRuntime().prewarmConsumerThreadId.compare_exchange_strong(
				expectedThread, currentThread, std::memory_order_acq_rel))
			{
				gLog.FormattedMessage(
					"tnvse_native_overlay: prewarm progress consumer active thread=%u owner=LoadingMenuThread policy=queued-snapshot presentation=graphical-only",
					currentThread);
			}

			const UInt32 published = OverlayRuntime().prewarmPublishedSequence.load(
				std::memory_order_acquire);
			if (!published || published == OverlayRuntime().prewarmConsumedSequence.load(
					std::memory_order_acquire))
			{
				return;
			}

			const PrewarmOverlayCommand command =
				ReadLatestPrewarmOverlayCommand();
			if (!command.sequence)
				return;
			RecordPrewarmOverlayCommandConsumeAttempt(command.sequence);

			if (command.ownerShutdown)
			{
				bool detached = true;
				try
				{
					ApplyPrewarmOverlayHidden();
				}
				catch (const std::exception& error)
				{
					detached = false;
					gLog.FormattedMessage(
						"tnvse_native_overlay: owner-thread prewarm shutdown detach failed reason=%s policy=forget-non-owning-pointers",
						error.what());
				}
				catch (...)
				{
					detached = false;
					gLog.FormattedMessage(
						"tnvse_native_overlay: owner-thread prewarm shutdown detach failed reason=unknown policy=forget-non-owning-pointers");
				}
				// This is the only thread allowed to clear LoadingMenu-owned raw
				// pointers.  If detaching failed, the child remains owned by the
				// engine and its normal LoadingMenu teardown reclaims it.
				ResetPrewarmForParent(nullptr);
				OverlayRuntime().prewarmReady.store(false, std::memory_order_release);
				OverlayRuntime().prewarmActive.store(false, std::memory_order_release);
				OverlayRuntime().prewarmConsumerDisabled.store(
					true, std::memory_order_release);
				CompletePrewarmOverlayCommand(
					command.sequence,
					"prewarm-command-consumed:owner-shutdown");
				OverlayRuntime().prewarmOwnerShutdown.Acknowledge(command.sequence);
				gLog.FormattedMessage(
					"tnvse_native_overlay: owner-thread prewarm shutdown acknowledged sequence=%u thread=%u detached=%u",
					command.sequence, currentThread, detached ? 1u : 0u);
				return;
			}
			if (OverlayRuntime().prewarmConsumerDisabled.load(std::memory_order_acquire))
			{
				OverlayRuntime().prewarmActive.store(false, std::memory_order_release);
				bool expected = false;
				const bool firstDisabledConsumption =
					OverlayRuntime().loadingMenuDiagnostics
						.loggedPrewarmConsumerDisabledConsumption
						.compare_exchange_strong(
							expected, true, std::memory_order_acq_rel);
				CompletePrewarmOverlayCommand(
					command.sequence,
					firstDisabledConsumption
						? "prewarm-command-consumed:consumer-disabled"
						: nullptr);
				return;
			}
			try
			{
				// Cell Offset Generator keeps generation on workers and limits its
				// foreground path to LoadingMenu progress presentation. Every Tile
				// mutation remains confined to LoadingMenuThread, while the component
				// itself contains only TileImage nodes and never enters a font route.
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
				OverlayRuntime().prewarmConsumerDisabled.store(true, std::memory_order_release);
				OverlayRuntime().prewarmReady.store(false, std::memory_order_release);
				OverlayRuntime().prewarmActive.store(false, std::memory_order_release);
				OverlayRuntime().state.prewarmLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: prewarm progress consumer disabled reason=%s policy=font-prewarm-continues",
					error.what());
				LogNativeLoadingMenuDiagnostic(
					"prewarm-consumer-exception:standard");
			}
			catch (...)
			{
				OverlayRuntime().prewarmConsumerDisabled.store(true, std::memory_order_release);
				OverlayRuntime().prewarmReady.store(false, std::memory_order_release);
				OverlayRuntime().prewarmActive.store(false, std::memory_order_release);
				OverlayRuntime().state.prewarmLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: prewarm progress consumer disabled reason=unknown policy=font-prewarm-continues");
				LogNativeLoadingMenuDiagnostic(
					"prewarm-consumer-exception:unknown");
			}
			CompletePrewarmOverlayCommand(command.sequence);
		}
	}
}

#include "native_tile_overlay_detail.h"

#include "game_hooks.h"

namespace fonthook
{
	namespace implementation::native_tile_overlay {}
	using namespace implementation::native_tile_overlay;

	namespace implementation::native_tile_overlay
	{
		float ReadTextHeight(Tile* tile)
		{
			const float height = tile
				? tile->GetValueFloat(Tile::kTileValue_height)
				: 0.0f;
			return std::isfinite(height) && height > 0.0f
				? std::max(kPrewarmMinimumTextHeight, height)
				: kPrewarmMinimumTextHeight;
		}

		void ClearPrewarmResolvedTiles()
		{
			OverlayRuntime().prewarmReady.store(false, std::memory_order_release);
			OverlayRuntime().state.prewarmRoot = nullptr;
			OverlayRuntime().state.prewarmShade = nullptr;
			OverlayRuntime().state.prewarmPanel = nullptr;
			OverlayRuntime().state.prewarmTrack = nullptr;
			OverlayRuntime().state.prewarmDetail = nullptr;
			OverlayRuntime().state.prewarmStage = nullptr;
			OverlayRuntime().state.prewarmFill = nullptr;
			OverlayRuntime().state.prewarmPercent = nullptr;
			OverlayRuntime().state.prewarmTitle = nullptr;
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

			OverlayRuntime().state.prewarmRoot = root;
			OverlayRuntime().state.prewarmShade = shade;
			OverlayRuntime().state.prewarmPanel = panel;
			OverlayRuntime().state.prewarmTrack = track;
			OverlayRuntime().state.prewarmDetail = detail;
			OverlayRuntime().state.prewarmStage = stage;
			OverlayRuntime().state.prewarmFill = fill;
			OverlayRuntime().state.prewarmPercent = percent;
			OverlayRuntime().state.prewarmTitle = title;
			return true;
		}


		bool IsResolvedPrewarmTreeAttached(Tile* parent)
		{
			if (!IsNamedDirectChild(
					parent, OverlayRuntime().state.prewarmRoot, "tNVSE_Prewarm"))
			{
				return false;
			}
			const std::array<Tile*, 8> required = {
				OverlayRuntime().state.prewarmShade,
				OverlayRuntime().state.prewarmPanel,
				OverlayRuntime().state.prewarmTrack,
				OverlayRuntime().state.prewarmDetail,
				OverlayRuntime().state.prewarmStage,
				OverlayRuntime().state.prewarmFill,
				OverlayRuntime().state.prewarmPercent,
				OverlayRuntime().state.prewarmTitle,
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
			if (!OverlayRuntime().state.prewarmRoot || !OverlayRuntime().state.prewarmPanel)
				return;
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

			const float titleHeight = ReadTextHeight(
				OverlayRuntime().state.prewarmTitle);
			const float detailHeight = ReadTextHeight(
				OverlayRuntime().state.prewarmDetail);
			const float stageHeight = ReadTextHeight(
				OverlayRuntime().state.prewarmStage);
			const float percentHeight = ReadTextHeight(
				OverlayRuntime().state.prewarmPercent);
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

			OverlayRuntime().state.prewarmPanel->SetValueFloat(
				Tile::kTileValue_x, panelX, true);
			OverlayRuntime().state.prewarmPanel->SetValueFloat(
				Tile::kTileValue_y, panelY, true);
			OverlayRuntime().state.prewarmPanel->SetValueFloat(
				Tile::kTileValue_width, panelWidth, true);
			OverlayRuntime().state.prewarmPanel->SetValueFloat(
				Tile::kTileValue_height, panelHeight, true);
			OverlayRuntime().state.prewarmTitle->SetValueFloat(
				Tile::kTileValue_x, panelX + 30.0f, true);
			OverlayRuntime().state.prewarmTitle->SetValueFloat(
				Tile::kTileValue_y, titleY, true);
			OverlayRuntime().state.prewarmDetail->SetValueFloat(
				Tile::kTileValue_x, panelX + 30.0f, true);
			OverlayRuntime().state.prewarmDetail->SetValueFloat(
				Tile::kTileValue_y, detailY, true);
			OverlayRuntime().state.prewarmDetail->SetValueFloat(
				Tile::kTileValue_wrapwidth,
				std::max(1.0f, panelWidth - 60.0f),
				true);
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
			OverlayRuntime().state.prewarmStage->SetValueFloat(
				Tile::kTileValue_x, trackX, true);
			OverlayRuntime().state.prewarmStage->SetValueFloat(
				Tile::kTileValue_y, stageY, true);
			OverlayRuntime().state.prewarmStage->SetValueFloat(
				Tile::kTileValue_wrapwidth,
				std::max(1.0f, panelWidth - 200.0f),
				true);
			OverlayRuntime().state.prewarmPercent->SetValueFloat(
				Tile::kTileValue_x,
				panelX + panelWidth - 120.0f,
				true);
			OverlayRuntime().state.prewarmPercent->SetValueFloat(
				Tile::kTileValue_y, stageY, true);
			OverlayRuntime().state.prewarmProgressWidth = progressWidth;
		}

		UInt32 NextPrewarmCommandSequenceLocked()
		{
			++OverlayRuntime().nextPrewarmCommandSequence;
			if (!OverlayRuntime().nextPrewarmCommandSequence)
				++OverlayRuntime().nextPrewarmCommandSequence;
			return OverlayRuntime().nextPrewarmCommandSequence;
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
			AcquireSRWLockExclusive(&OverlayRuntime().prewarmCommandLock);
			const UInt32 shutdownSequence =
				OverlayRuntime().prewarmOwnerShutdown.RequestedSequence();
			if (shutdownSequence)
			{
				ReleaseSRWLockExclusive(&OverlayRuntime().prewarmCommandLock);
				return shutdownSequence;
			}
			const UInt32 sequence = NextPrewarmCommandSequenceLocked();
			CopyPrewarmCommandText(OverlayRuntime().prewarmCommand.detail, detail);
			CopyPrewarmCommandText(OverlayRuntime().prewarmCommand.stage, stage);
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
			return sequence;
		}

		UInt32 PublishPrewarmOverlayRefresh()
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
			OverlayRuntime().prewarmCommand.refreshSequence = sequence;
			OverlayRuntime().prewarmCommand.ownerShutdown = false;
			OverlayRuntime().prewarmCommand.sequence = sequence;
			ReleaseSRWLockExclusive(&OverlayRuntime().prewarmCommandLock);
			OverlayRuntime().prewarmPublishedSequence.store(
				sequence, std::memory_order_release);
			return sequence;
		}

		UInt32 PublishPrewarmOverlayOwnerShutdown()
		{
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
			}
			ReleaseSRWLockExclusive(&OverlayRuntime().prewarmCommandLock);
			OverlayRuntime().prewarmActive.store(false, std::memory_order_release);
			OverlayRuntime().prewarmConsumerDisabled.store(
				true, std::memory_order_release);
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
				}
				return true;
			}

			if (!OverlayRuntime().state.prewarmTileVisible)
			{
				SetVisible(OverlayRuntime().state.prewarmRoot, true);
				OverlayRuntime().state.prewarmTileVisible = true;
			}

			SetText(OverlayRuntime().state.prewarmDetail, command.detail.data());
			SetText(OverlayRuntime().state.prewarmStage, command.stage.data());
			wchar_t percent[16] = {};
			_snwprintf_s(percent, _countof(percent), _TRUNCATE, L"%u%%",
				static_cast<UInt32>(std::lround(
					std::clamp(command.progress, 0.0f, 1.0f) * 100.0f)));
			SetText(OverlayRuntime().state.prewarmPercent, percent);

			if (command.refreshSequence
				&& command.refreshSequence != OverlayRuntime().prewarmAppliedRefreshSequence)
			{
				RebuildTextGeometry(OverlayRuntime().state.prewarmTitle);
				RebuildTextGeometry(OverlayRuntime().state.prewarmDetail);
				RebuildTextGeometry(OverlayRuntime().state.prewarmStage);
				RebuildTextGeometry(OverlayRuntime().state.prewarmPercent);
				OverlayRuntime().state.prewarmLayoutSignature.fill(0.0f);
				OverlayRuntime().prewarmAppliedRefreshSequence = command.refreshSequence;
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
					"tnvse_native_overlay: prewarm progress consumer active thread=%u owner=LoadingMenuThread policy=queued-snapshot fontRoute=freetype-native-no-precache",
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

			if (command.ownerShutdown)
			{
				bool detached = true;
				try
				{
					ScopedFreeTypeNoPrecacheRoute noPrecacheRoute;
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
				OverlayRuntime().prewarmAppliedRefreshSequence = 0;
				OverlayRuntime().prewarmReady.store(false, std::memory_order_release);
				OverlayRuntime().prewarmActive.store(false, std::memory_order_release);
				OverlayRuntime().prewarmConsumerDisabled.store(
					true, std::memory_order_release);
				OverlayRuntime().prewarmConsumedSequence.store(
					command.sequence, std::memory_order_release);
				OverlayRuntime().prewarmOwnerShutdown.Acknowledge(command.sequence);
				gLog.FormattedMessage(
					"tnvse_native_overlay: owner-thread prewarm shutdown acknowledged sequence=%u thread=%u detached=%u",
					command.sequence, currentThread, detached ? 1u : 0u);
				return;
			}
			if (OverlayRuntime().prewarmConsumerDisabled.load(std::memory_order_acquire))
			{
				OverlayRuntime().prewarmActive.store(false, std::memory_order_release);
				OverlayRuntime().prewarmConsumedSequence.store(
					command.sequence, std::memory_order_release);
				return;
			}
			if (command.visible && !IsPrewarmOverlayMakeNodeRouteInstalled())
			{
				// Without ownership of the real deferred TileText::MakeNode boundary,
				// displaying this overlay could re-enter virtual PrecacheGeometry while
				// LoadingMenu owns the renderer/UI locks. Keep the blocking prewarm
				// functional and fail closed only for its optional progress UI.
				OverlayRuntime().prewarmConsumerDisabled.store(
					true, std::memory_order_release);
				OverlayRuntime().prewarmActive.store(false, std::memory_order_release);
				OverlayRuntime().prewarmConsumedSequence.store(
					command.sequence, std::memory_order_release);
				gLog.FormattedMessage(
					"tnvse_native_overlay: prewarm progress disabled because TileText::MakeNode no-precache route is not the verified top-level owner; blocking font prewarm continues");
				return;
			}

			try
			{
				// Cell Offset Generator keeps generation on workers and limits its
				// foreground path to LoadingMenu progress presentation. This path
				// additionally confines every Tile mutation to LoadingMenuThread and
				// keeps synchronous FreeType presentation off renderer precache. The
				// TileText::MakeNode hook independently scopes the deferred geometry
				// call reached later from LoadingMenu::ShowChanges; both boundaries are
				// required because RebuildTextGeometry only marks the node dirty here.
				ScopedFreeTypeNoPrecacheRoute noPrecacheRoute;
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
			}
			catch (...)
			{
				OverlayRuntime().prewarmConsumerDisabled.store(true, std::memory_order_release);
				OverlayRuntime().prewarmReady.store(false, std::memory_order_release);
				OverlayRuntime().prewarmActive.store(false, std::memory_order_release);
				OverlayRuntime().state.prewarmLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: prewarm progress consumer disabled reason=unknown policy=font-prewarm-continues");
			}
			OverlayRuntime().prewarmConsumedSequence.store(
				command.sequence, std::memory_order_release);
		}
	}
}

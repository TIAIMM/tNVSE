#include "native_tile_overlay_detail.h"

namespace fonthook
{
	using namespace implementation::native_tile_overlay;

	namespace implementation::native_tile_overlay
	{
		const char* PrewarmOverlayCloseReasonName(
			PrewarmOverlayCloseReason reason) noexcept
		{
			switch (reason)
			{
			case PrewarmOverlayCloseReason::Completed:
				return "completed";
			case PrewarmOverlayCloseReason::Cancelled:
				return "cancelled";
			case PrewarmOverlayCloseReason::Failed:
				return "failed";
			case PrewarmOverlayCloseReason::Watchdog:
				return "watchdog";
			case PrewarmOverlayCloseReason::Shutdown:
				return "shutdown";
			default:
				return "unknown";
			}
		}

		void LogPrewarmMilestones(
			UInt64 runToken, const PrewarmOverlayPublishResult& result)
		{
			constexpr UInt32 percentages[] = { 25, 50, 75, 100 };
			for (UInt8 i = 0; i < 4; ++i)
			{
				if (result.milestones & static_cast<UInt8>(1u << i))
				{
					gLog.FormattedMessage(
						"tnvse_native_overlay: prewarm graphical-only milestone run=%llu progress=%u sequence=%u",
						static_cast<unsigned long long>(runToken),
						percentages[i], result.sequence);
				}
			}
		}

		void DisablePrewarmInstance(
			PrewarmOverlayInstance& instance,
			const PrewarmOverlayCommand& command,
			const char* reason)
		{
			OverlayRuntime().prewarmMailbox.DisableRun(command.runToken);
			instance.Disable(command.runToken);
			gLog.FormattedMessage(
				"tnvse_native_overlay: prewarm graphical-only presentation disabled run=%llu sequence=%u reason=%s policy=font-prewarm-continues",
				static_cast<unsigned long long>(command.runToken),
				command.sequence, reason ? reason : "unknown");
		}

		bool PreparePrewarmInstance(
			PrewarmOverlayInstance& instance,
			const PrewarmOverlayCommand& command,
			Tile* loadingRoot)
		{
			if (instance.runToken != command.runToken
				&& instance.componentRoot
				&& instance.loadingRoot == loadingRoot)
			{
				SetVisible(instance.componentRoot, false);
			}
			Tile* previousRoot = instance.loadingRoot;
			const PrewarmOverlayHostResult hostResult =
				instance.BeginCommand(command.runToken, loadingRoot);
			if (hostResult == PrewarmOverlayHostResult::HostChanged)
			{
				// LoadingMenu owns the old subtree and may already have reclaimed it.
				// Forget every non-owning pointer without inspecting or releasing it.
				gLog.FormattedMessage(
					"tnvse_native_overlay: LoadingMenu root changed during prewarm presentation run=%llu old=%p new=%p policy=disable-run-without-dereference",
					static_cast<unsigned long long>(command.runToken),
					previousRoot, loadingRoot);
				OverlayRuntime().prewarmMailbox.DisableRun(command.runToken);
				return false;
			}
			return hostResult == PrewarmOverlayHostResult::Ready;
		}

		bool AttachPrewarmInstance(
			PrewarmOverlayInstance& instance,
			const PrewarmOverlayCommand& command,
			Tile* loadingRoot)
		{
			Tile* componentRoot = ThisStdCall<Tile*>(
				kTileReadFile, loadingRoot, kPrewarmOverlayXmlPath);
			const UInt32 progressTrait =
				Tile::TraitNameToID("_tnvse_prewarm_progress");
			const bool attached = IsDirectChild(loadingRoot, componentRoot);
			const bool valid = componentRoot && attached
				&& !_stricmp(componentRoot->strName.c_str(), "tNVSE_Prewarm")
				&& progressTrait && componentRoot->GetValue(progressTrait);
			if (!valid)
			{
				// ReadFile attaches successful parses to the LoadingMenu root. Leave any
				// returned node there for the engine to reclaim; never destroy it here.
				if (componentRoot && attached)
					SetVisible(componentRoot, false);
				DisablePrewarmInstance(
					instance, command, "xml-load-or-root-trait-validation-failed");
				gLog.FormattedMessage(
					"tnvse_native_overlay: failed to load prewarm graphical-only component path='%s' root=%p attached=%u trait=%u",
					kPrewarmOverlayXmlPath, componentRoot,
					attached ? 1u : 0u, progressTrait);
				return false;
			}

			instance.Attach(loadingRoot, componentRoot, progressTrait);
			componentRoot->SetValueFloat(
				progressTrait, std::clamp(command.progress, 0.0f, 1.0f), true);
			SetVisible(componentRoot, true);
			gLog.FormattedMessage(
				"tnvse_native_overlay: attached prewarm graphical-only component once run=%llu sequence=%u path='%s' root=%p host=%p",
				static_cast<unsigned long long>(command.runToken),
				command.sequence, kPrewarmOverlayXmlPath,
				componentRoot, loadingRoot);
			return true;
		}

		void ConsumeNativePrewarmOverlayCommand(Menu* loadingMenu)
		{
			PrewarmOverlayMailbox& mailbox = OverlayRuntime().prewarmMailbox;
			if (!mailbox.HasPending())
				return;

			const PrewarmOverlayCommand command = mailbox.ReadLatest();
			if (!command.sequence)
				return;

			Tile* loadingRoot = loadingMenu && loadingMenu->pRootTile
				? static_cast<Tile*>(loadingMenu->pRootTile)
				: nullptr;
			if (!loadingRoot)
				return;

			PrewarmOverlayInstance& instance =
				OverlayRuntime().prewarmInstance;
			try
			{
				if (!PreparePrewarmInstance(instance, command, loadingRoot))
				{
					mailbox.MarkConsumed(command.sequence);
					return;
				}

				switch (command.action)
				{
				case PrewarmOverlayAction::Close:
					if (instance.componentRoot
						&& instance.loadingRoot == loadingRoot)
					{
						SetVisible(instance.componentRoot, false);
					}
					// The hidden component remains a child of the vanilla root. Clear only
					// tNVSE's non-owning references and let LoadingMenu reclaim its tree.
					instance.Close();
					break;

				case PrewarmOverlayAction::Suspend:
					if (instance.componentRoot)
						SetVisible(instance.componentRoot, false);
					instance.Suspend();
					break;

				case PrewarmOverlayAction::Present:
					if (!instance.componentRoot)
					{
						AttachPrewarmInstance(
							instance, command, loadingRoot);
					}
					else
					{
						const bool resumed = instance.state
							== PrewarmOverlayInstanceState::Suspended;
						instance.componentRoot->SetValueFloat(
							instance.progressTrait,
							std::clamp(command.progress, 0.0f, 1.0f), true);
						SetVisible(instance.componentRoot, true);
						instance.state = PrewarmOverlayInstanceState::Attached;
						if (resumed)
						{
							gLog.FormattedMessage(
								"tnvse_native_overlay: resumed prewarm graphical-only component run=%llu sequence=%u",
								static_cast<unsigned long long>(command.runToken),
								command.sequence);
						}
					}
					break;
				}
			}
			catch (const std::exception& error)
			{
				try
				{
					if (instance.componentRoot
						&& instance.loadingRoot == loadingRoot)
					{
						SetVisible(instance.componentRoot, false);
					}
				}
				catch (...) {}
				DisablePrewarmInstance(instance, command, error.what());
			}
			catch (...)
			{
				try
				{
					if (instance.componentRoot
						&& instance.loadingRoot == loadingRoot)
					{
						SetVisible(instance.componentRoot, false);
					}
				}
				catch (...) {}
				DisablePrewarmInstance(instance, command, "unknown-exception");
			}

			mailbox.MarkConsumed(command.sequence);
		}
	}

	void PublishNativePrewarmOverlayProgress(
		UInt64 runToken, float progress)
	{
		const PrewarmOverlayPublishResult result =
			OverlayRuntime().prewarmMailbox.PublishProgress(runToken, progress);
		if (!result)
			return;
		if (result.openedRun)
		{
			gLog.FormattedMessage(
				"tnvse_native_overlay: prewarm graphical-only run open run=%llu sequence=%u policy=latest-state-no-wait",
				static_cast<unsigned long long>(runToken), result.sequence);
		}
		else if (result.resumedRun)
		{
			gLog.FormattedMessage(
				"tnvse_native_overlay: prewarm graphical-only run resume requested run=%llu sequence=%u",
				static_cast<unsigned long long>(runToken), result.sequence);
		}
		LogPrewarmMilestones(runToken, result);
	}

	void SuspendNativePrewarmOverlay(UInt64 runToken)
	{
		const UInt32 sequence =
			OverlayRuntime().prewarmMailbox.PublishSuspend(runToken);
		if (sequence)
		{
			gLog.FormattedMessage(
				"tnvse_native_overlay: prewarm graphical-only run suspend requested run=%llu sequence=%u",
				static_cast<unsigned long long>(runToken), sequence);
		}
	}

	void CloseNativePrewarmOverlay(
		UInt64 runToken, PrewarmOverlayCloseReason reason)
	{
		const UInt32 sequence =
			OverlayRuntime().prewarmMailbox.PublishClose(runToken, reason);
		if (sequence)
		{
			gLog.FormattedMessage(
				"tnvse_native_overlay: prewarm graphical-only run close requested run=%llu sequence=%u reason=%s policy=no-wait",
				static_cast<unsigned long long>(runToken), sequence,
				PrewarmOverlayCloseReasonName(reason));
		}
	}

	bool IsNativePrewarmOverlayPresentationRequested()
	{
		return OverlayRuntime().prewarmMailbox.IsPresentationRequested();
	}

	void LogNativePrewarmOverlayBarrierState(
		UInt64 runToken, const char* barrier)
	{
		const UInt32 published =
			OverlayRuntime().prewarmMailbox.PublishedSequence();
		const UInt32 consumed =
			OverlayRuntime().prewarmMailbox.ConsumedSequence();
		if (published != consumed)
		{
			gLog.FormattedMessage(
				"tnvse_native_overlay: prewarm graphical-only command unconsumed at barrier run=%llu barrier=%s published=%u consumed=%u policy=no-wait",
				static_cast<unsigned long long>(runToken),
				barrier ? barrier : "unknown", published, consumed);
		}
	}
}

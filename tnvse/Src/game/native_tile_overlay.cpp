#include "native_tile_overlay_detail.h"

namespace fonthook
{
	namespace implementation::native_tile_overlay {}
	using namespace implementation::native_tile_overlay;

	namespace implementation::native_tile_overlay
	{
		NativeTileOverlayRuntimeState& OverlayRuntime()
		{
			static NativeTileOverlayRuntimeState state;
			return state;
		}

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
		return OverlayRuntime().imeReady.load(std::memory_order_acquire);
	}

	bool IsNativeImeOverlayVisible()
	{
		return IsNativeImeOverlayHostReady()
			&& OverlayRuntime().state.imeVisible
			&& OverlayRuntime().state.imeRoot
			&& OverlayRuntime().state.imeRoot->GetValueFloat(
				Tile::kTileValue_visible) > 0.5f
			&& HasExpectedImeLinePresentation(
				OverlayRuntime().state.imeVisibleLineCount);
	}

	UInt32 GetNativeImeOverlayHostGeneration()
	{
		return OverlayRuntime().imeHostGeneration.load(std::memory_order_acquire);
	}

	bool IsNativePrewarmOverlayHostReady()
	{
		return OverlayRuntime().prewarmReady.load(std::memory_order_acquire);
	}

	void UpdateNativeImeOverlay(
		const std::vector<NativeTileOverlayLine>& lines,
		bool forceTextGeometryRefresh)
	{
		if (!IsNativeImeOverlayHostReady())
			EnsureNativeImeOverlayHost();
		if (!IsNativeImeOverlayHostReady()
			|| OverlayRuntime().prewarmActive.load(std::memory_order_acquire))
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
			OverlayRuntime().state.imeRoot->GetValueFloat(
				Tile::kTileValue_visible) > 0.5f;
		const bool linePresentationIntact =
			HasExpectedImeLinePresentation(
				OverlayRuntime().state.imeVisibleLineCount);
		const bool presentationNeedsRepair =
			forceTextGeometryRefresh
			|| !OverlayRuntime().state.imeVisible
			|| !rootVisible
			|| !linePresentationIntact;

		// Publish text only after its Menu ancestor is visible. Line 00 is the
		// status-only presentation created when a text target first activates;
		// constructing it below the XML-default-hidden root can seal a zero-alpha
		// FreeType shape while later composition/candidate rows are built after
		// the root is visible.
		SynchronizeOverlayDepth(OverlayRuntime().state.imeRoot);
		if (!rootVisible)
			SetVisible(OverlayRuntime().state.imeRoot, true);
		OverlayRuntime().state.imeVisible = true;

		const std::wstring key = BuildImeKey(lines);
		const bool contentChanged = key != OverlayRuntime().state.imeKey;
		const bool republish = contentChanged || presentationNeedsRepair;
		if (republish)
		{
			OverlayRuntime().state.imeKey = key;
			OverlayRuntime().state.imeVisibleLineCount = visibleCount;
			for (Tile* highlight : OverlayRuntime().state.imeHighlights)
				SetVisible(highlight, false);
			for (size_t i = 0; i < kImeLineCount; ++i)
			{
				const bool visible = i < visibleCount;
				SetVisible(OverlayRuntime().state.imeLines[i], visible);
				if (visible)
				{
					PublishTextGeometry(
						OverlayRuntime().state.imeLines[i],
						lines[i].text,
						presentationNeedsRepair);
				}
			}
			if (presentationNeedsRepair && g_bMultibyteInputLog)
			{
				gLog.FormattedMessage(
					"tnvse_native_overlay: repaired IME text presentation host=%p lines=%u caller_force=%u root_visible=%u line_state_valid=%u",
					OverlayRuntime().state.imeRoot,
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
				OverlayRuntime().state.imeLines[i]->GetValueFloat(
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
				OverlayRuntime().state.imeLines[i]->GetValueFloat(
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
			std::fabs(lineHeight - OverlayRuntime().state.imeLineHeight) > 0.25f;
		const bool widthChanged =
			std::fabs(contentWidth - OverlayRuntime().state.imeContentWidth) > 0.25f;
		if (republish || metricsChanged || widthChanged)
		{
			OverlayRuntime().state.imeLineHeight = lineHeight;
			OverlayRuntime().state.imeContentWidth = contentWidth;
			const float left =
				(kImeMaximumWidth - contentWidth) * 0.5f;
			const float lineStride = lineHeight + kImeLineGap;
			const float height = kImeVerticalPadding * 2.0f
				+ static_cast<float>(visibleCount) * lineHeight
				+ static_cast<float>(
					visibleCount > 0 ? visibleCount - 1 : 0)
					* kImeLineGap;
			OverlayRuntime().state.imeRoot->SetValueFloat(
				Tile::kTileValue_height, height, true);
			OverlayRuntime().state.imeBackground->SetValueFloat(
				Tile::kTileValue_x, left, true);
			OverlayRuntime().state.imeBackground->SetValueFloat(
				Tile::kTileValue_height, height, true);
			OverlayRuntime().state.imeBackground->SetValueFloat(
				Tile::kTileValue_width, contentWidth, true);
			for (size_t i = 0; i < visibleCount; ++i)
			{
				const float lineY = kImeVerticalPadding
					+ static_cast<float>(i) * lineStride;
				OverlayRuntime().state.imeLines[i]->SetValueFloat(
					Tile::kTileValue_x, left + 10.0f, true);
				OverlayRuntime().state.imeLines[i]->SetValueFloat(
					Tile::kTileValue_user0,
					lineY,
					true);
				OverlayRuntime().state.imeLines[i]->SetValueFloat(
					Tile::kTileValue_user1,
					lineHeight,
					true);
				const float measuredHeight =
					OverlayRuntime().state.imeLines[i]->GetValueFloat(
						Tile::kTileValue_height);
				OverlayRuntime().state.imeLines[i]->SetValueFloat(
					Tile::kTileValue_user2,
					visualBounds[i].valid
						? visualBounds[i].top : 0.0f,
					true);
				OverlayRuntime().state.imeLines[i]->SetValueFloat(
					Tile::kTileValue_user3,
					visualBounds[i].valid
						? visualBounds[i].height
						: std::max(0.0f, measuredHeight),
					true);
				if (g_bEnableFreeTypeFontRenderingLog
					&& OverlayRuntime().imeVisualBoundsLogCount
						< kImeVisualBoundsLogLimit)
				{
					++OverlayRuntime().imeVisualBoundsLogCount;
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
				OverlayRuntime().state.imeLines[i]->SetValueFloat(
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
					OverlayRuntime().state.imeHighlights[highlightIndex++];
				highlight->SetValueFloat(
					Tile::kTileValue_y,
					kImeVerticalPadding
						+ static_cast<float>(i) * lineStride,
					true);
				highlight->SetValueFloat(
					Tile::kTileValue_height, lineHeight, true);
				SetVisible(highlight, true);
			}
			for (Tile* highlight : OverlayRuntime().state.imeHighlights)
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
			IsNamedDirectChild(parent, OverlayRuntime().state.imeRoot, "tNVSE_IME");
		if (OverlayRuntime().state.imeVisible
			&& IsNativeImeOverlayHostReady()
			&& attached)
			SetVisible(OverlayRuntime().state.imeRoot, false);
		if (OverlayRuntime().state.imeRoot && !attached)
			ClearImeResolvedTiles();
		OverlayRuntime().state.imeVisible = false;
		OverlayRuntime().state.imeKey.clear();
		OverlayRuntime().state.imeVisibleLineCount = 0;
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
			|| (!OverlayRuntime().prewarmConsumerThreadId.load(std::memory_order_acquire)
				&& !OverlayRuntime().prewarmReady.load(std::memory_order_acquire)))
		{
			if (timeoutMs && g_bEnableFreeTypeFontRenderingLog)
			{
				LogNativeLoadingMenuDiagnostic(
					"prewarm-refresh-wait-skipped:no-reachable-consumer");
			}
			return true;
		}

		const ULONGLONG startedAt = GetTickCount64();
		LogNativeLoadingMenuDiagnostic("prewarm-refresh-wait-begin");
		const ULONGLONG deadline = GetTickCount64() + timeoutMs;
		while (OverlayRuntime().prewarmConsumedSequence.load(std::memory_order_acquire)
			!= sequence)
		{
			if (GetTickCount64() >= deadline)
			{
				gLog.FormattedMessage(
					"tnvse_native_overlay: timed out synchronizing prewarm text refresh sequence=%u consumed=%u timeoutMs=%u policy=retain-referenced-retired-atlas",
					sequence,
					OverlayRuntime().prewarmConsumedSequence.load(
						std::memory_order_acquire),
						timeoutMs);
				LogNativeLoadingMenuDiagnostic("prewarm-refresh-wait-timeout");
				return false;
			}
			Sleep(1);
		}
		gLog.FormattedMessage(
			"tnvse_native_overlay: synchronized prewarm text refresh sequence=%u consumed=%u waitMs=%llu",
			sequence,
			OverlayRuntime().prewarmConsumedSequence.load(
				std::memory_order_acquire),
			static_cast<unsigned long long>(GetTickCount64() - startedAt));
		LogNativeLoadingMenuDiagnostic("prewarm-refresh-wait-complete");
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
			|| (!OverlayRuntime().prewarmConsumerThreadId.load(std::memory_order_acquire)
				&& !OverlayRuntime().prewarmReady.load(std::memory_order_acquire)))
		{
			if (timeoutMs && g_bEnableFreeTypeFontRenderingLog)
			{
				LogNativeLoadingMenuDiagnostic(
					"prewarm-quiesce-wait-skipped:no-reachable-consumer");
			}
			return true;
		}

		const ULONGLONG startedAt = GetTickCount64();
		LogNativeLoadingMenuDiagnostic("prewarm-quiesce-wait-begin");
		const ULONGLONG deadline = GetTickCount64() + timeoutMs;
		while (OverlayRuntime().prewarmConsumedSequence.load(std::memory_order_acquire)
			!= sequence)
		{
			if (GetTickCount64() >= deadline)
			{
				gLog.FormattedMessage(
					"tnvse_native_overlay: timed out quiescing prewarm progress sequence=%u consumed=%u timeoutMs=%u policy=skip-atlas-publication",
					sequence,
					OverlayRuntime().prewarmConsumedSequence.load(
						std::memory_order_acquire),
						timeoutMs);
				LogNativeLoadingMenuDiagnostic("prewarm-quiesce-wait-timeout");
				return false;
			}
			Sleep(1);
		}
		gLog.FormattedMessage(
			"tnvse_native_overlay: quiesced prewarm progress sequence=%u consumed=%u waitMs=%llu",
			sequence,
			OverlayRuntime().prewarmConsumedSequence.load(
				std::memory_order_acquire),
			static_cast<unsigned long long>(GetTickCount64() - startedAt));
		LogNativeLoadingMenuDiagnostic("prewarm-quiesce-wait-complete");
		return true;
	}

	bool IsNativePrewarmOverlayActive()
	{
		return OverlayRuntime().prewarmActive.load(std::memory_order_acquire);
	}

	void ShutdownNativeTileOverlayHost()
	{
		// NVSE dispatches exit messages synchronously on the game thread without
		// first joining LoadingMenuThread.  Publish owner-thread teardown and wait
		// only for a bounded acknowledgement; never clear its raw Tile pointers here.
		const UInt32 prewarmShutdownSequence =
			PublishPrewarmOverlayOwnerShutdown();
		OverlayRuntime().imeReady.store(false, std::memory_order_release);
		OverlayRuntime().prewarmReady.store(false, std::memory_order_release);
		OverlayRuntime().prewarmActive.store(false, std::memory_order_release);

		constexpr UInt32 kOwnerShutdownTimeoutMs = 500;
		const DWORD ownerThread = OverlayRuntime().prewarmConsumerThreadId.load(
			std::memory_order_acquire);
		const bool ownerReachable = HasVerifiedLoadingMenuUpdateHook()
			&& (ownerThread
				|| OverlayRuntime().prewarmOwnerShutdown.OwnerWorkInFlight());
		bool ownerQuiesced = OverlayRuntime().prewarmOwnerShutdown.IsQuiesced(
			prewarmShutdownSequence);
		if (!ownerQuiesced && ownerThread == GetCurrentThreadId())
		{
			ConsumeNativePrewarmOverlayCommand();
			ownerQuiesced = OverlayRuntime().prewarmOwnerShutdown.IsQuiesced(
				prewarmShutdownSequence);
		}
		if (!ownerQuiesced && ownerReachable)
		{
			const ULONGLONG deadline =
				GetTickCount64() + kOwnerShutdownTimeoutMs;
			while (!OverlayRuntime().prewarmOwnerShutdown.IsQuiesced(
				prewarmShutdownSequence)
				&& GetTickCount64() < deadline)
			{
				Sleep(1);
			}
			ownerQuiesced = OverlayRuntime().prewarmOwnerShutdown.IsQuiesced(
				prewarmShutdownSequence);
		}
		if (!ownerQuiesced
			&& (ownerThread
				|| OverlayRuntime().prewarmOwnerShutdown.OwnerWorkInFlight()))
		{
			gLog.FormattedMessage(
				"tnvse_native_overlay: owner-thread prewarm shutdown acknowledgement unavailable sequence=%u ownerThread=%u inFlight=%u timeoutMs=%u policy=retain-owner-state-no-cross-thread-clear",
				prewarmShutdownSequence, ownerThread,
				OverlayRuntime().prewarmOwnerShutdown.OwnerWorkInFlight(),
				kOwnerShutdownTimeoutMs);
			LogNativeLoadingMenuDiagnostic(
				"prewarm-owner-shutdown-wait-timeout");
		}
		OverlayRuntime().prewarmReady.store(false, std::memory_order_release);
		OverlayRuntime().prewarmActive.store(false, std::memory_order_release);
		InterfaceManager* manager = InterfaceManager::GetSingleton();
		Tile* currentImeParent =
			manager ? manager->pMenuRoot : nullptr;
		if (currentImeParent
			&& currentImeParent == OverlayRuntime().state.imeParent)
		{
			ReleaseAndDestroyAttachedRoot(
				currentImeParent,
				OverlayRuntime().state.imeRoot,
				"tNVSE_IME");
		}
		ResetImeForParent(nullptr);
		LogNativeLoadingMenuDiagnostic(ownerQuiesced
			? "native-overlay-shutdown-complete"
			: "native-overlay-shutdown-retained-owner-state");
	}
}

#include "multibyte_input_ime_internal.h"

#include "native_tile_overlay.h"

namespace fonthook
{
	namespace multibyte_input
	{
		const wchar_t* GetNativeModeLabel()
		{
			const ImeState& state = State();
			const bool native = (state.candidate.conversionMode
				& IME_CMODE_NATIVE) != 0;
			switch (g_uiEncoding)
			{
			case 3:
				return native ? L"\u65E5\u672C\u8A9E" : L"\u82F1\u6570";
			case 4:
				return native ? L"\uD55C\uAD6D\uC5B4" : L"\uC601\uBB38";
			default:
				return native ? L"\u4E2D\u6587" : L"\u82F1\u6587";
			}
		}

		std::wstring BuildImeStatusLineWide()
		{
			const ImeState& state = State();
			std::wstring line = state.candidate.imeName.empty()
				? L"IME"
				: state.candidate.imeName;
			line += state.candidate.imeOpen ? L" ON" : L" OFF";
			if (state.candidate.imeOpen)
			{
				line += L" ";
				line += GetNativeModeLabel();
				line += (state.candidate.conversionMode & IME_CMODE_FULLSHAPE)
					? L" \u5168\u89D2"
					: L" \u534A\u89D2";
			}
			return line;
		}

		std::vector<CandidateOverlayLine> BuildCandidateOverlayLines()
		{
			const ImeState& state = State();
			std::vector<CandidateOverlayLine> lines;
			if (!g_bMultibyteInputCompositionPreview
				|| !state.candidate.imeOpen)
			{
				return lines;
			}

			if (!HasOverlayInputTarget())
			{
				ClearStewieInputState();
				return lines;
			}

			lines.push_back({ BuildImeStatusLineWide(), false });
			if (!state.candidate.composition.empty())
			{
				std::wstring composition = L"> ";
				composition += state.candidate.composition;
				lines.push_back({ std::move(composition), false });
			}

			const size_t candidateCount = std::min<size_t>(
				state.candidate.candidates.size(), 9);
			for (size_t i = 0; i < candidateCount; ++i)
			{
				if (state.candidate.candidates[i].empty())
					continue;
				const DWORD globalIndex =
					state.candidate.pageStart + static_cast<DWORD>(i);
				wchar_t prefix[8] = {};
				std::swprintf(prefix, ARRAYSIZE(prefix), L"%u. ",
					static_cast<UInt32>(i + 1));
				std::wstring line = prefix;
				line += state.candidate.candidates[i];
				lines.push_back({
					std::move(line),
					globalIndex == state.candidate.selection
				});
			}
			return lines;
		}

		bool IsCandidateOverlayRendererAvailable()
		{
			return g_bMultibyteInputCompositionPreview
				&& IsNativeImeOverlayHostReady()
				&& !IsNativePrewarmOverlayPresentationRequested();
		}

		void HideCandidateOverlay()
		{
			ImeState& state = State();
			state.overlay.visible = false;
			state.overlayRefreshPending = true;
		}

		void UpdateCandidateOverlay()
		{
			ImeState& state = State();
			state.overlayRefreshPending = true;
		}

		void PumpCandidateOverlay()
		{
			ImeState& state = State();
			const DWORD now = GetTickCount();
			constexpr DWORD kOverlayHostValidationIntervalMs = 250;
			const bool validateHost = g_bMultibyteInputCompositionPreview
				&& (state.overlayRefreshPending
					|| !IsNativeImeOverlayHostReady()
					|| !state.lastNativeOverlayHostCheckTick
					|| now - state.lastNativeOverlayHostCheckTick
						>= kOverlayHostValidationIntervalMs);
			if (validateHost)
			{
				EnsureNativeImeOverlayHost();
				state.lastNativeOverlayHostCheckTick = now;
				if (state.overlay.visible
					&& !IsNativeImeOverlayVisible())
				{
					state.overlayRefreshPending = true;
				}
			}

			const UInt32 hostGeneration =
				GetNativeImeOverlayHostGeneration();
			const bool hostGenerationChanged =
				state.nativeOverlayHostGeneration != hostGeneration;
			if (hostGenerationChanged)
				state.overlayRefreshPending = true;

			if (!g_bMultibyteInputCompositionPreview
				|| IsNativePrewarmOverlayPresentationRequested()
				|| !IsCandidateOverlayRendererAvailable()
				|| !state.candidate.imeOpen)
			{
				const bool shouldHide = state.overlay.visible
					|| state.overlayRefreshPending;
				state.overlay.visible = false;
				if (shouldHide)
					HideNativeImeOverlay();
				state.nativeOverlayHostGeneration = hostGeneration;
				state.overlayRefreshPending = false;
				return;
			}

			if (!state.overlayRefreshPending
				&& state.overlay.visible
				&& state.nativeOverlayHostGeneration == hostGeneration)
			{
				return;
			}

			std::vector<CandidateOverlayLine> lines =
				BuildCandidateOverlayLines();
			if (lines.empty())
			{
				const bool shouldHide = state.overlay.visible
					|| state.overlayRefreshPending;
				state.overlay.visible = false;
				if (shouldHide)
					HideNativeImeOverlay();
				state.nativeOverlayHostGeneration = hostGeneration;
				state.overlayRefreshPending = false;
				return;
			}

			std::vector<NativeTileOverlayLine> nativeLines;
			nativeLines.reserve(lines.size());
			for (CandidateOverlayLine& line : lines)
			{
				nativeLines.push_back({
					std::move(line.text), line.highlighted
				});
			}
			// A target switch can deactivate and reactivate the logical overlay
			// before the main-thread pump ever hides its native Menu. Force one
			// text republish for that transition so the unchanged status string
			// cannot reuse geometry created under a hidden/stale ancestor.
			const bool forceTextGeometryRefresh =
				!state.overlay.visible
				|| hostGenerationChanged
				|| !IsNativeImeOverlayVisible();
			UpdateNativeImeOverlay(
				nativeLines, forceTextGeometryRefresh);
			state.overlay.visible = IsNativeImeOverlayVisible();
			state.nativeOverlayHostGeneration =
				GetNativeImeOverlayHostGeneration();
			state.overlayRefreshPending = false;
		}
	}
}

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
				&& !IsNativePrewarmOverlayActive();
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
			if (g_bMultibyteInputCompositionPreview)
				EnsureNativeImeOverlayHost();
			state.overlayRefreshPending = false;

			if (!g_bMultibyteInputCompositionPreview
				|| IsNativePrewarmOverlayActive()
				|| !IsCandidateOverlayRendererAvailable()
				|| !state.candidate.imeOpen)
			{
				state.overlay.visible = false;
				HideNativeImeOverlay();
				return;
			}

			std::vector<CandidateOverlayLine> lines =
				BuildCandidateOverlayLines();
			if (lines.empty())
			{
				state.overlay.visible = false;
				HideNativeImeOverlay();
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
			// Always enter the service so a pMenuRoot/IME Menu rebuild can
			// repopulate and show its fresh, XML-default-hidden Tile tree. The
			// service still compares its own content key before mutating traits.
			UpdateNativeImeOverlay(nativeLines);
			state.overlay.visible = true;
		}
	}
}

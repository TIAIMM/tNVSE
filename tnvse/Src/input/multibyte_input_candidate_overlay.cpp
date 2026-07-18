#include "multibyte_input_ime_internal.h"

namespace fonthook
{
	namespace multibyte_input
	{
		const wchar_t* GetNativeModeLabel()
		{
			const ImeState& state = State();
			const bool native = (state.candidate.conversionMode & IME_CMODE_NATIVE) != 0;
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
			if (!g_bMultibyteInputCompositionPreview || !state.candidate.imeOpen)
				return lines;

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

			for (size_t i = 0; i < state.candidate.candidates.size(); ++i)
			{
				if (state.candidate.candidates[i].empty())
					continue;

				const DWORD globalIndex = state.candidate.pageStart + static_cast<DWORD>(i);
				wchar_t prefix[8] = {};
				std::swprintf(prefix, ARRAYSIZE(prefix), L"%u. ", static_cast<UInt32>(i + 1));
				std::wstring line = prefix;
				line += state.candidate.candidates[i];
				lines.push_back({ std::move(line), globalIndex == state.candidate.selection });
			}

			return lines;
		}

		std::wstring BuildCandidateOverlayKey(const std::vector<CandidateOverlayLine>& lines)
		{
			std::wstring key;
			for (const CandidateOverlayLine& line : lines)
			{
				key += line.highlighted ? L"\x0001" : L"\x0000";
				key += line.text;
				key += L"\n";
			}
			return key;
		}

		void ReleaseCandidateOverlayTexture()
		{
			ImeState& state = State();
			if (state.overlay.texture)
			{
				state.overlay.texture->Release();
				state.overlay.texture = nullptr;
			}
			state.overlay.textureWidth = 0;
			state.overlay.textureHeight = 0;
		}

		void HideCandidateOverlay()
		{
			ImeState& state = State();
			state.overlay.visible = false;
			state.overlay.dirty = true;
			state.overlay.lastKey.clear();
		}

		void UpdateCandidateOverlay()
		{
			ImeState& state = State();
			if (!g_bMultibyteInputCompositionPreview || !IsCandidateOverlayRendererAvailable())
			{
				HideCandidateOverlay();
				return;
			}

			if (!state.candidate.imeOpen)
			{
				HideCandidateOverlay();
				return;
			}

			if (!HasOverlayInputTarget())
			{
				ClearStewieInputState();
				HideCandidateOverlay();
				return;
			}

			state.overlay.visible = true;
		}

		bool RenderOverlayTexture(
			LPDIRECT3DDEVICE9 device,
			const std::vector<CandidateOverlayLine>& lines)
		{
			ImeState& state = State();
			if (!device || lines.empty() || !IsCandidateOverlayRendererAvailable())
				return false;

			std::vector<UInt32> pixels;
			UInt32 width = 0;
			UInt32 height = 0;
			if (!RasterizeCandidateOverlay(lines, pixels, width, height)
				|| !width
				|| !height
				|| pixels.size() != static_cast<size_t>(width) * height)
			{
				return false;
			}

			if (!state.overlay.texture
				|| state.overlay.textureWidth != width
				|| state.overlay.textureHeight != height)
			{
				ReleaseCandidateOverlayTexture();
				if (FAILED(device->CreateTexture(
					width,
					height,
					1,
					0,
					D3DFMT_A8R8G8B8,
					D3DPOOL_MANAGED,
					&state.overlay.texture,
					nullptr)))
				{
					return false;
				}

				state.overlay.textureWidth = width;
				state.overlay.textureHeight = height;
			}

			D3DLOCKED_RECT locked = {};
			if (FAILED(state.overlay.texture->LockRect(0, &locked, nullptr, 0)))
				return false;

			for (UInt32 y = 0; y < height; ++y)
			{
				std::memcpy(
					static_cast<UInt8*>(locked.pBits) + y * locked.Pitch,
					pixels.data() + static_cast<size_t>(y) * width,
					width * sizeof(UInt32));
			}
			state.overlay.texture->UnlockRect(0);
			return true;
		}
		struct OverlayVertex
		{
			float x;
			float y;
			float z;
			float rhw;
			D3DCOLOR color;
			float u;
			float v;
		};

		void DrawCandidateOverlay()
		{
			ImeState& state = State();
			if (!g_bMultibyteInputCompositionPreview
				|| !IsCandidateOverlayRendererAvailable()
				|| !state.overlay.visible)
				return;

			std::vector<CandidateOverlayLine> lines = BuildCandidateOverlayLines();
			if (lines.empty())
			{
				HideCandidateOverlay();
				return;
			}

			std::wstring key = BuildCandidateOverlayKey(lines);
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			LPDIRECT3DDEVICE9 device = renderer ? renderer->GetD3DDevice() : nullptr;
			if (!device)
				return;

			if (state.overlay.dirty || key != state.overlay.lastKey || !state.overlay.texture)
			{
				if (!RenderOverlayTexture(device, lines))
				{
					state.overlay.dirty = true;
					return;
				}

				state.overlay.lastKey = std::move(key);
				state.overlay.dirty = false;
			}

			if (!state.overlay.texture)
				return;

			D3DVIEWPORT9 viewport = {};
			if (FAILED(device->GetViewport(&viewport)))
				return;

			const float x = std::max<float>(
				12.0f,
				(static_cast<float>(viewport.Width) - static_cast<float>(state.overlay.textureWidth)) * 0.5f);
			const float y = std::min<float>(
				static_cast<float>(viewport.Height) - static_cast<float>(state.overlay.textureHeight) - 12.0f,
				static_cast<float>(viewport.Height) * 0.58f);
			const float right = x + static_cast<float>(state.overlay.textureWidth);
			const float bottom = y + static_cast<float>(state.overlay.textureHeight);

			OverlayVertex vertices[4] = {
				{ x - 0.5f, y - 0.5f, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 0.0f },
				{ right - 0.5f, y - 0.5f, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 0.0f },
				{ x - 0.5f, bottom - 0.5f, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 1.0f },
				{ right - 0.5f, bottom - 0.5f, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 1.0f },
			};

			IDirect3DStateBlock9* stateBlock = nullptr;
			if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) && stateBlock)
				stateBlock->Capture();

			device->SetTexture(0, state.overlay.texture);
			device->SetPixelShader(nullptr);
			device->SetVertexShader(nullptr);
			device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
			device->SetRenderState(D3DRS_ZENABLE, FALSE);
			device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
			device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
			device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
			device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
			device->SetRenderState(D3DRS_LIGHTING, FALSE);
			device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
			device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
			device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
			device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(OverlayVertex));

			if (stateBlock)
			{
				stateBlock->Apply();
				stateBlock->Release();
			}
		}
	}
}

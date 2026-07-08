#include "multibyte_input_internal.h"

#include "load_config.h"
#include "NiDX9Renderer.hpp"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include <Windows.h>
#include <d3d9.h>

namespace fonthook
{
	namespace
	{
		struct CandidateOverlayState
		{
			LPDIRECT3DTEXTURE9 texture = nullptr;
			UInt32 textureWidth = 0;
			UInt32 textureHeight = 0;
			std::wstring lastKey;
			bool dirty = true;
			bool visible = false;
		};

		CandidateOverlayState s_candidateOverlay;

		const wchar_t* GetOverlayFontName()
		{
			switch (g_uiEncoding)
			{
			case 2:
				return L"Microsoft JhengHei UI";
			case 3:
				return L"Meiryo";
			case 4:
				return L"Malgun Gothic";
			default:
				return L"Microsoft YaHei UI";
			}
		}

		const wchar_t* GetNativeModeLabel()
		{
			const bool native = (s_imeCandidateState.conversionMode & IME_CMODE_NATIVE) != 0;
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
			std::wstring line = s_imeCandidateState.imeName.empty()
				? L"IME"
				: s_imeCandidateState.imeName;
			line += s_imeCandidateState.imeOpen ? L" ON" : L" OFF";
			if (s_imeCandidateState.imeOpen)
			{
				line += L" ";
				line += GetNativeModeLabel();
				line += (s_imeCandidateState.conversionMode & IME_CMODE_FULLSHAPE)
					? L" \u5168\u89D2"
					: L" \u534A\u89D2";
			}
			return line;
		}

		std::vector<CandidateOverlayLine> BuildCandidateOverlayLines()
		{
			std::vector<CandidateOverlayLine> lines;
			if (!g_bMultibyteInputCompositionPreview || !HasOverlayInputTarget() || !s_imeCandidateState.imeOpen)
				return lines;

			lines.push_back({ BuildImeStatusLineWide(), false });

			if (!s_imeCandidateState.composition.empty())
			{
				std::wstring composition = L"> ";
				composition += s_imeCandidateState.composition;
				lines.push_back({ std::move(composition), false });
			}

			for (size_t i = 0; i < s_imeCandidateState.candidates.size(); ++i)
			{
				if (s_imeCandidateState.candidates[i].empty())
					continue;

				const DWORD globalIndex = s_imeCandidateState.pageStart + static_cast<DWORD>(i);
				wchar_t prefix[8] = {};
				std::swprintf(prefix, ARRAYSIZE(prefix), L"%u. ", static_cast<UInt32>(i + 1));
				std::wstring line = prefix;
				line += s_imeCandidateState.candidates[i];
				lines.push_back({ std::move(line), globalIndex == s_imeCandidateState.selection });
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

		bool RenderOverlayTexture(
			LPDIRECT3DDEVICE9 device,
			const std::vector<CandidateOverlayLine>& lines)
		{
			if (!device || lines.empty())
				return false;

			HDC hdc = CreateCompatibleDC(nullptr);
			if (!hdc)
				return false;

			HFONT font = CreateFontW(
				-18,
				0,
				0,
				0,
				FW_NORMAL,
				FALSE,
				FALSE,
				FALSE,
				DEFAULT_CHARSET,
				OUT_DEFAULT_PRECIS,
				CLIP_DEFAULT_PRECIS,
				CLEARTYPE_QUALITY,
				DEFAULT_PITCH | FF_DONTCARE,
				GetOverlayFontName());
			HGDIOBJ oldFont = font ? SelectObject(hdc, font) : nullptr;

			UInt32 maxLineWidth = 0;
			for (const CandidateOverlayLine& line : lines)
			{
				SIZE current = {};
				if (GetTextExtentPoint32W(hdc, line.text.c_str(), static_cast<int>(line.text.size()), &current))
					maxLineWidth = std::max<UInt32>(maxLineWidth, static_cast<UInt32>(current.cx));
			}

			UInt32 width = std::clamp<UInt32>(maxLineWidth + kOverlayPadding * 2, kOverlayMinWidth, kOverlayMaxWidth);
			UInt32 height = kOverlayPadding * 2 + static_cast<UInt32>(lines.size()) * kOverlayLineHeight;
			width = std::max<UInt32>(width, 1);
			height = std::max<UInt32>(height, 1);

			BITMAPINFO bmi = {};
			bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bmi.bmiHeader.biWidth = static_cast<LONG>(width);
			bmi.bmiHeader.biHeight = -static_cast<LONG>(height);
			bmi.bmiHeader.biPlanes = 1;
			bmi.bmiHeader.biBitCount = 32;
			bmi.bmiHeader.biCompression = BI_RGB;

			void* pixels = nullptr;
			HBITMAP bitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
			if (!bitmap || !pixels)
			{
				if (oldFont)
					SelectObject(hdc, oldFont);
				if (font)
					DeleteObject(font);
				DeleteDC(hdc);
				return false;
			}

			HGDIOBJ oldBitmap = SelectObject(hdc, bitmap);
			RECT background = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
			HBRUSH backgroundBrush = CreateSolidBrush(RGB(18, 18, 18));
			FillRect(hdc, &background, backgroundBrush);
			DeleteObject(backgroundBrush);

			HBRUSH borderBrush = CreateSolidBrush(RGB(220, 220, 220));
			FrameRect(hdc, &background, borderBrush);
			DeleteObject(borderBrush);

			SetBkMode(hdc, TRANSPARENT);
			for (size_t i = 0; i < lines.size(); ++i)
			{
				RECT lineRect = {
					static_cast<LONG>(kOverlayPadding),
					static_cast<LONG>(kOverlayPadding + i * kOverlayLineHeight),
					static_cast<LONG>(width - kOverlayPadding),
					static_cast<LONG>(kOverlayPadding + (i + 1) * kOverlayLineHeight)
				};

				if (lines[i].highlighted)
				{
					RECT highlightRect = lineRect;
					highlightRect.left -= 4;
					highlightRect.right += 4;
					HBRUSH highlightBrush = CreateSolidBrush(RGB(58, 84, 126));
					FillRect(hdc, &highlightRect, highlightBrush);
					DeleteObject(highlightBrush);
				}

				SetTextColor(hdc, lines[i].highlighted ? RGB(255, 255, 255) : RGB(230, 230, 230));
				DrawTextW(
					hdc,
					lines[i].text.c_str(),
					static_cast<int>(lines[i].text.size()),
					&lineRect,
					DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
			}

			auto* argb = static_cast<UInt32*>(pixels);
			for (UInt32 i = 0; i < width * height; ++i)
				argb[i] |= 0xE8000000;

			if (!s_candidateOverlay.texture
				|| s_candidateOverlay.textureWidth != width
				|| s_candidateOverlay.textureHeight != height)
			{
				ReleaseCandidateOverlayTexture();
				if (FAILED(device->CreateTexture(
					width,
					height,
					1,
					0,
					D3DFMT_A8R8G8B8,
					D3DPOOL_MANAGED,
					&s_candidateOverlay.texture,
					nullptr)))
				{
					SelectObject(hdc, oldBitmap);
					DeleteObject(bitmap);
					if (oldFont)
						SelectObject(hdc, oldFont);
					if (font)
						DeleteObject(font);
					DeleteDC(hdc);
					return false;
				}

				s_candidateOverlay.textureWidth = width;
				s_candidateOverlay.textureHeight = height;
			}

			D3DLOCKED_RECT locked = {};
			if (SUCCEEDED(s_candidateOverlay.texture->LockRect(0, &locked, nullptr, 0)))
			{
				for (UInt32 y = 0; y < height; ++y)
				{
					std::memcpy(
						static_cast<UInt8*>(locked.pBits) + y * locked.Pitch,
						argb + y * width,
						width * sizeof(UInt32));
				}
				s_candidateOverlay.texture->UnlockRect(0);
			}

			SelectObject(hdc, oldBitmap);
			DeleteObject(bitmap);
			if (oldFont)
				SelectObject(hdc, oldFont);
			if (font)
				DeleteObject(font);
			DeleteDC(hdc);
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
	}

	void ReleaseCandidateOverlayTexture()
	{
		if (s_candidateOverlay.texture)
		{
			s_candidateOverlay.texture->Release();
			s_candidateOverlay.texture = nullptr;
		}
		s_candidateOverlay.textureWidth = 0;
		s_candidateOverlay.textureHeight = 0;
	}

	void HideCandidateOverlay()
	{
		s_candidateOverlay.visible = false;
		s_candidateOverlay.dirty = true;
		s_candidateOverlay.lastKey.clear();
	}

	void UpdateCandidateOverlay()
	{
		if (!g_bMultibyteInputCompositionPreview)
		{
			HideCandidateOverlay();
			return;
		}

		if (!HasOverlayInputTarget() || !s_imeCandidateState.imeOpen)
		{
			HideCandidateOverlay();
			return;
		}

		s_candidateOverlay.visible = true;
		s_candidateOverlay.dirty = true;
	}

	void DrawCandidateOverlay()
	{
		if (!g_bMultibyteInputCompositionPreview || !s_candidateOverlay.visible)
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

		if (s_candidateOverlay.dirty || key != s_candidateOverlay.lastKey || !s_candidateOverlay.texture)
		{
			if (!RenderOverlayTexture(device, lines))
				return;

			s_candidateOverlay.lastKey = std::move(key);
			s_candidateOverlay.dirty = false;
		}

		if (!s_candidateOverlay.texture)
			return;

		D3DVIEWPORT9 viewport = {};
		if (FAILED(device->GetViewport(&viewport)))
			return;

		const float x = std::max<float>(
			12.0f,
			(static_cast<float>(viewport.Width) - static_cast<float>(s_candidateOverlay.textureWidth)) * 0.5f);
		const float y = std::min<float>(
			static_cast<float>(viewport.Height) - static_cast<float>(s_candidateOverlay.textureHeight) - 12.0f,
			static_cast<float>(viewport.Height) * 0.58f);
		const float right = x + static_cast<float>(s_candidateOverlay.textureWidth);
		const float bottom = y + static_cast<float>(s_candidateOverlay.textureHeight);

		OverlayVertex vertices[4] = {
			{ x - 0.5f, y - 0.5f, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 0.0f },
			{ right - 0.5f, y - 0.5f, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 0.0f },
			{ x - 0.5f, bottom - 0.5f, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 1.0f },
			{ right - 0.5f, bottom - 0.5f, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 1.0f },
		};

		IDirect3DStateBlock9* stateBlock = nullptr;
		if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) && stateBlock)
			stateBlock->Capture();

		device->SetTexture(0, s_candidateOverlay.texture);
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
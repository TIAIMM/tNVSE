#include "font_vector_internal.h"

#include "load_config.h"
#include "tnvse.h"

#include "NiD3DPixelShader.hpp"
#include "NiDX9Renderer.hpp"
#include "NiRenderer.hpp"
#include "NiTriShape.hpp"

#include <algorithm>
#include <array>

namespace fonthook::vectorfont
{
	static_assert(sizeof(void*) == 4, "FreeType LCD rendering requires the Win32 runtime");

	namespace
	{
		constexpr UInt32 kShaderLoaderVersion = 140;
		// IDirect3DDevice9::DrawIndexedPrimitive and NiGeometry render slots.
		constexpr UInt32 kDrawIndexedPrimitiveSlot = 82;
		constexpr UInt32 kRenderImmediateSlot = 55;
		constexpr UInt32 kRenderImmediateAltSlot = 56;
		constexpr UInt32 kRendererRenderShapeSlot = 107;
		constexpr UInt32 kRendererRenderShapeAltSlot = 109;
		constexpr UInt32 kCopiedTriShapeVtableEntries = 64;
		constexpr UInt32 kShaderRefreshMessage = 0;

		using CreatePixelShaderFn = NiD3DPixelShader* (__cdecl*)(const char*);
		using DrawIndexedPrimitiveFn = HRESULT(__stdcall*)(IDirect3DDevice9*,
			D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
		using RenderImmediateFn = void(__thiscall*)(NiTriShape*, NiRenderer*);
		using RenderShapeFn = void(__thiscall*)(NiDX9Renderer*, NiTriShape*);

		std::array<NiD3DPixelShaderPtr, 3> s_lcdShaders;
		std::array<void*, kCopiedTriShapeVtableEntries + 1> s_lcdTriShapeVtable = {};
		void** s_originalTriShapeVtable = nullptr;
		RenderImmediateFn s_originalRenderImmediate = nullptr;
		RenderImmediateFn s_originalRenderImmediateAlt = nullptr;
		DrawIndexedPrimitiveFn s_originalDrawIndexedPrimitive = nullptr;
		RenderShapeFn s_originalRenderShape = nullptr;
		RenderShapeFn s_originalRenderShapeAlt = nullptr;
		IDirect3DDevice9* s_hookedDevice = nullptr;
		bool s_detectionFinalized = false;
		bool s_shaderLoaderCompatible = false;
		bool s_lcdAvailable = false;
		bool s_loggedHookConflict = false;
		thread_local UInt32 s_lcdRenderDepth = 0;

		bool HaveLcdShaders()
		{
			return s_lcdShaders[0] && s_lcdShaders[1] && s_lcdShaders[2]
				&& s_lcdShaders[0]->GetShaderHandle()
				&& s_lcdShaders[1]->GetShaderHandle()
				&& s_lcdShaders[2]->GetShaderHandle();
		}

		bool HasConfiguredLcdMode()
		{
			for (const auto& entry : g_configs)
			{
				const FontConfig& config = entry.second;
				for (const ByteStyle& style : config.styles)
				{
					if (style.renderMode != GlyphRenderMode::Gray)
						return true;
				}
			}
			return false;
		}

		bool LoadLcdShaders(CreatePixelShaderFn createPixelShader)
		{
			if (!createPixelShader)
				return false;
			static constexpr const char* kShaderFiles[] = {
				"tnvse_freetype_lcd_r.pso",
				"tnvse_freetype_lcd_g.pso",
				"tnvse_freetype_lcd_b.pso",
			};
			std::array<NiD3DPixelShaderPtr, 3> loaded;
			for (size_t i = 0; i < loaded.size(); ++i)
			{
				loaded[i] = createPixelShader(kShaderFiles[i]);
				if (!loaded[i] || !loaded[i]->GetShaderHandle())
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: Shader Loader failed to load %s; LCD rendering disabled",
						kShaderFiles[i]);
					return false;
				}
			}
			s_lcdShaders = loaded;
			if (g_bEnableFreeTypeFontRenderingLog)
				FreeTypeFontDebugLog("tnvse_freetype_font: loaded Shader Loader LCD RGB shaders");
			return true;
		}

		HRESULT __stdcall LcdDrawIndexedPrimitive(IDirect3DDevice9* device,
			D3DPRIMITIVETYPE primitiveType, INT baseVertexIndex, UINT minimumVertexIndex,
			UINT numberOfVertices, UINT startIndex, UINT primitiveCount)
		{
			if (!s_originalDrawIndexedPrimitive || !s_lcdRenderDepth || !HaveLcdShaders())
			{
				return s_originalDrawIndexedPrimitive
					? s_originalDrawIndexedPrimitive(device, primitiveType, baseVertexIndex,
						minimumVertexIndex, numberOfVertices, startIndex, primitiveCount)
					: D3DERR_INVALIDCALL;
			}

			IDirect3DPixelShader9* originalPixelShader = nullptr;
			DWORD originalColorWrite = 0;
			DWORD originalAlphaBlend = 0;
			DWORD originalAlphaTest = 0;
			DWORD originalSourceBlend = 0;
			DWORD originalDestinationBlend = 0;
			DWORD originalZWrite = 0;
			device->GetPixelShader(&originalPixelShader);
			device->GetRenderState(D3DRS_COLORWRITEENABLE, &originalColorWrite);
			device->GetRenderState(D3DRS_ALPHABLENDENABLE, &originalAlphaBlend);
			device->GetRenderState(D3DRS_ALPHATESTENABLE, &originalAlphaTest);
			device->GetRenderState(D3DRS_SRCBLEND, &originalSourceBlend);
			device->GetRenderState(D3DRS_DESTBLEND, &originalDestinationBlend);
			device->GetRenderState(D3DRS_ZWRITEENABLE, &originalZWrite);

			device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
			device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
			device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
			device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
			device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

			static constexpr DWORD kWriteMasks[] = {
				D3DCOLORWRITEENABLE_RED,
				D3DCOLORWRITEENABLE_GREEN,
				D3DCOLORWRITEENABLE_BLUE,
			};
			HRESULT result = D3D_OK;
			for (size_t i = 0; i < s_lcdShaders.size(); ++i)
			{
				device->SetPixelShader(s_lcdShaders[i]->GetShaderHandle());
				device->SetRenderState(D3DRS_COLORWRITEENABLE,
					kWriteMasks[i] & originalColorWrite);
				const HRESULT passResult = s_originalDrawIndexedPrimitive(device,
					primitiveType, baseVertexIndex, minimumVertexIndex, numberOfVertices,
					startIndex, primitiveCount);
				if (FAILED(passResult))
					result = passResult;
			}

			device->SetPixelShader(originalPixelShader);
			device->SetRenderState(D3DRS_COLORWRITEENABLE, originalColorWrite);
			device->SetRenderState(D3DRS_ALPHABLENDENABLE, originalAlphaBlend);
			device->SetRenderState(D3DRS_ALPHATESTENABLE, originalAlphaTest);
			device->SetRenderState(D3DRS_SRCBLEND, originalSourceBlend);
			device->SetRenderState(D3DRS_DESTBLEND, originalDestinationBlend);
			device->SetRenderState(D3DRS_ZWRITEENABLE, originalZWrite);
			if (originalPixelShader)
				originalPixelShader->Release();
			return result;
		}

		bool IsLcdAtlasShape(const NiTriShape* shape)
		{
			return shape && *reinterpret_cast<void* const* const*>(shape)
				== &s_lcdTriShapeVtable[1];
		}

		void __fastcall LcdRenderShape(NiDX9Renderer* renderer, void*, NiTriShape* shape)
		{
			const bool lcd = IsLcdAtlasShape(shape);
			if (lcd)
				++s_lcdRenderDepth;
			s_originalRenderShape(renderer, shape);
			if (lcd)
				--s_lcdRenderDepth;
		}

		void __fastcall LcdRenderShapeAlt(NiDX9Renderer* renderer, void*, NiTriShape* shape)
		{
			const bool lcd = IsLcdAtlasShape(shape);
			if (lcd)
				++s_lcdRenderDepth;
			s_originalRenderShapeAlt(renderer, shape);
			if (lcd)
				--s_lcdRenderDepth;
		}

		bool WriteVtableEntry(void** vtable, UInt32 slot, void* replacement)
		{
			DWORD oldProtect = 0;
			if (!VirtualProtect(&vtable[slot], sizeof(void*),
				PAGE_EXECUTE_READWRITE, &oldProtect))
				return false;
			vtable[slot] = replacement;
			DWORD ignored = 0;
			VirtualProtect(&vtable[slot], sizeof(void*), oldProtect, &ignored);
			FlushInstructionCache(GetCurrentProcess(), &vtable[slot], sizeof(void*));
			return true;
		}

		bool HookRendererShapeEntries(NiDX9Renderer* renderer)
		{
			void** vtable = renderer ? *reinterpret_cast<void***>(renderer) : nullptr;
			if (!vtable)
				return false;
			if (vtable[kRendererRenderShapeSlot] != reinterpret_cast<void*>(&LcdRenderShape))
			{
				if (s_originalRenderShape)
					return false;
				s_originalRenderShape = reinterpret_cast<RenderShapeFn>(
					vtable[kRendererRenderShapeSlot]);
				if (!WriteVtableEntry(vtable, kRendererRenderShapeSlot,
					reinterpret_cast<void*>(&LcdRenderShape)))
					return false;
			}
			if (vtable[kRendererRenderShapeAltSlot] != reinterpret_cast<void*>(&LcdRenderShapeAlt))
			{
				if (s_originalRenderShapeAlt)
					return false;
				s_originalRenderShapeAlt = reinterpret_cast<RenderShapeFn>(
					vtable[kRendererRenderShapeAltSlot]);
				if (!WriteVtableEntry(vtable, kRendererRenderShapeAltSlot,
					reinterpret_cast<void*>(&LcdRenderShapeAlt)))
					return false;
			}
			return s_originalRenderShape && s_originalRenderShapeAlt;
		}

		bool HookD3DDevice()
		{
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer ? renderer->GetD3DDevice() : nullptr;
			if (!device)
				return false;
			void** vtable = *reinterpret_cast<void***>(device);
			if (!vtable)
				return false;
			if (vtable[kDrawIndexedPrimitiveSlot]
				== reinterpret_cast<void*>(&LcdDrawIndexedPrimitive))
			{
				s_hookedDevice = device;
				return s_originalDrawIndexedPrimitive != nullptr
					&& HookRendererShapeEntries(renderer);
			}
			if (s_hookedDevice && s_hookedDevice == device)
			{
				if (!s_loggedHookConflict)
				{
					s_loggedHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: D3D9 DrawIndexedPrimitive hook was replaced; LCD rendering disabled");
				}
				return false;
			}

			s_originalDrawIndexedPrimitive = reinterpret_cast<DrawIndexedPrimitiveFn>(
				vtable[kDrawIndexedPrimitiveSlot]);
			if (!WriteVtableEntry(vtable, kDrawIndexedPrimitiveSlot,
				reinterpret_cast<void*>(&LcdDrawIndexedPrimitive)))
				return false;
			s_hookedDevice = device;
			if (g_bEnableFreeTypeFontRenderingLog)
				FreeTypeFontDebugLog("tnvse_freetype_font: installed LCD D3D9 draw bridge");
			return HookRendererShapeEntries(renderer);
		}

		void __fastcall LcdRenderImmediate(NiTriShape* shape, void*, NiRenderer* renderer)
		{
			++s_lcdRenderDepth;
			s_originalRenderImmediate(shape, renderer);
			--s_lcdRenderDepth;
		}

		void __fastcall LcdRenderImmediateAlt(NiTriShape* shape, void*, NiRenderer* renderer)
		{
			++s_lcdRenderDepth;
			s_originalRenderImmediateAlt(shape, renderer);
			--s_lcdRenderDepth;
		}

		bool InitializeLcdTriShapeVtable(NiTriShape* shape)
		{
			void** source = shape ? *reinterpret_cast<void***>(shape) : nullptr;
			if (!source)
				return false;
			if (source == &s_lcdTriShapeVtable[1])
				return true;
			if (s_originalTriShapeVtable)
				return source == s_originalTriShapeVtable;

			s_originalTriShapeVtable = source;
			s_lcdTriShapeVtable[0] = source[-1];
			std::copy(source, source + kCopiedTriShapeVtableEntries,
				s_lcdTriShapeVtable.begin() + 1);
			s_originalRenderImmediate = reinterpret_cast<RenderImmediateFn>(
				s_lcdTriShapeVtable[kRenderImmediateSlot + 1]);
			s_originalRenderImmediateAlt = reinterpret_cast<RenderImmediateFn>(
				s_lcdTriShapeVtable[kRenderImmediateAltSlot + 1]);
			s_lcdTriShapeVtable[kRenderImmediateSlot + 1]
				= reinterpret_cast<void*>(&LcdRenderImmediate);
			s_lcdTriShapeVtable[kRenderImmediateAltSlot + 1]
				= reinterpret_cast<void*>(&LcdRenderImmediateAlt);
			return s_originalRenderImmediate && s_originalRenderImmediateAlt;
		}
	}

	void FinalizeLcdRendererDetection()
	{
		if (s_detectionFinalized)
			return;
		s_detectionFinalized = true;
		if (!g_bEnableFreeTypeFontRendering)
			return;
		if (!HasConfiguredLcdMode())
			return;
		if (!g_cmdTableInterface || !g_cmdTableInterface->GetPluginInfoByDLLName)
			return;

		const PluginInfo* info = g_cmdTableInterface->GetPluginInfoByDLLName(
			"Fallout Shader Loader.dll");
		if (!info || info->infoVersion != PluginInfo::kInfoVersion
			|| info->version != kShaderLoaderVersion)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: Fallout Shader Loader 1.40 is required for LCD rendering; LCD styles use gray rendering");
			return;
		}
		HMODULE module = GetModuleHandleA("Fallout Shader Loader.dll");
		const auto createPixelShader = module
			? reinterpret_cast<CreatePixelShaderFn>(GetProcAddress(module, "CreatePixelShader"))
			: nullptr;
		if (!createPixelShader)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: Shader Loader CreatePixelShader export is unavailable; LCD styles use gray rendering");
			return;
		}
		s_shaderLoaderCompatible = true;
		s_lcdAvailable = LoadLcdShaders(createPixelShader) && HookD3DDevice();
	}

	void HandleLcdRendererMainLoop()
	{
		if (!s_detectionFinalized || !s_shaderLoaderCompatible || !HaveLcdShaders())
			return;
		s_lcdAvailable = HookD3DDevice();
	}

	void HandleShaderLoaderMessage(UInt32 messageType)
	{
		if (messageType != kShaderRefreshMessage || !s_shaderLoaderCompatible)
			return;
		HMODULE module = GetModuleHandleA("Fallout Shader Loader.dll");
		const auto createPixelShader = module
			? reinterpret_cast<CreatePixelShaderFn>(GetProcAddress(module, "CreatePixelShader"))
			: nullptr;
		if (LoadLcdShaders(createPixelShader))
			s_lcdAvailable = HookD3DDevice();
	}

	bool IsLcdRendererAvailable()
	{
		return s_lcdAvailable && HaveLcdShaders();
	}

	bool PrepareLcdAtlasShape(NiTriShape* shape)
	{
		if (!IsLcdRendererAvailable() || !InitializeLcdTriShapeVtable(shape))
			return false;
		*reinterpret_cast<void***>(shape) = &s_lcdTriShapeVtable[1];
		return true;
	}
}

namespace fonthook
{
	void FinalizeFreeTypeLcdDetection()
	{
		vectorfont::FinalizeLcdRendererDetection();
	}

	void HandleFreeTypeLcdMainLoop()
	{
		vectorfont::HandleLcdRendererMainLoop();
	}

	void HandleFreeTypeShaderLoaderMessage(UInt32 messageType)
	{
		vectorfont::HandleShaderLoaderMessage(messageType);
	}
}

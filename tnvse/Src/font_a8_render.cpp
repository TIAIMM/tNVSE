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
	static_assert(sizeof(void*) == 4, "FreeType A8 rendering requires the Win32 runtime");

	namespace
	{
		constexpr UInt32 kMinimumShaderLoaderVersion = 140;
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

		NiD3DPixelShaderPtr s_a8Shader;
		std::array<void*, kCopiedTriShapeVtableEntries + 1> s_a8TriShapeVtable = {};
		void** s_originalTriShapeVtable = nullptr;
		RenderImmediateFn s_originalRenderImmediate = nullptr;
		RenderImmediateFn s_originalRenderImmediateAlt = nullptr;
		DrawIndexedPrimitiveFn s_originalDrawIndexedPrimitive = nullptr;
		RenderShapeFn s_originalRenderShape = nullptr;
		RenderShapeFn s_originalRenderShapeAlt = nullptr;
		IDirect3DDevice9* s_hookedDevice = nullptr;
		bool s_detectionFinalized = false;
		bool s_shaderLoaderCompatible = false;
		bool s_a8Available = false;
		bool s_loggedHookConflict = false;
		thread_local UInt32 s_a8RenderDepth = 0;

		bool HaveA8Shader()
		{
			return s_a8Shader && s_a8Shader->GetShaderHandle();
		}

		bool LoadA8Shader(CreatePixelShaderFn createPixelShader)
		{
			if (!createPixelShader)
				return false;
			NiD3DPixelShaderPtr loaded = createPixelShader("tnvse_freetype_a8.pso");
			if (!loaded || !loaded->GetShaderHandle())
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: Shader Loader failed to load tnvse_freetype_a8.pso; using 32-bit atlases");
				return false;
			}
			s_a8Shader = loaded;
			if (g_bEnableFreeTypeFontRenderingLog)
				FreeTypeFontDebugLog("tnvse_freetype_font: loaded Shader Loader A8 font shader");
			return true;
		}

		HRESULT __stdcall A8DrawIndexedPrimitive(IDirect3DDevice9* device,
			D3DPRIMITIVETYPE primitiveType, INT baseVertexIndex, UINT minimumVertexIndex,
			UINT numberOfVertices, UINT startIndex, UINT primitiveCount)
		{
			if (!s_originalDrawIndexedPrimitive)
				return D3DERR_INVALIDCALL;
			if (!s_a8RenderDepth || !HaveA8Shader())
			{
				return s_originalDrawIndexedPrimitive(device, primitiveType, baseVertexIndex,
					minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
			}

			IDirect3DPixelShader9* originalPixelShader = nullptr;
			device->GetPixelShader(&originalPixelShader);
			device->SetPixelShader(s_a8Shader->GetShaderHandle());
			const HRESULT result = s_originalDrawIndexedPrimitive(device, primitiveType,
				baseVertexIndex, minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
			device->SetPixelShader(originalPixelShader);
			if (originalPixelShader)
				originalPixelShader->Release();
			return result;
		}

		bool IsA8AtlasShape(const NiTriShape* shape)
		{
			return shape && *reinterpret_cast<void* const* const*>(shape)
				== &s_a8TriShapeVtable[1];
		}

		void __fastcall A8RenderShape(NiDX9Renderer* renderer, void*, NiTriShape* shape)
		{
			const bool a8 = IsA8AtlasShape(shape);
			if (a8)
				++s_a8RenderDepth;
			s_originalRenderShape(renderer, shape);
			if (a8)
				--s_a8RenderDepth;
		}

		void __fastcall A8RenderShapeAlt(NiDX9Renderer* renderer, void*, NiTriShape* shape)
		{
			const bool a8 = IsA8AtlasShape(shape);
			if (a8)
				++s_a8RenderDepth;
			s_originalRenderShapeAlt(renderer, shape);
			if (a8)
				--s_a8RenderDepth;
		}

		bool WriteVtableEntry(void** vtable, UInt32 slot, void* replacement)
		{
			DWORD oldProtect = 0;
			if (!VirtualProtect(&vtable[slot], sizeof(void*),
				PAGE_EXECUTE_READWRITE, &oldProtect))
			{
				return false;
			}
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
			if (vtable[kRendererRenderShapeSlot] != reinterpret_cast<void*>(&A8RenderShape))
			{
				if (s_originalRenderShape)
					return false;
				s_originalRenderShape = reinterpret_cast<RenderShapeFn>(
					vtable[kRendererRenderShapeSlot]);
				if (!WriteVtableEntry(vtable, kRendererRenderShapeSlot,
					reinterpret_cast<void*>(&A8RenderShape)))
				{
					return false;
				}
			}
			if (vtable[kRendererRenderShapeAltSlot] != reinterpret_cast<void*>(&A8RenderShapeAlt))
			{
				if (s_originalRenderShapeAlt)
					return false;
				s_originalRenderShapeAlt = reinterpret_cast<RenderShapeFn>(
					vtable[kRendererRenderShapeAltSlot]);
				if (!WriteVtableEntry(vtable, kRendererRenderShapeAltSlot,
					reinterpret_cast<void*>(&A8RenderShapeAlt)))
				{
					return false;
				}
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
				== reinterpret_cast<void*>(&A8DrawIndexedPrimitive))
			{
				s_hookedDevice = device;
				return s_originalDrawIndexedPrimitive && HookRendererShapeEntries(renderer);
			}
			if (s_hookedDevice && s_hookedDevice == device)
			{
				if (!s_loggedHookConflict)
				{
					s_loggedHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: D3D9 draw bridge was replaced; new atlases use 32-bit fallback");
				}
				return false;
			}

			s_originalDrawIndexedPrimitive = reinterpret_cast<DrawIndexedPrimitiveFn>(
				vtable[kDrawIndexedPrimitiveSlot]);
			if (!WriteVtableEntry(vtable, kDrawIndexedPrimitiveSlot,
				reinterpret_cast<void*>(&A8DrawIndexedPrimitive)))
			{
				return false;
			}
			s_hookedDevice = device;
			if (g_bEnableFreeTypeFontRenderingLog)
				FreeTypeFontDebugLog("tnvse_freetype_font: installed A8 D3D9 draw bridge");
			return HookRendererShapeEntries(renderer);
		}

		void __fastcall A8RenderImmediate(NiTriShape* shape, void*, NiRenderer* renderer)
		{
			++s_a8RenderDepth;
			s_originalRenderImmediate(shape, renderer);
			--s_a8RenderDepth;
		}

		void __fastcall A8RenderImmediateAlt(NiTriShape* shape, void*, NiRenderer* renderer)
		{
			++s_a8RenderDepth;
			s_originalRenderImmediateAlt(shape, renderer);
			--s_a8RenderDepth;
		}

		bool InitializeA8TriShapeVtable(NiTriShape* shape)
		{
			void** source = shape ? *reinterpret_cast<void***>(shape) : nullptr;
			if (!source)
				return false;
			if (source == &s_a8TriShapeVtable[1])
				return true;
			if (s_originalTriShapeVtable)
				return source == s_originalTriShapeVtable;

			s_originalTriShapeVtable = source;
			s_a8TriShapeVtable[0] = source[-1];
			std::copy(source, source + kCopiedTriShapeVtableEntries,
				s_a8TriShapeVtable.begin() + 1);
			s_originalRenderImmediate = reinterpret_cast<RenderImmediateFn>(
				s_a8TriShapeVtable[kRenderImmediateSlot + 1]);
			s_originalRenderImmediateAlt = reinterpret_cast<RenderImmediateFn>(
				s_a8TriShapeVtable[kRenderImmediateAltSlot + 1]);
			s_a8TriShapeVtable[kRenderImmediateSlot + 1]
				= reinterpret_cast<void*>(&A8RenderImmediate);
			s_a8TriShapeVtable[kRenderImmediateAltSlot + 1]
				= reinterpret_cast<void*>(&A8RenderImmediateAlt);
			return s_originalRenderImmediate && s_originalRenderImmediateAlt;
		}
	}

	void FinalizeA8RendererDetection()
	{
		if (s_detectionFinalized)
			return;
		s_detectionFinalized = true;
		if (!g_bEnableFreeTypeFontRendering || !g_cmdTableInterface
			|| !g_cmdTableInterface->GetPluginInfoByDLLName)
		{
			return;
		}

		const PluginInfo* info = g_cmdTableInterface->GetPluginInfoByDLLName(
			"Fallout Shader Loader.dll");
		if (!info || info->infoVersion != PluginInfo::kInfoVersion
			|| info->version < kMinimumShaderLoaderVersion)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: Fallout Shader Loader 1.40 or newer is unavailable; using 32-bit atlases");
			return;
		}
		HMODULE module = GetModuleHandleA("Fallout Shader Loader.dll");
		const auto createPixelShader = module
			? reinterpret_cast<CreatePixelShaderFn>(GetProcAddress(module, "CreatePixelShader"))
			: nullptr;
		if (!createPixelShader)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: Shader Loader CreatePixelShader export is unavailable; using 32-bit atlases");
			return;
		}
		s_shaderLoaderCompatible = true;
		s_a8Available = LoadA8Shader(createPixelShader) && HookD3DDevice();
	}

	void HandleA8RendererMainLoop()
	{
		if (!s_detectionFinalized || !s_shaderLoaderCompatible || !HaveA8Shader())
			return;
		s_a8Available = HookD3DDevice();
	}

	void HandleA8ShaderLoaderMessage(UInt32 messageType)
	{
		if (messageType != kShaderRefreshMessage || !s_shaderLoaderCompatible)
			return;
		HMODULE module = GetModuleHandleA("Fallout Shader Loader.dll");
		const auto createPixelShader = module
			? reinterpret_cast<CreatePixelShaderFn>(GetProcAddress(module, "CreatePixelShader"))
			: nullptr;
		if (LoadA8Shader(createPixelShader))
			s_a8Available = HookD3DDevice();
	}

	bool IsA8RendererAvailable()
	{
		return s_a8Available && HaveA8Shader();
	}

	bool PrepareA8AtlasShape(NiTriShape* shape)
	{
		if (!IsA8RendererAvailable() || !InitializeA8TriShapeVtable(shape))
			return false;
		*reinterpret_cast<void***>(shape) = &s_a8TriShapeVtable[1];
		return true;
	}
}

namespace fonthook
{
	void FinalizeFreeTypeA8Detection()
	{
		vectorfont::FinalizeA8RendererDetection();
	}

	void HandleFreeTypeA8MainLoop()
	{
		vectorfont::HandleA8RendererMainLoop();
	}

	void HandleFreeTypeShaderLoaderMessage(UInt32 messageType)
	{
		vectorfont::HandleA8ShaderLoaderMessage(messageType);
	}
}

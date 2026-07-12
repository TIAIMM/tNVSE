#include "font_vector_internal.h"

#include "load_config.h"
#include "plugin_dependencies.h"
#include "tnvse.h"

#include "NiD3DPixelShader.hpp"
#include "NiDX9Renderer.hpp"
#include "NiAlphaProperty.hpp"
#include "NiRenderer.hpp"
#include "NiTriShape.hpp"
#include "NiTriShapeData.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace fonthook::vectorfont
{
	static_assert(sizeof(void*) == 4, "FreeType A8 rendering requires the Win32 runtime");

	namespace
	{
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
		thread_local NiTriShape* s_currentA8Shape = nullptr;

		struct A8ShapeDiagnostics
		{
			UInt32 fontId = 0;
			UInt32 glyphCount = 0;
			UInt32 quadCount = 0;
		};

		std::mutex s_diagnosticsMutex;
		std::unordered_map<const NiTriShape*, A8ShapeDiagnostics> s_shapeDiagnostics;
		std::unordered_set<const NiTriShape*> s_loggedShapes;
		UInt32 s_diagnosticLogCount = 0;
		constexpr UInt32 kMaximumDiagnosticShapes = 128;

		void LogA8DrawDiagnostics(IDirect3DDevice9* device,
			D3DPRIMITIVETYPE primitiveType, UINT numberOfVertices,
			UINT startIndex, UINT primitiveCount)
		{
			if (!g_bEnableFreeTypeFontRenderingLog || !device || !s_currentA8Shape)
				return;

			A8ShapeDiagnostics metadata;
			{
				std::lock_guard<std::mutex> lock(s_diagnosticsMutex);
				if (s_diagnosticLogCount >= kMaximumDiagnosticShapes
					|| !s_loggedShapes.insert(s_currentA8Shape).second)
				{
					return;
				}
				++s_diagnosticLogCount;
				auto found = s_shapeDiagnostics.find(s_currentA8Shape);
				if (found != s_shapeDiagnostics.end())
					metadata = found->second;
			}

			IDirect3DPixelShader9* pixelShader = nullptr;
			IDirect3DVertexShader9* vertexShader = nullptr;
			IDirect3DBaseTexture9* texture = nullptr;
			device->GetPixelShader(&pixelShader);
			device->GetVertexShader(&vertexShader);
			device->GetTexture(0, &texture);

			DWORD alphaBlend = 0, sourceBlend = 0, destinationBlend = 0, blendOperation = 0;
			DWORD separateAlpha = 0, alphaTest = 0, alphaFunction = 0, alphaReference = 0;
			DWORD zWrite = 0, colorWrite = 0, textureFactor = 0;
			DWORD minFilter = 0, magFilter = 0, mipFilter = 0;
			device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
			device->GetRenderState(D3DRS_SRCBLEND, &sourceBlend);
			device->GetRenderState(D3DRS_DESTBLEND, &destinationBlend);
			device->GetRenderState(D3DRS_BLENDOP, &blendOperation);
			device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &separateAlpha);
			device->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
			device->GetRenderState(D3DRS_ALPHAFUNC, &alphaFunction);
			device->GetRenderState(D3DRS_ALPHAREF, &alphaReference);
			device->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
			device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
			device->GetRenderState(D3DRS_TEXTUREFACTOR, &textureFactor);
			device->GetSamplerState(0, D3DSAMP_MINFILTER, &minFilter);
			device->GetSamplerState(0, D3DSAMP_MAGFILTER, &magFilter);
			device->GetSamplerState(0, D3DSAMP_MIPFILTER, &mipFilter);

			std::array<float, 16> pixelConstants = {};
			std::array<float, 32> vertexConstants = {};
			device->GetPixelShaderConstantF(0, pixelConstants.data(), 4);
			device->GetVertexShaderConstantF(4, vertexConstants.data(), 8);

			float minimumAlpha = std::numeric_limits<float>::infinity();
			float maximumAlpha = -std::numeric_limits<float>::infinity();
			NiPoint3 minimumVertex(std::numeric_limits<float>::infinity(),
				std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity());
			NiPoint3 maximumVertex(-std::numeric_limits<float>::infinity(),
				-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity());
			NiPoint2 minimumUv(std::numeric_limits<float>::infinity(),
				std::numeric_limits<float>::infinity());
			NiPoint2 maximumUv(-std::numeric_limits<float>::infinity(),
				-std::numeric_limits<float>::infinity());
			UInt32 vertexCount = 0;
			UInt32 triangleCount = 0;
			if (NiTriShapeData* data = s_currentA8Shape->GetModelData())
			{
				vertexCount = data->m_usVertices;
				triangleCount = data->m_uiTriListLength / 3;
				for (UInt32 index = 0; index < vertexCount; ++index)
				{
					if (data->m_pkVertex)
					{
						const NiPoint3& vertex = data->m_pkVertex[index];
						minimumVertex.x = std::min(minimumVertex.x, vertex.x);
						minimumVertex.y = std::min(minimumVertex.y, vertex.y);
						minimumVertex.z = std::min(minimumVertex.z, vertex.z);
						maximumVertex.x = std::max(maximumVertex.x, vertex.x);
						maximumVertex.y = std::max(maximumVertex.y, vertex.y);
						maximumVertex.z = std::max(maximumVertex.z, vertex.z);
					}
					if (data->m_pkTexture)
					{
						const NiPoint2& uv = data->m_pkTexture[index];
						minimumUv.x = std::min(minimumUv.x, uv.x);
						minimumUv.y = std::min(minimumUv.y, uv.y);
						maximumUv.x = std::max(maximumUv.x, uv.x);
						maximumUv.y = std::max(maximumUv.y, uv.y);
					}
					if (data->m_pkColor)
					{
						minimumAlpha = std::min(minimumAlpha, data->m_pkColor[index].a);
						maximumAlpha = std::max(maximumAlpha, data->m_pkColor[index].a);
					}
				}
			}
			if (!std::isfinite(minimumAlpha))
				minimumAlpha = maximumAlpha = -1.0f;
			if (!std::isfinite(minimumVertex.x))
				minimumVertex = maximumVertex = NiPoint3(-1.0f, -1.0f, -1.0f);
			if (!std::isfinite(minimumUv.x))
				minimumUv = maximumUv = NiPoint2(-1.0f, -1.0f);

			UInt16 alphaFlags = 0;
			UInt8 propertyAlphaReference = 0;
			if (NiAlphaProperty* property = s_currentA8Shape->GetAlphaProperty())
			{
				alphaFlags = property->m_usFlags.Get();
				propertyAlphaReference = property->m_ucAlphaTestRef;
			}
			NiColorA shadeColor = { -1.0f, -1.0f, -1.0f, -1.0f };
			if (NiShadeProperty* shade = s_currentA8Shape->GetShadeProperty())
				shadeColor = *reinterpret_cast<NiColorA*>(reinterpret_cast<UInt8*>(shade) + 0x68);

			FreeTypeFontDebugLog(
				"tnvse_freetype_a8_diag: shape=%p parent=%p font=%u glyphs=%u quads=%u vertices=%u triangles=%u draw=(type=%u vertices=%u start=%u primitives=%u)",
				s_currentA8Shape, s_currentA8Shape->m_pkParent, metadata.fontId,
				metadata.glyphCount, metadata.quadCount, vertexCount, triangleCount,
				static_cast<UInt32>(primitiveType), numberOfVertices, startIndex, primitiveCount);
			FreeTypeFontDebugLog(
				"tnvse_freetype_a8_diag:   local=(%.3f,%.3f,%.3f scale=%.3f) world=(%.3f,%.3f,%.3f scale=%.3f) vertexAlpha=[%.4f,%.4f] shadeColor=(%.4f,%.4f,%.4f,%.4f) alphaProperty=(flags=0x%04X ref=%u)",
				s_currentA8Shape->m_kLocal.m_Translate.x, s_currentA8Shape->m_kLocal.m_Translate.y,
				s_currentA8Shape->m_kLocal.m_Translate.z, s_currentA8Shape->m_kLocal.m_fScale,
				s_currentA8Shape->m_kWorld.m_Translate.x, s_currentA8Shape->m_kWorld.m_Translate.y,
				s_currentA8Shape->m_kWorld.m_Translate.z, s_currentA8Shape->m_kWorld.m_fScale,
				minimumAlpha, maximumAlpha, shadeColor.r, shadeColor.g, shadeColor.b, shadeColor.a,
				alphaFlags, propertyAlphaReference);
			FreeTypeFontDebugLog(
				"tnvse_freetype_a8_diag:   geometry xyz=[(%.3f,%.3f,%.3f)..(%.3f,%.3f,%.3f)] uv=[(%.6f,%.6f)..(%.6f,%.6f)]",
				minimumVertex.x, minimumVertex.y, minimumVertex.z,
				maximumVertex.x, maximumVertex.y, maximumVertex.z,
				minimumUv.x, minimumUv.y, maximumUv.x, maximumUv.y);
			FreeTypeFontDebugLog(
				"tnvse_freetype_a8_diag:   d3d ps=%p vs=%p tex0=%p blend=(enable=%u src=%u dst=%u op=%u separate=%u) alphaTest=(enable=%u func=%u ref=%u) zWrite=%u colorWrite=0x%X textureFactor=0x%08X sampler=(min=%u mag=%u mip=%u)",
				pixelShader, vertexShader, texture, alphaBlend, sourceBlend, destinationBlend,
				blendOperation, separateAlpha, alphaTest, alphaFunction, alphaReference,
				zWrite, colorWrite, textureFactor, minFilter, magFilter, mipFilter);
			FreeTypeFontDebugLog(
				"tnvse_freetype_a8_diag:   ps_c0_c3=(%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g)",
				pixelConstants[0], pixelConstants[1], pixelConstants[2], pixelConstants[3],
				pixelConstants[4], pixelConstants[5], pixelConstants[6], pixelConstants[7],
				pixelConstants[8], pixelConstants[9], pixelConstants[10], pixelConstants[11],
				pixelConstants[12], pixelConstants[13], pixelConstants[14], pixelConstants[15]);
			for (UInt32 row = 0; row < 2; ++row)
			{
				const UInt32 offset = row * 16;
				FreeTypeFontDebugLog(
					"tnvse_freetype_a8_diag:   vs_c%u_c%u=(%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g)",
					4 + row * 4, 7 + row * 4,
					vertexConstants[offset + 0], vertexConstants[offset + 1],
					vertexConstants[offset + 2], vertexConstants[offset + 3],
					vertexConstants[offset + 4], vertexConstants[offset + 5],
					vertexConstants[offset + 6], vertexConstants[offset + 7],
					vertexConstants[offset + 8], vertexConstants[offset + 9],
					vertexConstants[offset + 10], vertexConstants[offset + 11],
					vertexConstants[offset + 12], vertexConstants[offset + 13],
					vertexConstants[offset + 14], vertexConstants[offset + 15]);
			}

			if (texture)
				texture->Release();
			if (vertexShader)
				vertexShader->Release();
			if (pixelShader)
				pixelShader->Release();
		}

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

			LogA8DrawDiagnostics(device, primitiveType, numberOfVertices,
				startIndex, primitiveCount);

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
			NiTriShape* previousShape = s_currentA8Shape;
			if (a8)
			{
				++s_a8RenderDepth;
				s_currentA8Shape = shape;
			}
			s_originalRenderShape(renderer, shape);
			if (a8)
			{
				s_currentA8Shape = previousShape;
				--s_a8RenderDepth;
			}
		}

		void __fastcall A8RenderShapeAlt(NiDX9Renderer* renderer, void*, NiTriShape* shape)
		{
			const bool a8 = IsA8AtlasShape(shape);
			NiTriShape* previousShape = s_currentA8Shape;
			if (a8)
			{
				++s_a8RenderDepth;
				s_currentA8Shape = shape;
			}
			s_originalRenderShapeAlt(renderer, shape);
			if (a8)
			{
				s_currentA8Shape = previousShape;
				--s_a8RenderDepth;
			}
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
			NiTriShape* previousShape = s_currentA8Shape;
			++s_a8RenderDepth;
			s_currentA8Shape = shape;
			s_originalRenderImmediate(shape, renderer);
			s_currentA8Shape = previousShape;
			--s_a8RenderDepth;
		}

		void __fastcall A8RenderImmediateAlt(NiTriShape* shape, void*, NiRenderer* renderer)
		{
			NiTriShape* previousShape = s_currentA8Shape;
			++s_a8RenderDepth;
			s_currentA8Shape = shape;
			s_originalRenderImmediateAlt(shape, renderer);
			s_currentA8Shape = previousShape;
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
		if (!g_bEnableFreeTypeFontRendering || !g_bEnableFreeTypeA8Atlas
			|| !g_cmdTableInterface
			|| !g_cmdTableInterface->GetPluginInfoByDLLName)
		{
			return;
		}

		const PluginInfo* info = g_cmdTableInterface->GetPluginInfoByDLLName(
			"Fallout Shader Loader.dll");
		if (!info || info->infoVersion != PluginInfo::kInfoVersion
			|| info->version < dependencies::kShaderLoaderMinVersion)
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

	bool PrepareA8AtlasShape(NiTriShape* shape, UInt32 fontId,
		UInt32 glyphCount, UInt32 quadCount)
	{
		if (!IsA8RendererAvailable() || !InitializeA8TriShapeVtable(shape))
			return false;
		*reinterpret_cast<void***>(shape) = &s_a8TriShapeVtable[1];
		if (g_bEnableFreeTypeFontRenderingLog)
		{
			std::lock_guard<std::mutex> lock(s_diagnosticsMutex);
			if (s_shapeDiagnostics.size() >= 4096)
				s_shapeDiagnostics.clear();
			s_shapeDiagnostics[shape] = { fontId, glyphCount, quadCount };
		}
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

#include "font_vector_internal.h"

#include "load_config.h"
#include "plugin_dependencies.h"
#include "tnvse.h"

#include "NiD3DPixelShader.hpp"
#include "NiDX9RenderState.hpp"
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
		constexpr UInt32 kDeleteThisSlot = 1;
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
		using DeleteThisFn = void(__thiscall*)(NiTriShape*);

		NiD3DPixelShaderPtr s_a8Shader;
		std::array<NiD3DPixelShaderPtr, 3> s_effectShaders;
		std::array<void*, kCopiedTriShapeVtableEntries + 1> s_a8TriShapeVtable = {};
		void** s_originalTriShapeVtable = nullptr;
		RenderImmediateFn s_originalRenderImmediate = nullptr;
		RenderImmediateFn s_originalRenderImmediateAlt = nullptr;
		DrawIndexedPrimitiveFn s_originalDrawIndexedPrimitive = nullptr;
		RenderShapeFn s_originalRenderShape = nullptr;
		RenderShapeFn s_originalRenderShapeAlt = nullptr;
		DeleteThisFn s_originalDeleteThis = nullptr;
		IDirect3DDevice9* s_hookedDevice = nullptr;
		bool s_detectionFinalized = false;
		bool s_shaderLoaderCompatible = false;
		bool s_a8Available = false;
		bool s_loggedHookConflict = false;
		thread_local UInt32 s_a8RenderDepth = 0;
		thread_local NiTriShape* s_currentA8Shape = nullptr;
		UInt32 s_stateMismatchLogCount = 0;
		UInt32 s_shadowContractLogCount = 0;
		constexpr UInt32 kMaximumStateMismatchLogs = 16;

		struct A8ShapeMetadata
		{
			UInt32 fontId = 0;
			UInt32 glyphCount = 0;
			UInt32 quadCount = 0;
			A8ShapeColorContract colorContract;
			A8EffectShapeConfig effects;
		};

		std::mutex s_diagnosticsMutex;
		std::unordered_map<const NiTriShape*, A8ShapeMetadata> s_shapeMetadata;
		std::unordered_set<const NiTriShape*> s_loggedShapes;
		UInt32 s_diagnosticLogCount = 0;
		constexpr UInt32 kMaximumDiagnosticShapes = 128;

		void LogA8DrawDiagnostics(IDirect3DDevice9* device,
			D3DPRIMITIVETYPE primitiveType, UINT numberOfVertices,
			UINT startIndex, UINT primitiveCount)
		{
			if (!g_bEnableFreeTypeFontRenderingLog || !device || !s_currentA8Shape)
				return;

			A8ShapeMetadata metadata;
			{
				std::lock_guard<std::mutex> lock(s_diagnosticsMutex);
				if (s_diagnosticLogCount >= kMaximumDiagnosticShapes
					|| !s_loggedShapes.insert(s_currentA8Shape).second)
				{
					return;
				}
				++s_diagnosticLogCount;
				auto found = s_shapeMetadata.find(s_currentA8Shape);
				if (found != s_shapeMetadata.end())
					metadata = found->second;
			}

			IDirect3DPixelShader9* pixelShader = nullptr;
			IDirect3DVertexShader9* vertexShader = nullptr;
			IDirect3DBaseTexture9* texture = nullptr;
			device->GetPixelShader(&pixelShader);
			device->GetVertexShader(&vertexShader);
			device->GetTexture(0, &texture);
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			IDirect3DPixelShader9* cachedPixelShader = renderer && renderer->m_pkRenderState
				? renderer->m_pkRenderState->GetPixelShader() : nullptr;

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
				}
			}
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
				"tnvse_freetype_a8_diag:   local=(%.3f,%.3f,%.3f scale=%.3f) world=(%.3f,%.3f,%.3f scale=%.3f) shadeColor=(%.4f,%.4f,%.4f,%.4f) alphaProperty=(flags=0x%04X ref=%u)",
				s_currentA8Shape->m_kLocal.m_Translate.x, s_currentA8Shape->m_kLocal.m_Translate.y,
				s_currentA8Shape->m_kLocal.m_Translate.z, s_currentA8Shape->m_kLocal.m_fScale,
				s_currentA8Shape->m_kWorld.m_Translate.x, s_currentA8Shape->m_kWorld.m_Translate.y,
				s_currentA8Shape->m_kWorld.m_Translate.z, s_currentA8Shape->m_kWorld.m_fScale,
				shadeColor.r, shadeColor.g, shadeColor.b, shadeColor.a,
				alphaFlags, propertyAlphaReference);
			FreeTypeFontDebugLog(
				"tnvse_freetype_a8_diag:   contract=tile-uniform-v4-sdf abi=%u rgb=c0.rgb*c1.rgb alpha=coverage*c0.a*c1.a modifier=[(%.4f,%.4f,%.4f,%.4f)..(%.4f,%.4f,%.4f,%.4f)]",
				metadata.colorContract.abiVersion,
				metadata.colorContract.minimumModifier.r,
				metadata.colorContract.minimumModifier.g,
				metadata.colorContract.minimumModifier.b,
				metadata.colorContract.minimumModifier.a,
				metadata.colorContract.maximumModifier.r,
				metadata.colorContract.maximumModifier.g,
				metadata.colorContract.maximumModifier.b,
				metadata.colorContract.maximumModifier.a);
			FreeTypeFontDebugLog(
				"tnvse_freetype_a8_diag:   geometry xyz=[(%.3f,%.3f,%.3f)..(%.3f,%.3f,%.3f)] uv=[(%.6f,%.6f)..(%.6f,%.6f)]",
				minimumVertex.x, minimumVertex.y, minimumVertex.z,
				maximumVertex.x, maximumVertex.y, maximumVertex.z,
				minimumUv.x, minimumUv.y, maximumUv.x, maximumUv.y);
			FreeTypeFontDebugLog(
				"tnvse_freetype_a8_diag:   d3d ps=%p cachedPs=%p cacheMatch=%u vs=%p tex0=%p blend=(enable=%u src=%u dst=%u op=%u separate=%u) alphaTest=(enable=%u func=%u ref=%u) zWrite=%u colorWrite=0x%X textureFactor=0x%08X sampler=(min=%u mag=%u mip=%u)",
				pixelShader, cachedPixelShader, pixelShader == cachedPixelShader,
				vertexShader, texture, alphaBlend, sourceBlend, destinationBlend,
				blendOperation, separateAlpha, alphaTest, alphaFunction, alphaReference,
				zWrite, colorWrite, textureFactor, minFilter, magFilter, mipFilter);
			FreeTypeFontDebugLog(
				"tnvse_freetype_a8_diag:   ps_c0_c3=(%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g)",
				pixelConstants[0], pixelConstants[1], pixelConstants[2], pixelConstants[3],
				pixelConstants[4], pixelConstants[5], pixelConstants[6], pixelConstants[7],
				pixelConstants[8], pixelConstants[9], pixelConstants[10], pixelConstants[11],
				pixelConstants[12], pixelConstants[13], pixelConstants[14], pixelConstants[15]);
			if (metadata.effects.enabled)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_a8_diag:   effects quality=%u shaderEffects=%u atlasTexel=(%.7f,%.7f) spread=%.3f shadow=(blur=%.3f power=%.3f) glow=(inner=%.3f outer=%.3f power=%.3f) outline=(width=%.3f softness=%.3f) contract=tile-uniform-v4-sdf",
					static_cast<UInt32>(metadata.effects.quality),
					metadata.effects.shaderEffects ? 1 : 0,
					metadata.effects.inverseAtlasWidth,
					metadata.effects.inverseAtlasHeight,
					metadata.effects.sdfSpreadPixels,
					metadata.effects.shadowBlurPixels,
					metadata.effects.shadowPower,
					metadata.effects.glowInnerPixels,
					metadata.effects.glowOuterPixels,
					metadata.effects.glowPower,
					metadata.effects.outlineWidthPixels,
					metadata.effects.outlineSoftnessPixels);
				for (UInt32 rangeIndex = 0;
					rangeIndex < metadata.effects.ranges.size(); ++rangeIndex)
				{
					const A8DrawRange& range = metadata.effects.ranges[rangeIndex];
					FreeTypeFontDebugLog(
						"tnvse_freetype_a8_diag:     range=%u layer=%u color=(%.4f,%.4f,%.4f,%.4f) vertices=(first=%u count=%u) indices=(start=%u primitives=%u)",
						rangeIndex, range.layer,
						range.colorModifier.r, range.colorModifier.g,
						range.colorModifier.b, range.colorModifier.a,
						range.firstVertex, range.vertexCount,
						range.startIndex, range.primitiveCount);
				}
			}
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

		bool HaveEffectShader(EffectQuality quality)
		{
			const size_t index = static_cast<size_t>(quality);
			return index < s_effectShaders.size() && s_effectShaders[index]
				&& s_effectShaders[index]->GetShaderHandle();
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

		void LoadEffectShaders(CreatePixelShaderFn createPixelShader)
		{
			if (!createPixelShader)
				return;
			const char* names[] = {
				"tnvse_freetype_effects_fast.pso",
				"tnvse_freetype_effects_balanced.pso",
				"tnvse_freetype_effects_high.pso"
			};
			for (size_t index = 0; index < s_effectShaders.size(); ++index)
			{
				NiD3DPixelShaderPtr loaded = createPixelShader(names[index]);
				if (loaded && loaded->GetShaderHandle())
				{
					s_effectShaders[index] = loaded;
					if (g_bEnableFreeTypeFontRenderingLog)
						FreeTypeFontDebugLog("tnvse_freetype_font: loaded %s", names[index]);
				}
				else if (!s_effectShaders[index])
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: failed to load %s; using a lower shader quality or CPU effects",
						names[index]);
				}
			}
		}

		bool RefreshShaderSet(CreatePixelShaderFn createPixelShader)
		{
			if (!createPixelShader)
				return false;
			NiD3DPixelShaderPtr base = createPixelShader("tnvse_freetype_a8.pso");
			if (!base || !base->GetShaderHandle())
				return false;
			const char* names[] = {
				"tnvse_freetype_effects_fast.pso",
				"tnvse_freetype_effects_balanced.pso",
				"tnvse_freetype_effects_high.pso"
			};
			std::array<NiD3DPixelShaderPtr, 3> effects;
			for (size_t index = 0; index < effects.size(); ++index)
			{
				effects[index] = createPixelShader(names[index]);
				if (!effects[index] || !effects[index]->GetShaderHandle())
					return false;
			}
			s_a8Shader = base;
			s_effectShaders = effects;
			if (g_bEnableFreeTypeFontRenderingLog)
				FreeTypeFontDebugLog(
					"tnvse_freetype_font: atomically refreshed Tile-compatible shader set contract=tile-uniform-v4-sdf");
			return true;
		}

		void LogStateIsolationFailure(const char* reason,
			IDirect3DPixelShader9* actual = nullptr,
			IDirect3DPixelShader9* cached = nullptr)
		{
			if (s_stateMismatchLogCount++ >= kMaximumStateMismatchLogs)
				return;
			FreeTypeFontDebugLog(
				"tnvse_freetype_font: custom shader compatible fallback reason=%s actualPs=%p cachedPs=%p",
				reason ? reason : "unknown", actual, cached);
		}

		class ScopedA8RenderState
		{
		public:
			explicit ScopedA8RenderState(IDirect3DDevice9* device) : m_device(device)
			{
				NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
				m_renderState = renderer && renderer->GetD3DDevice() == device
					? renderer->m_pkRenderState : nullptr;
				if (!m_device || !m_renderState)
				{
					LogStateIsolationFailure("render-state-manager-unavailable");
					return;
				}

				if (FAILED(m_device->GetPixelShader(&m_originalPixelShader)))
				{
					LogStateIsolationFailure("get-pixel-shader-failed");
					return;
				}
				IDirect3DPixelShader9* cached = m_renderState->GetPixelShader();
				if (cached != m_originalPixelShader)
				{
					LogStateIsolationFailure("pixel-shader-cache-mismatch",
						m_originalPixelShader, cached);
					return;
				}
				if (FAILED(m_device->GetPixelShaderConstantF(1,
					m_originalConstants.data(), 4)))
				{
					LogStateIsolationFailure("get-private-constants-failed");
					return;
				}
				if (FAILED(m_device->GetPixelShaderConstantF(0, m_originalTileColor.data(), 1)))
				{
					LogStateIsolationFailure("get-tile-color-failed");
					return;
				}

				for (SamplerSetting& setting : m_samplerSettings)
				{
					if (FAILED(m_device->GetSamplerState(0, setting.state, &setting.value)))
					{
						LogStateIsolationFailure("get-sampler-state-failed");
						return;
					}
				}
				m_originalAlphaTest = m_renderState->GetRenderState(D3DRS_ALPHATESTENABLE);
				m_originalZWrite = m_renderState->GetRenderState(D3DRS_ZWRITEENABLE);
				m_valid = true;
			}

			~ScopedA8RenderState()
			{
				if (m_modified && m_device && m_renderState)
				{
					m_device->SetPixelShaderConstantF(1, m_originalConstants.data(), 4);
					for (const SamplerSetting& setting : m_samplerSettings)
					{
						m_renderState->SetSamplerState(0, setting.state,
							setting.value, false);
					}
					m_renderState->SetRenderState(D3DRS_ALPHATESTENABLE,
						m_originalAlphaTest, 0, false);
					m_renderState->SetRenderState(D3DRS_ZWRITEENABLE,
						m_originalZWrite, 0, false);
					m_renderState->SetPixelShader(m_originalPixelShader, false);
					for (const SamplerSetting& setting : m_samplerSettings)
					{
						DWORD restoredValue = 0;
						if (FAILED(m_device->GetSamplerState(0, setting.state, &restoredValue))
							|| restoredValue != setting.value)
						{
							LogStateIsolationFailure("sampler-restore-mismatch");
							break;
						}
					}
					std::array<float, 4> tileColor = {};
					if (FAILED(m_device->GetPixelShaderConstantF(0, tileColor.data(), 1))
						|| tileColor != m_originalTileColor)
					{
						LogStateIsolationFailure("tile-color-c0-changed");
					}

					IDirect3DPixelShader9* restored = nullptr;
					if (SUCCEEDED(m_device->GetPixelShader(&restored)))
					{
						if (restored != m_originalPixelShader
							|| m_renderState->GetPixelShader() != m_originalPixelShader)
						{
							LogStateIsolationFailure("pixel-shader-restore-mismatch",
								restored, m_renderState->GetPixelShader());
						}
						if (restored)
							restored->Release();
					}
				}
				if (m_originalPixelShader)
					m_originalPixelShader->Release();
			}

			bool IsValid() const { return m_valid; }

			bool Apply(IDirect3DPixelShader9* shader)
			{
				if (!m_valid || !shader)
					return false;
				m_renderState->SetSamplerState(0, D3DSAMP_MINFILTER,
					D3DTEXF_POINT, false);
				m_renderState->SetSamplerState(0, D3DSAMP_MAGFILTER,
					D3DTEXF_POINT, false);
				m_renderState->SetSamplerState(0, D3DSAMP_MIPFILTER,
					D3DTEXF_NONE, false);
				m_renderState->SetSamplerState(0, D3DSAMP_ADDRESSU,
					D3DTADDRESS_CLAMP, false);
				m_renderState->SetSamplerState(0, D3DSAMP_ADDRESSV,
					D3DTADDRESS_CLAMP, false);
				m_renderState->SetPixelShader(shader, false);
				m_modified = true;
				return true;
			}

			void SetEffectPassState(bool effectPass)
			{
				if (!m_valid || !m_renderState)
					return;
				m_renderState->SetRenderState(D3DRS_ALPHATESTENABLE,
					effectPass ? FALSE : m_originalAlphaTest, 0, false);
				m_renderState->SetRenderState(D3DRS_ZWRITEENABLE,
					effectPass ? FALSE : m_originalZWrite, 0, false);
				m_modified = true;
			}

			void SetSmoothEffectSampling(bool enabled)
			{
				if (!m_valid || !m_renderState)
					return;
				const DWORD filter = enabled ? D3DTEXF_LINEAR : D3DTEXF_POINT;
				m_renderState->SetSamplerState(0, D3DSAMP_MINFILTER,
					filter, false);
				m_renderState->SetSamplerState(0, D3DSAMP_MAGFILTER,
					filter, false);
				m_modified = true;
			}

		private:
			struct SamplerSetting
			{
				D3DSAMPLERSTATETYPE state;
				DWORD value = 0;
			};

			IDirect3DDevice9* m_device = nullptr;
			NiDX9RenderState* m_renderState = nullptr;
			IDirect3DPixelShader9* m_originalPixelShader = nullptr;
			std::array<float, 16> m_originalConstants = {};
			std::array<float, 4> m_originalTileColor = {};
			std::array<SamplerSetting, 5> m_samplerSettings = {{
				{ D3DSAMP_MINFILTER, 0 },
				{ D3DSAMP_MAGFILTER, 0 },
				{ D3DSAMP_MIPFILTER, 0 },
				{ D3DSAMP_ADDRESSU, 0 },
				{ D3DSAMP_ADDRESSV, 0 }
			}};
			UInt32 m_originalAlphaTest = FALSE;
			UInt32 m_originalZWrite = FALSE;
			bool m_valid = false;
			bool m_modified = false;
		};

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

			A8ShapeMetadata metadata;
			bool haveMetadata = false;
			{
				std::lock_guard<std::mutex> lock(s_diagnosticsMutex);
				auto found = s_shapeMetadata.find(s_currentA8Shape);
				if (found != s_shapeMetadata.end())
				{
					metadata = found->second;
					haveMetadata = true;
				}
			}

			ScopedA8RenderState state(device);
			if (!state.IsValid())
			{
				return s_originalDrawIndexedPrimitive(device, primitiveType, baseVertexIndex,
					minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
			}
			const bool validContract = haveMetadata
				&& metadata.colorContract.abiVersion
					== A8ShapeColorContract::kTileUniformColorAbi;
			const bool haveRanges = validContract && metadata.effects.enabled
				&& !metadata.effects.ranges.empty();
			const bool useShaderEffects = haveRanges && metadata.effects.shaderEffects
				&& HaveEffectShader(metadata.effects.quality);
			IDirect3DPixelShader9* shader = useShaderEffects
				? s_effectShaders[static_cast<size_t>(metadata.effects.quality)]->GetShaderHandle()
				: s_a8Shader->GetShaderHandle();
			if (!state.Apply(shader))
				return D3DERR_INVALIDCALL;

			if (!haveRanges)
			{
				const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
				device->SetPixelShaderConstantF(1, white, 1);
				return s_originalDrawIndexedPrimitive(device, primitiveType,
					baseVertexIndex, minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
			}

			HRESULT result = D3D_OK;
			bool drewRange = false;
			for (const A8DrawRange& range : metadata.effects.ranges)
			{
				if (!range.primitiveCount || !range.vertexCount || range.layer > 3)
					continue;
				// A shader-effect batch cannot reconstruct its CPU masks after a live
				// shader loss. Preserve readable text by drawing only the fill ranges.
				if (metadata.effects.shaderEffects && !useShaderEffects && range.layer != 3)
					continue;

				float parameter0 = metadata.effects.shadowBlurPixels;
				float parameter1 = metadata.effects.shadowPower;
				float parameter2 = 0.0f;
				if (range.layer == 1)
				{
					parameter0 = metadata.effects.glowInnerPixels;
					parameter1 = metadata.effects.glowOuterPixels;
					parameter2 = metadata.effects.glowPower;
				}
				else if (range.layer == 2)
				{
					parameter0 = metadata.effects.outlineWidthPixels;
					parameter1 = metadata.effects.outlineSoftnessPixels;
					parameter2 = 0.0f;
				}
				else if (range.layer == 3)
				{
					parameter0 = 0.0f;
					parameter1 = 0.0f;
					parameter2 = 0.0f;
				}
				const float constants[16] = {
					range.colorModifier.r, range.colorModifier.g,
					range.colorModifier.b, range.colorModifier.a,
					metadata.effects.inverseAtlasWidth,
					metadata.effects.inverseAtlasHeight,
					static_cast<float>(range.layer), metadata.effects.sdfSpreadPixels,
					parameter0, parameter1, parameter2, 0.0f,
					0.0f, 0.0f, 0.0f, 0.0f
				};
				if (FAILED(device->SetPixelShaderConstantF(1, constants, 4)))
				{
					if (SUCCEEDED(result))
						result = D3DERR_INVALIDCALL;
					continue;
				}
				state.SetEffectPassState(range.layer != 3);
				state.SetSmoothEffectSampling(range.layer == 1 || range.layer == 2
					|| (range.layer == 0
						&& metadata.effects.shadowBlurPixels > 0.001f));

				if (g_bEnableFreeTypeFontRenderingLog && range.layer == 0
					&& s_shadowContractLogCount++ < 8)
				{
					std::array<float, 4> actualTile = {};
					std::array<float, 4> actualLayer = {};
					device->GetPixelShaderConstantF(0, actualTile.data(), 1);
					device->GetPixelShaderConstantF(1, actualLayer.data(), 1);
					FreeTypeFontDebugLog(
						"tnvse_freetype_a8_diag: shadow contract=tile-uniform-v4-sdf c0=(%.4f,%.4f,%.4f,%.4f) c1=(%.4f,%.4f,%.4f,%.4f) coverage=atlas-sampled expectedMaxAlpha=%.4f blend=preserved alphaTest=disabled",
						actualTile[0], actualTile[1], actualTile[2], actualTile[3],
						actualLayer[0], actualLayer[1], actualLayer[2], actualLayer[3],
						actualTile[3] * actualLayer[3]);
				}

				UInt32 samples = 1;
				const UInt32 quality = static_cast<UInt32>(metadata.effects.quality);
				const bool sdfPass = range.layer == 1 || range.layer == 2
					|| (range.layer == 0 && metadata.effects.shadowBlurPixels > 0.001f);
				if (useShaderEffects && sdfPass)
					samples = quality == 0 ? 1 : quality == 1 ? 4 : 8;
				if (useShaderEffects)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::ShaderEffectPass);
					RecordFreeTypePerf(FreeTypePerfCounter::ShaderEffectSamples,
						static_cast<UInt64>(samples) * metadata.glyphCount);
				}
				const HRESULT passResult = s_originalDrawIndexedPrimitive(device,
					primitiveType, baseVertexIndex,
					minimumVertexIndex + range.firstVertex, range.vertexCount,
					startIndex + range.startIndex, range.primitiveCount);
				drewRange = true;
				if (FAILED(passResult) && SUCCEEDED(result))
					result = passResult;
			}

			return drewRange ? result : D3DERR_INVALIDCALL;
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

		void __fastcall A8DeleteThis(NiTriShape* shape, void*)
		{
			{
				std::lock_guard<std::mutex> lock(s_diagnosticsMutex);
				s_shapeMetadata.erase(shape);
				s_loggedShapes.erase(shape);
			}
			s_originalDeleteThis(shape);
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
			s_originalDeleteThis = reinterpret_cast<DeleteThisFn>(
				s_a8TriShapeVtable[kDeleteThisSlot + 1]);
			s_a8TriShapeVtable[kDeleteThisSlot + 1]
				= reinterpret_cast<void*>(&A8DeleteThis);
			s_a8TriShapeVtable[kRenderImmediateSlot + 1]
				= reinterpret_cast<void*>(&A8RenderImmediate);
			s_a8TriShapeVtable[kRenderImmediateAltSlot + 1]
				= reinterpret_cast<void*>(&A8RenderImmediateAlt);
			return s_originalRenderImmediate && s_originalRenderImmediateAlt
				&& s_originalDeleteThis;
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
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		if (!renderer || renderer->m_kD3DCaps9.PixelShaderVersion < D3DPS_VERSION(3, 0))
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: pixel shader 3.0 is unavailable; using 32-bit atlases");
			return;
		}
		s_shaderLoaderCompatible = true;
		const bool loaded = LoadA8Shader(createPixelShader);
		if (loaded)
			LoadEffectShaders(createPixelShader);
		s_a8Available = loaded && HookD3DDevice();
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
		if (RefreshShaderSet(createPixelShader))
		{
			s_a8Available = HookD3DDevice();
		}
		else
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: shader refresh validation failed; retaining the previous shader set");
		}
	}

	bool IsA8RendererAvailable()
	{
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		return s_a8Available && HaveA8Shader() && renderer
			&& renderer->m_pkRenderState;
	}

	bool ResolveA8EffectQuality(EffectQuality requested, EffectQuality& resolved)
	{
		for (int quality = static_cast<int>(requested); quality >= 0; --quality)
		{
			const EffectQuality candidate = static_cast<EffectQuality>(quality);
			if (HaveEffectShader(candidate))
			{
				resolved = candidate;
				return true;
			}
		}
		return false;
	}

	bool IsA8EffectRendererAvailable(EffectQuality quality)
	{
		EffectQuality resolved = quality;
		return IsA8RendererAvailable() && ResolveA8EffectQuality(quality, resolved);
	}

	bool PrepareA8AtlasShape(NiTriShape* shape, UInt32 fontId,
		UInt32 glyphCount, UInt32 quadCount, const A8EffectShapeConfig* effectConfig,
		const A8ShapeColorContract* colorContract)
	{
		if (!IsA8RendererAvailable() || !InitializeA8TriShapeVtable(shape))
			return false;
		*reinterpret_cast<void***>(shape) = &s_a8TriShapeVtable[1];
		{
			std::lock_guard<std::mutex> lock(s_diagnosticsMutex);
			A8ShapeMetadata metadata;
			metadata.fontId = fontId;
			metadata.glyphCount = glyphCount;
			metadata.quadCount = quadCount;
			if (colorContract)
				metadata.colorContract = *colorContract;
			if (effectConfig)
				metadata.effects = *effectConfig;
			s_shapeMetadata[shape] = metadata;
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

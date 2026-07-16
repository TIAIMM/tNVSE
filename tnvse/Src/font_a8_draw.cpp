#include "font_a8_internal.h"

#include "load_config.h"
#include "plugin_dependencies.h"
#include "tnvse.h"

#include "NiDX9RenderState.hpp"
#include "NiDX9TextureData.hpp"
#include "NiAlphaProperty.hpp"
#include "NiRenderer.hpp"
#include "NiTriShapeData.hpp"
#include "Utils/SafeWrite.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace fonthook::vectorfont
{
		void LogStateIsolationFailure(const char* reason,
			IDirect3DPixelShader9* actual = nullptr,
			IDirect3DPixelShader9* cached = nullptr)
		{
			if (State().stateMismatchLogCount++ >= kMaximumStateMismatchLogs)
				return;
			FreeTypeFontDebugLog(
				"tnvse_freetype_font: custom shader state diagnostic reason=%s actualPs=%p cachedPs=%p",
				reason ? reason : "unknown", actual, cached);
		}

		class ScopedA8RenderState
		{
			enum SamplerIndex : size_t
			{
				kMinFilterSampler,
				kMagFilterSampler,
				kMipFilterSampler,
				kAddressUSampler,
				kAddressVSampler,
				kMaxMipLevelSampler,
				kMipLodBiasSampler
			};

		public:
			explicit ScopedA8RenderState(IDirect3DDevice9* device) : m_device(device)
			{
				NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
				m_cachedRenderState = g_bEnableFreeTypeFontRenderingLog
					&& renderer && renderer->GetD3DDevice() == device
					? renderer->m_pkRenderState : nullptr;
				if (!m_device)
				{
					LogStateIsolationFailure("d3d-device-unavailable");
					return;
				}
				m_supportsSeparateAlphaBlend = renderer
					&& (renderer->m_kD3DCaps9.PrimitiveMiscCaps
						& D3DPMISCCAPS_SEPARATEALPHABLEND) != 0;

				if (FAILED(m_device->GetPixelShader(&m_originalPixelShader)))
				{
					LogStateIsolationFailure("get-pixel-shader-failed");
					return;
				}
				if (FAILED(m_device->GetTexture(0, &m_originalTexture)))
				{
					LogStateIsolationFailure("get-texture0-failed");
					return;
				}
				IDirect3DPixelShader9* cached = m_cachedRenderState
					? m_cachedRenderState->GetPixelShader() : nullptr;
				if (m_cachedRenderState && cached != m_originalPixelShader)
				{
					// NVR and other render extensions can update the D3D device without
					// updating Gamebryo's shadow cache. The actual device state is the
					// authoritative state for this transient draw bridge.
					LogStateIsolationFailure("pixel-shader-cache-mismatch",
						m_originalPixelShader, cached);
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
				if (FAILED(m_device->GetRenderState(D3DRS_ALPHABLENDENABLE,
					&m_originalAlphaBlend))
					|| FAILED(m_device->GetRenderState(D3DRS_ALPHATESTENABLE,
					&m_originalAlphaTest))
					|| FAILED(m_device->GetRenderState(D3DRS_ZWRITEENABLE,
						&m_originalZWrite))
					|| FAILED(m_device->GetRenderState(D3DRS_COLORWRITEENABLE,
						&m_originalColorWrite))
					|| FAILED(m_device->GetRenderState(D3DRS_STENCILWRITEMASK,
						&m_originalStencilWriteMask)))
				{
					LogStateIsolationFailure("get-render-state-failed");
					return;
				}
				if (m_supportsSeparateAlphaBlend
					&& (FAILED(m_device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,
						&m_originalSeparateAlphaBlend))
						|| FAILED(m_device->GetRenderState(D3DRS_SRCBLENDALPHA,
							&m_originalSourceAlphaBlend))
						|| FAILED(m_device->GetRenderState(D3DRS_DESTBLENDALPHA,
							&m_originalDestinationAlphaBlend))
						|| FAILED(m_device->GetRenderState(D3DRS_BLENDOPALPHA,
							&m_originalAlphaBlendOperation))))
				{
					LogStateIsolationFailure("get-separate-alpha-state-failed");
					return;
				}
				for (size_t index = 0; index < m_samplerSettings.size(); ++index)
					m_currentSamplerValues[index] = m_samplerSettings[index].value;
				m_currentTileColor = m_originalTileColor;
				m_currentConstants = m_originalConstants;
				m_currentPixelShader = m_originalPixelShader;
				m_currentTexture = m_originalTexture;
				m_currentAlphaTest = m_originalAlphaTest;
				m_currentZWrite = m_originalZWrite;
				m_currentColorWrite = m_originalColorWrite;
				m_currentSeparateAlphaBlend = m_originalSeparateAlphaBlend;
				m_currentSourceAlphaBlend = m_originalSourceAlphaBlend;
				m_currentDestinationAlphaBlend = m_originalDestinationAlphaBlend;
				m_currentAlphaBlendOperation = m_originalAlphaBlendOperation;
				m_currentStencilWriteMask = m_originalStencilWriteMask;
				m_valid = true;
			}

			~ScopedA8RenderState()
			{
				if (m_modified && m_device)
				{
					m_device->SetPixelShaderConstantF(0,
						m_originalTileColor.data(), 1);
					m_device->SetPixelShaderConstantF(1, m_originalConstants.data(), 4);
					for (const SamplerSetting& setting : m_samplerSettings)
						m_device->SetSamplerState(0, setting.state, setting.value);
					m_device->SetRenderState(D3DRS_ALPHATESTENABLE, m_originalAlphaTest);
					m_device->SetRenderState(D3DRS_ZWRITEENABLE, m_originalZWrite);
					m_device->SetRenderState(D3DRS_COLORWRITEENABLE, m_originalColorWrite);
					if (m_supportsSeparateAlphaBlend)
					{
						m_device->SetRenderState(D3DRS_SRCBLENDALPHA,
							m_originalSourceAlphaBlend);
						m_device->SetRenderState(D3DRS_DESTBLENDALPHA,
							m_originalDestinationAlphaBlend);
						m_device->SetRenderState(D3DRS_BLENDOPALPHA,
							m_originalAlphaBlendOperation);
						m_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,
							m_originalSeparateAlphaBlend);
					}
					m_device->SetRenderState(D3DRS_STENCILWRITEMASK,
						m_originalStencilWriteMask);
					m_device->SetPixelShader(m_originalPixelShader);
					m_device->SetTexture(0, m_originalTexture);
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
					VerifyRestoredRenderState(D3DRS_ALPHATESTENABLE,
						m_originalAlphaTest, "alpha-test-restore-mismatch");
					VerifyRestoredRenderState(D3DRS_ZWRITEENABLE,
						m_originalZWrite, "z-write-restore-mismatch");
					VerifyRestoredRenderState(D3DRS_COLORWRITEENABLE,
						m_originalColorWrite, "color-write-restore-mismatch");
					if (m_supportsSeparateAlphaBlend)
					{
						VerifyRestoredRenderState(D3DRS_SEPARATEALPHABLENDENABLE,
							m_originalSeparateAlphaBlend,
							"separate-alpha-restore-mismatch");
						VerifyRestoredRenderState(D3DRS_SRCBLENDALPHA,
							m_originalSourceAlphaBlend,
							"source-alpha-restore-mismatch");
						VerifyRestoredRenderState(D3DRS_DESTBLENDALPHA,
							m_originalDestinationAlphaBlend,
							"destination-alpha-restore-mismatch");
						VerifyRestoredRenderState(D3DRS_BLENDOPALPHA,
							m_originalAlphaBlendOperation,
							"alpha-operation-restore-mismatch");
					}
					VerifyRestoredRenderState(D3DRS_STENCILWRITEMASK,
						m_originalStencilWriteMask,
						"stencil-write-mask-restore-mismatch");

					IDirect3DPixelShader9* restored = nullptr;
					if (SUCCEEDED(m_device->GetPixelShader(&restored)))
					{
						if (restored != m_originalPixelShader)
						{
							LogStateIsolationFailure("pixel-shader-restore-mismatch",
								restored, m_originalPixelShader);
						}
						if (restored)
							restored->Release();
					}
				}
				if (m_originalPixelShader)
					m_originalPixelShader->Release();
				if (m_originalTexture)
					m_originalTexture->Release();
			}

			bool IsValid() const { return m_valid; }

			bool ApplySamplerContract()
			{
				if (!m_valid)
					return false;
				// Mark the guard dirty before the first mutation so a partial failure is
				// still restored. RGB blend, depth-test, scissor, stencil test/ref and
				// stream state remain exactly as established by the original Tile pass.
				m_modified = true;
				return SetSamplerStateIfChanged(kMinFilterSampler, D3DTEXF_POINT)
					&& SetSamplerStateIfChanged(kMagFilterSampler, D3DTEXF_POINT)
					&& SetSamplerStateIfChanged(kMipFilterSampler, D3DTEXF_NONE)
					&& SetSamplerStateIfChanged(kMaxMipLevelSampler, 0)
					&& SetSamplerStateIfChanged(kMipLodBiasSampler, 0)
					&& SetSamplerStateIfChanged(kAddressUSampler, D3DTADDRESS_CLAMP)
					&& SetSamplerStateIfChanged(kAddressVSampler, D3DTADDRESS_CLAMP);
			}

			bool SetShader(IDirect3DPixelShader9* shader)
			{
				if (!m_valid || !shader)
					return false;
				m_modified = true;
				return SetPixelShaderIfChanged(shader);
			}

			HRESULT SetConstants(const float* constants)
			{
				if (!m_valid || !constants)
					return D3DERR_INVALIDCALL;
				m_modified = true;
				if (std::memcmp(m_currentConstants.data(), constants,
					sizeof(m_currentConstants)) == 0)
				{
					return D3D_OK;
				}
				const HRESULT result = m_device->SetPixelShaderConstantF(1,
					constants, 4);
				if (SUCCEEDED(result))
					std::copy(constants, constants + m_currentConstants.size(),
						m_currentConstants.begin());
				return result;
			}

			bool SetPassState(bool effectPass, bool customCoverageShader)
			{
				if (!m_valid)
					return false;
				// Fill follows the original TILE1000 c0 contract. Effects instead own
				// their configured RGB and inherit only the live Tile alpha. This avoids
				// dim/zero Tile RGB channels destroying a configured shadow color, while
				// preserving menu fades and per-glyph alpha exactly.
				const std::array<float, 4> passTileColor = effectPass
					? std::array<float, 4>{ 1.0f, 1.0f, 1.0f,
						m_originalTileColor[3] }
					: m_originalTileColor;
				// Off-screen UI targets consume destination alpha when composited later.
				// RGB-only effects disappear outside the fill, while the caller's usual
				// non-separate SRCALPHA blend can reduce existing destination alpha. Use
				// source-over union for alpha only; RGB blend remains caller-controlled.
				const bool writeEffectAlpha = effectPass
					&& m_originalAlphaBlend && m_supportsSeparateAlphaBlend
					&& (m_originalColorWrite & D3DCOLORWRITEENABLE_ALPHA) != 0;
				const DWORD colorWrite = effectPass && !writeEffectAlpha
					? (m_originalColorWrite
						& ~static_cast<DWORD>(D3DCOLORWRITEENABLE_ALPHA))
					: m_originalColorWrite;
				const DWORD alphaTest = customCoverageShader
					? FALSE : (effectPass ? FALSE : m_originalAlphaTest);
				m_modified = true;
				bool tileColorReady = true;
				if (std::memcmp(m_currentTileColor.data(), passTileColor.data(),
					sizeof(m_currentTileColor)) != 0)
				{
					tileColorReady = SUCCEEDED(m_device->SetPixelShaderConstantF(
						0, passTileColor.data(), 1));
					if (tileColorReady)
						m_currentTileColor = passTileColor;
				}
				bool alphaStateReady = true;
				if (m_supportsSeparateAlphaBlend)
				{
					alphaStateReady = SetRenderStateIfChanged(D3DRS_SRCBLENDALPHA,
						writeEffectAlpha ? D3DBLEND_ONE : m_originalSourceAlphaBlend,
						m_currentSourceAlphaBlend)
						&& SetRenderStateIfChanged(D3DRS_DESTBLENDALPHA,
							writeEffectAlpha ? D3DBLEND_INVSRCALPHA
								: m_originalDestinationAlphaBlend,
							m_currentDestinationAlphaBlend)
						&& SetRenderStateIfChanged(D3DRS_BLENDOPALPHA,
							writeEffectAlpha ? D3DBLENDOP_ADD
								: m_originalAlphaBlendOperation,
							m_currentAlphaBlendOperation)
						&& SetRenderStateIfChanged(D3DRS_SEPARATEALPHABLENDENABLE,
							writeEffectAlpha ? TRUE : m_originalSeparateAlphaBlend,
							m_currentSeparateAlphaBlend);
				}
				return tileColorReady && alphaStateReady
					&& SetRenderStateIfChanged(D3DRS_ALPHATESTENABLE, alphaTest,
						m_currentAlphaTest)
					&& SetRenderStateIfChanged(D3DRS_ZWRITEENABLE,

						effectPass ? FALSE : m_originalZWrite, m_currentZWrite)
					&& SetRenderStateIfChanged(D3DRS_COLORWRITEENABLE, colorWrite,
						m_currentColorWrite)
					&& SetRenderStateIfChanged(D3DRS_STENCILWRITEMASK,
						effectPass ? 0 : m_originalStencilWriteMask,
						m_currentStencilWriteMask);
			}

			bool SetSampling(bool linear, bool mipmapped)
			{
				if (!m_valid)
					return false;
				const DWORD filter = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
				const DWORD mipFilter = linear && mipmapped
					? D3DTEXF_LINEAR : D3DTEXF_NONE;
				m_modified = true;
				return SetSamplerStateIfChanged(kMinFilterSampler, filter)
					&& SetSamplerStateIfChanged(kMagFilterSampler, filter)
					&& SetSamplerStateIfChanged(kMipFilterSampler, mipFilter);
			}

			bool SetTexture(IDirect3DBaseTexture9* texture)
			{
				if (!m_valid || !texture)
					return false;
				m_modified = true;
				if (m_currentTexture == texture)
					return true;
				if (FAILED(m_device->SetTexture(0, texture)))
					return false;
				m_currentTexture = texture;
				return true;
			}

		private:
			bool SetSamplerStateIfChanged(SamplerIndex sampler, DWORD value)
			{
				const size_t index = static_cast<size_t>(sampler);
				if (m_currentSamplerValues[index] == value)
					return true;
				if (FAILED(m_device->SetSamplerState(0,
					m_samplerSettings[index].state, value)))
				{
					return false;
				}
				m_currentSamplerValues[index] = value;
				return true;
			}

			bool SetRenderStateIfChanged(D3DRENDERSTATETYPE state, DWORD value,
				DWORD& current)
			{
				if (current == value)
					return true;
				if (FAILED(m_device->SetRenderState(state, value)))
					return false;
				current = value;
				return true;
			}

			bool SetPixelShaderIfChanged(IDirect3DPixelShader9* shader)
			{
				if (m_currentPixelShader == shader)
					return true;
				if (FAILED(m_device->SetPixelShader(shader)))
					return false;
				m_currentPixelShader = shader;
				return true;
			}

			void VerifyRestoredRenderState(D3DRENDERSTATETYPE state,
				DWORD expected, const char* reason)
			{
				DWORD actual = 0;
				if (FAILED(m_device->GetRenderState(state, &actual)) || actual != expected)
					LogStateIsolationFailure(reason);
			}

			struct SamplerSetting
			{
				D3DSAMPLERSTATETYPE state;
				DWORD value = 0;
			};

			IDirect3DDevice9* m_device = nullptr;
			NiDX9RenderState* m_cachedRenderState = nullptr;
			IDirect3DPixelShader9* m_originalPixelShader = nullptr;
			IDirect3DBaseTexture9* m_originalTexture = nullptr;
			std::array<float, 16> m_originalConstants = {};
			std::array<float, 4> m_originalTileColor = {};
			std::array<SamplerSetting, 7> m_samplerSettings = {{
				{ D3DSAMP_MINFILTER, 0 },
				{ D3DSAMP_MAGFILTER, 0 },
				{ D3DSAMP_MIPFILTER, 0 },
				{ D3DSAMP_ADDRESSU, 0 },
				{ D3DSAMP_ADDRESSV, 0 },
				{ D3DSAMP_MAXMIPLEVEL, 0 },
				{ D3DSAMP_MIPMAPLODBIAS, 0 }
			}};
			std::array<DWORD, 7> m_currentSamplerValues = {};
			std::array<float, 16> m_currentConstants = {};
			std::array<float, 4> m_currentTileColor = {};
			IDirect3DPixelShader9* m_currentPixelShader = nullptr;
			IDirect3DBaseTexture9* m_currentTexture = nullptr;
			UInt32 m_originalAlphaTest = FALSE;
			UInt32 m_originalAlphaBlend = FALSE;
			UInt32 m_originalZWrite = FALSE;
			UInt32 m_originalColorWrite = D3DCOLORWRITEENABLE_RED
				| D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE
				| D3DCOLORWRITEENABLE_ALPHA;
			UInt32 m_originalSeparateAlphaBlend = FALSE;
			UInt32 m_originalSourceAlphaBlend = D3DBLEND_ONE;
			UInt32 m_originalDestinationAlphaBlend = D3DBLEND_ZERO;
			UInt32 m_originalAlphaBlendOperation = D3DBLENDOP_ADD;
			UInt32 m_originalStencilWriteMask = 0xFFFFFFFFu;
			DWORD m_currentAlphaTest = FALSE;
			DWORD m_currentZWrite = FALSE;
			DWORD m_currentColorWrite = D3DCOLORWRITEENABLE_RED
				| D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE
				| D3DCOLORWRITEENABLE_ALPHA;
			DWORD m_currentSeparateAlphaBlend = FALSE;
			DWORD m_currentSourceAlphaBlend = D3DBLEND_ONE;
			DWORD m_currentDestinationAlphaBlend = D3DBLEND_ZERO;
			DWORD m_currentAlphaBlendOperation = D3DBLENDOP_ADD;
			DWORD m_currentStencilWriteMask = 0xFFFFFFFFu;
			bool m_supportsSeparateAlphaBlend = false;
			bool m_valid = false;
			bool m_modified = false;
		};

		HRESULT __stdcall A8DrawIndexedPrimitive(IDirect3DDevice9* device,
			D3DPRIMITIVETYPE primitiveType, INT baseVertexIndex, UINT minimumVertexIndex,
			UINT numberOfVertices, UINT startIndex, UINT primitiveCount)
		{
			A8State& a8State = State();
			A8ThreadState& thread = ThreadState();
			if (!a8State.originalDrawIndexedPrimitive)
				return D3DERR_INVALIDCALL;

			if (!thread.renderDepth)
			{
				return a8State.originalDrawIndexedPrimitive(device, primitiveType, baseVertexIndex,
					minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
			}

			UpdateCurrentRenderTraces([](A8RenderTraceContext& trace)
				{ ++trace.drawCalls; });
			A8RenderTraceContext* currentTrace = CurrentRenderTrace();
			const UInt32 drawCall = currentTrace ? currentTrace->drawCalls : 0;
			const bool detailedTrace = IsDetailedShadowTraceActive();
			static const A8ShapeMetadata emptyMetadata;
			const A8ShapeMetadata* metadataPtr = thread.currentMetadata.get();
			const bool haveMetadata = metadataPtr != nullptr;
			const A8ShapeMetadata& metadata = metadataPtr
				? *metadataPtr : emptyMetadata;
			const bool useOriginalShader = haveMetadata
				&& metadata.effects.useOriginalShader;
			if (!useOriginalShader && !HaveA8Shader())
			{
				UpdateCurrentRenderTraces([](A8RenderTraceContext& trace)
					{ ++trace.forwardedCalls; });
				if (detailedTrace)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_shadow_trace: dip-forward serial=%llu dip=%u reason=a8-shader-unavailable shape=%p depth=%u args=(type=%u base=%d min=%u vertices=%u start=%u primitives=%u)",
						static_cast<unsigned long long>(currentTrace->serial), drawCall,
						thread.currentShape, thread.renderDepth,
						static_cast<UInt32>(primitiveType), baseVertexIndex,
						minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
				}
				return a8State.originalDrawIndexedPrimitive(device, primitiveType, baseVertexIndex,
					minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
			}

			LogA8DrawDiagnostics(device, primitiveType, baseVertexIndex,
				minimumVertexIndex, numberOfVertices, startIndex, primitiveCount,
				metadata);
			const bool validContract = haveMetadata
				&& metadata.colorContract.abiVersion
					== A8ShapeColorContract::kTileUniformColorAbi;
			const bool geometryMatches = validContract
				&& primitiveType == D3DPT_TRIANGLELIST
				&& metadata.vertexCount == numberOfVertices
				&& metadata.primitiveCount == primitiveCount
				&& static_cast<UInt64>(metadata.indexCount)
					== static_cast<UInt64>(primitiveCount) * 3;
			if (detailedTrace)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_shadow_trace: dip-begin serial=%llu dip=%u shape=%p depth=%u multipleDip=%u args=(type=%u base=%d min=%u vertices=%u start=%u primitives=%u indices=%u) expected=(metadata=%u vertices=%u primitives=%u indices=%u ranges=%u geometryMatch=%u)",
					static_cast<unsigned long long>(currentTrace->serial), drawCall,
					thread.currentShape, thread.renderDepth, drawCall > 1 ? 1 : 0,
					static_cast<UInt32>(primitiveType), baseVertexIndex,
					minimumVertexIndex, numberOfVertices, startIndex, primitiveCount,
					primitiveCount * 3, haveMetadata ? 1 : 0,
					metadata.vertexCount, metadata.primitiveCount, metadata.indexCount,
					static_cast<UInt32>(metadata.effects.ranges.size()),
					geometryMatches ? 1 : 0);
				LogShadowTraceDeviceState(device, "dip-entry", drawCall, -1,
					nullptr, D3D_OK);
			}
			if (!geometryMatches)
			{
				UpdateCurrentRenderTraces([](A8RenderTraceContext& trace)
					{ ++trace.forwardedCalls; });
				if (a8State.rangeDrawFailureLogCount++ < kMaximumRangeDrawFailureLogs)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_a8_diag: draw contract mismatch; forwarding original draw metadata=%u abi=0x%08X type=%u vertices=%u/%u primitives=%u/%u indices=%llu/%u",
						haveMetadata ? 1 : 0, metadata.colorContract.abiVersion,
						static_cast<UInt32>(primitiveType), numberOfVertices,
						metadata.vertexCount, primitiveCount, metadata.primitiveCount,
						static_cast<unsigned long long>(
							static_cast<UInt64>(primitiveCount) * 3), metadata.indexCount);
				}
				if (detailedTrace)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_shadow_trace: dip-forward serial=%llu dip=%u reason=draw-contract-mismatch metadata=%u abi=0x%08X type=%u vertices=%u/%u primitives=%u/%u",
						static_cast<unsigned long long>(currentTrace->serial), drawCall,
						haveMetadata ? 1 : 0, metadata.colorContract.abiVersion,
						static_cast<UInt32>(primitiveType), numberOfVertices,
						metadata.vertexCount, primitiveCount, metadata.primitiveCount);
				}
				return a8State.originalDrawIndexedPrimitive(device, primitiveType, baseVertexIndex,
					minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
			}

			ScopedA8RenderState state(device);
			if (!state.IsValid())
			{
				UpdateCurrentRenderTraces([](A8RenderTraceContext& trace)
					{ ++trace.forwardedCalls; });
				if (detailedTrace)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_shadow_trace: dip-forward serial=%llu dip=%u reason=render-state-snapshot-failed",
						static_cast<unsigned long long>(currentTrace->serial), drawCall);
				}
				return a8State.originalDrawIndexedPrimitive(device, primitiveType, baseVertexIndex,
					minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
			}
			const bool haveRanges = validContract && metadata.effects.enabled
				&& !metadata.compiledRanges.empty();
			const bool scaledFillSampling = NeedsScaledFillSampling(thread.currentShape);
			bool useShaderEffects = !useOriginalShader && haveRanges
				&& metadata.effects.shaderEffects
				&& HaveEffectShader(metadata.effects.quality);
			IDirect3DPixelShader9* effectShader = useShaderEffects
				? a8State.effectShaders[static_cast<size_t>(metadata.effects.quality)]->GetShaderHandle()
				: nullptr;
			IDirect3DPixelShader9* fillShader = useOriginalShader
				? nullptr : a8State.a8Shader->GetShaderHandle();
			bool useCoverageShader = !useOriginalShader && a8State.coverageShader
				&& a8State.coverageShader->GetShaderHandle();
			IDirect3DPixelShader9* coverageShader = useCoverageShader
				? a8State.coverageShader->GetShaderHandle() : fillShader;
			auto resolveRangeShader = [&](A8CompiledShaderClass shaderClass)
				-> IDirect3DPixelShader9*
			{
				switch (shaderClass)
				{
				case A8CompiledShaderClass::Coverage:
					return useCoverageShader ? coverageShader : fillShader;
				case A8CompiledShaderClass::Effect:
					return effectShader;
				case A8CompiledShaderClass::Body:
				case A8CompiledShaderClass::Original:
				default:
					return fillShader;
				}
			};

			A8CompiledShaderClass initialShaderClass =
				A8CompiledShaderClass::Coverage;
			if (haveRanges)
			{
				initialShaderClass = metadata.effects.shaderEffects
					&& !useShaderEffects
					? metadata.firstFillShaderClass
					: metadata.firstRangeShaderClass;
			}
			IDirect3DPixelShader9* initialShader = useOriginalShader
				? nullptr : resolveRangeShader(initialShaderClass);
			if (!useOriginalShader && !state.ApplySamplerContract())
			{
				UpdateCurrentRenderTraces([](A8RenderTraceContext& trace)
					{ ++trace.fillFailures; });
				if (detailedTrace)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_shadow_trace: dip-abort serial=%llu dip=%u reason=sampler-contract-apply-failed shader=%p",
						static_cast<unsigned long long>(currentTrace->serial), drawCall,
						initialShader);
				}
				return D3DERR_INVALIDCALL;
			}
			if (!useOriginalShader && !state.SetShader(initialShader))
			{
				bool recovered = false;
				if (initialShaderClass == A8CompiledShaderClass::Coverage
					&& useCoverageShader)
				{
					useCoverageShader = false;
					initialShader = fillShader;
					recovered = state.SetShader(fillShader);
				}
				else if (initialShaderClass == A8CompiledShaderClass::Effect)
				{
					useShaderEffects = false;
					effectShader = nullptr;
					initialShader = fillShader;
					recovered = state.SetShader(fillShader);
				}
				if (!recovered)
				{
					UpdateCurrentRenderTraces([](A8RenderTraceContext& trace)
						{ ++trace.fillFailures; });
					if (detailedTrace)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_shadow_trace: dip-abort serial=%llu dip=%u reason=initial-shader-apply-failed shader=%p",
							static_cast<unsigned long long>(currentTrace->serial), drawCall,
							initialShader);
					}
					return D3DERR_INVALIDCALL;
				}
			}

			if (!haveRanges)
			{
				if (!metadata.effects.atlasTextures.empty())
				{
					NiTexture* atlasTexture = metadata.effects.atlasTextures.front();
					NiDX9TextureData* rendererData = atlasTexture
						? atlasTexture->GetDX9RendererData() : nullptr;
					IDirect3DBaseTexture9* d3dTexture = rendererData
						? rendererData->GetD3DTexture() : nullptr;
					if (!d3dTexture || !state.SetTexture(d3dTexture))
						return D3DERR_INVALIDCALL;
				}
				const float constants[16] = {
					1.0f, 1.0f, 1.0f, 1.0f,
					0.0f, 0.0f, 0.0f, 0.0f,
					0.0f, 0.0f, 0.0f, 0.0f,
					0.0f, 0.0f, 0.0f, 0.0f
				};
				const HRESULT constantResult = useOriginalShader ? D3D_OK
					: state.SetConstants(constants);
				if (FAILED(constantResult))
				{
					UpdateCurrentRenderTraces([](A8RenderTraceContext& trace)
						{ ++trace.fillFailures; });
					if (detailedTrace)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_shadow_trace: dip-abort serial=%llu dip=%u reason=fill-constant-upload-failed hr=0x%08X",
							static_cast<unsigned long long>(currentTrace->serial), drawCall,
							static_cast<UInt32>(constantResult));
					}
					return D3DERR_INVALIDCALL;

				}
				if (!state.SetPassState(false, !useOriginalShader)
					|| (!useOriginalShader
						&& !state.SetSampling(scaledFillSampling,
							scaledFillSampling)))
				{
					UpdateCurrentRenderTraces([](A8RenderTraceContext& trace)
						{ ++trace.fillFailures; });
					if (detailedTrace)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_shadow_trace: dip-abort serial=%llu dip=%u reason=fill-pass-state-failed",
							static_cast<unsigned long long>(currentTrace->serial), drawCall);
					}
					return D3DERR_INVALIDCALL;
				}
				LogShadowTraceDeviceState(device, "fill-ready-no-ranges", drawCall,
					3, initialShader, D3D_OK);
				const HRESULT fillResult = a8State.originalDrawIndexedPrimitive(device, primitiveType,
					baseVertexIndex, minimumVertexIndex, numberOfVertices, startIndex, primitiveCount);
				UpdateCurrentRenderTraces([fillResult](A8RenderTraceContext& trace)
					{
						if (SUCCEEDED(fillResult))
							++trace.fillSuccesses;
						else
							++trace.fillFailures;
					});
				LogShadowTraceDeviceState(device, "fill-complete-no-ranges", drawCall,
					3, initialShader, fillResult);
				return fillResult;
			}

			HRESULT firstEffectFailure = D3D_OK;
			HRESULT firstFillFailure = D3D_OK;
			bool drewRange = false;
			bool drewFill = false;
			UInt32 rangeOrdinal = 0;
			auto recordRangeResult = [](UInt32 layer, HRESULT result)
			{
				UpdateCurrentRenderTraces([layer, result](A8RenderTraceContext& trace)
					{
						if (layer == 3)
						{
							if (SUCCEEDED(result))
								++trace.fillSuccesses;
							else
								++trace.fillFailures;
						}
						else if (SUCCEEDED(result))
							++trace.effectSuccesses;
						else
							++trace.effectFailures;
					});
			};
			thread_local std::vector<IDirect3DBaseTexture9*> resolvedPageTextures;
			thread_local std::vector<UInt8> resolvedPageFlags;
			resolvedPageTextures.assign(metadata.effects.atlasTextures.size(), nullptr);
			resolvedPageFlags.assign(metadata.effects.atlasTextures.size(), 0);
			for (const A8CompiledRange& compiled : metadata.compiledRanges)
			{
				const A8DrawRange& range = compiled.range;
				const UInt32 currentOrdinal = rangeOrdinal++;
				UpdateCurrentRenderTraces([](A8RenderTraceContext& trace)
					{ ++trace.rangeAttempts; });
				// A shader-effect batch cannot reconstruct its CPU masks after a live
				// shader loss. Preserve readable text by drawing only the fill ranges.
				if (metadata.effects.shaderEffects && !useShaderEffects && range.layer != 3)
				{
					recordRangeResult(range.layer, D3DERR_NOTAVAILABLE);
					if (detailedTrace)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_shadow_trace: range-skip serial=%llu dip=%u ordinal=%u layer=%u reason=effect-shader-unavailable quality=%u",
							static_cast<unsigned long long>(currentTrace->serial), drawCall,
							currentOrdinal, range.layer,
							static_cast<UInt32>(metadata.effects.quality));
					}
					continue;
				}

				if (!metadata.effects.atlasTextures.empty())
				{
					if (!resolvedPageFlags[range.atlasPage])
					{
						NiTexture* atlasTexture =
							metadata.effects.atlasTextures[range.atlasPage];
						NiDX9TextureData* rendererData = atlasTexture
							? atlasTexture->GetDX9RendererData() : nullptr;
						resolvedPageTextures[range.atlasPage] = rendererData
							? rendererData->GetD3DTexture() : nullptr;
						resolvedPageFlags[range.atlasPage] = 1;
					}
					IDirect3DBaseTexture9* d3dTexture =
						resolvedPageTextures[range.atlasPage];
					if (!d3dTexture || !state.SetTexture(d3dTexture))
					{
						recordRangeResult(range.layer, D3DERR_INVALIDCALL);
						continue;
					}
				}

				IDirect3DPixelShader9* rangeShader = useOriginalShader
					? nullptr : resolveRangeShader(compiled.shaderClass);
				bool rangeShaderReady = true;
				if (!useOriginalShader)
				{
					rangeShaderReady = state.SetShader(rangeShader);
					if (!rangeShaderReady
						&& compiled.shaderClass == A8CompiledShaderClass::Coverage
						&& useCoverageShader)
					{
						// The coverage-only shader is an optional specialization. A
						// runtime SetPixelShader failure falls back to the generic body
						// shader just like a load failure, without dropping the range.
						useCoverageShader = false;
						rangeShader = fillShader;
						rangeShaderReady = state.SetShader(fillShader);
					}
				}
				// The fixed sampler contract is established once before the first
				// range. Adjacent ranges only switch a compiled target shader; their
				// dynamic filtering is handled by SetSampling below.
				if (!rangeShaderReady)
				{
					const HRESULT shaderResult = D3DERR_INVALIDCALL;
					recordRangeResult(range.layer, shaderResult);
					if (range.layer == 3)
					{
						if (SUCCEEDED(firstFillFailure))
							firstFillFailure = shaderResult;
					}
					else
					{
						if (SUCCEEDED(firstEffectFailure))
							firstEffectFailure = shaderResult;
						useShaderEffects = false;
					}
					if (detailedTrace)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_shadow_trace: range-skip serial=%llu dip=%u ordinal=%u layer=%u reason=shader-apply-failed shader=%p",
							static_cast<unsigned long long>(currentTrace->serial), drawCall,
							currentOrdinal, range.layer, rangeShader);
					}
					continue;
				}
				const HRESULT constantResult = useOriginalShader ? D3D_OK
					: state.SetConstants(compiled.constants.data());
				if (FAILED(constantResult))
				{
					recordRangeResult(range.layer, constantResult);
					if (range.layer == 3)
					{
						if (SUCCEEDED(firstFillFailure))
							firstFillFailure = constantResult;
					}
					else if (SUCCEEDED(firstEffectFailure))
						firstEffectFailure = constantResult;
					if (g_bEnableFreeTypeFontRenderingLog
						&& a8State.rangeDrawFailureLogCount++ < kMaximumRangeDrawFailureLogs)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_a8_diag: range constant upload failed hr=0x%08X layer=%u",
							static_cast<UInt32>(constantResult), range.layer);
					}
					if (detailedTrace)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_shadow_trace: range-skip serial=%llu dip=%u ordinal=%u layer=%u reason=constant-upload-failed hr=0x%08X",
							static_cast<unsigned long long>(currentTrace->serial), drawCall,
							currentOrdinal, range.layer,
							static_cast<UInt32>(constantResult));
					}
					continue;
				}
				const bool needsSmoothSampling = scaledFillSampling
					|| compiled.staticSmoothSampling;
				// SDF texels encode signed distances around the 0.5 isocontour.
				// Coverage mip averaging and trilinear blending can move that contour
				// and bridge neighbouring CJK strokes.  Keep bilinear MIN/MAG filtering
				// for subpixel placement, but force every SDF pass to atlas LOD 0.
				const bool useCoverageMipmaps = needsSmoothSampling && !range.usesSdf;
				const bool passStateReady = state.SetPassState(range.layer != 3,
					!useOriginalShader)
					&& (useOriginalShader
						|| state.SetSampling(needsSmoothSampling, useCoverageMipmaps));
				if (!passStateReady)
				{
					const HRESULT stateResult = D3DERR_INVALIDCALL;
					recordRangeResult(range.layer, stateResult);
					if (range.layer == 3)
					{
						if (SUCCEEDED(firstFillFailure))
							firstFillFailure = stateResult;
					}
					else if (SUCCEEDED(firstEffectFailure))
						firstEffectFailure = stateResult;
					if (detailedTrace)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_shadow_trace: range-skip serial=%llu dip=%u ordinal=%u layer=%u reason=pass-state-failed",
							static_cast<unsigned long long>(currentTrace->serial), drawCall,
							currentOrdinal, range.layer);
					}
					continue;
				}
				if (detailedTrace)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_shadow_trace: range-ready serial=%llu dip=%u ordinal=%u layer=%u geometry=(firstVertex=%u vertices=%u start=%u primitives=%u absoluteStart=%u) color=(%.5g,%.5g,%.5g,%.5g) sdf=%u",
						static_cast<unsigned long long>(currentTrace->serial), drawCall,
						currentOrdinal, range.layer, range.firstVertex, range.vertexCount,
						range.startIndex, range.primitiveCount,
						startIndex + range.startIndex, range.colorModifier.r,
						range.colorModifier.g, range.colorModifier.b,
						range.colorModifier.a, range.usesSdf ? 1 : 0);
					LogShadowTraceDeviceState(device, "range-ready", drawCall,
						static_cast<int>(range.layer), rangeShader, D3D_OK);
				}

				if (!useOriginalShader && g_bEnableFreeTypeFontRenderingLog
					&& range.layer == 0
					&& a8State.shadowContractLogCount++ < 8)
				{
					std::array<float, 4> actualTile = {};
					std::array<float, 4> actualLayer = {};
					DWORD actualSeparateAlpha = 0;
					DWORD actualSourceAlpha = 0;
					DWORD actualDestinationAlpha = 0;
					DWORD actualAlphaOperation = 0;
					DWORD actualColorWrite = 0;
					device->GetPixelShaderConstantF(0, actualTile.data(), 1);
					device->GetPixelShaderConstantF(1, actualLayer.data(), 1);
					device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,
						&actualSeparateAlpha);
					device->GetRenderState(D3DRS_SRCBLENDALPHA, &actualSourceAlpha);
					device->GetRenderState(D3DRS_DESTBLENDALPHA,
						&actualDestinationAlpha);
					device->GetRenderState(D3DRS_BLENDOPALPHA, &actualAlphaOperation);
					device->GetRenderState(D3DRS_COLORWRITEENABLE, &actualColorWrite);
					FreeTypeFontDebugLog(
						"tnvse_freetype_a8_diag: shadow contract=tile-fill-effect-rgb-v7 c0=(%.4f,%.4f,%.4f,%.4f) c1=(%.4f,%.4f,%.4f,%.4f) coverage=atlas-sampled expectedMaxAlpha=%.4f blend=rgb-caller-alpha-source-over alphaState=(separate=%u src=%u dst=%u op=%u) colorWrite=0x%X alphaTest=disabled",
						actualTile[0], actualTile[1], actualTile[2], actualTile[3],
						actualLayer[0], actualLayer[1], actualLayer[2], actualLayer[3],
						actualTile[3] * actualLayer[3], actualSeparateAlpha,
						actualSourceAlpha, actualDestinationAlpha,
						actualAlphaOperation, actualColorWrite);
				}

				if (useShaderEffects)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::ShaderEffectPass);
					RecordFreeTypePerf(FreeTypePerfCounter::ShaderEffectSamples,
						static_cast<UInt64>(compiled.textureSamplesPerGlyph)
							* (range.vertexCount / 4));
				}
				// The engine may submit a shape from a packed vertex buffer where both
				// BaseVertexIndex and MinVertexIndex are non-zero. The range indices are
				// already relative to the original draw, so offsetting MinVertexIndex a
				// second time can make otherwise valid fill passes fail validation.
				// Keep the original full vertex window and restrict only the index range.
				const HRESULT passResult = a8State.originalDrawIndexedPrimitive(device,
					primitiveType, baseVertexIndex, minimumVertexIndex, numberOfVertices,
					startIndex + range.startIndex, range.primitiveCount);
				drewRange = true;
				recordRangeResult(range.layer, passResult);
				if (range.layer == 3)
				{
					drewFill = drewFill || SUCCEEDED(passResult);
					if (FAILED(passResult) && SUCCEEDED(firstFillFailure))
						firstFillFailure = passResult;
				}
				else if (FAILED(passResult) && SUCCEEDED(firstEffectFailure))
					firstEffectFailure = passResult;
				if (FAILED(passResult) && g_bEnableFreeTypeFontRenderingLog
					&& a8State.rangeDrawFailureLogCount++ < kMaximumRangeDrawFailureLogs)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_a8_diag: range draw failed hr=0x%08X layer=%u base=%d min=%u vertices=%u start=%u rangeStart=%u rangePrimitives=%u",
						static_cast<UInt32>(passResult), range.layer, baseVertexIndex,
						minimumVertexIndex, numberOfVertices, startIndex,
						range.startIndex, range.primitiveCount);
				}
				if (detailedTrace)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_shadow_trace: range-result serial=%llu dip=%u ordinal=%u layer=%u hr=0x%08X fill=%u",
						static_cast<unsigned long long>(currentTrace->serial), drawCall,
						currentOrdinal, range.layer, static_cast<UInt32>(passResult),
						range.layer == 3 ? 1 : 0);
					LogShadowTraceDeviceState(device, "range-complete", drawCall,
						static_cast<int>(range.layer), rangeShader, passResult);
				}
			}

			// Effects are optional. A failed shadow/glow/outline pass must not report
			// the whole text draw as failed after its mandatory fill was rendered.
			if (detailedTrace)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_shadow_trace: dip-end serial=%llu dip=%u drewRange=%u drewFill=%u firstEffectFailure=0x%08X firstFillFailure=0x%08X result=%s",
					static_cast<unsigned long long>(currentTrace->serial), drawCall,
					drewRange ? 1 : 0, drewFill ? 1 : 0,
					static_cast<UInt32>(firstEffectFailure),
					static_cast<UInt32>(firstFillFailure), drewFill ? "success"
						: FAILED(firstFillFailure) ? "fill-failure"
						: FAILED(firstEffectFailure) ? "effect-failure" : "no-range");
			}
			if (drewFill)
				return D3D_OK;
			if (FAILED(firstFillFailure))
				return firstFillFailure;
			if (FAILED(firstEffectFailure))
				return firstEffectFailure;
			return drewRange ? D3D_OK : D3DERR_INVALIDCALL;
		}
}

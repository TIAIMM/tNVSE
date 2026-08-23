#include "font_native_shader_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_native_shader {}
	using namespace implementation::font_native_shader;

	namespace implementation::font_native_shader
	{
		NativeFontStandardBlendSemantics ClassifyStandardBlendCallback(
			void* callback)
		{
			if (callback == reinterpret_cast<void*>(
					&NativeSetupGeometryAlphaBlending))
			{
				return NativeFontStandardBlendSemantics::NativeOwned;
			}
			if (callback == reinterpret_cast<void*>(
					kBSShaderSetupGeometryAlphaBlending))
			{
				return NativeFontStandardBlendSemantics::Retail;
			}
			return NativeFontStandardBlendSemantics::Unknown;
		}

		void ResetSortedShaderStateCaches()
		{
			NativeSortedShaderBatch& batch = ShaderThread().sortedShaderBatch;
			batch.device = nullptr;
			batch.renderState = nullptr;
			batch.generation = 0;
			batch.packetProfile = nullptr;
			batch.packetRegisterCount = 0;
			batch.vertexAa = {};
			batch.samplerReady = false;

			// If invalidation happens during a nested pass inside one facade, its
			// per-facade fallback must not resurrect constants from before that pass.
			NativeFacadeShaderBatch& facadeBatch = ShaderThread().facadeShaderBatch;
			facadeBatch.packetProfile = nullptr;
			facadeBatch.packetRegisterCount = 0;
			facadeBatch.vertexAa = {};
			facadeBatch.samplerReady = false;
		}

		const char* StandardBlendSemanticsName(
			NativeFontStandardBlendSemantics semantics)
		{
			switch (semantics)
			{
			case NativeFontStandardBlendSemantics::Retail:
				return "retail";
			case NativeFontStandardBlendSemantics::NativeOwned:
				return "tnvse-owned";
			default:
				return "unknown";
			}
		}

		bool HasShaderHandle(const NiD3DVertexShaderPtr& shader)
		{
			return shader && shader->GetShaderHandle();
		}

		bool HasShaderHandle(const NiD3DPixelShaderPtr& shader)
		{
			return shader && shader->GetShaderHandle();
		}

		bool GenerationResourcesReady(const NativeShaderGeneration* generation)
		{
			if (!generation || !generation->renderer || !generation->device
				|| generation->runtimeFault.load(std::memory_order_acquire)
				|| !generation->declaration || !generation->d3dDeclaration
				|| !HasShaderHandle(generation->vertexShader)
				|| (UsesBakedEffectRoute()
					&& (!HasShaderHandle(generation->coverageShader)
						|| !HasShaderHandle(generation->argbShader))))
			{
				return false;
			}
			if (!UsesBakedEffectRoute())
			{
				for (const NiD3DPixelShaderPtr& shader
					: generation->distanceFieldFillShaders)
				{
					if (!HasShaderHandle(shader))
						return false;
				}
				for (const NiD3DPixelShaderPtr& shader
					: generation->effectShaders)
				{
					if (!HasShaderHandle(shader))
						return false;
				}
			}
			return true;
		}

		bool GenerationMatchesCurrentDevice(
			const NativeShaderGeneration* generation)
		{
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			if (ShaderState().resetLifecycle.InProgress())
				return false;
			IDirect3DDevice9* device = renderer ? renderer->GetD3DDevice() : nullptr;
			return GenerationResourcesReady(generation)
				&& generation->renderer == renderer && generation->device == device;
		}

		void MarkGenerationFault(NativeShaderGeneration* generation,
			const char* operation, HRESULT result)
		{
			if (!generation)
				return;
			generation->runtimeFault.store(true, std::memory_order_release);
			NotifyNativeFontCommandExternalMutation(
				NativeFontCommandFallback::Generation);
			bool expected = false;
			if (generation->runtimeFaultLogged.compare_exchange_strong(expected,
				true, std::memory_order_acq_rel))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: runtime fault generation=%u operation=%s hr=0x%08X; disabling this generation so subsequent submissions retry native",
					generation->id, operation ? operation : "unknown",
					static_cast<UInt32>(result));
			}
		}

		NativeTileVtableBlock* RecoverNativeVtableBlock(TileShader* shader)
		{
			if (!shader)
				return nullptr;
			void** vtable = *reinterpret_cast<void***>(shader);
			if (!vtable)
				return nullptr;
			auto* block = reinterpret_cast<NativeTileVtableBlock*>(
				reinterpret_cast<UInt8*>(vtable)
				- offsetof(NativeTileVtableBlock, slots));
			return block->magic == kNativeVtableMagic ? block : nullptr;
		}

		bool HasExactVanillaLayoutConstantCarryChain(TileShader* shader)
		{
			NativeTileVtableBlock* block = RecoverNativeVtableBlock(shader);
			NativeShaderProfile* profile = block ? block->profile : nullptr;
			if (!profile || profile->shader != shader
				|| !UsesNativeFontVanillaLayout(
					profile->key.vanillaLayoutKind))
			{
				return false;
			}
			const NativeFontCompiledPacketCommand& program =
				profile->retainedProgram;
			return program.setupGeometryTextures == reinterpret_cast<void*>(
					kTileShaderSetupGeometryTextures)
				&& block->vanillaSetupGeometryConstants
					== reinterpret_cast<VanillaSetupGeometryConstantsFn>(
						kTileShaderSetupGeometryConstants)
				&& program.setupGeometryConstants
					== reinterpret_cast<void*>(&NativeSetupGeometryConstants)
				&& program.setupGeometryAlphaBlending == reinterpret_cast<void*>(
					&NativeSetupGeometryAlphaBlending)
				&& program.setupGeometryAlphaTesting == reinterpret_cast<void*>(
					kBSShaderSetupGeometryAlphaTesting)
				&& program.setupGeometryRenderStates == reinterpret_cast<void*>(
					kBSShaderSetupGeometryRenderStates)
				&& program.prepareGeometryForRendering == reinterpret_cast<void*>(
					kNiD3DShaderPrepareGeometryForRendering)
				&& program.postGeometry == reinterpret_cast<void*>(
					kTileShaderPostGeometry);
		}

		bool ResolveVanillaTilePixelConstant(const NiPropertyState* properties,
			float* output)
		{
			if (!properties || !output)
				return false;
			const NiShadeProperty* shade = properties->m_spShadeProperty.m_pObject;
			if (!shade || shade->m_eShaderType != NiShadeProperty::PROP_Tile)
				return false;

			const auto* tile = reinterpret_cast<const TilePixelConstantView*>(shade);
			const NiMaterialProperty* material =
				properties->m_spMaterialProperty.m_pObject;
			const float materialAlpha = material ? material->m_fAlpha : 1.0f;
			output[0] = tile->overlayColor.r;
			output[1] = tile->overlayColor.g;
			output[2] = tile->overlayColor.b;
			output[3] = tile->tileAlpha * materialAlpha;
			return true;
		}

		NiD3DRenderState* ResolveEngineRenderState(
			IDirect3DDevice9* device)
		{
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			NiD3DRenderState* renderState = renderer
				? reinterpret_cast<NiD3DRenderState*>(
					renderer->m_pkRenderState)
				: nullptr;
			return device && renderer
				&& renderer->GetD3DDevice() == device
				&& renderState && renderState->m_pkD3DDevice == device
				? renderState : nullptr;
		}

		void ResetSortedShaderIdentity(NativeSortedShaderBatch& batch,
			IDirect3DDevice9* device, UInt32 generation)
		{
			batch.device = device;
			batch.renderState = ResolveEngineRenderState(device);
			batch.generation = generation;
			batch.packetProfile = nullptr;
			batch.packetRegisterCount = 0;
			batch.vertexAa = {};
			batch.samplerReady = false;
		}

		NiD3DRenderState* ResolveSortedRenderState(
			IDirect3DDevice9* device)
		{
			NativeSortedShaderBatch& batch = ShaderThread().sortedShaderBatch;
			if (batch.depth && batch.device == device && batch.renderState
				&& batch.renderState->m_pkD3DDevice == device)
			{
				return batch.renderState;
			}
			NiD3DRenderState* renderState =
				ResolveEngineRenderState(device);
			if (batch.depth && batch.device == device)
				batch.renderState = renderState;
			return renderState;
		}

		bool IsNativeSamplerMirrorReady(
			const NiD3DRenderState* renderState)
		{
			if (!renderState)
				return false;
			const auto& mirror = renderState->m_akSamplerStateSettings[0];
			// Keep the per-packet proof branch-free apart from short-circuit
			// failures. The table-driven loop belongs only to the cold publication
			// path; spelling out five constant entries lets Release fold this into
			// five direct mirror comparisons with no loop bookkeeping.
			return mirror[kNativeFontSamplerContract[0].mirrorIndex].m_uiCurrValue
					== kNativeFontSamplerContract[0].value
				&& mirror[kNativeFontSamplerContract[1].mirrorIndex].m_uiCurrValue
					== kNativeFontSamplerContract[1].value
				&& mirror[kNativeFontSamplerContract[2].mirrorIndex].m_uiCurrValue
					== kNativeFontSamplerContract[2].value
				&& mirror[kNativeFontSamplerContract[3].mirrorIndex].m_uiCurrValue
					== kNativeFontSamplerContract[3].value
				&& mirror[kNativeFontSamplerContract[4].mirrorIndex].m_uiCurrValue
					== kNativeFontSamplerContract[4].value;
		}

		bool ResolveEngineViewport(IDirect3DDevice9* device,
			D3DVIEWPORT9& viewport)
		{
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			if (!device || !renderer
				|| renderer->GetD3DDevice() != device)
			{
				return false;
			}
			viewport = renderer->m_kD3DPort;
			return viewport.Width && viewport.Height;
		}

		HRESULT EnsureNativeSamplerState(IDirect3DDevice9* device,
			bool& changed, const char*& operation);

		HRESULT PublishNativeVertexAaConstant(IDirect3DDevice9* device,
			float rasterScale, NativeVertexAaState* cache,
			bool forcePublish, bool* published, const char*& operation)
		{
			operation = "none";
			if (published)
				*published = false;
			if (!device || !std::isfinite(rasterScale) || rasterScale <= 0.0f)
			{
				operation = "resolve-vertex-aa-profile";
				return D3DERR_INVALIDCALL;
			}

			D3DVIEWPORT9 viewport = {};
			if (cache && cache->viewportReady)
			{
				viewport = cache->viewport;
			}
			else
			{
				if (!ResolveEngineViewport(device, viewport))
				{
					operation = "resolve-engine-viewport(vertex-aa)";
					return D3DERR_INVALIDCALL;
				}
				if (cache)
				{
					cache->viewport = viewport;
					cache->viewportReady = true;
				}
			}

			if (!forcePublish && cache && cache->aaConstantReady
				&& std::memcmp(&cache->rasterScale, &rasterScale,
					sizeof(rasterScale)) == 0)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::VertexAaConstantReuse);
				return D3D_OK;
			}

			const std::array<float, 4> aaProfile = {
				static_cast<float>(viewport.Width) * 0.5f,
				static_cast<float>(viewport.Height) * 0.5f,
				rasterScale, 1.0f
			};
			// The verified TileShader callback owns only c0-c4. Reuse c208 only
			// inside a native-owned execution segment; every unrelated RenderPass
			// hard-invalidates this cache even if device and viewport are unchanged.
			const HRESULT constantResult = device->SetVertexShaderConstantF(
				kNativeFontVertexAaConstantRegister, aaProfile.data(), 1);
			if (FAILED(constantResult))
			{
				operation = "SetVertexShaderConstantF(c208-aa)";
				return constantResult;
			}
			if (cache)
			{
				cache->rasterScale = rasterScale;
				cache->aaConstantReady = true;
			}
			if (published)
				*published = true;
			RecordFreeTypePerf(
				FreeTypePerfCounter::VertexAaConstantSet);
			return D3D_OK;
		}

		HRESULT PublishNativeVanillaLayoutVertexConstants(
			IDirect3DDevice9* device, float rasterScale, float spread,
			float distanceParameterScale, UInt8 layerMask,
			NativeFontVanillaLayoutKind layoutKind,
			NativeVertexAaState* cache,
			const char*& operation)
		{
			operation = "none";
			const bool uniformLayout =
				layoutKind == NativeFontVanillaLayoutKind::Uniform40;
			const bool parametricLayout =
				layoutKind == NativeFontVanillaLayoutKind::Parametric48;
			if (!device || !std::isfinite(rasterScale)
				|| rasterScale <= 0.0f || (!uniformLayout && !parametricLayout)
				|| !std::isfinite(spread)
				|| !std::isfinite(distanceParameterScale)
				|| (uniformLayout
					&& (spread <= 0.0f || distanceParameterScale < 1.0f))
				|| (parametricLayout
					&& (spread != 0.0f || distanceParameterScale != 1.0f))
				|| layerMask < kStaticCompositeLayerMaskFirst
				|| layerMask >= kStaticCompositeLayerMaskFirst
					+ kStaticCompositeLayerMaskCount)
			{
				operation = "resolve-vanilla-layout-vertex-profile";
				return D3DERR_INVALIDCALL;
			}

			D3DVIEWPORT9 viewport = {};
			if (cache && cache->viewportReady)
			{
				viewport = cache->viewport;
			}
			else
			{
				if (!ResolveEngineViewport(device, viewport))
				{
					operation =
						"resolve-engine-viewport(vanilla-layout-vertex)";
					return D3DERR_INVALIDCALL;
				}
				if (cache)
				{
					cache->viewport = viewport;
					cache->viewportReady = true;
				}
			}

			// c208 and c209 are deliberately adjacent: publish the analytic-AA
			// profile and immutable Vanilla-layout glyph profile in one driver call.
			// The cache becomes ready only after the complete two-register write.
			const std::array<float, 8> vertexConstants = {{
				static_cast<float>(viewport.Width) * 0.5f,
				static_cast<float>(viewport.Height) * 0.5f,
				rasterScale, 1.0f,
				spread, distanceParameterScale,
				static_cast<float>(layerMask), 0.0f
			}};
			if (cache)
			{
				cache->aaConstantReady = false;
				cache->vanillaGlyphConstantReady = false;
			}
			const HRESULT constantResult = device->SetVertexShaderConstantF(
				kNativeFontVertexAaConstantRegister,
				vertexConstants.data(), 2);
			if (FAILED(constantResult))
			{
				operation =
					"SetVertexShaderConstantF(c208-c209-vanilla-layout)";
				return constantResult;
			}
			if (cache)
			{
				cache->rasterScale = rasterScale;
				cache->aaConstantReady = true;
				cache->vanillaGlyphConstantReady = true;
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::VertexAaConstantSet);
			return D3D_OK;
		}

		void __fastcall NativeSetupGeometryAlphaBlending(
			TileShader*, void*, const NiPropertyState* properties)
		{
			// Normalize the category before applying the final state. This preserves
			// the blend-leak and No_Fade fixes without borrowing a third-party
			// callback whose identity or implementation may change independently.
			CdeclCall<void>(kBSRenderStateSetAlphaBlendEnable, 0, 0);
			const NativeFontBlendState state =
				ComputeNativeFontOwnedBlendState(properties);
			if (!state.enabled)
				return;

			CdeclCall<void>(kBSRenderStateSetAlphaBlendEnable, 1, 0);
			CdeclCall<void>(kBSRenderStateSetAlphaBlendFunc,
				static_cast<UInt32>(state.sourceFunction),
				static_cast<UInt32>(state.destinationFunction), 0);
		}

		void __fastcall NativeSetupGeometryConstants(TileShader* shader, void*,
			const NiPropertyState* properties)
		{
			// Preserve Tile's matrix, live color/fade, scissor and alpha contract.
			NativeTileVtableBlock* block = RecoverNativeVtableBlock(shader);
			VanillaSetupGeometryConstantsFn vanillaSetupGeometryConstants =
				block && block->vanillaSetupGeometryConstants
				? block->vanillaSetupGeometryConstants
				: reinterpret_cast<VanillaSetupGeometryConstantsFn>(
					kTileShaderSetupGeometryConstants);
			NativeFacadeShaderBatch& batch = ShaderThread().facadeShaderBatch;
			const bool batchActive = batch.depth != 0;
			// Retail TileShader::SetupGeometryConstants is also a live render-state
			// synchronization point: it reapplies Tile scissor and stencil state.
			// Standard v2 may suppress this whole wrapper only inside one
			// validated execution segment, with the verified retail slot retained,
			// identical transform/color/camera inputs, and no transient
			// scissor/stencil state. Every other packet still enters here and keeps
			// the exact vanilla slot-31/35 pairing.
			vanillaSetupGeometryConstants(shader, properties);
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaConstantUpdate);
			NativeShaderProfile* profile = block ? block->profile : nullptr;
			if (!profile || profile->shader != shader || !profile->owner)
			{
				bool expected = false;
				if (ShaderState().invalidVtableLogged.compare_exchange_strong(expected, true,
					std::memory_order_acq_rel))
				{
					gLog.FormattedMessage(
						"tnvse_freetype_native: invalid TileShader sidecar in SetupGeometryConstants; native generation disabled and affected submissions will be suppressed");
				}
				NativeShaderGeneration* current = ShaderState().publishedGeneration.load(
					std::memory_order_acquire);
				if (current)
				{
					current->runtimeFault.store(true, std::memory_order_release);
					NotifyNativeFontCommandExternalMutation(
						NativeFontCommandFallback::Generation);
				}
				return;
			}

			NativeShaderGeneration* generation = profile->owner;
			const bool coverageProfile =
				profile->key.shaderClass == NativeFontShaderClass::Coverage;
			const bool simpleColorProfile = coverageProfile
				|| profile->key.shaderClass == NativeFontShaderClass::Argb;
			IDirect3DDevice9* device = shader->m_pkD3DDevice;
			if (!device)
				device = generation->device;
			if (!device || device != generation->device)
			{
				MarkGenerationFault(generation, "device-mismatch",
					D3DERR_DEVICELOST);
				return;
			}
			// Both reverse targets prove that the unmodified vanilla slot 31 owns
			// tintcolor at PS c0 and submits it through the pixel constant map.
			// Since native packet data is discontiguous at c176-c183, a second c0
			// publication cannot protect or refresh any tNVSE-owned register.
			//
			// A plugin-replaced vtable slot does not carry that proof. Preserve
			// the old explicit publication only for that compatibility case.
			if (vanillaSetupGeometryConstants
				!= reinterpret_cast<VanillaSetupGeometryConstantsFn>(
					kTileShaderSetupGeometryConstants))
			{
				std::array<float, 4> tileConstant;
				if (!ResolveVanillaTilePixelConstant(
					properties, tileConstant.data()))
				{
					MarkGenerationFault(generation,
						"ResolveVanillaTilePixelConstant(compat)", E_FAIL);
					return;
				}
				const HRESULT vanillaConstantResult =
					device->SetPixelShaderConstantF(
						0, tileConstant.data(), 1);
				if (FAILED(vanillaConstantResult))
				{
					MarkGenerationFault(generation,
						"SetPixelShaderConstantF(vanilla-c0-compat)",
						vanillaConstantResult);
					return;
				}
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						VanillaPixelConstantCompatibilityRepublish);
			}
			// Saved c0 publications are derived from vanilla_constant_updates minus
			// compat_republishes when reporting, avoiding another per-packet
			// atomic counter update on the verified retail path.
			// Coverage/ARGB read only the vanilla c0 value. Their pixel programs do
			// not consume the private block or the VS analytic-width output, and
			// neither vanilla constant map touches the already cached high ranges.
			// Return before all sorted/facade high-state bookkeeping.
			if (simpleColorProfile)
				return;

			NativeSortedShaderBatch& sortedBatch = ShaderThread().sortedShaderBatch;
			const bool sortedBatchActive = sortedBatch.depth != 0;
			if (sortedBatchActive
				&& (sortedBatch.device != device
					|| sortedBatch.generation != generation->id))
			{
				// Device/generation identity is shared by the sampler and private
				// c176-c183 caches. A change invalidates both before this
				// submission can publish a new identity.
				ResetSortedShaderIdentity(
					sortedBatch, device, generation->id);
			}
			const NativeShaderProfile* cachedProfile = nullptr;
			UInt8 cachedRegisterCount = 0;
			if (sortedBatchActive && sortedBatch.packetProfile)
			{
				cachedProfile = sortedBatch.packetProfile;
				cachedRegisterCount =
					sortedBatch.packetRegisterCount;
			}
			else if (batchActive && batch.packetProfile)
			{
				cachedProfile = batch.packetProfile;
				cachedRegisterCount = batch.packetRegisterCount;
			}
			NativeVertexAaState* vertexAaCache = sortedBatchActive
				? &sortedBatch.vertexAa
				: batchActive ? &batch.vertexAa : nullptr;
			const size_t targetRegisterCount =
				profile->privateRegisterCount;

			HRESULT constantsResult = D3D_OK;
			const bool vertexConstantsReady = vertexAaCache
				&& vertexAaCache->aaConstantReady
				&& (!UsesNativeFontVanillaLayout(
						profile->key.vanillaLayoutKind)
					|| vertexAaCache->vanillaGlyphConstantReady);
			if (cachedProfile == profile && vertexConstantsReady)
			{
				// The profile is process-lifetime immutable. Exact identity proves
				// its c176-c183 block plus c208 and, for Vanilla-layout, c209 are
				// unchanged; traversal invalidation separately protects viewport and
				// device state.
				RecordFreeTypePerf(
					FreeTypePerfCounter::VertexAaConstantReuse);
				RecordFreeTypePerf(
					FreeTypePerfCounter::VertexAaConstantVanillaPreserved);
			}
			else if (UsesNativeFontVanillaLayout(
				profile->key.vanillaLayoutKind))
			{
				const char* vertexOperation = "none";
				const HRESULT vertexResult =
					PublishNativeVanillaLayoutVertexConstants(device,
						profile->constants[7],
						profile->vanillaUniformSdfSpread,
						profile->vanillaUniformDistanceParameterScale,
						profile->key.staticCompositeLayerMask,
						profile->key.vanillaLayoutKind,
						vertexAaCache, vertexOperation);
				if (FAILED(vertexResult))
				{
					MarkGenerationFault(
						generation, vertexOperation, vertexResult);
					return;
				}
			}
			else
			{
				const char* vertexAaOperation = "none";
				bool vertexAaPublished = false;
				const HRESULT vertexAaResult = PublishNativeVertexAaConstant(
					device, profile->constants[7], vertexAaCache, false,
					&vertexAaPublished,
					vertexAaOperation);
				if (FAILED(vertexAaResult))
				{
					MarkGenerationFault(generation, vertexAaOperation,
						vertexAaResult);
					return;
				}
				if (!vertexAaPublished)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							VertexAaConstantVanillaPreserved);
				}
			}

			// Vanilla slot 31 has already published live Tile color/alpha at c0.
			// Only the discontiguous, immutable tNVSE block needs attention here.
			const char* constantsOperation = "none";
			{
				if (!cachedProfile || !cachedRegisterCount)
				{
					constantsOperation =
						"SetPixelShaderConstantF(native-private-prefix)";
					constantsResult = device->SetPixelShaderConstantF(
						kNativeFontPixelConstantBaseRegister,
						profile->constants.data(), static_cast<UINT>(
							targetRegisterCount));
					if (SUCCEEDED(constantsResult))
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::CompositeConstantFullUpload);
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								NativePacketConstantRegisterUpload,
							targetRegisterCount);
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								NativePacketConstantFullTailElided,
							kNativeFontPacketConstantRegisterCount
								- targetRegisterCount);
					}
				}
				else
				{
					size_t firstChanged =
						targetRegisterCount;
					size_t lastChanged = 0;
					// A profile is immutable and interned by its exact key, so pointer
					// identity plus a complete valid prefix proves every register
					// read by the target shader is unchanged.
					if (cachedProfile != profile
						|| cachedRegisterCount < targetRegisterCount)
					{
						for (size_t packetRegister = 0;
							packetRegister
								< targetRegisterCount;
							++packetRegister)
						{
							const size_t firstFloat = packetRegister * 4u;
							if (packetRegister >= cachedRegisterCount
								|| std::memcmp(
									profile->constants.data() + firstFloat,
									cachedProfile->constants.data() + firstFloat,
									4u * sizeof(float)) != 0)
							{
								firstChanged = std::min(
									firstChanged, packetRegister);
								lastChanged = packetRegister;
							}
						}
					}
					if (firstChanged < targetRegisterCount)
					{
						constantsOperation =
							"SetPixelShaderConstantF(native-high-partial)";
						constantsResult = device->SetPixelShaderConstantF(
							static_cast<UINT>(
								kNativeFontPixelConstantBaseRegister
									+ firstChanged),
							profile->constants.data() + firstChanged * 4u,
							static_cast<UINT>(
								lastChanged - firstChanged + 1u));
						if (SUCCEEDED(constantsResult))
						{
							RecordFreeTypePerf(
								FreeTypePerfCounter::
									CompositeConstantPartialUpload);
							RecordFreeTypePerf(
								FreeTypePerfCounter::
									NativePacketConstantRegisterUpload,
								lastChanged - firstChanged + 1u);
						}
					}
					else
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::NativePacketConstantReuse);
					}
				}
			}
			if (FAILED(constantsResult))
			{
				MarkGenerationFault(
					generation, constantsOperation, constantsResult);
				return;
			}
			if (batchActive)
			{
				batch.packetProfile = profile;
				batch.packetRegisterCount =
					profile->privateRegisterCount;
			}
			if (sortedBatchActive)
			{
				sortedBatch.packetProfile = profile;
				sortedBatch.packetRegisterCount =
					profile->privateRegisterCount;
			}

			// Distance-field channels are numeric linear data and require the
			// explicit level-zero sampler state below.
			const bool samplerMirrorReady = IsNativeSamplerMirrorReady(
				ResolveSortedRenderState(device));
			const bool sortedSamplerReady = sortedBatch.depth
				&& sortedBatch.samplerReady
				&& sortedBatch.device == device
				&& sortedBatch.generation == generation->id
				&& samplerMirrorReady;
			if ((!batchActive || !batch.samplerReady
					|| !samplerMirrorReady)
				&& !sortedSamplerReady)
			{
				bool samplerChanged = false;
				const char* samplerOperation = "none";
				const HRESULT samplerResult = EnsureNativeSamplerState(
					device, samplerChanged, samplerOperation);
				if (FAILED(samplerResult))
				{
					MarkGenerationFault(generation,
						samplerOperation, samplerResult);
					return;
				}
				if (batchActive)
					batch.samplerReady = true;
				if (sortedBatch.depth)
				{
					sortedBatch.device = device;
					sortedBatch.generation = generation->id;
					sortedBatch.samplerReady = true;
				}
				RecordFreeTypePerf(samplerChanged
					? FreeTypePerfCounter::SamplerStateSet
					: FreeTypePerfCounter::SamplerStateReuse);
			}
			else
			{
				if (batchActive)
					batch.samplerReady = true;
				RecordFreeTypePerf(
					FreeTypePerfCounter::SamplerStateReuse);
			}

			// The retail-layout transition may retain only tNVSE's private high
			// registers. Sign the witness after every required publication and sampler
			// check has succeeded. If the callback is bypassed, re-entered, faulted, or
			// observes another shader/generation, End rejects the carry and clears it.
			NativeVanillaLayoutPublicationWitness& witness =
				ShaderThread().vanillaLayoutPublicationWitness;
			if (witness.armed && witness.expectedShader == shader
				&& UsesNativeFontVanillaLayout(
					profile->key.vanillaLayoutKind) && batchActive
				&& sortedBatchActive
				&& sortedBatch.packetProfile == profile
				&& sortedBatch.device == device
				&& sortedBatch.generation == generation->id
				&& sortedBatch.vertexAa.aaConstantReady
				&& sortedBatch.vertexAa.vanillaGlyphConstantReady)
			{
				witness.publishedToken = witness.token;
				witness.publishedProfile = profile;
				witness.publishedDevice = device;
				witness.publishedGeneration = generation->id;
			}
		}
	}

	void BeginNativeFontSortedShaderBatch()
	{
		NativeSortedShaderBatch& batch = ShaderThread().sortedShaderBatch;
		if (!batch.depth++)
		{
			batch.device = nullptr;
			batch.renderState = nullptr;
			batch.generation = 0;
			batch.packetProfile = nullptr;
			batch.packetRegisterCount = 0;
			batch.vertexAa = {};
			batch.samplerReady = false;
		}
	}

	void EndNativeFontSortedShaderBatch()
	{
		NativeSortedShaderBatch& batch = ShaderThread().sortedShaderBatch;
		if (!batch.depth)
			return;
		if (!--batch.depth)
		{
			batch.device = nullptr;
			batch.renderState = nullptr;
			batch.generation = 0;
			batch.packetProfile = nullptr;
			batch.packetRegisterCount = 0;
			batch.vertexAa = {};
			batch.samplerReady = false;
		}
	}

	void InvalidateNativeFontSortedShaderState()
	{
		InvalidateNativeFontCommandExecutionSegment(
			NativeFontCommandFallback::State);
		ResetSortedShaderStateCaches();
	}

	void InvalidateNativeFontSortedShaderStateWithinExecutionSegment()
	{
		ResetSortedShaderStateCaches();
	}

	void InvalidateNativeFontSortedShaderStateForForeignRenderPass()
	{
		const NativeSortedShaderBatch& sortedBatch = ShaderThread().sortedShaderBatch;
		const NativeFacadeShaderBatch& facadeBatch = ShaderThread().facadeShaderBatch;
		const bool hadPrivateState = (sortedBatch.depth
			&& (sortedBatch.packetProfile
				|| sortedBatch.vertexAa.aaConstantReady
				|| sortedBatch.vertexAa.vanillaGlyphConstantReady))
			|| (facadeBatch.depth
				&& (facadeBatch.packetProfile
					|| facadeBatch.vertexAa.aaConstantReady
					|| facadeBatch.vertexAa.vanillaGlyphConstantReady));
		InvalidateNativeFontSortedShaderState();
		if (hadPrivateState)
		{
			RecordFreeTypePerf(FreeTypePerfCounter::
				NativePrivateStateForeignRenderPassInvalidation);
		}
	}

	static void PrepareNativeFontVanillaLayoutPrivateStateCarry()
	{
		// This helper is private to the exact Vanilla-layout shader transition.
		// Preserve only the immutable c176-c183/c208/c209 shadow signed by that
		// transition while invalidating its command proof and mutable bindings.
		// Arbitrary non-native-font RenderPasses use the hard invalidation path above:
		// both reversed executables dispatch generic shader callbacks there.
		InvalidateNativeFontCommandExecutionSegment(
			NativeFontCommandFallback::State);
		NativeSortedShaderBatch& batch = ShaderThread().sortedShaderBatch;
		NiD3DRenderState* renderState =
			batch.depth && batch.device
				&& IsNativeFontShaderGenerationCurrent(batch.generation)
				? ResolveEngineRenderState(batch.device) : nullptr;
		if (!renderState)
		{
			batch.device = nullptr;
			batch.renderState = nullptr;
			batch.generation = 0;
			batch.packetProfile = nullptr;
			batch.packetRegisterCount = 0;
			batch.vertexAa = {};
		}
		else
		{
			batch.renderState = renderState;
			if (batch.vertexAa.viewportReady)
			{
				D3DVIEWPORT9 viewport = {};
				if (!ResolveEngineViewport(batch.device, viewport)
					|| viewport.Width != batch.vertexAa.viewport.Width
					|| viewport.Height != batch.vertexAa.viewport.Height)
				{
					// Pixel packet constants are viewport-independent; retain
					// them even when c208 must be rebuilt.
					batch.vertexAa = {};
				}
			}
		}
		batch.samplerReady = false;

		// A certified Vanilla-layout transition cannot occur inside another valid
		// facade scope, but clear the fallback cache defensively so a nested
		// compatibility path never inherits the outer facade's proof.
		NativeFacadeShaderBatch& facadeBatch = ShaderThread().facadeShaderBatch;
		facadeBatch.packetProfile = nullptr;
		facadeBatch.packetRegisterCount = 0;
		facadeBatch.vertexAa = {};
		facadeBatch.samplerReady = false;
	}

	static void ValidateNativeFontVanillaLayoutPrivateStateCarry()
	{
		NativeSortedShaderBatch& batch = ShaderThread().sortedShaderBatch;
		NiD3DRenderState* renderState =
			batch.depth && batch.device
				&& IsNativeFontShaderGenerationCurrent(batch.generation)
				? ResolveEngineRenderState(batch.device) : nullptr;
		if (!renderState)
		{
			// A reset/generation transition during the certified native draw
			// invalidates the private proof as well as ordinary bindings.
			batch.device = nullptr;
			batch.renderState = nullptr;
			batch.generation = 0;
			batch.packetProfile = nullptr;
			batch.packetRegisterCount = 0;
			batch.vertexAa = {};
			batch.samplerReady = false;
			return;
		}
		batch.renderState = renderState;
		if (batch.vertexAa.viewportReady)
		{
			D3DVIEWPORT9 viewport = {};
			if (!ResolveEngineViewport(batch.device, viewport)
				|| viewport.Width != batch.vertexAa.viewport.Width
				|| viewport.Height != batch.vertexAa.viewport.Height)
			{
				batch.vertexAa = {};
			}
		}
		if (batch.packetProfile || batch.vertexAa.aaConstantReady
			|| batch.vertexAa.vanillaGlyphConstantReady)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					NativePrivateStateVanillaLayoutPreserve);
		}
	}

	void BeginNativeFontFacadeShaderBatch()
	{
		NativeFacadeShaderBatch& batch = ShaderThread().facadeShaderBatch;
		++batch.depth;
		// Each scope owns one facade. A recursive scope deliberately discards the
		// parent's cached constants; End invalidates the parent again so its next
		// packet performs one full native pixel-constant upload.
		batch.packetProfile = nullptr;
		batch.packetRegisterCount = 0;
		batch.vertexAa = {};
		batch.samplerReady = false;
	}

	void EndNativeFontFacadeShaderBatch()
	{
		NativeFacadeShaderBatch& batch = ShaderThread().facadeShaderBatch;
		if (!batch.depth)
			return;
		--batch.depth;
		batch.packetProfile = nullptr;
		batch.packetRegisterCount = 0;
		batch.vertexAa = {};
		batch.samplerReady = false;
	}

	void MarkNativeFontGenerationFault(UInt32 generation,
		const char* operation, HRESULT result)
	{
		NativeShaderGeneration* current = ShaderState().publishedGeneration.load(
			std::memory_order_acquire);
		if (current && current->id == generation)
			MarkGenerationFault(current, operation, result);
	}

	TileShader* ResolveNativeFontPacketShader(const NativeFontPacketTemplate& packet,
		const NiTriShape* facade, bool scaledFillSampling,
		NativeFontVanillaLayoutKind vanillaLayoutKind)
	{
		if (!IsNativeFontShaderRendererAvailable())
			return nullptr;
		NativeShaderGeneration* generation = ShaderState().publishedGeneration.load(
			std::memory_order_acquire);
		if (!GenerationMatchesCurrentDevice(generation))
			return nullptr;

		const NativeFontSampling sampling = ResolveEffectiveSampling(packet,
			scaledFillSampling);
		const NiAlphaProperty* alpha = facade
			? facade->GetAlphaProperty() : nullptr;
		const bool writeEffectAlpha = packet.layer != 3
			&& generation->supportsSeparateAlpha
			&& alpha && alpha->GetAlphaBlending();
		const size_t cacheIndex = writeEffectAlpha ? 1u : 0u;
		NativeFontPacketShaderCacheEntry& packetCache =
			UsesNativeFontVanillaLayout(vanillaLayoutKind)
			? packet.vanillaLayoutResolvedShaders[cacheIndex]
			: packet.resolvedShaders[cacheIndex];
		NativeShaderProfile* cachedProfile =
			static_cast<NativeShaderProfile*>(
				packetCache.profile.load(std::memory_order_acquire));
		if (cachedProfile && cachedProfile->owner == generation
			&& cachedProfile->shader
			&& cachedProfile->key.sampling == sampling
			&& cachedProfile->key.writeEffectAlpha == writeEffectAlpha
			&& cachedProfile->key.vanillaLayoutKind == vanillaLayoutKind)
		{
			return cachedProfile->shader;
		}
		const NativeProfileKey key = MakeProfileKey(packet, sampling,
			writeEffectAlpha, vanillaLayoutKind);
		std::shared_ptr<const NativeProfileMap> snapshot =
			generation->profiles.load(std::memory_order_acquire);
		auto found = snapshot->find(key);
		if (found != snapshot->end())
		{
			packetCache.profile.store(
				found->second, std::memory_order_release);
			return found->second->shader;
		}

		std::lock_guard<std::mutex> lock(generation->profileMutex);
		if (!GenerationMatchesCurrentDevice(generation))
			return nullptr;
		snapshot = generation->profiles.load(std::memory_order_acquire);
		found = snapshot->find(key);
		if (found != snapshot->end())
		{
			packetCache.profile.store(
				found->second, std::memory_order_release);
			return found->second->shader;
		}

		NativeShaderProfile* profile = CreateProfile(*generation, packet, key);
		if (!profile)
		{
			if (packet.shaderClass == NativeFontShaderClass::Composite)
				return nullptr;
			MarkGenerationFault(generation, "profile-create",
				D3DERR_NOTAVAILABLE);
			return nullptr;
		}
		generation->ownedProfiles.push_back(profile);
		auto updated = std::make_shared<NativeProfileMap>(*snapshot);
		updated->emplace(key, profile);
		generation->profiles.store(
			std::shared_ptr<const NativeProfileMap>(std::move(updated)),
			std::memory_order_release);
		packetCache.profile.store(profile, std::memory_order_release);
		return profile->shader;
	}

	UInt64 BeginNativeFontVanillaLayoutShaderTransition(
		TileShader* shader, UInt32 currentPass)
	{
		// Official B98E80 and beta Standard::RenderPassImmediately both execute
		// SetupGeometryConstants before blend/state setup, buffer preparation and
		// OnlyRenderImmediate. This exact native callback chain may retain its
		// disjoint c176-c183/c208/c209 shadow. The command segment, bindings and
		// sampler proof are still hard boundaries; unrelated RenderPasses are not
		// eligible for this carry.
		PrepareNativeFontVanillaLayoutPrivateStateCarry();
		NativeVanillaLayoutPublicationWitness& witness =
			ShaderThread().vanillaLayoutPublicationWitness;
		UInt64 token = ++ShaderThread().nextVanillaLayoutPublicationToken;
		if (!token)
			token = ++ShaderThread().nextVanillaLayoutPublicationToken;
		witness = {};
		witness.token = token;
		witness.expectedShader = shader;
		// currentPass is the render-pass enum, not a zero-based pass index. Both
		// reverse targets contain special pass enums that bypass the virtual
		// SetupGeometryConstants callback, and their enum values differ. Do not
		// duplicate either executable's switch here: the actual
		// NativeSetupGeometryConstants
		// invocation signs this one-shot witness, while every bypass reaches End with
		// publishedToken == 0 and therefore fails closed. Callback identity is used
		// instead of code hashing; any replaced slot simply disables carry.
		(void)currentPass;
		witness.armed = shader
			&& HasExactVanillaLayoutConstantCarryChain(shader);
		if (!witness.armed)
			ResetSortedShaderStateCaches();
		return token;
	}

	bool EndNativeFontVanillaLayoutShaderTransition(
		UInt64 token, TileShader* shader)
	{
		NativeVanillaLayoutPublicationWitness& witness =
			ShaderThread().vanillaLayoutPublicationWitness;
		NativeSortedShaderBatch& batch = ShaderThread().sortedShaderBatch;
		NativeShaderProfile* publishedProfile = witness.publishedProfile;
		IDirect3DDevice9* publishedDevice = witness.publishedDevice;
		const bool witnessed = witness.armed && token
			&& witness.token == token && witness.publishedToken == token
			&& witness.expectedShader == shader && publishedProfile
			&& publishedProfile->shader == shader
			&& UsesNativeFontVanillaLayout(
				publishedProfile->key.vanillaLayoutKind)
			&& batch.depth && batch.packetProfile == publishedProfile
			&& batch.device == witness.publishedDevice
			&& batch.generation == witness.publishedGeneration
			&& batch.vertexAa.aaConstantReady
			&& batch.vertexAa.vanillaGlyphConstantReady
			&& IsNativeFontShaderGenerationCurrent(batch.generation);
		witness = {};
		if (!witnessed)
		{
			ResetSortedShaderStateCaches();
			RecordFreeTypePerf(FreeTypePerfCounter::
				VanillaLayoutPrivateStateCarryRejected);
			return false;
		}

		// SetupGeometryTextures and retail geometry preparation remain opaque
		// binding boundaries. The completed native constant callback is evidence
		// only for the private constant shadow, never for the sampler or command
		// execution segment.
		batch.samplerReady = false;
		ValidateNativeFontVanillaLayoutPrivateStateCarry();
		const bool retained = batch.packetProfile == publishedProfile
			&& batch.device == publishedDevice;
		RecordFreeTypePerf(retained
			? FreeTypePerfCounter::VanillaLayoutPrivateStateCarry
			: FreeTypePerfCounter::VanillaLayoutPrivateStateCarryRejected);
		return retained;
	}
}

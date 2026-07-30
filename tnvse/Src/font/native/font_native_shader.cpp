#include "font_native_internal.h"

#include "load_config.h"
#include "plugin_dependencies.h"
#include "tnvse.h"
#include "NiAlphaProperty.hpp"

#include "NiD3DPass.hpp"
#include "NiD3DPixelShader.hpp"
#include "NiD3DTextureStage.hpp"
#include "NiD3DVertexShader.hpp"
#include "NiDX9Renderer.hpp"
#include "NiDX9ShaderDeclaration.hpp"
#include "NiGeometryBufferData.hpp"
#include "NiMaterialProperty.hpp"
#include "NiTriShapeData.hpp"
#include "NiPropertyState.hpp"
#include "NiTexturingProperty.hpp"

#include <Windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fonthook::vectorfont
{
	namespace implementation::font_native_shader {}
	using namespace implementation::font_native_shader;

	namespace implementation::font_native_shader
	{
		static_assert(sizeof(void*) == 4,
			"The native FreeType TileShader path is a Win32 ABI implementation");

		struct TilePixelConstantView
		{
			std::array<UInt8, 0x68> prefix;
			NiColorA overlayColor;
			float tileAlpha = 1.0f;
		};

		static_assert(offsetof(TilePixelConstantView, overlayColor) == 0x68);
		static_assert(offsetof(TilePixelConstantView, tileAlpha) == 0x78);

		inline constexpr UInt32 kTileShaderCreate = 0xBCAE90;
		inline constexpr UInt32 kTileShaderUpdateConstants = 0xBCA980;
		inline constexpr UInt32 kShaderDeclarationCreate = 0xE76700;
		inline constexpr UInt32 kTextureStageSetProperties = 0xBE0CF0;
		inline constexpr UInt32 kTextureStageSetFilter = 0xE7DEF0;
		inline constexpr UInt32 kPassSetRenderState = 0xB71A10;
		inline constexpr UInt32 kCopiedTileShaderVtableEntries = 84;
		inline constexpr UInt32 kUpdateConstantsVtableSlot = 31;
		inline constexpr UInt32 kNativeVtableMagic = 0x35544D4E; // "NMT5"
		inline constexpr UInt32 kShaderRefreshMessage = 0;
		inline constexpr DWORD kInitializationRetryMilliseconds = 1000;
		inline constexpr UInt8 kStaticCompositeLayerMaskFirst = 8;
		inline constexpr size_t kStaticCompositeLayerMaskCount = 8;
		inline constexpr size_t kStaticCompositeShiftCount = 2;

		using CreateVertexShaderFn = NiD3DVertexShader* (__cdecl*)(const char*);
		using CreatePixelShaderFn = NiD3DPixelShader* (__cdecl*)(const char*);
		using StockUpdateConstantsFn = void(__thiscall*)(TileShader*,
			const NiPropertyState*);

		struct NativeShaderGeneration;
		struct NativeShaderProfile;

		struct NativeVertexAaState
		{
			D3DVIEWPORT9 viewport = {};
			bool viewportReady = false;
		};

		struct NativeSortedShaderBatch
		{
			IDirect3DDevice9* device = nullptr;
			UInt32 generation = 0;
			NativeShaderProfile* packetProfile = nullptr;
			std::array<float, kNativeA8PacketConstantFloatCount>
				packetConstants = {};
			UInt32 depth = 0;
			NativeVertexAaState vertexAa;
			bool packetConstantsReady = false;
			bool samplerReady = false;
		};

		thread_local NativeSortedShaderBatch s_sortedShaderBatch;

		struct NativeProfileKey
		{
			NativeA8ShaderClass shaderClass = NativeA8ShaderClass::Body;
			NativeA8Sampling sampling = NativeA8Sampling::Point;
			EffectQuality quality = EffectQuality::Balanced;
			DistanceFieldMethod distanceFieldMethod = DistanceFieldMethod::Mtsdf;
			std::array<UInt32, kNativeA8PacketConstantFloatCount> constantBits = {};
			UInt8 staticCompositeLayerMask = 0;
			bool compositeShiftedShadow = false;
			bool writeEffectAlpha = false;
			bool usesLiveTileRgb = true;
			size_t precomputedHash = 0;

			bool operator==(const NativeProfileKey& other) const
			{
				return shaderClass == other.shaderClass
					&& sampling == other.sampling
					&& quality == other.quality
					&& distanceFieldMethod == other.distanceFieldMethod
					&& staticCompositeLayerMask
						== other.staticCompositeLayerMask
					&& compositeShiftedShadow
						== other.compositeShiftedShadow
					&& writeEffectAlpha == other.writeEffectAlpha
					&& usesLiveTileRgb == other.usesLiveTileRgb
					&& constantBits == other.constantBits;
			}
		};

		struct NativeProfileKeyHash
		{
			size_t operator()(const NativeProfileKey& key) const
			{
				if (key.precomputedHash)
					return key.precomputedHash;
				// FNV-1a over the exact immutable profile. Float bit identity is
				// intentional: it preserves the compiled native-constant ABI.
				size_t hash = 2166136261u;
				auto mix = [&hash](UInt32 value)
				{
					hash ^= value;
					hash *= 16777619u;
				};
				mix(static_cast<UInt32>(key.shaderClass));
				mix(static_cast<UInt32>(key.sampling));
				mix(static_cast<UInt32>(key.quality));
				mix(static_cast<UInt32>(key.distanceFieldMethod));
				mix(key.staticCompositeLayerMask);
				mix(key.compositeShiftedShadow ? 1u : 0u);
				mix(key.writeEffectAlpha ? 1u : 0u);
				mix(key.usesLiveTileRgb ? 1u : 0u);
				for (UInt32 value : key.constantBits)
					mix(value);
				return hash;
			}
		};

		struct NativeTileVtableBlock
		{
			// Keep our immutable data before the MSVC RTTI prefix. The object's
			// vptr points at slots, so slots[-1] remains the stock COL pointer.
			NativeShaderProfile* profile = nullptr;
			UInt32 magic = kNativeVtableMagic;
			StockUpdateConstantsFn stockUpdateConstants = nullptr;
			void* rttiPrefix = nullptr;
			std::array<void*, kCopiedTileShaderVtableEntries> slots = {};
		};

		static_assert(offsetof(NativeTileVtableBlock, slots)
			== 4 * sizeof(void*));

		struct NativeShaderProfile
		{
			NativeShaderProfile(NativeShaderGeneration& generation,
				const NativeProfileKey& profileKey,
				const std::array<float,
					kNativeA8PacketConstantFloatCount>& packetConstants)
				: owner(&generation), key(profileKey), constants(packetConstants)
			{
			}

			NativeShaderGeneration* const owner;
			const NativeProfileKey key;
			const std::array<float,
				kNativeA8PacketConstantFloatCount> constants;
			NiPointer<TileShader> shaderOwner;
			TileShader* shader = nullptr;
			NativeTileVtableBlock* vtable = nullptr;
			NativeA8CompiledPacketCommand retainedProgram;
			bool effectPass = false;
		};

		using NativeProfileMap = std::unordered_map<NativeProfileKey,
			NativeShaderProfile*, NativeProfileKeyHash>;

		struct NativeShaderGeneration
		{
			UInt32 id = 0;
			NiDX9Renderer* renderer = nullptr;
			IDirect3DDevice9* device = nullptr;
			NiDX9ShaderDeclarationPtr declaration;
			IDirect3DVertexDeclaration9* d3dDeclaration = nullptr;
			NiD3DVertexShaderPtr vertexShader;
			NiD3DPixelShaderPtr coverageShader;
			NiD3DPixelShaderPtr argbShader;
			std::array<NiD3DPixelShaderPtr, 3> mtsdfFillShaders;
			std::array<NiD3DPixelShaderPtr, 3> effectShaders;
			std::array<NiD3DPixelShaderPtr, 3> compositeShaders;
			std::array<std::array<std::array<NiD3DPixelShaderPtr,
				kStaticCompositeShiftCount>,
				kStaticCompositeLayerMaskCount>, 3>
				mtsdfCompositeProfileShaders;
			std::array<std::array<std::array<bool,
				kStaticCompositeShiftCount>,
				kStaticCompositeLayerMaskCount>, 3>
				mtsdfCompositeProfileAttempts = {};
			CreatePixelShaderFn createPixelShader = nullptr;
			DistanceFieldMethod distanceFieldMethod = DistanceFieldMethod::Mtsdf;
			bool supportsSeparateAlpha = false;
			std::atomic<bool> runtimeFault = false;
			std::atomic<bool> runtimeFaultLogged = false;

			std::mutex profileMutex;
			std::atomic<std::shared_ptr<const NativeProfileMap>> profiles{
				std::make_shared<const NativeProfileMap>() };
			// Generations are process-lifetime objects because NiGeometry stores its
			// shader as a raw pointer. Retaining them makes refresh publication safe.
			std::vector<NativeShaderProfile*> ownedProfiles;
		};

		std::atomic<NativeShaderGeneration*> s_publishedGeneration = nullptr;
		std::mutex s_initializationMutex;
		std::vector<NativeShaderGeneration*> s_processGenerations;
		UInt32 s_nextGeneration = 1;
		DWORD s_lastInitializationAttempt = 0;
		std::atomic<bool> s_invalidVtableLogged = false;
		std::atomic<UInt32> s_compositeProfileLogCount = 0;
		std::atomic<bool> s_resetInProgress = false;
		NiDX9Renderer* s_resetRenderer = nullptr;

		struct NativeFacadeShaderBatch
		{
			std::array<float, kNativeA8PacketConstantFloatCount>
				packetConstants = {};
			UInt32 depth = 0;
			NativeVertexAaState vertexAa;
			bool packetConstantsReady = false;
			bool samplerReady = false;
		};

		thread_local NativeFacadeShaderBatch s_facadeShaderBatch;

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
				|| (g_bEnableFreeTypeFontAggressivePerformanceMode
					&& (!HasShaderHandle(generation->coverageShader)
						|| !HasShaderHandle(generation->argbShader))))
			{
				return false;
			}
			if (!g_bEnableFreeTypeFontAggressivePerformanceMode)
			{
				for (const NiD3DPixelShaderPtr& shader
					: generation->mtsdfFillShaders)
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
			if (s_resetInProgress.load(std::memory_order_acquire))
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
			NotifyNativeA8CommandExternalMutation(
				NativeA8CommandFallback::Generation);
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

		bool ResolveStockTilePixelConstant(const NiPropertyState* properties,
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

		HRESULT PublishNativeVertexAaConstant(IDirect3DDevice9* device,
			float rasterScale, NativeVertexAaState* cache,
			const char*& operation)
		{
			operation = "none";
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
				const HRESULT viewportResult = device->GetViewport(&viewport);
				if (FAILED(viewportResult))
				{
					operation = "GetViewport(vertex-aa)";
					return viewportResult;
				}
				if (!viewport.Width || !viewport.Height)
				{
					operation = "validate-viewport(vertex-aa)";
					return D3DERR_INVALIDCALL;
				}
				if (cache)
				{
					cache->viewport = viewport;
					cache->viewportReady = true;
				}
			}

			const std::array<float, 4> aaProfile = {
				static_cast<float>(viewport.Width) * 0.5f,
				static_cast<float>(viewport.Height) * 0.5f,
				rasterScale, 1.0f
			};
			// Stock TileShader::UpdateConstants runs immediately before this call.
			// Its reflected vertex-constant map is free to rewrite c4 for every
			// packet, including consecutive native facades. Cache only the stable
			// viewport query; the tNVSE-owned AA register must always be republished
			// after the stock update or later glyphs can inherit a zero/stale AA
			// footprint and become visibly aliased.
			const HRESULT constantResult = device->SetVertexShaderConstantF(
				kNativeA8VertexAaConstantRegister, aaProfile.data(), 1);
			if (FAILED(constantResult))
			{
				operation = "SetVertexShaderConstantF(c4-aa)";
				return constantResult;
			}
			return D3D_OK;
		}

		void __fastcall NativeUpdateConstants(TileShader* shader, void*,
			const NiPropertyState* properties)
		{
			// Preserve Tile's matrix, live color/fade, scissor and alpha contract.
			NativeTileVtableBlock* block = RecoverNativeVtableBlock(shader);
			StockUpdateConstantsFn stockUpdate = block && block->stockUpdateConstants
				? block->stockUpdateConstants
				: reinterpret_cast<StockUpdateConstantsFn>(kTileShaderUpdateConstants);
			NativeFacadeShaderBatch& batch = s_facadeShaderBatch;
			const bool batchActive = batch.depth != 0;
			// Retail TileShader::UpdateConstants is also a live render-state
			// synchronization point: it reapplies Tile scissor and alpha state.
			// Every native packet invokes a separate stock Tile render pass, so a
			// preceding effect pass may invalidate those states before the next
			// packet. Never reuse or skip the stock call across packets. Retail
			// slot 35 cleanup runs after that pass and releases the paired scissor
			// and alpha state; do not duplicate that cleanup here.
			stockUpdate(shader, properties);
			RecordFreeTypePerf(
				FreeTypePerfCounter::StockConstantUpdate);
			NativeShaderProfile* profile = block ? block->profile : nullptr;
			if (!profile || profile->shader != shader || !profile->owner)
			{
				bool expected = false;
				if (s_invalidVtableLogged.compare_exchange_strong(expected, true,
					std::memory_order_acq_rel))
				{
					gLog.FormattedMessage(
						"tnvse_freetype_native: invalid TileShader sidecar in UpdateConstants; native generation disabled and affected submissions will be suppressed");
				}
				NativeShaderGeneration* current = s_publishedGeneration.load(
					std::memory_order_acquire);
				if (current)
				{
					current->runtimeFault.store(true, std::memory_order_release);
					NotifyNativeA8CommandExternalMutation(
						NativeA8CommandFallback::Generation);
				}
				return;
			}

			NativeShaderGeneration* generation = profile->owner;
			const bool coverageProfile =
				profile->key.shaderClass == NativeA8ShaderClass::Coverage;
			const bool simpleColorProfile = coverageProfile
				|| profile->key.shaderClass == NativeA8ShaderClass::Argb;
			IDirect3DDevice9* device = shader->m_pkD3DDevice;
			if (!device)
				device = generation->device;
			if (!device || device != generation->device)
			{
				MarkGenerationFault(generation, "device-mismatch",
					D3DERR_DEVICELOST);
				return;
			}
			NativeSortedShaderBatch& sortedBatch = s_sortedShaderBatch;
			const bool sortedBatchActive = sortedBatch.depth != 0;
			if (sortedBatchActive
				&& (sortedBatch.device != device
					|| sortedBatch.generation != generation->id))
			{
				// Device/generation identity is shared by the sampler and c1-c8
				// caches. A change invalidates both before this submission can
				// publish a new identity.
				sortedBatch.device = device;
				sortedBatch.generation = generation->id;
				sortedBatch.packetProfile = nullptr;
				sortedBatch.vertexAa = {};
				sortedBatch.packetConstantsReady = false;
				sortedBatch.samplerReady = false;
			}
			// Distance-field profiles bind different pixel programs and upload
			// c0-c8 directly. The stock update refreshes Tile's color/alpha constant
			// map on every packet, so c0 must be republished after every stock call
			// even when its value is unchanged. Only tNVSE-owned c1-c8 may be reused.
			std::array<float, 4> tileConstant;
			if (!ResolveStockTilePixelConstant(
				properties, tileConstant.data()))
			{
				MarkGenerationFault(generation, "ResolveStockTilePixelConstant", E_FAIL);
				return;
			}
			HRESULT constantsResult = D3D_OK;
			if (!simpleColorProfile)
			{
				NativeVertexAaState* vertexAaCache = sortedBatchActive
					? &sortedBatch.vertexAa
					: batchActive ? &batch.vertexAa : nullptr;
				const char* vertexAaOperation = "none";
				const HRESULT vertexAaResult = PublishNativeVertexAaConstant(
					device, profile->constants[7], vertexAaCache,
					vertexAaOperation);
				if (FAILED(vertexAaResult))
				{
					MarkGenerationFault(generation, vertexAaOperation,
						vertexAaResult);
					return;
				}
			}
			if (simpleColorProfile)
			{
				// The baked-coverage program consumes only live Tile c0. Avoid the
				// eight distance/effect registers that make no contribution to this
				// stock-like one-sample path.
				constantsResult = device->SetPixelShaderConstantF(
					0, tileConstant.data(), 1);
			}
			else
			{
				const NativeShaderProfile* cachedProfile = nullptr;
				const std::array<float,
					kNativeA8PacketConstantFloatCount>* cachedConstants = nullptr;
				if (sortedBatchActive && sortedBatch.packetConstantsReady)
				{
					cachedProfile = sortedBatch.packetProfile;
					cachedConstants = &sortedBatch.packetConstants;
				}
				else if (batchActive && batch.packetConstantsReady)
				{
					cachedConstants = &batch.packetConstants;
				}

				if (!cachedConstants)
				{
					std::array<float,
						(kNativeA8PacketConstantRegisterCount + 1) * 4>
						fullConstants;
					std::copy(tileConstant.begin(), tileConstant.end(),
						fullConstants.begin());
					std::copy(profile->constants.begin(),
						profile->constants.end(), fullConstants.begin() + 4);
					constantsResult = device->SetPixelShaderConstantF(
						0, fullConstants.data(), static_cast<UINT>(
							kNativeA8PacketConstantRegisterCount + 1));
					if (SUCCEEDED(constantsResult))
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::CompositeConstantFullUpload);
					}
				}
				else
				{
					// c0 is owned by the live Tile submission and may have been
					// touched by the stock update above. c1-c8 remain tNVSE-owned
					// across consecutive native facades in the sorted Tile run.
					constantsResult = device->SetPixelShaderConstantF(
						0, tileConstant.data(), 1);
					if (FAILED(constantsResult))
					{
						MarkGenerationFault(generation,
							"SetPixelShaderConstantF(c0)", constantsResult);
						return;
					}

					size_t firstChanged =
						kNativeA8PacketConstantRegisterCount;
					size_t lastChanged = 0;
					// A profile is immutable and interned by its exact key, so pointer
					// identity proves all eight packet registers are unchanged.
					if (cachedProfile != profile)
					{
						for (size_t packetRegister = 0;
							packetRegister
								< kNativeA8PacketConstantRegisterCount;
							++packetRegister)
						{
							const size_t firstFloat = packetRegister * 4u;
							if (std::memcmp(
								profile->constants.data() + firstFloat,
								cachedConstants->data() + firstFloat,
								4u * sizeof(float)) != 0)
							{
								firstChanged = std::min(
									firstChanged, packetRegister);
								lastChanged = packetRegister;
							}
						}
					}
					if (firstChanged < kNativeA8PacketConstantRegisterCount)
					{
						constantsResult = device->SetPixelShaderConstantF(
							static_cast<UINT>(1u + firstChanged),
							profile->constants.data() + firstChanged * 4u,
							static_cast<UINT>(
								lastChanged - firstChanged + 1u));
						if (SUCCEEDED(constantsResult))
						{
							RecordFreeTypePerf(
								FreeTypePerfCounter::
									CompositeConstantPartialUpload);
						}
					}
					else
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::CompositeConstantC0Only);
					}
				}
			}
			if (FAILED(constantsResult))
			{
				MarkGenerationFault(generation, "SetPixelShaderConstantF(c0-c8)",
					constantsResult);
				return;
			}
			if (batchActive && !simpleColorProfile)
			{
				batch.packetConstants = profile->constants;
				batch.packetConstantsReady = true;
			}
			if (sortedBatchActive && !simpleColorProfile)
			{
				sortedBatch.packetProfile = profile;
				sortedBatch.packetConstants = profile->constants;
				sortedBatch.packetConstantsReady = true;
			}

			// Coverage reads only Alpha from a one-level A8 texture. sRGB sampling
			// cannot modify Alpha and no mip state can select another level, so the
			// pass's ordinary bilinear setup is sufficient without extra device
			// calls. Distance-field channels remain numeric linear data and require
			// the explicit state below.
			if (simpleColorProfile)
				return;

			const bool sortedSamplerReady = sortedBatch.depth
				&& sortedBatch.samplerReady
				&& sortedBatch.device == device
				&& sortedBatch.generation == generation->id;
			if ((!batchActive || !batch.samplerReady)
				&& !sortedSamplerReady)
			{
				const HRESULT srgbResult = device->SetSamplerState(
					0, D3DSAMP_SRGBTEXTURE, FALSE);
				if (FAILED(srgbResult))
				{
					MarkGenerationFault(generation,
						"SetSamplerState(SRGBTEXTURE)", srgbResult);
					return;
				}
				const HRESULT mipResult = device->SetSamplerState(
					0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
				if (FAILED(mipResult))
				{
					MarkGenerationFault(generation,
						"SetSamplerState(MIPFILTER)", mipResult);
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
				RecordFreeTypePerf(
					FreeTypePerfCounter::SamplerStateSet);
			}
			else
			{
				if (batchActive)
					batch.samplerReady = true;
				RecordFreeTypePerf(
					FreeTypePerfCounter::SamplerStateReuse);
			}
		}

		NativeProfileKey MakeProfileKey(const NativeA8PacketTemplate& packet,
			NativeA8Sampling sampling, bool writeEffectAlpha)
		{
			NativeProfileKey key;
			key.shaderClass = packet.shaderClass;
			key.sampling = sampling;
			key.quality = packet.quality;
			key.distanceFieldMethod = packet.distanceFieldMethod;
			key.staticCompositeLayerMask =
				packet.staticCompositeLayerMask;
			key.compositeShiftedShadow =
				packet.compositeShiftedShadow;
			key.writeEffectAlpha = writeEffectAlpha;
			key.usesLiveTileRgb = packet.usesLiveTileRgb;
			std::memcpy(key.constantBits.data(), packet.constants.data(),
				key.constantBits.size() * sizeof(UInt32));
			if (packet.sampling == sampling)
			{
				key.precomputedHash =
					packet.profileHashes[writeEffectAlpha ? 1u : 0u];
			}
			return key;
		}

		NativeA8Sampling ResolveEffectiveSampling(
			const NativeA8PacketTemplate& packet,
			bool scaledFillSampling)
		{
			static_cast<void>(scaledFillSampling);
			// Distance-field atlas pages are explicitly level-zero-only.
			return NativeA8Sampling::LinearLod0;
		}

		NiTexturingProperty::FilterMode ResolveFilterMode(
			NativeA8Sampling sampling)
		{
			switch (sampling)
			{
			case NativeA8Sampling::LinearMipmapped:
				return NiTexturingProperty::FILTER_TRILERP;
			case NativeA8Sampling::LinearLod0:
				return NiTexturingProperty::FILTER_BILERP;
			default:
				return NiTexturingProperty::FILTER_NEAREST;
			}
		}

		NiD3DPixelShader* ResolveProfilePixelShader(
			NativeShaderGeneration& generation,
			const NativeA8PacketTemplate& packet)
		{
			if (packet.shaderClass != NativeA8ShaderClass::Coverage
				&& packet.shaderClass != NativeA8ShaderClass::Argb
				&& packet.distanceFieldMethod != generation.distanceFieldMethod)
				return nullptr;
			switch (packet.shaderClass)
			{
			case NativeA8ShaderClass::Body:
			{
				const size_t index = static_cast<size_t>(packet.quality);
				return index < generation.mtsdfFillShaders.size()
					? generation.mtsdfFillShaders[index].m_pObject : nullptr;
			}
			case NativeA8ShaderClass::Effect:
			{
				const size_t index = static_cast<size_t>(packet.quality);
				return index < generation.effectShaders.size()
					? generation.effectShaders[index].m_pObject : nullptr;
			}
			case NativeA8ShaderClass::Composite:
			{
				const size_t index = static_cast<size_t>(packet.quality);
				if (generation.distanceFieldMethod
						== DistanceFieldMethod::Mtsdf
					&& index
						< generation.mtsdfCompositeProfileShaders.size()
					&& packet.staticCompositeLayerMask
						>= kStaticCompositeLayerMaskFirst
					&& packet.staticCompositeLayerMask
						< kStaticCompositeLayerMaskFirst
							+ kStaticCompositeLayerMaskCount)
				{
					const size_t maskIndex =
						packet.staticCompositeLayerMask
							- kStaticCompositeLayerMaskFirst;
					const size_t shiftIndex =
						packet.compositeShiftedShadow ? 1u : 0u;
					NiD3DPixelShaderPtr& specializedSlot =
						generation.mtsdfCompositeProfileShaders[index]
							[maskIndex][shiftIndex];
					bool& attempted =
						generation.mtsdfCompositeProfileAttempts[index]
							[maskIndex][shiftIndex];
					if (!HasShaderHandle(specializedSlot) && !attempted
						&& generation.createPixelShader)
					{
						// ResolveNativeA8PacketShader serializes profile creation
						// with profileMutex, so optional shader publication needs no
						// second lock and allocates only profiles actually observed.
						attempted = true;
						const char* qualityNames[] = {
							"fast", "balanced", "high"
						};
						char shaderName[128] = {};
						sprintf_s(shaderName,
							"tnvse_freetype_native_mtsdf_composite_%s_m%u%s.pso",
							qualityNames[index],
							static_cast<UInt32>(
								packet.staticCompositeLayerMask),
							packet.compositeShiftedShadow
								? "_shift" : "");
						specializedSlot =
							generation.createPixelShader(shaderName);
					}
					NiD3DPixelShader* specialized =
						specializedSlot.m_pObject;
					if (specialized && specialized->GetShaderHandle())
						return specialized;
				}
				return index < generation.compositeShaders.size()
					? generation.compositeShaders[index].m_pObject : nullptr;
			}
			case NativeA8ShaderClass::Coverage:
				return generation.coverageShader.m_pObject;
			case NativeA8ShaderClass::Argb:
				return generation.argbShader.m_pObject;
			default:
				return nullptr;
			}
		}

		void SetPassRenderState(NiD3DPass* pass,
			D3DRENDERSTATETYPE state, DWORD value)
		{
			ThisStdCall<void>(kPassSetRenderState, pass, state,
				static_cast<UInt32>(value), true);
		}

		void ConfigureProfilePass(NativeShaderGeneration& generation,
			NativeShaderProfile& profile, NiD3DPass& pass)
		{
			const NiTexturingProperty::FilterMode filter =
				ResolveFilterMode(profile.key.sampling);
			for (UInt32 stageIndex = 0;
				stageIndex < std::min<UInt32>(pass.m_uiStageCount, 2); ++stageIndex)
			{
				NiD3DTextureStage* stage = pass.GetStage(stageIndex);
				if (!stage)
					continue;
				CdeclCall<void>(kTextureStageSetProperties, stage, stageIndex,
					NiTexturingProperty::CLAMP_S_CLAMP_T,
					static_cast<UInt32>(filter), false);
				ThisStdCall<void>(kTextureStageSetFilter, stage, filter);
			}

			SetPassRenderState(&pass, D3DRS_ZENABLE, FALSE);
			SetPassRenderState(&pass, D3DRS_ZWRITEENABLE, FALSE);
			SetPassRenderState(&pass, D3DRS_ALPHATESTENABLE, FALSE);
			if (profile.effectPass)
			{
				SetPassRenderState(&pass, D3DRS_STENCILWRITEMASK, 0);
				const DWORD rgbWrite = D3DCOLORWRITEENABLE_RED
					| D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE;
				SetPassRenderState(&pass, D3DRS_COLORWRITEENABLE,
					rgbWrite | (profile.key.writeEffectAlpha
						? D3DCOLORWRITEENABLE_ALPHA : 0));
				if (generation.supportsSeparateAlpha
					&& profile.key.writeEffectAlpha)
				{
					SetPassRenderState(&pass, D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
					SetPassRenderState(&pass, D3DRS_DESTBLENDALPHA,
						D3DBLEND_INVSRCALPHA);
					SetPassRenderState(&pass, D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
					SetPassRenderState(&pass, D3DRS_SEPARATEALPHABLENDENABLE,
						TRUE);
				}
			}
			else
			{
				const DWORD colorWrite = D3DCOLORWRITEENABLE_RED
					| D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE
					| D3DCOLORWRITEENABLE_ALPHA;
				SetPassRenderState(&pass, D3DRS_COLORWRITEENABLE, colorWrite);
			}
		}

		NativeShaderProfile* CreateProfile(NativeShaderGeneration& generation,
			const NativeA8PacketTemplate& packet, const NativeProfileKey& key)
		{
			NiD3DPixelShader* pixelShader = ResolveProfilePixelShader(generation,
				packet);
			if (!pixelShader || !pixelShader->GetShaderHandle())
				return nullptr;
			if (packet.shaderClass == NativeA8ShaderClass::Composite
				&& g_bEnableFreeTypeFontRenderingLog)
			{
				const size_t qualityIndex =
					static_cast<size_t>(packet.quality);
				const bool specialized = qualityIndex
						< generation.compositeShaders.size()
					&& pixelShader
						!= generation.compositeShaders[
							qualityIndex].m_pObject;
				const UInt32 ordinal =
					s_compositeProfileLogCount.fetch_add(
						1, std::memory_order_relaxed);
				if (ordinal < 16u)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_native: composite pixel profile quality=%u layerMask=%u shiftedShadow=%u specialized=%u",
						static_cast<UInt32>(packet.quality),
						static_cast<UInt32>(
							packet.staticCompositeLayerMask),
						packet.compositeShiftedShadow ? 1u : 0u,
						specialized ? 1u : 0u);
				}
			}
			NiD3DVertexShader* vertexShader =
				generation.vertexShader.m_pObject;
			if (!vertexShader || !vertexShader->GetShaderHandle())
				return nullptr;

			NiPointer<TileShader> shaderGuard =
				StdCall<TileShader*>(kTileShaderCreate);
			TileShader* shader = shaderGuard.m_pObject;
			if (!shader)
				return nullptr;
			NiD3DPass* pass = shader->spPasses[0].m_pObject;
			void** stockVtable = *reinterpret_cast<void***>(shader);
			if (!pass || pass->m_uiStageCount < 1 || !stockVtable)
				return nullptr;

			// Preserve the complete immutable c1-c8 packet ABI. COLOR0 now owns
			// only the shared per-glyph base modifier, so replacing c1 with the
			// historical identity value would turn every fixed effect layer white.
			auto* profile = new NativeShaderProfile(generation, key,
				packet.constants);
			profile->shader = shader;
			profile->effectPass =
				packet.shaderClass != NativeA8ShaderClass::Composite
				&& packet.layer != 3;

			profile->shaderOwner = shaderGuard;
			shader->m_spShaderDecl = generation.declaration.m_pObject;
			shader->spShaderDeclarations[0] = generation.declaration.m_pObject;
			shader->spShaderDeclarations[1] = generation.declaration.m_pObject;
			for (NiD3DVertexShaderPtr& slot : shader->spVertexShaders)
				slot = vertexShader;
			for (NiD3DPixelShaderPtr& slot : shader->spPixelShaders)
				slot = pixelShader;
			pass->m_spVertexShader = vertexShader;
			pass->m_spPixelShader = pixelShader;
			// The TileShader factory already pairs the base pass with spPasses[0].
			// Avoid an unnecessary NiD3DPass smart-pointer ref-count transition;
			// CommonLib intentionally does not link those pool ref-count wrappers.
			//

			ConfigureProfilePass(generation, *profile, *pass);

			auto* vtable = new NativeTileVtableBlock();
			vtable->profile = profile;
			vtable->rttiPrefix = stockVtable[-1];
			std::copy_n(stockVtable, vtable->slots.size(), vtable->slots.begin());
			vtable->stockUpdateConstants = reinterpret_cast<StockUpdateConstantsFn>(
				stockVtable[kUpdateConstantsVtableSlot]);
			vtable->slots[kUpdateConstantsVtableSlot] =
				reinterpret_cast<void*>(&NativeUpdateConstants);
			profile->vtable = vtable;
			*reinterpret_cast<void***>(shader) = vtable->slots.data();
			NativeA8CompiledPacketCommand& program =
				profile->retainedProgram;
			program.profile = profile;
			program.shader = shader;
			program.shaderVtable = vtable->slots.data();
			program.device = generation.device;
			program.vertexShader = vertexShader->GetShaderHandle();
			program.pixelShader = pixelShader->GetShaderHandle();
			program.prepareGeometry = vtable->slots[27];
			program.setupPass = vtable->slots[30];
			program.updateConstants = vtable->slots[31];
			program.setupBlend = vtable->slots[32];
			program.setupAlphaTest = vtable->slots[33];
			program.setupDrawmode = vtable->slots[34];
			program.postGeometry = vtable->slots[35];
			program.setupNonFirstPass = vtable->slots[68];
			program.generation = generation.id;
			program.simpleColor =
				packet.shaderClass == NativeA8ShaderClass::Coverage
				|| packet.shaderClass == NativeA8ShaderClass::Argb;
			program.active = program.device && program.vertexShader
				&& program.pixelShader && program.setupPass;
			return profile;
		}

		bool CreateNativeDeclaration(NativeShaderGeneration& generation,
			const char*& failure)
		{
			// Runtime object size is 0x38. The 0xD4 CommonLib declaration includes
			// static lookup tables incorrectly; never allocate/copy it with sizeof.
			NiDX9ShaderDeclarationPtr declaration =
				CdeclCall<NiDX9ShaderDeclaration*>(kShaderDeclarationCreate,
					generation.renderer, 5u, 1u);
			if (!declaration)
			{
				failure = "declaration-factory";
				return false;
			}

			using Parameter = NiShaderDeclaration::ShaderParameter;
			using ParameterType = NiShaderDeclaration::ShaderParameterType;
			const bool entriesReady =
				declaration->SetEntry(0, 0, Parameter::SHADERPARAM_NI_POSITION,
					ParameterType::SPTYPE_FLOAT3, 0)
				&& declaration->SetEntry(1, 0,
					Parameter::SHADERPARAM_NI_TEXCOORD0,
					ParameterType::SPTYPE_FLOAT2, 0)
				&& declaration->SetEntry(2, 0,
					Parameter::SHADERPARAM_NI_COLOR,
					ParameterType::SPTYPE_UBYTECOLOR, 0)
				&& declaration->SetEntry(3, 0,
					Parameter::SHADERPARAM_NI_TEXCOORD1,
					ParameterType::SPTYPE_FLOAT3, 0)
				&& declaration->SetEntry(4, 0,
					Parameter::SHADERPARAM_NI_TEXCOORD2,
					ParameterType::SPTYPE_FLOAT4, 0);
			if (!entriesReady)
			{
				failure = "declaration-entries";
				return false;
			}

			IDirect3DVertexDeclaration9* d3dDeclaration =
				declaration->GetD3DDeclaration();
			if (!d3dDeclaration)
			{
				D3DCAPS9 caps = {};
				const HRESULT capsResult = generation.device
					? generation.device->GetDeviceCaps(&caps) : E_POINTER;
				IDirect3DVertexDeclaration9* retryDeclaration = nullptr;
				const HRESULT retryResult = generation.device
					&& declaration->m_pkElements
					? generation.device->CreateVertexDeclaration(
						declaration->m_pkElements, &retryDeclaration)
					: E_POINTER;
				gLog.FormattedMessage(
					"tnvse_freetype_native: vertex declaration creation failed format=float4 stride=%u capsHr=0x%08X declTypes=0x%08X retryHr=0x%08X elements=%p",
					static_cast<UInt32>(sizeof(NativeA8GpuVertex)),
					static_cast<UInt32>(capsResult), caps.DeclTypes,
					static_cast<UInt32>(retryResult),
					declaration->m_pkElements);
				if (declaration->m_pkElements)
				{
					const D3DVERTEXELEMENT9* elements =
						declaration->m_pkElements;
					gLog.FormattedMessage(
						"tnvse_freetype_native: declaration elements e0=%u/%u/%u/%u/%u/%u e1=%u/%u/%u/%u/%u/%u e2=%u/%u/%u/%u/%u/%u e3=%u/%u/%u/%u/%u/%u e4=%u/%u/%u/%u/%u/%u",
						elements[0].Stream, elements[0].Offset,
						elements[0].Type, elements[0].Method,
						elements[0].Usage, elements[0].UsageIndex,
						elements[1].Stream, elements[1].Offset,
						elements[1].Type, elements[1].Method,
						elements[1].Usage, elements[1].UsageIndex,
						elements[2].Stream, elements[2].Offset,
						elements[2].Type, elements[2].Method,
						elements[2].Usage, elements[2].UsageIndex,
						elements[3].Stream, elements[3].Offset,
						elements[3].Type, elements[3].Method,
						elements[3].Usage, elements[3].UsageIndex,
						elements[4].Stream, elements[4].Offset,
						elements[4].Type, elements[4].Method,
						elements[4].Usage, elements[4].UsageIndex);
				}
				if (SUCCEEDED(retryResult) && retryDeclaration)
				{
					// GetD3DDeclaration hides the original HRESULT. A direct retry
					// with its generated element table can recover a transient
					// device failure while preserving Gamebryo ownership.
					declaration->m_hVertexDecl = retryDeclaration;
					declaration->m_bModified = false;
					d3dDeclaration = retryDeclaration;
				}
				else
				{
					if (retryDeclaration)
						retryDeclaration->Release();
					failure = "d3d-declaration";
					return false;
				}
			}
			generation.declaration = declaration;
			generation.d3dDeclaration = d3dDeclaration;
			return true;
		}

		NativeShaderGeneration* BuildGeneration(CreateVertexShaderFn createVS,
			CreatePixelShaderFn createPS, NiDX9Renderer* renderer,
			IDirect3DDevice9* device, const char*& failure)
		{
			if (!createVS || !createPS || !renderer || !device)
			{
				failure = "loader-or-device";
				return nullptr;
			}

			auto generation = std::make_unique<NativeShaderGeneration>();
			generation->renderer = renderer;
			generation->device = device;
			generation->createPixelShader = createPS;
			generation->supportsSeparateAlpha =
				(renderer->m_kD3DCaps9.PrimitiveMiscCaps
					& D3DPMISCCAPS_SEPARATEALPHABLEND) != 0;
			generation->distanceFieldMethod =
				GetConfiguredDistanceFieldMethod();

			generation->vertexShader = createVS("tnvse_freetype_native_vs.vso");
			if (g_bEnableFreeTypeFontAggressivePerformanceMode)
			{
				generation->coverageShader =
					createPS("tnvse_freetype_native_coverage.pso");
				generation->argbShader =
					createPS("tnvse_freetype_native_argb.pso");
			}
			else
			{
				const char* mtsdfFillNames[] = {
					"tnvse_freetype_native_mtsdf_fill_fast.pso",
					"tnvse_freetype_native_mtsdf_fill_balanced.pso",
					"tnvse_freetype_native_mtsdf_fill_high.pso"
				};
				const char* trueSdfFillNames[] = {
					"tnvse_freetype_native_sdf_fill_fast.pso",
					"tnvse_freetype_native_sdf_fill_balanced.pso",
					"tnvse_freetype_native_sdf_fill_high.pso"
				};
				const char* const* fillNames =
					generation->distanceFieldMethod == DistanceFieldMethod::Mtsdf
						? mtsdfFillNames : trueSdfFillNames;
				for (size_t index = 0;
					index < generation->mtsdfFillShaders.size(); ++index)
				{
					generation->mtsdfFillShaders[index] =
						createPS(fillNames[index]);
				}
				const char* mtsdfEffectNames[] = {
					"tnvse_freetype_native_mtsdf_effects_fast.pso",
					"tnvse_freetype_native_mtsdf_effects_balanced.pso",
					"tnvse_freetype_native_mtsdf_effects_high.pso"
				};
				const char* trueSdfEffectNames[] = {
					"tnvse_freetype_native_sdf_effects_fast.pso",
					"tnvse_freetype_native_sdf_effects_balanced.pso",
					"tnvse_freetype_native_sdf_effects_high.pso"
				};
				const char* const* effectNames =
					generation->distanceFieldMethod == DistanceFieldMethod::Mtsdf
						? mtsdfEffectNames : trueSdfEffectNames;
				for (size_t index = 0;
					index < generation->effectShaders.size(); ++index)
				{
					generation->effectShaders[index] =
						createPS(effectNames[index]);
				}
				const char* mtsdfCompositeNames[] = {
					"tnvse_freetype_native_mtsdf_composite_fast.pso",
					"tnvse_freetype_native_mtsdf_composite_balanced.pso",
					"tnvse_freetype_native_mtsdf_composite_high.pso"
				};
				const char* trueSdfCompositeNames[] = {
					"tnvse_freetype_native_sdf_composite_fast.pso",
					"tnvse_freetype_native_sdf_composite_balanced.pso",
					"tnvse_freetype_native_sdf_composite_high.pso"
				};
				const char* const* compositeNames =
					generation->distanceFieldMethod == DistanceFieldMethod::Mtsdf
						? mtsdfCompositeNames : trueSdfCompositeNames;
				for (size_t index = 0;
					index < generation->compositeShaders.size(); ++index)
				{
					generation->compositeShaders[index] =
						createPS(compositeNames[index]);
				}
			}

			if (!HasShaderHandle(generation->vertexShader))
			{
				failure = "base-shader-set";
				return nullptr;
			}
			if (g_bEnableFreeTypeFontAggressivePerformanceMode
				&& (!HasShaderHandle(generation->coverageShader)
					|| !HasShaderHandle(generation->argbShader)))
			{
				// Treat an incomplete aggressive deployment exactly like a missing
				// Shader Loader. No A8 coverage facade may be published unless its
				// pixel program is present, so shape routing remains on the stock
				// ARGB32 TileShader fallback.
				failure = "aggressive-shader-set";
				return nullptr;
			}
			if (!g_bEnableFreeTypeFontAggressivePerformanceMode)
			{
				for (const NiD3DPixelShaderPtr& shader
					: generation->mtsdfFillShaders)
				{
					if (!HasShaderHandle(shader))
					{
						failure = "fill-shader-set";
						return nullptr;
					}
				}
				for (const NiD3DPixelShaderPtr& shader
					: generation->effectShaders)
				{
					if (!HasShaderHandle(shader))
					{
						failure = "effect-shader-set";
						return nullptr;
					}
				}
			}

			if (!CreateNativeDeclaration(*generation, failure))
				return nullptr;
			generation->vertexShader->m_hDecl = generation->d3dDeclaration;
			return generation.release();
		}

		bool ResolveShaderLoader(CreateVertexShaderFn& createVS,
			CreatePixelShaderFn& createPS, const char*& failure)
		{
			createVS = nullptr;
			createPS = nullptr;
			if (!g_cmdTableInterface
				|| !g_cmdTableInterface->GetPluginInfoByDLLName)
			{
				failure = "nvse-plugin-query";
				return false;
			}
			const PluginInfo* info = g_cmdTableInterface->GetPluginInfoByDLLName(
				dependencies::kShaderLoaderDllName);
			if (!dependencies::IsPluginInfoValid(info)
				|| info->version < dependencies::kShaderLoaderMinVersion)
			{
				failure = "shader-loader-version";
				return false;
			}
			HMODULE module = GetModuleHandleA(dependencies::kShaderLoaderDllName);
			if (!module)
			{
				failure = "shader-loader-module";
				return false;
			}
			createVS = reinterpret_cast<CreateVertexShaderFn>(
				GetProcAddress(module, "CreateVertexShader"));
			createPS = reinterpret_cast<CreatePixelShaderFn>(
				GetProcAddress(module, "CreatePixelShader"));
			if (!createVS || !createPS)
			{
				failure = "shader-loader-exports";
				return false;
			}
			return true;
		}

		bool NativeRendererResetCallback(bool beforeReset, void*)
		{
			if (beforeReset)
			{
				s_resetInProgress.store(true, std::memory_order_release);
				InvalidateNativeA8RingResources(
					NativeA8FallbackReason::DeviceReset);
				NativeShaderGeneration* current = s_publishedGeneration.load(
					std::memory_order_acquire);
				if (current)
				{
					current->runtimeFault.store(true, std::memory_order_release);
					NotifyNativeA8CommandExternalMutation(
						NativeA8CommandFallback::Generation);
				}
				gLog.FormattedMessage(
					"tnvse_freetype_native: generation-invalidated reason=device-reset generation=%u phase=release; dynamic VB/IB ring released",
					current ? current->id : 0);
				return true;
			}

			s_resetInProgress.store(false, std::memory_order_release);
			if (!InitializeNativeA8Renderer(true, true))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: initialization unavailable reason=device-reset phase=rebuild; no complete native generation available");
			}
			return true;
		}
	}

	bool InitializeNativeA8Renderer(bool forceAttempt, bool reportFailures)
	{
		if (s_resetInProgress.load(std::memory_order_acquire)
			|| !g_bEnableFreeTypeFontRendering || !g_bEnableFreeTypeA8Atlas)
			return false;

		std::lock_guard<std::mutex> lock(s_initializationMutex);
		if (s_resetInProgress.load(std::memory_order_acquire))
			return false;
		NativeShaderGeneration* current = s_publishedGeneration.load(
			std::memory_order_acquire);
		if (!forceAttempt && GenerationMatchesCurrentDevice(current))
			return true;

		const DWORD now = GetTickCount();
		if (!forceAttempt && s_lastInitializationAttempt
			&& now - s_lastInitializationAttempt < kInitializationRetryMilliseconds)
		{
			return GenerationMatchesCurrentDevice(current);
		}
		s_lastInitializationAttempt = now;

		const char* failure = "unknown";
		CreateVertexShaderFn createVS = nullptr;
		CreatePixelShaderFn createPS = nullptr;
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		IDirect3DDevice9* device = renderer ? renderer->GetD3DDevice() : nullptr;
		NativeShaderGeneration* candidate = nullptr;
		if (renderer && device && s_resetRenderer != renderer)
		{
			ThisStdCall<UInt32>(0x86BAE0, renderer,
				NativeRendererResetCallback, renderer);
			s_resetRenderer = renderer;
			gLog.FormattedMessage(
				"tnvse_freetype_native: registered renderer reset lifecycle renderer=%p",
				renderer);
		}
		if (renderer && device
			&& renderer->m_kD3DCaps9.VertexShaderVersion >= D3DVS_VERSION(3, 0)
			&& renderer->m_kD3DCaps9.PixelShaderVersion >= D3DPS_VERSION(3, 0)
			&& ResolveShaderLoader(createVS, createPS, failure))
		{
			candidate = BuildGeneration(createVS, createPS, renderer, device,
				failure);
		}
		else if (!renderer || !device)
		{
			failure = "renderer-device";
		}
		else if (renderer->m_kD3DCaps9.VertexShaderVersion < D3DVS_VERSION(3, 0)
			|| renderer->m_kD3DCaps9.PixelShaderVersion < D3DPS_VERSION(3, 0))
		{
			failure = "shader-model-3";
		}

		if (!candidate || !GenerationResourcesReady(candidate))
		{
			if (reportFailures)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: initialization failed reason=%s; retaining generation=%u when valid; new text uses the stock ARGB TileShader fallback when no complete native generation is available",
					failure, current ? current->id : 0);
			}
			return GenerationMatchesCurrentDevice(current);
		}

		candidate->id = s_nextGeneration++;
		s_processGenerations.push_back(candidate);
		s_publishedGeneration.store(candidate, std::memory_order_release);
		NotifyNativeA8CommandExternalMutation(
			NativeA8CommandFallback::Generation);
		const char* compositeProfileMode =
			!g_bEnableFreeTypeFontAggressivePerformanceMode
				&& candidate->distanceFieldMethod
					== DistanceFieldMethod::Mtsdf
				? "lazy-36" : "disabled";
		gLog.FormattedMessage(
			"tnvse_freetype_native: published complete TileShader generation=%u device=%p route=%s distanceField=%s mtsdfCompositeProfiles=%s vertexAa=analytic-c4-per-packet vertexFormat=float4 vertexStride=%u declTypes=0x%08X",
			candidate->id, candidate->device,
			g_bEnableFreeTypeFontAggressivePerformanceMode
				? "argb-composite" : "distance-field",
			g_bEnableFreeTypeFontAggressivePerformanceMode
				? "disabled" : GetConfiguredDistanceFieldMethodName(),
			compositeProfileMode,
			static_cast<UInt32>(sizeof(NativeA8GpuVertex)),
			candidate->renderer->m_kD3DCaps9.DeclTypes);
		return true;
	}

	void HandleNativeA8RendererMainLoop()
	{
		if (!g_bEnableFreeTypeFontRendering || !g_bEnableFreeTypeA8Atlas)
			return;
		NativeShaderGeneration* current = s_publishedGeneration.load(
			std::memory_order_acquire);
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		IDirect3DDevice9* device = renderer ? renderer->GetD3DDevice() : nullptr;
		const bool deviceChanged = current && device
			&& (current->renderer != renderer || current->device != device);
		if (!GenerationMatchesCurrentDevice(current))
			InitializeNativeA8Renderer(deviceChanged, false);
	}

	void HandleNativeA8ShaderLoaderMessage(UInt32 messageType)
	{
		if (messageType != kShaderRefreshMessage)
			return;
		NativeShaderGeneration* before = s_publishedGeneration.load(
			std::memory_order_acquire);
		const UInt32 beforeId = before ? before->id : 0;
		InitializeNativeA8Renderer(true, true);
		NativeShaderGeneration* after = s_publishedGeneration.load(
			std::memory_order_acquire);
		if (!after || after->id == beforeId)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_native: shader refresh did not publish a complete generation; retained generation=%u",
				beforeId);
		}
	}

	bool IsNativeA8RendererAvailable()
	{
		NativeShaderGeneration* generation = s_publishedGeneration.load(
			std::memory_order_acquire);
		if (GenerationMatchesCurrentDevice(generation))
			return true;
		if (!InitializeNativeA8Renderer(false, false))
			return false;
		return GenerationMatchesCurrentDevice(s_publishedGeneration.load(
			std::memory_order_acquire));
	}

	UInt32 GetNativeA8ShaderGeneration()
	{
		NativeShaderGeneration* generation = s_publishedGeneration.load(
			std::memory_order_acquire);
		return GenerationMatchesCurrentDevice(generation) ? generation->id : 0;
	}

	IDirect3DVertexDeclaration9* GetNativeA8D3DDeclaration(UInt32 generation)
	{
		NativeShaderGeneration* current = s_publishedGeneration.load(
			std::memory_order_acquire);
		return current && current->id == generation
			&& GenerationMatchesCurrentDevice(current)
			? current->d3dDeclaration : nullptr;
	}

	bool IsNativeA8ShaderGenerationCurrent(UInt32 generation)
	{
		NativeShaderGeneration* current = s_publishedGeneration.load(
			std::memory_order_acquire);
		return current && current->id == generation
			&& !s_resetInProgress.load(std::memory_order_acquire)
			&& !current->runtimeFault.load(std::memory_order_acquire);
	}

	void BeginNativeA8SortedShaderBatch()
	{
		NativeSortedShaderBatch& batch = s_sortedShaderBatch;
		if (!batch.depth++)
		{
			batch.device = nullptr;
			batch.generation = 0;
			batch.packetProfile = nullptr;
			batch.vertexAa = {};
			batch.packetConstantsReady = false;
			batch.samplerReady = false;
		}
	}

	void EndNativeA8SortedShaderBatch()
	{
		NativeSortedShaderBatch& batch = s_sortedShaderBatch;
		if (!batch.depth)
			return;
		if (!--batch.depth)
		{
			batch.device = nullptr;
			batch.generation = 0;
			batch.packetProfile = nullptr;
			batch.vertexAa = {};
			batch.packetConstantsReady = false;
			batch.samplerReady = false;
		}
	}

	void InvalidateNativeA8SortedShaderState()
	{
		InvalidateNativeA8CommandExecutionSegment(
			NativeA8CommandFallback::State);
		NativeSortedShaderBatch& batch = s_sortedShaderBatch;
		batch.device = nullptr;
		batch.generation = 0;
		batch.packetProfile = nullptr;
		batch.vertexAa = {};
		batch.packetConstantsReady = false;
		batch.samplerReady = false;
		// If invalidation happens during a nested pass inside one facade, its
		// per-facade fallback must not resurrect constants from before that pass.
		NativeFacadeShaderBatch& facadeBatch = s_facadeShaderBatch;
		facadeBatch.vertexAa = {};
		facadeBatch.packetConstantsReady = false;
		facadeBatch.samplerReady = false;
	}

	void BeginNativeA8FacadeShaderBatch()
	{
		NativeFacadeShaderBatch& batch = s_facadeShaderBatch;
		++batch.depth;
		// Each scope owns one facade. A recursive scope deliberately discards the
		// parent's cached constants; End invalidates the parent again so its next
		// packet performs one full native pixel-constant upload.
		batch.vertexAa = {};
		batch.packetConstantsReady = false;
		batch.samplerReady = false;
	}

	void EndNativeA8FacadeShaderBatch()
	{
		NativeFacadeShaderBatch& batch = s_facadeShaderBatch;
		if (!batch.depth)
			return;
		--batch.depth;
		batch.vertexAa = {};
		batch.packetConstantsReady = false;
		batch.samplerReady = false;
	}

	void MarkNativeA8GenerationFault(UInt32 generation,
		const char* operation, HRESULT result)
	{
		NativeShaderGeneration* current = s_publishedGeneration.load(
			std::memory_order_acquire);
		if (current && current->id == generation)
			MarkGenerationFault(current, operation, result);
	}

	TileShader* ResolveNativeA8PacketShader(const NativeA8PacketTemplate& packet,
		const NiTriShape* facade, bool scaledFillSampling)
	{
		if (!IsNativeA8RendererAvailable())
			return nullptr;
		NativeShaderGeneration* generation = s_publishedGeneration.load(
			std::memory_order_acquire);
		if (!GenerationMatchesCurrentDevice(generation))
			return nullptr;

		const NativeA8Sampling sampling = ResolveEffectiveSampling(packet,
			scaledFillSampling);
		const NiAlphaProperty* alpha = facade
			? facade->GetAlphaProperty() : nullptr;
		const bool writeEffectAlpha = packet.layer != 3
			&& generation->supportsSeparateAlpha
			&& alpha && alpha->GetAlphaBlending();
		const size_t cacheIndex = writeEffectAlpha ? 1u : 0u;
		NativeA8PacketShaderCacheEntry& packetCache =
			packet.resolvedShaders[cacheIndex];
		NativeShaderProfile* cachedProfile =
			static_cast<NativeShaderProfile*>(
				packetCache.profile.load(std::memory_order_acquire));
		if (cachedProfile && cachedProfile->owner == generation
			&& cachedProfile->shader
			&& cachedProfile->key.sampling == sampling
			&& cachedProfile->key.writeEffectAlpha == writeEffectAlpha)
		{
			return cachedProfile->shader;
		}
		const NativeProfileKey key = MakeProfileKey(packet, sampling,
			writeEffectAlpha);
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
			if (packet.shaderClass == NativeA8ShaderClass::Composite)
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

	bool ResolveNativeA8RetainedPacketProgram(
		const NativeA8PacketTemplate& packet,
		TileShader* shader, UInt32 generation,
		const NativeA8CompiledPacketCommand*& program)
	{
		program = nullptr;
		NativeTileVtableBlock* block = RecoverNativeVtableBlock(shader);
		NativeShaderProfile* profile = block ? block->profile : nullptr;
		NativeShaderGeneration* owner = profile ? profile->owner : nullptr;
		if (!profile || !owner || profile->shader != shader
			|| owner->id != generation
			|| !GenerationMatchesCurrentDevice(owner))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandRetainedProgramMiss);
			return false;
		}
		const size_t cacheIndex =
			profile->key.writeEffectAlpha ? 1u : 0u;
		NativeA8PacketShaderCacheEntry& packetCache =
			packet.resolvedShaders[cacheIndex];
		NativeShaderProfile* cachedProfile =
			static_cast<NativeShaderProfile*>(
				packetCache.profile.load(std::memory_order_acquire));
		const bool cacheHit = cachedProfile == profile;
		if (!cacheHit)
		{
			if (profile->key.shaderClass != packet.shaderClass
				|| profile->key.quality != packet.quality
				|| profile->key.distanceFieldMethod
					!= packet.distanceFieldMethod
				|| profile->key.staticCompositeLayerMask
					!= packet.staticCompositeLayerMask
				|| profile->key.compositeShiftedShadow
					!= packet.compositeShiftedShadow
				|| profile->key.usesLiveTileRgb
					!= packet.usesLiveTileRgb
				|| std::memcmp(profile->constants.data(),
					packet.constants.data(),
					packet.constants.size() * sizeof(float)) != 0)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandRetainedProgramMiss);
				return false;
			}
			packetCache.profile.store(profile, std::memory_order_release);
		}

		const NativeA8CompiledPacketCommand& retained =
			profile->retainedProgram;
		if (!retained.active || retained.profile != profile
			|| retained.shader != shader || retained.device != owner->device
			|| retained.generation != owner->id)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandRetainedProgramMiss);
			return false;
		}
		RecordFreeTypePerf(cacheHit
			? FreeTypePerfCounter::CommandRetainedProgramHit
			: FreeTypePerfCounter::CommandRetainedProgramMiss);
		program = &retained;
		return true;
	}

	bool BindNativeA8CommandPacket(
		const NativeA8CompiledPacketCommand& command,
		const void* atlasTexture, bool publishPrograms,
		const NiPropertyState* properties,
		const NativeA8CommandBindState& bindState,
		const char*& operation, HRESULT& result)
	{
		operation = "none";
		result = D3D_OK;
		auto* profile = static_cast<NativeShaderProfile*>(command.profile);
		NativeShaderGeneration* generation =
			profile ? profile->owner : nullptr;
		IDirect3DDevice9* device = command.device;
		void** shaderVtable = command.shader
			? *reinterpret_cast<void***>(command.shader) : nullptr;
		if (!command.active || !profile || !generation || !device
			|| profile->shader != command.shader
			|| !shaderVtable
			|| shaderVtable[30] != command.setupPass
			|| shaderVtable[32] != command.setupBlend
			|| shaderVtable[33] != command.setupAlphaTest
			|| shaderVtable[34] != command.setupDrawmode
			|| generation->id != command.generation
			|| generation->device != device
			|| !GenerationMatchesCurrentDevice(generation)
			|| !atlasTexture)
		{
			operation = "validate-command-packet";
			result = D3DERR_INVALIDCALL;
			return false;
		}

		if (publishPrograms)
		{
			if (!command.setupPass || !properties)
			{
				operation = "validate-command-pass-state";
				result = D3DERR_INVALIDCALL;
				return false;
			}
			using SetupPassFn = void(__thiscall*)(
				TileShader*, const NiPropertyState*);
			reinterpret_cast<SetupPassFn>(
				command.setupPass)(command.shader, properties);
			using SetupStateFn = void(__thiscall*)(
				TileShader*, const NiPropertyState*);
			using SetupDrawmodeFn = void(__thiscall*)(
				TileShader*, const NiPropertyState*, bool);
			if (bindState.applyBlend)
			{
				if (!command.setupBlend)
				{
					operation = "validate-command-blend-state";
					result = D3DERR_INVALIDCALL;
					return false;
				}
				reinterpret_cast<SetupStateFn>(
					command.setupBlend)(command.shader, properties);
			}
			if (bindState.applyAlphaTest)
			{
				if (!command.setupAlphaTest)
				{
					operation = "validate-command-alpha-state";
					result = D3DERR_INVALIDCALL;
					return false;
				}
				reinterpret_cast<SetupStateFn>(
					command.setupAlphaTest)(
						command.shader, properties);
			}
			if (bindState.applyDrawmode)
			{
				if (!command.setupDrawmode)
				{
					operation = "validate-command-drawmode-state";
					result = D3DERR_INVALIDCALL;
					return false;
				}
				reinterpret_cast<SetupDrawmodeFn>(
					command.setupDrawmode)(
						command.shader, properties,
						bindState.noFog);
			}
			result = device->SetVertexShader(command.vertexShader);
			if (FAILED(result))
			{
				operation = "SetVertexShader(command)";
				return false;
			}
			result = device->SetPixelShader(command.pixelShader);
			if (FAILED(result))
			{
				operation = "SetPixelShader(command)";
				return false;
			}
			if (!command.simpleColor)
			{
				result = device->SetPixelShaderConstantF(1,
					profile->constants.data(),
					static_cast<UINT>(
						kNativeA8PacketConstantRegisterCount));
				if (FAILED(result))
				{
					operation = "SetPixelShaderConstantF(command-c1-c8)";
					return false;
				}
				const char* vertexOperation = "none";
				NativeVertexAaState* vertexCache =
					s_sortedShaderBatch.depth
						? &s_sortedShaderBatch.vertexAa : nullptr;
				result = PublishNativeVertexAaConstant(device,
					profile->constants[7], vertexCache,
					vertexOperation);
				if (FAILED(result))
				{
					operation = vertexOperation;
					return false;
				}
				result = device->SetSamplerState(
					0, D3DSAMP_SRGBTEXTURE, FALSE);
				if (FAILED(result))
				{
					operation = "SetSamplerState(command-srgb)";
					return false;
				}
				result = device->SetSamplerState(
					0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
				if (FAILED(result))
				{
					operation = "SetSamplerState(command-mip)";
					return false;
				}
				if (s_sortedShaderBatch.depth)
				{
					s_sortedShaderBatch.device = device;
					s_sortedShaderBatch.generation = generation->id;
					s_sortedShaderBatch.packetProfile = profile;
					s_sortedShaderBatch.packetConstants =
						profile->constants;
					s_sortedShaderBatch.packetConstantsReady = true;
					s_sortedShaderBatch.samplerReady = true;
				}
			}
		}

		result = device->SetTexture(0,
			const_cast<IDirect3DBaseTexture9*>(
				static_cast<const IDirect3DBaseTexture9*>(atlasTexture)));
		if (FAILED(result))
		{
			operation = "SetTexture(command-page)";
			return false;
		}
		return true;
	}

}

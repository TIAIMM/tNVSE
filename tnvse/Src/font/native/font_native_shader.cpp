#include "font_native_internal.h"

#include "load_config.h"
#include "plugin_dependencies.h"
#include "tnvse.h"
#include "NiAlphaProperty.hpp"

#include "NiD3DPass.hpp"
#include "NiD3DPixelShader.hpp"
#include "NiD3DRenderState.hpp"
#include "NiD3DShaderConstantMap.hpp"
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

	NativeA8BlendState ComputeNativeA8OwnedBlendState(
		const NiPropertyState* properties)
	{
		NativeA8BlendState state;
		if (!properties)
			return state;

		const BSShaderProperty* shade =
			properties->GetShadeProperty<BSShaderProperty>();
		if (!shade || shade->m_eShaderType == -1)
			return state;

		const NiAlphaProperty* alpha = properties->GetAlphaProperty();
		const UInt16 flags = alpha ? alpha->m_usFlags.Get() : 0;
		const bool propertyBlend = alpha
			&& (flags & NiAlphaProperty::ALPHA_BLEND_MASK) != 0;
		const bool opacityBlend = shade->fFadeAlpha < 1.0f
			|| (shade->fAlpha < 1.0f && !shade->HasNoFade());
		state.enabled = propertyBlend || opacityBlend;
		if (propertyBlend)
		{
			state.sourceFunction = static_cast<UInt8>(
				(flags & NiAlphaProperty::SRC_BLEND_MASK)
					>> NiAlphaProperty::SRC_BLEND_POS);
			state.destinationFunction = static_cast<UInt8>(
				(flags & NiAlphaProperty::DEST_BLEND_MASK)
					>> NiAlphaProperty::DEST_BLEND_POS);
		}
		return state;
	}

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
		static_assert(offsetof(
			NiD3DRenderState, m_akSamplerStateSettings) == 0xE20);
		static_assert(offsetof(
			NiD3DRenderState, m_apkTextureStageTextures) == 0x10A0);

		inline constexpr UInt32 kTileShaderCreate = 0xBCAE90;
		inline constexpr UInt32 kTileShaderSetupGeometryTextures = 0xBCA760;
		inline constexpr UInt32 kTileShaderUpdateConstants = 0xBCA980;
		inline constexpr UInt32 kShaderSetupGeometryAlphaBlending = 0xBE1FF0;
		inline constexpr UInt32 kSetAlphaBlendEnable = 0xB97FA0;
		inline constexpr UInt32 kSetSourceAndDestinationBlends = 0xB97FF0;
		inline constexpr UInt32 kShaderSetupGeometryAlphaTesting = 0xBE20B0;
		inline constexpr UInt32 kShaderSetupGeometryRenderStates = 0xBE20E0;
		inline constexpr UInt32 kTileShaderPostGeometry = 0xBCAC60;
		inline constexpr UInt32 kNiD3DShaderPrepareGeometry = 0xE812F0;
		inline constexpr UInt32 kNiD3DShaderFirstPass = 0xE80580;
		inline constexpr UInt32 kShaderDeclarationCreate = 0xE76700;
		inline constexpr UInt32 kTextureStageSetProperties = 0xBE0CF0;
		inline constexpr UInt32 kTextureStageSetFilter = 0xE7DEF0;
		inline constexpr UInt32 kPassSetRenderState = 0xB71A10;
		inline constexpr UInt32 kCopiedTileShaderVtableEntries = 84;
		inline constexpr UInt32 kUpdateConstantsVtableSlot = 31;
		inline constexpr UInt32 kSetupBlendVtableSlot = 32;
		inline constexpr UInt32 kNativeVtableMagic = 0x35544D4E; // "NMT5"
		inline constexpr UInt32 kShaderRefreshMessage = 0;
		inline constexpr DWORD kInitializationRetryMilliseconds = 1000;
		inline constexpr UInt32 kVanillaLayoutVertexStride =
			sizeof(NativeA8VanillaLayoutVertex);
		static_assert(kVanillaLayoutVertexStride == 40u);
		inline constexpr UInt8 kStaticCompositeLayerMaskFirst = 8;
		inline constexpr size_t kStaticCompositeLayerMaskCount = 8;
		inline constexpr size_t kStaticCompositeShiftCount = 2;
		using CreateVertexShaderFn = NiD3DVertexShader* (__cdecl*)(const char*);
		using CreatePixelShaderFn = NiD3DPixelShader* (__cdecl*)(const char*);
		using VanillaUpdateConstantsFn = void(__thiscall*)(TileShader*,
			const NiPropertyState*);
		void __fastcall NativeUpdateConstants(TileShader*, void*,
			const NiPropertyState*);
		void __fastcall NativeSetupGeometryAlphaBlending(
			TileShader*, void*, const NiPropertyState*);

		struct NativeShaderGeneration;
		struct NativeShaderProfile;

		constexpr UInt8 NativePacketRegisterCount(
			NativeA8ShaderClass shaderClass)
		{
			switch (shaderClass)
			{
			case NativeA8ShaderClass::Body:
				// LayerColor c176 + AtlasPass c177.
				return 2;
			case NativeA8ShaderClass::Effect:
				// LayerColor c176 through MtsdfFlags c179.
				return 4;
			case NativeA8ShaderClass::Composite:
				// ShadowColor c176 through CompositeFlags c183.
				return 8;
			default:
				return 0;
			}
		}

		static_assert(NativePacketRegisterCount(
			NativeA8ShaderClass::Composite)
			== kNativeA8PacketConstantRegisterCount);

		struct NativeVertexAaState
		{
			D3DVIEWPORT9 viewport = {};
			float rasterScale = 0.0f;
			bool viewportReady = false;
			bool aaConstantReady = false;
			bool vanillaGlyphConstantReady = false;
		};

		struct NativeSortedShaderBatch
		{
			IDirect3DDevice9* device = nullptr;
			NiD3DRenderState* renderState = nullptr;
			UInt32 generation = 0;
			NativeShaderProfile* packetProfile = nullptr;
			UInt8 packetRegisterCount = 0;
			UInt32 depth = 0;
			NativeVertexAaState vertexAa;
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
			bool vanillaLayoutSdf = false;
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
					&& vanillaLayoutSdf == other.vanillaLayoutSdf
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
				mix(key.vanillaLayoutSdf ? 1u : 0u);
				for (UInt32 value : key.constantBits)
					mix(value);
				return hash;
			}
		};

		struct NativeTileVtableBlock
		{
			// Keep our immutable data before the MSVC RTTI prefix. The object's
			// vptr points at slots, so slots[-1] remains the vanilla COL pointer.
			NativeShaderProfile* profile = nullptr;
			UInt32 magic = kNativeVtableMagic;
			VanillaUpdateConstantsFn vanillaUpdateConstants = nullptr;
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
				: owner(&generation), key(profileKey), constants(packetConstants),
				  privateRegisterCount(
					  NativePacketRegisterCount(profileKey.shaderClass))
			{
			}

			NativeShaderGeneration* const owner;
			const NativeProfileKey key;
			const std::array<float,
				kNativeA8PacketConstantFloatCount> constants;
			const UInt8 privateRegisterCount;
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
			UInt32 deviceEpoch = 0;
			NiDX9Renderer* renderer = nullptr;
			IDirect3DDevice9* device = nullptr;
			NiDX9ShaderDeclarationPtr declaration;
			IDirect3DVertexDeclaration9* d3dDeclaration = nullptr;
			NiD3DVertexShaderPtr vertexShader;
			NiDX9ShaderDeclarationPtr vanillaLayoutDeclaration;
			IDirect3DVertexDeclaration9* vanillaLayoutD3DDeclaration = nullptr;
			// Shader Loader refreshes may publish a new generation while geometry
			// uploaded by an earlier generation remains resident.  Generations are
			// process-lifetime objects, so these exact same-device declarations remain
			// owned and form a safe immutable compatibility set.  Device reset advances
			// deviceEpoch and deliberately prevents reuse across that boundary.
			std::vector<IDirect3DVertexDeclaration9*>
				compatibleVanillaLayoutD3DDeclarations;
			NiD3DVertexShaderPtr vanillaLayoutVertexShader;
			bool vanillaLayoutReady = false;
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
			std::array<std::array<std::array<NiD3DPixelShaderPtr,
				kStaticCompositeShiftCount>,
				kStaticCompositeLayerMaskCount>, 3>
				vanillaLayoutCompositeShaders;
			std::array<std::array<std::array<bool,
				kStaticCompositeShiftCount>,
				kStaticCompositeLayerMaskCount>, 3>
				vanillaLayoutCompositeAttempts = {};
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
		std::atomic<UInt32> s_deviceEpoch = 1;
		DWORD s_lastInitializationAttempt = 0;
		std::atomic<bool> s_invalidVtableLogged = false;
		std::atomic<UInt32> s_compositeProfileLogCount = 0;
		std::atomic<bool> s_resetInProgress = false;
		std::atomic<UInt32> s_standardV2ProofLogGeneration = 0;
		NiDX9Renderer* s_resetRenderer = nullptr;

		struct NativeFacadeShaderBatch
		{
			NativeShaderProfile* packetProfile = nullptr;
			UInt8 packetRegisterCount = 0;
			UInt32 depth = 0;
			NativeVertexAaState vertexAa;
			bool samplerReady = false;
		};

		thread_local NativeFacadeShaderBatch s_facadeShaderBatch;

		struct NativeVanillaLayoutPublicationWitness
		{
			UInt64 token = 0;
			UInt64 publishedToken = 0;
			TileShader* expectedShader = nullptr;
			NativeShaderProfile* publishedProfile = nullptr;
			IDirect3DDevice9* publishedDevice = nullptr;
			UInt32 publishedGeneration = 0;
			bool armed = false;
		};

		thread_local NativeVanillaLayoutPublicationWitness
			s_vanillaLayoutPublicationWitness;
		thread_local UInt64 s_nextVanillaLayoutPublicationToken = 0;

		NativeA8StandardBlendSemantics ClassifyStandardBlendCallback(
			void* callback)
		{
			if (callback == reinterpret_cast<void*>(
					&NativeSetupGeometryAlphaBlending))
			{
				return NativeA8StandardBlendSemantics::NativeOwned;
			}
			if (callback == reinterpret_cast<void*>(
					kShaderSetupGeometryAlphaBlending))
			{
				return NativeA8StandardBlendSemantics::Retail;
			}
			return NativeA8StandardBlendSemantics::Unknown;
		}

		void ResetSortedShaderStateCaches()
		{
			NativeSortedShaderBatch& batch = s_sortedShaderBatch;
			batch.device = nullptr;
			batch.renderState = nullptr;
			batch.generation = 0;
			batch.packetProfile = nullptr;
			batch.packetRegisterCount = 0;
			batch.vertexAa = {};
			batch.samplerReady = false;

			// If invalidation happens during a nested pass inside one facade, its
			// per-facade fallback must not resurrect constants from before that pass.
			NativeFacadeShaderBatch& facadeBatch = s_facadeShaderBatch;
			facadeBatch.packetProfile = nullptr;
			facadeBatch.packetRegisterCount = 0;
			facadeBatch.vertexAa = {};
			facadeBatch.samplerReady = false;
		}

		const char* StandardBlendSemanticsName(
			NativeA8StandardBlendSemantics semantics)
		{
			switch (semantics)
			{
			case NativeA8StandardBlendSemantics::Retail:
				return "retail";
			case NativeA8StandardBlendSemantics::NativeOwned:
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

		bool HasExactVanillaLayoutConstantCarryChain(TileShader* shader)
		{
			NativeTileVtableBlock* block = RecoverNativeVtableBlock(shader);
			NativeShaderProfile* profile = block ? block->profile : nullptr;
			if (!profile || profile->shader != shader
				|| !profile->key.vanillaLayoutSdf)
			{
				return false;
			}
			const NativeA8CompiledPacketCommand& program =
				profile->retainedProgram;
			return program.setupPass == reinterpret_cast<void*>(
					kTileShaderSetupGeometryTextures)
				&& block->vanillaUpdateConstants
					== reinterpret_cast<VanillaUpdateConstantsFn>(
						kTileShaderUpdateConstants)
				&& program.updateConstants
					== reinterpret_cast<void*>(&NativeUpdateConstants)
				&& program.setupBlend == reinterpret_cast<void*>(
					&NativeSetupGeometryAlphaBlending)
				&& program.setupAlphaTest == reinterpret_cast<void*>(
					kShaderSetupGeometryAlphaTesting)
				&& program.setupDrawmode == reinterpret_cast<void*>(
					kShaderSetupGeometryRenderStates)
				&& program.prepareGeometry == reinterpret_cast<void*>(
					kNiD3DShaderPrepareGeometry)
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
			NativeSortedShaderBatch& batch = s_sortedShaderBatch;
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

		bool IsNativeMipFilterReady(
			const NiD3DRenderState* renderState)
		{
			// Retail NiDX9RenderState maps only ADDRESSU/V, MAG, MIN and MIP
			// into the five software-mirror slots. MIPFILTER is slot 4
			// (NiDX9RenderState::SetSamplerState at 0xE910A0).
			constexpr size_t kMipFilterMirrorIndex = 4;
			return renderState
				&& renderState->m_akSamplerStateSettings[0]
					[kMipFilterMirrorIndex].m_uiCurrValue
						== D3DTEXF_NONE;
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
				kNativeA8VertexAaConstantRegister, aaProfile.data(), 1);
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
			UInt8 layerMask, NativeVertexAaState* cache,
			const char*& operation)
		{
			operation = "none";
			if (!device || !std::isfinite(rasterScale)
				|| rasterScale <= 0.0f || !std::isfinite(spread)
				|| spread <= 0.0f
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
				spread, 1.0f, static_cast<float>(layerMask), 0.0f
			}};
			if (cache)
			{
				cache->aaConstantReady = false;
				cache->vanillaGlyphConstantReady = false;
			}
			const HRESULT constantResult = device->SetVertexShaderConstantF(
				kNativeA8VertexAaConstantRegister,
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
			CdeclCall<void>(kSetAlphaBlendEnable, 0, 0);
			const NativeA8BlendState state =
				ComputeNativeA8OwnedBlendState(properties);
			if (!state.enabled)
				return;

			CdeclCall<void>(kSetAlphaBlendEnable, 1, 0);
			CdeclCall<void>(kSetSourceAndDestinationBlends,
				static_cast<UInt32>(state.sourceFunction),
				static_cast<UInt32>(state.destinationFunction), 0);
		}

		void __fastcall NativeUpdateConstants(TileShader* shader, void*,
			const NiPropertyState* properties)
		{
			// Preserve Tile's matrix, live color/fade, scissor and alpha contract.
			NativeTileVtableBlock* block = RecoverNativeVtableBlock(shader);
			VanillaUpdateConstantsFn vanillaUpdate = block && block->vanillaUpdateConstants
				? block->vanillaUpdateConstants
				: reinterpret_cast<VanillaUpdateConstantsFn>(kTileShaderUpdateConstants);
			NativeFacadeShaderBatch& batch = s_facadeShaderBatch;
			const bool batchActive = batch.depth != 0;
			// Retail TileShader::UpdateConstants is also a live render-state
			// synchronization point: it reapplies Tile scissor and stencil state.
			// Standard v2 may suppress this whole wrapper only inside one
			// validated execution segment, with the verified retail slot retained,
			// identical transform/color/camera inputs, and no transient
			// scissor/stencil state. Every other packet still enters here and keeps
			// the exact vanilla slot-31/35 pairing.
			vanillaUpdate(shader, properties);
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaConstantUpdate);
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
			// Both reverse targets prove that the unmodified vanilla slot 31 owns
			// tintcolor at PS c0 and submits it through the pixel constant map.
			// Since native packet data is discontiguous at c176-c183, a second c0
			// publication cannot protect or refresh any tNVSE-owned register.
			//
			// A plugin-replaced vtable slot does not carry that proof. Preserve
			// the old explicit publication only for that compatibility case.
			if (vanillaUpdate
				!= reinterpret_cast<VanillaUpdateConstantsFn>(
					kTileShaderUpdateConstants))
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

			NativeSortedShaderBatch& sortedBatch = s_sortedShaderBatch;
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
				&& (!profile->key.vanillaLayoutSdf
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
			else if (profile->key.vanillaLayoutSdf)
			{
				const char* vertexOperation = "none";
				const HRESULT vertexResult =
					PublishNativeVanillaLayoutVertexConstants(device,
						profile->constants[7], profile->constants[6],
						profile->key.staticCompositeLayerMask,
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
						kNativeA8PixelConstantBaseRegister,
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
							kNativeA8PacketConstantRegisterCount
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
								kNativeA8PixelConstantBaseRegister
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
			const bool mipFilterReady = IsNativeMipFilterReady(
				ResolveSortedRenderState(device));
			const bool sortedSamplerReady = sortedBatch.depth
				&& sortedBatch.samplerReady
				&& sortedBatch.device == device
				&& sortedBatch.generation == generation->id
				&& mipFilterReady;
			if ((!batchActive || !batch.samplerReady
					|| !mipFilterReady)
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
				s_vanillaLayoutPublicationWitness;
			if (witness.armed && witness.expectedShader == shader
				&& profile->key.vanillaLayoutSdf && batchActive
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

		NativeProfileKey MakeProfileKey(const NativeA8PacketTemplate& packet,
			NativeA8Sampling sampling, bool writeEffectAlpha,
			bool vanillaLayoutSdf)
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
			key.vanillaLayoutSdf = vanillaLayoutSdf;
			std::memcpy(key.constantBits.data(), packet.constants.data(),
				key.constantBits.size() * sizeof(UInt32));
			if (vanillaLayoutSdf)
			{
				std::memcpy(&key.constantBits[6],
					&packet.uniformSdfSpread, sizeof(UInt32));
			}
			if (!vanillaLayoutSdf && packet.sampling == sampling)
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
			const NativeA8PacketTemplate& packet,
			bool vanillaLayoutSdf)
		{
			if (vanillaLayoutSdf)
			{
				const size_t qualityIndex =
					static_cast<size_t>(packet.quality);
				const bool supportedDistanceField =
					packet.distanceFieldMethod == DistanceFieldMethod::TrueSdf
					|| packet.distanceFieldMethod == DistanceFieldMethod::Mtsdf;
				if (!generation.vanillaLayoutReady
					|| !supportedDistanceField
					|| generation.distanceFieldMethod
						!= packet.distanceFieldMethod
					|| packet.shaderClass != NativeA8ShaderClass::Composite
					|| (packet.compositeShiftedShadow
						&& !(packet.staticCompositeLayerMask & 1u))
					|| packet.staticCompositeLayerMask
						< kStaticCompositeLayerMaskFirst
					|| packet.staticCompositeLayerMask
						>= kStaticCompositeLayerMaskFirst
							+ kStaticCompositeLayerMaskCount
					|| qualityIndex
						>= generation.vanillaLayoutCompositeShaders.size())
				{
					return nullptr;
				}
				const size_t maskIndex = packet.staticCompositeLayerMask
					- kStaticCompositeLayerMaskFirst;
				const size_t shiftedIndex = packet.compositeShiftedShadow
					? 1u : 0u;
				NiD3DPixelShaderPtr& slot =
					generation.vanillaLayoutCompositeShaders[qualityIndex]
						[maskIndex][shiftedIndex];
				bool& attempted =
					generation.vanillaLayoutCompositeAttempts[qualityIndex]
						[maskIndex][shiftedIndex];
				if (!HasShaderHandle(slot) && !attempted
					&& generation.createPixelShader)
				{
					attempted = true;
					const char* qualityNames[] = {
						"fast", "balanced", "high"
					};
					const char* shiftedSuffix =
						packet.compositeShiftedShadow ? "_shift" : "";
					const char* distanceFieldName =
						packet.distanceFieldMethod == DistanceFieldMethod::TrueSdf
							? "sdf" : "mtsdf";
					char shaderName[128] = {};
					sprintf_s(shaderName,
						"tnvse_freetype_native_%s_vanilla_layout_%s_m%u%s.pso",
						distanceFieldName, qualityNames[qualityIndex],
						static_cast<UInt32>(
							packet.staticCompositeLayerMask), shiftedSuffix);
					slot = generation.createPixelShader(shaderName);
				}
				return HasShaderHandle(slot) ? slot.m_pObject : nullptr;
			}
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
				packet, key.vanillaLayoutSdf);
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
						"tnvse_freetype_native: composite pixel profile quality=%u layerMask=%u shiftedShadow=%u specialized=%u vanillaLayout=%u",
						static_cast<UInt32>(packet.quality),
						static_cast<UInt32>(
							packet.staticCompositeLayerMask),
						packet.compositeShiftedShadow ? 1u : 0u,
						specialized ? 1u : 0u,
						key.vanillaLayoutSdf ? 1u : 0u);
				}
			}
			NiD3DVertexShader* vertexShader = key.vanillaLayoutSdf
				? generation.vanillaLayoutVertexShader.m_pObject
				: generation.vertexShader.m_pObject;
			if (!vertexShader || !vertexShader->GetShaderHandle())
				return nullptr;
			NiDX9ShaderDeclaration* declaration = key.vanillaLayoutSdf
				? generation.vanillaLayoutDeclaration.m_pObject
				: generation.declaration.m_pObject;
			if (!declaration)
				return nullptr;

			NiPointer<TileShader> shaderGuard =
				StdCall<TileShader*>(kTileShaderCreate);
			TileShader* shader = shaderGuard.m_pObject;
			if (!shader)
				return nullptr;
			NiD3DPass* pass = shader->spPasses[0].m_pObject;
			void** vanillaVtable = *reinterpret_cast<void***>(shader);
			if (!pass || pass->m_uiStageCount < 1 || !vanillaVtable)
				return nullptr;

			// Preserve the complete immutable private c176-c183 packet ABI.
			// COLOR0 now owns only the shared per-glyph base modifier, so
			// replacing c176 with the historical identity value would turn every
			// fixed effect layer white.
			auto profileConstants = packet.constants;
			if (key.vanillaLayoutSdf)
				profileConstants[6] = packet.uniformSdfSpread;
			auto* profile = new NativeShaderProfile(generation, key,
				profileConstants);
			profile->shader = shader;
			profile->effectPass =
				packet.shaderClass != NativeA8ShaderClass::Composite
				&& packet.layer != 3;

			profile->shaderOwner = shaderGuard;
			shader->m_spShaderDecl = declaration;
			shader->spShaderDeclarations[0] = declaration;
			shader->spShaderDeclarations[1] = declaration;
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
			vtable->rttiPrefix = vanillaVtable[-1];
			std::copy_n(vanillaVtable, vtable->slots.size(), vtable->slots.begin());
			vtable->vanillaUpdateConstants = reinterpret_cast<VanillaUpdateConstantsFn>(
				vanillaVtable[kUpdateConstantsVtableSlot]);
			vtable->slots[kUpdateConstantsVtableSlot] =
				reinterpret_cast<void*>(&NativeUpdateConstants);
			// The native FreeType shader owns its blend contract. Global TileShader
			// vtables remain untouched, so vanilla and third-party geometry continue
			// through their live callback chains without becoming a native proof.
			vtable->slots[kSetupBlendVtableSlot] =
				reinterpret_cast<void*>(&NativeSetupGeometryAlphaBlending);
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
			program.setupBlend = vtable->slots[kSetupBlendVtableSlot];
			program.setupAlphaTest = vtable->slots[33];
			program.setupDrawmode = vtable->slots[34];
			program.postGeometry = vtable->slots[35];
			program.setupNonFirstPass = vtable->slots[68];
			program.standardV2SlotProofs = 0;
			program.standardBlendSemantics =
				ClassifyStandardBlendCallback(program.setupBlend);
			if (program.setupPass == reinterpret_cast<void*>(
					kTileShaderSetupGeometryTextures))
			{
				program.standardV2SlotProofs |=
					NativeA8CompiledPacketCommand::kStandardSlot30Proof;
			}
			if (vtable->vanillaUpdateConstants
				== reinterpret_cast<VanillaUpdateConstantsFn>(
					kTileShaderUpdateConstants))
			{
				program.standardV2SlotProofs |=
					NativeA8CompiledPacketCommand::kStandardSlot31Proof;
			}
			if (HasPredictableNativeA8BlendSemantics(
					program.standardBlendSemantics))
			{
				program.standardV2SlotProofs |=
					NativeA8CompiledPacketCommand::kStandardSlot32Proof;
			}
			if (program.setupAlphaTest == reinterpret_cast<void*>(
					kShaderSetupGeometryAlphaTesting))
			{
				program.standardV2SlotProofs |=
					NativeA8CompiledPacketCommand::kStandardSlot33Proof;
			}
			if (program.setupDrawmode == reinterpret_cast<void*>(
					kShaderSetupGeometryRenderStates))
			{
				program.standardV2SlotProofs |=
					NativeA8CompiledPacketCommand::kStandardSlot34Proof;
			}
			if (program.postGeometry == reinterpret_cast<void*>(
					kTileShaderPostGeometry))
			{
				program.standardV2SlotProofs |=
					NativeA8CompiledPacketCommand::kStandardSlot35Proof;
			}
			program.directDrawLiteReady =
				program.prepareGeometry == reinterpret_cast<void*>(
					kNiD3DShaderPrepareGeometry)
				&& vtable->slots[36] == reinterpret_cast<void*>(
					kNiD3DShaderFirstPass);
			program.generation = generation.id;
			program.simpleColor =
				packet.shaderClass == NativeA8ShaderClass::Coverage
					|| packet.shaderClass == NativeA8ShaderClass::Argb;
			program.active = program.device && program.vertexShader
				&& program.pixelShader && program.setupPass;
			const UInt32 previousProofGeneration =
				s_standardV2ProofLogGeneration.exchange(
					generation.id, std::memory_order_relaxed);
			if (previousProofGeneration != generation.id)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: standard-v2 slot proofs generation=%u mask=%02X required=%02X blend=%s slot32=%p ready=%u directDrawLite=%u prepare=%p firstPass=%p",
					generation.id,
					static_cast<UInt32>(
						program.standardV2SlotProofs),
					static_cast<UInt32>(
						NativeA8CompiledPacketCommand::
							kStandardV2RequiredProofs),
					StandardBlendSemanticsName(
						program.standardBlendSemantics),
					program.setupBlend,
					program.standardV2SlotProofs
							== NativeA8CompiledPacketCommand::
								kStandardV2RequiredProofs
						? 1u : 0u,
					program.directDrawLiteReady ? 1u : 0u,
					program.prepareGeometry,
					vtable->slots[36]);
			}
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

		bool CreateVanillaLayoutDeclaration(
			NativeShaderGeneration& generation, const char*& status)
		{
			if (!generation.renderer || !generation.device
				|| generation.renderer->m_kD3DCaps9.MaxVertexShaderConst
					<= kNativeA8VanillaLayoutGlyphConstantRegister)
			{
				status = "constant-registers";
				return false;
			}
			NiDX9ShaderDeclarationPtr declaration =
				CdeclCall<NiDX9ShaderDeclaration*>(kShaderDeclarationCreate,
					generation.renderer, 5u, 1u);
			if (!declaration)
			{
				status = "declaration-factory";
				return false;
			}

			using Parameter = NiShaderDeclaration::ShaderParameter;
			using ParameterType = NiShaderDeclaration::ShaderParameterType;
			const bool entriesReady =
				declaration->SetEntry(0, 0,
					Parameter::SHADERPARAM_NI_POSITION,
					ParameterType::SPTYPE_FLOAT3, 0)
				&& declaration->SetEntry(1, 0,
					Parameter::SHADERPARAM_NI_TEXCOORD0,
					ParameterType::SPTYPE_FLOAT2, 0)
				&& declaration->SetEntry(2, 0,
					Parameter::SHADERPARAM_NI_COLOR,
					ParameterType::SPTYPE_UBYTECOLOR, 0)
				&& declaration->SetEntry(3, 0,
					Parameter::SHADERPARAM_NI_TEXCOORD1,
					ParameterType::SPTYPE_FLOAT2, 0)
				&& declaration->SetEntry(4, 0,
					Parameter::SHADERPARAM_NI_TEXCOORD2,
					ParameterType::SPTYPE_FLOAT2, 0);
			if (!entriesReady)
			{
				status = "declaration-entries";
				return false;
			}
			IDirect3DVertexDeclaration9* d3dDeclaration =
				declaration->GetD3DDeclaration();
			if (!d3dDeclaration)
			{
				status = "d3d-declaration";
				return false;
			}
			generation.vanillaLayoutDeclaration = declaration;
			generation.vanillaLayoutD3DDeclaration = d3dDeclaration;
			status = "ready";
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
			generation->deviceEpoch = s_deviceEpoch.load(
				std::memory_order_acquire);
			generation->renderer = renderer;
			generation->device = device;
			generation->createPixelShader = createPS;
			generation->supportsSeparateAlpha =
				(renderer->m_kD3DCaps9.PrimitiveMiscCaps
					& D3DPMISCCAPS_SEPARATEALPHABLEND) != 0;
			generation->distanceFieldMethod =
				GetConfiguredDistanceFieldMethod();

			generation->vertexShader = createVS("tnvse_freetype_native_vs.vso");
			const bool vanillaLayoutRequested =
				IsVanillaLayoutSdfEnabled(generation->distanceFieldMethod)
				&& (generation->distanceFieldMethod
						== DistanceFieldMethod::TrueSdf
					|| generation->distanceFieldMethod
						== DistanceFieldMethod::Mtsdf);
			if (vanillaLayoutRequested)
			{
				generation->vanillaLayoutVertexShader = createVS(
					"tnvse_freetype_native_vanilla_layout_vs.vso");
			}
			if (UsesBakedEffectRoute())
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
			if (UsesBakedEffectRoute()
				&& (!HasShaderHandle(generation->coverageShader)
					|| !HasShaderHandle(generation->argbShader)))
			{
				// Treat an incomplete aggressive deployment exactly like a missing
				// Shader Loader. No A8 coverage facade may be published unless its
				// pixel program is present, so shape routing remains on the vanilla
				// ARGB32 TileShader fallback.
				failure = "aggressive-shader-set";
				return nullptr;
			}
			if (!UsesBakedEffectRoute())
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
			const char* vanillaLayoutStatus = vanillaLayoutRequested
				? "vertex-shader" : "not-applicable";
			if (vanillaLayoutRequested
				&& HasShaderHandle(generation->vanillaLayoutVertexShader)
				&& CreateVanillaLayoutDeclaration(
					*generation, vanillaLayoutStatus))
			{
				generation->vanillaLayoutVertexShader->m_hDecl =
					generation->vanillaLayoutD3DDeclaration;
				generation->vanillaLayoutReady = true;
				for (NativeShaderGeneration* previous : s_processGenerations)
				{
					if (!previous || !previous->vanillaLayoutReady
						|| previous->deviceEpoch != generation->deviceEpoch
						|| previous->renderer != renderer
						|| previous->device != device
						|| !previous->vanillaLayoutD3DDeclaration)
					{
						continue;
					}
					IDirect3DVertexDeclaration9* declaration =
						previous->vanillaLayoutD3DDeclaration;
					if (declaration != generation->vanillaLayoutD3DDeclaration
						&& std::find(
							generation->compatibleVanillaLayoutD3DDeclarations.begin(),
							generation->compatibleVanillaLayoutD3DDeclarations.end(),
							declaration)
							== generation->compatibleVanillaLayoutD3DDeclarations.end())
					{
						generation->compatibleVanillaLayoutD3DDeclarations.push_back(
							declaration);
					}
				}
			}
			gLog.FormattedMessage(
				"tnvse_freetype_native: vanilla-layout SDF generation status=%s ready=%u stride=%u vertexConstants=c%u-c%u declaration=%p compatiblePrevious=%u deviceEpoch=%u",
				vanillaLayoutStatus,
				generation->vanillaLayoutReady ? 1u : 0u,
				kVanillaLayoutVertexStride,
				kNativeA8VertexAaConstantRegister,
				kNativeA8VanillaLayoutGlyphConstantRegister,
				generation->vanillaLayoutD3DDeclaration,
				static_cast<UInt32>(
					generation->compatibleVanillaLayoutD3DDeclarations.size()),
				generation->deviceEpoch);
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
				const UInt32 deviceEpoch = s_deviceEpoch.fetch_add(
					1u, std::memory_order_acq_rel) + 1u;
				s_resetInProgress.store(true, std::memory_order_release);
				ResetFreeTypeGpuTiming();
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
					"tnvse_freetype_native: generation-invalidated reason=device-reset generation=%u deviceEpoch=%u phase=release; dynamic VB/IB ring released",
					current ? current->id : 0, deviceEpoch);
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
		if (!forceAttempt)
		{
			NativeShaderGeneration* published = s_publishedGeneration.load(
				std::memory_order_acquire);
			if (GenerationMatchesCurrentDevice(published))
				return true;
		}

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
			&& renderer->m_kD3DCaps9.MaxVertexShaderConst
				> kNativeA8VertexAaConstantRegister
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
		else if (renderer->m_kD3DCaps9.MaxVertexShaderConst
			<= kNativeA8VertexAaConstantRegister)
		{
			failure = "shader-constant-registers";
		}

		if (!candidate || !GenerationResourcesReady(candidate))
		{
			if (reportFailures)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: initialization failed reason=%s; retaining generation=%u when valid; new text uses the vanilla ARGB TileShader fallback when no complete native generation is available",
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
			!UsesBakedEffectRoute()
				&& candidate->distanceFieldMethod
					== DistanceFieldMethod::Mtsdf
				? "lazy-36" : "disabled";
		gLog.FormattedMessage(
			"tnvse_freetype_native: published complete TileShader generation=%u device=%p route=%s distanceField=%s mtsdfCompositeProfiles=%s constantAbi=vanilla-ps-c0-vs-c0-c4-private-ps-c176-c183-vs-c208-c209 privateUpload=prefix-2-4-8 vanillaC0=map-owned vanillaLayoutCarry=witnessed-native-callback foreignPassBoundary=hard-invalidate vertexAa=analytic-c208-all-layouts vanillaGlyph=c209 vertexFormat=float4 vertexStride=%u declTypes=0x%08X maxVertexConstants=%u",
			candidate->id, candidate->device,
			UsesBakedEffectRoute()
				? "argb-composite" : "distance-field",
			UsesBakedEffectRoute()
				? "disabled" : GetConfiguredDistanceFieldMethodName(),
			compositeProfileMode,
			static_cast<UInt32>(sizeof(NativeA8GpuVertex)),
			candidate->renderer->m_kD3DCaps9.DeclTypes,
			candidate->renderer->m_kD3DCaps9.MaxVertexShaderConst);
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

	bool GetNativeA8RendererReadinessFast(
		NativeA8RendererReadinessView& view)
	{
		view = {};
		NativeShaderGeneration* generation = s_publishedGeneration.load(
			std::memory_order_acquire);
		if (!generation
			|| s_resetInProgress.load(std::memory_order_acquire)
			|| generation->runtimeFault.load(std::memory_order_acquire)
			|| !generation->renderer || !generation->device
			|| !generation->declaration || !generation->d3dDeclaration
			|| !generation->vertexShader)
		{
			return false;
		}
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		IDirect3DDevice9* device = renderer
			? renderer->GetD3DDevice() : nullptr;
		if (generation->renderer != renderer || generation->device != device)
			return false;
		view.renderer = renderer;
		view.device = device;
		view.generation = generation->id;
		view.ready = view.generation != 0;
		return view.ready;
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
			batch.renderState = nullptr;
			batch.generation = 0;
			batch.packetProfile = nullptr;
			batch.packetRegisterCount = 0;
			batch.vertexAa = {};
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
			batch.renderState = nullptr;
			batch.generation = 0;
			batch.packetProfile = nullptr;
			batch.packetRegisterCount = 0;
			batch.vertexAa = {};
			batch.samplerReady = false;
		}
	}

	void InvalidateNativeA8SortedShaderState()
	{
		InvalidateNativeA8CommandExecutionSegment(
			NativeA8CommandFallback::State);
		ResetSortedShaderStateCaches();
	}

	void InvalidateNativeA8SortedShaderStateWithinExecutionSegment()
	{
		ResetSortedShaderStateCaches();
	}

	void InvalidateNativeA8SortedShaderStateForForeignRenderPass()
	{
		const NativeSortedShaderBatch& sortedBatch = s_sortedShaderBatch;
		const NativeFacadeShaderBatch& facadeBatch = s_facadeShaderBatch;
		const bool hadPrivateState = (sortedBatch.depth
			&& (sortedBatch.packetProfile
				|| sortedBatch.vertexAa.aaConstantReady
				|| sortedBatch.vertexAa.vanillaGlyphConstantReady))
			|| (facadeBatch.depth
				&& (facadeBatch.packetProfile
					|| facadeBatch.vertexAa.aaConstantReady
					|| facadeBatch.vertexAa.vanillaGlyphConstantReady));
		InvalidateNativeA8SortedShaderState();
		if (hadPrivateState)
		{
			RecordFreeTypePerf(FreeTypePerfCounter::
				NativePrivateStateForeignRenderPassInvalidation);
		}
	}

	static void PrepareNativeA8VanillaLayoutPrivateStateCarry()
	{
		// This helper is private to the exact Vanilla-layout shader transition.
		// Preserve only the immutable c176-c183/c208/c209 shadow signed by that
		// transition while invalidating its command proof and mutable bindings.
		// Arbitrary non-A8 RenderPasses must use the hard invalidation path above:
		// both reversed executables dispatch generic shader callbacks there.
		InvalidateNativeA8CommandExecutionSegment(
			NativeA8CommandFallback::State);
		NativeSortedShaderBatch& batch = s_sortedShaderBatch;
		NiD3DRenderState* renderState =
			batch.depth && batch.device
				&& IsNativeA8ShaderGenerationCurrent(batch.generation)
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
		NativeFacadeShaderBatch& facadeBatch = s_facadeShaderBatch;
		facadeBatch.packetProfile = nullptr;
		facadeBatch.packetRegisterCount = 0;
		facadeBatch.vertexAa = {};
		facadeBatch.samplerReady = false;
	}

	static void ValidateNativeA8VanillaLayoutPrivateStateCarry()
	{
		NativeSortedShaderBatch& batch = s_sortedShaderBatch;
		NiD3DRenderState* renderState =
			batch.depth && batch.device
				&& IsNativeA8ShaderGenerationCurrent(batch.generation)
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

	void BeginNativeA8FacadeShaderBatch()
	{
		NativeFacadeShaderBatch& batch = s_facadeShaderBatch;
		++batch.depth;
		// Each scope owns one facade. A recursive scope deliberately discards the
		// parent's cached constants; End invalidates the parent again so its next
		// packet performs one full native pixel-constant upload.
		batch.packetProfile = nullptr;
		batch.packetRegisterCount = 0;
		batch.vertexAa = {};
		batch.samplerReady = false;
	}

	void EndNativeA8FacadeShaderBatch()
	{
		NativeFacadeShaderBatch& batch = s_facadeShaderBatch;
		if (!batch.depth)
			return;
		--batch.depth;
		batch.packetProfile = nullptr;
		batch.packetRegisterCount = 0;
		batch.vertexAa = {};
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
		const NiTriShape* facade, bool scaledFillSampling,
		bool vanillaLayoutSdf)
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
		NativeA8PacketShaderCacheEntry& packetCache = vanillaLayoutSdf
			? packet.vanillaLayoutResolvedShaders[cacheIndex]
			: packet.resolvedShaders[cacheIndex];
		NativeShaderProfile* cachedProfile =
			static_cast<NativeShaderProfile*>(
				packetCache.profile.load(std::memory_order_acquire));
		if (cachedProfile && cachedProfile->owner == generation
			&& cachedProfile->shader
			&& cachedProfile->key.sampling == sampling
			&& cachedProfile->key.writeEffectAlpha == writeEffectAlpha
			&& cachedProfile->key.vanillaLayoutSdf == vanillaLayoutSdf)
		{
			return cachedProfile->shader;
		}
		const NativeProfileKey key = MakeProfileKey(packet, sampling,
			writeEffectAlpha, vanillaLayoutSdf);
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

	UInt64 BeginNativeA8VanillaLayoutShaderTransition(
		TileShader* shader, UInt32 currentPass)
	{
		// Official B98E80 and beta Standard::RenderPassImmediately both execute
		// SetupGeometryConstants before blend/state setup, buffer preparation and
		// OnlyRenderImmediate. This exact native callback chain may retain its
		// disjoint c176-c183/c208/c209 shadow. The command segment, bindings and
		// sampler proof are still hard boundaries; unrelated RenderPasses are not
		// eligible for this carry.
		PrepareNativeA8VanillaLayoutPrivateStateCarry();
		NativeVanillaLayoutPublicationWitness& witness =
			s_vanillaLayoutPublicationWitness;
		UInt64 token = ++s_nextVanillaLayoutPublicationToken;
		if (!token)
			token = ++s_nextVanillaLayoutPublicationToken;
		witness = {};
		witness.token = token;
		witness.expectedShader = shader;
		// currentPass is the render-pass enum, not a zero-based pass index. Both
		// reverse targets contain special pass enums that bypass the virtual
		// SetupGeometryConstants callback, and their enum values differ. Do not
		// duplicate either executable's switch here: the actual NativeUpdateConstants
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

	bool EndNativeA8VanillaLayoutShaderTransition(
		UInt64 token, TileShader* shader)
	{
		NativeVanillaLayoutPublicationWitness& witness =
			s_vanillaLayoutPublicationWitness;
		NativeSortedShaderBatch& batch = s_sortedShaderBatch;
		NativeShaderProfile* publishedProfile = witness.publishedProfile;
		IDirect3DDevice9* publishedDevice = witness.publishedDevice;
		const bool witnessed = witness.armed && token
			&& witness.token == token && witness.publishedToken == token
			&& witness.expectedShader == shader && publishedProfile
			&& publishedProfile->shader == shader
			&& publishedProfile->key.vanillaLayoutSdf
			&& batch.depth && batch.packetProfile == publishedProfile
			&& batch.device == witness.publishedDevice
			&& batch.generation == witness.publishedGeneration
			&& batch.vertexAa.aaConstantReady
			&& batch.vertexAa.vanillaGlyphConstantReady
			&& IsNativeA8ShaderGenerationCurrent(batch.generation);
		witness = {};
		if (!witnessed)
		{
			ResetSortedShaderStateCaches();
			RecordFreeTypePerf(FreeTypePerfCounter::
				VanillaLayoutSdfPrivateStateCarryRejected);
			return false;
		}

		// SetupGeometryTextures and retail geometry preparation remain opaque
		// binding boundaries. The completed native constant callback is evidence
		// only for the private constant shadow, never for the sampler or command
		// execution segment.
		batch.samplerReady = false;
		ValidateNativeA8VanillaLayoutPrivateStateCarry();
		const bool retained = batch.packetProfile == publishedProfile
			&& batch.device == publishedDevice;
		RecordFreeTypePerf(retained
			? FreeTypePerfCounter::VanillaLayoutSdfPrivateStateCarry
			: FreeTypePerfCounter::VanillaLayoutSdfPrivateStateCarryRejected);
		return retained;
	}

	namespace
	{
		std::atomic<UInt32> s_vanillaLayoutReadinessLoggedMask{ 0 };
		std::atomic<UInt32> s_vanillaLayoutPostpackLoggedMask{ 0 };
		std::atomic<UInt32> s_vanillaLayoutUploadFailureLoggedMask{ 0 };
		std::atomic<bool> s_vanillaLayoutUploadSuccessLogged{ false };

		enum VanillaLayoutReadinessFailure : UInt32
		{
			kMissingShapeOrShader = 1u << 0,
			kShaderMismatch = 1u << 1,
			kProfileMismatch = 1u << 2,
			kGenerationMismatch = 1u << 3,
			kMissingModelData = 1u << 4,
			kMissingBuffer = 1u << 5,
			kDeclarationMismatch = 1u << 6,
			kStreamMismatch = 1u << 7,
			kStrideMismatch = 1u << 8,
			kVertexCountMismatch = 1u << 9,
			kVertexBufferMissing = 1u << 10,
			kNativePackIncomplete = 1u << 11,
			kNativePackContractMismatch = 1u << 12,
		};

		enum VanillaLayoutUploadFailure : UInt32
		{
			kUploadInvalidSource = 1u << 0,
			kUploadInvalidBuffer = 1u << 1,
			kUploadDescriptionFailed = 1u << 2,
			kUploadRangeInvalid = 1u << 3,
			kUploadLockFailed = 1u << 4,
			kUploadUnlockFailed = 1u << 5,
		};

		enum class VanillaLayoutDeclarationCompatibility : UInt8
		{
			Missing = 0,
			CurrentGeneration,
			CompatiblePreviousGeneration,
			Mismatch,
		};

		struct VanillaLayoutReadySnapshot
		{
			const NiTriShape* shape = nullptr;
			TileShader* shader = nullptr;
			const void* shaderVtable = nullptr;
			NativeShaderProfile* profile = nullptr;
			NativeShaderGeneration* generation = nullptr;
			IDirect3DVertexDeclaration9* generationDeclaration = nullptr;
			const NiTriShapeData* modelData = nullptr;
			const NiGeometryBufferData* buffer = nullptr;
			const void* bufferDeclaration = nullptr;
			const UInt32* strideArray = nullptr;
			const NiVBChip* vertexChip = nullptr;
			IDirect3DVertexBuffer9* vertexBuffer = nullptr;
			UInt32 generationId = 0;
			UInt32 deviceEpoch = 0;
			UInt32 streamCount = 0;
			UInt32 stride = 0;
			UInt32 bufferVertexCount = 0;
			UInt32 dataVertexCount = 0;
			UInt32 baseVertexIndex = 0;
			UInt32 vertexChipOffset = 0;
			UInt32 vertexChipSize = 0;
			UInt16 nativePackDataFlags = 0;
			UInt16 nativePackDirtyFlags = 0;
			UInt8 nativePackKeepFlags = 0;
			bool nativePackCompleted = false;
			bool priorGenerationDeclaration = false;
		};

		enum class VanillaLayoutDrawTokenMismatch : UInt8
		{
			None = 0,
			Uncertified,
			ShapeOrShader,
			Generation,
			Geometry,
			NativePack,
			Layout,
		};

		VanillaLayoutDeclarationCompatibility
		ClassifyVanillaLayoutDeclaration(const NativeShaderGeneration* generation,
			const NiGeometryBufferData* buffer)
		{
			if (!generation || !buffer || !buffer->m_hDeclaration
				|| !generation->vanillaLayoutD3DDeclaration)
			{
				return VanillaLayoutDeclarationCompatibility::Missing;
			}
			auto* declaration = static_cast<IDirect3DVertexDeclaration9*>(
				buffer->m_hDeclaration);
			if (declaration == generation->vanillaLayoutD3DDeclaration)
			{
				return VanillaLayoutDeclarationCompatibility::CurrentGeneration;
			}
			if (std::find(
					generation->compatibleVanillaLayoutD3DDeclarations.begin(),
					generation->compatibleVanillaLayoutD3DDeclarations.end(),
					declaration)
				!= generation->compatibleVanillaLayoutD3DDeclarations.end())
			{
				return VanillaLayoutDeclarationCompatibility::
					CompatiblePreviousGeneration;
			}
			return VanillaLayoutDeclarationCompatibility::Mismatch;
		}

		bool HasVanillaLayoutNativePackRetirementContract(
			const NiGeometryData* data)
		{
			if (!data)
				return false;
			const UInt8 retainedSources = static_cast<UInt8>(
				NiGeometryData::KEEP_COLOR | NiGeometryData::KEEP_UV);
			return (data->m_usDirtyFlags & NiGeometryData::CONSISTENCY_MASK)
					== NiGeometryData::STATIC
				&& !(data->m_ucKeepFlags & retainedSources);
		}

		bool HasVanillaLayoutNativePackCompletionProof(
			const NiGeometryData* data)
		{
			return HasVanillaLayoutNativePackRetirementContract(data)
				&& !(data->m_usDirtyFlags & NiGeometryData::DIRTY_MASK)
				&& !(data->m_usDataFlags & NiGeometryData::TEXTURE_SET_MASK)
				&& !data->m_pkColor && !data->m_pkTexture;
		}

		void RecordVanillaLayoutReadyObservations(
			bool nativePackCompleted, bool priorGenerationDeclaration)
		{
			if (nativePackCompleted)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutSdfPostpackCompletionReady);
			}
			if (priorGenerationDeclaration)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutSdfPriorGenerationDeclarationReady);
			}
		}

		VanillaLayoutDrawTokenMismatch MatchVanillaLayoutDrawToken(
			const NiTriShape* shape, TileShader* shader,
			const NativeA8ShapePayload& payload,
			const NativeA8VanillaLayoutDrawToken& token)
		{
			if (!token.valid)
				return VanillaLayoutDrawTokenMismatch::Uncertified;
			if (!shape || !shader || token.shapeIdentity != shape
				|| token.shaderIdentity != shader || shape->GetShader() != shader)
			{
				return VanillaLayoutDrawTokenMismatch::ShapeOrShader;
			}

			void** shaderVtable = *reinterpret_cast<void***>(shader);
			if (!shaderVtable || token.shaderVtableIdentity != shaderVtable)
				return VanillaLayoutDrawTokenMismatch::ShapeOrShader;
			NativeTileVtableBlock* block = RecoverNativeVtableBlock(shader);
			NativeShaderProfile* profile = block ? block->profile : nullptr;
			if (!profile || token.profileIdentity != profile
				|| profile->shader != shader || !profile->key.vanillaLayoutSdf)
			{
				return VanillaLayoutDrawTokenMismatch::ShapeOrShader;
			}
			const NativeA8PayloadTemplate* artifact =
				payload.payloadTemplate.get();
			const NativeA8PacketTemplate* packet = artifact
				&& artifact->compositePackets.size() == 1u
				? &artifact->compositePackets.front() : nullptr;
			if (!token.payloadUploaded || !payload.buildComplete
				|| token.payloadIdentity != &payload || !artifact || !packet
				|| token.artifactIdentity != artifact
				|| token.packetIdentity != packet)
			{
				return VanillaLayoutDrawTokenMismatch::Geometry;
			}

			NativeShaderGeneration* generation = s_publishedGeneration.load(
				std::memory_order_acquire);
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			// The generation and its declaration compatibility set are immutable and
			// process-lifetime once published. Exact generation/device-epoch identity
			// therefore preserves the earlier full declaration classification without
			// repeating its resource walk or compatible-declaration vector scan.
			if (!generation || token.generationIdentity != generation
				|| profile->owner != generation
				|| generation->id != token.generation
				|| generation->deviceEpoch != token.deviceEpoch
				|| s_deviceEpoch.load(std::memory_order_acquire)
					!= token.deviceEpoch
				|| s_resetInProgress.load(std::memory_order_acquire)
				|| generation->runtimeFault.load(std::memory_order_acquire)
				|| generation->renderer != renderer || generation->device != device
				|| !generation->vanillaLayoutReady
				|| !generation->vanillaLayoutDeclaration
				|| generation->vanillaLayoutD3DDeclaration
					!= token.generationDeclarationIdentity)
			{
				return VanillaLayoutDrawTokenMismatch::Generation;
			}

			const NiTriShapeData* data = shape->GetModelData();
			if (!data || token.modelDataIdentity != data
				|| !data->m_usVertices
				|| data->m_usVertices != token.dataVertexCount)
			{
				return VanillaLayoutDrawTokenMismatch::Geometry;
			}
			if (!token.nativePackCompleted
				|| !HasVanillaLayoutNativePackCompletionProof(data)
				|| token.nativePackDataFlags != data->m_usDataFlags
				|| token.nativePackDirtyFlags != data->m_usDirtyFlags
				|| token.nativePackKeepFlags != data->m_ucKeepFlags)
			{
				return VanillaLayoutDrawTokenMismatch::NativePack;
			}
			const NiGeometryBufferData* buffer = data->m_pkBuffData;
			if (!buffer || token.bufferIdentity != buffer)
				return VanillaLayoutDrawTokenMismatch::Geometry;

			const NiVBChip* chip = buffer->m_ppkVBChip
				&& buffer->m_uiStreamCount ? buffer->m_ppkVBChip[0] : nullptr;
			const UInt64 expectedByteOffset = chip
				? static_cast<UInt64>(chip->m_uiOffset)
					+ static_cast<UInt64>(buffer->m_uiBaseVertexIndex)
						* kVanillaLayoutVertexStride : 0u;
			const UInt64 expectedByteCount =
				static_cast<UInt64>(data->m_usVertices)
					* kVanillaLayoutVertexStride;
			if (!token.bufferDeclarationIdentity
				|| buffer->m_hDeclaration != token.bufferDeclarationIdentity
				|| token.streamCount != 1u
				|| buffer->m_uiStreamCount != token.streamCount
				|| !token.strideArrayIdentity
				|| buffer->m_puiVertexStride != token.strideArrayIdentity
				|| token.stride != kVanillaLayoutVertexStride
				|| buffer->m_puiVertexStride[0] != token.stride
				|| buffer->m_uiVertCount != token.bufferVertexCount
				|| token.bufferVertexCount < token.dataVertexCount
				|| !chip || token.vertexChipIdentity != chip
				|| !chip->m_pkVB || token.vertexBufferIdentity != chip->m_pkVB
				|| token.baseVertexIndex != buffer->m_uiBaseVertexIndex
				|| token.vertexChipOffset != chip->m_uiOffset
				|| token.vertexChipSize != chip->m_uiSize
				|| expectedByteOffset > std::numeric_limits<UInt32>::max()
				|| expectedByteCount > std::numeric_limits<UInt32>::max()
				|| token.uploadedByteOffset
					!= static_cast<UInt32>(expectedByteOffset)
				|| token.uploadedByteCount
					!= static_cast<UInt32>(expectedByteCount))
			{
				return VanillaLayoutDrawTokenMismatch::Layout;
			}
			return VanillaLayoutDrawTokenMismatch::None;
		}

		void RecordVanillaLayoutDrawTokenMiss(
			VanillaLayoutDrawTokenMismatch mismatch)
		{
			RecordFreeTypePerf(FreeTypePerfCounter::
				VanillaLayoutSdfDrawTokenFullValidation);
			switch (mismatch)
			{
			case VanillaLayoutDrawTokenMismatch::Uncertified:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutSdfDrawTokenCold);
				break;
			case VanillaLayoutDrawTokenMismatch::ShapeOrShader:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutSdfDrawTokenShapeShaderInvalidation);
				break;
			case VanillaLayoutDrawTokenMismatch::Generation:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutSdfDrawTokenGenerationInvalidation);
				break;
			case VanillaLayoutDrawTokenMismatch::Geometry:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutSdfDrawTokenGeometryInvalidation);
				break;
			case VanillaLayoutDrawTokenMismatch::NativePack:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutSdfDrawTokenNativePackInvalidation);
				break;
			case VanillaLayoutDrawTokenMismatch::Layout:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutSdfDrawTokenLayoutInvalidation);
				break;
			default:
				break;
			}
		}

		void CertifyVanillaLayoutDrawToken(
			NativeA8VanillaLayoutDrawToken& token,
			const VanillaLayoutReadySnapshot& snapshot,
			const NativeA8ShapePayload& payload,
			const NativeA8PayloadTemplate& artifact,
			const NativeA8PacketTemplate& packet,
			UInt32 uploadedByteOffset, UInt32 uploadedByteCount)
		{
			// Publish validity last so a later reuse cannot observe a partially
			// rewritten proof.
			token.valid = false;
			token.shapeIdentity = snapshot.shape;
			token.shaderIdentity = snapshot.shader;
			token.shaderVtableIdentity = snapshot.shaderVtable;
			token.profileIdentity = snapshot.profile;
			token.generationIdentity = snapshot.generation;
			token.generationDeclarationIdentity =
				snapshot.generationDeclaration;
			token.modelDataIdentity = snapshot.modelData;
			token.bufferIdentity = snapshot.buffer;
			token.bufferDeclarationIdentity = snapshot.bufferDeclaration;
			token.strideArrayIdentity = snapshot.strideArray;
			token.vertexChipIdentity = snapshot.vertexChip;
			token.vertexBufferIdentity = snapshot.vertexBuffer;
			token.payloadIdentity = &payload;
			token.artifactIdentity = &artifact;
			token.packetIdentity = &packet;
			token.generation = snapshot.generationId;
			token.deviceEpoch = snapshot.deviceEpoch;
			token.streamCount = snapshot.streamCount;
			token.stride = snapshot.stride;
			token.bufferVertexCount = snapshot.bufferVertexCount;
			token.dataVertexCount = snapshot.dataVertexCount;
			token.baseVertexIndex = snapshot.baseVertexIndex;
			token.vertexChipOffset = snapshot.vertexChipOffset;
			token.vertexChipSize = snapshot.vertexChipSize;
			token.uploadedByteOffset = uploadedByteOffset;
			token.uploadedByteCount = uploadedByteCount;
			token.nativePackDataFlags = snapshot.nativePackDataFlags;
			token.nativePackDirtyFlags = snapshot.nativePackDirtyFlags;
			token.nativePackKeepFlags = snapshot.nativePackKeepFlags;
			token.nativePackCompleted = snapshot.nativePackCompleted;
			token.priorGenerationDeclaration =
				snapshot.priorGenerationDeclaration;
			token.payloadUploaded = true;
			token.everCertified = true;
			token.valid = true;
		}

		bool IsNativeA8VanillaLayoutShapeReadyImpl(const NiTriShape* shape,
			TileShader* shader, bool logFailure,
			VanillaLayoutReadySnapshot* readySnapshot)
		{
			if (readySnapshot)
				*readySnapshot = {};
			const bool hasShapeAndShader = shape && shader;
			const bool shaderMatches = hasShapeAndShader
				&& shape->GetShader() == shader;
			NativeTileVtableBlock* block = shader
				? RecoverNativeVtableBlock(shader) : nullptr;
			NativeShaderProfile* profile = block ? block->profile : nullptr;
			NativeShaderGeneration* generation = profile
				? profile->owner : nullptr;
			const NiTriShapeData* data = shape ? shape->GetModelData() : nullptr;
			const NiGeometryBufferData* buffer = data
				? data->m_pkBuffData : nullptr;
			const bool profileMatches = profile && profile->shader == shader
				&& profile->key.vanillaLayoutSdf;
			const bool generationMatches = generation
				&& generation->vanillaLayoutReady
				&& GenerationMatchesCurrentDevice(generation);
			const UInt32 streamCount = buffer ? buffer->m_uiStreamCount : 0u;
			const UInt32 stride = buffer && buffer->m_puiVertexStride
				&& streamCount ? buffer->m_puiVertexStride[0] : 0u;
			const NiVBChip* vertexChip = buffer && buffer->m_ppkVBChip
				&& streamCount ? buffer->m_ppkVBChip[0] : nullptr;
			IDirect3DVertexBuffer9* vertexBuffer = vertexChip
				? vertexChip->m_pkVB : nullptr;
			IDirect3DVertexDeclaration9* expectedDeclaration = generation
				? generation->vanillaLayoutD3DDeclaration : nullptr;
			const VanillaLayoutDeclarationCompatibility declarationCompatibility =
				ClassifyVanillaLayoutDeclaration(generation, buffer);
			const bool declarationMatches = declarationCompatibility
				== VanillaLayoutDeclarationCompatibility::CurrentGeneration
				|| declarationCompatibility == VanillaLayoutDeclarationCompatibility::
					CompatiblePreviousGeneration;
			// Retail PerformPrecache clears the low 12 dirty bits at 0xE74469 and
			// then retires static color/UV sources through 0xE7447D -> 0xE6FA90.
			// Under the renderer lock, all of these postconditions together are the
			// completion proof that neither vanilla nor NVTF still owns a queued pack
			// which may overwrite our final 40-byte stream.
			const bool nativePackRetirementContract =
				HasVanillaLayoutNativePackRetirementContract(data);
			const bool nativePackCompleted =
				HasVanillaLayoutNativePackCompletionProof(data);
			const bool structurallyReady = hasShapeAndShader && shaderMatches
				&& profileMatches && generationMatches && data
				&& buffer && declarationMatches
				&& streamCount == 1u && stride == kVanillaLayoutVertexStride
				&& buffer->m_uiVertCount >= data->m_usVertices
				&& vertexChip && vertexBuffer;
			const bool ready = structurallyReady && nativePackCompleted;
			if (ready)
			{
				const bool priorGenerationDeclaration =
					declarationCompatibility == VanillaLayoutDeclarationCompatibility::
						CompatiblePreviousGeneration;
				if (readySnapshot)
				{
					readySnapshot->shape = shape;
					readySnapshot->shader = shader;
					readySnapshot->shaderVtable =
						*reinterpret_cast<void***>(shader);
					readySnapshot->profile = profile;
					readySnapshot->generation = generation;
					readySnapshot->generationDeclaration = expectedDeclaration;
					readySnapshot->modelData = data;
					readySnapshot->buffer = buffer;
					readySnapshot->bufferDeclaration = buffer->m_hDeclaration;
					readySnapshot->strideArray = buffer->m_puiVertexStride;
					readySnapshot->vertexChip = vertexChip;
					readySnapshot->vertexBuffer = vertexBuffer;
					readySnapshot->generationId = generation->id;
					readySnapshot->deviceEpoch = generation->deviceEpoch;
					readySnapshot->streamCount = streamCount;
					readySnapshot->stride = stride;
					readySnapshot->bufferVertexCount = buffer->m_uiVertCount;
					readySnapshot->dataVertexCount = data->m_usVertices;
					readySnapshot->baseVertexIndex =
						buffer->m_uiBaseVertexIndex;
					readySnapshot->vertexChipOffset = vertexChip->m_uiOffset;
					readySnapshot->vertexChipSize = vertexChip->m_uiSize;
					readySnapshot->nativePackDataFlags = data->m_usDataFlags;
					readySnapshot->nativePackDirtyFlags = data->m_usDirtyFlags;
					readySnapshot->nativePackKeepFlags = data->m_ucKeepFlags;
					readySnapshot->nativePackCompleted = nativePackCompleted;
					readySnapshot->priorGenerationDeclaration =
						priorGenerationDeclaration;
				}
				UInt32 observations = 0;
				if (nativePackCompleted)
				{
					observations |= 1u;
				}
				if (priorGenerationDeclaration)
				{
					observations |= 2u;
				}
				RecordVanillaLayoutReadyObservations(
					nativePackCompleted, priorGenerationDeclaration);
				if (observations && logFailure)
				{
					const UInt32 previous = s_vanillaLayoutPostpackLoggedMask.fetch_or(
						observations, std::memory_order_acq_rel);
					const UInt32 newlyObserved = observations & ~previous;
					if (newlyObserved)
					{
						gLog.FormattedMessage(
							"tnvse_freetype_vanilla_layout_sdf_postpack: observed=0x%08X new=0x%08X shape=%p generation=%u deviceEpoch=%u completion=1 dataFlags=0x%04X dirtyFlags=0x%04X keepFlags=0x%02X color=%p texture=%p declarationClass=%u declaration=%p expected=%p compatiblePrevious=%u stride=%u",
							observations, newlyObserved, shape,
							generation ? generation->id : 0u,
							generation ? generation->deviceEpoch : 0u,
							data ? static_cast<UInt32>(data->m_usDataFlags) : 0u,
							data ? static_cast<UInt32>(data->m_usDirtyFlags) : 0u,
							data ? static_cast<UInt32>(data->m_ucKeepFlags) : 0u,
							data ? data->m_pkColor : nullptr,
							data ? data->m_pkTexture : nullptr,
							static_cast<UInt32>(declarationCompatibility),
							buffer ? buffer->m_hDeclaration : nullptr,
							expectedDeclaration,
							generation ? static_cast<UInt32>(generation->
								compatibleVanillaLayoutD3DDeclarations.size()) : 0u,
							stride);
					}
				}
				return true;
			}
			if (!logFailure)
				return ready;

			UInt32 failures = 0;
			if (!hasShapeAndShader)
				failures |= kMissingShapeOrShader;
			if (hasShapeAndShader && !shaderMatches)
				failures |= kShaderMismatch;
			if (!profileMatches)
				failures |= kProfileMismatch;
			if (!generationMatches)
				failures |= kGenerationMismatch;
			if (!data)
				failures |= kMissingModelData;
			if (!buffer)
				failures |= kMissingBuffer;
			if (buffer && !declarationMatches)
				failures |= kDeclarationMismatch;
			if (buffer && streamCount != 1u)
				failures |= kStreamMismatch;
			if (buffer && stride != kVanillaLayoutVertexStride)
				failures |= kStrideMismatch;
			if (buffer && data && buffer->m_uiVertCount < data->m_usVertices)
				failures |= kVertexCountMismatch;
			if (buffer && (!vertexChip || !vertexBuffer))
				failures |= kVertexBufferMissing;
			if (data && !nativePackRetirementContract)
				failures |= kNativePackContractMismatch;
			if (structurallyReady && nativePackRetirementContract
				&& !nativePackCompleted)
			{
				failures |= kNativePackIncomplete;
				RecordFreeTypePerf(
					FreeTypePerfCounter::VanillaLayoutSdfNativePackPending);
			}

			const UInt32 previous = s_vanillaLayoutReadinessLoggedMask.fetch_or(
				failures, std::memory_order_acq_rel);
			const UInt32 newlyObserved = failures & ~previous;
			if (newlyObserved)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_vanilla_layout_sdf_readiness: failure=0x%08X new=0x%08X shape=%p shader=%p shapeShader=%p profile=%u vanillaProfile=%u generation=%u current=%u targetReady=%u deviceEpoch=%u completionContract=%u nativePackCompleted=%u dataFlags=0x%04X dirtyFlags=0x%04X keepFlags=0x%02X color=%p texture=%p buffer=%p declarationClass=%u declaration=%p expected=%p compatiblePrevious=%u streams=%u stride=%u bufferVertices=%u dataVertices=%u chip=%p vertexBuffer=%p",
					failures, newlyObserved, shape, shader,
					shape ? shape->GetShader() : nullptr,
					profile ? 1u : 0u,
					profileMatches ? 1u : 0u,
					generation ? generation->id : 0u,
					generationMatches ? 1u : 0u,
					generation && generation->vanillaLayoutReady ? 1u : 0u,
					generation ? generation->deviceEpoch : 0u,
					nativePackRetirementContract ? 1u : 0u,
					nativePackCompleted ? 1u : 0u,
					data ? static_cast<UInt32>(data->m_usDataFlags) : 0u,
					data ? static_cast<UInt32>(data->m_usDirtyFlags) : 0u,
					data ? static_cast<UInt32>(data->m_ucKeepFlags) : 0u,
					data ? data->m_pkColor : nullptr,
					data ? data->m_pkTexture : nullptr,
					buffer,
					static_cast<UInt32>(declarationCompatibility),
					buffer ? buffer->m_hDeclaration : nullptr,
					expectedDeclaration,
					generation ? static_cast<UInt32>(generation->
						compatibleVanillaLayoutD3DDeclarations.size()) : 0u,
					streamCount, stride,
					buffer ? buffer->m_uiVertCount : 0u,
					data ? data->m_usVertices : 0u,
					vertexChip, vertexBuffer);
			}
			return false;
		}

		class VanillaLayoutRendererLockScope final
		{
		public:
			explicit VanillaLayoutRendererLockScope(NiDX9Renderer* renderer)
				: m_renderer(renderer)
			{
				if (m_renderer)
					m_renderer->LockRenderer();
			}

			~VanillaLayoutRendererLockScope()
			{
				if (m_renderer)
					m_renderer->UnlockRenderer();
			}

			VanillaLayoutRendererLockScope(
				const VanillaLayoutRendererLockScope&) = delete;
			VanillaLayoutRendererLockScope& operator=(
				const VanillaLayoutRendererLockScope&) = delete;

		private:
			NiDX9Renderer* m_renderer = nullptr;
		};

		bool RejectVanillaLayoutPayloadUpload(UInt32 failure,
			const char* operation, HRESULT result,
			const VanillaLayoutReadySnapshot& snapshot,
			UInt32 bufferSize, UInt64 byteOffset, UInt64 byteCount)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutSdfPayloadUploadFailure);
			const UInt32 previous =
				s_vanillaLayoutUploadFailureLoggedMask.fetch_or(
					failure, std::memory_order_acq_rel);
			if (failure & ~previous)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_vanilla_layout_upload: status=failure failure=0x%08X new=0x%08X operation=%s hr=0x%08X shape=%p data=%p buffer=%p chip=%p vertexBuffer=%p stride=%u dataVertices=%u bufferVertices=%u baseVertex=%u chipOffset=%u chipSize=%u bufferSize=%u byteOffset=%I64u byteCount=%I64u",
					failure, failure & ~previous,
					operation ? operation : "unknown",
					static_cast<UInt32>(result), snapshot.shape,
					snapshot.modelData, snapshot.buffer, snapshot.vertexChip,
					snapshot.vertexBuffer, snapshot.stride,
					snapshot.dataVertexCount, snapshot.bufferVertexCount,
					snapshot.baseVertexIndex, snapshot.vertexChipOffset,
					snapshot.vertexChipSize, bufferSize, byteOffset, byteCount);
			}
			return false;
		}

		bool UploadVanillaLayoutPayload(
			const NativeA8ShapePayload& payload,
			const VanillaLayoutReadySnapshot& snapshot,
			const NativeA8PayloadTemplate*& artifactOut,
			const NativeA8PacketTemplate*& packetOut,
			UInt32& byteOffsetOut, UInt32& byteCountOut)
		{
			artifactOut = nullptr;
			packetOut = nullptr;
			byteOffsetOut = 0;
			byteCountOut = 0;
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutSdfPayloadUploadAttempt);

			const NativeA8PayloadTemplate* artifact =
				payload.payloadTemplate.get();
			const NativeA8PacketTemplate* packet = artifact
				&& artifact->compositePackets.size() == 1u
				? &artifact->compositePackets.front() : nullptr;
			const UInt64 packetEnd = packet
				? static_cast<UInt64>(packet->firstVertex)
					+ packet->vertexCount : 0u;
			const NiPoint3& origin = payload.geometryOrigin;
			const bool finiteOrigin = std::isfinite(origin.x)
				&& std::isfinite(origin.y) && std::isfinite(origin.z);
			if (!payload.buildComplete || !artifact || !packet
				|| !HasNativeA8PayloadValidationSeal(*artifact)
				|| artifact->pageCount != 1u
				|| packet->shaderClass != NativeA8ShaderClass::Composite
				|| (packet->distanceFieldMethod != DistanceFieldMethod::TrueSdf
					&& packet->distanceFieldMethod != DistanceFieldMethod::Mtsdf)
				|| packet->atlasPage != 0u || !packet->vertexCount
				|| (packet->firstVertex & 3u) || (packet->vertexCount & 3u)
				|| packet->staticCompositeLayerMask < 8u
				|| packet->staticCompositeLayerMask > 15u
				|| !std::isfinite(packet->uniformSdfSpread)
				|| packet->uniformSdfSpread <= 0.0f
				|| packet->vertexCount != snapshot.dataVertexCount
				|| packetEnd > artifact->gpuVertices.size() || !finiteOrigin)
			{
				return RejectVanillaLayoutPayloadUpload(kUploadInvalidSource,
					"source-contract", E_INVALIDARG, snapshot, 0u, 0u, 0u);
			}

			for (UInt32 ordinal = 0; ordinal < packet->vertexCount; ++ordinal)
			{
				const NativeA8GpuVertex& source = artifact->gpuVertices[
					static_cast<size_t>(packet->firstVertex) + ordinal];
				if (!std::isfinite(source.x) || !std::isfinite(source.y)
					|| !std::isfinite(source.z) || !std::isfinite(source.u)
					|| !std::isfinite(source.v)
					|| !std::isfinite(source.glyphU0)
					|| !std::isfinite(source.glyphV0)
					|| !std::isfinite(source.glyphU1)
					|| !std::isfinite(source.glyphV1)
					|| source.glyphU0 > source.glyphU1
					|| source.glyphV0 > source.glyphV1
					|| !std::isfinite(source.x + origin.x)
					|| !std::isfinite(source.y + origin.y)
					|| !std::isfinite(source.z + origin.z))
				{
					return RejectVanillaLayoutPayloadUpload(kUploadInvalidSource,
						"source-vertex", E_INVALIDARG, snapshot,
						0u, 0u, 0u);
				}
			}

			const NiVBChip* chip = snapshot.vertexChip;
			IDirect3DVertexBuffer9* vertexBuffer = snapshot.vertexBuffer;
			if (!snapshot.buffer || !chip || !vertexBuffer
				|| snapshot.buffer->m_uiStreamCount != snapshot.streamCount
				|| !snapshot.buffer->m_puiVertexStride
				|| snapshot.buffer->m_puiVertexStride[0] != snapshot.stride
				|| snapshot.buffer->m_uiVertCount != snapshot.bufferVertexCount
				|| snapshot.buffer->m_uiBaseVertexIndex
					!= snapshot.baseVertexIndex
				|| chip->m_pkVB != vertexBuffer
				|| chip->m_uiOffset != snapshot.vertexChipOffset
				|| chip->m_uiSize != snapshot.vertexChipSize
				|| snapshot.stride != sizeof(NativeA8VanillaLayoutVertex)
				|| !snapshot.dataVertexCount || !snapshot.vertexChipSize
				|| chip->m_uiLockFlags != 0u)
			{
				return RejectVanillaLayoutPayloadUpload(kUploadInvalidBuffer,
					"buffer-contract", D3DERR_INVALIDCALL, snapshot,
					0u, 0u, 0u);
			}

			D3DVERTEXBUFFER_DESC description = {};
			const HRESULT descriptionResult = vertexBuffer->GetDesc(&description);
			if (FAILED(descriptionResult))
			{
				return RejectVanillaLayoutPayloadUpload(
					kUploadDescriptionFailed, "GetDesc", descriptionResult,
					snapshot, 0u, 0u, 0u);
			}

			const UInt64 relativeByteOffset =
				static_cast<UInt64>(snapshot.baseVertexIndex) * snapshot.stride;
			const UInt64 byteOffset = static_cast<UInt64>(snapshot.vertexChipOffset)
				+ relativeByteOffset;
			const UInt64 byteCount = static_cast<UInt64>(snapshot.dataVertexCount)
				* snapshot.stride;
			const bool chipRangeValid = relativeByteOffset
				<= snapshot.vertexChipSize
				&& byteCount <= static_cast<UInt64>(snapshot.vertexChipSize)
					- relativeByteOffset;
			const bool bufferRangeValid = byteOffset <= description.Size
				&& byteCount <= static_cast<UInt64>(description.Size) - byteOffset;
			if (!byteCount || byteOffset > std::numeric_limits<UInt32>::max()
				|| byteCount > std::numeric_limits<UInt32>::max()
				|| !chipRangeValid || !bufferRangeValid)
			{
				return RejectVanillaLayoutPayloadUpload(kUploadRangeInvalid,
					"range", D3DERR_INVALIDCALL, snapshot, description.Size,
					byteOffset, byteCount);
			}

			void* locked = nullptr;
			const HRESULT lockResult = vertexBuffer->Lock(
				static_cast<UINT>(byteOffset), static_cast<UINT>(byteCount),
				&locked, 0u);
			if (FAILED(lockResult))
			{
				return RejectVanillaLayoutPayloadUpload(kUploadLockFailed,
					"Lock", lockResult,
					snapshot, description.Size, byteOffset, byteCount);
			}
			if (!locked)
			{
				// Preserve the successful Lock/Unlock pairing even for a broken
				// driver or interposer that violates D3D9's output contract.
				vertexBuffer->Unlock();
				return RejectVanillaLayoutPayloadUpload(kUploadLockFailed,
					"Lock-null", E_POINTER, snapshot, description.Size,
					byteOffset, byteCount);
			}

			auto* destination =
				static_cast<NativeA8VanillaLayoutVertex*>(locked);
			for (UInt32 ordinal = 0; ordinal < packet->vertexCount; ++ordinal)
			{
				const NativeA8GpuVertex& source = artifact->gpuVertices[
					static_cast<size_t>(packet->firstVertex) + ordinal];
				NativeA8VanillaLayoutVertex& packed = destination[ordinal];
				packed.x = source.x + origin.x;
				packed.y = source.y + origin.y;
				packed.z = source.z + origin.z;
				packed.u = source.u;
				packed.v = source.v;
				packed.color = source.color;
				packed.glyphU0 = source.glyphU0;
				packed.glyphV0 = source.glyphV0;
				packed.glyphU1 = source.glyphU1;
				packed.glyphV1 = source.glyphV1;
			}
			const HRESULT unlockResult = vertexBuffer->Unlock();
			if (FAILED(unlockResult))
			{
				return RejectVanillaLayoutPayloadUpload(kUploadUnlockFailed,
					"Unlock", unlockResult, snapshot, description.Size,
					byteOffset, byteCount);
			}

			artifactOut = artifact;
			packetOut = packet;
			byteOffsetOut = static_cast<UInt32>(byteOffset);
			byteCountOut = static_cast<UInt32>(byteCount);
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutSdfPayloadUploadSuccess);
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutSdfPayloadUploadBytes,
				byteCountOut);
			if (!s_vanillaLayoutUploadSuccessLogged.exchange(
				true, std::memory_order_acq_rel))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_vanilla_layout_upload: status=ready shape=%p buffer=%p chip=%p vertexBuffer=%p stride=%u vertices=%u byteOffset=%u byteCount=%u pool=%u usage=0x%X",
					snapshot.shape, snapshot.buffer, snapshot.vertexChip,
					snapshot.vertexBuffer, snapshot.stride,
					snapshot.dataVertexCount, byteOffsetOut, byteCountOut,
					static_cast<UInt32>(description.Pool), description.Usage);
			}
			return true;
		}
	}

	bool RequestNativeA8VanillaLayoutShapePrecache(NiTriShape* shape,
		TileShader* shader, bool& immediateReady)
	{
		immediateReady = false;
		if (!shape || !shader)
			return false;
		NativeTileVtableBlock* block = RecoverNativeVtableBlock(shader);
		NativeShaderProfile* profile = block ? block->profile : nullptr;
		NativeShaderGeneration* generation = profile ? profile->owner : nullptr;
		NiTriShapeData* data = shape->GetModelData();
		const UInt32 textureCoordinatesPresent = data
			? data->m_usDataFlags & NiGeometryData::TEXTURE_SET_MASK : 0u;
		if (!profile || profile->shader != shader
			|| !profile->key.vanillaLayoutSdf || !generation
			|| !generation->renderer || !generation->vanillaLayoutReady
			|| !generation->vanillaLayoutDeclaration
			|| !GenerationMatchesCurrentDevice(generation) || !data
			|| !data->m_usVertices || !data->m_pkVertex || !data->m_pkColor
			|| !data->m_pkTexture || !textureCoordinatesPresent
			|| !data->m_pusTriList || !data->m_uiTriListLength
			|| !HasVanillaLayoutNativePackRetirementContract(data))
		{
			return false;
		}

		shape->SetShader(shader);
		if (data->m_pkBuffData)
		{
			// Keep this on the virtual route so renderer queue/interoperability
			// detours retain ownership of synchronization and buffer lifetime.
			generation->renderer->PurgeGeometryData(data);
			if (data->m_pkBuffData)
				return false;
		}
		if (!generation->renderer->PrecacheGeometry(shape, 0u, 0u,
			generation->vanillaLayoutDeclaration.m_pObject))
		{
			return false;
		}

		// Never inspect or upload immediately after this virtual call. Vanilla
		// PrecacheGeometryEx may have attached a fully shaped buffer while its
		// PrePackObject is still queued, and NVTF may instead have queued the whole
		// request on its worker. The owning route finishes asynchronously; the first
		// draw performs the one-time upload only after the postpack proof is visible
		// under the renderer lock.
		return true;
	}

	bool IsNativeA8VanillaLayoutShapeReady(const NiTriShape* shape,
		TileShader* shader, const NativeA8ShapePayload& payload,
		NativeA8VanillaLayoutDrawToken& drawToken)
	{
		const VanillaLayoutDrawTokenMismatch mismatch =
			MatchVanillaLayoutDrawToken(shape, shader, payload, drawToken);
		if (mismatch == VanillaLayoutDrawTokenMismatch::None)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutSdfDrawTokenHit);
			RecordVanillaLayoutReadyObservations(
				drawToken.nativePackCompleted,
				drawToken.priorGenerationDeclaration);
			return true;
		}

		RecordVanillaLayoutDrawTokenMiss(mismatch);
		const bool wasCertified = drawToken.everCertified;
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		if (!renderer)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutSdfDrawTokenRejected);
			return false;
		}
		VanillaLayoutRendererLockScope rendererLock(renderer);
		drawToken.Invalidate();
		VanillaLayoutReadySnapshot snapshot;
		if (!IsNativeA8VanillaLayoutShapeReadyImpl(
			shape, shader, true, &snapshot))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutSdfDrawTokenRejected);
			return false;
		}

		const NativeA8PayloadTemplate* artifact = nullptr;
		const NativeA8PacketTemplate* packet = nullptr;
		UInt32 uploadedByteOffset = 0;
		UInt32 uploadedByteCount = 0;
		if (!UploadVanillaLayoutPayload(payload, snapshot, artifact, packet,
			uploadedByteOffset, uploadedByteCount)
			|| !artifact || !packet)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutSdfDrawTokenRejected);
			return false;
		}

		CertifyVanillaLayoutDrawToken(drawToken, snapshot, payload,
			*artifact, *packet, uploadedByteOffset, uploadedByteCount);
		RecordFreeTypePerf(wasCertified
			? FreeTypePerfCounter::VanillaLayoutSdfDrawTokenRecertification
			: FreeTypePerfCounter::VanillaLayoutSdfDrawTokenFirstCertification);
		return true;
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

	namespace implementation::font_native_shader
	{
		HRESULT EnsureNativeSamplerState(IDirect3DDevice9* device,
			bool& changed, const char*& operation)
		{
			changed = false;
			operation = "none";
			NiD3DRenderState* renderState =
				ResolveSortedRenderState(device);
			if (!renderState)
			{
				operation = "resolve-engine-render-state(sampler)";
				return D3DERR_INVALIDCALL;
			}
			NativeSortedShaderBatch& batch = s_sortedShaderBatch;
			if (batch.depth && batch.device == device
				&& batch.samplerReady
				&& IsNativeMipFilterReady(renderState))
			{
				return D3D_OK;
			}

			// The retail reverse map has no SRGBTEXTURE slot; routing that enum
			// through NiD3DRenderState is a no-op. Fallout's Tile texture path
			// never publishes SRGBTEXTURE and D3D9 defaults it to FALSE, so do
			// not pay for a redundant virtual call that cannot affect state.
			// MIPFILTER is mirrored. Read that zero-driver-cost value first and
			// enter the engine setter only when normalization is actually needed.
			if (!IsNativeMipFilterReady(renderState))
			{
				renderState->SetSamplerState(
					0, D3DSAMP_MIPFILTER, D3DTEXF_NONE, false);
				if (!IsNativeMipFilterReady(renderState))
				{
					operation = "SetSamplerState(mip-shadow)";
					return E_FAIL;
				}
				changed = true;
			}
			if (batch.depth)
				batch.samplerReady = true;
			return D3D_OK;
		}

		HRESULT EnsureNativeTexture0(IDirect3DDevice9* device,
			const void* atlasTexture, const char*& operation)
		{
			operation = "none";
			if (!device || !atlasTexture)
			{
				operation = "validate-command-texture";
				return D3DERR_INVALIDCALL;
			}
			auto* texture = const_cast<IDirect3DBaseTexture9*>(
				static_cast<const IDirect3DBaseTexture9*>(atlasTexture));
			NiD3DRenderState* renderState =
				ResolveSortedRenderState(device);
			if (!renderState)
			{
				operation = "resolve-engine-render-state(texture)";
				return D3DERR_INVALIDCALL;
			}
			// The engine mirror is authoritative for texture stage 0. Requiring
			// a second traversal-local ready bit caused every command to call
			// SetTexture when a vanilla bootstrap had already installed this page.
			if (renderState->m_apkTextureStageTextures[0] == texture)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandTextureBindReuse);
				return D3D_OK;
			}
			// Direct IDirect3DDevice9::SetTexture left Gamebryo believing the
			// previous page was still bound. A later vanilla setup could then
			// suppress a required rebind. Publish through NiD3DRenderState so
			// both the device and its software mirror advance together.
			renderState->SetTexture(0, texture);
			if (renderState->m_apkTextureStageTextures[0] != texture)
			{
				operation = "SetTexture(command-page-shadow)";
				return E_FAIL;
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandTextureBindSet);
			return D3D_OK;
		}

		HRESULT PublishRetainedPacketConstants(
			IDirect3DDevice9* device, NativeShaderGeneration* generation,
			NativeShaderProfile* profile, const char*& operation)
		{
			operation = "none";
			if (!device || !generation || !profile)
			{
				operation = "validate-command-packet-constants";
				return D3DERR_INVALIDCALL;
			}
			NativeSortedShaderBatch& batch = s_sortedShaderBatch;
			const bool batchActive = batch.depth != 0;
			if (batchActive && (batch.device != device
				|| batch.generation != generation->id))
			{
				ResetSortedShaderIdentity(
					batch, device, generation->id);
			}

			HRESULT result = D3D_OK;
			const size_t targetRegisterCount =
				profile->privateRegisterCount;
			if (!batchActive || !batch.packetProfile
				|| !batch.packetRegisterCount)
			{
				result = device->SetPixelShaderConstantF(
					kNativeA8PixelConstantBaseRegister,
					profile->constants.data(), static_cast<UINT>(
						targetRegisterCount));
				if (FAILED(result))
				{
					operation =
						"SetPixelShaderConstantF(command-private-prefix)";
					return result;
				}
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						CommandPacketConstantFullUpload);
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						CommandPacketConstantRegisterUpload,
					targetRegisterCount);
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						CommandPacketConstantFullTailElided,
					kNativeA8PacketConstantRegisterCount
						- targetRegisterCount);
			}
			else
			{
				size_t firstChanged =
					targetRegisterCount;
				size_t lastChanged = 0;
				if (batch.packetProfile != profile
					|| batch.packetRegisterCount < targetRegisterCount)
				{
					for (size_t packetRegister = 0;
						packetRegister
							< targetRegisterCount;
						++packetRegister)
					{
						const size_t firstFloat =
							packetRegister * 4u;
						if (packetRegister
								>= batch.packetRegisterCount
							|| std::memcmp(
								profile->constants.data() + firstFloat,
								batch.packetProfile->constants.data()
									+ firstFloat,
								4u * sizeof(float)) != 0)
						{
							firstChanged = std::min(
								firstChanged, packetRegister);
							lastChanged = packetRegister;
						}
					}
				}
				if (firstChanged
					< targetRegisterCount)
				{
					result = device->SetPixelShaderConstantF(
						static_cast<UINT>(
							kNativeA8PixelConstantBaseRegister
								+ firstChanged),
						profile->constants.data()
							+ firstChanged * 4u,
						static_cast<UINT>(
							lastChanged - firstChanged + 1u));
					if (FAILED(result))
					{
						operation =
							"SetPixelShaderConstantF(command-partial)";
						return result;
					}
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							CommandPacketConstantPartialUpload);
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							CommandPacketConstantRegisterUpload,
						lastChanged - firstChanged + 1u);
				}
				else
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							CommandPacketConstantReuse);
				}
			}
			if (batchActive)
			{
				batch.packetProfile = profile;
				batch.packetRegisterCount =
					profile->privateRegisterCount;
			}
			return D3D_OK;
		}

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

		NativeSortedShaderBatch& sortedBatch = s_sortedShaderBatch;
		if (sortedBatch.depth
			&& (sortedBatch.device != device
				|| sortedBatch.generation != generation->id))
		{
			ResetSortedShaderIdentity(
				sortedBatch, device, generation->id);
		}
		NiD3DRenderState* renderState =
			ResolveSortedRenderState(device);
		if (!renderState)
		{
			operation = "resolve-engine-render-state(program)";
			result = D3DERR_INVALIDCALL;
			return false;
		}
		const bool programsReady =
			renderState->m_hCurrentVertexShader == command.vertexShader
			&& renderState->m_hCurrentPixelShader == command.pixelShader;
		// A profile change can also change pass render state even when two
		// profiles share shader handles, so it still requires SetupPass. If the
		// caller retained the same profile but an unexpected program mutation
		// occurred, repair it here instead of trusting a stale local pointer.
		const bool setupRequired = publishPrograms || !programsReady;
		if (setupRequired)
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
						bindState.firstPass);
			}
			// Reverse-verified TileShader::SetupGeometryTextures publishes both
			// programs through NiD3DRenderState before configuring stage 0.
			// Reissuing SetVertexShader/SetPixelShader here was a guaranteed
			// duplicate driver submission. Validate the software mirror instead.
			renderState = ResolveSortedRenderState(device);
			if (!renderState
				|| renderState->m_hCurrentVertexShader
					!= command.vertexShader
				|| renderState->m_hCurrentPixelShader
					!= command.pixelShader)
			{
				operation = "validate-command-program-shadow";
				result = E_FAIL;
				return false;
			}
			if (sortedBatch.depth)
			{
				// SetupGeometryTextures may publish MIPFILTER. Preserve a ready
				// result when the engine mirror already contains the native
				// level-zero contract instead of invalidating it unconditionally.
				sortedBatch.samplerReady =
					IsNativeMipFilterReady(renderState);
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandProgramSetup);
		}
		else
		{
			// Both engine program handles already match the retained command.
			// Count the two VS/PS publications avoided by the profile-local fast
			// path; no driver query or setup callback is needed.
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandProgramBindElided, 2);
		}

		if (!command.simpleColor)
		{
			const bool packetStateReady = sortedBatch.depth
				&& sortedBatch.device == device
				&& sortedBatch.generation == generation->id
				&& sortedBatch.packetProfile == profile
				&& sortedBatch.packetRegisterCount
					>= profile->privateRegisterCount
				&& sortedBatch.vertexAa.aaConstantReady
				&& sortedBatch.samplerReady
				&& IsNativeMipFilterReady(renderState);
			const bool publishPacketState = publishPrograms
				|| !programsReady || !packetStateReady;
			if (publishPacketState)
			{
				result = PublishRetainedPacketConstants(
					device, generation, profile, operation);
				if (FAILED(result))
					return false;
				const char* vertexOperation = "none";
				NativeVertexAaState* vertexCache =
					sortedBatch.depth
						? &sortedBatch.vertexAa : nullptr;
				result = PublishNativeVertexAaConstant(device,
					profile->constants[7], vertexCache, false, nullptr,
					vertexOperation);
				if (FAILED(result))
				{
					operation = vertexOperation;
					return false;
				}
				bool samplerChanged = false;
				const char* samplerOperation = "none";
				result = EnsureNativeSamplerState(
					device, samplerChanged, samplerOperation);
				if (FAILED(result))
				{
					operation = samplerOperation;
					return false;
				}
				RecordFreeTypePerf(samplerChanged
					? FreeTypePerfCounter::SamplerStateSet
					: FreeTypePerfCounter::SamplerStateReuse);
				if (sortedBatch.depth)
					sortedBatch.samplerReady = true;
			}
			else
			{
				// Same retained profile and unchanged engine mirrors prove that
				// pixel c176-c183, vertex c208 and MIPFILTER all remain current.
				// Account the implicit fast path without invoking three helper
				// binders.
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandPacketConstantReuse);
				RecordFreeTypePerf(
					FreeTypePerfCounter::VertexAaConstantReuse);
				RecordFreeTypePerf(
					FreeTypePerfCounter::SamplerStateReuse);
			}
		}

		result = EnsureNativeTexture0(device, atlasTexture, operation);
		if (FAILED(result))
			return false;
		return true;
	}

}

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
#include "NiTriShapeData.hpp"
#include "NiPropertyState.hpp"
#include "NiTexturingProperty.hpp"

#include <Windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fonthook::vectorfont
{
	namespace
	{
		static_assert(sizeof(void*) == 4,
			"The native FreeType TileShader path is a Win32 ABI implementation");

		inline constexpr UInt32 kTileShaderCreate = 0xBCAE90;
		inline constexpr UInt32 kTileShaderUpdateConstants = 0xBCA980;
		inline constexpr UInt32 kShaderDeclarationCreate = 0xE76700;
		inline constexpr UInt32 kTextureStageSetProperties = 0xBE0CF0;
		inline constexpr UInt32 kTextureStageSetFilter = 0xE7DEF0;
		inline constexpr UInt32 kPassSetRenderState = 0xB71A10;
		inline constexpr UInt32 kCopiedTileShaderVtableEntries = 84;
		inline constexpr UInt32 kUpdateConstantsVtableSlot = 31;
		inline constexpr UInt32 kNativeVtableMagic = 0x3841544E; // "NTA8"
		inline constexpr UInt32 kShaderRefreshMessage = 0;
		inline constexpr DWORD kInitializationRetryMilliseconds = 1000;

		using CreateVertexShaderFn = NiD3DVertexShader* (__cdecl*)(const char*);
		using CreatePixelShaderFn = NiD3DPixelShader* (__cdecl*)(const char*);
		using StockUpdateConstantsFn = void(__thiscall*)(TileShader*,
			const NiPropertyState*);

		struct NativeShaderGeneration;
		struct NativeShaderProfile;

		struct NativeProfileKey
		{
			NativeA8ShaderClass shaderClass = NativeA8ShaderClass::Original;
			NativeA8Sampling sampling = NativeA8Sampling::Point;
			EffectQuality quality = EffectQuality::Balanced;
			std::array<UInt32, 16> constantBits = {};
			bool writeEffectAlpha = false;
			bool usesLiveTileRgb = true;

			bool operator==(const NativeProfileKey& other) const
			{
				return shaderClass == other.shaderClass
					&& sampling == other.sampling
					&& quality == other.quality
					&& writeEffectAlpha == other.writeEffectAlpha
					&& usesLiveTileRgb == other.usesLiveTileRgb
					&& constantBits == other.constantBits;
			}
		};

		struct NativeProfileKeyHash
		{
			size_t operator()(const NativeProfileKey& key) const
			{
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
				const std::array<float, 16>& packetConstants)
				: owner(&generation), key(profileKey), constants(packetConstants)
			{
			}

			NativeShaderGeneration* const owner;
			const NativeProfileKey key;
			const std::array<float, 16> constants;
			NiPointer<TileShader> shaderOwner;
			TileShader* shader = nullptr;
			NativeTileVtableBlock* vtable = nullptr;
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
			NiD3DPixelShaderPtr originalShader;
			NiD3DPixelShaderPtr coverageShader;
			NiD3DPixelShaderPtr sdfShader;
			std::array<NiD3DPixelShaderPtr, 3> effectShaders;
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
		std::atomic<bool> s_resetInProgress = false;
		NiDX9Renderer* s_resetRenderer = nullptr;

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
				|| !HasShaderHandle(generation->originalShader)
				|| !HasShaderHandle(generation->coverageShader)
				|| !HasShaderHandle(generation->sdfShader))
			{
				return false;
			}
			for (const NiD3DPixelShaderPtr& shader : generation->effectShaders)
			{
				if (!HasShaderHandle(shader))
					return false;
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

		void __fastcall NativeUpdateConstants(TileShader* shader, void*,
			const NiPropertyState* properties)
		{
			// Preserve Tile's matrix, live color/fade, scissor and alpha contract.
			NativeTileVtableBlock* block = RecoverNativeVtableBlock(shader);
			StockUpdateConstantsFn stockUpdate = block && block->stockUpdateConstants
				? block->stockUpdateConstants
				: reinterpret_cast<StockUpdateConstantsFn>(kTileShaderUpdateConstants);
			stockUpdate(shader, properties);
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
					current->runtimeFault.store(true, std::memory_order_release);
				return;
			}

			NativeShaderGeneration* generation = profile->owner;
			IDirect3DDevice9* device = shader->m_pkD3DDevice;
			if (!device)
				device = generation->device;
			if (!device || device != generation->device)
			{
				MarkGenerationFault(generation, "device-mismatch",
					D3DERR_DEVICELOST);
				return;
			}

			HRESULT firstFailure = D3D_OK;
			if (profile->effectPass && !profile->key.usesLiveTileRgb)
			{
				// Fixed effects own their configured vertex RGB. Neutralize only the
				// stock Tile RGB while retaining its live alpha for menu fades.
				const float* stockTileColor = reinterpret_cast<const float*>(0x1202188);
				const float effectTileColor[4] = {
					1.0f, 1.0f, 1.0f, stockTileColor[3]
				};
				const HRESULT result = device->SetPixelShaderConstantF(0,
					effectTileColor, 1);
				if (FAILED(result))
					firstFailure = result;
			}

			// c1 is the packet layer modifier. COLOR0 carries only the shared
			// per-glyph base modifier; c2-c4 retain atlas/effect parameters.
			const HRESULT constantsResult = device->SetPixelShaderConstantF(1,
				profile->constants.data(), 4);
			if (FAILED(constantsResult) && SUCCEEDED(firstFailure))
				firstFailure = constantsResult;
			if (FAILED(firstFailure))
				MarkGenerationFault(generation, "SetPixelShaderConstantF",
					firstFailure);
		}

		NativeProfileKey MakeProfileKey(const NativeA8Packet& packet,
			NativeA8Sampling sampling, bool writeEffectAlpha)
		{
			NativeProfileKey key;
			key.shaderClass = packet.shaderClass;
			key.sampling = sampling;
			key.quality = packet.quality;
			key.writeEffectAlpha = writeEffectAlpha;
			key.usesLiveTileRgb = packet.usesLiveTileRgb;
			std::memcpy(key.constantBits.data(), packet.constants.data(),
				key.constantBits.size() * sizeof(UInt32));
			return key;
		}

		NativeA8Sampling ResolveEffectiveSampling(const NativeA8Packet& packet,
			bool scaledFillSampling)
		{
			if (packet.sampling == NativeA8Sampling::LinearLod0)
				return NativeA8Sampling::LinearLod0;
			if (packet.sampling == NativeA8Sampling::LinearMipmapped
				|| scaledFillSampling || packet.staticSmoothSampling)
			{
				return NativeA8Sampling::LinearMipmapped;
			}
			return NativeA8Sampling::Point;
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
			NativeShaderGeneration& generation, const NativeA8Packet& packet)
		{
			switch (packet.shaderClass)
			{
			case NativeA8ShaderClass::Original:
				return generation.originalShader.m_pObject;
			case NativeA8ShaderClass::Coverage:
				return generation.coverageShader.m_pObject;
			case NativeA8ShaderClass::Body:
				return generation.sdfShader.m_pObject;
			case NativeA8ShaderClass::Effect:
			{
				const size_t index = static_cast<size_t>(packet.quality);
				return index < generation.effectShaders.size()
					? generation.effectShaders[index].m_pObject : nullptr;
			}
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
			if (profile.key.shaderClass != NativeA8ShaderClass::Original)
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
			}

			SetPassRenderState(&pass, D3DRS_ZENABLE, FALSE);
			SetPassRenderState(&pass, D3DRS_ZWRITEENABLE, FALSE);
			if (profile.effectPass
				|| profile.key.shaderClass != NativeA8ShaderClass::Original)
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
				SetPassRenderState(&pass, D3DRS_COLORWRITEENABLE,
					D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN
					| D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
			}
		}

		NativeShaderProfile* CreateProfile(NativeShaderGeneration& generation,
			const NativeA8Packet& packet, const NativeProfileKey& key)
		{
			NiD3DPixelShader* pixelShader = ResolveProfilePixelShader(generation,
				packet);
			if (!pixelShader || !pixelShader->GetShaderHandle())
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

			// Preserve the complete immutable c1-c4 packet ABI. COLOR0 now owns
			// only the shared per-glyph base modifier, so replacing c1 with the
			// historical identity value would turn every fixed effect layer white.
			auto* profile = new NativeShaderProfile(generation, key,
				packet.constants);
			profile->shader = shader;
			profile->effectPass = packet.layer != 3;

			profile->shaderOwner = shaderGuard;
			shader->m_spShaderDecl = generation.declaration.m_pObject;
			shader->spShaderDeclarations[0] = generation.declaration.m_pObject;
			shader->spShaderDeclarations[1] = generation.declaration.m_pObject;
			for (NiD3DVertexShaderPtr& slot : shader->spVertexShaders)
				slot = generation.vertexShader.m_pObject;
			for (NiD3DPixelShaderPtr& slot : shader->spPixelShaders)
				slot = pixelShader;
			pass->m_spVertexShader = generation.vertexShader.m_pObject;
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
			return profile;
		}

		bool CreateNativeDeclaration(NativeShaderGeneration& generation,
			const char*& failure)
		{
			// Runtime object size is 0x38. The 0xD4 CommonLib declaration includes
			// static lookup tables incorrectly; never allocate/copy it with sizeof.
			NiDX9ShaderDeclarationPtr declaration =
				CdeclCall<NiDX9ShaderDeclaration*>(kShaderDeclarationCreate,
					generation.renderer, 3u, 1u);
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
				failure = "d3d-declaration";
				return false;
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
			generation->supportsSeparateAlpha =
				(renderer->m_kD3DCaps9.PrimitiveMiscCaps
					& D3DPMISCCAPS_SEPARATEALPHABLEND) != 0;

			generation->vertexShader = createVS("tnvse_freetype_native_vs.vso");
			generation->originalShader = createPS(
				"tnvse_freetype_native_original.pso");
			generation->coverageShader = createPS(
				"tnvse_freetype_native_coverage.pso");
			generation->sdfShader = createPS("tnvse_freetype_native_sdf.pso");
			const char* effectNames[] = {
				"tnvse_freetype_native_effects_fast.pso",
				"tnvse_freetype_native_effects_balanced.pso",
				"tnvse_freetype_native_effects_high.pso"
			};
			for (size_t index = 0; index < generation->effectShaders.size(); ++index)
				generation->effectShaders[index] = createPS(effectNames[index]);

			if (!HasShaderHandle(generation->vertexShader)
				|| !HasShaderHandle(generation->originalShader)
				|| !HasShaderHandle(generation->coverageShader)
				|| !HasShaderHandle(generation->sdfShader))
			{
				failure = "base-shader-set";
				return nullptr;
			}
			for (const NiD3DPixelShaderPtr& shader : generation->effectShaders)
			{
				if (!HasShaderHandle(shader))
				{
					failure = "effect-shader-set";
					return nullptr;
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
					current->runtimeFault.store(true, std::memory_order_release);
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
					"tnvse_freetype_native: initialization failed reason=%s; retaining generation=%u when valid; incomplete native submissions will be suppressed",
					failure, current ? current->id : 0);
			}
			return GenerationMatchesCurrentDevice(current);
		}

		candidate->id = s_nextGeneration++;
		s_processGenerations.push_back(candidate);
		s_publishedGeneration.store(candidate, std::memory_order_release);
		gLog.FormattedMessage(
			"tnvse_freetype_native: published complete TileShader generation=%u device=%p",
			candidate->id, candidate->device);
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

	void MarkNativeA8GenerationFault(UInt32 generation,
		const char* operation, HRESULT result)
	{
		NativeShaderGeneration* current = s_publishedGeneration.load(
			std::memory_order_acquire);
		if (current && current->id == generation)
			MarkGenerationFault(current, operation, result);
	}

	TileShader* ResolveNativeA8PacketShader(const NativeA8Packet& packet,
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
		const NativeProfileKey key = MakeProfileKey(packet, sampling,
			writeEffectAlpha);
		std::shared_ptr<const NativeProfileMap> snapshot =
			generation->profiles.load(std::memory_order_acquire);
		auto found = snapshot->find(key);
		if (found != snapshot->end())
			return found->second->shader;

		std::lock_guard<std::mutex> lock(generation->profileMutex);
		if (!GenerationMatchesCurrentDevice(generation))
			return nullptr;
		snapshot = generation->profiles.load(std::memory_order_acquire);
		found = snapshot->find(key);
		if (found != snapshot->end())
			return found->second->shader;

		NativeShaderProfile* profile = CreateProfile(*generation, packet, key);
		if (!profile)
		{
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
		return profile->shader;
	}

}

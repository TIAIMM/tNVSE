#pragma once

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

namespace fonthook::vectorfont::implementation::font_native_shader
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

		inline constexpr UInt32 kTileShaderCreateShader = 0xBCAE90;
		inline constexpr UInt32 kTileShaderSetupGeometryTextures = 0xBCA760;
		inline constexpr UInt32 kTileShaderSetupGeometryConstants = 0xBCA980;
		inline constexpr UInt32 kBSShaderSetupGeometryAlphaBlending = 0xBE1FF0;
		inline constexpr UInt32 kBSRenderStateSetAlphaBlendEnable = 0xB97FA0;
		inline constexpr UInt32 kBSRenderStateSetAlphaBlendFunc = 0xB97FF0;
		inline constexpr UInt32 kBSShaderSetupGeometryAlphaTesting = 0xBE20B0;
		inline constexpr UInt32 kBSShaderSetupGeometryRenderStates = 0xBE20E0;
		inline constexpr UInt32 kTileShaderPostGeometry = 0xBCAC60;
		inline constexpr UInt32 kNiD3DShaderPrepareGeometryForRendering =
			0xE812F0;
		inline constexpr UInt32 kNiD3DShaderFirstPass = 0xE80580;
		inline constexpr UInt32 kNiDX9ShaderDeclarationCreate = 0xE76700;
		inline constexpr UInt32 kNiD3DTextureStageSetProperties = 0xBE0CF0;
		inline constexpr UInt32 kNiD3DTextureStageSetFilterMode = 0xE7DEF0;
		inline constexpr UInt32 kNiD3DPassSetRenderState = 0xB71A10;
		inline constexpr UInt32 kCopiedTileShaderVtableEntries = 84;
		inline constexpr UInt32 kSetupGeometryConstantsVtableSlot = 31;
		inline constexpr UInt32 kSetupGeometryAlphaBlendingVtableSlot = 32;
		inline constexpr UInt32 kNativeVtableMagic = 0x35544D4E; // "NMT5"
		inline constexpr UInt32 kShaderRefreshMessage = 0;
		inline constexpr DWORD kInitializationRetryMilliseconds = 1000;
		inline constexpr UInt32 kVanillaLayoutVertexStride =
			sizeof(NativeFontVanillaLayoutVertex);
		static_assert(kVanillaLayoutVertexStride == 40u);
		inline constexpr UInt32 kVanillaParametricVertexStride =
			sizeof(NativeFontVanillaParametricVertex);
		static_assert(kVanillaParametricVertexStride == 48u);
		inline constexpr UInt8 kStaticCompositeLayerMaskFirst = 8;
		inline constexpr size_t kStaticCompositeLayerMaskCount = 8;
		inline constexpr size_t kStaticCompositeShiftCount = 2;
		using CreateVertexShaderFn = NiD3DVertexShader* (__cdecl*)(const char*);
		using CreatePixelShaderFn = NiD3DPixelShader* (__cdecl*)(const char*);
		using VanillaSetupGeometryConstantsFn = void(__thiscall*)(TileShader*,
			const NiPropertyState*);
		void __fastcall NativeSetupGeometryConstants(TileShader*, void*,
			const NiPropertyState*);
		void __fastcall NativeSetupGeometryAlphaBlending(
			TileShader*, void*, const NiPropertyState*);

		struct NativeShaderGeneration;
		struct NativeShaderProfile;

		constexpr UInt8 NativePacketRegisterCount(
			NativeFontShaderClass shaderClass)
		{
			switch (shaderClass)
			{
			case NativeFontShaderClass::Body:
				// LayerColor c176 + AtlasPass c177.
				return 2;
			case NativeFontShaderClass::Effect:
				// LayerColor c176 through EffectFlags c179.
				return 4;
			case NativeFontShaderClass::Composite:
				// ShadowColor c176 through CompositeFlags c183.
				return 8;
			default:
				return 0;
			}
		}

		static_assert(NativePacketRegisterCount(
			NativeFontShaderClass::Composite)
			== kNativeFontPacketConstantRegisterCount);

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


		struct NativeProfileKey
		{
			NativeFontShaderClass shaderClass = NativeFontShaderClass::Body;
			NativeFontSampling sampling = NativeFontSampling::Point;
			EffectQuality quality = EffectQuality::Balanced;
			DistanceFieldMethod distanceFieldMethod = DistanceFieldMethod::Mtsdf;
			std::array<UInt32, kNativeFontPacketConstantFloatCount> constantBits = {};
			UInt8 staticCompositeLayerMask = 0;
			bool compositeShiftedShadow = false;
			bool writeEffectAlpha = false;
			bool usesLiveTileRgb = true;
			NativeFontVanillaLayoutKind vanillaLayoutKind =
				NativeFontVanillaLayoutKind::None;
			UInt32 uniformDistanceParameterScaleBits = 0;
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
					&& vanillaLayoutKind == other.vanillaLayoutKind
					&& uniformDistanceParameterScaleBits
						== other.uniformDistanceParameterScaleBits
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
				mix(static_cast<UInt32>(key.vanillaLayoutKind));
				mix(key.uniformDistanceParameterScaleBits);
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
			VanillaSetupGeometryConstantsFn vanillaSetupGeometryConstants = nullptr;
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
					kNativeFontPacketConstantFloatCount>& packetConstants,
				float uniformSdfSpread,
				float uniformDistanceParameterScale)
				: owner(&generation), key(profileKey), constants(packetConstants),
				  vanillaUniformSdfSpread(uniformSdfSpread),
				  vanillaUniformDistanceParameterScale(
					  uniformDistanceParameterScale),
				  privateRegisterCount(
					  NativePacketRegisterCount(profileKey.shaderClass))
			{
			}

			NativeShaderGeneration* const owner;
			const NativeProfileKey key;
			const std::array<float,
				kNativeFontPacketConstantFloatCount> constants;
			const float vanillaUniformSdfSpread;
			const float vanillaUniformDistanceParameterScale;
			const UInt8 privateRegisterCount;
			NiPointer<TileShader> shaderOwner;
			TileShader* shader = nullptr;
			NativeTileVtableBlock* vtable = nullptr;
			NativeFontCompiledPacketCommand retainedProgram;
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
			NiDX9ShaderDeclarationPtr vanillaParametricLayoutDeclaration;
			IDirect3DVertexDeclaration9*
				vanillaParametricLayoutD3DDeclaration = nullptr;
			std::vector<IDirect3DVertexDeclaration9*>
				compatibleVanillaParametricLayoutD3DDeclarations;
			NiD3DVertexShaderPtr vanillaParametricLayoutVertexShader;
			bool vanillaParametricLayoutReady = false;
			NiD3DPixelShaderPtr coverageShader;
			NiD3DPixelShaderPtr argbShader;
			std::array<NiD3DPixelShaderPtr, 3> distanceFieldFillShaders;
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

		inline UInt32 ResolveVanillaLayoutStride(
			NativeFontVanillaLayoutKind layoutKind)
		{
			switch (layoutKind)
			{
			case NativeFontVanillaLayoutKind::Uniform40:
				return kVanillaLayoutVertexStride;
			case NativeFontVanillaLayoutKind::Parametric48:
				return kVanillaParametricVertexStride;
			default:
				return 0u;
			}
		}

		inline bool IsVanillaLayoutGenerationReady(
			const NativeShaderGeneration* generation,
			NativeFontVanillaLayoutKind layoutKind)
		{
			if (!generation)
				return false;
			return layoutKind == NativeFontVanillaLayoutKind::Uniform40
				? generation->vanillaLayoutReady
				: layoutKind == NativeFontVanillaLayoutKind::Parametric48
					? generation->vanillaParametricLayoutReady : false;
		}

		inline NiDX9ShaderDeclaration* ResolveVanillaLayoutDeclaration(
			NativeShaderGeneration& generation,
			NativeFontVanillaLayoutKind layoutKind)
		{
			return layoutKind == NativeFontVanillaLayoutKind::Uniform40
				? generation.vanillaLayoutDeclaration.m_pObject
				: layoutKind == NativeFontVanillaLayoutKind::Parametric48
					? generation.vanillaParametricLayoutDeclaration.m_pObject
					: nullptr;
		}

		inline IDirect3DVertexDeclaration9* ResolveVanillaLayoutD3DDeclaration(
			NativeShaderGeneration& generation,
			NativeFontVanillaLayoutKind layoutKind)
		{
			return layoutKind == NativeFontVanillaLayoutKind::Uniform40
				? generation.vanillaLayoutD3DDeclaration
				: layoutKind == NativeFontVanillaLayoutKind::Parametric48
					? generation.vanillaParametricLayoutD3DDeclaration
					: nullptr;
		}

		inline NiD3DVertexShader* ResolveVanillaLayoutVertexShader(
			NativeShaderGeneration& generation,
			NativeFontVanillaLayoutKind layoutKind)
		{
			return layoutKind == NativeFontVanillaLayoutKind::Uniform40
				? generation.vanillaLayoutVertexShader.m_pObject
				: layoutKind == NativeFontVanillaLayoutKind::Parametric48
					? generation.vanillaParametricLayoutVertexShader.m_pObject
					: nullptr;
		}


		struct NativeFacadeShaderBatch
		{
			NativeShaderProfile* packetProfile = nullptr;
			UInt8 packetRegisterCount = 0;
			UInt32 depth = 0;
			NativeVertexAaState vertexAa;
			bool samplerReady = false;
		};

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

	struct NativeShaderRuntimeState
	{
		std::atomic<NativeShaderGeneration*> publishedGeneration{ nullptr };
		std::mutex initializationMutex;
		std::vector<NativeShaderGeneration*> processGenerations;
		UInt32 nextGeneration = 1;
		std::atomic<UInt32> deviceEpoch{ 1 };
		DWORD lastInitializationAttempt = 0;
		std::atomic<bool> invalidVtableLogged{ false };
		std::atomic<bool> resetInProgress{ false };
		std::atomic<UInt32> standardV2ProofLogGeneration{ 0 };
		NiDX9Renderer* resetRenderer = nullptr;
	};

	struct NativeShaderThreadState
	{
		NativeSortedShaderBatch sortedShaderBatch;
		NativeFacadeShaderBatch facadeShaderBatch;
		NativeVanillaLayoutPublicationWitness vanillaLayoutPublicationWitness;
		UInt64 nextVanillaLayoutPublicationToken = 0;
	};

	NativeShaderRuntimeState& ShaderState();
	NativeShaderThreadState& ShaderThread();

	NativeFontStandardBlendSemantics ClassifyStandardBlendCallback(void* callback);
	void ResetSortedShaderStateCaches();
	const char* StandardBlendSemanticsName(
		NativeFontStandardBlendSemantics semantics);
	bool HasShaderHandle(const NiD3DVertexShaderPtr& shader);
	bool HasShaderHandle(const NiD3DPixelShaderPtr& shader);
	bool GenerationResourcesReady(const NativeShaderGeneration* generation);
	bool GenerationMatchesCurrentDevice(
		const NativeShaderGeneration* generation);
	void MarkGenerationFault(NativeShaderGeneration* generation,
		const char* operation, HRESULT result);
	NativeTileVtableBlock* RecoverNativeVtableBlock(TileShader* shader);
	bool HasExactVanillaLayoutConstantCarryChain(TileShader* shader);
	bool ResolveVanillaTilePixelConstant(const NiPropertyState* properties,
		float* output);
	NiD3DRenderState* ResolveEngineRenderState(IDirect3DDevice9* device);
	void ResetSortedShaderIdentity(NativeSortedShaderBatch& batch,
		IDirect3DDevice9* device, UInt32 generation);
	NiD3DRenderState* ResolveSortedRenderState(IDirect3DDevice9* device);
	bool IsNativeMipFilterReady(const NiD3DRenderState* renderState);
	bool ResolveEngineViewport(IDirect3DDevice9* device,
		D3DVIEWPORT9& viewport);
	HRESULT EnsureNativeSamplerState(IDirect3DDevice9* device,
		bool& changed, const char*& operation);
	HRESULT PublishNativeVertexAaConstant(IDirect3DDevice9* device,
		float rasterScale, NativeVertexAaState* cache,
		bool forcePublish, bool* published, const char*& operation);
	HRESULT PublishNativeVanillaLayoutVertexConstants(
		IDirect3DDevice9* device, float rasterScale, float spread,
		float distanceParameterScale, UInt8 layerMask,
		NativeFontVanillaLayoutKind layoutKind,
		NativeVertexAaState* cache, const char*& operation);

	NativeProfileKey MakeProfileKey(const NativeFontPacketTemplate& packet,
		NativeFontSampling sampling, bool writeEffectAlpha,
		NativeFontVanillaLayoutKind vanillaLayoutKind);
	NativeFontSampling ResolveEffectiveSampling(
		const NativeFontPacketTemplate& packet, bool scaledFillSampling);
	NativeShaderProfile* CreateProfile(NativeShaderGeneration& generation,
		const NativeFontPacketTemplate& packet, const NativeProfileKey& key);
	NativeShaderGeneration* BuildGeneration(CreateVertexShaderFn createVS,
		CreatePixelShaderFn createPS, NiDX9Renderer* renderer,
		IDirect3DDevice9* device, const char*& failure);
	bool ResolveShaderLoader(CreateVertexShaderFn& createVS,
		CreatePixelShaderFn& createPS, const char*& failure);
	bool NativeRendererResetCallback(bool beforeReset, void*);
}

#pragma once

// Shared native-font shape model and sibling-module services.

#include "font_vector_internal.h"
#include "font_native_internal.h"

#include "NiDX9Renderer.hpp"
#include "NiTriShape.hpp"
#include "BSShaderProperty.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <Windows.h>
#include <d3d9.h>

namespace fonthook::vectorfont
{
	static_assert(sizeof(void*) == 4,
		"FreeType native-font rendering requires the Win32 runtime");
	static_assert(sizeof(BSShaderProperty::RenderPass) == 0x10,
		"Tile RenderPass ABI changed");

	inline constexpr UInt32 kDeleteThisSlot = 1;
	inline constexpr UInt32 kRenderImmediateSlot = 55;
	inline constexpr UInt32 kRenderImmediateAltSlot = 56;
	inline constexpr UInt32 kCopiedTriShapeVtableEntries = 61;
	inline constexpr UInt32 kShaderRefreshMessage = 0;
	// BSShaderAccumulator::RenderAlphaGeometry calls the static
	// BSBatchRenderer::RenderPassImmediately dispatcher here. The symbolized
	// Aug 22 test build supplies both names; retail 1.4.0.525 has the same call
	// graph at B65EA0 -> B64F90 and B64FD1 -> B994F0.
	inline constexpr UInt32 kRenderPassImmediatelyCallSite = 0xB64FD1;
	inline constexpr UInt32 kBSBatchRendererRenderPassImmediately = 0xB994F0;
	inline constexpr UInt32 kRenderAlphaGeometryCallSite = 0xB65EA0;
	inline constexpr UInt32 kBSShaderAccumulatorRenderAlphaGeometry = 0xB64F90;
	inline constexpr UInt32 kMaximumShapeValidationFailureLogs = 16;
	// Retail 1.4.0.525 and the symbolized Aug 22 beta agree that
	// NiGeometryBufferData owns a two-slot RendererData vtable. Use the
	// non-deleting destructor explicitly for placement-constructed descriptors;
	// slot 1 is ContainsVertexData, not DeleteThis.
	inline constexpr UInt32 kNiGeometryBufferDataConstructor = 0xE947C0;
	inline constexpr UInt32 kNiGeometryBufferDataDestructor = 0xE8F0F0;
	// Metadata generation slots are deliberately much larger than the live TLS
	// hot-cache set count. Shape allocators commonly return addresses with
	// repeating low bits; a small modulo table lets unrelated menu facades
	// continuously invalidate one another even when their hot entries do not
	// collide.
	inline constexpr size_t kMetadataGenerationSlotCount = 16384;
	static_assert((kMetadataGenerationSlotCount
			& (kMetadataGenerationSlotCount - 1)) == 0,
		"metadata generation slot count must remain a power of two");

	inline UInt32 HashMetadataShapeAddress(const NiTriShape* shape)
	{
		UInt32 value = static_cast<UInt32>(
			reinterpret_cast<uintptr_t>(shape) >> 4);
		value ^= value >> 16;
		value *= 0x7FEB352Du;
		value ^= value >> 15;
		value *= 0x846CA68Bu;
		value ^= value >> 16;
		return value;
	}

	inline size_t GetMetadataGenerationSlot(const NiTriShape* shape)
	{
		return HashMetadataShapeAddress(shape)
			& (kMetadataGenerationSlotCount - 1);
	}

	using RenderImmediateFn = void(__thiscall*)(NiTriShape*, NiRenderer*);
	using DeleteThisFn = void(__thiscall*)(NiTriShape*);
	using RenderPassImmediatelyFn = void(__cdecl*)(BSShaderProperty::RenderPass*,
		UInt32, bool, bool, bool);
	using RenderAlphaGeometryFn = void(__thiscall*)(BSShaderAccumulator*);

	enum class NativeFontCompiledShaderClass : UInt8
	{
		Body,
		Effect,
		Coverage,
		Argb
	};

	enum class FreeTypeShapeBackend : UInt8
	{
		CompatibilityFacade = 0,
		SingletonFacade,
		VanillaLayout
	};

	enum class SingletonFacadeFrameMode : UInt8
	{
		Facade = 0,
		Direct,
		Culled,
		Fault,
		Retired
	};

	struct NativeFontCompiledRange
	{
		NativeFontDrawRange drawRange;
		std::array<float, kNativeFontPacketConstantFloatCount> constants = {};
		NativeFontCompiledShaderClass shaderClass = NativeFontCompiledShaderClass::Body;
		bool staticSmoothSampling = false;
	};

	struct NativeFontShapeMetadata
	{
		UInt64 allocationId = 0;
		const NativeFontShapeMetadata* selfIdentity = nullptr;
		const NiTriShape* shapeIdentity = nullptr;
		UInt32 fontId = 0;
		UInt32 glyphCount = 0;
		UInt32 quadCount = 0;
		UInt32 vertexCount = 0;
		UInt32 primitiveCount = 0;
		UInt32 indexCount = 0;
		NativeFontShapeColorContract colorContract;
		mutable CpuMemoryLease cpuMemory;
		mutable NativeFontShapePayload nativePayload;
		FreeTypeShapeBackend backend =
			FreeTypeShapeBackend::CompatibilityFacade;
	};
	using NativeFontShapeMetadataPtr = std::shared_ptr<const NativeFontShapeMetadata>;

	// Keep a publication-time identity copy outside the owned object. A damaged
	// shared_ptr may no longer point at readable NativeFontShapeMetadata, so deletion
	// auditing must be able to compare its raw object pointer without first
	// dereferencing it.
	struct NativeFontShapeMetadataEntry
	{
		NativeFontShapeMetadataPtr metadata;
		UInt64 allocationId = 0;
		const NativeFontShapeMetadata* selfIdentity = nullptr;
		const NiTriShape* shapeIdentity = nullptr;
	};

	struct SingletonFacadeBinding
	{
		NiTriShape* shape = nullptr;
		NiGeometryBufferData* shellBuffer = nullptr;
		NiGeometryBufferData* bindingBuffer = nullptr;
		NiVBChip* bindingChip = nullptr;
		UInt32* bindingStride = nullptr;
		void* bindingChipMemory = nullptr;
		BSShader* shellShader = nullptr;
		UInt32 packetIndex = 0;
		UInt32 generation = 0;
		UInt32 resourceSerial = 0;
		UInt32 uploadEpoch = 0;
		UInt32 atlasTextureEpoch = 0;
		UInt32 baseVertex = 0;
		UInt32 vertexCount = 0;
		bool staticResident = false;
		bool bound = false;
	};

	// Every native text artifact owns one vanilla-visible facade. The embedded
	// binding is activated only when the current preflight topology contains one
	// packet; multi-packet topologies keep the shell and use the facade span.
	struct SingletonFacadeState
	{
		SingletonFacadeBinding slot;
		UInt64 topologyValidationToken = 0;
		UInt64 preflightValidationToken = 0;
		UInt64 preparedValidationToken = 0;
		UInt32 preparedGeneration = 0;
		UInt32 preparedAtlasTextureEpoch = 0;
		std::atomic<UInt32> directDrawCount = 0;
		NativeFontDrawCommand commandBuildCommand;
		std::atomic<UInt64> commandBuildValidationToken = 0;
		std::atomic<UInt64> commandValidationToken = 0;
		std::atomic<UInt32> commandDirectFacadeSinglePacketIndex =
			kInvalidNativeFontCommandIndex;
		std::atomic<SingletonFacadeFrameMode> frameMode =
			SingletonFacadeFrameMode::Facade;
	};

	struct SingletonFacadeMetadata final : NativeFontShapeMetadata
	{
		mutable SingletonFacadeState singleton;
	};

	struct VanillaLayoutMetadata final : NativeFontShapeMetadata
	{
		NativeFontVanillaLayoutKind layoutKind =
			NativeFontVanillaLayoutKind::None;
		mutable NativeFontVanillaLayoutDrawToken drawToken;
	};

	inline SingletonFacadeState* GetSingletonFacadeState(
		const NativeFontShapeMetadata& metadata)
	{
		return metadata.backend == FreeTypeShapeBackend::SingletonFacade
			? &static_cast<const SingletonFacadeMetadata&>(metadata).singleton
			: nullptr;
	}

	inline NativeFontVanillaLayoutDrawToken* GetVanillaLayoutDrawToken(
		const NativeFontShapeMetadata& metadata)
	{
		return metadata.backend == FreeTypeShapeBackend::VanillaLayout
			? &static_cast<const VanillaLayoutMetadata&>(metadata).drawToken
			: nullptr;
	}

	inline NativeFontVanillaLayoutKind GetVanillaLayoutKind(
		const NativeFontShapeMetadata& metadata)
	{
		return metadata.backend == FreeTypeShapeBackend::VanillaLayout
			? static_cast<const VanillaLayoutMetadata&>(metadata).layoutKind
			: NativeFontVanillaLayoutKind::None;
	}

	struct NativeFontShapeState
	{
		std::array<void*, kCopiedTriShapeVtableEntries + 1> triShapeVtable = {};
		std::array<void*, kCopiedTriShapeVtableEntries + 1>
			vanillaLayoutTriShapeVtable = {};
		void** originalTriShapeVtable = nullptr;
		RenderImmediateFn originalRenderImmediate = nullptr;
		RenderImmediateFn originalRenderImmediateAlt = nullptr;
		DeleteThisFn originalDeleteThis = nullptr;
		RenderPassImmediatelyFn predecessorRenderPassImmediately = nullptr;
		RenderAlphaGeometryFn predecessorRenderAlphaGeometry = nullptr;
		bool renderPassImmediatelyHookInstalled = false;
		bool renderAlphaGeometryHookInstalled = false;
		bool loggedRenderPassImmediatelyHookConflict = false;
		bool loggedRenderAlphaGeometryHookConflict = false;
		bool loggedRenderPassImmediatelyHit = false;
		bool standardPassLitePredicatesValidated = false;
		UInt32 shapeValidationFailureLogCount = 0;

		std::mutex metadataMutex;
		std::unordered_map<const NiTriShape*, NativeFontShapeMetadataEntry> shapeMetadata;
		std::atomic<UInt64> nextMetadataAllocationId = 1;
		std::array<std::atomic<UInt64>, kMetadataGenerationSlotCount>
			metadataGenerations = {};

	};

	NativeFontShapeState& State();
	struct NativeFontRuntimeReadinessView
	{
		NiDX9Renderer* renderer = nullptr;
		IDirect3DDevice9* device = nullptr;
		UInt32 generation = 0;
		UInt32 atlasTextureEpoch = 0;
		UInt32 hookEpoch = 0;
		bool ready = false;
	};
	bool GetNativeFontRuntimeReadinessCurrent(
		NativeFontRuntimeReadinessView& arView);
	NativeFontShapeMetadataPtr FindNativeFontShapeMetadata(const NiTriShape* shape);
	// Resolve a caller-supplied unique shape set under one metadataMutex hold.
	// The result stays positional; missing or identity-invalid entries are null.
	// Holding the returned owners through RenderAlphaGeometry keeps every raw
	// metadata view used by the sorted frame alive without a per-facade lookup.
	void AcquireNativeFontShapeMetadataBatch(
		const std::vector<NiTriShape*>& shapes,
		std::vector<NativeFontShapeMetadataPtr>& owners);
	bool IsNativeFontAtlasShape(const NiTriShape* shape);
	bool IsVanillaLayoutShape(const NiTriShape* shape);
	bool NeedsScaledFillSampling(const NiTriShape* shape);
	bool HookRenderPassImmediately();
	bool IsNativeFontRenderPassImmediatelyHookCurrent();
	bool IsNativeFontRenderPassImmediatelyHookCurrentUnchecked();
	bool IsNativeFontRenderPassImmediatelyHookCurrentFast();
	RenderPassImmediatelyFn ReadRenderPassImmediatelyCallTarget();
	void BeginNativeFontSortedTileConstantOwnership();
	void EndNativeFontSortedTileConstantOwnership();
	void __cdecl NativeFontRenderPassImmediately(BSShaderProperty::RenderPass* pass,
		UInt32 currentPass, bool testAlpha, bool blendAlpha, bool setupRenderStates);
	void __fastcall NativeFontRenderImmediate(NiTriShape* shape, void*, NiRenderer* renderer);
	void __fastcall NativeFontRenderImmediateAlt(NiTriShape* shape, void*, NiRenderer* renderer);
	bool InitializeNativeFontTriShapeVtable(NiTriShape* shape);
	bool PrepareNativeFontSingletonFacadeShape(Font& font, NiTriShape* shape,
		UInt32 fontId, UInt32 glyphCount, UInt32 quadCount,
		const NativeFontEffectShapeConfig* effectConfig,
		const NativeFontShapeColorContract* colorContract,
		NativeFontPayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin);
	bool PrepareNativeFontVanillaLayoutShape(Font& font, NiTriShape* shape,
		UInt32 fontId, UInt32 glyphCount, UInt32 quadCount,
		NativeFontVanillaLayoutKind layoutKind,
		const NativeFontEffectShapeConfig* effectConfig,
		const NativeFontShapeColorContract* colorContract,
		NativeFontPayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin);
	bool PrepareSingletonFacadeForSortedFrame(
		const NativeFontShapeMetadata& metadata, UInt32 generation,
		UInt32 atlasTextureEpoch, UInt64 validationToken);
	void RestoreSingletonFacade(
		const NativeFontShapeMetadata& metadata, NativeFontFallbackReason reason);
	void InvalidateAllSingletonFacadeBindings();
	void ReleaseSingletonFacadeBinding(
		NiTriShape* shape, const NativeFontShapeMetadata& metadata);
}

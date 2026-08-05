#pragma once

// Private native A8 shape model and sibling-module services.

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
		"FreeType A8 rendering requires the Win32 runtime");
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
	inline constexpr UInt32 kStockRenderPassImmediately = 0xB994F0;
	inline constexpr UInt32 kRenderAlphaGeometryCallSite = 0xB65EA0;
	inline constexpr UInt32 kStockRenderAlphaGeometry = 0xB64F90;
	inline constexpr UInt32 kMaximumShapeValidationFailureLogs = 16;
	// Retail 1.4.0.525 and the symbolized Aug 22 beta agree that
	// NiGeometryBufferData owns a two-slot RendererData vtable. Use the
	// non-deleting destructor explicitly for placement-constructed descriptors;
	// slot 1 is ContainsVertexData, not DeleteThis.
	inline constexpr UInt32 kGeometryBufferDataConstructor = 0xE947C0;
	inline constexpr UInt32 kGeometryBufferDataDestructor = 0xE8F0F0;
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

	enum class A8CompiledShaderClass : UInt8
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
		StockLayoutSdf
	};

	enum class SingletonFacadeFrameMode : UInt8
	{
		Facade = 0,
		Direct,
		Culled,
		Fault,
		Retired
	};

	struct A8CompiledRange
	{
		A8DrawRange range;
		std::array<float, kNativeA8PacketConstantFloatCount> constants = {};
		A8CompiledShaderClass shaderClass = A8CompiledShaderClass::Body;
		bool staticSmoothSampling = false;
	};

	struct A8ShapeMetadata
	{
		UInt64 allocationId = 0;
		const A8ShapeMetadata* selfIdentity = nullptr;
		const NiTriShape* shapeIdentity = nullptr;
		UInt32 fontId = 0;
		UInt32 glyphCount = 0;
		UInt32 quadCount = 0;
		UInt32 vertexCount = 0;
		UInt32 primitiveCount = 0;
		UInt32 indexCount = 0;
		A8ShapeColorContract colorContract;
		mutable CpuMemoryLease cpuMemory;
		mutable NativeA8ShapePayload nativePayload;
		FreeTypeShapeBackend backend =
			FreeTypeShapeBackend::CompatibilityFacade;
	};
	using A8ShapeMetadataPtr = std::shared_ptr<const A8ShapeMetadata>;

	// Keep a publication-time identity copy outside the owned object. A damaged
	// shared_ptr may no longer point at readable A8ShapeMetadata, so deletion
	// auditing must be able to compare its raw object pointer without first
	// dereferencing it.
	struct A8ShapeMetadataEntry
	{
		A8ShapeMetadataPtr metadata;
		UInt64 allocationId = 0;
		const A8ShapeMetadata* selfIdentity = nullptr;
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

	// Every native text artifact owns one stock-visible facade. The embedded
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
		NativeA8DrawCommand commandBuildCommand;
		std::atomic<UInt64> commandBuildValidationToken = 0;
		std::atomic<UInt64> commandValidationToken = 0;
		std::atomic<UInt32> commandDirectFacadeSinglePacketIndex =
			kInvalidNativeA8CommandIndex;
		std::atomic<SingletonFacadeFrameMode> frameMode =
			SingletonFacadeFrameMode::Facade;
	};

	struct SingletonFacadeMetadata final : A8ShapeMetadata
	{
		mutable SingletonFacadeState singleton;
	};

	inline SingletonFacadeState* GetSingletonFacadeState(
		const A8ShapeMetadata& metadata)
	{
		return metadata.backend == FreeTypeShapeBackend::SingletonFacade
			? &static_cast<const SingletonFacadeMetadata&>(metadata).singleton
			: nullptr;
	}

	struct A8State
	{
		std::array<void*, kCopiedTriShapeVtableEntries + 1> triShapeVtable = {};
		std::array<void*, kCopiedTriShapeVtableEntries + 1>
			stockLayoutTriShapeVtable = {};
		void** originalTriShapeVtable = nullptr;
		RenderImmediateFn originalRenderImmediate = nullptr;
		RenderImmediateFn originalRenderImmediateAlt = nullptr;
		DeleteThisFn originalDeleteThis = nullptr;
		RenderPassImmediatelyFn originalRenderPassImmediately = nullptr;
		RenderAlphaGeometryFn originalRenderAlphaGeometry = nullptr;
		bool renderPassImmediatelyHookInstalled = false;
		bool renderAlphaGeometryHookInstalled = false;
		bool loggedRenderPassImmediatelyHookConflict = false;
		bool loggedRenderAlphaGeometryHookConflict = false;
		bool loggedRenderPassImmediatelyHit = false;
		bool standardPassLitePredicatesValidated = false;
		UInt32 shapeValidationFailureLogCount = 0;

		std::mutex metadataMutex;
		std::unordered_map<const NiTriShape*, A8ShapeMetadataEntry> shapeMetadata;
		std::atomic<UInt64> nextMetadataAllocationId = 1;
		std::array<std::atomic<UInt64>, kMetadataGenerationSlotCount>
			metadataGenerations = {};

	};

	A8State& State();
	struct NativeA8RuntimeReadinessView
	{
		NiDX9Renderer* renderer = nullptr;
		IDirect3DDevice9* device = nullptr;
		UInt32 generation = 0;
		UInt32 atlasTextureEpoch = 0;
		UInt32 hookEpoch = 0;
		bool ready = false;
	};
	bool GetNativeA8RuntimeReadinessCurrent(
		NativeA8RuntimeReadinessView& arView);
	A8ShapeMetadataPtr FindA8ShapeMetadata(const NiTriShape* shape);
	// Resolve a caller-supplied unique shape set under one metadataMutex hold.
	// The result stays positional; missing or identity-invalid entries are null.
	// Holding the returned owners through RenderAlphaGeometry keeps every raw
	// metadata view used by the sorted frame alive without a per-facade lookup.
	void AcquireA8ShapeMetadataBatch(
		const std::vector<NiTriShape*>& shapes,
		std::vector<A8ShapeMetadataPtr>& owners);
	bool IsA8AtlasShape(const NiTriShape* shape);
	bool IsStockLayoutSdfShape(const NiTriShape* shape);
	bool NeedsScaledFillSampling(const NiTriShape* shape);
	bool HookRenderPassImmediately();
	bool IsA8RenderPassImmediatelyHookCurrent();
	bool IsA8RenderPassImmediatelyHookCurrentUnchecked();
	bool IsA8RenderPassImmediatelyHookCurrentFast();
	RenderPassImmediatelyFn ReadRenderPassImmediatelyCallTarget();
	void BeginA8SortedTileConstantOwnership();
	void EndA8SortedTileConstantOwnership();
	void __cdecl A8RenderPassImmediately(BSShaderProperty::RenderPass* pass,
		UInt32 currentPass, bool testAlpha, bool blendAlpha, bool setupDrawmode);
	void __fastcall A8RenderImmediate(NiTriShape* shape, void*, NiRenderer* renderer);
	void __fastcall A8RenderImmediateAlt(NiTriShape* shape, void*, NiRenderer* renderer);
	bool InitializeA8TriShapeVtable(NiTriShape* shape);
	bool PrepareSingletonFacadeA8Shape(Font& font, NiTriShape* shape,
		UInt32 fontId, UInt32 glyphCount, UInt32 quadCount,
		const A8EffectShapeConfig* effectConfig,
		const A8ShapeColorContract* colorContract,
		NativeA8PayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin);
	bool PrepareStockLayoutSdfA8Shape(Font& font, NiTriShape* shape,
		UInt32 fontId, UInt32 glyphCount, UInt32 quadCount,
		const A8EffectShapeConfig* effectConfig,
		const A8ShapeColorContract* colorContract,
		NativeA8PayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin);
	bool PrepareSingletonFacadeForSortedFrame(
		const A8ShapeMetadata& metadata, UInt32 generation,
		UInt32 atlasTextureEpoch, UInt64 validationToken);
	void RestoreSingletonFacade(
		const A8ShapeMetadata& metadata, NativeA8FallbackReason reason);
	void InvalidateAllSingletonFacadeBindings();
	void ReleaseSingletonFacadeBinding(
		NiTriShape* shape, const A8ShapeMetadata& metadata);
}

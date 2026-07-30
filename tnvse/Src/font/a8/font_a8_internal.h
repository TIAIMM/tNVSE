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
	inline constexpr UInt32 kTileRenderPassCallSite = 0xB64FD1;
	inline constexpr UInt32 kStockTileRenderPassImmediately = 0xB994F0;
	inline constexpr UInt32 kSortedTileRenderCallSite = 0xB65EA0;
	inline constexpr UInt32 kStockSortedTileRender = 0xB64F90;
	inline constexpr UInt32 kMaximumShapeValidationFailureLogs = 16;
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
	using TileRenderPassFn = void(__cdecl*)(BSShaderProperty::RenderPass*,
		UInt32, bool, bool, bool);
	using SortedTileRenderFn = int(__thiscall*)(BSShaderAccumulator*);

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
		VirtualStockNative,
		StockArgb
	};

	enum class VirtualStockFrameMode : UInt8
	{
		Facade = 0,
		Direct,
		Culled,
		Fault,
		Retired
	};

	struct VirtualStockShapeGroup;

	struct A8CompiledRange
	{
		A8DrawRange range;
		std::array<float, kNativeA8PacketConstantFloatCount> constants = {};
		A8CompiledShaderClass shaderClass = A8CompiledShaderClass::Body;
		bool staticSmoothSampling = false;
	};

	struct A8ShapeMetadata
	{
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
		VirtualStockShapeGroup* virtualStockGroup = nullptr;
		std::weak_ptr<VirtualStockShapeGroup> virtualStockOwner;
		UInt16 virtualStockSlot = 0;
		bool virtualStockPrimary = false;
	};
	using A8ShapeMetadataPtr = std::shared_ptr<const A8ShapeMetadata>;

	struct VirtualStockSlotBinding
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

	struct VirtualStockShapeGroup
	{
		std::mutex mutex;
		NativeA8PayloadTemplatePtr payloadTemplate;
		A8ShapeMetadataPtr primaryMetadataOwner;
		NiTriShape* primaryShape = nullptr;
		std::vector<VirtualStockSlotBinding> slots;
		CpuMemoryLease cpuMemory;
		BSShaderAccumulator* registrationAccumulator = nullptr;
		std::vector<SInt32> registrationItemIndices;
		UInt64 registrationCycle = 0;
		UInt64 preflightValidationToken = 0;
		UInt64 preparedValidationToken = 0;
		UInt32 preparedGeneration = 0;
		UInt32 preparedAtlasTextureEpoch = 0;
		UInt32 primarySlot = 0;
		UInt32 registeredSlotCount = 0;
		UInt32 liveSlotCount = 0;
		bool useCompositeTopology = false;
		bool registrationContiguous = true;
		bool duplicateRegistration = false;
		std::atomic<UInt32> directDrawCount = 0;
		std::atomic<UInt64> commandValidationToken = 0;
		std::atomic<UInt32> commandSpanIndex =
			kInvalidNativeA8CommandIndex;
		std::atomic<UInt32> commandLeaderSlot = 0;
		std::atomic<bool> metadataPublished = false;
		std::atomic<VirtualStockFrameMode> frameMode =
			VirtualStockFrameMode::Facade;
	};

	struct A8State
	{
		std::array<void*, kCopiedTriShapeVtableEntries + 1> triShapeVtable = {};
		void** originalTriShapeVtable = nullptr;
		RenderImmediateFn originalRenderImmediate = nullptr;
		RenderImmediateFn originalRenderImmediateAlt = nullptr;
		DeleteThisFn originalDeleteThis = nullptr;
		TileRenderPassFn originalTileRenderPass = nullptr;
		SortedTileRenderFn originalSortedTileRender = nullptr;
		bool tileRenderPassHookInstalled = false;
		bool sortedTileRenderHookInstalled = false;
		bool loggedTileRenderPassHookConflict = false;
		bool loggedSortedTileRenderHookConflict = false;
		bool loggedTileRenderPassHit = false;
		bool b98e80LitePredicatesValidated = false;
		UInt32 shapeValidationFailureLogCount = 0;

		std::mutex metadataMutex;
		std::unordered_map<const NiTriShape*, A8ShapeMetadataPtr> shapeMetadata;
		std::unordered_map<VirtualStockShapeGroup*,
			std::shared_ptr<VirtualStockShapeGroup>> virtualStockGroups;
		std::array<std::atomic<UInt64>, kMetadataGenerationSlotCount>
			metadataGenerations = {};

	};

	A8State& State();
	A8ShapeMetadataPtr FindA8ShapeMetadata(const NiTriShape* shape);
	std::shared_ptr<VirtualStockShapeGroup>
		AcquireVirtualStockShapeGroup(const A8ShapeMetadata& metadata);
	bool IsA8AtlasShape(const NiTriShape* shape);
	bool NeedsScaledFillSampling(const NiTriShape* shape);
	bool HookTileRenderPass();
	bool IsA8TileRenderPassHookCurrent();
	TileRenderPassFn ReadTileRenderPassCallTarget();
	void BeginA8SortedTileConstantBatch();
	void EndA8SortedTileConstantBatch();
	void __cdecl A8TileRenderPass(BSShaderProperty::RenderPass* pass,
		UInt32 currentPass, bool testAlpha, bool blendAlpha, bool setupDrawmode);
	void __fastcall A8RenderImmediate(NiTriShape* shape, void*, NiRenderer* renderer);
	void __fastcall A8RenderImmediateAlt(NiTriShape* shape, void*, NiRenderer* renderer);
	bool InitializeA8TriShapeVtable(NiTriShape* shape);
	bool PrepareVirtualStockA8ShapeGroup(Font& font,
		const std::vector<NiTriShape*>& shapes, UInt32 primarySlot,
		UInt32 fontId, UInt32 glyphCount, UInt32 quadCount,
		const A8EffectShapeConfig* effectConfig,
		const A8ShapeColorContract* colorContract,
		NativeA8PayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin, bool useCompositeTopology);
	bool PrepareVirtualStockGroupForSortedFrame(
		const std::shared_ptr<VirtualStockShapeGroup>& group,
		UInt32 generation, UInt32 atlasTextureEpoch,
		UInt64 validationToken);
	void RestoreVirtualStockGroupToFacade(
		const std::shared_ptr<VirtualStockShapeGroup>& group,
		NativeA8FallbackReason reason);
	void InvalidateAllVirtualStockBindings();
	void ReleaseVirtualStockShapeBinding(
		NiTriShape* shape, const A8ShapeMetadata& metadata);
}

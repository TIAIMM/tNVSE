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
	inline constexpr size_t kMetadataGenerationSlotCount = 64;

	inline size_t GetMetadataGenerationSlot(const NiTriShape* shape)
	{
		return (reinterpret_cast<uintptr_t>(shape) >> 4)
			% kMetadataGenerationSlotCount;
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
		Coverage
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
		UInt32 fontId = 0;
		UInt32 glyphCount = 0;
		UInt32 quadCount = 0;
		UInt32 vertexCount = 0;
		UInt32 primitiveCount = 0;
		UInt32 indexCount = 0;
		A8ShapeColorContract colorContract;
		mutable CpuMemoryLease cpuMemory;
		mutable NativeA8ShapePayload nativePayload;
	};
	using A8ShapeMetadataPtr = std::shared_ptr<const A8ShapeMetadata>;

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
		UInt32 shapeValidationFailureLogCount = 0;

		std::mutex metadataMutex;
		std::unordered_map<const NiTriShape*, A8ShapeMetadataPtr> shapeMetadata;
		std::array<std::atomic<UInt64>, kMetadataGenerationSlotCount>
			metadataGenerations = {};

	};

	A8State& State();
	A8ShapeMetadataPtr FindA8ShapeMetadata(const NiTriShape* shape);
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
}

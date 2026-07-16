#pragma once

// Private A8 bridge model and sibling-module services.

#include "font_vector_internal.h"

#include "NiD3DPixelShader.hpp"
#include "NiDX9Renderer.hpp"
#include "NiTriShape.hpp"
#include "BSShaderProperty.hpp"

#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <Windows.h>
#include <d3d9.h>

namespace fonthook::vectorfont
{
	static_assert(sizeof(void*) == 4,
		"FreeType A8 rendering requires the Win32 runtime");
	static_assert(sizeof(BSShaderProperty::RenderPass) == 0x10,
		"Tile RenderPass ABI changed");

	inline constexpr UInt32 kDrawIndexedPrimitiveSlot = 82;
	inline constexpr UInt32 kDeleteThisSlot = 1;
	inline constexpr UInt32 kRenderImmediateSlot = 55;
	inline constexpr UInt32 kRenderImmediateAltSlot = 56;
	inline constexpr UInt32 kRendererBeginBatchSlot = 103;
	inline constexpr UInt32 kRendererEndBatchSlot = 104;
	inline constexpr UInt32 kRendererBatchRenderShapeSlot = 105;
	inline constexpr UInt32 kRendererRenderShapeSlot = 107;
	inline constexpr UInt32 kRendererRenderShapeAltSlot = 109;
	inline constexpr UInt32 kCopiedTriShapeVtableEntries = 61;
	inline constexpr UInt32 kShaderRefreshMessage = 0;
	inline constexpr UInt32 kTileRenderPassCallSite = 0xB64FD1;
	inline constexpr UInt32 kStockTileRenderPassImmediately = 0xB994F0;
	inline constexpr UInt32 kMaximumStateMismatchLogs = 16;
	inline constexpr UInt32 kMaximumRangeDrawFailureLogs = 16;
	inline constexpr UInt32 kMaximumShapeValidationFailureLogs = 16;
	inline constexpr UInt32 kMaximumShadowTraceShapes = 256;
	inline constexpr UInt32 kMaximumRenderTraceDepth = 8;
	inline constexpr UInt32 kMaximumDiagnosticShapes = 128;

	using CreatePixelShaderFn = NiD3DPixelShader* (__cdecl*)(const char*);
	using DrawIndexedPrimitiveFn = HRESULT(__stdcall*)(IDirect3DDevice9*,
		D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
	using BeginBatchFn = void(__thiscall*)(NiDX9Renderer*,
		const NiPropertyState*, NiDynamicEffectState*);
	using EndBatchFn = void(__thiscall*)(NiDX9Renderer*);
	using BatchRenderShapeFn = void(__thiscall*)(NiDX9Renderer*, NiTriShape*);
	using RenderImmediateFn = void(__thiscall*)(NiTriShape*, NiRenderer*);
	using RenderShapeFn = void(__thiscall*)(NiDX9Renderer*, NiTriShape*);
	using DeleteThisFn = void(__thiscall*)(NiTriShape*);
	using TileRenderPassFn = SInt32(__cdecl*)(BSShaderProperty::RenderPass*,
		UInt32, bool, bool, bool);

	enum class A8CompiledShaderClass : UInt8
	{
		Original,
		Coverage,
		Body,
		Effect
	};

	struct A8CompiledRange
	{
		A8DrawRange range;
		std::array<float, 16> constants = {};
		A8CompiledShaderClass shaderClass = A8CompiledShaderClass::Original;
		UInt8 textureSamplesPerGlyph = 1;
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
		A8EffectShapeConfig effects;
		std::vector<A8CompiledRange> compiledRanges;
		A8CompiledShaderClass firstRangeShaderClass =
			A8CompiledShaderClass::Original;
		A8CompiledShaderClass firstFillShaderClass =
			A8CompiledShaderClass::Original;
		bool hasShadowRange = false;
	};
	using A8ShapeMetadataPtr = std::shared_ptr<const A8ShapeMetadata>;

	struct A8RenderTraceContext
	{
		UInt64 serial = 0;
		NiTriShape* shape = nullptr;
		const char* entryPoint = nullptr;
		bool detailed = false;
		UInt32 drawCalls = 0;
		UInt32 forwardedCalls = 0;
		UInt32 rangeAttempts = 0;
		UInt32 effectSuccesses = 0;
		UInt32 effectFailures = 0;
		UInt32 fillSuccesses = 0;
		UInt32 fillFailures = 0;
	};

	struct A8ThreadState
	{
		UInt32 renderDepth = 0;
		NiTriShape* currentShape = nullptr;
		A8ShapeMetadataPtr currentMetadata;
		std::array<A8RenderTraceContext, kMaximumRenderTraceDepth> renderTraceStack = {};
		UInt32 renderTraceDepth = 0;
	};

	struct A8State
	{
		NiD3DPixelShaderPtr a8Shader;
		NiD3DPixelShaderPtr coverageShader;
		std::array<NiD3DPixelShaderPtr, 3> effectShaders;
		std::array<void*, kCopiedTriShapeVtableEntries + 1> triShapeVtable = {};
		void** originalTriShapeVtable = nullptr;
		RenderImmediateFn originalRenderImmediate = nullptr;
		RenderImmediateFn originalRenderImmediateAlt = nullptr;
		DrawIndexedPrimitiveFn originalDrawIndexedPrimitive = nullptr;
		BeginBatchFn originalBeginBatch = nullptr;
		EndBatchFn originalEndBatch = nullptr;
		BatchRenderShapeFn originalBatchRenderShape = nullptr;
		RenderShapeFn originalRenderShape = nullptr;
		RenderShapeFn originalRenderShapeAlt = nullptr;
		DeleteThisFn originalDeleteThis = nullptr;
		TileRenderPassFn originalTileRenderPass = nullptr;
		IDirect3DDevice9* hookedDevice = nullptr;
		bool initializationInProgress = false;
		bool initializationAttempted = false;
		bool shaderLoaderCompatible = false;
		bool a8Available = false;
		bool rangeBridgeAvailable = false;
		bool tileRenderPassHookInstalled = false;
		bool loggedTileRenderPassHookConflict = false;
		bool loggedTileRenderPassHit = false;
		std::unordered_set<UInt32> loggedTileShadowResultFonts;
		bool loggedPendingRangeShape = false;
		bool loggedShaderLoaderUnavailable = false;
		bool loggedA8ShaderLoadFailure = false;
		bool loggedCoverageShaderLoadFailure = false;
		std::array<bool, 3> loggedEffectShaderLoadFailure = {};
		bool loggedHookConflict = false;
		bool loggedRendererHookConflict = false;
		bool loggedBatchRouteHit = false;
		UInt32 stateMismatchLogCount = 0;
		UInt32 shadowContractLogCount = 0;
		UInt32 rangeDrawFailureLogCount = 0;
		UInt32 shapeValidationFailureLogCount = 0;
		UInt64 shadowTraceSerial = 0;
		DWORD lastInitializationAttemptTick = 0;

		std::mutex diagnosticsMutex;
		std::unordered_map<const NiTriShape*, A8ShapeMetadataPtr> shapeMetadata;
		std::unordered_set<const NiTriShape*> loggedShapes;
		std::unordered_set<const NiTriShape*> tracedShadowShapes;
		std::deque<const NiTriShape*> tracedShadowShapeOrder;
		UInt32 diagnosticLogCount = 0;
	};

	A8State& State();
	A8ThreadState& ThreadState();

	A8ShapeMetadataPtr FindA8ShapeMetadata(const NiTriShape* shape);
	A8ShapeMetadataPtr ResolveRenderMetadata(const NiTriShape* shape);
	bool HasShadowRange(const A8ShapeMetadata& metadata);
	A8RenderTraceContext* CurrentRenderTrace();
	bool IsDetailedShadowTraceActive();
	void BeginA8RenderTrace(NiTriShape* shape, const char* entryPoint,
		const A8ShapeMetadataPtr& metadata);
	void EndA8RenderTrace(NiTriShape* shape, const char* entryPoint);
	void LogShadowTraceDeviceState(IDirect3DDevice9* device, const char* stage,
		UInt32 drawCall, int layer, IDirect3DPixelShader9* expectedShader,
		HRESULT result);
	void LogA8DrawDiagnostics(IDirect3DDevice9* device,
		D3DPRIMITIVETYPE primitiveType, INT baseVertexIndex,
		UINT minimumVertexIndex, UINT numberOfVertices, UINT startIndex,
		UINT primitiveCount, const A8ShapeMetadata& metadata);

	template <class Callback>
	inline void UpdateCurrentRenderTraces(Callback&& callback)
	{
		A8ThreadState& thread = ThreadState();
		for (UInt32 index = 0; index < thread.renderTraceDepth; ++index)
		{
			A8RenderTraceContext& trace = thread.renderTraceStack[index];
			if (trace.shape == thread.currentShape)
				callback(trace);
		}
	}

	bool HaveA8Shader();
	bool HaveEffectShader(EffectQuality quality);
	bool HaveAllEffectShaders();
	bool NeedsScaledFillSampling(const NiTriShape* shape);
	bool HookD3DDevice();
	bool HookTileRenderPass();
	TileRenderPassFn ReadTileRenderPassCallTarget();
	SInt32 __cdecl A8TileRenderPass(BSShaderProperty::RenderPass* pass,
		UInt32 currentPass, bool testAlpha, bool blendAlpha, bool setupDrawmode);
	void __fastcall A8RenderShape(NiDX9Renderer* renderer, void*,
		NiTriShape* shape);
	void __fastcall A8RenderShapeAlt(NiDX9Renderer* renderer, void*,
		NiTriShape* shape);
	void __fastcall A8BatchRenderShape(NiDX9Renderer* renderer, void*,
		NiTriShape* shape);
	bool IsPublishedRangeBridgeReady();
	bool InitializeA8TriShapeVtable(NiTriShape* shape);
	HRESULT __stdcall A8DrawIndexedPrimitive(IDirect3DDevice9* device,
		D3DPRIMITIVETYPE primitiveType, INT baseVertexIndex,
		UINT minimumVertexIndex, UINT numberOfVertices, UINT startIndex,
		UINT primitiveCount);
}

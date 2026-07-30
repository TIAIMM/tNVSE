#pragma once

// Private Gamebryo-native FreeType rendering model.

#include "font_vector_internal.h"

#include "BSShaderAccumulator.hpp"
#include "NiTriShape.hpp"
#include "TileShader.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <memory>
#include <vector>

class NiGeometryBufferData;
class NiVBChip;
class NiDX9Renderer;

namespace fonthook::vectorfont
{
	struct A8ShapeMetadata;
	struct VirtualStockShapeGroup;
	inline constexpr UInt32 kNativeA8MaximumQuads =
		std::numeric_limits<UInt16>::max() / 4u;
	inline constexpr UInt32 kMaximumVirtualStockShapes = 64;
	inline constexpr size_t kNativeA8PacketConstantRegisterCount = 8;
	inline constexpr size_t kNativeA8PacketConstantFloatCount =
		kNativeA8PacketConstantRegisterCount * 4;
	// c0-c3 retain the stock Tile WVP matrix. c4 is tNVSE-owned while a native
	// distance-field packet is active and carries viewport/raster information
	// used to compute the analytic AA footprint in the vertex shader.
	inline constexpr UInt32 kNativeA8VertexAaConstantRegister = 4;

	enum class NativeA8ShaderClass : UInt8
	{
		Body,
		Effect,
		Composite,
		Coverage,
		Argb
	};

	enum class NativeA8Sampling : UInt8
	{
		Point,
		LinearMipmapped,
		LinearLod0
	};

	enum class NativeA8FallbackReason : UInt8
	{
		None,
		ShaderGeneration,
		PacketBuild,
		PacketPrepare,
		AtlasGeneration,
		PageTexture,
		PropertySync,
		AccumulatorConflict,
		TileRouteConflict,
		DirectImmediate,
		DeviceReset,
		RuntimeFault
	};

	enum class NativeA8PacketPrepareFailure : UInt8
	{
		None,
		Generation,
		Geometry,
		ShaderBinding,
		Declaration,
		ProxyUnavailable,
		RingCapacity,
		IndexBuffer,
		VertexBuffer
	};

	struct NativeA8GpuVertex
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		float u = 0.0f;
		float v = 0.0f;
		// D3DDECLTYPE_D3DCOLOR expands this packed ARGB value to the shader's
		// normalized float4 COLOR0 input. Distance-field profiles retain their
		// per-packet layer color in c1; baked coverage instead places the complete
		// base/layer modifier here so different effects can share one packet.
		UInt32 color = 0xFFFFFFFFu;
		// Per-glyph distance-field data must not participate in packet identity.
		// Shared MTSDF double-byte atlases can mix source sizes in one text run;
		// carrying these values in TEXCOORD1 keeps those glyphs in the same
		// layer/page packet without changing their reconstruction parameters.
		float sdfSpread = 0.0f;
		float distanceParameterScale = 1.0f;
		// Exact integer mask (bits 0..3: Shadow/Glow/Outline/Fill) for distance
		// fields. Baked A8 coverage profiles instead store their per-quad live
		// Tile RGB selector here (0=fixed RGB, 1=live Tile RGB).
		float layerMask = 8.0f;
		// Exact physical glyph rectangle. The composite quad can extrapolate its
		// UVs to cover an offset shadow; the pixel shader bounds every sample to
		// this rectangle so atlas neighbours never bleed into that union.
		// Use an ordinary FLOAT4 declaration for compatibility with native D3D9
		// drivers and wrappers that reject the optional USHORT4N declaration
		// type. The HLSL input remains an exact normalized atlas rectangle.
		float glyphU0 = 0.0f;
		float glyphV0 = 0.0f;
		float glyphU1 = 0.0f;
		float glyphV1 = 0.0f;
	};
	static_assert(offsetof(NativeA8GpuVertex, u) == 3 * sizeof(float));
	static_assert(offsetof(NativeA8GpuVertex, color) == 5 * sizeof(float));
	static_assert(offsetof(NativeA8GpuVertex, sdfSpread) == 6 * sizeof(float));
	static_assert(offsetof(NativeA8GpuVertex, glyphU0) == 9 * sizeof(float));
	static_assert(sizeof(NativeA8GpuVertex) == 13 * sizeof(float));

	struct NativeA8PacketShaderCacheEntry
	{
		// Native shader profiles are process-lifetime objects. Keep this opaque in
		// the shared packet model; the shader implementation validates generation
		// and sampling before using the cached profile.
		std::atomic<void*> profile{ nullptr };

		NativeA8PacketShaderCacheEntry() = default;
		NativeA8PacketShaderCacheEntry(
			const NativeA8PacketShaderCacheEntry&) noexcept
		{
		}
		NativeA8PacketShaderCacheEntry(
			NativeA8PacketShaderCacheEntry&&) noexcept
		{
		}
		NativeA8PacketShaderCacheEntry& operator=(
			const NativeA8PacketShaderCacheEntry&) noexcept
		{
			profile.store(nullptr, std::memory_order_relaxed);
			return *this;
		}
		NativeA8PacketShaderCacheEntry& operator=(
			NativeA8PacketShaderCacheEntry&&) noexcept
		{
			profile.store(nullptr, std::memory_order_relaxed);
			return *this;
		}
	};

	struct NativeA8PacketTemplate
	{
		UInt32 firstVertex = 0;
		UInt32 vertexCount = 0;
		NiBound bound;
		std::array<float, kNativeA8PacketConstantFloatCount> constants = {};
		NativeA8ShaderClass shaderClass = NativeA8ShaderClass::Body;
		NativeA8Sampling sampling = NativeA8Sampling::Point;
		EffectQuality quality = EffectQuality::Balanced;
		DistanceFieldMethod distanceFieldMethod =
			GetConfiguredDistanceFieldMethod();
		UInt32 layer = 3;
		UInt16 atlasPage = 0;
		// Zero keeps the generic per-glyph mask shader. A non-zero value proves
		// that every quad in this composite packet carries the same exact mask
		// and permits an immutable MTSDF pixel-shader profile.
		UInt8 staticCompositeLayerMask = 0;
		bool compositeShiftedShadow = false;
		bool staticSmoothSampling = false;
		bool usesLiveTileRgb = true;
		// Profile hashes are immutable artifact data. The two slots correspond to
		// separate-alpha disabled/enabled. Resolved profiles are validated against
		// the current generation and reset automatically on packet copy or move.
		std::array<size_t, 2> profileHashes = {};
		mutable std::array<NativeA8PacketShaderCacheEntry, 2>
			resolvedShaders;
	};

	struct NativeA8CompositeSpan
	{
		UInt32 firstVertex = 0;
		UInt32 vertexCount = 0;
		UInt16 atlasPage = 0;
		bool fused = false;
	};

	// A retained run is immutable Text Artifact metadata. It deliberately
	// excludes Tile, renderer and D3D state; those identities are compiled into
	// the traversal-local command buffer only after sorted preflight.
	struct NativeA8RetainedRun
	{
		UInt32 firstPacket = 0;
		UInt32 packetCount = 0;
		UInt32 firstVertex = 0;
		UInt32 vertexCount = 0;
		NativeA8ShaderClass shaderClass = NativeA8ShaderClass::Body;
		UInt16 firstAtlasPage = 0;
		bool bridgeEligible = false;
	};

	struct NativeA8CompiledPacketCommand;

	// Renderer-owned VB locations are mutable cache data attached to the immutable
	// text artifact. Every read is protected by the ring mutex and validated
	// against the current resource serial/epoch before the location is used.
	struct NativeA8PayloadResidencyCache
	{
		UInt32 staticResourceSerial = 0;
		UInt32 staticBaseVertex = 0;
		UInt32 staticVertexCount = 0;
		UInt32 dynamicResourceSerial = 0;
		UInt32 dynamicUploadEpoch = 0;
		UInt32 dynamicBaseVertex = 0;
		UInt32 dynamicVertexCount = 0;
	};

	struct NativeA8PayloadTemplate
	{
		CpuMemoryLease cpuMemory;
		UInt32 pageCount = 0;
		UInt32 quadCount = 0;
		UInt32 sourceRangeCount = 0;
		NiBound bound;
		std::vector<NiTexturingPropertyPtr> atlasProperties;
		std::vector<NiTexturePtr> atlasTextures;
		std::vector<NativeA8GpuVertex> gpuVertices;
		std::vector<NativeA8PacketTemplate> packets;
		std::vector<NativeA8RetainedRun> retainedRuns;
		// Optional single-pass distance-field representation. The ordinary packet
		// list remains available if that shader class is unavailable.
		std::vector<NativeA8PacketTemplate> compositePackets;
		std::vector<NativeA8RetainedRun> compositeRetainedRuns;
		mutable NativeA8PayloadResidencyCache residency;
	};
	using NativeA8PayloadTemplatePtr =
		std::shared_ptr<const NativeA8PayloadTemplate>;

	struct NativeA8ShapePayload
	{
		NativeA8PayloadTemplatePtr payloadTemplate;
		NiPoint3 geometryOrigin;
		// Packet geometry, constants, page identity, and profile keys live only in
		// the shared text artifact. A Tile instance retains generation-bound
		// shader/program views; both are cleared with native preflight state.
		std::vector<TileShader*> packetShaders;
		std::vector<const NativeA8CompiledPacketCommand*> packetPrograms;
		std::atomic<bool> suppressNextSubmit = false;
		std::atomic<NativeA8FallbackReason> stickyReason =
			NativeA8FallbackReason::None;
		std::atomic<NativeA8PacketPrepareFailure> packetPrepareFailure =
			NativeA8PacketPrepareFailure::None;
		// A successful preflight is reusable while the shader generation, scaled
		// fill sampling class, and the referenced page textures remain unchanged.
		// Null entries belong to atlas pages that no packet in this payload uses.
		std::vector<const void*> preflightAtlasTextures;
		UInt32 preparedGeneration = 0;
		UInt32 compositeAttemptGeneration = 0;
		UInt32 preflightAtlasTextureEpoch = 0;
		bool preflightScaledFillSampling = false;
		bool preflightAlphaBlending = false;
		bool useCompositePackets = false;
		bool compositeUnavailable = false;
		bool stockLikeBitmapPackets = false;
		bool buildComplete = false;
	};

	inline const std::vector<NativeA8PacketTemplate>& GetNativeA8Packets(
		const NativeA8PayloadTemplate& payloadTemplate, bool useComposite)
	{
		return useComposite && !payloadTemplate.compositePackets.empty()
			? payloadTemplate.compositePackets : payloadTemplate.packets;
	}

	inline const std::vector<NativeA8RetainedRun>& GetNativeA8RetainedRuns(
		const NativeA8PayloadTemplate& payloadTemplate, bool useComposite)
	{
		return useComposite && !payloadTemplate.compositePackets.empty()
			? payloadTemplate.compositeRetainedRuns
			: payloadTemplate.retainedRuns;
	}

	inline bool UsesOnlyStockLikeBitmapPackets(
		const std::vector<NativeA8PacketTemplate>& packets)
	{
		if (packets.empty())
			return false;
		for (const NativeA8PacketTemplate& packet : packets)
		{
			if (packet.shaderClass != NativeA8ShaderClass::Argb
				&& packet.shaderClass != NativeA8ShaderClass::Coverage)
			{
				return false;
			}
		}
		return true;
	}

	enum class NativeA8VisibilityCull : UInt8
	{
		None = 0,
		AppCulled,
		ZeroAlpha,
		Clip,
		Scissor
	};

	struct NativeA8SortedFrameEntryView
	{
		const A8ShapeMetadata* metadata = nullptr;
		NativeA8ShapePayload* payload = nullptr;
		NativeA8FallbackReason preflightResult =
			NativeA8FallbackReason::RuntimeFault;
		NativeA8VisibilityCull visibilityCull =
			NativeA8VisibilityCull::None;
		UInt32 generation = 0;
		UInt64 validationToken = 0;
		UInt32 commandSpanIndex = std::numeric_limits<UInt32>::max();
	};

	inline constexpr UInt32 kInvalidNativeA8CommandIndex =
		std::numeric_limits<UInt32>::max();

	enum class NativeA8CommandSpanState : UInt8
	{
		Ready = 0,
		Executing,
		Consumed,
		Fault
	};

	enum class NativeA8CommandFallback : UInt8
	{
		None = 0,
		Token,
		Generation,
		Atlas,
		Resource,
		Topology,
		Hook,
		Nested,
		RenderTarget,
		State
	};

	struct NativeA8FramePacketBinding
	{
		IDirect3DVertexBuffer9* vertexBuffer = nullptr;
		IDirect3DIndexBuffer9* indexBuffer = nullptr;
		IDirect3DVertexDeclaration9* declaration = nullptr;
		UInt32 baseVertex = 0;
		UInt32 vertexCount = 0;
		UInt32 indexBytes = 0;
		UInt32 generation = 0;
		UInt32 resourceSerial = 0;
		UInt32 uploadEpoch = 0;
		bool staticResident = false;
		bool active = false;
	};

	// Shader profiles stay private to font_native_shader.cpp. This immutable,
	// generation-owned program is published once with its profile. Traversal-local
	// commands retain only a non-owning pointer and validate the generation before
	// every execution.
	struct NativeA8CompiledPacketCommand
	{
		void* profile = nullptr;
		TileShader* shader = nullptr;
		IDirect3DDevice9* device = nullptr;
		IDirect3DVertexShader9* vertexShader = nullptr;
		IDirect3DPixelShader9* pixelShader = nullptr;
		void* setupPass = nullptr;
		void* setupBlend = nullptr;
		void* setupAlphaTest = nullptr;
		void* setupDrawmode = nullptr;
		UInt32 generation = 0;
		bool simpleColor = false;
		bool active = false;
	};

	struct NativeA8CommandBindState
	{
		bool applyBlend = false;
		bool applyAlphaTest = false;
		bool applyDrawmode = false;
		bool noFog = false;
	};

	struct NativeA8FrameStamp
	{
		BSShaderAccumulator* accumulator = nullptr;
		NiDX9Renderer* renderer = nullptr;
		IDirect3DDevice9* device = nullptr;
		IDirect3DSurface9* renderTarget = nullptr;
		D3DVIEWPORT9 viewport = {};
		UInt64 validationToken = 0;
		UInt32 generation = 0;
		UInt32 atlasTextureEpoch = 0;
		UInt32 resourceSerial = 0;
		UInt32 uploadEpoch = 0;
		UInt64 nestedTraversalSerial = 0;
		bool renderTargetReady = false;
		bool viewportReady = false;
	};

	struct NativeA8DrawCommand
	{
		NiTriShape* sourceGeometry = nullptr;
		NiTriShape* expectedGeometry = nullptr;
		NativeA8ShapePayload* payload = nullptr;
		const NativeA8PacketTemplate* packet = nullptr;
		const void* atlasTexture = nullptr;
		NativeA8FramePacketBinding binding;
		const NativeA8CompiledPacketCommand* program = nullptr;
		UInt32 packetIndex = 0;
	};

	struct NativeA8FrameCommandRun
	{
		UInt32 firstCommand = 0;
		UInt32 commandCount = 0;
		bool bridgeEligible = false;
		// Binder runs keep exact shader profiles. Retained replay may continue
		// across an adjacent profile run when the live Tile state is proven
		// identical.
		bool continuesBridgeSpan = false;
	};

	struct NativeA8CommandSpan
	{
		NiTriShape* facade = nullptr;
		const A8ShapeMetadata* metadata = nullptr;
		NativeA8ShapePayload* payload = nullptr;
		VirtualStockShapeGroup* virtualStockGroup = nullptr;
		UInt32 firstCommand = 0;
		UInt32 commandCount = 0;
		UInt32 firstRun = 0;
		UInt32 runCount = 0;
		UInt32 leaderSlot = 0;
		UInt32 generation = 0;
		UInt32 atlasTextureEpoch = 0;
		UInt64 validationToken = 0;
		UInt64 executionValidationToken = 0;
		// Execution-only epochs are assigned when the span enters a validated
		// contiguous FreeType segment and cleared as soon as it is consumed.
		UInt32 executionSegmentEpoch = 0;
		UInt32 executionExternalMutationEpoch = 0;
		NativeA8CommandSpanState state = NativeA8CommandSpanState::Ready;
		bool virtualStock = false;
		bool bridgeEligible = false;
		bool partialDraw = false;
		bool useCompositePackets = false;
	};

	struct NativeA8CommandSpanView
	{
		const NativeA8FrameStamp* stamp = nullptr;
		const NativeA8CommandSpan* span = nullptr;
		const NativeA8DrawCommand* commands = nullptr;
		const NativeA8FrameCommandRun* runs = nullptr;
		UInt32 spanIndex = kInvalidNativeA8CommandIndex;
	};

	const char* NativeA8FallbackReasonName(NativeA8FallbackReason reason);
	const char* NativeA8PacketPrepareFailureName(
		NativeA8PacketPrepareFailure failure);

	NativeA8PayloadTemplatePtr BuildNativeA8PayloadTemplate(
		std::vector<NativeA8GpuVertex>&& vertices, UInt32 quadCount,
		const A8EffectShapeConfig& effects, const NiBound& bound,
		std::vector<NativeA8CompositeSpan>&& compositeSpans);
	bool InitializeNativeA8ShapePayload(Font& font,
		NiTriShape* facade, const A8ShapeMetadata& metadata,
		NativeA8PayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin, NativeA8ShapePayload& payload);
	size_t GetNativeA8PayloadTemplateBytes(
		const NativeA8PayloadTemplate& payloadTemplate);
	void InvalidateNativeA8RingResources(NativeA8FallbackReason reason);

	struct NativeA8RingSubmission
	{
		NiTriShape* proxyShape = nullptr;
		NiGeometryBufferData* proxyBuffer = nullptr;
		NiVBChip* proxyChip = nullptr;
		IDirect3DVertexBuffer9* vertexBuffer = nullptr;
		UInt32 proxyIndex = std::numeric_limits<UInt32>::max();
		UInt32 generation = 0;
		UInt32 resourceSerial = 0;
		UInt32 nextPacket = 0;
		UInt32 payloadBaseVertex = 0;
		UInt32 endVertex = 0;
		bool staticResident = false;
		bool active = false;
	};

	// A sorted single-packet shape can borrow the sealed ring/static resources
	// directly for the duration of its stock Tile render pass. Unlike
	// NativeA8RingSubmission this lease owns no proxy and copies no Tile state:
	// the actual facade remains the render-pass geometry, so its live transform,
	// scissor, alpha, material, cull, and stencil state stay authoritative.
	struct NativeA8DirectShapeSubmission
	{
		IDirect3DVertexBuffer9* vertexBuffer = nullptr;
		IDirect3DIndexBuffer9* indexBuffer = nullptr;
		IDirect3DVertexDeclaration9* declaration = nullptr;
		UInt32 baseVertex = 0;
		UInt32 vertexCount = 0;
		UInt32 indexBytes = 0;
		UInt32 generation = 0;
		UInt32 resourceSerial = 0;
		bool staticResident = false;
		bool active = false;
	};

	// A virtual-stock shape keeps a plugin-owned geometry descriptor and borrows
	// either immutable static residency or a traversal-sealed dynamic range for
	// the complete sorted Tile traversal. The sorted-frame lease owns the D3D
	// resources; this value is a validated non-owning view and must never outlive
	// that traversal.
	struct NativeA8VirtualStockPacketBinding
	{
		IDirect3DVertexBuffer9* vertexBuffer = nullptr;
		IDirect3DIndexBuffer9* indexBuffer = nullptr;
		IDirect3DVertexDeclaration9* declaration = nullptr;
		UInt32 baseVertex = 0;
		UInt32 vertexCount = 0;
		UInt32 indexBytes = 0;
		UInt32 generation = 0;
		UInt32 resourceSerial = 0;
		UInt32 uploadEpoch = 0;
		UInt32 atlasTextureEpoch = 0;
		bool staticResident = false;
		bool active = false;
	};

	bool EnsureNativeA8ProxyPool(Font& font);
	NativeA8FallbackReason BeginNativeA8DirectShapeSubmission(
		NiTriShape* facade, NativeA8ShapePayload& payload,
		NativeA8DirectShapeSubmission& submission);
	void EndNativeA8DirectShapeSubmission(
		NativeA8DirectShapeSubmission& submission);
	NativeA8FallbackReason ResolveNativeA8VirtualStockPacketBinding(
		NativeA8ShapePayload& payload, UInt32 packetIndex,
		NativeA8VirtualStockPacketBinding& binding);
	bool IsNativeA8VirtualStockPacketBindingCurrent(
		const NativeA8VirtualStockPacketBinding& binding);
	bool IsNativeA8VirtualStockPacketAtlasCurrent(
		const NiTriShape* shape, const NativeA8ShapePayload& payload,
		UInt32 packetIndex);
	NativeA8FallbackReason BeginNativeA8RingSubmission(
		NiTriShape* facade, NativeA8ShapePayload& payload,
		NativeA8RingSubmission& submission);
	NativeA8FallbackReason PrepareNativeA8RingPacket(
		NiTriShape* facade, NativeA8ShapePayload& payload,
		NativeA8RingSubmission& submission, UInt32 packetIndex,
		NiTriShape*& proxyShape);
	NativeA8FallbackReason SkipNativeA8RingPacket(
		NativeA8ShapePayload& payload,
		NativeA8RingSubmission& submission, UInt32 packetIndex);
	void EndNativeA8RingSubmission(NativeA8RingSubmission& submission);
	void ReleaseNativeA8RingResources();
	void PrepareSortedNativeA8Payloads(
		std::vector<NativeA8PayloadTemplatePtr>& payloadTemplates,
		UInt32 generation);
	void EndNativeA8SortedRingFrame();
	bool ResolveNativeA8FramePacketBinding(
		const NativeA8ShapePayload& payload, UInt32 packetIndex,
		NativeA8FramePacketBinding& binding);
	bool IsNativeA8FramePacketBindingCurrent(
		const NativeA8FramePacketBinding& binding);
	bool IsNativeA8FrameResourceStampCurrent(
		UInt32 generation, UInt32 resourceSerial, UInt32 uploadEpoch);
	void TrimNativeA8CpuCachesForTotalBudget();
	bool FindNativeA8SortedFrameEntry(NiTriShape* facade,
		NativeA8SortedFrameEntryView& view);
	UInt64 GetNativeA8SortedFrameValidationToken();
	UInt64 GetNativeA8SortedNestedTraversalSerial();
	NativeA8VisibilityCull EvaluateNativeA8SubmissionVisibility(
		const NiTriShape* facade, const NativeA8ShapePayload& payload);
	void RecordNativeA8VisibilityCull(NativeA8VisibilityCull reason,
		const NativeA8ShapePayload& payload);
	UInt32 GetNativeA8AtlasTextureEpoch();
	void NotifyNativeA8AtlasTextureMutation();

	bool InitializeNativeA8Renderer(bool forceAttempt, bool reportFailures);
	void HandleNativeA8RendererMainLoop();
	void HandleNativeA8ShaderLoaderMessage(UInt32 messageType);
	bool IsNativeA8RendererAvailable();
	void MarkNativeA8GenerationFault(UInt32 generation,
		const char* operation, HRESULT result);
	UInt32 GetNativeA8ShaderGeneration();
	IDirect3DVertexDeclaration9* GetNativeA8D3DDeclaration(UInt32 generation);
	bool IsNativeA8ShaderGenerationCurrent(UInt32 generation);
	void BeginNativeA8SortedShaderBatch();
	void EndNativeA8SortedShaderBatch();
	void InvalidateNativeA8SortedShaderState();
	void BeginNativeA8FacadeShaderBatch();
	void EndNativeA8FacadeShaderBatch();
	TileShader* ResolveNativeA8PacketShader(const NativeA8PacketTemplate& packet,
		const NiTriShape* facade, bool scaledFillSampling);
	bool ResolveNativeA8RetainedPacketProgram(
		const NativeA8PacketTemplate& packet,
		TileShader* shader, UInt32 generation,
		const NativeA8CompiledPacketCommand*& program);
	bool BindNativeA8CommandPacket(
		const NativeA8CompiledPacketCommand& command,
		const void* atlasTexture, bool publishPrograms,
		const NiPropertyState* properties,
		const NativeA8CommandBindState& bindState,
		const char*& operation, HRESULT& result);
	NativeA8FallbackReason PrepareNativeA8Group(NiTriShape* facade,
		const A8ShapeMetadata& metadata, NativeA8ShapePayload& payload);

	void BeginNativeA8FrameCommandBuffer(BSShaderAccumulator* accumulator,
		UInt64 validationToken, UInt32 generation, UInt32 atlasTextureEpoch);
	UInt32 AddNativeA8FrameCommandSpan(NiTriShape* facade,
		const A8ShapeMetadata* metadata, NativeA8ShapePayload* payload,
		VirtualStockShapeGroup* virtualStockGroup = nullptr);
	void ActivateNativeA8FrameCommandBuffer();
	void EndNativeA8FrameCommandBuffer();
	void InvalidateNativeA8CommandExecutionSegment(
		NativeA8CommandFallback reason = NativeA8CommandFallback::State);
	void NotifyNativeA8CommandExternalMutation(
		NativeA8CommandFallback reason);
	void InvalidateNativeA8CommandGeometry(NiTriShape* geometry);
	bool FindNativeA8CommandSpan(UInt32 spanIndex, UInt64 validationToken,
		NativeA8CommandSpanView& view);
	bool BeginNativeA8CommandSpanExecution(UInt32 spanIndex,
		NiTriShape* geometry, bool virtualLeader,
		NativeA8CommandSpanView& view);
	void EndNativeA8CommandSpanExecution(UInt32 spanIndex,
		bool success, bool drewPacket);
	bool ShouldConsumeNativeA8CommandFollower(UInt32 spanIndex,
		UInt64 validationToken, NiTriShape* geometry,
		UInt32 commandOffset);
	bool ValidateNativeA8Command(UInt32 spanIndex,
		UInt32 commandOffset, NiTriShape* geometry, NiRenderer* renderer);
	void RecordNativeA8CommandFallback(NativeA8CommandFallback reason);

	bool HookNativeA8Accumulator();
	bool IsNativeA8AccumulatorHookCurrent();
	bool IsNativeA8SortedTraversalHookCurrent();

	void RecordNativeA8Suppression(NiTriShape* shape,
		const A8ShapeMetadata& metadata, NativeA8FallbackReason reason,
		const char* phase);
	void MarkNativeA8RuntimeFault(const A8ShapeMetadata& metadata,
		NativeA8ShapePayload& payload,
		NativeA8FallbackReason reason);

}

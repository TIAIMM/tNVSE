#pragma once

// Private Gamebryo-native FreeType rendering model.

#include "font_vector_internal.h"

#include "BSShaderAccumulator.hpp"
#include "NiTriShape.hpp"
#include "TileShader.hpp"

#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <vector>

class NiGeometryBufferData;
class NiVBChip;

namespace fonthook::vectorfont
{
	struct A8ShapeMetadata;
	inline constexpr UInt32 kNativeA8MaximumQuads =
		std::numeric_limits<UInt16>::max() / 4u;
	inline constexpr size_t kNativeA8PacketConstantRegisterCount = 8;
	inline constexpr size_t kNativeA8PacketConstantFloatCount =
		kNativeA8PacketConstantRegisterCount * 4;

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
		// Normalized USHORT4 keeps the ABI compact in the 32-bit process while
		// retaining substantially finer precision than any supported POT atlas.
		UInt16 glyphU0 = 0;
		UInt16 glyphV0 = 0;
		UInt16 glyphU1 = 0;
		UInt16 glyphV1 = 0;
	};
	static_assert(sizeof(NativeA8GpuVertex) == 11 * sizeof(float));

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
		bool staticSmoothSampling = false;
		bool usesLiveTileRgb = true;
	};

	struct NativeA8CompositeSpan
	{
		UInt32 firstVertex = 0;
		UInt32 vertexCount = 0;
		UInt16 atlasPage = 0;
		bool fused = false;
	};

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
		// Optional single-pass distance-field representation. The ordinary packet
		// list remains available if that shader class is unavailable.
		std::vector<NativeA8PacketTemplate> compositePackets;
		mutable NativeA8PayloadResidencyCache residency;
	};
	using NativeA8PayloadTemplatePtr =
		std::shared_ptr<const NativeA8PayloadTemplate>;

	struct NativeA8ShapePayload
	{
		NativeA8PayloadTemplatePtr payloadTemplate;
		NiPoint3 geometryOrigin;
		// Packet geometry, constants, page identity, and profile keys live only in
		// the shared text artifact. A Tile instance retains just resolved shaders.
		std::vector<TileShader*> packetShaders;
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

	struct NativeA8SortedFrameEntryView
	{
		const A8ShapeMetadata* metadata = nullptr;
		NativeA8ShapePayload* payload = nullptr;
		NativeA8FallbackReason preflightResult =
			NativeA8FallbackReason::RuntimeFault;
		UInt32 generation = 0;
		UInt64 validationToken = 0;
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

	bool EnsureNativeA8ProxyPool(Font& font);
	NativeA8FallbackReason BeginNativeA8RingSubmission(
		NiTriShape* facade, NativeA8ShapePayload& payload,
		NativeA8RingSubmission& submission);
	NativeA8FallbackReason PrepareNativeA8RingPacket(
		NiTriShape* facade, NativeA8ShapePayload& payload,
		NativeA8RingSubmission& submission, UInt32 packetIndex,
		NiTriShape*& proxyShape);
	void EndNativeA8RingSubmission(NativeA8RingSubmission& submission);
	void ReleaseNativeA8RingResources();
	void PrepareSortedNativeA8Payloads(
		std::vector<NativeA8PayloadTemplatePtr>& payloadTemplates,
		UInt32 generation);
	void EndNativeA8SortedRingFrame();
	void TrimNativeA8CpuCachesForTotalBudget();
	bool FindNativeA8SortedFrameEntry(NiTriShape* facade,
		NativeA8SortedFrameEntryView& view);
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
	NativeA8FallbackReason PrepareNativeA8Group(NiTriShape* facade,
		const A8ShapeMetadata& metadata, NativeA8ShapePayload& payload);

	bool HookNativeA8Accumulator();
	bool IsNativeA8AccumulatorHookCurrent();

	void RecordNativeA8Suppression(NiTriShape* shape,
		const A8ShapeMetadata& metadata, NativeA8FallbackReason reason,
		const char* phase);
	void MarkNativeA8RuntimeFault(const A8ShapeMetadata& metadata,
		NativeA8ShapePayload& payload,
		NativeA8FallbackReason reason);

}

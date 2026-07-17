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

namespace fonthook::vectorfont
{
	struct A8ShapeMetadata;
	inline constexpr UInt32 kNativeA8MaximumQuads =
		std::numeric_limits<UInt16>::max() / 4u;

	enum class NativeA8ShaderClass : UInt8
	{
		Original,
		Coverage,
		Body,
		Effect
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

	struct NativeA8Packet
	{
		UInt32 templateIndex = 0;
		std::array<float, 16> constants = {};
		NativeA8ShaderClass shaderClass = NativeA8ShaderClass::Original;
		NativeA8Sampling sampling = NativeA8Sampling::Point;
		EffectQuality quality = EffectQuality::Balanced;
		UInt32 layer = 3;
		UInt16 atlasPage = 0;
		TileShader* shader = nullptr;
		bool staticSmoothSampling = false;
	};

	struct NativeA8GpuVertex
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		float u = 0.0f;
		float v = 0.0f;
		float r = 1.0f;
		float g = 1.0f;
		float b = 1.0f;
		float a = 1.0f;
	};
	static_assert(sizeof(NativeA8GpuVertex) == 9 * sizeof(float));

	struct NativeA8PacketTemplate
	{
		std::vector<NativeA8GpuVertex> vertices;
		NiBound bound;
		std::array<float, 16> constants = {};
		NativeA8ShaderClass shaderClass = NativeA8ShaderClass::Original;
		NativeA8Sampling sampling = NativeA8Sampling::Point;
		EffectQuality quality = EffectQuality::Balanced;
		UInt32 layer = 3;
		UInt16 atlasPage = 0;
		bool staticSmoothSampling = false;
	};

	struct NativeA8PayloadTemplate
	{
		UInt32 pageCount = 0;
		UInt32 quadCount = 0;
		std::vector<NativeA8PacketTemplate> packets;
	};
	using NativeA8PayloadTemplatePtr =
		std::shared_ptr<const NativeA8PayloadTemplate>;

	struct NativeA8ShapePayload
	{
		UInt32 fontId = 0;
		UInt32 pageCount = 0;
		UInt32 quadCount = 0;
		NativeA8PayloadTemplatePtr payloadTemplate;
		NiPoint3 geometryOrigin;
		std::vector<NativeA8Packet> packets;
		std::atomic<bool> suppressNextSubmit = false;
		std::atomic<NativeA8FallbackReason> stickyReason =
			NativeA8FallbackReason::None;
		std::atomic<NativeA8PacketPrepareFailure> packetPrepareFailure =
			NativeA8PacketPrepareFailure::None;
		UInt32 preparedGeneration = 0;
		bool buildComplete = false;
	};
	using NativeA8ShapePayloadPtr = std::shared_ptr<NativeA8ShapePayload>;

	const char* NativeA8FallbackReasonName(NativeA8FallbackReason reason);
	const char* NativeA8PacketPrepareFailureName(
		NativeA8PacketPrepareFailure failure);

	NativeA8ShapePayloadPtr BuildNativeA8ShapePayload(Font& font,
		NiTriShape* facade, const A8ShapeMetadata& metadata);
	NativeA8PayloadTemplatePtr BuildNativeA8PayloadTemplate(
		NiTriShape* facade, const A8ShapeMetadata& metadata,
		const NiPoint3& geometryOrigin);
	NativeA8ShapePayloadPtr InstantiateNativeA8ShapePayload(Font& font,
		NiTriShape* facade, const A8ShapeMetadata& metadata,
		NativeA8PayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin);
	size_t GetNativeA8PayloadTemplateBytes(
		const NativeA8PayloadTemplate& payloadTemplate);
	void InvalidateNativeA8RingResources(NativeA8FallbackReason reason);

	struct NativeA8RingSubmission
	{
		NiTriShape* proxyShape = nullptr;
		IDirect3DVertexBuffer9* vertexBuffer = nullptr;
		UInt32 proxyIndex = std::numeric_limits<UInt32>::max();
		UInt32 generation = 0;
		UInt32 nextPacket = 0;
		UInt32 nextBaseVertex = 0;
		UInt32 endVertex = 0;
		bool active = false;
	};

	bool EnsureNativeA8ProxyPool(Font& font);
	NativeA8FallbackReason BeginNativeA8RingSubmission(
		NiTriShape* facade, const A8ShapeMetadata& metadata,
		NativeA8ShapePayload& payload, NativeA8RingSubmission& submission);
	NativeA8FallbackReason PrepareNativeA8RingPacket(
		NiTriShape* facade, const A8ShapeMetadata& metadata,
		NativeA8ShapePayload& payload, NativeA8RingSubmission& submission,
		UInt32 packetIndex, NiTriShape*& proxyShape);
	void EndNativeA8RingSubmission(NativeA8RingSubmission& submission);
	void ReleaseNativeA8RingResources();

	bool InitializeNativeA8Renderer(bool forceAttempt, bool reportFailures);
	void HandleNativeA8RendererMainLoop();
	void HandleNativeA8ShaderLoaderMessage(UInt32 messageType);
	bool IsNativeA8RendererAvailable();
	void MarkNativeA8GenerationFault(UInt32 generation,
		const char* operation, HRESULT result);
	UInt32 GetNativeA8ShaderGeneration();
	bool IsNativeA8ShaderGenerationCurrent(UInt32 generation);
	TileShader* ResolveNativeA8PacketShader(const NativeA8Packet& packet,
		const NiTriShape* facade, bool scaledFillSampling);
	NativeA8FallbackReason PrepareNativeA8Group(NiTriShape* facade,
		const A8ShapeMetadata& metadata, NativeA8ShapePayload& payload);

	bool HookNativeA8Accumulator();
	bool IsNativeA8AccumulatorHookCurrent();

	void RecordNativeA8Suppression(NiTriShape* shape,
		const A8ShapeMetadata& metadata, NativeA8FallbackReason reason,
		const char* phase);
	void MarkNativeA8RuntimeFault(NativeA8ShapePayload& payload,
		NativeA8FallbackReason reason);
}

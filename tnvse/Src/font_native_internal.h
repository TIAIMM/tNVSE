#pragma once

// Private Gamebryo-native FreeType rendering model.  The legacy A8 range
// bridge remains a whole-shape fallback and is deliberately kept out of the
// packet/shader implementation exposed here.

#include "font_vector_internal.h"

#include "BSShaderAccumulator.hpp"
#include "NiTriShape.hpp"
#include "TileShader.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace fonthook::vectorfont
{
	struct A8ShapeMetadata;

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
		NativeInitialization,
		ShaderGeneration,
		PacketBuild,
		PacketPrepare,
		PacketPending,
		AtlasGeneration,
		PageTexture,
		PropertySync,
		AccumulatorConflict,
		TileRouteConflict,
		DirectImmediate,
		DeviceReset,
		ShaderRefresh,
		RuntimeFault,
		BridgeUnavailable
	};

	enum class NativeA8PacketPrepareFailure : UInt8
	{
		None,
		Generation,
		Profile,
		Geometry,
		Purge,
		ShaderSetup,
		ShaderBinding,
		Precache,
		RendererBuffer,
		Declaration,
		StreamCount,
		VertexStride,
		VertexCount,
		IndexCount,
		IndexBuffer,
		VertexBuffer
	};

	enum class NativeA8PacketPrepareStatus : UInt8
	{
		Ready,
		Pending,
		Failed
	};

	struct NativeA8PacketPrepareResult
	{
		NativeA8PacketPrepareStatus status =
			NativeA8PacketPrepareStatus::Failed;
		NativeA8PacketPrepareFailure failure =
			NativeA8PacketPrepareFailure::None;
	};

	struct NativeA8Packet
	{
		NiTriShapePtr shape;
		std::array<float, 16> constants = {};
		NativeA8ShaderClass shaderClass = NativeA8ShaderClass::Original;
		NativeA8Sampling sampling = NativeA8Sampling::Point;
		EffectQuality quality = EffectQuality::Balanced;
		UInt32 layer = 3;
		UInt16 atlasPage = 0;
		UInt32 queuedGeneration = 0;
		bool usesSdf = false;
		bool staticSmoothSampling = false;
	};

	struct NativeA8ShapePayload
	{
		UInt32 fontId = 0;
		UInt32 pageCount = 0;
		UInt32 quadCount = 0;
		std::vector<NativeA8Packet> packets;
		std::atomic<bool> bridgeNextSubmit = false;
		std::atomic<bool> buffersRequirePurge = false;
		std::atomic<NativeA8FallbackReason> stickyReason =
			NativeA8FallbackReason::None;
		std::atomic<UInt32> blockedGeneration = 0;
		std::atomic<NativeA8FallbackReason> blockedReason =
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
	bool SyncNativeA8PacketState(NiTriShape* facade,
		NativeA8ShapePayload& payload);
	bool PurgeNativeA8PacketBuffers(NativeA8ShapePayload& payload);
	void InvalidateNativeA8PacketBuffers(NativeA8FallbackReason reason);

	bool InitializeNativeA8Renderer(bool forceAttempt, bool reportFailures);
	void HandleNativeA8RendererMainLoop();
	void HandleNativeA8ShaderLoaderMessage(UInt32 messageType);
	bool IsNativeA8RendererAvailable();
	NativeA8PacketPrepareResult PrepareNativeA8PacketBuffer(
		NativeA8Packet& packet, TileShader* shader, bool& rebuilt);
	void MarkNativeA8GenerationFault(UInt32 generation,
		const char* operation, HRESULT result);
	UInt32 GetNativeA8ShaderGeneration();
	bool IsNativeA8ShaderGenerationCurrent(UInt32 generation);
	TileShader* ResolveNativeA8PacketShader(const NativeA8Packet& packet,
		bool scaledFillSampling);
	NativeA8FallbackReason PrepareNativeA8Group(NiTriShape* facade,
		const A8ShapeMetadata& metadata, NativeA8ShapePayload& payload);

	bool HookNativeA8Accumulator();
	bool IsNativeA8AccumulatorHookCurrent();

	bool EnsureA8BridgeFallbackReady();
	void RecordNativeA8Fallback(NiTriShape* shape,
		const A8ShapeMetadata& metadata, NativeA8FallbackReason reason,
		bool bridgeReady);
	void ForgetNativeA8FallbackShape(const NiTriShape* shape);
	void RecordNativeA8Recovery(NiTriShape* shape,
		const A8ShapeMetadata& metadata);
	void MarkNativeA8RuntimeFault(NativeA8ShapePayload& payload,
		NativeA8FallbackReason reason);
}

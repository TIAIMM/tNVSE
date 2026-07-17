#pragma once

// Private Gamebryo-native FreeType rendering model.

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
		RuntimeFault
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

	enum class NativeA8PacketPendingStage : UInt8
	{
		None,
		ExternalQueue,
		RendererPacking
	};

	struct NativeA8PacketPrepareResult
	{
		NativeA8PacketPrepareStatus status =
			NativeA8PacketPrepareStatus::Failed;
		NativeA8PacketPrepareFailure failure =
			NativeA8PacketPrepareFailure::None;
		NativeA8PacketPendingStage pendingStage =
			NativeA8PacketPendingStage::None;
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
		bool queuedViaStock = false;
		bool staticSmoothSampling = false;
	};

	struct NativeA8PacketTemplate
	{
		std::vector<NiPoint3> vertices;
		std::vector<NiPoint2> texture;
		std::vector<NiColorA> colors;
		std::vector<UInt16> indices;
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
		std::vector<NativeA8Packet> packets;
		std::atomic<bool> suppressNextSubmit = false;
		std::atomic<bool> buffersRequirePurge = false;
		std::atomic<NativeA8FallbackReason> stickyReason =
			NativeA8FallbackReason::None;
		std::atomic<UInt32> blockedGeneration = 0;
		std::atomic<NativeA8FallbackReason> blockedReason =
			NativeA8FallbackReason::None;
		std::atomic<NativeA8PacketPrepareFailure> packetPrepareFailure =
			NativeA8PacketPrepareFailure::None;
		std::atomic<NativeA8PacketPendingStage> packetPendingStage =
			NativeA8PacketPendingStage::None;
		UInt32 preparedGeneration = 0;
		bool buildComplete = false;
	};
	using NativeA8ShapePayloadPtr = std::shared_ptr<NativeA8ShapePayload>;

	const char* NativeA8FallbackReasonName(NativeA8FallbackReason reason);
	const char* NativeA8PacketPrepareFailureName(
		NativeA8PacketPrepareFailure failure);
	const char* NativeA8PacketPendingStageName(
		NativeA8PacketPendingStage stage);

	NativeA8ShapePayloadPtr BuildNativeA8ShapePayload(Font& font,
		NiTriShape* facade, const A8ShapeMetadata& metadata);
	NativeA8PayloadTemplatePtr BuildNativeA8PayloadTemplate(
		NiTriShape* facade, const A8ShapeMetadata& metadata,
		const NiPoint3& geometryOrigin);
	NativeA8ShapePayloadPtr InstantiateNativeA8ShapePayload(Font& font,
		NiTriShape* facade, const A8ShapeMetadata& metadata,
		const NativeA8PayloadTemplate& payloadTemplate,
		const NiPoint3& geometryOrigin);
	size_t GetNativeA8PayloadTemplateBytes(
		const NativeA8PayloadTemplate& payloadTemplate);
	bool SyncNativeA8PacketState(NiTriShape* facade,
		NativeA8ShapePayload& payload);
	bool PurgeNativeA8PacketBuffers(NativeA8ShapePayload& payload);
	void InvalidateNativeA8PacketBuffers(NativeA8FallbackReason reason);

	bool InitializeNativeA8Renderer(bool forceAttempt, bool reportFailures);
	void HandleNativeA8RendererMainLoop();
	void HandleNativeA8ShaderLoaderMessage(UInt32 messageType);
	bool IsNativeA8RendererAvailable();
	NativeA8PacketPrepareResult PrepareNativeA8PacketBuffer(
		NativeA8Packet& packet, TileShader* shader, bool& rebuilt,
		bool useStockPrecache);
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

	void RecordNativeA8Suppression(NiTriShape* shape,
		const A8ShapeMetadata& metadata, NativeA8FallbackReason reason,
		const char* phase);
	void MarkNativeA8RuntimeFault(NativeA8ShapePayload& payload,
		NativeA8FallbackReason reason);
}

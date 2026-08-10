#pragma once

#include "font_native_shape_internal.h"
#include "font_native_internal.h"

#include <atomic>
#include <vector>

namespace fonthook::vectorfont::implementation::font_native_command_buffer
{
	struct CommandTileShaderPropertyView : BSShaderProperty
	{
		NiTexturePtr sourceTexture;
		NiTexturePtr alphaTexture;
		NiColorA overlayColor;
		float tileAlpha = 1.0f;
		NiPoint4 textureTransform;
		NiTexturingProperty::ClampMode clampMode =
			NiTexturingProperty::CLAMP_S_CLAMP_T;
		bool byte90 = false;
		bool rotates = false;
		bool hasVertexColors = false;
		bool noTexture = false;
		BSStringT<char> texturePath;
		RECT scissorRect = {};
		bool useScissorTest = false;
	};

	static_assert(sizeof(CommandTileShaderPropertyView) == 0xB0);
	static_assert(offsetof(
		CommandTileShaderPropertyView, overlayColor) == 0x68);

	struct NativeFontExecutionSegmentState
	{
		UInt64 validationToken = 0;
		UInt32 boundaryEpoch = 0;
		UInt32 externalMutationEpoch = 0;
		NativeFontCommandFallback failure = NativeFontCommandFallback::State;
		bool validated = false;
	};

	struct NativeFontFrameCommandBuffer
	{
		NativeFontFrameStamp stamp;
		std::vector<NativeFontDrawCommand> commands;
		std::vector<NativeFontFrameCommandRun> runs;
		std::vector<NativeFontCommandSpan> spans;
		std::vector<NativeFontSinglePacketCommand> singlePacketCommands;
		std::vector<NativeFontDirectFacadeSinglePacketCommand>
			directFacadeSinglePacketCommands;
		CpuMemoryLease cpuMemory;
		NativeFontExecutionSegmentState executionSegment;
		UInt32 executionBoundaryEpoch = 1;
		UInt32 frameExternalMutationEpoch = 0;
		NativeFontCommandFallback executionBoundaryReason =
			NativeFontCommandFallback::State;
		size_t trackedCapacityBytes = 0;
		size_t highWaterCommands = 0;
		size_t highWaterRuns = 0;
		size_t highWaterSpans = 0;
		size_t highWaterSinglePackets = 0;
		size_t highWaterDirectFacadeSinglePackets = 0;
		bool enabled = false;
		bool active = false;
		bool building = false;
	};

	struct NativeFontCommandGlobalState
	{
		std::atomic<UInt32> externalMutationEpoch{ 1 };
		std::atomic<UInt8> externalMutationReason{
			static_cast<UInt8>(NativeFontCommandFallback::State) };
	};

	NativeFontFrameCommandBuffer& CommandBuffer();
	NativeFontCommandGlobalState& CommandGlobalState();

	NativeFontCommandFallback NormalizeMutationReason(
		NativeFontCommandFallback reason);
	UInt32 LoadExternalMutationEpoch();
	NativeFontCommandFallback LoadExternalMutationReason();
	void RecordSinglePacketCommandFallback(NativeFontCommandFallback reason);
	void RecordDirectFacadeSinglePacketCommandFallback(
		NativeFontCommandFallback reason);
	void AdvanceExecutionBoundaryEpoch(NativeFontFrameCommandBuffer& buffer,
		NativeFontCommandFallback reason);
	bool ValidateGeometryBinding(const NativeFontDrawCommand& command,
		NiTriShape* geometry);
	NativeFontCommandFallback ValidateDrawCommandState(
		const NativeFontFrameCommandBuffer& buffer,
		const NativeFontDrawCommand& command,
		const NativeFontShapePayload* expectedPayload,
		UInt32 expectedPacketIndex, NiTriShape* geometry,
		NiRenderer* renderer);
	bool CompileCompatibilityCommand(NiTriShape* facade,
		NativeFontShapePayload& payload,
		const NativeFontTileRetainedPacket& retained,
		const NativeFontFramePayloadBinding& payloadBinding,
		NativeFontDrawCommand& command);
	NativeFontCommandFallback EnsureExecutionSegmentValidated(
		NativeFontFrameCommandBuffer& buffer);
	NativeFontCommandFallback ValidateExecutionSegmentEpoch(
		NativeFontFrameCommandBuffer& buffer,
		UInt32 executionSegmentEpoch,
		UInt32 executionExternalMutationEpoch);
	NativeFontCommandFallback ValidatePacketExecutionGuard(
		NativeFontFrameCommandBuffer& buffer,
		NativeFontCommandSpanState state,
		UInt64 executionValidationToken,
		UInt32 executionSegmentEpoch,
		UInt32 executionExternalMutationEpoch,
		NiRenderer* renderer);
}

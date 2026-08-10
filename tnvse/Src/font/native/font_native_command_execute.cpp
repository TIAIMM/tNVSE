#include "font_native_command_detail.h"

#include "load_config.h"

#include "NiGeometryBufferData.hpp"
#include "NiMaterialProperty.hpp"
#include "NiDX9Renderer.hpp"
#include "NiRenderer.hpp"
#include "NiTriShapeData.hpp"
#include "NiVBChip.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace fonthook::vectorfont
{
	using namespace implementation::font_native_command_buffer;

	bool FindNativeFontSinglePacketCommand(UInt32 commandIndex,
		UInt64 validationToken, NativeFontSinglePacketCommandView& view)
	{
		view = {};
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		if (!buffer.active || !validationToken
			|| validationToken != buffer.stamp.validationToken
			|| commandIndex >= buffer.singlePacketCommands.size())
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandSinglePacketMiss);
			return false;
		}
		const NativeFontSinglePacketCommand& command =
			buffer.singlePacketCommands[commandIndex];
		if (command.validationToken != validationToken)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandSinglePacketMiss);
			return false;
		}
		view.stamp = &buffer.stamp;
		view.command = &command;
		view.commandIndex = commandIndex;
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandSinglePacketHit);
		return true;
	}

	bool BeginNativeFontSinglePacketCommandExecution(UInt32 commandIndex,
		NiTriShape* geometry, NativeFontSinglePacketCommandView& view)
	{
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		if (!FindNativeFontSinglePacketCommand(commandIndex,
				buffer.stamp.validationToken, view))
		{
			return false;
		}
		NativeFontCommandFallback failure =
			EnsureExecutionSegmentValidated(buffer);
		if (failure != NativeFontCommandFallback::None)
		{
			RecordSinglePacketCommandFallback(failure);
			return false;
		}

		NativeFontSinglePacketCommand& command =
			buffer.singlePacketCommands[commandIndex];
		if (command.validationToken != buffer.stamp.validationToken
			|| command.generation != buffer.stamp.generation
			|| command.atlasTextureEpoch
				!= buffer.stamp.atlasTextureEpoch
			|| !command.payload || !command.artifact
			|| !command.payload->payloadTemplate
			|| command.payload->payloadTemplate.get()
				!= command.artifact
			|| command.payload->useCompositePackets
				!= command.useCompositePackets
			|| command.draw.payload != command.payload
			|| !command.draw.packet
			|| command.draw.packetIndex != 0)
		{
			RecordSinglePacketCommandFallback(
				NativeFontCommandFallback::Topology);
			return false;
		}
		if (command.state != NativeFontCommandSpanState::Ready)
		{
			RecordSinglePacketCommandFallback(
				NativeFontCommandFallback::State);
			return false;
		}
		if (!geometry || command.facade != geometry
			|| command.draw.sourceGeometry != geometry)
		{
			command.state = NativeFontCommandSpanState::Fault;
			command.partialDraw = false;
			command.executionValidationToken = 0;
			command.executionSegmentEpoch = 0;
			command.executionExternalMutationEpoch = 0;
			RecordSinglePacketCommandFallback(
				NativeFontCommandFallback::Topology);
			return false;
		}

		command.state = NativeFontCommandSpanState::Executing;
		command.partialDraw = false;
		command.executionValidationToken =
			buffer.stamp.validationToken;
		command.executionSegmentEpoch =
			buffer.executionSegment.boundaryEpoch;
		command.executionExternalMutationEpoch =
			buffer.executionSegment.externalMutationEpoch;
		view.command = &command;
		return true;
	}

	void EndNativeFontSinglePacketCommandExecution(UInt32 commandIndex,
		bool success, bool drewPacket)
	{
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		if (commandIndex >= buffer.singlePacketCommands.size())
			return;
		NativeFontSinglePacketCommand& command =
			buffer.singlePacketCommands[commandIndex];
		command.partialDraw = drewPacket;
		command.executionValidationToken = 0;
		command.executionSegmentEpoch = 0;
		command.executionExternalMutationEpoch = 0;
		command.state = success
			? NativeFontCommandSpanState::Consumed
			: NativeFontCommandSpanState::Fault;
	}

	void AbandonNativeFontSinglePacketCommandExecution(UInt32 commandIndex)
	{
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		if (commandIndex >= buffer.singlePacketCommands.size())
			return;
		NativeFontSinglePacketCommand& command =
			buffer.singlePacketCommands[commandIndex];
		if (command.state != NativeFontCommandSpanState::Executing
			|| command.partialDraw)
		{
			return;
		}
		command.executionValidationToken = 0;
		command.executionSegmentEpoch = 0;
		command.executionExternalMutationEpoch = 0;
		command.state = NativeFontCommandSpanState::Ready;
	}

	bool IsNativeFontSinglePacketCommandConsumed(
		UInt32 commandIndex, UInt64 validationToken)
	{
		const NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		if (!validationToken
			|| validationToken != buffer.stamp.validationToken
			|| commandIndex >= buffer.singlePacketCommands.size())
		{
			return false;
		}
		const NativeFontSinglePacketCommand& command =
			buffer.singlePacketCommands[commandIndex];
		return command.validationToken == validationToken
			&& command.state == NativeFontCommandSpanState::Consumed
			&& command.partialDraw;
	}

	bool FindNativeFontCommandSpan(UInt32 spanIndex,
		UInt64 validationToken, NativeFontCommandSpanView& view)
	{
		view = {};
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		if (!buffer.active || !validationToken
			|| validationToken != buffer.stamp.validationToken
			|| spanIndex >= buffer.spans.size())
		{
			RecordFreeTypePerf(FreeTypePerfCounter::CommandSpanMiss);
			return false;
		}
		const NativeFontCommandSpan& span = buffer.spans[spanIndex];
		if (span.validationToken != validationToken
			|| span.firstCommand + span.commandCount
				> buffer.commands.size()
			|| span.firstRun + span.runCount > buffer.runs.size())
		{
			RecordFreeTypePerf(FreeTypePerfCounter::CommandSpanMiss);
			return false;
		}
		view.stamp = &buffer.stamp;
		view.span = &span;
		view.commands = buffer.commands.data();
		view.runs = buffer.runs.data();
		view.spanIndex = spanIndex;
		RecordFreeTypePerf(FreeTypePerfCounter::CommandSpanHit);
		return true;
	}

	bool BeginNativeFontCommandSpanExecution(UInt32 spanIndex,
		NiTriShape* geometry, NativeFontCommandSpanView& view)
	{
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		if (!FindNativeFontCommandSpan(spanIndex,
				buffer.stamp.validationToken, view))
		{
			return false;
		}
		NativeFontCommandFallback failure =
			EnsureExecutionSegmentValidated(buffer);
		if (failure != NativeFontCommandFallback::None)
		{
			RecordNativeFontCommandFallback(failure);
			return false;
		}
		NativeFontCommandSpan& span = buffer.spans[spanIndex];
		if (span.validationToken != buffer.stamp.validationToken
			|| span.generation != buffer.stamp.generation
			|| span.atlasTextureEpoch
				!= buffer.stamp.atlasTextureEpoch
			|| !span.payload || !span.payload->payloadTemplate
			|| span.payload->useCompositePackets
				!= span.useCompositePackets)
		{
			RecordNativeFontCommandFallback(
				NativeFontCommandFallback::Topology);
			return false;
		}
		if (span.state != NativeFontCommandSpanState::Ready)
		{
			RecordNativeFontCommandFallback(
				NativeFontCommandFallback::State);
			return false;
		}
		if (span.facade != geometry)
		{
			span.state = NativeFontCommandSpanState::Fault;
			span.partialDraw = false;
			span.executionValidationToken = 0;
			span.executionSegmentEpoch = 0;
			span.executionExternalMutationEpoch = 0;
			RecordNativeFontCommandFallback(
				NativeFontCommandFallback::Topology);
			return false;
		}
		span.state = NativeFontCommandSpanState::Executing;
		span.partialDraw = false;
		span.executionValidationToken =
			buffer.stamp.validationToken;
		span.executionSegmentEpoch =
			buffer.executionSegment.boundaryEpoch;
		span.executionExternalMutationEpoch =
			buffer.executionSegment.externalMutationEpoch;
		view.span = &span;
		return true;
	}

	void EndNativeFontCommandSpanExecution(UInt32 spanIndex,
		bool success, bool drewPacket)
	{
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		if (spanIndex >= buffer.spans.size())
			return;
		NativeFontCommandSpan& span = buffer.spans[spanIndex];
		span.partialDraw = drewPacket;
		span.executionValidationToken = 0;
		span.executionSegmentEpoch = 0;
		span.executionExternalMutationEpoch = 0;
		span.state = success
			? NativeFontCommandSpanState::Consumed
			: NativeFontCommandSpanState::Fault;
	}

	bool ValidateNativeFontCommand(UInt32 spanIndex,
		UInt32 commandOffset, NiTriShape* geometry, NiRenderer* renderer)
	{
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandPacketLightValidation);
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		NativeFontCommandFallback commandFailure =
			NativeFontCommandFallback::None;
		if (!buffer.active || spanIndex >= buffer.spans.size())
		{
			commandFailure = NativeFontCommandFallback::State;
		}
		if (commandFailure != NativeFontCommandFallback::None)
		{
			RecordNativeFontCommandFallback(commandFailure);
			return false;
		}

		const NativeFontCommandSpan& span = buffer.spans[spanIndex];
		if (span.state != NativeFontCommandSpanState::Executing
			|| span.executionValidationToken
				!= buffer.stamp.validationToken)
		{
			commandFailure = NativeFontCommandFallback::State;
		}
		else if (commandOffset >= span.commandCount
			|| span.firstCommand + commandOffset
				>= buffer.commands.size())
		{
			commandFailure = NativeFontCommandFallback::Topology;
		}
		else
			commandFailure =
				ValidateExecutionSegmentEpoch(buffer,
					span.executionSegmentEpoch,
					span.executionExternalMutationEpoch);
		if (commandFailure != NativeFontCommandFallback::None)
		{
			RecordNativeFontCommandFallback(commandFailure);
			return false;
		}

		const NativeFontDrawCommand& command =
			buffer.commands[span.firstCommand + commandOffset];
		commandFailure = ValidateDrawCommandState(buffer, command,
			span.payload, commandOffset, geometry, renderer);
		if (commandFailure != NativeFontCommandFallback::None)
		{
			RecordNativeFontCommandFallback(commandFailure);
			return false;
		}
		return true;
	}

	bool GuardNativeFontCommand(UInt32 spanIndex,
		UInt32 commandOffset, NiTriShape* geometry, NiRenderer* renderer)
	{
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandPacketEpochGuard);
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		NativeFontCommandFallback commandFailure =
			NativeFontCommandFallback::None;
		if (!buffer.active || spanIndex >= buffer.spans.size())
		{
			commandFailure = NativeFontCommandFallback::State;
		}
		if (commandFailure != NativeFontCommandFallback::None)
		{
			RecordNativeFontCommandFallback(commandFailure);
			return false;
		}

		const NativeFontCommandSpan& span = buffer.spans[spanIndex];
		if (commandOffset >= span.commandCount
			|| span.firstCommand + commandOffset
				>= buffer.commands.size())
		{
			commandFailure = NativeFontCommandFallback::Topology;
		}
		else
		{
			commandFailure = ValidatePacketExecutionGuard(buffer,
				span.state, span.executionValidationToken,
				span.executionSegmentEpoch,
				span.executionExternalMutationEpoch, renderer);
		}
		if (commandFailure == NativeFontCommandFallback::None)
		{
			const NativeFontDrawCommand& command =
				buffer.commands[span.firstCommand + commandOffset];
			if (!geometry
				|| (command.expectedGeometry
					&& command.expectedGeometry != geometry))
			{
				commandFailure = NativeFontCommandFallback::Topology;
			}
		}
		if (commandFailure != NativeFontCommandFallback::None)
		{
			RecordNativeFontCommandFallback(commandFailure);
			return false;
		}
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandPacketStateValidationElided);
		return true;
	}

	bool ValidateNativeFontSinglePacketCommand(UInt32 commandIndex,
		NiTriShape* geometry, NiRenderer* renderer)
	{
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandPacketLightValidation);
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		NativeFontCommandFallback commandFailure =
			NativeFontCommandFallback::None;
		if (!buffer.active
			|| commandIndex >= buffer.singlePacketCommands.size())
		{
			commandFailure = NativeFontCommandFallback::State;
		}
		if (commandFailure != NativeFontCommandFallback::None)
		{
			RecordSinglePacketCommandFallback(commandFailure);
			return false;
		}

		const NativeFontSinglePacketCommand& command =
			buffer.singlePacketCommands[commandIndex];
		if (command.state != NativeFontCommandSpanState::Executing
			|| command.executionValidationToken
				!= buffer.stamp.validationToken)
		{
			commandFailure = NativeFontCommandFallback::State;
		}
		else
		{
			commandFailure = ValidateExecutionSegmentEpoch(buffer,
				command.executionSegmentEpoch,
				command.executionExternalMutationEpoch);
		}
		if (commandFailure == NativeFontCommandFallback::None)
		{
			commandFailure = ValidateDrawCommandState(buffer,
				command.draw, command.payload, 0,
				geometry, renderer);
		}
		if (commandFailure != NativeFontCommandFallback::None)
		{
			RecordSinglePacketCommandFallback(commandFailure);
			return false;
		}
		return true;
	}

	bool GuardNativeFontSinglePacketCommand(UInt32 commandIndex,
		NiTriShape* geometry, NiRenderer* renderer)
	{
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandPacketEpochGuard);
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		NativeFontCommandFallback commandFailure =
			NativeFontCommandFallback::None;
		if (!buffer.active
			|| commandIndex >= buffer.singlePacketCommands.size())
		{
			commandFailure = NativeFontCommandFallback::State;
		}
		if (commandFailure != NativeFontCommandFallback::None)
		{
			RecordSinglePacketCommandFallback(commandFailure);
			return false;
		}

		const NativeFontSinglePacketCommand& command =
			buffer.singlePacketCommands[commandIndex];
		commandFailure = ValidatePacketExecutionGuard(buffer,
			command.state, command.executionValidationToken,
			command.executionSegmentEpoch,
			command.executionExternalMutationEpoch, renderer);
		if (commandFailure == NativeFontCommandFallback::None
			&& (!geometry || command.facade != geometry
				|| command.draw.sourceGeometry != geometry))
		{
			commandFailure = NativeFontCommandFallback::Topology;
		}
		if (commandFailure != NativeFontCommandFallback::None)
		{
			RecordSinglePacketCommandFallback(commandFailure);
			return false;
		}
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandPacketStateValidationElided);
		return true;
	}

	bool FindNativeFontDirectFacadeSinglePacketCommand(UInt32 commandIndex,
		UInt64 validationToken,
		NativeFontDirectFacadeSinglePacketCommandView& view)
	{
		view = {};
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		if (!buffer.active || !validationToken
			|| validationToken != buffer.stamp.validationToken
			|| commandIndex
				>= buffer.directFacadeSinglePacketCommands.size())
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					CommandDirectFacadeSinglePacketMiss);
			return false;
		}
		const NativeFontDirectFacadeSinglePacketCommand& command =
			buffer.directFacadeSinglePacketCommands[commandIndex];
		if (command.validationToken != validationToken)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					CommandDirectFacadeSinglePacketMiss);
			return false;
		}
		view.stamp = &buffer.stamp;
		view.command = &command;
		view.commandIndex = commandIndex;
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandDirectFacadeSinglePacketHit);
		return true;
	}

	bool BeginNativeFontDirectFacadeSinglePacketCommandExecution(
		UInt32 commandIndex, const NativeFontShapeMetadata* singletonMetadata,
		NiTriShape* geometry,
		NativeFontDirectFacadeSinglePacketCommandView& view)
	{
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		if (!FindNativeFontDirectFacadeSinglePacketCommand(commandIndex,
				buffer.stamp.validationToken, view))
		{
			return false;
		}
		NativeFontCommandFallback failure =
			EnsureExecutionSegmentValidated(buffer);
		if (failure != NativeFontCommandFallback::None)
		{
			RecordDirectFacadeSinglePacketCommandFallback(failure);
			return false;
		}

		NativeFontDirectFacadeSinglePacketCommand& command =
			buffer.directFacadeSinglePacketCommands[commandIndex];
		SingletonFacadeState* singleton = singletonMetadata
			? GetSingletonFacadeState(*singletonMetadata) : nullptr;
		if (command.validationToken != buffer.stamp.validationToken
			|| command.generation != buffer.stamp.generation
			|| command.atlasTextureEpoch
				!= buffer.stamp.atlasTextureEpoch
			|| !singleton || !command.singletonMetadata
			|| command.singletonMetadata != singletonMetadata
			|| !command.payload || !command.artifact
			|| !command.payload->payloadTemplate
			|| command.payload->payloadTemplate.get()
				!= command.artifact
			|| command.payload->useCompositePackets
				!= command.useCompositePackets
			|| !command.draw
			|| command.draw->payload != command.payload
			|| !command.draw->packet
			|| command.draw->packetIndex != 0
			|| command.draw->sourceGeometry != command.geometry
			|| command.draw->expectedGeometry != command.geometry
			|| singleton->commandBuildValidationToken.load(
				std::memory_order_acquire)
				!= buffer.stamp.validationToken
			|| singleton->preparedValidationToken
				!= buffer.stamp.validationToken
			|| singleton->preparedGeneration != buffer.stamp.generation
			|| singleton->preparedAtlasTextureEpoch
				!= buffer.stamp.atlasTextureEpoch
			|| singleton->commandValidationToken.load(
				std::memory_order_acquire)
				!= buffer.stamp.validationToken
			|| singleton->commandDirectFacadeSinglePacketIndex.load(
				std::memory_order_acquire) != commandIndex
			|| singleton->frameMode.load(std::memory_order_acquire)
				!= SingletonFacadeFrameMode::Direct)
		{
			RecordDirectFacadeSinglePacketCommandFallback(
				NativeFontCommandFallback::Topology);
			return false;
		}
		if (command.state != NativeFontCommandSpanState::Ready)
		{
			RecordDirectFacadeSinglePacketCommandFallback(
				NativeFontCommandFallback::State);
			return false;
		}
		if (!geometry || command.geometry != geometry)
		{
			command.state = NativeFontCommandSpanState::Fault;
			command.partialDraw = false;
			command.executionValidationToken = 0;
			command.executionSegmentEpoch = 0;
			command.executionExternalMutationEpoch = 0;
			RecordDirectFacadeSinglePacketCommandFallback(
				NativeFontCommandFallback::Topology);
			return false;
		}

		command.state = NativeFontCommandSpanState::Executing;
		command.partialDraw = false;
		command.executionValidationToken =
			buffer.stamp.validationToken;
		command.executionSegmentEpoch =
			buffer.executionSegment.boundaryEpoch;
		command.executionExternalMutationEpoch =
			buffer.executionSegment.externalMutationEpoch;
		view.command = &command;
		return true;
	}

	void EndNativeFontDirectFacadeSinglePacketCommandExecution(
		UInt32 commandIndex, bool success, bool drewPacket)
	{
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		if (commandIndex
			>= buffer.directFacadeSinglePacketCommands.size())
		{
			return;
		}
		NativeFontDirectFacadeSinglePacketCommand& command =
			buffer.directFacadeSinglePacketCommands[commandIndex];
		command.partialDraw = drewPacket;
		command.executionValidationToken = 0;
		command.executionSegmentEpoch = 0;
		command.executionExternalMutationEpoch = 0;
		command.state = success
			? NativeFontCommandSpanState::Consumed
			: NativeFontCommandSpanState::Fault;
	}

	void AbandonNativeFontDirectFacadeSinglePacketCommandExecution(
		UInt32 commandIndex)
	{
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		if (commandIndex >= buffer.directFacadeSinglePacketCommands.size())
			return;
		NativeFontDirectFacadeSinglePacketCommand& command =
			buffer.directFacadeSinglePacketCommands[commandIndex];
		if (command.state != NativeFontCommandSpanState::Executing
			|| command.partialDraw)
		{
			return;
		}
		command.executionValidationToken = 0;
		command.executionSegmentEpoch = 0;
		command.executionExternalMutationEpoch = 0;
		command.state = NativeFontCommandSpanState::Ready;
	}

	bool IsNativeFontDirectFacadeSinglePacketCommandConsumed(
		UInt32 commandIndex, UInt64 validationToken)
	{
		const NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		if (!validationToken
			|| validationToken != buffer.stamp.validationToken
			|| commandIndex >= buffer.directFacadeSinglePacketCommands.size())
		{
			return false;
		}
		const NativeFontDirectFacadeSinglePacketCommand& command =
			buffer.directFacadeSinglePacketCommands[commandIndex];
		return command.validationToken == validationToken
			&& command.state == NativeFontCommandSpanState::Consumed
			&& command.partialDraw;
	}

	bool ValidateNativeFontDirectFacadeSinglePacketCommand(UInt32 commandIndex,
		NiTriShape* geometry, NiRenderer* renderer)
	{
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandPacketLightValidation);
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		NativeFontCommandFallback commandFailure =
			NativeFontCommandFallback::None;
		if (!buffer.active
			|| commandIndex
				>= buffer.directFacadeSinglePacketCommands.size())
		{
			commandFailure = NativeFontCommandFallback::State;
		}
		if (commandFailure != NativeFontCommandFallback::None)
		{
			RecordDirectFacadeSinglePacketCommandFallback(commandFailure);
			return false;
		}

		const NativeFontDirectFacadeSinglePacketCommand& command =
			buffer.directFacadeSinglePacketCommands[commandIndex];
		SingletonFacadeState* singleton = command.singletonMetadata
			? GetSingletonFacadeState(*command.singletonMetadata)
			: nullptr;
		if (command.state != NativeFontCommandSpanState::Executing
			|| command.executionValidationToken
				!= buffer.stamp.validationToken)
		{
			commandFailure = NativeFontCommandFallback::State;
		}
		else
		{
			commandFailure = ValidateExecutionSegmentEpoch(buffer,
				command.executionSegmentEpoch,
				command.executionExternalMutationEpoch);
		}
		if (commandFailure == NativeFontCommandFallback::None
			&& (!singleton
				|| singleton->commandValidationToken.load(
					std::memory_order_acquire)
					!= buffer.stamp.validationToken
				|| singleton->commandDirectFacadeSinglePacketIndex.load(
						std::memory_order_acquire) != commandIndex
				|| singleton->frameMode.load(
					std::memory_order_acquire)
					!= SingletonFacadeFrameMode::Direct))
		{
			commandFailure = NativeFontCommandFallback::Topology;
		}
		if (commandFailure == NativeFontCommandFallback::None)
		{
			commandFailure = ValidateDrawCommandState(buffer,
				*command.draw, command.payload, 0,
				geometry, renderer);
		}
		if (commandFailure != NativeFontCommandFallback::None)
		{
			RecordDirectFacadeSinglePacketCommandFallback(commandFailure);
			return false;
		}
		return true;
	}

	bool GuardNativeFontDirectFacadeSinglePacketCommand(UInt32 commandIndex,
		NiTriShape* geometry, NiRenderer* renderer)
	{
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandPacketEpochGuard);
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		NativeFontCommandFallback commandFailure =
			NativeFontCommandFallback::None;
		if (!buffer.active
			|| commandIndex
				>= buffer.directFacadeSinglePacketCommands.size())
		{
			commandFailure = NativeFontCommandFallback::State;
		}
		if (commandFailure != NativeFontCommandFallback::None)
		{
			RecordDirectFacadeSinglePacketCommandFallback(commandFailure);
			return false;
		}

		const NativeFontDirectFacadeSinglePacketCommand& command =
			buffer.directFacadeSinglePacketCommands[commandIndex];
		SingletonFacadeState* singleton = command.singletonMetadata
			? GetSingletonFacadeState(*command.singletonMetadata)
			: nullptr;
		commandFailure = ValidatePacketExecutionGuard(buffer,
			command.state, command.executionValidationToken,
			command.executionSegmentEpoch,
			command.executionExternalMutationEpoch, renderer);
		if (commandFailure == NativeFontCommandFallback::None
			&& (!singleton
				|| singleton->frameMode.load(std::memory_order_acquire)
					!= SingletonFacadeFrameMode::Direct
				|| !geometry || command.geometry != geometry
				|| !command.draw
				|| command.draw->sourceGeometry != geometry
				|| command.draw->expectedGeometry != geometry))
		{
			commandFailure = NativeFontCommandFallback::Topology;
		}
		if (commandFailure != NativeFontCommandFallback::None)
		{
			RecordDirectFacadeSinglePacketCommandFallback(commandFailure);
			return false;
		}
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandPacketStateValidationElided);
		return true;
	}
}

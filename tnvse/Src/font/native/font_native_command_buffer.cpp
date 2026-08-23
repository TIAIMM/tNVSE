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

	namespace implementation::font_native_command_buffer
	{
		NativeFontFrameCommandBuffer& CommandBuffer()
		{
			thread_local NativeFontFrameCommandBuffer buffer;
			return buffer;
		}

		NativeFontCommandGlobalState& CommandGlobalState()
		{
			static NativeFontCommandGlobalState state;
			return state;
		}

		NativeFontCommandFallback NormalizeMutationReason(
			NativeFontCommandFallback reason)
		{
			return reason == NativeFontCommandFallback::None
				? NativeFontCommandFallback::State : reason;
		}

		UInt32 LoadExternalMutationEpoch()
		{
			return CommandGlobalState().externalMutationEpoch.load(std::memory_order_acquire);
		}

		NativeFontCommandFallback LoadExternalMutationReason()
		{
			return NormalizeMutationReason(
				static_cast<NativeFontCommandFallback>(
					CommandGlobalState().externalMutationReason.load(
						std::memory_order_acquire)));
		}

		void RecordSinglePacketCommandFallback(
			NativeFontCommandFallback reason)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandSinglePacketFallback);
			RecordNativeFontCommandFallback(reason);
		}

		void RecordDirectFacadeSinglePacketCommandFallback(
			NativeFontCommandFallback reason)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					CommandDirectFacadeSinglePacketFallback);
			RecordNativeFontCommandFallback(reason);
		}

		void AdvanceExecutionBoundaryEpoch(
			NativeFontFrameCommandBuffer& buffer,
			NativeFontCommandFallback reason)
		{
			NativeFontExecutionSegmentState& segment =
				buffer.executionSegment;
			if (!buffer.active || !segment.validated
				|| segment.boundaryEpoch
					!= buffer.executionBoundaryEpoch)
			{
				return;
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandSegmentInvalidation);
			if (++buffer.executionBoundaryEpoch == 0)
				++buffer.executionBoundaryEpoch;
			buffer.executionBoundaryReason =
				NormalizeMutationReason(reason);
			segment.validated = false;
		}

		void RefreshCommandMemory(NativeFontFrameCommandBuffer& buffer)
		{
			const size_t bytes =
				buffer.commands.capacity() * sizeof(NativeFontDrawCommand)
				+ buffer.runs.capacity()
					* sizeof(NativeFontFrameCommandRun)
				+ buffer.spans.capacity() * sizeof(NativeFontCommandSpan)
				+ buffer.singlePacketCommands.capacity()
					* sizeof(NativeFontSinglePacketCommand)
				+ buffer.directFacadeSinglePacketCommands.capacity()
					* sizeof(NativeFontDirectFacadeSinglePacketCommand);
			if (bytes == buffer.trackedCapacityBytes)
				return;
			buffer.trackedCapacityBytes = bytes;
			buffer.cpuMemory.Reset(
				CpuMemoryCategory::RuntimeMetadata, bytes);
		}

		void RecordCommandVectorGrowth(
			size_t previousCapacity, size_t currentCapacity)
		{
			if (currentCapacity > previousCapacity)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandBuildVectorGrowth);
			}
		}

		template <class T>
		void ReserveCommandVector(std::vector<T>& values, size_t count)
		{
			if (count <= values.capacity())
				return;
			const size_t previousCapacity = values.capacity();
			values.reserve(count);
			RecordCommandVectorGrowth(
				previousCapacity, values.capacity());
		}

		void UpdateCommandHighWater(NativeFontFrameCommandBuffer& buffer)
		{
			buffer.highWaterCommands = std::max(
				buffer.highWaterCommands, buffer.commands.size());
			buffer.highWaterRuns = std::max(
				buffer.highWaterRuns, buffer.runs.size());
			buffer.highWaterSpans = std::max(
				buffer.highWaterSpans, buffer.spans.size());
			buffer.highWaterSinglePackets = std::max(
				buffer.highWaterSinglePackets,
				buffer.singlePacketCommands.size());
			buffer.highWaterDirectFacadeSinglePackets = std::max(
				buffer.highWaterDirectFacadeSinglePackets,
				buffer.directFacadeSinglePacketCommands.size());
		}

		void TrimCommandCapacity(NativeFontFrameCommandBuffer& buffer)
		{
			if (buffer.commands.capacity() > 16384)
			{
				std::vector<NativeFontDrawCommand>().swap(buffer.commands);
				buffer.highWaterCommands = 0;
			}
			if (buffer.runs.capacity() > 8192)
			{
				std::vector<NativeFontFrameCommandRun>().swap(buffer.runs);
				buffer.highWaterRuns = 0;
			}
			if (buffer.spans.capacity() > 8192)
			{
				std::vector<NativeFontCommandSpan>().swap(buffer.spans);
				buffer.highWaterSpans = 0;
			}
			if (buffer.singlePacketCommands.capacity() > 8192)
			{
				std::vector<NativeFontSinglePacketCommand>().swap(
					buffer.singlePacketCommands);
				buffer.highWaterSinglePackets = 0;
			}
			if (buffer.directFacadeSinglePacketCommands.capacity() > 8192)
			{
				std::vector<NativeFontDirectFacadeSinglePacketCommand>().swap(
					buffer.directFacadeSinglePacketCommands);
				buffer.highWaterDirectFacadeSinglePackets = 0;
			}
			RefreshCommandMemory(buffer);
			if (!IsCpuMemoryBudgetExceeded())
				return;
			std::vector<NativeFontDrawCommand>().swap(buffer.commands);
			std::vector<NativeFontFrameCommandRun>().swap(buffer.runs);
			std::vector<NativeFontCommandSpan>().swap(buffer.spans);
			std::vector<NativeFontSinglePacketCommand>().swap(
				buffer.singlePacketCommands);
			std::vector<NativeFontDirectFacadeSinglePacketCommand>().swap(
				buffer.directFacadeSinglePacketCommands);
			buffer.highWaterCommands = 0;
			buffer.highWaterRuns = 0;
			buffer.highWaterSpans = 0;
			buffer.highWaterSinglePackets = 0;
			buffer.highWaterDirectFacadeSinglePackets = 0;
			buffer.trackedCapacityBytes = 0;
			buffer.cpuMemory.Release();
		}

		bool ResolveRenderContextStamp(NativeFontFrameStamp& stamp)
		{
			if (!stamp.renderer || !stamp.device
				|| stamp.renderer->GetD3DDevice() != stamp.device)
				return false;
			stamp.renderTargetGroup = nullptr;
			stamp.viewport = {};
			stamp.renderTargetReady = false;
			stamp.viewportReady = false;
			NiRenderTargetGroup* renderTargetGroup =
				stamp.renderer->m_pkCurrRenderTargetGroup;
			const D3DVIEWPORT9 viewport = stamp.renderer->m_kD3DPort;
			if (!renderTargetGroup || !viewport.Width || !viewport.Height)
				return false;
			stamp.renderTargetGroup = renderTargetGroup;
			stamp.viewport = viewport;
			stamp.renderTargetReady = true;
			stamp.viewportReady = true;
			return true;
		}

		bool ValidateGeometryBinding(
			const NativeFontDrawCommand& command,
			NiTriShape* geometry)
		{
			if (!geometry
				|| (command.expectedGeometry
					&& command.expectedGeometry != geometry))
			{
				return false;
			}
			if (!command.program
				|| geometry->GetShader() != command.program->shader)
				return false;
			NiTriShapeData* data = geometry->GetModelData();
			NiGeometryBufferData* buffer =
				data ? data->m_pkBuffData : nullptr;
			NiVBChip* chip =
				buffer && buffer->m_uiStreamCount
					&& buffer->m_ppkVBChip
					? buffer->m_ppkVBChip[0] : nullptr;
			if (!buffer || !chip || !command.binding.active
				|| buffer->m_hDeclaration
					!= command.binding.declaration
				|| buffer->m_pkIB != command.binding.indexBuffer
				|| chip->m_pkVB != command.binding.vertexBuffer)
			{
				return false;
			}
			if (buffer->m_uiBaseVertexIndex
					!= command.binding.baseVertex
				|| buffer->m_uiVertCount
					!= command.binding.vertexCount
				|| buffer->m_uiMaxVertCount
					!= command.binding.vertexCount
				|| buffer->m_uiStreamCount != 1
				|| !buffer->m_puiVertexStride
				|| buffer->m_puiVertexStride[0]
					!= sizeof(NativeFontGpuVertex)
				|| buffer->m_uiIBSize
					!= command.binding.indexBytes
				|| buffer->m_uiIndexCount
					!= command.binding.vertexCount / 4u * 6u
				|| buffer->m_eType != D3DPT_TRIANGLELIST
				|| buffer->m_uiTriCount
					!= command.binding.vertexCount / 4u * 2u
				|| buffer->m_uiMaxTriCount
					!= command.binding.vertexCount / 4u * 2u
				|| buffer->m_uiNumArrays != 1
				|| chip->m_uiOffset != 0
				|| chip->m_uiSize
					!= command.binding.vertexCount
						* sizeof(NativeFontGpuVertex))
			{
				return false;
			}
			return true;
		}

		NativeFontCommandFallback ValidateDrawCommandState(
			const NativeFontFrameCommandBuffer& buffer,
			const NativeFontDrawCommand& command,
			const NativeFontShapePayload* expectedPayload,
			UInt32 expectedPacketIndex, NiTriShape* geometry,
			NiRenderer* renderer)
		{
			if (!command.program || !command.program->active
				|| command.program->generation
					!= buffer.stamp.generation
				|| command.program->device != buffer.stamp.device
				|| !renderer || renderer != buffer.stamp.renderer)
			{
				return NativeFontCommandFallback::Generation;
			}
			if (!command.payload
				|| command.payload != expectedPayload
				|| !command.packet
				|| command.packetIndex != expectedPacketIndex
				|| expectedPacketIndex
					>= command.payload->packetShaders.size()
				|| command.program->shader
					!= command.payload->packetShaders[
						expectedPacketIndex])
			{
				return NativeFontCommandFallback::Topology;
			}
			if (command.payload->preflightAtlasTextureEpoch
					!= buffer.stamp.atlasTextureEpoch)
			{
				return NativeFontCommandFallback::Atlas;
			}
			if (!ValidateGeometryBinding(command, geometry))
				return NativeFontCommandFallback::Resource;
			return NativeFontCommandFallback::None;
		}

		bool CompileCompatibilityCommand(
			NiTriShape* facade, NativeFontShapePayload& payload,
			const NativeFontTileRetainedPacket& retained,
			const NativeFontFramePayloadBinding& payloadBinding,
			NativeFontDrawCommand& command)
		{
			command = {};
			command.sourceGeometry = facade;
			command.payload = &payload;
			command.packet = retained.packet;
			command.packetIndex = retained.packetIndex;
			command.program = retained.program;
			if (retained.packetIndex == 0
				&& payload.retainedText.packets.size() == 1)
			{
				command.standardPassLite =
					&payload.retainedText.standardPassLite;
			}
			if (!retained.packet || !retained.program
				|| retained.atlasPage
					>= payload.preflightAtlasTextures.size())
				return false;
			const UInt64 baseVertex =
				static_cast<UInt64>(
					payloadBinding.payloadBaseVertex)
					+ retained.firstVertex;
			command.atlasTexture =
				payload.preflightAtlasTextures[retained.atlasPage];
			if (!command.atlasTexture
				|| !payloadBinding.active
				|| !retained.vertexCount
				|| (retained.vertexCount & 3u)
				|| static_cast<UInt64>(retained.firstVertex)
						+ retained.vertexCount
					> payloadBinding.payloadVertexCount
				|| baseVertex
					> std::numeric_limits<UInt32>::max())
			{
				return false;
			}
			command.binding.vertexBuffer =
				payloadBinding.vertexBuffer;
			command.binding.indexBuffer =
				payloadBinding.indexBuffer;
			command.binding.declaration =
				payloadBinding.declaration;
			command.binding.baseVertex =
				static_cast<UInt32>(baseVertex);
			command.binding.vertexCount = retained.vertexCount;
			command.binding.indexBytes =
				payloadBinding.indexBytes;
			command.binding.generation =
				payloadBinding.generation;
			command.binding.resourceSerial =
				payloadBinding.resourceSerial;
			command.binding.uploadEpoch =
				payloadBinding.uploadEpoch;
			command.binding.staticResident =
				payloadBinding.staticResident;
			command.binding.active = payloadBinding.active;
			return true;
		}

		bool AppendCompatibilityCommand(
			NativeFontFrameCommandBuffer& buffer,
			NiTriShape* facade, NativeFontShapePayload& payload,
			const NativeFontTileRetainedPacket& retained,
			const NativeFontFramePayloadBinding& payloadBinding)
		{
			const size_t previousCapacity = buffer.commands.capacity();
			buffer.commands.emplace_back();
			RecordCommandVectorGrowth(
				previousCapacity, buffer.commands.capacity());
			NativeFontDrawCommand& command = buffer.commands.back();
			if (!CompileCompatibilityCommand(facade, payload,
					retained, payloadBinding, command))
			{
				buffer.commands.pop_back();
				return false;
			}
			return true;
		}

		const NativeFontTileRetainedText* ResolveTileRetainedText(
			const NativeFontFrameCommandBuffer& buffer,
			NiTriShape* ownerTile, NativeFontShapePayload& payload,
			bool recordResult = true)
		{
			if (!IsNativeFontTileRetainedTextCurrent(payload,
					ownerTile, buffer.stamp.generation,
					buffer.stamp.atlasTextureEpoch))
			{
				if (recordResult)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							CommandTileRetainedMiss);
				}
				return nullptr;
			}
			if (recordResult)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandTileRetainedHit);
			}
			return &payload.retainedText;
		}

		bool AdoptFrameResourceStamp(
			NativeFontFrameCommandBuffer& buffer,
			UInt32 resourceSerial, UInt32 uploadEpoch)
		{
			if (!resourceSerial
				|| (buffer.stamp.resourceSerial
					&& (buffer.stamp.resourceSerial != resourceSerial
						|| buffer.stamp.uploadEpoch != uploadEpoch)))
			{
				return false;
			}
			if (!buffer.stamp.resourceSerial)
			{
				buffer.stamp.resourceSerial = resourceSerial;
				buffer.stamp.uploadEpoch = uploadEpoch;
			}
			return true;
		}

		template <class RetainedRunContainer>
		bool BuildFrameRuns(NativeFontFrameCommandBuffer& buffer,
			NativeFontCommandSpan& span,
			const RetainedRunContainer& retainedRuns,
			bool retainedBridgeEligible)
		{
			UInt32 coveredPackets = 0;
			for (const NativeFontTileRetainedRun& retained : retainedRuns)
			{
				if (!retained.packetCount
					|| retained.firstPacket != coveredPackets
					|| retained.firstPacket + retained.packetCount
						> span.commandCount)
				{
					return false;
				}
				UInt32 first = retained.firstPacket;
				const UInt32 retainedEnd =
					retained.firstPacket + retained.packetCount;
				while (first < retainedEnd)
				{
					const NativeFontDrawCommand& firstCommand =
						buffer.commands[span.firstCommand + first];
					UInt32 end = first + 1;
					while (end < retainedEnd)
					{
						const NativeFontDrawCommand& candidate =
							buffer.commands[span.firstCommand + end];
						++end;
					}
					NativeFontFrameCommandRun run;
					run.firstCommand = span.firstCommand + first;
					run.commandCount = end - first;
					run.bridgeEligible = retained.bridgeEligible;
					if (buffer.runs.size() > span.firstRun)
					{
						const NativeFontFrameCommandRun& previous =
							buffer.runs.back();
						const NativeFontDrawCommand& previousCommand =
							buffer.commands[
								previous.firstCommand
									+ previous.commandCount - 1u];
						run.continuesBridgeSpan =
							retained.continuesBridgeSpan
							|| first != retained.firstPacket;
					}
					buffer.runs.push_back(run);
					first = end;
				}
				coveredPackets += retained.packetCount;
			}
			span.bridgeEligible =
				span.commandCount > 1 && retainedBridgeEligible;
			return coveredPackets == span.commandCount;
		}

		bool ValidateRenderContext(
			const NativeFontFrameStamp& stamp)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandRenderTargetValidation);
			if (!stamp.renderTargetReady || !stamp.viewportReady
				|| !stamp.renderer || !stamp.device
				|| !stamp.renderTargetGroup)
			{
				return false;
			}
			return stamp.renderer->GetD3DDevice() == stamp.device
				&& stamp.renderer->m_pkCurrRenderTargetGroup
					== stamp.renderTargetGroup
				&& std::memcmp(&stamp.renderer->m_kD3DPort, &stamp.viewport,
					sizeof(stamp.viewport)) == 0;
		}

		NativeFontCommandFallback ValidateExecutionSegmentState(
			NativeFontFrameCommandBuffer& buffer,
			UInt32 expectedExternalMutationEpoch)
		{
			NativeFontFrameStamp& stamp = buffer.stamp;
			if (!buffer.frameExternalMutationEpoch
				|| expectedExternalMutationEpoch
					!= buffer.frameExternalMutationEpoch)
			{
				return LoadExternalMutationReason();
			}
			if (!stamp.accumulator || !stamp.validationToken
				|| stamp.validationToken
					!= GetNativeFontSortedFrameValidationToken())
			{
				return NativeFontCommandFallback::Token;
			}
			if (stamp.nestedTraversalSerial
				!= GetNativeFontSortedNestedTraversalSerial())
			{
				return NativeFontCommandFallback::Nested;
			}
			NativeFontRuntimeReadinessView readiness;
			if (!GetNativeFontRuntimeReadinessCurrent(readiness))
			{
				return GetNativeFontAtlasTextureEpoch()
						!= stamp.atlasTextureEpoch
					? NativeFontCommandFallback::Atlas
					: NativeFontCommandFallback::Hook;
			}
			if (readiness.renderer != stamp.renderer
				|| readiness.device != stamp.device
				|| readiness.generation != stamp.generation
				|| !IsNativeFontShaderGenerationCurrent(stamp.generation))
			{
				return NativeFontCommandFallback::Generation;
			}
			if (readiness.atlasTextureEpoch != stamp.atlasTextureEpoch)
				return NativeFontCommandFallback::Atlas;
			if (!IsNativeFontFrameResourceStampCurrent(
				stamp.generation, stamp.resourceSerial,
				stamp.uploadEpoch))
			{
				return NativeFontCommandFallback::Resource;
			}
			if (!stamp.renderTargetReady || !stamp.viewportReady)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						CommandDeferredRenderTargetCapture);
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						CommandRenderTargetValidation);
				if (!ResolveRenderContextStamp(stamp))
				{
					return NativeFontCommandFallback::RenderTarget;
				}
			}
			else if (!ValidateRenderContext(stamp))
			{
				return NativeFontCommandFallback::RenderTarget;
			}
			if (LoadExternalMutationEpoch()
				!= expectedExternalMutationEpoch)
			{
				return LoadExternalMutationReason();
			}
			return NativeFontCommandFallback::None;
		}

		NativeFontCommandFallback EnsureExecutionSegmentValidated(
			NativeFontFrameCommandBuffer& buffer)
		{
			NativeFontExecutionSegmentState& segment =
				buffer.executionSegment;
			const UInt32 externalMutationEpoch =
				LoadExternalMutationEpoch();
			if (segment.validated
				&& segment.validationToken
					== buffer.stamp.validationToken
				&& segment.boundaryEpoch
					== buffer.executionBoundaryEpoch
				&& segment.externalMutationEpoch
					== externalMutationEpoch)
			{
				if (segment.failure
					== NativeFontCommandFallback::None)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							CommandSegmentValidationReuse);
				}
				return segment.failure;
			}

			if (segment.validated
				&& segment.boundaryEpoch
					== buffer.executionBoundaryEpoch
				&& segment.externalMutationEpoch
					!= externalMutationEpoch)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandSegmentInvalidation);
			}

			segment = {};
			segment.validationToken = buffer.stamp.validationToken;
			segment.boundaryEpoch = buffer.executionBoundaryEpoch;
			segment.externalMutationEpoch = externalMutationEpoch;
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandSegmentFullValidation);
			segment.failure = ValidateExecutionSegmentState(
				buffer, externalMutationEpoch);
			if (LoadExternalMutationEpoch()
				!= externalMutationEpoch)
			{
				segment.failure = LoadExternalMutationReason();
				segment.validated = false;
				return segment.failure;
			}
			segment.validated = true;
			if (segment.failure == NativeFontCommandFallback::None)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandExecutionSegment);
			}
			return segment.failure;
		}

		NativeFontCommandFallback ValidateExecutionSegmentEpoch(
			NativeFontFrameCommandBuffer& buffer,
			UInt32 executionSegmentEpoch,
			UInt32 executionExternalMutationEpoch)
		{
			const NativeFontExecutionSegmentState& segment =
				buffer.executionSegment;
			if (!segment.validated
				|| segment.failure != NativeFontCommandFallback::None
				|| executionSegmentEpoch
					!= buffer.executionBoundaryEpoch
				|| executionSegmentEpoch
					!= segment.boundaryEpoch)
			{
				return buffer.executionBoundaryReason;
			}
			const UInt32 externalMutationEpoch =
				LoadExternalMutationEpoch();
			if (executionExternalMutationEpoch
					!= externalMutationEpoch
				|| segment.externalMutationEpoch
					!= externalMutationEpoch)
			{
				const NativeFontCommandFallback reason =
					LoadExternalMutationReason();
				AdvanceExecutionBoundaryEpoch(buffer, reason);
				return reason;
			}
			return NativeFontCommandFallback::None;
		}

		NativeFontCommandFallback ValidatePacketExecutionGuard(
			NativeFontFrameCommandBuffer& buffer,
			NativeFontCommandSpanState state,
			UInt64 executionValidationToken,
			UInt32 executionSegmentEpoch,
			UInt32 executionExternalMutationEpoch,
			NiRenderer* renderer)
		{
			if (state != NativeFontCommandSpanState::Executing
				|| executionValidationToken
					!= buffer.stamp.validationToken)
			{
				return NativeFontCommandFallback::State;
			}
			if (!renderer || renderer != buffer.stamp.renderer)
				return NativeFontCommandFallback::Generation;
			return ValidateExecutionSegmentEpoch(buffer,
				executionSegmentEpoch,
				executionExternalMutationEpoch);
		}
	}

	void NotifyNativeFontCommandExternalMutation(
		NativeFontCommandFallback reason)
	{
		CommandGlobalState().externalMutationReason.store(static_cast<UInt8>(
			NormalizeMutationReason(reason)), std::memory_order_relaxed);
		UInt32 current =
			CommandGlobalState().externalMutationEpoch.load(std::memory_order_relaxed);
		for (;;)
		{
			UInt32 next = current + 1u;
			if (!next)
				next = 1u;
			if (CommandGlobalState().externalMutationEpoch.compare_exchange_weak(
				current, next, std::memory_order_release,
				std::memory_order_relaxed))
			{
				return;
			}
		}
	}

	void InvalidateNativeFontCommandExecutionSegment(
		NativeFontCommandFallback reason)
	{
		AdvanceExecutionBoundaryEpoch(
			CommandBuffer(), NormalizeMutationReason(reason));
	}

	void BeginNativeFontFrameCommandBuffer(BSShaderAccumulator* accumulator,
		UInt64 validationToken, UInt32 generation, UInt32 atlasTextureEpoch)
	{
		EndNativeFontFrameCommandBuffer();
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		buffer.enabled = IsFreeTypeFontCommandBufferEnabledForCurrentRoute();
		if (!buffer.enabled || !g_bEnableFreeTypeFontRendering
			|| !g_bEnableFreeTypeNativeAtlas || !accumulator
			|| !validationToken || !generation)
		{
			return;
		}
		buffer.stamp.accumulator = accumulator;
		buffer.stamp.validationToken = validationToken;
		buffer.stamp.generation = generation;
		buffer.stamp.atlasTextureEpoch = atlasTextureEpoch;
		buffer.stamp.nestedTraversalSerial =
			GetNativeFontSortedNestedTraversalSerial();
		buffer.stamp.renderer = NiDX9Renderer::GetSingleton();
		buffer.stamp.device = buffer.stamp.renderer
			? buffer.stamp.renderer->GetD3DDevice() : nullptr;
		if (!buffer.stamp.device)
			return;
		buffer.frameExternalMutationEpoch =
			LoadExternalMutationEpoch();
		buffer.building = true;
	}

	void ReserveNativeFontFrameCommandBuffer(size_t ordinaryEntryCount,
		size_t directFacadeCount)
	{
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		if (!buffer.building)
			return;

		ReserveCommandVector(buffer.commands,
			buffer.highWaterCommands);
		ReserveCommandVector(buffer.runs,
			buffer.highWaterRuns);
		ReserveCommandVector(buffer.spans,
			buffer.highWaterSpans);
		ReserveCommandVector(buffer.singlePacketCommands,
			std::max(buffer.highWaterSinglePackets,
				ordinaryEntryCount));
		ReserveCommandVector(buffer.directFacadeSinglePacketCommands,
			std::max(buffer.highWaterDirectFacadeSinglePackets,
				directFacadeCount));
		RefreshCommandMemory(buffer);
	}

	UInt32 AddNativeFontFrameSinglePacketCommand(NiTriShape* facade,
		const NativeFontShapeMetadata* metadata, NativeFontShapePayload* payload)
	{
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		if (!buffer.building || !facade || !metadata || !payload
			|| !payload->buildComplete || !payload->payloadTemplate)
		{
			return kInvalidNativeFontCommandIndex;
		}

		if (GetNativeFontPackets(*payload->payloadTemplate,
				payload->useCompositePackets).size() != 1)
		{
			return kInvalidNativeFontCommandIndex;
		}
		const NativeFontTileRetainedText* retainedText =
			ResolveTileRetainedText(buffer, facade, *payload);
		if (!retainedText || retainedText->packets.size() != 1)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					CommandSinglePacketBuildFallback);
			return kInvalidNativeFontCommandIndex;
		}

		NativeFontFramePayloadBinding payloadBinding;
		if (!ResolveNativeFontFramePayloadBinding(
				*payload, payloadBinding))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					CommandSinglePacketBuildFallback);
			return kInvalidNativeFontCommandIndex;
		}
		const UInt32 commandIndex = static_cast<UInt32>(
			buffer.singlePacketCommands.size());
		const size_t previousSingleCapacity =
			buffer.singlePacketCommands.capacity();
		buffer.singlePacketCommands.emplace_back();
		RecordCommandVectorGrowth(previousSingleCapacity,
			buffer.singlePacketCommands.capacity());
		NativeFontSinglePacketCommand& command =
			buffer.singlePacketCommands.back();
		command.facade = facade;
		command.payload = payload;
		command.artifact = payload->payloadTemplate.get();
		command.validationToken = buffer.stamp.validationToken;
		command.generation = buffer.stamp.generation;
		command.atlasTextureEpoch = buffer.stamp.atlasTextureEpoch;
		command.useCompositePackets = payload->useCompositePackets;
		if (!CompileCompatibilityCommand(facade, *payload,
				retainedText->packets.front(),
				payloadBinding, command.draw))
		{
			buffer.singlePacketCommands.pop_back();
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					CommandSinglePacketBuildFallback);
			return kInvalidNativeFontCommandIndex;
		}

		const UInt32 resourceSerial =
			command.draw.binding.resourceSerial;
		const UInt32 uploadEpoch = command.draw.binding.uploadEpoch;
		if (!AdoptFrameResourceStamp(
				buffer, resourceSerial, uploadEpoch))
		{
			buffer.singlePacketCommands.pop_back();
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					CommandSinglePacketBuildFallback);
			return kInvalidNativeFontCommandIndex;
		}

		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandSinglePacketRecorded);
		RecordFreeTypePerf(FreeTypePerfCounter::CommandPacketRecorded);
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandTileRetainedPacketReuse);
		return commandIndex;
	}

	UInt32 AddNativeFontFrameDirectFacadeCommand(
		const NativeFontShapeMetadata* metadata)
	{
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		SingletonFacadeState* singleton = metadata
			? GetSingletonFacadeState(*metadata) : nullptr;
		if (!buffer.building || !metadata || !singleton)
			return kInvalidNativeFontCommandIndex;

		const UInt64 existingToken =
			singleton->commandValidationToken.load(
				std::memory_order_acquire);
		const UInt32 existingIndex =
			singleton->commandDirectFacadeSinglePacketIndex.load(
				std::memory_order_acquire);
		if (existingToken == buffer.stamp.validationToken
			&& existingIndex != kInvalidNativeFontCommandIndex)
		{
			return existingIndex;
		}

		NativeFontShapePayload* payload = &metadata->nativePayload;
		const NativeFontDrawCommand& prepared =
			singleton->commandBuildCommand;
		if (singleton->commandBuildValidationToken.load(
				std::memory_order_acquire)
				!= buffer.stamp.validationToken
			|| singleton->frameMode.load(std::memory_order_acquire)
				!= SingletonFacadeFrameMode::Direct
			|| singleton->preparedValidationToken
				!= buffer.stamp.validationToken
			|| singleton->preparedGeneration != buffer.stamp.generation
			|| singleton->preparedAtlasTextureEpoch
				!= buffer.stamp.atlasTextureEpoch
			|| singleton->topologyValidationToken
				!= buffer.stamp.validationToken
			|| !payload->buildComplete || !payload->payloadTemplate
			|| prepared.sourceGeometry == nullptr
			|| prepared.expectedGeometry != prepared.sourceGeometry
			|| prepared.sourceGeometry != singleton->slot.shape
			|| prepared.payload != payload || !prepared.packet
			|| prepared.packetIndex != 0 || !prepared.program
			|| !prepared.atlasTexture || !prepared.binding.active)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandBuildViewMiss);
			return kInvalidNativeFontCommandIndex;
		}

		const NativeFontTileRetainedText* retainedText =
			ResolveTileRetainedText(buffer, singleton->slot.shape,
				*payload, false);
		if (!retainedText || retainedText->packets.size() != 1
			|| !AdoptFrameResourceStamp(buffer,
				prepared.binding.resourceSerial,
				prepared.binding.uploadEpoch))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandBuildViewMiss);
			return kInvalidNativeFontCommandIndex;
		}

		RecordFreeTypePerf(FreeTypePerfCounter::CommandBuildViewHit);
		const UInt32 commandIndex = static_cast<UInt32>(
			buffer.directFacadeSinglePacketCommands.size());
		const size_t previousCapacity =
			buffer.directFacadeSinglePacketCommands.capacity();
		buffer.directFacadeSinglePacketCommands.emplace_back();
		RecordCommandVectorGrowth(previousCapacity,
			buffer.directFacadeSinglePacketCommands.capacity());
		NativeFontDirectFacadeSinglePacketCommand& command =
			buffer.directFacadeSinglePacketCommands.back();
		command.singletonMetadata = metadata;
		command.geometry = prepared.expectedGeometry;
		command.payload = payload;
		command.artifact = payload->payloadTemplate.get();
		command.draw = &prepared;
		command.generation = buffer.stamp.generation;
		command.atlasTextureEpoch = buffer.stamp.atlasTextureEpoch;
		command.validationToken = buffer.stamp.validationToken;
		command.useCompositePackets = payload->useCompositePackets;
		singleton->commandDirectFacadeSinglePacketIndex.store(
			commandIndex, std::memory_order_relaxed);
		singleton->commandValidationToken.store(
			buffer.stamp.validationToken, std::memory_order_release);
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandDirectFacadeSinglePacketRecorded);
		RecordFreeTypePerf(FreeTypePerfCounter::CommandPacketRecorded);
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandBuildBindingReuse);
		return commandIndex;
	}

	UInt32 AddNativeFontFrameCommandSpan(NiTriShape* facade,
		const NativeFontShapeMetadata* metadata, NativeFontShapePayload* payload)
	{
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		if (!buffer.building || !facade || !metadata || !payload
			|| !payload->buildComplete || !payload->payloadTemplate)
			return kInvalidNativeFontCommandIndex;

		NativeFontCommandSpan span;
		span.facade = facade;
		span.metadata = metadata;
		span.payload = payload;
		span.validationToken = buffer.stamp.validationToken;
		span.generation = buffer.stamp.generation;
		span.atlasTextureEpoch = buffer.stamp.atlasTextureEpoch;
		span.firstCommand = static_cast<UInt32>(buffer.commands.size());
		span.firstRun = static_cast<UInt32>(buffer.runs.size());

		const size_t commandRollback = buffer.commands.size();
		const size_t runRollback = buffer.runs.size();
		bool appended = false;
		const NativeFontTileRetainedText* retainedText =
			ResolveTileRetainedText(buffer, facade, *payload);
		NativeFontFramePayloadBinding payloadBinding;
		if (retainedText
			&& ResolveNativeFontFramePayloadBinding(
				*payload, payloadBinding))
		{
			for (UInt32 index = 0;
				index < retainedText->packets.size(); ++index)
			{
				if (!AppendCompatibilityCommand(buffer, facade,
					*payload, retainedText->packets[index],
					payloadBinding))
				{
					appended = false;
					break;
				}
				appended = true;
			}
			if (appended && retainedText->packets.size() > 1)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandBuildBindingReuse,
					static_cast<UInt64>(
						retainedText->packets.size() - 1));
			}
			if (appended)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandTileRetainedPacketReuse,
					static_cast<UInt64>(retainedText->packets.size()));
			}
		}
		if (!appended)
		{
			buffer.commands.resize(commandRollback);
			return kInvalidNativeFontCommandIndex;
		}

		span.commandCount = static_cast<UInt32>(
			buffer.commands.size() - span.firstCommand);
		span.useCompositePackets =
			span.payload->useCompositePackets;
		if (!retainedText)
		{
			buffer.commands.resize(commandRollback);
			return kInvalidNativeFontCommandIndex;
		}
		const size_t previousRunCapacity = buffer.runs.capacity();
		const bool builtRuns =
			BuildFrameRuns(buffer, span, retainedText->runs,
				retainedText->bridgeEligible);
		RecordCommandVectorGrowth(
			previousRunCapacity, buffer.runs.capacity());
		if (!builtRuns)
		{
			buffer.commands.resize(commandRollback);
			buffer.runs.resize(runRollback);
			return kInvalidNativeFontCommandIndex;
		}
		span.runCount = static_cast<UInt32>(
			buffer.runs.size() - span.firstRun);
		if (!span.commandCount || !span.runCount)
		{
			buffer.commands.resize(commandRollback);
			buffer.runs.resize(runRollback);
			return kInvalidNativeFontCommandIndex;
		}

		const NativeFontFramePacketBinding& spanBinding =
			buffer.commands[span.firstCommand].binding;
		if (!AdoptFrameResourceStamp(buffer,
				spanBinding.resourceSerial, spanBinding.uploadEpoch))
		{
			buffer.commands.resize(commandRollback);
			buffer.runs.resize(runRollback);
			return kInvalidNativeFontCommandIndex;
		}

		const UInt32 spanIndex = static_cast<UInt32>(
			buffer.spans.size());
		const size_t previousSpanCapacity = buffer.spans.capacity();
		buffer.spans.push_back(span);
		RecordCommandVectorGrowth(
			previousSpanCapacity, buffer.spans.capacity());
		RecordFreeTypePerf(FreeTypePerfCounter::CommandSpanRecorded);
		RecordFreeTypePerf(FreeTypePerfCounter::CommandPacketRecorded,
			span.commandCount);
		return spanIndex;
	}

	void ActivateNativeFontFrameCommandBuffer()
	{
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		buffer.building = false;
		buffer.active = buffer.enabled && buffer.stamp.device
			&& ((!buffer.spans.empty() && !buffer.commands.empty())
				|| !buffer.singlePacketCommands.empty()
				|| !buffer.directFacadeSinglePacketCommands.empty());
		if (buffer.active)
			RecordFreeTypePerf(FreeTypePerfCounter::CommandRecorded);
		UpdateCommandHighWater(buffer);
		RefreshCommandMemory(buffer);
	}

	void EndNativeFontFrameCommandBuffer()
	{
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		for (const NativeFontDirectFacadeSinglePacketCommand& command
			: buffer.directFacadeSinglePacketCommands)
		{
			SingletonFacadeState* singleton = command.singletonMetadata
				? GetSingletonFacadeState(*command.singletonMetadata)
				: nullptr;
			if (!singleton
				|| singleton->commandValidationToken.load(
					std::memory_order_acquire)
					!= command.validationToken)
			{
				continue;
			}
			singleton->commandBuildValidationToken.store(
				0, std::memory_order_release);
			singleton->commandValidationToken.store(
				0, std::memory_order_release);
			singleton->commandDirectFacadeSinglePacketIndex.store(
				kInvalidNativeFontCommandIndex,
				std::memory_order_release);
		}
		buffer.active = false;
		buffer.building = false;
		buffer.stamp = {};
		buffer.executionSegment = {};
		buffer.executionBoundaryEpoch = 1;
		buffer.frameExternalMutationEpoch = 0;
		buffer.executionBoundaryReason =
			NativeFontCommandFallback::State;
		buffer.enabled = false;
		buffer.commands.clear();
		buffer.runs.clear();
		buffer.spans.clear();
		buffer.singlePacketCommands.clear();
		buffer.directFacadeSinglePacketCommands.clear();
		TrimCommandCapacity(buffer);
	}

	void InvalidateNativeFontCommandGeometry(NiTriShape* geometry)
	{
		NativeFontFrameCommandBuffer& buffer = CommandBuffer();
		if (!geometry)
			return;
		NotifyNativeFontCommandExternalMutation(
			NativeFontCommandFallback::Topology);
		InvalidateNativeFontCommandExecutionSegment(
			NativeFontCommandFallback::Topology);
		if (!buffer.active && !buffer.building)
			return;
		bool invalidated = false;
		bool singlePacketInvalidated = false;
		bool directFacadeSinglePacketInvalidated = false;
		for (NativeFontSinglePacketCommand& command
			: buffer.singlePacketCommands)
		{
			if (command.facade != geometry
				&& command.draw.sourceGeometry != geometry
				&& command.draw.expectedGeometry != geometry)
			{
				continue;
			}
			command.partialDraw = command.partialDraw
				|| command.state
					== NativeFontCommandSpanState::Executing;
			command.executionValidationToken = 0;
			command.executionSegmentEpoch = 0;
			command.executionExternalMutationEpoch = 0;
			command.state = NativeFontCommandSpanState::Fault;
			invalidated = true;
			singlePacketInvalidated = true;
		}
		for (NativeFontDirectFacadeSinglePacketCommand& command
			: buffer.directFacadeSinglePacketCommands)
		{
			if (command.geometry != geometry
				&& (!command.draw
					|| (command.draw->sourceGeometry != geometry
						&& command.draw->expectedGeometry != geometry)))
			{
				continue;
			}
			command.partialDraw = command.partialDraw
				|| command.state
					== NativeFontCommandSpanState::Executing;
			command.executionValidationToken = 0;
			command.executionSegmentEpoch = 0;
			command.executionExternalMutationEpoch = 0;
			command.state = NativeFontCommandSpanState::Fault;
			invalidated = true;
			directFacadeSinglePacketInvalidated = true;
		}
		for (NativeFontCommandSpan& span : buffer.spans)
		{
			bool member = span.facade == geometry;
			for (UInt32 index = 0;
				!member && index < span.commandCount; ++index)
			{
				const NativeFontDrawCommand& command =
					buffer.commands[span.firstCommand + index];
				member = command.sourceGeometry == geometry
					|| command.expectedGeometry == geometry;
			}
			if (!member)
				continue;
			span.partialDraw = span.partialDraw
				|| span.state == NativeFontCommandSpanState::Executing;
			span.executionValidationToken = 0;
			span.executionSegmentEpoch = 0;
			span.executionExternalMutationEpoch = 0;
			span.state = NativeFontCommandSpanState::Fault;
			invalidated = true;
		}
		if (invalidated)
		{
			if (singlePacketInvalidated)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						CommandSinglePacketFallback);
			}
			if (directFacadeSinglePacketInvalidated)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						CommandDirectFacadeSinglePacketFallback);
			}
			RecordNativeFontCommandFallback(
				NativeFontCommandFallback::Topology);
		}
	}

	void RecordNativeFontCommandFallback(
		NativeFontCommandFallback reason)
	{
		switch (reason)
		{
		case NativeFontCommandFallback::Token:
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandFallbackToken);
			break;
		case NativeFontCommandFallback::Generation:
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandFallbackGeneration);
			break;
		case NativeFontCommandFallback::Atlas:
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandFallbackAtlas);
			break;
		case NativeFontCommandFallback::Resource:
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandFallbackResource);
			break;
		case NativeFontCommandFallback::Topology:
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandFallbackTopology);
			break;
		case NativeFontCommandFallback::Hook:
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandFallbackHook);
			break;
		case NativeFontCommandFallback::Nested:
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandFallbackNested);
			break;
		case NativeFontCommandFallback::RenderTarget:
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandFallbackRenderTarget);
			break;
		case NativeFontCommandFallback::State:
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandFallbackState);
			break;
		default:
			break;
		}
	}
}

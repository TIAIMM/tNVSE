#include "font_a8_internal.h"
#include "font_native_internal.h"

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
	namespace implementation::font_native_command_buffer {}
	using namespace implementation::font_native_command_buffer;

	namespace implementation::font_native_command_buffer
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

		struct NativeA8ExecutionSegmentState
		{
			// A segment spans adjacent FreeType Tile submissions. Full frame,
			// hook, device, ring, RT, and viewport validation is shared until a
			// vanilla/non-FreeType boundary or an externally published resource
			// mutation advances one of these epochs.
			UInt64 validationToken = 0;
			UInt32 boundaryEpoch = 0;
			UInt32 externalMutationEpoch = 0;
			NativeA8CommandFallback failure =
				NativeA8CommandFallback::State;
			bool validated = false;
		};

		struct NativeA8FrameCommandBuffer
		{
			NativeA8FrameStamp stamp;
			std::vector<NativeA8DrawCommand> commands;
			std::vector<NativeA8FrameCommandRun> runs;
			std::vector<NativeA8CommandSpan> spans;
			std::vector<NativeA8SinglePacketCommand>
				singlePacketCommands;
			std::vector<NativeA8DirectFacadeSinglePacketCommand>
				directFacadeSinglePacketCommands;
			CpuMemoryLease cpuMemory;
			NativeA8ExecutionSegmentState executionSegment;
			UInt32 executionBoundaryEpoch = 1;
			UInt32 frameExternalMutationEpoch = 0;
			NativeA8CommandFallback executionBoundaryReason =
				NativeA8CommandFallback::State;
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

		thread_local NativeA8FrameCommandBuffer s_commandBuffer;
		// Resource lifecycles may publish from a reset/cache thread while the
		// command buffer itself is render-thread local. One process-wide epoch
		// lets packet callbacks reject that race with a single acquire load.
		std::atomic<UInt32> s_externalMutationEpoch = 1;
		std::atomic<UInt8> s_externalMutationReason =
			static_cast<UInt8>(NativeA8CommandFallback::State);

		NativeA8CommandFallback NormalizeMutationReason(
			NativeA8CommandFallback reason)
		{
			return reason == NativeA8CommandFallback::None
				? NativeA8CommandFallback::State : reason;
		}

		UInt32 LoadExternalMutationEpoch()
		{
			return s_externalMutationEpoch.load(std::memory_order_acquire);
		}

		NativeA8CommandFallback LoadExternalMutationReason()
		{
			return NormalizeMutationReason(
				static_cast<NativeA8CommandFallback>(
					s_externalMutationReason.load(
						std::memory_order_acquire)));
		}

		void RecordSinglePacketCommandFallback(
			NativeA8CommandFallback reason)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandSinglePacketFallback);
			RecordNativeA8CommandFallback(reason);
		}

		void RecordDirectFacadeSinglePacketCommandFallback(
			NativeA8CommandFallback reason)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					CommandDirectFacadeSinglePacketFallback);
			RecordNativeA8CommandFallback(reason);
		}

		void AdvanceExecutionBoundaryEpoch(
			NativeA8FrameCommandBuffer& buffer,
			NativeA8CommandFallback reason)
		{
			NativeA8ExecutionSegmentState& segment =
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

		void RefreshCommandMemory(NativeA8FrameCommandBuffer& buffer)
		{
			const size_t bytes =
				buffer.commands.capacity() * sizeof(NativeA8DrawCommand)
				+ buffer.runs.capacity()
					* sizeof(NativeA8FrameCommandRun)
				+ buffer.spans.capacity() * sizeof(NativeA8CommandSpan)
				+ buffer.singlePacketCommands.capacity()
					* sizeof(NativeA8SinglePacketCommand)
				+ buffer.directFacadeSinglePacketCommands.capacity()
					* sizeof(NativeA8DirectFacadeSinglePacketCommand);
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

		void UpdateCommandHighWater(NativeA8FrameCommandBuffer& buffer)
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

		void TrimCommandCapacity(NativeA8FrameCommandBuffer& buffer)
		{
			if (buffer.commands.capacity() > 16384)
			{
				std::vector<NativeA8DrawCommand>().swap(buffer.commands);
				buffer.highWaterCommands = 0;
			}
			if (buffer.runs.capacity() > 8192)
			{
				std::vector<NativeA8FrameCommandRun>().swap(buffer.runs);
				buffer.highWaterRuns = 0;
			}
			if (buffer.spans.capacity() > 8192)
			{
				std::vector<NativeA8CommandSpan>().swap(buffer.spans);
				buffer.highWaterSpans = 0;
			}
			if (buffer.singlePacketCommands.capacity() > 8192)
			{
				std::vector<NativeA8SinglePacketCommand>().swap(
					buffer.singlePacketCommands);
				buffer.highWaterSinglePackets = 0;
			}
			if (buffer.directFacadeSinglePacketCommands.capacity() > 8192)
			{
				std::vector<NativeA8DirectFacadeSinglePacketCommand>().swap(
					buffer.directFacadeSinglePacketCommands);
				buffer.highWaterDirectFacadeSinglePackets = 0;
			}
			RefreshCommandMemory(buffer);
			if (!IsCpuMemoryBudgetExceeded())
				return;
			std::vector<NativeA8DrawCommand>().swap(buffer.commands);
			std::vector<NativeA8FrameCommandRun>().swap(buffer.runs);
			std::vector<NativeA8CommandSpan>().swap(buffer.spans);
			std::vector<NativeA8SinglePacketCommand>().swap(
				buffer.singlePacketCommands);
			std::vector<NativeA8DirectFacadeSinglePacketCommand>().swap(
				buffer.directFacadeSinglePacketCommands);
			buffer.highWaterCommands = 0;
			buffer.highWaterRuns = 0;
			buffer.highWaterSpans = 0;
			buffer.highWaterSinglePackets = 0;
			buffer.highWaterDirectFacadeSinglePackets = 0;
			buffer.trackedCapacityBytes = 0;
			buffer.cpuMemory.Release();
		}

		bool ResolveRenderContextStamp(NativeA8FrameStamp& stamp)
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
			const NativeA8DrawCommand& command,
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
					!= sizeof(NativeA8GpuVertex)
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
						* sizeof(NativeA8GpuVertex))
			{
				return false;
			}
			return true;
		}

		NativeA8CommandFallback ValidateDrawCommandState(
			const NativeA8FrameCommandBuffer& buffer,
			const NativeA8DrawCommand& command,
			const NativeA8ShapePayload* expectedPayload,
			UInt32 expectedPacketIndex, NiTriShape* geometry,
			NiRenderer* renderer)
		{
			if (!command.program || !command.program->active
				|| command.program->generation
					!= buffer.stamp.generation
				|| command.program->device != buffer.stamp.device
				|| !renderer || renderer != buffer.stamp.renderer)
			{
				return NativeA8CommandFallback::Generation;
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
				return NativeA8CommandFallback::Topology;
			}
			if (command.payload->preflightAtlasTextureEpoch
					!= buffer.stamp.atlasTextureEpoch)
			{
				return NativeA8CommandFallback::Atlas;
			}
			if (!ValidateGeometryBinding(command, geometry))
				return NativeA8CommandFallback::Resource;
			return NativeA8CommandFallback::None;
		}

		bool CompileCompatibilityCommand(
			NiTriShape* facade, NativeA8ShapePayload& payload,
			const NativeA8TileRetainedPacket& retained,
			const NativeA8FramePayloadBinding& payloadBinding,
			NativeA8DrawCommand& command)
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
			NativeA8FrameCommandBuffer& buffer,
			NiTriShape* facade, NativeA8ShapePayload& payload,
			const NativeA8TileRetainedPacket& retained,
			const NativeA8FramePayloadBinding& payloadBinding)
		{
			const size_t previousCapacity = buffer.commands.capacity();
			buffer.commands.emplace_back();
			RecordCommandVectorGrowth(
				previousCapacity, buffer.commands.capacity());
			NativeA8DrawCommand& command = buffer.commands.back();
			if (!CompileCompatibilityCommand(facade, payload,
					retained, payloadBinding, command))
			{
				buffer.commands.pop_back();
				return false;
			}
			return true;
		}

		const NativeA8TileRetainedText* ResolveTileRetainedText(
			const NativeA8FrameCommandBuffer& buffer,
			NiTriShape* ownerTile, NativeA8ShapePayload& payload,
			bool recordResult = true)
		{
			if (!IsNativeA8TileRetainedTextCurrent(payload,
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
			NativeA8FrameCommandBuffer& buffer,
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
		bool BuildFrameRuns(NativeA8FrameCommandBuffer& buffer,
			NativeA8CommandSpan& span,
			const RetainedRunContainer& retainedRuns,
			bool retainedBridgeEligible)
		{
			UInt32 coveredPackets = 0;
			for (const NativeA8TileRetainedRun& retained : retainedRuns)
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
					const NativeA8DrawCommand& firstCommand =
						buffer.commands[span.firstCommand + first];
					UInt32 end = first + 1;
					while (end < retainedEnd)
					{
						const NativeA8DrawCommand& candidate =
							buffer.commands[span.firstCommand + end];
						++end;
					}
					NativeA8FrameCommandRun run;
					run.firstCommand = span.firstCommand + first;
					run.commandCount = end - first;
					run.bridgeEligible = retained.bridgeEligible;
					if (buffer.runs.size() > span.firstRun)
					{
						const NativeA8FrameCommandRun& previous =
							buffer.runs.back();
						const NativeA8DrawCommand& previousCommand =
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
			const NativeA8FrameStamp& stamp)
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

		NativeA8CommandFallback ValidateExecutionSegmentState(
			NativeA8FrameCommandBuffer& buffer,
			UInt32 expectedExternalMutationEpoch)
		{
			NativeA8FrameStamp& stamp = buffer.stamp;
			if (!buffer.frameExternalMutationEpoch
				|| expectedExternalMutationEpoch
					!= buffer.frameExternalMutationEpoch)
			{
				return LoadExternalMutationReason();
			}
			if (!stamp.accumulator || !stamp.validationToken
				|| stamp.validationToken
					!= GetNativeA8SortedFrameValidationToken())
			{
				return NativeA8CommandFallback::Token;
			}
			if (stamp.nestedTraversalSerial
				!= GetNativeA8SortedNestedTraversalSerial())
			{
				return NativeA8CommandFallback::Nested;
			}
			NativeA8RuntimeReadinessView readiness;
			if (!GetNativeA8RuntimeReadinessCurrent(readiness))
			{
				return GetNativeA8AtlasTextureEpoch()
						!= stamp.atlasTextureEpoch
					? NativeA8CommandFallback::Atlas
					: NativeA8CommandFallback::Hook;
			}
			if (readiness.renderer != stamp.renderer
				|| readiness.device != stamp.device
				|| readiness.generation != stamp.generation
				|| !IsNativeA8ShaderGenerationCurrent(stamp.generation))
			{
				return NativeA8CommandFallback::Generation;
			}
			if (readiness.atlasTextureEpoch != stamp.atlasTextureEpoch)
				return NativeA8CommandFallback::Atlas;
			if (!IsNativeA8FrameResourceStampCurrent(
				stamp.generation, stamp.resourceSerial,
				stamp.uploadEpoch))
			{
				return NativeA8CommandFallback::Resource;
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
					return NativeA8CommandFallback::RenderTarget;
				}
			}
			else if (!ValidateRenderContext(stamp))
			{
				return NativeA8CommandFallback::RenderTarget;
			}
			if (LoadExternalMutationEpoch()
				!= expectedExternalMutationEpoch)
			{
				return LoadExternalMutationReason();
			}
			return NativeA8CommandFallback::None;
		}

		NativeA8CommandFallback EnsureExecutionSegmentValidated(
			NativeA8FrameCommandBuffer& buffer)
		{
			NativeA8ExecutionSegmentState& segment =
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
					== NativeA8CommandFallback::None)
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
			if (segment.failure == NativeA8CommandFallback::None)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandExecutionSegment);
			}
			return segment.failure;
		}

		NativeA8CommandFallback ValidateExecutionSegmentEpoch(
			NativeA8FrameCommandBuffer& buffer,
			UInt32 executionSegmentEpoch,
			UInt32 executionExternalMutationEpoch)
		{
			const NativeA8ExecutionSegmentState& segment =
				buffer.executionSegment;
			if (!segment.validated
				|| segment.failure != NativeA8CommandFallback::None
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
				const NativeA8CommandFallback reason =
					LoadExternalMutationReason();
				AdvanceExecutionBoundaryEpoch(buffer, reason);
				return reason;
			}
			return NativeA8CommandFallback::None;
		}

		NativeA8CommandFallback ValidatePacketExecutionGuard(
			NativeA8FrameCommandBuffer& buffer,
			NativeA8CommandSpanState state,
			UInt64 executionValidationToken,
			UInt32 executionSegmentEpoch,
			UInt32 executionExternalMutationEpoch,
			NiRenderer* renderer)
		{
			if (state != NativeA8CommandSpanState::Executing
				|| executionValidationToken
					!= buffer.stamp.validationToken)
			{
				return NativeA8CommandFallback::State;
			}
			if (!renderer || renderer != buffer.stamp.renderer)
				return NativeA8CommandFallback::Generation;
			return ValidateExecutionSegmentEpoch(buffer,
				executionSegmentEpoch,
				executionExternalMutationEpoch);
		}
	}

	bool IsNativeA8StandardPassLiteDispatchCurrent(
		const NativeA8StandardPassLiteDispatch& dispatch,
		const NiTriShape* geometry,
		const NativeA8CompiledPacketCommand* program,
		UInt32 generation)
	{
		return dispatch.ready && geometry && program && generation
			&& dispatch.geometry == geometry
			&& dispatch.properties == &geometry->m_kProperties
			&& dispatch.program == program
			&& dispatch.shader == program->shader
			&& dispatch.renderer
			&& dispatch.generation == generation
			&& program->generation == generation
			&& dispatch.standardV2Ready
				== (program->standardV2SlotProofs
					== NativeA8CompiledPacketCommand::
						kStandardV2RequiredProofs);
	}

	void InvalidateNativeA8StandardPassLiteDispatch(
		NativeA8StandardPassLiteDispatch& dispatch)
	{
		dispatch = {};
	}

	bool BuildNativeA8StandardPassLiteDispatch(
		NiTriShape* geometry,
		const NativeA8CompiledPacketCommand* program,
		UInt32 generation,
		NativeA8StandardPassLiteDispatch& dispatch)
	{
		if (IsNativeA8StandardPassLiteDispatchCurrent(
				dispatch, geometry, program, generation))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::StandardPassLiteRetainedReuse);
			return true;
		}

		InvalidateNativeA8StandardPassLiteDispatch(dispatch);
		if (!g_bEnableFreeTypeFontCommandBuffer
			|| !geometry || !program || !generation
			|| !State().standardPassLitePredicatesValidated
			|| geometry->GetSkinInstance()
			|| !geometry->GetModelData())
		{
			return false;
		}

		void** geometryVtable =
			*reinterpret_cast<void***>(geometry);
		TileShader* shader = program->shader;
		void** shaderVtable = shader
			? *reinterpret_cast<void***>(shader) : nullptr;
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		if (!IsA8AtlasShape(geometry)
			|| geometryVtable[kRenderImmediateAltSlot]
				!= reinterpret_cast<void*>(&A8RenderImmediateAlt)
			|| !program->active || !program->profile
			|| program->generation != generation
			|| !shader || !shaderVtable
			|| shaderVtable != program->shaderVtable
			|| !program->prepareGeometry
			|| !program->setupPass
			|| !program->updateConstants
			|| !program->setupBlend
			|| !program->setupAlphaTest
			|| !program->setupDrawmode
			|| !program->postGeometry
			|| !program->setupNonFirstPass
			|| !renderer
			|| program->device != renderer->GetD3DDevice())
		{
			return false;
		}

		dispatch.geometry = geometry;
		dispatch.properties = &geometry->m_kProperties;
		dispatch.renderer = renderer;
		dispatch.shader = shader;
		dispatch.program = program;
		dispatch.generation = generation;
		dispatch.standardV2Ready =
			program->standardV2SlotProofs
				== NativeA8CompiledPacketCommand::
					kStandardV2RequiredProofs;
		dispatch.ready = true;
		RecordFreeTypePerf(
			FreeTypePerfCounter::StandardPassLiteRetainedBuild);
		return true;
	}

	size_t GetNativeA8TileRetainedCapacityBytes(
		const NativeA8ShapePayload& payload)
	{
		return payload.retainedText.packets.heap_capacity()
				* sizeof(NativeA8TileRetainedPacket)
			+ payload.retainedText.runs.heap_capacity()
				* sizeof(NativeA8TileRetainedRun);
	}

	void InvalidateNativeA8TileRetainedText(
		NativeA8ShapePayload& payload,
		bool preserveStandardPassLite)
	{
		NativeA8TileRetainedText& retained = payload.retainedText;
		retained.ready = false;
		retained.atlasTextureEpoch = 0;
		retained.bridgeEligible = false;
		if (!preserveStandardPassLite)
		{
			InvalidateNativeA8StandardPassLiteDispatch(
				retained.standardPassLite);
		}
	}

	bool BuildNativeA8TileRetainedText(NiTriShape* ownerTile,
		NativeA8ShapePayload& payload, UInt32 generation,
		UInt32 atlasTextureEpoch)
	{
		NativeA8TileRetainedText& retained = payload.retainedText;
		// Keep an identity-matching Standard-lite dispatch available while a
		// preflight refresh proves that the Tile/program pair is unchanged.
		retained.ready = false;
		retained.atlasTextureEpoch = 0;
		retained.bridgeEligible = false;
		if (!g_bEnableFreeTypeFontCommandBuffer || !ownerTile
			|| !generation || !atlasTextureEpoch
			|| !payload.payloadTemplate)
		{
			InvalidateNativeA8StandardPassLiteDispatch(
				retained.standardPassLite);
			return false;
		}

		const NativeA8PayloadTemplate& artifact =
			*payload.payloadTemplate;
		const std::vector<NativeA8PacketTemplate>& packets =
			GetNativeA8Packets(artifact, payload.useCompositePackets);
		auto discardRetained = [&retained]()
		{
			retained.ready = false;
			retained.ownerTile = nullptr;
			retained.artifact = nullptr;
			retained.generation = 0;
			retained.atlasTextureEpoch = 0;
			retained.useCompositePackets = false;
			retained.bridgeEligible = false;
			retained.packets.clear();
			retained.runs.clear();
			InvalidateNativeA8StandardPassLiteDispatch(
				retained.standardPassLite);
		};
		if (packets.empty()
			|| payload.packetShaders.size() != packets.size()
			|| payload.packetPrograms.size() != packets.size()
			|| payload.preflightAtlasTextures.size()
				!= artifact.atlasTextures.size())
		{
			discardRetained();
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandTileRetainedMiss);
			return false;
		}

		bool canRefresh = retained.ownerTile == ownerTile
			&& retained.artifact == &artifact
			&& retained.generation == generation
			&& retained.useCompositePackets
				== payload.useCompositePackets
			&& retained.packets.size() == packets.size()
			&& !retained.runs.empty();
		for (UInt32 index = 0;
			index < static_cast<UInt32>(packets.size()); ++index)
		{
			const NativeA8PacketTemplate& packet = packets[index];
			const NativeA8CompiledPacketCommand* program =
				payload.packetPrograms[index];
			const UInt64 packetEnd =
				static_cast<UInt64>(packet.firstVertex)
					+ packet.vertexCount;
			if (!packet.vertexCount || (packet.vertexCount & 3u)
				|| packetEnd > artifact.gpuVertices.size()
				|| packet.atlasPage
					>= payload.preflightAtlasTextures.size()
				|| !payload.preflightAtlasTextures[packet.atlasPage]
				|| !payload.packetShaders[index] || !program
				|| !program->active || !program->profile
				|| program->generation != generation
				|| program->shader != payload.packetShaders[index])
			{
				discardRetained();
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandTileRetainedMiss);
				return false;
			}

			if (canRefresh)
			{
				const NativeA8TileRetainedPacket& existing =
					retained.packets[index];
				canRefresh = existing.packet == &packet
					&& existing.program == program
					&& existing.packetIndex == index
					&& existing.firstVertex == packet.firstVertex
					&& existing.vertexCount == packet.vertexCount
					&& existing.atlasPage == packet.atlasPage;
				continue;
			}
		}

		if (canRefresh)
		{
			if (retained.packets.size() == 1)
			{
				BuildNativeA8StandardPassLiteDispatch(
					ownerTile, retained.packets.front().program,
					generation, retained.standardPassLite);
			}
			else
			{
				InvalidateNativeA8StandardPassLiteDispatch(
					retained.standardPassLite);
			}
			retained.atlasTextureEpoch = atlasTextureEpoch;
			retained.bridgeEligible = retained.packets.size() > 1;
			retained.ready = true;
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandTileRetainedRefresh);
			return true;
		}

		discardRetained();
		if (retained.packets.capacity() < packets.size()
			|| retained.runs.capacity() < packets.size())
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandTileRetainedMiss);
			return false;
		}

		for (UInt32 index = 0;
			index < static_cast<UInt32>(packets.size()); ++index)
		{
			const NativeA8PacketTemplate& packet = packets[index];
			NativeA8TileRetainedPacket command;
			command.packet = &packet;
			command.program = payload.packetPrograms[index];
			command.packetIndex = index;
			command.firstVertex = packet.firstVertex;
			command.vertexCount = packet.vertexCount;
			command.atlasPage = packet.atlasPage;
			retained.packets.push_back(command);
		}

		for (UInt32 first = 0;
			first < static_cast<UInt32>(retained.packets.size());)
		{
			const void* profile =
				retained.packets[first].program->profile;
			UInt32 end = first + 1u;
			while (end < retained.packets.size()
				&& retained.packets[end].program->profile == profile)
			{
				++end;
			}
			NativeA8TileRetainedRun run;
			run.firstPacket = first;
			run.packetCount = end - first;
			run.bridgeEligible = true;
			run.continuesBridgeSpan = first != 0;
			retained.runs.push_back(run);
			first = end;
		}

		retained.ownerTile = ownerTile;
		retained.artifact = &artifact;
		retained.generation = generation;
		retained.atlasTextureEpoch = atlasTextureEpoch;
		retained.useCompositePackets = payload.useCompositePackets;
		retained.bridgeEligible = retained.packets.size() > 1;
		if (retained.packets.size() == 1)
		{
			BuildNativeA8StandardPassLiteDispatch(
				ownerTile, retained.packets.front().program,
				generation, retained.standardPassLite);
		}
		else
		{
			InvalidateNativeA8StandardPassLiteDispatch(
				retained.standardPassLite);
		}
		retained.ready = !retained.packets.empty()
			&& !retained.runs.empty();
		if (!retained.ready)
		{
			discardRetained();
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandTileRetainedMiss);
			return false;
		}
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandTileRetainedBuild);
		return true;
	}

	bool IsNativeA8TileRetainedTextCurrent(
		const NativeA8ShapePayload& payload,
		const NiTriShape* ownerTile, UInt32 generation,
		UInt32 atlasTextureEpoch)
	{
		const NativeA8TileRetainedText& retained =
			payload.retainedText;
		if (!retained.ready || !ownerTile || !generation
			|| !atlasTextureEpoch || !payload.payloadTemplate
			|| retained.ownerTile != ownerTile
			|| retained.artifact != payload.payloadTemplate.get()
			|| retained.generation != generation
			|| retained.atlasTextureEpoch != atlasTextureEpoch
			|| retained.useCompositePackets
				!= payload.useCompositePackets)
		{
			return false;
		}
		const std::vector<NativeA8PacketTemplate>& packets =
			GetNativeA8Packets(*payload.payloadTemplate,
				payload.useCompositePackets);
		return !retained.packets.empty()
			&& !retained.runs.empty()
			&& retained.packets.size() == packets.size();
	}

	void NotifyNativeA8CommandExternalMutation(
		NativeA8CommandFallback reason)
	{
		s_externalMutationReason.store(static_cast<UInt8>(
			NormalizeMutationReason(reason)), std::memory_order_relaxed);
		UInt32 current =
			s_externalMutationEpoch.load(std::memory_order_relaxed);
		for (;;)
		{
			UInt32 next = current + 1u;
			if (!next)
				next = 1u;
			if (s_externalMutationEpoch.compare_exchange_weak(
				current, next, std::memory_order_release,
				std::memory_order_relaxed))
			{
				return;
			}
		}
	}

	void InvalidateNativeA8CommandExecutionSegment(
		NativeA8CommandFallback reason)
	{
		AdvanceExecutionBoundaryEpoch(
			s_commandBuffer, NormalizeMutationReason(reason));
	}

	void BeginNativeA8FrameCommandBuffer(BSShaderAccumulator* accumulator,
		UInt64 validationToken, UInt32 generation, UInt32 atlasTextureEpoch)
	{
		EndNativeA8FrameCommandBuffer();
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		buffer.enabled = g_bEnableFreeTypeFontCommandBuffer;
		if (!buffer.enabled || !g_bEnableFreeTypeFontRendering
			|| !g_bEnableFreeTypeA8Atlas || !accumulator
			|| !validationToken || !generation)
		{
			return;
		}
		buffer.stamp.accumulator = accumulator;
		buffer.stamp.validationToken = validationToken;
		buffer.stamp.generation = generation;
		buffer.stamp.atlasTextureEpoch = atlasTextureEpoch;
		buffer.stamp.nestedTraversalSerial =
			GetNativeA8SortedNestedTraversalSerial();
		buffer.stamp.renderer = NiDX9Renderer::GetSingleton();
		buffer.stamp.device = buffer.stamp.renderer
			? buffer.stamp.renderer->GetD3DDevice() : nullptr;
		if (!buffer.stamp.device)
			return;
		buffer.frameExternalMutationEpoch =
			LoadExternalMutationEpoch();
		buffer.building = true;
	}

	void ReserveNativeA8FrameCommandBuffer(size_t ordinaryEntryCount,
		size_t directFacadeCount)
	{
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
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

	UInt32 AddNativeA8FrameSinglePacketCommand(NiTriShape* facade,
		const A8ShapeMetadata* metadata, NativeA8ShapePayload* payload)
	{
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		if (!buffer.building || !facade || !metadata || !payload
			|| !payload->buildComplete || !payload->payloadTemplate)
		{
			return kInvalidNativeA8CommandIndex;
		}

		if (GetNativeA8Packets(*payload->payloadTemplate,
				payload->useCompositePackets).size() != 1)
		{
			return kInvalidNativeA8CommandIndex;
		}
		const NativeA8TileRetainedText* retainedText =
			ResolveTileRetainedText(buffer, facade, *payload);
		if (!retainedText || retainedText->packets.size() != 1)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					CommandSinglePacketBuildFallback);
			return kInvalidNativeA8CommandIndex;
		}

		NativeA8FramePayloadBinding payloadBinding;
		if (!ResolveNativeA8FramePayloadBinding(
				*payload, payloadBinding))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					CommandSinglePacketBuildFallback);
			return kInvalidNativeA8CommandIndex;
		}
		const UInt32 commandIndex = static_cast<UInt32>(
			buffer.singlePacketCommands.size());
		const size_t previousSingleCapacity =
			buffer.singlePacketCommands.capacity();
		buffer.singlePacketCommands.emplace_back();
		RecordCommandVectorGrowth(previousSingleCapacity,
			buffer.singlePacketCommands.capacity());
		NativeA8SinglePacketCommand& command =
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
			return kInvalidNativeA8CommandIndex;
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
			return kInvalidNativeA8CommandIndex;
		}

		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandSinglePacketRecorded);
		RecordFreeTypePerf(FreeTypePerfCounter::CommandPacketRecorded);
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandTileRetainedPacketReuse);
		return commandIndex;
	}

	UInt32 AddNativeA8FrameDirectFacadeCommand(
		const A8ShapeMetadata* metadata)
	{
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		SingletonFacadeState* singleton = metadata
			? GetSingletonFacadeState(*metadata) : nullptr;
		if (!buffer.building || !metadata || !singleton)
			return kInvalidNativeA8CommandIndex;

		const UInt64 existingToken =
			singleton->commandValidationToken.load(
				std::memory_order_acquire);
		const UInt32 existingIndex =
			singleton->commandDirectFacadeSinglePacketIndex.load(
				std::memory_order_acquire);
		if (existingToken == buffer.stamp.validationToken
			&& existingIndex != kInvalidNativeA8CommandIndex)
		{
			return existingIndex;
		}

		NativeA8ShapePayload* payload = &metadata->nativePayload;
		const NativeA8DrawCommand& prepared =
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
			return kInvalidNativeA8CommandIndex;
		}

		const NativeA8TileRetainedText* retainedText =
			ResolveTileRetainedText(buffer, singleton->slot.shape,
				*payload, false);
		if (!retainedText || retainedText->packets.size() != 1
			|| !AdoptFrameResourceStamp(buffer,
				prepared.binding.resourceSerial,
				prepared.binding.uploadEpoch))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandBuildViewMiss);
			return kInvalidNativeA8CommandIndex;
		}

		RecordFreeTypePerf(FreeTypePerfCounter::CommandBuildViewHit);
		const UInt32 commandIndex = static_cast<UInt32>(
			buffer.directFacadeSinglePacketCommands.size());
		const size_t previousCapacity =
			buffer.directFacadeSinglePacketCommands.capacity();
		buffer.directFacadeSinglePacketCommands.emplace_back();
		RecordCommandVectorGrowth(previousCapacity,
			buffer.directFacadeSinglePacketCommands.capacity());
		NativeA8DirectFacadeSinglePacketCommand& command =
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

	UInt32 AddNativeA8FrameCommandSpan(NiTriShape* facade,
		const A8ShapeMetadata* metadata, NativeA8ShapePayload* payload)
	{
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		if (!buffer.building || !facade || !metadata || !payload
			|| !payload->buildComplete || !payload->payloadTemplate)
			return kInvalidNativeA8CommandIndex;

		NativeA8CommandSpan span;
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
		const NativeA8TileRetainedText* retainedText =
			ResolveTileRetainedText(buffer, facade, *payload);
		NativeA8FramePayloadBinding payloadBinding;
		if (retainedText
			&& ResolveNativeA8FramePayloadBinding(
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
			return kInvalidNativeA8CommandIndex;
		}

		span.commandCount = static_cast<UInt32>(
			buffer.commands.size() - span.firstCommand);
		span.useCompositePackets =
			span.payload->useCompositePackets;
		if (!retainedText)
		{
			buffer.commands.resize(commandRollback);
			return kInvalidNativeA8CommandIndex;
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
			return kInvalidNativeA8CommandIndex;
		}
		span.runCount = static_cast<UInt32>(
			buffer.runs.size() - span.firstRun);
		if (!span.commandCount || !span.runCount)
		{
			buffer.commands.resize(commandRollback);
			buffer.runs.resize(runRollback);
			return kInvalidNativeA8CommandIndex;
		}

		const NativeA8FramePacketBinding& spanBinding =
			buffer.commands[span.firstCommand].binding;
		if (!AdoptFrameResourceStamp(buffer,
				spanBinding.resourceSerial, spanBinding.uploadEpoch))
		{
			buffer.commands.resize(commandRollback);
			buffer.runs.resize(runRollback);
			return kInvalidNativeA8CommandIndex;
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

	void ActivateNativeA8FrameCommandBuffer()
	{
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
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

	void EndNativeA8FrameCommandBuffer()
	{
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		for (const NativeA8DirectFacadeSinglePacketCommand& command
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
				kInvalidNativeA8CommandIndex,
				std::memory_order_release);
		}
		buffer.active = false;
		buffer.building = false;
		buffer.stamp = {};
		buffer.executionSegment = {};
		buffer.executionBoundaryEpoch = 1;
		buffer.frameExternalMutationEpoch = 0;
		buffer.executionBoundaryReason =
			NativeA8CommandFallback::State;
		buffer.enabled = false;
		buffer.commands.clear();
		buffer.runs.clear();
		buffer.spans.clear();
		buffer.singlePacketCommands.clear();
		buffer.directFacadeSinglePacketCommands.clear();
		TrimCommandCapacity(buffer);
	}

	void InvalidateNativeA8CommandGeometry(NiTriShape* geometry)
	{
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		if (!geometry)
			return;
		NotifyNativeA8CommandExternalMutation(
			NativeA8CommandFallback::Topology);
		InvalidateNativeA8CommandExecutionSegment(
			NativeA8CommandFallback::Topology);
		if (!buffer.active && !buffer.building)
			return;
		bool invalidated = false;
		bool singlePacketInvalidated = false;
		bool directFacadeSinglePacketInvalidated = false;
		for (NativeA8SinglePacketCommand& command
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
					== NativeA8CommandSpanState::Executing;
			command.executionValidationToken = 0;
			command.executionSegmentEpoch = 0;
			command.executionExternalMutationEpoch = 0;
			command.state = NativeA8CommandSpanState::Fault;
			invalidated = true;
			singlePacketInvalidated = true;
		}
		for (NativeA8DirectFacadeSinglePacketCommand& command
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
					== NativeA8CommandSpanState::Executing;
			command.executionValidationToken = 0;
			command.executionSegmentEpoch = 0;
			command.executionExternalMutationEpoch = 0;
			command.state = NativeA8CommandSpanState::Fault;
			invalidated = true;
			directFacadeSinglePacketInvalidated = true;
		}
		for (NativeA8CommandSpan& span : buffer.spans)
		{
			bool member = span.facade == geometry;
			for (UInt32 index = 0;
				!member && index < span.commandCount; ++index)
			{
				const NativeA8DrawCommand& command =
					buffer.commands[span.firstCommand + index];
				member = command.sourceGeometry == geometry
					|| command.expectedGeometry == geometry;
			}
			if (!member)
				continue;
			span.partialDraw = span.partialDraw
				|| span.state == NativeA8CommandSpanState::Executing;
			span.executionValidationToken = 0;
			span.executionSegmentEpoch = 0;
			span.executionExternalMutationEpoch = 0;
			span.state = NativeA8CommandSpanState::Fault;
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
			RecordNativeA8CommandFallback(
				NativeA8CommandFallback::Topology);
		}
	}

	bool FindNativeA8SinglePacketCommand(UInt32 commandIndex,
		UInt64 validationToken, NativeA8SinglePacketCommandView& view)
	{
		view = {};
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		if (!buffer.active || !validationToken
			|| validationToken != buffer.stamp.validationToken
			|| commandIndex >= buffer.singlePacketCommands.size())
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandSinglePacketMiss);
			return false;
		}
		const NativeA8SinglePacketCommand& command =
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

	bool BeginNativeA8SinglePacketCommandExecution(UInt32 commandIndex,
		NiTriShape* geometry, NativeA8SinglePacketCommandView& view)
	{
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		if (!FindNativeA8SinglePacketCommand(commandIndex,
				buffer.stamp.validationToken, view))
		{
			return false;
		}
		NativeA8CommandFallback failure =
			EnsureExecutionSegmentValidated(buffer);
		if (failure != NativeA8CommandFallback::None)
		{
			RecordSinglePacketCommandFallback(failure);
			return false;
		}

		NativeA8SinglePacketCommand& command =
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
				NativeA8CommandFallback::Topology);
			return false;
		}
		if (command.state != NativeA8CommandSpanState::Ready)
		{
			RecordSinglePacketCommandFallback(
				NativeA8CommandFallback::State);
			return false;
		}
		if (!geometry || command.facade != geometry
			|| command.draw.sourceGeometry != geometry)
		{
			command.state = NativeA8CommandSpanState::Fault;
			command.partialDraw = false;
			command.executionValidationToken = 0;
			command.executionSegmentEpoch = 0;
			command.executionExternalMutationEpoch = 0;
			RecordSinglePacketCommandFallback(
				NativeA8CommandFallback::Topology);
			return false;
		}

		command.state = NativeA8CommandSpanState::Executing;
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

	void EndNativeA8SinglePacketCommandExecution(UInt32 commandIndex,
		bool success, bool drewPacket)
	{
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		if (commandIndex >= buffer.singlePacketCommands.size())
			return;
		NativeA8SinglePacketCommand& command =
			buffer.singlePacketCommands[commandIndex];
		command.partialDraw = drewPacket;
		command.executionValidationToken = 0;
		command.executionSegmentEpoch = 0;
		command.executionExternalMutationEpoch = 0;
		command.state = success
			? NativeA8CommandSpanState::Consumed
			: NativeA8CommandSpanState::Fault;
	}

	void AbandonNativeA8SinglePacketCommandExecution(UInt32 commandIndex)
	{
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		if (commandIndex >= buffer.singlePacketCommands.size())
			return;
		NativeA8SinglePacketCommand& command =
			buffer.singlePacketCommands[commandIndex];
		if (command.state != NativeA8CommandSpanState::Executing
			|| command.partialDraw)
		{
			return;
		}
		command.executionValidationToken = 0;
		command.executionSegmentEpoch = 0;
		command.executionExternalMutationEpoch = 0;
		command.state = NativeA8CommandSpanState::Ready;
	}

	bool IsNativeA8SinglePacketCommandConsumed(
		UInt32 commandIndex, UInt64 validationToken)
	{
		const NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		if (!validationToken
			|| validationToken != buffer.stamp.validationToken
			|| commandIndex >= buffer.singlePacketCommands.size())
		{
			return false;
		}
		const NativeA8SinglePacketCommand& command =
			buffer.singlePacketCommands[commandIndex];
		return command.validationToken == validationToken
			&& command.state == NativeA8CommandSpanState::Consumed
			&& command.partialDraw;
	}

	bool FindNativeA8CommandSpan(UInt32 spanIndex,
		UInt64 validationToken, NativeA8CommandSpanView& view)
	{
		view = {};
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		if (!buffer.active || !validationToken
			|| validationToken != buffer.stamp.validationToken
			|| spanIndex >= buffer.spans.size())
		{
			RecordFreeTypePerf(FreeTypePerfCounter::CommandSpanMiss);
			return false;
		}
		const NativeA8CommandSpan& span = buffer.spans[spanIndex];
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

	bool BeginNativeA8CommandSpanExecution(UInt32 spanIndex,
		NiTriShape* geometry, NativeA8CommandSpanView& view)
	{
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		if (!FindNativeA8CommandSpan(spanIndex,
				buffer.stamp.validationToken, view))
		{
			return false;
		}
		NativeA8CommandFallback failure =
			EnsureExecutionSegmentValidated(buffer);
		if (failure != NativeA8CommandFallback::None)
		{
			RecordNativeA8CommandFallback(failure);
			return false;
		}
		NativeA8CommandSpan& span = buffer.spans[spanIndex];
		if (span.validationToken != buffer.stamp.validationToken
			|| span.generation != buffer.stamp.generation
			|| span.atlasTextureEpoch
				!= buffer.stamp.atlasTextureEpoch
			|| !span.payload || !span.payload->payloadTemplate
			|| span.payload->useCompositePackets
				!= span.useCompositePackets)
		{
			RecordNativeA8CommandFallback(
				NativeA8CommandFallback::Topology);
			return false;
		}
		if (span.state != NativeA8CommandSpanState::Ready)
		{
			RecordNativeA8CommandFallback(
				NativeA8CommandFallback::State);
			return false;
		}
		if (span.facade != geometry)
		{
			span.state = NativeA8CommandSpanState::Fault;
			span.partialDraw = false;
			span.executionValidationToken = 0;
			span.executionSegmentEpoch = 0;
			span.executionExternalMutationEpoch = 0;
			RecordNativeA8CommandFallback(
				NativeA8CommandFallback::Topology);
			return false;
		}
		span.state = NativeA8CommandSpanState::Executing;
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

	void EndNativeA8CommandSpanExecution(UInt32 spanIndex,
		bool success, bool drewPacket)
	{
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		if (spanIndex >= buffer.spans.size())
			return;
		NativeA8CommandSpan& span = buffer.spans[spanIndex];
		span.partialDraw = drewPacket;
		span.executionValidationToken = 0;
		span.executionSegmentEpoch = 0;
		span.executionExternalMutationEpoch = 0;
		span.state = success
			? NativeA8CommandSpanState::Consumed
			: NativeA8CommandSpanState::Fault;
	}

	bool ValidateNativeA8Command(UInt32 spanIndex,
		UInt32 commandOffset, NiTriShape* geometry, NiRenderer* renderer)
	{
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandPacketLightValidation);
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		NativeA8CommandFallback commandFailure =
			NativeA8CommandFallback::None;
		if (!buffer.active || spanIndex >= buffer.spans.size())
		{
			commandFailure = NativeA8CommandFallback::State;
		}
		if (commandFailure != NativeA8CommandFallback::None)
		{
			RecordNativeA8CommandFallback(commandFailure);
			return false;
		}

		const NativeA8CommandSpan& span = buffer.spans[spanIndex];
		if (span.state != NativeA8CommandSpanState::Executing
			|| span.executionValidationToken
				!= buffer.stamp.validationToken)
		{
			commandFailure = NativeA8CommandFallback::State;
		}
		else if (commandOffset >= span.commandCount
			|| span.firstCommand + commandOffset
				>= buffer.commands.size())
		{
			commandFailure = NativeA8CommandFallback::Topology;
		}
		else
			commandFailure =
				ValidateExecutionSegmentEpoch(buffer,
					span.executionSegmentEpoch,
					span.executionExternalMutationEpoch);
		if (commandFailure != NativeA8CommandFallback::None)
		{
			RecordNativeA8CommandFallback(commandFailure);
			return false;
		}

		const NativeA8DrawCommand& command =
			buffer.commands[span.firstCommand + commandOffset];
		commandFailure = ValidateDrawCommandState(buffer, command,
			span.payload, commandOffset, geometry, renderer);
		if (commandFailure != NativeA8CommandFallback::None)
		{
			RecordNativeA8CommandFallback(commandFailure);
			return false;
		}
		return true;
	}

	bool GuardNativeA8Command(UInt32 spanIndex,
		UInt32 commandOffset, NiTriShape* geometry, NiRenderer* renderer)
	{
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandPacketEpochGuard);
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		NativeA8CommandFallback commandFailure =
			NativeA8CommandFallback::None;
		if (!buffer.active || spanIndex >= buffer.spans.size())
		{
			commandFailure = NativeA8CommandFallback::State;
		}
		if (commandFailure != NativeA8CommandFallback::None)
		{
			RecordNativeA8CommandFallback(commandFailure);
			return false;
		}

		const NativeA8CommandSpan& span = buffer.spans[spanIndex];
		if (commandOffset >= span.commandCount
			|| span.firstCommand + commandOffset
				>= buffer.commands.size())
		{
			commandFailure = NativeA8CommandFallback::Topology;
		}
		else
		{
			commandFailure = ValidatePacketExecutionGuard(buffer,
				span.state, span.executionValidationToken,
				span.executionSegmentEpoch,
				span.executionExternalMutationEpoch, renderer);
		}
		if (commandFailure == NativeA8CommandFallback::None)
		{
			const NativeA8DrawCommand& command =
				buffer.commands[span.firstCommand + commandOffset];
			if (!geometry
				|| (command.expectedGeometry
					&& command.expectedGeometry != geometry))
			{
				commandFailure = NativeA8CommandFallback::Topology;
			}
		}
		if (commandFailure != NativeA8CommandFallback::None)
		{
			RecordNativeA8CommandFallback(commandFailure);
			return false;
		}
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandPacketStateValidationElided);
		return true;
	}

	bool ValidateNativeA8SinglePacketCommand(UInt32 commandIndex,
		NiTriShape* geometry, NiRenderer* renderer)
	{
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandPacketLightValidation);
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		NativeA8CommandFallback commandFailure =
			NativeA8CommandFallback::None;
		if (!buffer.active
			|| commandIndex >= buffer.singlePacketCommands.size())
		{
			commandFailure = NativeA8CommandFallback::State;
		}
		if (commandFailure != NativeA8CommandFallback::None)
		{
			RecordSinglePacketCommandFallback(commandFailure);
			return false;
		}

		const NativeA8SinglePacketCommand& command =
			buffer.singlePacketCommands[commandIndex];
		if (command.state != NativeA8CommandSpanState::Executing
			|| command.executionValidationToken
				!= buffer.stamp.validationToken)
		{
			commandFailure = NativeA8CommandFallback::State;
		}
		else
		{
			commandFailure = ValidateExecutionSegmentEpoch(buffer,
				command.executionSegmentEpoch,
				command.executionExternalMutationEpoch);
		}
		if (commandFailure == NativeA8CommandFallback::None)
		{
			commandFailure = ValidateDrawCommandState(buffer,
				command.draw, command.payload, 0,
				geometry, renderer);
		}
		if (commandFailure != NativeA8CommandFallback::None)
		{
			RecordSinglePacketCommandFallback(commandFailure);
			return false;
		}
		return true;
	}

	bool GuardNativeA8SinglePacketCommand(UInt32 commandIndex,
		NiTriShape* geometry, NiRenderer* renderer)
	{
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandPacketEpochGuard);
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		NativeA8CommandFallback commandFailure =
			NativeA8CommandFallback::None;
		if (!buffer.active
			|| commandIndex >= buffer.singlePacketCommands.size())
		{
			commandFailure = NativeA8CommandFallback::State;
		}
		if (commandFailure != NativeA8CommandFallback::None)
		{
			RecordSinglePacketCommandFallback(commandFailure);
			return false;
		}

		const NativeA8SinglePacketCommand& command =
			buffer.singlePacketCommands[commandIndex];
		commandFailure = ValidatePacketExecutionGuard(buffer,
			command.state, command.executionValidationToken,
			command.executionSegmentEpoch,
			command.executionExternalMutationEpoch, renderer);
		if (commandFailure == NativeA8CommandFallback::None
			&& (!geometry || command.facade != geometry
				|| command.draw.sourceGeometry != geometry))
		{
			commandFailure = NativeA8CommandFallback::Topology;
		}
		if (commandFailure != NativeA8CommandFallback::None)
		{
			RecordSinglePacketCommandFallback(commandFailure);
			return false;
		}
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandPacketStateValidationElided);
		return true;
	}

	bool FindNativeA8DirectFacadeSinglePacketCommand(UInt32 commandIndex,
		UInt64 validationToken,
		NativeA8DirectFacadeSinglePacketCommandView& view)
	{
		view = {};
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
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
		const NativeA8DirectFacadeSinglePacketCommand& command =
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

	bool BeginNativeA8DirectFacadeSinglePacketCommandExecution(
		UInt32 commandIndex, const A8ShapeMetadata* singletonMetadata,
		NiTriShape* geometry,
		NativeA8DirectFacadeSinglePacketCommandView& view)
	{
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		if (!FindNativeA8DirectFacadeSinglePacketCommand(commandIndex,
				buffer.stamp.validationToken, view))
		{
			return false;
		}
		NativeA8CommandFallback failure =
			EnsureExecutionSegmentValidated(buffer);
		if (failure != NativeA8CommandFallback::None)
		{
			RecordDirectFacadeSinglePacketCommandFallback(failure);
			return false;
		}

		NativeA8DirectFacadeSinglePacketCommand& command =
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
				NativeA8CommandFallback::Topology);
			return false;
		}
		if (command.state != NativeA8CommandSpanState::Ready)
		{
			RecordDirectFacadeSinglePacketCommandFallback(
				NativeA8CommandFallback::State);
			return false;
		}
		if (!geometry || command.geometry != geometry)
		{
			command.state = NativeA8CommandSpanState::Fault;
			command.partialDraw = false;
			command.executionValidationToken = 0;
			command.executionSegmentEpoch = 0;
			command.executionExternalMutationEpoch = 0;
			RecordDirectFacadeSinglePacketCommandFallback(
				NativeA8CommandFallback::Topology);
			return false;
		}

		command.state = NativeA8CommandSpanState::Executing;
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

	void EndNativeA8DirectFacadeSinglePacketCommandExecution(
		UInt32 commandIndex, bool success, bool drewPacket)
	{
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		if (commandIndex
			>= buffer.directFacadeSinglePacketCommands.size())
		{
			return;
		}
		NativeA8DirectFacadeSinglePacketCommand& command =
			buffer.directFacadeSinglePacketCommands[commandIndex];
		command.partialDraw = drewPacket;
		command.executionValidationToken = 0;
		command.executionSegmentEpoch = 0;
		command.executionExternalMutationEpoch = 0;
		command.state = success
			? NativeA8CommandSpanState::Consumed
			: NativeA8CommandSpanState::Fault;
	}

	void AbandonNativeA8DirectFacadeSinglePacketCommandExecution(
		UInt32 commandIndex)
	{
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		if (commandIndex >= buffer.directFacadeSinglePacketCommands.size())
			return;
		NativeA8DirectFacadeSinglePacketCommand& command =
			buffer.directFacadeSinglePacketCommands[commandIndex];
		if (command.state != NativeA8CommandSpanState::Executing
			|| command.partialDraw)
		{
			return;
		}
		command.executionValidationToken = 0;
		command.executionSegmentEpoch = 0;
		command.executionExternalMutationEpoch = 0;
		command.state = NativeA8CommandSpanState::Ready;
	}

	bool IsNativeA8DirectFacadeSinglePacketCommandConsumed(
		UInt32 commandIndex, UInt64 validationToken)
	{
		const NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		if (!validationToken
			|| validationToken != buffer.stamp.validationToken
			|| commandIndex >= buffer.directFacadeSinglePacketCommands.size())
		{
			return false;
		}
		const NativeA8DirectFacadeSinglePacketCommand& command =
			buffer.directFacadeSinglePacketCommands[commandIndex];
		return command.validationToken == validationToken
			&& command.state == NativeA8CommandSpanState::Consumed
			&& command.partialDraw;
	}

	bool ValidateNativeA8DirectFacadeSinglePacketCommand(UInt32 commandIndex,
		NiTriShape* geometry, NiRenderer* renderer)
	{
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandPacketLightValidation);
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		NativeA8CommandFallback commandFailure =
			NativeA8CommandFallback::None;
		if (!buffer.active
			|| commandIndex
				>= buffer.directFacadeSinglePacketCommands.size())
		{
			commandFailure = NativeA8CommandFallback::State;
		}
		if (commandFailure != NativeA8CommandFallback::None)
		{
			RecordDirectFacadeSinglePacketCommandFallback(commandFailure);
			return false;
		}

		const NativeA8DirectFacadeSinglePacketCommand& command =
			buffer.directFacadeSinglePacketCommands[commandIndex];
		SingletonFacadeState* singleton = command.singletonMetadata
			? GetSingletonFacadeState(*command.singletonMetadata)
			: nullptr;
		if (command.state != NativeA8CommandSpanState::Executing
			|| command.executionValidationToken
				!= buffer.stamp.validationToken)
		{
			commandFailure = NativeA8CommandFallback::State;
		}
		else
		{
			commandFailure = ValidateExecutionSegmentEpoch(buffer,
				command.executionSegmentEpoch,
				command.executionExternalMutationEpoch);
		}
		if (commandFailure == NativeA8CommandFallback::None
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
			commandFailure = NativeA8CommandFallback::Topology;
		}
		if (commandFailure == NativeA8CommandFallback::None)
		{
			commandFailure = ValidateDrawCommandState(buffer,
				*command.draw, command.payload, 0,
				geometry, renderer);
		}
		if (commandFailure != NativeA8CommandFallback::None)
		{
			RecordDirectFacadeSinglePacketCommandFallback(commandFailure);
			return false;
		}
		return true;
	}

	bool GuardNativeA8DirectFacadeSinglePacketCommand(UInt32 commandIndex,
		NiTriShape* geometry, NiRenderer* renderer)
	{
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandPacketEpochGuard);
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		NativeA8CommandFallback commandFailure =
			NativeA8CommandFallback::None;
		if (!buffer.active
			|| commandIndex
				>= buffer.directFacadeSinglePacketCommands.size())
		{
			commandFailure = NativeA8CommandFallback::State;
		}
		if (commandFailure != NativeA8CommandFallback::None)
		{
			RecordDirectFacadeSinglePacketCommandFallback(commandFailure);
			return false;
		}

		const NativeA8DirectFacadeSinglePacketCommand& command =
			buffer.directFacadeSinglePacketCommands[commandIndex];
		SingletonFacadeState* singleton = command.singletonMetadata
			? GetSingletonFacadeState(*command.singletonMetadata)
			: nullptr;
		commandFailure = ValidatePacketExecutionGuard(buffer,
			command.state, command.executionValidationToken,
			command.executionSegmentEpoch,
			command.executionExternalMutationEpoch, renderer);
		if (commandFailure == NativeA8CommandFallback::None
			&& (!singleton
				|| singleton->frameMode.load(std::memory_order_acquire)
					!= SingletonFacadeFrameMode::Direct
				|| !geometry || command.geometry != geometry
				|| !command.draw
				|| command.draw->sourceGeometry != geometry
				|| command.draw->expectedGeometry != geometry))
		{
			commandFailure = NativeA8CommandFallback::Topology;
		}
		if (commandFailure != NativeA8CommandFallback::None)
		{
			RecordDirectFacadeSinglePacketCommandFallback(commandFailure);
			return false;
		}
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandPacketStateValidationElided);
		return true;
	}

	void RecordNativeA8CommandFallback(
		NativeA8CommandFallback reason)
	{
		switch (reason)
		{
		case NativeA8CommandFallback::Token:
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandFallbackToken);
			break;
		case NativeA8CommandFallback::Generation:
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandFallbackGeneration);
			break;
		case NativeA8CommandFallback::Atlas:
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandFallbackAtlas);
			break;
		case NativeA8CommandFallback::Resource:
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandFallbackResource);
			break;
		case NativeA8CommandFallback::Topology:
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandFallbackTopology);
			break;
		case NativeA8CommandFallback::Hook:
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandFallbackHook);
			break;
		case NativeA8CommandFallback::Nested:
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandFallbackNested);
			break;
		case NativeA8CommandFallback::RenderTarget:
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandFallbackRenderTarget);
			break;
		case NativeA8CommandFallback::State:
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandFallbackState);
			break;
		default:
			break;
		}
	}
}

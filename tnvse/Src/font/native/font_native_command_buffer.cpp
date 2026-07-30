#include "font_a8_internal.h"
#include "font_native_internal.h"

#include "load_config.h"

#include "NiGeometryBufferData.hpp"
#include "NiMaterialProperty.hpp"
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
			// stock/non-FreeType boundary or an externally published resource
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
			CpuMemoryLease cpuMemory;
			NativeA8ExecutionSegmentState executionSegment;
			UInt32 executionBoundaryEpoch = 1;
			UInt32 frameExternalMutationEpoch = 0;
			NativeA8CommandFallback executionBoundaryReason =
				NativeA8CommandFallback::State;
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

		bool EqualTransform(const NiTransform& left,
			const NiTransform& right)
		{
			return std::memcmp(&left, &right, sizeof(left)) == 0;
		}

		bool EqualFloat(float left, float right)
		{
			return std::memcmp(&left, &right, sizeof(float)) == 0;
		}

		bool SameVirtualTileState(const NiTriShape* left,
			const NiTriShape* right)
		{
			if (!left || !right
				|| !EqualTransform(left->m_kLocal, right->m_kLocal)
				|| !EqualTransform(left->m_kWorld, right->m_kWorld)
				|| left->m_uiFlags.Get() != right->m_uiFlags.Get()
				|| std::memcmp(
					reinterpret_cast<const UInt8*>(left) + 0xC4,
					reinterpret_cast<const UInt8*>(right) + 0xC4,
					0x10) != 0)
			{
				return false;
			}
			const NiAlphaProperty* leftAlpha = left->GetAlphaProperty();
			const NiAlphaProperty* rightAlpha = right->GetAlphaProperty();
			if (!leftAlpha || !rightAlpha
				|| leftAlpha->m_usFlags.Get()
					!= rightAlpha->m_usFlags.Get()
				|| leftAlpha->m_ucAlphaTestRef
					!= rightAlpha->m_ucAlphaTestRef)
			{
				return false;
			}
			const NiMaterialProperty* leftMaterial =
				left->GetMaterialProperty();
			const NiMaterialProperty* rightMaterial =
				right->GetMaterialProperty();
			const float leftMaterialAlpha =
				leftMaterial ? leftMaterial->m_fAlpha : 1.0f;
			const float rightMaterialAlpha =
				rightMaterial ? rightMaterial->m_fAlpha : 1.0f;
			if (!EqualFloat(leftMaterialAlpha, rightMaterialAlpha))
				return false;

			const NiShadeProperty* leftShade = left->GetShadeProperty();
			const NiShadeProperty* rightShade = right->GetShadeProperty();
			if (!leftShade || !rightShade
				|| leftShade->m_eShaderType != NiShadeProperty::PROP_Tile
				|| rightShade->m_eShaderType != NiShadeProperty::PROP_Tile)
			{
				return false;
			}
			const auto* leftTile = reinterpret_cast<
				const CommandTileShaderPropertyView*>(leftShade);
			const auto* rightTile = reinterpret_cast<
				const CommandTileShaderPropertyView*>(rightShade);
			return leftTile->m_usFlags.Get()
					== rightTile->m_usFlags.Get()
				&& leftTile->ulFlags[0] == rightTile->ulFlags[0]
				&& leftTile->ulFlags[1] == rightTile->ulFlags[1]
				&& EqualFloat(leftTile->fAlpha, rightTile->fAlpha)
				&& EqualFloat(
					leftTile->fFadeAlpha, rightTile->fFadeAlpha)
				&& EqualFloat(leftTile->fEnvMapScale,
					rightTile->fEnvMapScale)
				&& EqualFloat(
					leftTile->fLODFade, rightTile->fLODFade)
				&& EqualFloat(
					leftTile->fDepthBias, rightTile->fDepthBias)
				&& leftTile->uiShaderIndex
					== rightTile->uiShaderIndex
				&& leftTile->alphaTexture.m_pObject
					== rightTile->alphaTexture.m_pObject
				&& std::memcmp(&leftTile->overlayColor,
					&rightTile->overlayColor,
					sizeof(leftTile->overlayColor)) == 0
				&& EqualFloat(leftTile->tileAlpha, rightTile->tileAlpha)
				&& std::memcmp(&leftTile->textureTransform,
					&rightTile->textureTransform,
					sizeof(leftTile->textureTransform)) == 0
				&& leftTile->clampMode == rightTile->clampMode
				&& leftTile->byte90 == rightTile->byte90
				&& leftTile->rotates == rightTile->rotates
				&& leftTile->hasVertexColors
					== rightTile->hasVertexColors
				&& leftTile->noTexture == rightTile->noTexture
				&& std::memcmp(&leftTile->scissorRect,
					&rightTile->scissorRect,
					sizeof(leftTile->scissorRect)) == 0
				&& leftTile->useScissorTest
					== rightTile->useScissorTest
				&& left->GetCullingProperty()
					== right->GetCullingProperty()
				&& left->GetStencilProperty()
					== right->GetStencilProperty();
		}

		void RefreshCommandMemory(NativeA8FrameCommandBuffer& buffer)
		{
			const size_t bytes =
				buffer.commands.capacity() * sizeof(NativeA8DrawCommand)
				+ buffer.runs.capacity()
					* sizeof(NativeA8FrameCommandRun)
				+ buffer.spans.capacity() * sizeof(NativeA8CommandSpan)
				+ buffer.singlePacketCommands.capacity()
					* sizeof(NativeA8SinglePacketCommand);
			buffer.cpuMemory.Reset(
				CpuMemoryCategory::RuntimeMetadata, bytes);
		}

		void TrimCommandCapacity(NativeA8FrameCommandBuffer& buffer)
		{
			if (buffer.commands.capacity() > 16384)
				std::vector<NativeA8DrawCommand>().swap(buffer.commands);
			if (buffer.runs.capacity() > 8192)
				std::vector<NativeA8FrameCommandRun>().swap(buffer.runs);
			if (buffer.spans.capacity() > 8192)
				std::vector<NativeA8CommandSpan>().swap(buffer.spans);
			if (buffer.singlePacketCommands.capacity() > 8192)
			{
				std::vector<NativeA8SinglePacketCommand>().swap(
					buffer.singlePacketCommands);
			}
			RefreshCommandMemory(buffer);
			if (!IsCpuMemoryBudgetExceeded())
				return;
			std::vector<NativeA8DrawCommand>().swap(buffer.commands);
			std::vector<NativeA8FrameCommandRun>().swap(buffer.runs);
			std::vector<NativeA8CommandSpan>().swap(buffer.spans);
			std::vector<NativeA8SinglePacketCommand>().swap(
				buffer.singlePacketCommands);
			buffer.cpuMemory.Release();
		}

		bool ResolveRenderTargetStamp(NativeA8FrameStamp& stamp)
		{
			if (!stamp.device)
				return false;
			IDirect3DSurface9* renderTarget = nullptr;
			const HRESULT targetResult =
				stamp.device->GetRenderTarget(0, &renderTarget);
			if (FAILED(targetResult) || !renderTarget)
				return false;
			stamp.renderTarget = renderTarget;
			renderTarget->Release();
			stamp.renderTargetReady = true;
			const HRESULT viewportResult =
				stamp.device->GetViewport(&stamp.viewport);
			stamp.viewportReady = SUCCEEDED(viewportResult)
				&& stamp.viewport.Width && stamp.viewport.Height;
			return stamp.viewportReady;
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
			const NativeA8PacketTemplate& packet, UInt32 packetIndex,
			NativeA8DrawCommand& command)
		{
			command = {};
			command.sourceGeometry = facade;
			command.payload = &payload;
			command.packet = &packet;
			command.packetIndex = packetIndex;
			if (packet.atlasPage >= payload.preflightAtlasTextures.size())
				return false;
			command.atlasTexture =
				payload.preflightAtlasTextures[packet.atlasPage];
			if (!command.atlasTexture
				|| packetIndex >= payload.packetShaders.size()
				|| packetIndex >= payload.packetPrograms.size()
				|| !ResolveNativeA8FramePacketBinding(
					payload, packetIndex, command.binding)
				|| !(command.program =
					payload.packetPrograms[packetIndex])
				|| !command.program->active
				|| command.program->shader
					!= payload.packetShaders[packetIndex]
				|| command.program->generation
					!= payload.preparedGeneration)
			{
				return false;
			}
			return true;
		}

		bool AppendCompatibilityCommand(
			NativeA8FrameCommandBuffer& buffer,
			NiTriShape* facade, NativeA8ShapePayload& payload,
			const NativeA8PacketTemplate& packet, UInt32 packetIndex)
		{
			NativeA8DrawCommand command;
			if (!CompileCompatibilityCommand(facade, payload,
					packet, packetIndex, command))
			{
				return false;
			}
			buffer.commands.push_back(command);
			return true;
		}

		bool AppendVirtualCommand(
			NativeA8FrameCommandBuffer& buffer,
			NativeA8ShapePayload& payload,
			const NativeA8PacketTemplate& packet,
			const VirtualStockSlotBinding& slot, UInt32 packetIndex)
		{
			NativeA8DrawCommand command;
			command.sourceGeometry = slot.shape;
			command.expectedGeometry = slot.shape;
			command.payload = &payload;
			command.packet = &packet;
			command.packetIndex = packetIndex;
			if (!slot.bound || slot.packetIndex != packetIndex
				|| packet.atlasPage
					>= payload.preflightAtlasTextures.size())
			{
				return false;
			}
			command.atlasTexture =
				payload.preflightAtlasTextures[packet.atlasPage];
			NativeA8FramePacketBinding resolved;
			if (!command.atlasTexture
				|| packetIndex >= payload.packetShaders.size()
				|| packetIndex >= payload.packetPrograms.size()
				|| !ResolveNativeA8FramePacketBinding(
					payload, packetIndex, resolved)
				|| !(command.program =
					payload.packetPrograms[packetIndex])
				|| !command.program->active
				|| command.program->shader
					!= payload.packetShaders[packetIndex]
				|| command.program->generation
					!= payload.preparedGeneration)
			{
				return false;
			}
			command.binding = resolved;
			command.binding.vertexBuffer =
				slot.bindingChip ? slot.bindingChip->m_pkVB : nullptr;
			command.binding.indexBuffer =
				slot.bindingBuffer ? slot.bindingBuffer->m_pkIB : nullptr;
			command.binding.declaration =
				slot.bindingBuffer
					? static_cast<IDirect3DVertexDeclaration9*>(
						slot.bindingBuffer->m_hDeclaration) : nullptr;
			command.binding.baseVertex = slot.baseVertex;
			command.binding.vertexCount = slot.vertexCount;
			command.binding.indexBytes =
				slot.bindingBuffer ? slot.bindingBuffer->m_uiIBSize : 0;
			command.binding.generation = slot.generation;
			command.binding.resourceSerial = slot.resourceSerial;
			command.binding.active = command.binding.vertexBuffer
				&& command.binding.indexBuffer
				&& command.binding.declaration
				&& command.binding.vertexBuffer == resolved.vertexBuffer
				&& command.binding.indexBuffer == resolved.indexBuffer
				&& command.binding.declaration == resolved.declaration
				&& command.binding.baseVertex == resolved.baseVertex
				&& command.binding.vertexCount == resolved.vertexCount;
			if (!command.binding.active)
				return false;
			buffer.commands.push_back(command);
			return true;
		}

		bool BuildFrameRuns(NativeA8FrameCommandBuffer& buffer,
			NativeA8CommandSpan& span,
			const std::vector<NativeA8RetainedRun>& retainedRuns)
		{
			UInt32 coveredPackets = 0;
			bool allBridgeEligible = !retainedRuns.empty();
			for (const NativeA8RetainedRun& retained : retainedRuns)
			{
				if (!retained.packetCount
					|| retained.firstPacket != coveredPackets
					|| retained.firstPacket + retained.packetCount
						> span.commandCount)
				{
					return false;
				}
				allBridgeEligible = allBridgeEligible
					&& retained.bridgeEligible;
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
						if (!candidate.program || !firstCommand.program
							|| candidate.program->profile
								!= firstCommand.program->profile)
						{
							break;
						}
						if (span.virtualStock
							&& !SameVirtualTileState(
								firstCommand.expectedGeometry,
								candidate.expectedGeometry))
						{
							break;
						}
						++end;
					}
					NativeA8FrameCommandRun run;
					run.firstCommand = span.firstCommand + first;
					run.commandCount = end - first;
					run.bridgeEligible = retained.bridgeEligible;
					if (!buffer.runs.empty()
						&& buffer.runs.size() > span.firstRun)
					{
						const NativeA8FrameCommandRun& previous =
							buffer.runs.back();
						const NativeA8DrawCommand& previousCommand =
							buffer.commands[
								previous.firstCommand
									+ previous.commandCount - 1u];
						run.continuesBridgeSpan = !span.virtualStock
							|| SameVirtualTileState(
								previousCommand.expectedGeometry,
								firstCommand.expectedGeometry);
					}
					buffer.runs.push_back(run);
					first = end;
				}
				coveredPackets += retained.packetCount;
			}
			span.bridgeEligible =
				span.commandCount > 1 && allBridgeEligible;
			return coveredPackets == span.commandCount;
		}

		bool ValidateRenderTarget(
			const NativeA8FrameStamp& stamp)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandRenderTargetValidation);
			if (!stamp.renderTargetReady || !stamp.viewportReady
				|| !stamp.device)
			{
				return false;
			}
			IDirect3DSurface9* currentTarget = nullptr;
			if (FAILED(stamp.device->GetRenderTarget(
					0, &currentTarget))
				|| !currentTarget)
			{
				return false;
			}
			const bool sameTarget =
				currentTarget == stamp.renderTarget;
			currentTarget->Release();
			D3DVIEWPORT9 viewport = {};
			return sameTarget
				&& SUCCEEDED(stamp.device->GetViewport(&viewport))
				&& std::memcmp(&viewport, &stamp.viewport,
					sizeof(viewport)) == 0;
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
			if (!IsNativeA8AccumulatorHookCurrent()
				|| !IsNativeA8SortedTraversalHookCurrent()
				|| !IsA8TileRenderPassHookCurrent())
			{
				return NativeA8CommandFallback::Hook;
			}

			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device =
				renderer ? renderer->GetD3DDevice() : nullptr;
			if (!renderer || renderer != stamp.renderer
				|| !device || device != stamp.device
				|| !IsNativeA8ShaderGenerationCurrent(
					stamp.generation))
			{
				return NativeA8CommandFallback::Generation;
			}
			if (GetNativeA8AtlasTextureEpoch()
				!= stamp.atlasTextureEpoch)
			{
				return NativeA8CommandFallback::Atlas;
			}
			if (!IsNativeA8FrameResourceStampCurrent(
				stamp.generation, stamp.resourceSerial,
				stamp.uploadEpoch))
			{
				return NativeA8CommandFallback::Resource;
			}
			if (!ValidateRenderTarget(stamp))
				return NativeA8CommandFallback::RenderTarget;
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
		ResolveRenderTargetStamp(buffer.stamp);
		buffer.building = true;
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

		const std::vector<NativeA8PacketTemplate>& packets =
			GetNativeA8Packets(*payload->payloadTemplate,
				payload->useCompositePackets);
		if (packets.size() != 1)
			return kInvalidNativeA8CommandIndex;
		if (payload->packetShaders.size() != 1
			|| payload->packetPrograms.size() != 1)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					CommandSinglePacketBuildFallback);
			return kInvalidNativeA8CommandIndex;
		}

		NativeA8SinglePacketCommand command;
		command.facade = facade;
		command.payload = payload;
		command.artifact = payload->payloadTemplate.get();
		command.validationToken = buffer.stamp.validationToken;
		command.generation = buffer.stamp.generation;
		command.atlasTextureEpoch = buffer.stamp.atlasTextureEpoch;
		command.useCompositePackets = payload->useCompositePackets;
		if (!CompileCompatibilityCommand(facade, *payload,
				packets.front(), 0, command.draw))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					CommandSinglePacketBuildFallback);
			return kInvalidNativeA8CommandIndex;
		}

		const UInt32 resourceSerial =
			command.draw.binding.resourceSerial;
		const UInt32 uploadEpoch = command.draw.binding.uploadEpoch;
		if (!resourceSerial
			|| (buffer.stamp.resourceSerial
				&& (buffer.stamp.resourceSerial != resourceSerial
					|| buffer.stamp.uploadEpoch != uploadEpoch)))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					CommandSinglePacketBuildFallback);
			return kInvalidNativeA8CommandIndex;
		}
		if (!buffer.stamp.resourceSerial)
		{
			buffer.stamp.resourceSerial = resourceSerial;
			buffer.stamp.uploadEpoch = uploadEpoch;
		}

		const UInt32 commandIndex = static_cast<UInt32>(
			buffer.singlePacketCommands.size());
		buffer.singlePacketCommands.push_back(command);
		RecordFreeTypePerf(
			FreeTypePerfCounter::CommandSinglePacketRecorded);
		RecordFreeTypePerf(FreeTypePerfCounter::CommandPacketRecorded);
		return commandIndex;
	}

	UInt32 AddNativeA8FrameCommandSpan(NiTriShape* facade,
		const A8ShapeMetadata* metadata, NativeA8ShapePayload* payload,
		VirtualStockShapeGroup* virtualStockGroup)
	{
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		if (!buffer.building || (!metadata && !virtualStockGroup))
			return kInvalidNativeA8CommandIndex;

		if (virtualStockGroup)
		{
			const UInt64 existingToken =
				virtualStockGroup->commandValidationToken.load(
					std::memory_order_acquire);
			const UInt32 existingIndex =
				virtualStockGroup->commandSpanIndex.load(
					std::memory_order_acquire);
			if (existingToken == buffer.stamp.validationToken
				&& existingIndex != kInvalidNativeA8CommandIndex)
			{
				return existingIndex;
			}
		}

		NativeA8CommandSpan span;
		span.facade = facade;
		span.metadata = metadata;
		span.payload = payload;
		span.virtualStockGroup = virtualStockGroup;
		span.virtualStock = virtualStockGroup != nullptr;
		span.validationToken = buffer.stamp.validationToken;
		span.generation = buffer.stamp.generation;
		span.atlasTextureEpoch = buffer.stamp.atlasTextureEpoch;
		span.firstCommand = static_cast<UInt32>(buffer.commands.size());
		span.firstRun = static_cast<UInt32>(buffer.runs.size());

		const size_t commandRollback = buffer.commands.size();
		const size_t runRollback = buffer.runs.size();
		bool appended = false;
		if (virtualStockGroup)
		{
			std::lock_guard<std::mutex> lock(
				virtualStockGroup->mutex);
			if (!virtualStockGroup->primaryMetadataOwner
				|| virtualStockGroup->preparedValidationToken
					!= buffer.stamp.validationToken
				|| virtualStockGroup->frameMode.load(
					std::memory_order_acquire)
					!= VirtualStockFrameMode::Direct
				|| virtualStockGroup->duplicateRegistration
				|| !virtualStockGroup->registrationContiguous
				|| virtualStockGroup->registeredSlotCount
					!= virtualStockGroup->slots.size())
			{
				return kInvalidNativeA8CommandIndex;
			}
			payload =
				&virtualStockGroup->primaryMetadataOwner->nativePayload;
			span.payload = payload;
			span.facade = virtualStockGroup->primaryShape;
			span.metadata =
				virtualStockGroup->primaryMetadataOwner.get();
			const std::vector<NativeA8PacketTemplate>& packets =
				GetNativeA8Packets(*payload->payloadTemplate,
					payload->useCompositePackets);
			if (packets.size() != virtualStockGroup->slots.size())
				return kInvalidNativeA8CommandIndex;
			for (UInt32 index = 0;
				index < packets.size(); ++index)
			{
				if (!AppendVirtualCommand(buffer, *payload,
					packets[index], virtualStockGroup->slots[index],
					index))
				{
					appended = false;
					break;
				}
				appended = true;
			}
		}
		else if (facade && payload && payload->buildComplete
			&& payload->payloadTemplate)
		{
			const std::vector<NativeA8PacketTemplate>& packets =
				GetNativeA8Packets(*payload->payloadTemplate,
					payload->useCompositePackets);
			for (UInt32 index = 0;
				index < packets.size(); ++index)
			{
				if (!AppendCompatibilityCommand(buffer, facade,
					*payload, packets[index], index))
				{
					appended = false;
					break;
				}
				appended = true;
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
		const std::vector<NativeA8RetainedRun>& retainedRuns =
			GetNativeA8RetainedRuns(*span.payload->payloadTemplate,
				span.useCompositePackets);
		if (!BuildFrameRuns(buffer, span, retainedRuns))
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

		UInt32 spanResourceSerial = 0;
		UInt32 spanUploadEpoch = 0;
		for (UInt32 index = 0; index < span.commandCount; ++index)
		{
			const NativeA8DrawCommand& command =
				buffer.commands[span.firstCommand + index];
			if (!spanResourceSerial)
			{
				spanResourceSerial =
					command.binding.resourceSerial;
				spanUploadEpoch =
					command.binding.uploadEpoch;
			}
			else if (spanResourceSerial
					!= command.binding.resourceSerial
				|| spanUploadEpoch
					!= command.binding.uploadEpoch)
			{
				buffer.commands.resize(commandRollback);
				buffer.runs.resize(runRollback);
				return kInvalidNativeA8CommandIndex;
			}
		}
		if ((buffer.stamp.resourceSerial
				&& (buffer.stamp.resourceSerial != spanResourceSerial
					|| buffer.stamp.uploadEpoch != spanUploadEpoch))
			|| !spanResourceSerial)
		{
			buffer.commands.resize(commandRollback);
			buffer.runs.resize(runRollback);
			return kInvalidNativeA8CommandIndex;
		}
		if (!buffer.stamp.resourceSerial)
		{
			buffer.stamp.resourceSerial = spanResourceSerial;
			buffer.stamp.uploadEpoch = spanUploadEpoch;
		}

		const UInt32 spanIndex = static_cast<UInt32>(
			buffer.spans.size());
		buffer.spans.push_back(span);
		if (virtualStockGroup)
		{
			virtualStockGroup->commandLeaderSlot.store(
				0, std::memory_order_relaxed);
			virtualStockGroup->commandSpanIndex.store(
				spanIndex, std::memory_order_relaxed);
			virtualStockGroup->commandValidationToken.store(
				buffer.stamp.validationToken,
				std::memory_order_release);
		}
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
				|| !buffer.singlePacketCommands.empty());
		if (buffer.active)
			RecordFreeTypePerf(FreeTypePerfCounter::CommandRecorded);
		RefreshCommandMemory(buffer);
	}

	void EndNativeA8FrameCommandBuffer()
	{
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		for (const NativeA8CommandSpan& span : buffer.spans)
		{
			VirtualStockShapeGroup* group =
				span.virtualStockGroup;
			if (!group
				|| group->commandValidationToken.load(
					std::memory_order_acquire)
					!= span.validationToken)
			{
				continue;
			}
			group->commandValidationToken.store(
				0, std::memory_order_release);
			group->commandSpanIndex.store(
				kInvalidNativeA8CommandIndex,
				std::memory_order_release);
			group->commandLeaderSlot.store(
				0, std::memory_order_release);
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
		NiTriShape* geometry, bool virtualLeader,
		NativeA8CommandSpanView& view)
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
		if (span.state != NativeA8CommandSpanState::Ready
			|| span.virtualStock != virtualLeader)
		{
			RecordNativeA8CommandFallback(
				NativeA8CommandFallback::State);
			return false;
		}
		const NativeA8DrawCommand& first =
			buffer.commands[span.firstCommand];
		if ((!span.virtualStock && span.facade != geometry)
			|| (span.virtualStock
				&& first.expectedGeometry != geometry))
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

	bool ShouldConsumeNativeA8CommandFollower(UInt32 spanIndex,
		UInt64 validationToken, NiTriShape* geometry,
		UInt32 commandOffset)
	{
		NativeA8FrameCommandBuffer& buffer = s_commandBuffer;
		if (!buffer.active || validationToken
				!= buffer.stamp.validationToken
			|| spanIndex >= buffer.spans.size())
		{
			return false;
		}
		NativeA8CommandSpan& span = buffer.spans[spanIndex];
		if (!span.virtualStock
			|| commandOffset >= span.commandCount)
			return false;
		if (buffer.commands[
				span.firstCommand + commandOffset].
					expectedGeometry != geometry)
			return false;
		if (span.state == NativeA8CommandSpanState::Consumed
			|| (span.state == NativeA8CommandSpanState::Fault
				&& span.partialDraw)
			|| span.state == NativeA8CommandSpanState::Executing)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandVirtualFollowerConsumed);
			return true;
		}
		if (span.state == NativeA8CommandSpanState::Ready
			&& geometry
				!= buffer.commands[span.firstCommand].
					expectedGeometry)
		{
			// Unexpected traversal order: keep every packet on the current
			// per-shape route and prevent a later leader from replaying it.
			span.state = NativeA8CommandSpanState::Fault;
			span.partialDraw = false;
			span.executionValidationToken = 0;
			span.executionSegmentEpoch = 0;
			span.executionExternalMutationEpoch = 0;
			RecordNativeA8CommandFallback(
				NativeA8CommandFallback::Topology);
		}
		return false;
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

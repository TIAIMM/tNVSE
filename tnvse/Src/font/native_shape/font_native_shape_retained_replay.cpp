#include "font_native_shape_hooks_detail.h"
#include "font_native_shape_standard_lite_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_native_shape_hooks {}
	using namespace implementation::font_native_shape_hooks;

	namespace implementation::font_native_shape_hooks
	{
		struct NativeBridgeExecutionContext
		{
			NativeFontCommandSpanView view;
			NiTriShape* facade = nullptr;
			NativeFontShapePayload* payload = nullptr;
			NativeFontRingSubmission* ringSubmission = nullptr;
			UInt32 bootstrapCommandOffset = 0;
			UInt32 nextCommandOffset = 0;
			UInt32 endCommandOffset = 0;
			void* boundProfile = nullptr;
			NativeFontCommandBindState bindState;
			UInt32 drewPackets = 0;
			NativeFontFallbackReason failure =
				NativeFontFallbackReason::RuntimeFault;
			const char* operation = "retained-bridge";
			HRESULT result = E_FAIL;
			bool packetStatePrevalidated = false;
			bool failed = false;
			bool constantStateFault = false;
		};

		void FailRetainedBridge(NativeBridgeExecutionContext& context,
			NativeFontFallbackReason failure, const char* operation,
			HRESULT result, bool constantStateFault = false)
		{
			context.failure = failure;
			context.operation = operation;
			context.result = result;
			context.constantStateFault = constantStateFault;
			context.failed = true;
		}

		bool DrawRetainedBridgeCommand(
			NativeBridgeExecutionContext& context,
			const NativeFontDrawCommand& command, UInt32 commandOffset,
			NiRenderer* renderer, bool alternate)
		{
			NiTriShape* geometry = nullptr;
			if (!context.facade || !context.payload
				|| !context.ringSubmission
				|| command.packetIndex != commandOffset)
			{
				FailRetainedBridge(context,
					NativeFontFallbackReason::PacketBuild,
					"retained-command-order", E_FAIL);
				return false;
			}
			const NativeFontFallbackReason prepare =
				PrepareNativeFontRingPacket(context.facade,
					*context.payload, *context.ringSubmission,
					command.packetIndex, geometry);
			if (prepare != NativeFontFallbackReason::None || !geometry)
			{
				FailRetainedBridge(context,
					prepare != NativeFontFallbackReason::None
						? prepare
						: NativeFontFallbackReason::RuntimeFault,
					"retained-ring-packet", E_FAIL);
				return false;
			}

			auto drawGeometry = [&]() -> bool
			{
				const bool commandValid =
					context.packetStatePrevalidated
						? GuardNativeFontCommand(
							context.view.spanIndex, commandOffset,
							geometry, renderer)
						: ValidateNativeFontCommand(
							context.view.spanIndex, commandOffset,
							geometry, renderer);
				if (!commandValid)
				{
					FailRetainedBridge(context,
						NativeFontFallbackReason::PacketPrepare,
						"retained-command-binding", E_FAIL);
					return false;
				}
				const bool publishPrograms =
					context.boundProfile != command.program->profile;
				const char* operation = "none";
				HRESULT result = D3D_OK;
				if (!BindNativeFontCommandPacket(*command.program,
					command.atlasTexture, publishPrograms,
					&geometry->m_kProperties,
					context.bindState,
					operation, result))
				{
					FailRetainedBridge(context,
						NativeFontFallbackReason::RuntimeFault,
						operation, result, true);
					return false;
				}
				context.boundProfile = command.program->profile;
				RenderImmediateFn immediate = alternate
					? State().originalRenderImmediateAlt
					: State().originalRenderImmediate;
				if (!immediate)
				{
					FailRetainedBridge(context,
						NativeFontFallbackReason::RuntimeFault,
						"retained-immediate-missing", E_FAIL);
					return false;
				}
				immediate(geometry, renderer);
				++context.drewPackets;
				RecordRetainedPacketDraw(command, true);
				if (!IsNativeFontShaderGenerationCurrent(
					command.program->generation))
				{
					FailRetainedBridge(context,
						NativeFontFallbackReason::DeviceReset,
						"retained-generation-after-draw",
						D3DERR_DEVICELOST);
					return false;
				}
				return true;
			};

			return drawGeometry();
		}

		bool ContinueRetainedBridge(
			void* opaque, NiRenderer* renderer, bool alternate)
		{
			auto* context =
				static_cast<NativeBridgeExecutionContext*>(opaque);
			if (!context || context->failed)
				return false;
			while (context->nextCommandOffset
				< context->endCommandOffset)
			{
				const UInt32 commandOffset =
					context->nextCommandOffset++;
				const NativeFontDrawCommand* command =
					ResolveNativeCommand(context->view, commandOffset);
				if (!command || !DrawRetainedBridgeCommand(
					*context, *command, commandOffset,
					renderer, alternate))
				{
					return false;
				}
			}
			const NativeFontDrawCommand* bootstrap =
				ResolveNativeCommand(
					context->view, context->bootstrapCommandOffset);
			if (!bootstrap || !bootstrap->program)
			{
				FailRetainedBridge(*context,
					NativeFontFallbackReason::PacketBuild,
					"retained-bootstrap-command", E_FAIL);
				return false;
			}
			// B994F0's global selected-shader identity still names the bootstrap
			// profile. Restore the corresponding D3D program/texture before its
			// one vanilla slot-35 cleanup returns, otherwise the next Tile can skip
			// B99390 while the driver still has the final retained profile bound.
			const char* operation = "none";
			HRESULT result = D3D_OK;
			NiTriShape* bootstrapGeometry = context->ringSubmission
				? context->ringSubmission->proxyShape : nullptr;
			if (!BindNativeFontCommandPacket(*bootstrap->program,
				bootstrap->atlasTexture,
				context->boundProfile
					!= bootstrap->program->profile,
				bootstrapGeometry
					? &bootstrapGeometry->m_kProperties : nullptr,
				context->bindState,
				operation, result))
			{
				FailRetainedBridge(*context,
					NativeFontFallbackReason::RuntimeFault,
					operation, result, true);
				return false;
			}
			context->boundProfile = bootstrap->program->profile;
			return true;
		}

		bool ResolveNextBridgeGroup(
			const NativeFontCommandSpanView& view, UInt32& runCursor,
			UInt32& firstCommandOffset, UInt32& endCommandOffset)
		{
			if (!view.span || !view.runs
				|| runCursor >= view.span->runCount)
			{
				return false;
			}
			const UInt32 spanFirst = view.span->firstCommand;
			const UInt32 spanEnd =
				spanFirst + view.span->commandCount;
			const NativeFontFrameCommandRun& firstRun =
				view.runs[view.span->firstRun + runCursor];
			if (!firstRun.commandCount
				|| firstRun.firstCommand < spanFirst
				|| firstRun.firstCommand + firstRun.commandCount
					> spanEnd)
			{
				return false;
			}
			UInt32 end = firstRun.firstCommand
				+ firstRun.commandCount;
			++runCursor;
			while (runCursor < view.span->runCount)
			{
				const NativeFontFrameCommandRun& next =
					view.runs[view.span->firstRun + runCursor];
				if (!next.continuesBridgeSpan)
					break;
				if (!next.commandCount || next.firstCommand != end
					|| next.firstCommand + next.commandCount
						> spanEnd)
				{
					return false;
				}
				end = next.firstCommand + next.commandCount;
				++runCursor;
			}
			firstCommandOffset = firstRun.firstCommand - spanFirst;
			endCommandOffset = end - spanFirst;
			return firstCommandOffset < endCommandOffset;
		}

		bool TryDrawNativeRetainedSpan(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool setupRenderStates, NiTriShape* facade,
			NativeFontShapePayload& payload,
			UInt32 commandSpanIndex,
			NativePacketDrawResult& draw)
		{
			if (!IsFreeTypeFontCommandBufferEnabledForCurrentRoute()
				|| !pass || !facade
				|| commandSpanIndex == kInvalidNativeFontCommandIndex)
			{
				return false;
			}
			if (currentPass == kForcedShaderSelectionPass
				|| !IsDefaultNativeReplayPass(currentPass))
			{
				RecordNativeFontCommandFallback(
					NativeFontCommandFallback::State);
				return false;
			}

			NativeFontCommandSpanView view;
			if (!BeginNativeFontCommandSpanExecution(
				commandSpanIndex, facade, view))
			{
				return false;
			}
			if (!view.span || view.span->payload != &payload
				|| view.span->commandCount < 2
				|| !view.span->bridgeEligible)
			{
				EndNativeFontCommandSpanExecution(
					commandSpanIndex, false, false);
				RecordNativeFontCommandFallback(
					NativeFontCommandFallback::Topology);
				return false;
			}

			draw.vanillaLikeBitmapRoute =
				payload.vanillaLikeBitmapPackets;
			NativeRingSubmissionScope ringScope;
			const NativeFontFallbackReason ringFailure =
				BeginNativeFontRingSubmission(
					facade, payload, ringScope.submission);
			if (ringFailure != NativeFontFallbackReason::None)
			{
				EndNativeFontCommandSpanExecution(
					commandSpanIndex, false, false);
				return false;
			}

			NiDX9Renderer* renderer = draw.vanillaLikeBitmapRoute
				? nullptr : NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			const bool isolatePacketConstants =
				!draw.vanillaLikeBitmapRoute;
			const bool batchedConstants = isolatePacketConstants
				&& ConstantOwnershipBatch().FrameActive();
			std::optional<NativePassConstantScope> localConstants;
			std::optional<NativeFacadeShaderBatchScope> shaderBatch;
			if (isolatePacketConstants)
			{
				shaderBatch.emplace();
				if (!device)
				draw.runtimeFault = true;
				else if (batchedConstants)
				{
					if (!ConstantOwnershipBatch().EnsureOwned(device))
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation =
							ConstantOwnershipBatch().Operation();
						draw.result = ConstantOwnershipBatch().Result();
						draw.mismatchRegister =
							ConstantOwnershipBatch().MismatchRegister();
					}
				}
				else
				{
					localConstants.emplace(device);
					if (!localConstants->Owned())
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation =
							localConstants->Operation();
						draw.result = localConstants->Result();
					}
				}
			}

			NativeTilePacketScope packetScope(pass);
			UInt32 runCursor = 0;
			while (!draw.runtimeFault
				&& runCursor < view.span->runCount)
			{
				UInt32 firstOffset = 0;
				UInt32 endOffset = 0;
				if (!ResolveNextBridgeGroup(
					view, runCursor, firstOffset, endOffset))
				{
					draw.runtimeFault = true;
					draw.failure =
						NativeFontFallbackReason::PacketBuild;
					draw.operation = "retained-run-topology";
					draw.result = E_FAIL;
					break;
				}
				const NativeFontDrawCommand* first =
					ResolveNativeCommand(view, firstOffset);
				NiTriShape* proxy = nullptr;
				if (!first || first->packetIndex != firstOffset)
				{
					draw.runtimeFault = true;
					draw.failure =
						NativeFontFallbackReason::PacketBuild;
					draw.operation = "retained-first-command";
					draw.result = E_FAIL;
					break;
				}
				const NativeFontFallbackReason prepare =
					PrepareNativeFontRingPacket(facade, payload,
						ringScope.submission,
						first->packetIndex, proxy);
				if (prepare != NativeFontFallbackReason::None
					|| !proxy)
				{
					draw.runtimeFault = true;
					draw.failure =
						prepare != NativeFontFallbackReason::None
							? prepare
							: NativeFontFallbackReason::RuntimeFault;
					draw.operation = "retained-first-packet";
					draw.result = E_FAIL;
					break;
				}

				packetScope.Select(proxy);
				NativeImmediateHookVtableScope hookVtable(proxy);
				if (!hookVtable.Active())
				{
					draw.runtimeFault = true;
					draw.failure =
						NativeFontFallbackReason::RuntimeFault;
					draw.operation = "retained-immediate-vtable";
					draw.result = E_FAIL;
					break;
				}

				NativeBridgeExecutionContext bridge;
				bridge.view = view;
				bridge.facade = facade;
				bridge.payload = &payload;
				bridge.ringSubmission = &ringScope.submission;
				bridge.bootstrapCommandOffset = firstOffset;
				bridge.nextCommandOffset = firstOffset + 1u;
				bridge.endCommandOffset = endOffset;
				bridge.boundProfile = first->program
					? first->program->profile : nullptr;
				bridge.bindState = MakeNativeCommandBindState(
					pass, currentPass, false, true,
					setupRenderStates);
				// PrepareNativeFontRingPacket just established the exact retained
				// packet binding on this private proxy. The immediate callback
				// only needs the mutation-epoch guard; rereading the full proxy
				// descriptor would duplicate the packet preparation proof.
				bridge.packetStatePrevalidated = true;
				const bool hasContinuation =
					bridge.nextCommandOffset
						< bridge.endCommandOffset;
				NativeDirectImmediateScope immediateScope(
					proxy, commandSpanIndex, firstOffset, true,
					hasContinuation ? &bridge : nullptr,
					hasContinuation
						? &ContinueRetainedBridge : nullptr,
					NativeImmediateCommandKind::SpanPacket, true);
				InvokeNativeCommandBootstrap(pass, currentPass,
					false, true, setupRenderStates, proxy, first,
					false, true);
				if (immediateScope.Drew())
				{
					draw.drewPacket = true;
					++draw.drawnPacketCount;
					RecordRetainedPacketDraw(*first, false);
				}
				if (bridge.drewPackets)
				{
					draw.drewPacket = true;
					draw.drawnPacketCount += bridge.drewPackets;
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							CommandVanillaBootstrapSaved,
						bridge.drewPackets);
				}
				if (!immediateScope.Invoked()
					|| !immediateScope.ValidationPassed()
					|| !immediateScope.Drew())
				{
					draw.runtimeFault = true;
					draw.failure =
						NativeFontFallbackReason::RuntimeFault;
					draw.operation =
						"retained-bootstrap-immediate";
					draw.result = E_FAIL;
					break;
				}
				if (!immediateScope.ContinuationSucceeded()
					|| bridge.failed)
				{
					draw.runtimeFault = true;
					draw.failure = bridge.failure;
					draw.operation = bridge.operation;
					draw.result = bridge.result;
					draw.constantStateFault =
						bridge.constantStateFault;
					break;
				}
			}

			if (isolatePacketConstants && !batchedConstants
				&& localConstants
				&& !localConstants->Release())
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = localConstants->Operation();
				draw.result = localConstants->Result();
				draw.mismatchRegister =
					localConstants->MismatchRegister();
			}
			if (isolatePacketConstants && batchedConstants
				&& draw.runtimeFault
				&& !ReleaseNativeConstantOwnershipBatch(
					"retained-command-runtime-fault"))
			{
				draw.constantStateFault = true;
				draw.operation = ConstantOwnershipBatch().Operation();
				draw.result = ConstantOwnershipBatch().Result();
				draw.mismatchRegister =
					ConstantOwnershipBatch().MismatchRegister();
			}

			const bool success = !draw.runtimeFault
				&& draw.drawnPacketCount == view.span->commandCount;
			EndNativeFontCommandSpanExecution(
				commandSpanIndex, success, draw.drewPacket);
			if (success)
				return true;
			// No command reached the driver: the unmodified current path can
			// safely acquire a fresh proxy and render the complete payload.
			return draw.drewPacket || draw.constantStateFault;
		}

		bool TryDrawSingletonFacadePacket(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool setupRenderStates, NiTriShape* shape,
			const NativeFontShapeMetadata& metadata, UInt64 validationToken,
			NativePacketDrawResult& draw,
			UInt32 directFacadeSinglePacketCommandIndex)
		{
			SingletonFacadeState* singleton =
				GetSingletonFacadeState(metadata);
			if (!pass || !shape || !singleton
				|| validationToken == 0)
			{
				return false;
			}
			const NativeFontShapePayload* payload = &metadata.nativePayload;
			const NativeFontPacketTemplate* packet = nullptr;
			NativeFontDirectFacadePacketBinding binding;
			NiGeometryBufferData* expectedBuffer = nullptr;
			NiVBChip* expectedChip = nullptr;
			TileShader* expectedShader = nullptr;
			UInt32 packetIndex = 0;
			auto captureBinding = [&](const SingletonFacadeBinding& slot)
			{
				if (!payload || !payload->buildComplete
					|| !payload->payloadTemplate)
				{
					return false;
				}
				const std::vector<NativeFontPacketTemplate>& packets =
					GetNativeFontPackets(*payload->payloadTemplate,
						payload->useCompositePackets);
				if (slot.shape != shape || slot.packetIndex >= packets.size()
					|| slot.packetIndex >= payload->packetShaders.size())
				{
					return false;
				}
				packet = &packets[slot.packetIndex];
				packetIndex = slot.packetIndex;
				expectedBuffer = slot.bindingBuffer;
				expectedChip = slot.bindingChip;
				expectedShader = payload->packetShaders[slot.packetIndex];
				binding.vertexBuffer = slot.bindingChip
					? slot.bindingChip->m_pkVB : nullptr;
				binding.indexBuffer = slot.bindingBuffer
					? slot.bindingBuffer->m_pkIB : nullptr;
				binding.declaration = slot.bindingBuffer
					? static_cast<IDirect3DVertexDeclaration9*>(
						slot.bindingBuffer->m_hDeclaration) : nullptr;
				binding.baseVertex = slot.baseVertex;
				binding.vertexCount = slot.vertexCount;
				binding.indexBytes = slot.bindingBuffer
					? slot.bindingBuffer->m_uiIBSize : 0;
				binding.generation = slot.generation;
				binding.resourceSerial = slot.resourceSerial;
				binding.uploadEpoch = slot.uploadEpoch;
				binding.atlasTextureEpoch = slot.atlasTextureEpoch;
				binding.staticResident = slot.staticResident;
				binding.active = slot.bound;
				return true;
			};
			if (singleton->slot.shape != shape
				|| singleton->preparedValidationToken != validationToken
				|| singleton->frameMode.load(std::memory_order_acquire)
					!= SingletonFacadeFrameMode::Direct)
			{
				return false;
			}
			if (!captureBinding(singleton->slot))
				return false;

			NativeFontDirectFacadeSinglePacketCommandView
				directFacadeCommandView;
			const NativeFontDrawCommand* command = nullptr;
			bool commandExecution = false;
			bool commandBegun = false;
			bool directFacadeCommandExecution = false;
			if (IsFreeTypeFontCommandBufferEnabledForCurrentRoute()
				&& directFacadeSinglePacketCommandIndex
					!= kInvalidNativeFontCommandIndex)
			{
				commandBegun =
					BeginNativeFontDirectFacadeSinglePacketCommandExecution(
						directFacadeSinglePacketCommandIndex,
						&metadata, shape, directFacadeCommandView);
				if (commandBegun
					&& directFacadeCommandView.command)
				{
					command =
						directFacadeCommandView.command->draw;
					commandExecution = command
						&& command->payload == payload
						&& command->expectedGeometry == shape
						&& command->packetIndex == packetIndex;
					directFacadeCommandExecution =
						commandExecution;
				}
				if (commandBegun && !commandExecution)
				{
					EndNativeFontDirectFacadeSinglePacketCommandExecution(
						directFacadeSinglePacketCommandIndex,
						false, false);
				}
			}
			draw.directShapeRoute = true;
			draw.vanillaLikeBitmapRoute = payload->vanillaLikeBitmapPackets;
			NiTriShapeData* data = shape->GetModelData();
			const bool bindingDescriptorCurrent =
				data && data->m_pkBuffData == expectedBuffer
				&& shape->GetShader() == expectedShader
				&& expectedBuffer && expectedChip
				&& expectedBuffer->m_hDeclaration == binding.declaration
				&& expectedBuffer->m_pkIB == binding.indexBuffer
				&& expectedBuffer->m_uiBaseVertexIndex
					== binding.baseVertex
				&& expectedBuffer->m_uiVertCount
					== binding.vertexCount
				&& expectedBuffer->m_uiMaxVertCount
					== binding.vertexCount
				&& expectedBuffer->m_uiStreamCount == 1
				&& expectedBuffer->m_puiVertexStride
				&& expectedBuffer->m_puiVertexStride[0]
					== sizeof(NativeFontGpuVertex)
				&& expectedBuffer->m_ppkVBChip
				&& expectedBuffer->m_ppkVBChip[0] == expectedChip
				&& expectedBuffer->m_uiIndexCount
					== packet->vertexCount / 4u * 6u
				&& expectedBuffer->m_uiIBSize == binding.indexBytes
				&& expectedBuffer->m_eType == D3DPT_TRIANGLELIST
				&& expectedBuffer->m_uiTriCount
					== packet->vertexCount / 4u * 2u
				&& expectedBuffer->m_uiMaxTriCount
					== packet->vertexCount / 4u * 2u
				&& expectedBuffer->m_uiNumArrays == 1
				&& expectedChip->m_pkVB == binding.vertexBuffer
				&& expectedChip->m_uiOffset == 0
				&& expectedChip->m_uiSize
					== binding.vertexCount
						* sizeof(NativeFontGpuVertex)
				&& binding.vertexCount == packet->vertexCount
				&& IsNativeFontDirectFacadePacketAtlasCurrent(
					shape, *payload, packetIndex);
			const bool bindingCurrent = bindingDescriptorCurrent
				&& (commandExecution
					? command
						&& SameNativePacketBinding(
							command->binding, binding)
					: validationToken
							== GetNativeFontSortedFrameValidationToken()
						&& IsNativeFontDirectFacadePacketBindingCurrent(
							binding));
			if (!bindingCurrent)
			{
				draw.runtimeFault = true;
				draw.failure = NativeFontFallbackReason::PacketPrepare;
				draw.operation = "singleton-facade-binding";
				draw.result = E_FAIL;
			}

			NiDX9Renderer* renderer = draw.vanillaLikeBitmapRoute
				? nullptr : NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			if (!draw.runtimeFault && !draw.vanillaLikeBitmapRoute && !device)
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = "acquire-pass-constant-ownership";
				draw.result = D3DERR_DEVICELOST;
			}

			const bool isolatePacketConstants =
				!draw.vanillaLikeBitmapRoute;
			const bool batchedConstants = isolatePacketConstants
				&& ConstantOwnershipBatch().FrameActive();
			std::optional<NativePassConstantScope> localConstants;
			std::optional<NativeFacadeShaderBatchScope> shaderBatch;
			if (!draw.runtimeFault)
			{
				if (isolatePacketConstants)
					shaderBatch.emplace();
				if (batchedConstants)
				{
					if (!ConstantOwnershipBatch().EnsureOwned(device))
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation = ConstantOwnershipBatch().Operation();
						draw.result = ConstantOwnershipBatch().Result();
						draw.mismatchRegister =
							ConstantOwnershipBatch().MismatchRegister();
					}
				}
				else if (isolatePacketConstants)
				{
					localConstants.emplace(device);
					if (!localConstants->Owned())
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation = localConstants->Operation();
						draw.result = localConstants->Result();
					}
				}
			}

			if (!draw.runtimeFault)
			{
				SingletonFacadeTileStateScope tileState(
					shape, *payload, *packet);
				if (!tileState.Active())
				{
					draw.runtimeFault = true;
					draw.failure = NativeFontFallbackReason::PropertySync;
					draw.operation = "singleton-facade-tile-state";
					draw.result = E_FAIL;
				}
				else
				{
					bool usedNativeReplay = false;
					const UInt32 validationCommandIndex =
						commandExecution
							? directFacadeSinglePacketCommandIndex
							: kInvalidNativeFontCommandIndex;
					NativeDirectImmediateScope immediateScope(
						shape, validationCommandIndex,
						kInvalidNativeFontCommandIndex,
						commandExecution, nullptr, nullptr,
						NativeImmediateCommandKind::DirectFacadeSinglePacket,
						commandExecution && bindingCurrent);
					if (commandExecution)
					{
						NativeFontSegmentDeviceStateStamp
							segmentDeviceStateStamp;
						const NativeFontSegmentDeviceStateStamp*
							segmentDeviceState = nullptr;
						if (directFacadeCommandExecution
							&& directFacadeCommandView.stamp
							&& directFacadeCommandView.command
							&& BuildSegmentDeviceStateStamp(
								directFacadeCommandView.stamp,
								directFacadeCommandView.command->
									executionSegmentEpoch,
								directFacadeCommandView.command->
									executionExternalMutationEpoch,
								segmentDeviceStateStamp))
						{
							segmentDeviceState =
								&segmentDeviceStateStamp;
						}
						usedNativeReplay =
							InvokeNativeCommandBootstrap(pass,
							currentPass, false, true,
							setupRenderStates, shape, command,
							directFacadeCommandExecution,
							bindingCurrent,
							expectedBuffer,
							segmentDeviceState);
					}
					else
					{
						InvalidateSegmentDeviceStateCache();
						State().predecessorRenderPassImmediately(pass,
							currentPass, false, true,
							setupRenderStates);
					}
					if (immediateScope.Drew())
					{
						draw.drewPacket = true;
						draw.drawnPacketCount = 1;
						singleton->directDrawCount.fetch_add(
							1, std::memory_order_acq_rel);
						RecordFreeTypePerf(
							FreeTypePerfCounter::TilePass);
						if (usedNativeReplay
							&& directFacadeCommandExecution)
						{
							RecordFreeTypePerf(
								FreeTypePerfCounter::
									CommandDirectFacadeSinglePacketReplay);
						}
						if (packet->shaderClass
							== NativeFontShaderClass::Composite)
						{
							RecordFreeTypePerf(
								FreeTypePerfCounter::CompositeDraw);
						}
					}
					else
					{
						draw.runtimeFault = true;
						draw.failure =
							NativeFontFallbackReason::RuntimeFault;
						draw.operation = immediateScope.Invoked()
							? "singleton-facade-command-validation"
							: "singleton-facade-immediate-not-invoked";
						draw.result = E_FAIL;
					}
				}
				if (!IsNativeFontShaderGenerationCurrent(
					payload->preparedGeneration))
				{
					draw.runtimeFault = true;
					draw.failure = NativeFontFallbackReason::DeviceReset;
					draw.operation =
						"generation-changed-after-singleton-facade";
					draw.result = D3DERR_DEVICELOST;
				}
			}

			if (isolatePacketConstants && !batchedConstants
				&& localConstants
				&& !localConstants->Release())
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = localConstants->Operation();
				draw.result = localConstants->Result();
				draw.mismatchRegister =
					localConstants->MismatchRegister();
			}
			if (isolatePacketConstants && batchedConstants
				&& draw.runtimeFault
				&& !ReleaseNativeConstantOwnershipBatch(
					"singleton-facade-runtime-fault"))
			{
				draw.constantStateFault = true;
				draw.operation = ConstantOwnershipBatch().Operation();
				draw.result = ConstantOwnershipBatch().Result();
				draw.mismatchRegister =
					ConstantOwnershipBatch().MismatchRegister();
			}
			if (directFacadeCommandExecution)
			{
				EndNativeFontDirectFacadeSinglePacketCommandExecution(
					directFacadeSinglePacketCommandIndex,
					!draw.runtimeFault && draw.drewPacket,
					draw.drewPacket);
			}
			if (draw.runtimeFault)
			{
				const bool facadeFallbackSafe =
					!draw.drewPacket && !draw.constantStateFault
					&& singleton->directDrawCount.load(
						std::memory_order_acquire) == 0;
				if (facadeFallbackSafe)
				{
					RestoreSingletonFacade(metadata, draw.failure);
				}
				else
				{
					if (singleton->frameMode.load(
							std::memory_order_acquire)
						!= SingletonFacadeFrameMode::Retired)
					{
						singleton->frameMode.store(
							SingletonFacadeFrameMode::Fault,
							std::memory_order_release);
					}
					RecordFreeTypePerf(FreeTypePerfCounter::
						SingletonFacadeFallback);
				}
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::SingletonFacadeDirectFrame);
			if (draw.runtimeFault && draw.drewPacket)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::SingletonFacadePartialFault);
			}
			return true;
		}

	}

}

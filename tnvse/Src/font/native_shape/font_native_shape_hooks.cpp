#include "font_native_shape_hooks_detail.h"
#include "font_native_shape_standard_lite_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_native_shape_hooks {}
	using namespace implementation::font_native_shape_hooks;

	RenderPassImmediatelyFn ReadRenderPassImmediatelyCallTarget()
	{
		SIZE_T callTarget = 0;
		if (!hook_identity::ReadRel32Target(
			kRenderPassImmediatelyCallSite,
			hook_identity::Rel32Opcode::Call,
			callTarget))
		{
			return nullptr;
		}
		return reinterpret_cast<RenderPassImmediatelyFn>(callTarget);
	}

	bool IsNativeFontRenderPassImmediatelyHookCurrent()
	{
		const RenderPassImmediatelyFn adapterTarget =
			&NativeFontRenderPassImmediately;
		const RenderPassImmediatelyFn predecessorTarget =
			State().predecessorRenderPassImmediately;
		return predecessorTarget && predecessorTarget != adapterTarget
			&& hook_identity::IsExecutableTarget(
				reinterpret_cast<SIZE_T>(predecessorTarget))
			&& ReadRenderPassImmediatelyCallTarget() == adapterTarget;
	}

	bool IsNativeFontRenderPassImmediatelyHookCurrentFast()
	{
		const bool routeCurrent =
			IsNativeFontRenderPassImmediatelyHookCurrentUnchecked();
		if (!routeCurrent)
		{
			RecordFreeTypePerf(FreeTypePerfCounter::
				StructuralReadinessImmediateMismatch);
		}
		return routeCurrent;
	}

	bool IsNativeFontRenderPassImmediatelyHookCurrentUnchecked()
	{
		return State().predecessorRenderPassImmediately
			&& State().predecessorRenderPassImmediately
				!= &NativeFontRenderPassImmediately
			&& hook_identity::MatchesRel32InstructionImageUnchecked(
				kRenderPassImmediatelyCallSite,
				s_renderPassImmediatelyHookImage);
	}

	void __cdecl NativeFontRenderPassImmediately(BSShaderProperty::RenderPass* pass,
		UInt32 currentPass, bool testAlpha, bool blendAlpha, bool setupRenderStates)
	{
		NativeFontShapeState& state = State();
		if (!state.predecessorRenderPassImmediately)
			return;
		NiTriShape* shape = pass
			? reinterpret_cast<NiTriShape*>(pass->pGeometry) : nullptr;
		const bool vanillaLayoutShape = IsVanillaLayoutShape(shape);
		const bool nativeFontShape = IsNativeFontAtlasShape(shape);
		if (!nativeFontShape)
		{
			BreakVisibilityCameraRun();
			RecordFreeTypeGpuEnvelopeForeignPass();
			if (ConstantOwnershipBatch().FrameActive())
				ReleaseNativeConstantOwnershipBatch(
					"before-foreign-render-pass");
			SegmentDeviceStateCache().Reset();
			// Official B994F0/B98E80 and beta RenderPassImmediately dispatch
			// arbitrary attached-shader pass and geometry callbacks. None of those
			// callbacks owns tNVSE's c176-c183/c208/c209 contract, so an unrelated
			// pass is a hard private-constant boundary even when renderer and
			// viewport identity remain unchanged.
			InvalidateNativeFontSortedShaderStateForForeignRenderPass();
			state.predecessorRenderPassImmediately(pass, currentPass, testAlpha,
				blendAlpha, setupRenderStates);
			// Clear any state rebuilt by a nested callback without advancing the
			// command boundary twice. The next native submission must republish all
			// private pixel and Vanilla-layout vertex constants.
			SegmentDeviceStateCache().Reset();
			InvalidateNativeFontSortedShaderStateWithinExecutionSegment();
			return;
		}
		if (vanillaLayoutShape)
			RecordFreeTypeGpuEnvelopeVanillaPass();

		FreeTypePerfScope dispatchRoutePerf(
			FreeTypePerfPhase::DispatchRoute,
			ShouldSampleNativeFontDispatchRoute());
		NativeFontSortedFrameEntryView frameEntry;
		const bool sortedFrameHit =
			FindNativeFontSortedFrameEntry(shape, frameEntry);
		const NativeFontVisibilityPreflight* frameVisibility =
			sortedFrameHit ? frameEntry.visibility : nullptr;
		if (frameVisibility
			&& (frameVisibility->cull
					== NativeFontVisibilityCull::Clip
				|| frameVisibility->cull
					== NativeFontVisibilityCull::Scissor))
		{
			// The sorted-frame witness is compared directly with the live volatile
			// inputs before it suppresses the dispatch. Honoring
			// must precede the singleton-facade direct-draw path so culled
			// singleton texts never arm a packet draw. Any drift revokes the frame
			// proof and falls open to the ordinary draw path.
			const bool reuseCertifiedCamera =
				CanReuseVisibilityCameraRun(frameEntry, *frameVisibility);
			if (!reuseCertifiedCamera)
				BreakVisibilityCameraRun();
			if (HonorNativeFontPreflightClipCull(shape,
					*frameVisibility, reuseCertifiedCamera))
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					VisibilityPreflightClipHonored);
				if (frameEntry.payload)
				{
					RecordNativeFontVisibilityCull(
						frameVisibility->cull, *frameEntry.payload);
				}
				else
				{
					RecordNativeFontVisibilityCull(
						frameVisibility->cull);
				}
				if (vanillaLayoutShape)
				{
					RecordGpuEnvelopeVanillaCull(
						frameVisibility->cull);
				}
				// Retail B64FD6-B64FEC performs only index/pointer updates before
				// the next B994F0 call. Publish continuity last so no engine or
				// plugin callback can occur after certification in this hook.
				ContinueVisibilityCameraRun(frameEntry, *frameVisibility);
				return;
			}
			RecordFreeTypePerf(FreeTypePerfCounter::
				VisibilityPreflightClipRevoked);
		}
		BreakVisibilityCameraRun();
		if (frameVisibility
			&& frameVisibility->cull
				== NativeFontVisibilityCull::ZeroAlpha
			&& EvaluateNativeFontSubmissionVisibility(shape)
				== NativeFontVisibilityCull::ZeroAlpha)
		{
			if (frameEntry.payload)
			{
				RecordNativeFontVisibilityCull(
					NativeFontVisibilityCull::ZeroAlpha,
					*frameEntry.payload);
			}
			else
			{
				RecordNativeFontVisibilityCull(
					NativeFontVisibilityCull::ZeroAlpha);
			}
			if (vanillaLayoutShape)
			{
				RecordGpuEnvelopeVanillaCull(
					NativeFontVisibilityCull::ZeroAlpha);
			}
			return;
		}
		if (vanillaLayoutShape)
		{
			NativeFontVisibilityCull visibilityCull =
				EvaluateNativeFontSubmissionVisibility(shape);
			const bool reuseOverlap = frameVisibility
				&& ReuseNativeFontPreflightClipOverlap(
					*frameVisibility);
			if (visibilityCull == NativeFontVisibilityCull::None
				&& !reuseOverlap)
			{
				visibilityCull = EvaluateNativeFontPreflightClipVisibility(
					shape).cull;
			}
			if (visibilityCull != NativeFontVisibilityCull::None)
			{
				RecordNativeFontVisibilityCull(visibilityCull);
				RecordGpuEnvelopeVanillaCull(visibilityCull);
				RecordFreeTypePerf(
					FreeTypePerfCounter::VanillaLayoutDispatchLocalCull);
				return;
			}
		}
		NativeFontShapeMetadataPtr metadataOwner;
		const NativeFontShapeMetadata* metadata = nullptr;
		NativeFontShapePayload* payload = nullptr;
		bool metadataPayloadCompatibilityFallback = false;
		if (sortedFrameHit && frameEntry.metadata)
		{
			metadata = frameEntry.metadata;
			payload = frameEntry.payload;
		}
		else
		{
			metadataOwner = FindNativeFontShapeMetadata(shape);
			metadata = metadataOwner.get();
			if (metadata && metadata->nativePayload.buildComplete)
				payload = &metadata->nativePayload;
		}
		if (!metadata)
		{
			LogMissingMetadata(shape, "tile-render-pass");
			return;
		}
		if (metadata->backend == FreeTypeShapeBackend::VanillaLayout)
		{
			bool shiftedVanillaLayout = false;
			if (metadata->nativePayload.buildComplete
				&& metadata->nativePayload.payloadTemplate
				&& metadata->nativePayload.payloadTemplate->compositePackets.size()
					== 1)
			{
				const NativeFontPacketTemplate& packet =
					metadata->nativePayload.payloadTemplate->
						compositePackets.front();
				const NativeFontVanillaLayoutKind layoutKind =
					GetVanillaLayoutKind(*metadata);
				shiftedVanillaLayout = packet.compositeShiftedShadow;
				TileShader* shader = ResolveNativeFontPacketShader(
					packet, shape, false, layoutKind);
				if (shader && shape->GetShader() != shader)
					shape->SetShader(shader);
				NativeFontVanillaLayoutDrawToken* drawToken =
					GetVanillaLayoutDrawToken(*metadata);
				bool drawTokenHit = false;
				if (shader && drawToken && EnsureNativeFontVanillaLayoutShapeReady(
					shape, shader, metadata->nativePayload, *drawToken,
					drawTokenHit))
				{
					if (ConstantOwnershipBatch().FrameActive())
					{
						ReleaseNativeConstantOwnershipBatch(
							"before-vanilla-layout");
					}
					SegmentDeviceStateCache().Reset();
					UInt64 vanillaLayoutTransition = 0;
					bool vanillaLayoutTransitionActive = false;
					bool vanillaLayoutDrawn = false;
					bool vanillaLayoutStandardLiteDrawn = false;
					{
						NativeFacadeShaderBatchScope shaderBatch;
						if (drawTokenHit)
						{
							vanillaLayoutTransition =
								BeginNativeFontVanillaLayoutShaderTransition(
									shader, currentPass);
							vanillaLayoutTransitionActive = true;
							vanillaLayoutStandardLiteDrawn =
								TryDrawVanillaLayoutStandardPassLite(
									pass, currentPass, testAlpha,
									blendAlpha, setupRenderStates,
									shape, shader,
									metadata->nativePayload,
									*metadata->nativePayload.payloadTemplate,
									packet, *drawToken);
							vanillaLayoutDrawn =
								vanillaLayoutStandardLiteDrawn;
						}
						if (!vanillaLayoutDrawn)
						{
							VanillaLayoutOriginalVtableScope vanillaVtable(shape);
							if (vanillaVtable.Active())
							{
								if (!vanillaLayoutTransitionActive)
								{
									vanillaLayoutTransition =
										BeginNativeFontVanillaLayoutShaderTransition(
											shader, currentPass);
									vanillaLayoutTransitionActive = true;
								}
								state.predecessorRenderPassImmediately(pass,
									currentPass, testAlpha, blendAlpha,
									setupRenderStates);
								vanillaLayoutDrawn = true;
							}
						}
						if (vanillaLayoutTransitionActive)
						{
							EndNativeFontVanillaLayoutShaderTransition(
								vanillaLayoutTransition, shader);
						}
						if (vanillaLayoutDrawn)
						{
							RecordFreeTypePerf(
								FreeTypePerfCounter::VanillaLayoutDraw);
							RecordFreeTypeGpuEnvelopeVanillaDraw(
								vanillaLayoutStandardLiteDrawn);
							if (shiftedVanillaLayout)
							{
								RecordFreeTypePerf(FreeTypePerfCounter::
									VanillaLayoutShiftedDraw);
							}
						}
					}
					if (vanillaLayoutDrawn)
					{
						return;
					}
					InvalidateNativeFontSortedShaderState();
				}
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutRuntimeFallback);
			if (shiftedVanillaLayout)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutShiftedRuntimeFallback);
			}

			// The sorted-frame entry deliberately keeps payload null so a
			// Vanilla-layout shape cannot enter the 52-byte ring or command
			// builders.  If its engine-layout draw cannot be made ready, restore
			// only this local payload view for the established compatibility
			// fallback below.
			if (!payload && metadata->nativePayload.buildComplete)
			{
				payload = &metadata->nativePayload;
				metadataPayloadCompatibilityFallback = true;
			}
		}

		if (metadata->backend
			== FreeTypeShapeBackend::SingletonFacade)
		{
			SingletonFacadeState* singleton =
				GetSingletonFacadeState(*metadata);
			if (!singleton || singleton->slot.shape != shape)
			{
				RecordNativeFontSuppression(shape, *metadata,
					NativeFontFallbackReason::PacketBuild,
					"singleton-facade-singleton-tile");
				return;
			}
			const SingletonFacadeFrameMode mode =
				singleton->frameMode.load(std::memory_order_acquire);
			if (mode == SingletonFacadeFrameMode::Culled
				|| mode == SingletonFacadeFrameMode::Fault
				|| mode == SingletonFacadeFrameMode::Retired)
			{
				return;
			}

			const UInt64 validationToken =
				GetNativeFontSortedFrameValidationToken();
			if (mode == SingletonFacadeFrameMode::Direct)
			{
				NativePacketDrawResult draw;
				const UInt64 commandToken =
					singleton->commandValidationToken.load(
						std::memory_order_acquire);
				const UInt32 directFacadeSinglePacketCommandIndex =
					singleton->commandDirectFacadeSinglePacketIndex.load(
						std::memory_order_acquire);
				const bool commandCurrent = g_bEnableFreeTypeFontCommandBuffer
					&& commandToken && commandToken == validationToken
					&& directFacadeSinglePacketCommandIndex
						!= kInvalidNativeFontCommandIndex;
				bool handled = TryDrawSingletonFacadePacket(pass, currentPass,
					setupRenderStates, shape, *metadata, validationToken, draw,
					commandCurrent ? directFacadeSinglePacketCommandIndex
						: kInvalidNativeFontCommandIndex);
				if (!handled)
				{
					if (validationToken
						&& singleton->directDrawCount.load(
							std::memory_order_acquire) == 0)
					{
						RestoreSingletonFacade(*metadata,
							NativeFontFallbackReason::PacketPrepare);
					}
					else
					{
						singleton->frameMode.store(
							SingletonFacadeFrameMode::Fault,
							std::memory_order_release);
						RecordFreeTypePerf(FreeTypePerfCounter::
							SingletonFacadeFallback);
					}
					if (singleton->frameMode.load(std::memory_order_acquire)
						!= SingletonFacadeFrameMode::Facade)
					{
						return;
					}
				}
				else if (!draw.runtimeFault)
				{
					return;
				}
				else
				{
					InvalidateNativeFontSortedShaderState();
					NativeFontShapePayload* singletonPayload =
						&metadata->nativePayload;
					if (draw.constantStateFault)
					{
						MarkNativeFontGenerationFault(
							singletonPayload->preparedGeneration,
							draw.operation, draw.result);
						gLog.FormattedMessage(
							"tnvse_freetype_native: singleton-facade singleton pass-constant ownership fault operation=%s hr=0x%08X register=%d shape=%p font=%u generation=%u drewPacket=%u action=suppress-shape",
							draw.operation,
							static_cast<UInt32>(draw.result),
							draw.mismatchRegister, shape,
							metadata->fontId,
							singletonPayload->preparedGeneration,
							draw.drewPacket ? 1 : 0);
					}
					if (draw.drewPacket)
					{
						MarkNativeFontRuntimeFault(*metadata,
							*singletonPayload, draw.failure);
					}
					if (draw.drewPacket || draw.constantStateFault
						|| singleton->frameMode.load(
							std::memory_order_acquire)
							!= SingletonFacadeFrameMode::Facade)
					{
						return;
					}
				}
			}
			payload = &metadata->nativePayload;
		}
		if (payload)
		{
			const bool needsVisibilityCheck = !frameVisibility
				|| frameVisibility->cull
					!= NativeFontVisibilityCull::None;
			if (needsVisibilityCheck)
			{
				const NativeFontVisibilityCull visibilityCull =
					EvaluateNativeFontSubmissionVisibility(shape, *payload);
				if (visibilityCull != NativeFontVisibilityCull::None)
				{
					RecordNativeFontVisibilityCull(
						visibilityCull, *payload);
					return;
				}
			}
		}
		NativeFontFallbackReason failure = NativeFontFallbackReason::None;
		if (!payload)
			failure = NativeFontFallbackReason::PacketBuild;
		else if (payload->suppressNextSubmit.exchange(false,
			std::memory_order_acq_rel))
		{
			failure = payload->stickyReason.exchange(
				NativeFontFallbackReason::None, std::memory_order_acq_rel);
			if (failure == NativeFontFallbackReason::None)
				failure = NativeFontFallbackReason::RuntimeFault;
		}
		else if (sortedFrameHit
			&& !metadataPayloadCompatibilityFallback
			&& frameEntry.payload == payload
			&& frameEntry.preflightResult == NativeFontFallbackReason::None
			&& frameEntry.validationToken
			&& frameEntry.generation == payload->preparedGeneration
			&& payload->preflightAtlasTextureEpoch
				== GetNativeFontAtlasTextureEpoch()
			)
		{
			// NativeFontRenderAlphaGeometry retained the metadata owner and validated
			// this exact published payload immediately before the vanilla sorted Tile
			// traversal. A Vanilla-layout compatibility payload is intentionally not
			// published, so it must use the ordinary preparation path below.
			failure = NativeFontFallbackReason::None;
		}
		else
			failure = PrepareNativeFontFacade(shape, *metadata, *payload);

		if (failure == NativeFontFallbackReason::None)
		{
			NativeFontShapePayload* const sourcePayload = payload;
			NativePacketDrawResult draw;
			const UInt32 commandSpanIndex =
				sortedFrameHit && !metadataPayloadCompatibilityFallback
					? frameEntry.commandSpanIndex
					: kInvalidNativeFontCommandIndex;
			const UInt32 singlePacketCommandIndex =
				sortedFrameHit && !metadataPayloadCompatibilityFallback
					? frameEntry.singlePacketCommandIndex
					: kInvalidNativeFontCommandIndex;
			bool commandHandled = false;
			if (g_bEnableFreeTypeFontCommandBuffer
				&& commandSpanIndex
					!= kInvalidNativeFontCommandIndex)
			{
				NativeFontCommandSpanView commandView;
				if (FindNativeFontCommandSpan(commandSpanIndex,
						frameEntry.validationToken, commandView)
					&& commandView.span
					&& commandView.span->commandCount > 1)
				{
					commandHandled = TryDrawNativeRetainedSpan(
						pass, currentPass, setupRenderStates,
						shape, *sourcePayload,
						commandSpanIndex, draw);
				}
			}
			if (commandHandled && metadata->backend
				== FreeTypeShapeBackend::SingletonFacade)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::SingletonFacadeSpanFrame);
			}
			if (!commandHandled)
			{
				draw = {};
				const bool directShapeHandled = sortedFrameHit
					&& !metadataPayloadCompatibilityFallback
					&& TryDrawNativeSinglePacketDirect(
						pass, currentPass, setupRenderStates,
						shape, *sourcePayload, draw,
						commandSpanIndex,
						singlePacketCommandIndex);
				if (!directShapeHandled)
				{
					if (metadata->backend
						== FreeTypeShapeBackend::SingletonFacade)
					{
						RecordFreeTypePerf(FreeTypePerfCounter::
							SingletonFacadePacketLoopFrame);
					}
					draw = DrawNativePacketSet(pass, currentPass,
						setupRenderStates, shape, *sourcePayload,
						kInvalidNativeFontCommandIndex);
				}
			}
			if (!draw.runtimeFault)
			{
				if (g_bEnableFreeTypeFontRenderingLog
					&& !state.loggedRenderPassImmediatelyHit)
				{
					state.loggedRenderPassImmediatelyHit = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: native Tile facade route hit shape=%p font=%u pass=%u packets=%u ranges=%u route=%s",
						shape, metadata->fontId, currentPass,
						static_cast<UInt32>(
							sourcePayload->packetShaders.size()),
						sourcePayload->payloadTemplate
							? sourcePayload->payloadTemplate->sourceRangeCount
							: 0u,
						draw.directShapeRoute
							? "direct-single-packet-shape"
							: (draw.vanillaLikeBitmapRoute
								? "vanilla-like-bitmap-pages"
								: "effect-packets"));
				}
				return;
			}
			InvalidateNativeFontSortedShaderState();
			if (draw.constantStateFault)
			{
				MarkNativeFontGenerationFault(
					sourcePayload->preparedGeneration,
					draw.operation, draw.result);
				gLog.FormattedMessage(
					"tnvse_freetype_native: pass-constant ownership fault operation=%s hr=0x%08X register=%d shape=%p font=%u generation=%u drewPacket=%u action=suppress-native-facade",
					draw.operation, static_cast<UInt32>(draw.result),
					draw.mismatchRegister, shape, metadata->fontId,
					sourcePayload->preparedGeneration,
					draw.drewPacket ? 1 : 0);
			}
			if (draw.drewPacket)
			{
				if (metadata->backend
					== FreeTypeShapeBackend::SingletonFacade)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						SingletonFacadePartialFault);
				}
				MarkNativeFontRuntimeFault(*metadata, *sourcePayload,
					draw.failure);
				return;
			}
			failure = draw.failure;
		}

		RecordNativeFontSuppression(shape, *metadata, failure, "tile-render-pass");
	}

	bool HookRenderPassImmediately()
	{
		RenderPassImmediatelyFn currentTarget =
			ReadRenderPassImmediatelyCallTarget();
		const RenderPassImmediatelyFn adapterTarget =
			&NativeFontRenderPassImmediately;
		if (currentTarget == adapterTarget)
		{
			const RenderPassImmediatelyFn predecessorTarget =
				State().predecessorRenderPassImmediately;
			State().renderPassImmediatelyHookInstalled = predecessorTarget
				&& predecessorTarget != adapterTarget
				&& hook_identity::IsExecutableTarget(
					reinterpret_cast<SIZE_T>(predecessorTarget));
			if (!State().renderPassImmediatelyHookInstalled)
				InvalidateAllSingletonFacadeBindings();
			return State().renderPassImmediatelyHookInstalled;
		}
		if (!currentTarget)
		{
			if (State().renderPassImmediatelyHookInstalled)
			{
				State().renderPassImmediatelyHookInstalled = false;
				InvalidateAllSingletonFacadeBindings();
			}
			if (!State().loggedRenderPassImmediatelyHookConflict)
			{
				State().loggedRenderPassImmediatelyHookConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: BSShaderAccumulator::RenderAlphaGeometry -> BSBatchRenderer::RenderPassImmediately call site is not CALL rel32; native route unavailable");
			}
			return false;
		}
		if (State().renderPassImmediatelyHookInstalled)
		{
			State().renderPassImmediatelyHookInstalled = false;
			InvalidateAllSingletonFacadeBindings();
			if (!State().loggedRenderPassImmediatelyHookConflict)
			{
				State().loggedRenderPassImmediatelyHookConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: RenderPassImmediately native route was replaced; marked facades will be suppressed");
			}
			return false;
		}
		if (reinterpret_cast<UInt32>(currentTarget)
			!= kBSBatchRendererRenderPassImmediately)
		{
			if (!State().loggedRenderPassImmediatelyHookConflict)
			{
				State().loggedRenderPassImmediatelyHookConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: RenderPassImmediately call site already has a non-vanilla target=%p; leaving it untouched",
					currentTarget);
			}
			return false;
		}

		State().predecessorRenderPassImmediately = currentTarget;
		// BSBatchRenderer immediate pass call (__cdecl).
		WriteRelCall(kRenderPassImmediatelyCallSite,
			&NativeFontRenderPassImmediately);
		const RenderPassImmediatelyFn observedTarget =
			ReadRenderPassImmediatelyCallTarget();
		if (observedTarget == adapterTarget)
		{
			State().renderPassImmediatelyHookInstalled = true;
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: installed RenderPassImmediately native route original=%p vanilla=1",
					currentTarget);
			}
			return true;
		}

		State().renderPassImmediatelyHookInstalled = false;
		if (observedTarget == currentTarget)
		{
			State().predecessorRenderPassImmediately = nullptr;
			gLog.FormattedMessage(
				"tnvse_freetype_native: RenderPassImmediately hook write did not publish; vanilla target remains=%p",
				currentTarget);
			return false;
		}

		// Do not replace an observed later owner with vanilla. It may already
		// chain through this hook, so retain the predecessor target for any call
		// that still reaches tNVSE while the strict top-level route fails closed.
		State().loggedRenderPassImmediatelyHookConflict = true;
		gLog.FormattedMessage(
			"tnvse_freetype_native: RenderPassImmediately hook retained below observed target=%p predecessor=%p; native route marked unavailable",
			observedTarget,
			currentTarget);
		return false;
	}

	bool IsNativeFontAtlasShape(const NiTriShape* shape)
	{
		if (!shape)
			return false;
		void* const* vtable =
			*reinterpret_cast<void* const* const*>(shape);
		return vtable == &State().triShapeVtable[1]
			|| vtable == &State().vanillaLayoutTriShapeVtable[1];
	}

	bool IsVanillaLayoutShape(const NiTriShape* shape)
	{
		return shape && *reinterpret_cast<void* const* const*>(shape)
			== &State().vanillaLayoutTriShapeVtable[1];
	}

	void __fastcall NativeFontRenderImmediate(NiTriShape* shape, void*,
		NiRenderer* renderer)
	{
		if (DirectImmediateContext()
			&& DirectImmediateContext()->shape == shape
			&& State().originalRenderImmediate)
		{
			NativeDirectImmediateContext& context =
				*DirectImmediateContext();
			context.invoked = true;
			if (context.strictValidation
				&& context.commandSpanIndex
					!= kInvalidNativeFontCommandIndex)
			{
				context.validationPassed =
					ValidateNativeImmediateCommand(
						context, shape, renderer);
			}
			if (!context.validationPassed
				&& context.strictValidation)
			{
				return;
			}
			State().originalRenderImmediate(shape, renderer);
			context.drew = true;
			if (!context.strictValidation
				&& context.commandSpanIndex
					!= kInvalidNativeFontCommandIndex)
			{
				context.validationPassed =
					ValidateNativeImmediateCommand(
						context, shape, renderer);
			}
			if (context.continueImmediate)
			{
				context.continuationSucceeded =
					context.continueImmediate(
						context.continuation, renderer, false);
			}
			return;
		}
		SuppressImmediateRoute(shape, "shape-immediate");
	}

	void __fastcall NativeFontRenderImmediateAlt(NiTriShape* shape, void*,
		NiRenderer* renderer)
	{
		if (DirectImmediateContext()
			&& DirectImmediateContext()->shape == shape
			&& State().originalRenderImmediateAlt)
		{
			NativeDirectImmediateContext& context =
				*DirectImmediateContext();
			context.invoked = true;
			if (context.strictValidation
				&& context.commandSpanIndex
					!= kInvalidNativeFontCommandIndex)
			{
				context.validationPassed =
					ValidateNativeImmediateCommand(
						context, shape, renderer);
			}
			if (!context.validationPassed
				&& context.strictValidation)
			{
				return;
			}
			if (context.directDrawLite)
			{
				ExecuteNativeDirectDrawLite(*context.directDrawLite);
			}
			else
			{
				State().originalRenderImmediateAlt(shape, renderer);
			}
			context.drew = true;
			if (!context.strictValidation
				&& context.commandSpanIndex
					!= kInvalidNativeFontCommandIndex)
			{
				context.validationPassed =
					ValidateNativeImmediateCommand(
						context, shape, renderer);
			}
			if (context.continueImmediate)
			{
				context.continuationSucceeded =
					context.continueImmediate(
						context.continuation, renderer, true);
			}
			return;
		}
		SuppressImmediateRoute(shape, "shape-immediate-alt");
	}

	bool InitializeNativeFontTriShapeVtable(NiTriShape* shape)
	{
		void** source = shape ? *reinterpret_cast<void***>(shape) : nullptr;
		if (!source)
			return false;
		if (source == &State().triShapeVtable[1]
			|| source == &State().vanillaLayoutTriShapeVtable[1])
			return true;
		if (State().originalTriShapeVtable)
			return source == State().originalTriShapeVtable;

		const SIZE_T sourceAddress = reinterpret_cast<SIZE_T>(source);
		constexpr SIZE_T kCopiedVtableBytes =
			(kCopiedTriShapeVtableEntries + 1u) * sizeof(void*);
		if (sourceAddress < sizeof(void*)
			|| !hook_identity::IsAccessibleRegion(
				sourceAddress - sizeof(void*), kCopiedVtableBytes, false))
		{
			return false;
		}

		const RenderImmediateFn originalRenderImmediate =
			reinterpret_cast<RenderImmediateFn>(
				source[kRenderImmediateSlot]);
		const RenderImmediateFn originalRenderImmediateAlt =
			reinterpret_cast<RenderImmediateFn>(
				source[kRenderImmediateAltSlot]);
		const DeleteThisFn originalDeleteThis =
			reinterpret_cast<DeleteThisFn>(source[kDeleteThisSlot]);
		const SIZE_T originalRenderImmediateAddress =
			reinterpret_cast<SIZE_T>(originalRenderImmediate);
		const SIZE_T originalRenderImmediateAltAddress =
			reinterpret_cast<SIZE_T>(originalRenderImmediateAlt);
		const SIZE_T originalDeleteThisAddress =
			reinterpret_cast<SIZE_T>(originalDeleteThis);
		if (!originalRenderImmediate || !originalRenderImmediateAlt
			|| !originalDeleteThis
			|| originalRenderImmediateAddress
				== reinterpret_cast<SIZE_T>(&NativeFontRenderImmediate)
			|| originalRenderImmediateAltAddress
				== reinterpret_cast<SIZE_T>(&NativeFontRenderImmediateAlt)
			|| originalDeleteThisAddress
				== reinterpret_cast<SIZE_T>(&NativeFontDeleteThis)
			|| !hook_identity::IsExecutableTarget(
				originalRenderImmediateAddress)
			|| !hook_identity::IsExecutableTarget(
				originalRenderImmediateAltAddress)
			|| !hook_identity::IsExecutableTarget(
				originalDeleteThisAddress))
		{
			return false;
		}

		NativeFontShapeState& state = State();
		const void* expectedFalsePredicate =
			reinterpret_cast<void*>(kNiObjectNullGeometryCastPredicate);
		const bool predicateSlotsMatch =
			source[kGeometrySpecialPredicateSlot]
				== expectedFalsePredicate
			&& source[kGeometryAlternatePredicateSlot]
				== expectedFalsePredicate;
		// Verify the reverse-engineered identity and behavior once while the
		// object still owns the vanilla vtable. The lite hot path then needs only
		// the tNVSE vtable identity plus this immutable result.
		state.standardPassLitePredicatesValidated =
			predicateSlotsMatch
			&& !CallGeometryPredicate(
				shape, kGeometrySpecialPredicateSlot)
			&& !CallGeometryPredicate(
				shape, kGeometryAlternatePredicateSlot);
		if (g_bEnableFreeTypeFontRenderingLog)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_native: RenderPassImmediately_Standard-lite predicate-envelope validated=%u special=%p alternate=%p expected=%p segmentState=independent-effective-v11 slot34=vendor-atoc-independent standardV2=classified-slot-delta slot31=constants-key+native-lite-transient+translation-lite-c0-c3+mismatch-first-field+world-mask slot35=need-only",
				state.standardPassLitePredicatesValidated ? 1u : 0u,
				source[kGeometrySpecialPredicateSlot],
				source[kGeometryAlternatePredicateSlot],
				expectedFalsePredicate);
		}

		std::array<void*, kCopiedTriShapeVtableEntries + 1>
			triShapeVtable = {};
		std::array<void*, kCopiedTriShapeVtableEntries + 1>
			vanillaLayoutTriShapeVtable = {};
		triShapeVtable[0] = source[-1];
		std::copy(source, source + kCopiedTriShapeVtableEntries,
			triShapeVtable.begin() + 1);
		vanillaLayoutTriShapeVtable = triShapeVtable;
		if (g_bEnableFreeTypeFontRenderingLog)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_native: direct-draw-lite immediate proof ready=%u original=%p expected=%p",
				originalRenderImmediateAlt
					== reinterpret_cast<RenderImmediateFn>(
						kNiTriShapeOnlyRenderImmediate) ? 1u : 0u,
				originalRenderImmediateAlt,
				reinterpret_cast<void*>(kNiTriShapeOnlyRenderImmediate));
		}
		triShapeVtable[kDeleteThisSlot + 1]
			= reinterpret_cast<void*>(&NativeFontDeleteThis);
		triShapeVtable[kRenderImmediateSlot + 1]
			= reinterpret_cast<void*>(&NativeFontRenderImmediate);
		triShapeVtable[kRenderImmediateAltSlot + 1]
			= reinterpret_cast<void*>(&NativeFontRenderImmediateAlt);
		vanillaLayoutTriShapeVtable[kDeleteThisSlot + 1]
			= reinterpret_cast<void*>(&NativeFontDeleteThis);
		vanillaLayoutTriShapeVtable[kRenderImmediateSlot + 1]
			= reinterpret_cast<void*>(&NativeFontRenderImmediate);
		vanillaLayoutTriShapeVtable[kRenderImmediateAltSlot + 1]
			= reinterpret_cast<void*>(&NativeFontRenderImmediateAlt);

		state.triShapeVtable = triShapeVtable;
		state.vanillaLayoutTriShapeVtable = vanillaLayoutTriShapeVtable;
		state.originalRenderImmediate = originalRenderImmediate;
		state.originalRenderImmediateAlt = originalRenderImmediateAlt;
		state.originalDeleteThis = originalDeleteThis;
		state.originalTriShapeVtable = source;
		return true;
	}
}

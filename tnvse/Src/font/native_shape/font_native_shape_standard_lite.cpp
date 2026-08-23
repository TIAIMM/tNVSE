#include "font_native_shape_hooks_detail.h"
#include "font_native_shape_standard_lite_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_native_shape_hooks {}
	using namespace implementation::font_native_shape_hooks;

	namespace implementation::font_native_shape_hooks
	{
		const NativeFontDrawCommand* ResolveNativeCommand(
			const NativeFontCommandSpanView& view, UInt32 commandOffset)
		{
			if (!view.span || !view.commands
				|| commandOffset >= view.span->commandCount)
			{
				return nullptr;
			}
			return &view.commands[
				view.span->firstCommand + commandOffset];
		}

		bool IsDefaultNativeReplayPass(UInt32 pass)
		{
			switch (pass)
			{
			case 0xCA:
			case 0xD1:
			case 0xFC:
			case 0xFD:
			case 0x102:
				return false;
			default:
				return true;
			}
		}

		NativeFontCommandBindState MakeNativeCommandBindState(
			const BSShaderProperty::RenderPass* pass,
			UInt32 currentPass, bool testAlpha,
			bool blendAlpha, bool setupRenderStates)
		{
			NativeFontCommandBindState state;
			state.firstPass = pass && pass->bIsFirst;
			state.applyBlend = state.firstPass && blendAlpha
				&& !CdeclCall<bool>(
					kBSBatchRendererPassSuppressesBlendAlpha, currentPass);
			state.applyAlphaTest = state.firstPass && testAlpha
				&& (currentPass < 4 || currentPass > 5)
				&& (currentPass < 0xE || currentPass > 0xF)
				&& currentPass != 570;
			state.applyRenderStates = setupRenderStates;
			return state;
		}

		bool CallGeometryPredicate(NiGeometry* geometry, UInt32 slot)
		{
			void** vtable = geometry
				? *reinterpret_cast<void***>(geometry) : nullptr;
			if (!vtable || !vtable[slot])
				return true;
			using PredicateFn = bool(__thiscall*)(NiGeometry*);
			return reinterpret_cast<PredicateFn>(vtable[slot])(geometry);
		}

		bool RendererUsesSpecialPass(NiGeometry* geometry)
		{
			void* rendererState =
				*reinterpret_cast<void**>(kBSShaderManager_pRenderer);
			if (!rendererState || !geometry)
				return true;
			// Retail B994F0 loads dword_11F9508 into ECX before calling
			// E72C20(rendererState, geometry, 0). Calling it as __cdecl leaves
			// ECX undefined and can classify every Tile as a multi-pass shape.
			using PredicateFn =
				bool(__thiscall*)(void*, NiGeometry*, UInt32);
			return reinterpret_cast<PredicateFn>(
				kNiDX9RendererIsHardwareSkinned)(
					rendererState, geometry, 0);
		}

		bool CanUseNativeReplayBase(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			NiTriShape* geometry, const NativeFontDrawCommand& command,
			bool packetStatePrevalidated)
		{
			if (!IsFreeTypeFontCommandBufferEnabledForCurrentRoute()
				|| !pass || !geometry || !command.program
				|| !command.program->active
				|| pass->pGeometry != geometry
				|| pass->usPassEnum != currentPass
				|| currentPass == kForcedShaderSelectionPass
				|| !IsDefaultNativeReplayPass(currentPass)
				|| geometry->GetSkinInstance()
				|| pass->ucNumLights || pass->ppSceneLights)
			{
				return false;
			}
			if (!packetStatePrevalidated)
			{
				BSShader* shader = geometry->GetShader();
				if (!shader || shader != command.program->shader
					|| !shader->IsTileShader())
				{
					return false;
				}
			}
			return true;
		}

		bool CanUseGuardedNativeReplay(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			NiTriShape* geometry, const NativeFontDrawCommand& command,
			bool packetStatePrevalidated)
		{
			if (!CanUseNativeReplayBase(
					pass, currentPass, geometry, command,
					packetStatePrevalidated))
			{
				return false;
			}
			// These are the three mutually exclusive branches immediately before
			// Retail BSBatchRenderer::RenderPassImmediately reaches
			// BSBatchRenderer::RenderPassImmediately_Standard. E72C20 is a renderer-state
			// __thiscall, while the other two are NiGeometry virtual calls.
			// Treat an absent vtable predicate as unsafe instead of guessing.
			if (RendererUsesSpecialPass(geometry)
				|| CallGeometryPredicate(
					geometry, kGeometrySpecialPredicateSlot)
				|| CallGeometryPredicate(
					geometry, kGeometryAlternatePredicateSlot))
			{
				return false;
			}
			return true;
		}

		bool CanUseStandardPassLiteEnvelope(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			NiTriShape* geometry, const NativeFontDrawCommand& command,
			bool packetStatePrevalidated,
			const NativeFontStandardPassLiteDispatch*& retainedDispatch)
		{
			retainedDispatch = nullptr;
			const NativeFontCompiledPacketCommand* program =
				command.program;
			const NativeFontStandardPassLiteDispatch* dispatch =
				command.standardPassLite;
			const bool dispatchCurrent =
				dispatch && program
				&& (packetStatePrevalidated
					? dispatch->ready
						&& dispatch->geometry == geometry
						&& dispatch->program == program
					: IsNativeFontStandardPassLiteDispatchCurrent(
						*dispatch, geometry, program,
						program->generation));
			if (!dispatchCurrent)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						StandardPassLiteRetainedMiss);
				return false;
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::StandardPassLiteRetainedHit);
			retainedDispatch = dispatch;

			// The retained dispatch has already proved the Tile-owned vtable,
			// null skin, model data, renderer/device, shader vtable, and complete
			// slot table. Only RenderPass fields are live per traversal.
			return IsFreeTypeFontCommandBufferEnabledForCurrentRoute()
				&& pass && geometry
				&& pass->pGeometry == geometry
				&& pass->usPassEnum == currentPass
				&& currentPass != kForcedShaderSelectionPass
				&& IsDefaultNativeReplayPass(currentPass)
				&& !pass->ucNumLights
				&& !pass->ppSceneLights
				&& (packetStatePrevalidated
					|| (program->active
						&& geometry->GetShader() == dispatch->shader
						&& dispatch->shader->IsTileShader()));
		}

		bool ShouldEnableNativeFontVendorAlphaToCoverage(
			NiTriShape* geometry)
		{
			if (!geometry)
				return false;
			const NiAlphaProperty* alpha = geometry->GetAlphaProperty();
			if (!alpha || !alpha->HasAlphaTest())
				return false;
			const BSShaderProperty* shade =
				geometry->GetShadeProperty<BSShaderProperty>();
			return (!shade || !shade->HasNoTMSAA())
				&& !geometry->IsParticlesGeom();
		}

		bool PrepareGuardedNativeReplay(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			NiTriShape* geometry, BSShader* validatedShader)
		{
			if (!pass || !geometry)
				return false;
			BSShader* shader = validatedShader
				? validatedShader : geometry->GetShader();
			if (!shader
				|| (!validatedShader && !shader->IsTileShader()))
				return false;

			// The hook replaces the B994F0 call at B64FD1, so none of B994F0's
			// prelude has executed yet. Mirror the retail order before entering
			// the confirmed RenderPassImmediately_Standard branch.
			*reinterpret_cast<BSShaderProperty::RenderPass**>(
				kBSShaderManager_pCurrentRenderPass) = pass;
			*reinterpret_cast<UInt32*>(kBSShaderManager_eCurrentPass) =
				currentPass;

			const bool selectShader =
				*reinterpret_cast<UInt32*>(
					kBSBatchRenderer_uiLastPass) != currentPass
				|| *reinterpret_cast<BSShader**>(
					kBSBatchRenderer_pLastShader) != shader;
			if (selectShader)
			{
				// B99390 first tears down the previously selected shader and
				// invokes the new shader's pass callbacks. Those callbacks are
				// outside the four Standard-lite slots and may republish any of
				// their device-state categories. Do not carry a cache head
				// across that transition, even if the command stamp itself is
				// otherwise unchanged.
				InvalidateSegmentDeviceStateCache();
				CdeclCall<void>(kBSBatchRendererBeginPass,
					currentPass, shader);
				if (*reinterpret_cast<UInt32*>(
						kBSBatchRenderer_uiLastPass) != currentPass
					|| *reinterpret_cast<BSShader**>(
						kBSBatchRenderer_pLastShader) != shader)
				{
					return false;
				}
			}
			// B994F0 performs post-pass restoration only for shader types 1-5
			// and 17. Guarded replay accepts only TileShader (type 20), so the
			// retail default branch has no corresponding restoration call.

			if (*reinterpret_cast<UInt8*>(
					kBSShaderManager_bTransparencyMultisampling))
			{
				// B98540 is the PC vendor alpha-to-coverage publisher, not an
				// alpha-test-state owner. Publish the final Tile rule locally so
				// this private replay does not depend on a replacement inside the
				// skipped B994F0 wrapper. The formal build writes only render
				// state 154 with A2M0/A2M1 or state 181 with 0/ATOC. Slot 33
				// owns states 24/25, while slot 34 owns cull plus state 15, so
				// this mandatory per-pass call cannot invalidate either cached
				// output. Keep executing it to preserve the vendor extension's
				// nesting semantics, but retain all four Standard-lite proofs.
				CdeclCall<void>(kBSRenderStateSetAlphaToCoverageEnable,
					ShouldEnableNativeFontVendorAlphaToCoverage(geometry),
					false);
			}
			return true;
		}


		void RecordStandardPassLiteFallback(StandardPassLiteFallback fallback)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::StandardPassLiteVanillaFallback);
			switch (fallback)
			{
			case StandardPassLiteFallback::Envelope:
				RecordFreeTypePerf(
					FreeTypePerfCounter::StandardPassLiteFallbackEnvelope);
				break;
			case StandardPassLiteFallback::Program:
				RecordFreeTypePerf(
					FreeTypePerfCounter::StandardPassLiteFallbackProgram);
				break;
			case StandardPassLiteFallback::Renderer:
				RecordFreeTypePerf(
					FreeTypePerfCounter::StandardPassLiteFallbackRenderer);
				break;
			case StandardPassLiteFallback::Geometry:
				RecordFreeTypePerf(
					FreeTypePerfCounter::StandardPassLiteFallbackGeometry);
				break;
			case StandardPassLiteFallback::Binding:
				RecordFreeTypePerf(
					FreeTypePerfCounter::StandardPassLiteFallbackBinding);
				break;
			case StandardPassLiteFallback::Prelude:
				RecordFreeTypePerf(
					FreeTypePerfCounter::StandardPassLiteFallbackPrelude);
				break;
			default:
				break;
			}
		}

		StandardPassLiteFallback ResolveStandardPassLiteResidency(
			const NativeFontDrawCommand& command,
			NiGeometryBufferData* preparedBuffer,
			bool packetStatePrevalidated)
		{
			if (!preparedBuffer)
				return StandardPassLiteFallback::Geometry;

			if (!packetStatePrevalidated)
			{
				NiVBChip* chip = preparedBuffer->m_uiStreamCount
						&& preparedBuffer->m_ppkVBChip
					? preparedBuffer->m_ppkVBChip[0] : nullptr;
				if (!chip || !command.binding.active
					|| preparedBuffer->m_hDeclaration
							!= command.binding.declaration
					|| preparedBuffer->m_pkIB
							!= command.binding.indexBuffer
					|| chip->m_pkVB
							!= command.binding.vertexBuffer
					|| preparedBuffer->m_uiBaseVertexIndex
							!= command.binding.baseVertex
					|| preparedBuffer->m_uiVertCount
							!= command.binding.vertexCount)
				{
					return StandardPassLiteFallback::Binding;
				}
			}
			return StandardPassLiteFallback::None;
		}

		void ExecuteStandardPassLite(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool testAlpha, bool blendAlpha, bool setupRenderStates,
			NiTriShape* geometry,
			const NativeFontStandardPassLiteDispatch& dispatch,
			const NativeFontDrawCommand* command,
			const void* atlasTexture,
			NiGeometryBufferData* preparedBuffer,
			const NativeFontSegmentDeviceStateStamp*
				deviceStateStamp,
			const NativeDirectDrawLiteSubmission*
				certifiedDirectDraw = nullptr)
		{
			NiDX9Renderer* renderer = dispatch.renderer;
			TileShader* shader = dispatch.shader;
			const NiPropertyState* properties = dispatch.properties;
			const NativeFontCompiledPacketCommand& program =
				*dispatch.program;
			// InvokeGuardedNativeReplay admits only a completely classified
			// callback table. Unknown injected callbacks return to vanilla B994F0
			// before its prelude or any draw has executed.
			RecordFreeTypePerf(
				FreeTypePerfCounter::StandardPassV2Replay);
			// Retail RenderPassImmediately_Standard first publishes the current
			// property/effect state. Its geometry-group helper is deliberately
			// absent here: formal E88DC0 and the symbolized test build both prove
			// that the exact (skin=null) call has no side effects when
			// modelData->m_pkBuffData is already resident, which stage 2 proved.
			renderer->m_pkCurrProp =
				const_cast<NiPropertyState*>(properties);
			renderer->m_pkCurrEffects = nullptr;

			NativeSegmentDeviceStateCache* deviceState =
				EnterSegmentDeviceStateCache(deviceStateStamp);

			using SetupStateFn = void(__thiscall*)(
				TileShader*, const NiPropertyState*);
			using SetupGeometryRenderStatesFn = void(__thiscall*)(
				TileShader*, const NiPropertyState*, bool);
			using PrepareGeometryFn = NiGeometryBufferData* (__thiscall*)(
				TileShader*, NiGeometry*, UInt32,
				NiGeometryBufferData*, const NiPropertyState*);

			// The formal build and the symbolized test build agree on these
			// disjoint effects:
			//   slot 30: programs, declaration, texture stages and clamp;
			//   slot 32: blend enable/function;
			//   slot 33: alpha-test function/reference;
			//   slot 34: cull mode and alpha-test enable.
			// Slot 31 changes constants, scissor and stencil state only; slot 35
			// restores scissor/stencil only. Standard v2 retains slot 31 as a fifth
			// independent category for packets that require neither transient
			// state, and omits the verified no-op slot 35 for those packets. The
			// former all-or-nothing aggregate made a page texture or Tile alpha
			// change force every slot to run and therefore produced zero reuse in
			// real traversals.
			const bool firstPass = pass->bIsFirst;
			const bool blendApplicable = firstPass && blendAlpha
				&& !CdeclCall<bool>(
					kBSBatchRendererPassSuppressesBlendAlpha, currentPass);
			const bool alphaTestApplicable = firstPass && testAlpha
				&& (currentPass < 4 || currentPass > 5)
				&& (currentPass < 0xE || currentPass > 0xF)
				&& currentPass != 570;
			const bool renderStatesApplicable = setupRenderStates;

			NativeSegmentPassStateKey passState;
			NativeSegmentBlendStateKey blendState;
			NativeSegmentAlphaTestStateKey alphaState;
			NativeSegmentRenderStatesKey renderStatesKey;
			const bool passKeyReady =
				BuildSegmentPassStateKey(
					geometry, &program, atlasTexture, passState);
			const bool passStateReady = deviceState && passKeyReady
				&& deviceState->passReady
				&& SameSegmentPassState(
					deviceState->pass, passState);
			if (passStateReady)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						SegmentDevicePassReuse);
			}
			else
			{
				reinterpret_cast<SetupStateFn>(
					program.setupGeometryTextures)(shader, properties);
				if (deviceState)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							SegmentDevicePassSet);
				}
			}
			if (deviceState)
			{
				deviceState->passReady = passKeyReady;
				if (passKeyReady)
					deviceState->pass = passState;
			}

			NativeSegmentConstantsStateKey constantsState;
			bool cleanupRequired = true;
			const bool constantsKeyReady =
				dispatch.standardV2Ready
				&& BuildSegmentConstantsStateKey(
					geometry, renderer, &program,
					constantsState, cleanupRequired);
			const NativeSegmentConstantsStateRelation constantsRelation =
				deviceState && constantsKeyReady
					&& deviceState->constantsReady
				? CompareSegmentConstantsState(
					deviceState->constants, constantsState)
				: NativeSegmentConstantsStateRelation::Different;
			const bool constantsStateReady = constantsRelation
				== NativeSegmentConstantsStateRelation::Exact;
			bool constantsLiteApplied = false;
			if (constantsStateReady && cleanupRequired)
			{
				const NativeTileConstantsLiteResult liteResult =
					ApplyNativeTileConstantsLite(geometry, properties);
				constantsLiteApplied = liteResult
					== NativeTileConstantsLiteResult::Applied;
				if (constantsLiteApplied)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::NativeTileConstantsLiteReplay);
				}
				else
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::NativeTileConstantsLiteFallback);
					if (liteResult
						== NativeTileConstantsLiteResult::ScaledScissor)
					{
						RecordFreeTypePerf(FreeTypePerfCounter::
							NativeTileConstantsLiteScaledScissorFallback);
					}
				}
			}
			bool constantsTranslationLiteApplied = false;
			if (constantsRelation
				== NativeSegmentConstantsStateRelation::TranslationOnly)
			{
				const NativeTileConstantsTranslationLiteResult liteResult =
					ApplyNativeTileConstantsTranslationLite(
						geometry, properties, renderer, program.device);
				constantsTranslationLiteApplied = liteResult
					== NativeTileConstantsTranslationLiteResult::Applied
					|| liteResult
						== NativeTileConstantsTranslationLiteResult::
							AppliedTransient;
				if (constantsTranslationLiteApplied)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						NativeTileConstantsTranslationLiteReplay);
					if (liteResult
						== NativeTileConstantsTranslationLiteResult::
							AppliedTransient)
					{
						RecordFreeTypePerf(FreeTypePerfCounter::
							NativeTileConstantsTranslationLiteTransientReplay);
					}
				}
				else
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						NativeTileConstantsTranslationLiteFallback);
					switch (liteResult)
					{
					case NativeTileConstantsTranslationLiteResult::
						NotApplicable:
						RecordFreeTypePerf(FreeTypePerfCounter::
							NativeTileConstantsTranslationLiteNotApplicableFallback);
						break;
					case NativeTileConstantsTranslationLiteResult::
						ScaledScissor:
						RecordFreeTypePerf(FreeTypePerfCounter::
							NativeTileConstantsTranslationLiteScaledScissorFallback);
						break;
					case NativeTileConstantsTranslationLiteResult::NonFinite:
						RecordFreeTypePerf(FreeTypePerfCounter::
							NativeTileConstantsTranslationLiteNonFiniteFallback);
						break;
					case NativeTileConstantsTranslationLiteResult::DeviceFailure:
						RecordFreeTypePerf(FreeTypePerfCounter::
							NativeTileConstantsTranslationLiteDeviceFailure);
						break;
					default:
						break;
					}
				}
			}
			if ((constantsStateReady && (!cleanupRequired
				|| constantsLiteApplied))
				|| constantsTranslationLiteApplied)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						SegmentDeviceConstantsReuse);
			}
			else
			{
				reinterpret_cast<SetupStateFn>(
					program.setupGeometryConstants)(shader, properties);
				if (deviceState)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							SegmentDeviceConstantsSet);
				}
			}
			if (deviceState)
			{
				deviceState->constantsReady =
					constantsKeyReady;
				if (constantsKeyReady)
				deviceState->constants = constantsState;
			}
			if (firstPass)
			{
				if (blendApplicable)
				{
					const bool blendKeyReady =
						BuildSegmentBlendStateKey(
							geometry,
							program.standardBlendSemantics,
							blendState);
					const bool blendStateReady =
						deviceState && blendKeyReady
						&& deviceState->blendReady
						&& SameSegmentBlendState(
							deviceState->blend, blendState);
					if (blendStateReady)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								SegmentDeviceBlendReuse);
					}
					else
					{
						reinterpret_cast<SetupStateFn>(
							program.setupGeometryAlphaBlending)(
								shader, properties);
						if (deviceState)
						{
							RecordFreeTypePerf(
								FreeTypePerfCounter::
									SegmentDeviceBlendSet);
						}
					}
					if (deviceState)
					{
						deviceState->blendReady =
							blendKeyReady;
						if (blendKeyReady)
							deviceState->blend = blendState;
					}
				}
				if (alphaTestApplicable)
				{
					const bool alphaKeyReady =
						BuildSegmentAlphaTestStateKey(
							geometry, alphaState);
					const bool alphaStateReady =
						deviceState && alphaKeyReady
						&& deviceState->alphaTestReady
						&& SameSegmentAlphaTestState(
							deviceState->alphaTest, alphaState);
					if (alphaStateReady)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								SegmentDeviceAlphaTestReuse);
					}
					else
					{
						reinterpret_cast<SetupStateFn>(
							program.setupGeometryAlphaTesting)(
								shader, properties);
						if (deviceState)
						{
							RecordFreeTypePerf(
								FreeTypePerfCounter::
									SegmentDeviceAlphaTestSet);
						}
					}
					if (deviceState)
					{
						deviceState->alphaTestReady =
							alphaKeyReady;
						if (alphaKeyReady)
						{
							deviceState->alphaTest =
								alphaState;
						}
					}
				}
			}
			else if (*reinterpret_cast<UInt8*>(kBSBatchRenderer_bFirstPass)
				&& !CdeclCall<bool>(
					kBSBatchRendererPassSuppressesBlendAlpha, currentPass))
			{
				reinterpret_cast<SetupStateFn>(
					program.setupNonFirstPass)(shader, properties);
				*reinterpret_cast<UInt8*>(kBSBatchRenderer_bFirstPass) = 0;
				if (deviceState)
				{
					// Slot 68 changes blend, Z-write and Z-function state
					// after slot 30. The next pass cannot treat the resulting
					// aggregate as a reusable first-pass setup.
					deviceState->InvalidateStates();
				}
			}
			if (renderStatesApplicable)
			{
				const bool renderStatesKeyReady =
					BuildSegmentRenderStatesKey(
						geometry, currentPass, firstPass,
						renderStatesKey);
				const bool renderStatesStateReady =
					deviceState && renderStatesKeyReady
					&& deviceState->renderStatesReady
					&& SameSegmentRenderStates(
						deviceState->renderStates, renderStatesKey);
				if (renderStatesStateReady)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							SegmentDeviceRenderStatesReuse);
				}
				else
				{
					reinterpret_cast<SetupGeometryRenderStatesFn>(
						program.setupGeometryRenderStates)(
							shader, properties, firstPass);
					if (deviceState)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								SegmentDeviceRenderStatesSet);
					}
				}
				if (deviceState)
				{
					deviceState->renderStatesReady =
						renderStatesKeyReady;
					if (renderStatesKeyReady)
						deviceState->renderStates = renderStatesKey;
				}
			}

			RecordFreeTypePerf(
				FreeTypePerfCounter::NativeDirectDrawLiteCandidate);
			NativeDirectDrawLiteSubmission builtDirectDraw;
			const NativeDirectDrawLiteSubmission* directDrawLite =
				certifiedDirectDraw;
			NativeDirectDrawLiteFallback directDrawFailure =
				NativeDirectDrawLiteFallback::None;
			if (!directDrawLite && command)
			{
				directDrawFailure = BuildNativeDirectDrawLiteSubmission(
					geometry, renderer, properties, program, *command,
					preparedBuffer, deviceState, builtDirectDraw);
				if (directDrawFailure == NativeDirectDrawLiteFallback::None)
					directDrawLite = &builtDirectDraw;
			}
			bool directDrawArmed = false;
			if (directDrawLite)
			{
				NativeDirectDrawLiteArmScope directDrawScope(
					geometry, *directDrawLite);
				directDrawArmed = directDrawScope.Active();
				if (directDrawArmed)
					NativeFontRenderImmediateAlt(geometry, nullptr, renderer);
			}
			if (!directDrawArmed)
			{
				RecordNativeDirectDrawLiteFallback(
					directDrawFailure == NativeDirectDrawLiteFallback::None
						? NativeDirectDrawLiteFallback::Program
						: directDrawFailure);
				if (deviceState)
					deviceState->geometryBindingReady = false;
				reinterpret_cast<PrepareGeometryFn>(
					program.prepareGeometryForRendering)(
						shader, geometry, 0,
						preparedBuffer, properties);
				NativeFontRenderImmediateAlt(geometry, nullptr, renderer);
			}
			const bool verifiedPost =
				(program.standardV2SlotProofs
					& NativeFontCompiledPacketCommand::
						kStandardSlot35Proof) != 0;
			if (!verifiedPost || cleanupRequired)
			{
				reinterpret_cast<SetupStateFn>(
					program.postGeometry)(shader, properties);
				RecordFreeTypePerf(
					FreeTypePerfCounter::SegmentDevicePostSet);
			}
			else
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						SegmentDevicePostElision);
			}
		}


		void RecordVanillaLayoutStandardLiteFallback(
			VanillaLayoutStandardLiteFallback fallback)
		{
			RecordFreeTypePerf(FreeTypePerfCounter::
				VanillaLayoutStandardLiteFallback);
			switch (fallback)
			{
			case VanillaLayoutStandardLiteFallback::Envelope:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutStandardLiteFallbackEnvelope);
				break;
			case VanillaLayoutStandardLiteFallback::Program:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutStandardLiteFallbackProgram);
				break;
			case VanillaLayoutStandardLiteFallback::Renderer:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutStandardLiteFallbackRenderer);
				break;
			case VanillaLayoutStandardLiteFallback::Geometry:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutStandardLiteFallbackGeometry);
				break;
			case VanillaLayoutStandardLiteFallback::Binding:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutStandardLiteFallbackBinding);
				break;
			case VanillaLayoutStandardLiteFallback::Declaration:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutStandardLiteFallbackDeclaration);
				break;
			case VanillaLayoutStandardLiteFallback::Prelude:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutStandardLiteFallbackPrelude);
				break;
			default:
				break;
			}
		}

		bool TryDrawVanillaLayoutStandardPassLite(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool testAlpha, bool blendAlpha, bool setupRenderStates,
			NiTriShape* geometry, TileShader* shader,
			const NativeFontShapePayload& payload,
			const NativeFontPayloadTemplate& artifact,
			const NativeFontPacketTemplate& packet,
			const NativeFontVanillaLayoutDrawToken& drawToken)
		{
			// This route owns no ring residency, frame command, command index, or
			// execution segment. It consumes only the metadata-owned token and the
			// generation-owned callback program, then falls back to the complete
			// predecessor before drawing whenever either proof is incomplete.
			RecordFreeTypePerf(FreeTypePerfCounter::
				VanillaLayoutStandardLiteCandidate);
			NativeFontShapeState& state = State();
			if (!state.standardPassLitePredicatesValidated
				|| state.predecessorRenderPassImmediately
					!= reinterpret_cast<RenderPassImmediatelyFn>(
						kBSBatchRendererRenderPassImmediately)
				|| !pass || !geometry || !shader
				|| !IsVanillaLayoutShape(geometry)
				|| pass->pGeometry != geometry
				|| pass->usPassEnum != currentPass
				|| currentPass == kForcedShaderSelectionPass
				|| !IsDefaultNativeReplayPass(currentPass)
				|| pass->ucNumLights || pass->ppSceneLights
				|| geometry->GetSkinInstance()
				|| geometry->GetShader() != shader
				|| !shader->IsTileShader()
				|| RendererUsesSpecialPass(geometry)
				|| CallGeometryPredicate(
					geometry, kGeometrySpecialPredicateSlot)
				|| CallGeometryPredicate(
					geometry, kGeometryAlternatePredicateSlot))
			{
				RecordVanillaLayoutStandardLiteFallback(
					VanillaLayoutStandardLiteFallback::Envelope);
				return false;
			}

			const NativeFontCompiledPacketCommand* program =
				drawToken.standardLiteProgramIdentity;
			void** shaderVtable = *reinterpret_cast<void***>(shader);
			if (!program || !program->active || !program->profile
				|| program->profile != drawToken.profileIdentity
				|| program->shader != shader
				|| program->shaderVtable != shaderVtable
				|| program->generation != drawToken.generation
				|| !program->directDrawLiteReady
				|| program->standardV2SlotProofs
					!= NativeFontCompiledPacketCommand::
						kStandardV2RequiredProofs
				|| !program->prepareGeometryForRendering
				|| !program->setupGeometryTextures
				|| !program->setupGeometryConstants
				|| !program->setupGeometryAlphaBlending
				|| !program->setupGeometryAlphaTesting
				|| !program->setupGeometryRenderStates
				|| !program->postGeometry
				|| !program->setupNonFirstPass)
			{
				RecordVanillaLayoutStandardLiteFallback(
					VanillaLayoutStandardLiteFallback::Program);
				return false;
			}

			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			if (!renderer || !program->device
				|| program->device != renderer->GetD3DDevice()
				|| shader->m_pkD3DDevice != program->device
				|| shader->m_pkD3DRenderer != renderer
				|| !shader->m_pkD3DRenderState
				|| shader->m_pkD3DRenderState != renderer->m_pkRenderState
				|| !renderer->GetInsideFrameState()
				|| !renderer->m_bRenderTargetGroupActive
				|| renderer->m_bDeviceLost)
			{
				RecordVanillaLayoutStandardLiteFallback(
					VanillaLayoutStandardLiteFallback::Renderer);
				return false;
			}

			DirectTileShaderPropertyView* tile =
				GetDirectTileProperty(geometry);
			NiTexture* expectedTexture = packet.atlasPage
					< artifact.atlasTextures.size()
				? artifact.atlasTextures[packet.atlasPage].m_pObject
				: nullptr;
			NiDX9TextureData* textureData = expectedTexture
				? expectedTexture->GetDX9RendererData() : nullptr;
			const void* atlasTexture = textureData
				? textureData->GetD3DTexture() : nullptr;
			if (!payload.buildComplete
				|| payload.payloadTemplate.get() != &artifact
				|| drawToken.payloadIdentity != &payload
				|| drawToken.artifactIdentity != &artifact
				|| drawToken.packetIdentity != &packet
				|| !HasNativeFontPayloadValidationSeal(artifact)
				|| artifact.compositePackets.size() != 1u
				|| &artifact.compositePackets.front() != &packet
				|| !tile || !expectedTexture || !atlasTexture
				|| tile->sourceTexture.m_pObject != expectedTexture
				|| tile->alphaTexture.m_pObject || tile->noTexture)
			{
				RecordVanillaLayoutStandardLiteFallback(
					VanillaLayoutStandardLiteFallback::Geometry);
				return false;
			}

			NativeDirectDrawLiteSubmission directDraw;
			bool directDrawSucceeded = false;
			UInt64 bindingFailures = 0;
			const NativeDirectDrawLiteFallback directDrawFailure =
				BuildVanillaLayoutDirectDrawLiteSubmission(
					geometry, renderer, *program, packet,
					drawToken, directDraw, bindingFailures);
			if (directDrawFailure != NativeDirectDrawLiteFallback::None)
			{
				if (directDrawFailure == NativeDirectDrawLiteFallback::Binding)
				{
					if (!bindingFailures)
					{
						AddVanillaLayoutBindingFailure(bindingFailures,
							VanillaLayoutBindingFailure::Unclassified);
					}
				}
				VanillaLayoutStandardLiteFallback failure =
					VanillaLayoutStandardLiteFallback::Binding;
				switch (directDrawFailure)
				{
				case NativeDirectDrawLiteFallback::Program:
					failure = VanillaLayoutStandardLiteFallback::Program;
					break;
				case NativeDirectDrawLiteFallback::Renderer:
					failure = VanillaLayoutStandardLiteFallback::Renderer;
					break;
				case NativeDirectDrawLiteFallback::Geometry:
					failure = VanillaLayoutStandardLiteFallback::Geometry;
					break;
				case NativeDirectDrawLiteFallback::Declaration:
					failure = VanillaLayoutStandardLiteFallback::Declaration;
					break;
				default:
					break;
				}
				RecordVanillaLayoutStandardLiteFallback(failure);
				return false;
			}
			directDraw.successfulDrawWitness = &directDrawSucceeded;

			NativeFontStandardPassLiteDispatch dispatch;
			dispatch.geometry = geometry;
			dispatch.properties = &geometry->m_kProperties;
			dispatch.renderer = renderer;
			dispatch.shader = shader;
			dispatch.program = program;
			dispatch.generation = program->generation;
			dispatch.standardV2Ready = true;
			dispatch.ready = true;

			if (!PrepareGuardedNativeReplay(
					pass, currentPass, geometry, shader)
				|| shader->m_uiCurrentPass != 0u)
			{
				RecordVanillaLayoutStandardLiteFallback(
					VanillaLayoutStandardLiteFallback::Prelude);
				return false;
			}
			NativeDirectImmediateScope immediateScope(geometry);
			ExecuteStandardPassLite(pass, currentPass,
				testAlpha, blendAlpha, setupRenderStates,
				geometry, dispatch, nullptr, atlasTexture,
				directDraw.data->m_pkBuffData, nullptr,
				&directDraw);
			if (!immediateScope.Drew() || !directDrawSucceeded)
			{
				RecordVanillaLayoutStandardLiteFallback(
					VanillaLayoutStandardLiteFallback::Binding);
				return false;
			}
			RecordFreeTypePerf(drawToken.priorGenerationDeclaration
				? FreeTypePerfCounter::
					VanillaLayoutStandardLiteCompatibleDeclarationReplay
				: FreeTypePerfCounter::
					VanillaLayoutStandardLiteCurrentDeclarationReplay);
			RecordFreeTypePerf(FreeTypePerfCounter::
				VanillaLayoutStandardLiteReplay);
			return true;
		}

		bool InvokeGuardedNativeReplay(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool testAlpha, bool blendAlpha, bool setupRenderStates,
			NiTriShape* geometry, const NativeFontDrawCommand* command,
			bool preferStandardPassLite,
			bool packetStatePrevalidated,
			NiGeometryBufferData* preparedBuffer,
			const NativeFontSegmentDeviceStateStamp*
				deviceStateStamp)
		{
			if (preferStandardPassLite)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::StandardPassLiteCandidate);
			}

			bool guardedEligible = false;
			bool liteEnvelope = false;
			const NativeFontStandardPassLiteDispatch* liteDispatch = nullptr;
			if (preferStandardPassLite)
			{
				liteEnvelope = command
					&& CanUseStandardPassLiteEnvelope(
						pass, currentPass, geometry, *command,
						packetStatePrevalidated, liteDispatch);
				if (liteEnvelope)
				{
					guardedEligible = true;
					RecordFreeTypePerf(
						FreeTypePerfCounter::StandardPassLiteStage1Eligible);
					if (!liteDispatch
						|| !liteDispatch->standardV2Ready)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								StandardPassV2CompatibilityReplay);
						RecordStandardPassLiteFallback(
							StandardPassLiteFallback::Program);
						return false;
					}
				}
				else
				{
					RecordStandardPassLiteFallback(
						StandardPassLiteFallback::Envelope);
				}
			}
			if (!guardedEligible && command)
			{
				guardedEligible = CanUseGuardedNativeReplay(
					pass, currentPass, geometry, *command,
					packetStatePrevalidated);
			}
			if (!command || !guardedEligible)
			{
				if (IsFreeTypeFontCommandBufferEnabledForCurrentRoute())
					RecordNativeFontCommandFallback(
					NativeFontCommandFallback::State);
				return false;
			}

			bool useStandardPassLite = false;
			if (liteEnvelope)
			{
				const StandardPassLiteFallback liteFailure =
					ResolveStandardPassLiteResidency(
						*command, preparedBuffer,
						packetStatePrevalidated);
				if (liteFailure == StandardPassLiteFallback::None)
				{
					useStandardPassLite = true;
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							StandardPassLiteStage2Resident);
				}
				else
				{
					RecordStandardPassLiteFallback(liteFailure);
				}
			}

			if (!PrepareGuardedNativeReplay(
					pass, currentPass, geometry,
					packetStatePrevalidated && command
						? command->program->shader : nullptr))
			{
				if (useStandardPassLite)
				{
					RecordStandardPassLiteFallback(
						StandardPassLiteFallback::Prelude);
				}
				if (IsFreeTypeFontCommandBufferEnabledForCurrentRoute())
					RecordNativeFontCommandFallback(
						NativeFontCommandFallback::State);
				return false;
			}

			if (useStandardPassLite)
			{
				ExecuteStandardPassLite(pass, currentPass,
					testAlpha, blendAlpha, setupRenderStates,
					geometry, *liteDispatch, command,
					command->atlasTexture,
					preparedBuffer, deviceStateStamp);
				RecordFreeTypePerf(
					FreeTypePerfCounter::StandardPassLiteStage3Replay);
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandNativeReplay);
				return true;
			}

			// The complete vanilla standard path may touch every device-state
			// category. A later dedicated one-packet Tile must establish a new
			// cache head even when it remains in the same validation segment.
			InvalidateSegmentDeviceStateCache();
			using DefaultPassFn = void(__cdecl*)(
				BSShaderProperty::RenderPass*, bool, bool, bool);
			reinterpret_cast<DefaultPassFn>(
				kBSBatchRendererRenderPassImmediatelyStandard)(
				pass, testAlpha, blendAlpha, setupRenderStates);
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandNativeReplay);
			return true;
		}

		bool InvokeNativeCommandBootstrap(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool testAlpha, bool blendAlpha, bool setupRenderStates,
			NiTriShape* geometry, const NativeFontDrawCommand* command,
			bool preferStandardPassLite,
			bool packetStatePrevalidated,
			NiGeometryBufferData* preparedBuffer,
			const NativeFontSegmentDeviceStateStamp*
				deviceStateStamp)
		{
			if (InvokeGuardedNativeReplay(pass, currentPass,
				testAlpha, blendAlpha, setupRenderStates,
				geometry, command, preferStandardPassLite,
				packetStatePrevalidated, preparedBuffer,
				deviceStateStamp))
			{
				return true;
			}
			InvalidateSegmentDeviceStateCache();
			State().predecessorRenderPassImmediately(pass, currentPass,
				testAlpha, blendAlpha, setupRenderStates);
			return false;
		}

		void RecordRetainedPacketDraw(
			const NativeFontDrawCommand& command, bool retainedExtra)
		{
			if (retainedExtra)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandRetainedBridgeDraw);
			}
			RecordFreeTypePerf(FreeTypePerfCounter::TilePass);
			if (command.packet
				&& command.packet->shaderClass
					== NativeFontShaderClass::Composite)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CompositeDraw);
			}
		}

	}

}

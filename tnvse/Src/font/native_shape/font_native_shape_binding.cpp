#include "font_native_shape_hooks_detail.h"
#include "font_native_shape_standard_lite_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_native_shape_hooks {}
	using namespace implementation::font_native_shape_hooks;

	namespace implementation::font_native_shape_hooks
	{
		NativeSegmentDeviceStateCache& SegmentDeviceStateCache()
		{
			thread_local NativeSegmentDeviceStateCache cache;
			return cache;
		}

		VisibilityCameraRunTracker& VisibilityCameraRun()
		{
			thread_local VisibilityCameraRunTracker tracker;
			return tracker;
		}

		UInt32& NativeFontDispatchRouteSampleCursor()
		{
			thread_local UInt32 cursor = 0;
			return cursor;
		}

		bool SameSegmentDeviceStateStamp(
			const NativeFontSegmentDeviceStateStamp& left,
			const NativeFontSegmentDeviceStateStamp& right)
		{
			return left.ready && right.ready
				&& left.renderer == right.renderer
				&& left.device == right.device
				&& left.renderTargetGroup == right.renderTargetGroup
				&& left.validationToken == right.validationToken
				&& left.generation == right.generation
				&& left.atlasTextureEpoch == right.atlasTextureEpoch
				&& left.resourceSerial == right.resourceSerial
				&& left.uploadEpoch == right.uploadEpoch
				&& left.executionSegmentEpoch
					== right.executionSegmentEpoch
				&& left.externalMutationEpoch
					== right.externalMutationEpoch
				&& std::memcmp(&left.viewport, &right.viewport,
					sizeof(left.viewport)) == 0;
		}

		NativeSegmentDeviceStateCache*
			EnterSegmentDeviceStateCache(
				const NativeFontSegmentDeviceStateStamp* stamp)
		{
			NativeSegmentDeviceStateCache& cache =
				SegmentDeviceStateCache();
			if (!stamp || !stamp->ready || !stamp->renderer
				|| !stamp->device
				|| !stamp->renderTargetGroup
				|| !stamp->validationToken || !stamp->generation
				|| !stamp->atlasTextureEpoch
				|| !stamp->resourceSerial
				|| !stamp->executionSegmentEpoch
				|| !stamp->externalMutationEpoch
				|| !stamp->viewport.Width
				|| !stamp->viewport.Height)
			{
				cache.Reset();
				return nullptr;
			}
			if (!cache.stampReady
				|| !SameSegmentDeviceStateStamp(cache.stamp, *stamp))
			{
				cache.Reset();
				cache.stamp = *stamp;
				cache.stampReady = true;
				RecordFreeTypePerf(
					FreeTypePerfCounter::SegmentDeviceStateStart);
			}
			else
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::SegmentDeviceStateReuse);
			}
			return &cache;
		}

		void InvalidateSegmentDeviceStateCache()
		{
			SegmentDeviceStateCache().Reset();
		}

		bool SameSegmentPassState(
			const NativeSegmentPassStateKey& left,
			const NativeSegmentPassStateKey& right)
		{
			return left.program == right.program
				&& left.sourceTexture == right.sourceTexture
				&& left.alphaTexture == right.alphaTexture
				&& left.atlasTexture == right.atlasTexture
				&& left.effectiveClampMode
					== right.effectiveClampMode
				&& left.textureModeFlags
					== right.textureModeFlags;
		}

		bool BuildSegmentPassStateKey(NiTriShape* geometry,
			const NativeFontCompiledPacketCommand* program,
			const void* atlasTexture,
			NativeSegmentPassStateKey& key)
		{
			DirectTileShaderPropertyView* tile =
				GetDirectTileProperty(geometry);
			if (!tile || !program || !atlasTexture
				|| !tile->sourceTexture.m_pObject
				|| tile->alphaTexture.m_pObject
				|| tile->noTexture)
			{
				return false;
			}
			key = {};
			key.program = program;
			key.sourceTexture = tile->sourceTexture.m_pObject;
			key.alphaTexture = tile->alphaTexture.m_pObject;
			key.atlasTexture = atlasTexture;
			// Official TileShader::SetupGeometryTextures (BCA760) reads
			// rotates, hasVertexColors, noTexture, both texture pointers and
			// clampMode. byte90 and every RenderPass flag are irrelevant.
			key.effectiveClampMode = tile->rotates
				? NiTexturingProperty::WRAP_S_WRAP_T
				: tile->clampMode;
			key.textureModeFlags =
				(tile->rotates ? 1u : 0u)
				| (tile->hasVertexColors ? 2u : 0u);
			return true;
		}

		bool BuildSegmentConstantsStateKey(NiTriShape* geometry,
			NiDX9Renderer* renderer,
			const NativeFontCompiledPacketCommand* program,
			NativeSegmentConstantsStateKey& key,
			bool& cleanupRequired)
		{
			key = {};
			cleanupRequired = true;
			DirectTileShaderPropertyView* tile =
				GetDirectTileProperty(geometry);
			if (!geometry || !renderer || !tile || !program)
				return false;

			const NiStencilProperty* stencil =
				geometry->GetStencilProperty();
			cleanupRequired = tile->useScissorTest
				|| (stencil && stencil->IsEnabled());
			const NiMaterialProperty* material =
				geometry->GetMaterialProperty();
			key.program = program;
			key.world = geometry->m_kWorld;
			key.view = renderer->m_kD3DView;
			key.projection = renderer->m_kD3DProj;
			key.viewProjection = renderer->m_kViewProj;
			key.cameraRight = renderer->m_kCamRight;
			key.cameraUp = renderer->m_kCamUp;
			key.overlayColor = tile->overlayColor;
			key.tileAlpha = tile->tileAlpha;
			key.materialAlpha = material ? material->m_fAlpha : 1.0f;
			key.nearDepth = renderer->m_fNearDepth;
			key.depthRange = renderer->m_fDepthRange;
			key.rotates = tile->rotates;
			if (tile->rotates)
				key.textureTransform = tile->textureTransform;
			return true;
		}


		NativeSegmentConstantsStateRelation CompareSegmentConstantsState(
			const NativeSegmentConstantsStateKey& left,
			const NativeSegmentConstantsStateKey& right)
		{
			// Preserve the original short-circuit order and record exactly one
			// diagnostic per failed comparison. This identifies the first field
			// blocking reuse. Translation-only world changes continue through the
			// remaining fields because the light path must prove that no later input
			// changed, but they retain world as the first-mismatch diagnostic.
			if (left.program != right.program)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchProgram);
				return NativeSegmentConstantsStateRelation::Different;
			}
			if (left.rotates != right.rotates)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchRotates);
				return NativeSegmentConstantsStateRelation::Different;
			}
			bool translationOnly = false;
			if (std::memcmp(&left.world, &right.world,
				sizeof(left.world)) != 0)
			{
				// Classify the complete transform with one counter write. The
				// seven buckets retain overlap information while avoiding as many
				// as three relaxed atomic increments for one failed key comparison.
				UInt8 mismatchMask = 0;
				if (std::memcmp(&left.world.m_Rotate,
					&right.world.m_Rotate,
					sizeof(left.world.m_Rotate)) != 0)
				{
					mismatchMask |= 1u;
				}
				if (std::memcmp(&left.world.m_Translate,
					&right.world.m_Translate,
					sizeof(left.world.m_Translate)) != 0)
				{
					mismatchMask |= 2u;
				}
				if (std::memcmp(&left.world.m_fScale,
					&right.world.m_fScale,
					sizeof(left.world.m_fScale)) != 0)
				{
					mismatchMask |= 4u;
				}
				switch (mismatchMask)
				{
				case 1u:
					RecordFreeTypePerf(FreeTypePerfCounter::
						SegmentDeviceConstantsWorldMismatchRotationOnly);
					break;
				case 2u:
					RecordFreeTypePerf(FreeTypePerfCounter::
						SegmentDeviceConstantsWorldMismatchTranslationOnly);
					break;
				case 3u:
					RecordFreeTypePerf(FreeTypePerfCounter::
						SegmentDeviceConstantsWorldMismatchRotationTranslation);
					break;
				case 4u:
					RecordFreeTypePerf(FreeTypePerfCounter::
						SegmentDeviceConstantsWorldMismatchScaleOnly);
					break;
				case 5u:
					RecordFreeTypePerf(FreeTypePerfCounter::
						SegmentDeviceConstantsWorldMismatchRotationScale);
					break;
				case 6u:
					RecordFreeTypePerf(FreeTypePerfCounter::
						SegmentDeviceConstantsWorldMismatchTranslationScale);
					break;
				case 7u:
					RecordFreeTypePerf(FreeTypePerfCounter::
						SegmentDeviceConstantsWorldMismatchRotationTranslationScale);
					break;
				default:
					// NiTransform is currently the exact concatenation of these
					// three fields. Keep a conservative diagnostic bucket if that
					// representation ever changes.
					RecordFreeTypePerf(FreeTypePerfCounter::
						SegmentDeviceConstantsFirstMismatchWorld);
					break;
				}
				if (mismatchMask != 2u)
					return NativeSegmentConstantsStateRelation::Different;
				translationOnly = true;
			}
			const auto rejectLater = [translationOnly](
				FreeTypePerfCounter counter)
			{
				if (!translationOnly)
					RecordFreeTypePerf(counter);
				return NativeSegmentConstantsStateRelation::Different;
			};
			if (std::memcmp(&left.view, &right.view,
				sizeof(left.view)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchView);
			}
			if (std::memcmp(&left.projection, &right.projection,
				sizeof(left.projection)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchProjection);
			}
			if (std::memcmp(&left.viewProjection,
				&right.viewProjection,
				sizeof(left.viewProjection)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchViewProjection);
			}
			if (std::memcmp(&left.cameraRight, &right.cameraRight,
				sizeof(left.cameraRight)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchCameraRight);
			}
			if (std::memcmp(&left.cameraUp, &right.cameraUp,
				sizeof(left.cameraUp)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchCameraUp);
			}
			if (std::memcmp(&left.overlayColor, &right.overlayColor,
				sizeof(left.overlayColor)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchOverlayColor);
			}
			if (left.rotates
				&& std::memcmp(&left.textureTransform,
					&right.textureTransform,
					sizeof(left.textureTransform)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchTextureTransform);
			}
			if (std::memcmp(&left.tileAlpha, &right.tileAlpha,
				sizeof(left.tileAlpha)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchTileAlpha);
			}
			if (std::memcmp(&left.materialAlpha, &right.materialAlpha,
				sizeof(left.materialAlpha)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchMaterialAlpha);
			}
			if (std::memcmp(&left.nearDepth, &right.nearDepth,
				sizeof(left.nearDepth)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchNearDepth);
			}
			if (std::memcmp(&left.depthRange, &right.depthRange,
				sizeof(left.depthRange)) != 0)
			{
				return rejectLater(FreeTypePerfCounter::
					SegmentDeviceConstantsFirstMismatchDepthRange);
			}
			return translationOnly
				? NativeSegmentConstantsStateRelation::TranslationOnly
				: NativeSegmentConstantsStateRelation::Exact;
		}

		bool BuildSegmentBlendStateKey(NiTriShape* geometry,
			NativeFontStandardBlendSemantics semantics,
			NativeSegmentBlendStateKey& key)
		{
			if (!geometry
				|| !HasPredictableNativeFontBlendSemantics(semantics))
			{
				return false;
			}
			if (semantics == NativeFontStandardBlendSemantics::NativeOwned)
			{
				key = ComputeNativeFontOwnedBlendState(
					&geometry->m_kProperties);
				return true;
			}

			DirectTileShaderPropertyView* tile =
				GetDirectTileProperty(geometry);
			if (!tile
				|| semantics != NativeFontStandardBlendSemantics::Retail)
				return false;
			const NiAlphaProperty* alpha =
				geometry->GetAlphaProperty();
			key = {};
			const UInt16 flags = alpha
				? alpha->m_usFlags.Get() : 0;
			const bool propertyBlend = alpha
				&& (flags & NiAlphaProperty::ALPHA_BLEND_MASK) != 0;
			// BE1FF0/BSShader::SetupGeometryAlphaBlending compares both
			// values only against 1.0. Preserve its NaN behavior by negating
			// the pair of >= comparisons instead of using <.
			const bool opacityBlend = !(tile->fAlpha >= 1.0f
				&& tile->fFadeAlpha >= 1.0f);
			key.enabled = propertyBlend || opacityBlend;
			if (propertyBlend)
			{
				key.sourceFunction = static_cast<UInt8>(
					(flags & NiAlphaProperty::SRC_BLEND_MASK)
						>> NiAlphaProperty::SRC_BLEND_POS);
				key.destinationFunction = static_cast<UInt8>(
					(flags & NiAlphaProperty::DEST_BLEND_MASK)
						>> NiAlphaProperty::DEST_BLEND_POS);
			}
			else
			{
				key.sourceFunction = static_cast<UInt8>(
					NiAlphaProperty::ALPHA_SRCALPHA);
				key.destinationFunction = static_cast<UInt8>(
					NiAlphaProperty::ALPHA_INVSRCALPHA);
			}
			return true;
		}

		bool SameSegmentBlendState(
			const NativeSegmentBlendStateKey& left,
			const NativeSegmentBlendStateKey& right)
		{
			return left.enabled == right.enabled
				&& (!left.enabled
					|| (left.sourceFunction == right.sourceFunction
						&& left.destinationFunction
							== right.destinationFunction));
		}

		bool BuildSegmentAlphaTestStateKey(NiTriShape* geometry,
			NativeSegmentAlphaTestStateKey& key)
		{
			const NiAlphaProperty* alpha =
				geometry ? geometry->GetAlphaProperty() : nullptr;
			if (!alpha)
				return false;
			const UInt16 flags = alpha->m_usFlags.Get();
			key.testFunction = static_cast<UInt8>(
				(flags & NiAlphaProperty::TEST_FUNC_MASK)
					>> NiAlphaProperty::TEST_FUNC_POS);
			key.alphaTestRef = alpha->m_ucAlphaTestRef;
			return true;
		}

		bool SameSegmentAlphaTestState(
			const NativeSegmentAlphaTestStateKey& left,
			const NativeSegmentAlphaTestStateKey& right)
		{
			return left.testFunction == right.testFunction
				&& left.alphaTestRef == right.alphaTestRef;
		}

		bool BuildSegmentRenderStatesKey(NiTriShape* geometry,
			UInt32 currentPass, bool firstPass,
			NativeSegmentRenderStatesKey& key)
		{
			if (!geometry)
				return false;
			const NiStencilProperty* stencil =
				geometry->GetStencilProperty();
			const NiAlphaProperty* alpha =
				geometry->GetAlphaProperty();
			key = {};
			// BE20E0/BSShader::SetupGeometryRenderStates publishes only two
			// effective outputs. Other stencil/alpha bits do not participate.
			key.drawBoth = (currentPass >= 86u && currentPass <= 87u)
				|| (stencil
					&& ((stencil->m_usFlags.Get()
							& NiStencilProperty::DRAWMODE_MASK)
						>> NiStencilProperty::DRAWMODE_POS)
						== NiStencilProperty::DRAW_BOTH);
			key.alphaTestEnabled = firstPass
				&& alpha && alpha->HasAlphaTest();
			return true;
		}

		bool SameSegmentRenderStates(
			const NativeSegmentRenderStatesKey& left,
			const NativeSegmentRenderStatesKey& right)
		{
			return left.drawBoth == right.drawBoth
				&& left.alphaTestEnabled
					== right.alphaTestEnabled;
		}

		bool SameSegmentGeometryBinding(
			const NativeSegmentGeometryBindingKey& left,
			const NativeSegmentGeometryBindingKey& right)
		{
			return left.declaration == right.declaration
				&& left.vertexBuffer == right.vertexBuffer
				&& left.indexBuffer == right.indexBuffer
				&& left.streamOffset == right.streamOffset
				&& left.stride == right.stride;
		}


		void BreakVisibilityCameraRun()
		{
			VisibilityCameraRun().ready = false;
		}

		bool CanReuseVisibilityCameraRun(
			const NativeFontSortedFrameEntryView& frameEntry,
			const NativeFontVisibilityPreflight& visibility)
		{
			const VisibilityCameraRunTracker& tracker =
				VisibilityCameraRun();
			return tracker.ready && visibility.frameToken
				&& frameEntry.retailSortedItemMatched
				&& tracker.frameToken == visibility.frameToken
				&& tracker.nestedTraversalSerial
					== frameEntry.nestedTraversalSerial
				&& tracker.previousSortedItem > 0
				&& frameEntry.retailSortedItemIndex
					== tracker.previousSortedItem - 1;
		}

		void ContinueVisibilityCameraRun(
			const NativeFontSortedFrameEntryView& frameEntry,
			const NativeFontVisibilityPreflight& visibility)
		{
			if (!visibility.frameToken
				|| !frameEntry.retailSortedItemMatched
				|| frameEntry.retailSortedItemIndex < 0
				|| !frameEntry.nestedTraversalSerial)
			{
				BreakVisibilityCameraRun();
				return;
			}
			VisibilityCameraRunTracker& tracker = VisibilityCameraRun();
			if (!tracker.ready)
			{
				tracker.frameToken = visibility.frameToken;
				tracker.nestedTraversalSerial =
					frameEntry.nestedTraversalSerial;
				tracker.ready = true;
			}
			tracker.previousSortedItem = frameEntry.retailSortedItemIndex;
		}

		bool ShouldSampleNativeFontDispatchRoute()
		{
			if (!g_bEnableFreeTypeFontRenderingLog)
				return false;
			const UInt32 sample = NativeFontDispatchRouteSampleCursor()++;
			return sample % kNativeFontDispatchRouteCpuSampleRate == 0u;
		}

		bool BuildSegmentDeviceStateStamp(
			const NativeFontFrameStamp* frame,
			UInt32 executionSegmentEpoch,
			UInt32 externalMutationEpoch,
			NativeFontSegmentDeviceStateStamp& stamp)
		{
			stamp = {};
			if (!frame || !frame->renderer || !frame->device
				|| !frame->renderTargetReady
				|| !frame->viewportReady
				|| !frame->renderTargetGroup
				|| !frame->validationToken
				|| !frame->generation
				|| !frame->atlasTextureEpoch
				|| !frame->resourceSerial
				|| !executionSegmentEpoch
				|| !externalMutationEpoch)
			{
				return false;
			}
			stamp.renderer = frame->renderer;
			stamp.device = frame->device;
			stamp.renderTargetGroup = frame->renderTargetGroup;
			stamp.viewport = frame->viewport;
			stamp.validationToken = frame->validationToken;
			stamp.generation = frame->generation;
			stamp.atlasTextureEpoch =
				frame->atlasTextureEpoch;
			stamp.resourceSerial = frame->resourceSerial;
			stamp.uploadEpoch = frame->uploadEpoch;
			stamp.executionSegmentEpoch =
				executionSegmentEpoch;
			stamp.externalMutationEpoch =
				externalMutationEpoch;
			stamp.ready = true;
			return true;
		}

	}

}

#include "font_native_internal.h"

#include "NiAlphaProperty.hpp"
#include "NiDX9Renderer.hpp"
#include "NiDX9RenderState.hpp"
#include "NiMaterialProperty.hpp"
#include "NiPropertyState.hpp"
#include "NiTexturingProperty.hpp"

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace fonthook::vectorfont
{
	namespace implementation::font_native_visibility {}
	using namespace implementation::font_native_visibility;

	namespace implementation::font_native_visibility
	{
		inline constexpr UInt32 kCurrentRenderPass = 0x11F91E0;
		inline constexpr UInt32 kScaledScissorActive = 0x11F9426;
		inline constexpr UInt32 kRendererPositionAdjust = 0x11F474C;
		inline constexpr double kScissorSafetyMarginPixels = 2.0;
		inline constexpr double kClipIntervalRelativeSlack = 1.0e-6;

		thread_local NativeA8LateVisibilityScope* s_lateVisibilityScope = nullptr;

		enum class ClipProofResult : UInt8
		{
			Unproven = 0,
			Overlap,
			Outside
		};

		struct ClipTransformCache
		{
			const NiDX9Renderer* renderer = nullptr;
			D3DXMATRIX world;
			D3DXMATRIX view;
			D3DXMATRIX projection;
			D3DXMATRIX worldViewProjection;
			bool valid = false;
		};

		thread_local ClipTransformCache s_clipTransformCache;

		// Font::MakeTriShape returns a TileShaderProperty, but the concrete class
		// is not exposed by this CommonLib snapshot. Keep this read-only view tied
		// to the same retail ABI already used by native packet state mirroring.
		struct TileVisibilityPropertyView : BSShaderProperty
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

		static_assert(sizeof(TileVisibilityPropertyView) == 0xB0);
		static_assert(offsetof(TileVisibilityPropertyView, overlayColor) == 0x68);
		static_assert(offsetof(TileVisibilityPropertyView, tileAlpha) == 0x78);
		static_assert(offsetof(TileVisibilityPropertyView, scissorRect) == 0x9C);
		static_assert(offsetof(TileVisibilityPropertyView, useScissorTest) == 0xAC);

		const TileVisibilityPropertyView* GetTileProperty(
			const NiTriShape* facade)
		{
			NiShadeProperty* property =
				facade ? facade->GetShadeProperty() : nullptr;
			if (!property
				|| property->m_eShaderType != NiShadeProperty::PROP_Tile)
			{
				return nullptr;
			}
			return static_cast<const TileVisibilityPropertyView*>(
				static_cast<const BSShaderProperty*>(property));
		}

		const TileVisibilityPropertyView* GetTileProperty(
			const NiPropertyState* properties)
		{
			NiShadeProperty* property = properties
				? properties->m_spShadeProperty.m_pObject : nullptr;
			if (!property
				|| property->m_eShaderType != NiShadeProperty::PROP_Tile)
			{
				return nullptr;
			}
			return static_cast<const TileVisibilityPropertyView*>(
				static_cast<const BSShaderProperty*>(property));
		}

		bool IsFiniteMatrix(const D3DXMATRIX& matrix)
		{
			for (UInt32 row = 0; row < 4; ++row)
			{
				for (UInt32 column = 0; column < 4; ++column)
				{
					if (!std::isfinite(matrix.m[row][column]))
						return false;
				}
			}
			return true;
		}

		bool BuildRetailTileWorldMatrix(const NiTransform& transform,
			D3DXMATRIX& world)
		{
			const NiPoint3 positionAdjust =
				*reinterpret_cast<const NiPoint3*>(
					kRendererPositionAdjust);
			if (!std::isfinite(transform.m_fScale)
				|| !std::isfinite(transform.m_Translate.x)
				|| !std::isfinite(transform.m_Translate.y)
				|| !std::isfinite(transform.m_Translate.z)
				|| !std::isfinite(positionAdjust.x)
				|| !std::isfinite(positionAdjust.y)
				|| !std::isfinite(positionAdjust.z))
			{
				return false;
			}

			// D3DXMATRIX's default constructor leaves its float storage untouched.
			// Formal GetD3DFromNi at B71A40 writes these affine entries as exact
			// zero; make that part of the proof explicit so clip/scissor evaluation
			// never consumes stack-dependent values.
			world._14 = 0.0f;
			world._24 = 0.0f;
			world._34 = 0.0f;
			for (UInt32 row = 0; row < 3; ++row)
			{
				for (UInt32 column = 0; column < 3; ++column)
				{
					const float rotation =
						transform.m_Rotate.m_pEntry[column][row];
					if (!std::isfinite(rotation))
						return false;
					// Retail NiD3DUtility::GetD3DFromNi at B71A40
					// transposes NiMatrix3 for D3D's row-vector layout and
					// applies the uniform NiTransform scale while storing.
					world.m[row][column] =
						rotation * transform.m_fScale;
				}
			}
			world.m[3][0] = transform.m_Translate.x - positionAdjust.x;
			world.m[3][1] = transform.m_Translate.y - positionAdjust.y;
			world.m[3][2] = transform.m_Translate.z - positionAdjust.z;
			world.m[3][3] = 1.0f;
			return IsFiniteMatrix(world);
		}

		struct ClipColumn
		{
			double x = 0.0;
			double y = 0.0;
			double z = 0.0;
			double translation = 0.0;
		};

		ClipColumn GetClipColumn(const D3DXMATRIX& matrix, UInt32 column)
		{
			return {
				matrix.m[0][column], matrix.m[1][column],
				matrix.m[2][column], matrix.m[3][column]
			};
		}

		double EvaluateColumn(const ClipColumn& column,
			const NiPoint3& point)
		{
			return column.x * point.x + column.y * point.y
				+ column.z * point.z + column.translation;
		}

		double ColumnEvaluationMagnitude(const ClipColumn& column,
			const NiPoint3& point)
		{
			return std::abs(column.x * point.x)
				+ std::abs(column.y * point.y)
				+ std::abs(column.z * point.z)
				+ std::abs(column.translation);
		}

		double CubeExtent(const ClipColumn& column, double radius)
		{
			return radius * (std::abs(column.x) + std::abs(column.y)
				+ std::abs(column.z));
		}

		ClipColumn SubtractScaled(const ClipColumn& left,
			const ClipColumn& right, double scale)
		{
			return {
				left.x - right.x * scale,
				left.y - right.y * scale,
				left.z - right.z * scale,
				left.translation - right.translation * scale
			};
		}

		bool CubeIsOutsidePlane(const ClipColumn& insidePlane,
			const NiBound& bound)
		{
			const double center = EvaluateColumn(
				insidePlane, bound.m_kCenter);
			const double extent = CubeExtent(
				insidePlane, bound.m_fRadius);
			// Use the pre-cancellation term magnitude. abs(center) alone can be
			// tiny after subtracting large world/viewport terms and would not
			// bound the float shader's dot-product rounding.
			const double slack = (ColumnEvaluationMagnitude(
				insidePlane, bound.m_kCenter) + extent + 1.0)
				* kClipIntervalRelativeSlack;
			return center + extent < -slack;
		}

		bool IsValidScissorForViewport(const RECT& scissor,
			const D3DVIEWPORT9& viewport)
		{
			if (!viewport.Width || !viewport.Height
				|| scissor.left < 0 || scissor.top < 0
				|| scissor.left >= scissor.right
				|| scissor.top >= scissor.bottom)
			{
				return false;
			}
			const std::int64_t viewportLeft = viewport.X;
			const std::int64_t viewportTop = viewport.Y;
			const std::int64_t viewportRight = viewportLeft + viewport.Width;
			const std::int64_t viewportBottom = viewportTop + viewport.Height;
			return scissor.left >= viewportLeft
				&& scissor.top >= viewportTop
				&& scissor.right <= viewportRight
				&& scissor.bottom <= viewportBottom;
		}

		bool BuildViewportRect(const D3DVIEWPORT9& viewport, RECT& rect)
		{
			if (!viewport.Width || !viewport.Height
				|| viewport.X > static_cast<DWORD>(LONG_MAX)
				|| viewport.Y > static_cast<DWORD>(LONG_MAX))
			{
				return false;
			}
			const std::uint64_t right = static_cast<std::uint64_t>(viewport.X)
				+ viewport.Width;
			const std::uint64_t bottom = static_cast<std::uint64_t>(viewport.Y)
				+ viewport.Height;
			if (right > static_cast<std::uint64_t>(LONG_MAX)
				|| bottom > static_cast<std::uint64_t>(LONG_MAX))
			{
				return false;
			}
			rect.left = static_cast<LONG>(viewport.X);
			rect.top = static_cast<LONG>(viewport.Y);
			rect.right = static_cast<LONG>(right);
			rect.bottom = static_cast<LONG>(bottom);
			return true;
		}

		bool IsPrimitiveClippingEnabled(const NiDX9Renderer& renderer)
		{
			const NiDX9RenderState* state = renderer.m_pkRenderState;
			return state
				&& state->m_akRenderStateSettings[D3DRS_CLIPPING].m_uiCurrValue
					!= FALSE;
		}

		bool BuildWorldViewProjection(const NiDX9Renderer& renderer,
			const D3DXMATRIX& world, D3DXMATRIX& worldViewProjection,
			bool recordLateDiagnostics)
		{
			ClipTransformCache& cache = s_clipTransformCache;
			if (cache.valid && cache.renderer == &renderer
				&& std::memcmp(&cache.world, &world, sizeof(world)) == 0
				&& std::memcmp(&cache.view, &renderer.m_kD3DView,
					sizeof(cache.view)) == 0
				&& std::memcmp(&cache.projection, &renderer.m_kD3DProj,
					sizeof(cache.projection)) == 0)
			{
				worldViewProjection = cache.worldViewProjection;
				if (recordLateDiagnostics)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						VisibilityLateClipTransformHit);
				}
				return true;
			}

			if (recordLateDiagnostics)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					VisibilityLateClipTransformMiss);
			}
			if (!IsFiniteMatrix(world)
				|| !IsFiniteMatrix(renderer.m_kD3DView)
				|| !IsFiniteMatrix(renderer.m_kD3DProj))
			{
				return false;
			}
			D3DXMATRIX worldView = {};
			D3DXMatrixMultiply(
				&worldView, &world, &renderer.m_kD3DView);
			D3DXMatrixMultiply(&worldViewProjection,
				&worldView, &renderer.m_kD3DProj);
			if (!IsFiniteMatrix(worldViewProjection))
				return false;

			cache.renderer = &renderer;
			std::memcpy(&cache.world, &world, sizeof(world));
			std::memcpy(&cache.view, &renderer.m_kD3DView,
				sizeof(cache.view));
			std::memcpy(&cache.projection, &renderer.m_kD3DProj,
				sizeof(cache.projection));
			cache.worldViewProjection = worldViewProjection;
			cache.valid = true;
			return true;
		}

		ClipProofResult EvaluatePayloadClip(
			const NativeA8ShapePayload& payload,
			const TileVisibilityPropertyView& tile,
			const NiDX9Renderer& renderer, const D3DXMATRIX& world,
			bool allowViewport, NativeA8VisibilityCull& reason,
			bool recordLateDiagnostics = false,
			const D3DXMATRIX* preparedWorldViewProjection = nullptr)
		{
			reason = NativeA8VisibilityCull::None;
			if (recordLateDiagnostics)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::VisibilityLateClipCheck);
			}
			auto failOpen = [&]()
			{
				if (recordLateDiagnostics)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::VisibilityLateClipFailOpen);
				}
				return ClipProofResult::Unproven;
			};

			RECT clipRect = {};
			const D3DVIEWPORT9& viewport = renderer.m_kD3DPort;
			const bool useScissor = tile.useScissorTest
				&& !*reinterpret_cast<const UInt8*>(kScaledScissorActive)
				&& IsValidScissorForViewport(tile.scissorRect, viewport);
			if (useScissor)
			{
				clipRect = tile.scissorRect;
				reason = NativeA8VisibilityCull::Scissor;
				if (recordLateDiagnostics)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						VisibilityLateClipScissorCheck);
				}
			}
			else
			{
				// D3D9 clips the post-projection x/y volume to [-w,+w].  Only
				// use that larger, state-independent target when the structural
				// fast paths are enabled and Gamebryo's exact render-state mirror
				// proves primitive clipping is currently active.  A scaled or
				// malformed Tile scissor therefore falls back to the full viewport,
				// never to an approximation of the narrower rectangle.
				if (!allowViewport || !IsPrimitiveClippingEnabled(renderer)
					|| !BuildViewportRect(viewport, clipRect))
				{
					return failOpen();
				}
				reason = NativeA8VisibilityCull::Clip;
				if (recordLateDiagnostics)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						VisibilityLateClipViewportCheck);
				}
			}

			if (!payload.payloadTemplate)
				return failOpen();
			const NiBound& bound = payload.payloadTemplate->bound;
			if (!std::isfinite(bound.m_kCenter.x)
				|| !std::isfinite(bound.m_kCenter.y)
				|| !std::isfinite(bound.m_kCenter.z)
				|| !std::isfinite(bound.m_fRadius)
				|| bound.m_fRadius < 0.0f)
			{
				return failOpen();
			}

			// WorldViewProjTranspose is predefined mapping 23 in the retail
			// constant map at E85D10. It calls D3DXMatrixMultiply twice in this
			// exact association order, then transposes for VS c0-c3. Use the same
			// D3DX entry point and retain the non-transposed matrix for row-vector
			// homogeneous half-space evaluation below.
			D3DXMATRIX builtWorldViewProjection = {};
			const D3DXMATRIX* worldViewProjection =
				preparedWorldViewProjection;
			if (worldViewProjection)
			{
				if (!IsFiniteMatrix(*worldViewProjection))
					return failOpen();
			}
			else if (!BuildWorldViewProjection(renderer, world,
					builtWorldViewProjection, recordLateDiagnostics))
			{
				return failOpen();
			}
			else
			{
				worldViewProjection = &builtWorldViewProjection;
			}

			const ClipColumn clipX = GetClipColumn(*worldViewProjection, 0);
			const ClipColumn clipY = GetClipColumn(*worldViewProjection, 1);
			const ClipColumn clipW = GetClipColumn(*worldViewProjection, 3);
			const double radius = bound.m_fRadius;
			const double centerW = EvaluateColumn(clipW, bound.m_kCenter);
			const double extentW = CubeExtent(clipW, radius);
			const double wSlack = (ColumnEvaluationMagnitude(
				clipW, bound.m_kCenter) + extentW + 1.0)
				* kClipIntervalRelativeSlack;
			// Perspective division is monotonic over the complete conservative cube
			// only when every point is strictly in front of the w=0 plane.
			if (centerW - extentW <= wSlack)
				return failOpen();

			const double inverseWidth = 1.0 / viewport.Width;
			const double inverseHeight = 1.0 / viewport.Height;
			const double leftNdc = ((clipRect.left
				- kScissorSafetyMarginPixels - viewport.X)
				* 2.0 * inverseWidth) - 1.0;
			const double rightNdc = ((clipRect.right
				+ kScissorSafetyMarginPixels - viewport.X)
				* 2.0 * inverseWidth) - 1.0;
			const double topNdc = 1.0 - ((clipRect.top
				- kScissorSafetyMarginPixels - viewport.Y)
				* 2.0 * inverseHeight);
			const double bottomNdc = 1.0 - ((clipRect.bottom
				+ kScissorSafetyMarginPixels - viewport.Y)
				* 2.0 * inverseHeight);

			// Each expression is non-negative inside the expanded scissor.  The
			// payload sphere is first expanded to a cube; only a negative maximum
			// for the whole cube proves that every glyph triangle is outside.
			const ClipColumn leftPlane = SubtractScaled(
				clipX, clipW, leftNdc);
			const ClipColumn rightPlane = SubtractScaled(
				ClipColumn{}, SubtractScaled(clipX, clipW, rightNdc), 1.0);
			const ClipColumn topPlane = SubtractScaled(
				ClipColumn{}, SubtractScaled(clipY, clipW, topNdc), 1.0);
			const ClipColumn bottomPlane = SubtractScaled(
				clipY, clipW, bottomNdc);
			const bool outside = CubeIsOutsidePlane(leftPlane, bound)
				|| CubeIsOutsidePlane(rightPlane, bound)
				|| CubeIsOutsidePlane(topPlane, bound)
				|| CubeIsOutsidePlane(bottomPlane, bound);
			return outside
				? ClipProofResult::Outside : ClipProofResult::Overlap;
		}

		bool IsZeroAlphaNoOpBlend(const NiAlphaProperty* alpha)
		{
			if (!alpha || !alpha->GetAlphaBlending())
				return false;
			const NiAlphaProperty::AlphaFunction source =
				static_cast<NiAlphaProperty::AlphaFunction>(
					alpha->m_usFlags.GetField(
						NiAlphaProperty::SRC_BLEND_MASK,
						NiAlphaProperty::SRC_BLEND_POS));
			const NiAlphaProperty::AlphaFunction destination =
				static_cast<NiAlphaProperty::AlphaFunction>(
					alpha->m_usFlags.GetField(
						NiAlphaProperty::DEST_BLEND_MASK,
						NiAlphaProperty::DEST_BLEND_POS));
			const bool sourceContributesNothing =
				source == NiAlphaProperty::ALPHA_ZERO
				|| source == NiAlphaProperty::ALPHA_SRCALPHA
				|| source == NiAlphaProperty::ALPHA_SRCALPHASAT;
			const bool destinationUnchanged =
				destination == NiAlphaProperty::ALPHA_ONE
				|| destination == NiAlphaProperty::ALPHA_INVSRCALPHA;
			return sourceContributesNothing && destinationUnchanged;
		}
	}

	NativeA8LateVisibilityScope::NativeA8LateVisibilityScope(
		const NiTriShape* geometry, const NativeA8ShapePayload* payload)
		: m_geometry(geometry), m_payload(payload),
		  m_previous(s_lateVisibilityScope)
	{
		s_lateVisibilityScope = this;
	}

	NativeA8LateVisibilityScope::~NativeA8LateVisibilityScope()
	{
		if (s_lateVisibilityScope == this)
			s_lateVisibilityScope = m_previous;
	}

	NativeA8VisibilityCull EvaluateNativeA8SubmissionVisibility(
		const NiTriShape* facade, const NativeA8ShapePayload& payload)
	{
		(void)payload;
		RecordFreeTypePerf(FreeTypePerfCounter::VisibilityCheck);
		if (!facade)
			return NativeA8VisibilityCull::None;

		const TileVisibilityPropertyView* tile = GetTileProperty(facade);
		if (!tile)
			return NativeA8VisibilityCull::None;
		const NiMaterialProperty* material = facade->GetMaterialProperty();
		const float materialAlpha = material ? material->m_fAlpha : 1.0f;
		if (std::isfinite(tile->tileAlpha) && std::isfinite(materialAlpha)
			&& IsZeroAlphaNoOpBlend(facade->GetAlphaProperty())
			&& (tile->tileAlpha == 0.0f || materialAlpha == 0.0f))
		{
			return NativeA8VisibilityCull::ZeroAlpha;
		}
		return NativeA8VisibilityCull::None;
	}

	NativeA8VisibilityCull EvaluateNativeA8PreflightClipVisibility(
		const NiTriShape* facade, const NativeA8ShapePayload& payload)
	{
		if (!g_bEnableFreeTypeFontPreflightClipCull)
			return NativeA8VisibilityCull::None;
		RecordFreeTypePerf(
			FreeTypePerfCounter::VisibilityPreflightClipCheck);
		FreeTypePerfScope clipProofPerf(
			FreeTypePerfPhase::PreflightClipTotal);
		auto failOpen = [&]()
		{
			RecordFreeTypePerf(FreeTypePerfCounter::
				VisibilityPreflightClipFailOpen);
			return NativeA8VisibilityCull::None;
		};

		// The sorted-frame preflight runs after every RegisterObject for this
		// flush and before the stock immediate pass loop. The retained
		// constants-state keys prove view/projection/viewport never mutate
		// between this stage and replay (zero view/projection mismatches
		// recorded session-wide), so the exact retail clip proof the late
		// slot-31 scope runs is already sound here. Anything uncertain fails
		// open, and every preflight cull is additionally revalidated by
		// HonorNativeA8PreflightClipCull at dispatch before it is honored.
		if (!facade || !payload.buildComplete || !payload.payloadTemplate)
			return failOpen();
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		if (!renderer)
			return failOpen();
		const TileVisibilityPropertyView* tile = GetTileProperty(facade);
		if (!tile)
			return failOpen();
		D3DXMATRIX world = {};
		{
			FreeTypePerfScope worldPerf(
				FreeTypePerfPhase::PreflightClipWorld);
			if (!BuildRetailTileWorldMatrix(facade->m_kWorld, world))
				return failOpen();
		}
		// Classify the shared single-slot transform cache before the proof so
		// the hit/miss split shows whether the per-entry world rebuild or the
		// frame-constant view/projection dominates the proof cost.
		const bool transformCacheHit = s_clipTransformCache.valid
			&& s_clipTransformCache.renderer == renderer
			&& std::memcmp(&s_clipTransformCache.world, &world,
				sizeof(world)) == 0
			&& std::memcmp(&s_clipTransformCache.view,
				&renderer->m_kD3DView,
				sizeof(s_clipTransformCache.view)) == 0
			&& std::memcmp(&s_clipTransformCache.projection,
				&renderer->m_kD3DProj,
				sizeof(s_clipTransformCache.projection)) == 0;
		NativeA8VisibilityCull reason = NativeA8VisibilityCull::None;
		ClipProofResult proof;
		{
			FreeTypePerfScope proofPerf(
				FreeTypePerfPhase::PreflightClipProof);
			proof = EvaluatePayloadClip(payload, *tile,
				*renderer, world, g_bEnableFreeTypeFontStructuralFastPaths,
				reason);
		}
		RecordFreeTypePerf(transformCacheHit
			? FreeTypePerfCounter::VisibilityPreflightClipTransformHit
			: FreeTypePerfCounter::VisibilityPreflightClipTransformMiss);
		if (proof != ClipProofResult::Outside)
			return NativeA8VisibilityCull::None;
		RecordFreeTypePerf(
			FreeTypePerfCounter::VisibilityPreflightClipCulled);
		RecordFreeTypePerf(reason == NativeA8VisibilityCull::Scissor
			? FreeTypePerfCounter::VisibilityPreflightClipScissor
			: FreeTypePerfCounter::VisibilityPreflightClipViewport);
		return reason;
	}

	bool HonorNativeA8PreflightClipCull(const NiTriShape* facade,
		NativeA8VisibilityCull preflightCull)
	{
		if (!g_bEnableFreeTypeFontPreflightClipCull)
			return false;
		FreeTypePerfScope honorGatePerf(
			FreeTypePerfPhase::PreflightClipHonorGate);
		if (preflightCull == NativeA8VisibilityCull::Clip)
		{
			// Viewport-branch proofs consume only the sealed payload bound,
			// the facade world matrix and the interface viewport; all of them
			// are frozen for the current flush.
			return true;
		}
		if (preflightCull != NativeA8VisibilityCull::Scissor)
			return false;
		// Scissor-branch proofs additionally depend on the live Tile scissor
		// rectangle and the engine's scaled-scissor global. Tile state is
		// frozen during the render flush, but the scaled-scissor flag is an
		// engine-owned global; any activation revokes the cached decision and
		// returns the entry to the ordinary draw path.
		if (*reinterpret_cast<const UInt8*>(kScaledScissorActive))
			return false;
		const TileVisibilityPropertyView* tile = GetTileProperty(facade);
		return tile && tile->useScissorTest;
	}

	NativeA8VisibilityCull EvaluateNativeA8PreAccumulatorVisibility(
		const NiTriShape* facade, const NativeA8ShapePayload& payload,
		const NiPropertyState* properties,
		const BSShaderProperty* shaderProperty, BSShader* shader)
	{
		RecordFreeTypePerf(
			FreeTypePerfCounter::VisibilityPreAccumulatorCheck);
		RecordFreeTypePerf(FreeTypePerfCounter::VisibilityCheck);
		auto failOpen = []()
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::VisibilityPreAccumulatorFailOpen);
			return NativeA8VisibilityCull::None;
		};

		// RegisterObject receives the live state stock would append to the Tile
		// accumulator.  Suppress only when every pointer names the facade's exact
		// current property state; an alternate property state or chained shader
		// identity remains entirely on the predecessor path.
		if (!facade || !payload.buildComplete || !properties
			|| properties != &facade->m_kProperties || !shaderProperty
			|| !shader || shader != facade->GetShader()
			|| !shader->IsTileShader())
		{
			return failOpen();
		}
		NiShadeProperty* shade =
			properties->m_spShadeProperty.m_pObject;
		if (!shade || shade != facade->GetShadeProperty())
		{
			return failOpen();
		}
		const TileVisibilityPropertyView* tile = GetTileProperty(properties);
		if (!tile || tile != GetTileProperty(facade)
			|| static_cast<const BSShaderProperty*>(tile) != shaderProperty)
			return failOpen();

		const NiAlphaProperty* alpha =
			properties->m_spAlphaProperty.m_pObject;
		const NiMaterialProperty* material =
			properties->m_spMaterialProperty.m_pObject;
		if (alpha != facade->GetAlphaProperty()
			|| material != facade->GetMaterialProperty())
		{
			return failOpen();
		}
		RecordFreeTypePerf(
			FreeTypePerfCounter::VisibilityPreAccumulatorEligible);
		const float materialAlpha = material ? material->m_fAlpha : 1.0f;
		if (std::isfinite(tile->tileAlpha) && std::isfinite(materialAlpha)
			&& IsZeroAlphaNoOpBlend(alpha)
			&& (tile->tileAlpha == 0.0f || materialAlpha == 0.0f))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::VisibilityPreAccumulatorCulled);
			RecordFreeTypePerf(
				FreeTypePerfCounter::VisibilityPreAccumulatorZeroAlpha);
			return NativeA8VisibilityCull::ZeroAlpha;
		}

		// RegisterObject runs before TileShader has established the authoritative
		// interface view/projection for this entry. The Tile scissor itself is live,
		// but combining it with the renderer's current matrices here can compare two
		// different passes and falsely reject visible text. Defer every scissored
		// entry to the existing list-subtree and slot-31 visibility proofs.
		if (tile->useScissorTest)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::VisibilityPreAccumulatorScissorDeferred);
		}
		return NativeA8VisibilityCull::None;
	}

	bool IsNativeA8PayloadOutsideScissorForWorld(
		const NativeA8ShapePayload& payload,
		const NiPropertyState* properties,
		const NiDX9Renderer* renderer,
		const NiTransform& effectiveWorld)
	{
		if (!properties || !renderer
			|| *reinterpret_cast<const UInt8*>(kScaledScissorActive))
		{
			return false;
		}
		const TileVisibilityPropertyView* tile = GetTileProperty(properties);
		if (!tile || !tile->useScissorTest)
			return false;
		D3DXMATRIX world = {};
		NativeA8VisibilityCull reason = NativeA8VisibilityCull::None;
		return BuildRetailTileWorldMatrix(effectiveWorld, world)
			&& EvaluatePayloadClip(payload, *tile, *renderer, world,
				false, reason) == ClipProofResult::Outside
			&& reason == NativeA8VisibilityCull::Scissor;
	}

	NativeA8VisibilityCull EvaluateNativeA8SnapshotVisibility(
		const NativeA8ShapePayload& payload,
		const NiPropertyState* properties,
		const NiDX9Renderer* renderer,
		const NativeTileInstancingSnapshot& snapshot,
		bool allowViewport)
	{
		if (!properties || !renderer)
			return NativeA8VisibilityCull::None;
		const TileVisibilityPropertyView* tile = GetTileProperty(properties);
		if (!tile)
			return NativeA8VisibilityCull::None;

		// NativeTileConstantsLite stores the exact retail c0-c3 values, which are
		// the transpose of the row-vector world-view-projection matrix used by the
		// homogeneous interval proof. Reusing that completed live snapshot avoids a
		// second pair of matrix multiplies for every Credit/list member.
		D3DXMATRIX worldViewProjection = {};
		D3DXMatrixTranspose(
			&worldViewProjection, &snapshot.wvpColumns);
		NativeA8VisibilityCull reason = NativeA8VisibilityCull::None;
		return EvaluatePayloadClip(payload, *tile, *renderer,
				snapshot.retailWorld, allowViewport, reason, true,
				&worldViewProjection) == ClipProofResult::Outside
			? reason : NativeA8VisibilityCull::None;
	}

	bool EvaluateNativeA8PreConstantsVisibility(
		const NiTriShape* geometry, const NativeA8ShapePayload& payload,
		const NiPropertyState* properties, NiDX9Renderer* renderer,
		IDirect3DDevice9* device, bool verifiedRetailSlot31)
	{
		NativeA8LateVisibilityScope* scope = s_lateVisibilityScope;
		if (!scope || scope->m_evaluated || !verifiedRetailSlot31
			|| !geometry || scope->m_geometry != geometry
			|| scope->m_payload != &payload || !properties
			|| properties != &geometry->m_kProperties || !renderer
			|| renderer != NiDX9Renderer::GetSingleton() || !device
			|| renderer->GetD3DDevice() != device)
		{
			return false;
		}
		BSShaderProperty::RenderPass* currentPass =
			*reinterpret_cast<BSShaderProperty::RenderPass**>(
				kCurrentRenderPass);
		if (!currentPass || currentPass->pGeometry != geometry)
			return false;
		const TileVisibilityPropertyView* tile = GetTileProperty(properties);
		if (!tile || !payload.payloadTemplate)
			return false;
		if (IsNativeA8CrossTextBatchVisibilityResolved(geometry))
		{
			// The leader-time complete snapshots proved every member separately and
			// compacted only the invisible instances. Treating the leader bound as the
			// bound of the whole instanced draw would incorrectly suppress visible
			// followers when the first Credit/list row is outside the viewport.
			scope->m_evaluated = true;
			return false;
		}
		D3DXMATRIX world = {};
		if (!BuildRetailTileWorldMatrix(geometry->m_kWorld, world))
			return false;
		NativeA8VisibilityCull reason = NativeA8VisibilityCull::None;
		const ClipProofResult proof = EvaluatePayloadClip(
			payload, *tile, *renderer, world,
			g_bEnableFreeTypeFontStructuralFastPaths, reason, true);
		if (proof == ClipProofResult::Unproven)
			return false;
		// Slot 31 does not mutate any proof input; it only publishes this same
		// model matrix/constants and device state. Once the exact pre-slot
		// inputs were evaluated, repeating the identical test in the post-slot
		// fallback would add CPU work without recovering an indeterminate case.
		scope->m_evaluated = true;
		if (proof != ClipProofResult::Outside)
			return false;

		// This is the exact matrix slot 31 would publish: formal PC BCA980
		// calls E6FBB0 -> B71A40 with currentPass->geometry->m_kWorld, and
		// the symbolized test build expresses the same operation as
		// NiXenonRenderer::SetModelTransform. No slot-31 state has run, so
		// Standard-lite can suppress the complete pass without a slot-35 pop.
		scope->m_cull = reason;
		scope->m_preConstantsCull = true;
		return true;
	}

	void EvaluateNativeA8PostConstantsVisibility(
		const NiPropertyState* properties,
		IDirect3DDevice9* device, bool verifiedRetailSlot31)
	{
		NativeA8LateVisibilityScope* scope = s_lateVisibilityScope;
		if (!scope || scope->m_evaluated)
			return;
		scope->m_evaluated = true;
		if (!verifiedRetailSlot31 || !scope->m_geometry || !scope->m_payload
			|| !properties
			|| properties != &scope->m_geometry->m_kProperties)
		{
			return;
		}
		if (IsNativeA8CrossTextBatchVisibilityResolved(scope->m_geometry))
			return;
		BSShaderProperty::RenderPass* currentPass =
			*reinterpret_cast<BSShaderProperty::RenderPass**>(
				kCurrentRenderPass);
		if (!currentPass || currentPass->pGeometry != scope->m_geometry)
			return;
		const TileVisibilityPropertyView* tile = GetTileProperty(properties);
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		if (!tile || !renderer || !device
			|| renderer->GetD3DDevice() != device)
		{
			return;
		}
		NativeA8VisibilityCull reason = NativeA8VisibilityCull::None;
		if (EvaluatePayloadClip(*scope->m_payload, *tile, *renderer,
			renderer->m_kD3DMat,
			g_bEnableFreeTypeFontStructuralFastPaths, reason, true)
			== ClipProofResult::Outside)
		{
			scope->m_cull = reason;
			scope->m_preConstantsCull = false;
		}
	}

	bool ConsumeNativeA8LateVisibilityCull(const NiTriShape* geometry)
	{
		NativeA8LateVisibilityScope* scope = s_lateVisibilityScope;
		if (!scope || scope->m_geometry != geometry
			|| scope->m_cull == NativeA8VisibilityCull::None)
		{
			return false;
		}
		if (!scope->m_recorded && scope->m_payload)
		{
			scope->m_recorded = true;
			if (scope->m_cull == NativeA8VisibilityCull::Scissor)
			{
				RecordFreeTypePerf(scope->m_preConstantsCull
					? FreeTypePerfCounter::VisibilityScissorPreConstants
					: FreeTypePerfCounter::VisibilityScissorPostConstants);
			}
			else if (scope->m_cull == NativeA8VisibilityCull::Clip)
			{
				RecordFreeTypePerf(scope->m_preConstantsCull
					? FreeTypePerfCounter::VisibilityClipPreConstants
					: FreeTypePerfCounter::VisibilityClipPostConstants);
			}
			RecordNativeA8VisibilityCull(
				scope->m_cull, *scope->m_payload);
		}
		return true;
	}

	void RecordNativeA8VisibilityCull(NativeA8VisibilityCull reason,
		const NativeA8ShapePayload& payload)
	{
		if (reason == NativeA8VisibilityCull::None)
			return;
		RecordFreeTypePerf(FreeTypePerfCounter::VisibilityCulled);
		switch (reason)
		{
		case NativeA8VisibilityCull::AppCulled:
			RecordFreeTypePerf(FreeTypePerfCounter::VisibilityAppCulled);
			break;
		case NativeA8VisibilityCull::ZeroAlpha:
			RecordFreeTypePerf(FreeTypePerfCounter::VisibilityZeroAlpha);
			break;
		case NativeA8VisibilityCull::Clip:
			RecordFreeTypePerf(FreeTypePerfCounter::VisibilityClip);
			break;
		case NativeA8VisibilityCull::Scissor:
			RecordFreeTypePerf(FreeTypePerfCounter::VisibilityScissor);
			break;
		default:
			break;
		}
		if (!payload.payloadTemplate)
			return;
		const std::vector<NativeA8PacketTemplate>& packets =
			GetNativeA8Packets(*payload.payloadTemplate,
				payload.useCompositePackets);
		RecordFreeTypePerf(FreeTypePerfCounter::VisibilityPacketsSaved,
			static_cast<UInt64>(packets.size()));
		UInt64 vertices = 0;
		for (const NativeA8PacketTemplate& packet : packets)
			vertices += packet.vertexCount;
		RecordFreeTypePerf(FreeTypePerfCounter::VisibilityVerticesSaved,
			vertices);
	}
}

#include "font_native_internal.h"

#include "NiAlphaProperty.hpp"
#include "NiDX9Renderer.hpp"
#include "NiDX9RenderState.hpp"
#include "NiMaterialProperty.hpp"
#include "NiPropertyState.hpp"
#include "NiTexturingProperty.hpp"

#include <array>
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
		inline constexpr UInt32 kScaledScissorActive = 0x11F9426;
		inline constexpr UInt32 kRendererPositionAdjust = 0x11F474C;
		inline constexpr double kScissorSafetyMarginPixels = 2.0;
		inline constexpr double kClipIntervalRelativeSlack = 1.0e-6;
		inline constexpr size_t kClipTransformCacheSetCount = 16;
		inline constexpr size_t kClipTransformCacheWays = 4;

		enum class ClipProofResult : UInt8
		{
			Unproven = 0,
			Overlap,
			Outside
		};

		enum class ClipTransformBuildResult : UInt8
		{
			Unavailable = 0,
			Reused,
			IdentityMiss,
			KeyMiss
		};

		struct ClipTransformCacheEntry
		{
			// Identity selects a small candidate set only. It is never dereferenced,
			// and renderer plus all three matrices remain the correctness key. A
			// retired payload address can therefore be reused without returning a
			// stale transform.
			const void* identity = nullptr;
			const NiDX9Renderer* renderer = nullptr;
			D3DXMATRIX world;
			D3DXMATRIX view;
			D3DXMATRIX projection;
			D3DXMATRIX worldViewProjection;
			bool valid = false;
		};

		struct ClipTransformCacheSet
		{
			std::array<ClipTransformCacheEntry, kClipTransformCacheWays> ways;
			size_t replacementWay = 0;
		};

		static_assert((kClipTransformCacheSetCount
			& (kClipTransformCacheSetCount - 1u)) == 0);
		thread_local std::array<ClipTransformCacheSet,
			kClipTransformCacheSetCount> s_clipTransformCache;

		size_t HashClipTransformIdentity(const void* identity)
		{
			size_t value = reinterpret_cast<size_t>(identity) >> 4;
			value ^= value >> 16;
			value *= static_cast<size_t>(0x45D9F3Bu);
			value ^= value >> 16;
			return value;
		}

		void RecordClipTransformBuildResult(
			ClipTransformBuildResult result)
		{
			switch (result)
			{
			case ClipTransformBuildResult::Reused:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VisibilityPreflightClipTransformHit);
				break;
			case ClipTransformBuildResult::IdentityMiss:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VisibilityPreflightClipTransformMiss);
				RecordFreeTypePerf(FreeTypePerfCounter::
					VisibilityPreflightClipTransformIdentityMiss);
				break;
			case ClipTransformBuildResult::KeyMiss:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VisibilityPreflightClipTransformMiss);
				RecordFreeTypePerf(FreeTypePerfCounter::
					VisibilityPreflightClipTransformKeyMiss);
				break;
			case ClipTransformBuildResult::Unavailable:
			default:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VisibilityPreflightClipTransformUnavailable);
				break;
			}
		}

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

		ClipTransformBuildResult BuildWorldViewProjection(
			const void* identity, const NiDX9Renderer& renderer,
			const D3DXMATRIX& world, D3DXMATRIX& worldViewProjection)
		{
			ClipTransformCacheSet* cacheSet = nullptr;
			ClipTransformCacheEntry* identityEntry = nullptr;
			ClipTransformCacheEntry* invalidEntry = nullptr;
			if (identity)
			{
				cacheSet = &s_clipTransformCache[
					HashClipTransformIdentity(identity)
						& (kClipTransformCacheSetCount - 1u)];
				for (ClipTransformCacheEntry& candidate : cacheSet->ways)
				{
					if (!candidate.valid)
					{
						if (!invalidEntry)
							invalidEntry = &candidate;
						continue;
					}
					if (candidate.identity == identity)
					{
						identityEntry = &candidate;
						break;
					}
				}
			}

			if (identityEntry && identityEntry->renderer == &renderer
				&& std::memcmp(&identityEntry->world, &world,
					sizeof(world)) == 0
				&& std::memcmp(&identityEntry->view, &renderer.m_kD3DView,
					sizeof(identityEntry->view)) == 0
				&& std::memcmp(&identityEntry->projection,
					&renderer.m_kD3DProj,
					sizeof(identityEntry->projection)) == 0)
			{
				worldViewProjection = identityEntry->worldViewProjection;
				return ClipTransformBuildResult::Reused;
			}

			if (!IsFiniteMatrix(world)
				|| !IsFiniteMatrix(renderer.m_kD3DView)
				|| !IsFiniteMatrix(renderer.m_kD3DProj))
			{
				return ClipTransformBuildResult::Unavailable;
			}
			D3DXMATRIX worldView = {};
			D3DXMatrixMultiply(
				&worldView, &world, &renderer.m_kD3DView);
			D3DXMatrixMultiply(&worldViewProjection,
				&worldView, &renderer.m_kD3DProj);
			if (!IsFiniteMatrix(worldViewProjection))
				return ClipTransformBuildResult::Unavailable;

			if (cacheSet)
			{
				ClipTransformCacheEntry* target = identityEntry
					? identityEntry : invalidEntry;
				if (!target)
				{
					target = &cacheSet->ways[cacheSet->replacementWay];
					cacheSet->replacementWay =
						(cacheSet->replacementWay + 1u)
							% kClipTransformCacheWays;
				}
				target->identity = identity;
				target->renderer = &renderer;
				std::memcpy(&target->world, &world, sizeof(world));
				std::memcpy(&target->view, &renderer.m_kD3DView,
					sizeof(target->view));
				std::memcpy(&target->projection, &renderer.m_kD3DProj,
					sizeof(target->projection));
				target->worldViewProjection = worldViewProjection;
				target->valid = true;
			}
			return identityEntry
				? ClipTransformBuildResult::KeyMiss
				: ClipTransformBuildResult::IdentityMiss;
		}

		ClipProofResult EvaluateBoundClip(const NiBound& bound,
			const void* transformIdentity,
			const TileVisibilityPropertyView& tile,
			const NiDX9Renderer& renderer, const D3DXMATRIX& world,
			bool allowViewport, NativeA8VisibilityCull& reason,
			ClipTransformBuildResult* transformBuildResult)
		{
			reason = NativeA8VisibilityCull::None;
			if (transformBuildResult)
			{
				*transformBuildResult =
					ClipTransformBuildResult::Unavailable;
			}
			auto failOpen = [&]()
			{
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
			}

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
			const ClipTransformBuildResult buildResult =
				BuildWorldViewProjection(transformIdentity,
					renderer, world, builtWorldViewProjection);
			if (transformBuildResult)
				*transformBuildResult = buildResult;
			if (buildResult == ClipTransformBuildResult::Unavailable)
			{
				return failOpen();
			}
			const D3DXMATRIX* worldViewProjection =
				&builtWorldViewProjection;

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

	NativeA8VisibilityCull EvaluateNativeA8SubmissionVisibility(
		const NiTriShape* facade)
	{
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

	NativeA8VisibilityCull EvaluateNativeA8SubmissionVisibility(
		const NiTriShape* facade, const NativeA8ShapePayload& payload)
	{
		(void)payload;
		return EvaluateNativeA8SubmissionVisibility(facade);
	}

	NativeA8VisibilityCull EvaluateNativeA8PreflightClipVisibility(
		const NiTriShape* facade)
	{
		if (!g_bEnableFreeTypeFontPreflightClipCull)
			return NativeA8VisibilityCull::None;
		RecordFreeTypePerf(
			FreeTypePerfCounter::VisibilityPreflightClipCheck);
		ClipTransformBuildResult transformBuildResult =
			ClipTransformBuildResult::Unavailable;
		auto failOpen = [&]()
		{
			RecordFreeTypePerf(FreeTypePerfCounter::
				VisibilityPreflightClipFailOpen);
			RecordClipTransformBuildResult(transformBuildResult);
			return NativeA8VisibilityCull::None;
		};

		// Facades call this after every RegisterObject for the flush, once the
		// final model bound, world transform, viewport, and Tile scissor exist.
		// Stock-layout SDF shapes call it immediately before their stock geometry
		// draw. Anything uncertain fails open. A facade proof is additionally
		// revalidated by HonorNativeA8PreflightClipCull at dispatch before it is
		// honored; a stock-layout proof already consumes the live dispatch state.
		const NiTriShapeData* data = facade ? facade->GetModelData() : nullptr;
		if (!facade || !data)
			return failOpen();
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		if (!renderer)
			return failOpen();
		const TileVisibilityPropertyView* tile = GetTileProperty(facade);
		if (!tile)
			return failOpen();
		D3DXMATRIX world = {};
		if (!BuildRetailTileWorldMatrix(facade->m_kWorld, world))
			return failOpen();
		NativeA8VisibilityCull reason = NativeA8VisibilityCull::None;
		// This is the final stock-visible model bound. Facade payload vertices are
		// relative and apply geometryOrigin during replay; stock-layout vertices
		// are already engine-owned full geometry. Both representations publish
		// the same full bound before reaching this proof.
		const ClipProofResult proof = EvaluateBoundClip(data->m_kBound,
			facade, *tile, *renderer, world,
			true,
			reason, &transformBuildResult);
		RecordClipTransformBuildResult(transformBuildResult);
		if (proof != ClipProofResult::Outside)
			return NativeA8VisibilityCull::None;
		RecordFreeTypePerf(
			FreeTypePerfCounter::VisibilityPreflightClipCulled);
		RecordFreeTypePerf(reason == NativeA8VisibilityCull::Scissor
			? FreeTypePerfCounter::VisibilityPreflightClipScissor
			: FreeTypePerfCounter::VisibilityPreflightClipViewport);
		return reason;
	}

	NativeA8VisibilityCull EvaluateNativeA8PreflightClipVisibility(
		const NiTriShape* facade, const NativeA8ShapePayload& payload)
	{
		(void)payload;
		return EvaluateNativeA8PreflightClipVisibility(facade);
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
			// Viewport-branch proofs consume only the final model bound,
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
		if (!payload.payloadTemplate)
			return false;
		return BuildRetailTileWorldMatrix(effectiveWorld, world)
			&& EvaluateBoundClip(payload.payloadTemplate->bound, &payload,
				*tile, *renderer, world,
				false, reason, nullptr) == ClipProofResult::Outside
			&& reason == NativeA8VisibilityCull::Scissor;
	}

	void RecordNativeA8VisibilityCull(NativeA8VisibilityCull reason)
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
	}

	void RecordNativeA8VisibilityCull(NativeA8VisibilityCull reason,
		const NativeA8ShapePayload& payload)
	{
		RecordNativeA8VisibilityCull(reason);
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

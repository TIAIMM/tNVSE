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
		inline constexpr UInt32 kBSShaderManager_bLetterbox = 0x11F9426;
		inline constexpr UInt32 kBSGraphics_CameraWorldTranslate = 0x11F474C;
		inline constexpr double kScissorSafetyMarginPixels = 2.0;
		inline constexpr double kClipIntervalRelativeSlack = 1.0e-6;
		inline constexpr double kVanillaUiOrthographicRelativeSlack = 4.0e-6;
		// The cache stores only exact source-state keys and the conservative proof,
		// not four complete matrices per facade. Four-way 4096-entry storage remains
		// bounded below half a MiB on Win32 while covering large Interface batches.
		inline constexpr size_t kClipProofCacheSetCount = 1024;
		inline constexpr size_t kClipProofCacheWays = 4;
		inline constexpr size_t kClipRectNdcCacheEntries = 16;

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

		struct ClipNdcBounds
		{
			double left = 0.0;
			double right = 0.0;
			double top = 0.0;
			double bottom = 0.0;
		};

		struct ClipRectNdcCacheEntry
		{
			RECT rect = {};
			ClipNdcBounds bounds;
			bool valid = false;
		};

		struct ClipCameraKey
		{
			const NiDX9Renderer* renderer = nullptr;
			D3DXMATRIX view;
			D3DXMATRIX projection;
			D3DVIEWPORT9 viewport = {};
			NiPoint3 positionAdjust;
			bool primitiveClipping = false;
			bool scaledScissor = false;
		};

		struct VanillaUiOrthographicAxis
		{
			UInt8 modelAxis = 0;
			float viewCoefficient = 0.0f;
			float viewTranslation = 0.0f;
			float projectionScale = 0.0f;
			float projectionTranslation = 0.0f;
		};

		struct VanillaUiOrthographicContext
		{
			VanillaUiOrthographicAxis x;
			VanillaUiOrthographicAxis y;
			bool valid = false;
		};

		struct ClipFrameContext
		{
			ClipCameraKey camera;
			VanillaUiOrthographicContext vanillaUiOrthographic;
			RECT viewportRect = {};
			ClipNdcBounds viewportNdc;
			std::array<ClipRectNdcCacheEntry,
				kClipRectNdcCacheEntries> rectNdcCache;
			size_t rectNdcReplacement = 0;
			UInt64 frameToken = 0;
			UInt64 cameraEpoch = 0;
			UInt64 sortedTilePropertyLookupsElided = 0;
			UInt64 sortedAlphaPropertyLookupsElided = 0;
			double inverseWidth = 0.0;
			double inverseHeight = 0.0;
			bool active = false;
			bool valid = false;
			bool preflightOpen = false;
		};

		struct ClipProofCacheEntry
		{
			// Identity only selects candidates. Every value that affects the proof is
			// compared exactly before reuse, so a retired facade address can be reused
			// without returning stale visibility.
			const void* identity = nullptr;
			const NiDX9Renderer* renderer = nullptr;
			std::array<UInt32, 13> transformBits = {};
			std::array<UInt32, 4> boundBits = {};
			RECT scissorRect = {};
			UInt64 cameraEpoch = 0;
			NativeFontVisibilityCull reason = NativeFontVisibilityCull::None;
			ClipProofResult result = ClipProofResult::Unproven;
			bool tileUsesScissor = false;
			bool allowViewport = false;
			bool valid = false;
		};

		struct ClipProofCacheSet
		{
			std::array<ClipProofCacheEntry, kClipProofCacheWays> ways;
			size_t replacementWay = 0;
		};

		using ClipProofCacheStorage = std::array<ClipProofCacheSet,
			kClipProofCacheSetCount>;
		static_assert((kClipProofCacheSetCount
			& (kClipProofCacheSetCount - 1u)) == 0);
		static_assert(sizeof(ClipProofCacheStorage) <= 512u * 1024u,
			"Visibility proof cache must remain bounded to 512 KiB per thread");
		thread_local ClipProofCacheStorage s_clipProofCache;
		thread_local ClipFrameContext s_clipFrameContext;
		thread_local ClipCameraKey s_previousClipCamera;
		thread_local UInt64 s_clipCameraEpoch = 0;
		thread_local UInt64 s_clipFrameToken = 0;
		thread_local bool s_previousClipCameraValid = false;
		thread_local UInt32 s_visibilityHonorSampleCursor = 0;

		bool ShouldSampleVisibilityHonorGate()
		{
			if (!g_bEnableFreeTypeFontRenderingLog)
				return false;
			const UInt32 sample = s_visibilityHonorSampleCursor++;
			return sample % kNativeFontVisibilityHonorCpuSampleRate == 0u;
		}

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

		// FreeType text-shape construction returns a TileShaderProperty, but the
		// concrete class is not exposed by this CommonLib snapshot. Keep this
		// read-only view tied to the retail ABI used by packet state mirroring.
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
			const NiPoint3& positionAdjust, D3DXMATRIX& world)
		{
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
			const NiBound& bound,
			double relativeSlack = kClipIntervalRelativeSlack)
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
				* relativeSlack;
			return center + extent < -slack;
		}

		bool CubeIsOutsideOrthographicAxisInterval(
			const ClipColumn& axis, double minimum, double maximum,
			const NiBound& bound)
		{
			// Orthographic Interface geometry has clipW=1. Collapse the two
			// opposing homogeneous planes for one axis into a shared linear
			// center, extent, and magnitude. This is the same conservative cube
			// proof as CubeIsOutsidePlane, with its larger UI safety slack.
			const NiPoint3& center = bound.m_kCenter;
			const double linearCenter = axis.x * center.x
				+ axis.y * center.y + axis.z * center.z;
			const double linearMagnitude = std::abs(axis.x * center.x)
				+ std::abs(axis.y * center.y)
				+ std::abs(axis.z * center.z);
			const double extent = CubeExtent(axis, bound.m_fRadius);

			const double minimumTranslation = axis.translation - minimum;
			const double minimumSlack = (linearMagnitude
				+ std::abs(minimumTranslation) + extent + 1.0)
				* kVanillaUiOrthographicRelativeSlack;
			if (linearCenter + minimumTranslation + extent < -minimumSlack)
				return true;

			const double maximumTranslation = maximum - axis.translation;
			const double maximumSlack = (linearMagnitude
				+ std::abs(maximumTranslation) + extent + 1.0)
				* kVanillaUiOrthographicRelativeSlack;
			return -linearCenter + maximumTranslation + extent
				< -maximumSlack;
		}

		bool ResolveVanillaUiViewAxis(const D3DXMATRIX& view, UInt32 column,
			UInt8& modelAxis, float& coefficient)
		{
			bool found = false;
			for (UInt8 row = 0; row < 3; ++row)
			{
				const float value = view.m[row][column];
				if (value == 0.0f)
					continue;
				if (found || (value != 1.0f && value != -1.0f))
					return false;
				modelAxis = row;
				coefficient = value;
				found = true;
			}
			return found;
		}

		bool BuildVanillaUiOrthographicContext(const ClipCameraKey& camera,
			VanillaUiOrthographicContext& context)
		{
			context = {};
			const D3DXMATRIX& view = camera.view;
			const D3DXMATRIX& projection = camera.projection;

			// InterfaceManager::CreateSceneGraph creates an orthographic camera
			// whose right/up vectors are exact model-axis permutations. Formal PC
			// 712E90/E6C780 and the symbolized test build independently agree on
			// that layout. Require the complete sparse x/y/w structure rather than
			// inferring it from a menu or shader identity, so camera mods and every
			// perspective/rotated view retain the homogeneous fallback below.
			if (view._14 != 0.0f || view._24 != 0.0f
				|| view._34 != 0.0f || view._44 != 1.0f
				|| projection._21 != 0.0f || projection._31 != 0.0f
				|| projection._12 != 0.0f || projection._32 != 0.0f
				|| projection._14 != 0.0f || projection._24 != 0.0f
				|| projection._34 != 0.0f || projection._44 != 1.0f
				|| projection._11 == 0.0f || projection._22 == 0.0f)
			{
				return false;
			}

			if (!ResolveVanillaUiViewAxis(view, 0, context.x.modelAxis,
					context.x.viewCoefficient)
				|| !ResolveVanillaUiViewAxis(view, 1, context.y.modelAxis,
					context.y.viewCoefficient)
				|| context.x.modelAxis == context.y.modelAxis)
			{
				return false;
			}

			context.x.viewTranslation = view._41;
			context.y.viewTranslation = view._42;
			context.x.projectionScale = projection._11;
			context.y.projectionScale = projection._22;
			context.x.projectionTranslation = projection._41;
			context.y.projectionTranslation = projection._42;
			context.valid = true;
			return true;
		}

		float PointComponent(const NiPoint3& point, UInt8 axis)
		{
			switch (axis)
			{
			case 0:
				return point.x;
			case 1:
				return point.y;
			case 2:
				return point.z;
			default:
				return 0.0f;
			}
		}

		bool IsIdentityRotation(const NiMatrix3& rotation)
		{
			for (UInt32 row = 0; row < 3; ++row)
			{
				for (UInt32 column = 0; column < 3; ++column)
				{
					const float expected = row == column ? 1.0f : 0.0f;
					if (rotation.m_pEntry[row][column] != expected)
						return false;
				}
			}
			return true;
		}

		bool BuildVanillaUiOrthographicColumn(
			const VanillaUiOrthographicAxis& axis,
			const NiTransform& transform, const NiPoint3& positionAdjust,
			ClipColumn& column)
		{
			const float translation = PointComponent(
				transform.m_Translate, axis.modelAxis)
				- PointComponent(positionAdjust, axis.modelAxis);
			const float viewLinear =
				transform.m_fScale * axis.viewCoefficient;
			const float viewTranslation =
				translation * axis.viewCoefficient + axis.viewTranslation;
			const float clipLinear = viewLinear * axis.projectionScale;
			const float clipTranslation = viewTranslation
				* axis.projectionScale + axis.projectionTranslation;
			if (!std::isfinite(translation) || !std::isfinite(viewLinear)
				|| !std::isfinite(viewTranslation)
				|| !std::isfinite(clipLinear)
				|| !std::isfinite(clipTranslation))
			{
				return false;
			}

			column = {};
			switch (axis.modelAxis)
			{
			case 0:
				column.x = clipLinear;
				break;
			case 1:
				column.y = clipLinear;
				break;
			case 2:
				column.z = clipLinear;
				break;
			default:
				return false;
			}
			column.translation = clipTranslation;
			return true;
		}

		bool BuildVanillaUiOrthographicColumns(const ClipFrameContext& context,
			const NiTransform& transform, ClipColumn& clipX,
			ClipColumn& clipY)
		{
			if (!context.vanillaUiOrthographic.valid
				|| !std::isfinite(transform.m_fScale)
				|| !std::isfinite(transform.m_Translate.x)
				|| !std::isfinite(transform.m_Translate.y)
				|| !std::isfinite(transform.m_Translate.z)
				|| !IsIdentityRotation(transform.m_Rotate)
				|| !BuildVanillaUiOrthographicColumn(
					context.vanillaUiOrthographic.x, transform,
					context.camera.positionAdjust, clipX)
				|| !BuildVanillaUiOrthographicColumn(
					context.vanillaUiOrthographic.y, transform,
					context.camera.positionAdjust, clipY))
			{
				return false;
			}
			return true;
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

		bool SameRect(const RECT& lhs, const RECT& rhs)
		{
			return lhs.left == rhs.left && lhs.top == rhs.top
				&& lhs.right == rhs.right && lhs.bottom == rhs.bottom;
		}

		UInt32 FloatBits(float value)
		{
			UInt32 bits = 0;
			std::memcpy(&bits, &value, sizeof(bits));
			return bits;
		}

		std::array<UInt32, 13> CaptureTransformBits(
			const NiTransform& transform)
		{
			std::array<UInt32, 13> bits = {};
			size_t write = 0;
			for (UInt32 row = 0; row < 3; ++row)
			{
				for (UInt32 column = 0; column < 3; ++column)
				{
					bits[write++] = FloatBits(
						transform.m_Rotate.m_pEntry[row][column]);
				}
			}
			bits[write++] = FloatBits(transform.m_Translate.x);
			bits[write++] = FloatBits(transform.m_Translate.y);
			bits[write++] = FloatBits(transform.m_Translate.z);
			bits[write] = FloatBits(transform.m_fScale);
			return bits;
		}

		std::array<UInt32, 4> CaptureBoundBits(const NiBound& bound)
		{
			return {
				FloatBits(bound.m_kCenter.x), FloatBits(bound.m_kCenter.y),
				FloatBits(bound.m_kCenter.z), FloatBits(bound.m_fRadius)
			};
		}

		NativeFontVisibilityProofStatus ToVisibilityProofStatus(
			ClipProofResult result)
		{
			switch (result)
			{
			case ClipProofResult::Overlap:
				return NativeFontVisibilityProofStatus::Overlap;
			case ClipProofResult::Outside:
				return NativeFontVisibilityProofStatus::Outside;
			case ClipProofResult::Unproven:
			default:
				return NativeFontVisibilityProofStatus::Unproven;
			}
		}

		void CaptureVisibilityProofWitness(
			NativeFontVisibilityProofWitness* witness,
			const ClipFrameContext& context,
			const std::array<UInt32, 13>& transformBits,
			const std::array<UInt32, 4>& boundBits,
			const TileVisibilityPropertyView& tile,
			ClipProofResult result, NativeFontVisibilityCull reason)
		{
			if (!witness || !context.cameraEpoch
				|| result == ClipProofResult::Unproven)
			{
				return;
			}
			witness->renderer = context.camera.renderer;
			witness->transformBits = transformBits;
			witness->boundBits = boundBits;
			witness->tileScissorRect = tile.scissorRect;
			witness->cameraEpoch = context.cameraEpoch;
			witness->status = ToVisibilityProofStatus(result);
			witness->cullReason = reason;
			witness->tileUsesScissor = tile.useScissorTest;
			witness->valid = true;
		}

		bool SameCameraKey(const ClipCameraKey& left,
			const ClipCameraKey& right)
		{
			return left.renderer == right.renderer
				&& std::memcmp(&left.view, &right.view,
					sizeof(left.view)) == 0
				&& std::memcmp(&left.projection, &right.projection,
					sizeof(left.projection)) == 0
				&& std::memcmp(&left.viewport, &right.viewport,
					sizeof(left.viewport)) == 0
				&& FloatBits(left.positionAdjust.x)
					== FloatBits(right.positionAdjust.x)
				&& FloatBits(left.positionAdjust.y)
					== FloatBits(right.positionAdjust.y)
				&& FloatBits(left.positionAdjust.z)
					== FloatBits(right.positionAdjust.z)
				&& left.primitiveClipping == right.primitiveClipping
				&& left.scaledScissor == right.scaledScissor;
		}

		bool BuildClipNdcBounds(const ClipFrameContext& context,
			const RECT& rect, ClipNdcBounds& bounds)
		{
			if (!context.valid)
				return false;
			const D3DVIEWPORT9& viewport = context.camera.viewport;
			bounds.left = ((static_cast<double>(rect.left)
				- kScissorSafetyMarginPixels - viewport.X)
				* 2.0 * context.inverseWidth) - 1.0;
			bounds.right = ((static_cast<double>(rect.right)
				+ kScissorSafetyMarginPixels - viewport.X)
				* 2.0 * context.inverseWidth) - 1.0;
			bounds.top = 1.0 - ((static_cast<double>(rect.top)
				- kScissorSafetyMarginPixels - viewport.Y)
				* 2.0 * context.inverseHeight);
			bounds.bottom = 1.0 - ((static_cast<double>(rect.bottom)
				+ kScissorSafetyMarginPixels - viewport.Y)
				* 2.0 * context.inverseHeight);
			return std::isfinite(bounds.left) && std::isfinite(bounds.right)
				&& std::isfinite(bounds.top) && std::isfinite(bounds.bottom);
		}

		bool CaptureClipFrameContext(ClipFrameContext& context,
			bool persistent, NiDX9Renderer* rendererOverride = nullptr)
		{
			context = ClipFrameContext{};
			context.active = true;
			if (persistent)
			{
				context.frameToken = ++s_clipFrameToken;
				if (!context.frameToken)
					context.frameToken = ++s_clipFrameToken;
			}

			NiDX9Renderer* renderer = rendererOverride
				? rendererOverride : NiDX9Renderer::GetSingleton();
			if (!renderer)
				return false;
			context.camera.renderer = renderer;
			context.camera.view = renderer->m_kD3DView;
			context.camera.projection = renderer->m_kD3DProj;
			context.camera.viewport = renderer->m_kD3DPort;
			context.camera.positionAdjust =
				*reinterpret_cast<const NiPoint3*>(
					kBSGraphics_CameraWorldTranslate);
			context.camera.primitiveClipping =
				IsPrimitiveClippingEnabled(*renderer);
			context.camera.scaledScissor =
				*reinterpret_cast<const UInt8*>(kBSShaderManager_bLetterbox) != 0;
			if (!IsFiniteMatrix(context.camera.view)
				|| !IsFiniteMatrix(context.camera.projection)
				|| !std::isfinite(context.camera.positionAdjust.x)
				|| !std::isfinite(context.camera.positionAdjust.y)
				|| !std::isfinite(context.camera.positionAdjust.z)
				|| !BuildViewportRect(context.camera.viewport,
					context.viewportRect))
			{
				return false;
			}

			context.inverseWidth = 1.0
				/ static_cast<double>(context.camera.viewport.Width);
			context.inverseHeight = 1.0
				/ static_cast<double>(context.camera.viewport.Height);
			context.valid = true;
			BuildVanillaUiOrthographicContext(context.camera,
				context.vanillaUiOrthographic);
			if (!BuildClipNdcBounds(context, context.viewportRect,
					context.viewportNdc))
			{
				context.valid = false;
				return false;
			}

			if (persistent)
			{
				if (!s_previousClipCameraValid
					|| !SameCameraKey(
						s_previousClipCamera, context.camera))
				{
					++s_clipCameraEpoch;
					if (!s_clipCameraEpoch)
					{
						s_clipCameraEpoch = 1;
						for (ClipProofCacheSet& set : s_clipProofCache)
						{
							set.replacementWay = 0;
							for (ClipProofCacheEntry& entry : set.ways)
								entry.valid = false;
						}
					}
					s_previousClipCamera = context.camera;
					s_previousClipCameraValid = true;
				}
				context.cameraEpoch = s_clipCameraEpoch;
			}
			return true;
		}

		bool ResolveClipNdcBounds(ClipFrameContext& context,
			const RECT& rect, bool useScissor, ClipNdcBounds& bounds)
		{
			if (!useScissor)
			{
				bounds = context.viewportNdc;
				return true;
			}
			for (const ClipRectNdcCacheEntry& candidate
				: context.rectNdcCache)
			{
				if (candidate.valid && SameRect(candidate.rect, rect))
				{
					bounds = candidate.bounds;
					return true;
				}
			}
			ClipNdcBounds built;
			if (!BuildClipNdcBounds(context, rect, built))
				return false;
			ClipRectNdcCacheEntry& target = context.rectNdcCache[
				context.rectNdcReplacement];
			context.rectNdcReplacement = (context.rectNdcReplacement + 1u)
				% kClipRectNdcCacheEntries;
			target.rect = rect;
			target.bounds = built;
			target.valid = true;
			bounds = built;
			return true;
		}

		bool SameLiveTransformBits(
			const std::array<UInt32, 13>& expected,
			const NiTransform& transform)
		{
			size_t read = 0;
			for (UInt32 row = 0; row < 3; ++row)
			{
				for (UInt32 column = 0; column < 3; ++column)
				{
					if (expected[read++] != FloatBits(
							transform.m_Rotate.m_pEntry[row][column]))
					{
						return false;
					}
				}
			}
			return expected[read++] == FloatBits(transform.m_Translate.x)
				&& expected[read++] == FloatBits(transform.m_Translate.y)
				&& expected[read++] == FloatBits(transform.m_Translate.z)
				&& expected[read] == FloatBits(transform.m_fScale);
		}

		bool SameLiveBoundBits(const std::array<UInt32, 4>& expected,
			const NiBound& bound)
		{
			return expected[0] == FloatBits(bound.m_kCenter.x)
				&& expected[1] == FloatBits(bound.m_kCenter.y)
				&& expected[2] == FloatBits(bound.m_kCenter.z)
				&& expected[3] == FloatBits(bound.m_fRadius);
		}

		bool SameProofKeyLive(const ClipProofCacheEntry& entry,
			const void* identity, const ClipFrameContext& context,
			const NiTransform& transform, const NiBound& bound,
			const TileVisibilityPropertyView& tile, bool allowViewport)
		{
			return entry.valid && entry.identity == identity
				&& entry.renderer == context.camera.renderer
				&& entry.cameraEpoch == context.cameraEpoch
				&& SameRect(entry.scissorRect, tile.scissorRect)
				&& entry.tileUsesScissor == tile.useScissorTest
				&& entry.allowViewport == allowViewport
				&& SameLiveTransformBits(entry.transformBits, transform)
				&& SameLiveBoundBits(entry.boundBits, bound);
		}

		ClipProofCacheEntry* PrepareClipProofCacheEntry(
			const void* identity, const ClipFrameContext& context,
			const NiTransform& transform, const NiBound& bound,
			const TileVisibilityPropertyView& tile, bool allowViewport,
			ClipTransformBuildResult& cacheResult,
			ClipProofResult& cachedProof, NativeFontVisibilityCull& cachedReason)
		{
			cachedProof = ClipProofResult::Unproven;
			cachedReason = NativeFontVisibilityCull::None;
			if (!identity || !context.cameraEpoch)
			{
				cacheResult = ClipTransformBuildResult::IdentityMiss;
				return nullptr;
			}
			ClipProofCacheSet& set = s_clipProofCache[
				HashClipTransformIdentity(identity)
					& (kClipProofCacheSetCount - 1u)];
			ClipProofCacheEntry* identityEntry = nullptr;
			ClipProofCacheEntry* invalidEntry = nullptr;
			for (ClipProofCacheEntry& candidate : set.ways)
			{
				if (!candidate.valid)
				{
					if (!invalidEntry)
						invalidEntry = &candidate;
					continue;
				}
				if (candidate.identity != identity)
					continue;
				identityEntry = &candidate;
				if (SameProofKeyLive(candidate, identity, context,
						transform, bound, tile, allowViewport))
				{
					cacheResult = ClipTransformBuildResult::Reused;
					cachedProof = candidate.result;
					cachedReason = candidate.reason;
					return &candidate;
				}
				break;
			}
			cacheResult = identityEntry
				? ClipTransformBuildResult::KeyMiss
				: ClipTransformBuildResult::IdentityMiss;
			if (identityEntry)
				return identityEntry;
			if (invalidEntry)
				return invalidEntry;
			ClipProofCacheEntry* replacement =
				&set.ways[set.replacementWay];
			set.replacementWay = (set.replacementWay + 1u)
				% kClipProofCacheWays;
			return replacement;
		}

		void StoreClipProof(ClipProofCacheEntry* entry,
			const void* identity, const ClipFrameContext& context,
			const std::array<UInt32, 13>& transformBits,
			const std::array<UInt32, 4>& boundBits,
			const TileVisibilityPropertyView& tile, bool allowViewport,
			ClipProofResult result, NativeFontVisibilityCull reason)
		{
			if (!entry || !context.cameraEpoch
				|| result == ClipProofResult::Unproven)
			{
				return;
			}
			entry->identity = identity;
			entry->renderer = context.camera.renderer;
			entry->transformBits = transformBits;
			entry->boundBits = boundBits;
			entry->scissorRect = tile.scissorRect;
			entry->cameraEpoch = context.cameraEpoch;
			entry->reason = reason;
			entry->result = result;
			entry->tileUsesScissor = tile.useScissorTest;
			entry->allowViewport = allowViewport;
			entry->valid = true;
		}

		bool IsClipFrameCameraCurrent(const ClipFrameContext& context)
		{
			if (!context.active || !context.valid
				|| NiDX9Renderer::GetSingleton() != context.camera.renderer)
			{
				return false;
			}
			const NiDX9Renderer& renderer = *context.camera.renderer;
			const NiPoint3 positionAdjust =
				*reinterpret_cast<const NiPoint3*>(
					kBSGraphics_CameraWorldTranslate);
			return std::memcmp(&renderer.m_kD3DView, &context.camera.view,
					sizeof(context.camera.view)) == 0
				&& std::memcmp(&renderer.m_kD3DProj,
					&context.camera.projection,
					sizeof(context.camera.projection)) == 0
				&& std::memcmp(&renderer.m_kD3DPort,
					&context.camera.viewport,
					sizeof(context.camera.viewport)) == 0
				&& FloatBits(positionAdjust.x)
					== FloatBits(context.camera.positionAdjust.x)
				&& FloatBits(positionAdjust.y)
					== FloatBits(context.camera.positionAdjust.y)
				&& FloatBits(positionAdjust.z)
					== FloatBits(context.camera.positionAdjust.z)
				&& IsPrimitiveClippingEnabled(renderer)
					== context.camera.primitiveClipping
				&& (*reinterpret_cast<const UInt8*>(
					kBSShaderManager_bLetterbox) != 0)
					== context.camera.scaledScissor;
		}

		ClipProofResult EvaluateBoundClip(const NiBound& bound,
			const void* transformIdentity, const NiTransform& transform,
			const TileVisibilityPropertyView& tile,
			ClipFrameContext& context, bool allowViewport,
			NativeFontVisibilityCull& reason,
			ClipTransformBuildResult* transformBuildResult,
			NativeFontVisibilityProofWitness* proofWitness)
		{
			reason = NativeFontVisibilityCull::None;
			if (proofWitness)
				*proofWitness = {};
			if (transformBuildResult)
			{
				*transformBuildResult =
					ClipTransformBuildResult::Unavailable;
			}
			auto failOpen = [&]()
			{
				return ClipProofResult::Unproven;
			};

			if (!context.valid)
				return failOpen();
			RECT clipRect = {};
			const D3DVIEWPORT9& viewport = context.camera.viewport;
			const bool useScissor = tile.useScissorTest
				&& !context.camera.scaledScissor
				&& IsValidScissorForViewport(tile.scissorRect, viewport);
			if (useScissor)
			{
				clipRect = tile.scissorRect;
				reason = NativeFontVisibilityCull::Scissor;
			}
			else
			{
				// D3D9 clips the post-projection x/y volume to [-w,+w].  Only
				// use that larger, state-independent target when the structural
				// fast paths are enabled and Gamebryo's exact render-state mirror
				// proves primitive clipping is currently active.  A scaled or
				// malformed Tile scissor therefore falls back to the full viewport,
				// never to an approximation of the narrower rectangle.
				if (!allowViewport || !context.camera.primitiveClipping)
				{
					return failOpen();
				}
				clipRect = context.viewportRect;
				reason = NativeFontVisibilityCull::Clip;
			}

			if (!std::isfinite(bound.m_kCenter.x)
				|| !std::isfinite(bound.m_kCenter.y)
				|| !std::isfinite(bound.m_kCenter.z)
				|| !std::isfinite(bound.m_fRadius)
				|| bound.m_fRadius < 0.0f)
			{
				return failOpen();
			}

			ClipProofResult cachedProof = ClipProofResult::Unproven;
			NativeFontVisibilityCull cachedReason =
				NativeFontVisibilityCull::None;
			ClipTransformBuildResult buildResult =
				ClipTransformBuildResult::Unavailable;
			ClipProofCacheEntry* cacheEntry = PrepareClipProofCacheEntry(
				transformIdentity, context, transform, bound, tile,
				allowViewport, buildResult, cachedProof, cachedReason);
			if (cachedProof != ClipProofResult::Unproven)
			{
				reason = cachedReason;
				CaptureVisibilityProofWitness(proofWitness, context,
					cacheEntry->transformBits, cacheEntry->boundBits,
					tile, cachedProof, cachedReason);
				if (transformBuildResult)
					*transformBuildResult = buildResult;
				return cachedProof;
			}
			// Only a miss needs an owned key for cache publication and the
			// dispatch-time witness. Exact hits above compare the live fields
			// directly and reuse the already-certified cache arrays.
			const std::array<UInt32, 13> transformBits =
				CaptureTransformBits(transform);
			const std::array<UInt32, 4> boundBits = CaptureBoundBits(bound);

			ClipNdcBounds ndc;
			if (!ResolveClipNdcBounds(context, clipRect, useScissor, ndc))
				return failOpen();

			ClipColumn clipX;
			ClipColumn clipY;
			const bool vanillaUiOrthographic =
				BuildVanillaUiOrthographicColumns(
					context, transform, clipX, clipY);
			bool outside = false;
			if (vanillaUiOrthographic)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					VisibilityPreflightClipVanillaUiOrthographicTranslation);
				outside = CubeIsOutsideOrthographicAxisInterval(
						clipX, ndc.left, ndc.right, bound)
					|| CubeIsOutsideOrthographicAxisInterval(
						clipY, ndc.bottom, ndc.top, bound);
			}
			else
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					VisibilityPreflightClipGenericTransform);
				D3DXMATRIX world = {};
				if (!BuildRetailTileWorldMatrix(transform,
						context.camera.positionAdjust, world))
				{
					if (transformBuildResult)
					{
						*transformBuildResult =
							ClipTransformBuildResult::Unavailable;
					}
					return failOpen();
				}

				// WorldViewProjTranspose is predefined mapping 23 in the retail
				// constant map at E85D10. It calls D3DXMatrixMultiply twice in this
				// exact association order, then transposes for VS c0-c3. Use the same
				// D3DX entry point and retain the non-transposed matrix for row-vector
				// homogeneous half-space evaluation below.
				D3DXMATRIX worldView = {};
				D3DXMATRIX worldViewProjection = {};
				D3DXMatrixMultiply(&worldView, &world, &context.camera.view);
				D3DXMatrixMultiply(&worldViewProjection,
					&worldView, &context.camera.projection);
				if (!IsFiniteMatrix(worldViewProjection))
				{
					buildResult = ClipTransformBuildResult::Unavailable;
					if (transformBuildResult)
						*transformBuildResult = buildResult;
					return failOpen();
				}
				clipX = GetClipColumn(worldViewProjection, 0);
				clipY = GetClipColumn(worldViewProjection, 1);
				const ClipColumn clipW = GetClipColumn(
					worldViewProjection, 3);
				const double radius = bound.m_fRadius;
				const double centerW = EvaluateColumn(
					clipW, bound.m_kCenter);
				const double extentW = CubeExtent(clipW, radius);
				const double wSlack = (ColumnEvaluationMagnitude(
					clipW, bound.m_kCenter) + extentW + 1.0)
					* kClipIntervalRelativeSlack;
				// Perspective division is monotonic over the complete conservative
				// cube only when every point is strictly in front of w=0.
				if (centerW - extentW <= wSlack)
					return failOpen();

				// Each expression is non-negative inside the expanded scissor. The
				// payload sphere is expanded to a cube; only a negative maximum for
				// the whole cube proves that every glyph triangle is outside.
				const ClipColumn leftPlane = SubtractScaled(
					clipX, clipW, ndc.left);
				const ClipColumn rightPlane = SubtractScaled(
					ClipColumn{}, SubtractScaled(
						clipX, clipW, ndc.right), 1.0);
				const ClipColumn topPlane = SubtractScaled(
					ClipColumn{}, SubtractScaled(
						clipY, clipW, ndc.top), 1.0);
				const ClipColumn bottomPlane = SubtractScaled(
					clipY, clipW, ndc.bottom);
				outside = CubeIsOutsidePlane(
						leftPlane, bound, kClipIntervalRelativeSlack)
					|| CubeIsOutsidePlane(
						rightPlane, bound, kClipIntervalRelativeSlack)
					|| CubeIsOutsidePlane(
						topPlane, bound, kClipIntervalRelativeSlack)
					|| CubeIsOutsidePlane(
						bottomPlane, bound, kClipIntervalRelativeSlack);
			}
			if (transformBuildResult)
				*transformBuildResult = buildResult;
			const ClipProofResult result = outside
				? ClipProofResult::Outside : ClipProofResult::Overlap;
			StoreClipProof(cacheEntry, transformIdentity, context,
				transformBits, boundBits, tile, allowViewport, result, reason);
			CaptureVisibilityProofWitness(proofWitness, context,
				transformBits, boundBits, tile, result, reason);
			return result;
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

	namespace
	{
		NativeFontVisibilityCull EvaluateSubmissionVisibilityResolved(
			const NiTriShape* facade,
			const TileVisibilityPropertyView* tile,
			bool* alphaPropertyLookupElided)
		{
			RecordFreeTypePerf(FreeTypePerfCounter::VisibilityCheck);
			if (alphaPropertyLookupElided)
				*alphaPropertyLookupElided = false;
			if (!facade || !tile)
				return NativeFontVisibilityCull::None;

			const NiMaterialProperty* material = facade->GetMaterialProperty();
			const float materialAlpha = material ? material->m_fAlpha : 1.0f;
			if (!std::isfinite(tile->tileAlpha)
				|| !std::isfinite(materialAlpha))
			{
				return NativeFontVisibilityCull::None;
			}
			// AlphaProperty can only authorize a zero-alpha no-op after one of
			// the two effective alpha inputs is exactly zero. Test that cheap,
			// already-resolved state first; the old order queried the property
			// for every ordinary opaque/non-zero TileText submission.
			if (tile->tileAlpha != 0.0f && materialAlpha != 0.0f)
			{
				if (alphaPropertyLookupElided)
					*alphaPropertyLookupElided = true;
				return NativeFontVisibilityCull::None;
			}
			return IsZeroAlphaNoOpBlend(facade->GetAlphaProperty())
				? NativeFontVisibilityCull::ZeroAlpha
				: NativeFontVisibilityCull::None;
		}
	}

	NativeFontVisibilityCull EvaluateNativeFontSubmissionVisibility(
		const NiTriShape* facade)
	{
		const TileVisibilityPropertyView* tile = GetTileProperty(facade);
		return EvaluateSubmissionVisibilityResolved(facade, tile, nullptr);
	}

	NativeFontVisibilityCull EvaluateNativeFontSubmissionVisibility(
		const NiTriShape* facade, const NativeFontShapePayload& payload)
	{
		(void)payload;
		return EvaluateNativeFontSubmissionVisibility(facade);
	}

	void BeginNativeFontVisibilityFrame()
	{
		EndNativeFontVisibilityFrame();
		if (!g_bEnableFreeTypeFontPreflightClipCull)
			return;
		CaptureClipFrameContext(s_clipFrameContext, true);
		s_clipFrameContext.preflightOpen = true;
	}

	void CompleteNativeFontVisibilityPreflight()
	{
		RecordFreeTypePerf(FreeTypePerfCounter::
			VisibilitySortedTilePropertyLookupElided,
			s_clipFrameContext.sortedTilePropertyLookupsElided);
		RecordFreeTypePerf(FreeTypePerfCounter::
			VisibilitySortedAlphaPropertyLookupElided,
			s_clipFrameContext.sortedAlphaPropertyLookupsElided);
		s_clipFrameContext.preflightOpen = false;
	}

	void EndNativeFontVisibilityFrame()
	{
		s_clipFrameContext.active = false;
		s_clipFrameContext.valid = false;
		s_clipFrameContext.frameToken = 0;
		s_clipFrameContext.preflightOpen = false;
		s_clipFrameContext.sortedTilePropertyLookupsElided = 0;
		s_clipFrameContext.sortedAlphaPropertyLookupsElided = 0;
	}

	static void EvaluatePreflightClipVisibilityResolvedInPlace(
		const NiTriShape* facade, const NiTriShapeData* data,
		const TileVisibilityPropertyView* tile,
		NativeFontVisibilityPreflight& visibility)
	{
		if (!g_bEnableFreeTypeFontPreflightClipCull)
			return;
		if (s_clipFrameContext.active)
			visibility.frameToken = s_clipFrameContext.frameToken;
		RecordFreeTypePerf(
			FreeTypePerfCounter::VisibilityPreflightClipCheck);
		ClipTransformBuildResult transformBuildResult =
			ClipTransformBuildResult::Unavailable;
		auto failOpen = [&]()
		{
			RecordFreeTypePerf(FreeTypePerfCounter::
				VisibilityPreflightClipFailOpen);
			RecordClipTransformBuildResult(transformBuildResult);
		};

		// Facades call this after every RegisterObject for the flush, once the
		// final model bound, world transform, viewport, and Tile scissor exist.
		// Vanilla-layout SDF shapes call it immediately before their vanilla geometry
		// draw. Anything uncertain fails open. A facade proof is additionally
		// revalidated by HonorNativeFontPreflightClipCull at dispatch before it is
		// honored; a vanilla-layout proof already consumes the live dispatch state.
		if (!facade || !data)
		{
			failOpen();
			return;
		}
		if (!tile)
		{
			failOpen();
			return;
		}
		ClipFrameContext localContext;
		ClipFrameContext* context = &s_clipFrameContext;
		if (!context->active
			|| (!context->preflightOpen
				&& !IsClipFrameCameraCurrent(*context)))
		{
			if (!CaptureClipFrameContext(localContext, false))
			{
				failOpen();
				return;
			}
			context = &localContext;
		}
		NativeFontVisibilityCull reason = NativeFontVisibilityCull::None;
		// This is the final vanilla-visible model bound. Facade payload vertices are
		// relative and apply geometryOrigin during replay; vanilla-layout vertices
		// are already engine-owned full geometry. Both representations publish
		// the same full bound before reaching this proof.
		const ClipProofResult proof = EvaluateBoundClip(data->m_kBound,
			facade, facade->m_kWorld, *tile, *context, true,
			reason, &transformBuildResult, &visibility.proofWitness);
		if (proof == ClipProofResult::Unproven)
		{
			failOpen();
			return;
		}
		RecordClipTransformBuildResult(transformBuildResult);
		visibility.status = proof == ClipProofResult::Outside
			? NativeFontVisibilityProofStatus::Outside
			: NativeFontVisibilityProofStatus::Overlap;
		if (proof != ClipProofResult::Outside)
			return;
		RecordFreeTypePerf(
			FreeTypePerfCounter::VisibilityPreflightClipCulled);
		RecordFreeTypePerf(reason == NativeFontVisibilityCull::Scissor
			? FreeTypePerfCounter::VisibilityPreflightClipScissor
			: FreeTypePerfCounter::VisibilityPreflightClipViewport);
		visibility.cull = reason;
	}

	void EvaluateNativeFontSortedVisibilityInPlace(
		const NiTriShape* facade, NativeFontVisibilityPreflight& visibility)
	{
		// The sorted traversal owns a stable facade for this synchronous proof.
		// Resolve its Tile property once and pass the exact pointer through both
		// the alpha and clip/scissor gates.
		const TileVisibilityPropertyView* tile = GetTileProperty(facade);
		bool alphaPropertyLookupElided = false;
		visibility.cull = EvaluateSubmissionVisibilityResolved(
			facade, tile, &alphaPropertyLookupElided);
		if (s_clipFrameContext.active && alphaPropertyLookupElided)
			++s_clipFrameContext.sortedAlphaPropertyLookupsElided;
		if (visibility.cull != NativeFontVisibilityCull::None)
			return;

		const NiTriShapeData* data = facade ? facade->GetModelData() : nullptr;
		if (s_clipFrameContext.active && facade && data)
		{
			// The former clip-only call resolved this same Tile property again
			// after its model-data check, including the null-property case.
			++s_clipFrameContext.sortedTilePropertyLookupsElided;
		}
		EvaluatePreflightClipVisibilityResolvedInPlace(
			facade, data, tile, visibility);
	}

	void EvaluateNativeFontPreflightClipVisibilityInPlace(
		const NiTriShape* facade, NativeFontVisibilityPreflight& visibility)
	{
		const NiTriShapeData* data = facade ? facade->GetModelData() : nullptr;
		const TileVisibilityPropertyView* tile = data
			? GetTileProperty(facade) : nullptr;
		EvaluatePreflightClipVisibilityResolvedInPlace(
			facade, data, tile, visibility);
	}

	NativeFontVisibilityPreflight EvaluateNativeFontPreflightClipVisibility(
		const NiTriShape* facade)
	{
		NativeFontVisibilityPreflight visibility;
		EvaluateNativeFontPreflightClipVisibilityInPlace(facade, visibility);
		return visibility;
	}

	NativeFontVisibilityPreflight EvaluateNativeFontPreflightClipVisibility(
		const NiTriShape* facade, const NativeFontShapePayload& payload)
	{
		(void)payload;
		return EvaluateNativeFontPreflightClipVisibility(facade);
	}

	bool HonorNativeFontPreflightClipCull(const NiTriShape* facade,
		const NativeFontVisibilityPreflight& preflight)
	{
		FreeTypePerfScope honorGatePerf(
			FreeTypePerfPhase::PreflightClipHonorGate,
			ShouldSampleVisibilityHonorGate());
		auto reject = [](FreeTypePerfCounter reason)
		{
			RecordFreeTypePerf(reason);
			return false;
		};
		if (!g_bEnableFreeTypeFontPreflightClipCull)
		{
			return reject(FreeTypePerfCounter::
				VisibilityPreflightClipRevokeInvalid);
		}
		if (!facade
			|| preflight.status != NativeFontVisibilityProofStatus::Outside
			|| (preflight.cull != NativeFontVisibilityCull::Clip
				&& preflight.cull != NativeFontVisibilityCull::Scissor))
		{
			return reject(FreeTypePerfCounter::
				VisibilityPreflightClipRevokeInvalid);
		}
		const NativeFontVisibilityProofWitness& witness =
			preflight.proofWitness;
		if (!witness.valid)
		{
			return reject(FreeTypePerfCounter::
				VisibilityPreflightClipRevokeInvalid);
		}
		if (witness.status != preflight.status
			|| witness.cullReason != preflight.cull)
		{
			return reject(FreeTypePerfCounter::
				VisibilityPreflightClipRevokeProof);
		}
		if (!preflight.frameToken
			|| preflight.frameToken != s_clipFrameContext.frameToken)
		{
			return reject(FreeTypePerfCounter::
				VisibilityPreflightClipRevokeFrame);
		}
		if (!s_clipFrameContext.active || !s_clipFrameContext.valid
			|| witness.renderer != s_clipFrameContext.camera.renderer
			|| !witness.cameraEpoch
			|| witness.cameraEpoch != s_clipFrameContext.cameraEpoch
			|| !IsClipFrameCameraCurrent(s_clipFrameContext))
		{
			return reject(FreeTypePerfCounter::
				VisibilityPreflightClipRevokeCamera);
		}
		const NiTriShapeData* data = facade->GetModelData();
		const TileVisibilityPropertyView* tile = GetTileProperty(facade);
		if (!data || !tile)
		{
			return reject(FreeTypePerfCounter::
				VisibilityPreflightClipRevokeGeometry);
		}
		const std::array<UInt32, 13> currentTransformBits =
			CaptureTransformBits(facade->m_kWorld);
		if (currentTransformBits != witness.transformBits)
		{
			return reject(FreeTypePerfCounter::
				VisibilityPreflightClipRevokeTransform);
		}
		const std::array<UInt32, 4> currentBoundBits =
			CaptureBoundBits(data->m_kBound);
		if (currentBoundBits != witness.boundBits)
		{
			return reject(FreeTypePerfCounter::
				VisibilityPreflightClipRevokeBound);
		}
		if (tile->useScissorTest != witness.tileUsesScissor
			|| !SameRect(tile->scissorRect, witness.tileScissorRect))
		{
			return reject(FreeTypePerfCounter::
				VisibilityPreflightClipRevokeScissor);
		}
		return true;
	}

	bool ReuseNativeFontPreflightClipOverlap(
		const NativeFontVisibilityPreflight& preflight)
	{
		// A stale Overlap can only retain an otherwise GPU-clipped draw; it can
		// never suppress visible geometry. Restrict reuse to the owning sorted
		// frame, while Outside continues through the exact live honor gate above.
		return g_bEnableFreeTypeFontPreflightClipCull
			&& preflight.status == NativeFontVisibilityProofStatus::Overlap
			&& preflight.cull == NativeFontVisibilityCull::None
			&& preflight.frameToken
			&& preflight.frameToken == s_clipFrameContext.frameToken
			&& s_clipFrameContext.active;
	}

	bool IsNativeFontPayloadOutsideScissorForWorld(
		const NativeFontShapePayload& payload,
		const NiPropertyState* properties,
		const NiDX9Renderer* renderer,
		const NiTransform& effectiveWorld)
	{
		if (!properties || !renderer
			|| *reinterpret_cast<const UInt8*>(kBSShaderManager_bLetterbox))
		{
			return false;
		}
		const TileVisibilityPropertyView* tile = GetTileProperty(properties);
		if (!tile || !tile->useScissorTest)
			return false;
		NativeFontVisibilityCull reason = NativeFontVisibilityCull::None;
		if (!payload.payloadTemplate)
			return false;
		ClipFrameContext localContext;
		ClipFrameContext* context = nullptr;
		if (s_clipFrameContext.active && s_clipFrameContext.valid
			&& s_clipFrameContext.camera.renderer == renderer
			&& (s_clipFrameContext.preflightOpen
				|| IsClipFrameCameraCurrent(s_clipFrameContext)))
		{
			context = &s_clipFrameContext;
		}
		else if (CaptureClipFrameContext(localContext, false,
				const_cast<NiDX9Renderer*>(renderer)))
		{
			context = &localContext;
		}
		if (!context)
			return false;
		return EvaluateBoundClip(payload.payloadTemplate->bound, &payload,
				effectiveWorld, *tile, *context, false,
				reason, nullptr, nullptr) == ClipProofResult::Outside
			&& reason == NativeFontVisibilityCull::Scissor;
	}

	void RecordNativeFontVisibilityCull(NativeFontVisibilityCull reason)
	{
		if (reason == NativeFontVisibilityCull::None)
			return;
		RecordFreeTypePerf(FreeTypePerfCounter::VisibilityCulled);
		switch (reason)
		{
		case NativeFontVisibilityCull::AppCulled:
			RecordFreeTypePerf(FreeTypePerfCounter::VisibilityAppCulled);
			break;
		case NativeFontVisibilityCull::ZeroAlpha:
			RecordFreeTypePerf(FreeTypePerfCounter::VisibilityZeroAlpha);
			break;
		case NativeFontVisibilityCull::Clip:
			RecordFreeTypePerf(FreeTypePerfCounter::VisibilityClip);
			break;
		case NativeFontVisibilityCull::Scissor:
			RecordFreeTypePerf(FreeTypePerfCounter::VisibilityScissor);
			break;
		default:
			break;
		}
	}

	void RecordNativeFontVisibilityCull(NativeFontVisibilityCull reason,
		const NativeFontShapePayload& payload)
	{
		RecordNativeFontVisibilityCull(reason);
		if (!payload.payloadTemplate)
			return;
		const std::vector<NativeFontPacketTemplate>& packets =
			GetNativeFontPackets(*payload.payloadTemplate,
				payload.useCompositePackets);
		RecordFreeTypePerf(FreeTypePerfCounter::VisibilityPacketsSaved,
			static_cast<UInt64>(packets.size()));
		UInt64 vertices = 0;
		for (const NativeFontPacketTemplate& packet : packets)
			vertices += packet.vertexCount;
		RecordFreeTypePerf(FreeTypePerfCounter::VisibilityVerticesSaved,
			vertices);
	}
}

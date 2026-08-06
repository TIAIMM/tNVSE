#include "font_native_internal.h"

#include "BSShaderProperty.hpp"
#include "NiDX9Renderer.hpp"
#include "NiPropertyState.hpp"
#include "NiStencilProperty.hpp"
#include "NiTriShape.hpp"
#include "Utils/Memory.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace fonthook::vectorfont
{
	namespace implementation::font_native_tile_constants {}
	using namespace implementation::font_native_tile_constants;

	namespace implementation::font_native_tile_constants
	{
		inline constexpr UInt32 kScaledScissorActive = 0x11F9426;
		inline constexpr UInt32 kSetScissorTestEnable = 0xB984F0;
		inline constexpr UInt32 kSetScissorRectangle = 0xB97D20;
		inline constexpr UInt32 kSetStencilEnable = 0xB98070;
		inline constexpr UInt32 kSetStencilOperations = 0xB980C0;
		inline constexpr UInt32 kSetStencilFunctions = 0xB98180;
		inline constexpr UInt32 kRendererPositionAdjust = 0x11F474C;

		// Font::MakeTriShape supplies the retail TileShaderProperty concrete type,
		// which this CommonLib snapshot does not declare. Slot 31 reads only this
		// tail after publishing constants. Keep the cross-version proof local and
		// refuse the Lite path if the property identity is not exact.
		struct TileConstantsLitePropertyView : BSShaderProperty
		{
			std::array<UInt8, 0x08> beforeOverlay;
			NiColorA overlayColor;
			float tileAlpha = 1.0f;
			std::array<UInt8, 0x20> beforeScissor;
			RECT scissorRect = {};
			bool useScissorTest = false;
			std::array<UInt8, 3> tail;
		};

		static_assert(sizeof(BSShaderProperty) == 0x60);
		static_assert(sizeof(TileConstantsLitePropertyView) == 0xB0);
		static_assert(offsetof(
			TileConstantsLitePropertyView, overlayColor) == 0x68);
		static_assert(offsetof(
			TileConstantsLitePropertyView, tileAlpha) == 0x78);
		static_assert(offsetof(
			TileConstantsLitePropertyView, scissorRect) == 0x9C);
		static_assert(offsetof(
			TileConstantsLitePropertyView, useScissorTest) == 0xAC);

		struct TileConstantsTransientState
		{
			const TileConstantsLitePropertyView* tile = nullptr;
			const NiStencilProperty* stencil = nullptr;
			bool stencilEnabled = false;
			bool cleanupRequired = false;
		};

		bool ResolveTileConstantsTransientState(const NiTriShape* geometry,
			const NiPropertyState* properties,
			TileConstantsTransientState& state)
		{
			if (!geometry || !properties
				|| properties != &geometry->m_kProperties)
			{
				return false;
			}

			const NiShadeProperty* shade =
				properties->m_spShadeProperty.m_pObject;
			if (!shade || shade->m_eShaderType != NiShadeProperty::PROP_Tile)
				return false;
			state.tile = static_cast<const TileConstantsLitePropertyView*>(
				static_cast<const BSShaderProperty*>(shade));
			state.stencil = properties->m_spStencilProperty.m_pObject;
			state.stencilEnabled = state.stencil
				&& state.stencil->IsEnabled();
			state.cleanupRequired = state.tile->useScissorTest
				|| state.stencilEnabled;
			return true;
		}

		bool UsesScaledScissor(const TileConstantsTransientState& state)
		{
			return state.tile->useScissorTest
				&& *reinterpret_cast<const UInt8*>(kScaledScissorActive);
		}

		void ApplyTileConstantsTransientState(
			const TileConstantsTransientState& state)
		{
			if (state.tile->useScissorTest)
			{
				CdeclCall<void>(kSetScissorTestEnable, 1, 0);
				CdeclCall<void>(kSetScissorRectangle,
					state.tile->scissorRect.left,
					state.tile->scissorRect.top,
					state.tile->scissorRect.right,
					state.tile->scissorRect.bottom, 0);
			}
			if (state.stencilEnabled)
			{
				const UInt16 flags = state.stencil->m_usFlags.Get();
				CdeclCall<void>(kSetStencilEnable, 1, 0);
				CdeclCall<void>(kSetStencilOperations,
					(flags >> NiStencilProperty::FAILACTION_POS) & 7u,
					(flags >> NiStencilProperty::ZFAILACTION_POS) & 7u,
					(flags >> NiStencilProperty::PASSACTION_POS) & 7u, 0);
				CdeclCall<void>(kSetStencilFunctions,
					(flags & NiStencilProperty::TESTFUNC_MASK)
						>> NiStencilProperty::TESTFUNC_POS,
					state.stencil->m_uiRef, state.stencil->m_uiMask, 0);
			}
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

		bool BuildRetailWorldMatrix(const NiTransform& transform,
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

			// D3DXMATRIX has a user-provided default constructor, so `world = {}`
			// does not zero its float storage. Retail GetD3DFromNi (B71A40)
			// explicitly publishes the affine column as zero; leaving these three
			// entries untouched feeds stack garbage into W * View * Projection and
			// can stretch otherwise valid glyph quads across the render target.
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
					world.m[row][column] =
						rotation * transform.m_fScale;
				}
			}
			world.m[3][0] =
				transform.m_Translate.x - positionAdjust.x;
			world.m[3][1] =
				transform.m_Translate.y - positionAdjust.y;
			world.m[3][2] =
				transform.m_Translate.z - positionAdjust.z;
			world.m[3][3] = 1.0f;
			return IsFiniteMatrix(world);
		}

	}

	void ApplyNativeA8GeometryOrigin(NiTransform& destination,
		const NiTransform& source, const NiPoint3& origin)
	{
		destination = source;
		if (origin.x != 0.0f || origin.y != 0.0f || origin.z != 0.0f)
			destination.m_Translate = source * origin;
	}

	NativeTileConstantsLiteResult ApplyNativeTileConstantsLite(
		const NiTriShape* geometry, const NiPropertyState* properties)
	{
		TileConstantsTransientState state;
		if (!ResolveTileConstantsTransientState(
				geometry, properties, state)
			|| !state.cleanupRequired)
		{
			return NativeTileConstantsLiteResult::NotApplicable;
		}

		// Retail PC BCA980 has a resolution-dependent coordinate conversion while
		// this global is set. Its exact inputs are outside the retained command
		// proof, so preserve the complete stock slot rather than approximating it.
		if (UsesScaledScissor(state))
		{
			return NativeTileConstantsLiteResult::ScaledScissor;
		}

		// Official PC BCAAD7-BCAC48 and symbolized Xenon 82251F44-82251FC8
		// agree on this suffix. The final zero argument is the stock NOLOCK/no
		// counter-mark value. Calling the same render-state entry points retains
		// their software mirrors and slot-35 pairing exactly.
		ApplyTileConstantsTransientState(state);
		return NativeTileConstantsLiteResult::Applied;
	}

	NativeTileConstantsTranslationLiteResult
		ApplyNativeTileConstantsTranslationLite(
			const NiTriShape* geometry,
			const NiPropertyState* properties,
			NiDX9Renderer* renderer, IDirect3DDevice9* device)
	{
		TileConstantsTransientState transientState;
		if (!renderer || !device
			|| !ResolveTileConstantsTransientState(
				geometry, properties, transientState))
		{
			return NativeTileConstantsTranslationLiteResult::NotApplicable;
		}
		if (UsesScaledScissor(transientState))
		{
			return NativeTileConstantsTranslationLiteResult::ScaledScissor;
		}

		// Formal PC BCA980 -> E6FBB0 -> B71A40 and the symbolized Xenon
		// TileShader::SetupGeometryConstants -> SetModelTransform ->
		// GetD3DFromNi agree on this world layout. Rotation and scale are proved
		// unchanged by the caller. Rebuilding the complete 4x4 mirror is still
		// cheap, avoids inheriting stale padding/state, and is bitwise equivalent
		// to the retail conversion for finite inputs.
		D3DXMATRIX world = {};
		if (!BuildRetailWorldMatrix(geometry->m_kWorld, world)
			|| !IsFiniteMatrix(renderer->m_kD3DView)
			|| !IsFiniteMatrix(renderer->m_kD3DProj))
		{
			return NativeTileConstantsTranslationLiteResult::NonFinite;
		}

		// Tile's vertex constant map defines only WorldViewProjTranspose at
		// c0-c3 and TexScroll at c4. The exact retained key proves c4, PS c0,
		// model-camera vectors and normalize-normal state unchanged. Preserve the
		// retail (W * View) * Projection association order and publish only c0-c3.
		D3DXMATRIX worldView = {};
		D3DXMATRIX worldViewProjection = {};
		D3DXMATRIX worldViewProjectionTranspose = {};
		D3DXMatrixMultiply(
			&worldView, &world, &renderer->m_kD3DView);
		D3DXMatrixMultiply(&worldViewProjection,
			&worldView, &renderer->m_kD3DProj);
		D3DXMatrixTranspose(&worldViewProjectionTranspose,
			&worldViewProjection);
		if (!IsFiniteMatrix(worldViewProjectionTranspose))
		{
			return NativeTileConstantsTranslationLiteResult::NonFinite;
		}

		std::memcpy(&renderer->m_kD3DMat, &world, sizeof(world));
		const HRESULT result = device->SetVertexShaderConstantF(
			0, &worldViewProjectionTranspose._11, 4);
		if (FAILED(result))
		{
			return NativeTileConstantsTranslationLiteResult::DeviceFailure;
		}
		if (transientState.cleanupRequired)
		{
			ApplyTileConstantsTransientState(transientState);
			return NativeTileConstantsTranslationLiteResult::
				AppliedTransient;
		}
		return NativeTileConstantsTranslationLiteResult::Applied;
	}
}

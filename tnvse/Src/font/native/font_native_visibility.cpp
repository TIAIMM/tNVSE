#include "font_native_internal.h"

#include "NiAlphaProperty.hpp"
#include "NiMaterialProperty.hpp"
#include "NiTexturingProperty.hpp"

namespace fonthook::vectorfont
{
	namespace
	{
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
		if (IsZeroAlphaNoOpBlend(facade->GetAlphaProperty())
			&& (tile->tileAlpha == 0.0f || materialAlpha == 0.0f))
		{
			return NativeA8VisibilityCull::ZeroAlpha;
		}
		return NativeA8VisibilityCull::None;
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

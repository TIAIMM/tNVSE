#include "font_native_shape_hooks_detail.h"
#include "font_native_shape_standard_lite_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_native_shape_hooks {}
	using namespace implementation::font_native_shape_hooks;

	namespace implementation::font_native_shape_hooks
	{
		std::unique_ptr<NativeFontMetadataHotSets>& MetadataHotSets()
		{
			thread_local std::unique_ptr<NativeFontMetadataHotSets> sets;
			return sets;
		}

		NativePassConstantBatch& ConstantOwnershipBatch()
		{
			thread_local NativePassConstantBatch batch;
			return batch;
		}

		NativeDirectImmediateContext*& DirectImmediateContext()
		{
			thread_local NativeDirectImmediateContext* context = nullptr;
			return context;
		}

		bool ReleaseNativeConstantOwnershipBatch(const char* phase)
		{
			if (ConstantOwnershipBatch().Release())
				return true;
			MarkNativeFontGenerationFault(ConstantOwnershipBatch().Generation(),
				ConstantOwnershipBatch().Operation(),
				ConstantOwnershipBatch().Result());
			gLog.FormattedMessage(
				"tnvse_freetype_native: pass-constant ownership release fault phase=%s operation=%s hr=0x%08X register=%d generation=%u",
				phase ? phase : "unknown",
				ConstantOwnershipBatch().Operation(),
				static_cast<UInt32>(ConstantOwnershipBatch().Result()),
				ConstantOwnershipBatch().MismatchRegister(),
				ConstantOwnershipBatch().Generation());
			return false;
		}

		bool ValidateNativeImmediateCommand(
			const NativeDirectImmediateContext& context,
			NiTriShape* shape, NiRenderer* renderer)
		{
			if (context.packetStatePrevalidated)
			{
				switch (context.commandKind)
				{
				case NativeImmediateCommandKind::SpanPacket:
					return context.commandOffset
							!= kInvalidNativeFontCommandIndex
						&& GuardNativeFontCommand(
							context.commandSpanIndex,
							context.commandOffset, shape, renderer);
				case NativeImmediateCommandKind::SinglePacket:
					return GuardNativeFontSinglePacketCommand(
						context.commandSpanIndex, shape, renderer);
				case NativeImmediateCommandKind::DirectFacadeSinglePacket:
					return GuardNativeFontDirectFacadeSinglePacketCommand(
						context.commandSpanIndex, shape, renderer);
				default:
					return true;
				}
			}
			switch (context.commandKind)
			{
			case NativeImmediateCommandKind::SpanPacket:
				return context.commandOffset
						!= kInvalidNativeFontCommandIndex
					&& ValidateNativeFontCommand(
						context.commandSpanIndex,
						context.commandOffset, shape, renderer);
			case NativeImmediateCommandKind::SinglePacket:
				return ValidateNativeFontSinglePacketCommand(
					context.commandSpanIndex, shape, renderer);
			case NativeImmediateCommandKind::DirectFacadeSinglePacket:
				return ValidateNativeFontDirectFacadeSinglePacketCommand(
					context.commandSpanIndex, shape, renderer);
			default:
				return true;
			}
		}


		void ApplyNativeGeometryOrigin(NiTransform& destination,
			const NiTransform& source, const NiPoint3& origin)
		{
			destination = source;
			if (origin.x != 0.0f || origin.y != 0.0f || origin.z != 0.0f)
				destination.m_Translate = source * origin;
		}

		DirectTileShaderPropertyView* GetDirectTileProperty(
			NiTriShape* shape)
		{
			NiShadeProperty* shade =
				shape ? shape->GetShadeProperty() : nullptr;
			return shade
					&& shade->m_eShaderType == NiShadeProperty::PROP_Tile
				? reinterpret_cast<DirectTileShaderPropertyView*>(shade)
				: nullptr;
		}

		void RecordGpuEnvelopeVanillaCull(NativeFontVisibilityCull cull)
		{
			switch (cull)
			{
			case NativeFontVisibilityCull::AppCulled:
			case NativeFontVisibilityCull::ZeroAlpha:
			case NativeFontVisibilityCull::Clip:
			case NativeFontVisibilityCull::Scissor:
				RecordFreeTypeGpuEnvelopeVanillaCull();
				break;
			default:
				break;
			}
		}
	}

	void BeginNativeFontSortedTileConstantOwnership()
	{
		InvalidateSegmentDeviceStateCache();
		ConstantOwnershipBatch().BeginFrame();
	}

	void EndNativeFontSortedTileConstantOwnership()
	{
		InvalidateSegmentDeviceStateCache();
		ReleaseNativeConstantOwnershipBatch("sorted-frame-end");
		ConstantOwnershipBatch().EndFrame();
	}
}

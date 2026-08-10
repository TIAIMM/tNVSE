#include "font_native_shape_hooks_detail.h"
#include "font_native_shape_standard_lite_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_native_shape_hooks {}
	using namespace implementation::font_native_shape_hooks;

	namespace implementation::font_native_shape_hooks
	{
		void AddVanillaLayoutBindingFailure(UInt64& failures,
			VanillaLayoutBindingFailure failure)
		{
			failures |= static_cast<UInt64>(failure);
		}

		void RecordNativeDirectDrawLiteFallback(
			NativeDirectDrawLiteFallback failure)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::NativeDirectDrawLiteFallback);
			switch (failure)
			{
			case NativeDirectDrawLiteFallback::Renderer:
				RecordFreeTypePerf(FreeTypePerfCounter::
					NativeDirectDrawLiteFallbackRenderer);
				break;
			case NativeDirectDrawLiteFallback::Geometry:
				RecordFreeTypePerf(FreeTypePerfCounter::
					NativeDirectDrawLiteFallbackGeometry);
				break;
			case NativeDirectDrawLiteFallback::Binding:
				RecordFreeTypePerf(FreeTypePerfCounter::
					NativeDirectDrawLiteFallbackBinding);
				break;
			case NativeDirectDrawLiteFallback::Declaration:
				RecordFreeTypePerf(FreeTypePerfCounter::
					NativeDirectDrawLiteFallbackDeclaration);
				break;
			case NativeDirectDrawLiteFallback::None:
			case NativeDirectDrawLiteFallback::Program:
			default:
				RecordFreeTypePerf(FreeTypePerfCounter::
					NativeDirectDrawLiteFallbackProgram);
				break;
			}
		}

		NativeDirectDrawLiteFallback BuildNativeDirectDrawLiteSubmission(
			NiTriShape* geometry, NiDX9Renderer* renderer,
			const NiPropertyState* properties,
			const NativeFontCompiledPacketCommand& program,
			const NativeFontDrawCommand& command,
			NiGeometryBufferData* buffer,
			NativeSegmentDeviceStateCache* deviceState,
			NativeDirectDrawLiteSubmission& submission)
		{
			submission = {};
			TileShader* shader = program.shader;
			void** geometryVtable = geometry
				? *reinterpret_cast<void***>(geometry) : nullptr;
			void** shaderVtable = shader
				? *reinterpret_cast<void***>(shader) : nullptr;
			if (!program.directDrawLiteReady || !geometry || !shader
				|| !shaderVtable || shaderVtable != program.shaderVtable
				|| command.program != &program
				|| State().originalRenderImmediateAlt
					!= reinterpret_cast<RenderImmediateFn>(
						kNiTriShapeOnlyRenderImmediate))
			{
				return NativeDirectDrawLiteFallback::Program;
			}

			if (!renderer || !program.device
				|| program.device != renderer->GetD3DDevice()
				|| shader->m_pkD3DDevice != program.device
				|| shader->m_pkD3DRenderer != renderer
				|| !shader->m_pkD3DRenderState
				|| shader->m_pkD3DRenderState != renderer->m_pkRenderState
				|| !renderer->GetInsideFrameState()
				|| !renderer->m_bRenderTargetGroupActive
				|| renderer->m_bDeviceLost)
			{
				return NativeDirectDrawLiteFallback::Renderer;
			}

			NiTriShapeData* data = geometry->GetModelData();
			if (!geometryVtable
				|| !IsNativeFontAtlasShape(geometry)
				|| geometryVtable[kGeometrySegmentedPredicateSlot]
					!= reinterpret_cast<void*>(kNiObjectNullGeometryCastPredicate)
				|| geometryVtable[kGeometryResizablePredicateSlot]
					!= reinterpret_cast<void*>(kNiObjectNullGeometryCastPredicate)
				|| geometry->GetSkinInstance() || geometry->GetControllers()
				|| geometry->GetShader() != shader || !data
				|| data->m_pkBuffData != buffer
				|| data->m_spAdditionalGeomData.m_pObject
				|| (data->m_usDirtyFlags
					& NiGeometryData::CONSISTENCY_MASK)
					!= NiGeometryData::STATIC
				|| !data->GetActiveVertexCount()
				|| properties != &geometry->m_kProperties
				|| renderer->m_pkCurrProp != properties
				|| renderer->m_pkCurrEffects
				|| shader->m_uiCurrentPass != 0)
			{
				return NativeDirectDrawLiteFallback::Geometry;
			}

			const NativeFontFramePacketBinding& expected = command.binding;
			NiVBChip* chip = buffer && buffer->m_uiStreamCount == 1
				&& buffer->m_ppkVBChip
				? buffer->m_ppkVBChip[0] : nullptr;
			const UInt32 vertexCount = expected.vertexCount;
			const UInt32 quadCount = vertexCount / 4u;
			const UInt32 triangleCount = quadCount * 2u;
			const UInt32 indexCount = quadCount * 6u;
			const UInt64 requiredVertexBytes =
				static_cast<UInt64>(vertexCount)
					* sizeof(NativeFontGpuVertex);
			const UInt64 requiredIndexBytes =
				static_cast<UInt64>(indexCount) * sizeof(UInt16);
			if (!expected.active || !expected.vertexBuffer
				|| !expected.indexBuffer || !expected.declaration
				|| !vertexCount || (vertexCount & 3u)
				|| !command.packet
				|| command.packet->vertexCount != vertexCount
				|| !buffer || buffer->m_uiFlags
				|| buffer->m_pkGeometryGroup || buffer->m_uiFVF
				|| buffer->m_hDeclaration != expected.declaration
				|| buffer->m_bSoftwareVP
				|| buffer->m_uiVertCount != vertexCount
				|| buffer->m_uiMaxVertCount != vertexCount
				|| buffer->m_uiStreamCount != 1
				|| !buffer->m_puiVertexStride
				|| buffer->m_puiVertexStride[0]
					!= sizeof(NativeFontGpuVertex)
				|| !chip || chip->m_uiIndex
				|| chip->m_pkVB != expected.vertexBuffer
				|| chip->m_uiOffset || chip->m_uiLockFlags
				|| chip->m_uiSize < requiredVertexBytes
				|| buffer->m_uiIndexCount != indexCount
				|| buffer->m_uiIBSize != expected.indexBytes
				|| requiredIndexBytes > buffer->m_uiIBSize
				|| buffer->m_pkIB != expected.indexBuffer
				|| buffer->m_uiBaseVertexIndex != expected.baseVertex
				|| buffer->m_eType != D3DPT_TRIANGLELIST
				|| buffer->m_uiTriCount != triangleCount
				|| buffer->m_uiMaxTriCount != triangleCount
				|| buffer->m_uiNumArrays != 1
				|| buffer->m_pusArrayLengths
				|| buffer->m_pusIndexArray)
			{
				return NativeDirectDrawLiteFallback::Binding;
			}

			NiD3DShaderDeclaration* shaderDeclaration =
				shader->m_spShaderDecl.m_pObject;
			if (!shaderDeclaration
				|| shaderDeclaration->GetD3DDeclaration()
					!= expected.declaration)
			{
				return NativeDirectDrawLiteFallback::Declaration;
			}

			submission.data = data;
			submission.renderState = shader->m_pkD3DRenderState;
			submission.device = program.device;
			submission.deviceState = deviceState;
			submission.binding.declaration = expected.declaration;
			submission.binding.vertexBuffer = expected.vertexBuffer;
			submission.binding.indexBuffer = expected.indexBuffer;
			submission.binding.streamOffset = 0;
			submission.binding.stride = sizeof(NativeFontGpuVertex);
			submission.baseVertex = expected.baseVertex;
			submission.vertexCount = vertexCount;
			submission.triangleCount = triangleCount;
			return NativeDirectDrawLiteFallback::None;
		}

		NativeDirectDrawLiteFallback
		BuildVanillaLayoutDirectDrawLiteSubmission(
			NiTriShape* geometry, NiDX9Renderer* renderer,
			const NativeFontCompiledPacketCommand& program,
			const NativeFontPacketTemplate& packet,
			const NativeFontVanillaLayoutDrawToken& drawToken,
			NativeDirectDrawLiteSubmission& submission,
			UInt64& bindingFailures)
		{
			// A draw-token hit already certifies the post-Precache 40/48-byte
			// vertex stream. Direct replay additionally needs the stock indexed
			// triangle-list envelope; prove every live VB/IB/declaration/range
			// field here before the Standard-lite prelude mutates renderer state.
			submission = {};
			bindingFailures = 0;
			TileShader* shader = program.shader;
			void** geometryVtable = geometry
				? *reinterpret_cast<void***>(geometry) : nullptr;
			void** shaderVtable = shader
				? *reinterpret_cast<void***>(shader) : nullptr;
			if (!program.directDrawLiteReady || !program.active
				|| !program.profile || !geometry || !shader
				|| !shaderVtable || shaderVtable != program.shaderVtable
				|| program.standardV2SlotProofs
					!= NativeFontCompiledPacketCommand::
						kStandardV2RequiredProofs
				|| drawToken.standardLiteProgramIdentity != &program
				|| drawToken.shaderIdentity != shader
				|| drawToken.generation != program.generation
				|| State().originalRenderImmediateAlt
					!= reinterpret_cast<RenderImmediateFn>(
						kNiTriShapeOnlyRenderImmediate))
			{
				return NativeDirectDrawLiteFallback::Program;
			}

			if (!renderer || !program.device
				|| program.device != renderer->GetD3DDevice()
				|| shader->m_pkD3DDevice != program.device
				|| shader->m_pkD3DRenderer != renderer
				|| !shader->m_pkD3DRenderState
				|| shader->m_pkD3DRenderState != renderer->m_pkRenderState
				|| !renderer->GetInsideFrameState()
				|| !renderer->m_bRenderTargetGroupActive
				|| renderer->m_bDeviceLost)
			{
				return NativeDirectDrawLiteFallback::Renderer;
			}

			NiTriShapeData* data = geometry->GetModelData();
			NiGeometryBufferData* buffer = data
				? data->m_pkBuffData : nullptr;
			if (!geometryVtable || !IsVanillaLayoutShape(geometry)
				|| geometryVtable[kGeometrySegmentedPredicateSlot]
					!= reinterpret_cast<void*>(
						kNiObjectNullGeometryCastPredicate)
				|| geometryVtable[kGeometryResizablePredicateSlot]
					!= reinterpret_cast<void*>(
						kNiObjectNullGeometryCastPredicate)
				|| geometry->GetSkinInstance() || geometry->GetControllers()
				|| geometry->GetShader() != shader || !data || !buffer
				|| data != drawToken.modelDataIdentity
				|| buffer != drawToken.bufferIdentity
				|| data->m_spAdditionalGeomData.m_pObject
				|| (data->m_usDirtyFlags
					& NiGeometryData::CONSISTENCY_MASK)
					!= NiGeometryData::STATIC
				|| !data->GetActiveVertexCount()
				|| data->GetActiveVertexCount() != packet.vertexCount)
			{
				return NativeDirectDrawLiteFallback::Geometry;
			}

			NiVBChip* chip = buffer->m_uiStreamCount == 1u
				&& buffer->m_ppkVBChip
				? buffer->m_ppkVBChip[0] : nullptr;
			const UInt32 vertexCount = packet.vertexCount;
			const UInt32 quadCount = vertexCount / 4u;
			const UInt32 triangleCount = quadCount * 2u;
			const UInt32 indexCount = quadCount * 6u;
			const UInt64 requiredVertexBytes =
				static_cast<UInt64>(vertexCount) * drawToken.stride;
			const UInt64 relativeVertexEnd =
				static_cast<UInt64>(drawToken.baseVertexIndex)
					* drawToken.stride + requiredVertexBytes;
			const UInt64 requiredIndexBytes =
				static_cast<UInt64>(indexCount) * sizeof(UInt16);
			auto markBindingFailure = [&bindingFailures](bool failed,
				VanillaLayoutBindingFailure failure) {
				if (failed)
					AddVanillaLayoutBindingFailure(bindingFailures, failure);
			};
			markBindingFailure(!drawToken.valid || !drawToken.payloadUploaded,
				VanillaLayoutBindingFailure::TokenState);
			markBindingFailure(!vertexCount || (vertexCount & 3u),
				VanillaLayoutBindingFailure::PacketVertexCount);
			markBindingFailure(drawToken.packetIdentity != &packet,
				VanillaLayoutBindingFailure::PacketIdentity);
			markBindingFailure(drawToken.dataVertexCount != vertexCount,
				VanillaLayoutBindingFailure::DataVertexCount);
			markBindingFailure(drawToken.streamCount != 1u || !drawToken.stride,
				VanillaLayoutBindingFailure::TokenStream);
			markBindingFailure(!buffer->m_hDeclaration
					|| buffer->m_hDeclaration
						!= drawToken.bufferDeclarationIdentity,
				VanillaLayoutBindingFailure::DeclarationIdentity);
			// Retail E71FE0 records packed color/UV/NBT state in m_uiFlags and
			// retains the owning NiGeometryGroup.  Those fields are deliberately
			// nonzero/non-null for a stock PrecacheGeometry buffer; require the
			// post-pack identities certified by the draw token instead of imposing
			// the zeroed synthetic-facade contract.
			markBindingFailure(buffer->m_uiFlags != drawToken.bufferFlags,
				VanillaLayoutBindingFailure::BufferFlags);
			markBindingFailure(buffer->m_pkGeometryGroup
					!= drawToken.geometryGroupIdentity,
				VanillaLayoutBindingFailure::GeometryGroup);
			markBindingFailure(buffer->m_uiFVF != 0u,
				VanillaLayoutBindingFailure::Fvf);
			markBindingFailure(buffer->m_bSoftwareVP,
				VanillaLayoutBindingFailure::SoftwareVertexProcessing);
			markBindingFailure(
				buffer->m_uiVertCount != drawToken.bufferVertexCount,
				VanillaLayoutBindingFailure::BufferVertexSnapshot);
			markBindingFailure(buffer->m_uiVertCount != vertexCount,
				VanillaLayoutBindingFailure::BufferVertexPacket);
			markBindingFailure(buffer->m_uiMaxVertCount < vertexCount,
				VanillaLayoutBindingFailure::BufferMaxVertices);
			markBindingFailure(
				buffer->m_uiStreamCount != drawToken.streamCount,
				VanillaLayoutBindingFailure::BufferStreamCount);
			markBindingFailure(!buffer->m_puiVertexStride,
				VanillaLayoutBindingFailure::StrideArray);
			if (buffer->m_puiVertexStride)
			{
				markBindingFailure(buffer->m_puiVertexStride
						!= drawToken.strideArrayIdentity,
					VanillaLayoutBindingFailure::StrideIdentity);
				markBindingFailure(buffer->m_puiVertexStride[0]
						!= drawToken.stride,
					VanillaLayoutBindingFailure::StrideValue);
			}
			markBindingFailure(!chip,
				VanillaLayoutBindingFailure::VertexChip);
			if (chip)
			{
				markBindingFailure(chip != drawToken.vertexChipIdentity,
					VanillaLayoutBindingFailure::VertexChipIdentity);
				markBindingFailure(chip->m_uiIndex != 0u,
					VanillaLayoutBindingFailure::VertexChipIndex);
				markBindingFailure(!chip->m_pkVB,
					VanillaLayoutBindingFailure::VertexBuffer);
				markBindingFailure(chip->m_pkVB
						&& chip->m_pkVB != drawToken.vertexBufferIdentity,
					VanillaLayoutBindingFailure::VertexBufferIdentity);
				markBindingFailure(
					chip->m_uiOffset != drawToken.vertexChipOffset,
					VanillaLayoutBindingFailure::VertexChipOffset);
				markBindingFailure(
					chip->m_uiSize != drawToken.vertexChipSize,
					VanillaLayoutBindingFailure::VertexChipSize);
				markBindingFailure(chip->m_uiLockFlags != 0u,
					VanillaLayoutBindingFailure::VertexChipLock);
				markBindingFailure(relativeVertexEnd > chip->m_uiSize,
					VanillaLayoutBindingFailure::VertexRange);
			}
			markBindingFailure(!buffer->m_pkIB
					|| buffer->m_pkIB != drawToken.indexBufferIdentity,
				VanillaLayoutBindingFailure::IndexBuffer);
			markBindingFailure(buffer->m_uiIndexCount != drawToken.indexCount
					|| buffer->m_uiIndexCount != indexCount,
				VanillaLayoutBindingFailure::IndexCount);
			markBindingFailure(buffer->m_uiIBSize
					!= drawToken.indexBufferSize
					|| requiredIndexBytes > buffer->m_uiIBSize,
				VanillaLayoutBindingFailure::IndexSize);
			markBindingFailure(buffer->m_uiBaseVertexIndex
					!= drawToken.baseVertexIndex,
				VanillaLayoutBindingFailure::BaseVertex);
			markBindingFailure(buffer->m_eType != D3DPT_TRIANGLELIST
					|| buffer->m_uiTriCount != triangleCount
					|| buffer->m_uiMaxTriCount < triangleCount,
				VanillaLayoutBindingFailure::PrimitiveTopology);
			// Formal E71FE0 stores the NiTriShape CPU triangle-list pointer in
			// m_pusIndexArray even after the D3D index buffer is ready.  E745A0
			// does not dereference it during the one-array indexed draw.  Preserve
			// that stock pointer as an identity witness; never null or own it.
			markBindingFailure(buffer->m_uiNumArrays != 1u
					|| buffer->m_uiNumArrays != drawToken.arrayCount
					|| buffer->m_pusArrayLengths
					|| buffer->m_pusArrayLengths
						!= drawToken.arrayLengthsIdentity
					|| buffer->m_pusIndexArray
						!= drawToken.indexArrayIdentity,
				VanillaLayoutBindingFailure::ArrayTopology);
			if (bindingFailures)
			{
				return NativeDirectDrawLiteFallback::Binding;
			}

			NiD3DShaderDeclaration* shaderDeclaration =
				shader->m_spShaderDecl.m_pObject;
			const void* liveShaderDeclaration = shaderDeclaration
				? shaderDeclaration->GetD3DDeclaration() : nullptr;
			const void* liveBufferDeclaration = buffer->m_hDeclaration;
			const bool currentDeclaration = liveShaderDeclaration
				&& liveBufferDeclaration == liveShaderDeclaration;
			const bool compatiblePreviousDeclaration =
				drawToken.priorGenerationDeclaration
				&& liveShaderDeclaration
					== drawToken.generationDeclarationIdentity
				&& liveBufferDeclaration
					== drawToken.bufferDeclarationIdentity
				&& liveBufferDeclaration != liveShaderDeclaration;
			// A same-device shader refresh publishes a new declaration object even
			// though its 40/48-byte vertex contract is identical. Retail static
			// packing has already retired the CPU color/UV sources, so forcing a
			// Purge/Precache migration would destroy the only authoritative packed
			// stream. The draw token instead migrates the shape to the current
			// program/generation while retaining the exact compatible declaration
			// owned by its resident buffer. Device-epoch and compatibility-set proof
			// were completed under the renderer lock before the token was published.
			if (!liveShaderDeclaration
				|| liveShaderDeclaration
					!= drawToken.generationDeclarationIdentity
				|| (!currentDeclaration
					&& !compatiblePreviousDeclaration))
			{
				return NativeDirectDrawLiteFallback::Declaration;
			}

			submission.data = data;
			submission.renderState = shader->m_pkD3DRenderState;
			submission.device = program.device;
			submission.binding.declaration =
				static_cast<IDirect3DVertexDeclaration9*>(
					buffer->m_hDeclaration);
			submission.binding.vertexBuffer = chip->m_pkVB;
			submission.binding.indexBuffer = buffer->m_pkIB;
			// Formal E812F0 binds every NiGeometryBufferData stream with byte
			// offset zero; the stock geometry-group location is already represented
			// by m_uiBaseVertexIndex in the indexed draw.
			submission.binding.streamOffset = 0;
			submission.binding.stride = drawToken.stride;
			submission.baseVertex = drawToken.baseVertexIndex;
			submission.vertexCount = vertexCount;
			submission.triangleCount = triangleCount;
			return NativeDirectDrawLiteFallback::None;
		}


		void ExecuteNativeDirectDrawLite(
			const NativeDirectDrawLiteSubmission& submission)
		{
			if (submission.successfulDrawWitness)
				*submission.successfulDrawWitness = false;
			NativeSegmentDeviceStateCache* deviceState =
				submission.deviceState;
			const bool bindingReady = deviceState
				&& deviceState->geometryBindingReady
				&& SameSegmentGeometryBinding(
					deviceState->geometryBinding, submission.binding);
			HRESULT streamResult = D3D_OK;
			HRESULT indexResult = D3D_OK;
			if (bindingReady)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					NativeDirectDrawLiteBindingReuse);
			}
			else
			{
				if (deviceState)
					deviceState->geometryBindingReady = false;
				submission.renderState->vSetDeclaration(
					submission.binding.declaration, false);
				streamResult = submission.device->SetStreamSource(
					0, submission.binding.vertexBuffer,
					submission.binding.streamOffset,
					submission.binding.stride);
				indexResult = submission.device->SetIndices(
					submission.binding.indexBuffer);
				RecordFreeTypePerf(FreeTypePerfCounter::
					NativeDirectDrawLiteBindingSet);
				if (FAILED(streamResult) || FAILED(indexResult))
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						NativeDirectDrawLiteBindingDeviceFailure);
					if (submission.successfulDrawWitness)
						return;
				}
				else if (deviceState)
				{
					deviceState->geometryBinding = submission.binding;
					deviceState->geometryBindingReady = true;
				}
			}

			const HRESULT drawResult = submission.device->DrawIndexedPrimitive(
				D3DPT_TRIANGLELIST,
				static_cast<INT>(submission.baseVertex), 0,
				submission.vertexCount, 0, submission.triangleCount);
			// Formal E745A0 clears the low dirty/revision bits after the indexed
			// loop even when the D3D call fails. Preserve that exact side effect.
			submission.data->m_usDirtyFlags &= 0xF000u;
			if (FAILED(drawResult))
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					NativeDirectDrawLiteDrawDeviceFailure);
				if (submission.successfulDrawWitness)
					return;
			}
			if (submission.successfulDrawWitness)
				*submission.successfulDrawWitness = true;
			RecordFreeTypePerf(
				FreeTypePerfCounter::NativeDirectDrawLiteReplay);
		}

	}

}

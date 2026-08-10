#include "font_native_shape_hooks_detail.h"
#include "font_native_shape_standard_lite_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_native_shape_hooks {}
	using namespace implementation::font_native_shape_hooks;

	namespace implementation::font_native_shape_hooks
	{
		enum class NativeDirectShapeBindingFailure : UInt8
		{
			None = 0,
			Input,
			Topology,
			Atlas,
			FacadeModelData,
			FacadeAlphaProperty,
			FacadeBufferData,
			FacadeTileProperty,
			FacadeStreamCount,
			FacadeVertexStride,
			FacadeVertexChipArray,
			FacadeVertexChip,
			Property,
			Texture,
			Shader,
		};

		class NativeDirectShapeBinding
		{
		public:
			NativeDirectShapeBinding(NiTriShape* shape,
				NativeFontShapePayload& payload,
				const NativeFontDirectShapeSubmission& submission)
				: m_shape(shape)
			{
				Initialize(payload, submission.vertexBuffer,
					submission.indexBuffer, submission.declaration,
					submission.baseVertex, submission.vertexCount,
					submission.indexBytes);
			}

			NativeDirectShapeBinding(NiTriShape* shape,
				NativeFontShapePayload& payload,
				const NativeFontFramePacketBinding& binding)
				: m_shape(shape)
			{
				if (!binding.active)
					return;
				Initialize(payload, binding.vertexBuffer,
					binding.indexBuffer, binding.declaration,
					binding.baseVertex, binding.vertexCount,
					binding.indexBytes);
			}

			NativeDirectShapeBinding(
				const NativeDirectShapeBinding&) = delete;
			NativeDirectShapeBinding& operator=(
				const NativeDirectShapeBinding&) = delete;

			~NativeDirectShapeBinding()
			{
				if (m_active)
					Restore();
			}

			bool Active() const
			{
				return m_active;
			}

			NativeDirectShapeBindingFailure Failure() const
			{
				return m_failure;
			}

			NiGeometryBufferData* Buffer() const
			{
				return m_active ? m_buffer : nullptr;
			}

		private:
			bool AttachSyntheticBuffer()
			{
				if (!m_data || m_data->m_pkBuffData
					|| m_syntheticBufferConstructed)
				{
					return false;
				}

				m_originalBuffer = m_data->m_pkBuffData;
				m_buffer = reinterpret_cast<NiGeometryBufferData*>(
					m_syntheticBufferStorage.data());
				ThisStdCall<void>(
					kNiGeometryBufferDataConstructor, m_buffer);
				m_syntheticBufferConstructed = true;
				std::memset(&m_syntheticChip, 0,
					sizeof(m_syntheticChip));
				m_syntheticChips[0] = &m_syntheticChip;
				m_syntheticStride = sizeof(NativeFontGpuVertex);
				m_buffer->m_uiStreamCount = 1;
				m_buffer->m_puiVertexStride =
					&m_syntheticStride;
				m_buffer->m_ppkVBChip = m_syntheticChips.data();
				m_buffer->m_eType = D3DPT_TRIANGLELIST;
				m_buffer->m_uiNumArrays = 1;
				m_buffer->m_pusArrayLengths = nullptr;
				m_buffer->m_pusIndexArray = nullptr;
				m_syntheticChip.m_uiIndex = 0;
				m_data->m_pkBuffData = m_buffer;
				if (m_data->m_pkBuffData != m_buffer)
				{
					DestroySyntheticBuffer();
					return false;
				}
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						SinglePacketDirectSyntheticBuffer);
				return true;
			}

			void DestroySyntheticBuffer()
			{
				if (!m_syntheticBufferConstructed || !m_buffer)
					return;
				if (m_data && m_data->m_pkBuffData == m_buffer)
					m_data->m_pkBuffData = m_originalBuffer;

				// E8F0F0 owns declaration, IB, stride, and chip arrays. This
				// stack-local descriptor borrows all of them, so detach every
				// owned field before invoking the non-deleting retail destructor.
				m_buffer->m_pkGeometryGroup = nullptr;
				m_buffer->m_hDeclaration = nullptr;
				m_buffer->m_uiStreamCount = 0;
				m_buffer->m_puiVertexStride = nullptr;
				m_buffer->m_ppkVBChip = nullptr;
				m_buffer->m_pkIB = nullptr;
				ThisStdCall<void>(
					kNiGeometryBufferDataDestructor, m_buffer);
				m_buffer = m_originalBuffer;
				m_chip = nullptr;
				m_syntheticBufferConstructed = false;
			}

			void Initialize(NativeFontShapePayload& payload,
				IDirect3DVertexBuffer9* vertexBuffer,
				IDirect3DIndexBuffer9* indexBuffer,
				IDirect3DVertexDeclaration9* declaration,
				UInt32 baseVertex, UInt32 vertexCount,
				UInt32 indexBytes)
			{
				m_failure = NativeDirectShapeBindingFailure::Input;
				if (!m_shape || m_shape->GetSkinInstance()
					|| !payload.payloadTemplate
					|| payload.packetShaders.size() != 1
					|| !payload.packetShaders[0]
					|| !vertexBuffer || !indexBuffer
					|| !declaration || !vertexCount || !indexBytes
					|| (vertexCount & 3u))
				{
					return;
				}
				const NativeFontPayloadTemplate& artifact =
					*payload.payloadTemplate;
				const std::vector<NativeFontPacketTemplate>& packets =
					GetNativeFontPackets(artifact,
						payload.useCompositePackets);
				if (packets.size() != 1
					|| packets[0].vertexCount != vertexCount)
				{
					m_failure =
						NativeDirectShapeBindingFailure::Topology;
					return;
				}
				const NativeFontPacketTemplate& packet = packets[0];
				const UInt64 vertexEnd =
					static_cast<UInt64>(packet.firstVertex)
						+ packet.vertexCount;
				if (vertexEnd > artifact.gpuVertices.size()
					|| packet.atlasPage
						>= artifact.atlasProperties.size()
					|| packet.atlasPage
						>= artifact.atlasTextures.size()
					|| !artifact.atlasProperties[packet.atlasPage]
					|| !artifact.atlasTextures[packet.atlasPage])
				{
					m_failure = NativeDirectShapeBindingFailure::Atlas;
					return;
				}

				m_data = m_shape->GetModelData();
				m_alpha = m_shape->GetAlphaProperty();
				m_buffer = m_data ? m_data->m_pkBuffData : nullptr;
				m_tile = GetDirectTileProperty(m_shape);
				if (!m_data)
				{
					m_failure =
						NativeDirectShapeBindingFailure::
							FacadeModelData;
					return;
				}
				if (!m_alpha)
				{
					m_failure =
						NativeDirectShapeBindingFailure::
							FacadeAlphaProperty;
					return;
				}
				if (!m_tile)
				{
					m_failure =
						NativeDirectShapeBindingFailure::
							FacadeTileProperty;
					return;
				}
				if (!m_buffer && !AttachSyntheticBuffer())
				{
					m_failure =
						NativeDirectShapeBindingFailure::
							FacadeBufferData;
					return;
				}
				if (!m_buffer->m_uiStreamCount)
				{
					m_failure =
						NativeDirectShapeBindingFailure::
							FacadeStreamCount;
					return;
				}
				if (!m_buffer->m_puiVertexStride)
				{
					m_failure =
						NativeDirectShapeBindingFailure::
							FacadeVertexStride;
					return;
				}
				if (!m_buffer->m_ppkVBChip)
				{
					m_failure =
						NativeDirectShapeBindingFailure::
							FacadeVertexChipArray;
					return;
				}
				if (!m_buffer->m_ppkVBChip[0])
				{
					m_failure =
						NativeDirectShapeBindingFailure::
							FacadeVertexChip;
					return;
				}
				m_chip = m_buffer->m_ppkVBChip[0];

				m_local = m_shape->m_kLocal;
				m_world = m_shape->m_kWorld;
				m_bound = m_data->m_kBound;
				m_shader = m_shape->GetShader();
				m_alphaFlags = m_alpha->m_usFlags;
				m_alphaTestRef = m_alpha->m_ucAlphaTestRef;
				m_atlasProperty = m_shape->GetTexturingProperty();
				m_atlasTexture = m_tile->sourceTexture;

				m_bufferFlags = m_buffer->m_uiFlags;
				m_geometryGroup = m_buffer->m_pkGeometryGroup;
				m_fvf = m_buffer->m_uiFVF;
				m_declaration = m_buffer->m_hDeclaration;
				m_softwareVertexProcessing =
					m_buffer->m_bSoftwareVP;
				m_vertexCount = m_buffer->m_uiVertCount;
				m_maxVertexCount = m_buffer->m_uiMaxVertCount;
				m_streamCount = m_buffer->m_uiStreamCount;
				m_stride = m_buffer->m_puiVertexStride[0];
				m_indexCount = m_buffer->m_uiIndexCount;
				m_indexBytes = m_buffer->m_uiIBSize;
				m_indexBuffer = m_buffer->m_pkIB;
				m_baseVertex = m_buffer->m_uiBaseVertexIndex;
				m_primitiveType = m_buffer->m_eType;
				m_triangleCount = m_buffer->m_uiTriCount;
				m_maxTriangleCount = m_buffer->m_uiMaxTriCount;
				m_arrayCount = m_buffer->m_uiNumArrays;
				m_arrayLengths = m_buffer->m_pusArrayLengths;
				m_indexArray = m_buffer->m_pusIndexArray;

				m_chipIndex = m_chip->m_uiIndex;
				m_vertexBuffer = m_chip->m_pkVB;
				m_chipOffset = m_chip->m_uiOffset;
				m_chipLockFlags = m_chip->m_uiLockFlags;
				m_chipSize = m_chip->m_uiSize;
				m_stateCaptured = true;

				NiTexturingProperty* desiredProperty =
					artifact.atlasProperties[
						packet.atlasPage].m_pObject;
				NiTexture* desiredTexture =
					artifact.atlasTextures[
						packet.atlasPage].m_pObject;
				if (m_atlasProperty.m_pObject != desiredProperty)
				{
					m_atlasPropertyChanged = true;
					m_shape->RemoveProperty(NiProperty::TEXTURING);
					m_shape->AddProperty(desiredProperty);
					m_shape->UpdateProperties();
					if (m_shape->GetTexturingProperty()
						!= desiredProperty)
					{
						m_failure =
							NativeDirectShapeBindingFailure::Property;
						Restore();
						return;
					}
				}
				if (m_atlasTexture.m_pObject != desiredTexture)
				{
					m_atlasTextureChanged = true;
					ThisStdCall<void>(kTileImageSetSourceTexture,
						m_tile, desiredTexture);
					if (m_tile->sourceTexture.m_pObject
						!= desiredTexture)
					{
						m_failure =
							NativeDirectShapeBindingFailure::Texture;
						Restore();
						return;
					}
				}

				const UInt32 quadCount = vertexCount / 4u;
				ApplyNativeGeometryOrigin(m_shape->m_kLocal, m_local,
					payload.geometryOrigin);
				ApplyNativeGeometryOrigin(m_shape->m_kWorld, m_world,
					payload.geometryOrigin);
				m_data->m_kBound = packet.bound;
				m_alpha->SetAlphaTesting(false);
				m_shape->SetShader(payload.packetShaders[0]);

				m_buffer->m_uiFlags = 0;
				m_buffer->m_pkGeometryGroup = nullptr;
				m_buffer->m_uiFVF = 0;
				m_buffer->m_hDeclaration = declaration;
				m_buffer->m_bSoftwareVP = false;
				m_buffer->m_uiVertCount = vertexCount;
				m_buffer->m_uiMaxVertCount = vertexCount;
				m_buffer->m_uiStreamCount = 1;
				m_buffer->m_puiVertexStride[0] =
					sizeof(NativeFontGpuVertex);
				m_buffer->m_uiIndexCount = quadCount * 6u;
				m_buffer->m_uiIBSize = indexBytes;
				m_buffer->m_pkIB = indexBuffer;
				m_buffer->m_uiBaseVertexIndex = baseVertex;
				m_buffer->m_eType = D3DPT_TRIANGLELIST;
				m_buffer->m_uiTriCount = quadCount * 2u;
				m_buffer->m_uiMaxTriCount = quadCount * 2u;
				m_buffer->m_uiNumArrays = 1;
				m_buffer->m_pusArrayLengths = nullptr;
				m_buffer->m_pusIndexArray = nullptr;

				m_chip->m_uiIndex = 0;
				m_chip->m_pkVB = vertexBuffer;
				m_chip->m_uiOffset = 0;
				m_chip->m_uiLockFlags = 0;
				m_chip->m_uiSize =
					vertexCount * sizeof(NativeFontGpuVertex);

				if (m_shape->GetShader() != payload.packetShaders[0])
				{
					m_failure =
						NativeDirectShapeBindingFailure::Shader;
					Restore();
					return;
				}
				m_failure = NativeDirectShapeBindingFailure::None;
				m_active = true;
			}

			void Restore()
			{
				if (!m_stateCaptured || !m_shape || !m_data
					|| !m_alpha || !m_buffer || !m_chip)
					return;
				if (m_atlasPropertyChanged)
				{
					m_shape->RemoveProperty(NiProperty::TEXTURING);
					if (m_atlasProperty)
						m_shape->AddProperty(m_atlasProperty.m_pObject);
					m_shape->UpdateProperties();
				}
				if (m_atlasTextureChanged && m_tile)
				{
					ThisStdCall<void>(kTileImageSetSourceTexture,
						m_tile, m_atlasTexture.m_pObject);
				}

				m_chip->m_uiIndex = m_chipIndex;
				m_chip->m_pkVB = m_vertexBuffer;
				m_chip->m_uiOffset = m_chipOffset;
				m_chip->m_uiLockFlags = m_chipLockFlags;
				m_chip->m_uiSize = m_chipSize;

				m_buffer->m_uiFlags = m_bufferFlags;
				m_buffer->m_pkGeometryGroup = m_geometryGroup;
				m_buffer->m_uiFVF = m_fvf;
				m_buffer->m_hDeclaration = m_declaration;
				m_buffer->m_bSoftwareVP =
					m_softwareVertexProcessing;
				m_buffer->m_uiVertCount = m_vertexCount;
				m_buffer->m_uiMaxVertCount = m_maxVertexCount;
				m_buffer->m_uiStreamCount = m_streamCount;
				m_buffer->m_puiVertexStride[0] = m_stride;
				m_buffer->m_uiIndexCount = m_indexCount;
				m_buffer->m_uiIBSize = m_indexBytes;
				m_buffer->m_pkIB = m_indexBuffer;
				m_buffer->m_uiBaseVertexIndex = m_baseVertex;
				m_buffer->m_eType = m_primitiveType;
				m_buffer->m_uiTriCount = m_triangleCount;
				m_buffer->m_uiMaxTriCount = m_maxTriangleCount;
				m_buffer->m_uiNumArrays = m_arrayCount;
				m_buffer->m_pusArrayLengths = m_arrayLengths;
				m_buffer->m_pusIndexArray = m_indexArray;

				m_shape->SetShader(m_shader);
				m_alpha->m_usFlags = m_alphaFlags;
				m_alpha->m_ucAlphaTestRef = m_alphaTestRef;
				m_data->m_kBound = m_bound;
				m_shape->m_kLocal = m_local;
				m_shape->m_kWorld = m_world;
				DestroySyntheticBuffer();
				m_active = false;
				m_stateCaptured = false;
			}

			NiTriShape* m_shape = nullptr;
			NiTriShapeData* m_data = nullptr;
			NiAlphaProperty* m_alpha = nullptr;
			DirectTileShaderPropertyView* m_tile = nullptr;
			NiGeometryBufferData* m_buffer = nullptr;
			NiVBChip* m_chip = nullptr;
			NiTransform m_local;
			NiTransform m_world;
			NiBound m_bound;
			BSShader* m_shader = nullptr;
			NiTexturingPropertyPtr m_atlasProperty;
			NiTexturePtr m_atlasTexture;
			Bitfield16 m_alphaFlags;
			UInt8 m_alphaTestRef = 0;
			UInt32 m_bufferFlags = 0;
			NiGeometryGroup* m_geometryGroup = nullptr;
			UInt32 m_fvf = 0;
			void* m_declaration = nullptr;
			bool m_softwareVertexProcessing = false;
			UInt32 m_vertexCount = 0;
			UInt32 m_maxVertexCount = 0;
			UInt32 m_streamCount = 0;
			UInt32 m_stride = 0;
			UInt32 m_indexCount = 0;
			UInt32 m_indexBytes = 0;
			IDirect3DIndexBuffer9* m_indexBuffer = nullptr;
			UInt32 m_baseVertex = 0;
			D3DPRIMITIVETYPE m_primitiveType = D3DPT_FORCE_DWORD;
			UInt32 m_triangleCount = 0;
			UInt32 m_maxTriangleCount = 0;
			UInt32 m_arrayCount = 0;
			const UInt16* m_arrayLengths = nullptr;
			const UInt16* m_indexArray = nullptr;
			UInt32 m_chipIndex = 0;
			IDirect3DVertexBuffer9* m_vertexBuffer = nullptr;
			UInt32 m_chipOffset = 0;
			UInt32 m_chipLockFlags = 0;
			UInt32 m_chipSize = 0;
			alignas(NiGeometryBufferData)
				std::array<UInt8, sizeof(NiGeometryBufferData)>
					m_syntheticBufferStorage = {};
			std::array<NiVBChip*, 1> m_syntheticChips = {};
			NiVBChip m_syntheticChip = {};
			UInt32 m_syntheticStride = 0;
			NiGeometryBufferData* m_originalBuffer = nullptr;
			bool m_atlasPropertyChanged = false;
			bool m_atlasTextureChanged = false;
			bool m_stateCaptured = false;
			NativeDirectShapeBindingFailure m_failure =
				NativeDirectShapeBindingFailure::Input;
			bool m_syntheticBufferConstructed = false;
			bool m_active = false;
		};

		bool SameNativePacketBinding(
			const NativeFontFramePacketBinding& command,
			const NativeFontDirectFacadePacketBinding& live)
		{
			return command.active && live.active
				&& command.vertexBuffer == live.vertexBuffer
				&& command.indexBuffer == live.indexBuffer
				&& command.declaration == live.declaration
				&& command.baseVertex == live.baseVertex
				&& command.vertexCount == live.vertexCount
				&& command.indexBytes == live.indexBytes
				&& command.generation == live.generation
				&& command.resourceSerial == live.resourceSerial
				&& command.uploadEpoch == live.uploadEpoch
				&& command.staticResident == live.staticResident;
		}

		FreeTypePerfCounter DirectShapeBindingFallbackCounter(
			NativeDirectShapeBindingFailure failure)
		{
			switch (failure)
			{
			case NativeDirectShapeBindingFailure::Topology:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackBindingTopology;
			case NativeDirectShapeBindingFailure::Atlas:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackBindingAtlas;
			case NativeDirectShapeBindingFailure::FacadeModelData:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeModelData;
			case NativeDirectShapeBindingFailure::FacadeAlphaProperty:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeAlphaProperty;
			case NativeDirectShapeBindingFailure::FacadeBufferData:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeBufferData;
			case NativeDirectShapeBindingFailure::FacadeTileProperty:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeTileProperty;
			case NativeDirectShapeBindingFailure::FacadeStreamCount:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeStreamCount;
			case NativeDirectShapeBindingFailure::FacadeVertexStride:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeVertexStride;
			case NativeDirectShapeBindingFailure::FacadeVertexChipArray:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeVertexChipArray;
			case NativeDirectShapeBindingFailure::FacadeVertexChip:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackFacadeVertexChip;
			case NativeDirectShapeBindingFailure::Property:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackBindingProperty;
			case NativeDirectShapeBindingFailure::Texture:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackBindingTexture;
			case NativeDirectShapeBindingFailure::Shader:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackBindingShader;
			case NativeDirectShapeBindingFailure::None:
			case NativeDirectShapeBindingFailure::Input:
			default:
				return FreeTypePerfCounter::
					SinglePacketDirectFallbackBindingInput;
			}
		}

		void RecordSinglePacketDirectFallback(
			FreeTypePerfCounter stage)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::SinglePacketDirectFallback);
			RecordFreeTypePerf(stage);
			switch (stage)
			{
			case FreeTypePerfCounter::
				SinglePacketDirectFallbackFacadeModelData:
			case FreeTypePerfCounter::
				SinglePacketDirectFallbackFacadeAlphaProperty:
			case FreeTypePerfCounter::
				SinglePacketDirectFallbackFacadeBufferData:
			case FreeTypePerfCounter::
				SinglePacketDirectFallbackFacadeTileProperty:
			case FreeTypePerfCounter::
				SinglePacketDirectFallbackFacadeStreamCount:
			case FreeTypePerfCounter::
				SinglePacketDirectFallbackFacadeVertexStride:
			case FreeTypePerfCounter::
				SinglePacketDirectFallbackFacadeVertexChipArray:
			case FreeTypePerfCounter::
				SinglePacketDirectFallbackFacadeVertexChip:
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						SinglePacketDirectFallbackBindingFacade);
				break;
			default:
				break;
			}
		}


		NativePacketDrawResult DrawNativePacketSet(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool setupRenderStates, NiTriShape* facade,
			NativeFontShapePayload& payload,
			UInt32 commandSpanIndex)
		{
			FreeTypePerfScope perf(
				FreeTypePerfPhase::Submit);
			FreeTypePerfScope commandPerf(
				FreeTypePerfPhase::CommandSubmit,
				commandSpanIndex != kInvalidNativeFontCommandIndex);
			InvalidateSegmentDeviceStateCache();
			NativePacketDrawResult draw;
			NativeRingSubmissionScope ringScope;
			NativeFontFallbackReason failure = BeginNativeFontRingSubmission(
				facade, payload, ringScope.submission);
			if (failure != NativeFontFallbackReason::None)
			{
				draw.runtimeFault = true;
				draw.failure = failure;
				draw.operation = "ring-submission";
			}
			NativeTilePacketScope packetScope(pass);
			draw.vanillaLikeBitmapRoute =
				payload.vanillaLikeBitmapPackets;
			NiDX9Renderer* renderer = draw.vanillaLikeBitmapRoute
				? nullptr : NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			if (!draw.vanillaLikeBitmapRoute && !device)
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = "acquire-pass-constant-ownership";
				draw.result = D3DERR_DEVICELOST;
			}
			// Retail NiDX9Renderer's indexed-array loop cannot express
			// triangle-list page boundaries: m_pusArrayLengths is interpreted as
			// strip lengths (primitiveCount = length - 2). Keep the page split at
			// the one vanilla sorted Tile callsite instead. Each packet selects a
			// FreeType text-shape proxy and then executes the untouched
			// RenderPassImmediately -> NiTriShape::RenderImmediate renderer path.
			//
			// Final ARGB and baked-coverage bitmaps use only vanilla c0. Skipping the
			// distance-field private-high-range ownership and facade bookkeeping
			// removes the only per-facade isolation work from this vanilla-like
			// multipage route.
			const bool isolatePacketConstants =
				!draw.vanillaLikeBitmapRoute;
			const bool batchedConstants = isolatePacketConstants
				&& ConstantOwnershipBatch().FrameActive();
			std::optional<NativePassConstantScope> localConstants;
			std::optional<NativeFacadeShaderBatchScope> shaderBatch;
			if (!draw.runtimeFault)
			{
				if (isolatePacketConstants)
					shaderBatch.emplace();
				// Retail 0xB64F90 calls RenderPassImmediately once per sorted item
				// with no intervening draw. Own PS c176-c183 and VS c208 for the
				// contiguous native segment.
				if (batchedConstants)
				{
					if (!ConstantOwnershipBatch().EnsureOwned(device))
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation = ConstantOwnershipBatch().Operation();
						draw.result = ConstantOwnershipBatch().Result();
						draw.mismatchRegister =
							ConstantOwnershipBatch().MismatchRegister();
					}
				}
				else if (isolatePacketConstants)
				{
					localConstants.emplace(device);
					if (!localConstants->Owned())
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation = localConstants->Operation();
						draw.result = localConstants->Result();
					}
				}
				for (UInt32 packetIndex = 0; !draw.runtimeFault
					&& packetIndex < payload.packetShaders.size(); ++packetIndex)
				{
					NiTriShape* proxyShape = nullptr;
					const NativeFontFallbackReason packetFailure =
						PrepareNativeFontRingPacket(facade, payload,
							ringScope.submission, packetIndex, proxyShape);
					if (packetFailure != NativeFontFallbackReason::None
						|| !proxyShape)
					{
						draw.runtimeFault = true;
						draw.failure =
							packetFailure != NativeFontFallbackReason::None
								? packetFailure
								: NativeFontFallbackReason::RuntimeFault;
						draw.operation = "ring-packet";
						break;
					}
					packetScope.Select(proxyShape);
					State().predecessorRenderPassImmediately(pass,
						currentPass, false, true,
						setupRenderStates);
					draw.drewPacket = true;
					++draw.drawnPacketCount;
					RecordFreeTypePerf(FreeTypePerfCounter::TilePass);
					const std::vector<NativeFontPacketTemplate>& activePackets =
						GetNativeFontPackets(*payload.payloadTemplate,
							payload.useCompositePackets);
					if (packetIndex < activePackets.size()
						&& activePackets[packetIndex].shaderClass
							== NativeFontShaderClass::Composite)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::CompositeDraw);
					}
					if (!IsNativeFontShaderGenerationCurrent(
						payload.preparedGeneration))
					{
						draw.runtimeFault = true;
						break;
					}
				}
				if (isolatePacketConstants && !batchedConstants
					&& localConstants
					&& !localConstants->Release())
				{
					draw.runtimeFault = true;
					draw.constantStateFault = true;
					draw.operation = localConstants->Operation();
					draw.result = localConstants->Result();
					draw.mismatchRegister =
						localConstants->MismatchRegister();
				}
			}
			if (isolatePacketConstants && batchedConstants && draw.runtimeFault
				&& !ReleaseNativeConstantOwnershipBatch("native-runtime-fault"))
			{
				draw.constantStateFault = true;
				draw.operation = ConstantOwnershipBatch().Operation();
				draw.result = ConstantOwnershipBatch().Result();
				draw.mismatchRegister =
					ConstantOwnershipBatch().MismatchRegister();
			}
			return draw;
		}

		bool TryDrawNativeSinglePacketDirect(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool setupRenderStates, NiTriShape* facade,
			NativeFontShapePayload& payload,
			NativePacketDrawResult& draw,
			UInt32 commandSpanIndex,
			UInt32 singlePacketCommandIndex)
		{
			if (!pass || !facade || !payload.buildComplete
				|| !payload.payloadTemplate
				|| payload.packetShaders.size() != 1)
			{
				return false;
			}
			const NativeFontPayloadTemplate& artifact =
				*payload.payloadTemplate;
			const std::vector<NativeFontPacketTemplate>& packets =
				GetNativeFontPackets(artifact, payload.useCompositePackets);
			if (packets.size() != 1 || !packets[0].vertexCount
				|| (packets[0].vertexCount & 3u)
				|| static_cast<UInt64>(packets[0].firstVertex)
					+ packets[0].vertexCount
						> artifact.gpuVertices.size()
				|| packets[0].atlasPage
					>= artifact.atlasProperties.size()
				|| packets[0].atlasPage
					>= artifact.atlasTextures.size())
			{
				return false;
			}
			const bool expandedDirectEligibility =
				artifact.pageCount != 1
				|| packets[0].atlasPage != 0
				|| packets[0].firstVertex != 0
				|| packets[0].vertexCount
					!= artifact.gpuVertices.size();

			FreeTypePerfScope perf(FreeTypePerfPhase::Submit);
			FreeTypePerfScope commandPerf(
				FreeTypePerfPhase::CommandSubmit,
				commandSpanIndex != kInvalidNativeFontCommandIndex
					|| singlePacketCommandIndex
						!= kInvalidNativeFontCommandIndex);
			RecordFreeTypePerf(
				FreeTypePerfCounter::SinglePacketDirectCandidate);

			const bool commandRequested =
				g_bEnableFreeTypeFontCommandBuffer
				&& (singlePacketCommandIndex
						!= kInvalidNativeFontCommandIndex
					|| commandSpanIndex
						!= kInvalidNativeFontCommandIndex);
			NativeFontCommandSpanView commandView;
			NativeFontSinglePacketCommandView singleCommandView;
			const NativeFontDrawCommand* command = nullptr;
			bool commandExecution = false;
			bool singleCommandExecution = false;
			if (commandRequested
				&& singlePacketCommandIndex
					!= kInvalidNativeFontCommandIndex
				&& BeginNativeFontSinglePacketCommandExecution(
					singlePacketCommandIndex, facade,
					singleCommandView))
			{
				if (singleCommandView.command
					&& singleCommandView.command->payload == &payload)
				{
					command = &singleCommandView.command->draw;
					commandExecution = command->program
						&& command->packet == &packets[0]
						&& command->binding.active
						&& command->binding.vertexCount
							== packets[0].vertexCount;
					singleCommandExecution = commandExecution;
				}
				if (!commandExecution)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							CommandSinglePacketFallback);
					RecordNativeFontCommandFallback(
						NativeFontCommandFallback::Topology);
					EndNativeFontSinglePacketCommandExecution(
						singlePacketCommandIndex, false, false);
				}
			}
			else if (commandRequested
				&& commandSpanIndex
					!= kInvalidNativeFontCommandIndex
				&& BeginNativeFontCommandSpanExecution(
					commandSpanIndex, facade, commandView))
			{
				if (commandView.span
					&& commandView.span->payload == &payload
					&& commandView.span->commandCount == 1)
				{
					command = ResolveNativeCommand(commandView, 0);
					commandExecution = command && command->program
						&& command->packet == &packets[0]
						&& command->binding.active
						&& command->binding.vertexCount
							== packets[0].vertexCount;
				}
				if (!commandExecution)
				{
					EndNativeFontCommandSpanExecution(
						commandSpanIndex, false, false);
				}
			}

			auto endCommandExecution =
				[&](bool success, bool drewPacket)
			{
				if (singleCommandExecution)
				{
					EndNativeFontSinglePacketCommandExecution(
						singlePacketCommandIndex,
						success, drewPacket);
				}
				else
				{
					EndNativeFontCommandSpanExecution(
						commandSpanIndex, success, drewPacket);
				}
			};

			NativeDirectShapeSubmissionScope submissionScope;
			std::optional<NativeDirectShapeBinding> binding;
			UInt32 submittedVertexCount = 0;
			if (commandExecution)
			{
				binding.emplace(facade, payload, command->binding);
				submittedVertexCount = command->binding.vertexCount;
			}
			else
			{
				const NativeFontFallbackReason submissionFailure =
					BeginNativeFontDirectShapeSubmission(
						facade, payload, submissionScope.submission);
				if (submissionFailure != NativeFontFallbackReason::None)
				{
					RecordSinglePacketDirectFallback(
						commandRequested
							? FreeTypePerfCounter::
								SinglePacketDirectFallbackCommand
							: FreeTypePerfCounter::
								SinglePacketDirectFallbackSubmission);
					return false;
				}
				binding.emplace(facade, payload,
					submissionScope.submission);
				submittedVertexCount =
					submissionScope.submission.vertexCount;
			}
			if (!binding || !binding->Active())
			{
				if (commandExecution)
				{
					if (singleCommandExecution)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								CommandSinglePacketFallback);
						RecordNativeFontCommandFallback(
							NativeFontCommandFallback::Resource);
					}
					endCommandExecution(false, false);
				}
				RecordSinglePacketDirectFallback(
					binding
						? DirectShapeBindingFallbackCounter(
							binding->Failure())
						: FreeTypePerfCounter::
							SinglePacketDirectFallbackBindingInput);
				return false;
			}

			draw.directShapeRoute = true;
			draw.vanillaLikeBitmapRoute =
				payload.vanillaLikeBitmapPackets;
			NiDX9Renderer* renderer = draw.vanillaLikeBitmapRoute
				? nullptr : NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			if (!draw.vanillaLikeBitmapRoute && !device)
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = "acquire-pass-constant-ownership";
				draw.result = D3DERR_DEVICELOST;
			}

			const bool isolatePacketConstants =
				!draw.vanillaLikeBitmapRoute;
			const bool batchedConstants = isolatePacketConstants
				&& ConstantOwnershipBatch().FrameActive();
			std::optional<NativePassConstantScope> localConstants;
			std::optional<NativeFacadeShaderBatchScope> shaderBatch;
			if (!draw.runtimeFault)
			{
				if (isolatePacketConstants)
					shaderBatch.emplace();
				if (batchedConstants)
				{
					if (!ConstantOwnershipBatch().EnsureOwned(device))
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation = ConstantOwnershipBatch().Operation();
						draw.result = ConstantOwnershipBatch().Result();
						draw.mismatchRegister =
							ConstantOwnershipBatch().MismatchRegister();
					}
				}
				else if (isolatePacketConstants)
				{
					localConstants.emplace(device);
					if (!localConstants->Owned())
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation = localConstants->Operation();
						draw.result = localConstants->Result();
					}
				}
			}

			if (!draw.runtimeFault)
			{
				const UInt32 validationCommandIndex =
					commandExecution
						? (singleCommandExecution
							? singlePacketCommandIndex
							: commandSpanIndex)
						: kInvalidNativeFontCommandIndex;
				NativeDirectImmediateScope immediateScope(
					facade, validationCommandIndex,
					commandExecution && !singleCommandExecution
						? 0u : kInvalidNativeFontCommandIndex,
					commandExecution, nullptr, nullptr,
					singleCommandExecution
						? NativeImmediateCommandKind::SinglePacket
						: NativeImmediateCommandKind::SpanPacket,
					commandExecution);
				bool usedNativeReplay = false;
				if (commandExecution)
				{
					NativeFontSegmentDeviceStateStamp
						segmentDeviceStateStamp;
					const NativeFontSegmentDeviceStateStamp*
						segmentDeviceState = nullptr;
					if (singleCommandExecution
						&& singleCommandView.stamp
						&& singleCommandView.command
						&& BuildSegmentDeviceStateStamp(
							singleCommandView.stamp,
							singleCommandView.command->
								executionSegmentEpoch,
							singleCommandView.command->
								executionExternalMutationEpoch,
							segmentDeviceStateStamp))
					{
						segmentDeviceState =
							&segmentDeviceStateStamp;
					}
					usedNativeReplay =
						InvokeNativeCommandBootstrap(pass, currentPass,
						false, true, setupRenderStates,
						facade, command, singleCommandExecution,
						true, binding->Buffer(),
						segmentDeviceState);
					if (!usedNativeReplay
						&& singleCommandExecution)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								CommandSinglePacketFallback);
					}
				}
				else
				{
					InvalidateSegmentDeviceStateCache();
					State().predecessorRenderPassImmediately(pass,
						currentPass, false, true,
						setupRenderStates);
				}
				if (immediateScope.Drew())
				{
					draw.drewPacket = true;
					draw.drawnPacketCount = 1;
					RecordFreeTypePerf(
						FreeTypePerfCounter::SinglePacketDirectDraw);
					RecordFreeTypePerf(
						FreeTypePerfCounter::SinglePacketDirectVertex,
						submittedVertexCount);
					if (usedNativeReplay
						&& expandedDirectEligibility)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								CommandDirectRangeReplay);
					}
					if (usedNativeReplay
						&& singleCommandExecution)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								CommandSinglePacketReplay);
					}
					RecordFreeTypePerf(FreeTypePerfCounter::TilePass);
					if (packets[0].shaderClass
						== NativeFontShaderClass::Composite)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::CompositeDraw);
					}
				}
				else
				{
					draw.runtimeFault = true;
					draw.failure = NativeFontFallbackReason::RuntimeFault;
					draw.operation = immediateScope.Invoked()
						? "direct-shape-command-validation"
						: "direct-shape-immediate-not-invoked";
					draw.result = E_FAIL;
				}
				if (!IsNativeFontShaderGenerationCurrent(
					payload.preparedGeneration))
				{
					draw.runtimeFault = true;
					draw.failure = NativeFontFallbackReason::RuntimeFault;
					draw.operation =
						"generation-changed-after-direct-shape";
					draw.result = D3DERR_DEVICELOST;
				}
			}

			if (isolatePacketConstants && !batchedConstants
				&& localConstants
				&& !localConstants->Release())
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = localConstants->Operation();
				draw.result = localConstants->Result();
				draw.mismatchRegister =
					localConstants->MismatchRegister();
			}
			if (isolatePacketConstants && batchedConstants
				&& draw.runtimeFault
				&& !ReleaseNativeConstantOwnershipBatch(
					"direct-shape-runtime-fault"))
			{
				draw.constantStateFault = true;
				draw.operation = ConstantOwnershipBatch().Operation();
				draw.result = ConstantOwnershipBatch().Result();
				draw.mismatchRegister =
					ConstantOwnershipBatch().MismatchRegister();
			}
			if (draw.runtimeFault)
			{
				RecordSinglePacketDirectFallback(
					FreeTypePerfCounter::
						SinglePacketDirectFallbackRuntime);
			}
			if (commandExecution)
			{
				const bool success =
					!draw.runtimeFault && draw.drewPacket;
				endCommandExecution(success, draw.drewPacket);
				if (!success && !draw.drewPacket
					&& !draw.constantStateFault)
				{
					return false;
				}
			}
			// Once the vanilla Tile pass has been entered, this route owns the item
			// even if the immediate callback was unexpectedly skipped. Replaying
			// through a proxy could duplicate a draw whose driver result is opaque.
			return true;
		}

	}

}

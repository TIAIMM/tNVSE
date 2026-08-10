#include "font_native_shader_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_native_shader {}
	using namespace implementation::font_native_shader;

	namespace
	{
		std::atomic<UInt32> s_vanillaLayoutReadinessLoggedMask{ 0 };
		std::atomic<UInt32> s_vanillaLayoutUploadFailureLoggedMask{ 0 };

		enum VanillaLayoutReadinessFailure : UInt32
		{
			kMissingShapeOrShader = 1u << 0,
			kShaderMismatch = 1u << 1,
			kProfileMismatch = 1u << 2,
			kGenerationMismatch = 1u << 3,
			kMissingModelData = 1u << 4,
			kMissingBuffer = 1u << 5,
			kDeclarationMismatch = 1u << 6,
			kStreamMismatch = 1u << 7,
			kStrideMismatch = 1u << 8,
			kVertexCountMismatch = 1u << 9,
			kVertexBufferMissing = 1u << 10,
			kNativePackIncomplete = 1u << 11,
			kNativePackContractMismatch = 1u << 12,
		};

		enum VanillaLayoutUploadFailure : UInt32
		{
			kUploadInvalidSource = 1u << 0,
			kUploadInvalidBuffer = 1u << 1,
			kUploadDescriptionFailed = 1u << 2,
			kUploadRangeInvalid = 1u << 3,
			kUploadLockFailed = 1u << 4,
			kUploadUnlockFailed = 1u << 5,
		};

		enum class VanillaLayoutDeclarationCompatibility : UInt8
		{
			Missing = 0,
			CurrentGeneration,
			CompatiblePreviousGeneration,
			Mismatch,
		};

		struct VanillaLayoutReadySnapshot
		{
			const NiTriShape* shape = nullptr;
			TileShader* shader = nullptr;
			const void* shaderVtable = nullptr;
			NativeShaderProfile* profile = nullptr;
			NativeShaderGeneration* generation = nullptr;
			IDirect3DVertexDeclaration9* generationDeclaration = nullptr;
			const NiTriShapeData* modelData = nullptr;
			const NiGeometryBufferData* buffer = nullptr;
			const void* bufferDeclaration = nullptr;
			const void* geometryGroup = nullptr;
			const UInt32* strideArray = nullptr;
			const NiVBChip* vertexChip = nullptr;
			IDirect3DVertexBuffer9* vertexBuffer = nullptr;
			IDirect3DIndexBuffer9* indexBuffer = nullptr;
			const UInt16* arrayLengths = nullptr;
			const UInt16* indexArray = nullptr;
			UInt32 generationId = 0;
			UInt32 deviceEpoch = 0;
			UInt32 bufferFlags = 0;
			UInt32 streamCount = 0;
			UInt32 stride = 0;
			UInt32 bufferVertexCount = 0;
			UInt32 dataVertexCount = 0;
			UInt32 baseVertexIndex = 0;
			UInt32 vertexChipOffset = 0;
			UInt32 vertexChipSize = 0;
			UInt32 indexCount = 0;
			UInt32 indexBufferSize = 0;
			UInt32 arrayCount = 0;
			UInt16 nativePackDataFlags = 0;
			UInt16 nativePackDirtyFlags = 0;
			UInt8 nativePackKeepFlags = 0;
			NativeFontVanillaLayoutKind layoutKind =
				NativeFontVanillaLayoutKind::None;
			bool nativePackCompleted = false;
			bool priorGenerationDeclaration = false;
		};

		enum class VanillaLayoutDrawTokenMismatch : UInt8
		{
			None = 0,
			Uncertified,
			ShapeOrShader,
			Generation,
			Geometry,
			NativePack,
			Layout,
		};

		VanillaLayoutDeclarationCompatibility
		ClassifyVanillaLayoutDeclaration(const NativeShaderGeneration* generation,
			NativeFontVanillaLayoutKind layoutKind,
			const NiGeometryBufferData* buffer)
		{
			IDirect3DVertexDeclaration9* expected = generation
				? (layoutKind == NativeFontVanillaLayoutKind::Uniform40
					? generation->vanillaLayoutD3DDeclaration
					: layoutKind == NativeFontVanillaLayoutKind::Parametric48
						? generation->vanillaParametricLayoutD3DDeclaration
						: nullptr)
				: nullptr;
			if (!generation || !buffer || !buffer->m_hDeclaration || !expected)
			{
				return VanillaLayoutDeclarationCompatibility::Missing;
			}
			auto* declaration = static_cast<IDirect3DVertexDeclaration9*>(
				buffer->m_hDeclaration);
			if (declaration == expected)
			{
				return VanillaLayoutDeclarationCompatibility::CurrentGeneration;
			}
			const std::vector<IDirect3DVertexDeclaration9*>* compatible =
				layoutKind == NativeFontVanillaLayoutKind::Uniform40
					? &generation->compatibleVanillaLayoutD3DDeclarations
					: layoutKind == NativeFontVanillaLayoutKind::Parametric48
						? &generation->
							compatibleVanillaParametricLayoutD3DDeclarations
						: nullptr;
			if (compatible && std::find(compatible->begin(), compatible->end(),
					declaration) != compatible->end())
			{
				return VanillaLayoutDeclarationCompatibility::
					CompatiblePreviousGeneration;
			}
			return VanillaLayoutDeclarationCompatibility::Mismatch;
		}

		bool HasVanillaLayoutNativePackRetirementContract(
			const NiGeometryData* data)
		{
			if (!data)
				return false;
			const UInt8 retainedSources = static_cast<UInt8>(
				NiGeometryData::KEEP_COLOR | NiGeometryData::KEEP_UV);
			return (data->m_usDirtyFlags & NiGeometryData::CONSISTENCY_MASK)
					== NiGeometryData::STATIC
				&& !(data->m_ucKeepFlags & retainedSources);
		}

		bool HasVanillaLayoutNativePackCompletionProof(
			const NiGeometryData* data)
		{
			return HasVanillaLayoutNativePackRetirementContract(data)
				&& !(data->m_usDirtyFlags & NiGeometryData::DIRTY_MASK)
				&& !(data->m_usDataFlags & NiGeometryData::TEXTURE_SET_MASK)
				&& !data->m_pkColor && !data->m_pkTexture;
		}

		void RecordPriorGenerationDeclarationUse(
			bool priorGenerationDeclaration)
		{
			if (priorGenerationDeclaration)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutPriorGenerationDeclarationUse);
			}
		}

		VanillaLayoutDrawTokenMismatch MatchVanillaLayoutDrawToken(
			const NiTriShape* shape, TileShader* shader,
			const NativeFontShapePayload& payload,
			const NativeFontVanillaLayoutDrawToken& token)
		{
			if (!token.valid)
				return VanillaLayoutDrawTokenMismatch::Uncertified;
			if (!shape || !shader || token.shapeIdentity != shape
				|| token.shaderIdentity != shader || shape->GetShader() != shader)
			{
				return VanillaLayoutDrawTokenMismatch::ShapeOrShader;
			}

			void** shaderVtable = *reinterpret_cast<void***>(shader);
			if (!shaderVtable || token.shaderVtableIdentity != shaderVtable)
				return VanillaLayoutDrawTokenMismatch::ShapeOrShader;
			NativeTileVtableBlock* block = RecoverNativeVtableBlock(shader);
			NativeShaderProfile* profile = block ? block->profile : nullptr;
			if (!profile || token.profileIdentity != profile
				|| profile->shader != shader
				|| !UsesNativeFontVanillaLayout(
					profile->key.vanillaLayoutKind)
				|| token.layoutKind != profile->key.vanillaLayoutKind)
			{
				return VanillaLayoutDrawTokenMismatch::ShapeOrShader;
			}
			const NativeFontPayloadTemplate* artifact =
				payload.payloadTemplate.get();
			const NativeFontPacketTemplate* packet = artifact
				&& artifact->compositePackets.size() == 1u
				? &artifact->compositePackets.front() : nullptr;
			if (!token.payloadUploaded || !payload.buildComplete
				|| token.payloadIdentity != &payload || !artifact || !packet
				|| token.artifactIdentity != artifact
				|| token.packetIdentity != packet
				|| token.standardLiteProgramIdentity
					!= &profile->retainedProgram)
			{
				return VanillaLayoutDrawTokenMismatch::Geometry;
			}

			NativeShaderGeneration* generation = ShaderState().publishedGeneration.load(
				std::memory_order_acquire);
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			// The generation and its declaration compatibility set are immutable and
			// process-lifetime once published. Exact generation/device-epoch identity
			// therefore preserves the earlier full declaration classification without
			// repeating its resource walk or compatible-declaration vector scan.
			const NativeFontVanillaLayoutKind layoutKind =
				profile->key.vanillaLayoutKind;
			const UInt32 expectedStride = ResolveVanillaLayoutStride(layoutKind);
			IDirect3DVertexDeclaration9* expectedDeclaration = generation
				? ResolveVanillaLayoutD3DDeclaration(*generation, layoutKind)
				: nullptr;
			if (!generation || !expectedStride || !expectedDeclaration
				|| token.generationIdentity != generation
				|| profile->owner != generation
				|| generation->id != token.generation
				|| generation->deviceEpoch != token.deviceEpoch
				|| ShaderState().deviceEpoch.load(std::memory_order_acquire)
					!= token.deviceEpoch
				|| ShaderState().resetInProgress.load(std::memory_order_acquire)
				|| generation->runtimeFault.load(std::memory_order_acquire)
				|| generation->renderer != renderer || generation->device != device
				|| !IsVanillaLayoutGenerationReady(generation, layoutKind)
				|| expectedDeclaration
					!= token.generationDeclarationIdentity)
			{
				return VanillaLayoutDrawTokenMismatch::Generation;
			}

			const NiTriShapeData* data = shape->GetModelData();
			if (!data || token.modelDataIdentity != data
				|| !data->m_usVertices
				|| data->m_usVertices != token.dataVertexCount)
			{
				return VanillaLayoutDrawTokenMismatch::Geometry;
			}
			if (!token.nativePackCompleted
				|| !HasVanillaLayoutNativePackCompletionProof(data)
				|| token.nativePackDataFlags != data->m_usDataFlags
				|| token.nativePackDirtyFlags != data->m_usDirtyFlags
				|| token.nativePackKeepFlags != data->m_ucKeepFlags)
			{
				return VanillaLayoutDrawTokenMismatch::NativePack;
			}
			const NiGeometryBufferData* buffer = data->m_pkBuffData;
			if (!buffer || token.bufferIdentity != buffer)
				return VanillaLayoutDrawTokenMismatch::Geometry;

			const NiVBChip* chip = buffer->m_ppkVBChip
				&& buffer->m_uiStreamCount ? buffer->m_ppkVBChip[0] : nullptr;
			const UInt64 expectedByteOffset = chip
				? static_cast<UInt64>(chip->m_uiOffset)
					+ static_cast<UInt64>(buffer->m_uiBaseVertexIndex)
						* expectedStride : 0u;
			const UInt64 expectedByteCount =
				static_cast<UInt64>(data->m_usVertices)
					* expectedStride;
			if (!token.bufferDeclarationIdentity
				|| buffer->m_hDeclaration != token.bufferDeclarationIdentity
				|| buffer->m_uiFlags != token.bufferFlags
				|| buffer->m_pkGeometryGroup != token.geometryGroupIdentity
				|| token.streamCount != 1u
				|| buffer->m_uiStreamCount != token.streamCount
				|| !token.strideArrayIdentity
				|| buffer->m_puiVertexStride != token.strideArrayIdentity
				|| token.stride != expectedStride
				|| buffer->m_puiVertexStride[0] != token.stride
				|| buffer->m_uiVertCount != token.bufferVertexCount
				|| token.bufferVertexCount < token.dataVertexCount
				|| !chip || token.vertexChipIdentity != chip
				|| !chip->m_pkVB || token.vertexBufferIdentity != chip->m_pkVB
				|| token.baseVertexIndex != buffer->m_uiBaseVertexIndex
				|| token.vertexChipOffset != chip->m_uiOffset
				|| token.vertexChipSize != chip->m_uiSize
				|| !token.indexBufferIdentity
				|| buffer->m_pkIB != token.indexBufferIdentity
				|| buffer->m_uiIndexCount != token.indexCount
				|| buffer->m_uiIBSize != token.indexBufferSize
				|| buffer->m_uiNumArrays != token.arrayCount
				|| buffer->m_pusArrayLengths != token.arrayLengthsIdentity
				|| buffer->m_pusIndexArray != token.indexArrayIdentity
				|| expectedByteOffset > std::numeric_limits<UInt32>::max()
				|| expectedByteCount > std::numeric_limits<UInt32>::max()
				|| token.uploadedByteOffset
					!= static_cast<UInt32>(expectedByteOffset)
				|| token.uploadedByteCount
					!= static_cast<UInt32>(expectedByteCount))
			{
				return VanillaLayoutDrawTokenMismatch::Layout;
			}
			return VanillaLayoutDrawTokenMismatch::None;
		}

		void RecordVanillaLayoutDrawTokenMiss(
			VanillaLayoutDrawTokenMismatch mismatch)
		{
			RecordFreeTypePerf(FreeTypePerfCounter::
				VanillaLayoutDrawTokenSlowPath);
			switch (mismatch)
			{
			case VanillaLayoutDrawTokenMismatch::Uncertified:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutDrawTokenUncertified);
				break;
			case VanillaLayoutDrawTokenMismatch::ShapeOrShader:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutDrawTokenShapeShaderMismatch);
				break;
			case VanillaLayoutDrawTokenMismatch::Generation:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutDrawTokenGenerationMismatch);
				break;
			case VanillaLayoutDrawTokenMismatch::Geometry:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutDrawTokenGeometryMismatch);
				break;
			case VanillaLayoutDrawTokenMismatch::NativePack:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutDrawTokenNativePackMismatch);
				break;
			case VanillaLayoutDrawTokenMismatch::Layout:
				RecordFreeTypePerf(FreeTypePerfCounter::
					VanillaLayoutDrawTokenLayoutMismatch);
				break;
			default:
				break;
			}
		}

		void CertifyVanillaLayoutDrawToken(
			NativeFontVanillaLayoutDrawToken& token,
			const VanillaLayoutReadySnapshot& snapshot,
			const NativeFontShapePayload& payload,
			const NativeFontPayloadTemplate& artifact,
			const NativeFontPacketTemplate& packet,
			UInt32 uploadedByteOffset, UInt32 uploadedByteCount)
		{
			// Publish validity last so a later reuse cannot observe a partially
			// rewritten proof.
			token.valid = false;
			token.shapeIdentity = snapshot.shape;
			token.shaderIdentity = snapshot.shader;
			token.shaderVtableIdentity = snapshot.shaderVtable;
			token.profileIdentity = snapshot.profile;
			token.generationIdentity = snapshot.generation;
			token.generationDeclarationIdentity =
				snapshot.generationDeclaration;
			token.modelDataIdentity = snapshot.modelData;
			token.bufferIdentity = snapshot.buffer;
			token.bufferDeclarationIdentity = snapshot.bufferDeclaration;
			token.geometryGroupIdentity = snapshot.geometryGroup;
			token.strideArrayIdentity = snapshot.strideArray;
			token.vertexChipIdentity = snapshot.vertexChip;
			token.vertexBufferIdentity = snapshot.vertexBuffer;
			token.indexBufferIdentity = snapshot.indexBuffer;
			token.arrayLengthsIdentity = snapshot.arrayLengths;
			token.indexArrayIdentity = snapshot.indexArray;
			token.payloadIdentity = &payload;
			token.artifactIdentity = &artifact;
			token.packetIdentity = &packet;
			token.standardLiteProgramIdentity =
				&snapshot.profile->retainedProgram;
			token.generation = snapshot.generationId;
			token.deviceEpoch = snapshot.deviceEpoch;
			token.bufferFlags = snapshot.bufferFlags;
			token.streamCount = snapshot.streamCount;
			token.stride = snapshot.stride;
			token.bufferVertexCount = snapshot.bufferVertexCount;
			token.dataVertexCount = snapshot.dataVertexCount;
			token.baseVertexIndex = snapshot.baseVertexIndex;
			token.vertexChipOffset = snapshot.vertexChipOffset;
			token.vertexChipSize = snapshot.vertexChipSize;
			token.indexCount = snapshot.indexCount;
			token.indexBufferSize = snapshot.indexBufferSize;
			token.arrayCount = snapshot.arrayCount;
			token.uploadedByteOffset = uploadedByteOffset;
			token.uploadedByteCount = uploadedByteCount;
			token.nativePackDataFlags = snapshot.nativePackDataFlags;
			token.nativePackDirtyFlags = snapshot.nativePackDirtyFlags;
			token.nativePackKeepFlags = snapshot.nativePackKeepFlags;
			token.layoutKind = snapshot.layoutKind;
			token.nativePackCompleted = snapshot.nativePackCompleted;
			token.priorGenerationDeclaration =
				snapshot.priorGenerationDeclaration;
			token.payloadUploaded = true;
			token.everCertified = true;
			token.valid = true;
		}

		bool ValidateNativeFontVanillaLayoutShapeReadiness(
			const NiTriShape* shape,
			TileShader* shader, bool logFailure,
			VanillaLayoutReadySnapshot* readySnapshot)
		{
			if (readySnapshot)
				*readySnapshot = {};
			const bool hasShapeAndShader = shape && shader;
			const bool shaderMatches = hasShapeAndShader
				&& shape->GetShader() == shader;
			NativeTileVtableBlock* block = shader
				? RecoverNativeVtableBlock(shader) : nullptr;
			NativeShaderProfile* profile = block ? block->profile : nullptr;
			NativeShaderGeneration* generation = profile
				? profile->owner : nullptr;
			const NiTriShapeData* data = shape ? shape->GetModelData() : nullptr;
			const NiGeometryBufferData* buffer = data
				? data->m_pkBuffData : nullptr;
			const NativeFontVanillaLayoutKind layoutKind = profile
				? profile->key.vanillaLayoutKind
				: NativeFontVanillaLayoutKind::None;
			const UInt32 expectedStride = ResolveVanillaLayoutStride(layoutKind);
			const bool profileMatches = profile && profile->shader == shader
				&& UsesNativeFontVanillaLayout(layoutKind);
			const bool generationMatches = generation
				&& IsVanillaLayoutGenerationReady(generation, layoutKind)
				&& GenerationMatchesCurrentDevice(generation);
			const UInt32 streamCount = buffer ? buffer->m_uiStreamCount : 0u;
			const UInt32 stride = buffer && buffer->m_puiVertexStride
				&& streamCount ? buffer->m_puiVertexStride[0] : 0u;
			const NiVBChip* vertexChip = buffer && buffer->m_ppkVBChip
				&& streamCount ? buffer->m_ppkVBChip[0] : nullptr;
			IDirect3DVertexBuffer9* vertexBuffer = vertexChip
				? vertexChip->m_pkVB : nullptr;
			IDirect3DVertexDeclaration9* expectedDeclaration = generation
				? ResolveVanillaLayoutD3DDeclaration(*generation, layoutKind)
				: nullptr;
			const VanillaLayoutDeclarationCompatibility declarationCompatibility =
				ClassifyVanillaLayoutDeclaration(generation, layoutKind, buffer);
			const UInt32 compatibleDeclarationCount = generation
				? static_cast<UInt32>(layoutKind
					== NativeFontVanillaLayoutKind::Uniform40
						? generation->compatibleVanillaLayoutD3DDeclarations.size()
						: layoutKind
							== NativeFontVanillaLayoutKind::Parametric48
								? generation->
									compatibleVanillaParametricLayoutD3DDeclarations.size()
								: 0u)
				: 0u;
			const bool declarationMatches = declarationCompatibility
				== VanillaLayoutDeclarationCompatibility::CurrentGeneration
				|| declarationCompatibility == VanillaLayoutDeclarationCompatibility::
					CompatiblePreviousGeneration;
			// Retail PerformPrecache clears the low 12 dirty bits at 0xE74469 and
			// then retires static color/UV sources through 0xE7447D -> 0xE6FA90.
			// Under the renderer lock, all of these postconditions together are the
			// completion proof that neither vanilla nor NVTF still owns a queued pack
			// which may overwrite our final certified layout stream.
			const bool nativePackRetirementContract =
				HasVanillaLayoutNativePackRetirementContract(data);
			const bool nativePackCompleted =
				HasVanillaLayoutNativePackCompletionProof(data);
			const bool structurallyReady = hasShapeAndShader && shaderMatches
				&& profileMatches && generationMatches && data
				&& buffer && declarationMatches
				&& expectedStride && streamCount == 1u
				&& stride == expectedStride
				&& buffer->m_uiVertCount >= data->m_usVertices
				&& vertexChip && vertexBuffer;
			const bool ready = structurallyReady && nativePackCompleted;
			if (ready)
			{
				const bool priorGenerationDeclaration =
					declarationCompatibility == VanillaLayoutDeclarationCompatibility::
						CompatiblePreviousGeneration;
				if (readySnapshot)
				{
					readySnapshot->shape = shape;
					readySnapshot->shader = shader;
					readySnapshot->shaderVtable =
						*reinterpret_cast<void***>(shader);
					readySnapshot->profile = profile;
					readySnapshot->generation = generation;
					readySnapshot->generationDeclaration = expectedDeclaration;
					readySnapshot->modelData = data;
					readySnapshot->buffer = buffer;
					readySnapshot->bufferDeclaration = buffer->m_hDeclaration;
					readySnapshot->geometryGroup = buffer->m_pkGeometryGroup;
					readySnapshot->strideArray = buffer->m_puiVertexStride;
					readySnapshot->vertexChip = vertexChip;
					readySnapshot->vertexBuffer = vertexBuffer;
					readySnapshot->indexBuffer = buffer->m_pkIB;
					readySnapshot->arrayLengths = buffer->m_pusArrayLengths;
					readySnapshot->indexArray = buffer->m_pusIndexArray;
					readySnapshot->generationId = generation->id;
					readySnapshot->deviceEpoch = generation->deviceEpoch;
					readySnapshot->bufferFlags = buffer->m_uiFlags;
					readySnapshot->streamCount = streamCount;
					readySnapshot->stride = stride;
					readySnapshot->bufferVertexCount = buffer->m_uiVertCount;
					readySnapshot->dataVertexCount = data->m_usVertices;
					readySnapshot->baseVertexIndex =
						buffer->m_uiBaseVertexIndex;
					readySnapshot->vertexChipOffset = vertexChip->m_uiOffset;
					readySnapshot->vertexChipSize = vertexChip->m_uiSize;
					readySnapshot->indexCount = buffer->m_uiIndexCount;
					readySnapshot->indexBufferSize = buffer->m_uiIBSize;
					readySnapshot->arrayCount = buffer->m_uiNumArrays;
					readySnapshot->nativePackDataFlags = data->m_usDataFlags;
					readySnapshot->nativePackDirtyFlags = data->m_usDirtyFlags;
					readySnapshot->nativePackKeepFlags = data->m_ucKeepFlags;
					readySnapshot->layoutKind = layoutKind;
					readySnapshot->nativePackCompleted = nativePackCompleted;
					readySnapshot->priorGenerationDeclaration =
						priorGenerationDeclaration;
				}
				RecordPriorGenerationDeclarationUse(
					priorGenerationDeclaration);
				return true;
			}
			if (!logFailure)
				return ready;

			UInt32 failures = 0;
			if (!hasShapeAndShader)
				failures |= kMissingShapeOrShader;
			if (hasShapeAndShader && !shaderMatches)
				failures |= kShaderMismatch;
			if (!profileMatches)
				failures |= kProfileMismatch;
			if (!generationMatches)
				failures |= kGenerationMismatch;
			if (!data)
				failures |= kMissingModelData;
			if (!buffer)
				failures |= kMissingBuffer;
			if (buffer && !declarationMatches)
				failures |= kDeclarationMismatch;
			if (buffer && streamCount != 1u)
				failures |= kStreamMismatch;
			if (buffer && stride != expectedStride)
				failures |= kStrideMismatch;
			if (buffer && data && buffer->m_uiVertCount < data->m_usVertices)
				failures |= kVertexCountMismatch;
			if (buffer && (!vertexChip || !vertexBuffer))
				failures |= kVertexBufferMissing;
			if (data && !nativePackRetirementContract)
				failures |= kNativePackContractMismatch;
			if (structurallyReady && nativePackRetirementContract
				&& !nativePackCompleted)
			{
				failures |= kNativePackIncomplete;
				RecordFreeTypePerf(
					FreeTypePerfCounter::VanillaLayoutNativePackPending);
			}

			const UInt32 previous = s_vanillaLayoutReadinessLoggedMask.fetch_or(
				failures, std::memory_order_acq_rel);
			const UInt32 newlyObserved = failures & ~previous;
			if (newlyObserved)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_vanilla_layout_readiness: failure=0x%08X new=0x%08X shape=%p shader=%p shapeShader=%p profile=%u vanillaProfile=%u layoutKind=%u generation=%u current=%u targetReady=%u deviceEpoch=%u completionContract=%u nativePackCompleted=%u dataFlags=0x%04X dirtyFlags=0x%04X keepFlags=0x%02X color=%p texture=%p buffer=%p declarationClass=%u declaration=%p expected=%p compatiblePrevious=%u streams=%u stride=%u expectedStride=%u bufferVertices=%u dataVertices=%u chip=%p vertexBuffer=%p",
					failures, newlyObserved, shape, shader,
					shape ? shape->GetShader() : nullptr,
					profile ? 1u : 0u,
					profileMatches ? 1u : 0u,
					static_cast<UInt32>(layoutKind),
					generation ? generation->id : 0u,
					generationMatches ? 1u : 0u,
					IsVanillaLayoutGenerationReady(generation, layoutKind) ? 1u : 0u,
					generation ? generation->deviceEpoch : 0u,
					nativePackRetirementContract ? 1u : 0u,
					nativePackCompleted ? 1u : 0u,
					data ? static_cast<UInt32>(data->m_usDataFlags) : 0u,
					data ? static_cast<UInt32>(data->m_usDirtyFlags) : 0u,
					data ? static_cast<UInt32>(data->m_ucKeepFlags) : 0u,
					data ? data->m_pkColor : nullptr,
					data ? data->m_pkTexture : nullptr,
					buffer,
					static_cast<UInt32>(declarationCompatibility),
					buffer ? buffer->m_hDeclaration : nullptr,
					expectedDeclaration,
					compatibleDeclarationCount,
					streamCount, stride, expectedStride,
					buffer ? buffer->m_uiVertCount : 0u,
					data ? data->m_usVertices : 0u,
					vertexChip, vertexBuffer);
			}
			return false;
		}

		class VanillaLayoutRendererLockScope final
		{
		public:
			explicit VanillaLayoutRendererLockScope(NiDX9Renderer* renderer)
				: m_renderer(renderer)
			{
				if (m_renderer)
					m_renderer->LockRenderer();
			}

			~VanillaLayoutRendererLockScope()
			{
				if (m_renderer)
					m_renderer->UnlockRenderer();
			}

			VanillaLayoutRendererLockScope(
				const VanillaLayoutRendererLockScope&) = delete;
			VanillaLayoutRendererLockScope& operator=(
				const VanillaLayoutRendererLockScope&) = delete;

		private:
			NiDX9Renderer* m_renderer = nullptr;
		};

		bool RejectVanillaLayoutPayloadUpload(UInt32 failure,
			const char* operation, HRESULT result,
			const VanillaLayoutReadySnapshot& snapshot,
			UInt32 bufferSize, UInt64 byteOffset, UInt64 byteCount)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutPayloadUploadFailure);
			const UInt32 failureShift = snapshot.layoutKind
				== NativeFontVanillaLayoutKind::Parametric48 ? 8u : 0u;
			const UInt32 keyedFailure = failure << failureShift;
			const UInt32 previous =
				s_vanillaLayoutUploadFailureLoggedMask.fetch_or(
					keyedFailure, std::memory_order_acq_rel);
			const UInt32 newFailure =
				(keyedFailure & ~previous) >> failureShift;
			if (newFailure)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_vanilla_layout_upload: status=failure layoutKind=%u failure=0x%08X new=0x%08X operation=%s hr=0x%08X shape=%p data=%p buffer=%p chip=%p vertexBuffer=%p stride=%u dataVertices=%u bufferVertices=%u baseVertex=%u chipOffset=%u chipSize=%u bufferSize=%u byteOffset=%I64u byteCount=%I64u",
					static_cast<UInt32>(snapshot.layoutKind), failure, newFailure,
					operation ? operation : "unknown",
					static_cast<UInt32>(result), snapshot.shape,
					snapshot.modelData, snapshot.buffer, snapshot.vertexChip,
					snapshot.vertexBuffer, snapshot.stride,
					snapshot.dataVertexCount, snapshot.bufferVertexCount,
					snapshot.baseVertexIndex, snapshot.vertexChipOffset,
					snapshot.vertexChipSize, bufferSize, byteOffset, byteCount);
			}
			return false;
		}

		bool UploadVanillaLayoutPayload(
			const NativeFontShapePayload& payload,
			const VanillaLayoutReadySnapshot& snapshot,
			const NativeFontPayloadTemplate*& artifactOut,
			const NativeFontPacketTemplate*& packetOut,
			UInt32& byteOffsetOut, UInt32& byteCountOut)
		{
			artifactOut = nullptr;
			packetOut = nullptr;
			byteOffsetOut = 0;
			byteCountOut = 0;
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutPayloadUploadAttempt);

			const NativeFontPayloadTemplate* artifact =
				payload.payloadTemplate.get();
			const NativeFontPacketTemplate* packet = artifact
				&& artifact->compositePackets.size() == 1u
				? &artifact->compositePackets.front() : nullptr;
			const UInt64 packetEnd = packet
				? static_cast<UInt64>(packet->firstVertex)
					+ packet->vertexCount : 0u;
			const NiPoint3& origin = payload.geometryOrigin;
			const bool finiteOrigin = std::isfinite(origin.x)
				&& std::isfinite(origin.y) && std::isfinite(origin.z);
			const NativeFontVanillaLayoutKind layoutKind = snapshot.layoutKind;
			const bool uniformLayout =
				layoutKind == NativeFontVanillaLayoutKind::Uniform40;
			const bool parametricLayout =
				layoutKind == NativeFontVanillaLayoutKind::Parametric48;
			const UInt32 expectedStride = ResolveVanillaLayoutStride(layoutKind);
			if (!payload.buildComplete || !artifact || !packet
				|| (!uniformLayout && !parametricLayout) || !expectedStride
				|| !snapshot.profile
				|| snapshot.profile->key.vanillaLayoutKind != layoutKind
				|| !HasNativeFontPayloadValidationSeal(*artifact)
				|| artifact->pageCount != 1u
				|| packet->shaderClass != NativeFontShaderClass::Composite
				|| (packet->distanceFieldMethod != DistanceFieldMethod::TrueSdf
					&& packet->distanceFieldMethod != DistanceFieldMethod::Mtsdf)
				|| packet->atlasPage != 0u || !packet->vertexCount
				|| (packet->firstVertex & 3u) || (packet->vertexCount & 3u)
				|| packet->staticCompositeLayerMask < 8u
				|| packet->staticCompositeLayerMask > 15u
				|| !std::isfinite(packet->uniformSdfSpread)
				|| packet->uniformSdfSpread < 0.0f
				|| !std::isfinite(packet->uniformDistanceParameterScale)
				|| packet->uniformDistanceParameterScale < 0.0f
				|| (packet->uniformDistanceParameterScale > 0.0f
					&& packet->uniformDistanceParameterScale < 1.0f)
				|| (uniformLayout
					&& (packet->uniformSdfSpread <= 0.0f
						|| packet->uniformDistanceParameterScale < 1.0f))
				|| packet->vertexCount != snapshot.dataVertexCount
				|| packetEnd > artifact->gpuVertices.size() || !finiteOrigin)
			{
				return RejectVanillaLayoutPayloadUpload(kUploadInvalidSource,
					"source-contract", E_INVALIDARG, snapshot, 0u, 0u, 0u);
			}

			for (UInt32 ordinal = 0; ordinal < packet->vertexCount; ++ordinal)
			{
				const NativeFontGpuVertex& source = artifact->gpuVertices[
					static_cast<size_t>(packet->firstVertex) + ordinal];
				const NativeFontGpuVertex& quadFirst = artifact->gpuVertices[
					static_cast<size_t>(packet->firstVertex)
						+ (ordinal & ~UInt32(3u))];
				if (!std::isfinite(source.x) || !std::isfinite(source.y)
					|| !std::isfinite(source.z) || !std::isfinite(source.u)
					|| !std::isfinite(source.v)
					|| !std::isfinite(source.sdfSpread)
					|| source.sdfSpread <= 0.0f
					|| !std::isfinite(source.distanceParameterScale)
					|| source.distanceParameterScale < 1.0f
					|| source.layerMask != static_cast<float>(
						packet->staticCompositeLayerMask)
					|| source.sdfSpread != quadFirst.sdfSpread
					|| source.distanceParameterScale
						!= quadFirst.distanceParameterScale
					|| (uniformLayout
						&& (source.sdfSpread != packet->uniformSdfSpread
							|| source.distanceParameterScale
								!= packet->uniformDistanceParameterScale))
					|| !std::isfinite(source.glyphU0)
					|| !std::isfinite(source.glyphV0)
					|| !std::isfinite(source.glyphU1)
					|| !std::isfinite(source.glyphV1)
					|| source.glyphU0 > source.glyphU1
					|| source.glyphV0 > source.glyphV1
					|| source.glyphU0 != quadFirst.glyphU0
					|| source.glyphV0 != quadFirst.glyphV0
					|| source.glyphU1 != quadFirst.glyphU1
					|| source.glyphV1 != quadFirst.glyphV1
					|| !std::isfinite(source.x + origin.x)
					|| !std::isfinite(source.y + origin.y)
					|| !std::isfinite(source.z + origin.z))
				{
					return RejectVanillaLayoutPayloadUpload(kUploadInvalidSource,
						"source-vertex", E_INVALIDARG, snapshot,
						0u, 0u, 0u);
				}
			}

			const NiVBChip* chip = snapshot.vertexChip;
			IDirect3DVertexBuffer9* vertexBuffer = snapshot.vertexBuffer;
			if (!snapshot.buffer || !chip || !vertexBuffer
				|| snapshot.buffer->m_uiStreamCount != snapshot.streamCount
				|| !snapshot.buffer->m_puiVertexStride
				|| snapshot.buffer->m_puiVertexStride[0] != snapshot.stride
				|| snapshot.buffer->m_uiVertCount != snapshot.bufferVertexCount
				|| snapshot.buffer->m_uiBaseVertexIndex
					!= snapshot.baseVertexIndex
				|| chip->m_pkVB != vertexBuffer
				|| chip->m_uiOffset != snapshot.vertexChipOffset
				|| chip->m_uiSize != snapshot.vertexChipSize
				|| snapshot.stride != expectedStride
				|| !snapshot.dataVertexCount || !snapshot.vertexChipSize
				|| chip->m_uiLockFlags != 0u)
			{
				return RejectVanillaLayoutPayloadUpload(kUploadInvalidBuffer,
					"buffer-contract", D3DERR_INVALIDCALL, snapshot,
					0u, 0u, 0u);
			}

			D3DVERTEXBUFFER_DESC description = {};
			const HRESULT descriptionResult = vertexBuffer->GetDesc(&description);
			if (FAILED(descriptionResult))
			{
				return RejectVanillaLayoutPayloadUpload(
					kUploadDescriptionFailed, "GetDesc", descriptionResult,
					snapshot, 0u, 0u, 0u);
			}

			const UInt64 relativeByteOffset =
				static_cast<UInt64>(snapshot.baseVertexIndex) * snapshot.stride;
			const UInt64 byteOffset = static_cast<UInt64>(snapshot.vertexChipOffset)
				+ relativeByteOffset;
			const UInt64 byteCount = static_cast<UInt64>(snapshot.dataVertexCount)
				* snapshot.stride;
			const bool chipRangeValid = relativeByteOffset
				<= snapshot.vertexChipSize
				&& byteCount <= static_cast<UInt64>(snapshot.vertexChipSize)
					- relativeByteOffset;
			const bool bufferRangeValid = byteOffset <= description.Size
				&& byteCount <= static_cast<UInt64>(description.Size) - byteOffset;
			if (!byteCount || byteOffset > std::numeric_limits<UInt32>::max()
				|| byteCount > std::numeric_limits<UInt32>::max()
				|| !chipRangeValid || !bufferRangeValid)
			{
				return RejectVanillaLayoutPayloadUpload(kUploadRangeInvalid,
					"range", D3DERR_INVALIDCALL, snapshot, description.Size,
					byteOffset, byteCount);
			}

			void* locked = nullptr;
			const HRESULT lockResult = vertexBuffer->Lock(
				static_cast<UINT>(byteOffset), static_cast<UINT>(byteCount),
				&locked, 0u);
			if (FAILED(lockResult))
			{
				return RejectVanillaLayoutPayloadUpload(kUploadLockFailed,
					"Lock", lockResult,
					snapshot, description.Size, byteOffset, byteCount);
			}
			if (!locked)
			{
				// Preserve the successful Lock/Unlock pairing even for a broken
				// driver or interposer that violates D3D9's output contract.
				vertexBuffer->Unlock();
				return RejectVanillaLayoutPayloadUpload(kUploadLockFailed,
					"Lock-null", E_POINTER, snapshot, description.Size,
					byteOffset, byteCount);
			}

			if (uniformLayout)
			{
				auto* destination =
					static_cast<NativeFontVanillaLayoutVertex*>(locked);
				for (UInt32 ordinal = 0; ordinal < packet->vertexCount; ++ordinal)
				{
					const NativeFontGpuVertex& source = artifact->gpuVertices[
						static_cast<size_t>(packet->firstVertex) + ordinal];
					NativeFontVanillaLayoutVertex& packed = destination[ordinal];
					packed.x = source.x + origin.x;
					packed.y = source.y + origin.y;
					packed.z = source.z + origin.z;
					packed.u = source.u;
					packed.v = source.v;
					packed.color = source.color;
					packed.glyphU0 = source.glyphU0;
					packed.glyphV0 = source.glyphV0;
					packed.glyphU1 = source.glyphU1;
					packed.glyphV1 = source.glyphV1;
				}
			}
			else
			{
				auto* destination =
					static_cast<NativeFontVanillaParametricVertex*>(locked);
				for (UInt32 ordinal = 0; ordinal < packet->vertexCount; ++ordinal)
				{
					const NativeFontGpuVertex& source = artifact->gpuVertices[
						static_cast<size_t>(packet->firstVertex) + ordinal];
					NativeFontVanillaParametricVertex& packed =
						destination[ordinal];
					packed.x = source.x + origin.x;
					packed.y = source.y + origin.y;
					packed.z = source.z + origin.z;
					packed.u = source.u;
					packed.v = source.v;
					packed.color = source.color;
					packed.sdfSpread = source.sdfSpread;
					packed.distanceParameterScale =
						source.distanceParameterScale;
					packed.glyphU0 = source.glyphU0;
					packed.glyphV0 = source.glyphV0;
					packed.glyphU1 = source.glyphU1;
					packed.glyphV1 = source.glyphV1;
				}
			}
			const HRESULT unlockResult = vertexBuffer->Unlock();
			if (FAILED(unlockResult))
			{
				return RejectVanillaLayoutPayloadUpload(kUploadUnlockFailed,
					"Unlock", unlockResult, snapshot, description.Size,
					byteOffset, byteCount);
			}

			artifactOut = artifact;
			packetOut = packet;
			byteOffsetOut = static_cast<UInt32>(byteOffset);
			byteCountOut = static_cast<UInt32>(byteCount);
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutPayloadUploadSuccess);
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutPayloadUploadBytes,
				byteCountOut);
			return true;
		}
	}

	bool RequestNativeFontVanillaLayoutShapePrecache(NiTriShape* shape,
		TileShader* shader)
	{
		if (!shape || !shader)
			return false;
		NativeTileVtableBlock* block = RecoverNativeVtableBlock(shader);
		NativeShaderProfile* profile = block ? block->profile : nullptr;
		NativeShaderGeneration* generation = profile ? profile->owner : nullptr;
		const NativeFontVanillaLayoutKind layoutKind = profile
			? profile->key.vanillaLayoutKind
			: NativeFontVanillaLayoutKind::None;
		NiDX9ShaderDeclaration* declaration = generation
			? ResolveVanillaLayoutDeclaration(*generation, layoutKind)
			: nullptr;
		NiTriShapeData* data = shape->GetModelData();
		const UInt32 textureCoordinatesPresent = data
			? data->m_usDataFlags & NiGeometryData::TEXTURE_SET_MASK : 0u;
		if (!profile || profile->shader != shader
			|| !UsesNativeFontVanillaLayout(layoutKind) || !generation
			|| !generation->renderer
			|| !IsVanillaLayoutGenerationReady(generation, layoutKind)
			|| !declaration
			|| !GenerationMatchesCurrentDevice(generation) || !data
			|| !data->m_usVertices || !data->m_pkVertex || !data->m_pkColor
			|| !data->m_pkTexture || !textureCoordinatesPresent
			|| !data->m_pusTriList || !data->m_uiTriListLength
			|| !HasVanillaLayoutNativePackRetirementContract(data))
		{
			return false;
		}

		shape->SetShader(shader);
		if (data->m_pkBuffData)
		{
			// Keep this on the virtual route so renderer queue/interoperability
			// detours retain ownership of synchronization and buffer lifetime.
			generation->renderer->PurgeGeometryData(data);
			if (data->m_pkBuffData)
				return false;
		}
		if (!generation->renderer->PrecacheGeometry(shape, 0u, 0u,
			declaration))
		{
			return false;
		}

		// Never inspect or upload immediately after this virtual call. Vanilla
		// PrecacheGeometryEx may have attached a fully shaped buffer while its
		// PrePackObject is still queued, and NVTF may instead have queued the whole
		// request on its worker. The owning route finishes asynchronously; the first
		// draw performs the one-time upload only after the postpack proof is visible
		// under the renderer lock.
		return true;
	}

	bool EnsureNativeFontVanillaLayoutShapeReady(const NiTriShape* shape,
		TileShader* shader, const NativeFontShapePayload& payload,
		NativeFontVanillaLayoutDrawToken& drawToken, bool& drawTokenHit)
	{
		drawTokenHit = false;
		const VanillaLayoutDrawTokenMismatch mismatch =
			MatchVanillaLayoutDrawToken(shape, shader, payload, drawToken);
		if (mismatch == VanillaLayoutDrawTokenMismatch::None)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutDrawTokenHit);
			RecordPriorGenerationDeclarationUse(
				drawToken.priorGenerationDeclaration);
			drawTokenHit = true;
			return true;
		}

		RecordVanillaLayoutDrawTokenMiss(mismatch);
		const bool wasCertified = drawToken.everCertified;
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		if (!renderer)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutDrawTokenRejected);
			return false;
		}
		VanillaLayoutRendererLockScope rendererLock(renderer);
		drawToken.Invalidate();
		VanillaLayoutReadySnapshot snapshot;
		if (!ValidateNativeFontVanillaLayoutShapeReadiness(
			shape, shader, true, &snapshot))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutDrawTokenRejected);
			return false;
		}

		const NativeFontPayloadTemplate* artifact = nullptr;
		const NativeFontPacketTemplate* packet = nullptr;
		UInt32 uploadedByteOffset = 0;
		UInt32 uploadedByteCount = 0;
		if (!UploadVanillaLayoutPayload(payload, snapshot, artifact, packet,
			uploadedByteOffset, uploadedByteCount)
			|| !artifact || !packet)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutDrawTokenRejected);
			return false;
		}

		CertifyVanillaLayoutDrawToken(drawToken, snapshot, payload,
			*artifact, *packet, uploadedByteOffset, uploadedByteCount);
		RecordFreeTypePerf(wasCertified
			? FreeTypePerfCounter::VanillaLayoutDrawTokenRecertification
			: FreeTypePerfCounter::VanillaLayoutDrawTokenFirstCertification);
		return true;
	}

	bool ResolveNativeFontRetainedPacketProgram(
		const NativeFontPacketTemplate& packet,
		TileShader* shader, UInt32 generation,
		const NativeFontCompiledPacketCommand*& program)
	{
		program = nullptr;
		NativeTileVtableBlock* block = RecoverNativeVtableBlock(shader);
		NativeShaderProfile* profile = block ? block->profile : nullptr;
		NativeShaderGeneration* owner = profile ? profile->owner : nullptr;
		if (!profile || !owner || profile->shader != shader
			|| owner->id != generation
			|| !GenerationMatchesCurrentDevice(owner))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandRetainedProgramMiss);
			return false;
		}
		const size_t cacheIndex =
			profile->key.writeEffectAlpha ? 1u : 0u;
		NativeFontPacketShaderCacheEntry& packetCache =
			packet.resolvedShaders[cacheIndex];
		NativeShaderProfile* cachedProfile =
			static_cast<NativeShaderProfile*>(
				packetCache.profile.load(std::memory_order_acquire));
		const bool cacheHit = cachedProfile == profile;
		if (!cacheHit)
		{
			if (profile->key.shaderClass != packet.shaderClass
				|| profile->key.quality != packet.quality
				|| profile->key.distanceFieldMethod
					!= packet.distanceFieldMethod
				|| profile->key.staticCompositeLayerMask
					!= packet.staticCompositeLayerMask
				|| profile->key.compositeShiftedShadow
					!= packet.compositeShiftedShadow
				|| profile->key.usesLiveTileRgb
					!= packet.usesLiveTileRgb
				|| std::memcmp(profile->constants.data(),
					packet.constants.data(),
					packet.constants.size() * sizeof(float)) != 0)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandRetainedProgramMiss);
				return false;
			}
			packetCache.profile.store(profile, std::memory_order_release);
		}

		const NativeFontCompiledPacketCommand& retained =
			profile->retainedProgram;
		if (!retained.active || retained.profile != profile
			|| retained.shader != shader || retained.device != owner->device
			|| retained.generation != owner->id)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandRetainedProgramMiss);
			return false;
		}
		RecordFreeTypePerf(cacheHit
			? FreeTypePerfCounter::CommandRetainedProgramHit
			: FreeTypePerfCounter::CommandRetainedProgramMiss);
		program = &retained;
		return true;
	}
}

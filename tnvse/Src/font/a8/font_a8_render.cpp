#include "font_a8_internal.h"

#include "load_config.h"
#include "tnvse.h"

#include "NiGeometryBufferData.hpp"
#include "NiMemory.hpp"
#include "NiTriShapeData.hpp"
#include "NiVBChip.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace fonthook::vectorfont
{
	namespace implementation::font_a8_render {}
	using namespace implementation::font_a8_render;

	namespace implementation::font_a8_render
	{
		inline constexpr UInt32 kCanonicalArrayCount = 1;
		inline constexpr size_t kVirtualStockEstimatedShapeBytes = 1024;

		A8State s_a8State;

		void InitializeA8MetadataIdentity(
			A8ShapeMetadata& metadata, const NiTriShape* shape)
		{
			A8State& state = State();
			metadata.allocationId =
				state.nextMetadataAllocationId.fetch_add(
					1, std::memory_order_relaxed);
			if (!metadata.allocationId)
			{
				metadata.allocationId =
					state.nextMetadataAllocationId.fetch_add(
						1, std::memory_order_relaxed);
			}
			metadata.selfIdentity = &metadata;
			metadata.shapeIdentity = shape;
		}

		A8ShapeMetadataEntry MakeA8MetadataEntry(
			A8ShapeMetadataPtr metadata)
		{
			A8ShapeMetadataEntry entry;
			if (metadata)
			{
				entry.allocationId = metadata->allocationId;
				entry.selfIdentity = metadata->selfIdentity;
				entry.shapeIdentity = metadata->shapeIdentity;
			}
			entry.metadata = std::move(metadata);
			return entry;
		}

		bool IsFiniteColor(const NiColorA& color)
		{
			return std::isfinite(color.r) && std::isfinite(color.g)
				&& std::isfinite(color.b) && std::isfinite(color.a);
		}

		bool IsFiniteBound(const NiBound& bound)
		{
			return std::isfinite(bound.m_kCenter.x)
				&& std::isfinite(bound.m_kCenter.y)
				&& std::isfinite(bound.m_kCenter.z)
				&& std::isfinite(bound.m_fRadius) && bound.m_fRadius >= 0.0f;
		}

		bool IsFiniteVertex(const NativeA8GpuVertex& vertex)
		{
			return std::isfinite(vertex.x) && std::isfinite(vertex.y)
				&& std::isfinite(vertex.z) && std::isfinite(vertex.u)
				&& std::isfinite(vertex.v)
				// Coverage and precomposed ARGB profiles do not carry a distance
				// field, so zero is their canonical spread. Profile-specific
				// validation below still requires a positive spread for MTSDF.
				&& std::isfinite(vertex.sdfSpread) && vertex.sdfSpread >= 0.0f
				&& std::isfinite(vertex.distanceParameterScale)
				&& vertex.distanceParameterScale >= 1.0f
				&& std::isfinite(vertex.layerMask)
				&& vertex.layerMask >= 1.0f && vertex.layerMask <= 15.0f
				&& vertex.glyphU0 <= vertex.glyphU1
				&& vertex.glyphV0 <= vertex.glyphV1;
		}

		bool RejectA8Shape(const char* reason)
		{
			if (g_bEnableFreeTypeFontRenderingLog
				&& State().shapeValidationFailureLogCount++
					< kMaximumShapeValidationFailureLogs)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_a8_diag: rejected shape contract=one-glyph-composite-quad-v13 reason=%s",
					reason ? reason : "unknown");
			}
			return false;
		}

		bool ValidateA8Shape(NiTriShape* shape,
			const A8EffectShapeConfig* effectConfig,
			const A8ShapeColorContract* colorContract,
			const NativeA8PayloadTemplate* payloadTemplate)
		{
			if (!shape || !colorContract || !payloadTemplate)
				return RejectA8Shape("missing-shape-color-or-text-artifact");
			if (colorContract->abiVersion
				!= A8ShapeColorContract::kTileUniformColorAbi)
			{
				return RejectA8Shape("color-contract-abi-mismatch");
			}
			if (!IsFiniteColor(colorContract->minimumModifier)
				|| !IsFiniteColor(colorContract->maximumModifier))
			{
				return RejectA8Shape("non-finite-color-contract");
			}

			NiTriShapeData* data = shape->GetModelData();
			if (!data || !data->m_usVertices || !data->m_usTriangles
				|| !data->m_pkVertex || !data->m_pusTriList)
			{
				return RejectA8Shape("missing-geometry-data");
			}
			if (!data->m_pkColor)
				return RejectA8Shape("missing-base-vertex-color-stream");
			for (UInt32 index = 0; index < data->m_usVertices; ++index)
			{
				if (!IsFiniteColor(data->m_pkColor[index]))
					return RejectA8Shape("non-finite-base-vertex-color");
			}

			const bool sealed =
				HasNativeA8PayloadValidationSeal(*payloadTemplate);
			RecordFreeTypePerf(sealed
				? FreeTypePerfCounter::NativeRegistrationArtifactSealed
				: FreeTypePerfCounter::NativeRegistrationArtifactFallback);
			if (!sealed)
			{
				// Fail closed for any legacy or accidentally unsealed producer.  The
				// normal builder seals the same immutable fields once before publishing
				// the shared_ptr<const>, eliminating this O(vertex + packet) work from
				// registration without weakening the fallback contract.
				if (!payloadTemplate->quadCount
					|| payloadTemplate->quadCount > kNativeA8MaximumQuads
					|| payloadTemplate->gpuVertices.size()
						< static_cast<size_t>(payloadTemplate->quadCount) * 4u
					|| (payloadTemplate->gpuVertices.size() & 3u)
					|| payloadTemplate->gpuVertices.size() / 4u
						> kNativeA8MaximumQuads
					|| payloadTemplate->packets.empty()
					|| payloadTemplate->pageCount
						!= payloadTemplate->atlasProperties.size()
					|| payloadTemplate->pageCount
						!= payloadTemplate->atlasTextures.size()
					|| !IsFiniteBound(payloadTemplate->bound))
				{
					return RejectA8Shape("invalid-text-artifact");
				}
				if (!std::all_of(payloadTemplate->gpuVertices.begin(),
					payloadTemplate->gpuVertices.end(), IsFiniteVertex))
				{
					return RejectA8Shape(
						"non-finite-text-artifact-vertex");
				}
				for (const NativeA8PacketTemplate& packet
					: payloadTemplate->packets)
				{
					const UInt64 vertexEnd =
						static_cast<UInt64>(packet.firstVertex)
						+ packet.vertexCount;
					if (!packet.vertexCount || (packet.firstVertex & 3u)
						|| (packet.vertexCount & 3u)
						|| vertexEnd > payloadTemplate->gpuVertices.size()
						|| packet.layer > 3 || !IsFiniteBound(packet.bound)
						|| !std::all_of(packet.constants.begin(),
							packet.constants.end(), [](float value)
							{ return std::isfinite(value); }))
					{
						return RejectA8Shape(
							"invalid-text-artifact-packet");
					}
				}
				for (const NativeA8PacketTemplate& packet
					: payloadTemplate->compositePackets)
				{
					const UInt64 vertexEnd =
						static_cast<UInt64>(packet.firstVertex)
						+ packet.vertexCount;
					if (!packet.vertexCount || (packet.firstVertex & 3u)
						|| (packet.vertexCount & 3u)
						|| vertexEnd > payloadTemplate->gpuVertices.size()
						|| packet.layer > 3 || !IsFiniteBound(packet.bound)
						|| packet.shaderClass
							!= NativeA8ShaderClass::Composite
						|| packet.staticCompositeLayerMask > 15u
						|| !std::all_of(packet.constants.begin(),
							packet.constants.end(), [](float value)
							{ return std::isfinite(value); }))
					{
						return RejectA8Shape("invalid-composite-packet");
					}
				}
			}
			if (!effectConfig || !effectConfig->enabled)
				return RejectA8Shape("native-route-requires-enabled-profile");
			const UInt32 profileKinds =
				(effectConfig->shaderEffects ? 1u : 0u)
				+ (effectConfig->bakedCoverage ? 1u : 0u)
				+ (effectConfig->precomposedArgb ? 1u : 0u);
			if (profileKinds != 1u)
				return RejectA8Shape("native-route-profile-kind-conflict");
			const bool distanceFieldProfile = effectConfig->shaderEffects;

			const std::array<float, 16> scalarValues = {
				effectConfig->rasterScale,
				effectConfig->inverseAtlasWidth,
				effectConfig->inverseAtlasHeight,
				effectConfig->sdfSpreadPixels,
				effectConfig->shadowBlurPixels,
				effectConfig->shadowPower,
				effectConfig->shadowGlowAlpha,
				effectConfig->shadowOutlineAlpha,
				effectConfig->shadowOffsetX,
				effectConfig->shadowOffsetY,
				effectConfig->shadowOffsetRasterScale,
				effectConfig->glowInnerPixels,
				effectConfig->glowOuterPixels,
				effectConfig->glowPower,
				effectConfig->outlineWidthPixels,
				effectConfig->outlineSoftnessPixels
			};
			if (!std::all_of(scalarValues.begin(), scalarValues.end(),
				[](float value) { return std::isfinite(value); }))
			{
				return RejectA8Shape("non-finite-effect-configuration");
			}
			if (effectConfig->rasterScale <= 0.0f)
				return RejectA8Shape("invalid-effect-raster-scale");
			if (effectConfig->shadowGlowAlpha < 0.0f
				|| effectConfig->shadowGlowAlpha > 1.0f
				|| effectConfig->shadowOutlineAlpha < 0.0f
				|| effectConfig->shadowOutlineAlpha > 1.0f)
			{
				return RejectA8Shape("invalid-shadow-component-alpha");
			}
			if (static_cast<UInt32>(effectConfig->quality)
				> static_cast<UInt32>(EffectQuality::High))
			{
				return RejectA8Shape("invalid-effect-quality");
			}

			const UInt64 availableVertices = payloadTemplate->gpuVertices.size();
			const UInt64 availableIndices =
				static_cast<UInt64>(payloadTemplate->quadCount) * 6u;
			UInt64 previousVertexEnd = 0;
			UInt64 previousIndexEnd = 0;
			UInt32 previousLayerRank = 0;
			bool firstRange = true;
			bool haveFill = false;
			if (effectConfig->atlasTextures.size()
				!= effectConfig->atlasInverseSizes.size())
			{
				return RejectA8Shape("atlas-page-metadata-size-mismatch");
			}
			if (!effectConfig->atlasProperties.empty()
				&& effectConfig->atlasProperties.size()
					!= effectConfig->atlasTextures.size())
			{
				return RejectA8Shape("atlas-page-property-size-mismatch");
			}
			if (effectConfig->atlasProperties.empty()
				|| payloadTemplate->pageCount != effectConfig->atlasTextures.size())
			{
				return RejectA8Shape("text-artifact-page-count-mismatch");
			}
			if (!sealed)
			{
				for (const NativeA8PacketTemplate& packet
					: payloadTemplate->packets)
				{
					if (packet.atlasPage >= effectConfig->atlasTextures.size())
					{
						return RejectA8Shape(
							"text-artifact-packet-page-out-of-bounds");
					}
				}
				for (const NativeA8PacketTemplate& packet
					: payloadTemplate->compositePackets)
				{
					if (packet.atlasPage >= effectConfig->atlasTextures.size())
					{
						return RejectA8Shape(
							"composite-packet-page-out-of-bounds");
					}
				}
			}
			for (const NiPoint2& inverseSize : effectConfig->atlasInverseSizes)
			{
				if (!std::isfinite(inverseSize.x) || !std::isfinite(inverseSize.y))
					return RejectA8Shape("non-finite-atlas-inverse-size");
			}
			for (const A8DrawRange& range : effectConfig->ranges)
			{
				if (range.layer > 3 || !range.vertexCount || !range.primitiveCount
					|| (range.firstVertex & 3u) || (range.vertexCount & 3u)
					|| (range.startIndex % 6u) || (range.primitiveCount & 1u)
					|| range.vertexCount / 4u != range.primitiveCount / 2u
					|| !IsFiniteColor(range.layerColorModifier)
					|| !std::isfinite(range.sdfSpreadPixels)
					|| (distanceFieldProfile
						&& range.sdfSpreadPixels <= 0.0f)
					|| (!distanceFieldProfile
						&& range.sdfSpreadPixels < 0.0f)
					|| !std::isfinite(range.sourceToLogicalScale)
					|| range.sourceToLogicalScale <= 0.0f
					|| range.sourceToLogicalScale > 1.0f)
				{
					return RejectA8Shape("invalid-draw-range");
				}
				if (!effectConfig->atlasTextures.empty()
					&& (range.atlasPage >= effectConfig->atlasTextures.size()
						|| !effectConfig->atlasTextures[range.atlasPage]))
				{
					return RejectA8Shape("invalid-atlas-page");
				}
				const UInt64 vertexEnd = static_cast<UInt64>(range.firstVertex)
					+ range.vertexCount;
				const UInt64 indexEnd = static_cast<UInt64>(range.startIndex)
					+ static_cast<UInt64>(range.primitiveCount) * 3;
				const UInt32 layerRank = GetA8LayerDrawRank(range.layer);
				if (vertexEnd > availableVertices || indexEnd > availableIndices)
					return RejectA8Shape("draw-range-out-of-bounds");
				if (!firstRange && (layerRank < previousLayerRank
					|| (layerRank == previousLayerRank
						&& (range.firstVertex < previousVertexEnd
							|| range.startIndex < previousIndexEnd))))
				{
					return RejectA8Shape("draw-ranges-not-layer-monotonic");
				}
				if (distanceFieldProfile != range.usesSdf)
					return RejectA8Shape(distanceFieldProfile
						? "distance-field-range-without-sdf"
						: effectConfig->precomposedArgb
							? "argb-range-marked-as-sdf"
							: "coverage-range-marked-as-sdf");
				if (distanceFieldProfile
					&& range.sdfSpreadPixels <= 0.0f)
				{
					return RejectA8Shape(
						"distance-field-range-without-positive-spread");
				}
				haveFill = haveFill || range.layer == 3;
				previousLayerRank = layerRank;
				previousVertexEnd = vertexEnd;
				previousIndexEnd = indexEnd;
				firstRange = false;
			}
			return haveFill || RejectA8Shape("missing-fill-range");
		}

		bool ValidateVirtualStockShellShape(NiTriShape* shape)
		{
			NiTriShapeData* data =
				shape ? shape->GetModelData() : nullptr;
			if (!data || data->m_usVertices < 4
				|| data->m_usTriangles < 2 || !data->m_pkVertex
				|| !data->m_pkTexture || !data->m_pkColor
				|| !data->m_pusTriList)
			{
				return RejectA8Shape(
					"invalid-virtual-stock-shell");
			}
			for (UInt32 index = 0; index < 4; ++index)
			{
				const NiPoint3& position = data->m_pkVertex[index];
				const NiPoint2& texture = data->m_pkTexture[index];
				if (!std::isfinite(position.x)
					|| !std::isfinite(position.y)
					|| !std::isfinite(position.z)
					|| !std::isfinite(texture.x)
					|| !std::isfinite(texture.y)
					|| !IsFiniteColor(data->m_pkColor[index]))
				{
					return RejectA8Shape(
						"non-finite-virtual-stock-shell");
				}
			}
			return true;
		}

		bool AllocateVirtualStockBindingBuffer(
			VirtualStockSlotBinding& slot)
		{
			if (slot.bindingBuffer && slot.bindingChip
				&& slot.bindingStride && slot.bindingChipMemory)
			{
				return true;
			}
			NiGeometryBufferData* buffer = NiNew<NiGeometryBufferData>();
			UInt32* stride = NiAlloc<UInt32>(1);
			void* chipMemory =
				NiAlloc(sizeof(NiVBChip*) + sizeof(NiVBChip));
			if (!buffer || !stride || !chipMemory)
			{
				if (buffer)
					NiDelete(buffer, sizeof(NiGeometryBufferData));
				if (stride)
					NiFree(stride);
				if (chipMemory)
					NiFree(chipMemory);
				return false;
			}

			ThisStdCall<void>(kGeometryBufferDataConstructor, buffer);
			std::memset(chipMemory, 0,
				sizeof(NiVBChip*) + sizeof(NiVBChip));
			auto** chips = static_cast<NiVBChip**>(chipMemory);
			NiVBChip* chip = reinterpret_cast<NiVBChip*>(
				static_cast<UInt8*>(chipMemory) + sizeof(NiVBChip*));
			chips[0] = chip;
			*stride = sizeof(NativeA8GpuVertex);
			buffer->m_uiStreamCount = 1;
			buffer->m_puiVertexStride = stride;
			buffer->m_ppkVBChip = chips;
			buffer->m_eType = D3DPT_TRIANGLELIST;
			buffer->m_uiNumArrays = kCanonicalArrayCount;
			buffer->m_pusArrayLengths = nullptr;
			buffer->m_pusIndexArray = nullptr;
			chip->m_uiIndex = 0;

			slot.bindingBuffer = buffer;
			slot.bindingChip = chip;
			slot.bindingStride = stride;
			slot.bindingChipMemory = chipMemory;
			return true;
		}

		bool IsVirtualStockBindingConfigured(
			const VirtualStockSlotBinding& slot,
			const NativeA8VirtualStockPacketBinding& source,
			TileShader* shader)
		{
			const NiTriShapeData* data = slot.shape
				? slot.shape->GetModelData() : nullptr;
			const NiGeometryBufferData* buffer = slot.bindingBuffer;
			const NiVBChip* chip = slot.bindingChip;
			if (!slot.bound || !slot.shape || !data || !buffer || !chip
				|| !slot.bindingStride || !slot.bindingChipMemory
				|| !source.active || !shader
				|| (source.vertexCount & 3u))
			{
				return false;
			}
			const UInt32 quads = source.vertexCount / 4u;
			return data->m_pkBuffData == buffer
				&& slot.shape->GetShader() == shader
				&& buffer->m_uiFlags == 0
				&& !buffer->m_pkGeometryGroup
				&& buffer->m_uiFVF == 0
				&& buffer->m_hDeclaration == source.declaration
				&& !buffer->m_bSoftwareVP
				&& buffer->m_uiVertCount == source.vertexCount
				&& buffer->m_uiMaxVertCount == source.vertexCount
				&& buffer->m_uiStreamCount == 1
				&& buffer->m_puiVertexStride == slot.bindingStride
				&& buffer->m_puiVertexStride[0]
					== sizeof(NativeA8GpuVertex)
				&& buffer->m_ppkVBChip
				&& buffer->m_ppkVBChip[0] == chip
				&& buffer->m_uiIndexCount == quads * 6u
				&& buffer->m_uiIBSize == source.indexBytes
				&& buffer->m_pkIB == source.indexBuffer
				&& buffer->m_uiBaseVertexIndex == source.baseVertex
				&& buffer->m_eType == D3DPT_TRIANGLELIST
				&& buffer->m_uiTriCount == quads * 2u
				&& buffer->m_uiMaxTriCount == quads * 2u
				&& buffer->m_uiNumArrays == kCanonicalArrayCount
				&& !buffer->m_pusArrayLengths
				&& !buffer->m_pusIndexArray
				&& chip->m_uiIndex == 0
				&& chip->m_pkVB == source.vertexBuffer
				&& chip->m_uiOffset == 0
				&& chip->m_uiLockFlags == 0
				&& chip->m_uiSize
					== source.vertexCount
						* sizeof(NativeA8GpuVertex);
		}

		void ClearVirtualStockGpuFields(VirtualStockSlotBinding& slot)
		{
			if (slot.bindingChip)
			{
				slot.bindingChip->m_pkVB = nullptr;
				slot.bindingChip->m_uiOffset = 0;
				slot.bindingChip->m_uiLockFlags = 0;
				slot.bindingChip->m_uiSize = 0;
			}
			if (slot.bindingBuffer)
			{
				// The ring owns these COM objects. Never release them from the
				// shape-owned descriptor.
				slot.bindingBuffer->m_hDeclaration = nullptr;
				slot.bindingBuffer->m_pkIB = nullptr;
				slot.bindingBuffer->m_uiVertCount = 0;
				slot.bindingBuffer->m_uiMaxVertCount = 0;
				slot.bindingBuffer->m_uiIndexCount = 0;
				slot.bindingBuffer->m_uiIBSize = 0;
				slot.bindingBuffer->m_uiBaseVertexIndex = 0;
				slot.bindingBuffer->m_uiTriCount = 0;
				slot.bindingBuffer->m_uiMaxTriCount = 0;
			}
			slot.generation = 0;
			slot.resourceSerial = 0;
			slot.uploadEpoch = 0;
			slot.atlasTextureEpoch = 0;
			slot.baseVertex = 0;
			slot.vertexCount = 0;
			slot.staticResident = false;
			slot.bound = false;
		}

		void RestoreVirtualStockSlot(VirtualStockSlotBinding& slot)
		{
			NiTriShapeData* data = slot.shape
				? slot.shape->GetModelData() : nullptr;
			if (data && data->m_pkBuffData == slot.bindingBuffer)
				data->m_pkBuffData = slot.shellBuffer;
			if (slot.shape)
				slot.shape->SetShader(slot.shellShader);
			ClearVirtualStockGpuFields(slot);
		}

		void DestroyVirtualStockBindingBuffer(
			VirtualStockSlotBinding& slot)
		{
			RestoreVirtualStockSlot(slot);
			if (slot.bindingBuffer)
			{
				NiGeometryBufferData* buffer = slot.bindingBuffer;
				// E8F0F0 releases geometry-group chips, the declaration, the IB,
				// and both stream arrays. This descriptor never owns its ring COM
				// bindings and its arrays are released below, so detach every
				// destructor-owned field before calling the verified non-deleting
				// destructor. Virtual slot 1 is ContainsVertexData(parameter) and
				// calling it as DeleteThis() corrupts ESP because it returns with
				// retn 4.
				buffer->m_pkGeometryGroup = nullptr;
				buffer->m_hDeclaration = nullptr;
				buffer->m_uiStreamCount = 0;
				buffer->m_puiVertexStride = nullptr;
				buffer->m_ppkVBChip = nullptr;
				buffer->m_pkIB = nullptr;
				ThisStdCall<void>(
					kGeometryBufferDataDestructor, buffer);
				NiDelete(buffer, sizeof(NiGeometryBufferData));
			}
			if (slot.bindingStride)
				NiFree(slot.bindingStride);
			if (slot.bindingChipMemory)
				NiFree(slot.bindingChipMemory);
			slot.bindingBuffer = nullptr;
			slot.bindingChip = nullptr;
			slot.bindingStride = nullptr;
			slot.bindingChipMemory = nullptr;
		}

		FreeTypePerfCounter VirtualStockFallbackCounter(
			NativeA8FallbackReason reason)
		{
			switch (reason)
			{
			case NativeA8FallbackReason::ShaderGeneration:
				return FreeTypePerfCounter::VirtualStockFallbackShader;
			case NativeA8FallbackReason::AtlasGeneration:
			case NativeA8FallbackReason::PageTexture:
			case NativeA8FallbackReason::PropertySync:
				return FreeTypePerfCounter::VirtualStockFallbackAtlas;
			case NativeA8FallbackReason::DeviceReset:
				return FreeTypePerfCounter::VirtualStockFallbackGeneration;
			case NativeA8FallbackReason::PacketBuild:
				return FreeTypePerfCounter::VirtualStockFallbackTopology;
			default:
				return FreeTypePerfCounter::VirtualStockFallbackResource;
			}
		}

		void SetVirtualStockFacadeMode(VirtualStockShapeGroup& group,
			NativeA8FallbackReason reason)
		{
			group.commandBuildValidationToken.store(
				0, std::memory_order_release);
			for (VirtualStockSlotBinding& slot : group.slots)
				RestoreVirtualStockSlot(slot);
			group.preparedValidationToken = 0;
			group.preflightValidationToken = 0;
			group.preparedGeneration = 0;
			group.preparedAtlasTextureEpoch = 0;
			group.directDrawCount.store(0, std::memory_order_release);
			group.commandValidationToken.store(
				0, std::memory_order_release);
			group.commandSpanIndex.store(
				kInvalidNativeA8CommandIndex,
				std::memory_order_release);
			group.commandLeaderSlot.store(
				0, std::memory_order_release);
			group.frameMode.store(VirtualStockFrameMode::Facade,
				std::memory_order_release);
			RecordFreeTypePerf(
				FreeTypePerfCounter::VirtualStockFacadeFallback);
			RecordFreeTypePerf(VirtualStockFallbackCounter(reason));
		}

		void SetVirtualStockSingletonFacadeMode(
			const A8ShapeMetadata& metadata, NativeA8FallbackReason reason)
		{
			VirtualStockSingletonState* singleton =
				GetVirtualStockSingletonState(metadata);
			if (!singleton)
				return;
			singleton->commandBuildValidationToken.store(
				0, std::memory_order_release);
			RestoreVirtualStockSlot(singleton->slot);
			singleton->preparedValidationToken = 0;
			singleton->preflightValidationToken = 0;
			singleton->preparedGeneration = 0;
			singleton->preparedAtlasTextureEpoch = 0;
			singleton->directDrawCount.store(0, std::memory_order_release);
			singleton->commandValidationToken.store(
				0, std::memory_order_release);
			singleton->commandVirtualSinglePacketIndex.store(
				kInvalidNativeA8CommandIndex,
				std::memory_order_release);
			singleton->frameMode.store(VirtualStockFrameMode::Facade,
				std::memory_order_release);
			RecordFreeTypePerf(
				FreeTypePerfCounter::VirtualStockFacadeFallback);
			RecordFreeTypePerf(VirtualStockFallbackCounter(reason));
		}

	}

	A8State& State()
	{
		return s_a8State;
	}

	bool NeedsScaledFillSampling(const NiTriShape* shape)
	{
		if (!shape)
			return false;
		const float worldScale = std::abs(shape->m_kWorld.m_fScale);
		return std::isfinite(worldScale) && std::abs(worldScale - 1.0f) > 0.001f;
	}

	void FinalizeA8RendererDetection()
	{
		const bool accumulatorReady = HookNativeA8Accumulator();
		const bool immediateRouteReady = HookRenderPassImmediately();
		const bool shaderReady = InitializeNativeA8Renderer(true, true);
		const bool nativeReady =
			accumulatorReady && immediateRouteReady && shaderReady;
		SynchronizePersistentFontCacheRoute(ResolveFontAtlasRoute(
			nativeReady, g_bEnableFreeTypeFontAggressivePerformanceMode));
		gLog.FormattedMessage(
			"tnvse_freetype_native: initialization nativeReady=%u accumulator=%u alphaRenderHook=%u sortAnchorHook=%u immediateRoute=%u shader=%u",
			nativeReady ? 1 : 0,
			accumulatorReady ? 1 : 0,
			State().renderAlphaGeometryHookInstalled ? 1 : 0,
			State().tileSortAnchorHookInstalled ? 1 : 0,
			immediateRouteReady ? 1 : 0,
			shaderReady ? 1 : 0);
	}

	void HandleA8RendererMainLoop()
	{
		if (!g_bEnableFreeTypeFontRendering)
			return;
		HandleNativeA8RendererMainLoop();
		HookNativeA8Accumulator();
		HookRenderPassImmediately();
		EnforceCpuMemoryBudget("main-loop");
	}

	void HandleA8ShaderLoaderMessage(UInt32 messageType)
	{
		if (messageType == kShaderRefreshMessage)
			HandleNativeA8ShaderLoaderMessage(messageType);
	}

	bool IsA8RendererAvailable()
	{
		if (!g_bEnableFreeTypeFontRendering)
			return false;
		// The normal registration path only needs readback of the already
		// installed chain and the published device generation.  Preserve the
		// complete install/retry/conflict path whenever any identity changed.
		if (IsNativeA8RegistrationHookChainCurrent()
			&& IsA8RenderPassImmediatelyHookCurrent()
			&& IsNativeA8RendererAvailable())
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::NativeRegistrationHookFast);
			return true;
		}
		RecordFreeTypePerf(
			FreeTypePerfCounter::NativeRegistrationHookSlow);
		return HookNativeA8Accumulator() && HookRenderPassImmediately()
			&& InitializeNativeA8Renderer(false, false);
	}

	bool ResolveA8EffectQuality(EffectQuality requested, EffectQuality& resolved)
	{
		if (static_cast<UInt32>(requested) > static_cast<UInt32>(EffectQuality::High)
			|| !IsA8RendererAvailable())
		{
			return false;
		}
		resolved = requested;
		return true;
	}

	bool PrepareA8AtlasShape(Font& font, NiTriShape* shape, UInt32 fontId,
		UInt32 glyphCount, UInt32 quadCount, const A8EffectShapeConfig* effectConfig,
		const A8ShapeColorContract* colorContract,
		NativeA8PayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin)
	{
		FreeTypePerfScope perf(
			FreeTypePerfPhase::NativeRegistration);
		if (!IsA8RendererAvailable()
			|| !payloadTemplate || payloadTemplate->quadCount != quadCount
			|| !ValidateA8Shape(shape, effectConfig, colorContract,
				payloadTemplate.get())
			|| !InitializeA8TriShapeVtable(shape))
		{
			return false;
		}
		auto metadata = std::make_shared<A8ShapeMetadata>();
		InitializeA8MetadataIdentity(*metadata, shape);
		metadata->fontId = fontId;
		metadata->glyphCount = glyphCount;
		metadata->quadCount = quadCount;
		metadata->vertexCount = static_cast<UInt32>(
			payloadTemplate->gpuVertices.size());
		metadata->primitiveCount = payloadTemplate->quadCount * 2u;
		metadata->indexCount = payloadTemplate->quadCount * 6u;
		if (colorContract)
			metadata->colorContract = *colorContract;
		if (!InitializeNativeA8ShapePayload(font, shape, *metadata,
			std::move(payloadTemplate), geometryOrigin, metadata->nativePayload))
			return false;
		metadata->cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
			sizeof(A8ShapeMetadata)
				+ metadata->nativePayload.packetShaders.capacity()
					* sizeof(TileShader*)
				+ metadata->nativePayload.packetPrograms.capacity()
					* sizeof(const NativeA8CompiledPacketCommand*)
				+ metadata->nativePayload.preflightAtlasTextures.capacity()
					* sizeof(const void*)
				+ GetNativeA8TileRetainedCapacityBytes(
					metadata->nativePayload)
				+ sizeof(A8ShapeMetadataEntry) + 6u * sizeof(void*));
		{
			A8State& state = State();
			std::lock_guard<std::mutex> lock(state.metadataMutex);
			state.metadataGenerations[GetMetadataGenerationSlot(shape)].fetch_add(
				1, std::memory_order_release);
			A8ShapeMetadataPtr publishedMetadata = std::move(metadata);
			state.shapeMetadata[shape] =
				MakeA8MetadataEntry(std::move(publishedMetadata));
		}
		*reinterpret_cast<void***>(shape) = &State().triShapeVtable[1];
		return true;
	}

	bool PrepareVirtualStockA8Singleton(Font& font, NiTriShape* shape,
		UInt32 fontId, UInt32 glyphCount, UInt32 quadCount,
		const A8EffectShapeConfig* effectConfig,
		const A8ShapeColorContract* colorContract,
		NativeA8PayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin, bool useCompositeTopology)
	{
		FreeTypePerfScope perf(FreeTypePerfPhase::NativeRegistration);
		if (!IsA8RendererAvailable() || !shape || !payloadTemplate
			|| payloadTemplate->quadCount != quadCount)
		{
			return false;
		}
		const std::vector<NativeA8PacketTemplate>& topology =
			GetNativeA8Packets(*payloadTemplate, useCompositeTopology);
		if (topology.size() != 1
			|| !ValidateA8Shape(shape, effectConfig, colorContract,
				payloadTemplate.get())
			|| !InitializeA8TriShapeVtable(shape))
		{
			return false;
		}

		const size_t estimatedBytes =
			sizeof(VirtualStockSingletonMetadata)
			+ sizeof(A8ShapeMetadataEntry)
			+ kVirtualStockEstimatedShapeBytes + 6u * sizeof(void*);
		if (GetCpuMemoryCategoryHeadroom(CpuMemoryCategory::RuntimeMetadata,
				estimatedBytes) < estimatedBytes)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::VirtualStockFallbackCpuBudget);
			return false;
		}

		auto metadata = std::make_shared<VirtualStockSingletonMetadata>();
		InitializeA8MetadataIdentity(*metadata, shape);
		metadata->fontId = fontId;
		metadata->glyphCount = glyphCount;
		metadata->quadCount = quadCount;
		metadata->vertexCount = static_cast<UInt32>(
			payloadTemplate->gpuVertices.size());
		metadata->primitiveCount = payloadTemplate->quadCount * 2u;
		metadata->indexCount = payloadTemplate->quadCount * 6u;
		if (colorContract)
			metadata->colorContract = *colorContract;
		metadata->backend = FreeTypeShapeBackend::VirtualStockSingleton;
		metadata->virtualStockPrimary = true;

		VirtualStockSingletonState& singleton = metadata->singleton;
		singleton.useCompositeTopology = useCompositeTopology;
		singleton.slot.shape = shape;
		singleton.slot.packetIndex = 0;
		singleton.slot.shellShader = shape->GetShader();
		NiTriShapeData* data = shape->GetModelData();
		singleton.slot.shellBuffer = data ? data->m_pkBuffData : nullptr;
		if (!InitializeNativeA8ShapePayload(font, shape, *metadata,
			payloadTemplate, geometryOrigin, metadata->nativePayload))
		{
			return false;
		}
		metadata->cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
			estimatedBytes
				+ metadata->nativePayload.packetShaders.capacity()
					* sizeof(TileShader*)
				+ metadata->nativePayload.packetPrograms.capacity()
					* sizeof(const NativeA8CompiledPacketCommand*)
				+ metadata->nativePayload.preflightAtlasTextures.capacity()
					* sizeof(const void*)
				+ GetNativeA8TileRetainedCapacityBytes(
					metadata->nativePayload));

		{
			A8State& state = State();
			std::lock_guard<std::mutex> lock(state.metadataMutex);
			state.metadataGenerations[GetMetadataGenerationSlot(shape)].fetch_add(
				1, std::memory_order_release);
			A8ShapeMetadataPtr publishedMetadata = metadata;
			state.shapeMetadata[shape] =
				MakeA8MetadataEntry(std::move(publishedMetadata));
		}
		*reinterpret_cast<void***>(shape) = &State().triShapeVtable[1];
		RecordFreeTypePerf(FreeTypePerfCounter::VirtualStockSingleton);
		RecordFreeTypePerf(FreeTypePerfCounter::VirtualStockShape);
		return true;
	}

	bool PrepareVirtualStockA8ShapeGroup(Font& font,
		const std::vector<NiTriShape*>& shapes, UInt32 primarySlot,
		UInt32 fontId, UInt32 glyphCount, UInt32 quadCount,
		const A8EffectShapeConfig* effectConfig,
		const A8ShapeColorContract* colorContract,
		NativeA8PayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin, bool useCompositeTopology)
	{
		if (shapes.size() == 1 && primarySlot == 0)
		{
			return PrepareVirtualStockA8Singleton(font, shapes.front(),
				fontId, glyphCount, quadCount, effectConfig, colorContract,
				std::move(payloadTemplate), geometryOrigin,
				useCompositeTopology);
		}
		FreeTypePerfScope perf(FreeTypePerfPhase::NativeRegistration);
		if (!IsA8RendererAvailable() || shapes.size() < 2
			|| shapes.size() > kMaximumVirtualStockShapes
			|| primarySlot >= shapes.size() || !payloadTemplate
			|| payloadTemplate->quadCount != quadCount)
		{
			return false;
		}
		const std::vector<NativeA8PacketTemplate>& topology =
			GetNativeA8Packets(*payloadTemplate, useCompositeTopology);
		if (topology.size() != shapes.size())
			return false;
		const size_t estimatedBytes = sizeof(VirtualStockShapeGroup)
			+ shapes.size() * (sizeof(VirtualStockSlotBinding)
				+ sizeof(A8ShapeMetadata) + sizeof(SInt32)
				+ kVirtualStockEstimatedShapeBytes)
			+ (g_bEnableFreeTypeFontCommandBuffer
				? topology.size()
					* (sizeof(NativeA8TileRetainedPacket)
						+ sizeof(NativeA8TileRetainedRun))
				: 0)
			+ 6u * sizeof(void*);
		if (GetCpuMemoryCategoryHeadroom(CpuMemoryCategory::RuntimeMetadata,
				estimatedBytes) < estimatedBytes)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::VirtualStockFallbackCpuBudget);
			return false;
		}

		for (size_t index = 0; index < shapes.size(); ++index)
		{
			NiTriShape* shape = shapes[index];
			const bool shapeValid = index == primarySlot
				? ValidateA8Shape(shape, effectConfig, colorContract,
					payloadTemplate.get())
				: ValidateVirtualStockShellShape(shape);
			if (!shapeValid || !InitializeA8TriShapeVtable(shape))
			{
				return false;
			}
		}

		auto group = std::make_shared<VirtualStockShapeGroup>();
		group->payloadTemplate = payloadTemplate;
		group->primaryShape = shapes[primarySlot];
		group->primarySlot = primarySlot;
		group->useCompositeTopology = useCompositeTopology;
		group->slots.resize(shapes.size());
		if (g_bEnableFreeTypeFontCommandBuffer)
			group->commandBuildCommands.resize(shapes.size());
		group->liveSlotCount = static_cast<UInt32>(shapes.size());
		if (shapes.size() > 1)
			group->registrationItemIndices.assign(shapes.size(), -1);
		group->cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
			sizeof(VirtualStockShapeGroup)
				+ group->slots.capacity() * sizeof(VirtualStockSlotBinding)
				+ group->commandBuildCommands.capacity()
					* sizeof(NativeA8DrawCommand)
				+ group->registrationItemIndices.capacity() * sizeof(SInt32)
				+ shapes.size() * kVirtualStockEstimatedShapeBytes
				+ 6u * sizeof(void*));

		std::vector<std::shared_ptr<A8ShapeMetadata>> metadataEntries;
		metadataEntries.reserve(shapes.size());
		for (size_t index = 0; index < shapes.size(); ++index)
		{
			auto metadata = std::make_shared<A8ShapeMetadata>();
			InitializeA8MetadataIdentity(*metadata, shapes[index]);
			metadata->fontId = fontId;
			metadata->glyphCount = glyphCount;
			metadata->quadCount = quadCount;
			metadata->vertexCount = static_cast<UInt32>(
				payloadTemplate->gpuVertices.size());
			metadata->primitiveCount = payloadTemplate->quadCount * 2u;
			metadata->indexCount = payloadTemplate->quadCount * 6u;
			if (colorContract)
				metadata->colorContract = *colorContract;
			metadata->backend = FreeTypeShapeBackend::VirtualStockNative;
			metadata->virtualStockGroup = group.get();
			metadata->virtualStockOwner = group;
			metadata->virtualStockSlot = static_cast<UInt16>(index);
			metadata->virtualStockPrimary = index == primarySlot;
			metadata->cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
				sizeof(A8ShapeMetadata) + sizeof(A8ShapeMetadataEntry)
					+ 4u * sizeof(void*));

			VirtualStockSlotBinding& slot = group->slots[index];
			slot.shape = shapes[index];
			slot.packetIndex = static_cast<UInt32>(index);
			slot.shellShader = shapes[index]->GetShader();
			NiTriShapeData* data = shapes[index]->GetModelData();
			slot.shellBuffer = data ? data->m_pkBuffData : nullptr;
			metadataEntries.push_back(std::move(metadata));
		}

		A8ShapeMetadata& primary =
			*metadataEntries[primarySlot];
		if (!InitializeNativeA8ShapePayload(font, shapes[primarySlot],
			primary, payloadTemplate, geometryOrigin,
			primary.nativePayload))
		{
			return false;
		}
		primary.cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
			primary.cpuMemory.GetBytes()
				+ primary.nativePayload.packetShaders.capacity()
					* sizeof(TileShader*)
				+ primary.nativePayload.packetPrograms.capacity()
					* sizeof(const NativeA8CompiledPacketCommand*)
				+ primary.nativePayload.preflightAtlasTextures.capacity()
					* sizeof(const void*)
				+ GetNativeA8TileRetainedCapacityBytes(
					primary.nativePayload));
		group->primaryMetadataOwner = metadataEntries[primarySlot];

		{
			A8State& state = State();
			std::lock_guard<std::mutex> lock(state.metadataMutex);
			state.virtualStockGroups[group.get()] = group;
			group->metadataPublished.store(true,
				std::memory_order_release);
			for (size_t index = 0; index < shapes.size(); ++index)
			{
				state.metadataGenerations[
					GetMetadataGenerationSlot(shapes[index])].fetch_add(
						1, std::memory_order_release);
				A8ShapeMetadataPtr publishedMetadata =
					metadataEntries[index];
				state.shapeMetadata[shapes[index]] =
					MakeA8MetadataEntry(
						std::move(publishedMetadata));
			}
		}
		for (NiTriShape* shape : shapes)
			*reinterpret_cast<void***>(shape) = &State().triShapeVtable[1];

		RecordFreeTypePerf(FreeTypePerfCounter::VirtualStockGroup);
		RecordFreeTypePerf(FreeTypePerfCounter::VirtualStockShape,
			static_cast<UInt64>(shapes.size()));
		return true;
	}

	bool PrepareVirtualStockSingletonForSortedFrame(
		const A8ShapeMetadata& metadata, UInt32 generation,
		UInt32 atlasTextureEpoch, UInt64 validationToken)
	{
		VirtualStockSingletonState* singleton =
			GetVirtualStockSingletonState(metadata);
		if (!singleton || !generation || !validationToken)
			return false;
		singleton->commandBuildValidationToken.store(
			0, std::memory_order_release);
		NiTriShape* shape = singleton->slot.shape;
		if (!shape || shape != metadata.shapeIdentity
			|| singleton->frameMode.load(std::memory_order_acquire)
				== VirtualStockFrameMode::Retired)
		{
			return false;
		}

		NativeA8ShapePayload& payload = metadata.nativePayload;
		if (singleton->preflightValidationToken != validationToken
			|| !payload.buildComplete || !payload.payloadTemplate
			|| payload.preparedGeneration != generation
			|| payload.preflightAtlasTextureEpoch != atlasTextureEpoch)
		{
			SetVirtualStockSingletonFacadeMode(metadata,
				NativeA8FallbackReason::ShaderGeneration);
			return false;
		}
		const std::vector<NativeA8PacketTemplate>& packets =
			GetNativeA8Packets(*payload.payloadTemplate,
				payload.useCompositePackets);
		const bool buildCommandView =
			g_bEnableFreeTypeFontCommandBuffer;
		const NativeA8TileRetainedText* retainedText = nullptr;
		if (buildCommandView)
		{
			if (!IsNativeA8TileRetainedTextCurrent(payload, shape,
					generation, atlasTextureEpoch))
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandTileRetainedMiss);
				SetVirtualStockSingletonFacadeMode(metadata,
					NativeA8FallbackReason::PacketBuild);
				return false;
			}
			retainedText = &payload.retainedText;
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandTileRetainedHit);
		}
		if (payload.useCompositePackets
				!= singleton->useCompositeTopology
			|| packets.size() != 1 || payload.packetShaders.size() != 1
			|| (buildCommandView
				&& (!retainedText || retainedText->packets.size() != 1))
			|| !singleton->registrationContiguous
			|| singleton->duplicateRegistration
			|| singleton->registeredSlotCount != 1)
		{
			SetVirtualStockSingletonFacadeMode(metadata,
				NativeA8FallbackReason::PropertySync);
			RecordFreeTypePerf(
				FreeTypePerfCounter::VirtualStockFallbackNoncontiguous);
			return false;
		}

		NativeA8VirtualStockPacketBinding resolved;
		const NativeA8FallbackReason resolveResult =
			ResolveNativeA8VirtualStockPacketBinding(payload, 0, resolved);
		if (resolveResult != NativeA8FallbackReason::None)
		{
			SetVirtualStockSingletonFacadeMode(metadata, resolveResult);
			if (resolveResult == NativeA8FallbackReason::PacketPrepare)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					VirtualStockFallbackStaticNotReady);
			}
			return false;
		}

		VirtualStockSlotBinding& slot = singleton->slot;
		NiTriShapeData* data = shape->GetModelData();
		if (!data || !IsNativeA8VirtualStockPacketAtlasCurrent(
				shape, payload, 0)
			|| !AllocateVirtualStockBindingBuffer(slot))
		{
			SetVirtualStockSingletonFacadeMode(metadata,
				NativeA8FallbackReason::PropertySync);
			return false;
		}
		if (data->m_pkBuffData != slot.bindingBuffer
			&& data->m_pkBuffData != slot.shellBuffer)
		{
			slot.shellBuffer = data->m_pkBuffData;
		}
		if (!slot.bound && shape->GetShader() != slot.shellShader)
			slot.shellShader = shape->GetShader();
		const bool changed = !slot.bound
			|| slot.generation != resolved.generation
			|| slot.resourceSerial != resolved.resourceSerial
			|| slot.uploadEpoch != resolved.uploadEpoch
			|| slot.atlasTextureEpoch != resolved.atlasTextureEpoch
			|| slot.baseVertex != resolved.baseVertex
			|| slot.vertexCount != resolved.vertexCount
			|| slot.staticResident != resolved.staticResident
			|| shape->GetShader() != payload.packetShaders[0];
		const bool needsRebind = changed
			|| !IsVirtualStockBindingConfigured(
				slot, resolved, payload.packetShaders[0]);
		const UInt32 quadCount = resolved.vertexCount / 4u;
		if (needsRebind)
		{
			slot.bindingBuffer->m_uiFlags = 0;
			slot.bindingBuffer->m_pkGeometryGroup = nullptr;
			slot.bindingBuffer->m_uiFVF = 0;
			slot.bindingBuffer->m_hDeclaration = resolved.declaration;
			slot.bindingBuffer->m_bSoftwareVP = false;
			slot.bindingBuffer->m_uiVertCount = resolved.vertexCount;
			slot.bindingBuffer->m_uiMaxVertCount = resolved.vertexCount;
			slot.bindingBuffer->m_uiStreamCount = 1;
			slot.bindingBuffer->m_puiVertexStride[0] =
				sizeof(NativeA8GpuVertex);
			slot.bindingBuffer->m_uiIndexCount = quadCount * 6u;
			slot.bindingBuffer->m_uiIBSize = resolved.indexBytes;
			slot.bindingBuffer->m_pkIB = resolved.indexBuffer;
			slot.bindingBuffer->m_uiBaseVertexIndex = resolved.baseVertex;
			slot.bindingBuffer->m_eType = D3DPT_TRIANGLELIST;
			slot.bindingBuffer->m_uiTriCount = quadCount * 2u;
			slot.bindingBuffer->m_uiMaxTriCount = quadCount * 2u;
			slot.bindingBuffer->m_uiNumArrays = kCanonicalArrayCount;
			slot.bindingBuffer->m_pusArrayLengths = nullptr;
			slot.bindingBuffer->m_pusIndexArray = nullptr;
			slot.bindingChip->m_uiIndex = 0;
			slot.bindingChip->m_pkVB = resolved.vertexBuffer;
			slot.bindingChip->m_uiOffset = 0;
			slot.bindingChip->m_uiLockFlags = 0;
			slot.bindingChip->m_uiSize =
				resolved.vertexCount * sizeof(NativeA8GpuVertex);
			shape->SetShader(payload.packetShaders[0]);
			data->m_pkBuffData = slot.bindingBuffer;
		}
		slot.generation = resolved.generation;
		slot.resourceSerial = resolved.resourceSerial;
		slot.uploadEpoch = resolved.uploadEpoch;
		slot.atlasTextureEpoch = resolved.atlasTextureEpoch;
		slot.baseVertex = resolved.baseVertex;
		slot.vertexCount = resolved.vertexCount;
		slot.staticResident = resolved.staticResident;
		slot.bound = true;
		if (needsRebind)
			RecordFreeTypePerf(FreeTypePerfCounter::VirtualStockRebind);
		if (needsRebind && !IsVirtualStockBindingConfigured(
				slot, resolved, payload.packetShaders[0]))
		{
			SetVirtualStockSingletonFacadeMode(metadata,
				NativeA8FallbackReason::PropertySync);
			return false;
		}

		if (buildCommandView)
		{
			const NativeA8TileRetainedPacket& retained =
				retainedText->packets.front();
			const NativeA8PacketTemplate& packet = *retained.packet;
			NativeA8DrawCommand& command =
				singleton->commandBuildCommand;
			command = {};
			if (!resolved.active
				|| packet.atlasPage >= payload.preflightAtlasTextures.size()
				|| !payload.preflightAtlasTextures[packet.atlasPage]
				|| retained.packetIndex != 0
				|| retained.vertexCount != resolved.vertexCount
				|| !retained.program)
			{
				SetVirtualStockSingletonFacadeMode(metadata,
					NativeA8FallbackReason::PacketBuild);
				return false;
			}
			command.sourceGeometry = shape;
			command.expectedGeometry = shape;
			command.payload = &payload;
			command.packet = &packet;
			command.packetIndex = 0;
			command.atlasTexture =
				payload.preflightAtlasTextures[packet.atlasPage];
			command.program = retained.program;
			command.standardPassLite =
				&retainedText->standardPassLite;
			command.binding.vertexBuffer = resolved.vertexBuffer;
			command.binding.indexBuffer = resolved.indexBuffer;
			command.binding.declaration = resolved.declaration;
			command.binding.baseVertex = resolved.baseVertex;
			command.binding.vertexCount = resolved.vertexCount;
			command.binding.indexBytes = resolved.indexBytes;
			command.binding.generation = resolved.generation;
			command.binding.resourceSerial = resolved.resourceSerial;
			command.binding.uploadEpoch = resolved.uploadEpoch;
			command.binding.staticResident = resolved.staticResident;
			command.binding.active = resolved.active;
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandTileRetainedPacketReuse);
		}

		singleton->preparedValidationToken = validationToken;
		singleton->preparedGeneration = generation;
		singleton->preparedAtlasTextureEpoch = atlasTextureEpoch;
		singleton->directDrawCount.store(0, std::memory_order_release);
		if (buildCommandView)
		{
			singleton->commandBuildValidationToken.store(
				validationToken, std::memory_order_release);
		}
		singleton->frameMode.store(VirtualStockFrameMode::Direct,
			std::memory_order_release);
		RecordFreeTypePerf(resolved.staticResident
			? FreeTypePerfCounter::VirtualStockStaticHit
			: FreeTypePerfCounter::VirtualStockDynamicHit);
		RecordFreeTypePerf(
			FreeTypePerfCounter::VirtualStockSortedPreflightSaved);
		RecordFreeTypePerf(
			FreeTypePerfCounter::VirtualStockProxyPacketSaved);
		return true;
	}

	bool PrepareVirtualStockGroupForSortedFrame(
		const std::shared_ptr<VirtualStockShapeGroup>& group,
		UInt32 generation, UInt32 atlasTextureEpoch,
		UInt64 validationToken)
	{
		if (!group || !generation || !validationToken)
			return false;
		std::lock_guard<std::mutex> lock(group->mutex);
		group->commandBuildValidationToken.store(
			0, std::memory_order_release);
		if (!group->primaryMetadataOwner || !group->primaryShape
			|| group->frameMode.load(std::memory_order_acquire)
				== VirtualStockFrameMode::Retired)
		{
			return false;
		}
		NativeA8ShapePayload& payload =
			group->primaryMetadataOwner->nativePayload;
		if (group->preflightValidationToken != validationToken
			|| !payload.buildComplete || !payload.payloadTemplate
			|| payload.payloadTemplate.get() != group->payloadTemplate.get()
			|| payload.preparedGeneration != generation
			|| payload.preflightAtlasTextureEpoch != atlasTextureEpoch)
		{
			SetVirtualStockFacadeMode(*group,
				NativeA8FallbackReason::ShaderGeneration);
			return false;
		}
		const std::vector<NativeA8PacketTemplate>& packets =
			GetNativeA8Packets(*payload.payloadTemplate,
				payload.useCompositePackets);
		const bool buildCommandView =
			g_bEnableFreeTypeFontCommandBuffer;
		const NativeA8TileRetainedText* retainedText = nullptr;
		if (buildCommandView)
		{
			if (!IsNativeA8TileRetainedTextCurrent(payload,
					group->primaryShape, generation,
					atlasTextureEpoch))
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandTileRetainedMiss);
				SetVirtualStockFacadeMode(*group,
					NativeA8FallbackReason::PacketBuild);
				return false;
			}
			retainedText = &payload.retainedText;
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandTileRetainedHit);
		}
		if (payload.useCompositePackets != group->useCompositeTopology
			|| packets.size() != group->slots.size()
			|| payload.packetShaders.size() != packets.size()
			|| (buildCommandView
				&& (!retainedText
					|| retainedText->packets.size() != packets.size()
					|| group->commandBuildCommands.size()
						!= packets.size())))
		{
			SetVirtualStockFacadeMode(*group,
				NativeA8FallbackReason::PacketBuild);
			return false;
		}
		if (!group->registrationContiguous
			|| group->registeredSlotCount != group->slots.size())
		{
			SetVirtualStockFacadeMode(*group,
				NativeA8FallbackReason::PropertySync);
			RecordFreeTypePerf(
				FreeTypePerfCounter::VirtualStockFallbackNoncontiguous);
			return false;
		}

		// Resolve overwrites every used entry before it is consumed. Keeping the
		// fixed-capacity scratch per thread avoids value-initializing all 64
		// entries for the overwhelmingly singleton virtual-stock workload.
		thread_local std::array<NativeA8VirtualStockPacketBinding,
			kMaximumVirtualStockShapes> resolved;
		bool configurationFailed = false;
		UInt64 staticBindings = 0;
		UInt64 dynamicBindings = 0;
		for (size_t index = 0; index < group->slots.size(); ++index)
		{
			const NativeA8FallbackReason result =
				ResolveNativeA8VirtualStockPacketBinding(
					payload, static_cast<UInt32>(index), resolved[index]);
			if (result != NativeA8FallbackReason::None)
			{
				SetVirtualStockFacadeMode(*group, result);
				if (result == NativeA8FallbackReason::PacketPrepare)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							VirtualStockFallbackStaticNotReady);
				}
				return false;
			}
			if (resolved[index].staticResident)
				++staticBindings;
			else
				++dynamicBindings;
		}

		for (size_t index = 0; index < group->slots.size(); ++index)
		{
			VirtualStockSlotBinding& slot = group->slots[index];
			NiTriShapeData* data = slot.shape
				? slot.shape->GetModelData() : nullptr;
			if (!data
				|| !IsNativeA8VirtualStockPacketAtlasCurrent(
					slot.shape, payload, static_cast<UInt32>(index))
				|| !AllocateVirtualStockBindingBuffer(slot))
			{
				SetVirtualStockFacadeMode(*group,
					NativeA8FallbackReason::PropertySync);
				return false;
			}
		}

		for (size_t index = 0; index < group->slots.size(); ++index)
		{
			VirtualStockSlotBinding& slot = group->slots[index];
			const NativeA8VirtualStockPacketBinding& source =
				resolved[index];
			NiTriShapeData* data = slot.shape->GetModelData();
			if (data->m_pkBuffData != slot.bindingBuffer
				&& data->m_pkBuffData != slot.shellBuffer)
			{
				slot.shellBuffer = data->m_pkBuffData;
			}
			if (!slot.bound
				&& slot.shape->GetShader() != slot.shellShader)
			{
				slot.shellShader = slot.shape->GetShader();
			}
			const bool changed = !slot.bound
				|| slot.generation != source.generation
				|| slot.resourceSerial != source.resourceSerial
				|| slot.uploadEpoch != source.uploadEpoch
				|| slot.atlasTextureEpoch != source.atlasTextureEpoch
				|| slot.baseVertex != source.baseVertex
				|| slot.vertexCount != source.vertexCount
				|| slot.staticResident != source.staticResident
				|| slot.shape->GetShader()
					!= payload.packetShaders[index];
			const bool needsRebind = changed
				|| !IsVirtualStockBindingConfigured(
					slot, source, payload.packetShaders[index]);
			const UInt32 quadCount = source.vertexCount / 4u;
			if (needsRebind)
			{
				slot.bindingBuffer->m_uiFlags = 0;
				slot.bindingBuffer->m_pkGeometryGroup = nullptr;
				slot.bindingBuffer->m_uiFVF = 0;
				slot.bindingBuffer->m_hDeclaration = source.declaration;
				slot.bindingBuffer->m_bSoftwareVP = false;
				slot.bindingBuffer->m_uiVertCount = source.vertexCount;
				slot.bindingBuffer->m_uiMaxVertCount = source.vertexCount;
				slot.bindingBuffer->m_uiStreamCount = 1;
				slot.bindingBuffer->m_puiVertexStride[0] =
					sizeof(NativeA8GpuVertex);
				slot.bindingBuffer->m_uiIndexCount = quadCount * 6u;
				slot.bindingBuffer->m_uiIBSize = source.indexBytes;
				slot.bindingBuffer->m_pkIB = source.indexBuffer;
				slot.bindingBuffer->m_uiBaseVertexIndex =
					source.baseVertex;
				slot.bindingBuffer->m_eType = D3DPT_TRIANGLELIST;
				slot.bindingBuffer->m_uiTriCount = quadCount * 2u;
				slot.bindingBuffer->m_uiMaxTriCount = quadCount * 2u;
				slot.bindingBuffer->m_uiNumArrays =
					kCanonicalArrayCount;
				slot.bindingBuffer->m_pusArrayLengths = nullptr;
				slot.bindingBuffer->m_pusIndexArray = nullptr;
				slot.bindingChip->m_uiIndex = 0;
				slot.bindingChip->m_pkVB = source.vertexBuffer;
				slot.bindingChip->m_uiOffset = 0;
				slot.bindingChip->m_uiLockFlags = 0;
				slot.bindingChip->m_uiSize =
					source.vertexCount * sizeof(NativeA8GpuVertex);
				slot.shape->SetShader(payload.packetShaders[index]);
				data->m_pkBuffData = slot.bindingBuffer;
			}
			slot.generation = source.generation;
			slot.resourceSerial = source.resourceSerial;
			slot.uploadEpoch = source.uploadEpoch;
			slot.atlasTextureEpoch = source.atlasTextureEpoch;
			slot.baseVertex = source.baseVertex;
			slot.vertexCount = source.vertexCount;
			slot.staticResident = source.staticResident;
			slot.bound = true;
			if (needsRebind)
				RecordFreeTypePerf(
					FreeTypePerfCounter::VirtualStockRebind);
			if (needsRebind && !IsVirtualStockBindingConfigured(
				slot, resolved[index], payload.packetShaders[index]))
			{
				configurationFailed = true;
				break;
			}
		}
		if (configurationFailed)
		{
			SetVirtualStockFacadeMode(*group,
				NativeA8FallbackReason::PropertySync);
			return false;
		}

		if (buildCommandView)
		{
			for (UInt32 index = 0;
				index < packets.size(); ++index)
			{
				const NativeA8TileRetainedPacket& retained =
					retainedText->packets[index];
				const NativeA8PacketTemplate& packet =
					*retained.packet;
				const NativeA8VirtualStockPacketBinding& source =
					resolved[index];
				const VirtualStockSlotBinding& slot =
					group->slots[index];
				NativeA8DrawCommand& command =
					group->commandBuildCommands[index];
				command = {};
				if (!slot.shape || !slot.bound || !source.active
					|| packet.atlasPage
						>= payload.preflightAtlasTextures.size()
					|| !payload.preflightAtlasTextures[packet.atlasPage]
					|| retained.packetIndex != index
					|| retained.vertexCount != source.vertexCount
					|| !retained.program)
				{
					SetVirtualStockFacadeMode(*group,
						NativeA8FallbackReason::PacketBuild);
					return false;
				}
				command.sourceGeometry = slot.shape;
				command.expectedGeometry = slot.shape;
				command.payload = &payload;
				command.packet = &packet;
				command.packetIndex = index;
				command.atlasTexture =
					payload.preflightAtlasTextures[packet.atlasPage];
				command.program = retained.program;
				if (packets.size() == 1)
				{
					command.standardPassLite =
						&retainedText->standardPassLite;
				}
				command.binding.vertexBuffer = source.vertexBuffer;
				command.binding.indexBuffer = source.indexBuffer;
				command.binding.declaration = source.declaration;
				command.binding.baseVertex = source.baseVertex;
				command.binding.vertexCount = source.vertexCount;
				command.binding.indexBytes = source.indexBytes;
				command.binding.generation = source.generation;
				command.binding.resourceSerial = source.resourceSerial;
				command.binding.uploadEpoch = source.uploadEpoch;
				command.binding.staticResident = source.staticResident;
				command.binding.active = source.active;
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandTileRetainedPacketReuse,
				static_cast<UInt64>(packets.size()));
		}
		group->preparedValidationToken = validationToken;
		group->preparedGeneration = generation;
		group->preparedAtlasTextureEpoch = atlasTextureEpoch;
		group->directDrawCount.store(0, std::memory_order_release);
		if (buildCommandView)
		{
			group->commandBuildValidationToken.store(
				validationToken, std::memory_order_release);
		}
		group->frameMode.store(VirtualStockFrameMode::Direct,
			std::memory_order_release);
		RecordFreeTypePerf(
			FreeTypePerfCounter::VirtualStockStaticHit,
			staticBindings);
		RecordFreeTypePerf(
			FreeTypePerfCounter::VirtualStockDynamicHit,
			dynamicBindings);
		RecordFreeTypePerf(
			FreeTypePerfCounter::VirtualStockSortedPreflightSaved,
			group->slots.size() > 1 ? group->slots.size() - 1 : 1);
		RecordFreeTypePerf(
			FreeTypePerfCounter::VirtualStockProxyPacketSaved,
			static_cast<UInt64>(group->slots.size()));
		return true;
	}

	void RestoreVirtualStockGroupToFacade(
		const std::shared_ptr<VirtualStockShapeGroup>& group,
		NativeA8FallbackReason reason)
	{
		if (!group)
			return;
		std::lock_guard<std::mutex> lock(group->mutex);
		if (group->frameMode.load(std::memory_order_acquire)
			== VirtualStockFrameMode::Retired)
		{
			return;
		}
		SetVirtualStockFacadeMode(*group, reason);
	}

	void RestoreVirtualStockSingletonToFacade(
		const A8ShapeMetadata& metadata, NativeA8FallbackReason reason)
	{
		VirtualStockSingletonState* singleton =
			GetVirtualStockSingletonState(metadata);
		if (!singleton
			|| singleton->frameMode.load(std::memory_order_acquire)
				== VirtualStockFrameMode::Retired)
		{
			return;
		}
		SetVirtualStockSingletonFacadeMode(metadata, reason);
	}

	void InvalidateAllVirtualStockBindings()
	{
		NotifyNativeA8CommandExternalMutation(
			NativeA8CommandFallback::Resource);
		std::vector<std::shared_ptr<VirtualStockShapeGroup>> groups;
		std::vector<A8ShapeMetadataPtr> singletons;
		{
			A8State& state = State();
			std::lock_guard<std::mutex> lock(state.metadataMutex);
			groups.reserve(state.virtualStockGroups.size());
			for (const auto& entry : state.virtualStockGroups)
				groups.push_back(entry.second);
			for (const auto& entry : state.shapeMetadata)
			{
				if (entry.second.metadata
					&& entry.second.metadata->backend
						== FreeTypeShapeBackend::VirtualStockSingleton)
				{
					singletons.push_back(entry.second.metadata);
				}
			}
		}
		for (const A8ShapeMetadataPtr& metadata : singletons)
		{
			VirtualStockSingletonState* singleton = metadata
				? GetVirtualStockSingletonState(*metadata) : nullptr;
			if (!singleton)
				continue;
			const bool revoked = singleton->slot.bound;
			RestoreVirtualStockSlot(singleton->slot);
			singleton->preparedValidationToken = 0;
			singleton->preflightValidationToken = 0;
			singleton->preparedGeneration = 0;
			singleton->preparedAtlasTextureEpoch = 0;
			singleton->directDrawCount.store(
				0, std::memory_order_release);
			singleton->commandBuildValidationToken.store(
				0, std::memory_order_release);
			singleton->commandValidationToken.store(
				0, std::memory_order_release);
			singleton->commandVirtualSinglePacketIndex.store(
				kInvalidNativeA8CommandIndex,
				std::memory_order_release);
			if (singleton->frameMode.load(std::memory_order_acquire)
				!= VirtualStockFrameMode::Retired)
			{
				singleton->frameMode.store(VirtualStockFrameMode::Facade,
					std::memory_order_release);
			}
			if (revoked)
				RecordFreeTypePerf(
					FreeTypePerfCounter::VirtualStockRevoke);
		}
		for (const std::shared_ptr<VirtualStockShapeGroup>& group : groups)
		{
			std::lock_guard<std::mutex> lock(group->mutex);
			bool revoked = false;
			for (VirtualStockSlotBinding& slot : group->slots)
			{
				revoked = revoked || slot.bound;
				RestoreVirtualStockSlot(slot);
			}
			group->preparedValidationToken = 0;
			group->preflightValidationToken = 0;
			group->preparedGeneration = 0;
			group->preparedAtlasTextureEpoch = 0;
			group->directDrawCount.store(0, std::memory_order_release);
			group->commandBuildValidationToken.store(
				0, std::memory_order_release);
			group->commandValidationToken.store(
				0, std::memory_order_release);
			group->commandSpanIndex.store(
				kInvalidNativeA8CommandIndex,
				std::memory_order_release);
			group->commandLeaderSlot.store(
				0, std::memory_order_release);
			if (group->frameMode.load(std::memory_order_acquire)
				!= VirtualStockFrameMode::Retired)
			{
				group->frameMode.store(VirtualStockFrameMode::Facade,
					std::memory_order_release);
			}
			if (revoked)
				RecordFreeTypePerf(
					FreeTypePerfCounter::VirtualStockRevoke);
		}
	}

	void ReleaseVirtualStockSingletonBinding(
		NiTriShape* shape, const A8ShapeMetadata& metadata)
	{
		VirtualStockSingletonState* singleton =
			GetVirtualStockSingletonState(metadata);
		if (!shape || !singleton || singleton->slot.shape != shape)
			return;
		DestroyVirtualStockBindingBuffer(singleton->slot);
		singleton->slot.shape = nullptr;
		singleton->registeredSlotCount = 0;
		singleton->registrationContiguous = false;
		singleton->commandBuildValidationToken.store(
			0, std::memory_order_release);
		singleton->commandValidationToken.store(
			0, std::memory_order_release);
		singleton->commandVirtualSinglePacketIndex.store(
			kInvalidNativeA8CommandIndex,
			std::memory_order_release);
		singleton->frameMode.store(VirtualStockFrameMode::Retired,
			std::memory_order_release);
	}

	void ReleaseVirtualStockShapeBinding(
		NiTriShape* shape, const A8ShapeMetadata& metadata)
	{
		if (!shape || !metadata.virtualStockGroup)
			return;
		std::shared_ptr<VirtualStockShapeGroup> group =
			AcquireVirtualStockShapeGroup(metadata);
		if (!group)
			return;
		bool releaseGroup = false;
		{
			std::lock_guard<std::mutex> lock(group->mutex);
			if (metadata.virtualStockSlot >= group->slots.size())
				return;
			VirtualStockSlotBinding& slot =
				group->slots[metadata.virtualStockSlot];
			if (slot.shape != shape)
				return;
			DestroyVirtualStockBindingBuffer(slot);
			slot.shape = nullptr;
			if (group->liveSlotCount)
				--group->liveSlotCount;
			if (metadata.virtualStockPrimary)
			{
				group->primaryShape = nullptr;
				group->primaryMetadataOwner.reset();
				group->commandBuildValidationToken.store(
					0, std::memory_order_release);
				group->commandValidationToken.store(
					0, std::memory_order_release);
				group->commandSpanIndex.store(
					kInvalidNativeA8CommandIndex,
					std::memory_order_release);
				group->frameMode.store(VirtualStockFrameMode::Retired,
					std::memory_order_release);
			}
			releaseGroup = group->liveSlotCount == 0;
		}
		if (releaseGroup)
		{
			A8State& state = State();
			std::lock_guard<std::mutex> lock(state.metadataMutex);
			group->metadataPublished.store(false,
				std::memory_order_release);
			const auto found =
				state.virtualStockGroups.find(group.get());
			if (found != state.virtualStockGroups.end()
				&& found->second == group)
			{
				state.virtualStockGroups.erase(found);
			}
		}
	}

}

namespace fonthook
{
	void FinalizeFreeTypeA8Detection()
	{
		vectorfont::FinalizeA8RendererDetection();
	}

	void HandleFreeTypeA8MainLoop()
	{
		vectorfont::HandleA8RendererMainLoop();
	}

	void HandleFreeTypeShaderLoaderMessage(UInt32 messageType)
	{
		vectorfont::HandleA8ShaderLoaderMessage(messageType);
	}
}

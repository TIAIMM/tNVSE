#include "font_native_shape_internal.h"

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
	namespace implementation::font_native_shape_render {}
	using namespace implementation::font_native_shape_render;

	namespace implementation::font_native_shape_render
	{
		inline constexpr UInt32 kCanonicalArrayCount = 1;
		inline constexpr size_t kSingletonFacadeEstimatedShapeBytes = 1024;

		struct NativeFontRuntimeReadinessSnapshot
		{
			NiDX9Renderer* renderer = nullptr;
			IDirect3DDevice9* device = nullptr;
			UInt32 generation = 0;
			UInt32 atlasTextureEpoch = 0;
			UInt32 hookEpoch = 0;
			UInt32 readyFlags = 0;
		};

		struct NativeFontRuntimeReadinessPublication
		{
			std::atomic_flag writer = ATOMIC_FLAG_INIT;
			std::atomic<UInt32> sequence = 0;
			std::atomic<bool> ready = false;
			std::atomic<UInt32> nextHookEpoch = 1;
			std::atomic<NiDX9Renderer*> renderer = nullptr;
			std::atomic<IDirect3DDevice9*> device = nullptr;
			std::atomic<UInt32> generation = 0;
			std::atomic<UInt32> atlasTextureEpoch = 0;
			std::atomic<UInt32> hookEpoch = 0;
			std::atomic<UInt32> readyFlags = 0;
		};

		NativeFontRuntimeReadinessPublication s_runtimeReadiness;

		void PublishRuntimeReadiness(bool accumulatorReady,
			bool immediateReady, bool shaderReady)
		{
			while (s_runtimeReadiness.writer.test_and_set(
				std::memory_order_acquire))
			{
				YieldProcessor();
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::StructuralReadinessFullAudit);
			NativeFontRendererReadinessView rendererView;
			const bool rendererReady = shaderReady
				&& GetNativeFontRendererReadinessFast(rendererView);
			NativeFontRuntimeReadinessSnapshot snapshot;
			snapshot.renderer = rendererView.renderer;
			snapshot.device = rendererView.device;
			snapshot.generation = rendererView.generation;
			snapshot.atlasTextureEpoch = GetNativeFontAtlasTextureEpoch();
			snapshot.hookEpoch = s_runtimeReadiness.nextHookEpoch.fetch_add(
				1, std::memory_order_relaxed);
			if (!snapshot.hookEpoch)
			{
				snapshot.hookEpoch = s_runtimeReadiness.nextHookEpoch.fetch_add(
					1, std::memory_order_relaxed);
			}
			snapshot.readyFlags = (accumulatorReady ? 1u : 0u)
				| (immediateReady ? 2u : 0u)
				| (rendererReady ? 4u : 0u);
			const bool ready = snapshot.readyFlags == 7u;
			s_runtimeReadiness.ready.store(false, std::memory_order_release);
			s_runtimeReadiness.sequence.fetch_add(
				1, std::memory_order_acq_rel);
			s_runtimeReadiness.renderer.store(
				snapshot.renderer, std::memory_order_relaxed);
			s_runtimeReadiness.device.store(
				snapshot.device, std::memory_order_relaxed);
			s_runtimeReadiness.generation.store(
				snapshot.generation, std::memory_order_relaxed);
			s_runtimeReadiness.atlasTextureEpoch.store(
				snapshot.atlasTextureEpoch, std::memory_order_relaxed);
			s_runtimeReadiness.hookEpoch.store(
				snapshot.hookEpoch, std::memory_order_relaxed);
			s_runtimeReadiness.readyFlags.store(
				snapshot.readyFlags, std::memory_order_relaxed);
			s_runtimeReadiness.sequence.fetch_add(
				1, std::memory_order_release);
			s_runtimeReadiness.ready.store(ready, std::memory_order_release);
			s_runtimeReadiness.writer.clear(std::memory_order_release);
		}

		bool LoadRuntimeReadiness(
			NativeFontRuntimeReadinessSnapshot& snapshot)
		{
			if (!s_runtimeReadiness.ready.load(std::memory_order_acquire))
				return false;
			for (UInt32 attempt = 0; attempt < 3; ++attempt)
			{
				const UInt32 before = s_runtimeReadiness.sequence.load(
					std::memory_order_acquire);
				if (before & 1u)
					continue;
				snapshot.renderer = s_runtimeReadiness.renderer.load(
					std::memory_order_relaxed);
				snapshot.device = s_runtimeReadiness.device.load(
					std::memory_order_relaxed);
				snapshot.generation = s_runtimeReadiness.generation.load(
					std::memory_order_relaxed);
				snapshot.atlasTextureEpoch =
					s_runtimeReadiness.atlasTextureEpoch.load(
						std::memory_order_relaxed);
				snapshot.hookEpoch = s_runtimeReadiness.hookEpoch.load(
					std::memory_order_relaxed);
				snapshot.readyFlags = s_runtimeReadiness.readyFlags.load(
					std::memory_order_relaxed);
				const UInt32 after = s_runtimeReadiness.sequence.load(
					std::memory_order_acquire);
				if (before == after && !(after & 1u)
					&& s_runtimeReadiness.ready.load(
						std::memory_order_acquire))
				{
					return true;
				}
			}
			return false;
		}

		bool LoadCurrentRuntimeReadiness(
			NativeFontRuntimeReadinessView* view)
		{
			NativeFontRuntimeReadinessSnapshot published;
			if (!LoadRuntimeReadiness(published)
				|| published.readyFlags != 7u)
			{
				return false;
			}
			NativeFontRendererReadinessView current;
			const bool rendererCurrent =
				GetNativeFontRendererReadinessFast(current)
				&& current.renderer == published.renderer
				&& current.device == published.device
				&& current.generation == published.generation;
			const bool atlasCurrent = published.atlasTextureEpoch
				== GetNativeFontAtlasTextureEpoch();
			const bool accumulatorCurrent =
				IsNativeFontRegistrationHookChainCurrentFast();
			const bool immediateCurrent =
				IsNativeFontRenderPassImmediatelyHookCurrentFast();
			if (rendererCurrent && atlasCurrent
				&& accumulatorCurrent && immediateCurrent)
			{
				if (view)
				{
					view->renderer = current.renderer;
					view->device = current.device;
					view->generation = current.generation;
					view->atlasTextureEpoch = published.atlasTextureEpoch;
					view->hookEpoch = published.hookEpoch;
					view->ready = true;
				}
				RecordFreeTypePerf(
					FreeTypePerfCounter::StructuralReadinessRawHit);
				RecordFreeTypePerf(
					FreeTypePerfCounter::StructuralReadinessVirtualQueryAvoided,
					4);
				return true;
			}
			if (!rendererCurrent)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					StructuralReadinessRendererMismatch);
			}
			if (!atlasCurrent)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					StructuralReadinessAtlasMismatch);
			}
			if (!accumulatorCurrent || !immediateCurrent)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					StructuralReadinessHookMismatch);
			}
			s_runtimeReadiness.ready.store(false, std::memory_order_release);
			return false;
		}

		NativeFontShapeState s_a8State;
		CpuMemoryLease s_metadataBucketMemory;
		size_t s_metadataHighWater = 0;

		void RefreshMetadataBucketAccounting(const NativeFontShapeState& state)
		{
			s_metadataBucketMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
				state.shapeMetadata.bucket_count() * sizeof(void*));
		}

		void ReserveStructuralMetadataMap(size_t requestedEntries = 4096)
		{
			NativeFontShapeState& state = State();
			std::lock_guard<std::mutex> lock(state.metadataMutex);
			const size_t currentBytes =
				state.shapeMetadata.bucket_count() * sizeof(void*);
			const size_t estimatedBytes = requestedEntries * sizeof(void*);
			const size_t additionalBytes = estimatedBytes > currentBytes
				? estimatedBytes - currentBytes : 0;
			if (!additionalBytes
				|| GetCpuMemoryCategoryHeadroom(
						CpuMemoryCategory::RuntimeMetadata, additionalBytes)
					< additionalBytes)
			{
				return;
			}
			state.shapeMetadata.reserve(requestedEntries);
			RefreshMetadataBucketAccounting(state);
			RecordFreeTypePerf(FreeTypePerfCounter::MetadataMapReserve);
		}

		void MaintainStructuralMetadataMap()
		{
			NativeFontShapeState& state = State();
			size_t target = 0;
			{
				std::lock_guard<std::mutex> lock(state.metadataMutex);
				s_metadataHighWater = std::max(
					s_metadataHighWater, state.shapeMetadata.size());
				const size_t buckets = state.shapeMetadata.bucket_count();
				if (buckets && state.shapeMetadata.size() * 4u
					< buckets * 3u)
				{
					return;
				}
				target = std::max<size_t>(4096,
					std::max<size_t>(state.shapeMetadata.size() * 2u,
						s_metadataHighWater + s_metadataHighWater / 2u));
			}
			ReserveStructuralMetadataMap(target);
		}

		void RecordMetadataInsertionRehash(NativeFontShapeState& state,
			size_t previousBucketCount)
		{
			if (state.shapeMetadata.bucket_count() == previousBucketCount)
				return;
			RefreshMetadataBucketAccounting(state);
			RecordFreeTypePerf(FreeTypePerfCounter::MetadataMapRehash);
		}

		void InitializeNativeFontMetadataIdentity(
			NativeFontShapeMetadata& metadata, const NiTriShape* shape)
		{
			NativeFontShapeState& state = State();
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

		NativeFontShapeMetadataEntry MakeNativeFontMetadataEntry(
			NativeFontShapeMetadataPtr metadata)
		{
			NativeFontShapeMetadataEntry entry;
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

		bool IsFiniteVertex(const NativeFontGpuVertex& vertex)
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

		bool RejectNativeFontShape(const char* reason)
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

		bool ValidateNativeFontShape(NiTriShape* shape,
			const NativeFontEffectShapeConfig* effectConfig,
			const NativeFontShapeColorContract* colorContract,
			const NativeFontPayloadTemplate* payloadTemplate)
		{
			if (!shape || !colorContract || !payloadTemplate)
				return RejectNativeFontShape("missing-shape-color-or-text-artifact");
			if (colorContract->abiVersion
				!= NativeFontShapeColorContract::kTileUniformColorAbi)
			{
				return RejectNativeFontShape("color-contract-abi-mismatch");
			}
			if (!IsFiniteColor(colorContract->minimumModifier)
				|| !IsFiniteColor(colorContract->maximumModifier))
			{
				return RejectNativeFontShape("non-finite-color-contract");
			}

			NiTriShapeData* data = shape->GetModelData();
			if (!data || !data->m_usVertices || !data->m_usTriangles
				|| !data->m_pkVertex || !data->m_pusTriList)
			{
				return RejectNativeFontShape("missing-geometry-data");
			}
			if (!data->m_pkColor)
				return RejectNativeFontShape("missing-base-vertex-color-stream");
			for (UInt32 index = 0; index < data->m_usVertices; ++index)
			{
				if (!IsFiniteColor(data->m_pkColor[index]))
					return RejectNativeFontShape("non-finite-base-vertex-color");
			}

			const bool sealed =
				HasNativeFontPayloadValidationSeal(*payloadTemplate);
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
					|| payloadTemplate->quadCount > kNativeFontMaximumQuads
					|| payloadTemplate->gpuVertices.size()
						< static_cast<size_t>(payloadTemplate->quadCount) * 4u
					|| (payloadTemplate->gpuVertices.size() & 3u)
					|| payloadTemplate->gpuVertices.size() / 4u
						> kNativeFontMaximumQuads
					|| payloadTemplate->packets.empty()
					|| payloadTemplate->pageCount
						!= payloadTemplate->atlasProperties.size()
					|| payloadTemplate->pageCount
						!= payloadTemplate->atlasTextures.size()
					|| !IsFiniteBound(payloadTemplate->bound))
				{
					return RejectNativeFontShape("invalid-text-artifact");
				}
				if (!std::all_of(payloadTemplate->gpuVertices.begin(),
					payloadTemplate->gpuVertices.end(), IsFiniteVertex))
				{
					return RejectNativeFontShape(
						"non-finite-text-artifact-vertex");
				}
				for (const NativeFontPacketTemplate& packet
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
						return RejectNativeFontShape(
							"invalid-text-artifact-packet");
					}
				}
				for (const NativeFontPacketTemplate& packet
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
							!= NativeFontShaderClass::Composite
						|| packet.staticCompositeLayerMask > 15u
						|| !std::all_of(packet.constants.begin(),
							packet.constants.end(), [](float value)
							{ return std::isfinite(value); }))
					{
						return RejectNativeFontShape("invalid-composite-packet");
					}
				}
			}
			if (!effectConfig || !effectConfig->enabled)
				return RejectNativeFontShape("native-route-requires-enabled-profile");
			const UInt32 profileKinds =
				(effectConfig->shaderEffects ? 1u : 0u)
				+ (effectConfig->bakedCoverage ? 1u : 0u)
				+ (effectConfig->precomposedArgb ? 1u : 0u);
			if (profileKinds != 1u)
				return RejectNativeFontShape("native-route-profile-kind-conflict");
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
				return RejectNativeFontShape("non-finite-effect-configuration");
			}
			if (effectConfig->rasterScale <= 0.0f)
				return RejectNativeFontShape("invalid-effect-raster-scale");
			if (effectConfig->shadowGlowAlpha < 0.0f
				|| effectConfig->shadowGlowAlpha > 1.0f
				|| effectConfig->shadowOutlineAlpha < 0.0f
				|| effectConfig->shadowOutlineAlpha > 1.0f)
			{
				return RejectNativeFontShape("invalid-shadow-component-alpha");
			}
			if (static_cast<UInt32>(effectConfig->quality)
				> static_cast<UInt32>(EffectQuality::High))
			{
				return RejectNativeFontShape("invalid-effect-quality");
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
				return RejectNativeFontShape("atlas-page-metadata-size-mismatch");
			}
			if (!effectConfig->atlasProperties.empty()
				&& effectConfig->atlasProperties.size()
					!= effectConfig->atlasTextures.size())
			{
				return RejectNativeFontShape("atlas-page-property-size-mismatch");
			}
			if (effectConfig->atlasProperties.empty()
				|| payloadTemplate->pageCount != effectConfig->atlasTextures.size())
			{
				return RejectNativeFontShape("text-artifact-page-count-mismatch");
			}
			if (!sealed)
			{
				for (const NativeFontPacketTemplate& packet
					: payloadTemplate->packets)
				{
					if (packet.atlasPage >= effectConfig->atlasTextures.size())
					{
						return RejectNativeFontShape(
							"text-artifact-packet-page-out-of-bounds");
					}
				}
				for (const NativeFontPacketTemplate& packet
					: payloadTemplate->compositePackets)
				{
					if (packet.atlasPage >= effectConfig->atlasTextures.size())
					{
						return RejectNativeFontShape(
							"composite-packet-page-out-of-bounds");
					}
				}
			}
			for (const NiPoint2& inverseSize : effectConfig->atlasInverseSizes)
			{
				if (!std::isfinite(inverseSize.x) || !std::isfinite(inverseSize.y))
					return RejectNativeFontShape("non-finite-atlas-inverse-size");
			}
			for (const NativeFontDrawRange& range : effectConfig->ranges)
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
					return RejectNativeFontShape("invalid-draw-range");
				}
				if (!effectConfig->atlasTextures.empty()
					&& (range.atlasPage >= effectConfig->atlasTextures.size()
						|| !effectConfig->atlasTextures[range.atlasPage]))
				{
					return RejectNativeFontShape("invalid-atlas-page");
				}
				const UInt64 vertexEnd = static_cast<UInt64>(range.firstVertex)
					+ range.vertexCount;
				const UInt64 indexEnd = static_cast<UInt64>(range.startIndex)
					+ static_cast<UInt64>(range.primitiveCount) * 3;
				const UInt32 layerRank = GetNativeFontLayerDrawRank(range.layer);
				if (vertexEnd > availableVertices || indexEnd > availableIndices)
					return RejectNativeFontShape("draw-range-out-of-bounds");
				if (!firstRange && (layerRank < previousLayerRank
					|| (layerRank == previousLayerRank
						&& (range.firstVertex < previousVertexEnd
							|| range.startIndex < previousIndexEnd))))
				{
					return RejectNativeFontShape("draw-ranges-not-layer-monotonic");
				}
				if (distanceFieldProfile != range.usesSdf)
					return RejectNativeFontShape(distanceFieldProfile
						? "distance-field-range-without-sdf"
						: effectConfig->precomposedArgb
							? "argb-range-marked-as-sdf"
							: "coverage-range-marked-as-sdf");
				if (distanceFieldProfile
					&& range.sdfSpreadPixels <= 0.0f)
				{
					return RejectNativeFontShape(
						"distance-field-range-without-positive-spread");
				}
				haveFill = haveFill || range.layer == 3;
				previousLayerRank = layerRank;
				previousVertexEnd = vertexEnd;
				previousIndexEnd = indexEnd;
				firstRange = false;
			}
			return haveFill || RejectNativeFontShape("missing-fill-range");
		}

		bool ValidateSingletonFacadeShellShape(NiTriShape* shape)
		{
			NiTriShapeData* data =
				shape ? shape->GetModelData() : nullptr;
			if (!data || data->m_usVertices < 4
				|| data->m_usTriangles < 2 || !data->m_pkVertex
				|| !data->m_pkTexture || !data->m_pkColor
				|| !data->m_pusTriList)
			{
				return RejectNativeFontShape(
					"invalid-singleton-facade-shell");
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
					return RejectNativeFontShape(
						"non-finite-singleton-facade-shell");
				}
			}
			return true;
		}

		bool AllocateSingletonFacadeBindingBuffer(
			SingletonFacadeBinding& slot)
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
			*stride = sizeof(NativeFontGpuVertex);
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

		bool IsSingletonFacadeBindingConfigured(
			const SingletonFacadeBinding& slot,
			const NativeFontDirectFacadePacketBinding& source,
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
					== sizeof(NativeFontGpuVertex)
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
						* sizeof(NativeFontGpuVertex);
		}

		void ClearSingletonFacadeGpuFields(SingletonFacadeBinding& slot)
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

		void RestoreSingletonFacadeSlot(SingletonFacadeBinding& slot)
		{
			const bool wasBound = slot.bound;
			NiTriShapeData* data = slot.shape
				? slot.shape->GetModelData() : nullptr;
			if (data && data->m_pkBuffData == slot.bindingBuffer)
				data->m_pkBuffData = slot.shellBuffer;
			// Font::MakeTriShape(..., false) leaves NiGeometry::m_pShader null
			// until the caller runs PrepareObject.  A facade is published before
			// that call, so its creation-time shellShader is not authoritative.
			// Only restore shader state after this slot actually replaced it; the
			// first single-packet bind refreshes shellShader from the live shape.
			if (wasBound && slot.shape)
				slot.shape->SetShader(slot.shellShader);
			ClearSingletonFacadeGpuFields(slot);
		}

		void DestroySingletonFacadeBindingBuffer(
			SingletonFacadeBinding& slot)
		{
			RestoreSingletonFacadeSlot(slot);
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

		void SetSingletonFacadeMode(
			const NativeFontShapeMetadata& metadata, NativeFontFallbackReason reason,
			bool recordFallback = true)
		{
			SingletonFacadeState* singleton =
				GetSingletonFacadeState(metadata);
			if (!singleton)
				return;
			singleton->commandBuildValidationToken.store(
				0, std::memory_order_release);
			RestoreSingletonFacadeSlot(singleton->slot);
			singleton->preparedValidationToken = 0;
			singleton->preflightValidationToken = 0;
			singleton->preparedGeneration = 0;
			singleton->preparedAtlasTextureEpoch = 0;
			singleton->directDrawCount.store(0, std::memory_order_release);
			singleton->commandValidationToken.store(
				0, std::memory_order_release);
			singleton->commandDirectFacadeSinglePacketIndex.store(
				kInvalidNativeFontCommandIndex,
				std::memory_order_release);
			singleton->frameMode.store(SingletonFacadeFrameMode::Facade,
				std::memory_order_release);
			if (recordFallback)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::SingletonFacadeFallback);
			}
		}

	}

	NativeFontShapeState& State()
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

	void FinalizeNativeFontRendererDetection()
	{
		ReserveStructuralMetadataMap();
		const bool accumulatorReady = HookNativeFontAccumulator();
		const bool immediateRouteReady = HookRenderPassImmediately();
		const bool shaderReady = InitializeNativeFontRenderer(true, true);
		const bool nativeReady =
			accumulatorReady && immediateRouteReady && shaderReady;
		PublishRuntimeReadiness(accumulatorReady,
			immediateRouteReady, shaderReady);
		SynchronizePersistentFontCacheRoute(ResolveFontAtlasRoute(
			nativeReady, UsesBakedEffectRoute()));
		gLog.FormattedMessage(
			"tnvse_freetype_native: initialization nativeReady=%u accumulator=%u alphaRenderHook=%u sortOwner=active-virtual sortHook=0 immediateRoute=%u shader=%u",
			nativeReady ? 1 : 0,
			accumulatorReady ? 1 : 0,
			State().renderAlphaGeometryHookInstalled ? 1 : 0,
			immediateRouteReady ? 1 : 0,
			shaderReady ? 1 : 0);
	}

	void HandleNativeFontRendererMainLoop()
	{
		if (!g_bEnableFreeTypeFontRendering)
			return;
		HandleNativeFontShaderRendererMainLoop();
		MaintainStructuralMetadataMap();
		const bool accumulatorReady = HookNativeFontAccumulator();
		const bool immediateReady = HookRenderPassImmediately();
		PublishRuntimeReadiness(accumulatorReady, immediateReady,
			IsNativeFontShaderRendererAvailable());
		EnforceCpuMemoryBudget("main-loop");
	}

	void DispatchNativeFontShaderLoaderMessage(UInt32 messageType)
	{
		if (messageType == kShaderRefreshMessage)
			HandleNativeFontShaderLoaderMessage(messageType);
	}

	bool IsNativeFontRendererAvailable()
	{
		if (!g_bEnableFreeTypeFontRendering)
			return false;
		if (LoadCurrentRuntimeReadiness(nullptr))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::NativeRegistrationHookFast);
			return true;
		}
		// The normal registration path only needs readback of the already
		// installed chain and the published device generation.  Preserve the
		// complete install/retry/conflict path whenever any identity changed.
		RecordFreeTypePerf(
			FreeTypePerfCounter::NativeRegistrationHookSlow);
		const bool accumulatorReady = HookNativeFontAccumulator();
		const bool immediateReady = HookRenderPassImmediately();
		const bool shaderReady = accumulatorReady && immediateReady
			&& InitializeNativeFontRenderer(false, false);
		PublishRuntimeReadiness(accumulatorReady,
			immediateReady, shaderReady);
		return accumulatorReady && immediateReady && shaderReady;
	}

	bool GetNativeFontRuntimeReadinessCurrent(
		NativeFontRuntimeReadinessView& view)
	{
		view = {};
		return LoadCurrentRuntimeReadiness(&view);
	}

	bool ResolveNativeFontEffectQuality(EffectQuality requested, EffectQuality& resolved)
	{
		if (static_cast<UInt32>(requested) > static_cast<UInt32>(EffectQuality::High)
			|| !IsNativeFontRendererAvailable())
		{
			return false;
		}
		resolved = requested;
		return true;
	}

	bool PrepareNativeFontAtlasShape(Font& font, NiTriShape* shape, UInt32 fontId,
		UInt32 glyphCount, UInt32 quadCount, const NativeFontEffectShapeConfig* effectConfig,
		const NativeFontShapeColorContract* colorContract,
		NativeFontPayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin)
	{
		FreeTypePerfScope perf(
			FreeTypePerfPhase::NativeRegistration);
		if (!IsNativeFontRendererAvailable()
			|| !payloadTemplate || payloadTemplate->quadCount != quadCount
			|| !ValidateNativeFontShape(shape, effectConfig, colorContract,
				payloadTemplate.get())
			|| !InitializeNativeFontTriShapeVtable(shape))
		{
			return false;
		}
		auto metadata = std::make_shared<NativeFontShapeMetadata>();
		InitializeNativeFontMetadataIdentity(*metadata, shape);
		metadata->fontId = fontId;
		metadata->glyphCount = glyphCount;
		metadata->quadCount = quadCount;
		metadata->vertexCount = static_cast<UInt32>(
			payloadTemplate->gpuVertices.size());
		metadata->primitiveCount = payloadTemplate->quadCount * 2u;
		metadata->indexCount = payloadTemplate->quadCount * 6u;
		if (colorContract)
			metadata->colorContract = *colorContract;
		if (!InitializeNativeFontShapePayload(font, shape, *metadata,
			std::move(payloadTemplate), geometryOrigin, metadata->nativePayload))
			return false;
		metadata->cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
			sizeof(NativeFontShapeMetadata)
				+ metadata->nativePayload.packetShaders.heap_capacity()
					* sizeof(TileShader*)
				+ metadata->nativePayload.packetPrograms.heap_capacity()
					* sizeof(const NativeFontCompiledPacketCommand*)
				+ metadata->nativePayload.preflightAtlasTextures.heap_capacity()
					* sizeof(const void*)
				+ GetNativeFontTileRetainedCapacityBytes(
					metadata->nativePayload)
				+ sizeof(NativeFontShapeMetadataEntry) + 6u * sizeof(void*));
		{
			NativeFontShapeState& state = State();
			std::lock_guard<std::mutex> lock(state.metadataMutex);
			state.metadataGenerations[GetMetadataGenerationSlot(shape)].fetch_add(
				1, std::memory_order_release);
			NativeFontShapeMetadataPtr publishedMetadata = std::move(metadata);
			const size_t previousBucketCount =
				state.shapeMetadata.bucket_count();
			state.shapeMetadata[shape] =
				MakeNativeFontMetadataEntry(std::move(publishedMetadata));
			RecordMetadataInsertionRehash(state, previousBucketCount);
		}
		*reinterpret_cast<void***>(shape) = &State().triShapeVtable[1];
		return true;
	}

	bool PrepareNativeFontSingletonFacadeShape(Font& font, NiTriShape* shape,
		UInt32 fontId, UInt32 glyphCount, UInt32 quadCount,
		const NativeFontEffectShapeConfig* effectConfig,
		const NativeFontShapeColorContract* colorContract,
		NativeFontPayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin)
	{
		FreeTypePerfScope perf(FreeTypePerfPhase::NativeRegistration);
		bool rendererAvailable = false;
		{
			FreeTypePerfScope readiness(
				FreeTypePerfPhase::NativeRegistrationReadiness);
			rendererAvailable = IsNativeFontRendererAvailable();
		}
		if (!rendererAvailable || !shape || !payloadTemplate
			|| payloadTemplate->quadCount != quadCount)
		{
			return false;
		}
		bool shapeReady = false;
		{
			FreeTypePerfScope shapeProof(
				FreeTypePerfPhase::NativeRegistrationShape);
			shapeReady = !payloadTemplate->packets.empty()
				&& ValidateNativeFontShape(shape, effectConfig, colorContract,
					payloadTemplate.get())
				&& InitializeNativeFontTriShapeVtable(shape);
		}
		if (!shapeReady)
		{
			return false;
		}

		const size_t estimatedBytes =
			sizeof(SingletonFacadeMetadata)
			+ sizeof(NativeFontShapeMetadataEntry)
			+ kSingletonFacadeEstimatedShapeBytes + 6u * sizeof(void*);
		bool budgetReady = false;
		{
			FreeTypePerfScope budget(
				FreeTypePerfPhase::NativeRegistrationBudget);
			budgetReady = GetCpuMemoryCategoryHeadroom(
				CpuMemoryCategory::RuntimeMetadata, estimatedBytes)
				>= estimatedBytes;
		}
		if (!budgetReady)
			return false;

		std::shared_ptr<SingletonFacadeMetadata> metadata;
		{
			FreeTypePerfScope allocation(
				FreeTypePerfPhase::NativeRegistrationAllocation);
			metadata = std::make_shared<SingletonFacadeMetadata>();
		}
		InitializeNativeFontMetadataIdentity(*metadata, shape);
		metadata->fontId = fontId;
		metadata->glyphCount = glyphCount;
		metadata->quadCount = quadCount;
		metadata->vertexCount = static_cast<UInt32>(
			payloadTemplate->gpuVertices.size());
		metadata->primitiveCount = payloadTemplate->quadCount * 2u;
		metadata->indexCount = payloadTemplate->quadCount * 6u;
		if (colorContract)
			metadata->colorContract = *colorContract;
		metadata->backend = FreeTypeShapeBackend::SingletonFacade;

		SingletonFacadeState& singleton = metadata->singleton;
		singleton.slot.shape = shape;
		singleton.slot.packetIndex = 0;
		singleton.slot.shellShader = shape->GetShader();
		NiTriShapeData* data = shape->GetModelData();
		singleton.slot.shellBuffer = data ? data->m_pkBuffData : nullptr;
		{
			FreeTypePerfScope payloadBuild(
				FreeTypePerfPhase::NativeRegistrationPayload);
			if (!InitializeNativeFontShapePayload(font, shape, *metadata,
				payloadTemplate, geometryOrigin, metadata->nativePayload))
			{
				return false;
			}
		}
		const UInt32 inlineContainers =
			(metadata->nativePayload.packetShaders.uses_inline_storage() ? 1u : 0u)
			+ (metadata->nativePayload.packetPrograms.uses_inline_storage() ? 1u : 0u)
			+ (metadata->nativePayload.preflightAtlasTextures.uses_inline_storage()
				? 1u : 0u)
			+ (metadata->nativePayload.retainedText.packets.uses_inline_storage()
				? 1u : 0u)
			+ (metadata->nativePayload.retainedText.runs.uses_inline_storage()
				? 1u : 0u);
		RecordFreeTypePerf(
			inlineContainers == 5u
				? FreeTypePerfCounter::SingletonFacadeInlinePayload
				: FreeTypePerfCounter::SingletonFacadeHeapPayload);
		const UInt32 avoidedChildAllocations =
			(metadata->nativePayload.packetShaders.uses_inline_storage()
				&& !metadata->nativePayload.packetShaders.empty() ? 1u : 0u)
			+ (metadata->nativePayload.packetPrograms.uses_inline_storage()
				&& !metadata->nativePayload.packetPrograms.empty() ? 1u : 0u)
			+ (metadata->nativePayload.preflightAtlasTextures.uses_inline_storage()
				&& !metadata->nativePayload.preflightAtlasTextures.empty() ? 1u : 0u)
			+ (g_bEnableFreeTypeFontCommandBuffer
				&& metadata->nativePayload.retainedText.packets.uses_inline_storage()
					? 1u : 0u)
			+ (g_bEnableFreeTypeFontCommandBuffer
				&& metadata->nativePayload.retainedText.runs.uses_inline_storage()
					? 1u : 0u);
		RecordFreeTypePerf(
			FreeTypePerfCounter::SingletonFacadeChildAllocationAvoided,
			avoidedChildAllocations);
		{
			FreeTypePerfScope accounting(
				FreeTypePerfPhase::NativeRegistrationAccounting);
			metadata->cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
				estimatedBytes
					+ metadata->nativePayload.packetShaders.heap_capacity()
						* sizeof(TileShader*)
					+ metadata->nativePayload.packetPrograms.heap_capacity()
						* sizeof(const NativeFontCompiledPacketCommand*)
					+ metadata->nativePayload.preflightAtlasTextures.heap_capacity()
						* sizeof(const void*)
					+ GetNativeFontTileRetainedCapacityBytes(
						metadata->nativePayload));
		}

		{
			FreeTypePerfScope publish(
				FreeTypePerfPhase::NativeRegistrationPublish);
			NativeFontShapeState& state = State();
			std::lock_guard<std::mutex> lock(state.metadataMutex);
			state.metadataGenerations[GetMetadataGenerationSlot(shape)].fetch_add(
				1, std::memory_order_release);
			NativeFontShapeMetadataPtr publishedMetadata = metadata;
			const size_t previousBucketCount =
				state.shapeMetadata.bucket_count();
			state.shapeMetadata[shape] =
				MakeNativeFontMetadataEntry(std::move(publishedMetadata));
			RecordMetadataInsertionRehash(state, previousBucketCount);
			*reinterpret_cast<void***>(shape) = &State().triShapeVtable[1];
		}
		return true;
	}

	bool PrepareNativeFontVanillaLayoutShape(Font& font, NiTriShape* shape,
		UInt32 fontId, UInt32 glyphCount, UInt32 quadCount,
		NativeFontVanillaLayoutKind layoutKind,
		const NativeFontEffectShapeConfig* effectConfig,
		const NativeFontShapeColorContract* colorContract,
		NativeFontPayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin)
	{
		FreeTypePerfScope perf(FreeTypePerfPhase::NativeRegistration);
		if (!IsNativeFontRendererAvailable() || !shape || !payloadTemplate
			|| payloadTemplate->quadCount != quadCount
			|| payloadTemplate->pageCount != 1
			|| payloadTemplate->compositePackets.size() != 1)
		{
			return false;
		}
		const NativeFontPacketTemplate& packet =
			payloadTemplate->compositePackets.front();
		const UInt64 packetEnd = static_cast<UInt64>(packet.firstVertex)
			+ packet.vertexCount;
		const NiTriShapeData* shapeData = shape->GetModelData();
		const bool supportedDistanceField =
			packet.distanceFieldMethod == DistanceFieldMethod::TrueSdf
			|| packet.distanceFieldMethod == DistanceFieldMethod::Mtsdf;
		const bool uniformLayout =
			layoutKind == NativeFontVanillaLayoutKind::Uniform40;
		const bool parametricLayout =
			layoutKind == NativeFontVanillaLayoutKind::Parametric48;
		if (packet.shaderClass != NativeFontShaderClass::Composite
			|| !supportedDistanceField
			|| (!uniformLayout && !parametricLayout)
			|| !IsVanillaLayoutEnabled(packet.distanceFieldMethod)
			|| packet.atlasPage != 0 || (packet.firstVertex & 3u)
			|| !packet.vertexCount || (packet.vertexCount & 3u)
			|| packetEnd > payloadTemplate->gpuVertices.size()
			|| !shapeData || shapeData->m_usVertices != packet.vertexCount
			|| packet.staticCompositeLayerMask < 8u
			|| packet.staticCompositeLayerMask > 15u
			|| !std::isfinite(packet.uniformSdfSpread)
			|| packet.uniformSdfSpread < 0.0f
			|| !std::isfinite(packet.uniformDistanceParameterScale)
			|| packet.uniformDistanceParameterScale < 0.0f
			|| (packet.uniformDistanceParameterScale > 0.0f
				&& packet.uniformDistanceParameterScale < 1.0f)
			|| (uniformLayout
				&& (packet.uniformSdfSpread <= 0.0f
					|| packet.uniformDistanceParameterScale < 1.0f))
			|| !ValidateNativeFontShape(shape, effectConfig, colorContract,
				payloadTemplate.get())
			|| !InitializeNativeFontTriShapeVtable(shape))
		{
			return false;
		}
		const float layerMask = static_cast<float>(
			packet.staticCompositeLayerMask);
		for (UInt32 relative = 0; relative < packet.vertexCount; ++relative)
		{
			const NativeFontGpuVertex& vertex = payloadTemplate->gpuVertices[
				static_cast<size_t>(packet.firstVertex) + relative];
			const NativeFontGpuVertex& quadFirst = payloadTemplate->gpuVertices[
				static_cast<size_t>(packet.firstVertex)
					+ (relative & ~UInt32(3u))];
			if (!std::isfinite(vertex.sdfSpread) || vertex.sdfSpread <= 0.0f
				|| !std::isfinite(vertex.distanceParameterScale)
				|| vertex.distanceParameterScale < 1.0f
				|| vertex.layerMask != layerMask
				|| vertex.sdfSpread != quadFirst.sdfSpread
				|| vertex.distanceParameterScale
					!= quadFirst.distanceParameterScale
				|| (uniformLayout
					&& (vertex.sdfSpread != packet.uniformSdfSpread
						|| vertex.distanceParameterScale
							!= packet.uniformDistanceParameterScale)))
			{
				return false;
			}
		}

		TileShader* shader = ResolveNativeFontPacketShader(
			packet, shape, false, layoutKind);
		if (!shader)
			return false;

		auto metadata = std::make_shared<VanillaLayoutMetadata>();
		InitializeNativeFontMetadataIdentity(*metadata, shape);
		metadata->fontId = fontId;
		metadata->glyphCount = glyphCount;
		metadata->quadCount = quadCount;
		metadata->vertexCount = packet.vertexCount;
		metadata->primitiveCount = packet.vertexCount / 2u;
		metadata->indexCount = packet.vertexCount / 4u * 6u;
		metadata->backend = FreeTypeShapeBackend::VanillaLayout;
		metadata->layoutKind = layoutKind;
		if (colorContract)
			metadata->colorContract = *colorContract;
		if (!InitializeNativeFontShapePayload(font, shape, *metadata,
			payloadTemplate, geometryOrigin, metadata->nativePayload))
		{
			return false;
		}
		metadata->cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
			sizeof(VanillaLayoutMetadata)
				+ metadata->nativePayload.packetShaders.heap_capacity()
					* sizeof(TileShader*)
				+ metadata->nativePayload.packetPrograms.heap_capacity()
					* sizeof(const NativeFontCompiledPacketCommand*)
				+ metadata->nativePayload.preflightAtlasTextures.heap_capacity()
					* sizeof(const void*)
				+ GetNativeFontTileRetainedCapacityBytes(
					metadata->nativePayload)
				+ sizeof(NativeFontShapeMetadataEntry) + 6u * sizeof(void*));
		shape->SetShader(shader);
		{
			NativeFontShapeState& state = State();
			std::lock_guard<std::mutex> lock(state.metadataMutex);
			state.metadataGenerations[GetMetadataGenerationSlot(shape)].fetch_add(
				1, std::memory_order_release);
			NativeFontShapeMetadataPtr publishedMetadata = std::move(metadata);
			const size_t previousBucketCount =
				state.shapeMetadata.bucket_count();
			state.shapeMetadata[shape] =
				MakeNativeFontMetadataEntry(std::move(publishedMetadata));
			RecordMetadataInsertionRehash(state, previousBucketCount);
			*reinterpret_cast<void***>(shape) =
				&state.vanillaLayoutTriShapeVtable[1];
		}
		return true;
	}

	bool PrepareSingletonFacadeForSortedFrame(
		const NativeFontShapeMetadata& metadata, UInt32 generation,
		UInt32 atlasTextureEpoch, UInt64 validationToken)
	{
		SingletonFacadeState* singleton =
			GetSingletonFacadeState(metadata);
		if (!singleton || !generation || !validationToken)
			return false;
		singleton->commandBuildValidationToken.store(
			0, std::memory_order_release);
		NiTriShape* shape = singleton->slot.shape;
		if (!shape || shape != metadata.shapeIdentity
			|| singleton->frameMode.load(std::memory_order_acquire)
				== SingletonFacadeFrameMode::Retired)
		{
			return false;
		}

		NativeFontShapePayload& payload = metadata.nativePayload;
		if (singleton->preflightValidationToken != validationToken
			|| !payload.buildComplete || !payload.payloadTemplate
			|| payload.preparedGeneration != generation
			|| payload.preflightAtlasTextureEpoch != atlasTextureEpoch)
		{
			SetSingletonFacadeMode(metadata,
				NativeFontFallbackReason::ShaderGeneration);
			return false;
		}
		const std::vector<NativeFontPacketTemplate>& packets =
			GetNativeFontPackets(*payload.payloadTemplate,
				payload.useCompositePackets);
		const bool buildCommandView =
			g_bEnableFreeTypeFontCommandBuffer;
		const NativeFontTileRetainedText* retainedText = nullptr;
		if (buildCommandView)
		{
			if (!IsNativeFontTileRetainedTextCurrent(payload, shape,
					generation, atlasTextureEpoch))
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CommandTileRetainedMiss);
				SetSingletonFacadeMode(metadata,
					NativeFontFallbackReason::PacketBuild);
				return false;
			}
			retainedText = &payload.retainedText;
			RecordFreeTypePerf(
				FreeTypePerfCounter::CommandTileRetainedHit);
		}
		if (singleton->topologyValidationToken != validationToken)
		{
			SetSingletonFacadeMode(metadata,
				NativeFontFallbackReason::PropertySync);
			return false;
		}
		if (packets.size() != 1 || payload.packetShaders.size() != 1
			|| (buildCommandView
				&& (!retainedText || retainedText->packets.size() != 1)))
		{
			// The facade remains the sole vanilla Sort item. Multi-packet
			// topology is submitted through the ordinary retained span (or
			// packet loop when command recording is disabled). This is a normal
			// facade mode rather than a singleton-facade failure.
			SetSingletonFacadeMode(metadata,
				NativeFontFallbackReason::None, false);
			return true;
		}

		NativeFontDirectFacadePacketBinding resolved;
		const NativeFontFallbackReason resolveResult =
			ResolveNativeFontDirectFacadePacketBinding(payload, 0, resolved);
		if (resolveResult != NativeFontFallbackReason::None)
		{
			SetSingletonFacadeMode(metadata, resolveResult);
			return false;
		}

		SingletonFacadeBinding& slot = singleton->slot;
		NiTriShapeData* data = shape->GetModelData();
		if (!data || !IsNativeFontDirectFacadePacketAtlasCurrent(
				shape, payload, 0)
			|| !AllocateSingletonFacadeBindingBuffer(slot))
		{
			SetSingletonFacadeMode(metadata,
				NativeFontFallbackReason::PropertySync);
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
			|| !IsSingletonFacadeBindingConfigured(
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
				sizeof(NativeFontGpuVertex);
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
				resolved.vertexCount * sizeof(NativeFontGpuVertex);
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
			RecordFreeTypePerf(FreeTypePerfCounter::SingletonFacadeRebind);
		if (needsRebind && !IsSingletonFacadeBindingConfigured(
				slot, resolved, payload.packetShaders[0]))
		{
			SetSingletonFacadeMode(metadata,
				NativeFontFallbackReason::PropertySync);
			return false;
		}

		if (buildCommandView)
		{
			const NativeFontTileRetainedPacket& retained =
				retainedText->packets.front();
			const NativeFontPacketTemplate& packet = *retained.packet;
			NativeFontDrawCommand& command =
				singleton->commandBuildCommand;
			command = {};
			if (!resolved.active
				|| packet.atlasPage >= payload.preflightAtlasTextures.size()
				|| !payload.preflightAtlasTextures[packet.atlasPage]
				|| retained.packetIndex != 0
				|| retained.vertexCount != resolved.vertexCount
				|| !retained.program)
			{
				SetSingletonFacadeMode(metadata,
					NativeFontFallbackReason::PacketBuild);
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
		singleton->frameMode.store(SingletonFacadeFrameMode::Direct,
			std::memory_order_release);
		RecordFreeTypePerf(resolved.staticResident
			? FreeTypePerfCounter::SingletonFacadeStaticHit
			: FreeTypePerfCounter::SingletonFacadeDynamicHit);
		RecordFreeTypePerf(
			FreeTypePerfCounter::SingletonFacadeSortedPreflightSaved);
		RecordFreeTypePerf(
			FreeTypePerfCounter::SingletonFacadeProxyPacketSaved);
		return true;
	}

	void RestoreSingletonFacade(
		const NativeFontShapeMetadata& metadata, NativeFontFallbackReason reason)
	{
		SingletonFacadeState* singleton =
			GetSingletonFacadeState(metadata);
		if (!singleton
			|| singleton->frameMode.load(std::memory_order_acquire)
				== SingletonFacadeFrameMode::Retired)
		{
			return;
		}
		SetSingletonFacadeMode(metadata, reason);
	}

	void InvalidateAllSingletonFacadeBindings()
	{
		NotifyNativeFontCommandExternalMutation(
			NativeFontCommandFallback::Resource);
		std::vector<NativeFontShapeMetadataPtr> singletons;
		{
			NativeFontShapeState& state = State();
			std::lock_guard<std::mutex> lock(state.metadataMutex);
			for (const auto& entry : state.shapeMetadata)
			{
				if (entry.second.metadata
					&& entry.second.metadata->backend
						== FreeTypeShapeBackend::SingletonFacade)
				{
					singletons.push_back(entry.second.metadata);
				}
			}
		}
		for (const NativeFontShapeMetadataPtr& metadata : singletons)
		{
			SingletonFacadeState* singleton = metadata
				? GetSingletonFacadeState(*metadata) : nullptr;
			if (!singleton)
				continue;
			const bool revoked = singleton->slot.bound;
			RestoreSingletonFacadeSlot(singleton->slot);
			singleton->preparedValidationToken = 0;
			singleton->topologyValidationToken = 0;
			singleton->preflightValidationToken = 0;
			singleton->preparedGeneration = 0;
			singleton->preparedAtlasTextureEpoch = 0;
			singleton->directDrawCount.store(
				0, std::memory_order_release);
			singleton->commandBuildValidationToken.store(
				0, std::memory_order_release);
			singleton->commandValidationToken.store(
				0, std::memory_order_release);
			singleton->commandDirectFacadeSinglePacketIndex.store(
				kInvalidNativeFontCommandIndex,
				std::memory_order_release);
			if (singleton->frameMode.load(std::memory_order_acquire)
				!= SingletonFacadeFrameMode::Retired)
			{
				singleton->frameMode.store(SingletonFacadeFrameMode::Facade,
					std::memory_order_release);
			}
			if (revoked)
				RecordFreeTypePerf(
					FreeTypePerfCounter::SingletonFacadeRevoke);
		}
	}

	void ReleaseSingletonFacadeBinding(
		NiTriShape* shape, const NativeFontShapeMetadata& metadata)
	{
		SingletonFacadeState* singleton =
			GetSingletonFacadeState(metadata);
		if (!shape || !singleton || singleton->slot.shape != shape)
			return;
		DestroySingletonFacadeBindingBuffer(singleton->slot);
		singleton->slot.shape = nullptr;
		singleton->topologyValidationToken = 0;
		singleton->commandBuildValidationToken.store(
			0, std::memory_order_release);
		singleton->commandValidationToken.store(
			0, std::memory_order_release);
		singleton->commandDirectFacadeSinglePacketIndex.store(
			kInvalidNativeFontCommandIndex,
			std::memory_order_release);
		singleton->frameMode.store(SingletonFacadeFrameMode::Retired,
			std::memory_order_release);
	}

}

namespace fonthook
{
	void FinalizeFreeTypeNativeRendererDetection()
	{
		vectorfont::FinalizeNativeFontRendererDetection();
	}

	void HandleFreeTypeNativeRendererMainLoop()
	{
		vectorfont::HandleNativeFontRendererMainLoop();
	}

	void HandleFreeTypeShaderLoaderMessage(UInt32 messageType)
	{
		vectorfont::DispatchNativeFontShaderLoaderMessage(messageType);
	}
}

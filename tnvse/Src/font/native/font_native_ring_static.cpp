#include "font_native_ring_detail.h"

#include "load_config.h"
#include "tnvse.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fonthook::vectorfont
{
	using namespace implementation::font_native_ring;

	namespace implementation::font_native_ring
	{
		struct LiveStaticPayload
		{
			NativeFontPayloadTemplatePtr owner;
			UInt32 baseVertex = 0;
		};

		UInt32 GetStaticObservationFrame(const NativeFontRingState& state)
		{
			return state.renderer ? state.renderer->m_uiFrameID : 0;
		}

		struct StaticPromotionPolicy
		{
			UInt32 maximumBytes = 0;
			UInt32 minimumActiveFrames = 0;
			UInt32 minimumDynamicUploadEpochs = 0;
			UInt32 coldResidentFrames = 0;
		};

		inline constexpr std::array<StaticPromotionPolicy, 5>
			kStaticPromotionPolicies = {{
				{ 32u * 1024u, 2, 2, 1800 },
				{ 128u * 1024u, 3, 2, 900 },
				{ 512u * 1024u, 5, 3, 450 },
				{ 2u * 1024u * 1024u, 8, 4, 240 },
				{ std::numeric_limits<UInt32>::max(), 16, 6, 120 },
			}};

		const StaticPromotionPolicy& GetStaticPromotionPolicy(UInt32 vertexCount)
		{
			const UInt64 bytes = static_cast<UInt64>(vertexCount)
				* sizeof(NativeFontGpuVertex);
			for (const StaticPromotionPolicy& policy : kStaticPromotionPolicies)
			{
				if (bytes <= policy.maximumBytes)
					return policy;
			}
			return kStaticPromotionPolicies.back();
		}

		bool IsFrameBefore(UInt32 current, UInt32 target)
		{
			return static_cast<std::int32_t>(current - target) < 0;
		}

		StaticPromotionReadiness GetStaticPromotionReadiness(
			const NativeFontRingState& state,
			const NativeFontStaticCandidate& candidate, UInt32 vertexCount,
			UInt32 frame, UInt32 maturityMultiplier)
		{
			if (candidate.promotionDisabled)
				return StaticPromotionReadiness::Disabled;
			if ((state.staticPromotionGlobalRetryFrame
					&& IsFrameBefore(frame,
						state.staticPromotionGlobalRetryFrame))
				|| (candidate.nextRetryFrame
					&& IsFrameBefore(frame, candidate.nextRetryFrame)))
			{
				return StaticPromotionReadiness::Retry;
			}
			const StaticPromotionPolicy& policy =
				GetStaticPromotionPolicy(vertexCount);
			const UInt32 requiredFrames = policy.minimumActiveFrames
				* maturityMultiplier;
			if (!candidate.observationFrameValid
				|| candidate.activeObservedFrames < requiredFrames
				|| static_cast<UInt32>(frame
					- candidate.firstObservedFrame) + 1u < requiredFrames
				|| static_cast<UInt32>(frame - candidate.lastObservedFrame)
					> kStaticPromotionMaximumFrameGap)
			{
				return StaticPromotionReadiness::Lifecycle;
			}
			const UInt32 requiredUploads = policy.minimumDynamicUploadEpochs
				* maturityMultiplier;
			if (candidate.dynamicUploadEpochCount < requiredUploads)
				return StaticPromotionReadiness::UploadHistory;
			return StaticPromotionReadiness::Ready;
		}

		void ResetStaticPromotionBudgetFrame(NativeFontRingState& state,
			UInt32 frame)
		{
			if (state.staticPromotionBudgetFrameValid
				&& state.staticPromotionBudgetFrame == frame)
			{
				return;
			}
			state.staticPromotionBudgetFrame = frame;
			state.staticPromotionBytesThisFrame = 0;
			state.staticPromotionPayloadsThisFrame = 0;
			state.staticPromotionBudgetFrameValid = true;
		}

		bool FitsStaticPromotionBudget(NativeFontRingState& state,
			const NativeFontStaticCandidate& candidate, UInt32 vertexCount,
			UInt32 pendingBytes, UInt32 pendingPayloads)
		{
			const UInt32 frame = GetStaticObservationFrame(state);
			ResetStaticPromotionBudgetFrame(state, frame);
			const UInt64 byteCount = static_cast<UInt64>(vertexCount)
				* sizeof(NativeFontGpuVertex);
			const UInt64 usedBytes = static_cast<UInt64>(
				state.staticPromotionBytesThisFrame) + pendingBytes;
			const UInt64 usedPayloads = static_cast<UInt64>(
				state.staticPromotionPayloadsThisFrame) + pendingPayloads;
			if (byteCount > kStaticPromotionBudgetBytes)
			{
				return !usedBytes && !usedPayloads
					&& GetStaticPromotionReadiness(state, candidate, vertexCount,
						frame, 2) == StaticPromotionReadiness::Ready;
			}
			return usedPayloads < kStaticPromotionPayloadLimit
				&& usedBytes + byteCount <= kStaticPromotionBudgetBytes;
		}

		void CommitStaticPromotionBudget(NativeFontRingState& state,
			UInt32 bytes, UInt32 payloads)
		{
			ResetStaticPromotionBudgetFrame(state,
				GetStaticObservationFrame(state));
			state.staticPromotionBytesThisFrame += bytes;
			state.staticPromotionPayloadsThisFrame += payloads;
		}

		void DeferStaticPromotionsLocked(NativeFontRingState& state, UInt32 frame)
		{
			state.staticPromotionGlobalRetryFrame = frame
				+ kStaticPromotionRetryFrames;
			if (!state.staticPromotionGlobalRetryFrame)
				state.staticPromotionGlobalRetryFrame = 1;
		}

		void ClearMatchingStaticResidency(
			const NativeFontRingState& state,
			const NativeFontStaticPayload& payload,
			const NativeFontPayloadTemplatePtr& owner)
		{
			if (!owner)
				return;
			NativeFontPayloadResidencyCache& residency = owner->residency;
			if (residency.staticResourceSerial
					!= state.resourceSerial.load(std::memory_order_relaxed)
				|| residency.staticBaseVertex != payload.baseVertex
				|| residency.staticVertexCount != payload.vertexCount)
			{
				return;
			}
			residency.staticResourceSerial = 0;
			residency.staticBaseVertex = 0;
			residency.staticVertexCount = 0;
			residency.staticLastUsedFrame = 0;
		}

		void ReclaimExpiredAndColdStaticPayloadsLocked(NativeFontRingState& state,
			UInt32 requiredVertices)
		{
			// Hot entries never own payload lifetime. Clear the local location cache
			// before erasing residency so it cannot resurrect a reclaimed tail slot.
			RingThread().staticPayload = {};

			const UInt32 previousNextVertex = state.nextStaticVertex;
			const UInt32 currentFrame = GetStaticObservationFrame(state);
			const bool underPressure = state.nextStaticVertex
				> state.staticVertexCapacity
				|| requiredVertices > state.staticVertexCapacity
					- std::min(state.nextStaticVertex,
						state.staticVertexCapacity);
			struct ColdPayload
			{
				const NativeFontPayloadTemplate* key = nullptr;
				UInt32 vertexCount = 0;
				UInt32 ageFrames = 0;
			};
			std::vector<ColdPayload> coldPayloads;
			CpuMemoryLease reclaimCpuMemory;
			if (underPressure)
				coldPayloads.reserve(state.staticPayloads.size());
			UInt64 liveVertexCount = 0;
			UInt32 removedPayloads = 0;
			UInt32 coldRemovedPayloads = 0;
			UInt64 coldRemovedVertices = 0;
			for (auto current = state.staticPayloads.begin();
				current != state.staticPayloads.end();)
			{
				const NativeFontStaticPayload payload = current->second;
				NativeFontPayloadTemplatePtr owner = payload.owner.lock();
				const bool valid = owner && owner.get() == current->first
					&& owner->gpuVertices.size() == payload.vertexCount
					&& payload.baseVertex <= state.staticVertexCapacity
					&& payload.vertexCount <= state.staticVertexCapacity
						- payload.baseVertex
					&& payload.baseVertex <= previousNextVertex
					&& payload.vertexCount <= previousNextVertex
						- payload.baseVertex;
				if (!valid)
				{
					if (owner && owner.get() == current->first)
						ClearMatchingStaticResidency(state, payload, owner);
					current = state.staticPayloads.erase(current);
					++removedPayloads;
					continue;
				}
				liveVertexCount += payload.vertexCount;
				if (underPressure)
				{
					const NativeFontPayloadResidencyCache& residency =
						owner->residency;
					const UInt32 ageFrames = static_cast<UInt32>(
						currentFrame - residency.staticLastUsedFrame);
					const StaticPromotionPolicy& policy =
						GetStaticPromotionPolicy(payload.vertexCount);
					if (ageFrames && ageFrames >= policy.coldResidentFrames)
					{
						coldPayloads.push_back({
							current->first, payload.vertexCount, ageFrames });
					}
				}
				++current;
			}
			reclaimCpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
				coldPayloads.capacity() * sizeof(ColdPayload));

			if (underPressure
				&& liveVertexCount + requiredVertices
					> state.staticVertexCapacity && !coldPayloads.empty())
			{
				std::sort(coldPayloads.begin(), coldPayloads.end(),
					[](const ColdPayload& left, const ColdPayload& right)
					{
						const UInt64 leftScore = static_cast<UInt64>(
							left.ageFrames) * left.vertexCount;
						const UInt64 rightScore = static_cast<UInt64>(
							right.ageFrames) * right.vertexCount;
						if (leftScore != rightScore)
							return leftScore > rightScore;
						return left.vertexCount > right.vertexCount;
					});
				for (const ColdPayload& cold : coldPayloads)
				{
					if (liveVertexCount + requiredVertices
						<= state.staticVertexCapacity)
					{
						break;
					}
					auto found = state.staticPayloads.find(cold.key);
					if (found == state.staticPayloads.end())
						continue;
					const NativeFontStaticPayload payload = found->second;
					NativeFontPayloadTemplatePtr owner = payload.owner.lock();
					if (owner && owner.get() == found->first)
						ClearMatchingStaticResidency(state, payload, owner);
					liveVertexCount -= std::min<UInt64>(
						liveVertexCount, payload.vertexCount);
					coldRemovedVertices += payload.vertexCount;
					state.staticPayloads.erase(found);
					++coldRemovedPayloads;
					++removedPayloads;
				}
			}

			UInt32 liveEndVertex = 0;
			for (const auto& entry : state.staticPayloads)
			{
				liveEndVertex = std::max(liveEndVertex,
					entry.second.baseVertex + entry.second.vertexCount);
			}

			if (liveEndVertex < state.nextStaticVertex)
				state.nextStaticVertex = liveEndVertex;
			if (coldRemovedPayloads)
			{
				// Cold entries can still have live command/singleton-facade descriptors.
				// Invalidate the shared resource identity before a reclaimed tail range
				// is reused, then republish only the retained static locations.
				InvalidateAllSingletonFacadeBindings();
				AdvanceUploadEpochLocked(state);
				state.uploadedPayloads.clear();
				const UInt32 resourceSerial =
					AdvanceResourceSerialLocked(state);
				for (const auto& entry : state.staticPayloads)
				{
					NativeFontPayloadTemplatePtr owner = entry.second.owner.lock();
					if (!owner || owner.get() != entry.first)
						continue;
					NativeFontPayloadResidencyCache& residency = owner->residency;
					residency.staticResourceSerial = resourceSerial;
					residency.staticBaseVertex = entry.second.baseVertex;
					residency.staticVertexCount = entry.second.vertexCount;
				}
				RingThread().staticPayload = {};
				RingThread().uploadedPayload = {};
				RingThread().staticCandidate = {};
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticResidentColdEviction,
					coldRemovedPayloads);
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticResidentColdEvictionBytes,
					coldRemovedVertices * sizeof(NativeFontGpuVertex));
			}
			if (removedPayloads)
				RefreshRingCpuMemoryLocked(state);

			const UInt32 reclaimedVertices =
				previousNextVertex - state.nextStaticVertex;
			if (g_bEnableFreeTypeFontRenderingLog
				&& (removedPayloads || reclaimedVertices))
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_native: static vertex residency reclaimed removedPayloads=%u coldPayloads=%u coldBytes=%llu reclaimedTailVertices=%u residentVertices=%u capacity=%u requestedVertices=%u",
					removedPayloads, coldRemovedPayloads,
					static_cast<unsigned long long>(coldRemovedVertices
						* sizeof(NativeFontGpuVertex)), reclaimedVertices,
					state.nextStaticVertex, state.staticVertexCapacity,
					requiredVertices);
			}
		}

		bool TryGrowStaticVertexBufferLocked(NativeFontRingState& state,
			UInt32 requiredVertices, bool& permanentFailure)
		{
			permanentFailure = false;
			if (requiredVertices > kStaticTargetVertexCapacity)
			{
				permanentFailure = true;
				return false;
			}
			if (!state.device || !state.staticVertexBuffer || !requiredVertices)
				return false;

			// A published submission can still be issuing packets against the old
			// buffer. The one permitted in-use proxy is only the caller's reserved,
			// not-yet-published proxy; any other proxy or active submission makes
			// replacement unsafe.
			const UInt32 activeProxies = static_cast<UInt32>(std::count_if(
				state.proxies.begin(), state.proxies.begin() + state.proxyCount,
				[](const NativeFontProxy& proxy) { return proxy.inUse; }));
			if (state.activeSubmissions.load(std::memory_order_acquire)
				|| state.sortedFrameLeases.load(std::memory_order_acquire)
				|| activeProxies > 1)
				return false;

			ReclaimExpiredAndColdStaticPayloadsLocked(state, requiredVertices);
			if (state.nextStaticVertex <= state.staticVertexCapacity
				&& requiredVertices <= state.staticVertexCapacity
					- state.nextStaticVertex)
			{
				return true;
			}

			std::vector<LiveStaticPayload> livePayloads;
			CpuMemoryLease rebuildCpuMemory;
			livePayloads.reserve(state.staticPayloads.size());
			rebuildCpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
				livePayloads.capacity() * sizeof(LiveStaticPayload));
			UInt64 liveVertexCount = 0;
			for (const auto& entry : state.staticPayloads)
			{
				NativeFontPayloadTemplatePtr owner = entry.second.owner.lock();
				if (!owner || owner.get() != entry.first
					|| owner->gpuVertices.size() != entry.second.vertexCount)
				{
					continue;
				}
				if (liveVertexCount + owner->gpuVertices.size()
					> kStaticTargetVertexCapacity)
				{
					return false;
				}
				livePayloads.push_back({ std::move(owner),
					static_cast<UInt32>(liveVertexCount) });
				liveVertexCount += livePayloads.back().owner->gpuVertices.size();
			}

			const UInt64 requiredCapacity = liveVertexCount + requiredVertices;
			if (requiredCapacity > kStaticTargetVertexCapacity)
				return false;
			const UInt64 currentCapacity = state.staticVertexCapacity;
			const UInt64 reserveVertices = std::max<UInt64>(4,
				currentCapacity / kStaticCompactionReserveDivisor);
			const UInt64 capacityTarget = std::min<UInt64>(
				kStaticTargetVertexCapacity,
				requiredCapacity + reserveVertices);
			UInt64 desiredCapacity = currentCapacity;
			while (desiredCapacity < capacityTarget
				&& desiredCapacity < kStaticTargetVertexCapacity)
			{
				desiredCapacity = std::min<UInt64>(
					desiredCapacity * 2u, kStaticTargetVertexCapacity);
			}
			// Rebuilding at the same size is useful only when expired payloads left
			// holes behind the bump pointer. It compacts the live prefix in one upload.
			if (desiredCapacity == state.staticVertexCapacity
				&& liveVertexCount == state.nextStaticVertex)
				return false;
			const bool sameSizeCompaction =
				desiredCapacity == state.staticVertexCapacity;
			const UInt32 currentFrame = GetStaticObservationFrame(state);
			if (sameSizeCompaction && state.staticCompactionFrameValid
				&& static_cast<UInt32>(currentFrame
					- state.lastStaticCompactionFrame)
					< kStaticCompactionCooldownFrames)
			{
				if (g_bEnableFreeTypeFontRenderingLog
					&& (!state.staticCompactionDeferredLogFrameValid
						|| state.lastStaticCompactionDeferredLogFrame
							!= currentFrame))
				{
					state.lastStaticCompactionDeferredLogFrame = currentFrame;
					state.staticCompactionDeferredLogFrameValid = true;
					FreeTypeFontDebugLog(
						"tnvse_freetype_native: static vertex buffer rebuild deferred reason=compaction-cooldown frame=%u lastCompactionFrame=%u cooldownFrames=%u liveVertices=%u capacity=%u requestedVertices=%u",
						currentFrame, state.lastStaticCompactionFrame,
						kStaticCompactionCooldownFrames,
						static_cast<UInt32>(liveVertexCount),
						state.staticVertexCapacity, requiredVertices);
				}
				return false;
			}

			IDirect3DVertexBuffer9* replacement = nullptr;
			HRESULT result = state.device->CreateVertexBuffer(
				static_cast<UINT>(desiredCapacity) * sizeof(NativeFontGpuVertex),
				D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &replacement, nullptr);
			if (FAILED(result) || !replacement)
			{
				if (replacement)
					replacement->Release();
				return false;
			}

			if (liveVertexCount)
			{
				void* destination = nullptr;
				const UINT liveBytes = static_cast<UINT>(liveVertexCount)
					* sizeof(NativeFontGpuVertex);
				result = replacement->Lock(0, liveBytes, &destination, 0);
				if (FAILED(result) || !destination)
				{
					replacement->Release();
					return false;
				}
				for (const LiveStaticPayload& payload : livePayloads)
				{
					std::memcpy(static_cast<UInt8*>(destination)
						+ payload.baseVertex * sizeof(NativeFontGpuVertex),
						payload.owner->gpuVertices.data(),
						payload.owner->gpuVertices.size()
							* sizeof(NativeFontGpuVertex));
				}
				result = replacement->Unlock();
				if (FAILED(result))
				{
					replacement->Release();
					return false;
				}
			}
			if (liveVertexCount)
				AdvanceDiagnosticSerial(state.staticWriteSerial);

			std::unordered_map<const NativeFontPayloadTemplate*,
				NativeFontStaticPayload> rebuilt;
			rebuilt.reserve(livePayloads.size());
			for (const LiveStaticPayload& payload : livePayloads)
			{
				rebuilt.emplace(payload.owner.get(), NativeFontStaticPayload{
					payload.owner, payload.baseVertex,
					static_cast<UInt32>(payload.owner->gpuVertices.size()),
					state.staticWriteSerial,
					HashDiagnosticPayload(*payload.owner) });
			}
			rebuildCpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
				livePayloads.capacity() * sizeof(LiveStaticPayload)
					+ EstimateUnorderedMapBytes(rebuilt));
			for (UInt32 index = 0; index < state.proxyCount; ++index)
			{
				if (state.proxies[index].chip)
					state.proxies[index].chip->m_pkVB = state.vertexBuffer;
			}
			InvalidateAllSingletonFacadeBindings();
			state.staticVertexBuffer->Release();
			state.staticVertexBuffer = replacement;
			state.staticVertexCapacity = static_cast<UInt32>(desiredCapacity);
			state.nextStaticVertex = static_cast<UInt32>(liveVertexCount);
			state.staticPayloads = std::move(rebuilt);
			if (sameSizeCompaction)
			{
				state.lastStaticCompactionFrame = currentFrame;
				state.staticCompactionFrameValid = true;
			}
			else
			{
				state.lastStaticCompactionFrame = 0;
				state.staticCompactionFrameValid = false;
			}
			rebuildCpuMemory.Release();
			RefreshRingCpuMemoryLocked(state);
			const UInt32 resourceSerial = AdvanceResourceSerialLocked(state);
			for (const LiveStaticPayload& payload : livePayloads)
			{
				NativeFontPayloadResidencyCache& residency =
					payload.owner->residency;
				residency.staticResourceSerial = resourceSerial;
				residency.staticBaseVertex = payload.baseVertex;
				residency.staticVertexCount = static_cast<UInt32>(
					payload.owner->gpuVertices.size());
			}
			RingThread().staticPayload = {};
			RingThread().uploadedPayload = {};
			RingThread().staticCandidate = {};
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				const UInt32 retainedHeadroom = static_cast<UInt32>(
					desiredCapacity - requiredCapacity);
				gLog.FormattedMessage(
					"tnvse_freetype_native: static vertex buffer rebuilt capacity=%u bytes=%u liveVertices=%u livePayloads=%u requestedVertices=%u mode=%s reserveVertices=%u copiedBytes=%u",
					state.staticVertexCapacity,
					state.staticVertexCapacity * sizeof(NativeFontGpuVertex),
					state.nextStaticVertex,
					static_cast<UInt32>(state.staticPayloads.size()),
					requiredVertices,
					sameSizeCompaction ? "compact" : "grow",
					retainedHeadroom,
					state.nextStaticVertex
						* static_cast<UInt32>(sizeof(NativeFontGpuVertex)));
			}
			return true;
		}

		bool BindPacketAtlasPage(NativeFontProxy& proxy,
			const NativeFontPayloadTemplate& artifact, UInt16 page)
		{
			if (page >= artifact.atlasProperties.size()
				|| page >= artifact.atlasTextures.size()
				|| !artifact.atlasProperties[page]
				|| !artifact.atlasTextures[page])
			{
				return false;
			}

			NiTexturingProperty* desiredProperty =
				artifact.atlasProperties[page].m_pObject;
			NiTexture* desiredTexture = artifact.atlasTextures[page].m_pObject;
			if (!proxy.shape || !proxy.tile)
				return false;
			if (proxy.atlasProperty != desiredProperty)
			{
				proxy.shape->RemoveProperty(NiProperty::TEXTURING);
				proxy.shape->AddProperty(desiredProperty);
				proxy.shape->UpdateProperties();
				proxy.atlasProperty = proxy.shape->GetTexturingProperty();
				if (proxy.atlasProperty != desiredProperty)
					return false;
			}
			if (proxy.atlasTexture != desiredTexture
				|| proxy.tile->sourceTexture.m_pObject != desiredTexture)
			{
				ThisStdCall<void>(0xBB7A10, proxy.tile, desiredTexture);
				proxy.atlasTexture = proxy.tile->sourceTexture.m_pObject;
			}
			return proxy.atlasTexture == desiredTexture;
		}

		void CopyScissorTail(const NiTriShape& source, NiTriShape& destination)
		{
			std::memcpy(reinterpret_cast<UInt8*>(&destination) + kScissorTailOffset,
				reinterpret_cast<const UInt8*>(&source) + kScissorTailOffset,
				kScissorTailSize);
		}

		void CopyTileDynamicState(const TileShaderPropertyView& source,
			TileShaderPropertyView& destination)
		{
			destination.m_usFlags = source.m_usFlags;
			destination.ulFlags[0] = source.ulFlags[0];
			destination.ulFlags[1] = source.ulFlags[1];
			destination.fAlpha = source.fAlpha;
			destination.fFadeAlpha = source.fFadeAlpha;
			destination.fEnvMapScale = source.fEnvMapScale;
			destination.fLODFade = source.fLODFade;
			destination.fDepthBias = source.fDepthBias;
			destination.uiShaderIndex = source.uiShaderIndex;
			if (destination.alphaTexture.m_pObject != source.alphaTexture.m_pObject)
				destination.alphaTexture = source.alphaTexture;
			destination.overlayColor = source.overlayColor;
			destination.tileAlpha = source.tileAlpha;
			destination.textureTransform = source.textureTransform;
			destination.clampMode = source.clampMode;
			destination.byte90 = source.byte90;
			destination.rotates = source.rotates;
			destination.hasVertexColors = true;
			destination.noTexture = false;
			destination.scissorRect = source.scissorRect;
			destination.useScissorTest = source.useScissorTest;
			// sourceTexture and texturePath deliberately remain page-specific.
		}

		void ApplyRelativeOrigin(NiTransform& destination,
			const NiTransform& source, const NiPoint3& origin)
		{
			destination = source;
			if (origin.x != 0.0f || origin.y != 0.0f || origin.z != 0.0f)
				destination.m_Translate = source * origin;
		}

		bool SyncProxyState(const NiTriShape& facade, NativeFontProxy& proxyState,
			const NiPoint3& geometryOrigin)
		{
			const TileShaderPropertyView* sourceTile = GetTileProperty(&facade);
			const NiAlphaProperty* sourceAlpha = facade.GetAlphaProperty();
			NiTriShape* proxy = proxyState.shape.m_pObject;
			NiAlphaProperty* proxyAlpha = proxyState.alphaProperty.m_pObject;
			TileShaderPropertyView* proxyTile = proxyState.tile;
			if (!sourceTile || !proxyTile || !proxyTile->sourceTexture
				|| !sourceAlpha || !proxyAlpha || !proxy
				|| !proxyState.atlasProperty)
			{
				return false;
			}

			ApplyRelativeOrigin(proxy->m_kLocal, facade.m_kLocal, geometryOrigin);
			ApplyRelativeOrigin(proxy->m_kWorld, facade.m_kWorld, geometryOrigin);
			CopyScissorTail(facade, *proxy);

			if (facade.m_pWorldBound)
			{
				if (!proxy->m_pWorldBound)
					proxy->CreateWorldBoundIfMissing();
				if (!proxy->m_pWorldBound)
					return false;
				*proxy->m_pWorldBound = *facade.m_pWorldBound;
			}
			proxy->m_uiFlags = facade.m_uiFlags;
			// Keep the blend/sort contract but clear only alpha testing. Aliasing
			// the facade property lets the vanilla pass re-enable the threshold.
			proxyAlpha->m_usFlags = sourceAlpha->m_usFlags;
			proxyAlpha->m_ucAlphaTestRef = sourceAlpha->m_ucAlphaTestRef;
			proxyAlpha->SetAlphaTesting(false);
			if (proxy->m_kProperties.m_spAlphaProperty.m_pObject
				!= proxyAlpha)
				proxy->m_kProperties.m_spAlphaProperty = proxyAlpha;
			if (proxy->m_kProperties.m_spCullingProperty.m_pObject
				!= facade.m_kProperties.m_spCullingProperty.m_pObject)
				proxy->m_kProperties.m_spCullingProperty =
					facade.m_kProperties.m_spCullingProperty;
			if (proxy->m_kProperties.m_spMaterialProperty.m_pObject
				!= facade.m_kProperties.m_spMaterialProperty.m_pObject)
				proxy->m_kProperties.m_spMaterialProperty =
					facade.m_kProperties.m_spMaterialProperty;
			if (proxy->m_kProperties.m_spStencilProperty.m_pObject
				!= facade.m_kProperties.m_spStencilProperty.m_pObject)
				proxy->m_kProperties.m_spStencilProperty =
					facade.m_kProperties.m_spStencilProperty;
			if (proxy->m_kProperties.m_spUnknownProperty.m_pObject
				!= facade.m_kProperties.m_spUnknownProperty.m_pObject)
				proxy->m_kProperties.m_spUnknownProperty =
					facade.m_kProperties.m_spUnknownProperty;
			CopyTileDynamicState(*sourceTile, *proxyTile);
			return true;
		}

		UInt32 AcquireProxyLocked(NativeFontRingState& state,
			NativeFontRingThreadState& thread)
		{
			if (thread.preferredProxy < state.proxyCount
				&& !state.proxies[thread.preferredProxy].inUse)
			{
				state.proxies[thread.preferredProxy].inUse = true;
				return thread.preferredProxy;
			}
			for (UInt32 index = 0; index < state.proxyCount; ++index)
			{
				if (!state.proxies[index].inUse)
				{
					state.proxies[index].inUse = true;
					thread.preferredProxy = index;
					return index;
				}
			}
			return std::numeric_limits<UInt32>::max();
		}

		void MarkStaticPayloadUsedLocked(NativeFontRingState& state,
			const NativeFontPayloadTemplatePtr& payloadTemplate)
		{
			NativeFontPayloadResidencyCache& residency =
				payloadTemplate->residency;
			const UInt32 frame = GetStaticObservationFrame(state);
			if (residency.staticLastUsedFrame != frame)
				residency.staticLastUsedFrame = frame;
		}

		bool ResolveStaticPayloadLocked(NativeFontRingState& state,
			const NativeFontPayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount, UInt32& baseVertex)
		{
			if (!state.staticVertexBuffer)
				return false;
			const UInt32 resourceSerial = state.resourceSerial.load(
				std::memory_order_relaxed);
			NativeFontPayloadResidencyCache& residency =
				payloadTemplate->residency;
			if (residency.staticResourceSerial == resourceSerial
				&& residency.staticVertexCount == vertexCount
				&& residency.staticBaseVertex <= state.staticVertexCapacity
				&& vertexCount <= state.staticVertexCapacity
					- residency.staticBaseVertex)
			{
				baseVertex = residency.staticBaseVertex;
				MarkStaticPayloadUsedLocked(state, payloadTemplate);
				RecordFreeTypePerf(FreeTypePerfCounter::StaticVertexHit);
				RecordFreeTypePerf(
					FreeTypePerfCounter::DirectStaticResidencyHit);
				return true;
			}
			NativeFontStaticHotEntry& hot = RingThread().staticPayload;
			if (hot.key == payloadTemplate.get()
				&& hot.resourceSerial == resourceSerial)
			{
				const NativeFontPayloadTemplatePtr hotOwner = hot.owner.lock();
				if (hotOwner.get() == payloadTemplate.get()
					&& residency.staticResourceSerial == resourceSerial
					&& residency.staticBaseVertex == hot.baseVertex
					&& residency.staticVertexCount == hot.vertexCount
					&& hot.vertexCount == vertexCount
					&& hot.baseVertex <= state.staticVertexCapacity
					&& vertexCount <= state.staticVertexCapacity - hot.baseVertex)
				{
					baseVertex = hot.baseVertex;
					MarkStaticPayloadUsedLocked(state, payloadTemplate);
					RecordFreeTypePerf(FreeTypePerfCounter::StaticVertexHit);
					return true;
				}
				hot = {};
			}
			auto found = state.staticPayloads.find(payloadTemplate.get());
			if (found == state.staticPayloads.end())
				return false;
			const std::shared_ptr<const NativeFontPayloadTemplate> owner =
				found->second.owner.lock();
			if (owner.get() == payloadTemplate.get()
				&& found->second.vertexCount == vertexCount
				&& found->second.baseVertex <= state.staticVertexCapacity
				&& vertexCount <= state.staticVertexCapacity
					- found->second.baseVertex)
			{
				baseVertex = found->second.baseVertex;
				hot.key = payloadTemplate.get();
				hot.owner = owner;
				hot.baseVertex = found->second.baseVertex;
				hot.vertexCount = found->second.vertexCount;
				hot.resourceSerial = resourceSerial;
				residency.staticResourceSerial = resourceSerial;
				residency.staticBaseVertex = found->second.baseVertex;
				residency.staticVertexCount = found->second.vertexCount;
				MarkStaticPayloadUsedLocked(state, payloadTemplate);
				RecordFreeTypePerf(FreeTypePerfCounter::StaticVertexHit);
				return true;
			}
			state.staticPayloads.erase(found);
			RefreshRingCpuMemoryLocked(state);
			return false;
		}

		bool IsStaticPayloadCurrentLocked(const NativeFontRingState& state,
			const NativeFontPayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount)
		{
			if (!state.staticVertexBuffer || !payloadTemplate)
				return false;
			const NativeFontPayloadResidencyCache& residency =
				payloadTemplate->residency;
			return residency.staticResourceSerial
					== state.resourceSerial.load(std::memory_order_relaxed)
				&& residency.staticVertexCount == vertexCount
				&& residency.staticBaseVertex <= state.staticVertexCapacity
				&& vertexCount <= state.staticVertexCapacity
					- residency.staticBaseVertex;
		}

		NativeFontStaticCandidate* ResolveStaticCandidateLocked(
			NativeFontRingState& state,
			const NativeFontPayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount, bool allowCreate)
		{
			// Candidate observation must use the strategy's growth ceiling rather
			// than the current allocation. Otherwise a payload larger than the
			// initial 4 MiB buffer can never mature and therefore can never trigger
			// the compaction/growth path that was designed to accommodate it.
			if (!state.staticVertexBuffer || vertexCount > kStaticTargetVertexCapacity)
				return nullptr;
			const UInt32 resourceSerial = state.resourceSerial.load(
				std::memory_order_relaxed);
			NativeFontCandidateHotEntry& hot = RingThread().staticCandidate;
			if (hot.key == payloadTemplate.get()
				&& hot.resourceSerial == resourceSerial && hot.candidate)
			{
				const NativeFontPayloadTemplatePtr hotOwner = hot.owner.lock();
				if (hotOwner.get() == payloadTemplate.get())
					return hot.candidate.get();
				hot = {};
			}
			bool memoryChanged = false;
			auto found = state.staticCandidates.find(payloadTemplate.get());
			if (found == state.staticCandidates.end())
			{
				if (!allowCreate)
					return nullptr;
				if (state.staticCandidates.size() >= kStaticCandidateLimit)
				{
					const UInt32 frame = GetStaticObservationFrame(state);
					if (!state.staticCandidateSweepFrameValid
						|| static_cast<UInt32>(frame
							- state.lastStaticCandidateSweepFrame)
							>= kStaticCandidateSweepIntervalFrames)
					{
						state.lastStaticCandidateSweepFrame = frame;
						state.staticCandidateSweepFrameValid = true;
						RingThread().staticCandidate = {};
						for (auto current = state.staticCandidates.begin();
							current != state.staticCandidates.end();)
						{
							const bool inactive = current->second
								&& current->second->observationFrameValid
								&& static_cast<UInt32>(frame
									- current->second->lastObservedFrame)
									> kStaticCandidateInactiveFrames;
							if (!current->second
								|| current->second->owner.expired() || inactive)
							{
								current = state.staticCandidates.erase(current);
								memoryChanged = true;
							}
							else
								++current;
						}
					}
					if (state.staticCandidates.size() >= kStaticCandidateLimit)
					{
						if (memoryChanged)
							RefreshRingCpuMemoryLocked(state);
						return nullptr;
					}
				}
				auto candidate = CreateStaticCandidate(payloadTemplate);
				found = state.staticCandidates.emplace(
					payloadTemplate.get(), std::move(candidate)).first;
				memoryChanged = true;
				if (state.staticCandidates.size() >= kStaticCandidateLimit)
				{
					state.lastStaticCandidateSweepFrame =
						GetStaticObservationFrame(state);
					state.staticCandidateSweepFrameValid = true;
				}
			}
			else
			{
				const std::shared_ptr<const NativeFontPayloadTemplate> owner =
					found->second->owner.lock();
				if (owner.get() != payloadTemplate.get())
				{
					found->second = CreateStaticCandidate(payloadTemplate);
					memoryChanged = true;
				}
			}
			if (memoryChanged)
				RefreshRingCpuMemoryLocked(state);
			hot.key = payloadTemplate.get();
			hot.owner = payloadTemplate;
			hot.candidate = found->second;
			hot.resourceSerial = resourceSerial;
			return hot.candidate.get();
		}

		NativeFontStaticCandidate* ObserveStaticCandidateLocked(
			NativeFontRingState& state,
			const NativeFontPayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount, bool allowCreate)
		{
			NativeFontStaticCandidate* candidate = ResolveStaticCandidateLocked(
				state, payloadTemplate, vertexCount, allowCreate);
			if (!candidate || candidate->promotionDisabled)
				return candidate;

			const UInt32 frame = GetStaticObservationFrame(state);
			if (!candidate->observationFrameValid)
			{
				candidate->firstObservedFrame = frame;
				candidate->lastObservedFrame = frame;
				candidate->activeObservedFrames = 1;
				candidate->observationFrameValid = true;
			}
			else if (candidate->lastObservedFrame != frame)
			{
				const UInt32 gap = static_cast<UInt32>(
					frame - candidate->lastObservedFrame);
				if (gap > kStaticPromotionMaximumFrameGap)
				{
					candidate->firstObservedFrame = frame;
					candidate->activeObservedFrames = 1;
					candidate->dynamicUploadEpochCount = 0;
					candidate->lastDynamicUploadEpoch = 0;
					candidate->lastDynamicUploadResourceSerial = 0;
				}
				else if (candidate->activeObservedFrames
					< std::numeric_limits<UInt32>::max())
				{
					++candidate->activeObservedFrames;
				}
				candidate->lastObservedFrame = frame;
			}
			return candidate;
		}

		void NoteStaticCandidateDynamicUploadLocked(NativeFontRingState& state,
			const NativeFontPayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount)
		{
			NativeFontStaticCandidate* candidate = ObserveStaticCandidateLocked(
				state, payloadTemplate, vertexCount, true);
			const UInt32 resourceSerial = state.resourceSerial.load(
				std::memory_order_relaxed);
			if (!candidate || candidate->promotionDisabled
				|| (candidate->lastDynamicUploadEpoch == state.uploadEpoch
					&& candidate->lastDynamicUploadResourceSerial
						== resourceSerial))
			{
				return;
			}
			candidate->lastDynamicUploadEpoch = state.uploadEpoch;
			candidate->lastDynamicUploadResourceSerial = resourceSerial;
			if (candidate->dynamicUploadEpochCount
				< std::numeric_limits<UInt32>::max())
			{
				++candidate->dynamicUploadEpochCount;
			}
		}

		void RecordStaticPromotionDeferral(StaticPromotionReadiness readiness,
			UInt64 amount)
		{
			if (!amount)
				return;
			switch (readiness)
			{
			case StaticPromotionReadiness::Lifecycle:
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticPromotionDeferredLifecycle,
					amount);
				break;
			case StaticPromotionReadiness::UploadHistory:
				RecordFreeTypePerf(FreeTypePerfCounter::
					StaticPromotionDeferredUploadHistory, amount);
				break;
			case StaticPromotionReadiness::Retry:
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticPromotionDeferredRetry,
					amount);
				break;
			default:
				break;
			}
		}

		bool PromoteStaticPayloadLocked(NativeFontRingState& state,
			const NativeFontPayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount, UInt32& baseVertex)
		{
			NativeFontStaticCandidate* candidate =
				ObserveStaticCandidateLocked(
					state, payloadTemplate, vertexCount, false);
			if (!candidate)
			{
				return false;
			}
			const UInt32 frame = GetStaticObservationFrame(state);
			const StaticPromotionReadiness readiness =
				GetStaticPromotionReadiness(state, *candidate,
					vertexCount, frame);
			if (readiness != StaticPromotionReadiness::Ready)
			{
				RecordStaticPromotionDeferral(readiness);
				return false;
			}
			const UInt32 byteCount = vertexCount * sizeof(NativeFontGpuVertex);
			if (!FitsStaticPromotionBudget(state, *candidate,
				vertexCount, 0, 0))
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticPromotionDeferredBudget);
				return false;
			}

			if (state.nextStaticVertex > state.staticVertexCapacity
				|| vertexCount > state.staticVertexCapacity
					- state.nextStaticVertex)
			{
				bool permanentFailure = false;
				if (!TryGrowStaticVertexBufferLocked(state, vertexCount,
					permanentFailure))
				{
					candidate->promotionDisabled = permanentFailure;
					if (!permanentFailure)
					{
						candidate->nextRetryFrame = frame
							+ kStaticPromotionRetryFrames;
						DeferStaticPromotionsLocked(state, frame);
					}
					RecordFreeTypePerf(
						FreeTypePerfCounter::StaticVertexPromotionFailed);
					return false;
				}
			}

			baseVertex = state.nextStaticVertex;
			const UINT byteOffset = baseVertex * sizeof(NativeFontGpuVertex);
			void* destination = nullptr;
			HRESULT result = state.staticVertexBuffer->Lock(byteOffset, byteCount,
				&destination, 0);
			if (FAILED(result) || !destination)
			{
				candidate->nextRetryFrame = frame
					+ kStaticPromotionRetryFrames;
				DeferStaticPromotionsLocked(state, frame);
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticVertexPromotionFailed);
				return false;
			}
			std::memcpy(destination, payloadTemplate->gpuVertices.data(), byteCount);
			result = state.staticVertexBuffer->Unlock();
			if (FAILED(result))
			{
				state.nextStaticVertex = baseVertex + vertexCount;
				CommitStaticPromotionBudget(state, byteCount, 1);
				candidate->nextRetryFrame = frame
					+ kStaticPromotionRetryFrames;
				DeferStaticPromotionsLocked(state, frame);
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticVertexPromotionFailed);
				return false;
			}

			AdvanceDiagnosticSerial(state.staticWriteSerial);
			state.nextStaticVertex = baseVertex + vertexCount;
			state.staticPayloads[payloadTemplate.get()] = {
				payloadTemplate, baseVertex, vertexCount,
				state.staticWriteSerial,
				HashDiagnosticPayload(*payloadTemplate) };
			NativeFontPayloadResidencyCache& residency =
				payloadTemplate->residency;
			residency.staticResourceSerial = state.resourceSerial.load(
				std::memory_order_relaxed);
			residency.staticBaseVertex = baseVertex;
			residency.staticVertexCount = vertexCount;
			residency.staticLastUsedFrame = frame;
			state.staticCandidates.erase(payloadTemplate.get());
			RingThread().staticPayload = {
				payloadTemplate.get(), payloadTemplate, baseVertex, vertexCount,
				state.resourceSerial.load(std::memory_order_relaxed) };
			if (RingThread().staticCandidate.key == payloadTemplate.get())
				RingThread().staticCandidate = {};
			RefreshRingCpuMemoryLocked(state);
			CommitStaticPromotionBudget(state, byteCount, 1);
			RecordFreeTypePerf(FreeTypePerfCounter::StaticVertexUpload);
			RecordFreeTypePerf(FreeTypePerfCounter::StaticVertexUploadBytes,
				byteCount);
			return true;
		}

		bool ResolveUploadedPayloadLocked(NativeFontRingState& state,
			const NativeFontPayloadTemplatePtr& payloadTemplate,
			UInt32 vertexCount, UInt32& baseVertex)
		{
			const UInt32 resourceSerial = state.resourceSerial.load(
				std::memory_order_relaxed);
			NativeFontPayloadResidencyCache& residency =
				payloadTemplate->residency;
			if (residency.dynamicResourceSerial == resourceSerial
				&& residency.dynamicUploadEpoch == state.uploadEpoch
				&& residency.dynamicVertexCount == vertexCount
				&& residency.dynamicBaseVertex <= state.vertexCapacity
				&& vertexCount <= state.vertexCapacity
					- residency.dynamicBaseVertex)
			{
				baseVertex = residency.dynamicBaseVertex;
				RecordFreeTypePerf(FreeTypePerfCounter::DynamicVertexReuse);
				RecordFreeTypePerf(
					FreeTypePerfCounter::DirectDynamicResidencyHit);
				return true;
			}

			NativeFontUploadHotEntry& hot = RingThread().uploadedPayload;
			if (hot.key == payloadTemplate.get()
				&& hot.resourceSerial == resourceSerial
				&& hot.epoch == state.uploadEpoch)
			{
				if (hot.owner.get() == payloadTemplate.get()
					&& hot.vertexCount == vertexCount
					&& hot.baseVertex <= state.vertexCapacity
					&& vertexCount <= state.vertexCapacity - hot.baseVertex)
				{
					baseVertex = hot.baseVertex;
					residency.dynamicResourceSerial = resourceSerial;
					residency.dynamicUploadEpoch = state.uploadEpoch;
					residency.dynamicBaseVertex = hot.baseVertex;
					residency.dynamicVertexCount = hot.vertexCount;
					RecordFreeTypePerf(FreeTypePerfCounter::DynamicVertexReuse);
					return true;
				}
				hot = {};
			}

			auto uploaded = state.uploadedPayloads.find(payloadTemplate.get());
			if (uploaded == state.uploadedPayloads.end())
				return false;
			const std::shared_ptr<const NativeFontPayloadTemplate> owner =
				uploaded->second.owner.lock();
			if (owner.get() == payloadTemplate.get()
				&& uploaded->second.epoch == state.uploadEpoch
				&& uploaded->second.vertexCount == vertexCount
				&& uploaded->second.baseVertex <= state.vertexCapacity
				&& vertexCount <= state.vertexCapacity
					- uploaded->second.baseVertex)
			{
				baseVertex = uploaded->second.baseVertex;
				hot = {
					payloadTemplate.get(), owner,
					uploaded->second.baseVertex, uploaded->second.vertexCount,
					uploaded->second.epoch, resourceSerial };
				residency.dynamicResourceSerial = resourceSerial;
				residency.dynamicUploadEpoch = state.uploadEpoch;
				residency.dynamicBaseVertex = uploaded->second.baseVertex;
				residency.dynamicVertexCount = uploaded->second.vertexCount;
				RecordFreeTypePerf(FreeTypePerfCounter::DynamicVertexReuse);
				return true;
			}
			state.uploadedPayloads.erase(uploaded);
			RefreshRingCpuMemoryLocked(state);
			return false;
		}

		bool HasDirectStaticPayloadLocked(const NativeFontRingState& state,
			const NativeFontPayloadTemplate& payloadTemplate,
			UInt32 vertexCount)
		{
			const NativeFontPayloadResidencyCache& residency =
				payloadTemplate.residency;
			return residency.staticResourceSerial
					== state.resourceSerial.load(std::memory_order_relaxed)
				&& residency.staticVertexCount == vertexCount
				&& residency.staticBaseVertex <= state.staticVertexCapacity
				&& vertexCount <= state.staticVertexCapacity
					- residency.staticBaseVertex;
		}

		bool HasDirectUploadedPayloadLocked(const NativeFontRingState& state,
			const NativeFontPayloadTemplate& payloadTemplate,
			UInt32 vertexCount)
		{
			const NativeFontPayloadResidencyCache& residency =
				payloadTemplate.residency;
			return residency.dynamicResourceSerial
					== state.resourceSerial.load(std::memory_order_relaxed)
				&& residency.dynamicUploadEpoch == state.uploadEpoch
				&& residency.dynamicVertexCount == vertexCount
				&& residency.dynamicBaseVertex <= state.vertexCapacity
				&& vertexCount <= state.vertexCapacity
					- residency.dynamicBaseVertex;
		}

		void PublishUploadedPayloadLocked(NativeFontRingState& state,
			const NativeFontPayloadTemplatePtr& payloadTemplate,
			UInt32 baseVertex, UInt32 vertexCount)
		{
			const UInt32 resourceSerial = state.resourceSerial.load(
				std::memory_order_relaxed);
			state.uploadedPayloads[payloadTemplate.get()] = {
				payloadTemplate, baseVertex, vertexCount, state.uploadEpoch,
				state.dynamicWriteSerial, state.dynamicDiscardSerial,
				HashDiagnosticPayload(*payloadTemplate) };
			NativeFontPayloadResidencyCache& residency =
				payloadTemplate->residency;
			residency.dynamicResourceSerial = resourceSerial;
			residency.dynamicUploadEpoch = state.uploadEpoch;
			residency.dynamicBaseVertex = baseVertex;
			residency.dynamicVertexCount = vertexCount;
			NoteStaticCandidateDynamicUploadLocked(state, payloadTemplate,
				vertexCount);
			RingThread().uploadedPayload = {
				payloadTemplate.get(), payloadTemplate, baseVertex, vertexCount,
				state.uploadEpoch, resourceSerial };
		}

		bool ResolveSortedLeaseResidency(
			const NativeFontRingState& state,
			const NativeFontPayloadTemplate& artifact, UInt32 vertexCount,
			UInt32 resourceSerial, UInt32 uploadEpoch,
			UInt32& baseVertex, bool& staticResident)
		{
			NativeFontPayloadResidencyCache& residency =
				artifact.residency;
			if (state.staticVertexBuffer
				&& residency.staticResourceSerial == resourceSerial
				&& residency.staticVertexCount == vertexCount
				&& residency.staticBaseVertex <= state.staticVertexCapacity
				&& vertexCount <= state.staticVertexCapacity
					- residency.staticBaseVertex)
			{
				baseVertex = residency.staticBaseVertex;
				staticResident = true;
				return true;
			}
			if (state.vertexBuffer
				&& residency.dynamicResourceSerial == resourceSerial
				&& residency.dynamicUploadEpoch == uploadEpoch
				&& residency.dynamicVertexCount == vertexCount
				&& residency.dynamicBaseVertex <= state.vertexCapacity
				&& vertexCount <= state.vertexCapacity
					- residency.dynamicBaseVertex)
			{
				baseVertex = residency.dynamicBaseVertex;
				staticResident = false;
				return true;
			}
			return false;
		}

		bool PublishSortedRingLeaseLocked(NativeFontRingState& state,
			const std::vector<NativeFontPayloadTemplatePtr>& payloadTemplates,
			UInt32 generation, bool residencyAlreadyValidated)
		{
			if (SortedRingLease().active || !generation
				|| state.generation != generation || !state.vertexBuffer
				|| !state.indexBuffer || !state.declaration
				|| state.releasePending.load(std::memory_order_acquire)
				|| state.sortedFrameLeases.load(std::memory_order_acquire)
				|| state.activeSubmissions.load(std::memory_order_acquire))
			{
				return false;
			}
			const UInt32 resourceSerial = state.resourceSerial.load(
				std::memory_order_acquire);
			const UInt32 uploadEpoch = state.uploadEpoch;
			if (!residencyAlreadyValidated)
			{
				for (const NativeFontPayloadTemplatePtr& payloadTemplate
					: payloadTemplates)
				{
					if (!payloadTemplate || payloadTemplate->gpuVertices.empty()
						|| payloadTemplate->gpuVertices.size()
							> std::numeric_limits<UInt32>::max())
					{
						return false;
					}
					const UInt32 vertexCount = static_cast<UInt32>(
						payloadTemplate->gpuVertices.size());
					UInt32 baseVertex = 0;
					bool staticResident = false;
					if (!ResolveSortedLeaseResidency(state, *payloadTemplate,
						vertexCount, resourceSerial, uploadEpoch,
						baseVertex, staticResident))
					{
						return false;
					}
				}
			}
			RefreshRingCpuMemoryLocked(state);
			state.sortedFrameLeases.fetch_add(1,
				std::memory_order_release);
			SortedRingLease().state = &state;
			SortedRingLease().dynamicVertexBuffer = state.vertexBuffer;
			SortedRingLease().staticVertexBuffer = state.staticVertexBuffer;
			SortedRingLease().indexBuffer = state.indexBuffer;
			SortedRingLease().declaration = state.declaration;
			SortedRingLease().generation = generation;
			SortedRingLease().resourceSerial = resourceSerial;
			SortedRingLease().uploadEpoch = uploadEpoch;
			SortedRingLease().active = true;
			return true;
		}
	}
}

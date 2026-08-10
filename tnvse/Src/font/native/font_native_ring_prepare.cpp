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
		bool TryBeginSortedRingSubmission(NiTriShape* facade,
			NativeFontShapePayload& payload,
			NativeFontRingSubmission& submission,
			NativeFontFallbackReason& result)
		{
			if (!SortedRingLease().active)
				return false;
			result = NativeFontFallbackReason::RuntimeFault;
			NativeFontSortedRingLease& lease = SortedRingLease();
			NativeFontRingState* state = lease.state;
			if (!state || !facade || !payload.payloadTemplate
				|| payload.preparedGeneration != lease.generation
				|| !IsNativeFontShaderGenerationCurrent(lease.generation)
				|| state->generation != lease.generation
				|| state->resourceSerial.load(std::memory_order_acquire)
					!= lease.resourceSerial
				|| state->uploadEpoch != lease.uploadEpoch
				|| state->vertexBuffer != lease.dynamicVertexBuffer
				|| state->staticVertexBuffer != lease.staticVertexBuffer
				|| state->indexBuffer != lease.indexBuffer
				|| state->declaration != lease.declaration)
			{
				return true;
			}
			const UInt64 vertexCount64 =
				payload.payloadTemplate->gpuVertices.size();
			if (!vertexCount64
				|| vertexCount64 > std::numeric_limits<UInt32>::max())
			{
				result = NativeFontFallbackReason::PacketBuild;
				return true;
			}
			const UInt32 vertexCount = static_cast<UInt32>(vertexCount64);
			UInt32 baseVertex = 0;
			bool staticResident = false;
			if (!ResolveSortedLeaseResidency(*state,
				*payload.payloadTemplate, vertexCount,
				lease.resourceSerial, lease.uploadEpoch,
				baseVertex, staticResident))
			{
				result = NativeFontFallbackReason::PacketPrepare;
				return true;
			}

			const UInt32 proxyIndex =
				AcquireProxyLocked(*state, RingThread());
			if (proxyIndex == std::numeric_limits<UInt32>::max())
			{
				payload.packetPrepareFailure.store(
					NativeFontPacketPrepareFailure::ProxyUnavailable,
					std::memory_order_relaxed);
				result = NativeFontFallbackReason::PacketPrepare;
				return true;
			}
			NativeFontProxy& proxy = state->proxies[proxyIndex];
			if (!proxy.shape || !proxy.buffer || !proxy.chip
				|| !SyncProxyState(*facade, proxy,
					payload.geometryOrigin))
			{
				proxy.inUse = false;
				payload.packetPrepareFailure.store(
					NativeFontPacketPrepareFailure::Geometry,
					std::memory_order_relaxed);
				result = NativeFontFallbackReason::PropertySync;
				return true;
			}

			submission.proxyShape = proxy.shape.m_pObject;
			submission.proxyBuffer = proxy.buffer;
			submission.proxyChip = proxy.chip;
			submission.vertexBuffer = staticResident
				? state->staticVertexBuffer : state->vertexBuffer;
			submission.proxyIndex = proxyIndex;
			submission.generation = lease.generation;
			submission.resourceSerial = lease.resourceSerial;
			submission.nextPacket = 0;
			submission.payloadBaseVertex = baseVertex;
			submission.endVertex = baseVertex + vertexCount;
			submission.staticResident = staticResident;
			submission.active = true;
			state->activeSubmissions.fetch_add(1, std::memory_order_release);
			payload.packetPrepareFailure.store(
				NativeFontPacketPrepareFailure::None,
				std::memory_order_relaxed);
			result = NativeFontFallbackReason::None;
			return true;
		}
	}

	bool EnsureNativeFontProxyPool()
	{
		NativeFontRingState& state = RingState();
		if (state.proxyPoolReady.load(std::memory_order_acquire))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::NativeRegistrationProxyFast);
			return true;
		}
		RecordFreeTypePerf(
			FreeTypePerfCounter::NativeRegistrationProxySlow);
		std::lock_guard<std::mutex> lock(state.mutex);
		if (state.proxyPoolReady.load(std::memory_order_relaxed))
			return true;
		const NiColorA white{ 1.0f, 1.0f, 1.0f, 1.0f };
		while (state.proxyCount < kProxyPoolSize)
		{
			NativeFontProxy proxy;
			proxy.shape = CreateFreeTypePlaceholderTextShape(
				1, white, false);
			NiTriShapeData* data = proxy.shape
				? proxy.shape->GetModelData() : nullptr;
			if (!data || !GetTileProperty(proxy.shape.m_pObject)
				|| !InstallProxyVertexColors(*data) || !AttachProxyBuffer(proxy))
			{
				break;
			}
			proxy.shape->UpdateProperties();
			proxy.alphaProperty = proxy.shape->GetAlphaProperty();
			proxy.tile = GetTileProperty(proxy.shape.m_pObject);
			proxy.atlasProperty = proxy.shape->GetTexturingProperty();
			proxy.atlasTexture = proxy.tile
				? proxy.tile->sourceTexture.m_pObject : nullptr;
			proxy.shader = proxy.shape->GetShader();
			if (!proxy.alphaProperty || !proxy.tile || !proxy.atlasProperty
				|| !proxy.atlasTexture)
				break;
			proxy.alphaProperty->SetAlphaTesting(false);
			state.proxies[state.proxyCount++] = std::move(proxy);
		}
		if (state.proxyCount == kProxyPoolSize)
			state.proxyPoolReady.store(true, std::memory_order_release);
		return state.proxyCount != 0;
	}

	void TrimNativeFontCpuCachesForTotalBudget()
	{
		NativeFontRingState& state = RingState();
		std::lock_guard<std::mutex> lock(state.mutex);
		state.uploadedPayloads.clear();
		state.staticCandidates.clear();
		for (auto current = state.staticPayloads.begin();
			current != state.staticPayloads.end();)
		{
			if (current->second.owner.expired())
				current = state.staticPayloads.erase(current);
			else
				++current;
		}
		RingThread().uploadedPayload = {};
		RingThread().staticCandidate = {};
		RefreshRingCpuMemoryLocked(state);
	}

	void PrepareSortedNativeFontPayloads(
		std::vector<NativeFontPayloadTemplatePtr>& payloadTemplates,
		UInt32 generation)
	{
		EndNativeFontSortedRingFrame();
		if (!generation || payloadTemplates.empty()
			|| !IsNativeFontShaderGenerationCurrent(generation))
		{
			return;
		}

		const SInt64 inputScanStart = BeginFreeTypePerfSample();
		UInt32 maximumVertices = 0;
		for (const NativeFontPayloadTemplatePtr& payloadTemplate : payloadTemplates)
		{
			if (!payloadTemplate || payloadTemplate->gpuVertices.empty()
				|| payloadTemplate->gpuVertices.size()
					> std::numeric_limits<UInt32>::max())
			{
				continue;
			}
			const UInt32 vertexCount = static_cast<UInt32>(
				payloadTemplate->gpuVertices.size());
			if ((vertexCount & 3u) || vertexCount / 4u > kNativeFontMaximumQuads)
				continue;
			maximumVertices = std::max(maximumVertices, vertexCount);
		}
		EndFreeTypePerfSample(
			FreeTypePerfPhase::FramePrepRingInputScan, inputScanStart);
		if (!maximumVertices)
			return;

		const SInt64 resourceStart = BeginFreeTypePerfSample();
		NativeFontRingState& state = RingState();
		std::lock_guard<std::mutex> lock(state.mutex);
		const char* operation = "sorted-frame-resource";
		HRESULT result = D3DERR_DEVICELOST;
		const bool resourcesReady = EnsureRingResourcesLocked(
			state, generation, maximumVertices, operation, result);
		EndFreeTypePerfSample(
			FreeTypePerfPhase::FramePrepRingResource, resourceStart);
		if (!resourcesReady)
		{
			return;
		}
		const auto publishLease = [&](bool allStatic = false)
		{
			FreeTypePerfScope publishPerf(
				FreeTypePerfPhase::FramePrepRingLeasePublish);
			return PublishSortedRingLeaseLocked(
				state, payloadTemplates, generation, allStatic);
		};
		auto isValidPayload = [](
			const NativeFontPayloadTemplatePtr& payloadTemplate)
		{
			return payloadTemplate
				&& !payloadTemplate->gpuVertices.empty()
				&& payloadTemplate->gpuVertices.size()
					<= std::numeric_limits<UInt32>::max()
				&& !(payloadTemplate->gpuVertices.size() & 3u)
				&& payloadTemplate->gpuVertices.size() / 4u
					<= kNativeFontMaximumQuads;
		};
		size_t validatedStaticPayloads = 0;
		size_t residentStaticPayloads = 0;
		const UInt32 staticScanResourceSerial = state.resourceSerial.load(
			std::memory_order_relaxed);
		if (state.staticVertexBuffer)
		{
			const SInt64 staticScanStart = BeginFreeTypePerfSample();
			std::vector<NativeFontPayloadTemplatePtr> selected;
			selected.reserve(std::min<size_t>(payloadTemplates.size(),
				kStaticPromotionPayloadLimit));
			CpuMemoryLease selectedCpuMemory;
			selectedCpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
				selected.capacity() * sizeof(NativeFontPayloadTemplatePtr));
			UInt32 requestedVertices = 0;
			UInt32 requestedBytes = 0;
			UInt32 requestedPayloads = 0;
			UInt64 lifecycleDeferred = 0;
			UInt64 uploadDeferred = 0;
			UInt64 retryDeferred = 0;
			UInt64 budgetDeferred = 0;
			const UInt32 frame = GetStaticObservationFrame(state);
			for (const NativeFontPayloadTemplatePtr& payloadTemplate
				: payloadTemplates)
			{
				if (!isValidPayload(payloadTemplate))
					continue;
				++validatedStaticPayloads;
				const UInt32 vertexCount = static_cast<UInt32>(
					payloadTemplate->gpuVertices.size());
				UInt32 baseVertex = 0;
				if (ResolveStaticPayloadLocked(state, payloadTemplate,
					vertexCount, baseVertex))
				{
					++residentStaticPayloads;
					continue;
				}
				NativeFontStaticCandidate* candidate =
					ObserveStaticCandidateLocked(state, payloadTemplate,
						vertexCount, false);
				if (!candidate)
					continue;
				const StaticPromotionReadiness readiness =
					GetStaticPromotionReadiness(state, *candidate,
						vertexCount, frame);
				if (readiness != StaticPromotionReadiness::Ready)
				{
					switch (readiness)
					{
					case StaticPromotionReadiness::Lifecycle:
						++lifecycleDeferred;
						break;
					case StaticPromotionReadiness::UploadHistory:
						++uploadDeferred;
						break;
					case StaticPromotionReadiness::Retry:
						++retryDeferred;
						break;
					default:
						break;
					}
					continue;
				}
				if (!FitsStaticPromotionBudget(state, *candidate,
					vertexCount, requestedBytes, requestedPayloads))
				{
					++budgetDeferred;
					continue;
				}
				requestedVertices += vertexCount;
				requestedBytes += vertexCount * sizeof(NativeFontGpuVertex);
				++requestedPayloads;
				selected.push_back(payloadTemplate);
			}
			RecordStaticPromotionDeferral(
				StaticPromotionReadiness::Lifecycle, lifecycleDeferred);
			RecordStaticPromotionDeferral(
				StaticPromotionReadiness::UploadHistory, uploadDeferred);
			RecordStaticPromotionDeferral(
				StaticPromotionReadiness::Retry, retryDeferred);
			if (budgetDeferred)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::StaticPromotionDeferredBudget,
					budgetDeferred);
			}

			const UInt32 availableVertices = state.nextStaticVertex
				<= state.staticVertexCapacity
				? state.staticVertexCapacity - state.nextStaticVertex : 0;
			if (requestedVertices > availableVertices
				&& requestedVertices)
			{
				bool permanentFailure = false;
				if (!TryGrowStaticVertexBufferLocked(state,
					requestedVertices, permanentFailure)
					&& !permanentFailure)
				{
					DeferStaticPromotionsLocked(state, frame);
				}
			}

			const UInt32 staticAvailable = state.nextStaticVertex
				<= state.staticVertexCapacity
				? state.staticVertexCapacity - state.nextStaticVertex : 0;
			auto deferSelectedCandidate = [&](
				const NativeFontPayloadTemplatePtr& payloadTemplate)
			{
				auto found = state.staticCandidates.find(payloadTemplate.get());
				if (found != state.staticCandidates.end() && found->second)
				{
					found->second->nextRetryFrame = frame
						+ kStaticPromotionRetryFrames;
				}
			};
			UInt32 selectedVertices = 0;
			size_t selectedPayloads = 0;
			for (size_t index = 0; index < selected.size(); ++index)
			{
				const NativeFontPayloadTemplatePtr& payloadTemplate = selected[index];
				const UInt32 vertexCount = static_cast<UInt32>(
					payloadTemplate->gpuVertices.size());
				if (vertexCount > staticAvailable - selectedVertices)
				{
					deferSelectedCandidate(payloadTemplate);
					continue;
				}
				selected[selectedPayloads] = payloadTemplate;
				selectedVertices += vertexCount;
				++selectedPayloads;
			}
			selected.resize(selectedPayloads);
			EndFreeTypePerfSample(
				FreeTypePerfPhase::FramePrepRingStaticScan,
				staticScanStart);

			if (selectedPayloads && selectedVertices)
			{
				state.staticPayloads.reserve(
					state.staticPayloads.size() + selectedPayloads);
				const UINT byteOffset = state.nextStaticVertex
					* sizeof(NativeFontGpuVertex);
				const UINT byteCount =
					selectedVertices * sizeof(NativeFontGpuVertex);
				void* destination = nullptr;
				{
					FreeTypePerfScope staticLockPerf(
						FreeTypePerfPhase::FramePrepRingStaticLock);
					result = state.staticVertexBuffer->Lock(
						byteOffset, byteCount, &destination, 0);
				}
				if (SUCCEEDED(result) && destination)
				{
					UInt32 copiedVertices = 0;
					{
						FreeTypePerfScope staticCopyPerf(
							FreeTypePerfPhase::FramePrepRingStaticCopy);
						for (const NativeFontPayloadTemplatePtr& payloadTemplate
							: selected)
						{
							const UInt32 vertexCount = static_cast<UInt32>(
								payloadTemplate->gpuVertices.size());
							std::memcpy(static_cast<UInt8*>(destination)
									+ copiedVertices
										* sizeof(NativeFontGpuVertex),
								payloadTemplate->gpuVertices.data(),
								vertexCount * sizeof(NativeFontGpuVertex));
							copiedVertices += vertexCount;
						}
					}
					{
						FreeTypePerfScope staticUnlockPerf(
							FreeTypePerfPhase::FramePrepRingStaticUnlock);
						result = state.staticVertexBuffer->Unlock();
					}
					{
						FreeTypePerfScope staticCommitPerf(
							FreeTypePerfPhase::FramePrepRingStaticCommit);
						if (copiedVertices == selectedVertices
							&& SUCCEEDED(result))
						{
							AdvanceDiagnosticSerial(state.staticWriteSerial);
							const UInt32 resourceSerial =
								state.resourceSerial.load(
									std::memory_order_relaxed);
							UInt32 mappedVertices = 0;
							for (const NativeFontPayloadTemplatePtr& payloadTemplate
								: selected)
							{
								const UInt32 vertexCount = static_cast<UInt32>(
									payloadTemplate->gpuVertices.size());
								const UInt32 baseVertex =
									state.nextStaticVertex + mappedVertices;
								state.staticPayloads[payloadTemplate.get()] = {
									payloadTemplate, baseVertex, vertexCount,
									state.staticWriteSerial,
									HashDiagnosticPayload(*payloadTemplate) };
								NativeFontPayloadResidencyCache& residency =
									payloadTemplate->residency;
								residency.staticResourceSerial = resourceSerial;
								residency.staticBaseVertex = baseVertex;
								residency.staticVertexCount = vertexCount;
								residency.staticLastUsedFrame = frame;
								state.staticCandidates.erase(payloadTemplate.get());
								if (RingThread().staticCandidate.key
									== payloadTemplate.get())
								{
									RingThread().staticCandidate = {};
								}
								mappedVertices += vertexCount;
							}
							state.nextStaticVertex += selectedVertices;
							residentStaticPayloads += selectedPayloads;
							CommitStaticPromotionBudget(state, byteCount,
								static_cast<UInt32>(selectedPayloads));
							RecordFreeTypePerf(
								FreeTypePerfCounter::StaticVertexUpload);
							RecordFreeTypePerf(
								FreeTypePerfCounter::StaticVertexUploadBytes,
								byteCount);
							RecordFreeTypePerf(
								FreeTypePerfCounter::SortedStaticBatch);
							RecordFreeTypePerf(
								FreeTypePerfCounter::SortedStaticPayload,
								static_cast<UInt64>(selectedPayloads));
							RecordFreeTypePerf(
								FreeTypePerfCounter::SortedStaticBytes,
								byteCount);
							if (g_bEnableFreeTypeFontRenderingLog)
							{
								FreeTypeFontDebugLog(
									"tnvse_freetype_native: sorted static batch generation=%u payloads=%u vertices=%u bytes=%u residentVertices=%u capacity=%u",
									generation,
									static_cast<UInt32>(selectedPayloads),
									selectedVertices, byteCount,
									state.nextStaticVertex,
									state.staticVertexCapacity);
							}
						}
						else
						{
							// The interval was written but was not published. Reserve it
							// so no later append can overwrite geometry that might have
							// been partially accepted by the driver.
							state.nextStaticVertex += selectedVertices;
							CommitStaticPromotionBudget(state, byteCount,
								static_cast<UInt32>(selectedPayloads));
							for (const NativeFontPayloadTemplatePtr& payloadTemplate
								: selected)
							{
								deferSelectedCandidate(payloadTemplate);
							}
							DeferStaticPromotionsLocked(state, frame);
							RecordFreeTypePerf(
								FreeTypePerfCounter::StaticVertexPromotionFailed);
						}
					}
				}
				else
				{
					FreeTypePerfScope staticCommitPerf(
						FreeTypePerfPhase::FramePrepRingStaticCommit);
					for (const NativeFontPayloadTemplatePtr& payloadTemplate
						: selected)
					{
						deferSelectedCandidate(payloadTemplate);
					}
					DeferStaticPromotionsLocked(state, frame);
					RecordFreeTypePerf(
						FreeTypePerfCounter::StaticVertexPromotionFailed);
				}
			}
		}
		if (validatedStaticPayloads == payloadTemplates.size()
			&& residentStaticPayloads == validatedStaticPayloads
			&& state.resourceSerial.load(std::memory_order_relaxed)
				== staticScanResourceSerial
			&& publishLease(true))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::SortedAllStaticFastExit);
			RecordFreeTypePerf(FreeTypePerfCounter::
				SortedAllStaticPayloadValidationElided,
				static_cast<UInt64>(payloadTemplates.size()));
			return;
		}

		const SInt64 dynamicResolveStart = BeginFreeTypePerfSample();
		RefreshRingCpuMemoryLocked(state);

		// Resolve existing locations first. If an append cannot fit, rebatch every
		// currently visible non-static payload under one DISCARD so no earlier
		// location survives an epoch change and no shape performs its own lock.
		UInt64 allDynamicVertices = 0;
		UInt64 missingDynamicVertices = 0;
		size_t allDynamicPayloads = 0;
		size_t missingDynamicPayloads = 0;
		for (const NativeFontPayloadTemplatePtr& payloadTemplate : payloadTemplates)
		{
			if (!isValidPayload(payloadTemplate))
				continue;
			const UInt32 vertexCount = static_cast<UInt32>(
				payloadTemplate->gpuVertices.size());
			UInt32 baseVertex = 0;
			const bool staticResident = IsStaticPayloadCurrentLocked(
				state, payloadTemplate, vertexCount);
			if (staticResident)
			{
				continue;
			}
			allDynamicVertices += vertexCount;
			++allDynamicPayloads;
			if (!ResolveUploadedPayloadLocked(state, payloadTemplate,
				vertexCount, baseVertex))
			{
				missingDynamicVertices += vertexCount;
				++missingDynamicPayloads;
			}
		}
		if (!missingDynamicVertices)
		{
			EndFreeTypePerfSample(
				FreeTypePerfPhase::FramePrepRingDynamicResolve,
				dynamicResolveStart);
			publishLease();
			return;
		}

		const UInt64 appendCapacity = state.nextVertex <= state.vertexCapacity
			? state.vertexCapacity - state.nextVertex : 0;
		const bool discard = !state.nextVertex
			|| missingDynamicVertices > appendCapacity;
		const UInt64 uploadVertices64 = discard
			? allDynamicVertices : missingDynamicVertices;
		const size_t uploadPayloads = discard
			? allDynamicPayloads : missingDynamicPayloads;
		if (!uploadVertices64 || uploadVertices64 > state.vertexCapacity
			|| uploadVertices64 > std::numeric_limits<UInt32>::max())
		{
			EndFreeTypePerfSample(
				FreeTypePerfPhase::FramePrepRingDynamicResolve,
				dynamicResolveStart);
			return;
		}
		if (discard
			&& state.activeSubmissions.load(std::memory_order_acquire))
		{
			EndFreeTypePerfSample(
				FreeTypePerfPhase::FramePrepRingDynamicResolve,
				dynamicResolveStart);
			return;
		}

		UInt32 startVertex = state.nextVertex;
		DWORD lockFlags = D3DLOCK_NOOVERWRITE;
		if (discard)
		{
			startVertex = 0;
			lockFlags = D3DLOCK_DISCARD;
			AdvanceUploadEpochLocked(state);
			AdvanceDiagnosticSerial(state.dynamicDiscardSerial);
			state.uploadedPayloads.clear();
			RingThread().uploadedPayload = {};
			state.nextVertex = 0;
			RecordFreeTypePerf(FreeTypePerfCounter::DynamicVertexDiscard);
		}

		const UInt32 uploadVertices = static_cast<UInt32>(uploadVertices64);
		state.uploadedPayloads.reserve(
			state.uploadedPayloads.size() + uploadPayloads);
		const UINT byteOffset =
			startVertex * sizeof(NativeFontGpuVertex);
		const UINT byteCount =
			uploadVertices * sizeof(NativeFontGpuVertex);
		EndFreeTypePerfSample(
			FreeTypePerfPhase::FramePrepRingDynamicResolve,
			dynamicResolveStart);
		void* destination = nullptr;
		{
			FreeTypePerfScope dynamicLockPerf(
				FreeTypePerfPhase::FramePrepRingDynamicLock);
			result = state.vertexBuffer->Lock(
				byteOffset, byteCount, &destination, lockFlags);
		}
		if (FAILED(result) || !destination)
			return;

		UInt32 copiedVertices = 0;
		{
			FreeTypePerfScope dynamicCopyPerf(
				FreeTypePerfPhase::FramePrepRingDynamicCopy);
			for (const NativeFontPayloadTemplatePtr& payloadTemplate
				: payloadTemplates)
			{
				if (!isValidPayload(payloadTemplate))
					continue;
				const UInt32 vertexCount = static_cast<UInt32>(
					payloadTemplate->gpuVertices.size());
				if (HasDirectStaticPayloadLocked(state, *payloadTemplate,
						vertexCount)
					|| (!discard && HasDirectUploadedPayloadLocked(state,
						*payloadTemplate, vertexCount)))
				{
					continue;
				}
				std::memcpy(static_cast<UInt8*>(destination)
						+ copiedVertices * sizeof(NativeFontGpuVertex),
					payloadTemplate->gpuVertices.data(),
					vertexCount * sizeof(NativeFontGpuVertex));
				copiedVertices += vertexCount;
			}
		}
		{
			FreeTypePerfScope dynamicUnlockPerf(
				FreeTypePerfPhase::FramePrepRingDynamicUnlock);
			result = state.vertexBuffer->Unlock();
		}
		if (copiedVertices != uploadVertices || FAILED(result))
			return;
		AdvanceDiagnosticSerial(state.dynamicWriteSerial);

		UInt32 mappedVertices = 0;
		{
			FreeTypePerfScope dynamicCommitPerf(
				FreeTypePerfPhase::FramePrepRingDynamicCommit);
			for (const NativeFontPayloadTemplatePtr& payloadTemplate
				: payloadTemplates)
			{
				if (!isValidPayload(payloadTemplate))
					continue;
				const UInt32 vertexCount = static_cast<UInt32>(
					payloadTemplate->gpuVertices.size());
				if (HasDirectStaticPayloadLocked(state, *payloadTemplate,
						vertexCount)
					|| (!discard && HasDirectUploadedPayloadLocked(state,
						*payloadTemplate, vertexCount)))
				{
					continue;
				}
				PublishUploadedPayloadLocked(state, payloadTemplate,
					startVertex + mappedVertices, vertexCount);
				mappedVertices += vertexCount;
			}

			// Even a partially published written interval must remain reserved so
			// the per-shape fallback cannot overwrite data accepted by the driver.
			state.nextVertex = startVertex + uploadVertices;
			RefreshRingCpuMemoryLocked(state);
			if (mappedVertices == uploadVertices)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::DynamicVertexUpload);
				RecordFreeTypePerf(
					FreeTypePerfCounter::DynamicVertexUploadBytes, byteCount);
				RecordFreeTypePerf(FreeTypePerfCounter::SortedDynamicBatch);
				RecordFreeTypePerf(FreeTypePerfCounter::SortedDynamicPayload,
					static_cast<UInt64>(uploadPayloads));
				RecordFreeTypePerf(
					FreeTypePerfCounter::SortedDynamicBytes, byteCount);
			}
		}
		publishLease();
	}
}

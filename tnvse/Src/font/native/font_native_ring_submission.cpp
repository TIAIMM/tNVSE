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

	void EndNativeFontDirectShapeSubmission(
		NativeFontDirectShapeSubmission& submission)
	{
		if (submission.active)
		{
			NativeFontRingState& state = RingState();
			if (SortedRingLease().active
				&& SortedRingLease().state == &state)
			{
				state.activeSubmissions.fetch_sub(
					1, std::memory_order_acq_rel);
			}
			else
			{
				std::lock_guard<std::mutex> lock(state.mutex);
				state.activeSubmissions.fetch_sub(
					1, std::memory_order_acq_rel);
				if (!state.activeSubmissions.load(std::memory_order_acquire)
					&& state.releasePending.load(
						std::memory_order_acquire))
				{
					ReleaseRingResourcesLocked(state);
				}
			}
		}
		submission = NativeFontDirectShapeSubmission{};
	}

	NativeFontFallbackReason BeginNativeFontDirectShapeSubmission(
		NiTriShape* facade, NativeFontShapePayload& payload,
		NativeFontDirectShapeSubmission& submission)
	{
		EndNativeFontDirectShapeSubmission(submission);
		if (!facade || !payload.buildComplete || !payload.payloadTemplate
			|| payload.packetShaders.size() != 1)
		{
			return NativeFontFallbackReason::PacketBuild;
		}

		const NativeFontPayloadTemplate& artifact =
			*payload.payloadTemplate;
		const std::vector<NativeFontPacketTemplate>& packets =
			GetNativeFontPackets(artifact, payload.useCompositePackets);
		if (artifact.pageCount != 1 || artifact.atlasProperties.size() != 1
			|| artifact.atlasTextures.size() != 1 || packets.size() != 1
			|| !payload.packetShaders[0])
		{
			return NativeFontFallbackReason::PacketBuild;
		}
		const NativeFontPacketTemplate& packet = packets[0];
		if (packet.atlasPage != 0 || packet.firstVertex != 0
			|| !packet.vertexCount
			|| packet.vertexCount != artifact.gpuVertices.size()
			|| (packet.vertexCount & 3u)
			|| packet.vertexCount / 4u > kNativeFontMaximumQuads)
		{
			return NativeFontFallbackReason::PacketBuild;
		}

		// The direct shape already owns the first physical atlas property from
		// construction. Requiring exact wrapper and source-texture identity keeps
		// this path mutation-free; page/property changes fall back to the proxy
		// route, which retains its complete synchronization contract.
		const TileShaderPropertyView* tile = GetTileProperty(facade);
		if (!tile || facade->GetTexturingProperty()
				!= artifact.atlasProperties[0].m_pObject
			|| tile->sourceTexture.m_pObject
				!= artifact.atlasTextures[0].m_pObject)
		{
			return NativeFontFallbackReason::PropertySync;
		}

		if (!SortedRingLease().active)
			return NativeFontFallbackReason::PacketPrepare;
		NativeFontSortedRingLease& lease = SortedRingLease();
		NativeFontRingState* state = lease.state;
		if (!state || payload.preparedGeneration != lease.generation
			|| !IsNativeFontShaderGenerationCurrent(lease.generation)
			|| state->generation != lease.generation
			|| state->resourceSerial.load(std::memory_order_acquire)
				!= lease.resourceSerial
			|| state->uploadEpoch != lease.uploadEpoch
			|| state->vertexBuffer != lease.dynamicVertexBuffer
			|| state->staticVertexBuffer != lease.staticVertexBuffer
			|| state->indexBuffer != lease.indexBuffer
			|| state->declaration != lease.declaration
			|| !lease.indexBuffer || !lease.declaration)
		{
			return NativeFontFallbackReason::PacketPrepare;
		}

		UInt32 baseVertex = 0;
		bool staticResident = false;
		if (!ResolveSortedLeaseResidency(*state, artifact,
			packet.vertexCount, lease.resourceSerial, lease.uploadEpoch,
			baseVertex, staticResident))
		{
			return NativeFontFallbackReason::PacketPrepare;
		}
		IDirect3DVertexBuffer9* vertexBuffer = staticResident
			? lease.staticVertexBuffer : lease.dynamicVertexBuffer;
		if (!vertexBuffer)
			return NativeFontFallbackReason::PacketPrepare;

		submission.vertexBuffer = vertexBuffer;
		submission.indexBuffer = lease.indexBuffer;
		submission.declaration = lease.declaration;
		submission.baseVertex = baseVertex;
		submission.vertexCount = packet.vertexCount;
		submission.indexBytes = kCanonicalIndexBytes;
		submission.generation = lease.generation;
		submission.resourceSerial = lease.resourceSerial;
		submission.staticResident = staticResident;
		submission.active = true;
		state->activeSubmissions.fetch_add(1, std::memory_order_release);
		payload.packetPrepareFailure.store(
			NativeFontPacketPrepareFailure::None, std::memory_order_relaxed);
		return NativeFontFallbackReason::None;
	}

	NativeFontFallbackReason ResolveNativeFontDirectFacadePacketBinding(
		NativeFontShapePayload& payload, UInt32 packetIndex,
		NativeFontDirectFacadePacketBinding& binding)
	{
		binding = {};
		if (!payload.buildComplete || !payload.payloadTemplate
			|| payload.preparedGeneration == 0)
		{
			return NativeFontFallbackReason::PacketBuild;
		}
		const NativeFontPayloadTemplate& artifact =
			*payload.payloadTemplate;
		const std::vector<NativeFontPacketTemplate>& packets =
			GetNativeFontPackets(artifact, payload.useCompositePackets);
		if (packetIndex >= packets.size()
			|| packetIndex >= payload.packetShaders.size()
			|| !payload.packetShaders[packetIndex]
			|| artifact.gpuVertices.empty()
			|| artifact.gpuVertices.size()
				> std::numeric_limits<UInt32>::max())
		{
			return NativeFontFallbackReason::PacketBuild;
		}
		const NativeFontPacketTemplate& packet = packets[packetIndex];
		const UInt32 artifactVertexCount = static_cast<UInt32>(
			artifact.gpuVertices.size());
		const UInt64 vertexEnd = static_cast<UInt64>(packet.firstVertex)
			+ packet.vertexCount;
		if (!packet.vertexCount || (packet.firstVertex & 3u)
			|| (packet.vertexCount & 3u)
			|| packet.vertexCount / 4u > kNativeFontMaximumQuads
			|| vertexEnd > artifactVertexCount)
		{
			return NativeFontFallbackReason::PacketBuild;
		}

		if (!IsNativeFontShaderGenerationCurrent(
				payload.preparedGeneration))
		{
			return NativeFontFallbackReason::PacketPrepare;
		}
		NativeFontFramePacketBinding frameBinding;
		if (!ResolveNativeFontFramePacketBinding(
				payload, packetIndex, frameBinding))
		{
			return NativeFontFallbackReason::PacketPrepare;
		}

		binding.vertexBuffer = frameBinding.vertexBuffer;
		binding.indexBuffer = frameBinding.indexBuffer;
		binding.declaration = frameBinding.declaration;
		binding.baseVertex = frameBinding.baseVertex;
		binding.vertexCount = frameBinding.vertexCount;
		binding.indexBytes = frameBinding.indexBytes;
		binding.generation = frameBinding.generation;
		binding.resourceSerial = frameBinding.resourceSerial;
		binding.uploadEpoch = frameBinding.uploadEpoch;
		binding.atlasTextureEpoch = payload.preflightAtlasTextureEpoch;
		binding.staticResident = frameBinding.staticResident;
		binding.active = true;
		return NativeFontFallbackReason::None;
	}

	bool IsNativeFontDirectFacadePacketBindingCurrent(
		const NativeFontDirectFacadePacketBinding& binding)
	{
		if (!binding.active || !SortedRingLease().active)
			return false;
		NativeFontFramePacketBinding frameBinding;
		frameBinding.vertexBuffer = binding.vertexBuffer;
		frameBinding.indexBuffer = binding.indexBuffer;
		frameBinding.declaration = binding.declaration;
		frameBinding.baseVertex = binding.baseVertex;
		frameBinding.vertexCount = binding.vertexCount;
		frameBinding.indexBytes = binding.indexBytes;
		frameBinding.generation = binding.generation;
		frameBinding.resourceSerial = binding.resourceSerial;
		frameBinding.uploadEpoch = binding.uploadEpoch;
		frameBinding.staticResident = binding.staticResident;
		frameBinding.active = binding.active;
		return binding.atlasTextureEpoch
				== GetNativeFontAtlasTextureEpoch()
			&& IsNativeFontFramePacketBindingCurrent(frameBinding);
	}

	bool IsNativeFontDirectFacadePacketAtlasCurrent(
		const NiTriShape* shape, const NativeFontShapePayload& payload,
		UInt32 packetIndex)
	{
		if (!shape || !payload.buildComplete
			|| !payload.payloadTemplate)
		{
			return false;
		}
		const NativeFontPayloadTemplate& artifact =
			*payload.payloadTemplate;
		const std::vector<NativeFontPacketTemplate>& packets =
			GetNativeFontPackets(artifact, payload.useCompositePackets);
		if (packetIndex >= packets.size())
			return false;
		const UInt16 page = packets[packetIndex].atlasPage;
		if (page >= artifact.atlasProperties.size()
			|| page >= artifact.atlasTextures.size()
			|| !artifact.atlasProperties[page]
			|| !artifact.atlasTextures[page])
		{
			return false;
		}
		const TileShaderPropertyView* tile = GetTileProperty(shape);
		return tile
			&& shape->GetTexturingProperty()
				== artifact.atlasProperties[page].m_pObject
			&& tile->sourceTexture.m_pObject
				== artifact.atlasTextures[page].m_pObject;
	}

	NativeFontFallbackReason BeginNativeFontRingSubmission(
		NiTriShape* facade, NativeFontShapePayload& payload,
		NativeFontRingSubmission& submission)
	{
		EndNativeFontRingSubmission(submission);
		if (!facade || !payload.buildComplete || !payload.payloadTemplate
			|| payload.packetShaders.empty())
		{
			return NativeFontFallbackReason::PacketBuild;
		}
		const std::vector<NativeFontPacketTemplate>& activePackets =
			GetNativeFontPackets(*payload.payloadTemplate,
				payload.useCompositePackets);
		if (payload.packetShaders.size() != activePackets.size())
			return NativeFontFallbackReason::PacketBuild;
		NativeFontFallbackReason leaseResult =
			NativeFontFallbackReason::RuntimeFault;
		if (TryBeginSortedRingSubmission(facade, payload, submission,
			leaseResult))
		{
			if (leaseResult == NativeFontFallbackReason::None)
				return leaseResult;
			// A generation/resource/range mismatch invalidates the complete frame
			// snapshot. Drop its lease, then run the existing locked per-facade
			// path so the affected text still has a correctness-preserving fallback.
			// A recursive draw can still own a proxy from this lease; in that case
			// retaining the lease is mandatory because proxy inUse state is
			// render-thread confined until the final lockless submission ends.
			if (SortedRingLease().state
				&& SortedRingLease().state->activeSubmissions.load(
					std::memory_order_acquire))
			{
				return leaseResult;
			}
			EndNativeFontSortedRingFrame();
		}

		const UInt64 totalVertexCount = payload.payloadTemplate->gpuVertices.size();
		if (!totalVertexCount || totalVertexCount > std::numeric_limits<UInt32>::max())
			return NativeFontFallbackReason::PacketBuild;
		// InitializeNativeFontShapePayload and the generation preflight validate every
		// immutable packet span. Rewalking all spans here made the steady sorted path
		// pay the same O(packet count) validation twice for every facade.
		const UInt32 totalVertices = static_cast<UInt32>(totalVertexCount);

		NativeFontRingState& state = RingState();
		std::lock_guard<std::mutex> lock(state.mutex);
		if (state.sortedFrameLeases.load(std::memory_order_acquire))
		{
			payload.packetPrepareFailure.store(
				NativeFontPacketPrepareFailure::ProxyUnavailable,
				std::memory_order_relaxed);
			return NativeFontFallbackReason::PacketPrepare;
		}
		const UInt32 proxyIndex = AcquireProxyLocked(state, RingThread());
		if (proxyIndex == std::numeric_limits<UInt32>::max())
		{
			payload.packetPrepareFailure.store(
				NativeFontPacketPrepareFailure::ProxyUnavailable,
				std::memory_order_relaxed);
			return NativeFontFallbackReason::PacketPrepare;
		}

		const char* operation = "ring-resource";
		HRESULT result = D3DERR_DEVICELOST;
		if (!EnsureRingResourcesLocked(state, payload.preparedGeneration,
			totalVertices,
			operation, result))
		{
			state.proxies[proxyIndex].inUse = false;
			NativeFontPacketPrepareFailure prepareFailure =
				NativeFontPacketPrepareFailure::VertexBuffer;
			if (operation && std::strcmp(operation, "ring-context") == 0)
				prepareFailure = NativeFontPacketPrepareFailure::Generation;
			else if (operation && std::strcmp(operation, "ring-declaration") == 0)
				prepareFailure = NativeFontPacketPrepareFailure::Declaration;
			else if (operation && std::strcmp(operation, "ring-capacity") == 0)
				prepareFailure = NativeFontPacketPrepareFailure::RingCapacity;
			else if (operation && std::strcmp(operation, "ring-busy") == 0)
				prepareFailure = NativeFontPacketPrepareFailure::ProxyUnavailable;
			else if (operation && (std::strcmp(operation, "CreateIndexBuffer") == 0
				|| std::strcmp(operation, "canonical-index-upload") == 0))
			{
				prepareFailure = NativeFontPacketPrepareFailure::IndexBuffer;
			}
			payload.packetPrepareFailure.store(prepareFailure,
				std::memory_order_relaxed);
			if (prepareFailure != NativeFontPacketPrepareFailure::RingCapacity
				&& prepareFailure
					!= NativeFontPacketPrepareFailure::ProxyUnavailable)
			{
				MarkNativeFontGenerationFault(payload.preparedGeneration,
					operation, result);
			}
			return NativeFontFallbackReason::PacketPrepare;
		}

		UInt32 startVertex = 0;
		bool staticResident = ResolveStaticPayloadLocked(state,
			payload.payloadTemplate, totalVertices, startVertex);
		if (!staticResident && !State().renderAlphaGeometryHookInstalled)
		{
			staticResident = PromoteStaticPayloadLocked(state,
				payload.payloadTemplate, totalVertices, startVertex);
		}

		if (!staticResident)
		{
			const bool reusedUpload = ResolveUploadedPayloadLocked(state,
				payload.payloadTemplate, totalVertices, startVertex);
			if (!reusedUpload)
			{
				startVertex = state.nextVertex;
				DWORD lockFlags = D3DLOCK_NOOVERWRITE;
				if (!startVertex || startVertex > state.vertexCapacity
					|| totalVertices > state.vertexCapacity - startVertex)
				{
					if (state.activeSubmissions.load(
						std::memory_order_acquire))
					{
						state.proxies[proxyIndex].inUse = false;
						payload.packetPrepareFailure.store(
							NativeFontPacketPrepareFailure::ProxyUnavailable,
							std::memory_order_relaxed);
						return NativeFontFallbackReason::PacketPrepare;
					}
					startVertex = 0;
					lockFlags = D3DLOCK_DISCARD;
					AdvanceUploadEpochLocked(state);
					AdvanceDiagnosticSerial(state.dynamicDiscardSerial);
					state.uploadedPayloads.clear();
					RefreshRingCpuMemoryLocked(state);
					RingThread().uploadedPayload = {};
					RecordFreeTypePerf(
						FreeTypePerfCounter::DynamicVertexDiscard);
				}
				void* destination = nullptr;
				const UINT byteOffset = startVertex
					* sizeof(NativeFontGpuVertex);
				const UINT byteCount = totalVertices
					* sizeof(NativeFontGpuVertex);
				result = state.vertexBuffer->Lock(byteOffset, byteCount,
					&destination, lockFlags);
				if (FAILED(result) || !destination)
				{
					if (SUCCEEDED(result))
						result = E_FAIL;
					state.proxies[proxyIndex].inUse = false;
					payload.packetPrepareFailure.store(
						NativeFontPacketPrepareFailure::VertexBuffer,
						std::memory_order_relaxed);
					MarkNativeFontGenerationFault(payload.preparedGeneration,
						"dynamic-vb-lock", result);
					return NativeFontFallbackReason::PacketPrepare;
				}

				std::memcpy(destination,
					payload.payloadTemplate->gpuVertices.data(), byteCount);
				result = state.vertexBuffer->Unlock();
				if (FAILED(result))
				{
					state.proxies[proxyIndex].inUse = false;
					payload.packetPrepareFailure.store(
						NativeFontPacketPrepareFailure::VertexBuffer,
						std::memory_order_relaxed);
					MarkNativeFontGenerationFault(payload.preparedGeneration,
						"dynamic-vb-unlock", result);
					return NativeFontFallbackReason::PacketPrepare;
				}
				AdvanceDiagnosticSerial(state.dynamicWriteSerial);

				state.nextVertex = startVertex + totalVertices;
				PublishUploadedPayloadLocked(state, payload.payloadTemplate,
					startVertex, totalVertices);
				RefreshRingCpuMemoryLocked(state);
				RecordFreeTypePerf(FreeTypePerfCounter::DynamicVertexUpload);
				RecordFreeTypePerf(
					FreeTypePerfCounter::DynamicVertexUploadBytes, byteCount);
			}

			ObserveStaticCandidateLocked(state, payload.payloadTemplate,
				totalVertices, false);
		}

		NativeFontProxy& proxy = state.proxies[proxyIndex];
		if (!proxy.shape || !proxy.buffer || !proxy.chip
			|| !SyncProxyState(*facade, proxy, payload.geometryOrigin))
		{
			proxy.inUse = false;
			payload.packetPrepareFailure.store(
				NativeFontPacketPrepareFailure::Geometry,
				std::memory_order_relaxed);
			return NativeFontFallbackReason::PropertySync;
		}

		submission.proxyShape = proxy.shape.m_pObject;
		submission.proxyBuffer = proxy.buffer;
		submission.proxyChip = proxy.chip;
		submission.vertexBuffer = staticResident
			? state.staticVertexBuffer : state.vertexBuffer;
		submission.proxyIndex = proxyIndex;
		submission.generation = state.generation;
		submission.resourceSerial = state.resourceSerial.load(
			std::memory_order_acquire);
		submission.nextPacket = 0;
		submission.payloadBaseVertex = startVertex;
		submission.endVertex = startVertex + totalVertices;
		submission.staticResident = staticResident;
		submission.active = true;
		state.activeSubmissions.fetch_add(1, std::memory_order_release);
		payload.packetPrepareFailure.store(
			NativeFontPacketPrepareFailure::None, std::memory_order_relaxed);
		return NativeFontFallbackReason::None;
	}

	NativeFontFallbackReason PrepareNativeFontRingPacket(
		NiTriShape* facade, NativeFontShapePayload& payload,
		NativeFontRingSubmission& submission, UInt32 packetIndex,
		NiTriShape*& proxyShape)
	{
		proxyShape = nullptr;
		if (!submission.active || !facade || !submission.proxyShape
			|| packetIndex != submission.nextPacket
			|| packetIndex >= payload.packetShaders.size()
			|| submission.generation != payload.preparedGeneration
			|| !IsNativeFontShaderGenerationCurrent(submission.generation)
			|| !payload.payloadTemplate)
		{
			return NativeFontFallbackReason::RuntimeFault;
		}

		const std::vector<NativeFontPacketTemplate>& activePackets =
			GetNativeFontPackets(*payload.payloadTemplate,
				payload.useCompositePackets);
		if (packetIndex >= activePackets.size())
			return NativeFontFallbackReason::PacketBuild;
		TileShader* shader = payload.packetShaders[packetIndex];
		const NativeFontPacketTemplate& source =
			activePackets[packetIndex];
		if (!shader || source.atlasPage
			>= payload.payloadTemplate->atlasTextures.size())
		{
			return NativeFontFallbackReason::AtlasGeneration;
		}
		const UInt32 vertexCount = source.vertexCount;
		const UInt64 baseVertex = static_cast<UInt64>(
			submission.payloadBaseVertex) + source.firstVertex;
		if (!vertexCount || (vertexCount & 3u)
			|| vertexCount / 4u > kNativeFontMaximumQuads
			|| baseVertex + vertexCount > submission.endVertex)
		{
			return NativeFontFallbackReason::PacketBuild;
		}

		NativeFontRingState& state = RingState();
		// Begin reserved this proxy under either the ring mutex or the validated
		// sorted-frame lease, then incremented activeSubmissions. Resource
		// replacement/release is deferred until End, so packet-local property and
		// buffer updates need no global lock.
		IDirect3DVertexBuffer9* expectedVertexBuffer =
			submission.staticResident
			? state.staticVertexBuffer : state.vertexBuffer;
		if (state.resourceSerial.load(std::memory_order_acquire)
			!= submission.resourceSerial
			|| submission.proxyIndex >= state.proxyCount
			|| !state.proxies[submission.proxyIndex].inUse
			|| expectedVertexBuffer != submission.vertexBuffer
			|| state.generation != submission.generation)
		{
			return NativeFontFallbackReason::RuntimeFault;
		}
		NativeFontProxy& reservedProxy = state.proxies[submission.proxyIndex];
		NiTriShape* proxy = reservedProxy.shape.m_pObject;
		NiGeometryBufferData* buffer = reservedProxy.buffer;
		NiVBChip* chip = reservedProxy.chip;
		if (!proxy || !buffer || !chip
			|| proxy != submission.proxyShape
			|| buffer != submission.proxyBuffer
			|| chip != submission.proxyChip
			|| !BindPacketAtlasPage(reservedProxy, *payload.payloadTemplate,
				source.atlasPage))
		{
			payload.packetPrepareFailure.store(
				NativeFontPacketPrepareFailure::Geometry,
				std::memory_order_relaxed);
			return NativeFontFallbackReason::PropertySync;
		}

		const UInt32 quadCount = vertexCount / 4u;
		chip->m_pkVB = submission.vertexBuffer;
		chip->m_uiOffset = 0;
		chip->m_uiLockFlags = 0;
		chip->m_uiSize = vertexCount * sizeof(NativeFontGpuVertex);
		buffer->m_uiVertCount = vertexCount;
		buffer->m_uiMaxVertCount = vertexCount;
		buffer->m_uiIndexCount = quadCount * 6u;
		buffer->m_uiBaseVertexIndex = static_cast<UInt32>(baseVertex);
		buffer->m_eType = D3DPT_TRIANGLELIST;
		buffer->m_uiTriCount = quadCount * 2u;
		buffer->m_uiMaxTriCount = quadCount * 2u;
		// One contiguous indexed array is required even when m_pusArrayLengths is
		// null; vanilla then uses m_uiTriCount for this single draw.
		buffer->m_uiNumArrays = kCanonicalArrayCount;
		proxy->GetModelData()->m_kBound = source.bound;
		if (reservedProxy.shader != shader)
		{
			proxy->SetShader(shader);
			reservedProxy.shader = proxy->GetShader();
			if (reservedProxy.shader != shader)
			{
				payload.packetPrepareFailure.store(
					NativeFontPacketPrepareFailure::ShaderBinding,
					std::memory_order_relaxed);
				return NativeFontFallbackReason::PacketPrepare;
			}
		}

		++submission.nextPacket;
		proxyShape = proxy;
		RecordFreeTypePerf(FreeTypePerfCounter::LocklessPacketPrepare);
		return NativeFontFallbackReason::None;
	}

	NativeFontFallbackReason SkipNativeFontRingPacket(
		NativeFontShapePayload& payload,
		NativeFontRingSubmission& submission, UInt32 packetIndex)
	{
		if (!submission.active || packetIndex != submission.nextPacket
			|| packetIndex >= payload.packetShaders.size()
			|| submission.generation != payload.preparedGeneration
			|| !IsNativeFontShaderGenerationCurrent(submission.generation)
			|| !payload.payloadTemplate)
		{
			return NativeFontFallbackReason::RuntimeFault;
		}
		const std::vector<NativeFontPacketTemplate>& activePackets =
			GetNativeFontPackets(*payload.payloadTemplate,
				payload.useCompositePackets);
		if (packetIndex >= activePackets.size())
			return NativeFontFallbackReason::PacketBuild;
		const NativeFontPacketTemplate& packet = activePackets[packetIndex];
		const UInt64 end = static_cast<UInt64>(packet.firstVertex)
			+ packet.vertexCount;
		if (!packet.vertexCount || (packet.vertexCount & 3u)
			|| packet.vertexCount / 4u > kNativeFontMaximumQuads
			|| end > payload.payloadTemplate->gpuVertices.size())
		{
			return NativeFontFallbackReason::PacketBuild;
		}
		++submission.nextPacket;
		return NativeFontFallbackReason::None;
	}

	void EndNativeFontRingSubmission(NativeFontRingSubmission& submission)
	{
		if (submission.active)
		{
			NativeFontRingState& state = RingState();
			if (SortedRingLease().active
				&& SortedRingLease().state == &state)
			{
				if (submission.proxyIndex < state.proxyCount)
					state.proxies[submission.proxyIndex].inUse = false;
				state.activeSubmissions.fetch_sub(
					1, std::memory_order_acq_rel);
			}
			else
			{
				std::lock_guard<std::mutex> lock(state.mutex);
				if (submission.proxyIndex < state.proxyCount)
					state.proxies[submission.proxyIndex].inUse = false;
				state.activeSubmissions.fetch_sub(
					1, std::memory_order_acq_rel);
				if (!state.activeSubmissions.load(std::memory_order_acquire)
					&& state.releasePending.load(
						std::memory_order_acquire))
				{
					ReleaseRingResourcesLocked(state);
				}
			}
		}
		submission = NativeFontRingSubmission{};
	}
	bool ResolveNativeFontFramePayloadBinding(
		const NativeFontShapePayload& payload,
		NativeFontFramePayloadBinding& binding)
	{
		binding = {};
		if (!SortedRingLease().active || !payload.payloadTemplate
			|| payload.preparedGeneration != SortedRingLease().generation)
		{
			return false;
		}
		NativeFontSortedRingLease& lease = SortedRingLease();
		NativeFontRingState* state = lease.state;
		if (!state || state->generation != lease.generation
			|| state->resourceSerial.load(std::memory_order_acquire)
				!= lease.resourceSerial
			|| state->uploadEpoch != lease.uploadEpoch
			|| state->indexBuffer != lease.indexBuffer
			|| state->declaration != lease.declaration)
		{
			return false;
		}
		const NativeFontPayloadTemplate& artifact =
			*payload.payloadTemplate;
		if (artifact.gpuVertices.empty()
			|| artifact.gpuVertices.size()
				> std::numeric_limits<UInt32>::max())
		{
			return false;
		}
		const UInt32 artifactVertices = static_cast<UInt32>(
			artifact.gpuVertices.size());
		UInt32 payloadBaseVertex = 0;
		bool staticResident = false;
		if (!ResolveSortedLeaseResidency(*state, artifact,
			artifactVertices, lease.resourceSerial, lease.uploadEpoch,
			payloadBaseVertex, staticResident))
		{
			return false;
		}

		binding.vertexBuffer = staticResident
			? lease.staticVertexBuffer : lease.dynamicVertexBuffer;
		binding.indexBuffer = lease.indexBuffer;
		binding.declaration = lease.declaration;
		binding.payloadBaseVertex = payloadBaseVertex;
		binding.payloadVertexCount = artifactVertices;
		binding.indexBytes = kCanonicalIndexBytes;
		binding.generation = lease.generation;
		binding.resourceSerial = lease.resourceSerial;
		binding.uploadEpoch = lease.uploadEpoch;
		binding.staticResident = staticResident;
		binding.active = binding.vertexBuffer && binding.indexBuffer
			&& binding.declaration;
		return binding.active;
	}

	bool ResolveNativeFontFramePacketBinding(
		const NativeFontShapePayload& payload, UInt32 packetIndex,
		NativeFontFramePacketBinding& binding)
	{
		binding = {};
		if (!payload.payloadTemplate)
			return false;
		const NativeFontPayloadTemplate& artifact =
			*payload.payloadTemplate;
		const std::vector<NativeFontPacketTemplate>& packets =
			GetNativeFontPackets(artifact, payload.useCompositePackets);
		if (packetIndex >= packets.size())
			return false;
		const NativeFontPacketTemplate& packet = packets[packetIndex];
		const UInt64 packetEnd = static_cast<UInt64>(packet.firstVertex)
			+ packet.vertexCount;
		if (!packet.vertexCount || (packet.vertexCount & 3u)
			|| packet.vertexCount / 4u > kNativeFontMaximumQuads
			|| packetEnd > artifact.gpuVertices.size())
		{
			return false;
		}
		NativeFontFramePayloadBinding payloadBinding;
		if (!ResolveNativeFontFramePayloadBinding(
				payload, payloadBinding))
			return false;
		const UInt64 baseVertex = static_cast<UInt64>(
			payloadBinding.payloadBaseVertex)
			+ packet.firstVertex;
		if (baseVertex > std::numeric_limits<UInt32>::max())
			return false;

		binding.vertexBuffer = payloadBinding.vertexBuffer;
		binding.indexBuffer = payloadBinding.indexBuffer;
		binding.declaration = payloadBinding.declaration;
		binding.baseVertex = static_cast<UInt32>(baseVertex);
		binding.vertexCount = packet.vertexCount;
		binding.indexBytes = payloadBinding.indexBytes;
		binding.generation = payloadBinding.generation;
		binding.resourceSerial = payloadBinding.resourceSerial;
		binding.uploadEpoch = payloadBinding.uploadEpoch;
		binding.staticResident = payloadBinding.staticResident;
		binding.active = payloadBinding.active;
		return binding.active;
	}

	bool IsNativeFontFramePacketBindingCurrent(
		const NativeFontFramePacketBinding& binding)
	{
		if (!binding.active || !SortedRingLease().active)
			return false;
		const NativeFontSortedRingLease& lease = SortedRingLease();
		const NativeFontRingState* state = lease.state;
		const IDirect3DVertexBuffer9* expectedVertexBuffer =
			binding.staticResident
				? lease.staticVertexBuffer : lease.dynamicVertexBuffer;
		return state && lease.active
			&& binding.generation == lease.generation
			&& binding.resourceSerial == lease.resourceSerial
			&& binding.uploadEpoch == lease.uploadEpoch
			&& binding.vertexBuffer == expectedVertexBuffer
			&& binding.indexBuffer == lease.indexBuffer
			&& binding.declaration == lease.declaration
			&& state->generation == lease.generation
			&& state->resourceSerial.load(std::memory_order_acquire)
				== lease.resourceSerial
			&& state->uploadEpoch == lease.uploadEpoch;
	}

	bool IsNativeFontFrameResourceStampCurrent(
		UInt32 generation, UInt32 resourceSerial, UInt32 uploadEpoch)
	{
		if (!generation || !resourceSerial
			|| !SortedRingLease().active)
		{
			return false;
		}
		const NativeFontSortedRingLease& lease = SortedRingLease();
		const NativeFontRingState* state = lease.state;
		return state && lease.active
			&& generation == lease.generation
			&& resourceSerial == lease.resourceSerial
			&& uploadEpoch == lease.uploadEpoch
			&& lease.indexBuffer && lease.declaration
			&& (lease.staticVertexBuffer || lease.dynamicVertexBuffer)
			&& state->generation == lease.generation
			&& state->resourceSerial.load(std::memory_order_acquire)
				== lease.resourceSerial
			&& state->uploadEpoch == lease.uploadEpoch
			&& state->indexBuffer == lease.indexBuffer
			&& state->declaration == lease.declaration;
	}
}

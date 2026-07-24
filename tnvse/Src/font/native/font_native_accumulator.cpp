#include "font_a8_internal.h"
#include "font_native_internal.h"
#include "load_config.h"
#include "tnvse.h"

#include "BSShaderManager.hpp"
#include "NiDX9TextureData.hpp"
#include "Utils/SafeWrite.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

namespace fonthook::vectorfont
{
	namespace
	{
		inline constexpr UInt32 kBSShaderAccumulatorVtable = 0x10ADFF8;
		inline constexpr UInt32 kRegisterObjectVtableSlot = 38;
		inline constexpr UInt32 kRegisterObjectVtableEntry =
			kBSShaderAccumulatorVtable + kRegisterObjectVtableSlot * sizeof(void*);
		inline constexpr UInt32 kMaximumMissingMetadataLogs = 8;

		using RegisterObjectFn = bool(__thiscall*)(BSShaderAccumulator*, NiGeometry*);

		RegisterObjectFn s_originalRegisterObject = nullptr;
		bool s_hookAttempted = false;
		std::atomic<UInt32> s_missingMetadataLogCount = 0;
		std::atomic<UInt32> s_atlasTextureEpoch = 1;

		int __fastcall NativeA8RenderSorted(
			BSShaderAccumulator* accumulator, void*);

		struct RegisteredFacade
		{
			NiTriShape* facade = nullptr;
			A8ShapeMetadataPtr metadata;
		};

		struct SortedFrameEntry
		{
			NiTriShape* facade = nullptr;
			A8ShapeMetadataPtr metadata;
			NativeA8ShapePayload* payload = nullptr;
			NativeA8FallbackReason preflightResult =
				NativeA8FallbackReason::RuntimeFault;
			UInt32 generation = 0;
		};

		struct NativePreflightFrameContext
		{
			UInt32 generation = 0;
			UInt32 atlasTextureEpoch = 0;
			bool accumulatorCurrent = false;
			bool tileRouteCurrent = false;
			bool rendererAvailable = false;
		};

		struct SortedPayloadScratch
		{
			BSShaderAccumulator* pendingAccumulator = nullptr;
			std::vector<RegisteredFacade> pendingRegistrations;
			std::vector<UInt32> registrationLookup;
			std::vector<SortedFrameEntry> frameEntries;
			std::vector<UInt32> facadeLookup;
			std::vector<NativeA8PayloadTemplatePtr> payloadTemplates;
			std::vector<UInt32> payloadLookup;
			CpuMemoryLease cpuMemory;
			UInt32 nestedBypassDepth = 0;
			bool active = false;
		};

		thread_local SortedPayloadScratch s_sortedPayloadScratch;

		size_t HashPointer(const void* pointer)
		{
			size_t value = reinterpret_cast<size_t>(pointer) >> 4;
			value ^= value >> 16;
			value *= static_cast<size_t>(0x45D9F3Bu);
			value ^= value >> 16;
			return value;
		}

		size_t GetLookupCapacity(size_t expectedEntries)
		{
			size_t capacity = 8;
			const size_t required = std::max<size_t>(8, expectedEntries * 2u);
			while (capacity < required)
				capacity <<= 1;
			return capacity;
		}

		void PrepareLookup(std::vector<UInt32>& lookup, size_t expectedEntries)
		{
			const size_t capacity = GetLookupCapacity(expectedEntries);
			if (lookup.size() != capacity)
				lookup.assign(capacity, 0);
			else
				std::fill(lookup.begin(), lookup.end(), 0);
		}

		size_t LookupSortedFacade(const SortedPayloadScratch& scratch,
			const NiTriShape* facade)
		{
			if (!facade || scratch.facadeLookup.empty())
				return std::numeric_limits<size_t>::max();
			const size_t mask = scratch.facadeLookup.size() - 1u;
			size_t slot = HashPointer(facade) & mask;
			for (size_t probe = 0; probe < scratch.facadeLookup.size(); ++probe)
			{
				const UInt32 stored = scratch.facadeLookup[slot];
				if (!stored)
					return std::numeric_limits<size_t>::max();
				const size_t index = static_cast<size_t>(stored - 1u);
				if (index < scratch.frameEntries.size()
					&& scratch.frameEntries[index].facade == facade)
				{
					return index;
				}
				slot = (slot + 1u) & mask;
			}
			return std::numeric_limits<size_t>::max();
		}

		void InsertSortedFacade(SortedPayloadScratch& scratch,
			NiTriShape* facade, size_t entryIndex)
		{
			const size_t mask = scratch.facadeLookup.size() - 1u;
			size_t slot = HashPointer(facade) & mask;
			while (scratch.facadeLookup[slot])
				slot = (slot + 1u) & mask;
			scratch.facadeLookup[slot] = static_cast<UInt32>(entryIndex + 1u);
		}

		bool InsertUniquePayload(SortedPayloadScratch& scratch,
			const NativeA8PayloadTemplatePtr& payloadTemplate)
		{
			if (!payloadTemplate || scratch.payloadLookup.empty())
				return false;
			const size_t mask = scratch.payloadLookup.size() - 1u;
			size_t slot = HashPointer(payloadTemplate.get()) & mask;
			for (size_t probe = 0; probe < scratch.payloadLookup.size(); ++probe)
			{
				const UInt32 stored = scratch.payloadLookup[slot];
				if (!stored)
				{
					scratch.payloadTemplates.push_back(payloadTemplate);
					scratch.payloadLookup[slot] = static_cast<UInt32>(
						scratch.payloadTemplates.size());
					return true;
				}
				const size_t index = static_cast<size_t>(stored - 1u);
				if (index < scratch.payloadTemplates.size()
					&& scratch.payloadTemplates[index].get()
						== payloadTemplate.get())
				{
					return false;
				}
				slot = (slot + 1u) & mask;
			}
			return false;
		}

		void RefreshSortedScratchMemory(SortedPayloadScratch& scratch)
		{
			const size_t bytes =
				scratch.pendingRegistrations.capacity()
					* sizeof(RegisteredFacade)
				+ scratch.registrationLookup.capacity() * sizeof(UInt32)
				+ scratch.frameEntries.capacity() * sizeof(SortedFrameEntry)
				+ scratch.facadeLookup.capacity() * sizeof(UInt32)
				+ scratch.payloadTemplates.capacity()
					* sizeof(NativeA8PayloadTemplatePtr)
				+ scratch.payloadLookup.capacity() * sizeof(UInt32);
			scratch.cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata, bytes);
		}

		void ClearSortedFrame(SortedPayloadScratch& scratch)
		{
			scratch.active = false;
			scratch.frameEntries.clear();
			scratch.payloadTemplates.clear();
			scratch.pendingAccumulator = nullptr;
			scratch.pendingRegistrations.clear();
			if (scratch.pendingRegistrations.capacity() > 8192)
			{
				std::vector<RegisteredFacade>().swap(
					scratch.pendingRegistrations);
			}
			if (scratch.registrationLookup.capacity() > 16384)
				std::vector<UInt32>().swap(scratch.registrationLookup);
			if (scratch.frameEntries.capacity() > 8192)
				std::vector<SortedFrameEntry>().swap(scratch.frameEntries);
			if (scratch.facadeLookup.capacity() > 16384)
				std::vector<UInt32>().swap(scratch.facadeLookup);
			if (scratch.payloadTemplates.capacity() > 8192)
			{
				std::vector<NativeA8PayloadTemplatePtr>().swap(
					scratch.payloadTemplates);
			}
			if (scratch.payloadLookup.capacity() > 16384)
				std::vector<UInt32>().swap(scratch.payloadLookup);
			RefreshSortedScratchMemory(scratch);
			if (IsCpuMemoryBudgetExceeded())
			{
				std::vector<RegisteredFacade>().swap(
					scratch.pendingRegistrations);
				std::vector<UInt32>().swap(scratch.registrationLookup);
				std::vector<SortedFrameEntry>().swap(scratch.frameEntries);
				std::vector<UInt32>().swap(scratch.facadeLookup);
				std::vector<NativeA8PayloadTemplatePtr>().swap(
					scratch.payloadTemplates);
				std::vector<UInt32>().swap(scratch.payloadLookup);
				scratch.cpuMemory.Release();
			}
		}

		SortedTileRenderFn ReadSortedTileRenderCallTarget()
		{
			const UInt8* call = reinterpret_cast<const UInt8*>(
				kSortedTileRenderCallSite);
			if (!call || call[0] != 0xE8)
				return nullptr;
			SInt32 displacement = 0;
			std::memcpy(&displacement, call + 1, sizeof(displacement));
			return reinterpret_cast<SortedTileRenderFn>(
				kSortedTileRenderCallSite + 5 + displacement);
		}

		void ClearPendingRegistrations(SortedPayloadScratch& scratch)
		{
			scratch.pendingAccumulator = nullptr;
			scratch.pendingRegistrations.clear();
		}

		void RecordSortedRegistration(BSShaderAccumulator* accumulator,
			NiTriShape* facade, const A8ShapeMetadataPtr& metadata)
		{
			SortedPayloadScratch& scratch = s_sortedPayloadScratch;
			const bool hookCurrent = ReadSortedTileRenderCallTarget()
				== reinterpret_cast<SortedTileRenderFn>(
					&NativeA8RenderSorted);
			if (!accumulator || !facade || !metadata || scratch.active
				|| scratch.nestedBypassDepth
				|| !hookCurrent)
			{
				if (!scratch.active && !scratch.nestedBypassDepth
					&& !hookCurrent)
				{
					ClearPendingRegistrations(scratch);
				}
				return;
			}
			if (scratch.pendingAccumulator != accumulator)
			{
				ClearPendingRegistrations(scratch);
				scratch.pendingAccumulator = accumulator;
			}
			scratch.pendingRegistrations.push_back({ facade, metadata });
		}

		void PrepareRegistrationLookup(SortedPayloadScratch& scratch)
		{
			PrepareLookup(scratch.registrationLookup,
				scratch.pendingRegistrations.size());
			const size_t mask = scratch.registrationLookup.size() - 1u;
			for (size_t index = 0;
				index < scratch.pendingRegistrations.size(); ++index)
			{
				const NiTriShape* facade =
					scratch.pendingRegistrations[index].facade;
				size_t slot = HashPointer(facade) & mask;
				for (;;)
				{
					const UInt32 stored = scratch.registrationLookup[slot];
					if (!stored)
					{
						scratch.registrationLookup[slot] =
							static_cast<UInt32>(index + 1u);
						break;
					}
					const size_t storedIndex =
						static_cast<size_t>(stored - 1u);
					if (storedIndex < scratch.pendingRegistrations.size()
						&& scratch.pendingRegistrations[storedIndex].facade
							== facade)
					{
						// The newest registration is the one represented in the
						// accumulator if a facade was submitted more than once.
						scratch.registrationLookup[slot] =
							static_cast<UInt32>(index + 1u);
						break;
					}
					slot = (slot + 1u) & mask;
				}
			}
		}

		const A8ShapeMetadataPtr* FindRegisteredMetadata(
			const SortedPayloadScratch& scratch, const NiTriShape* facade)
		{
			if (!facade || scratch.registrationLookup.empty())
				return nullptr;
			const size_t mask = scratch.registrationLookup.size() - 1u;
			size_t slot = HashPointer(facade) & mask;
			for (size_t probe = 0;
				probe < scratch.registrationLookup.size(); ++probe)
			{
				const UInt32 stored = scratch.registrationLookup[slot];
				if (!stored)
					return nullptr;
				const size_t index = static_cast<size_t>(stored - 1u);
				if (index < scratch.pendingRegistrations.size()
					&& scratch.pendingRegistrations[index].facade == facade)
				{
					return &scratch.pendingRegistrations[index].metadata;
				}
				slot = (slot + 1u) & mask;
			}
			return nullptr;
		}

		bool IsFreeTypeFacade(const NiGeometry* geometry)
		{
			if (!geometry || !State().originalTriShapeVtable)
				return false;
			void* const* vtable = *reinterpret_cast<void* const* const*>(geometry);
			return vtable == &State().triShapeVtable[1];
		}

		void ClearNativePacketFailure(NativeA8ShapePayload& payload)
		{
			payload.packetPrepareFailure.store(
				NativeA8PacketPrepareFailure::None, std::memory_order_relaxed);
		}

		void InvalidateNativePreflight(NativeA8ShapePayload& payload)
		{
			payload.preparedGeneration = 0;
			payload.preflightAtlasTextureEpoch = 0;
			std::fill(payload.preflightAtlasTextures.begin(),
				payload.preflightAtlasTextures.end(), nullptr);
			std::fill(payload.packetShaders.begin(),
				payload.packetShaders.end(), nullptr);
		}

		bool IsNativePreflightCacheCurrent(const NativeA8ShapePayload& payload,
			UInt32 generation,
			UInt32 atlasTextureEpoch, bool scaledFillSampling,
			bool alphaBlending)
		{
			if (!payload.payloadTemplate)
				return false;
			const NativeA8PayloadTemplate& artifact = *payload.payloadTemplate;
			const bool compositeDesired =
				g_bEnableFreeTypeFontCompositePass
				&& !artifact.compositePackets.empty()
				&& artifact.compositeRejectedGeneration.load(
					std::memory_order_acquire) != generation
				&& !(payload.compositeUnavailable
					&& payload.compositeAttemptGeneration == generation);
			const std::vector<NativeA8PacketTemplate>& packets =
				GetNativeA8Packets(artifact, payload.useCompositePackets);
			if (payload.preparedGeneration != generation
				|| payload.preflightAtlasTextureEpoch != atlasTextureEpoch
				|| payload.preflightScaledFillSampling != scaledFillSampling
				|| payload.preflightAlphaBlending != alphaBlending
				|| payload.useCompositePackets != compositeDesired
				|| payload.preflightAtlasTextures.size()
					!= artifact.atlasTextures.size()
				|| payload.packetShaders.size() != packets.size())
			{
				return false;
			}
			return true;
		}

		NativeA8FallbackReason PreflightNativeGroupImpl(NiTriShape* facade,
			const A8ShapeMetadata& metadata, NativeA8ShapePayload& payload,
			const NativePreflightFrameContext* frameContext = nullptr)
		{
			if (!facade || !payload.buildComplete || !payload.payloadTemplate
				|| payload.payloadTemplate->packets.empty())
			{
				return NativeA8FallbackReason::PacketBuild;
			}
			const NativeA8PayloadTemplate& artifact = *payload.payloadTemplate;
			if (!(frameContext
				? frameContext->accumulatorCurrent
				: IsNativeA8AccumulatorHookCurrent()))
				return NativeA8FallbackReason::AccumulatorConflict;
			if (!(frameContext
				? frameContext->tileRouteCurrent
				: IsA8TileRenderPassHookCurrent()))
				return NativeA8FallbackReason::TileRouteConflict;
			if (!(frameContext
				? frameContext->rendererAvailable
				: IsNativeA8RendererAvailable()))
				return NativeA8FallbackReason::ShaderGeneration;

			const UInt32 generation = frameContext
				? frameContext->generation : GetNativeA8ShaderGeneration();
			if (!generation)
				return NativeA8FallbackReason::ShaderGeneration;
			const bool scaledFillSampling = NeedsScaledFillSampling(facade);
			const NiAlphaProperty* alpha = facade->GetAlphaProperty();
			const bool alphaBlending = alpha && alpha->GetAlphaBlending();
			const UInt32 atlasTextureEpoch = frameContext
				? frameContext->atlasTextureEpoch
				: GetNativeA8AtlasTextureEpoch();
			if (IsNativePreflightCacheCurrent(payload, generation,
				atlasTextureEpoch, scaledFillSampling, alphaBlending))
			{
				RecordFreeTypePerf(FreeTypePerfCounter::PreflightFastHit);
				ClearNativePacketFailure(payload);
				return NativeA8FallbackReason::None;
			}

			RecordFreeTypePerf(FreeTypePerfCounter::PreflightFullValidation);
			InvalidateNativePreflight(payload);
			payload.preflightScaledFillSampling = scaledFillSampling;
			payload.preflightAlphaBlending = alphaBlending;
			const bool attemptComposite =
				g_bEnableFreeTypeFontCompositePass
				&& !artifact.compositePackets.empty()
				&& artifact.compositeRejectedGeneration.load(
					std::memory_order_acquire) != generation
				&& !(payload.compositeUnavailable
					&& payload.compositeAttemptGeneration == generation);
			payload.useCompositePackets = attemptComposite;
			const std::vector<NativeA8PacketTemplate>* packets =
				&GetNativeA8Packets(artifact, payload.useCompositePackets);
			payload.packetShaders.assign(packets->size(), nullptr);
			if (payload.preflightAtlasTextures.size() != artifact.atlasTextures.size())
				return NativeA8FallbackReason::PacketBuild;
			for (const NativeA8PacketTemplate& packetTemplate : *packets)
			{
				const UInt64 vertexEnd = static_cast<UInt64>(
					packetTemplate.firstVertex) + packetTemplate.vertexCount;
				if (!packetTemplate.vertexCount
					|| (packetTemplate.vertexCount & 3u)
					|| vertexEnd > artifact.gpuVertices.size()
					|| packetTemplate.atlasPage >= artifact.atlasTextures.size())
				{
					return NativeA8FallbackReason::PacketBuild;
				}
				const size_t page = packetTemplate.atlasPage;
				if (payload.preflightAtlasTextures[page])
					continue;
				NiTexture* texture = artifact.atlasTextures[page].m_pObject;
				NiDX9TextureData* textureData = texture
					? texture->GetDX9RendererData() : nullptr;
				const void* d3dTexture = textureData
					? textureData->GetD3DTexture() : nullptr;
				if (!d3dTexture)
					return NativeA8FallbackReason::PageTexture;
				payload.preflightAtlasTextures[page] = d3dTexture;
			}

			bool shaderSetReady = true;
			for (size_t index = 0; index < packets->size(); ++index)
			{
				payload.packetShaders[index] = ResolveNativeA8PacketShader(
					(*packets)[index],
					facade, scaledFillSampling);
				if (!payload.packetShaders[index])
				{
					shaderSetReady = false;
					break;
				}
			}
			if (!shaderSetReady && attemptComposite)
			{
				// Composite shaders are optional generation members.  Reject only
				// this optimization for the current generation and immediately
				// resolve the ordinary quality-equivalent packet set.
				payload.compositeAttemptGeneration = generation;
				payload.compositeUnavailable = true;
				RecordFreeTypePerf(
					FreeTypePerfCounter::CompositeShaderFallback);
				payload.useCompositePackets = false;
				packets = &artifact.packets;
				payload.packetShaders.assign(packets->size(), nullptr);
				shaderSetReady = true;
				for (size_t index = 0; index < packets->size(); ++index)
				{
					payload.packetShaders[index] = ResolveNativeA8PacketShader(
						(*packets)[index], facade, scaledFillSampling);
					if (!payload.packetShaders[index])
					{
						shaderSetReady = false;
						break;
					}
				}
			}
			if (!shaderSetReady)
				return NativeA8FallbackReason::ShaderGeneration;
			if (attemptComposite && payload.useCompositePackets)
			{
				payload.compositeAttemptGeneration = generation;
				payload.compositeUnavailable = false;
			}
			if (GetNativeA8AtlasTextureEpoch() != atlasTextureEpoch)
			{
				InvalidateNativePreflight(payload);
				return NativeA8FallbackReason::AtlasGeneration;
			}

			payload.preparedGeneration = generation;
			payload.preflightAtlasTextureEpoch = atlasTextureEpoch;
			ClearNativePacketFailure(payload);
			return NativeA8FallbackReason::None;
		}

		bool SuppressNativeGroup(NiTriShape* facade,
			const A8ShapeMetadata& metadata, NativeA8FallbackReason reason,
			const char* phase)
		{
			RecordNativeA8Suppression(facade, metadata, reason, phase);
			// Match stock's accepted/skipped result while preventing a marked facade
			// from entering a renderer path that cannot interpret native packet data.
			return true;
		}

		bool __fastcall NativeA8RegisterObject(BSShaderAccumulator* accumulator,
			void*, NiGeometry* geometry)
		{
			if (!s_originalRegisterObject || !accumulator || !geometry
				|| accumulator->eRenderMode != BSShaderManager::BSSM_RENDER_TILES
				|| !IsFreeTypeFacade(geometry))
			{
				return s_originalRegisterObject
					? s_originalRegisterObject(accumulator, geometry) : false;
			}

			NiTriShape* facade = static_cast<NiTriShape*>(geometry);
			const A8ShapeMetadataPtr metadata = FindA8ShapeMetadata(facade);
			if (!metadata || !metadata->nativePayload.buildComplete)
			{
				if (metadata)
					return SuppressNativeGroup(facade, *metadata,
						NativeA8FallbackReason::PacketBuild, "register-object");
				if (g_bEnableFreeTypeFontRenderingLog)
				{
					const UInt32 ordinal = s_missingMetadataLogCount.fetch_add(
						1, std::memory_order_relaxed);
					if (ordinal < kMaximumMissingMetadataLogs)
					{
						gLog.FormattedMessage(
							"tnvse_freetype_native: submission-suppressed reason=metadata-missing phase=register-object shape=%p thread=%u",
							facade, GetCurrentThreadId());
					}
					else if (ordinal == kMaximumMissingMetadataLogs)
					{
						gLog.FormattedMessage(
							"tnvse_freetype_native: metadata-missing registration logs capped at %u entries",
							kMaximumMissingMetadataLogs);
					}
				}
				return true;
			}

			// Keep one facade in the stock Tile alpha list. Equal-depth entries are
			// quicksorted unstably, so individually registered packets cannot retain
			// Glow/Shadow/Outline/Fill order. Expand only after stock UI sorting.
			if (!IsA8TileRenderPassHookCurrent())
				return SuppressNativeGroup(facade, *metadata,
					NativeA8FallbackReason::TileRouteConflict, "register-object");
			RecordSortedRegistration(accumulator, facade, metadata);
			return s_originalRegisterObject(accumulator, facade);
		}

		int __fastcall NativeA8RenderSorted(BSShaderAccumulator* accumulator, void*)
		{
			A8State& state = State();
			if (!state.originalSortedTileRender)
				return 0;

			SortedPayloadScratch& scratch = s_sortedPayloadScratch;
			if (scratch.active || scratch.nestedBypassDepth)
			{
				// A nested stock Tile pass must not see facade entries from the outer
				// accumulator. It retains the fully validated map/preflight fallback.
				const bool restoreActive = scratch.active;
				scratch.active = false;
				++scratch.nestedBypassDepth;
				const int result = state.originalSortedTileRender(accumulator);
				--scratch.nestedBypassDepth;
				scratch.active = restoreActive;
				return result;
			}

			if (accumulator
				&& accumulator->eRenderMode == BSShaderManager::BSSM_RENDER_TILES
				&& accumulator->m_iNumItems > 0 && accumulator->m_ppkItems)
			{
				const size_t itemCount = static_cast<size_t>(
					accumulator->m_iNumItems);
				scratch.frameEntries.clear();
				scratch.payloadTemplates.clear();
				scratch.frameEntries.reserve(itemCount);
				scratch.payloadTemplates.reserve(itemCount);
				PrepareLookup(scratch.facadeLookup, itemCount);
				PrepareLookup(scratch.payloadLookup, itemCount);
				const bool haveRegisteredMetadata =
					scratch.pendingAccumulator == accumulator;
				if (haveRegisteredMetadata)
					PrepareRegistrationLookup(scratch);
				else
					ClearPendingRegistrations(scratch);
				NativePreflightFrameContext preflightContext;
				preflightContext.accumulatorCurrent =
					IsNativeA8AccumulatorHookCurrent();
				preflightContext.tileRouteCurrent =
					IsA8TileRenderPassHookCurrent();
				preflightContext.rendererAvailable =
					IsNativeA8RendererAvailable();
				preflightContext.generation =
					GetNativeA8ShaderGeneration();
				preflightContext.atlasTextureEpoch =
					GetNativeA8AtlasTextureEpoch();
				const UInt32 generation = preflightContext.generation;
				for (SInt32 index = accumulator->m_iNumItems - 1;
					index >= 0; --index)
				{
					NiGeometry* geometry = accumulator->m_ppkItems[index];
					if (!IsFreeTypeFacade(geometry))
						continue;
					NiTriShape* facade = static_cast<NiTriShape*>(geometry);
					if (LookupSortedFacade(scratch, facade)
						!= std::numeric_limits<size_t>::max())
					{
						continue;
					}

					SortedFrameEntry entry;
					entry.facade = facade;
					const A8ShapeMetadataPtr* registeredMetadata =
						haveRegisteredMetadata
							? FindRegisteredMetadata(scratch, facade) : nullptr;
					entry.metadata = registeredMetadata
						? *registeredMetadata : FindA8ShapeMetadata(facade);
					entry.generation = generation;
					if (entry.metadata
						&& entry.metadata->nativePayload.buildComplete)
					{
						entry.payload = &entry.metadata->nativePayload;
						entry.preflightResult = PreflightNativeGroupImpl(
							facade, *entry.metadata, *entry.payload,
							&preflightContext);
						if (entry.preflightResult == NativeA8FallbackReason::None)
							entry.generation = entry.payload->preparedGeneration;
					}
					else
					{
						entry.preflightResult =
							NativeA8FallbackReason::PacketBuild;
					}

					const size_t entryIndex = scratch.frameEntries.size();
					scratch.frameEntries.push_back(std::move(entry));
					InsertSortedFacade(scratch, facade, entryIndex);
					RecordFreeTypePerf(FreeTypePerfCounter::SortedFrameFacade);
					const SortedFrameEntry& stored =
						scratch.frameEntries.back();
					if (stored.preflightResult == NativeA8FallbackReason::None
						&& stored.payload && stored.payload->payloadTemplate
						&& stored.generation == generation)
					{
						if (InsertUniquePayload(scratch,
							stored.payload->payloadTemplate))
						{
							RecordFreeTypePerf(
								FreeTypePerfCounter::SortedFramePayload);
						}
						const NativeA8PayloadTemplatePtr cachedArtifact =
							GetNativeA8CompositeCacheArtifact(*stored.payload);
						if (cachedArtifact
							&& InsertUniquePayload(scratch, cachedArtifact))
						{
							RecordFreeTypePerf(
								FreeTypePerfCounter::SortedFramePayload);
						}
					}
				}
				PrepareSortedNativeA8Payloads(
					scratch.payloadTemplates, generation);
				RefreshSortedScratchMemory(scratch);
				BeginNativeA8CompositeCacheFrame();
				BeginNativeA8SortedShaderBatch();
				BeginA8SortedTileConstantBatch();
				scratch.active = true;
				const int result = state.originalSortedTileRender(accumulator);
				EndA8SortedTileConstantBatch();
				EndNativeA8SortedShaderBatch();
				EndNativeA8SortedRingFrame();
				EndNativeA8CompositeCacheFrame();
				ClearSortedFrame(scratch);
				return result;
			}

			if (scratch.pendingAccumulator == accumulator)
			{
				ClearPendingRegistrations(scratch);
				RefreshSortedScratchMemory(scratch);
			}
			return state.originalSortedTileRender(accumulator);
		}

		bool HookSortedTileRender()
		{
			A8State& state = State();
			const SortedTileRenderFn hook = reinterpret_cast<SortedTileRenderFn>(
				&NativeA8RenderSorted);
			const SortedTileRenderFn current = ReadSortedTileRenderCallTarget();
			if (current == hook)
			{
				state.sortedTileRenderHookInstalled =
					state.originalSortedTileRender != nullptr;
				return state.sortedTileRenderHookInstalled;
			}
			if (!current)
			{
				if (!state.loggedSortedTileRenderHookConflict)
				{
					state.loggedSortedTileRenderHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: sorted Tile render call site is not CALL rel32; frame upload batching disabled");
				}
				return false;
			}
			if (state.sortedTileRenderHookInstalled)
			{
				state.sortedTileRenderHookInstalled = false;
				if (!state.loggedSortedTileRenderHookConflict)
				{
					state.loggedSortedTileRenderHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: sorted Tile frame upload hook was replaced; per-shape upload fallback remains active");
				}
				return false;
			}
			if (reinterpret_cast<UInt32>(current) != kStockSortedTileRender)
			{
				if (!state.loggedSortedTileRenderHookConflict)
				{
					state.loggedSortedTileRenderHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: sorted Tile render call site already has a non-stock target=%p; frame upload batching left disabled",
						current);
				}
				return false;
			}

			state.originalSortedTileRender = current;
			WriteRelCall(kSortedTileRenderCallSite, hook);
			state.sortedTileRenderHookInstalled =
				ReadSortedTileRenderCallTarget() == hook;
			if (!state.sortedTileRenderHookInstalled)
			{
				state.originalSortedTileRender = nullptr;
				return false;
			}
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: installed sorted Tile frame upload route original=%p stock=1",
					current);
			}
			return true;
		}
	}

	bool FindNativeA8SortedFrameEntry(NiTriShape* facade,
		NativeA8SortedFrameEntryView& view)
	{
		view = {};
		const SortedPayloadScratch& scratch = s_sortedPayloadScratch;
		if (!scratch.active || !facade)
			return false;
		const size_t index = LookupSortedFacade(scratch, facade);
		if (index == std::numeric_limits<size_t>::max()
			|| index >= scratch.frameEntries.size())
		{
			return false;
		}
		const SortedFrameEntry& entry = scratch.frameEntries[index];
		view.metadata = entry.metadata.get();
		view.payload = entry.payload;
		view.preflightResult = entry.preflightResult;
		view.generation = entry.generation;
		RecordFreeTypePerf(FreeTypePerfCounter::SortedFrameLookupHit);
		return true;
	}

	UInt32 GetNativeA8AtlasTextureEpoch()
	{
		return s_atlasTextureEpoch.load(std::memory_order_acquire);
	}

	void NotifyNativeA8AtlasTextureMutation()
	{
		UInt32 current = s_atlasTextureEpoch.load(std::memory_order_relaxed);
		for (;;)
		{
			UInt32 next = current + 1u;
			if (!next)
				next = 1u;
			if (s_atlasTextureEpoch.compare_exchange_weak(current, next,
				std::memory_order_release, std::memory_order_relaxed))
			{
				return;
			}
		}
	}

	NativeA8FallbackReason PrepareNativeA8Group(NiTriShape* facade,
		const A8ShapeMetadata& metadata, NativeA8ShapePayload& payload)
	{
		return PreflightNativeGroupImpl(facade, metadata, payload);
	}

	bool HookNativeA8Accumulator()
	{
		void* current = *reinterpret_cast<void**>(kRegisterObjectVtableEntry);
		if (current == reinterpret_cast<void*>(&NativeA8RegisterObject))
		{
			HookSortedTileRender();
			return s_originalRegisterObject != nullptr;
		}
		if (s_hookAttempted)
			return false;
		s_hookAttempted = true;
		if (!current)
			return false;
		s_originalRegisterObject = reinterpret_cast<RegisterObjectFn>(current);
		SafeWrite32(kRegisterObjectVtableEntry,
			reinterpret_cast<UInt32>(&NativeA8RegisterObject));
		const bool accumulatorReady = IsNativeA8AccumulatorHookCurrent();
		if (accumulatorReady)
			HookSortedTileRender();
		return accumulatorReady;
	}

	bool IsNativeA8AccumulatorHookCurrent()
	{
		return *reinterpret_cast<void**>(kRegisterObjectVtableEntry)
			== reinterpret_cast<void*>(&NativeA8RegisterObject)
			&& s_originalRegisterObject != nullptr;
	}
}

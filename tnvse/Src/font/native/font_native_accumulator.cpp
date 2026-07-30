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
	namespace implementation::font_native_accumulator {}
	using namespace implementation::font_native_accumulator;

	namespace implementation::font_native_accumulator
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
			// pendingRegistrations owns metadata for the normal sorted route.
			// Entries only need a stable non-owning view during the stock traversal;
			// fallbackMetadataOwners covers the uncommon map-lookup fallback.
			const A8ShapeMetadata* metadata = nullptr;
			NativeA8ShapePayload* payload = nullptr;
			NativeA8FallbackReason preflightResult =
				NativeA8FallbackReason::RuntimeFault;
			NativeA8VisibilityCull visibilityCull =
				NativeA8VisibilityCull::None;
			UInt32 generation = 0;
			UInt64 validationToken = 0;
			UInt32 commandSpanIndex = kInvalidNativeA8CommandIndex;
			UInt32 singlePacketCommandIndex =
				kInvalidNativeA8CommandIndex;
		};

		struct VirtualStockFrameSlot
		{
			NiTriShape* shape = nullptr;
			VirtualStockShapeGroup* group = nullptr;
			UInt32 slotIndex = 0;
			SInt32 itemIndex = -1;
			UInt32 occurrences = 0;
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
			std::vector<NiTriShape*> frameCandidates;
			std::vector<SortedFrameEntry> frameEntries;
			std::vector<UInt32> facadeLookup;
			std::vector<A8ShapeMetadataPtr> fallbackMetadataOwners;
			std::vector<NativeA8PayloadTemplatePtr> payloadTemplates;
			std::vector<UInt32> payloadLookup;
			std::vector<std::shared_ptr<VirtualStockShapeGroup>>
				virtualStockGroups;
			std::vector<VirtualStockFrameSlot> virtualStockSlots;
			std::vector<UInt32> virtualStockSlotLookup;
			CpuMemoryLease cpuMemory;
			UInt32 nestedBypassDepth = 0;
			UInt64 nestedTraversalSerial = 1;
			UInt64 registrationCycle = 1;
			UInt64 nextValidationToken = 0;
			UInt64 activeValidationToken = 0;
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
				+ scratch.frameCandidates.capacity() * sizeof(NiTriShape*)
				+ scratch.frameEntries.capacity() * sizeof(SortedFrameEntry)
				+ scratch.facadeLookup.capacity() * sizeof(UInt32)
				+ scratch.fallbackMetadataOwners.capacity()
					* sizeof(A8ShapeMetadataPtr)
				+ scratch.payloadTemplates.capacity()
					* sizeof(NativeA8PayloadTemplatePtr)
				+ scratch.payloadLookup.capacity() * sizeof(UInt32)
				+ scratch.virtualStockGroups.capacity()
					* sizeof(std::shared_ptr<VirtualStockShapeGroup>)
				+ scratch.virtualStockSlots.capacity()
					* sizeof(VirtualStockFrameSlot)
				+ scratch.virtualStockSlotLookup.capacity() * sizeof(UInt32);
			scratch.cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata, bytes);
		}

		void ClearSortedFrame(SortedPayloadScratch& scratch)
		{
			EndNativeA8FrameCommandBuffer();
			scratch.active = false;
			scratch.activeValidationToken = 0;
			scratch.frameCandidates.clear();
			scratch.frameEntries.clear();
			scratch.fallbackMetadataOwners.clear();
			scratch.payloadTemplates.clear();
			scratch.virtualStockGroups.clear();
			scratch.virtualStockSlots.clear();
			scratch.pendingAccumulator = nullptr;
			scratch.pendingRegistrations.clear();
			if (++scratch.registrationCycle == 0)
				++scratch.registrationCycle;
			if (scratch.pendingRegistrations.capacity() > 8192)
			{
				std::vector<RegisteredFacade>().swap(
					scratch.pendingRegistrations);
			}
			if (scratch.registrationLookup.capacity() > 16384)
				std::vector<UInt32>().swap(scratch.registrationLookup);
			if (scratch.frameEntries.capacity() > 8192)
				std::vector<SortedFrameEntry>().swap(scratch.frameEntries);
			if (scratch.frameCandidates.capacity() > 8192)
				std::vector<NiTriShape*>().swap(scratch.frameCandidates);
			if (scratch.facadeLookup.capacity() > 16384)
				std::vector<UInt32>().swap(scratch.facadeLookup);
			if (scratch.fallbackMetadataOwners.capacity() > 8192)
			{
				std::vector<A8ShapeMetadataPtr>().swap(
					scratch.fallbackMetadataOwners);
			}
			if (scratch.payloadTemplates.capacity() > 8192)
			{
				std::vector<NativeA8PayloadTemplatePtr>().swap(
					scratch.payloadTemplates);
			}
			if (scratch.payloadLookup.capacity() > 16384)
				std::vector<UInt32>().swap(scratch.payloadLookup);
			if (scratch.virtualStockGroups.capacity() > 8192)
			{
				std::vector<std::shared_ptr<VirtualStockShapeGroup>>().swap(
					scratch.virtualStockGroups);
			}
			if (scratch.virtualStockSlots.capacity() > 8192)
				std::vector<VirtualStockFrameSlot>().swap(
					scratch.virtualStockSlots);
			if (scratch.virtualStockSlotLookup.capacity() > 16384)
				std::vector<UInt32>().swap(scratch.virtualStockSlotLookup);
			RefreshSortedScratchMemory(scratch);
			if (IsCpuMemoryBudgetExceeded())
			{
				std::vector<RegisteredFacade>().swap(
					scratch.pendingRegistrations);
				std::vector<UInt32>().swap(scratch.registrationLookup);
				std::vector<SortedFrameEntry>().swap(scratch.frameEntries);
				std::vector<NiTriShape*>().swap(scratch.frameCandidates);
				std::vector<UInt32>().swap(scratch.facadeLookup);
				std::vector<A8ShapeMetadataPtr>().swap(
					scratch.fallbackMetadataOwners);
				std::vector<NativeA8PayloadTemplatePtr>().swap(
					scratch.payloadTemplates);
				std::vector<UInt32>().swap(scratch.payloadLookup);
				std::vector<std::shared_ptr<VirtualStockShapeGroup>>().swap(
					scratch.virtualStockGroups);
				std::vector<VirtualStockFrameSlot>().swap(
					scratch.virtualStockSlots);
				std::vector<UInt32>().swap(
					scratch.virtualStockSlotLookup);
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

		UInt64 RecordSortedRegistration(BSShaderAccumulator* accumulator,
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
				return 0;
			}
			if (scratch.pendingAccumulator != accumulator)
			{
				ClearPendingRegistrations(scratch);
				scratch.pendingAccumulator = accumulator;
				if (++scratch.registrationCycle == 0)
					++scratch.registrationCycle;
			}
			scratch.pendingRegistrations.push_back({ facade, metadata });
			return scratch.registrationCycle;
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

		const A8ShapeMetadata* FindRegisteredMetadata(
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
					return scratch.pendingRegistrations[index].metadata.get();
				}
				slot = (slot + 1u) & mask;
			}
			return nullptr;
		}

		size_t LookupVirtualStockFrameSlot(
			const SortedPayloadScratch& scratch, const NiTriShape* shape)
		{
			if (!shape || scratch.virtualStockSlotLookup.empty())
				return std::numeric_limits<size_t>::max();
			const size_t mask = scratch.virtualStockSlotLookup.size() - 1u;
			size_t slot = HashPointer(shape) & mask;
			for (size_t probe = 0;
				probe < scratch.virtualStockSlotLookup.size(); ++probe)
			{
				const UInt32 stored = scratch.virtualStockSlotLookup[slot];
				if (!stored)
					return std::numeric_limits<size_t>::max();
				const size_t index = static_cast<size_t>(stored - 1u);
				if (index < scratch.virtualStockSlots.size()
					&& scratch.virtualStockSlots[index].shape == shape)
				{
					return index;
				}
				slot = (slot + 1u) & mask;
			}
			return std::numeric_limits<size_t>::max();
		}

		void InsertVirtualStockFrameSlot(SortedPayloadScratch& scratch,
			NiTriShape* shape, size_t entryIndex)
		{
			const size_t mask = scratch.virtualStockSlotLookup.size() - 1u;
			size_t slot = HashPointer(shape) & mask;
			for (size_t probe = 0;
				probe < scratch.virtualStockSlotLookup.size(); ++probe)
			{
				const UInt32 stored = scratch.virtualStockSlotLookup[slot];
				if (!stored)
				{
					scratch.virtualStockSlotLookup[slot] =
						static_cast<UInt32>(entryIndex + 1u);
					return;
				}
				const size_t existing = static_cast<size_t>(stored - 1u);
				if (existing < scratch.virtualStockSlots.size()
					&& scratch.virtualStockSlots[existing].shape == shape)
				{
					return;
				}
				slot = (slot + 1u) & mask;
			}
		}

		void ResolveVirtualStockRegistrationLayout(
			SortedPayloadScratch& scratch, BSShaderAccumulator* accumulator)
		{
			scratch.virtualStockSlots.clear();
			if (!accumulator || accumulator->m_iNumItems <= 0
				|| !accumulator->m_ppkItems
				|| scratch.virtualStockGroups.empty())
			{
				return;
			}

			size_t slotCount = 0;
			for (const std::shared_ptr<VirtualStockShapeGroup>& group
				: scratch.virtualStockGroups)
			{
				if (group)
					slotCount += group->slots.size();
			}
			scratch.virtualStockSlots.reserve(slotCount);
			for (const std::shared_ptr<VirtualStockShapeGroup>& groupOwner
				: scratch.virtualStockGroups)
			{
				if (!groupOwner)
					continue;
				std::lock_guard<std::mutex> lock(groupOwner->mutex);
				for (size_t slot = 0; slot < groupOwner->slots.size(); ++slot)
				{
					scratch.virtualStockSlots.push_back({
						groupOwner->slots[slot].shape,
						groupOwner.get(),
						static_cast<UInt32>(slot),
						-1,
						0
					});
				}
			}
			PrepareLookup(scratch.virtualStockSlotLookup,
				scratch.virtualStockSlots.size());
			for (size_t index = 0;
				index < scratch.virtualStockSlots.size(); ++index)
			{
				if (scratch.virtualStockSlots[index].shape)
				{
					InsertVirtualStockFrameSlot(scratch,
						scratch.virtualStockSlots[index].shape, index);
				}
			}

			for (SInt32 itemIndex = 0;
				itemIndex < accumulator->m_iNumItems; ++itemIndex)
			{
				const size_t slotIndex = LookupVirtualStockFrameSlot(
					scratch, static_cast<NiTriShape*>(
						accumulator->m_ppkItems[itemIndex]));
				if (slotIndex == std::numeric_limits<size_t>::max())
					continue;
				VirtualStockFrameSlot& slot =
					scratch.virtualStockSlots[slotIndex];
				if (!slot.occurrences)
					slot.itemIndex = itemIndex;
				++slot.occurrences;
			}

			size_t frameSlotOffset = 0;
			for (const std::shared_ptr<VirtualStockShapeGroup>& groupOwner
				: scratch.virtualStockGroups)
			{
				if (!groupOwner)
					continue;
				UInt64 resolved = 0;
				UInt64 missing = 0;
				UInt64 duplicate = 0;
				bool orderMismatch = false;
				{
					std::lock_guard<std::mutex> lock(groupOwner->mutex);
					VirtualStockShapeGroup& group = *groupOwner;
					const bool singleton = group.slots.size() == 1;
					std::fill(group.registrationItemIndices.begin(),
						group.registrationItemIndices.end(), -1);
					bool contiguous =
						group.registrationContiguous
						&& group.registrationAccumulator == accumulator
						&& group.registeredSlotCount == group.slots.size()
						&& (singleton
							|| group.registrationItemIndices.size()
								== group.slots.size())
						&& group.primarySlot + 1u == group.slots.size();
					for (size_t slot = 0; slot < group.slots.size(); ++slot)
					{
						const VirtualStockFrameSlot& frameSlot =
							scratch.virtualStockSlots[frameSlotOffset + slot];
						if (frameSlot.group != groupOwner.get()
							|| frameSlot.slotIndex != slot
							|| frameSlot.shape != group.slots[slot].shape)
						{
							contiguous = false;
							orderMismatch = true;
							continue;
						}
						if (frameSlot.occurrences == 1
							&& frameSlot.itemIndex >= 0)
						{
							if (!singleton)
							{
								group.registrationItemIndices[slot] =
									frameSlot.itemIndex;
							}
							++resolved;
						}
						else if (!frameSlot.occurrences)
						{
							++missing;
							contiguous = false;
						}
						else
						{
							duplicate += frameSlot.occurrences - 1u;
							group.duplicateRegistration = true;
							contiguous = false;
						}
					}
					SInt32 previous = -1;
					for (SInt32 slot = singleton
							? -1
							: static_cast<SInt32>(group.primarySlot);
						contiguous && slot >= 0; --slot)
					{
						const SInt32 item =
							group.registrationItemIndices[slot];
						if (item < 0 || item >= accumulator->m_iNumItems
							|| accumulator->m_ppkItems[item]
								!= group.slots[slot].shape
							|| (previous >= 0 && item != previous + 1))
						{
							contiguous = false;
							orderMismatch = true;
							break;
						}
						previous = item;
					}
					group.registrationContiguous = contiguous;
				}
				frameSlotOffset += groupOwner->slots.size();
				if (resolved)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							VirtualStockRegistrationResolved,
						resolved);
				}
				if (missing)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							VirtualStockRegistrationMissing,
						missing);
				}
				if (duplicate)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							VirtualStockRegistrationDuplicate,
						duplicate);
				}
				if (orderMismatch)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							VirtualStockRegistrationOrderMismatch);
				}
			}
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
			payload.stockLikeBitmapPackets = false;
			// Full preflight often refreshes only atlas/resource stamps. Keep
			// the Tile/program dispatch until retained rebuild can compare its
			// geometry, program, and generation identities.
			InvalidateNativeA8TileRetainedText(payload, true);
			std::fill(payload.preflightAtlasTextures.begin(),
				payload.preflightAtlasTextures.end(), nullptr);
			std::fill(payload.packetShaders.begin(),
				payload.packetShaders.end(), nullptr);
			std::fill(payload.packetPrograms.begin(),
				payload.packetPrograms.end(), nullptr);
		}

		bool IsNativePreflightCacheCurrent(const NativeA8ShapePayload& payload,
			UInt32 generation,
			UInt32 atlasTextureEpoch, bool scaledFillSampling,
			bool alphaBlending, const bool* forcedCompositeTopology)
		{
			if (!payload.payloadTemplate)
				return false;
			const NativeA8PayloadTemplate& artifact = *payload.payloadTemplate;
			const bool compositeDesired = forcedCompositeTopology
				? *forcedCompositeTopology
				: g_bEnableFreeTypeFontCompositePass
					&& !artifact.compositePackets.empty()
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
				|| payload.packetShaders.size() != packets.size()
				|| payload.packetPrograms.size() != packets.size())
			{
				return false;
			}
			return true;
		}

		NativeA8FallbackReason PreflightNativeGroupImpl(NiTriShape* facade,
			const A8ShapeMetadata& metadata, NativeA8ShapePayload& payload,
			const NativePreflightFrameContext* frameContext = nullptr,
			const bool* forcedCompositeTopology = nullptr)
		{
			FreeTypePerfScope perf(
				FreeTypePerfPhase::Preflight);
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
			if (forcedCompositeTopology && *forcedCompositeTopology
				&& artifact.compositePackets.empty())
			{
				return NativeA8FallbackReason::PacketBuild;
			}
			if (forcedCompositeTopology && *forcedCompositeTopology
				&& payload.compositeUnavailable
				&& payload.compositeAttemptGeneration == generation)
			{
				return NativeA8FallbackReason::ShaderGeneration;
			}
			if (IsNativePreflightCacheCurrent(payload, generation,
					atlasTextureEpoch, scaledFillSampling, alphaBlending,
					forcedCompositeTopology))
			{
				RecordFreeTypePerf(FreeTypePerfCounter::PreflightFastHit);
				ClearNativePacketFailure(payload);
				return NativeA8FallbackReason::None;
			}

			RecordFreeTypePerf(FreeTypePerfCounter::PreflightFullValidation);
			InvalidateNativePreflight(payload);
			payload.preflightScaledFillSampling = scaledFillSampling;
			payload.preflightAlphaBlending = alphaBlending;
			const bool attemptComposite = forcedCompositeTopology
				? *forcedCompositeTopology
				: g_bEnableFreeTypeFontCompositePass
					&& !artifact.compositePackets.empty()
					&& !(payload.compositeUnavailable
						&& payload.compositeAttemptGeneration == generation);
			payload.useCompositePackets = attemptComposite;
			const std::vector<NativeA8PacketTemplate>* packets =
				&GetNativeA8Packets(artifact, payload.useCompositePackets);
			payload.stockLikeBitmapPackets =
				UsesOnlyStockLikeBitmapPackets(*packets);
			payload.packetShaders.assign(packets->size(), nullptr);
			payload.packetPrograms.assign(packets->size(), nullptr);
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
			if (!shaderSetReady && attemptComposite
				&& !forcedCompositeTopology)
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
				payload.stockLikeBitmapPackets =
					UsesOnlyStockLikeBitmapPackets(*packets);
				payload.packetShaders.assign(packets->size(), nullptr);
				payload.packetPrograms.assign(packets->size(), nullptr);
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
			else if (!shaderSetReady && attemptComposite)
			{
				payload.compositeAttemptGeneration = generation;
				payload.compositeUnavailable = true;
			}
			if (!shaderSetReady)
				return NativeA8FallbackReason::ShaderGeneration;
			if (g_bEnableFreeTypeFontCommandBuffer)
			{
				for (size_t index = 0; index < packets->size(); ++index)
				{
					ResolveNativeA8RetainedPacketProgram(
						(*packets)[index],
						payload.packetShaders[index], generation,
						payload.packetPrograms[index]);
				}
			}
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
			if (g_bEnableFreeTypeFontCommandBuffer)
			{
				BuildNativeA8TileRetainedText(facade, payload,
					generation, atlasTextureEpoch);
			}
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

		bool RegisterVirtualStockShape(BSShaderAccumulator* accumulator,
			NiTriShape* shape, const A8ShapeMetadataPtr& metadata)
		{
			if (!accumulator || !shape || !metadata
				|| metadata->backend
					!= FreeTypeShapeBackend::VirtualStockNative
				|| !metadata->virtualStockGroup)
			{
				return true;
			}
			VirtualStockShapeGroup* group =
				metadata->virtualStockGroup;
			const UInt32 slotIndex = metadata->virtualStockSlot;
			if (slotIndex >= group->slots.size())
			{
				return SuppressNativeGroup(shape, *metadata,
					NativeA8FallbackReason::PacketBuild,
					"virtual-stock-register");
			}
			if (s_sortedPayloadScratch.active
				|| s_sortedPayloadScratch.nestedBypassDepth)
			{
				if (!metadata->virtualStockPrimary)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							VirtualStockFollowerSkipped);
					return true;
				}
				if (!metadata->nativePayload.buildComplete)
				{
					return SuppressNativeGroup(shape, *metadata,
						NativeA8FallbackReason::PacketBuild,
						"virtual-stock-nested-register");
				}
				const NativeA8VisibilityCull visibility =
					EvaluateNativeA8SubmissionVisibility(
						shape, metadata->nativePayload);
				if (visibility != NativeA8VisibilityCull::None)
				{
					RecordNativeA8VisibilityCull(
						visibility, metadata->nativePayload);
					return true;
				}
				RecordFreeTypePerf(
					FreeTypePerfCounter::VirtualStockFacadeFallback);
				return s_originalRegisterObject(accumulator, shape);
			}

			UInt64 registrationCycle = 0;
			if (metadata->virtualStockPrimary)
			{
				if (!metadata->nativePayload.buildComplete)
				{
					return SuppressNativeGroup(shape, *metadata,
						NativeA8FallbackReason::PacketBuild,
						"virtual-stock-register");
				}
				registrationCycle = RecordSortedRegistration(
					accumulator, shape, metadata);
				if (!registrationCycle)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							VirtualStockFacadeFallback);
				}
				{
					std::lock_guard<std::mutex> lock(group->mutex);
					const bool newCycle =
						group->registrationCycle != registrationCycle
						|| group->registrationAccumulator != accumulator;
					if (newCycle)
					{
						group->registrationAccumulator = accumulator;
						group->registrationCycle = registrationCycle;
						group->preflightValidationToken = 0;
						std::fill(
							group->registrationItemIndices.begin(),
							group->registrationItemIndices.end(), -1);
						group->registeredSlotCount = 0;
						group->registrationContiguous =
							registrationCycle != 0;
						group->duplicateRegistration = false;
					}
					else if (group->registeredSlotCount)
					{
						// A second primary in one accumulator cycle means the
						// group was submitted twice. Keep stock fallback atomic
						// rather than trying to publish ambiguous sibling roles.
						group->registrationContiguous = false;
						group->duplicateRegistration = true;
					}
					if (group->frameMode.load(
						std::memory_order_acquire)
						!= VirtualStockFrameMode::Retired)
					{
						group->frameMode.store(
							VirtualStockFrameMode::Facade,
							std::memory_order_release);
					}
					if (group->duplicateRegistration)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::
								VirtualStockFollowerSkipped);
						return true;
					}
				}

				bool dynamicStateSynchronized = true;
				for (const VirtualStockSlotBinding& slot : group->slots)
				{
					if (!slot.shape || slot.shape == shape)
						continue;
					dynamicStateSynchronized =
						SynchronizeFreeTypeStockPageShapeState(
							*shape, *slot.shape)
						&& dynamicStateSynchronized;
				}
				if (!dynamicStateSynchronized)
				{
					std::lock_guard<std::mutex> lock(group->mutex);
					group->registrationContiguous = false;
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							VirtualStockFallbackAtlas);
				}

				const NativeA8VisibilityCull visibility =
					EvaluateNativeA8SubmissionVisibility(
						shape, metadata->nativePayload);
				if (visibility != NativeA8VisibilityCull::None)
				{
					{
						std::lock_guard<std::mutex> lock(group->mutex);
						if (group->registrationCycle == registrationCycle
							&& group->registrationAccumulator == accumulator
							&& group->frameMode.load(
								std::memory_order_acquire)
								!= VirtualStockFrameMode::Retired)
						{
							group->frameMode.store(
								VirtualStockFrameMode::Culled,
								std::memory_order_release);
						}
					}
					RecordNativeA8VisibilityCull(
						visibility, metadata->nativePayload);
					return true;
				}
			}
			else
			{
				std::lock_guard<std::mutex> lock(group->mutex);
				const VirtualStockFrameMode mode =
					group->frameMode.load(std::memory_order_acquire);
				if (mode == VirtualStockFrameMode::Culled
					|| mode == VirtualStockFrameMode::Retired)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							VirtualStockFollowerSkipped);
					return true;
				}
				if (group->duplicateRegistration)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							VirtualStockFollowerSkipped);
					return true;
				}
				if (!group->registrationContiguous)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							VirtualStockFollowerSkipped);
					return true;
				}
				registrationCycle = group->registrationCycle;
				if (!registrationCycle
					|| group->registrationAccumulator != accumulator
					|| !group->registeredSlotCount)
				{
					group->registrationContiguous = false;
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							VirtualStockFollowerSkipped);
					return true;
				}
			}

			const bool result =
				s_originalRegisterObject(accumulator, shape);
			if (!result)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::
						VirtualStockRegistrationRejected);
			}

			{
				std::lock_guard<std::mutex> lock(group->mutex);
				if (group->registrationCycle != registrationCycle
					|| group->registrationAccumulator != accumulator
					|| group->frameMode.load(std::memory_order_acquire)
						== VirtualStockFrameMode::Retired)
				{
					group->registrationContiguous = false;
					return result;
				}
				const UInt32 ordinal = group->registeredSlotCount;
				const bool expectedSlotValid =
					ordinal <= group->primarySlot;
				const UInt32 expectedSlot = expectedSlotValid
					? group->primarySlot - ordinal : 0;
				if (!result || !expectedSlotValid
					|| slotIndex != expectedSlot)
				{
					group->registrationContiguous = false;
				}
				++group->registeredSlotCount;
			}
			return result;
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
			if (metadata && metadata->backend
				== FreeTypeShapeBackend::VirtualStockNative)
			{
				if (!IsA8TileRenderPassHookCurrent())
				{
					return SuppressNativeGroup(facade, *metadata,
						NativeA8FallbackReason::TileRouteConflict,
						"virtual-stock-register");
				}
				return RegisterVirtualStockShape(
					accumulator, facade, metadata);
			}
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
				// Its draws may also change sampler and private c176-c183 state
				// outside the outer traversal, so neither side may inherit the
				// other's sorted cache.
				const bool restoreActive = scratch.active;
				scratch.active = false;
				++scratch.nestedBypassDepth;
				if (++scratch.nestedTraversalSerial == 0)
					++scratch.nestedTraversalSerial;
				RecordNativeA8CommandFallback(
					NativeA8CommandFallback::Nested);
				InvalidateNativeA8CommandExecutionSegment(
					NativeA8CommandFallback::Nested);
				InvalidateNativeA8SortedShaderState();
				const int result = state.originalSortedTileRender(accumulator);
				InvalidateNativeA8SortedShaderState();
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
				scratch.fallbackMetadataOwners.clear();
				scratch.payloadTemplates.clear();
				scratch.virtualStockGroups.clear();
				const bool haveRegisteredMetadata =
					scratch.pendingAccumulator == accumulator;
				if (!haveRegisteredMetadata)
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
				UInt64 frameValidationToken =
					++scratch.nextValidationToken;
				if (!frameValidationToken)
					frameValidationToken =
						++scratch.nextValidationToken;
				const bool onlyVirtualStockRegistrations =
					haveRegisteredMetadata
					&& !scratch.pendingRegistrations.empty()
					&& std::all_of(
						scratch.pendingRegistrations.begin(),
						scratch.pendingRegistrations.end(),
						[](const RegisteredFacade& registration)
						{
							return registration.metadata
								&& registration.metadata->backend
									== FreeTypeShapeBackend::
										VirtualStockNative
								&& registration.metadata->
									virtualStockPrimary;
						});
				if (haveRegisteredMetadata
					&& !onlyVirtualStockRegistrations)
				{
					PrepareRegistrationLookup(scratch);
				}
				scratch.frameCandidates.clear();
				if (!onlyVirtualStockRegistrations)
				{
					for (SInt32 index = accumulator->m_iNumItems - 1;
						index >= 0; --index)
					{
						NiGeometry* geometry =
							accumulator->m_ppkItems[index];
						if (IsFreeTypeFacade(geometry))
						{
							scratch.frameCandidates.push_back(
								static_cast<NiTriShape*>(geometry));
						}
					}
				}
				const size_t trackedCount = onlyVirtualStockRegistrations
					? scratch.pendingRegistrations.size()
					: scratch.frameCandidates.size();
				if (!onlyVirtualStockRegistrations)
					scratch.frameEntries.reserve(trackedCount);
				scratch.payloadTemplates.reserve(trackedCount);
				scratch.virtualStockGroups.reserve(
					std::min<size_t>(trackedCount, 1024));
				if (onlyVirtualStockRegistrations)
				{
					scratch.facadeLookup.clear();
					PrepareLookup(scratch.payloadLookup,
						scratch.pendingRegistrations.size());
				}
				else
				{
					PrepareLookup(scratch.facadeLookup, trackedCount);
					PrepareLookup(scratch.payloadLookup, trackedCount);
				}
				if (onlyVirtualStockRegistrations)
				{
					for (const RegisteredFacade& registration
						: scratch.pendingRegistrations)
					{
						VirtualStockShapeGroup* group =
							registration.metadata->virtualStockGroup;
						if (!group)
							continue;
						const VirtualStockFrameMode mode =
							group->frameMode.load(
								std::memory_order_acquire);
						if (mode == VirtualStockFrameMode::Culled
							|| mode == VirtualStockFrameMode::Retired)
						{
							continue;
						}
						{
							std::lock_guard<std::mutex> lock(group->mutex);
							if (group->preflightValidationToken
								== frameValidationToken)
							{
								continue;
							}
						}
						std::shared_ptr<VirtualStockShapeGroup> groupOwner =
							AcquireVirtualStockShapeGroup(
								*registration.metadata);
						if (!groupOwner)
							continue;
						NativeA8ShapePayload& payload =
							registration.metadata->nativePayload;
						const NativeA8FallbackReason preflight =
							PreflightNativeGroupImpl(
								registration.facade,
								*registration.metadata, payload,
								&preflightContext,
								&groupOwner->useCompositeTopology);
						if (preflight != NativeA8FallbackReason::None)
						{
							RestoreVirtualStockGroupToFacade(
								groupOwner, preflight);
							continue;
						}
						{
							std::lock_guard<std::mutex> lock(
								groupOwner->mutex);
							groupOwner->preflightValidationToken =
								frameValidationToken;
						}
						scratch.virtualStockGroups.push_back(
							std::move(groupOwner));
						if (payload.payloadTemplate
							&& payload.preparedGeneration == generation
							&& InsertUniquePayload(
								scratch, payload.payloadTemplate))
						{
							RecordFreeTypePerf(
								FreeTypePerfCounter::
									SortedFramePayload);
						}
					}
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							VirtualStockSortedPreflightSaved,
						static_cast<UInt64>(itemCount));
				}
				for (NiTriShape* facade : scratch.frameCandidates)
				{
					if (LookupSortedFacade(scratch, facade)
						!= std::numeric_limits<size_t>::max())
					{
						continue;
					}

					SortedFrameEntry entry;
					std::shared_ptr<VirtualStockShapeGroup>
						virtualStockGroup;
					entry.facade = facade;
					const A8ShapeMetadata* registeredMetadata =
						haveRegisteredMetadata
							? FindRegisteredMetadata(scratch, facade) : nullptr;
					entry.metadata = registeredMetadata;
					if (!entry.metadata)
					{
						A8ShapeMetadataPtr fallbackOwner =
							FindA8ShapeMetadata(facade);
						entry.metadata = fallbackOwner.get();
						if (fallbackOwner)
						{
							scratch.fallbackMetadataOwners.push_back(
								std::move(fallbackOwner));
						}
					}
					if (entry.metadata
						&& entry.metadata->backend
							== FreeTypeShapeBackend::VirtualStockNative)
					{
						if (!entry.metadata->virtualStockPrimary)
							continue;
						virtualStockGroup =
							AcquireVirtualStockShapeGroup(
								*entry.metadata);
					}
					entry.generation = generation;
					if (entry.metadata
						&& entry.metadata->nativePayload.buildComplete)
					{
						entry.payload = &entry.metadata->nativePayload;
						entry.visibilityCull =
							entry.metadata->backend
								== FreeTypeShapeBackend::
									VirtualStockNative
							? NativeA8VisibilityCull::None
							: EvaluateNativeA8SubmissionVisibility(
								facade, *entry.payload);
						if (entry.visibilityCull
							!= NativeA8VisibilityCull::None)
						{
							RecordFreeTypePerf(FreeTypePerfCounter::
								VisibilityPreflightSkipped);
						}
						else
						{
							entry.preflightResult = PreflightNativeGroupImpl(
								facade, *entry.metadata, *entry.payload,
								&preflightContext,
								entry.metadata->backend
										== FreeTypeShapeBackend::
											VirtualStockNative
									&& entry.metadata->virtualStockGroup
									? &entry.metadata->virtualStockGroup->
										useCompositeTopology
									: nullptr);
							if (entry.preflightResult
								== NativeA8FallbackReason::None)
							{
								entry.generation =
									entry.payload->preparedGeneration;
								entry.validationToken =
									frameValidationToken;
								if (virtualStockGroup)
								{
									{
										std::lock_guard<std::mutex> lock(
											virtualStockGroup->mutex);
										virtualStockGroup->
											preflightValidationToken =
												frameValidationToken;
									}
									scratch.virtualStockGroups.push_back(
										virtualStockGroup);
								}
							}
							else if (virtualStockGroup)
							{
								RestoreVirtualStockGroupToFacade(
									virtualStockGroup,
									entry.preflightResult);
							}
						}
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
					}
				}
				ResolveVirtualStockRegistrationLayout(scratch, accumulator);
				PrepareSortedNativeA8Payloads(
					scratch.payloadTemplates, generation);
				for (const std::shared_ptr<VirtualStockShapeGroup>& group
					: scratch.virtualStockGroups)
				{
					if (!group || !group->primaryShape)
						continue;
					bool preflightValidated = false;
					{
						std::lock_guard<std::mutex> lock(group->mutex);
						preflightValidated =
							group->preflightValidationToken
								== frameValidationToken;
					}
					if (!preflightValidated)
					{
						RestoreVirtualStockGroupToFacade(group,
							NativeA8FallbackReason::RuntimeFault);
						continue;
					}

					PrepareVirtualStockGroupForSortedFrame(group,
						generation, preflightContext.atlasTextureEpoch,
						frameValidationToken);
				}
				if (g_bEnableFreeTypeFontCommandBuffer)
				{
					FreeTypePerfScope commandBuild(
						FreeTypePerfPhase::CommandBuild);
					{
						FreeTypePerfScope commandBuildStamp(
							FreeTypePerfPhase::CommandBuildStamp);
						BeginNativeA8FrameCommandBuffer(accumulator,
							frameValidationToken, generation,
							preflightContext.atlasTextureEpoch);
						const size_t ordinaryCapacityHint =
							scratch.frameEntries.size()
								>= scratch.virtualStockSlots.size()
							? scratch.frameEntries.size()
								- scratch.virtualStockSlots.size()
							: 0;
						ReserveNativeA8FrameCommandBuffer(
							ordinaryCapacityHint,
							scratch.virtualStockGroups.size());
					}
					{
						FreeTypePerfScope commandBuildVirtual(
							FreeTypePerfPhase::CommandBuildVirtual);
						for (const std::shared_ptr<
							VirtualStockShapeGroup>& group
							: scratch.virtualStockGroups)
						{
							if (!group)
								continue;
							AddNativeA8FrameCommandSpan(
								nullptr, nullptr, nullptr,
								group.get());
						}
					}
					{
						FreeTypePerfScope commandBuildOrdinary(
							FreeTypePerfPhase::CommandBuildOrdinary);
						for (SortedFrameEntry& entry
							: scratch.frameEntries)
						{
							if (!entry.metadata || !entry.payload
								|| entry.preflightResult
									!= NativeA8FallbackReason::None
								|| entry.visibilityCull
									!= NativeA8VisibilityCull::None)
							{
								continue;
							}
							if (entry.metadata->backend
								== FreeTypeShapeBackend::
									VirtualStockNative)
							{
								VirtualStockShapeGroup* group =
									entry.metadata->virtualStockGroup;
								if (group
									&& group->
										commandValidationToken.load(
											std::memory_order_acquire)
										== frameValidationToken)
								{
									entry.commandSpanIndex =
										group->commandSpanIndex.load(
											std::memory_order_acquire);
								}
							}
							else
							{
								entry.singlePacketCommandIndex =
									AddNativeA8FrameSinglePacketCommand(
										entry.facade, entry.metadata,
										entry.payload);
								if (entry.singlePacketCommandIndex
									== kInvalidNativeA8CommandIndex)
								{
									entry.commandSpanIndex =
										AddNativeA8FrameCommandSpan(
											entry.facade,
											entry.metadata,
											entry.payload);
								}
							}
						}
					}
					{
						FreeTypePerfScope commandBuildFinalize(
							FreeTypePerfPhase::CommandBuildFinalize);
						ActivateNativeA8FrameCommandBuffer();
					}
				}
				else
				{
					EndNativeA8FrameCommandBuffer();
				}
				RefreshSortedScratchMemory(scratch);
				BeginNativeA8SortedShaderBatch();
				BeginA8SortedTileConstantOwnership();
				scratch.activeValidationToken = frameValidationToken;
				scratch.active = true;
				const int result = state.originalSortedTileRender(accumulator);
				EndA8SortedTileConstantOwnership();
				EndNativeA8SortedShaderBatch();
				EndNativeA8FrameCommandBuffer();
				EndNativeA8SortedRingFrame();
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
				if (state.sortedTileRenderHookInstalled)
				{
					state.sortedTileRenderHookInstalled = false;
					InvalidateAllVirtualStockBindings();
				}
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
				InvalidateAllVirtualStockBindings();
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
		view.metadata = entry.metadata;
		view.payload = entry.payload;
		view.preflightResult = entry.preflightResult;
		view.visibilityCull = entry.visibilityCull;
		view.generation = entry.generation;
		view.validationToken = entry.validationToken;
		view.commandSpanIndex = entry.commandSpanIndex;
		view.singlePacketCommandIndex =
			entry.singlePacketCommandIndex;
		RecordFreeTypePerf(FreeTypePerfCounter::SortedFrameLookupHit);
		return true;
	}

	UInt64 GetNativeA8SortedFrameValidationToken()
	{
		const SortedPayloadScratch& scratch = s_sortedPayloadScratch;
		return scratch.active ? scratch.activeValidationToken : 0;
	}

	UInt64 GetNativeA8SortedNestedTraversalSerial()
	{
		return s_sortedPayloadScratch.nestedTraversalSerial;
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
				InvalidateAllVirtualStockBindings();
				NotifyNativeA8CommandExternalMutation(
					NativeA8CommandFallback::Atlas);
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

	bool IsNativeA8SortedTraversalHookCurrent()
	{
		return State().originalSortedTileRender
			&& ReadSortedTileRenderCallTarget()
				== reinterpret_cast<SortedTileRenderFn>(
					&NativeA8RenderSorted);
	}
}

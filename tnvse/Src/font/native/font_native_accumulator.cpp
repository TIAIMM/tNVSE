#include "font_a8_internal.h"
#include "font_native_internal.h"
#include "hook_identity.h"
#include "load_config.h"
#include "tnvse.h"

#include "BSShaderManager.hpp"
#include "NiDX9TextureData.hpp"
#include "NiMemory.hpp"
#include "Utils/SafeWrite.h"

#include <algorithm>
#include <atomic>
#include <cmath>
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
		// BSShaderAccumulator::RegisterObject (0xB63F10) performs the common
		// geometry checks and then dispatches through
		// pRegisterObjectFunc[eRenderMode].  BSShaderManager::Initialize writes
		// the Tile/Interface callback at 0x11F9F80[10] (instruction 0xB576D6).
		// Hook that narrow callback instead of the global RegisterObject vtable so
		// scene, water, shadow, and post-process accumulators never enter tNVSE.
		inline constexpr UInt32 kRegisterObjectFunctionTable = 0x11F9F80;
		inline constexpr UInt32 kTileRegisterObjectFunctionEntry =
			kRegisterObjectFunctionTable
			+ static_cast<UInt32>(BSShaderManager::BSSM_RENDER_TILES)
				* sizeof(void*);
		inline constexpr UInt32 kStockTileRegisterObject = 0xB65A90;
		inline constexpr UInt32 kMaximumMissingMetadataLogs = 8;
		inline constexpr size_t kMaximumStableTieItems = 8192;

		using TileRegisterObjectFn = bool(__cdecl*)(BSShaderAccumulator*,
			NiGeometry*, const NiPropertyState*, BSShaderProperty*, BSShader*);
		using AccumulatorSortFn = void(__thiscall*)(BSShaderAccumulator*);
		static_assert(kTileRegisterObjectFunctionEntry == 0x11F9FA8);
		static_assert(sizeof(TileRegisterObjectFn) == sizeof(UInt32));
		static_assert(sizeof(AccumulatorSortFn) == sizeof(UInt32));
		static_assert(kTileSortDispatchPatchSize == 9);

		std::atomic<TileRegisterObjectFn> s_originalTileRegisterObject = nullptr;
		bool s_loggedTileRegisterObjectConflict = false;
		bool s_loggedTileRegisterObjectSlotUnavailable = false;
		std::atomic<UInt32> s_missingMetadataLogCount = 0;
		std::atomic<UInt32> s_atlasTextureEpoch = 1;

		bool __cdecl NativeA8RegisterObject(BSShaderAccumulator* accumulator,
			NiGeometry* geometry, const NiPropertyState* properties,
			BSShaderProperty* shaderProperty, BSShader* shader);
		bool IsTileRegisterObjectSlotWritable();
		void __fastcall NativeA8RenderAlphaGeometry(
			BSShaderAccumulator* accumulator, void*);
		void __fastcall NativeA8SortAlphaGeometry(
			BSShaderAccumulator* accumulator, AccumulatorSortFn originalSort);

		struct RegisteredFacade
		{
			NiTriShape* facade = nullptr;
			A8ShapeMetadataPtr metadata;
			// Exact AddTail ordinal committed only after the predecessor actually
			// appends this facade to m_kItems.  The sort anchor never infers an
			// ordinal from an address, so repeated geometry remains unambiguous.
			UInt32 acceptedOrdinal = kInvalidNativeA8CommandIndex;
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
			UInt32 crossTextSequenceIndex =
				kInvalidNativeA8CommandIndex;
			UInt32 crossTextOccurrences = 0;
		};

		struct VirtualStockFrameSlot
		{
			NiTriShape* shape = nullptr;
			VirtualStockShapeGroup* group = nullptr;
			UInt32 slotIndex = 0;
			SInt32 itemIndex = -1;
			UInt32 occurrences = 0;
		};

		struct StableTileTieItem
		{
			NiGeometry* geometry = nullptr;
			float depth = 0.0f;
			UInt32 registrationOrdinal = 0;
			UInt32 registrationBlockOrdinal = 0;
		};

		struct OriginalOrderAnchorRun
		{
			UInt32 begin = 0;
			UInt32 end = 0;
			UInt32 write = 0;
			bool mixed = false;
			bool changed = false;
		};

		enum class OriginalOrderAnchorFailure : UInt8
		{
			None,
			Gate,
			Count,
			Storage,
			Source,
			Depth,
			Metadata,
			Registration,
			Group,
			Singleton,
			Coverage,
			Apply
		};

		enum class OriginalOrderSortOutcome : UInt8
		{
			MustCallPredecessor,
			Anchored,
			OrdinalSidecarRecovered,
			StockEquivalentLegacy
		};

		struct OriginalOrderSortAttempt
		{
			OriginalOrderSortOutcome outcome =
				OriginalOrderSortOutcome::MustCallPredecessor;
			OriginalOrderAnchorFailure failure =
				OriginalOrderAnchorFailure::Gate;
		};

		struct NativePreflightFrameContext
		{
			UInt32 generation = 0;
			UInt32 atlasTextureEpoch = 0;
			bool accumulatorCurrent = false;
			bool immediateRouteCurrent = false;
			bool rendererAvailable = false;
		};

		struct SortedPayloadScratch
		{
			BSShaderAccumulator* pendingAccumulator = nullptr;
			std::vector<RegisteredFacade> pendingRegistrations;
			// Sort() copies its source NiSortedObjectList into m_ppkItems without
			// consuming the list. Snapshot that non-owning AddTail order only when
			// a mixed equal-depth FreeType repair may be needed.
			std::vector<NiGeometry*> acceptedTileRegistrations;
			std::vector<UInt32> acceptedRegistrationLookup;
			std::vector<UInt32> sortedRegistrationOrdinals;
			std::vector<UInt32> sortedItemIndicesByRegistrationOrdinal;
			std::vector<UInt32> registrationBlockOrdinals;
			std::vector<StableTileTieItem> stableTieItems;
			std::vector<UInt8> originalOrderFreeTypeFlags;
			std::vector<UInt32> originalOrderDesiredOrdinals;
			std::vector<UInt32> originalOrderRunIds;
			std::vector<OriginalOrderAnchorRun> originalOrderRuns;
			std::vector<NiGeometry*> originalOrderOutput;
			std::vector<UInt32> registrationLookup;
			std::vector<NiTriShape*> frameCandidates;
			std::vector<SortedFrameEntry> frameEntries;
			std::vector<UInt32> facadeLookup;
			std::vector<A8ShapeMetadataPtr> fallbackMetadataOwners;
			std::vector<NativeA8PayloadTemplatePtr> payloadTemplates;
			std::vector<UInt32> payloadLookup;
			std::vector<std::shared_ptr<VirtualStockShapeGroup>>
				virtualStockGroups;
			std::vector<const A8ShapeMetadata*> virtualStockSingletons;
			std::vector<VirtualStockFrameSlot> virtualStockSlots;
			std::vector<UInt32> virtualStockSlotLookup;
			CpuMemoryLease cpuMemory;
			UInt32 nestedBypassDepth = 0;
			UInt64 nestedTraversalSerial = 1;
			UInt64 registrationCycle = 1;
			UInt64 nextValidationToken = 0;
			UInt64 activeValidationToken = 0;
			UInt32 acceptedOriginalOrderAnchorCount = 0;
			BSShaderAccumulator* originalOrderAnchorAccumulator = nullptr;
			UInt64 originalOrderAnchorCycle = 0;
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
				+ scratch.acceptedTileRegistrations.capacity()
					* sizeof(NiGeometry*)
				+ scratch.acceptedRegistrationLookup.capacity()
					* sizeof(UInt32)
				+ scratch.sortedRegistrationOrdinals.capacity()
					* sizeof(UInt32)
				+ scratch.sortedItemIndicesByRegistrationOrdinal.capacity()
					* sizeof(UInt32)
				+ scratch.registrationBlockOrdinals.capacity()
					* sizeof(UInt32)
				+ scratch.stableTieItems.capacity()
					* sizeof(StableTileTieItem)
				+ scratch.originalOrderFreeTypeFlags.capacity()
					* sizeof(UInt8)
				+ scratch.originalOrderDesiredOrdinals.capacity()
					* sizeof(UInt32)
				+ scratch.originalOrderRunIds.capacity()
					* sizeof(UInt32)
				+ scratch.originalOrderRuns.capacity()
					* sizeof(OriginalOrderAnchorRun)
				+ scratch.originalOrderOutput.capacity()
					* sizeof(NiGeometry*)
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
				+ scratch.virtualStockSingletons.capacity()
					* sizeof(const A8ShapeMetadata*)
				+ scratch.virtualStockSlots.capacity()
					* sizeof(VirtualStockFrameSlot)
				+ scratch.virtualStockSlotLookup.capacity() * sizeof(UInt32);
			scratch.cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata, bytes);
		}

		void ClearSortedFrame(SortedPayloadScratch& scratch)
		{
			if (g_bEnableFreeTypeFontCrossTextBatch)
				EndNativeA8CrossTextBatchFrame();
			EndNativeA8FrameCommandBuffer();
			scratch.active = false;
			scratch.activeValidationToken = 0;
			scratch.frameCandidates.clear();
			scratch.frameEntries.clear();
			scratch.fallbackMetadataOwners.clear();
			scratch.payloadTemplates.clear();
			scratch.virtualStockGroups.clear();
			scratch.virtualStockSingletons.clear();
			scratch.virtualStockSlots.clear();
			scratch.pendingAccumulator = nullptr;
			scratch.pendingRegistrations.clear();
			scratch.acceptedTileRegistrations.clear();
			scratch.acceptedOriginalOrderAnchorCount = 0;
			scratch.originalOrderAnchorAccumulator = nullptr;
			scratch.originalOrderAnchorCycle = 0;
			if (++scratch.registrationCycle == 0)
				++scratch.registrationCycle;
			if (scratch.pendingRegistrations.capacity() > 8192)
			{
				std::vector<RegisteredFacade>().swap(
					scratch.pendingRegistrations);
			}
			if (scratch.acceptedTileRegistrations.capacity() > 8192)
			{
				std::vector<NiGeometry*>().swap(
					scratch.acceptedTileRegistrations);
			}
			if (scratch.acceptedRegistrationLookup.capacity() > 16384)
			{
				std::vector<UInt32>().swap(
					scratch.acceptedRegistrationLookup);
			}
			if (scratch.sortedRegistrationOrdinals.capacity() > 8192)
			{
				std::vector<UInt32>().swap(
					scratch.sortedRegistrationOrdinals);
			}
			if (scratch.sortedItemIndicesByRegistrationOrdinal.capacity() > 8192)
			{
				std::vector<UInt32>().swap(
					scratch.sortedItemIndicesByRegistrationOrdinal);
			}
			if (scratch.registrationBlockOrdinals.capacity() > 8192)
			{
				std::vector<UInt32>().swap(
					scratch.registrationBlockOrdinals);
			}
			if (scratch.stableTieItems.capacity() > 8192)
			{
				std::vector<StableTileTieItem>().swap(
					scratch.stableTieItems);
			}
			if (scratch.originalOrderFreeTypeFlags.capacity() > 8192)
			{
				std::vector<UInt8>().swap(
					scratch.originalOrderFreeTypeFlags);
			}
			if (scratch.originalOrderDesiredOrdinals.capacity() > 8192)
			{
				std::vector<UInt32>().swap(
					scratch.originalOrderDesiredOrdinals);
			}
			if (scratch.originalOrderRunIds.capacity() > 8192)
			{
				std::vector<UInt32>().swap(
					scratch.originalOrderRunIds);
			}
			if (scratch.originalOrderRuns.capacity() > 8192)
			{
				std::vector<OriginalOrderAnchorRun>().swap(
					scratch.originalOrderRuns);
			}
			if (scratch.originalOrderOutput.capacity() > 8192)
			{
				std::vector<NiGeometry*>().swap(
					scratch.originalOrderOutput);
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
			if (scratch.virtualStockSingletons.capacity() > 8192)
			{
				std::vector<const A8ShapeMetadata*>().swap(
					scratch.virtualStockSingletons);
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
				std::vector<NiGeometry*>().swap(
					scratch.acceptedTileRegistrations);
				std::vector<UInt32>().swap(
					scratch.acceptedRegistrationLookup);
				std::vector<UInt32>().swap(
					scratch.sortedRegistrationOrdinals);
				std::vector<UInt32>().swap(
					scratch.sortedItemIndicesByRegistrationOrdinal);
				std::vector<UInt32>().swap(
					scratch.registrationBlockOrdinals);
				std::vector<StableTileTieItem>().swap(
					scratch.stableTieItems);
				std::vector<UInt8>().swap(
					scratch.originalOrderFreeTypeFlags);
				std::vector<UInt32>().swap(
					scratch.originalOrderDesiredOrdinals);
				std::vector<UInt32>().swap(
					scratch.originalOrderRunIds);
				std::vector<OriginalOrderAnchorRun>().swap(
					scratch.originalOrderRuns);
				std::vector<NiGeometry*>().swap(
					scratch.originalOrderOutput);
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
				std::vector<const A8ShapeMetadata*>().swap(
					scratch.virtualStockSingletons);
				std::vector<VirtualStockFrameSlot>().swap(
					scratch.virtualStockSlots);
				std::vector<UInt32>().swap(
					scratch.virtualStockSlotLookup);
				scratch.cpuMemory.Release();
			}
		}

		RenderAlphaGeometryFn ReadRenderAlphaGeometryCallTarget()
		{
			SIZE_T target = 0;
			if (!hook_identity::ReadRel32Target(
				kRenderAlphaGeometryCallSite,
				hook_identity::Rel32Opcode::Call,
				target))
			{
				return nullptr;
			}
			return reinterpret_cast<RenderAlphaGeometryFn>(target);
		}

		bool IsTileSortAnchorHookCurrent()
		{
			SIZE_T target = 0;
			if (!hook_identity::ReadRel32Target(
				kTileSortDispatchPatch,
				hook_identity::Rel32Opcode::Call,
				target)
				|| target != reinterpret_cast<SIZE_T>(
					&NativeA8SortAlphaGeometry))
			{
				return false;
			}
			static constexpr UInt8 kTail[] = { 0x90, 0x90, 0x90, 0x90 };
			return hook_identity::IsAccessibleRegion(
				kTileSortDispatchPatch + 5u, sizeof(kTail), true)
				&& std::memcmp(reinterpret_cast<const void*>(
					kTileSortDispatchPatch + 5u), kTail, sizeof(kTail)) == 0;
		}

		TileRegisterObjectFn ReadTileRegisterObjectTarget()
		{
			if (!IsTileRegisterObjectSlotWritable())
				return nullptr;
			return *reinterpret_cast<TileRegisterObjectFn volatile*>(
				kTileRegisterObjectFunctionEntry);
		}

		bool IsTileRegisterObjectSlotWritable()
		{
			static const bool writable = []()
			{
				MEMORY_BASIC_INFORMATION region = {};
				if (VirtualQuery(reinterpret_cast<const void*>(
					kTileRegisterObjectFunctionEntry), &region, sizeof(region))
					!= sizeof(region)
					|| region.State != MEM_COMMIT
					|| (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
				{
					return false;
				}
				const DWORD protection = region.Protect & 0xFFu;
				return protection == PAGE_READWRITE
					|| protection == PAGE_WRITECOPY
					|| protection == PAGE_EXECUTE_READWRITE
					|| protection == PAGE_EXECUTE_WRITECOPY;
			}();
			return writable;
		}

		bool PublishTileRegisterObjectHook(TileRegisterObjectFn expected)
		{
			if (!expected || !IsTileRegisterObjectSlotWritable())
				return false;
			const UInt32 expectedBits = reinterpret_cast<UInt32>(expected);
			const UInt32 hookBits = reinterpret_cast<UInt32>(
				&NativeA8RegisterObject);
			const LONG observed = InterlockedCompareExchange(
				reinterpret_cast<volatile LONG*>(
					kTileRegisterObjectFunctionEntry),
				static_cast<LONG>(hookBits),
				static_cast<LONG>(expectedBits));
			return static_cast<UInt32>(observed) == expectedBits;
		}

		__forceinline bool ForwardTileRegisterObject(
			TileRegisterObjectFn original, BSShaderAccumulator* accumulator,
			NiGeometry* geometry,
			const NiPropertyState* properties,
			BSShaderProperty* shaderProperty, BSShader* shader)
		{
			return original && original(accumulator, geometry, properties,
				shaderProperty, shader);
		}

		void ClearPendingRegistrations(SortedPayloadScratch& scratch)
		{
			scratch.pendingAccumulator = nullptr;
			scratch.pendingRegistrations.clear();
			scratch.acceptedTileRegistrations.clear();
			scratch.acceptedOriginalOrderAnchorCount = 0;
			scratch.originalOrderAnchorAccumulator = nullptr;
			scratch.originalOrderAnchorCycle = 0;
		}

		bool EnsurePendingAccumulator(BSShaderAccumulator* accumulator)
		{
			SortedPayloadScratch& scratch = s_sortedPayloadScratch;
			const bool hookCurrent = ReadRenderAlphaGeometryCallTarget()
				== reinterpret_cast<RenderAlphaGeometryFn>(
					&NativeA8RenderAlphaGeometry);
			if (!accumulator || scratch.active || scratch.nestedBypassDepth
				|| !hookCurrent)
			{
				if (!scratch.active && !scratch.nestedBypassDepth
					&& !hookCurrent)
				{
					ClearPendingRegistrations(scratch);
				}
				return false;
			}
			if (scratch.pendingAccumulator != accumulator)
			{
				ClearPendingRegistrations(scratch);
				scratch.pendingAccumulator = accumulator;
				if (++scratch.registrationCycle == 0)
					++scratch.registrationCycle;
			}
			return true;
		}

		UInt64 RecordSortedRegistration(BSShaderAccumulator* accumulator,
			NiTriShape* facade, const A8ShapeMetadataPtr& metadata)
		{
			SortedPayloadScratch& scratch = s_sortedPayloadScratch;
			if (!facade || !metadata || !EnsurePendingAccumulator(accumulator))
				return 0;
			scratch.pendingRegistrations.push_back({ facade, metadata });
			return scratch.registrationCycle;
		}

		void CommitSortedRegistration(BSShaderAccumulator* accumulator,
			NiTriShape* facade, UInt64 registrationCycle, UInt32 sizeBefore,
			bool forwarded)
		{
			SortedPayloadScratch& scratch = s_sortedPayloadScratch;
			if (!accumulator || !facade
				|| !registrationCycle
				|| scratch.registrationCycle != registrationCycle
				|| scratch.pendingAccumulator != accumulator
				|| scratch.pendingRegistrations.empty())
			{
				return;
			}
			RegisteredFacade& registration =
				scratch.pendingRegistrations.back();
			if (registration.facade != facade
				|| registration.acceptedOrdinal
					!= kInvalidNativeA8CommandIndex)
			{
				return;
			}
			const UInt32 sizeAfter = accumulator->m_kItems.GetSize();
			if (forwarded && sizeBefore != std::numeric_limits<UInt32>::max()
				&& sizeAfter == sizeBefore + 1u
				&& accumulator->m_kItems.GetTailPos()
				&& accumulator->m_kItems.GetTail() == facade)
			{
				registration.acceptedOrdinal = sizeBefore;
				++scratch.acceptedOriginalOrderAnchorCount;
			}
		}

		void DiscardUnacceptedSortedRegistration(
			BSShaderAccumulator* accumulator, NiTriShape* facade,
			UInt64 registrationCycle)
		{
			SortedPayloadScratch& scratch = s_sortedPayloadScratch;
			if (!accumulator || !facade || !registrationCycle
				|| scratch.registrationCycle != registrationCycle
				|| scratch.pendingAccumulator != accumulator
				|| scratch.pendingRegistrations.empty())
			{
				return;
			}
			const RegisteredFacade& registration =
				scratch.pendingRegistrations.back();
			if (registration.facade == facade
				&& registration.acceptedOrdinal
					== kInvalidNativeA8CommandIndex)
			{
				scratch.pendingRegistrations.pop_back();
			}
		}

		bool IsFreeTypeFacade(const NiGeometry* geometry);

		void RecordOriginalOrderAnchorFailure(
			OriginalOrderAnchorFailure failure)
		{
			FreeTypePerfCounter counter;
			switch (failure)
			{
			case OriginalOrderAnchorFailure::Gate:
				counter = FreeTypePerfCounter::
					SortedOriginalOrderAnchorFailGate;
				break;
			case OriginalOrderAnchorFailure::Count:
				counter = FreeTypePerfCounter::
					SortedOriginalOrderAnchorFailCount;
				break;
			case OriginalOrderAnchorFailure::Storage:
				counter = FreeTypePerfCounter::
					SortedOriginalOrderAnchorFailStorage;
				break;
			case OriginalOrderAnchorFailure::Source:
				counter = FreeTypePerfCounter::
					SortedOriginalOrderAnchorFailSource;
				break;
			case OriginalOrderAnchorFailure::Depth:
				counter = FreeTypePerfCounter::
					SortedOriginalOrderAnchorFailDepth;
				break;
			case OriginalOrderAnchorFailure::Metadata:
				counter = FreeTypePerfCounter::
					SortedOriginalOrderAnchorFailMetadata;
				break;
			case OriginalOrderAnchorFailure::Registration:
				counter = FreeTypePerfCounter::
					SortedOriginalOrderAnchorFailRegistration;
				break;
			case OriginalOrderAnchorFailure::Group:
				counter = FreeTypePerfCounter::
					SortedOriginalOrderAnchorFailGroup;
				break;
			case OriginalOrderAnchorFailure::Singleton:
				counter = FreeTypePerfCounter::
					SortedOriginalOrderAnchorFailSingleton;
				break;
			case OriginalOrderAnchorFailure::Coverage:
				counter = FreeTypePerfCounter::
					SortedOriginalOrderAnchorFailCoverage;
				break;
			case OriginalOrderAnchorFailure::Apply:
				counter = FreeTypePerfCounter::
					SortedOriginalOrderAnchorFailApply;
				break;
			default:
				return;
			}
			RecordFreeTypePerf(counter);
		}

		float ChooseOriginalDepthPivot(const BSShaderAccumulator& accumulator,
			SInt32 left, SInt32 right)
		{
			const SInt32 middle = (left + right) >> 1;
			const float leftDepth = accumulator.m_pfDepths[left];
			const float middleDepth = accumulator.m_pfDepths[middle];
			const float rightDepth = accumulator.m_pfDepths[right];
			// Preserve the exact branch order used by both the retail and the
			// symbolized test-build ChoosePivot implementation.  Equal values must
			// select the same pivot or the stock-only tie permutation would drift.
			if (leftDepth >= middleDepth)
			{
				if (leftDepth < rightDepth)
					return leftDepth;
				if (middleDepth < rightDepth)
					return rightDepth;
				return middleDepth;
			}
			if (middleDepth < rightDepth)
				return middleDepth;
			if (leftDepth >= rightDepth)
				return leftDepth;
			return rightDepth;
		}

		void SortOriginalDepthsWithOrdinals(BSShaderAccumulator& accumulator,
			std::vector<UInt32>& registrationOrdinals,
			SInt32 left, SInt32 right)
		{
			while (right > left)
			{
				SInt32 lower = left - 1;
				SInt32 upper = right + 1;
				const float pivot = ChooseOriginalDepthPivot(
					accumulator, left, right);
				for (;;)
				{
					do
					{
						--upper;
					}
					while (accumulator.m_pfDepths[upper] > pivot);
					do
					{
						++lower;
					}
					while (accumulator.m_pfDepths[lower] < pivot);
					if (lower >= upper)
						break;
					std::swap(accumulator.m_ppkItems[lower],
						accumulator.m_ppkItems[upper]);
					std::swap(accumulator.m_pfDepths[lower],
						accumulator.m_pfDepths[upper]);
					std::swap(registrationOrdinals[lower],
						registrationOrdinals[upper]);
				}
				if (upper == right)
				{
					right = upper - 1;
				}
				else
				{
					SortOriginalDepthsWithOrdinals(accumulator,
						registrationOrdinals, left, upper);
					left = upper + 1;
				}
			}
		}

		bool EnsureOriginalOrderSortStorage(BSShaderAccumulator& accumulator,
			size_t itemCount)
		{
			if (!itemCount || itemCount > kMaximumStableTieItems
				|| itemCount > static_cast<size_t>(
					std::numeric_limits<SInt32>::max()))
			{
				return false;
			}
			if (itemCount <= static_cast<size_t>(
				std::max<SInt32>(0, accumulator.m_iMaxItems)))
			{
				return accumulator.m_ppkItems && accumulator.m_pfDepths;
			}
			NiGeometry** items = static_cast<NiGeometry**>(
				NiAlloc(itemCount * sizeof(NiGeometry*)));
			float* depths = static_cast<float*>(
				NiAlloc(itemCount * sizeof(float)));
			if (!items || !depths)
			{
				NiFree(items);
				NiFree(depths);
				return false;
			}
			NiFree(accumulator.m_ppkItems);
			NiFree(accumulator.m_pfDepths);
			accumulator.m_ppkItems = items;
			accumulator.m_pfDepths = depths;
			accumulator.m_iMaxItems = static_cast<SInt32>(itemCount);
			return true;
		}

		OriginalOrderAnchorFailure PrepareOriginalOrderAnchorTopology(
			SortedPayloadScratch& scratch, BSShaderAccumulator& accumulator,
			size_t itemCount)
		{
			// Empty means the common all-single-block topology, where a block's
			// ordinal is its registration ordinal. Materialize the O(N) table only
			// after an accepted multi-slot Virtual-stock block is actually proven.
			scratch.registrationBlockOrdinals.clear();
			scratch.originalOrderFreeTypeFlags.assign(itemCount, 0);

			auto markSingle = [&](const RegisteredFacade& registration)
			{
				const UInt32 ordinal = registration.acceptedOrdinal;
				if (ordinal == kInvalidNativeA8CommandIndex
					|| ordinal >= itemCount
					|| accumulator.m_ppkItems[ordinal]
						!= registration.facade
					|| scratch.originalOrderFreeTypeFlags[ordinal])
				{
					return false;
				}
				scratch.originalOrderFreeTypeFlags[ordinal] = 1;
				return true;
			};

			for (const RegisteredFacade& registration
				: scratch.pendingRegistrations)
			{
				// An attempted facade that the predecessor did not prove as the
				// exact new AddTail cannot contribute an ordinal.  It also cannot
				// poison the anchor: the source-wide coverage proof below still
				// rejects any FreeType facade that actually entered m_kItems without
				// a committed registration.
				if (registration.acceptedOrdinal
					== kInvalidNativeA8CommandIndex)
				{
					continue;
				}
				const A8ShapeMetadata* metadata = registration.metadata.get();
				if (!metadata || metadata->shapeIdentity != registration.facade)
					return OriginalOrderAnchorFailure::Metadata;

				if (metadata->backend == FreeTypeShapeBackend::VirtualStockNative)
				{
					if (!metadata->virtualStockPrimary
						|| !metadata->virtualStockGroup)
					{
						return OriginalOrderAnchorFailure::Metadata;
					}
					VirtualStockShapeGroup& group =
						*metadata->virtualStockGroup;
					std::lock_guard<std::mutex> lock(group.mutex);
					const VirtualStockFrameMode mode =
						group.frameMode.load(std::memory_order_acquire);
					const size_t slotCount = group.slots.size();
					const size_t blockStart = registration.acceptedOrdinal;
					if (mode != VirtualStockFrameMode::Facade
						|| group.registrationAccumulator != &accumulator
						|| group.registrationCycle != scratch.registrationCycle
						|| !group.registrationContiguous
						|| group.duplicateRegistration
						|| !slotCount
						|| group.registeredSlotCount != slotCount
						|| group.primarySlot + 1u != slotCount
						|| blockStart + slotCount > itemCount)
					{
						return OriginalOrderAnchorFailure::Group;
					}
					if (slotCount > 1u
						&& scratch.registrationBlockOrdinals.empty())
					{
						scratch.registrationBlockOrdinals.resize(itemCount);
						for (size_t ordinal = 0; ordinal < itemCount; ++ordinal)
						{
							scratch.registrationBlockOrdinals[ordinal] =
								static_cast<UInt32>(ordinal);
						}
					}
					const float blockDepth =
						accumulator.m_pfDepths[blockStart];
					for (size_t offset = 0; offset < slotCount; ++offset)
					{
						const size_t ordinal = blockStart + offset;
						const UInt32 slotIndex = group.primarySlot
							- static_cast<UInt32>(offset);
						if (accumulator.m_ppkItems[ordinal]
								!= group.slots[slotIndex].shape
							|| accumulator.m_pfDepths[ordinal] != blockDepth
							|| scratch.originalOrderFreeTypeFlags[ordinal])
						{
							return OriginalOrderAnchorFailure::Group;
						}
						scratch.originalOrderFreeTypeFlags[ordinal] = 1;
						if (!scratch.registrationBlockOrdinals.empty())
						{
							scratch.registrationBlockOrdinals[ordinal] =
								static_cast<UInt32>(blockStart);
						}
					}
					continue;
				}

				if (metadata->backend
					== FreeTypeShapeBackend::VirtualStockSingleton)
				{
					VirtualStockSingletonState* singleton =
						GetVirtualStockSingletonState(*metadata);
					if (!singleton)
						return OriginalOrderAnchorFailure::Metadata;
					const VirtualStockFrameMode mode = singleton->frameMode.load(
						std::memory_order_acquire);
					if (mode != VirtualStockFrameMode::Facade
						|| singleton->registrationAccumulator != &accumulator
						|| singleton->registrationCycle != scratch.registrationCycle
						|| !singleton->registrationContiguous
						|| singleton->duplicateRegistration
						|| singleton->registeredSlotCount != 1
						|| singleton->slot.shape != registration.facade)
					{
						return OriginalOrderAnchorFailure::Singleton;
					}
					if (!markSingle(registration))
						return OriginalOrderAnchorFailure::Registration;
					continue;
				}

				if (!markSingle(registration))
					return OriginalOrderAnchorFailure::Registration;
			}

			bool haveFreeType = false;
			for (size_t ordinal = 0; ordinal < itemCount; ++ordinal)
			{
				const bool tracked =
					scratch.originalOrderFreeTypeFlags[ordinal] != 0;
				if (IsFreeTypeFacade(accumulator.m_ppkItems[ordinal]) != tracked)
					return OriginalOrderAnchorFailure::Coverage;
				haveFreeType = haveFreeType || tracked;
			}
			if (!haveFreeType)
				return OriginalOrderAnchorFailure::Coverage;
			return OriginalOrderAnchorFailure::None;
		}

		bool BuildOriginalOrderDesiredOrdinals(
			SortedPayloadScratch& scratch, size_t itemCount)
		{
			if (!scratch.registrationBlockOrdinals.empty()
				&& scratch.registrationBlockOrdinals.size() != itemCount)
			{
				return false;
			}
			auto blockStartFor = [&](size_t ordinal)
			{
				return scratch.registrationBlockOrdinals.empty()
					? static_cast<UInt32>(ordinal)
					: scratch.registrationBlockOrdinals[ordinal];
			};
			scratch.originalOrderDesiredOrdinals.clear();
			scratch.originalOrderDesiredOrdinals.reserve(itemCount);
			for (size_t cursor = itemCount; cursor;)
			{
				const size_t candidate = cursor - 1u;
				const size_t blockStart = blockStartFor(candidate);
				if (blockStart > candidate)
					return false;
				if (blockStart == candidate)
				{
					scratch.originalOrderDesiredOrdinals.push_back(
						static_cast<UInt32>(candidate));
					--cursor;
					continue;
				}
				for (size_t ordinal = blockStart;
					ordinal <= candidate; ++ordinal)
				{
					if (blockStartFor(ordinal) != blockStart)
					{
						return false;
					}
					scratch.originalOrderDesiredOrdinals.push_back(
						static_cast<UInt32>(ordinal));
				}
				cursor = blockStart;
			}
			return scratch.originalOrderDesiredOrdinals.size() == itemCount;
		}

		bool ApplyOriginalOrderAnchors(SortedPayloadScratch& scratch,
			BSShaderAccumulator& accumulator, size_t itemCount)
		{
			scratch.originalOrderRuns.clear();
			scratch.originalOrderRunIds.resize(itemCount);
			bool haveMixedRun = false;
			for (size_t runBegin = 0; runBegin < itemCount;)
			{
				const float runDepth = accumulator.m_pfDepths[runBegin];
				size_t runEnd = runBegin + 1u;
				while (runEnd < itemCount
					&& accumulator.m_pfDepths[runEnd] == runDepth)
				{
					++runEnd;
				}
				bool hasFreeType = false;
				bool hasStock = false;
				for (size_t item = runBegin; item < runEnd; ++item)
				{
					const UInt32 ordinal =
						scratch.sortedRegistrationOrdinals[item];
					const bool isFreeType =
						scratch.originalOrderFreeTypeFlags[ordinal] != 0;
					hasFreeType = hasFreeType || isFreeType;
					hasStock = hasStock || !isFreeType;
				}
				const UInt32 runId = static_cast<UInt32>(
					scratch.originalOrderRuns.size());
				const bool mixed = hasFreeType && hasStock
					&& runEnd - runBegin > 1u;
				scratch.originalOrderRuns.push_back({
					static_cast<UInt32>(runBegin),
					static_cast<UInt32>(runEnd),
					static_cast<UInt32>(runBegin),
					mixed,
					false
				});
				haveMixedRun = haveMixedRun || mixed;
				for (size_t item = runBegin; item < runEnd; ++item)
					scratch.originalOrderRunIds[item] = runId;
				runBegin = runEnd;
			}
			if (!haveMixedRun)
				return true;

			scratch.sortedItemIndicesByRegistrationOrdinal.assign(
				itemCount, kInvalidNativeA8CommandIndex);
			for (size_t item = 0; item < itemCount; ++item)
			{
				const UInt32 ordinal = scratch.sortedRegistrationOrdinals[item];
				if (ordinal >= itemCount
					|| scratch.sortedItemIndicesByRegistrationOrdinal[ordinal]
						!= kInvalidNativeA8CommandIndex)
				{
					return false;
				}
				scratch.sortedItemIndicesByRegistrationOrdinal[ordinal] =
					static_cast<UInt32>(item);
			}
			if (!BuildOriginalOrderDesiredOrdinals(scratch, itemCount))
				return false;
			scratch.originalOrderOutput.resize(itemCount);

			for (const UInt32 ordinal
				: scratch.originalOrderDesiredOrdinals)
			{
				if (ordinal >= itemCount)
					return false;
				const UInt32 sortedItem =
					scratch.sortedItemIndicesByRegistrationOrdinal[ordinal];
				if (sortedItem >= itemCount)
					return false;
				const UInt32 runId = scratch.originalOrderRunIds[sortedItem];
				if (runId >= scratch.originalOrderRuns.size())
					return false;
				OriginalOrderAnchorRun& run =
					scratch.originalOrderRuns[runId];
				if (!run.mixed)
					continue;
				if (run.write >= run.end)
					return false;
				NiGeometry* desired = accumulator.m_ppkItems[sortedItem];
				run.changed = run.changed
					|| accumulator.m_ppkItems[run.write] != desired;
				scratch.originalOrderOutput[run.write++] = desired;
			}

			for (const OriginalOrderAnchorRun& run
				: scratch.originalOrderRuns)
			{
				if (!run.mixed)
					continue;
				if (run.write != run.end)
					return false;
			}

			// Commit only after every mixed run has a complete destination.
			// A late proof failure must leave the stock-equivalent quicksort
			// output intact for the ordinal-sidecar or predecessor fallback.
			for (const OriginalOrderAnchorRun& run
				: scratch.originalOrderRuns)
			{
				if (!run.mixed)
					continue;
				if (!run.changed)
					continue;
				for (size_t item = run.begin; item < run.end; ++item)
				{
					accumulator.m_ppkItems[item] =
						scratch.originalOrderOutput[item];
				}
				RecordFreeTypePerf(
					FreeTypePerfCounter::SortedOriginalOrderAnchorMixedRun);
			}
			return true;
		}

		bool RestoreSingleBlockMixedEqualDepthPainterOrderFromOrdinals(
			SortedPayloadScratch& scratch, BSShaderAccumulator& accumulator,
			size_t itemCount)
		{
			if (scratch.sortedRegistrationOrdinals.size() != itemCount)
				return false;

			// A single facade is its own painter-order block.  This covers the
			// ordinary native backend and the Virtual-stock singleton backend.
			// Keep the existing compatibility path for every real multi-slot
			// group; its primary/follower array convention needs the full mutable
			// topology proof and must never be guessed from facade addresses.
			for (const RegisteredFacade& registration
				: scratch.pendingRegistrations)
			{
				const A8ShapeMetadata* metadata = registration.metadata.get();
				if (!metadata || metadata->shapeIdentity != registration.facade)
					return false;
				if (metadata->backend != FreeTypeShapeBackend::VirtualStockNative)
					continue;
				if (!metadata->virtualStockPrimary
					|| !metadata->virtualStockGroup)
				{
					return false;
				}
				VirtualStockShapeGroup& group = *metadata->virtualStockGroup;
				std::lock_guard<std::mutex> lock(group.mutex);
				if (group.slots.size() != 1u)
					return false;
			}

			// The sidecar started as [0, itemCount) and was swapped beside every
			// stock-equivalent quicksort exchange.  Validate the permutation before
			// planning any writes so a rejected frame remains byte-for-byte stock.
			scratch.sortedItemIndicesByRegistrationOrdinal.assign(
				itemCount, kInvalidNativeA8CommandIndex);
			scratch.originalOrderRuns.clear();
			for (size_t runBegin = 0; runBegin < itemCount;)
			{
				const float runDepth = accumulator.m_pfDepths[runBegin];
				if (!std::isfinite(runDepth))
					return false;
				size_t runEnd = runBegin + 1u;
				while (runEnd < itemCount
					&& accumulator.m_pfDepths[runEnd] == runDepth)
				{
					++runEnd;
				}

				bool hasFreeType = false;
				bool hasStock = false;
				bool painterOrderChanged = false;
				UInt32 previousOrdinal = 0;
				for (size_t item = runBegin; item < runEnd; ++item)
				{
					const UInt32 ordinal =
						scratch.sortedRegistrationOrdinals[item];
					if (ordinal >= itemCount
						|| scratch.sortedItemIndicesByRegistrationOrdinal[ordinal]
							!= kInvalidNativeA8CommandIndex)
					{
						return false;
					}
					scratch.sortedItemIndicesByRegistrationOrdinal[ordinal] =
						static_cast<UInt32>(item);
					const bool isFreeType =
						IsFreeTypeFacade(accumulator.m_ppkItems[item]);
					hasFreeType = hasFreeType || isFreeType;
					hasStock = hasStock || !isFreeType;
					if (item != runBegin)
					{
						// The renderer consumes this array backwards, so singleton
						// blocks must descend by registration ordinal in storage.
						painterOrderChanged = painterOrderChanged
							|| ordinal > previousOrdinal;
					}
					previousOrdinal = ordinal;
				}
				if (hasFreeType && hasStock && painterOrderChanged
					&& runEnd - runBegin > 1u)
				{
					scratch.originalOrderRuns.push_back({
						static_cast<UInt32>(runBegin),
						static_cast<UInt32>(runEnd), 0, true, true
					});
				}
				runBegin = runEnd;
			}

			// All fallible validation is complete.  Only the compact changed runs
			// are copied and sorted; the source-list snapshot, pointer hash, full
			// block table, and sorted-to-registration reconstruction are avoided.
			for (const OriginalOrderAnchorRun& run
				: scratch.originalOrderRuns)
			{
				scratch.stableTieItems.clear();
				scratch.stableTieItems.reserve(run.end - run.begin);
				for (size_t item = run.begin; item < run.end; ++item)
				{
					const UInt32 ordinal =
						scratch.sortedRegistrationOrdinals[item];
					scratch.stableTieItems.push_back({
						accumulator.m_ppkItems[item],
						accumulator.m_pfDepths[item], ordinal, ordinal
					});
				}
				std::sort(scratch.stableTieItems.begin(),
					scratch.stableTieItems.end(),
					[](const StableTileTieItem& left,
						const StableTileTieItem& right)
					{
						return left.registrationOrdinal
							> right.registrationOrdinal;
					});
				for (size_t offset = 0;
					offset < scratch.stableTieItems.size(); ++offset)
				{
					const StableTileTieItem& item =
						scratch.stableTieItems[offset];
					accumulator.m_ppkItems[run.begin + offset] =
						item.geometry;
					accumulator.m_pfDepths[run.begin + offset] = item.depth;
				}
				RecordFreeTypePerf(FreeTypePerfCounter::
					SortedOriginalOrderSidecarMixedRun);
			}
			return true;
		}

		OriginalOrderSortAttempt TrySortWithOriginalOrderAnchors(
			SortedPayloadScratch& scratch, BSShaderAccumulator* accumulator)
		{
			OriginalOrderSortAttempt attempt;
			if (!accumulator || scratch.pendingAccumulator != accumulator
				|| scratch.pendingRegistrations.empty()
				|| !accumulator->m_bInterfaceSort)
			{
				attempt.failure = OriginalOrderAnchorFailure::Gate;
				return attempt;
			}
			NiSortedObjectList* sourceList = accumulator->m_pGeometryList;
			if (!sourceList)
			{
				accumulator->m_pGeometryList = &accumulator->m_kItems;
				sourceList = &accumulator->m_kItems;
			}
			const size_t itemCount = sourceList->GetSize();
			if (itemCount < 2 || itemCount > kMaximumStableTieItems)
			{
				attempt.failure = OriginalOrderAnchorFailure::Count;
				return attempt;
			}
			if (!EnsureOriginalOrderSortStorage(*accumulator, itemCount))
			{
				attempt.failure = OriginalOrderAnchorFailure::Storage;
				return attempt;
			}

			accumulator->m_iNumItems = static_cast<SInt32>(itemCount);
			scratch.sortedRegistrationOrdinals.resize(itemCount);
			NiTListIterator position = sourceList->GetHeadPos();
			for (size_t ordinal = 0; ordinal < itemCount; ++ordinal)
			{
				if (!position)
				{
					attempt.failure = OriginalOrderAnchorFailure::Source;
					return attempt;
				}
				NiGeometry* geometry = sourceList->GetNext(position);
				if (!geometry)
				{
					attempt.failure = OriginalOrderAnchorFailure::Source;
					return attempt;
				}
				const float depth = geometry->m_kWorld.m_Translate.y;
				if (!std::isfinite(depth))
				{
					attempt.failure = OriginalOrderAnchorFailure::Depth;
					return attempt;
				}
				accumulator->m_ppkItems[ordinal] = geometry;
				accumulator->m_pfDepths[ordinal] = depth;
				scratch.sortedRegistrationOrdinals[ordinal] =
					static_cast<UInt32>(ordinal);
			}
			if (position)
			{
				attempt.failure = OriginalOrderAnchorFailure::Source;
				return attempt;
			}
			const OriginalOrderAnchorFailure topologyFailure =
				PrepareOriginalOrderAnchorTopology(
					scratch, *accumulator, itemCount);

			// Once the source list and every finite depth are proven, this is a
			// byte-for-byte decision copy of the retail interface quicksort.  Keep
			// its exact ordinal sidecar even when the stricter FreeType topology
			// proof below fails; calling the predecessor again would only discard
			// information and force the later snapshot/hash reconstruction.
			SortOriginalDepthsWithOrdinals(*accumulator,
				scratch.sortedRegistrationOrdinals, 0,
				static_cast<SInt32>(itemCount - 1u));
			if (topologyFailure == OriginalOrderAnchorFailure::None
				&& ApplyOriginalOrderAnchors(
					scratch, *accumulator, itemCount))
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::SortedOriginalOrderAnchorSort);
				RecordFreeTypePerf(
					FreeTypePerfCounter::SortedOriginalOrderAnchorItem,
					static_cast<UInt64>(itemCount));
				attempt.outcome = OriginalOrderSortOutcome::Anchored;
				attempt.failure = OriginalOrderAnchorFailure::None;
				return attempt;
			}

			attempt.failure = topologyFailure
				!= OriginalOrderAnchorFailure::None
				? topologyFailure : OriginalOrderAnchorFailure::Apply;
			RecordFreeTypePerf(FreeTypePerfCounter::
				SortedOriginalOrderStockEquivalentSort);
			RecordFreeTypePerf(FreeTypePerfCounter::
				SortedOriginalOrderStockEquivalentItem,
				static_cast<UInt64>(itemCount));
			if (RestoreSingleBlockMixedEqualDepthPainterOrderFromOrdinals(
				scratch, *accumulator, itemCount))
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					SortedOriginalOrderSidecarRecovered);
				attempt.outcome =
					OriginalOrderSortOutcome::OrdinalSidecarRecovered;
				return attempt;
			}

			RecordFreeTypePerf(
				FreeTypePerfCounter::SortedOriginalOrderSidecarLegacy);
			attempt.outcome =
				OriginalOrderSortOutcome::StockEquivalentLegacy;
			return attempt;
		}

		bool RestoreFreeTypeMixedEqualDepthPainterOrder(
			SortedPayloadScratch& scratch, BSShaderAccumulator* accumulator)
		{
			if (scratch.pendingRegistrations.empty())
				return true;
			const size_t itemCount = accumulator
				? static_cast<size_t>(accumulator->m_iNumItems) : 0;
			auto reject = []()
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::SortedMixedEqualDepthRestoreRejected);
				return false;
			};
			if (!accumulator || scratch.pendingAccumulator != accumulator
				|| !accumulator->m_ppkItems || !accumulator->m_pfDepths
				|| itemCount < 2 || itemCount > kMaximumStableTieItems)
			{
				return itemCount < 2 ? true : reject();
			}
			bool hasMixedTieCandidate = false;
			for (size_t runBegin = 0;
				runBegin < itemCount && !hasMixedTieCandidate;)
			{
				const float runDepth = accumulator->m_pfDepths[runBegin];
				size_t runEnd = runBegin + 1u;
				if (std::isfinite(runDepth))
				{
					while (runEnd < itemCount
						&& std::isfinite(accumulator->m_pfDepths[runEnd])
						&& accumulator->m_pfDepths[runEnd] == runDepth)
					{
						++runEnd;
					}
				}
				bool hasFreeType = false;
				bool hasStock = false;
				for (size_t item = runBegin; item < runEnd; ++item)
				{
					const bool isFreeType =
						IsFreeTypeFacade(accumulator->m_ppkItems[item]);
					hasFreeType = hasFreeType || isFreeType;
					hasStock = hasStock || !isFreeType;
				}
				hasMixedTieCandidate = hasFreeType && hasStock
					&& runEnd - runBegin > 1u;
				runBegin = runEnd;
			}
			if (!hasMixedTieCandidate)
				return true;
			const NiSortedObjectList* sourceList =
				accumulator->m_pGeometryList
					? accumulator->m_pGeometryList : &accumulator->m_kItems;
			if (!sourceList || sourceList->GetSize() != itemCount)
				return reject();
			scratch.acceptedTileRegistrations.clear();
			scratch.acceptedTileRegistrations.reserve(itemCount);
			NiTListIterator position = sourceList->GetHeadPos();
			while (position
				&& scratch.acceptedTileRegistrations.size() < itemCount)
			{
				scratch.acceptedTileRegistrations.push_back(
					sourceList->GetNext(position));
			}
			if (position
				|| scratch.acceptedTileRegistrations.size() != itemCount)
			{
				return reject();
			}

			constexpr UInt32 kConsumedBit = 0x80000000u;
			constexpr UInt32 kIndexMask = 0x7FFFFFFFu;
			PrepareLookup(scratch.acceptedRegistrationLookup, itemCount);
			const size_t lookupMask =
				scratch.acceptedRegistrationLookup.size() - 1u;
			for (size_t ordinal = 0; ordinal < itemCount; ++ordinal)
			{
				NiGeometry* geometry =
					scratch.acceptedTileRegistrations[ordinal];
				if (!geometry)
					return reject();
				size_t slot = HashPointer(geometry) & lookupMask;
				bool inserted = false;
				for (size_t probe = 0;
					probe < scratch.acceptedRegistrationLookup.size(); ++probe)
				{
					const UInt32 stored =
						scratch.acceptedRegistrationLookup[slot];
					if (!stored)
					{
						scratch.acceptedRegistrationLookup[slot] =
							static_cast<UInt32>(ordinal + 1u);
						inserted = true;
						break;
					}
					const size_t existing =
						static_cast<size_t>((stored & kIndexMask) - 1u);
					if (existing < itemCount
						&& scratch.acceptedTileRegistrations[existing]
							== geometry)
					{
						// Ambiguous repeated geometry is legal for the engine but
						// cannot be assigned a unique insertion ordinal here.
						return reject();
					}
					slot = (slot + 1u) & lookupMask;
				}
				if (!inserted)
					return reject();
			}

			scratch.sortedRegistrationOrdinals.resize(itemCount);
			scratch.sortedItemIndicesByRegistrationOrdinal.resize(itemCount);
			for (size_t item = 0; item < itemCount; ++item)
			{
				NiGeometry* geometry = accumulator->m_ppkItems[item];
				size_t slot = HashPointer(geometry) & lookupMask;
				bool found = false;
				for (size_t probe = 0;
					probe < scratch.acceptedRegistrationLookup.size(); ++probe)
				{
					const UInt32 stored =
						scratch.acceptedRegistrationLookup[slot];
					if (!stored)
						break;
					const size_t ordinal = static_cast<size_t>(
						(stored & kIndexMask) - 1u);
					if (ordinal < itemCount
						&& scratch.acceptedTileRegistrations[ordinal]
							== geometry)
					{
						if (stored & kConsumedBit)
							return reject();
						scratch.acceptedRegistrationLookup[slot] =
							stored | kConsumedBit;
						scratch.sortedRegistrationOrdinals[item] =
							static_cast<UInt32>(ordinal);
						scratch.sortedItemIndicesByRegistrationOrdinal[ordinal] =
							static_cast<UInt32>(item);
						found = true;
						break;
					}
					slot = (slot + 1u) & lookupMask;
				}
				if (!found)
					return reject();
			}

			// Treat a Virtual-stock logical text as one painter-order block. Its
			// primary/follower array order must remain ascending by registration so
			// the stock reverse traversal submits slot 0 through primary and the
			// later topology validator still sees one contiguous span.
			scratch.registrationBlockOrdinals.resize(itemCount);
			for (size_t ordinal = 0; ordinal < itemCount; ++ordinal)
			{
				scratch.registrationBlockOrdinals[ordinal] =
					static_cast<UInt32>(ordinal);
			}
			auto lookupAcceptedOrdinal = [&](const NiGeometry* geometry,
				size_t& ordinal)
			{
				if (!geometry)
					return false;
				size_t slot = HashPointer(geometry) & lookupMask;
				for (size_t probe = 0;
					probe < scratch.acceptedRegistrationLookup.size(); ++probe)
				{
					const UInt32 stored =
						scratch.acceptedRegistrationLookup[slot];
					if (!stored)
						return false;
					const size_t candidate = static_cast<size_t>(
						(stored & kIndexMask) - 1u);
					if (candidate < itemCount
						&& scratch.acceptedTileRegistrations[candidate]
							== geometry)
					{
						ordinal = candidate;
						return true;
					}
					slot = (slot + 1u) & lookupMask;
				}
				return false;
			};
			for (const RegisteredFacade& registration
				: scratch.pendingRegistrations)
			{
				const A8ShapeMetadata* metadata = registration.metadata.get();
				if (!metadata
					|| metadata->backend
						!= FreeTypeShapeBackend::VirtualStockNative)
				{
					continue;
				}
				if (!metadata->virtualStockPrimary
					|| !metadata->virtualStockGroup)
				{
					return reject();
				}
				VirtualStockShapeGroup& group =
					*metadata->virtualStockGroup;
				std::lock_guard<std::mutex> lock(group.mutex);
				const VirtualStockFrameMode mode =
					group.frameMode.load(std::memory_order_acquire);
				if (mode == VirtualStockFrameMode::Culled
					|| group.registeredSlotCount == 0)
				{
					continue;
				}
				const size_t slotCount = group.slots.size();
				if (mode != VirtualStockFrameMode::Facade
					|| group.registrationAccumulator != accumulator
					|| group.registrationCycle != scratch.registrationCycle
					|| !group.registrationContiguous
					|| group.duplicateRegistration
					|| !slotCount
					|| group.registeredSlotCount != slotCount
					|| group.primarySlot + 1u != slotCount)
				{
					return reject();
				}
				if (slotCount == 1u)
					continue;

				size_t blockStart = itemCount;
				float blockDepth = 0.0f;
				for (SInt32 slotIndex =
						static_cast<SInt32>(group.primarySlot);
					slotIndex >= 0; --slotIndex)
				{
					const size_t offset = static_cast<size_t>(
						group.primarySlot - static_cast<UInt32>(slotIndex));
					size_t ordinal = itemCount;
					if (!lookupAcceptedOrdinal(
						group.slots[slotIndex].shape, ordinal))
					{
						return reject();
					}
					if (!offset)
						blockStart = ordinal;
					else if (ordinal != blockStart + offset)
						return reject();
					const size_t sortedItem =
						scratch.sortedItemIndicesByRegistrationOrdinal[ordinal];
					if (sortedItem >= itemCount)
						return reject();
					const float depth = accumulator->m_pfDepths[sortedItem];
					if (!std::isfinite(depth)
						|| (offset && depth != blockDepth))
					{
						return reject();
					}
					if (!offset)
						blockDepth = depth;
				}
				for (size_t offset = 0; offset < slotCount; ++offset)
				{
					scratch.registrationBlockOrdinals[blockStart + offset] =
						static_cast<UInt32>(blockStart);
				}
			}

			for (size_t runBegin = 0; runBegin < itemCount;)
			{
				const float runDepth = accumulator->m_pfDepths[runBegin];
				size_t runEnd = runBegin + 1u;
				if (std::isfinite(runDepth))
				{
					while (runEnd < itemCount
						&& std::isfinite(accumulator->m_pfDepths[runEnd])
						&& accumulator->m_pfDepths[runEnd] == runDepth)
					{
						++runEnd;
					}
				}
				bool hasFreeType = false;
				bool hasStock = false;
				bool painterOrderChanged = false;
				for (size_t item = runBegin; item < runEnd; ++item)
				{
					const bool isFreeType =
						IsFreeTypeFacade(accumulator->m_ppkItems[item]);
					hasFreeType = hasFreeType || isFreeType;
					hasStock = hasStock || !isFreeType;
					if (item > runBegin)
					{
						const UInt32 previousOrdinal =
							scratch.sortedRegistrationOrdinals[item - 1u];
						const UInt32 currentOrdinal =
							scratch.sortedRegistrationOrdinals[item];
						const UInt32 previousBlock =
							scratch.registrationBlockOrdinals[previousOrdinal];
						const UInt32 currentBlock =
							scratch.registrationBlockOrdinals[currentOrdinal];
						// FinishAccumulating consumes the array backwards. Blocks
						// therefore descend by registration, while members of one
						// Virtual-stock block retain their original ascending order.
						painterOrderChanged = painterOrderChanged
							|| currentBlock > previousBlock
							|| (currentBlock == previousBlock
								&& currentOrdinal < previousOrdinal);
					}
				}
				if (hasFreeType && hasStock && painterOrderChanged
					&& runEnd - runBegin > 1u)
				{
					scratch.stableTieItems.clear();
					scratch.stableTieItems.reserve(runEnd - runBegin);
					for (size_t item = runBegin; item < runEnd; ++item)
					{
						scratch.stableTieItems.push_back({
							accumulator->m_ppkItems[item],
							accumulator->m_pfDepths[item],
							scratch.sortedRegistrationOrdinals[item],
							scratch.registrationBlockOrdinals[
								scratch.sortedRegistrationOrdinals[item]]
						});
					}
					std::sort(scratch.stableTieItems.begin(),
						scratch.stableTieItems.end(),
						[](const StableTileTieItem& left,
							const StableTileTieItem& right)
						{
							if (left.registrationBlockOrdinal
								!= right.registrationBlockOrdinal)
							{
								return left.registrationBlockOrdinal
									> right.registrationBlockOrdinal;
							}
							return left.registrationOrdinal
								< right.registrationOrdinal;
						});
					for (size_t offset = 0;
						offset < scratch.stableTieItems.size(); ++offset)
					{
						const StableTileTieItem& item =
							scratch.stableTieItems[offset];
						accumulator->m_ppkItems[runBegin + offset] =
							item.geometry;
						accumulator->m_pfDepths[runBegin + offset] =
							item.depth;
					}
					RecordFreeTypePerf(
						FreeTypePerfCounter::SortedMixedEqualDepthRunRestored);
					RecordFreeTypePerf(
						FreeTypePerfCounter::SortedMixedEqualDepthItemRestored,
						static_cast<UInt64>(runEnd - runBegin));
				}
				runBegin = runEnd;
			}
			return true;
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
				? frameContext->immediateRouteCurrent
				: IsA8RenderPassImmediatelyHookCurrent()))
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

		bool RegisterVirtualStockSingleton(BSShaderAccumulator* accumulator,
			NiTriShape* shape, const A8ShapeMetadataPtr& metadata,
			TileRegisterObjectFn original,
			const NiPropertyState* properties,
			BSShaderProperty* shaderProperty, BSShader* shader)
		{
			VirtualStockSingletonState* singleton = metadata
				? GetVirtualStockSingletonState(*metadata) : nullptr;
			if (!accumulator || !shape || !metadata || !singleton
				|| singleton->slot.shape != shape)
			{
				return true;
			}
			if (s_sortedPayloadScratch.active
				|| s_sortedPayloadScratch.nestedBypassDepth)
			{
				if (!metadata->nativePayload.buildComplete)
				{
					return SuppressNativeGroup(shape, *metadata,
						NativeA8FallbackReason::PacketBuild,
						"virtual-stock-singleton-nested-register");
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
				return ForwardTileRegisterObject(original, accumulator, shape,
					properties, shaderProperty, shader);
			}

			if (!metadata->nativePayload.buildComplete)
			{
				return SuppressNativeGroup(shape, *metadata,
					NativeA8FallbackReason::PacketBuild,
					"virtual-stock-singleton-register");
			}
			const UInt64 registrationCycle = RecordSortedRegistration(
				accumulator, shape, metadata);
			const bool newCycle =
				singleton->registrationCycle != registrationCycle
				|| singleton->registrationAccumulator != accumulator;
			if (newCycle)
			{
				singleton->registrationAccumulator = accumulator;
				singleton->registrationCycle = registrationCycle;
				singleton->preflightValidationToken = 0;
				singleton->registeredSlotCount = 0;
				singleton->registrationContiguous = registrationCycle != 0;
				singleton->duplicateRegistration = false;
			}
			else if (singleton->registeredSlotCount)
			{
				singleton->registrationContiguous = false;
				singleton->duplicateRegistration = true;
			}
			if (singleton->frameMode.load(std::memory_order_acquire)
				!= VirtualStockFrameMode::Retired)
			{
				singleton->frameMode.store(VirtualStockFrameMode::Facade,
					std::memory_order_release);
			}
			if (!registrationCycle)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::VirtualStockFacadeFallback);
			}
			if (singleton->duplicateRegistration)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::VirtualStockRegistrationDuplicate);
				return true;
			}

			const NativeA8VisibilityCull visibility =
				registrationCycle
					? EvaluateNativeA8PreAccumulatorVisibility(shape,
						metadata->nativePayload, properties, shaderProperty, shader)
					: EvaluateNativeA8SubmissionVisibility(
						shape, metadata->nativePayload);
			if (visibility != NativeA8VisibilityCull::None)
			{
				if (singleton->registrationCycle == registrationCycle
					&& singleton->registrationAccumulator == accumulator
					&& singleton->frameMode.load(std::memory_order_acquire)
						!= VirtualStockFrameMode::Retired)
				{
					singleton->frameMode.store(VirtualStockFrameMode::Culled,
						std::memory_order_release);
				}
				RecordNativeA8VisibilityCull(
					visibility, metadata->nativePayload);
				DiscardUnacceptedSortedRegistration(
					accumulator, shape, registrationCycle);
				return true;
			}

			const UInt32 sizeBefore = accumulator->m_kItems.GetSize();
			const bool result = ForwardTileRegisterObject(original, accumulator,
				shape, properties, shaderProperty, shader);
			CommitSortedRegistration(accumulator, shape, registrationCycle,
				sizeBefore, result);
			if (!result)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::VirtualStockRegistrationRejected);
			}
			if (singleton->registrationCycle != registrationCycle
				|| singleton->registrationAccumulator != accumulator
				|| singleton->frameMode.load(std::memory_order_acquire)
					== VirtualStockFrameMode::Retired)
			{
				singleton->registrationContiguous = false;
				return result;
			}
			singleton->registeredSlotCount = 1;
			singleton->registrationContiguous =
				singleton->registrationContiguous && result;
			if (result)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::VirtualStockRegistrationResolved);
			}
			return result;
		}

		bool RegisterVirtualStockShape(BSShaderAccumulator* accumulator,
			NiTriShape* shape, const A8ShapeMetadataPtr& metadata,
			TileRegisterObjectFn original,
			const NiPropertyState* properties,
			BSShaderProperty* shaderProperty, BSShader* shader)
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
				return ForwardTileRegisterObject(original, accumulator, shape,
					properties, shaderProperty, shader);
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

				const NativeA8VisibilityCull visibility =
					registrationCycle
						? EvaluateNativeA8PreAccumulatorVisibility(shape,
							metadata->nativePayload, properties, shaderProperty, shader)
						: EvaluateNativeA8SubmissionVisibility(
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
					DiscardUnacceptedSortedRegistration(
						accumulator, shape, registrationCycle);
					return true;
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

			const UInt32 sizeBefore = metadata->virtualStockPrimary
				? accumulator->m_kItems.GetSize()
				: std::numeric_limits<UInt32>::max();
			const bool result = ForwardTileRegisterObject(original, accumulator,
				shape, properties, shaderProperty, shader);
			if (metadata->virtualStockPrimary)
			{
				CommitSortedRegistration(accumulator, shape, registrationCycle,
					sizeBefore, result);
			}
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

		bool __cdecl NativeA8RegisterObject(BSShaderAccumulator* accumulator,
			NiGeometry* geometry, const NiPropertyState* properties,
			BSShaderProperty* shaderProperty, BSShader* shader)
		{
			const TileRegisterObjectFn original =
				s_originalTileRegisterObject.load(std::memory_order_acquire);
			if (!original || !accumulator || !geometry
				|| accumulator->eRenderMode != BSShaderManager::BSSM_RENDER_TILES
				|| !IsFreeTypeFacade(geometry))
			{
				return ForwardTileRegisterObject(original, accumulator, geometry,
					properties, shaderProperty, shader);
			}

			NiTriShape* facade = static_cast<NiTriShape*>(geometry);
			const A8ShapeMetadataPtr metadata = FindA8ShapeMetadata(facade);
			if (metadata && metadata->backend
				== FreeTypeShapeBackend::VirtualStockSingleton)
			{
				if (!IsA8RenderPassImmediatelyHookCurrent())
				{
					return SuppressNativeGroup(facade, *metadata,
						NativeA8FallbackReason::TileRouteConflict,
						"virtual-stock-singleton-register");
				}
				return RegisterVirtualStockSingleton(accumulator, facade,
					metadata, original, properties, shaderProperty, shader);
			}
			if (metadata && metadata->backend
				== FreeTypeShapeBackend::VirtualStockNative)
			{
				if (!IsA8RenderPassImmediatelyHookCurrent())
				{
					return SuppressNativeGroup(facade, *metadata,
						NativeA8FallbackReason::TileRouteConflict,
						"virtual-stock-register");
				}
				return RegisterVirtualStockShape(accumulator, facade, metadata,
					original, properties, shaderProperty, shader);
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

			// A provably invisible zero-alpha entry is handled before either tNVSE or
			// the predecessor records it. Kept text contributes one facade to the stock
			// Tile alpha list. Equal-depth entries are quicksorted unstably, so
			// individually registered packets cannot retain Glow/Shadow/Outline/Fill
			// order. Expand only after stock UI sorting.
			if (!IsA8RenderPassImmediatelyHookCurrent())
				return SuppressNativeGroup(facade, *metadata,
					NativeA8FallbackReason::TileRouteConflict, "register-object");
			if (EnsurePendingAccumulator(accumulator))
			{
				const NativeA8VisibilityCull visibility =
					EvaluateNativeA8PreAccumulatorVisibility(facade,
						metadata->nativePayload, properties, shaderProperty, shader);
				if (visibility != NativeA8VisibilityCull::None)
				{
					RecordNativeA8VisibilityCull(
						visibility, metadata->nativePayload);
					return true;
				}
			}
			const UInt64 registrationCycle = RecordSortedRegistration(
				accumulator, facade, metadata);
			const UInt32 sizeBefore = accumulator->m_kItems.GetSize();
			const bool result = ForwardTileRegisterObject(original, accumulator,
				facade, properties, shaderProperty, shader);
			CommitSortedRegistration(accumulator, facade, registrationCycle,
				sizeBefore, result);
			return result;
		}

		void __fastcall NativeA8SortAlphaGeometry(
			BSShaderAccumulator* accumulator, AccumulatorSortFn originalSort)
		{
			if (!accumulator)
				return;

			// Reproduce the first instruction overwritten at 0xB65E95 before
			// choosing either the anchored implementation or the vtable predecessor
			// already loaded into EDX by FinishAccumulating_Tiles.
			accumulator->m_pGeometryList = nullptr;
			SortedPayloadScratch& scratch = s_sortedPayloadScratch;
			scratch.originalOrderAnchorAccumulator = nullptr;
			scratch.originalOrderAnchorCycle = 0;
			const bool haveAnchorCandidate = !scratch.active
				&& !scratch.nestedBypassDepth
				&& scratch.pendingAccumulator == accumulator
				&& accumulator->eRenderMode
					== BSShaderManager::BSSM_RENDER_TILES
				&& scratch.acceptedOriginalOrderAnchorCount != 0;
			const bool stockSortPredecessor = reinterpret_cast<SIZE_T>(
				originalSort) == kStockInterfaceAlphaSort;
			if (haveAnchorCandidate && stockSortPredecessor)
			{
				const OriginalOrderSortAttempt attempt =
					TrySortWithOriginalOrderAnchors(scratch, accumulator);
				if (attempt.outcome != OriginalOrderSortOutcome::Anchored)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						SortedOriginalOrderAnchorProofFallback);
					RecordFreeTypePerf(FreeTypePerfCounter::
						SortedOriginalOrderAnchorFallback);
					RecordOriginalOrderAnchorFailure(attempt.failure);
				}
				if (attempt.outcome == OriginalOrderSortOutcome::Anchored
					|| attempt.outcome
						== OriginalOrderSortOutcome::OrdinalSidecarRecovered)
				{
					scratch.originalOrderAnchorAccumulator = accumulator;
					scratch.originalOrderAnchorCycle = scratch.registrationCycle;
					return;
				}
				if (attempt.outcome
					== OriginalOrderSortOutcome::StockEquivalentLegacy)
				{
					// The stock-equivalent arrays are already final.  Leave the
					// anchor marker clear so RenderAlphaGeometry applies only the
					// existing conservative multi-slot compatibility repair.
					return;
				}
			}
			else if (haveAnchorCandidate)
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					SortedOriginalOrderAnchorPredecessorFallback);
				RecordFreeTypePerf(
					FreeTypePerfCounter::SortedOriginalOrderAnchorFallback);
			}

			if (originalSort)
				originalSort(accumulator);
			else
				accumulator->Sort();
		}

		void __fastcall NativeA8RenderAlphaGeometry(BSShaderAccumulator* accumulator, void*)
		{
			A8State& state = State();
			if (!state.originalRenderAlphaGeometry)
				return;

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
				state.originalRenderAlphaGeometry(accumulator);
				InvalidateNativeA8SortedShaderState();
				--scratch.nestedBypassDepth;
				scratch.active = restoreActive;
				return;
			}

			if (accumulator
				&& accumulator->eRenderMode == BSShaderManager::BSSM_RENDER_TILES
				&& accumulator->m_iNumItems > 0 && accumulator->m_ppkItems)
			{
				const size_t itemCount = static_cast<size_t>(
					accumulator->m_iNumItems);
				const bool originalOrderAnchored =
					scratch.originalOrderAnchorAccumulator == accumulator
					&& scratch.originalOrderAnchorCycle
						== scratch.registrationCycle;
				if (!originalOrderAnchored)
				{
					// Compatibility path when the strict pre-sort proof failed or the
					// call-site patch was unavailable. It preserves correctness, but pays
					// the older post-sort snapshot/hash/repair cost.
					RestoreFreeTypeMixedEqualDepthPainterOrder(
						scratch, accumulator);
				}
				scratch.frameEntries.clear();
				scratch.fallbackMetadataOwners.clear();
				scratch.payloadTemplates.clear();
				scratch.virtualStockGroups.clear();
				scratch.virtualStockSingletons.clear();
				const bool haveRegisteredMetadata =
					scratch.pendingAccumulator == accumulator;
				if (!haveRegisteredMetadata)
					ClearPendingRegistrations(scratch);
				NativePreflightFrameContext preflightContext;
				preflightContext.accumulatorCurrent =
					IsNativeA8AccumulatorHookCurrent();
				preflightContext.immediateRouteCurrent =
					IsA8RenderPassImmediatelyHookCurrent();
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
								&& (registration.metadata->backend
										== FreeTypeShapeBackend::
											VirtualStockNative
									|| registration.metadata->backend
										== FreeTypeShapeBackend::
											VirtualStockSingleton)
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
				scratch.virtualStockSingletons.reserve(
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
						if (registration.metadata->backend
							== FreeTypeShapeBackend::VirtualStockSingleton)
						{
							VirtualStockSingletonState* singleton =
								GetVirtualStockSingletonState(
									*registration.metadata);
							if (!singleton)
								continue;
							const VirtualStockFrameMode mode =
								singleton->frameMode.load(
									std::memory_order_acquire);
							if (mode == VirtualStockFrameMode::Culled
								|| mode == VirtualStockFrameMode::Retired
								|| singleton->preflightValidationToken
									== frameValidationToken)
							{
								continue;
							}
							if (singleton->registrationAccumulator
									!= accumulator
								|| singleton->registrationCycle
									!= scratch.registrationCycle
								|| !singleton->registrationContiguous
								|| singleton->duplicateRegistration
								|| singleton->registeredSlotCount != 1)
							{
								RestoreVirtualStockSingletonToFacade(
									*registration.metadata,
									NativeA8FallbackReason::PropertySync);
								RecordFreeTypePerf(FreeTypePerfCounter::
									VirtualStockFallbackNoncontiguous);
								continue;
							}
							NativeA8ShapePayload& payload =
								registration.metadata->nativePayload;
							const NativeA8FallbackReason preflight =
								PreflightNativeGroupImpl(
									registration.facade,
									*registration.metadata, payload,
									&preflightContext,
									&singleton->useCompositeTopology);
							if (preflight != NativeA8FallbackReason::None)
							{
								RestoreVirtualStockSingletonToFacade(
									*registration.metadata, preflight);
								continue;
							}
							singleton->preflightValidationToken =
								frameValidationToken;
							scratch.virtualStockSingletons.push_back(
								registration.metadata.get());
							if (payload.payloadTemplate
								&& payload.preparedGeneration == generation
								&& InsertUniquePayload(
									scratch, payload.payloadTemplate))
							{
								RecordFreeTypePerf(FreeTypePerfCounter::
									SortedFramePayload);
							}
							continue;
						}
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
					VirtualStockSingletonState* virtualStockSingleton =
						nullptr;
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
					else if (entry.metadata
						&& entry.metadata->backend
							== FreeTypeShapeBackend::VirtualStockSingleton)
					{
						virtualStockSingleton =
							GetVirtualStockSingletonState(*entry.metadata);
						if (!virtualStockSingleton)
							continue;
					}
					entry.generation = generation;
					if (entry.metadata
						&& entry.metadata->nativePayload.buildComplete)
					{
						entry.payload = &entry.metadata->nativePayload;
						entry.visibilityCull =
							(entry.metadata->backend
									== FreeTypeShapeBackend::
										VirtualStockNative
								|| entry.metadata->backend
									== FreeTypeShapeBackend::
										VirtualStockSingleton)
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
							const bool* forcedCompositeTopology =
								virtualStockGroup
									? &virtualStockGroup->useCompositeTopology
									: virtualStockSingleton
										? &virtualStockSingleton->
											useCompositeTopology
										: nullptr;
							entry.preflightResult = PreflightNativeGroupImpl(
								facade, *entry.metadata, *entry.payload,
								&preflightContext,
								forcedCompositeTopology);
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
								else if (virtualStockSingleton)
								{
									virtualStockSingleton->
										preflightValidationToken =
											frameValidationToken;
									scratch.virtualStockSingletons.push_back(
										entry.metadata);
								}
							}
							else if (virtualStockGroup)
							{
								RestoreVirtualStockGroupToFacade(
									virtualStockGroup,
									entry.preflightResult);
							}
							else if (virtualStockSingleton)
							{
								RestoreVirtualStockSingletonToFacade(
									*entry.metadata,
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
				for (const A8ShapeMetadata* metadata
					: scratch.virtualStockSingletons)
				{
					VirtualStockSingletonState* singleton = metadata
						? GetVirtualStockSingletonState(*metadata) : nullptr;
					if (!singleton
						|| singleton->preflightValidationToken
							!= frameValidationToken)
					{
						if (metadata)
						{
							RestoreVirtualStockSingletonToFacade(*metadata,
								NativeA8FallbackReason::RuntimeFault);
						}
						continue;
					}
					PrepareVirtualStockSingletonForSortedFrame(*metadata,
						generation, preflightContext.atlasTextureEpoch,
						frameValidationToken);
				}
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
						const size_t virtualEntryCount =
							scratch.virtualStockSlots.size()
								+ scratch.virtualStockSingletons.size();
						const size_t ordinaryCapacityHint =
							scratch.frameEntries.size()
								>= virtualEntryCount
							? scratch.frameEntries.size()
								- virtualEntryCount
							: 0;
						ReserveNativeA8FrameCommandBuffer(
							ordinaryCapacityHint,
							scratch.virtualStockSingletons.size());
					}
					{
						FreeTypePerfScope commandBuildVirtual(
							FreeTypePerfPhase::CommandBuildVirtual);
						for (const A8ShapeMetadata* metadata
							: scratch.virtualStockSingletons)
						{
							if (metadata)
							{
								AddNativeA8FrameVirtualSingletonCommand(
									metadata);
							}
						}
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
							else if (entry.metadata->backend
								== FreeTypeShapeBackend::
									VirtualStockSingleton)
							{
								continue;
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
						if (g_bEnableFreeTypeFontCrossTextBatch)
						{
							BeginNativeA8CrossTextBatchFrame(
								static_cast<size_t>(accumulator->m_iNumItems),
								frameValidationToken);
						}
						if (g_bEnableFreeTypeFontCrossTextBatch
							&& accumulator->m_ppkItems
							&& accumulator->m_pfDepths)
						{
							for (SInt32 itemIndex =
								accumulator->m_iNumItems - 1;
								itemIndex >= 0; --itemIndex)
							{
								NiTriShape* geometry = IsFreeTypeFacade(
									accumulator->m_ppkItems[itemIndex])
									? static_cast<NiTriShape*>(
										accumulator->m_ppkItems[itemIndex])
									: nullptr;
								NativeA8CrossTextCommandKind kind =
									NativeA8CrossTextCommandKind::Barrier;
								const A8ShapeMetadata* metadata = nullptr;
								NativeA8ShapePayload* payload = nullptr;
								UInt32 commandIndex =
									kInvalidNativeA8CommandIndex;
								SortedFrameEntry* entry = nullptr;
								if (geometry)
								{
									const size_t entryIndex =
										LookupSortedFacade(scratch, geometry);
									if (entryIndex !=
											std::numeric_limits<size_t>::max()
										&& entryIndex
											< scratch.frameEntries.size())
									{
										entry = &scratch.frameEntries[entryIndex];
										metadata = entry->metadata;
										payload = entry->payload;
									}
								}
								if (entry && ++entry->crossTextOccurrences == 1
									&& entry->preflightResult
										== NativeA8FallbackReason::None
									&& entry->visibilityCull
										== NativeA8VisibilityCull::None
									&& metadata && payload)
								{
									if (metadata->backend
										== FreeTypeShapeBackend::
											VirtualStockSingleton)
									{
										VirtualStockSingletonState* singleton =
											GetVirtualStockSingletonState(*metadata);
										if (singleton
											&& singleton->frameMode.load(
												std::memory_order_acquire)
												== VirtualStockFrameMode::Direct
											&& singleton->commandValidationToken.load(
												std::memory_order_acquire)
												== frameValidationToken)
										{
											commandIndex = singleton->
												commandVirtualSinglePacketIndex.load(
													std::memory_order_acquire);
											if (commandIndex
												!= kInvalidNativeA8CommandIndex)
											{
												kind = NativeA8CrossTextCommandKind::
													VirtualSinglePacket;
											}
										}
									}
									else if (metadata->backend
										!= FreeTypeShapeBackend::VirtualStockNative
										&& entry->singlePacketCommandIndex
											!= kInvalidNativeA8CommandIndex)
									{
										kind = NativeA8CrossTextCommandKind::
											SinglePacket;
										commandIndex =
											entry->singlePacketCommandIndex;
									}
								}
								else if (entry
									&& entry->crossTextOccurrences > 1)
								{
									MarkNativeA8CrossTextBatchSequenceBarrier(
										entry->crossTextSequenceIndex);
									entry->crossTextSequenceIndex =
										kInvalidNativeA8CommandIndex;
								}

								const UInt32 sequenceIndex =
									AddNativeA8CrossTextBatchSequenceItem(
										kind, geometry, metadata, payload,
										commandIndex,
										accumulator->m_pfDepths[itemIndex]);
								if (entry && entry->crossTextOccurrences == 1
									&& kind
										!= NativeA8CrossTextCommandKind::Barrier)
								{
									entry->crossTextSequenceIndex = sequenceIndex;
								}
							}
						}
						if (g_bEnableFreeTypeFontCrossTextBatch)
							PrepareNativeA8CrossTextBatches();
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
				state.originalRenderAlphaGeometry(accumulator);
				EndA8SortedTileConstantOwnership();
				EndNativeA8SortedShaderBatch();
				if (g_bEnableFreeTypeFontCrossTextBatch)
					EndNativeA8CrossTextBatchFrame();
				EndNativeA8FrameCommandBuffer();
				EndNativeA8SortedRingFrame();
				ClearSortedFrame(scratch);
				return;
			}

			if (scratch.pendingAccumulator == accumulator)
			{
				ClearPendingRegistrations(scratch);
				RefreshSortedScratchMemory(scratch);
			}
			state.originalRenderAlphaGeometry(accumulator);
		}

		bool HookRenderAlphaGeometry()
		{
			A8State& state = State();
			const RenderAlphaGeometryFn hook = reinterpret_cast<RenderAlphaGeometryFn>(
				&NativeA8RenderAlphaGeometry);
			const RenderAlphaGeometryFn current = ReadRenderAlphaGeometryCallTarget();
			if (current == hook)
			{
				state.renderAlphaGeometryHookInstalled =
					state.originalRenderAlphaGeometry != nullptr;
				return state.renderAlphaGeometryHookInstalled;
			}
			if (!current)
			{
				if (state.renderAlphaGeometryHookInstalled)
				{
					state.renderAlphaGeometryHookInstalled = false;
					InvalidateAllVirtualStockBindings();
				}
				if (!state.loggedRenderAlphaGeometryHookConflict)
				{
					state.loggedRenderAlphaGeometryHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: BSShaderAccumulator::RenderAlphaGeometry call site is not CALL rel32; frame upload batching disabled");
				}
				return false;
			}
			if (state.renderAlphaGeometryHookInstalled)
			{
				state.renderAlphaGeometryHookInstalled = false;
				InvalidateAllVirtualStockBindings();
				if (!state.loggedRenderAlphaGeometryHookConflict)
				{
					state.loggedRenderAlphaGeometryHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: RenderAlphaGeometry frame hook was replaced; per-shape upload fallback remains active");
				}
				return false;
			}
			if (reinterpret_cast<UInt32>(current) != kStockRenderAlphaGeometry)
			{
				if (!state.loggedRenderAlphaGeometryHookConflict)
				{
					state.loggedRenderAlphaGeometryHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: RenderAlphaGeometry call site already has a non-stock target=%p; frame upload batching left disabled",
						current);
				}
				return false;
			}

			state.originalRenderAlphaGeometry = current;
			WriteRelCall(kRenderAlphaGeometryCallSite, hook);
			state.renderAlphaGeometryHookInstalled =
				ReadRenderAlphaGeometryCallTarget() == hook;
			if (!state.renderAlphaGeometryHookInstalled)
			{
				WriteRelCall(kRenderAlphaGeometryCallSite, current);
				state.originalRenderAlphaGeometry = nullptr;
				return false;
			}
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: installed RenderAlphaGeometry frame route original=%p stock=1",
					current);
			}
			return true;
		}

		bool HookTileSortAnchor()
		{
			static constexpr UInt8 kStockBytes[kTileSortDispatchPatchSize] = {
				0xC7, 0x46, 0x18, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xD2
			};
			A8State& state = State();
			if (IsTileSortAnchorHookCurrent())
			{
				state.tileSortAnchorHookInstalled = true;
				return true;
			}
			if (state.tileSortAnchorHookInstalled)
			{
				state.tileSortAnchorHookInstalled = false;
				if (!state.loggedTileSortAnchorHookConflict)
				{
					state.loggedTileSortAnchorHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: original-order sort anchor hook was replaced; post-sort compatibility repair remains active");
				}
				return false;
			}
			if (!hook_identity::IsAccessibleRegion(kTileSortDispatchPatch,
				kTileSortDispatchPatchSize, true)
				|| std::memcmp(reinterpret_cast<const void*>(
					kTileSortDispatchPatch), kStockBytes,
					sizeof(kStockBytes)) != 0)
			{
				if (!state.loggedTileSortAnchorHookConflict)
				{
					state.loggedTileSortAnchorHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: FinishAccumulating_Tiles sort dispatch bytes are not stock; original-order anchor left disabled site=%08X",
						kTileSortDispatchPatch);
				}
				return false;
			}

			const std::intptr_t displacementWide =
				static_cast<std::intptr_t>(reinterpret_cast<SIZE_T>(
					&NativeA8SortAlphaGeometry))
				- static_cast<std::intptr_t>(kTileSortDispatchPatch + 5u);
			if (displacementWide < std::numeric_limits<SInt32>::min()
				|| displacementWide > std::numeric_limits<SInt32>::max())
			{
				if (!state.loggedTileSortAnchorHookConflict)
				{
					state.loggedTileSortAnchorHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: original-order sort anchor target is outside rel32 range site=%08X target=%p",
						kTileSortDispatchPatch, &NativeA8SortAlphaGeometry);
				}
				return false;
			}
			const SInt32 displacement = static_cast<SInt32>(displacementWide);
			UInt8 patch[kTileSortDispatchPatchSize] = {
				0xE8, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90
			};
			std::memcpy(patch + 1u, &displacement, sizeof(displacement));
			SafeWriteBuf(kTileSortDispatchPatch, patch, sizeof(patch));
			FlushInstructionCache(GetCurrentProcess(),
				reinterpret_cast<const void*>(kTileSortDispatchPatch),
				sizeof(patch));
			state.tileSortAnchorHookInstalled = IsTileSortAnchorHookCurrent();
			if (!state.tileSortAnchorHookInstalled)
			{
				if (hook_identity::IsAccessibleRegion(kTileSortDispatchPatch,
					kTileSortDispatchPatchSize, true)
					&& std::memcmp(reinterpret_cast<const void*>(
						kTileSortDispatchPatch), patch, sizeof(patch)) == 0)
				{
					SafeWriteBuf(kTileSortDispatchPatch, kStockBytes,
						sizeof(kStockBytes));
					FlushInstructionCache(GetCurrentProcess(),
						reinterpret_cast<const void*>(kTileSortDispatchPatch),
						sizeof(kStockBytes));
				}
				return false;
			}
			state.loggedTileSortAnchorHookConflict = false;
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: installed original-order sort anchor site=%08X stock=1",
					kTileSortDispatchPatch);
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
		view.crossTextSequenceIndex = entry.crossTextSequenceIndex;
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
		if (!IsTileRegisterObjectSlotWritable())
		{
			if (!s_loggedTileRegisterObjectSlotUnavailable)
			{
				s_loggedTileRegisterObjectSlotUnavailable = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile RegisterObject dispatch hook skipped; function-table slot is unavailable entry=%08X",
					kTileRegisterObjectFunctionEntry);
			}
			return false;
		}

		const TileRegisterObjectFn hook = &NativeA8RegisterObject;
		const TileRegisterObjectFn current = ReadTileRegisterObjectTarget();
		if (current == hook)
		{
			if (!s_originalTileRegisterObject.load(
				std::memory_order_acquire))
			{
				if (!s_loggedTileRegisterObjectConflict)
				{
					s_loggedTileRegisterObjectConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: Tile RegisterObject dispatch points at tNVSE without a predecessor; native route disabled");
				}
				return false;
			}
			if (HookRenderAlphaGeometry())
				HookTileSortAnchor();
			return true;
		}

		if (!current)
		{
			if (g_bEnableFreeTypeFontRenderingLog
				&& !s_loggedTileRegisterObjectSlotUnavailable)
			{
				s_loggedTileRegisterObjectSlotUnavailable = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile RegisterObject dispatch is not initialized yet; installation will retry entry=%08X",
					kTileRegisterObjectFunctionEntry);
			}
			return false;
		}

		if (!hook_identity::IsExecutableTarget(
			reinterpret_cast<SIZE_T>(current)))
		{
			if (!s_loggedTileRegisterObjectConflict)
			{
				s_loggedTileRegisterObjectConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile RegisterObject dispatch hook skipped; target is not executable target=%p entry=%08X",
					current, kTileRegisterObjectFunctionEntry);
				InvalidateAllVirtualStockBindings();
			}
			return false;
		}

		// A stock reset, or restoration of the predecessor we previously owned,
		// is safe to republish over.  Any other target observed after installation
		// may be a successor that already chains to tNVSE; never reassert over it or
		// the two hooks could recurse through each other.
		const TileRegisterObjectFn installedPredecessor =
			s_originalTileRegisterObject.load(std::memory_order_acquire);
		if (installedPredecessor
			&& current != reinterpret_cast<TileRegisterObjectFn>(
				kStockTileRegisterObject)
			&& current != installedPredecessor)
		{
			if (!s_loggedTileRegisterObjectConflict)
			{
				s_loggedTileRegisterObjectConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile RegisterObject dispatch was replaced by a successor target=%p; tNVSE will not reassert ownership",
					current);
				InvalidateAllVirtualStockBindings();
			}
			return false;
		}

		if (reinterpret_cast<SIZE_T>(current) != kStockTileRegisterObject
			&& g_bEnableFreeTypeFontRenderingLog)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_native: chaining pre-existing Tile RegisterObject dispatch target=%p stock=%08X",
				current, kStockTileRegisterObject);
		}

		const TileRegisterObjectFn previousOriginal = installedPredecessor;
		s_originalTileRegisterObject.store(current, std::memory_order_release);
		if (!PublishTileRegisterObjectHook(current))
		{
			// The slot changed after validation.  Preserve an older predecessor if
			// one may still be reached through a successor chain; otherwise allow a
			// clean retry against the newly observed initial target.
			s_originalTileRegisterObject.store(previousOriginal,
				std::memory_order_release);
			if (!s_loggedTileRegisterObjectConflict)
			{
				s_loggedTileRegisterObjectConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile RegisterObject dispatch changed during publication; hook not installed expected=%p actual=%p",
					current, ReadTileRegisterObjectTarget());
			}
			return false;
		}

		const bool accumulatorReady = IsNativeA8AccumulatorHookCurrent();
		if (!accumulatorReady)
		{
			// Do not overwrite a successor that raced the readback.  It may already
			// retain NativeA8RegisterObject as its predecessor, so the saved target
			// must remain valid even though direct ownership was lost.
			if (!s_loggedTileRegisterObjectConflict)
			{
				s_loggedTileRegisterObjectConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile RegisterObject dispatch ownership changed immediately after publication actual=%p",
					ReadTileRegisterObjectTarget());
				InvalidateAllVirtualStockBindings();
			}
			return false;
		}

		s_loggedTileRegisterObjectConflict = false;
		s_loggedTileRegisterObjectSlotUnavailable = false;
		if (HookRenderAlphaGeometry())
			HookTileSortAnchor();
		if (g_bEnableFreeTypeFontRenderingLog)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_native: installed Tile RegisterObject dispatch route entry=%08X predecessor=%p stock=%u",
				kTileRegisterObjectFunctionEntry, current,
				reinterpret_cast<SIZE_T>(current)
					== kStockTileRegisterObject ? 1u : 0u);
		}
		return true;
	}

	bool IsNativeA8AccumulatorHookCurrent()
	{
		return ReadTileRegisterObjectTarget() == &NativeA8RegisterObject
			&& s_originalTileRegisterObject.load(std::memory_order_acquire)
				!= nullptr;
	}

	bool IsNativeA8RenderAlphaGeometryHookCurrent()
	{
		return State().originalRenderAlphaGeometry
			&& ReadRenderAlphaGeometryCallTarget()
				== reinterpret_cast<RenderAlphaGeometryFn>(
					&NativeA8RenderAlphaGeometry);
	}

	bool IsNativeA8RegistrationHookChainCurrent()
	{
		return IsNativeA8AccumulatorHookCurrent()
			&& IsNativeA8RenderAlphaGeometryHookCurrent()
			&& IsTileSortAnchorHookCurrent();
	}
}

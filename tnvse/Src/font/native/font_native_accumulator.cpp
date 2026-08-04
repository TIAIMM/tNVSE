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
		inline constexpr UInt32 kRegisterRouteSampleRate = 256;

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
		const hook_identity::Rel32InstructionImage
			s_renderAlphaGeometryHookImage =
				hook_identity::MakeRel32InstructionImage(
					kRenderAlphaGeometryCallSite,
					hook_identity::Rel32Opcode::Call,
					reinterpret_cast<SIZE_T>(
						&NativeA8RenderAlphaGeometry));
		const hook_identity::Rel32InstructionImage s_tileSortHookImage =
			hook_identity::MakeRel32InstructionImage(
				kTileSortDispatchPatch, hook_identity::Rel32Opcode::Call,
				reinterpret_cast<SIZE_T>(&NativeA8SortAlphaGeometry));

		struct SortedFrameEntry
		{
			NiTriShape* facade = nullptr;
			// metadataOwners holds the batch-acquired shared owner until the stock
			// traversal completes; entries use stable non-owning views.
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

		enum class ExecutionSkeletonClass : UInt8
		{
			Barrier = 0,
			NativeFacade,
		};

		struct ExecutionSequenceSkeletonItem
		{
			NiTriShape* facade = nullptr;
			UInt32 depthBits = 0;
			UInt32 originalOrdinal = 0;
			ExecutionSkeletonClass classification =
				ExecutionSkeletonClass::Barrier;
		};
		static_assert(sizeof(ExecutionSequenceSkeletonItem) == 16);

		struct SortedPayloadScratch
		{
			BSShaderAccumulator* sourceAccumulator = nullptr;
			// Exact predecessor-accepted AddTail order, captured once at Sort. It is
			// non-owning; metadataOwners below keep the FreeType side alive through
			// the matching RenderAlphaGeometry traversal.
			std::vector<NiGeometry*> acceptedTileRegistrations;
			std::vector<NiTriShape*> metadataShapes;
			std::vector<A8ShapeMetadataPtr> metadataOwners;
			std::vector<UInt32> metadataLookup;
			std::vector<UInt32> sourceOccurrenceCounts;
			std::vector<SInt32> sourceFirstIndices;
			std::vector<UInt32> sortedOccurrenceCounts;
			std::vector<SInt32> sortedFirstIndices;
			std::vector<UInt32> acceptedRegistrationLookup;
			std::vector<UInt32> acceptedOccurrenceHeads;
			std::vector<UInt32> acceptedOccurrenceTails;
			std::vector<UInt32> acceptedOccurrenceNext;
			std::vector<UInt32> sortedRegistrationOrdinals;
			std::vector<UInt32> sortedItemIndicesByRegistrationOrdinal;
			std::vector<UInt32> registrationBlockOrdinals;
			std::vector<StableTileTieItem> stableTieItems;
			std::vector<UInt8> originalOrderFreeTypeFlags;
			std::vector<UInt32> originalOrderDesiredOrdinals;
			std::vector<UInt32> originalOrderRunIds;
			std::vector<OriginalOrderAnchorRun> originalOrderRuns;
			std::vector<NiGeometry*> originalOrderOutput;
			std::vector<NiTriShape*> frameCandidates;
			std::vector<ExecutionSequenceSkeletonItem> executionSkeleton;
			BSShaderAccumulator* executionSkeletonAccumulator = nullptr;
			const void* executionSkeletonItems = nullptr;
			const void* executionSkeletonDepths = nullptr;
			UInt64 executionSkeletonValidationToken = 0;
			std::vector<SortedFrameEntry> frameEntries;
			std::vector<UInt32> facadeLookup;
			std::vector<NativeA8PayloadTemplatePtr> payloadTemplates;
			std::vector<UInt32> payloadLookup;
			std::vector<std::shared_ptr<VirtualStockShapeGroup>>
				virtualStockGroups;
			std::vector<const A8ShapeMetadata*> virtualStockSingletons;
			CpuMemoryLease cpuMemory;
			UInt32 nestedBypassDepth = 0;
			UInt64 nestedTraversalSerial = 1;
			UInt64 sortCycle = 1;
			UInt64 nextValidationToken = 0;
			UInt64 activeValidationToken = 0;
			BSShaderAccumulator* originalOrderAnchorAccumulator = nullptr;
			UInt64 originalOrderAnchorCycle = 0;
			OriginalOrderAnchorFailure sourceTopologyFailure =
				OriginalOrderAnchorFailure::Gate;
			bool active = false;
		};

		thread_local SortedPayloadScratch s_sortedPayloadScratch;

		struct ThinRegistrationDiagnostics
		{
			UInt64 calls = 0;
			UInt64 samples = 0;
			UInt64 fastForward = 0;
			UInt64 hookMismatch = 0;
			UInt64 slowAudits = 0;
			UInt64 suppressed = 0;
			UInt64 metadataBatches = 0;
			UInt64 metadataShapes = 0;
			UInt64 metadataMissing = 0;
			UInt64 sourceFallback = 0;
			UInt64 singletonTopology = 0;
			UInt64 singletonFallback = 0;
			UInt64 groupTopology = 0;
			UInt64 groupFallback = 0;
			UInt64 occurrenceFallback = 0;
		};

		thread_local ThinRegistrationDiagnostics s_thinRegistrationDiagnostics;
		std::atomic<UInt64> s_thinRejectedHookFingerprint = 0;
		std::mutex s_thinHookAuditMutex;

		UInt64 BuildThinHookFingerprintUnchecked()
		{
			UInt64 hash = 1469598103934665603ull;
			auto mix = [&](const void* bytes, size_t size)
			{
				const UInt8* cursor = static_cast<const UInt8*>(bytes);
				for (size_t index = 0; index < size; ++index)
				{
					hash ^= cursor[index];
					hash *= 1099511628211ull;
				}
			};
			mix(reinterpret_cast<const void*>(kTileRegisterObjectFunctionEntry),
				sizeof(void*));
			mix(reinterpret_cast<const void*>(kRenderAlphaGeometryCallSite), 5);
			mix(reinterpret_cast<const void*>(kTileSortDispatchPatch),
				kTileSortDispatchPatchSize);
			mix(reinterpret_cast<const void*>(kRenderPassImmediatelyCallSite), 5);
			const A8State& state = State();
			const TileRegisterObjectFn predecessor =
				s_originalTileRegisterObject.load(std::memory_order_relaxed);
			mix(&predecessor, sizeof(predecessor));
			mix(&state.originalRenderAlphaGeometry,
				sizeof(state.originalRenderAlphaGeometry));
			mix(&state.originalRenderPassImmediately,
				sizeof(state.originalRenderPassImmediately));
			return hash ? hash : 1;
		}

		class ThinRegistrationSampleScope
		{
		public:
			explicit ThinRegistrationSampleScope(bool sample)
				: m_start(sample ? BeginFreeTypePerfSample() : 0)
			{
			}

			~ThinRegistrationSampleScope()
			{
				if (m_start)
				{
					EndFreeTypePerfSample(
						FreeTypePerfPhase::RegisterRoute, m_start);
				}
			}

		private:
			SInt64 m_start = 0;
		};

		void FlushThinRegistrationDiagnostics()
		{
			if (!g_bEnableFreeTypeFontRenderingLog)
			{
				s_thinRegistrationDiagnostics = {};
				return;
			}
			ThinRegistrationDiagnostics values;
			std::swap(values, s_thinRegistrationDiagnostics);
			auto record = [](FreeTypePerfCounter counter, UInt64 value)
			{
				if (value)
					RecordFreeTypePerf(counter, value);
			};
			record(FreeTypePerfCounter::ThinRegistrationCall, values.calls);
			record(FreeTypePerfCounter::ThinRegistrationTimingSample,
				values.samples);
			record(FreeTypePerfCounter::ThinRegistrationFastForward,
				values.fastForward);
			record(FreeTypePerfCounter::ThinRegistrationHookMismatch,
				values.hookMismatch);
			record(FreeTypePerfCounter::ThinRegistrationSlowAudit,
				values.slowAudits);
			record(FreeTypePerfCounter::ThinRegistrationSuppressed,
				values.suppressed);
			record(FreeTypePerfCounter::ThinRegistrationMetadataBatch,
				values.metadataBatches);
			record(FreeTypePerfCounter::ThinRegistrationMetadataShape,
				values.metadataShapes);
			record(FreeTypePerfCounter::ThinRegistrationMetadataMissing,
				values.metadataMissing);
			record(FreeTypePerfCounter::ThinRegistrationSourceFallback,
				values.sourceFallback);
			record(FreeTypePerfCounter::ThinRegistrationSingletonTopology,
				values.singletonTopology);
			record(FreeTypePerfCounter::ThinRegistrationSingletonFallback,
				values.singletonFallback);
			record(FreeTypePerfCounter::ThinRegistrationGroupTopology,
				values.groupTopology);
			record(FreeTypePerfCounter::ThinRegistrationGroupFallback,
				values.groupFallback);
			record(FreeTypePerfCounter::ThinRegistrationOccurrenceFallback,
				values.occurrenceFallback);
		}

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
				scratch.acceptedTileRegistrations.capacity()
					* sizeof(NiGeometry*)
				+ scratch.metadataShapes.capacity() * sizeof(NiTriShape*)
				+ scratch.metadataOwners.capacity()
					* sizeof(A8ShapeMetadataPtr)
				+ scratch.metadataLookup.capacity() * sizeof(UInt32)
				+ scratch.sourceOccurrenceCounts.capacity() * sizeof(UInt32)
				+ scratch.sourceFirstIndices.capacity() * sizeof(SInt32)
				+ scratch.sortedOccurrenceCounts.capacity() * sizeof(UInt32)
				+ scratch.sortedFirstIndices.capacity() * sizeof(SInt32)
				+ scratch.acceptedRegistrationLookup.capacity()
					* sizeof(UInt32)
				+ scratch.acceptedOccurrenceHeads.capacity() * sizeof(UInt32)
				+ scratch.acceptedOccurrenceTails.capacity() * sizeof(UInt32)
				+ scratch.acceptedOccurrenceNext.capacity() * sizeof(UInt32)
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
				+ scratch.frameCandidates.capacity() * sizeof(NiTriShape*)
				+ scratch.executionSkeleton.capacity()
					* sizeof(ExecutionSequenceSkeletonItem)
				+ scratch.frameEntries.capacity() * sizeof(SortedFrameEntry)
				+ scratch.facadeLookup.capacity() * sizeof(UInt32)
				+ scratch.payloadTemplates.capacity()
					* sizeof(NativeA8PayloadTemplatePtr)
				+ scratch.payloadLookup.capacity() * sizeof(UInt32)
				+ scratch.virtualStockGroups.capacity()
					* sizeof(std::shared_ptr<VirtualStockShapeGroup>)
				+ scratch.virtualStockSingletons.capacity()
					* sizeof(const A8ShapeMetadata*);
			scratch.cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata, bytes);
		}

		void ClearSortedFrame(SortedPayloadScratch& scratch)
		{
			FlushThinRegistrationDiagnostics();
			if (g_bEnableFreeTypeFontCrossTextBatch)
				EndNativeA8CrossTextBatchFrame();
			EndNativeA8FrameCommandBuffer();
			scratch.active = false;
			scratch.activeValidationToken = 0;
			scratch.frameCandidates.clear();
			scratch.executionSkeleton.clear();
			scratch.executionSkeletonAccumulator = nullptr;
			scratch.executionSkeletonItems = nullptr;
			scratch.executionSkeletonDepths = nullptr;
			scratch.executionSkeletonValidationToken = 0;
			scratch.frameEntries.clear();
			scratch.payloadTemplates.clear();
			scratch.virtualStockGroups.clear();
			scratch.virtualStockSingletons.clear();
			scratch.sourceAccumulator = nullptr;
			scratch.acceptedTileRegistrations.clear();
			scratch.metadataShapes.clear();
			scratch.metadataOwners.clear();
			scratch.sourceOccurrenceCounts.clear();
			scratch.sourceFirstIndices.clear();
			scratch.sortedOccurrenceCounts.clear();
			scratch.sortedFirstIndices.clear();
			scratch.originalOrderAnchorAccumulator = nullptr;
			scratch.originalOrderAnchorCycle = 0;
			scratch.sourceTopologyFailure = OriginalOrderAnchorFailure::Gate;
			if (++scratch.sortCycle == 0)
				++scratch.sortCycle;
			if (scratch.acceptedTileRegistrations.capacity() > 8192)
			{
				std::vector<NiGeometry*>().swap(
					scratch.acceptedTileRegistrations);
			}
			if (scratch.metadataShapes.capacity() > 8192)
				std::vector<NiTriShape*>().swap(scratch.metadataShapes);
			if (scratch.metadataOwners.capacity() > 8192)
				std::vector<A8ShapeMetadataPtr>().swap(scratch.metadataOwners);
			if (scratch.metadataLookup.capacity() > 16384)
				std::vector<UInt32>().swap(scratch.metadataLookup);
			if (scratch.sourceOccurrenceCounts.capacity() > 8192)
				std::vector<UInt32>().swap(scratch.sourceOccurrenceCounts);
			if (scratch.sourceFirstIndices.capacity() > 8192)
				std::vector<SInt32>().swap(scratch.sourceFirstIndices);
			if (scratch.sortedOccurrenceCounts.capacity() > 8192)
				std::vector<UInt32>().swap(scratch.sortedOccurrenceCounts);
			if (scratch.sortedFirstIndices.capacity() > 8192)
				std::vector<SInt32>().swap(scratch.sortedFirstIndices);
			if (scratch.acceptedRegistrationLookup.capacity() > 16384)
			{
				std::vector<UInt32>().swap(
					scratch.acceptedRegistrationLookup);
			}
			if (scratch.acceptedOccurrenceHeads.capacity() > 16384)
				std::vector<UInt32>().swap(scratch.acceptedOccurrenceHeads);
			if (scratch.acceptedOccurrenceTails.capacity() > 16384)
				std::vector<UInt32>().swap(scratch.acceptedOccurrenceTails);
			if (scratch.acceptedOccurrenceNext.capacity() > 8192)
				std::vector<UInt32>().swap(scratch.acceptedOccurrenceNext);
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
			if (scratch.frameEntries.capacity() > 8192)
				std::vector<SortedFrameEntry>().swap(scratch.frameEntries);
			if (scratch.frameCandidates.capacity() > 8192)
				std::vector<NiTriShape*>().swap(scratch.frameCandidates);
			if (scratch.executionSkeleton.capacity() > 8192)
			{
				std::vector<ExecutionSequenceSkeletonItem>().swap(
					scratch.executionSkeleton);
			}
			if (scratch.facadeLookup.capacity() > 16384)
				std::vector<UInt32>().swap(scratch.facadeLookup);
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
			RefreshSortedScratchMemory(scratch);
			if (IsCpuMemoryBudgetExceeded())
			{
				std::vector<NiGeometry*>().swap(
					scratch.acceptedTileRegistrations);
				std::vector<NiTriShape*>().swap(scratch.metadataShapes);
				std::vector<A8ShapeMetadataPtr>().swap(scratch.metadataOwners);
				std::vector<UInt32>().swap(scratch.metadataLookup);
				std::vector<UInt32>().swap(scratch.sourceOccurrenceCounts);
				std::vector<SInt32>().swap(scratch.sourceFirstIndices);
				std::vector<UInt32>().swap(scratch.sortedOccurrenceCounts);
				std::vector<SInt32>().swap(scratch.sortedFirstIndices);
				std::vector<UInt32>().swap(
					scratch.acceptedRegistrationLookup);
				std::vector<UInt32>().swap(scratch.acceptedOccurrenceHeads);
				std::vector<UInt32>().swap(scratch.acceptedOccurrenceTails);
				std::vector<UInt32>().swap(scratch.acceptedOccurrenceNext);
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
				std::vector<SortedFrameEntry>().swap(scratch.frameEntries);
				std::vector<NiTriShape*>().swap(scratch.frameCandidates);
				std::vector<ExecutionSequenceSkeletonItem>().swap(
					scratch.executionSkeleton);
				std::vector<UInt32>().swap(scratch.facadeLookup);
				std::vector<NativeA8PayloadTemplatePtr>().swap(
					scratch.payloadTemplates);
				std::vector<UInt32>().swap(scratch.payloadLookup);
				std::vector<std::shared_ptr<VirtualStockShapeGroup>>().swap(
					scratch.virtualStockGroups);
				std::vector<const A8ShapeMetadata*>().swap(
					scratch.virtualStockSingletons);
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

		bool IsFreeTypeFacade(const NiGeometry* geometry);

		size_t LookupMetadataShapeIndex(const SortedPayloadScratch& scratch,
			const NiTriShape* shape)
		{
			if (!shape || scratch.metadataLookup.empty())
				return std::numeric_limits<size_t>::max();
			const size_t mask = scratch.metadataLookup.size() - 1u;
			size_t slot = HashPointer(shape) & mask;
			for (size_t probe = 0; probe < scratch.metadataLookup.size(); ++probe)
			{
				const UInt32 stored = scratch.metadataLookup[slot];
				if (!stored)
					return std::numeric_limits<size_t>::max();
				const size_t index = static_cast<size_t>(stored - 1u);
				if (index < scratch.metadataShapes.size()
					&& scratch.metadataShapes[index] == shape)
				{
					return index;
				}
				slot = (slot + 1u) & mask;
			}
			return std::numeric_limits<size_t>::max();
		}

		const A8ShapeMetadata* FindBatchedMetadata(
			const SortedPayloadScratch& scratch, const NiTriShape* shape)
		{
			const size_t index = LookupMetadataShapeIndex(scratch, shape);
			return index < scratch.metadataOwners.size()
				? scratch.metadataOwners[index].get() : nullptr;
		}

		void RebuildMetadataBatch(SortedPayloadScratch& scratch)
		{
			PrepareLookup(scratch.metadataLookup, scratch.metadataShapes.size());
			const size_t mask = scratch.metadataLookup.size() - 1u;
			for (size_t index = 0; index < scratch.metadataShapes.size(); ++index)
			{
				NiTriShape* shape = scratch.metadataShapes[index];
				size_t slot = HashPointer(shape) & mask;
				while (scratch.metadataLookup[slot])
					slot = (slot + 1u) & mask;
				scratch.metadataLookup[slot] = static_cast<UInt32>(index + 1u);
			}

			AcquireA8ShapeMetadataBatch(
				scratch.metadataShapes, scratch.metadataOwners);
			++s_thinRegistrationDiagnostics.metadataBatches;
			s_thinRegistrationDiagnostics.metadataShapes +=
				static_cast<UInt64>(scratch.metadataShapes.size());
			for (const A8ShapeMetadataPtr& owner : scratch.metadataOwners)
			{
				if (!owner)
					++s_thinRegistrationDiagnostics.metadataMissing;
			}

			scratch.sourceOccurrenceCounts.assign(
				scratch.metadataShapes.size(), 0);
			scratch.sourceFirstIndices.assign(
				scratch.metadataShapes.size(), -1);
			for (size_t ordinal = 0;
				ordinal < scratch.acceptedTileRegistrations.size(); ++ordinal)
			{
				NiGeometry* geometry = scratch.acceptedTileRegistrations[ordinal];
				if (!IsFreeTypeFacade(geometry))
					continue;
				const size_t index = LookupMetadataShapeIndex(scratch,
					static_cast<NiTriShape*>(geometry));
				if (index >= scratch.sourceOccurrenceCounts.size())
					continue;
				if (!scratch.sourceOccurrenceCounts[index])
					scratch.sourceFirstIndices[index] =
						static_cast<SInt32>(ordinal);
				++scratch.sourceOccurrenceCounts[index];
			}
		}

		bool CaptureSourceRegistrationOrder(SortedPayloadScratch& scratch,
			BSShaderAccumulator* accumulator)
		{
			scratch.sourceAccumulator = nullptr;
			scratch.acceptedTileRegistrations.clear();
			scratch.metadataShapes.clear();
			scratch.metadataOwners.clear();
			scratch.sourceTopologyFailure = OriginalOrderAnchorFailure::Gate;
			if (!accumulator || scratch.active || scratch.nestedBypassDepth)
				return false;

			const size_t itemCount = accumulator->m_kItems.GetSize();
			if (!itemCount || itemCount > kMaximumStableTieItems)
			{
				++s_thinRegistrationDiagnostics.sourceFallback;
				return false;
			}
			scratch.acceptedTileRegistrations.reserve(itemCount);
			PrepareLookup(scratch.metadataLookup, itemCount);
			const size_t metadataMask = scratch.metadataLookup.size() - 1u;
			NiTListIterator position = accumulator->m_kItems.GetHeadPos();
			while (position
				&& scratch.acceptedTileRegistrations.size() < itemCount)
			{
				NiGeometry* geometry = accumulator->m_kItems.GetNext(position);
				if (!geometry)
					break;
				scratch.acceptedTileRegistrations.push_back(geometry);
				if (IsFreeTypeFacade(geometry))
				{
					NiTriShape* facade = static_cast<NiTriShape*>(geometry);
					size_t slot = HashPointer(facade) & metadataMask;
					for (size_t probe = 0;
						probe < scratch.metadataLookup.size(); ++probe)
					{
						const UInt32 stored = scratch.metadataLookup[slot];
						if (!stored)
						{
							scratch.metadataShapes.push_back(facade);
							scratch.metadataLookup[slot] =
								static_cast<UInt32>(
									scratch.metadataShapes.size());
							break;
						}
						const size_t existing = static_cast<size_t>(stored - 1u);
						if (existing < scratch.metadataShapes.size()
							&& scratch.metadataShapes[existing] == facade)
						{
							break;
						}
						slot = (slot + 1u) & metadataMask;
					}
				}
			}
			if (position || scratch.acceptedTileRegistrations.size() != itemCount)
			{
				scratch.acceptedTileRegistrations.clear();
				scratch.metadataShapes.clear();
				++s_thinRegistrationDiagnostics.sourceFallback;
				return false;
			}
			if (++scratch.sortCycle == 0)
				++scratch.sortCycle;
			scratch.sourceAccumulator = accumulator;
			RebuildMetadataBatch(scratch);
			return true;
		}

		void EnsureSortedMetadataCoverage(SortedPayloadScratch& scratch,
			const BSShaderAccumulator& accumulator)
		{
			bool rebuild = false;
			for (SInt32 index = accumulator.m_iNumItems - 1; index >= 0; --index)
			{
				NiGeometry* geometry = accumulator.m_ppkItems[index];
				if (!IsFreeTypeFacade(geometry))
					continue;
				NiTriShape* facade = static_cast<NiTriShape*>(geometry);
				if (LookupMetadataShapeIndex(scratch, facade)
					!= std::numeric_limits<size_t>::max())
				{
					continue;
				}
				if (std::find(scratch.metadataShapes.begin(),
					scratch.metadataShapes.end(), facade)
					== scratch.metadataShapes.end())
				{
					scratch.metadataShapes.push_back(facade);
					rebuild = true;
				}
			}
			if (rebuild || scratch.metadataOwners.size()
				!= scratch.metadataShapes.size())
			{
				RebuildMetadataBatch(scratch);
			}
		}

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
			scratch.registrationBlockOrdinals.clear();
			scratch.originalOrderFreeTypeFlags.assign(itemCount, 0);
			if (scratch.sourceAccumulator != &accumulator
				|| scratch.acceptedTileRegistrations.size() != itemCount)
			{
				return OriginalOrderAnchorFailure::Source;
			}

			OriginalOrderAnchorFailure failure =
				OriginalOrderAnchorFailure::None;
			auto fail = [&](OriginalOrderAnchorFailure candidate)
			{
				if (failure == OriginalOrderAnchorFailure::None)
					failure = candidate;
			};
			bool haveFreeType = false;
			for (size_t ordinal = 0; ordinal < itemCount; ++ordinal)
			{
				NiGeometry* geometry =
					scratch.acceptedTileRegistrations[ordinal];
				if (!IsFreeTypeFacade(geometry))
					continue;
				haveFreeType = true;
				scratch.originalOrderFreeTypeFlags[ordinal] = 1;
				const A8ShapeMetadata* metadata = FindBatchedMetadata(
					scratch, static_cast<NiTriShape*>(geometry));
				if (!metadata || metadata->shapeIdentity != geometry
					|| metadata->selfIdentity != metadata
					|| !metadata->allocationId)
				{
					fail(OriginalOrderAnchorFailure::Metadata);
				}
			}
			if (!haveFreeType)
				return OriginalOrderAnchorFailure::Coverage;

			for (const A8ShapeMetadataPtr& owner : scratch.metadataOwners)
			{
				if (!owner)
					continue;
				if (owner->backend
					== FreeTypeShapeBackend::VirtualStockSingleton)
				{
					VirtualStockSingletonState* singleton =
						GetVirtualStockSingletonState(*owner);
					if (!singleton)
						continue;
					singleton->sourceTopologyToken = 0;
					singleton->topologyValidationToken = 0;
					singleton->preflightValidationToken = 0;
					if (singleton->frameMode.load(std::memory_order_acquire)
						!= VirtualStockFrameMode::Retired)
					{
						singleton->frameMode.store(
							VirtualStockFrameMode::Facade,
							std::memory_order_release);
					}
				}
				else if (owner->backend
					== FreeTypeShapeBackend::VirtualStockNative
					&& owner->virtualStockPrimary)
				{
					std::shared_ptr<VirtualStockShapeGroup> group =
						AcquireVirtualStockShapeGroup(*owner);
					if (!group)
						continue;
					std::lock_guard<std::mutex> lock(group->mutex);
					group->sourceTopologyToken = 0;
					group->topologyValidationToken = 0;
					group->preflightValidationToken = 0;
					std::fill(group->sortedItemIndices.begin(),
						group->sortedItemIndices.end(), -1);
					if (group->frameMode.load(std::memory_order_acquire)
						!= VirtualStockFrameMode::Retired)
					{
						group->frameMode.store(VirtualStockFrameMode::Facade,
							std::memory_order_release);
					}
				}
			}

			for (const A8ShapeMetadataPtr& owner : scratch.metadataOwners)
			{
				const A8ShapeMetadata* metadata = owner.get();
				if (!metadata)
					continue;
				const size_t metadataIndex = LookupMetadataShapeIndex(
					scratch, metadata->shapeIdentity);
				if (metadataIndex >= scratch.sourceOccurrenceCounts.size())
					continue;

				if (metadata->backend
					== FreeTypeShapeBackend::VirtualStockSingleton)
				{
					VirtualStockSingletonState* singleton =
						GetVirtualStockSingletonState(*metadata);
					const bool valid = singleton
						&& singleton->slot.shape == metadata->shapeIdentity
						&& metadata->virtualStockPrimary
						&& scratch.sourceOccurrenceCounts[metadataIndex] == 1
						&& scratch.sourceFirstIndices[metadataIndex] >= 0;
					if (valid)
					{
						singleton->sourceTopologyToken = scratch.sortCycle;
						++s_thinRegistrationDiagnostics.singletonTopology;
					}
					else
					{
						++s_thinRegistrationDiagnostics.singletonFallback;
						++s_thinRegistrationDiagnostics.occurrenceFallback;
						fail(OriginalOrderAnchorFailure::Singleton);
					}
					continue;
				}

				if (metadata->backend
					!= FreeTypeShapeBackend::VirtualStockNative
					|| !metadata->virtualStockPrimary)
				{
					continue;
				}

				std::shared_ptr<VirtualStockShapeGroup> group =
					AcquireVirtualStockShapeGroup(*metadata);
				bool valid = group != nullptr;
				size_t blockStart = itemCount;
				if (valid)
				{
					std::lock_guard<std::mutex> lock(group->mutex);
					valid = group->primaryMetadataOwner.get() == metadata
						&& group->primaryShape == metadata->shapeIdentity
						&& !group->slots.empty()
						&& group->primarySlot + 1u == group->slots.size()
						&& group->sortedItemIndices.size()
							== group->slots.size();
					for (SInt32 slotIndex = valid
							? static_cast<SInt32>(group->primarySlot) : -1;
						slotIndex >= 0; --slotIndex)
					{
						const size_t offset = static_cast<size_t>(
							group->primarySlot
								- static_cast<UInt32>(slotIndex));
						NiTriShape* shape = group->slots[slotIndex].shape;
						const size_t shapeIndex =
							LookupMetadataShapeIndex(scratch, shape);
						const A8ShapeMetadata* slotMetadata =
							FindBatchedMetadata(scratch, shape);
						if (!shape || shapeIndex
								>= scratch.sourceOccurrenceCounts.size()
							|| scratch.sourceOccurrenceCounts[shapeIndex] != 1
							|| scratch.sourceFirstIndices[shapeIndex] < 0
							|| !slotMetadata
							|| slotMetadata->virtualStockGroup != group.get()
							|| slotMetadata->virtualStockSlot
								!= static_cast<UInt32>(slotIndex))
						{
							valid = false;
							break;
						}
						const size_t sourceIndex = static_cast<size_t>(
							scratch.sourceFirstIndices[shapeIndex]);
						if (!offset)
							blockStart = sourceIndex;
						else if (sourceIndex != blockStart + offset)
						{
							valid = false;
							break;
						}
					}
					if (valid)
					{
						for (const VirtualStockSlotBinding& slot : group->slots)
						{
							if (!slot.shape || slot.shape == group->primaryShape)
								continue;
							valid = SynchronizeFreeTypeStockPageShapeState(
								*group->primaryShape, *slot.shape) && valid;
						}
					}
					if (valid)
						group->sourceTopologyToken = scratch.sortCycle;
				}

				if (!valid)
				{
					++s_thinRegistrationDiagnostics.groupFallback;
					++s_thinRegistrationDiagnostics.occurrenceFallback;
					RecordFreeTypePerf(
						FreeTypePerfCounter::VirtualStockFallbackNoncontiguous);
					fail(OriginalOrderAnchorFailure::Group);
					continue;
				}

				++s_thinRegistrationDiagnostics.groupTopology;
				if (group->slots.size() > 1u)
				{
					if (scratch.registrationBlockOrdinals.empty())
					{
						scratch.registrationBlockOrdinals.resize(itemCount);
						for (size_t ordinal = 0; ordinal < itemCount; ++ordinal)
						{
							scratch.registrationBlockOrdinals[ordinal] =
								static_cast<UInt32>(ordinal);
						}
					}
					for (size_t offset = 0; offset < group->slots.size(); ++offset)
					{
						scratch.registrationBlockOrdinals[blockStart + offset] =
							static_cast<UInt32>(blockStart);
					}
				}
			}

			return failure;
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
			for (const A8ShapeMetadataPtr& owner : scratch.metadataOwners)
			{
				const A8ShapeMetadata* metadata = owner.get();
				if (!metadata)
					continue;
				if (metadata->backend != FreeTypeShapeBackend::VirtualStockNative)
					continue;
				if (!metadata->virtualStockPrimary
					|| !metadata->virtualStockGroup)
				{
					return false;
				}
				std::shared_ptr<VirtualStockShapeGroup> group =
					AcquireVirtualStockShapeGroup(*metadata);
				if (!group)
					return false;
				std::lock_guard<std::mutex> lock(group->mutex);
				if (group->slots.size() != 1u)
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
			if (!accumulator || scratch.sourceAccumulator != accumulator
				|| scratch.metadataShapes.empty()
				|| !accumulator->m_bInterfaceSort)
			{
				attempt.failure = OriginalOrderAnchorFailure::Gate;
				return attempt;
			}
			const size_t itemCount = scratch.acceptedTileRegistrations.size();
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
			for (size_t ordinal = 0; ordinal < itemCount; ++ordinal)
			{
				NiGeometry* geometry =
					scratch.acceptedTileRegistrations[ordinal];
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
			const OriginalOrderAnchorFailure topologyFailure =
				scratch.sourceTopologyFailure;

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
			const size_t itemCount = accumulator
				? static_cast<size_t>(accumulator->m_iNumItems) : 0;
			auto reject = [&]()
			{
				++s_thinRegistrationDiagnostics.occurrenceFallback;
				RecordFreeTypePerf(
					FreeTypePerfCounter::SortedMixedEqualDepthRestoreRejected);
				return false;
			};
			if (!accumulator || scratch.sourceAccumulator != accumulator
				|| !accumulator->m_ppkItems || !accumulator->m_pfDepths
				|| itemCount < 2 || itemCount > kMaximumStableTieItems
				|| scratch.acceptedTileRegistrations.size() != itemCount)
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

			PrepareLookup(scratch.acceptedRegistrationLookup, itemCount);
			const size_t lookupMask =
				scratch.acceptedRegistrationLookup.size() - 1u;
			scratch.acceptedOccurrenceHeads.assign(
				scratch.acceptedRegistrationLookup.size(), 0);
			scratch.acceptedOccurrenceTails.assign(
				scratch.acceptedRegistrationLookup.size(), 0);
			scratch.acceptedOccurrenceNext.assign(
				itemCount, kInvalidNativeA8CommandIndex);
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
						scratch.acceptedOccurrenceHeads[slot] =
							static_cast<UInt32>(ordinal + 1u);
						scratch.acceptedOccurrenceTails[slot] =
							static_cast<UInt32>(ordinal + 1u);
						inserted = true;
						break;
					}
					const size_t representative =
						static_cast<size_t>(stored - 1u);
					if (representative < itemCount
						&& scratch.acceptedTileRegistrations[representative]
							== geometry)
					{
						const UInt32 tail =
							scratch.acceptedOccurrenceTails[slot];
						if (!tail || tail - 1u >= itemCount)
							return reject();
						scratch.acceptedOccurrenceNext[tail - 1u] =
							static_cast<UInt32>(ordinal);
						scratch.acceptedOccurrenceTails[slot] =
							static_cast<UInt32>(ordinal + 1u);
						inserted = true;
						break;
					}
					slot = (slot + 1u) & lookupMask;
				}
				if (!inserted)
					return reject();
			}

			scratch.sortedRegistrationOrdinals.resize(itemCount);
			scratch.sortedItemIndicesByRegistrationOrdinal.assign(
				itemCount, kInvalidNativeA8CommandIndex);
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
					const size_t representative =
						static_cast<size_t>(stored - 1u);
					if (representative < itemCount
						&& scratch.acceptedTileRegistrations[representative]
							== geometry)
					{
						const UInt32 head =
							scratch.acceptedOccurrenceHeads[slot];
						if (!head || head - 1u >= itemCount)
							return reject();
						const UInt32 ordinal = head - 1u;
						const UInt32 next =
							scratch.acceptedOccurrenceNext[ordinal];
						scratch.acceptedOccurrenceHeads[slot] =
							next == kInvalidNativeA8CommandIndex
								? 0 : next + 1u;
						scratch.sortedRegistrationOrdinals[item] = ordinal;
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
			for (UInt32 head : scratch.acceptedOccurrenceHeads)
			{
				if (head)
					return reject();
			}

			if (scratch.registrationBlockOrdinals.size() != itemCount)
			{
				scratch.registrationBlockOrdinals.resize(itemCount);
				for (size_t ordinal = 0; ordinal < itemCount; ++ordinal)
				{
					scratch.registrationBlockOrdinals[ordinal] =
						static_cast<UInt32>(ordinal);
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
						const UInt32 ordinal =
							scratch.sortedRegistrationOrdinals[item];
						scratch.stableTieItems.push_back({
							accumulator->m_ppkItems[item],
							accumulator->m_pfDepths[item],
							ordinal,
							scratch.registrationBlockOrdinals[ordinal]
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

		void ResolveVirtualStockSortedTopology(
			SortedPayloadScratch& scratch, BSShaderAccumulator* accumulator,
			UInt64 validationToken)
		{
			scratch.sortedOccurrenceCounts.assign(
				scratch.metadataShapes.size(), 0);
			scratch.sortedFirstIndices.assign(
				scratch.metadataShapes.size(), -1);
			if (!accumulator || !validationToken
				|| scratch.sourceAccumulator != accumulator
				|| !accumulator->m_ppkItems)
			{
				++s_thinRegistrationDiagnostics.sourceFallback;
				return;
			}

			for (SInt32 itemIndex = 0;
				itemIndex < accumulator->m_iNumItems; ++itemIndex)
			{
				NiGeometry* geometry = accumulator->m_ppkItems[itemIndex];
				if (!IsFreeTypeFacade(geometry))
					continue;
				const size_t metadataIndex = LookupMetadataShapeIndex(
					scratch, static_cast<NiTriShape*>(geometry));
				if (metadataIndex >= scratch.sortedOccurrenceCounts.size())
					continue;
				if (!scratch.sortedOccurrenceCounts[metadataIndex])
					scratch.sortedFirstIndices[metadataIndex] = itemIndex;
				++scratch.sortedOccurrenceCounts[metadataIndex];
			}

			for (const A8ShapeMetadataPtr& owner : scratch.metadataOwners)
			{
				const A8ShapeMetadata* metadata = owner.get();
				if (!metadata)
					continue;
				const size_t metadataIndex = LookupMetadataShapeIndex(
					scratch, metadata->shapeIdentity);
				if (metadataIndex >= scratch.sortedOccurrenceCounts.size())
					continue;

				if (metadata->backend
					== FreeTypeShapeBackend::VirtualStockSingleton)
				{
					VirtualStockSingletonState* singleton =
						GetVirtualStockSingletonState(*metadata);
					const bool valid = singleton
						&& singleton->sourceTopologyToken == scratch.sortCycle
						&& singleton->slot.shape == metadata->shapeIdentity
						&& scratch.sourceOccurrenceCounts[metadataIndex] == 1
						&& scratch.sortedOccurrenceCounts[metadataIndex] == 1
						&& scratch.sortedFirstIndices[metadataIndex] >= 0
						&& singleton->frameMode.load(std::memory_order_acquire)
							!= VirtualStockFrameMode::Retired;
					if (valid)
					{
						singleton->topologyValidationToken = validationToken;
						RecordFreeTypePerf(
							FreeTypePerfCounter::VirtualStockRegistrationResolved);
					}
					else
					{
						if (singleton)
							singleton->topologyValidationToken = 0;
						++s_thinRegistrationDiagnostics.singletonFallback;
						++s_thinRegistrationDiagnostics.occurrenceFallback;
						RestoreVirtualStockSingletonToFacade(*metadata,
							NativeA8FallbackReason::PropertySync);
						RecordFreeTypePerf(FreeTypePerfCounter::
							VirtualStockFallbackNoncontiguous);
					}
					continue;
				}

				if (metadata->backend
					!= FreeTypeShapeBackend::VirtualStockNative
					|| !metadata->virtualStockPrimary)
				{
					continue;
				}

				std::shared_ptr<VirtualStockShapeGroup> group =
					AcquireVirtualStockShapeGroup(*metadata);
				bool valid = group != nullptr;
				UInt64 missing = 0;
				UInt64 duplicate = 0;
				bool orderMismatch = false;
				if (valid)
				{
					std::lock_guard<std::mutex> lock(group->mutex);
					valid = group->sourceTopologyToken == scratch.sortCycle
						&& group->primaryMetadataOwner.get() == metadata
						&& group->primaryShape == metadata->shapeIdentity
						&& !group->slots.empty()
						&& group->primarySlot + 1u == group->slots.size()
						&& group->sortedItemIndices.size()
							== group->slots.size()
						&& group->frameMode.load(std::memory_order_acquire)
							!= VirtualStockFrameMode::Retired;
					SInt32 previous = -1;
					for (SInt32 slotIndex = valid
							? static_cast<SInt32>(group->primarySlot) : -1;
						slotIndex >= 0; --slotIndex)
					{
						NiTriShape* shape = group->slots[slotIndex].shape;
						const size_t shapeIndex =
							LookupMetadataShapeIndex(scratch, shape);
						if (shapeIndex >= scratch.sortedOccurrenceCounts.size())
						{
							++missing;
							valid = false;
							break;
						}
						const UInt32 occurrences =
							scratch.sortedOccurrenceCounts[shapeIndex];
						if (!occurrences)
							++missing;
						else if (occurrences > 1)
							duplicate += occurrences - 1u;
						const SInt32 item =
							scratch.sortedFirstIndices[shapeIndex];
						if (occurrences != 1 || item < 0
							|| item >= accumulator->m_iNumItems
							|| accumulator->m_ppkItems[item] != shape
							|| (previous >= 0 && item != previous + 1))
						{
							orderMismatch = orderMismatch
								|| (occurrences == 1);
							valid = false;
							break;
						}
						group->sortedItemIndices[slotIndex] = item;
						previous = item;
					}
					group->topologyValidationToken =
						valid ? validationToken : 0;
				}
				if (!valid)
				{
					++s_thinRegistrationDiagnostics.groupFallback;
					++s_thinRegistrationDiagnostics.occurrenceFallback;
					if (group)
						RestoreVirtualStockGroupToFacade(group,
							NativeA8FallbackReason::PropertySync);
					RecordFreeTypePerf(FreeTypePerfCounter::
						VirtualStockFallbackNoncontiguous);
					if (missing)
						RecordFreeTypePerf(FreeTypePerfCounter::
							VirtualStockRegistrationMissing, missing);
					if (duplicate)
						RecordFreeTypePerf(FreeTypePerfCounter::
							VirtualStockRegistrationDuplicate, duplicate);
					if (orderMismatch)
						RecordFreeTypePerf(FreeTypePerfCounter::
							VirtualStockRegistrationOrderMismatch);
					continue;
				}
				RecordFreeTypePerf(
					FreeTypePerfCounter::VirtualStockRegistrationResolved,
					static_cast<UInt64>(group->slots.size()));
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
			NativePreflightFrameContext structuralContext;
			const NativePreflightFrameContext* currentContext = frameContext;
			if (!currentContext && g_bEnableFreeTypeFontStructuralFastPaths)
			{
				NativeA8RuntimeReadinessView readiness;
				const bool ready =
					GetNativeA8RuntimeReadinessCurrent(readiness);
				structuralContext.accumulatorCurrent = ready;
				structuralContext.immediateRouteCurrent = ready;
				structuralContext.rendererAvailable = ready;
				structuralContext.generation = readiness.generation;
				structuralContext.atlasTextureEpoch =
					readiness.atlasTextureEpoch;
				currentContext = &structuralContext;
			}
			if (!(currentContext
				? currentContext->accumulatorCurrent
				: IsNativeA8AccumulatorHookCurrent()))
				return NativeA8FallbackReason::AccumulatorConflict;
			if (!(currentContext
				? currentContext->immediateRouteCurrent
				: IsA8RenderPassImmediatelyHookCurrent()))
				return NativeA8FallbackReason::TileRouteConflict;
			if (!(currentContext
				? currentContext->rendererAvailable
				: IsNativeA8RendererAvailable()))
				return NativeA8FallbackReason::ShaderGeneration;

			const UInt32 generation = currentContext
				? currentContext->generation : GetNativeA8ShaderGeneration();
			if (!generation)
				return NativeA8FallbackReason::ShaderGeneration;
			const bool scaledFillSampling = NeedsScaledFillSampling(facade);
			const NiAlphaProperty* alpha = facade->GetAlphaProperty();
			const bool alphaBlending = alpha && alpha->GetAlphaBlending();
			const UInt32 atlasTextureEpoch = currentContext
				? currentContext->atlasTextureEpoch
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

		__forceinline bool IsThinRegistrationHookChainCurrentUnchecked()
		{
			A8State& state = State();
			const TileRegisterObjectFn current =
				*reinterpret_cast<TileRegisterObjectFn volatile*>(
					kTileRegisterObjectFunctionEntry);
			static constexpr UInt8 kSortTail[] = {
				0x90, 0x90, 0x90, 0x90
			};
			return current == &NativeA8RegisterObject
				&& s_originalTileRegisterObject.load(
					std::memory_order_relaxed) != nullptr
				&& state.originalRenderAlphaGeometry
				&& hook_identity::MatchesRel32InstructionImageUnchecked(
					kRenderAlphaGeometryCallSite,
					s_renderAlphaGeometryHookImage)
				&& hook_identity::MatchesRel32InstructionImageUnchecked(
					kTileSortDispatchPatch, s_tileSortHookImage)
				&& hook_identity::MatchesBytesUnchecked(
					kTileSortDispatchPatch + 5u,
					kSortTail, sizeof(kSortTail))
				&& IsA8RenderPassImmediatelyHookCurrentUnchecked();
		}

		bool __cdecl NativeA8RegisterObject(BSShaderAccumulator* accumulator,
			NiGeometry* geometry, const NiPropertyState* properties,
			BSShaderProperty* shaderProperty, BSShader* shader)
		{
			const TileRegisterObjectFn original =
				s_originalTileRegisterObject.load(std::memory_order_relaxed);
			if (!original || !accumulator || !geometry
				|| accumulator->eRenderMode
					!= BSShaderManager::BSSM_RENDER_TILES
				|| !IsFreeTypeFacade(geometry))
			{
				return ForwardTileRegisterObject(original, accumulator, geometry,
					properties, shaderProperty, shader);
			}

			ThinRegistrationDiagnostics& diagnostics =
				s_thinRegistrationDiagnostics;
			const UInt64 call = ++diagnostics.calls;
			const bool sample = g_bEnableFreeTypeFontRenderingLog
				&& (call & (kRegisterRouteSampleRate - 1u)) == 0;
			if (sample)
				++diagnostics.samples;
			ThinRegistrationSampleScope timing(sample);

			if (!IsThinRegistrationHookChainCurrentUnchecked())
			{
				++diagnostics.hookMismatch;
				const UInt64 fingerprint =
					BuildThinHookFingerprintUnchecked();
				bool auditedCurrent = false;
				if (fingerprint != s_thinRejectedHookFingerprint.load(
					std::memory_order_acquire))
				{
					std::lock_guard<std::mutex> auditLock(
						s_thinHookAuditMutex);
					// A second render thread may have completed the same cold
					// audit while this thread waited. Recheck both the live image
					// and the globally rejected image before querying pages.
					if (IsThinRegistrationHookChainCurrentUnchecked())
					{
						auditedCurrent = true;
					}
					else
					{
						const UInt64 currentFingerprint =
							BuildThinHookFingerprintUnchecked();
						if (currentFingerprint
							!= s_thinRejectedHookFingerprint.load(
								std::memory_order_relaxed))
						{
							++diagnostics.slowAudits;
							// The install path has already certified these pages.
							// The page-query audit is reserved for one newly
							// observed instruction image, never one audit per
							// facade.
							auditedCurrent =
								IsNativeA8RegistrationHookChainCurrent()
								&& IsA8RenderPassImmediatelyHookCurrent()
								&& IsThinRegistrationHookChainCurrentUnchecked();
							if (!auditedCurrent)
							{
								s_thinRejectedHookFingerprint.store(
									currentFingerprint,
									std::memory_order_release);
							}
						}
					}
				}
				if (!auditedCurrent
					&& !IsThinRegistrationHookChainCurrentUnchecked())
				{
					++diagnostics.suppressed;
					// A FreeType facade contains native payload state that an unknown
					// RenderAlpha/RenderPass chain cannot interpret safely.
					return true;
				}
			}

			++diagnostics.fastForward;
			return ForwardTileRegisterObject(original, accumulator, geometry,
				properties, shaderProperty, shader);
		}

		void __fastcall NativeA8SortAlphaGeometry(
			BSShaderAccumulator* accumulator, AccumulatorSortFn originalSort)
		{
			if (!accumulator)
				return;
			FreeTypePerfScope sortRoutePerf(
				FreeTypePerfPhase::SortRouteTotal);
			SortedPayloadScratch& scratch = s_sortedPayloadScratch;
			FlushThinRegistrationDiagnostics();
			const bool sourceCaptured =
				CaptureSourceRegistrationOrder(scratch, accumulator);
			if (sourceCaptured && !scratch.metadataShapes.empty())
			{
				scratch.sourceTopologyFailure =
					PrepareOriginalOrderAnchorTopology(scratch,
						*accumulator,
						scratch.acceptedTileRegistrations.size());
			}

			// Reproduce the first instruction overwritten at 0xB65E95 before
			// choosing either the anchored implementation or the vtable predecessor
			// already loaded into EDX by FinishAccumulating_Tiles.
			accumulator->m_pGeometryList = nullptr;
			scratch.originalOrderAnchorAccumulator = nullptr;
			scratch.originalOrderAnchorCycle = 0;
			const bool haveAnchorCandidate = sourceCaptured
				&& !scratch.metadataShapes.empty()
				&& accumulator->eRenderMode
					== BSShaderManager::BSSM_RENDER_TILES;
			const bool stockSortPredecessor = reinterpret_cast<SIZE_T>(
				originalSort) == kStockInterfaceAlphaSort;
			if (haveAnchorCandidate && stockSortPredecessor)
			{
				OriginalOrderSortAttempt attempt;
				{
					FreeTypePerfScope sortAnchoredPerf(
						FreeTypePerfPhase::SortAnchored);
					attempt = TrySortWithOriginalOrderAnchors(scratch,
						accumulator);
				}
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
					scratch.originalOrderAnchorCycle = scratch.sortCycle;
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

			{
				FreeTypePerfScope sortStockPerf(FreeTypePerfPhase::SortStock);
				if (originalSort)
					originalSort(accumulator);
				else
					accumulator->Sort();
			}
		}

		void __fastcall NativeA8RenderAlphaGeometry(BSShaderAccumulator* accumulator, void*)
		{
			A8State& state = State();
			if (!state.originalRenderAlphaGeometry)
				return;
			FreeTypePerfScope frameRoutePerf(
				FreeTypePerfPhase::FrameRouteTotal);

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
				FreeTypePerfScope framePrepPerf(
					FreeTypePerfPhase::FrameRoutePrep);
				const size_t itemCount = static_cast<size_t>(
					accumulator->m_iNumItems);
				const bool originalOrderAnchored =
					scratch.originalOrderAnchorAccumulator == accumulator
						&& scratch.originalOrderAnchorCycle
							== scratch.sortCycle;
				if (!originalOrderAnchored)
				{
					RestoreFreeTypeMixedEqualDepthPainterOrder(
						scratch, accumulator);
				}
				scratch.frameEntries.clear();
				scratch.payloadTemplates.clear();
				scratch.virtualStockGroups.clear();
				scratch.virtualStockSingletons.clear();
				EnsureSortedMetadataCoverage(scratch, *accumulator);

				NativePreflightFrameContext preflightContext;
				if (g_bEnableFreeTypeFontStructuralFastPaths)
				{
					NativeA8RuntimeReadinessView readiness;
					bool ready = GetNativeA8RuntimeReadinessCurrent(readiness);
					if (!ready && IsA8RendererAvailable())
						ready = GetNativeA8RuntimeReadinessCurrent(readiness);
					preflightContext.accumulatorCurrent = ready;
					preflightContext.immediateRouteCurrent = ready;
					preflightContext.rendererAvailable = ready;
					preflightContext.generation = readiness.generation;
					preflightContext.atlasTextureEpoch =
						readiness.atlasTextureEpoch;
				}
				else
				{
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
				}
				const UInt32 generation = preflightContext.generation;
				UInt64 frameValidationToken = ++scratch.nextValidationToken;
				if (!frameValidationToken)
					frameValidationToken = ++scratch.nextValidationToken;

				scratch.frameCandidates = scratch.metadataShapes;
				scratch.executionSkeleton.clear();
				scratch.executionSkeletonAccumulator = nullptr;
				scratch.executionSkeletonItems = nullptr;
				scratch.executionSkeletonDepths = nullptr;
				scratch.executionSkeletonValidationToken = 0;
				const bool buildExecutionSkeleton =
					g_bEnableFreeTypeFontStructuralFastPaths
						&& g_bEnableFreeTypeFontCrossTextBatch
						&& accumulator->m_pfDepths;
				if (buildExecutionSkeleton)
				{
					scratch.executionSkeleton.reserve(itemCount);
					scratch.executionSkeletonAccumulator = accumulator;
					scratch.executionSkeletonItems = accumulator->m_ppkItems;
					scratch.executionSkeletonDepths = accumulator->m_pfDepths;
					scratch.executionSkeletonValidationToken =
						frameValidationToken;
					for (SInt32 index = accumulator->m_iNumItems - 1;
						index >= 0; --index)
					{
						NiGeometry* geometry = accumulator->m_ppkItems[index];
						const bool isFacade = IsFreeTypeFacade(geometry);
						ExecutionSequenceSkeletonItem item;
						item.facade = isFacade
							? static_cast<NiTriShape*>(geometry) : nullptr;
						std::memcpy(&item.depthBits,
							&accumulator->m_pfDepths[index],
							sizeof(item.depthBits));
						item.originalOrdinal = static_cast<UInt32>(index);
						item.classification = isFacade
							? ExecutionSkeletonClass::NativeFacade
							: ExecutionSkeletonClass::Barrier;
						scratch.executionSkeleton.push_back(item);
					}
				}

				const size_t trackedCount = scratch.frameCandidates.size();
				scratch.frameEntries.reserve(trackedCount);
				scratch.payloadTemplates.reserve(trackedCount);
				scratch.virtualStockGroups.reserve(
					std::min<size_t>(trackedCount, 1024));
				scratch.virtualStockSingletons.reserve(
					std::min<size_t>(trackedCount, 1024));
				PrepareLookup(scratch.facadeLookup, trackedCount);
				PrepareLookup(scratch.payloadLookup, trackedCount);
				ResolveVirtualStockSortedTopology(
					scratch, accumulator, frameValidationToken);

				for (NiTriShape* facade : scratch.frameCandidates)
				{
					if (LookupSortedFacade(scratch, facade)
						!= std::numeric_limits<size_t>::max())
					{
						continue;
					}

					SortedFrameEntry entry;
					entry.facade = facade;
					entry.metadata = FindBatchedMetadata(scratch, facade);
					entry.generation = generation;
					std::shared_ptr<VirtualStockShapeGroup>
						virtualStockGroup;
					VirtualStockSingletonState* virtualStockSingleton =
						nullptr;
					bool topologyReady = false;
					if (!entry.metadata)
					{
						if (g_bEnableFreeTypeFontRenderingLog)
						{
							const UInt32 ordinal =
								s_missingMetadataLogCount.fetch_add(
									1, std::memory_order_relaxed);
							if (ordinal < kMaximumMissingMetadataLogs)
							{
								gLog.FormattedMessage(
									"tnvse_freetype_native: submission-suppressed reason=metadata-missing phase=sorted-batch shape=%p thread=%u",
									facade, GetCurrentThreadId());
							}
						}
					}
					else if (entry.metadata->backend
						== FreeTypeShapeBackend::VirtualStockNative)
					{
						if (entry.metadata->virtualStockPrimary)
						{
							virtualStockGroup =
								AcquireVirtualStockShapeGroup(*entry.metadata);
							if (virtualStockGroup)
							{
								std::lock_guard<std::mutex> lock(
									virtualStockGroup->mutex);
								topologyReady =
									virtualStockGroup->topologyValidationToken
										== frameValidationToken;
							}
						}
						else
						{
							// Followers need a sorted metadata view so their render
							// callback never reacquires metadata ownership. The primary
							// alone owns preflight and the compatibility payload.
							entry.preflightResult =
								NativeA8FallbackReason::None;
						}
					}
					else if (entry.metadata->backend
						== FreeTypeShapeBackend::VirtualStockSingleton)
					{
						virtualStockSingleton =
							GetVirtualStockSingletonState(*entry.metadata);
						topologyReady = virtualStockSingleton
							&& virtualStockSingleton->topologyValidationToken
								== frameValidationToken;
					}

					const bool groupFollower = entry.metadata
						&& entry.metadata->backend
							== FreeTypeShapeBackend::VirtualStockNative
						&& !entry.metadata->virtualStockPrimary;
					if (!groupFollower && entry.metadata
						&& entry.metadata->nativePayload.buildComplete)
					{
						entry.payload = &entry.metadata->nativePayload;
						entry.visibilityCull =
							EvaluateNativeA8SubmissionVisibility(
								facade, *entry.payload);
						if (entry.visibilityCull
								== NativeA8VisibilityCull::None
							&& preflightContext.rendererAvailable)
						{
							entry.visibilityCull =
								EvaluateNativeA8PreflightClipVisibility(
									facade, *entry.payload);
						}
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
								&preflightContext, forcedCompositeTopology);
							if (entry.preflightResult
								== NativeA8FallbackReason::None)
							{
								entry.generation =
									entry.payload->preparedGeneration;
								entry.validationToken = frameValidationToken;
								if (virtualStockGroup && topologyReady)
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
								else if (virtualStockSingleton
									&& topologyReady)
								{
									virtualStockSingleton->
										preflightValidationToken =
											frameValidationToken;
									scratch.virtualStockSingletons.push_back(
										entry.metadata);
								}
							}
							else if (virtualStockGroup && topologyReady)
							{
								RestoreVirtualStockGroupToFacade(
									virtualStockGroup, entry.preflightResult);
							}
							else if (virtualStockSingleton && topologyReady)
							{
								RestoreVirtualStockSingletonToFacade(
									*entry.metadata, entry.preflightResult);
							}
						}
					}
					else if (!groupFollower)
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
						size_t virtualEntryCount =
							scratch.virtualStockSingletons.size();
						for (const std::shared_ptr<
							VirtualStockShapeGroup>& group
							: scratch.virtualStockGroups)
						{
							if (group)
								virtualEntryCount += group->slots.size();
						}
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
							auto appendSequenceItem = [&](NiTriShape* geometry,
								float accumulatorDepth)
							{
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
										&& entryIndex < scratch.frameEntries.size())
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
										commandIndex = entry->singlePacketCommandIndex;
									}
								}
								else if (entry && entry->crossTextOccurrences > 1)
								{
									MarkNativeA8CrossTextBatchSequenceBarrier(
										entry->crossTextSequenceIndex);
									entry->crossTextSequenceIndex =
										kInvalidNativeA8CommandIndex;
								}

								const UInt32 sequenceIndex =
									AddNativeA8CrossTextBatchSequenceItem(
										kind, geometry, metadata, payload,
										commandIndex, accumulatorDepth);
								if (entry && entry->crossTextOccurrences == 1
									&& kind != NativeA8CrossTextCommandKind::Barrier)
								{
									entry->crossTextSequenceIndex = sequenceIndex;
								}
							};

							bool usedSkeleton = false;
							if (g_bEnableFreeTypeFontStructuralFastPaths)
							{
								usedSkeleton =
									scratch.executionSkeletonAccumulator == accumulator
									&& scratch.executionSkeletonItems
										== accumulator->m_ppkItems
									&& scratch.executionSkeletonDepths
										== accumulator->m_pfDepths
									&& scratch.executionSkeletonValidationToken
										== frameValidationToken
									&& scratch.executionSkeleton.size()
										== static_cast<size_t>(accumulator->m_iNumItems)
									&& !scratch.executionSkeleton.empty()
									&& scratch.executionSkeleton.front().originalOrdinal
										== static_cast<UInt32>(accumulator->m_iNumItems - 1)
									&& scratch.executionSkeleton.back().originalOrdinal == 0;
								if (usedSkeleton)
								{
									RecordFreeTypePerf(FreeTypePerfCounter::
										CommandSequenceSkeletonHit);
									for (const ExecutionSequenceSkeletonItem& item
										: scratch.executionSkeleton)
									{
										float depth = 0.0f;
										std::memcpy(&depth, &item.depthBits,
											sizeof(depth));
										appendSequenceItem(
											item.classification
												== ExecutionSkeletonClass::NativeFacade
												? item.facade : nullptr,
											depth);
									}
								}
								else
								{
									RecordFreeTypePerf(FreeTypePerfCounter::
										CommandSequenceSkeletonFallback);
								}
							}
							if (!usedSkeleton)
							{
								RecordFreeTypePerf(FreeTypePerfCounter::
									CommandSequenceSkeletonItem,
									static_cast<UInt64>(accumulator->m_iNumItems));
								for (SInt32 itemIndex = accumulator->m_iNumItems - 1;
									itemIndex >= 0; --itemIndex)
								{
									NiTriShape* geometry = IsFreeTypeFacade(
										accumulator->m_ppkItems[itemIndex])
										? static_cast<NiTriShape*>(
											accumulator->m_ppkItems[itemIndex])
										: nullptr;
									appendSequenceItem(geometry,
										accumulator->m_pfDepths[itemIndex]);
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
				{
					FreeTypePerfScope stockRenderPerf(
						FreeTypePerfPhase::FrameRouteStockRender);
					state.originalRenderAlphaGeometry(accumulator);
				}
				EndA8SortedTileConstantOwnership();
				EndNativeA8SortedShaderBatch();
				if (g_bEnableFreeTypeFontCrossTextBatch)
					EndNativeA8CrossTextBatchFrame();
				EndNativeA8FrameCommandBuffer();
				EndNativeA8SortedRingFrame();
				ClearSortedFrame(scratch);
				return;
			}

			const bool clearSortedOwners =
				scratch.sourceAccumulator == accumulator;
			{
				FreeTypePerfScope stockRenderPerf(
					FreeTypePerfPhase::FrameRouteStockRender);
				state.originalRenderAlphaGeometry(accumulator);
			}
			if (clearSortedOwners)
				ClearSortedFrame(scratch);
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

	bool IsNativeA8RegistrationHookChainCurrentFast()
	{
		A8State& state = State();
		const bool tileCurrent =
			ReadTileRegisterObjectTarget() == &NativeA8RegisterObject
			&& s_originalTileRegisterObject.load(std::memory_order_acquire)
				!= nullptr;
		const bool renderAlphaCurrent = state.originalRenderAlphaGeometry
			&& hook_identity::MatchesRel32InstructionImageUnchecked(
				kRenderAlphaGeometryCallSite,
				s_renderAlphaGeometryHookImage);
		const bool sortCallCurrent =
			hook_identity::MatchesRel32InstructionImageUnchecked(
				kTileSortDispatchPatch, s_tileSortHookImage);
		static constexpr UInt8 kTail[] = { 0x90, 0x90, 0x90, 0x90 };
		const bool sortTailCurrent = hook_identity::MatchesBytesUnchecked(
			kTileSortDispatchPatch + 5u, kTail, sizeof(kTail));
		if (!tileCurrent)
		{
			RecordFreeTypePerf(FreeTypePerfCounter::
				StructuralReadinessTileCallbackMismatch);
		}
		if (!renderAlphaCurrent)
		{
			RecordFreeTypePerf(FreeTypePerfCounter::
				StructuralReadinessRenderAlphaMismatch);
		}
		if (!sortCallCurrent)
		{
			RecordFreeTypePerf(FreeTypePerfCounter::
				StructuralReadinessSortCallMismatch);
		}
		if (!sortTailCurrent)
		{
			RecordFreeTypePerf(FreeTypePerfCounter::
				StructuralReadinessSortTailMismatch);
		}
		return tileCurrent && renderAlphaCurrent
			&& sortCallCurrent && sortTailCurrent;
	}
}

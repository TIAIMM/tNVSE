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
#include <cstdint>
#include <cstring>
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
		inline constexpr size_t kMaximumLinearTieRepairItems = 8192;
		inline constexpr UInt32 kRegisterRouteSampleRate = 256;

		using TileRegisterObjectFn = bool(__cdecl*)(BSShaderAccumulator*,
			NiGeometry*, const NiPropertyState*, BSShaderProperty*, BSShader*);
		static_assert(kTileRegisterObjectFunctionEntry == 0x11F9FA8);
		static_assert(sizeof(TileRegisterObjectFn) == sizeof(UInt32));

		std::atomic<TileRegisterObjectFn> s_originalTileRegisterObject = nullptr;
		bool s_loggedTileRegisterObjectConflict = false;
		bool s_loggedTileRegisterObjectSlotUnavailable = false;
		std::atomic<UInt32> s_missingMetadataLogCount = 0;
		std::atomic<UInt32> s_atlasTextureEpoch = 1;
		thread_local bool s_loggedEqualDepthTieRepair = false;
		thread_local bool s_loggedEqualDepthTieRepairFailure = false;

		bool __cdecl NativeA8RegisterObject(BSShaderAccumulator* accumulator,
			NiGeometry* geometry, const NiPropertyState* properties,
			BSShaderProperty* shaderProperty, BSShader* shader);
		bool IsTileRegisterObjectSlotWritable();
		void __fastcall NativeA8RenderAlphaGeometry(
			BSShaderAccumulator* accumulator, void*);
		const hook_identity::Rel32InstructionImage
			s_renderAlphaGeometryHookImage =
				hook_identity::MakeRel32InstructionImage(
					kRenderAlphaGeometryCallSite,
					hook_identity::Rel32Opcode::Call,
					reinterpret_cast<SIZE_T>(
						&NativeA8RenderAlphaGeometry));

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
			bool uniqueOccurrence = false;
		};

		struct EqualDepthTieRepairRun
		{
			UInt32 begin = 0;
			UInt32 end = 0;
			UInt32 write = 0;
		};

		struct EqualDepthTieRepairResult
		{
			bool valid = false;
			UInt32 mixedRuns = 0;
			UInt32 changedRuns = 0;
			UInt32 changedItems = 0;
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
			// The active predecessor Sort owns m_ppkItems/m_pfDepths. tNVSE only
			// rewrites geometry pointers inside a proved exact-equal-depth mixed run;
			// depths and every other run remain the predecessor's result.
			BSShaderAccumulator* frameAccumulator = nullptr;
			std::vector<NiTriShape*> metadataShapes;
			std::vector<A8ShapeMetadataPtr> metadataOwners;
			std::vector<NiTriShape*> metadataAcquireShapes;
			std::vector<A8ShapeMetadataPtr> metadataAcquireOwners;
			std::vector<NativeA8VisibilityCull> preflightCulls;
			std::vector<UInt32> metadataLookup;
			std::vector<UInt32> sortedOccurrenceCounts;
			std::vector<UInt32> tieSortedLookup;
			std::vector<UInt32> tieSortedOccurrenceCursor;
			std::vector<UInt32> tieSortedOccurrenceNext;
			std::vector<UInt32> tieRunIds;
			std::vector<EqualDepthTieRepairRun> tieRuns;
			std::vector<NiGeometry*> tieOutput;
			std::vector<NiTriShape*> frameCandidates;
			std::vector<SortedFrameEntry> frameEntries;
			std::vector<UInt32> facadeLookup;
			std::vector<NativeA8PayloadTemplatePtr> payloadTemplates;
			std::vector<UInt32> payloadLookup;
			std::vector<const A8ShapeMetadata*> singletonFacades;
			CpuMemoryLease cpuMemory;
			UInt32 nestedBypassDepth = 0;
			UInt64 nestedTraversalSerial = 1;
			UInt64 nextValidationToken = 0;
			UInt64 activeValidationToken = 0;
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
			UInt64 sortedScanFallback = 0;
			UInt64 facadeTopology = 0;
			UInt64 facadeFallback = 0;
			UInt64 occurrenceFallback = 0;
		};

		thread_local ThinRegistrationDiagnostics s_thinRegistrationDiagnostics;
		// Diagnostics are flushed at every frame-clear boundary. Keep the timing
		// phase separate so short registration cycles still contribute one sample
		// per kRegisterRouteSampleRate calls across the lifetime of this thread.
		thread_local UInt32 s_registerRouteSampleCountdown =
			kRegisterRouteSampleRate;
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

			bool IsActive() const
			{
				return m_start != 0;
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

		__forceinline bool ShouldSampleRegisterRoute()
		{
			if (!g_bEnableFreeTypeFontRenderingLog)
				return false;
			if (--s_registerRouteSampleCountdown != 0)
				return false;
			s_registerRouteSampleCountdown = kRegisterRouteSampleRate;
			return true;
		}

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
			record(FreeTypePerfCounter::ThinRegistrationSortedScanFallback,
				values.sortedScanFallback);
			record(FreeTypePerfCounter::ThinRegistrationFacadeTopology,
				values.facadeTopology);
			record(FreeTypePerfCounter::ThinRegistrationFacadeFallback,
				values.facadeFallback);
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
				scratch.metadataShapes.capacity() * sizeof(NiTriShape*)
				+ scratch.metadataOwners.capacity()
					* sizeof(A8ShapeMetadataPtr)
				+ scratch.metadataAcquireShapes.capacity()
					* sizeof(NiTriShape*)
				+ scratch.metadataAcquireOwners.capacity()
					* sizeof(A8ShapeMetadataPtr)
				+ scratch.preflightCulls.capacity()
					* sizeof(NativeA8VisibilityCull)
				+ scratch.metadataLookup.capacity() * sizeof(UInt32)
				+ scratch.sortedOccurrenceCounts.capacity() * sizeof(UInt32)
				+ scratch.tieSortedLookup.capacity() * sizeof(UInt32)
				+ scratch.tieSortedOccurrenceCursor.capacity() * sizeof(UInt32)
				+ scratch.tieSortedOccurrenceNext.capacity() * sizeof(UInt32)
				+ scratch.tieRunIds.capacity() * sizeof(UInt32)
				+ scratch.tieRuns.capacity() * sizeof(EqualDepthTieRepairRun)
				+ scratch.tieOutput.capacity() * sizeof(NiGeometry*)
				+ scratch.frameCandidates.capacity() * sizeof(NiTriShape*)
				+ scratch.frameEntries.capacity() * sizeof(SortedFrameEntry)
				+ scratch.facadeLookup.capacity() * sizeof(UInt32)
				+ scratch.payloadTemplates.capacity()
					* sizeof(NativeA8PayloadTemplatePtr)
				+ scratch.payloadLookup.capacity() * sizeof(UInt32)
				+ scratch.singletonFacades.capacity()
					* sizeof(const A8ShapeMetadata*);
			scratch.cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata, bytes);
		}

		void ResetSortedPrepScratch(SortedPayloadScratch& scratch)
		{
			scratch.frameAccumulator = nullptr;
			scratch.metadataShapes.clear();
			scratch.metadataOwners.clear();
			scratch.metadataAcquireShapes.clear();
			scratch.metadataAcquireOwners.clear();
			scratch.preflightCulls.clear();
			scratch.sortedOccurrenceCounts.clear();
			scratch.tieRuns.clear();
			scratch.frameEntries.clear();
			scratch.payloadTemplates.clear();
			scratch.singletonFacades.clear();
		}

		void ClearSortedFrame(SortedPayloadScratch& scratch)
		{
			FlushThinRegistrationDiagnostics();
			if (g_bEnableFreeTypeFontCrossTextBatch)
				EndNativeA8CrossTextBatchFrame();
			EndNativeA8FrameCommandBuffer();
			scratch.active = false;
			scratch.activeValidationToken = 0;
			scratch.frameAccumulator = nullptr;
			scratch.metadataShapes.clear();
			scratch.metadataOwners.clear();
			scratch.metadataAcquireShapes.clear();
			scratch.metadataAcquireOwners.clear();
			scratch.preflightCulls.clear();
			scratch.sortedOccurrenceCounts.clear();
			scratch.tieSortedLookup.clear();
			scratch.tieSortedOccurrenceCursor.clear();
			scratch.tieSortedOccurrenceNext.clear();
			scratch.tieRunIds.clear();
			scratch.tieRuns.clear();
			scratch.tieOutput.clear();
			scratch.frameCandidates.clear();
			scratch.frameEntries.clear();
			scratch.payloadTemplates.clear();
			scratch.singletonFacades.clear();

			if (scratch.metadataShapes.capacity() > 8192)
				std::vector<NiTriShape*>().swap(scratch.metadataShapes);
			if (scratch.metadataOwners.capacity() > 8192)
				std::vector<A8ShapeMetadataPtr>().swap(scratch.metadataOwners);
			if (scratch.metadataAcquireShapes.capacity() > 8192)
			{
				std::vector<NiTriShape*>().swap(
					scratch.metadataAcquireShapes);
			}
			if (scratch.metadataAcquireOwners.capacity() > 8192)
			{
				std::vector<A8ShapeMetadataPtr>().swap(
					scratch.metadataAcquireOwners);
			}
			if (scratch.preflightCulls.capacity() > 8192)
			{
				std::vector<NativeA8VisibilityCull>().swap(
					scratch.preflightCulls);
			}
			if (scratch.metadataLookup.capacity() > 16384)
				std::vector<UInt32>().swap(scratch.metadataLookup);
			if (scratch.sortedOccurrenceCounts.capacity() > 8192)
				std::vector<UInt32>().swap(scratch.sortedOccurrenceCounts);
			if (scratch.tieSortedLookup.capacity() > 16384)
				std::vector<UInt32>().swap(scratch.tieSortedLookup);
			if (scratch.tieSortedOccurrenceCursor.capacity() > 16384)
				std::vector<UInt32>().swap(scratch.tieSortedOccurrenceCursor);
			if (scratch.tieSortedOccurrenceNext.capacity() > 8192)
				std::vector<UInt32>().swap(scratch.tieSortedOccurrenceNext);
			if (scratch.tieRunIds.capacity() > 8192)
				std::vector<UInt32>().swap(scratch.tieRunIds);
			if (scratch.tieRuns.capacity() > 8192)
				std::vector<EqualDepthTieRepairRun>().swap(scratch.tieRuns);
			if (scratch.tieOutput.capacity() > 8192)
				std::vector<NiGeometry*>().swap(scratch.tieOutput);
			if (scratch.frameEntries.capacity() > 8192)
				std::vector<SortedFrameEntry>().swap(scratch.frameEntries);
			if (scratch.frameCandidates.capacity() > 8192)
				std::vector<NiTriShape*>().swap(scratch.frameCandidates);
			if (scratch.facadeLookup.capacity() > 16384)
				std::vector<UInt32>().swap(scratch.facadeLookup);
			if (scratch.payloadTemplates.capacity() > 8192)
			{
				std::vector<NativeA8PayloadTemplatePtr>().swap(
					scratch.payloadTemplates);
			}
			if (scratch.payloadLookup.capacity() > 16384)
				std::vector<UInt32>().swap(scratch.payloadLookup);
			if (scratch.singletonFacades.capacity() > 8192)
			{
				std::vector<const A8ShapeMetadata*>().swap(
					scratch.singletonFacades);
			}

			RefreshSortedScratchMemory(scratch);
			if (IsCpuMemoryBudgetExceeded())
			{
				std::vector<NiTriShape*>().swap(scratch.metadataShapes);
				std::vector<A8ShapeMetadataPtr>().swap(scratch.metadataOwners);
				std::vector<NiTriShape*>().swap(
					scratch.metadataAcquireShapes);
				std::vector<A8ShapeMetadataPtr>().swap(
					scratch.metadataAcquireOwners);
				std::vector<NativeA8VisibilityCull>().swap(
					scratch.preflightCulls);
				std::vector<UInt32>().swap(scratch.metadataLookup);
				std::vector<UInt32>().swap(scratch.sortedOccurrenceCounts);
				std::vector<UInt32>().swap(scratch.tieSortedLookup);
				std::vector<UInt32>().swap(
					scratch.tieSortedOccurrenceCursor);
				std::vector<UInt32>().swap(scratch.tieSortedOccurrenceNext);
				std::vector<UInt32>().swap(scratch.tieRunIds);
				std::vector<EqualDepthTieRepairRun>().swap(scratch.tieRuns);
				std::vector<NiGeometry*>().swap(scratch.tieOutput);
				std::vector<SortedFrameEntry>().swap(scratch.frameEntries);
				std::vector<NiTriShape*>().swap(scratch.frameCandidates);
				std::vector<UInt32>().swap(scratch.facadeLookup);
				std::vector<NativeA8PayloadTemplatePtr>().swap(
					scratch.payloadTemplates);
				std::vector<UInt32>().swap(scratch.payloadLookup);
				std::vector<const A8ShapeMetadata*>().swap(
					scratch.singletonFacades);
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

		EqualDepthTieRepairResult RepairMixedEqualDepthRunsLinear(
			SortedPayloadScratch& scratch, BSShaderAccumulator& accumulator)
		{
			EqualDepthTieRepairResult result;
			const size_t itemCount = static_cast<size_t>(accumulator.m_iNumItems);
			if (!itemCount || itemCount > kMaximumLinearTieRepairItems
				|| !accumulator.m_ppkItems || !accumulator.m_pfDepths)
			{
				return result;
			}

			NiSortedObjectList* sourceList = accumulator.m_pGeometryList
				? accumulator.m_pGeometryList : &accumulator.m_kItems;
			if (!sourceList || static_cast<size_t>(sourceList->GetSize()) != itemCount)
				return result;

			result.mixedRuns = static_cast<UInt32>(scratch.tieRuns.size());
			if (scratch.tieRuns.empty())
			{
				result.valid = true;
				return result;
			}

			// CaptureSortedFacadeTopology has already identified these runs during
			// its required facade scan. Revalidate only the candidate ranges before
			// using them; do not classify the complete sorted array a second time.
			scratch.tieRunIds.assign(itemCount, kInvalidNativeA8CommandIndex);
			UInt32 previousEnd = 0;
			for (size_t runIndex = 0;
				runIndex < scratch.tieRuns.size(); ++runIndex)
			{
				EqualDepthTieRepairRun& run = scratch.tieRuns[runIndex];
				if (run.begin >= run.end || run.end > itemCount
					|| run.begin < previousEnd)
				{
					return result;
				}
				const float depth = accumulator.m_pfDepths[run.begin];
				if ((run.begin && accumulator.m_pfDepths[run.begin - 1u] == depth)
					|| (run.end < itemCount
						&& accumulator.m_pfDepths[run.end] == depth))
				{
					return result;
				}
				bool hasFreeType = false;
				bool hasStock = false;
				const UInt32 runId = static_cast<UInt32>(runIndex);
				for (UInt32 item = run.begin; item < run.end; ++item)
				{
					if (accumulator.m_pfDepths[item] != depth)
						return result;
					const bool isFreeType =
						IsFreeTypeFacade(accumulator.m_ppkItems[item]);
					hasFreeType = hasFreeType || isFreeType;
					hasStock = hasStock || !isFreeType;
					scratch.tieRunIds[item] = runId;
				}
				if (!hasFreeType || !hasStock)
					return result;
				run.write = run.begin;
				previousEnd = run.end;
			}

			// Index every sorted occurrence once. A per-pointer stack preserves exact
			// multiplicity without copying the source list or building a bidirectional
			// source-ordinal map. Duplicate pointers are interchangeable here because
			// the desired output pointer is identical for every such occurrence.
			PrepareLookup(scratch.tieSortedLookup, itemCount);
			const size_t lookupMask = scratch.tieSortedLookup.size() - 1u;
			scratch.tieSortedOccurrenceCursor.assign(
				scratch.tieSortedLookup.size(), 0);
			scratch.tieSortedOccurrenceNext.assign(itemCount, 0);
			for (size_t item = 0; item < itemCount; ++item)
			{
				NiGeometry* geometry = accumulator.m_ppkItems[item];
				if (!geometry)
					return result;
				size_t slot = HashPointer(geometry) & lookupMask;
				bool inserted = false;
				for (size_t probe = 0;
					probe < scratch.tieSortedLookup.size(); ++probe)
				{
					const UInt32 stored = scratch.tieSortedLookup[slot];
					if (!stored)
					{
						const UInt32 occurrence = static_cast<UInt32>(item + 1u);
						scratch.tieSortedLookup[slot] = occurrence;
						scratch.tieSortedOccurrenceCursor[slot] = occurrence;
						inserted = true;
						break;
					}
					const size_t keyItem = static_cast<size_t>(stored - 1u);
					if (keyItem < itemCount
						&& accumulator.m_ppkItems[keyItem] == geometry)
					{
						const UInt32 occurrence = static_cast<UInt32>(item + 1u);
						scratch.tieSortedOccurrenceNext[item] =
							scratch.tieSortedOccurrenceCursor[slot];
						scratch.tieSortedOccurrenceCursor[slot] = occurrence;
						inserted = true;
						break;
					}
					slot = (slot + 1u) & lookupMask;
				}
				if (!inserted)
					return result;
			}

			// The stock renderer consumes high index to low index. Walk the original
			// AddTail list from tail to head and fill each mixed run from begin to end;
			// reverse rendering therefore observes the original registration order.
			// This is a single O(n) list pass and performs no comparison sort.
			scratch.tieOutput.resize(itemCount);
			NiTListIterator position = sourceList->GetTailPos();
			size_t sourceItems = 0;
			while (position && sourceItems < itemCount)
			{
				NiGeometry* geometry = sourceList->GetPrev(position);
				++sourceItems;
				if (!geometry)
					return result;
				size_t slot = HashPointer(geometry) & lookupMask;
				UInt32 sortedItem = kInvalidNativeA8CommandIndex;
				for (size_t probe = 0;
					probe < scratch.tieSortedLookup.size(); ++probe)
				{
					const UInt32 stored = scratch.tieSortedLookup[slot];
					if (!stored)
						break;
					const size_t keyItem = static_cast<size_t>(stored - 1u);
					if (keyItem < itemCount
						&& accumulator.m_ppkItems[keyItem] == geometry)
					{
						const UInt32 occurrence =
							scratch.tieSortedOccurrenceCursor[slot];
						if (!occurrence || occurrence > itemCount)
							return result;
						sortedItem = occurrence - 1u;
						scratch.tieSortedOccurrenceCursor[slot] =
							scratch.tieSortedOccurrenceNext[sortedItem];
						break;
					}
					slot = (slot + 1u) & lookupMask;
				}
				if (sortedItem >= itemCount)
					return result;
				const UInt32 runId = scratch.tieRunIds[sortedItem];
				if (runId == kInvalidNativeA8CommandIndex)
					continue;
				if (runId >= scratch.tieRuns.size())
					return result;
				EqualDepthTieRepairRun& run = scratch.tieRuns[runId];
				if (run.write >= run.end)
					return result;
				scratch.tieOutput[run.write++] = geometry;
			}
			if (position || sourceItems != itemCount)
				return result;
			for (size_t slot = 0; slot < scratch.tieSortedLookup.size(); ++slot)
			{
				if (scratch.tieSortedLookup[slot]
					&& scratch.tieSortedOccurrenceCursor[slot])
				{
					return result;
				}
			}

			for (const EqualDepthTieRepairRun& run : scratch.tieRuns)
			{
				if (run.write != run.end)
					return result;
			}
			for (const EqualDepthTieRepairRun& run : scratch.tieRuns)
			{
				bool changed = false;
				for (UInt32 item = run.begin; item < run.end; ++item)
				{
					if (accumulator.m_ppkItems[item] != scratch.tieOutput[item])
					{
						changed = true;
						++result.changedItems;
					}
				}
				if (!changed)
					continue;
				++result.changedRuns;
				for (UInt32 item = run.begin; item < run.end; ++item)
					accumulator.m_ppkItems[item] = scratch.tieOutput[item];
			}
			result.valid = true;
			return result;
		}

		bool CaptureSortedFacadeTopology(SortedPayloadScratch& scratch,
			BSShaderAccumulator* accumulator, SInt64* topologyTicks)
		{
			const SInt64 topologyStart = BeginFreeTypePerfSample();
			auto finishTopology = [&](bool result)
			{
				const SInt64 ticks = EndFreeTypePerfSample(
					FreeTypePerfPhase::FramePrepTopology, topologyStart);
				if (topologyTicks)
					*topologyTicks = ticks;
				return result;
			};

			if (!accumulator || scratch.active || scratch.nestedBypassDepth
				|| accumulator->eRenderMode
					!= BSShaderManager::BSSM_RENDER_TILES
				|| accumulator->m_iNumItems <= 0
				|| !accumulator->m_ppkItems)
			{
				++s_thinRegistrationDiagnostics.sortedScanFallback;
				return finishTopology(false);
			}

			const size_t itemCount =
				static_cast<size_t>(accumulator->m_iNumItems);
			PrepareLookup(scratch.metadataLookup, itemCount);
			const size_t metadataMask = scratch.metadataLookup.size() - 1u;
			const bool trackEqualDepthRuns = accumulator->m_pfDepths != nullptr;
			float activeRunDepth = trackEqualDepthRuns
				? accumulator->m_pfDepths[0] : 0.0f;
			size_t activeRunBegin = 0;
			bool activeRunHasFreeType = false;
			bool activeRunHasStock = false;
			bool mixedEqualDepthCandidate = false;
			for (SInt32 itemIndex = 0;
				itemIndex < accumulator->m_iNumItems; ++itemIndex)
			{
				NiGeometry* geometry = accumulator->m_ppkItems[itemIndex];
				const bool isFreeType = IsFreeTypeFacade(geometry);
				if (trackEqualDepthRuns)
				{
					const float depth = accumulator->m_pfDepths[itemIndex];
					if (itemIndex && depth != activeRunDepth)
					{
						if (activeRunHasFreeType && activeRunHasStock)
						{
							scratch.tieRuns.push_back({
								static_cast<UInt32>(activeRunBegin),
								static_cast<UInt32>(itemIndex),
								static_cast<UInt32>(activeRunBegin) });
							mixedEqualDepthCandidate = true;
						}
						activeRunDepth = depth;
						activeRunBegin = static_cast<size_t>(itemIndex);
						activeRunHasFreeType = false;
						activeRunHasStock = false;
					}
					activeRunHasFreeType = activeRunHasFreeType || isFreeType;
					activeRunHasStock = activeRunHasStock || !isFreeType;
				}
				if (!isFreeType)
					continue;

				NiTriShape* facade = static_cast<NiTriShape*>(geometry);
				size_t slot = HashPointer(facade) & metadataMask;
				bool accounted = false;
				for (size_t probe = 0;
					probe < scratch.metadataLookup.size(); ++probe)
				{
					const UInt32 stored = scratch.metadataLookup[slot];
					if (!stored)
					{
						const size_t metadataIndex =
							scratch.metadataShapes.size();
						scratch.metadataShapes.push_back(facade);
						scratch.sortedOccurrenceCounts.push_back(1);
						scratch.metadataLookup[slot] =
							static_cast<UInt32>(metadataIndex + 1u);
						accounted = true;
						break;
					}
					const size_t metadataIndex =
						static_cast<size_t>(stored - 1u);
					if (metadataIndex < scratch.metadataShapes.size()
						&& scratch.metadataShapes[metadataIndex] == facade)
					{
						++scratch.sortedOccurrenceCounts[metadataIndex];
						accounted = true;
						break;
					}
					slot = (slot + 1u) & metadataMask;
				}
				if (!accounted)
				{
					scratch.metadataShapes.clear();
					scratch.sortedOccurrenceCounts.clear();
					++s_thinRegistrationDiagnostics.sortedScanFallback;
					return finishTopology(false);
				}
			}
			if (trackEqualDepthRuns
				&& activeRunHasFreeType && activeRunHasStock)
			{
				scratch.tieRuns.push_back({
					static_cast<UInt32>(activeRunBegin),
					static_cast<UInt32>(itemCount),
					static_cast<UInt32>(activeRunBegin) });
				mixedEqualDepthCandidate = true;
			}

			if (mixedEqualDepthCandidate)
			{
				const EqualDepthTieRepairResult repair =
					RepairMixedEqualDepthRunsLinear(scratch, *accumulator);
				if (!repair.valid)
				{
					if (g_bEnableFreeTypeFontRenderingLog
						&& !s_loggedEqualDepthTieRepairFailure)
					{
						s_loggedEqualDepthTieRepairFailure = true;
						FreeTypeFontDebugLog(
							"tnvse_freetype_equal_depth_repair: status=fail-open items=%u",
							static_cast<UInt32>(itemCount));
					}
				}
				else if (repair.changedRuns
					&& g_bEnableFreeTypeFontRenderingLog
					&& !s_loggedEqualDepthTieRepair)
				{
					s_loggedEqualDepthTieRepair = true;
					FreeTypeFontDebugLog(
						"tnvse_freetype_equal_depth_repair: status=applied items=%u mixed_runs=%u changed_runs=%u changed_items=%u algorithm=linear-source-ordinal",
						static_cast<UInt32>(itemCount), repair.mixedRuns,
						repair.changedRuns, repair.changedItems);
				}
			}

			scratch.frameAccumulator = accumulator;
			return finishTopology(true);
		}

		void ResolveSingletonFacadeSortedTopology(
			SortedPayloadScratch& scratch, BSShaderAccumulator* accumulator,
			UInt64 validationToken)
		{
			if (!accumulator || !validationToken
				|| scratch.frameAccumulator != accumulator
				|| !accumulator->m_ppkItems)
			{
				++s_thinRegistrationDiagnostics.sortedScanFallback;
				return;
			}

			for (const A8ShapeMetadataPtr& owner : scratch.metadataOwners)
			{
				const A8ShapeMetadata* metadata = owner.get();
				if (!metadata || metadata->backend
					!= FreeTypeShapeBackend::SingletonFacade)
				{
					continue;
				}

				SingletonFacadeState* singleton =
					GetSingletonFacadeState(*metadata);
				if (singleton)
				{
					singleton->topologyValidationToken = 0;
					singleton->preflightValidationToken = 0;
					if (singleton->frameMode.load(std::memory_order_acquire)
						!= SingletonFacadeFrameMode::Retired)
					{
						singleton->frameMode.store(
							SingletonFacadeFrameMode::Facade,
							std::memory_order_release);
					}
				}

				const size_t metadataIndex = LookupMetadataShapeIndex(
					scratch, metadata->shapeIdentity);
				const bool occurrenceValid =
					metadataIndex < scratch.sortedOccurrenceCounts.size()
					&& scratch.sortedOccurrenceCounts[metadataIndex] == 1;
				const bool valid = singleton
					&& metadata->selfIdentity == metadata
					&& metadata->allocationId
					&& singleton->slot.shape == metadata->shapeIdentity
					&& occurrenceValid
					&& singleton->frameMode.load(std::memory_order_acquire)
						!= SingletonFacadeFrameMode::Retired;
				if (valid)
				{
					singleton->topologyValidationToken = validationToken;
					++s_thinRegistrationDiagnostics.facadeTopology;
				}
				else
				{
					++s_thinRegistrationDiagnostics.facadeFallback;
					if (!occurrenceValid)
						++s_thinRegistrationDiagnostics.occurrenceFallback;
					RestoreSingletonFacade(*metadata,
						NativeA8FallbackReason::PropertySync);
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

		NativeA8FallbackReason PreflightNativeFacadeImpl(NiTriShape* facade,
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
			if (payload.topologyObserved
				&& payload.lastTopologyComposite
					!= payload.useCompositePackets)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::SingletonFacadeTopologySwitch);
			}
			payload.topologyObserved = true;
			payload.lastTopologyComposite = payload.useCompositePackets;

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
			return current == &NativeA8RegisterObject
				&& s_originalTileRegisterObject.load(
					std::memory_order_relaxed) != nullptr
				&& state.originalRenderAlphaGeometry
				&& hook_identity::MatchesRel32InstructionImageUnchecked(
					kRenderAlphaGeometryCallSite,
					s_renderAlphaGeometryHookImage)
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
			++diagnostics.calls;
			ThinRegistrationSampleScope timing(ShouldSampleRegisterRoute());
			if (timing.IsActive())
				++diagnostics.samples;

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
				const SInt64 framePrepStart = BeginFreeTypePerfSample();
				const size_t itemCount = static_cast<size_t>(
					accumulator->m_iNumItems);
				FreeTypeAccumulatorPrepTailSample prepTailSample;
				prepTailSample.itemCount = static_cast<UInt32>(itemCount);
				{
					FreeTypePerfScope resetPrepPerf(
						FreeTypePerfPhase::FramePrepReset, true,
						&prepTailSample.resetTicks);
					ResetSortedPrepScratch(scratch);
				}
				CaptureSortedFacadeTopology(scratch, accumulator,
					&prepTailSample.topologyTicks);
				if (scratch.metadataShapes.empty())
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::AccumulatorEmptyFastPath);
					// The predecessor Sort contains no captured native facade.  This is
					// either an entirely stock traversal or a fail-open topology scan;
					// in both cases the ordinary dispatch path remains authoritative.
					// Do not build a readiness stamp, command frame, or empty cross-text
					// sequence.
					const SInt64 framePrepTicks = EndFreeTypePerfSample(
						FreeTypePerfPhase::FrameRoutePrep, framePrepStart);
					prepTailSample.totalTicks = framePrepTicks;
					RecordFreeTypeAccumulatorPrepTailSample(prepTailSample);
					{
						FreeTypePerfScope stockRenderPerf(
							FreeTypePerfPhase::FrameRouteStockRender);
						state.originalRenderAlphaGeometry(accumulator);
					}
					ClearSortedFrame(scratch);
					return;
				}

				// Visibility is now the first post-Sort preflight.  The facade owns
				// the final model bound and Tile/scissor state, so a proven cull does
				// not need a metadata owner, packet artifact, or renderer-ring slot.
				{
					FreeTypePerfScope visibilityPrepPerf(
						FreeTypePerfPhase::FramePrepVisibility, true,
						&prepTailSample.visibilityTicks);
					scratch.preflightCulls.assign(scratch.metadataShapes.size(),
						NativeA8VisibilityCull::None);
					scratch.metadataAcquireShapes.clear();
					scratch.metadataAcquireShapes.reserve(
						scratch.metadataShapes.size());
					for (size_t index = 0;
						index < scratch.metadataShapes.size(); ++index)
					{
						NiTriShape* facade = scratch.metadataShapes[index];
						NativeA8VisibilityCull cull =
							EvaluateNativeA8SubmissionVisibility(facade);
						if (cull == NativeA8VisibilityCull::None)
						{
							cull = EvaluateNativeA8PreflightClipVisibility(facade);
						}
						scratch.preflightCulls[index] = cull;
						if (cull == NativeA8VisibilityCull::None)
							scratch.metadataAcquireShapes.push_back(facade);
						else
						{
							RecordFreeTypePerf(FreeTypePerfCounter::
								VisibilityPreflightSkipped);
							RecordFreeTypePerf(FreeTypePerfCounter::
								AccumulatorMetadataCullSkipped);
						}
					}
					scratch.metadataOwners.assign(
						scratch.metadataShapes.size(), {});
				}
				if (!scratch.metadataAcquireShapes.empty())
				{
					FreeTypePerfScope metadataPerf(
						FreeTypePerfPhase::FramePrepMetadata, true,
						&prepTailSample.metadataTicks);
					AcquireA8ShapeMetadataBatch(
						scratch.metadataAcquireShapes,
						scratch.metadataAcquireOwners);
					for (const A8ShapeMetadataPtr& owner
						: scratch.metadataAcquireOwners)
					{
						if (!owner)
							++s_thinRegistrationDiagnostics.metadataMissing;
					}
					for (size_t ownerIndex = 0;
						ownerIndex < scratch.metadataAcquireShapes.size();
						++ownerIndex)
					{
						const size_t metadataIndex = LookupMetadataShapeIndex(
							scratch,
							scratch.metadataAcquireShapes[ownerIndex]);
						if (metadataIndex < scratch.metadataOwners.size()
							&& ownerIndex
								< scratch.metadataAcquireOwners.size())
						{
							scratch.metadataOwners[metadataIndex] =
								std::move(
									scratch.metadataAcquireOwners[ownerIndex]);
						}
					}
					++s_thinRegistrationDiagnostics.metadataBatches;
					s_thinRegistrationDiagnostics.metadataShapes +=
						static_cast<UInt64>(
							scratch.metadataAcquireShapes.size());
				}

				NativePreflightFrameContext preflightContext;
				const SInt64 facadePrepStart = BeginFreeTypePerfSample();
				const bool hasMetadataSurvivors =
					!scratch.metadataAcquireShapes.empty();
				UInt32 generation = 0;
				UInt64 frameValidationToken = 0;
				{
					FreeTypePerfScope readinessPrepPerf(
						FreeTypePerfPhase::FramePrepReadiness, true,
						&prepTailSample.readinessTicks);
					if (hasMetadataSurvivors
						&& g_bEnableFreeTypeFontStructuralFastPaths)
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
					else if (hasMetadataSurvivors)
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
					generation = preflightContext.generation;
					frameValidationToken = ++scratch.nextValidationToken;
					if (!frameValidationToken)
						frameValidationToken = ++scratch.nextValidationToken;
				}
				{
					FreeTypePerfScope lookupPrepPerf(
						FreeTypePerfPhase::FramePrepLookup, true,
						&prepTailSample.lookupTicks);
					scratch.frameCandidates = scratch.metadataShapes;

					const size_t trackedCount = scratch.frameCandidates.size();
					scratch.frameEntries.reserve(trackedCount);
					scratch.payloadTemplates.reserve(trackedCount);
					scratch.singletonFacades.reserve(
						std::min<size_t>(trackedCount, 1024));
					PrepareLookup(scratch.facadeLookup, trackedCount);
					PrepareLookup(scratch.payloadLookup, trackedCount);
					if (hasMetadataSurvivors)
					{
						ResolveSingletonFacadeSortedTopology(
							scratch, accumulator, frameValidationToken);
					}
				}

				{
					FreeTypePerfScope facadeLoopPrepPerf(
						FreeTypePerfPhase::FramePrepFacadeLoop, true,
						&prepTailSample.facadeLoopTicks);
					for (size_t candidateIndex = 0;
						candidateIndex < scratch.frameCandidates.size();
						++candidateIndex)
					{
					NiTriShape* facade =
						scratch.frameCandidates[candidateIndex];
					if (LookupSortedFacade(scratch, facade)
						!= std::numeric_limits<size_t>::max())
					{
						continue;
					}

					SortedFrameEntry entry;
					entry.facade = facade;
					entry.visibilityCull = candidateIndex
						< scratch.preflightCulls.size()
						? scratch.preflightCulls[candidateIndex]
						: NativeA8VisibilityCull::None;
					if (entry.visibilityCull == NativeA8VisibilityCull::None)
						entry.metadata = FindBatchedMetadata(scratch, facade);
					entry.generation = generation;
					SingletonFacadeState* singletonFacade =
						nullptr;
					bool topologyReady = false;
					if (!entry.metadata
						&& entry.visibilityCull == NativeA8VisibilityCull::None)
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
					else if (entry.metadata
						&& entry.metadata->backend
						== FreeTypeShapeBackend::SingletonFacade)
					{
						singletonFacade =
							GetSingletonFacadeState(*entry.metadata);
						topologyReady = singletonFacade
							&& singletonFacade->topologyValidationToken
								== frameValidationToken;
					}

					if (entry.visibilityCull != NativeA8VisibilityCull::None)
					{
						// Dispatch revalidates the volatile cull inputs.  A revoked proof
						// falls back to a one-shape metadata lookup instead of making every
						// proven-offscreen facade pay the batch acquisition cost.
					}
					else if (entry.metadata
						&& entry.metadata->nativePayload.buildComplete)
					{
						entry.payload = &entry.metadata->nativePayload;
						entry.preflightResult = PreflightNativeFacadeImpl(
							facade, *entry.metadata, *entry.payload,
							&preflightContext, nullptr);
						if (entry.preflightResult
								== NativeA8FallbackReason::None)
						{
							entry.generation =
								entry.payload->preparedGeneration;
							entry.validationToken = frameValidationToken;
							if (singletonFacade
								&& topologyReady)
							{
								singletonFacade->
									preflightValidationToken =
										frameValidationToken;
								scratch.singletonFacades.push_back(
									entry.metadata);
							}
						}
						else if (singletonFacade && topologyReady)
						{
							RestoreSingletonFacade(
								*entry.metadata, entry.preflightResult);
						}
					}
					else
					{
						entry.preflightResult =
							NativeA8FallbackReason::PacketBuild;
					}

					const size_t metadataIndex = entry.metadata
						? LookupMetadataShapeIndex(scratch, facade)
						: std::numeric_limits<size_t>::max();
					entry.uniqueOccurrence = metadataIndex
						< scratch.sortedOccurrenceCounts.size()
						&& scratch.sortedOccurrenceCounts[metadataIndex] == 1;
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
				}
				EndFreeTypePerfSample(
					FreeTypePerfPhase::FramePrepFacades,
					facadePrepStart);
				const bool hasPreparedPayloads =
					!scratch.payloadTemplates.empty();
				if (!hasPreparedPayloads)
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::AccumulatorNoPreparedPayload);
				}
				if (hasPreparedPayloads)
				{
					FreeTypePerfScope ringPrepPerf(
						FreeTypePerfPhase::FramePrepRing, true,
						&prepTailSample.ringTicks);
					PrepareSortedNativeA8Payloads(
						scratch.payloadTemplates, generation);
				}
				if (hasPreparedPayloads)
				{
					FreeTypePerfScope singletonPrepPerf(
						FreeTypePerfPhase::FramePrepSingletons, true,
						&prepTailSample.singletonTicks);
					for (const A8ShapeMetadata* metadata
						: scratch.singletonFacades)
					{
						SingletonFacadeState* singleton = metadata
							? GetSingletonFacadeState(*metadata) : nullptr;
						if (!singleton
							|| singleton->preflightValidationToken
								!= frameValidationToken)
						{
							if (metadata)
							{
								RestoreSingletonFacade(*metadata,
									NativeA8FallbackReason::RuntimeFault);
							}
							continue;
						}
						PrepareSingletonFacadeForSortedFrame(*metadata,
							generation, preflightContext.atlasTextureEpoch,
							frameValidationToken);
					}
				}
				const bool commandFrameActive =
					g_bEnableFreeTypeFontCommandBuffer
					&& hasPreparedPayloads;
				if (commandFrameActive)
				{
					FreeTypePerfScope commandBuild(
						FreeTypePerfPhase::CommandBuild, true,
						&prepTailSample.commandTicks);
					{
						FreeTypePerfScope commandBuildStamp(
							FreeTypePerfPhase::CommandBuildStamp);
						BeginNativeA8FrameCommandBuffer(accumulator,
							frameValidationToken, generation,
							preflightContext.atlasTextureEpoch);
						ReserveNativeA8FrameCommandBuffer(
							scratch.frameEntries.size(),
							scratch.singletonFacades.size());
					}
					{
						FreeTypePerfScope commandBuildDirectFacade(
							FreeTypePerfPhase::CommandBuildDirectFacade);
						for (const A8ShapeMetadata* metadata
							: scratch.singletonFacades)
						{
							if (metadata)
							{
								AddNativeA8FrameDirectFacadeCommand(
									metadata);
							}
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
									!= NativeA8VisibilityCull::None
								|| !entry.uniqueOccurrence)
							{
								continue;
							}
							if (entry.metadata->backend
								== FreeTypeShapeBackend::
									SingletonFacade)
							{
								SingletonFacadeState* singleton =
									GetSingletonFacadeState(
										*entry.metadata);
								if (singleton
									&& singleton->frameMode.load(
										std::memory_order_acquire)
										== SingletonFacadeFrameMode::Direct
									&& singleton->commandValidationToken.load(
										std::memory_order_acquire)
										== frameValidationToken)
								{
									continue;
								}
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
											SingletonFacade)
									{
										SingletonFacadeState* singleton =
											GetSingletonFacadeState(*metadata);
										if (singleton
											&& singleton->frameMode.load(
												std::memory_order_acquire)
												== SingletonFacadeFrameMode::Direct
											&& singleton->commandValidationToken.load(
												std::memory_order_acquire)
												== frameValidationToken)
										{
											commandIndex = singleton->
												commandDirectFacadeSinglePacketIndex.load(
													std::memory_order_acquire);
											if (commandIndex
												!= kInvalidNativeA8CommandIndex)
											{
												kind = NativeA8CrossTextCommandKind::
													DirectFacadeSinglePacket;
											}
										}
									}
									else if (entry->singlePacketCommandIndex
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

							// Consume the predecessor Sort result directly in the same
							// high-to-low order as retail RenderAlphaGeometry, without a
							// second per-item staging vector and its write/read round trip.
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
						if (g_bEnableFreeTypeFontCrossTextBatch)
							PrepareNativeA8CrossTextBatches();
					}
				}
				else
				{
					EndNativeA8FrameCommandBuffer();
				}
				{
					FreeTypePerfScope publishPrepPerf(
						FreeTypePerfPhase::FramePrepPublish, true,
						&prepTailSample.publishTicks);
					RefreshSortedScratchMemory(scratch);
					if (hasPreparedPayloads)
					{
						BeginNativeA8SortedShaderBatch();
						BeginA8SortedTileConstantOwnership();
					}
					scratch.activeValidationToken = frameValidationToken;
					scratch.active = true;
				}
				const SInt64 framePrepTicks = EndFreeTypePerfSample(
					FreeTypePerfPhase::FrameRoutePrep, framePrepStart);
				prepTailSample.totalTicks = framePrepTicks;
				prepTailSample.facadeCount =
					static_cast<UInt32>(scratch.metadataShapes.size());
				prepTailSample.survivorCount = static_cast<UInt32>(
					scratch.metadataAcquireShapes.size());
				prepTailSample.payloadCount =
					static_cast<UInt32>(scratch.payloadTemplates.size());
				prepTailSample.singletonCount =
					static_cast<UInt32>(scratch.singletonFacades.size());
				prepTailSample.commandFrameActive = commandFrameActive;
				RecordFreeTypeAccumulatorPrepTailSample(prepTailSample);
				{
					FreeTypePerfScope stockRenderPerf(
						FreeTypePerfPhase::FrameRouteStockRender);
					state.originalRenderAlphaGeometry(accumulator);
				}
				if (hasPreparedPayloads)
				{
					EndA8SortedTileConstantOwnership();
					EndNativeA8SortedShaderBatch();
				}
				if (commandFrameActive
					&& g_bEnableFreeTypeFontCrossTextBatch)
					EndNativeA8CrossTextBatchFrame();
				EndNativeA8FrameCommandBuffer();
				EndNativeA8SortedRingFrame();
				ClearSortedFrame(scratch);
				return;
			}

			const bool clearSortedOwners =
				scratch.frameAccumulator == accumulator;
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
					InvalidateAllSingletonFacadeBindings();
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
				InvalidateAllSingletonFacadeBindings();
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
				InvalidateAllSingletonFacadeBindings();
				NotifyNativeA8CommandExternalMutation(
					NativeA8CommandFallback::Atlas);
				return;
			}
		}
	}

	NativeA8FallbackReason PrepareNativeA8Facade(NiTriShape* facade,
		const A8ShapeMetadata& metadata, NativeA8ShapePayload& payload)
	{
		return PreflightNativeFacadeImpl(facade, metadata, payload);
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
			HookRenderAlphaGeometry();
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
				InvalidateAllSingletonFacadeBindings();
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
				InvalidateAllSingletonFacadeBindings();
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
				InvalidateAllSingletonFacadeBindings();
			}
			return false;
		}

		s_loggedTileRegisterObjectConflict = false;
		s_loggedTileRegisterObjectSlotUnavailable = false;
		HookRenderAlphaGeometry();
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
			&& IsNativeA8RenderAlphaGeometryHookCurrent();
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
		return tileCurrent && renderAlphaCurrent;
	}
}

#include "font_native_shape_internal.h"
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
		inline constexpr UInt32 kBSShaderAccumulatorRegisterObjectFunctions =
			0x11F9F80;
		inline constexpr UInt32 kTileRegisterObjectFunctionEntry =
			kBSShaderAccumulatorRegisterObjectFunctions
			+ static_cast<UInt32>(BSShaderManager::BSSM_RENDER_TILES)
				* sizeof(void*);
		inline constexpr UInt32 kBSShaderAccumulatorRegisterObjectInterface =
			0xB65A90;
		inline constexpr UInt32 kMaximumMissingMetadataLogs = 8;
		inline constexpr size_t kMaximumLinearTieRepairItems = 8192;
		inline constexpr UInt32 kRegisterRouteSampleRate = 256;

		using TileRegisterObjectFn = bool(__cdecl*)(BSShaderAccumulator*,
			NiGeometry*, const NiPropertyState*, BSShaderProperty*, BSShader*);
		static_assert(kTileRegisterObjectFunctionEntry == 0x11F9FA8);
		static_assert(sizeof(TileRegisterObjectFn) == sizeof(UInt32));

		std::atomic<TileRegisterObjectFn> s_predecessorTileRegisterObject = nullptr;
		bool s_loggedTileRegisterObjectConflict = false;
		bool s_loggedTileRegisterObjectSlotUnavailable = false;
		std::atomic<UInt32> s_missingMetadataLogCount = 0;
		std::atomic<UInt32> s_atlasTextureEpoch = 1;
		thread_local bool s_loggedEqualDepthTieRepair = false;
		thread_local bool s_loggedEqualDepthTieRepairFailure = false;

		bool __cdecl NativeFontRegisterObject(BSShaderAccumulator* accumulator,
			NiGeometry* geometry, const NiPropertyState* properties,
			BSShaderProperty* shaderProperty, BSShader* shader);
		bool IsTileRegisterObjectSlotWritable();
		void __fastcall NativeFontRenderAlphaGeometry(
			BSShaderAccumulator* accumulator, void*);
		const hook_identity::Rel32InstructionImage
			s_renderAlphaGeometryHookImage =
				hook_identity::MakeRel32InstructionImage(
					kRenderAlphaGeometryCallSite,
					hook_identity::Rel32Opcode::Call,
					reinterpret_cast<SIZE_T>(
						&NativeFontRenderAlphaGeometry));

		struct SortedFrameEntry
		{
			NiTriShape* facade = nullptr;
			// metadataOwners holds the batch-acquired shared owner until the vanilla
			// traversal completes; entries use stable non-owning views.
			const NativeFontShapeMetadata* metadata = nullptr;
			NativeFontShapePayload* payload = nullptr;
			NativeFontFallbackReason preflightResult =
				NativeFontFallbackReason::RuntimeFault;
			NativeFontVisibilityPreflight visibility;
			UInt32 generation = 0;
			UInt64 validationToken = 0;
			UInt32 commandSpanIndex = kInvalidNativeFontCommandIndex;
			UInt32 singlePacketCommandIndex =
				kInvalidNativeFontCommandIndex;
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
			std::vector<NativeFontShapeMetadataPtr> metadataOwners;
			std::vector<NiTriShape*> metadataAcquireShapes;
			std::vector<NativeFontShapeMetadataPtr> metadataAcquireOwners;
			std::vector<NativeFontVisibilityPreflight> visibilityPreflights;
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
			std::vector<NativeFontPayloadTemplatePtr> payloadTemplates;
			std::vector<UInt32> payloadLookup;
			std::vector<const NativeFontShapeMetadata*> singletonFacades;
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
			const NativeFontShapeState& state = State();
			const TileRegisterObjectFn predecessor =
				s_predecessorTileRegisterObject.load(std::memory_order_relaxed);
			mix(&predecessor, sizeof(predecessor));
			mix(&state.predecessorRenderAlphaGeometry,
				sizeof(state.predecessorRenderAlphaGeometry));
			mix(&state.predecessorRenderPassImmediately,
				sizeof(state.predecessorRenderPassImmediately));
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
			const NativeFontPayloadTemplatePtr& payloadTemplate)
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
					* sizeof(NativeFontShapeMetadataPtr)
				+ scratch.metadataAcquireShapes.capacity()
					* sizeof(NiTriShape*)
				+ scratch.metadataAcquireOwners.capacity()
					* sizeof(NativeFontShapeMetadataPtr)
				+ scratch.visibilityPreflights.capacity()
					* sizeof(NativeFontVisibilityPreflight)
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
					* sizeof(NativeFontPayloadTemplatePtr)
				+ scratch.payloadLookup.capacity() * sizeof(UInt32)
				+ scratch.singletonFacades.capacity()
					* sizeof(const NativeFontShapeMetadata*);
			scratch.cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata, bytes);
		}

		void ResetSortedPrepScratch(SortedPayloadScratch& scratch)
		{
			EndNativeFontVisibilityFrame();
			scratch.frameAccumulator = nullptr;
			scratch.metadataShapes.clear();
			scratch.metadataOwners.clear();
			scratch.metadataAcquireShapes.clear();
			scratch.metadataAcquireOwners.clear();
			scratch.visibilityPreflights.clear();
			scratch.sortedOccurrenceCounts.clear();
			scratch.tieRuns.clear();
			scratch.frameEntries.clear();
			scratch.payloadTemplates.clear();
			scratch.singletonFacades.clear();
		}

		void ClearSortedFrame(SortedPayloadScratch& scratch)
		{
			EndNativeFontVisibilityFrame();
			FlushThinRegistrationDiagnostics();
			EndNativeFontFrameCommandBuffer();
			scratch.active = false;
			scratch.activeValidationToken = 0;
			scratch.frameAccumulator = nullptr;
			scratch.metadataShapes.clear();
			scratch.metadataOwners.clear();
			scratch.metadataAcquireShapes.clear();
			scratch.metadataAcquireOwners.clear();
			scratch.visibilityPreflights.clear();
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
				std::vector<NativeFontShapeMetadataPtr>().swap(scratch.metadataOwners);
			if (scratch.metadataAcquireShapes.capacity() > 8192)
			{
				std::vector<NiTriShape*>().swap(
					scratch.metadataAcquireShapes);
			}
			if (scratch.metadataAcquireOwners.capacity() > 8192)
			{
				std::vector<NativeFontShapeMetadataPtr>().swap(
					scratch.metadataAcquireOwners);
			}
			if (scratch.visibilityPreflights.capacity() > 8192)
			{
				std::vector<NativeFontVisibilityPreflight>().swap(
					scratch.visibilityPreflights);
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
				std::vector<NativeFontPayloadTemplatePtr>().swap(
					scratch.payloadTemplates);
			}
			if (scratch.payloadLookup.capacity() > 16384)
				std::vector<UInt32>().swap(scratch.payloadLookup);
			if (scratch.singletonFacades.capacity() > 8192)
			{
				std::vector<const NativeFontShapeMetadata*>().swap(
					scratch.singletonFacades);
			}

			RefreshSortedScratchMemory(scratch);
			if (IsCpuMemoryBudgetExceeded())
			{
				std::vector<NiTriShape*>().swap(scratch.metadataShapes);
				std::vector<NativeFontShapeMetadataPtr>().swap(scratch.metadataOwners);
				std::vector<NiTriShape*>().swap(
					scratch.metadataAcquireShapes);
				std::vector<NativeFontShapeMetadataPtr>().swap(
					scratch.metadataAcquireOwners);
				std::vector<NativeFontVisibilityPreflight>().swap(
					scratch.visibilityPreflights);
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
				std::vector<NativeFontPayloadTemplatePtr>().swap(
					scratch.payloadTemplates);
				std::vector<UInt32>().swap(scratch.payloadLookup);
				std::vector<const NativeFontShapeMetadata*>().swap(
					scratch.singletonFacades);
				scratch.cpuMemory.Release();
			}
		}

		RenderAlphaGeometryFn ReadRenderAlphaGeometryCallTarget()
		{
			SIZE_T callTarget = 0;
			if (!hook_identity::ReadRel32Target(
				kRenderAlphaGeometryCallSite,
				hook_identity::Rel32Opcode::Call,
				callTarget))
			{
				return nullptr;
			}
			return reinterpret_cast<RenderAlphaGeometryFn>(callTarget);
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

		SafeWrite32IfEqualResult PublishTileRegisterObjectHook(
			TileRegisterObjectFn expected)
		{
			SafeWrite32IfEqualResult result;
			if (!expected)
			{
				result.protectionError = ERROR_INVALID_PARAMETER;
				return result;
			}
			if (!IsTileRegisterObjectSlotWritable())
			{
				result.protectionError = ERROR_NOACCESS;
				return result;
			}
			const SIZE_T expectedBits = reinterpret_cast<SIZE_T>(expected);
			// BSShaderAccumulator::Tile RegisterObject callback (__cdecl).
			// Use CAS because NVTF/NVHR may publish this slot concurrently.
			return SafeWrite32IfEqualDetailed(kTileRegisterObjectFunctionEntry,
				reinterpret_cast<SIZE_T>(&NativeFontRegisterObject),
				expectedBits);
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

		// The 52-byte compatibility/singleton facade participates in the native
		// payload, ring and command paths.  The 40/48-byte Vanilla-layout shape is
		// tracked only across the predecessor's final Sort so it can reuse the
		// persistent visibility context and the witnessed shader-state scope.
		enum class NativeSortedShapeKind : UInt8
		{
			None,
			DirectFacade,
			VanillaLayout
		};
		NativeSortedShapeKind ClassifyNativeSortedShape(
			const NiGeometry* geometry);
		bool IsDirectNativeFacade(const NiGeometry* geometry);

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

		const NativeFontShapeMetadata* FindBatchedMetadata(
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

			// CaptureSortedTrackedShapeTopology has already identified these runs
			// during its required sorted scan. Revalidate only the candidate ranges
			// before using them; do not classify the complete sorted array twice.
			scratch.tieRunIds.assign(itemCount, kInvalidNativeFontCommandIndex);
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
				bool hasDirectFacade = false;
				bool hasForeignGeometry = false;
				const UInt32 runId = static_cast<UInt32>(runIndex);
				for (UInt32 item = run.begin; item < run.end; ++item)
				{
					if (accumulator.m_pfDepths[item] != depth)
						return result;
					const bool isDirectFacade =
						IsDirectNativeFacade(accumulator.m_ppkItems[item]);
					hasDirectFacade = hasDirectFacade || isDirectFacade;
					hasForeignGeometry = hasForeignGeometry || !isDirectFacade;
					scratch.tieRunIds[item] = runId;
				}
				if (!hasDirectFacade || !hasForeignGeometry)
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

			// The vanilla renderer consumes high index to low index. Walk the original
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
				UInt32 sortedItem = kInvalidNativeFontCommandIndex;
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
				if (runId == kInvalidNativeFontCommandIndex)
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

		bool CaptureSortedTrackedShapeTopology(SortedPayloadScratch& scratch,
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
			bool activeRunHasDirectFacade = false;
			bool activeRunHasForeignGeometry = false;
			bool mixedEqualDepthCandidate = false;
			for (SInt32 itemIndex = 0;
				itemIndex < accumulator->m_iNumItems; ++itemIndex)
			{
				NiGeometry* geometry = accumulator->m_ppkItems[itemIndex];
				const NativeSortedShapeKind shapeKind =
					ClassifyNativeSortedShape(geometry);
				const bool isDirectFacade = shapeKind
					== NativeSortedShapeKind::DirectFacade;
				const bool isTrackedShape = shapeKind
					!= NativeSortedShapeKind::None;
				if (trackEqualDepthRuns)
				{
					const float depth = accumulator->m_pfDepths[itemIndex];
					if (itemIndex && depth != activeRunDepth)
					{
						if (activeRunHasDirectFacade
							&& activeRunHasForeignGeometry)
						{
							scratch.tieRuns.push_back({
								static_cast<UInt32>(activeRunBegin),
								static_cast<UInt32>(itemIndex),
								static_cast<UInt32>(activeRunBegin) });
							mixedEqualDepthCandidate = true;
						}
						activeRunDepth = depth;
						activeRunBegin = static_cast<size_t>(itemIndex);
						activeRunHasDirectFacade = false;
						activeRunHasForeignGeometry = false;
					}
					activeRunHasDirectFacade =
						activeRunHasDirectFacade || isDirectFacade;
					activeRunHasForeignGeometry =
						activeRunHasForeignGeometry || !isDirectFacade;
				}
				if (!isTrackedShape)
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
				&& activeRunHasDirectFacade && activeRunHasForeignGeometry)
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

			for (const NativeFontShapeMetadataPtr& owner : scratch.metadataOwners)
			{
				const NativeFontShapeMetadata* metadata = owner.get();
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
						NativeFontFallbackReason::PropertySync);
				}
			}
		}

		NativeSortedShapeKind ClassifyNativeSortedShape(
			const NiGeometry* geometry)
		{
			if (!geometry || !State().originalTriShapeVtable)
				return NativeSortedShapeKind::None;
			void* const* vtable = *reinterpret_cast<void* const* const*>(geometry);
			if (vtable == &State().triShapeVtable[1])
				return NativeSortedShapeKind::DirectFacade;
			if (vtable == &State().vanillaLayoutTriShapeVtable[1])
				return NativeSortedShapeKind::VanillaLayout;
			return NativeSortedShapeKind::None;
		}

		bool IsDirectNativeFacade(const NiGeometry* geometry)
		{
			return ClassifyNativeSortedShape(geometry)
				== NativeSortedShapeKind::DirectFacade;
		}

		void ClearNativePacketFailure(NativeFontShapePayload& payload)
		{
			payload.packetPrepareFailure.store(
				NativeFontPacketPrepareFailure::None, std::memory_order_relaxed);
		}

		void InvalidateNativePreflight(NativeFontShapePayload& payload)
		{
			payload.preparedGeneration = 0;
			payload.preflightAtlasTextureEpoch = 0;
			payload.vanillaLikeBitmapPackets = false;
			// Full preflight often refreshes only atlas/resource stamps. Keep
			// the Tile/program dispatch until retained rebuild can compare its
			// geometry, program, and generation identities.
			InvalidateNativeFontTileRetainedText(payload, true);
			std::fill(payload.preflightAtlasTextures.begin(),
				payload.preflightAtlasTextures.end(), nullptr);
			std::fill(payload.packetShaders.begin(),
				payload.packetShaders.end(), nullptr);
			std::fill(payload.packetPrograms.begin(),
				payload.packetPrograms.end(), nullptr);
		}

		bool IsNativePreflightCacheCurrent(const NativeFontShapePayload& payload,
			UInt32 generation,
			UInt32 atlasTextureEpoch, bool scaledFillSampling,
			bool alphaBlending, const bool* forcedCompositeTopology)
		{
			if (!payload.payloadTemplate)
				return false;
			const NativeFontPayloadTemplate& artifact = *payload.payloadTemplate;
			const bool compositeDesired = forcedCompositeTopology
				? *forcedCompositeTopology
				: g_bEnableFreeTypeFontCompositePass
					&& !artifact.compositePackets.empty()
					&& !(payload.compositeUnavailable
						&& payload.compositeAttemptGeneration == generation);
			const std::vector<NativeFontPacketTemplate>& packets =
				GetNativeFontPackets(artifact, payload.useCompositePackets);
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

		NativeFontFallbackReason PreflightNativeFacadeImpl(NiTriShape* facade,
			const NativeFontShapeMetadata& metadata, NativeFontShapePayload& payload,
			const NativePreflightFrameContext* frameContext = nullptr,
			const bool* forcedCompositeTopology = nullptr)
		{
			FreeTypePerfScope perf(
				FreeTypePerfPhase::Preflight);
			if (!facade || !payload.buildComplete || !payload.payloadTemplate
				|| payload.payloadTemplate->packets.empty())
			{
				return NativeFontFallbackReason::PacketBuild;
			}
			const NativeFontPayloadTemplate& artifact = *payload.payloadTemplate;
			NativePreflightFrameContext structuralContext;
			const NativePreflightFrameContext* currentContext = frameContext;
			if (!currentContext)
			{
				NativeFontRuntimeReadinessView readiness;
				const bool ready =
					GetNativeFontRuntimeReadinessCurrent(readiness);
				structuralContext.accumulatorCurrent = ready;
				structuralContext.immediateRouteCurrent = ready;
				structuralContext.rendererAvailable = ready;
				structuralContext.generation = readiness.generation;
				structuralContext.atlasTextureEpoch =
					readiness.atlasTextureEpoch;
				currentContext = &structuralContext;
			}
			if (!currentContext->accumulatorCurrent)
				return NativeFontFallbackReason::AccumulatorConflict;
			if (!currentContext->immediateRouteCurrent)
				return NativeFontFallbackReason::TileRouteConflict;
			if (!currentContext->rendererAvailable)
				return NativeFontFallbackReason::ShaderGeneration;

			const UInt32 generation = currentContext->generation;
			if (!generation)
				return NativeFontFallbackReason::ShaderGeneration;
			const bool scaledFillSampling = NeedsScaledFillSampling(facade);
			const NiAlphaProperty* alpha = facade->GetAlphaProperty();
			const bool alphaBlending = alpha && alpha->GetAlphaBlending();
			const UInt32 atlasTextureEpoch = currentContext->atlasTextureEpoch;
			if (forcedCompositeTopology && *forcedCompositeTopology
				&& artifact.compositePackets.empty())
			{
				return NativeFontFallbackReason::PacketBuild;
			}
			if (forcedCompositeTopology && *forcedCompositeTopology
				&& payload.compositeUnavailable
				&& payload.compositeAttemptGeneration == generation)
			{
				return NativeFontFallbackReason::ShaderGeneration;
			}
			if (IsNativePreflightCacheCurrent(payload, generation,
					atlasTextureEpoch, scaledFillSampling, alphaBlending,
					forcedCompositeTopology))
			{
				RecordFreeTypePerf(FreeTypePerfCounter::PreflightFastHit);
				ClearNativePacketFailure(payload);
				return NativeFontFallbackReason::None;
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
			const std::vector<NativeFontPacketTemplate>* packets =
				&GetNativeFontPackets(artifact, payload.useCompositePackets);
			payload.vanillaLikeBitmapPackets =
				UsesOnlyVanillaLikeBitmapPackets(*packets);
			payload.packetShaders.assign(packets->size(), nullptr);
			payload.packetPrograms.assign(packets->size(), nullptr);
			if (payload.preflightAtlasTextures.size() != artifact.atlasTextures.size())
				return NativeFontFallbackReason::PacketBuild;
			for (const NativeFontPacketTemplate& packetTemplate : *packets)
			{
				const UInt64 vertexEnd = static_cast<UInt64>(
					packetTemplate.firstVertex) + packetTemplate.vertexCount;
				if (!packetTemplate.vertexCount
					|| (packetTemplate.vertexCount & 3u)
					|| vertexEnd > artifact.gpuVertices.size()
					|| packetTemplate.atlasPage >= artifact.atlasTextures.size())
				{
					return NativeFontFallbackReason::PacketBuild;
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
					return NativeFontFallbackReason::PageTexture;
				payload.preflightAtlasTextures[page] = d3dTexture;
			}

			bool shaderSetReady = true;
			for (size_t index = 0; index < packets->size(); ++index)
			{
				payload.packetShaders[index] = ResolveNativeFontPacketShader(
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
				payload.vanillaLikeBitmapPackets =
					UsesOnlyVanillaLikeBitmapPackets(*packets);
				payload.packetShaders.assign(packets->size(), nullptr);
				payload.packetPrograms.assign(packets->size(), nullptr);
				shaderSetReady = true;
				for (size_t index = 0; index < packets->size(); ++index)
				{
					payload.packetShaders[index] = ResolveNativeFontPacketShader(
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
				return NativeFontFallbackReason::ShaderGeneration;
			if (g_bEnableFreeTypeFontCommandBuffer)
			{
				for (size_t index = 0; index < packets->size(); ++index)
				{
					ResolveNativeFontRetainedPacketProgram(
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
			if (GetNativeFontAtlasTextureEpoch() != atlasTextureEpoch)
			{
				InvalidateNativePreflight(payload);
				return NativeFontFallbackReason::AtlasGeneration;
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
				BuildNativeFontTileRetainedText(facade, payload,
					generation, atlasTextureEpoch);
			}
			ClearNativePacketFailure(payload);
			return NativeFontFallbackReason::None;
		}

		__forceinline bool IsThinRegistrationHookChainCurrentUnchecked()
		{
			NativeFontShapeState& state = State();
			const TileRegisterObjectFn current =
				*reinterpret_cast<TileRegisterObjectFn volatile*>(
					kTileRegisterObjectFunctionEntry);
			return current == &NativeFontRegisterObject
				&& s_predecessorTileRegisterObject.load(
					std::memory_order_relaxed) != nullptr
				&& state.predecessorRenderAlphaGeometry
				&& hook_identity::MatchesRel32InstructionImageUnchecked(
					kRenderAlphaGeometryCallSite,
					s_renderAlphaGeometryHookImage)
				&& IsNativeFontRenderPassImmediatelyHookCurrentUnchecked();
		}

		bool __cdecl NativeFontRegisterObject(BSShaderAccumulator* accumulator,
			NiGeometry* geometry, const NiPropertyState* properties,
			BSShaderProperty* shaderProperty, BSShader* shader)
		{
			const TileRegisterObjectFn original =
				s_predecessorTileRegisterObject.load(std::memory_order_relaxed);
			if (!original || !accumulator || !geometry
				|| accumulator->eRenderMode
					!= BSShaderManager::BSSM_RENDER_TILES
				|| !IsDirectNativeFacade(geometry))
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
								IsNativeFontRegistrationHookChainCurrent()
								&& IsNativeFontRenderPassImmediatelyHookCurrent()
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

		void __fastcall NativeFontRenderAlphaGeometry(BSShaderAccumulator* accumulator, void*)
		{
			NativeFontShapeState& state = State();
			if (!state.predecessorRenderAlphaGeometry)
				return;
			FreeTypePerfScope frameRoutePerf(
				FreeTypePerfPhase::FrameRouteTotal);

			SortedPayloadScratch& scratch = s_sortedPayloadScratch;
			if (scratch.active || scratch.nestedBypassDepth)
			{
				// A nested vanilla Tile pass must not see facade entries from the outer
				// accumulator. It retains the fully validated map/preflight fallback.
				// Its draws may also change sampler and private c176-c183 state
				// outside the outer traversal, so neither side may inherit the
				// other's sorted cache.
				const bool restoreActive = scratch.active;
				scratch.active = false;
				++scratch.nestedBypassDepth;
				if (++scratch.nestedTraversalSerial == 0)
					++scratch.nestedTraversalSerial;
				RecordNativeFontCommandFallback(
					NativeFontCommandFallback::Nested);
				InvalidateNativeFontCommandExecutionSegment(
					NativeFontCommandFallback::Nested);
				InvalidateNativeFontSortedShaderState();
				state.predecessorRenderAlphaGeometry(accumulator);
				InvalidateNativeFontSortedShaderState();
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
				CaptureSortedTrackedShapeTopology(scratch, accumulator,
					&prepTailSample.topologyTicks);
				if (scratch.metadataShapes.empty())
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::AccumulatorEmptyFastPath);
					// The predecessor Sort contains no tracked native shape. This is
					// either an entirely vanilla traversal or a fail-open topology scan;
					// in both cases the ordinary dispatch path remains authoritative.
					// Do not build a readiness stamp or command frame.
					const SInt64 framePrepTicks = EndFreeTypePerfSample(
						FreeTypePerfPhase::FrameRoutePrep, framePrepStart);
					prepTailSample.totalTicks = framePrepTicks;
					RecordFreeTypeAccumulatorPrepTailSample(prepTailSample);
					{
						FreeTypePerfScope vanillaRenderPerf(
							FreeTypePerfPhase::FrameRouteVanillaRender);
						state.predecessorRenderAlphaGeometry(accumulator);
					}
					ClearSortedFrame(scratch);
					return;
				}

				// Visibility is now the first post-Sort preflight. The tracked shape owns
				// the final model bound and Tile/scissor state, so a proven cull does
				// not need a metadata owner, packet artifact, or renderer-ring slot.
				{
					FreeTypePerfScope visibilityPrepPerf(
						FreeTypePerfPhase::FramePrepVisibility, true,
						&prepTailSample.visibilityTicks);
					BeginNativeFontVisibilityFrame();
					scratch.visibilityPreflights.assign(
						scratch.metadataShapes.size(), {});
					scratch.metadataAcquireShapes.clear();
					scratch.metadataAcquireShapes.reserve(
						scratch.metadataShapes.size());
					for (size_t index = 0;
						index < scratch.metadataShapes.size(); ++index)
					{
						NiTriShape* facade = scratch.metadataShapes[index];
						NativeFontVisibilityPreflight visibility;
						visibility.cull =
							EvaluateNativeFontSubmissionVisibility(facade);
						if (visibility.cull == NativeFontVisibilityCull::None)
						{
							visibility =
								EvaluateNativeFontPreflightClipVisibility(facade);
						}
						scratch.visibilityPreflights[index] = visibility;
						if (visibility.cull == NativeFontVisibilityCull::None)
							scratch.metadataAcquireShapes.push_back(facade);
						else
						{
							RecordFreeTypePerf(FreeTypePerfCounter::
								VisibilityPreflightSkipped);
							RecordFreeTypePerf(FreeTypePerfCounter::
								AccumulatorMetadataCullSkipped);
						}
					}
					CompleteNativeFontVisibilityPreflight();
					scratch.metadataOwners.assign(
						scratch.metadataShapes.size(), {});
				}
				if (!scratch.metadataAcquireShapes.empty())
				{
					FreeTypePerfScope metadataPerf(
						FreeTypePerfPhase::FramePrepMetadata, true,
						&prepTailSample.metadataTicks);
					AcquireNativeFontShapeMetadataBatch(
						scratch.metadataAcquireShapes,
						scratch.metadataAcquireOwners);
					for (const NativeFontShapeMetadataPtr& owner
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
					if (hasMetadataSurvivors)
					{
						NativeFontRuntimeReadinessView readiness;
						bool ready = GetNativeFontRuntimeReadinessCurrent(readiness);
						if (!ready && IsNativeFontRendererAvailable())
							ready = GetNativeFontRuntimeReadinessCurrent(readiness);
						preflightContext.accumulatorCurrent = ready;
						preflightContext.immediateRouteCurrent = ready;
						preflightContext.rendererAvailable = ready;
						preflightContext.generation = readiness.generation;
						preflightContext.atlasTextureEpoch =
							readiness.atlasTextureEpoch;
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

				bool hasVanillaLayoutSurvivors = false;
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
					entry.visibility = candidateIndex
						< scratch.visibilityPreflights.size()
						? scratch.visibilityPreflights[candidateIndex]
						: NativeFontVisibilityPreflight{};
					if (entry.visibility.cull == NativeFontVisibilityCull::None)
						entry.metadata = FindBatchedMetadata(scratch, facade);
					entry.generation = generation;
					SingletonFacadeState* singletonFacade =
						nullptr;
					const bool isVanillaLayout = entry.metadata
						&& entry.metadata->backend
							== FreeTypeShapeBackend::VanillaLayout;
					bool topologyReady = false;
					if (!entry.metadata
						&& entry.visibility.cull == NativeFontVisibilityCull::None)
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

					if (entry.visibility.cull != NativeFontVisibilityCull::None)
					{
						// Dispatch revalidates the volatile cull inputs.  A revoked proof
						// falls back to a one-shape metadata lookup instead of making every
						// proven-offscreen facade pay the batch acquisition cost.
					}
					else if (isVanillaLayout)
					{
						// Vanilla-layout owns an engine-prepared 40/48-byte buffer. Keep its
						// metadata and exact post-Sort visibility proof alive, but never
						// publish that payload to the 52-byte facade ring/command path.
						if (entry.metadata->nativePayload.buildComplete)
						{
							entry.preflightResult =
								NativeFontFallbackReason::None;
							entry.validationToken = frameValidationToken;
							hasVanillaLayoutSurvivors = true;
						}
						else
						{
							entry.preflightResult =
								NativeFontFallbackReason::PacketBuild;
						}
					}
					else if (entry.metadata
						&& entry.metadata->nativePayload.buildComplete)
					{
						entry.payload = &entry.metadata->nativePayload;
						entry.preflightResult = PreflightNativeFacadeImpl(
							facade, *entry.metadata, *entry.payload,
							&preflightContext, nullptr);
						if (entry.preflightResult
								== NativeFontFallbackReason::None)
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
							NativeFontFallbackReason::PacketBuild;
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
					if (stored.preflightResult == NativeFontFallbackReason::None
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
				const bool hasTrackedNativeSubmissions =
					hasPreparedPayloads || hasVanillaLayoutSurvivors;
				// Vanilla-layout participates only in the outer shader-state scope.
				// The direct-facade payload gate remains authoritative for ring,
				// command-buffer, constant-ownership and native replay work.
				const bool hasSortedShaderParticipants =
					hasTrackedNativeSubmissions;
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
					PrepareSortedNativeFontPayloads(
						scratch.payloadTemplates, generation);
				}
				if (hasPreparedPayloads)
				{
					FreeTypePerfScope singletonPrepPerf(
						FreeTypePerfPhase::FramePrepSingletons, true,
						&prepTailSample.singletonTicks);
					for (const NativeFontShapeMetadata* metadata
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
									NativeFontFallbackReason::RuntimeFault);
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
						BeginNativeFontFrameCommandBuffer(accumulator,
							frameValidationToken, generation,
							preflightContext.atlasTextureEpoch);
						ReserveNativeFontFrameCommandBuffer(
							scratch.frameEntries.size(),
							scratch.singletonFacades.size());
					}
					{
						FreeTypePerfScope commandBuildDirectFacade(
							FreeTypePerfPhase::CommandBuildDirectFacade);
						for (const NativeFontShapeMetadata* metadata
							: scratch.singletonFacades)
						{
							if (metadata)
							{
								AddNativeFontFrameDirectFacadeCommand(
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
									!= NativeFontFallbackReason::None
								|| entry.visibility.cull
									!= NativeFontVisibilityCull::None
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
									AddNativeFontFrameSinglePacketCommand(
										entry.facade, entry.metadata,
										entry.payload);
								if (entry.singlePacketCommandIndex
									== kInvalidNativeFontCommandIndex)
								{
									entry.commandSpanIndex =
										AddNativeFontFrameCommandSpan(
											entry.facade,
											entry.metadata,
											entry.payload);
								}
							}
							else
							{
								entry.singlePacketCommandIndex =
									AddNativeFontFrameSinglePacketCommand(
										entry.facade, entry.metadata,
										entry.payload);
								if (entry.singlePacketCommandIndex
									== kInvalidNativeFontCommandIndex)
								{
									entry.commandSpanIndex =
										AddNativeFontFrameCommandSpan(
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
						ActivateNativeFontFrameCommandBuffer();
					}
				}
				else
				{
					EndNativeFontFrameCommandBuffer();
				}
				{
					FreeTypePerfScope publishPrepPerf(
						FreeTypePerfPhase::FramePrepPublish, true,
						&prepTailSample.publishTicks);
					RefreshSortedScratchMemory(scratch);
					if (hasSortedShaderParticipants)
						BeginNativeFontSortedShaderBatch();
					if (hasPreparedPayloads)
					{
						BeginNativeFontSortedTileConstantOwnership();
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
					FreeTypePerfScope vanillaRenderPerf(
						FreeTypePerfPhase::FrameRouteVanillaRender);
					// GPU timing has its own tracked-submission gate. Vanilla-layout
					// remains outside ring/command work, but a preflight-surviving
					// shape now admits a sampled asynchronous Tile alpha envelope.
					NiDX9Renderer* renderer = hasTrackedNativeSubmissions
						? NiDX9Renderer::GetSingleton() : nullptr;
					FreeTypeGpuEnvelopeViewport gpuViewport;
					if (renderer)
					{
						gpuViewport.x = renderer->m_kD3DPort.X;
						gpuViewport.y = renderer->m_kD3DPort.Y;
						gpuViewport.width = renderer->m_kD3DPort.Width;
						gpuViewport.height = renderer->m_kD3DPort.Height;
					}
					FreeTypeGpuAlphaEnvelopeScope gpuTiming(
						renderer ? renderer->GetD3DDevice() : nullptr,
						hasPreparedPayloads, hasVanillaLayoutSurvivors,
						gpuViewport);
					state.predecessorRenderAlphaGeometry(accumulator);
				}
				if (hasPreparedPayloads)
				{
					EndNativeFontSortedTileConstantOwnership();
				}
				if (hasSortedShaderParticipants)
				{
					EndNativeFontSortedShaderBatch();
				}
				EndNativeFontFrameCommandBuffer();
				EndNativeFontSortedRingFrame();
				ClearSortedFrame(scratch);
				return;
			}

			const bool clearSortedOwners =
				scratch.frameAccumulator == accumulator;
			{
				FreeTypePerfScope vanillaRenderPerf(
					FreeTypePerfPhase::FrameRouteVanillaRender);
				state.predecessorRenderAlphaGeometry(accumulator);
			}
			if (clearSortedOwners)
				ClearSortedFrame(scratch);
		}

		bool HookRenderAlphaGeometry()
		{
			NativeFontShapeState& state = State();
			const RenderAlphaGeometryFn adapterTarget =
				reinterpret_cast<RenderAlphaGeometryFn>(
				&NativeFontRenderAlphaGeometry);
			const RenderAlphaGeometryFn currentTarget =
				ReadRenderAlphaGeometryCallTarget();
			if (currentTarget == adapterTarget)
			{
				state.renderAlphaGeometryHookInstalled =
					state.predecessorRenderAlphaGeometry
					&& state.predecessorRenderAlphaGeometry != adapterTarget
					&& hook_identity::IsExecutableTarget(
						reinterpret_cast<SIZE_T>(
							state.predecessorRenderAlphaGeometry));
				return state.renderAlphaGeometryHookInstalled;
			}
			if (!currentTarget)
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
			if (!hook_identity::IsExecutableTarget(
				reinterpret_cast<SIZE_T>(currentTarget)))
			{
				if (!state.loggedRenderAlphaGeometryHookConflict)
				{
					state.loggedRenderAlphaGeometryHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: RenderAlphaGeometry call target is not executable target=%p; frame upload batching left disabled",
						currentTarget);
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

			// A vanilla reset, or restoration of the predecessor we previously owned,
			// is safe to republish over. Any other target observed after installation
			// may be a successor that already chains through tNVSE; never move back on
			// top of it or the two hooks could recurse through each other.
			const RenderAlphaGeometryFn installedPredecessor =
				state.predecessorRenderAlphaGeometry;
			if (installedPredecessor
				&& currentTarget != reinterpret_cast<RenderAlphaGeometryFn>(
					kBSShaderAccumulatorRenderAlphaGeometry)
				&& currentTarget != installedPredecessor)
			{
				if (!state.loggedRenderAlphaGeometryHookConflict)
				{
					state.loggedRenderAlphaGeometryHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: RenderAlphaGeometry frame hook was replaced by a successor target=%p; tNVSE will not reassert ownership",
						currentTarget);
				}
				return false;
			}

			const RenderAlphaGeometryFn previousPredecessor =
				installedPredecessor;
			state.predecessorRenderAlphaGeometry = currentTarget;
			// BSShaderAccumulator frame -> RenderAlphaGeometry
			// (__thiscall target via __fastcall shim).
			WriteRelCall(kRenderAlphaGeometryCallSite,
				&NativeFontRenderAlphaGeometry);
			const RenderAlphaGeometryFn publishedTarget =
				ReadRenderAlphaGeometryCallTarget();
			state.renderAlphaGeometryHookInstalled =
				publishedTarget == adapterTarget;
			if (!state.renderAlphaGeometryHookInstalled)
			{
				// If the target is unchanged, publication failed before tNVSE became
				// reachable and the previous state remains authoritative. Otherwise a
				// successor may already retain tNVSE, so keep the predecessor that this
				// publication exposed and never overwrite the successor.
				if (publishedTarget == currentTarget || !publishedTarget)
				{
					state.predecessorRenderAlphaGeometry = previousPredecessor;
				}
				else if (!state.loggedRenderAlphaGeometryHookConflict)
				{
					state.loggedRenderAlphaGeometryHookConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: RenderAlphaGeometry target changed during publication successor=%p predecessor=%p; tNVSE will not reassert ownership",
						publishedTarget, currentTarget);
				}
				return false;
			}
			state.loggedRenderAlphaGeometryHookConflict = false;
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: installed RenderAlphaGeometry frame route predecessor=%p vanilla=%u",
					currentTarget,
					reinterpret_cast<SIZE_T>(currentTarget)
						== kBSShaderAccumulatorRenderAlphaGeometry ? 1u : 0u);
			}
			return true;
		}

	}

	bool FindNativeFontSortedFrameEntry(NiTriShape* facade,
		NativeFontSortedFrameEntryView& view)
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
		view.visibility = &entry.visibility;
		view.generation = entry.generation;
		view.validationToken = entry.validationToken;
		view.commandSpanIndex = entry.commandSpanIndex;
		view.singlePacketCommandIndex =
			entry.singlePacketCommandIndex;
		RecordFreeTypePerf(FreeTypePerfCounter::SortedFrameLookupHit);
		return true;
	}

	UInt64 GetNativeFontSortedFrameValidationToken()
	{
		const SortedPayloadScratch& scratch = s_sortedPayloadScratch;
		return scratch.active ? scratch.activeValidationToken : 0;
	}

	UInt64 GetNativeFontSortedNestedTraversalSerial()
	{
		return s_sortedPayloadScratch.nestedTraversalSerial;
	}

	UInt32 GetNativeFontAtlasTextureEpoch()
	{
		return s_atlasTextureEpoch.load(std::memory_order_acquire);
	}

	void NotifyNativeFontAtlasTextureMutation()
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
				NotifyNativeFontCommandExternalMutation(
					NativeFontCommandFallback::Atlas);
				return;
			}
		}
	}

	NativeFontFallbackReason PrepareNativeFontFacade(NiTriShape* facade,
		const NativeFontShapeMetadata& metadata, NativeFontShapePayload& payload)
	{
		return PreflightNativeFacadeImpl(facade, metadata, payload);
	}

	bool HookNativeFontAccumulator()
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

		const TileRegisterObjectFn adapterTarget = &NativeFontRegisterObject;
		const TileRegisterObjectFn currentTarget =
			ReadTileRegisterObjectTarget();
		if (currentTarget == adapterTarget)
		{
			const TileRegisterObjectFn predecessorTarget =
				s_predecessorTileRegisterObject.load(std::memory_order_acquire);
			if (!predecessorTarget || predecessorTarget == adapterTarget
				|| !hook_identity::IsExecutableTarget(
					reinterpret_cast<SIZE_T>(predecessorTarget)))
			{
				if (!s_loggedTileRegisterObjectConflict)
				{
					s_loggedTileRegisterObjectConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: Tile RegisterObject dispatch points at tNVSE without a predecessor; native route disabled");
				}
				return false;
			}
			return HookRenderAlphaGeometry();
		}

		if (!currentTarget)
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
			reinterpret_cast<SIZE_T>(currentTarget)))
		{
			if (!s_loggedTileRegisterObjectConflict)
			{
				s_loggedTileRegisterObjectConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile RegisterObject dispatch hook skipped; target is not executable target=%p entry=%08X",
					currentTarget, kTileRegisterObjectFunctionEntry);
				InvalidateAllSingletonFacadeBindings();
			}
			return false;
		}

		// A vanilla reset, or restoration of the predecessor we previously owned,
		// is safe to republish over.  Any other target observed after installation
		// may be a successor that already chains to tNVSE; never reassert over it or
		// the two hooks could recurse through each other.
		const TileRegisterObjectFn installedPredecessor =
			s_predecessorTileRegisterObject.load(std::memory_order_acquire);
		if (installedPredecessor
			&& currentTarget != reinterpret_cast<TileRegisterObjectFn>(
				kBSShaderAccumulatorRegisterObjectInterface)
			&& currentTarget != installedPredecessor)
		{
			if (!s_loggedTileRegisterObjectConflict)
			{
				s_loggedTileRegisterObjectConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile RegisterObject dispatch was replaced by a successor target=%p; tNVSE will not reassert ownership",
					currentTarget);
				InvalidateAllSingletonFacadeBindings();
			}
			return false;
		}

		if (reinterpret_cast<SIZE_T>(currentTarget)
			!= kBSShaderAccumulatorRegisterObjectInterface
			&& g_bEnableFreeTypeFontRenderingLog)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_native: chaining pre-existing Tile RegisterObject dispatch target=%p vanilla=%08X",
				currentTarget, kBSShaderAccumulatorRegisterObjectInterface);
		}

		const TileRegisterObjectFn previousPredecessor = installedPredecessor;
		s_predecessorTileRegisterObject.store(
			currentTarget, std::memory_order_release);
		const SafeWrite32IfEqualResult publication =
			PublishTileRegisterObjectHook(currentTarget);
		if (!publication.WasPublished())
		{
			// The slot changed after validation.  Preserve an older predecessor if
			// one may still be reached through a successor chain; otherwise allow a
			// clean retry against the newly observed initial target.
			s_predecessorTileRegisterObject.store(previousPredecessor,
				std::memory_order_release);
			if (!s_loggedTileRegisterObjectConflict)
			{
				s_loggedTileRegisterObjectConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile RegisterObject dispatch CAS did not publish expected=%p actual=%p compared=%u observed=%p protectionError=%lu",
					currentTarget, ReadTileRegisterObjectTarget(),
					publication.comparisonPerformed ? 1u : 0u,
					reinterpret_cast<void*>(publication.observed),
					publication.protectionError);
			}
			return false;
		}
		if (!publication.PostconditionsComplete())
		{
			gLog.FormattedMessage(
				"tnvse_freetype_native: Tile RegisterObject dispatch published with incomplete write postconditions protectionRestored=%u protectionError=%lu cacheFlushed=%u cacheError=%lu",
				publication.protectionRestored ? 1u : 0u,
				publication.protectionError,
				publication.instructionCacheFlushed ? 1u : 0u,
				publication.cacheFlushError);
		}

		const bool accumulatorReady = IsNativeFontAccumulatorHookCurrent();
		if (!accumulatorReady)
		{
			// Do not overwrite a successor that raced the readback.  It may already
			// retain NativeFontRegisterObject as its predecessor, so the saved target
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
		const bool renderAlphaReady = HookRenderAlphaGeometry();
		if (g_bEnableFreeTypeFontRenderingLog)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_native: installed Tile RegisterObject dispatch route entry=%08X predecessor=%p vanilla=%u renderAlphaReady=%u",
				kTileRegisterObjectFunctionEntry, currentTarget,
				reinterpret_cast<SIZE_T>(currentTarget)
					== kBSShaderAccumulatorRegisterObjectInterface ? 1u : 0u,
				renderAlphaReady ? 1u : 0u);
		}
		return renderAlphaReady;
	}

	bool IsNativeFontAccumulatorHookCurrent()
	{
		const TileRegisterObjectFn adapterTarget = &NativeFontRegisterObject;
		const TileRegisterObjectFn predecessorTarget =
			s_predecessorTileRegisterObject.load(std::memory_order_acquire);
		return ReadTileRegisterObjectTarget() == adapterTarget
			&& predecessorTarget && predecessorTarget != adapterTarget
			&& hook_identity::IsExecutableTarget(
				reinterpret_cast<SIZE_T>(predecessorTarget));
	}

	bool IsNativeFontRenderAlphaGeometryHookCurrent()
	{
		const RenderAlphaGeometryFn adapterTarget =
			reinterpret_cast<RenderAlphaGeometryFn>(
				&NativeFontRenderAlphaGeometry);
		const RenderAlphaGeometryFn predecessorTarget =
			State().predecessorRenderAlphaGeometry;
		return predecessorTarget && predecessorTarget != adapterTarget
			&& hook_identity::IsExecutableTarget(
				reinterpret_cast<SIZE_T>(predecessorTarget))
			&& ReadRenderAlphaGeometryCallTarget() == adapterTarget;
	}

	bool IsNativeFontRegistrationHookChainCurrent()
	{
		return IsNativeFontAccumulatorHookCurrent()
			&& IsNativeFontRenderAlphaGeometryHookCurrent();
	}

	bool IsNativeFontRegistrationHookChainCurrentFast()
	{
		NativeFontShapeState& state = State();
		const TileRegisterObjectFn tileAdapterTarget =
			&NativeFontRegisterObject;
		const TileRegisterObjectFn tilePredecessor =
			s_predecessorTileRegisterObject.load(std::memory_order_acquire);
		const bool tileCurrent =
			ReadTileRegisterObjectTarget() == tileAdapterTarget
			&& tilePredecessor && tilePredecessor != tileAdapterTarget;
		const RenderAlphaGeometryFn renderAlphaAdapterTarget =
			reinterpret_cast<RenderAlphaGeometryFn>(
				&NativeFontRenderAlphaGeometry);
		const bool renderAlphaCurrent = state.predecessorRenderAlphaGeometry
			&& state.predecessorRenderAlphaGeometry
				!= renderAlphaAdapterTarget
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

#pragma once

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


#include <mutex>

namespace fonthook::vectorfont::implementation::font_native_accumulator
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
		ASSERT_OFFSET(NiBackToFrontAccumulator, m_iNumItems, 0x1C);
		ASSERT_OFFSET(NiBackToFrontAccumulator, m_ppkItems, 0x24);
		ASSERT_OFFSET(NiBackToFrontAccumulator, m_iCurrItem, 0x2C);

		using TileRegisterObjectFn = bool(__cdecl*)(BSShaderAccumulator*,
			NiGeometry*, const NiPropertyState*, BSShaderProperty*, BSShader*);
		static_assert(kTileRegisterObjectFunctionEntry == 0x11F9FA8);
		static_assert(sizeof(TileRegisterObjectFn) == sizeof(UInt32));


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
			// vector::emplace_back() value-initializes an entry. Keep this
			// constructor user-provided so that operation does not first zero the
			// complete object, including the immediately replaced proof payload.
			SortedFrameEntry() noexcept
			{
			}

			NiTriShape* facade = nullptr;
			// Dense metadataOwners holds each surviving batch-acquired shared owner
			// until the vanilla traversal completes; entries use stable non-owning
			// views and culled entries do not allocate an empty owner slot.
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
			// These three arrays share survivor order. Owners keep metadata alive;
			// captured indices attach each result directly to its full frame entry.
			std::vector<NativeFontShapeMetadataPtr> metadataOwners;
			std::vector<NiTriShape*> metadataAcquireShapes;
			std::vector<UInt32> metadataAcquireEntryIndices;
			std::vector<UInt32> metadataLookup;
			std::vector<UInt32> sortedOccurrenceCounts;
			// Retail B64F90 publishes m_iCurrItem before each B994F0 call.  This
			// dense final-Sort index maps that item directly to frameEntries; an
			// invalid slot retains the facade-hash fallback.
			std::vector<UInt32> sortedFrameEntryIndices;
			std::vector<UInt32> tieSortedLookup;
			std::vector<UInt32> tieSortedOccurrenceCursor;
			std::vector<UInt32> tieSortedOccurrenceNext;
			std::vector<UInt32> tieRunIds;
			std::vector<EqualDepthTieRepairRun> tieRuns;
			std::vector<NiGeometry*> tieOutput;
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
			bool stockRenderAlphaTraversal = false;
			bool stockImmediateDispatch = false;
		};


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

		// Diagnostics are flushed at every frame-clear boundary. Keep the timing
		// phase separate so short registration cycles still contribute one sample
		// per kRegisterRouteSampleRate calls across the lifetime of this thread.


	struct AccumulatorRuntimeState
	{
		std::atomic<TileRegisterObjectFn> predecessorTileRegisterObject{ nullptr };
		bool loggedTileRegisterObjectConflict = false;
		bool loggedTileRegisterObjectSlotUnavailable = false;
		std::atomic<UInt32> missingMetadataLogCount{ 0 };
		std::atomic<UInt32> atlasTextureEpoch{ 1 };
		std::atomic<UInt64> thinRejectedHookFingerprint{ 0 };
		std::mutex thinHookAuditMutex;
	};

	struct AccumulatorThreadState
	{
		bool loggedEqualDepthTieRepair = false;
		bool loggedEqualDepthTieRepairFailure = false;
		SortedPayloadScratch sortedPayloadScratch;
		ThinRegistrationDiagnostics thinRegistrationDiagnostics;
		UInt32 registerRouteSampleCountdown = kRegisterRouteSampleRate;
	};

	AccumulatorRuntimeState& AccumulatorState();
	AccumulatorThreadState& AccumulatorThread();

	enum class NativeSortedShapeKind : UInt8
	{
		None,
		DirectFacade,
		VanillaLayout
	};

	UInt64 BuildThinHookFingerprintUnchecked();
	bool ShouldSampleRegisterRoute();
	void FlushThinRegistrationDiagnostics();
	size_t HashPointer(const void* pointer);
	size_t GetLookupCapacity(size_t expectedEntries);
	void PrepareLookup(std::vector<UInt32>& lookup, size_t expectedEntries);
	size_t LookupSortedFacade(const SortedPayloadScratch& scratch,
		const NiTriShape* facade);
	void InsertSortedFacade(SortedPayloadScratch& scratch,
		NiTriShape* facade, size_t entryIndex);
	bool InsertUniquePayload(SortedPayloadScratch& scratch,
		const NativeFontPayloadTemplatePtr& payloadTemplate);
	void RefreshSortedScratchMemory(SortedPayloadScratch& scratch);
	void ResetSortedPrepScratch(SortedPayloadScratch& scratch);
	void ClearSortedFrame(SortedPayloadScratch& scratch);
	RenderAlphaGeometryFn ReadRenderAlphaGeometryCallTarget();
	TileRegisterObjectFn ReadTileRegisterObjectTarget();
	SafeWrite32IfEqualResult PublishTileRegisterObjectHook(
		TileRegisterObjectFn expected);

	EqualDepthTieRepairResult RepairMixedEqualDepthRunsLinear(
		SortedPayloadScratch& scratch, BSShaderAccumulator& accumulator);
	bool CaptureSortedTrackedShapeTopology(SortedPayloadScratch& scratch,
		BSShaderAccumulator* accumulator, bool stockRenderAlphaTraversal,
		bool stockImmediateDispatch, SInt64* topologyTicks);
	void ResolveSingletonFacadeSortedTopology(
		SortedPayloadScratch& scratch, BSShaderAccumulator* accumulator,
		UInt64 validationToken);
	NativeSortedShapeKind ClassifyNativeSortedShape(
		const NiGeometry* geometry);
	bool IsDirectNativeFacade(const NiGeometry* geometry);
	void ClearNativePacketFailure(NativeFontShapePayload& payload);
	void InvalidateNativePreflight(NativeFontShapePayload& payload);
	bool IsNativePreflightCacheCurrent(const NativeFontShapePayload& payload,
		UInt32 generation, UInt32 atlasTextureEpoch, bool scaledFillSampling,
		bool alphaBlending, const bool* forcedCompositeTopology);
	NativeFontFallbackReason PreflightNativeFacadeImpl(NiTriShape* facade,
		const NativeFontShapeMetadata& metadata, NativeFontShapePayload& payload,
		const NativePreflightFrameContext* frameContext = nullptr,
		const bool* forcedCompositeTopology = nullptr);
	bool HookRenderAlphaGeometry();
}

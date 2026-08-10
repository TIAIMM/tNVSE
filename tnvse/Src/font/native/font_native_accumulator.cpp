#include "font_native_accumulator_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_native_accumulator {}
	using namespace implementation::font_native_accumulator;

	namespace implementation::font_native_accumulator
	{
		AccumulatorRuntimeState& AccumulatorState()
		{
			static AccumulatorRuntimeState state;
			return state;
		}

		AccumulatorThreadState& AccumulatorThread()
		{
			thread_local AccumulatorThreadState state;
			return state;
		}

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
				AccumulatorState().predecessorTileRegisterObject.load(std::memory_order_relaxed);
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
			if (--AccumulatorThread().registerRouteSampleCountdown != 0)
				return false;
			AccumulatorThread().registerRouteSampleCountdown = kRegisterRouteSampleRate;
			return true;
		}

		void FlushThinRegistrationDiagnostics()
		{
			if (!g_bEnableFreeTypeFontRenderingLog)
			{
				AccumulatorThread().thinRegistrationDiagnostics = {};
				return;
			}
			ThinRegistrationDiagnostics values;
			std::swap(values, AccumulatorThread().thinRegistrationDiagnostics);
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
				+ scratch.metadataAcquireEntryIndices.capacity()
					* sizeof(UInt32)
				+ scratch.metadataLookup.capacity() * sizeof(UInt32)
				+ scratch.sortedOccurrenceCounts.capacity() * sizeof(UInt32)
				+ scratch.sortedFrameEntryIndices.capacity() * sizeof(UInt32)
				+ scratch.tieSortedLookup.capacity() * sizeof(UInt32)
				+ scratch.tieSortedOccurrenceCursor.capacity() * sizeof(UInt32)
				+ scratch.tieSortedOccurrenceNext.capacity() * sizeof(UInt32)
				+ scratch.tieRunIds.capacity() * sizeof(UInt32)
				+ scratch.tieRuns.capacity() * sizeof(EqualDepthTieRepairRun)
				+ scratch.tieOutput.capacity() * sizeof(NiGeometry*)
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
			scratch.metadataAcquireEntryIndices.clear();
			scratch.sortedOccurrenceCounts.clear();
			scratch.sortedFrameEntryIndices.clear();
			scratch.tieRuns.clear();
			scratch.frameEntries.clear();
			scratch.payloadTemplates.clear();
			scratch.singletonFacades.clear();
			scratch.stockRenderAlphaTraversal = false;
			scratch.stockImmediateDispatch = false;
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
			scratch.metadataAcquireEntryIndices.clear();
			scratch.sortedOccurrenceCounts.clear();
			scratch.sortedFrameEntryIndices.clear();
			scratch.tieSortedLookup.clear();
			scratch.tieSortedOccurrenceCursor.clear();
			scratch.tieSortedOccurrenceNext.clear();
			scratch.tieRunIds.clear();
			scratch.tieRuns.clear();
			scratch.tieOutput.clear();
			scratch.frameEntries.clear();
			scratch.payloadTemplates.clear();
			scratch.singletonFacades.clear();
			scratch.stockRenderAlphaTraversal = false;
			scratch.stockImmediateDispatch = false;

			if (scratch.metadataShapes.capacity() > 8192)
				std::vector<NiTriShape*>().swap(scratch.metadataShapes);
			if (scratch.metadataOwners.capacity() > 8192)
				std::vector<NativeFontShapeMetadataPtr>().swap(scratch.metadataOwners);
			if (scratch.metadataAcquireShapes.capacity() > 8192)
			{
				std::vector<NiTriShape*>().swap(
					scratch.metadataAcquireShapes);
			}
			if (scratch.metadataAcquireEntryIndices.capacity() > 8192)
			{
				std::vector<UInt32>().swap(
					scratch.metadataAcquireEntryIndices);
			}
			if (scratch.metadataLookup.capacity() > 16384)
				std::vector<UInt32>().swap(scratch.metadataLookup);
			if (scratch.sortedOccurrenceCounts.capacity() > 8192)
				std::vector<UInt32>().swap(scratch.sortedOccurrenceCounts);
			if (scratch.sortedFrameEntryIndices.capacity() > 8192)
				std::vector<UInt32>().swap(scratch.sortedFrameEntryIndices);
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
				std::vector<UInt32>().swap(
					scratch.metadataAcquireEntryIndices);
				std::vector<UInt32>().swap(scratch.metadataLookup);
				std::vector<UInt32>().swap(scratch.sortedOccurrenceCounts);
				std::vector<UInt32>().swap(
					scratch.sortedFrameEntryIndices);
				std::vector<UInt32>().swap(scratch.tieSortedLookup);
				std::vector<UInt32>().swap(
					scratch.tieSortedOccurrenceCursor);
				std::vector<UInt32>().swap(scratch.tieSortedOccurrenceNext);
				std::vector<UInt32>().swap(scratch.tieRunIds);
				std::vector<EqualDepthTieRepairRun>().swap(scratch.tieRuns);
				std::vector<NiGeometry*>().swap(scratch.tieOutput);
				std::vector<SortedFrameEntry>().swap(scratch.frameEntries);
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

		__forceinline bool IsThinRegistrationHookChainCurrentUnchecked()
		{
			NativeFontShapeState& state = State();
			const TileRegisterObjectFn current =
				*reinterpret_cast<TileRegisterObjectFn volatile*>(
					kTileRegisterObjectFunctionEntry);
			return current == &NativeFontRegisterObject
				&& AccumulatorState().predecessorTileRegisterObject.load(
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
				AccumulatorState().predecessorTileRegisterObject.load(std::memory_order_relaxed);
			if (!original || !accumulator || !geometry
				|| accumulator->eRenderMode
					!= BSShaderManager::BSSM_RENDER_TILES
				|| !IsDirectNativeFacade(geometry))
			{
				return ForwardTileRegisterObject(original, accumulator, geometry,
					properties, shaderProperty, shader);
			}

			ThinRegistrationDiagnostics& diagnostics =
				AccumulatorThread().thinRegistrationDiagnostics;
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
				if (fingerprint != AccumulatorState().thinRejectedHookFingerprint.load(
					std::memory_order_acquire))
				{
					std::lock_guard<std::mutex> auditLock(
						AccumulatorState().thinHookAuditMutex);
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
							!= AccumulatorState().thinRejectedHookFingerprint.load(
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
								AccumulatorState().thinRejectedHookFingerprint.store(
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

	bool HookNativeFontAccumulator()
	{
		if (!IsTileRegisterObjectSlotWritable())
		{
			if (!AccumulatorState().loggedTileRegisterObjectSlotUnavailable)
			{
				AccumulatorState().loggedTileRegisterObjectSlotUnavailable = true;
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
				AccumulatorState().predecessorTileRegisterObject.load(std::memory_order_acquire);
			if (!predecessorTarget || predecessorTarget == adapterTarget
				|| !hook_identity::IsExecutableTarget(
					reinterpret_cast<SIZE_T>(predecessorTarget)))
			{
				if (!AccumulatorState().loggedTileRegisterObjectConflict)
				{
					AccumulatorState().loggedTileRegisterObjectConflict = true;
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
				&& !AccumulatorState().loggedTileRegisterObjectSlotUnavailable)
			{
				AccumulatorState().loggedTileRegisterObjectSlotUnavailable = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile RegisterObject dispatch is not initialized yet; installation will retry entry=%08X",
					kTileRegisterObjectFunctionEntry);
			}
			return false;
		}

		if (!hook_identity::IsExecutableTarget(
			reinterpret_cast<SIZE_T>(currentTarget)))
		{
			if (!AccumulatorState().loggedTileRegisterObjectConflict)
			{
				AccumulatorState().loggedTileRegisterObjectConflict = true;
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
			AccumulatorState().predecessorTileRegisterObject.load(std::memory_order_acquire);
		if (installedPredecessor
			&& currentTarget != reinterpret_cast<TileRegisterObjectFn>(
				kBSShaderAccumulatorRegisterObjectInterface)
			&& currentTarget != installedPredecessor)
		{
			if (!AccumulatorState().loggedTileRegisterObjectConflict)
			{
				AccumulatorState().loggedTileRegisterObjectConflict = true;
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
		AccumulatorState().predecessorTileRegisterObject.store(
			currentTarget, std::memory_order_release);
		const SafeWrite32IfEqualResult publication =
			PublishTileRegisterObjectHook(currentTarget);
		if (!publication.WasPublished())
		{
			// The slot changed after validation.  Preserve an older predecessor if
			// one may still be reached through a successor chain; otherwise allow a
			// clean retry against the newly observed initial target.
			AccumulatorState().predecessorTileRegisterObject.store(previousPredecessor,
				std::memory_order_release);
			if (!AccumulatorState().loggedTileRegisterObjectConflict)
			{
				AccumulatorState().loggedTileRegisterObjectConflict = true;
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
			if (!AccumulatorState().loggedTileRegisterObjectConflict)
			{
				AccumulatorState().loggedTileRegisterObjectConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile RegisterObject dispatch ownership changed immediately after publication actual=%p",
					ReadTileRegisterObjectTarget());
				InvalidateAllSingletonFacadeBindings();
			}
			return false;
		}

		AccumulatorState().loggedTileRegisterObjectConflict = false;
		AccumulatorState().loggedTileRegisterObjectSlotUnavailable = false;
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
			AccumulatorState().predecessorTileRegisterObject.load(std::memory_order_acquire);
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
			AccumulatorState().predecessorTileRegisterObject.load(std::memory_order_acquire);
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

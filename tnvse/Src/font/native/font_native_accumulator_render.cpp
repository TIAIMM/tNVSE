#include "font_native_accumulator_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_native_accumulator {}
	using namespace implementation::font_native_accumulator;

	namespace implementation::font_native_accumulator
	{
		void __fastcall NativeFontRenderAlphaGeometry(BSShaderAccumulator* accumulator, void*)
		{
			NativeFontShapeState& state = State();
			if (!state.predecessorRenderAlphaGeometry)
				return;
			FreeTypePerfScope frameRoutePerf(
				FreeTypePerfPhase::FrameRouteTotal);

			SortedPayloadScratch& scratch = AccumulatorThread().sortedPayloadScratch;
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
				{
					ResetSortedPrepScratch(scratch);
				}
				const bool stockRenderAlphaTraversal =
					state.predecessorRenderAlphaGeometry
						== reinterpret_cast<RenderAlphaGeometryFn>(
							kBSShaderAccumulatorRenderAlphaGeometry);
				const bool stockImmediateDispatch = stockRenderAlphaTraversal
					&& IsNativeFontRenderPassImmediatelyHookCurrentUnchecked();
				CaptureSortedTrackedShapeTopology(scratch, accumulator,
					stockRenderAlphaTraversal, stockImmediateDispatch);
				if (scratch.metadataShapes.empty())
				{
					RecordFreeTypePerf(
						FreeTypePerfCounter::AccumulatorEmptyFastPath);
					// The predecessor Sort contains no tracked native shape. This is
					// either an entirely vanilla traversal or a fail-open topology scan;
					// in both cases the ordinary dispatch path remains authoritative.
					// Do not build a readiness stamp or command frame.
					EndFreeTypePerfSample(
						FreeTypePerfPhase::FrameRoutePrep, framePrepStart);
					{
						FreeTypePerfScope vanillaRenderPerf(
							FreeTypePerfPhase::FrameRouteVanillaRender);
						state.predecessorRenderAlphaGeometry(accumulator);
					}
					ClearSortedFrame(scratch);
					return;
				}
				// All per-item diagnostics inside a tracked traversal retain exact totals,
				// but publish only once per touched counter when the stock alpha loop
				// returns. Empty/foreign traversals keep the cheaper direct counter path.
				// The route timer was declared outside, so the final flush remains part of
				// the measured end-to-end CPU cost.
				FreeTypePerfCounterBatchScope perfCounterBatch;

				// Visibility is now the first post-Sort preflight. The tracked shape owns
				// the final model bound and Tile/scissor state, so a proven cull does
				// not need a metadata owner, packet artifact, or renderer-ring slot.
				{
					BeginNativeFontVisibilityFrame();
					// Every tracked facade is unique at this point. Allocate its final
					// frame entry before visibility evaluation and populate the witness
					// there directly; no parallel preflight vector or full-witness copy
					// is needed later in the facade loop.
					scratch.frameEntries.reserve(scratch.metadataShapes.size());
					scratch.metadataAcquireShapes.clear();
					scratch.metadataAcquireShapes.reserve(
						scratch.metadataShapes.size());
					scratch.metadataAcquireEntryIndices.clear();
					scratch.metadataAcquireEntryIndices.reserve(
						scratch.metadataShapes.size());
					for (size_t index = 0;
						index < scratch.metadataShapes.size(); ++index)
					{
						NiTriShape* facade = scratch.metadataShapes[index];
						scratch.frameEntries.emplace_back();
						SortedFrameEntry& entry = scratch.frameEntries.back();
						entry.facade = facade;
						EvaluateNativeFontSortedVisibilityInPlace(
							facade, entry.visibility);
						if (entry.visibility.cull
							== NativeFontVisibilityCull::None)
						{
							scratch.metadataAcquireShapes.push_back(facade);
							scratch.metadataAcquireEntryIndices.push_back(
								static_cast<UInt32>(index));
						}
						else
						{
							RecordFreeTypePerf(FreeTypePerfCounter::
								VisibilityPreflightSkipped);
							RecordFreeTypePerf(FreeTypePerfCounter::
								AccumulatorMetadataCullSkipped);
						}
					}
					CompleteNativeFontVisibilityPreflight();
					RecordFreeTypePerf(FreeTypePerfCounter::
						AccumulatorMetadataOwnerSlotAvoided,
						scratch.metadataShapes.size()
							- scratch.metadataAcquireShapes.size());
				}
				if (!scratch.metadataAcquireShapes.empty())
				{
					AcquireNativeFontShapeMetadataBatch(
						scratch.metadataAcquireShapes,
						scratch.metadataOwners);
					for (const NativeFontShapeMetadataPtr& owner
						: scratch.metadataOwners)
					{
						if (!owner)
							++AccumulatorThread().thinRegistrationDiagnostics.metadataMissing;
					}
					const size_t mappedOwnerCount = std::min(
						scratch.metadataOwners.size(),
						scratch.metadataAcquireEntryIndices.size());
					for (size_t ownerIndex = 0;
						ownerIndex < mappedOwnerCount; ++ownerIndex)
					{
						const size_t entryIndex = static_cast<size_t>(
							scratch.metadataAcquireEntryIndices[ownerIndex]);
						if (entryIndex < scratch.frameEntries.size()
							&& ownerIndex < scratch.metadataAcquireShapes.size()
							&& scratch.frameEntries[entryIndex].facade
								== scratch.metadataAcquireShapes[ownerIndex])
						{
							scratch.frameEntries[entryIndex].metadata =
								scratch.metadataOwners[ownerIndex].get();
						}
					}
					RecordFreeTypePerf(FreeTypePerfCounter::
						AccumulatorMetadataIndexLookupElided,
						mappedOwnerCount);
					++AccumulatorThread().thinRegistrationDiagnostics.metadataBatches;
					AccumulatorThread().thinRegistrationDiagnostics.metadataShapes +=
						static_cast<UInt64>(
							scratch.metadataAcquireShapes.size());
				}

				NativePreflightFrameContext preflightContext;
				const bool hasMetadataSurvivors =
					!scratch.metadataAcquireShapes.empty();
				UInt32 generation = 0;
				UInt64 frameValidationToken = 0;
				{
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
					const size_t trackedCount = scratch.frameEntries.size();
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
					for (size_t candidateIndex = 0;
						candidateIndex < scratch.frameEntries.size();
						++candidateIndex)
					{
					SortedFrameEntry& entry =
						scratch.frameEntries[candidateIndex];
					NiTriShape* facade = entry.facade;
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
								AccumulatorState().missingMetadataLogCount.fetch_add(
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

					entry.uniqueOccurrence = entry.metadata && candidateIndex
						< scratch.sortedOccurrenceCounts.size()
						&& scratch.sortedOccurrenceCounts[candidateIndex] == 1;
					InsertSortedFacade(scratch, facade, candidateIndex);
					RecordFreeTypePerf(FreeTypePerfCounter::SortedFrameFacade);
					if (entry.preflightResult == NativeFontFallbackReason::None
						&& entry.payload && entry.payload->payloadTemplate
						&& entry.generation == generation)
					{
						if (InsertUniquePayload(scratch,
							entry.payload->payloadTemplate))
						{
							RecordFreeTypePerf(
								FreeTypePerfCounter::SortedFramePayload);
						}
					}
					}
				}
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
					PrepareSortedNativeFontPayloads(
						scratch.payloadTemplates, generation);
				}
				if (hasPreparedPayloads)
				{
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
					IsFreeTypeFontCommandBufferEnabledForCurrentRoute()
					&& hasPreparedPayloads;
				if (commandFrameActive)
				{
					{
						BeginNativeFontFrameCommandBuffer(accumulator,
							frameValidationToken, generation,
							preflightContext.atlasTextureEpoch);
						ReserveNativeFontFrameCommandBuffer(
							scratch.frameEntries.size(),
							scratch.singletonFacades.size());
					}
					{
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
						ActivateNativeFontFrameCommandBuffer();
					}
				}
				else
				{
					EndNativeFontFrameCommandBuffer();
				}
				{
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
				EndFreeTypePerfSample(
					FreeTypePerfPhase::FrameRoutePrep, framePrepStart);
				{
					FreeTypePerfScope vanillaRenderPerf(
						FreeTypePerfPhase::FrameRouteVanillaRender);
					// GPU timing has its own tracked-submission gate. Vanilla-layout
					// remains outside ring/command work, but a preflight-surviving
					// shape now admits a sampled asynchronous Tile alpha envelope.
					NiDX9Renderer* renderer = hasTrackedNativeSubmissions
						? NiDX9Renderer::GetSingleton() : nullptr;
					FreeTypeGpuAlphaEnvelopeScope gpuTiming(
						renderer ? renderer->GetD3DDevice() : nullptr,
						hasPreparedPayloads, hasVanillaLayoutSurvivors);
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

	}

	bool FindNativeFontSortedFrameEntry(NiTriShape* facade,
		NativeFontSortedFrameEntryView& view)
	{
		view = {};
		const SortedPayloadScratch& scratch = AccumulatorThread().sortedPayloadScratch;
		if (!scratch.active || !facade)
			return false;

		const auto publishEntry = [&](size_t index)
		{
			if (index == std::numeric_limits<size_t>::max()
				|| index >= scratch.frameEntries.size())
			{
				return false;
			}
			const SortedFrameEntry& entry = scratch.frameEntries[index];
			if (entry.facade != facade)
				return false;
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
		};

		// Retail B64F90 stores the reverse traversal index at this+0x2C before
		// its B64FD1 -> B994F0 call.  Use it only for the exact stock predecessor
		// and only while the live item count, array slot, and captured facade all
		// agree.  A plugin traversal, tie repair, mutation, or nested route falls
		// through to the identity-based hash table.
		const BSShaderAccumulator* accumulator = scratch.frameAccumulator;
		if (scratch.stockRenderAlphaTraversal
			&& scratch.stockImmediateDispatch && accumulator
			&& accumulator->m_iNumItems > 0
			&& static_cast<size_t>(accumulator->m_iNumItems)
				== scratch.sortedFrameEntryIndices.size()
			&& accumulator->m_ppkItems)
		{
			const SInt32 currentItem = accumulator->m_iCurrItem;
			if (currentItem >= 0
				&& static_cast<size_t>(currentItem)
					< scratch.sortedFrameEntryIndices.size()
				&& accumulator->m_ppkItems[currentItem] == facade)
			{
				const UInt32 entryIndex = scratch.sortedFrameEntryIndices[
					static_cast<size_t>(currentItem)];
				if (entryIndex != kInvalidNativeFontCommandIndex
					&& publishEntry(static_cast<size_t>(entryIndex)))
				{
					view.retailSortedItemIndex = currentItem;
					view.nestedTraversalSerial =
						scratch.nestedTraversalSerial;
					view.retailSortedItemMatched = true;
					RecordFreeTypePerf(FreeTypePerfCounter::
						SortedFrameItemIndexLookupHit);
					return true;
				}
			}
		}

		RecordFreeTypePerf(
			FreeTypePerfCounter::SortedFrameFacadeHashLookup);
		return publishEntry(LookupSortedFacade(scratch, facade));
	}

	UInt64 GetNativeFontSortedFrameValidationToken()
	{
		const SortedPayloadScratch& scratch = AccumulatorThread().sortedPayloadScratch;
		return scratch.active ? scratch.activeValidationToken : 0;
	}

	UInt64 GetNativeFontSortedNestedTraversalSerial()
	{
		return AccumulatorThread().sortedPayloadScratch.nestedTraversalSerial;
	}

	UInt32 GetNativeFontAtlasTextureEpoch()
	{
		return AccumulatorState().atlasTextureEpoch.load(std::memory_order_acquire);
	}

	void NotifyNativeFontAtlasTextureMutation()
	{
		UInt32 current = AccumulatorState().atlasTextureEpoch.load(std::memory_order_relaxed);
		for (;;)
		{
			UInt32 next = current + 1u;
			if (!next)
				next = 1u;
			if (AccumulatorState().atlasTextureEpoch.compare_exchange_weak(current, next,
				std::memory_order_release, std::memory_order_relaxed))
			{
				InvalidateAllSingletonFacadeBindings();
				NotifyNativeFontCommandExternalMutation(
					NativeFontCommandFallback::Atlas);
				return;
			}
		}
	}

}

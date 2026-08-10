#include "font_native_accumulator_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_native_accumulator {}
	using namespace implementation::font_native_accumulator;

	namespace implementation::font_native_accumulator
	{
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
				{
					accumulator.m_ppkItems[item] = scratch.tieOutput[item];
					// This exact-depth run no longer has the item-to-entry order
					// captured by the first final-array scan.  Invalidate the whole
					// run so dispatch uses the facade hash instead of a stale index.
					if (item < scratch.sortedFrameEntryIndices.size())
					{
						scratch.sortedFrameEntryIndices[item] =
							kInvalidNativeFontCommandIndex;
					}
				}
			}
			result.valid = true;
			return result;
		}

		bool CaptureSortedTrackedShapeTopology(SortedPayloadScratch& scratch,
			BSShaderAccumulator* accumulator, bool stockRenderAlphaTraversal,
			bool stockImmediateDispatch)
		{
			if (!accumulator || scratch.active || scratch.nestedBypassDepth
				|| accumulator->eRenderMode
					!= BSShaderManager::BSSM_RENDER_TILES
				|| accumulator->m_iNumItems <= 0
				|| !accumulator->m_ppkItems)
			{
				++AccumulatorThread().thinRegistrationDiagnostics.sortedScanFallback;
				return false;
			}

			const size_t itemCount =
				static_cast<size_t>(accumulator->m_iNumItems);
			scratch.sortedFrameEntryIndices.assign(
				itemCount, kInvalidNativeFontCommandIndex);
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
				size_t capturedMetadataIndex =
					std::numeric_limits<size_t>::max();
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
						capturedMetadataIndex = metadataIndex;
						accounted = true;
						break;
					}
					const size_t metadataIndex =
						static_cast<size_t>(stored - 1u);
					if (metadataIndex < scratch.metadataShapes.size()
						&& scratch.metadataShapes[metadataIndex] == facade)
					{
						++scratch.sortedOccurrenceCounts[metadataIndex];
						capturedMetadataIndex = metadataIndex;
						accounted = true;
						break;
					}
					slot = (slot + 1u) & metadataMask;
				}
				if (!accounted
					|| capturedMetadataIndex
						>= static_cast<size_t>(kInvalidNativeFontCommandIndex))
				{
					scratch.metadataShapes.clear();
					scratch.sortedOccurrenceCounts.clear();
					scratch.sortedFrameEntryIndices.clear();
					++AccumulatorThread().thinRegistrationDiagnostics.sortedScanFallback;
					return false;
				}
				scratch.sortedFrameEntryIndices[
					static_cast<size_t>(itemIndex)] =
						static_cast<UInt32>(capturedMetadataIndex);
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
						&& !AccumulatorThread().loggedEqualDepthTieRepairFailure)
					{
						AccumulatorThread().loggedEqualDepthTieRepairFailure = true;
						FreeTypeFontDebugLog(
							"tnvse_freetype_equal_depth_repair: status=fail-open items=%u",
							static_cast<UInt32>(itemCount));
					}
				}
				else if (repair.changedRuns
					&& g_bEnableFreeTypeFontRenderingLog
					&& !AccumulatorThread().loggedEqualDepthTieRepair)
				{
					AccumulatorThread().loggedEqualDepthTieRepair = true;
					FreeTypeFontDebugLog(
						"tnvse_freetype_equal_depth_repair: status=applied items=%u mixed_runs=%u changed_runs=%u changed_items=%u algorithm=linear-source-ordinal",
						static_cast<UInt32>(itemCount), repair.mixedRuns,
						repair.changedRuns, repair.changedItems);
				}
			}

			scratch.frameAccumulator = accumulator;
			scratch.stockRenderAlphaTraversal = stockRenderAlphaTraversal;
			scratch.stockImmediateDispatch = stockImmediateDispatch;
			return true;
		}

		void ResolveSingletonFacadeSortedTopology(
			SortedPayloadScratch& scratch, BSShaderAccumulator* accumulator,
			UInt64 validationToken)
		{
			if (!accumulator || !validationToken
				|| scratch.frameAccumulator != accumulator
				|| !accumulator->m_ppkItems)
			{
				++AccumulatorThread().thinRegistrationDiagnostics.sortedScanFallback;
				return;
			}

			for (UInt32 capturedEntryIndex
				: scratch.metadataAcquireEntryIndices)
			{
				const size_t entryIndex = static_cast<size_t>(capturedEntryIndex);
				if (entryIndex >= scratch.frameEntries.size())
					continue;
				SortedFrameEntry& frameEntry =
					scratch.frameEntries[entryIndex];
				const NativeFontShapeMetadata* metadata = frameEntry.metadata;
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

				const bool occurrenceValid =
					entryIndex < scratch.sortedOccurrenceCounts.size()
					&& scratch.sortedOccurrenceCounts[entryIndex] == 1;
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
					++AccumulatorThread().thinRegistrationDiagnostics.facadeTopology;
				}
				else
				{
					++AccumulatorThread().thinRegistrationDiagnostics.facadeFallback;
					if (!occurrenceValid)
						++AccumulatorThread().thinRegistrationDiagnostics.occurrenceFallback;
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

	}

}

#include "font_native_shape_hooks_detail.h"
#include "font_native_shape_standard_lite_detail.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_native_shape_hooks {}
	using namespace implementation::font_native_shape_hooks;

	namespace implementation::font_native_shape_hooks
	{
		NativeFontMetadataHotSet& GetMetadataHotSet(const NiTriShape* shape)
		{
			if (!MetadataHotSets())
			{
				MetadataHotSets() = std::make_unique<
					std::array<NativeFontMetadataHotSet, kMetadataHotSetCount>>();
			}
			const size_t index = HashMetadataShapeAddress(shape)
				& (kMetadataHotSetCount - 1);
			return (*MetadataHotSets())[index];
		}

		NativeFontMetadataHotEntry& SelectMetadataHotVictim(
			NativeFontMetadataHotSet& set)
		{
			for (NativeFontMetadataHotEntry& entry : set.ways)
			{
				if (!entry.shape)
					return entry;
			}
			for (NativeFontMetadataHotEntry& entry : set.ways)
			{
				if (entry.metadata.expired())
				{
					entry = {};
					return entry;
				}
			}
			NativeFontMetadataHotEntry& victim =
				set.ways[set.nextVictim % kMetadataHotWayCount];
			set.nextVictim = static_cast<UInt8>(
				(set.nextVictim + 1) % kMetadataHotWayCount);
			victim = {};
			return victim;
		}

		void LogMissingMetadata(NiTriShape* shape, const char* phase)
		{
			if (!g_bEnableFreeTypeFontRenderingLog)
				return;
			static std::atomic<UInt32> logCount = 0;
			constexpr UInt32 kMaximumLogs = 8;
			const UInt32 ordinal = logCount.fetch_add(1, std::memory_order_relaxed);
			if (ordinal < kMaximumLogs)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: submission-suppressed reason=metadata-missing phase=%s shape=%p thread=%u",
					phase ? phase : "unknown", shape, GetCurrentThreadId());
			}
			else if (ordinal == kMaximumLogs)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: metadata-missing render logs capped at %u entries",
					kMaximumLogs);
			}
		}

		void SuppressImmediateRoute(NiTriShape* shape, const char* phase)
		{
			const NativeFontShapeMetadataPtr metadata = FindNativeFontShapeMetadata(shape);
			if (!metadata)
			{
				LogMissingMetadata(shape, phase);
				return;
			}
			RecordNativeFontSuppression(shape, *metadata,
				metadata->nativePayload.buildComplete
					? NativeFontFallbackReason::DirectImmediate
					: NativeFontFallbackReason::PacketBuild,
				phase);
		}

		struct NativeFontMetadataIdentitySnapshot
		{
			UInt64 allocationId = 0;
			const NativeFontShapeMetadata* selfIdentity = nullptr;
			const NiTriShape* shapeIdentity = nullptr;
			UInt32 fontId = 0;
			UInt32 backend = 0;
			bool buildComplete = false;
		};

		bool TryReadNativeFontMetadataIdentity(
			const NativeFontShapeMetadata* metadata,
			NativeFontMetadataIdentitySnapshot& snapshot)
		{
			if (!metadata)
				return false;
			__try
			{
				snapshot.allocationId = metadata->allocationId;
				snapshot.selfIdentity = metadata->selfIdentity;
				snapshot.shapeIdentity = metadata->shapeIdentity;
				snapshot.fontId = metadata->fontId;
				snapshot.backend =
					static_cast<UInt32>(metadata->backend);
				snapshot.buildComplete =
					metadata->nativePayload.buildComplete;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool LogNativeFontMetadataDeleteAudit(NiTriShape* shape,
			bool registryFound, const NativeFontShapeMetadataEntry& entry)
		{
			const NativeFontShapeMetadata* mappedMetadata =
				entry.metadata.get();
			NativeFontMetadataIdentitySnapshot snapshot;
			const bool readable = TryReadNativeFontMetadataIdentity(
				mappedMetadata, snapshot);
			const bool registryIdentityValid =
				registryFound && entry.allocationId
				&& entry.selfIdentity
				&& entry.shapeIdentity == shape;
			const bool pointerMatch =
				mappedMetadata == entry.selfIdentity;
			const bool allocationMatch = readable
				&& snapshot.allocationId == entry.allocationId;
			const bool selfMatch = readable
				&& snapshot.selfIdentity == entry.selfIdentity
				&& snapshot.selfIdentity == mappedMetadata;
			const bool shapeMatch = readable
				&& snapshot.shapeIdentity == entry.shapeIdentity
				&& snapshot.shapeIdentity == shape;
			const bool integrity = registryIdentityValid
				&& pointerMatch && allocationMatch
				&& selfMatch && shapeMatch;

			static std::atomic<UInt32> failureLogCount = 0;
			const UInt32 failureOrdinal = integrity ? 0
				: failureLogCount.fetch_add(
					1, std::memory_order_relaxed);
			const bool logFailure = !integrity
				&& failureOrdinal < 64;
			// Successful delete audits were useful while proving the lifetime
			// guard, but they dominate normal diagnostic logs. Keep the complete
			// record only for an actual integrity failure, even when verbose font
			// logging is enabled.
			if (!logFailure)
				return integrity;

			gLog.FormattedMessage(
				"tnvse_freetype_native: metadata-delete-integrity-failure shape=%p registry=%u mapped=%p expectedSelf=%p expectedShape=%p allocationId=%llu readable=%u objectAllocationId=%llu objectSelf=%p objectShape=%p font=%u backend=%u build=%u registryIdentity=%u pointer=%u allocation=%u self=%u shapeIdentity=%u integrity=%u",
				shape, registryFound ? 1u : 0u,
				mappedMetadata, entry.selfIdentity,
				entry.shapeIdentity,
				static_cast<unsigned long long>(entry.allocationId),
				readable ? 1u : 0u,
				static_cast<unsigned long long>(
					snapshot.allocationId),
				snapshot.selfIdentity, snapshot.shapeIdentity,
				snapshot.fontId, snapshot.backend,
				snapshot.buildComplete ? 1u : 0u,
				registryIdentityValid ? 1u : 0u,
				pointerMatch ? 1u : 0u,
				allocationMatch ? 1u : 0u,
				selfMatch ? 1u : 0u,
				shapeMatch ? 1u : 0u,
				integrity ? 1u : 0u);
			return integrity;
		}

		void __fastcall NativeFontDeleteThis(NiTriShape* shape, void*)
		{
			NativeFontShapeState& state = State();
			InvalidateNativeFontCommandGeometry(shape);
			NativeFontShapeMetadataEntry retiredEntry;
			bool registryFound = false;
			{
				std::lock_guard<std::mutex> lock(state.metadataMutex);
				const auto found = state.shapeMetadata.find(shape);
				if (found != state.shapeMetadata.end())
				{
					registryFound = true;
					state.metadataGenerations[GetMetadataGenerationSlot(shape)].fetch_add(1,
						std::memory_order_release);
					retiredEntry = std::move(found->second);
					state.shapeMetadata.erase(found);
				}
			}
			const bool metadataIntegrity =
				LogNativeFontMetadataDeleteAudit(
				shape, registryFound, retiredEntry);
			NativeFontShapeMetadataPtr retiredMetadata =
				std::move(retiredEntry.metadata);
			if (metadataIntegrity && retiredMetadata
				&& retiredMetadata->nativePayload.buildComplete)
			{
				InvalidateNativeFontTileRetainedText(
					retiredMetadata->nativePayload);
			}
			if (metadataIntegrity && retiredMetadata
				&& retiredMetadata->backend
					== FreeTypeShapeBackend::SingletonFacade)
			{
				ReleaseSingletonFacadeBinding(
					shape, *retiredMetadata);
			}
			retiredMetadata.reset();
			state.originalDeleteThis(shape);
		}
	}

	NativeFontShapeMetadataPtr FindNativeFontShapeMetadata(const NiTriShape* shape)
	{
		if (!shape)
			return {};
		NativeFontShapeState& state = State();
		const size_t generationSlot = GetMetadataGenerationSlot(shape);
		const UInt64 generation = state.metadataGenerations[generationSlot].load(
			std::memory_order_acquire);
		NativeFontMetadataHotSet& hotSet = GetMetadataHotSet(shape);
		NativeFontMetadataHotEntry* replacement = nullptr;
		for (NativeFontMetadataHotEntry& hot : hotSet.ways)
		{
			if (hot.shape != shape)
				continue;
			if (hot.generation == generation)
			{
				NativeFontShapeMetadataPtr metadata = hot.metadata.lock();
				if (metadata)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::MetadataHotHit);
					return metadata;
				}
			}
			hot = {};
			replacement = &hot;
		}

		RecordFreeTypePerf(FreeTypePerfCounter::MetadataLockedLookup);
		std::lock_guard<std::mutex> lock(state.metadataMutex);
		const auto found = state.shapeMetadata.find(shape);
		if (found == state.shapeMetadata.end())
			return {};
		NativeFontMetadataHotEntry& hot = replacement
			? *replacement : SelectMetadataHotVictim(hotSet);
		hot.shape = shape;
		hot.generation = state.metadataGenerations[generationSlot].load(
			std::memory_order_relaxed);
		hot.metadata = found->second.metadata;
		return found->second.metadata;
	}

	void AcquireNativeFontShapeMetadataBatch(
		const std::vector<NiTriShape*>& shapes,
		std::vector<NativeFontShapeMetadataPtr>& owners)
	{
		owners.clear();
		owners.resize(shapes.size());
		if (shapes.empty())
			return;

		NativeFontShapeState& state = State();
		std::lock_guard<std::mutex> lock(state.metadataMutex);
		for (size_t index = 0; index < shapes.size(); ++index)
		{
			const NiTriShape* shape = shapes[index];
			if (!shape)
				continue;
			const auto found = state.shapeMetadata.find(shape);
			if (found == state.shapeMetadata.end())
				continue;

			const NativeFontShapeMetadataEntry& entry = found->second;
			const NativeFontShapeMetadataPtr& metadata = entry.metadata;
			if (!metadata || entry.allocationId == 0
				|| entry.allocationId != metadata->allocationId
				|| entry.selfIdentity != metadata.get()
				|| entry.selfIdentity != metadata->selfIdentity
				|| entry.shapeIdentity != shape
				|| entry.shapeIdentity != metadata->shapeIdentity)
			{
				continue;
			}
			owners[index] = metadata;
		}
	}
}

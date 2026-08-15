#include "font_atlas_snapshot_internal.h"

#include "font_atlas_nvtf_compat.h"
#include "font_atlas_resource_internal.h"

#include "font_manager.h"
#include "load_config.h"
#include "native_calls.h"

#include "NiDX9Renderer.hpp"
#include "NiFixedString.hpp"
#include "NiGlobalStringTable.hpp"
#include "NiDX9TextureData.hpp"
#include "NiTriShapeData.hpp"
#include "Utils/SafeWrite.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include <winioctl.h>

namespace fonthook::vectorfont
{
	namespace implementation::font_atlas_snapshot {}
	using namespace implementation::font_atlas_snapshot;

	bool LoadGlyphAtlasSnapshotRole(RuntimeFont& runtime,
		VectorFontByteClass byteClass, float rasterScale, bool metadataOnly)
	{
		const FontConfig& config = GetRuntimeConfig(runtime);
		AtlasCacheKey key;
		if (!ResolvePrewarmAtlasKey(config, byteClass, rasterScale, key))
			return false;
		if (!metadataOnly && TryReuseCompleteAtlasProfile(key))
			return true;
		const bool directDefaultPublication = !metadataOnly
			&& g_bEnableFreeTypeDefaultPoolAtlas;
		DefaultPoolPublicationScope publicationScope(
			directDefaultPublication);
		const NvtfTextureLockCompatibilityState lockCompatibility =
			GetNvtfTextureLockCompatibilityState(true);
		if (directDefaultPublication
			&& (!publicationScope.Ready() || !publicationScope.IsCurrent()))
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: direct atlas shadow publication rejected font=%u role=%s reason=%s deviceEpoch=%llu",
				key.fontId,
				key.byteClass == VectorFontByteClass::DoubleByte
					? "doubleByte" : "singleByte",
				publicationScope.Ready()
					? "device-generation-changed"
					: publicationScope.FailureReason(),
				static_cast<unsigned long long>(
					publicationScope.DeviceEpoch()));
			return false;
		}
		if (directDefaultPublication && lockCompatibility.active
			&& !publicationScope.DeviceIsMultithreaded())
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: direct atlas shadow publication rejected font=%u role=%s pattern=%s reason=d3d9-device-not-multithreaded policy=retain-current-generation",
				key.fontId,
				key.byteClass == VectorFontByteClass::DoubleByte
					? "doubleByte" : "singleByte",
				lockCompatibility.exactMode2Pattern
					? "nvtf-mode-2" : "partial-or-foreign");
			return false;
		}
		UInt64 snapshotHash = 0;
		UInt64 maskContentHash = 0;
		std::vector<std::pair<AtlasCacheKey, std::shared_ptr<AtlasResource>>> pages;
		UInt64 totalBytes = 0;
		UInt64 totalPlacements = 0;
		UInt16 pageCount = 0;
		UInt16 defaultPoolPages = 0;
		UInt16 deduplicatedPages = 0;
		UInt16 physicalAliasPages = 0;
		UInt64 deduplicatedGpuBytes = 0;
		UInt64 snapshotPayloadBytesRead = 0;
		for (UInt16 pageIndex = 0; pageIndex == 0 || pageIndex < pageCount; ++pageIndex)
		{
			AtlasCacheKey pageKey = key;
			pageKey.pageIndex = pageIndex;
			UInt64 pageSnapshotHash = 0;
			UInt64 pageContentHash = 0;
			const std::wstring path = GetAtlasSnapshotPath(runtime, pageKey,
				pageSnapshotHash, pageContentHash);
			if (path.empty())
				return false;
			if (pageIndex == 0)
			{
				snapshotHash = pageSnapshotHash;
				maskContentHash = pageContentHash;
			}
			AtlasSnapshotHeader header = {};
			std::vector<AtlasSnapshotPlacement> placementList;
			SnapshotPayloadSource payload;
			if (pageSnapshotHash != snapshotHash
				|| pageContentHash != maskContentHash
				|| !ReadSnapshotMetadata(path, header, &placementList,
					payload))
				return false;
			MarkFreeTypeFontCacheFileUsed(payload.path);
			if (header.flags & kAtlasSnapshotFlagPhysicalPayloadAlias)
				++physicalAliasPages;
			const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'A', 'T', 'L', '9' };
			const bool expectsPlacedLevelZero = UsesPlacedLevelZeroSnapshot(pageKey);
			const AtlasSnapshotStorage storageMode =
				static_cast<AtlasSnapshotStorage>(header.storageMode);
			const bool storageModeValid = expectsPlacedLevelZero
				? storageMode == AtlasSnapshotStorage::PlacedLevelZeroRects
				: storageMode == AtlasSnapshotStorage::FullMipChain;
			const UInt64 placementsBytes = static_cast<UInt64>(header.placementCount)
				* sizeof(AtlasSnapshotPlacement);
			const UInt64 payloadOffset = sizeof(header) + placementsBytes;
			const bool payloadSizeValid = payloadOffset >= sizeof(header)
				&& header.storedPixelBytes
					<= std::numeric_limits<UInt64>::max() - payloadOffset
				&& payloadOffset + header.storedPixelBytes
					== payload.fileBytes;
			// Producer caps are intentionally not persisted or compared. Accept any
			// snapshot whose actual stored page shape is legal on this device.
			const SnapshotPackingCaps targetCaps =
				GetSnapshotPackingCaps();
			const VectorFontByteClass packingByteClass =
				(header.flags & kAtlasSnapshotFlagJointByteRoles)
					? VectorFontByteClass::DoubleByte : pageKey.byteClass;
			const bool shapeValid = IsSnapshotPageShapeValid(
				header.width, header.height,
				GetSnapshotMaximumSize(
					targetCaps, packingByteClass), targetCaps)
				&& header.mipLevels >= 1
				&& header.mipLevels <= kMaximumAtlasMipLevels;
			const size_t fullPixelBytes = shapeValid
				? GetAtlasStorageBytes(header.width, header.height,
					pageKey.pixelMode, header.mipLevels)
				: 0;
			if (std::memcmp(header.magic, magic, sizeof(magic)) != 0
				|| header.version != kAtlasSnapshotVersion
				|| header.headerSize != sizeof(header)
				|| header.snapshotHash != snapshotHash
				|| header.maskContentHash != maskContentHash
				|| header.atlasContentHash != pageKey.atlasContentHash
				|| (header.flags & ~kAtlasSnapshotKnownFlags) != 0
				|| header.scaleMilli != pageKey.scaleMilli
				|| header.pixelMode != static_cast<UInt8>(pageKey.pixelMode)
				|| header.renderMode != static_cast<UInt8>(pageKey.renderMode)
				|| !storageModeValid
				|| header.byteClass != static_cast<UInt8>(pageKey.byteClass)
				|| header.padding != pageKey.padding || !payloadSizeValid
				|| header.pageIndex != pageIndex || !header.pageCount
				|| header.pageCount > kMaximumAtlasSnapshotPages
				|| (!metadataOnly
					&& !MatchesDefaultPoolSnapshotLayout(header))
				|| (pageIndex && header.pageCount != pageCount)
				|| !shapeValid
				|| header.mipLevels != GetAtlasMipLevelCount(header.width, header.height,
					pageKey.levelZeroOnly)
				|| header.pixelBytes > fullPixelBytes
				|| header.pixelBytes > std::numeric_limits<size_t>::max()
				|| header.storedPixelBytes > std::numeric_limits<size_t>::max()
				|| !header.pageContentHash
				|| (!expectsPlacedLevelZero && (header.pixelBytes != fullPixelBytes
					|| header.storedPixelBytes != header.pixelBytes))
				|| header.checksum != HashAtlasBytes(&header,
					offsetof(AtlasSnapshotHeader, checksum)))
				return false;
			if (!pageIndex)
				pageCount = header.pageCount;
			auto resource = std::make_shared<AtlasResource>();
			resource->width = header.width;
			resource->height = header.height;
			resource->cursorX = header.cursorX;
			resource->cursorY = header.cursorY;
			resource->shelfHeight = header.shelfHeight;
			resource->padding = header.padding;
			resource->mipLevels = header.mipLevels;
			resource->pixelMode = pageKey.pixelMode;
			resource->renderMode = pageKey.renderMode;
			resource->byteClass = pageKey.byteClass;
			resource->levelZeroOnly = pageKey.levelZeroOnly;
			if (header.storedPixelBytes != header.pixelBytes)
				return false;
			resource->glyphs.reserve(header.placementCount);
			for (UInt32 index = 0; index < header.placementCount; ++index)
			{
				const AtlasRect& rect = placementList[index].rect;
				if (!placementList[index].cacheId || !rect.width || !rect.height
					|| rect.x > header.width || rect.width > header.width - rect.x
					|| rect.y > header.height || rect.height > header.height - rect.y
					|| !IsValidSnapshotPlacement(placementList[index])
					|| !IsValidAtlasSnapshotGlyphPlacement(placementList[index],
						header.width, header.height, header.pageIndex))
					return false;
				resource->glyphs.push_back({ placementList[index].cacheId,
					rect, nullptr, index });
			}
			SortAtlasGlyphs(*resource);
			if (std::adjacent_find(resource->glyphs.begin(), resource->glyphs.end(),
				[](const AtlasGlyphRecord& lhs, const AtlasGlyphRecord& rhs)
				{
					return lhs.cacheId == rhs.cacheId;
				}) != resource->glyphs.end())
			{
				return false;
			}
			auto compactSnapshot = std::make_shared<CompactAtlasSnapshot>();
			compactSnapshot->pixelMode = pageKey.pixelMode;
			compactSnapshot->placements = std::move(placementList);
			compactSnapshot->sourcePath = std::move(payload.path);
			compactSnapshot->sourceHeader = payload.header;
			resource->compactSnapshot = compactSnapshot;
			bool restoredToDefaultPool = false;
			if (metadataOnly)
			{
				// The repacker only needs dimensions, placements, and the immutable
				// source-file identity. Do not create a temporary D3D9 texture or
				// retain a decoded CPU page for this intermediate generation.
				resource->backend = AtlasBackend::DefaultPool;
				resource->pageContentHash = header.pageContentHash;
			}
			else
			{
				// Level-zero-only pages keep only placed texels. DEFAULT-pool restore
				// streams them directly; later resets reuse the validated snapshot
				// instead of retaining a second CPU pixel copy.
				const bool compactDefaultEligible = g_bEnableFreeTypeDefaultPoolAtlas
					&& !State().defaultPoolShutdown
					&& expectsPlacedLevelZero;
				if (compactDefaultEligible)
				{
					const UInt64 candidateGpuBytes = GetAtlasStorageBytes(
						header.width, header.height, pageKey.pixelMode,
						header.mipLevels);
					LogNvtfMode2DirectAtlasPublication(
						header.width, header.height, candidateGpuBytes,
						publicationScope.DeviceIsMultithreaded(),
						publicationScope.DeviceEpoch());
					size_t packedBytes = 0;
					if (!GetPlacedLevelZeroSnapshotBytes(compactSnapshot->placements,
						header.width, header.height, pageKey.pixelMode, packedBytes)
						|| packedBytes != header.pixelBytes)
					{
						return false;
					}
					resource->backend = AtlasBackend::DefaultPool;
					resource->pageContentHash = header.pageContentHash;
					const bool deduplicated = TryReuseDefaultPoolAtlasPage(resource,
						header.pageContentHash);
					restoredToDefaultPool = deduplicated || CreateDefaultPoolAtlas(
						*resource, pageKey.pixelMode);
					if (restoredToDefaultPool)
					{
						++defaultPoolPages;
						if (deduplicated)
						{
							++deduplicatedPages;
							deduplicatedGpuBytes += GetAtlasStorageBytes(resource->width,
								resource->height, resource->pixelMode, resource->mipLevels);
						}
						else
						{
							RegisterDefaultPoolAtlasPage(resource, header.pageContentHash);
							snapshotPayloadBytesRead += header.pixelBytes;
						}
					}
				}
				if (!restoredToDefaultPool)
				{
					if (g_bEnableFreeTypeDefaultPoolAtlas)
						return false;
					resource->backend = AtlasBackend::Managed;
					std::vector<UInt8> storedPixelVector;
					if (!LoadCompactAtlasSnapshotPixels(*compactSnapshot,
						storedPixelVector)
						|| header.pageContentHash != ComputeAtlasPageContentHash(header,
							compactSnapshot->placements, storedPixelVector))
					{
						return false;
					}
					std::vector<UInt8> pixels;
					if (!DecodeAtlasSnapshotPixels(header, compactSnapshot->placements,
						storedPixelVector.data(), pixels))
						return false;
					NiTexturingProperty* property = CreateManagedAtlasProperty(header.width,
						header.height, pageKey.pixelMode, header.mipLevels,
						pixels, resource->pixelData);
					if (!property)
					{
						if (IsFontPrewarmActive())
							MarkAtlasAllocationMemoryPressure();
						return false;
					}
					resource->property = property;
					snapshotPayloadBytesRead += storedPixelVector.size();
				}
			}
			compactSnapshot->cpuMemory.Reset(CpuMemoryCategory::AtlasMetadata,
				sizeof(CompactAtlasSnapshot)
					+ compactSnapshot->placements.capacity()
						* sizeof(AtlasSnapshotPlacement)
					+ compactSnapshot->pixels.capacity()
					+ compactSnapshot->sourcePath.capacity() * sizeof(wchar_t));
			RefreshAtlasResourceCpuMemory(*resource);
			resource->generation = 1;
			for (AtlasGlyphRecord& glyph : resource->glyphs)
			{
				if (glyph.snapshotPlacementIndex >= compactSnapshot->placements.size()
					|| !RestoreAtlasSnapshotGlyphPlacement(
						compactSnapshot->placements[glyph.snapshotPlacementIndex],
						*resource, header.pageIndex, header.pageIndex,
						glyph.placement))
				{
					return false;
				}
			}
			totalBytes += header.pixelBytes;
			totalPlacements += header.placementCount;
			pages.push_back({ pageKey, resource });
		}
		if (pages.empty() || pages.size() != pageCount)
			return false;
		if (directDefaultPublication && !publicationScope.IsCurrent())
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: direct atlas shadow publication aborted before profile swap font=%u role=%s reason=device-generation-changed deviceEpoch=%llu policy=destroy-shadow-retain-current-generation",
				key.fontId,
				key.byteClass == VectorFontByteClass::DoubleByte
					? "doubleByte" : "singleByte",
				static_cast<unsigned long long>(
					publicationScope.DeviceEpoch()));
			return false;
		}
		UInt32 replacedPages = 0;
		bool insertedAllPages = true;
		{
			AtlasState& state = State();
			std::lock_guard<std::mutex> lock(state.atlasMutex);
			// Incremental prewarm can encounter demand-created pages for the same
			// profile, including pages used by the native Tile progress component,
			// before the validated snapshot is restored. Keeping those entries would
			// discard the full resources built above, then incorrectly mark the
			// partial profile complete. Replace the whole profile generation
			// atomically, retire externally-held DEFAULT wrappers through the normal
			// lifetime path, and let the prewarm host rebuild its text geometry after
			// this restore step.
			const AtlasProfileKey profileKey = MakeAtlasProfileKey(key);
			if (metadataOnly)
				InvalidateCompleteAtlasProfileLocked(state, profileKey);
			const auto existingProfile = state.atlasProfiles.find(profileKey);
			std::vector<UInt16> existingPages = existingProfile
				!= state.atlasProfiles.end() ? existingProfile->second.pages
				: std::vector<UInt16>();
			// A previous interrupted profile-index update can leave a cache node
			// whose page is absent from AtlasProfileIndex. Treat every target-key
			// collision as part of the old generation before inserting anything;
			// this makes all subsequent emplace operations non-conflicting while
			// atlasMutex excludes demand publication.
			for (const auto& page : pages)
			{
				if (state.atlasCache.find(page.first) != state.atlasCache.end()
					&& std::find(existingPages.begin(), existingPages.end(),
						page.first.pageIndex) == existingPages.end())
				{
					existingPages.push_back(page.first.pageIndex);
				}
			}
			std::sort(existingPages.begin(), existingPages.end());
			existingPages.erase(std::unique(existingPages.begin(),
				existingPages.end()), existingPages.end());
			for (UInt16 existingPageIndex : existingPages)
			{
				AtlasCacheKey existingKey = key;
				existingKey.pageIndex = existingPageIndex;
				const auto existing = state.atlasCache.find(existingKey);
				if (existing == state.atlasCache.end())
				{
					UnindexAtlasPage(state, existingKey);
					continue;
				}
				if (existing->second.resource)
					RetireDefaultGeneration(*existing->second.resource);
				UnindexAtlasPage(state, existingKey);
				state.atlasCacheBytes -= existing->second.bytes;
				state.atlasLru.erase(existing->second.lru);
				state.atlasCache.erase(existing);
				++replacedPages;
			}
			for (const auto& page : pages)
			{
				const size_t storageBytes = metadataOnly ? 0
					: (page.second->sharedGpuPage ? 0
						: GetAtlasStorageBytes(page.second->width,
							page.second->height, page.second->pixelMode,
							page.second->mipLevels));
				state.atlasLru.push_front(page.first);
				const auto inserted = state.atlasCache.emplace(page.first, AtlasCacheEntry{
					page.second, storageBytes, state.atlasLru.begin() });
				if (!inserted.second)
				{
					state.atlasLru.pop_front();
					insertedAllPages = false;
					break;
				}
				inserted.first->second.cpuMemory.Reset(CpuMemoryCategory::AtlasMetadata,
					sizeof(AtlasCacheEntry) + 2u * sizeof(AtlasCacheKey)
						+ 4u * sizeof(void*));
				IndexAtlasPage(state, page.first, *page.second);
				state.atlasCacheBytes += storageBytes;
			}
			if (insertedAllPages)
			{
				if (metadataOnly)
				{
					const auto stagedProfile = state.atlasProfiles.find(profileKey);
					insertedAllPages = stagedProfile != state.atlasProfiles.end()
						&& stagedProfile->second.pages.size() == pageCount;
				}
				else if (IsCompleteAtlasProfileResidentLocked(state, key))
					state.completeAtlasProfiles.insert(MakeAtlasProfileKey(key));
				else
					insertedAllPages = false;
			}
			if (!metadataOnly)
				RefreshAtlasCacheGpuAccountingLocked(state);
		}
		if (!insertedAllPages)
			return false;
		if (metadataOnly)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: atlas snapshot metadata staged font=%u role=%s pages=%u physicalAliasPages=%u replacedPages=%u placements=%llu sourceBytes=%llu gpuBytes=0",
				key.fontId, key.byteClass == VectorFontByteClass::DoubleByte
					? "doubleByte" : "singleByte", pageCount,
				physicalAliasPages, replacedPages,
				static_cast<unsigned long long>(totalPlacements),
				static_cast<unsigned long long>(totalBytes));
		}
		else
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: atlas snapshot restored font=%u role=%s pages=%u physicalAliasPages=%u replacedPages=%u defaultPoolPages=%u deduplicatedPages=%u deduplicatedGpuBytes=%llu placements=%llu bytes=%llu snapshotPayloadBytesRead=%llu",
				key.fontId, key.byteClass == VectorFontByteClass::DoubleByte
					? "doubleByte" : "singleByte", pageCount,
				physicalAliasPages, replacedPages,
				defaultPoolPages, deduplicatedPages,
				static_cast<unsigned long long>(deduplicatedGpuBytes),
				static_cast<unsigned long long>(totalPlacements),
				static_cast<unsigned long long>(totalBytes),
				static_cast<unsigned long long>(snapshotPayloadBytesRead));
		}
		return true;
	}
}

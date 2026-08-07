#include "font_atlas_snapshot_internal.h"

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

	namespace implementation::font_atlas_snapshot
	{
		bool MarkPhysicalAtlasGroupFallback(RuntimeFont& runtime,
			const AtlasCacheKey& baseKey, UInt32 pageCount)
		{
			if (!pageCount || pageCount > kMaximumAtlasSnapshotPages)
				return false;
			const UInt8 magic[8] =
				{ 'T', 'N', 'V', 'F', 'A', 'T', 'L', '9' };
			for (UInt32 pageIndex = 0; pageIndex < pageCount; ++pageIndex)
			{
				AtlasCacheKey pageKey = baseKey;
				pageKey.pageIndex = static_cast<UInt16>(pageIndex);
				UInt64 snapshotHash = 0;
				UInt64 maskContentHash = 0;
				const std::wstring path = GetAtlasSnapshotPath(
					runtime, pageKey, snapshotHash, maskContentHash);
				if (path.empty())
					return false;
				HANDLE file = CreateFileW(path.c_str(),
					GENERIC_READ | GENERIC_WRITE,
					FILE_SHARE_READ | FILE_SHARE_WRITE
						| FILE_SHARE_DELETE,
					nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (file == INVALID_HANDLE_VALUE)
					return false;
				AtlasSnapshotHeader header = {};
				bool valid = ReadSnapshotBytesExact(
					file, &header, sizeof(header))
					&& std::memcmp(header.magic, magic, sizeof(magic)) == 0
					&& header.version == kAtlasSnapshotVersion
					&& header.headerSize == sizeof(header)
					&& header.snapshotHash == snapshotHash
					&& header.maskContentHash == maskContentHash
					&& header.atlasContentHash
						== pageKey.atlasContentHash
					&& header.pageIndex == pageIndex
					&& header.pageCount == pageCount
					&& !(header.flags & ~kAtlasSnapshotKnownFlags)
					&& header.checksum == HashAtlasBytes(&header,
						offsetof(AtlasSnapshotHeader, checksum));
				if (valid
					&& !(header.flags
						& kAtlasSnapshotFlagPhysicalFontGroupFallback))
				{
					header.flags |=
						kAtlasSnapshotFlagPhysicalFontGroupFallback;
					header.checksum = HashAtlasBytes(&header,
						offsetof(AtlasSnapshotHeader, checksum));
					LARGE_INTEGER beginning = {};
					valid = SetFilePointerEx(file, beginning, nullptr,
							FILE_BEGIN) != FALSE
						&& WriteSequentialFileBytes(file,
							&header, sizeof(header))
						&& FlushFileBuffers(file) != FALSE;
				}
				CloseHandle(file);
				if (!valid)
					return false;
			}
			return true;
		}

		UInt64 PackAtlasRectKey(const AtlasRect& rect)
		{
			if (rect.x > 0xFFFFu || rect.y > 0xFFFFu
				|| rect.width > 0xFFFFu || rect.height > 0xFFFFu)
			{
				return 0;
			}
			return static_cast<UInt64>(rect.x)
				| (static_cast<UInt64>(rect.y) << 16)
				| (static_cast<UInt64>(rect.width) << 32)
				| (static_cast<UInt64>(rect.height) << 48);
		}

		bool ResolveVanillaLetterAtlasRect(const FontLetter& letter,
			UInt32 atlasWidth, UInt32 atlasHeight, AtlasRect& rect)
		{
			if (!atlasWidth || !atlasHeight || letter.iTextureIndex < 0)
				return false;
			float minimumU = letter.pMapping[0].fU;
			float maximumU = minimumU;
			float minimumV = letter.pMapping[0].fV;
			float maximumV = minimumV;
			for (const UVMap& uv : letter.pMapping)
			{
				if (!std::isfinite(uv.fU) || !std::isfinite(uv.fV))
					return false;
				minimumU = std::min(minimumU, uv.fU);
				maximumU = std::max(maximumU, uv.fU);
				minimumV = std::min(minimumV, uv.fV);
				maximumV = std::max(maximumV, uv.fV);
			}
			const long left = std::lround(minimumU * atlasWidth);
			const long right = std::lround(maximumU * atlasWidth);
			const long top = std::lround(minimumV * atlasHeight);
			const long bottom = std::lround(maximumV * atlasHeight);
			if (left < 0 || top < 0 || right <= left || bottom <= top
				|| static_cast<UInt64>(right)
					> static_cast<UInt64>(atlasWidth)
				|| static_cast<UInt64>(bottom)
					> static_cast<UInt64>(atlasHeight))
			{
				return false;
			}
			rect = {
				static_cast<UInt32>(left),
				static_cast<UInt32>(top),
				static_cast<UInt32>(right - left),
				static_cast<UInt32>(bottom - top)
			};
			return true;
		}

		std::shared_ptr<AtlasResource> CreateRoleFilteredAtlasView(
			const AtlasResource& source,
			const DirectAtlasGlyphTable& table, UInt16 pageSlot)
		{
			if (!source.compactSnapshot || pageSlot >= table.pages.size())
				return nullptr;
			auto view = std::make_shared<AtlasResource>();
			view->width = source.width;
			view->height = source.height;
			view->cursorX = source.cursorX;
			view->cursorY = source.cursorY;
			view->shelfHeight = source.shelfHeight;
			view->padding = source.padding;
			view->generation = source.generation;
			view->mipLevels = source.mipLevels;
			view->pixelMode = source.pixelMode;
			view->backend = source.backend;
			view->renderMode = source.renderMode;
			view->byteClass = table.byteClass;
			view->levelZeroOnly = source.levelZeroOnly;
			view->pageContentHash = source.pageContentHash;
			view->compactSnapshot = source.compactSnapshot;

			std::unordered_set<UInt64> selectedCacheIds;
			if (table.recordKind
				== DirectCachedLetterKind::EffectLayers)
			{
				for (const DirectAtlasGlyphRecord& glyph : table.glyphs)
				{
					if (!(glyph.flags & kDirectCachedLetterValid)
						|| (glyph.flags
							& kDirectCachedLetterKnownEmpty))
					{
						continue;
					}
					for (const DirectAtlasGlyphLayer& layer : glyph.layers)
					{
						if (!layer.valid() || layer.pageSlot != pageSlot
							|| layer.snapshotPlacementIndex
								>= source.compactSnapshot
									->placements.size())
						{
							continue;
						}
						const UInt64 cacheId =
							source.compactSnapshot->placements[
								layer.snapshotPlacementIndex].cacheId;
						if (!cacheId)
							return nullptr;
						selectedCacheIds.insert(cacheId);
					}
				}
			}
			else
			{
				std::unordered_map<UInt64, UInt64> rectCacheIds;
				rectCacheIds.reserve(source.glyphs.size());
				for (const AtlasGlyphRecord& glyph : source.glyphs)
				{
					const UInt64 rectKey = PackAtlasRectKey(glyph.rect);
					if (!rectKey
						|| !rectCacheIds.emplace(
							rectKey, glyph.cacheId).second)
					{
						return nullptr;
					}
				}
				for (const FontLetter& letter : table.vanillaGlyphs)
				{
					if (letter.iTextureIndex != pageSlot)
						continue;
					AtlasRect rect;
					if (!ResolveVanillaLetterAtlasRect(
						letter, source.width, source.height, rect))
					{
						return nullptr;
					}
					const auto cacheId =
						rectCacheIds.find(PackAtlasRectKey(rect));
					if (cacheId == rectCacheIds.end()
						|| !cacheId->second)
					{
						return nullptr;
					}
					selectedCacheIds.insert(cacheId->second);
				}
			}
			if (selectedCacheIds.empty())
				return nullptr;

			view->glyphs.reserve(selectedCacheIds.size());
			for (UInt64 cacheId : selectedCacheIds)
			{
				const AtlasGlyphRecord* glyph =
					FindAtlasGlyph(source, cacheId);
				if (!glyph)
					return nullptr;
				view->glyphs.push_back(*glyph);
			}
			std::sort(view->glyphs.begin(), view->glyphs.end(),
				[](const AtlasGlyphRecord& left,
					const AtlasGlyphRecord& right)
				{
					return left.cacheId < right.cacheId;
				});
			return view;
		}

		bool CollectRoleFilteredResourcesLocked(
			const AtlasCacheKey& roleKey,
			const DirectAtlasGlyphTable& directTable,
			std::vector<std::pair<AtlasCacheKey,
				std::shared_ptr<AtlasResource>>>& destination,
			std::vector<std::shared_ptr<AtlasResource>>& physicalSources)
		{
			AtlasState& state = State();
			const auto profile = state.atlasProfiles.find(
				MakeAtlasProfileKey(roleKey));
			if (profile == state.atlasProfiles.end()
				|| profile->second.pages.empty()
				|| directTable.pages.size()
					!= profile->second.pages.size())
			{
				return false;
			}
			UInt16 expectedPage = 0;
			for (size_t pageSlot = 0;
				pageSlot < profile->second.pages.size(); ++pageSlot)
			{
				const UInt16 pageIndex =
					profile->second.pages[pageSlot];
				if (pageIndex != expectedPage++)
					return false;
				AtlasCacheKey pageKey = roleKey;
				pageKey.pageIndex = pageIndex;
				const auto page = state.atlasCache.find(pageKey);
				if (page == state.atlasCache.end()
					|| !page->second.resource)
				{
					return false;
				}
				physicalSources.push_back(page->second.resource);
				std::shared_ptr<AtlasResource> view =
					CreateRoleFilteredAtlasView(
						*page->second.resource,
						directTable,
						static_cast<UInt16>(pageSlot));
				if (!view)
					return false;
				destination.push_back({
					pageKey, std::move(view) });
			}
			return true;
		}
	}

	bool SaveGlyphAtlasSnapshotRole(RuntimeFont& runtime,
		VectorFontByteClass byteClass, float rasterScale,
		bool* jointRolePublished,
		const PhysicalAtlasGroup* physicalGroup,
		bool* physicalGroupFallback,
		PhysicalAtlasGroupPreview* physicalGroupPreview)
	{
		if (jointRolePublished)
			*jointRolePublished = false;
		if (physicalGroupFallback)
			*physicalGroupFallback = false;
		if (physicalGroupPreview)
			*physicalGroupPreview = {};
		const FontConfig& config = GetRuntimeConfig(runtime);
		AtlasCacheKey key;
		if (!ResolvePrewarmAtlasKey(config, byteClass, rasterScale, key))
			return false;
		UInt64 snapshotHash = 0;
		UInt64 maskContentHash = 0;
		if (GetAtlasSnapshotPath(runtime, key, snapshotHash, maskContentHash).empty())
			return false;
		std::vector<SnapshotPageData> pages;
		UInt64 totalBytes = 0;
		UInt64 totalPlacements = 0;
		UInt64 originalGpuBytes = 0;
		UInt64 snapshotGpuBytes = 0;
		UInt64 physicalGroupSourceGpuBytes = 0;
		UInt64 physicalAliasSavedBytes = 0;
		UInt32 sourcePageCount = 0;
		UInt32 jointDoubleByteSourcePageCount = 0;
		UInt32 physicalAliasFiles = 0;
		bool jointlyPackedRoles = false;
		const bool jointlyPackedFontGroup =
			physicalGroup && byteClass == VectorFontByteClass::SingleByte;
		struct PhysicalGroupRoleSource
		{
			AtlasCacheKey key;
			std::shared_ptr<const DirectAtlasGlyphTable> table;
		};
		std::vector<PhysicalGroupRoleSource> physicalGroupRoleSources;
		if (jointlyPackedFontGroup)
		{
			if (!physicalGroup || physicalGroup->members.empty())
				return false;
			std::unordered_set<const DirectAtlasGlyphTable*> uniqueTables;
			physicalGroupRoleSources.reserve(
				physicalGroup->members.size() * 2u);
			for (const PhysicalAtlasGroupMember& member : physicalGroup->members)
			{
				RuntimeFont* memberRuntime =
					EnsureRuntimeFont(member.config->fontId);
				const std::shared_ptr<const SealedDirectFontProfile> sealed =
					memberRuntime
						? LoadRuntimeSealedDirectProfile(*memberRuntime) : nullptr;
				if (!memberRuntime || !sealed
					|| !IsSealedDirectFontProfileUsable(
						*memberRuntime, sealed, rasterScale))
				{
					return false;
				}
				for (size_t roleIndex = 0; roleIndex < 2; ++roleIndex)
				{
					const VectorFontByteClass role =
						static_cast<VectorFontByteClass>(roleIndex);
					const AtlasCacheKey& roleKey = role
						== VectorFontByteClass::SingleByte
						? member.singleByteKey : member.doubleByteKey;
					const auto& table = sealed->tables[roleIndex];
					if (!table || !table->validity
						|| !table->validity->load(std::memory_order_acquire)
						|| table->byteClass != role
						|| table->atlasIdentity != roleKey.atlasContentHash
						|| table->layoutIdentity
							!= GetRuntimeDirectRoleLayoutIdentity(
								*memberRuntime, role)
						|| table->pages.empty()
						|| table->pages.size() > kMaximumAtlasSnapshotPages)
					{
						return false;
					}
					if (uniqueTables.insert(table.get()).second)
					{
						physicalGroupRoleSources.push_back(
							{ roleKey, table });
					}
				}
			}
			if (physicalGroupRoleSources.empty())
				return false;
		}
		VectorFontByteClass packingByteClass = byteClass;
		AtlasCacheKey singleByteKey;
		AtlasCacheKey doubleByteKey;
		const bool canJointlyPackRoles = g_bEnableFreeTypeDefaultPoolAtlas
			&& IsDbcsCodePage(GetFreeTypeTextCodePage())
			&& !IsPrewarmAtlasAlias(config, VectorFontByteClass::DoubleByte)
			&& ResolvePrewarmAtlasKey(config, VectorFontByteClass::SingleByte,
				rasterScale, singleByteKey)
			&& ResolvePrewarmAtlasKey(config, VectorFontByteClass::DoubleByte,
				rasterScale, doubleByteKey)
			&& singleByteKey.scaleMilli == doubleByteKey.scaleMilli
			&& singleByteKey.pixelMode == doubleByteKey.pixelMode
			&& singleByteKey.renderMode == doubleByteKey.renderMode
			&& singleByteKey.padding == doubleByteKey.padding
			&& singleByteKey.levelZeroOnly == doubleByteKey.levelZeroOnly
			&& UsesPlacedLevelZeroSnapshot(singleByteKey)
			&& UsesPlacedLevelZeroSnapshot(doubleByteKey);
		{
			std::lock_guard<std::mutex> lock(State().atlasMutex);
			std::vector<std::pair<AtlasCacheKey, std::shared_ptr<AtlasResource>>> resources;
			auto collectRole = [&](const AtlasCacheKey& roleKey,
				std::vector<std::pair<AtlasCacheKey,
					std::shared_ptr<AtlasResource>>>& destination,
				UInt32* rolePageCount = nullptr)
			{
				const auto profile = State().atlasProfiles.find(
					MakeAtlasProfileKey(roleKey));
				if (profile == State().atlasProfiles.end()
					|| profile->second.pages.empty())
					return false;
				if (rolePageCount)
					*rolePageCount = static_cast<UInt32>(
						profile->second.pages.size());
				UInt16 expectedPage = 0;
				for (UInt16 pageIndex : profile->second.pages)
				{
					if (pageIndex != expectedPage++)
						return false;
					AtlasCacheKey pageKey = roleKey;
					pageKey.pageIndex = pageIndex;
					const auto page = State().atlasCache.find(pageKey);
					if (page == State().atlasCache.end()
						|| !page->second.resource)
						return false;
					destination.push_back({ pageKey, page->second.resource });
				}
				return true;
			};

			if (!collectRole(key, resources, &sourcePageCount))
				return false;
			if (jointlyPackedFontGroup)
			{
				std::vector<std::pair<AtlasCacheKey,
					std::shared_ptr<AtlasResource>>> groupResources;
				std::vector<std::shared_ptr<AtlasResource>>
					groupPhysicalSources;
				bool collected = true;
				for (const PhysicalGroupRoleSource& source :
					physicalGroupRoleSources)
				{
					collected = collected
						&& CollectRoleFilteredResourcesLocked(
							source.key, *source.table, groupResources,
							groupPhysicalSources);
				}
				if (collected)
				{
					std::unordered_set<UInt64> uniqueCacheIds;
					std::vector<std::pair<AtlasCacheKey,
						std::shared_ptr<AtlasResource>>> deduplicated;
					deduplicated.reserve(groupResources.size());
					for (auto& item : groupResources)
					{
						auto& glyphs = item.second->glyphs;
						glyphs.erase(std::remove_if(glyphs.begin(),
							glyphs.end(),
							[&](const AtlasGlyphRecord& glyph)
							{
								return !uniqueCacheIds.insert(
									glyph.cacheId).second;
							}), glyphs.end());
						if (!glyphs.empty())
							deduplicated.push_back(std::move(item));
					}
					groupResources.swap(deduplicated);
				}
				if (collected)
				{
					std::vector<std::shared_ptr<AtlasResource>>
						uniquePhysicalSources;
					uniquePhysicalSources.reserve(
						groupPhysicalSources.size());
					for (const auto& source : groupPhysicalSources)
					{
						if (!source)
						{
							collected = false;
							break;
						}
						const bool duplicate = std::any_of(
							uniquePhysicalSources.begin(),
							uniquePhysicalSources.end(),
							[&](const auto& existing)
							{
								return existing.get() == source.get()
									|| AreAtlasResourcesBackedBySameTexture(
										*existing, *source);
							});
						if (duplicate)
							continue;
						const UInt64 bytes = GetAtlasStorageBytes(
							source->width, source->height,
							source->pixelMode, source->mipLevels);
						if (bytes > std::numeric_limits<UInt64>::max()
								- physicalGroupSourceGpuBytes)
						{
							collected = false;
							break;
						}
						physicalGroupSourceGpuBytes += bytes;
						uniquePhysicalSources.push_back(source);
					}
					collected = collected
						&& physicalGroupSourceGpuBytes != 0;
				}
				const size_t maximumSourcePages =
					static_cast<size_t>(kMaximumAtlasSnapshotPages)
						* physicalGroupRoleSources.size();
				if (!collected || groupResources.empty()
					|| groupResources.size() > maximumSourcePages)
				{
					return false;
				}
				resources.swap(groupResources);
				jointlyPackedRoles = true;
				packingByteClass = VectorFontByteClass::DoubleByte;
			}
			else if (canJointlyPackRoles)
			{
				std::vector<std::pair<AtlasCacheKey,
					std::shared_ptr<AtlasResource>>> jointResources;
				jointResources.reserve(resources.size() + 8u);
				if (collectRole(singleByteKey, jointResources)
					&& collectRole(doubleByteKey, jointResources,
						&jointDoubleByteSourcePageCount)
					&& !jointResources.empty()
					&& jointResources.size()
						<= static_cast<size_t>(kMaximumAtlasSnapshotPages) * 2u)
				{
					resources.swap(jointResources);
					jointlyPackedRoles = true;
					packingByteClass = VectorFontByteClass::DoubleByte;
				}
			}
			if (resources.empty()
				|| (!jointlyPackedFontGroup
					&& resources.size()
						> static_cast<size_t>(kMaximumAtlasSnapshotPages) * 2u))
				return false;
			if (UsesPlacedLevelZeroSnapshot(key))
			{
				if (!BuildRepackedSnapshotPages(key, resources, pages,
					originalGpuBytes, packingByteClass,
					jointlyPackedFontGroup && physicalGroup->version
						== kPhysicalAtlasPoolVersion ? 1u : 0u,
					physicalGroupPreview == nullptr))
					return false;
			}
			else
			{
				for (size_t index = 0; index < resources.size(); ++index)
				{
					SnapshotPageData page;
					page.key = resources[index].first;
					AtlasResource& resource = *resources[index].second;
					originalGpuBytes += GetAtlasStorageBytes(resource.width,
						resource.height, resource.pixelMode, resource.mipLevels);
					page.header.width = resource.width;
					page.header.height = resource.height;
					page.header.cursorX = resource.cursorX;
					page.header.cursorY = resource.cursorY;
					page.header.shelfHeight = resource.shelfHeight;
					page.header.padding = resource.padding;
					page.placements.reserve(resource.glyphs.size());
					for (const AtlasGlyphRecord& record : resource.glyphs)
					{
						AtlasSnapshotPlacement placement;
						if (!MakeSnapshotPlacement(resource, record.cacheId,
							record.rect, placement))
						{
							return false;
						}
						page.placements.push_back(placement);
					}
					std::sort(page.placements.begin(), page.placements.end(),
						[](const AtlasSnapshotPlacement& lhs,
							const AtlasSnapshotPlacement& rhs)
						{
							return lhs.cacheId < rhs.cacheId;
						});
					if (!BuildAtlasSnapshotPixels(resource, page.placements,
						AtlasSnapshotStorage::FullMipChain, page.pixels))
					{
						return false;
					}
					pages.push_back(std::move(page));
				}
			}
		}
		if (pages.empty() || pages.size() > kMaximumAtlasSnapshotPages)
			return false;
		if (jointlyPackedFontGroup)
		{
			UInt64 candidateGpuBytes = 0;
			bool candidateBytesValid = physicalGroupSourceGpuBytes != 0;
			for (const SnapshotPageData& page : pages)
			{
				const UInt64 bytes = GetAtlasStorageBytes(
					page.header.width, page.header.height,
					key.pixelMode, GetAtlasMipLevelCount(
						page.header.width, page.header.height,
						key.levelZeroOnly));
				if (bytes > std::numeric_limits<UInt64>::max()
						- candidateGpuBytes)
				{
					candidateBytesValid = false;
					break;
				}
				candidateGpuBytes += bytes;
			}
			const bool multiplePages = pages.size() != 1;
			const bool gpuGrowth = !candidateBytesValid
				|| candidateGpuBytes > physicalGroupSourceGpuBytes;
			if (physicalGroupPreview)
			{
				physicalGroupPreview->evaluated = true;
				physicalGroupPreview->feasible =
					!multiplePages && !gpuGrowth;
				physicalGroupPreview->pageCount = static_cast<UInt32>(
					pages.size());
				physicalGroupPreview->sourceGpuBytes =
					physicalGroupSourceGpuBytes;
				physicalGroupPreview->candidateGpuBytes = candidateGpuBytes;
				for (const SnapshotPageData& page : pages)
				{
					physicalGroupPreview->placementCount +=
						page.placements.size();
				}
				if (!multiplePages)
				{
					physicalGroupPreview->width = pages.front().header.width;
					physicalGroupPreview->height = pages.front().header.height;
				}
				return true;
			}
			if (multiplePages || gpuGrowth)
			{
				const bool fallbackMarked = physicalGroup->version
						== kPhysicalAtlasGroupVersion
					&& MarkPhysicalAtlasGroupFallback(
						runtime, key, sourcePageCount);
				if (physicalGroupFallback)
					*physicalGroupFallback = fallbackMarked;
				gLog.FormattedMessage(
					"tnvse_freetype_font: physical atlas group fallback version=%u owner=%u members=%u uniqueSingleProfiles=%u doubleLayouts=%u logicalRoleSources=%u pages=%u sourceGpuBytes=%llu candidateGpuBytes=%llu reason=%s markerPersisted=%u policy=retain-per-font-atlases",
					physicalGroup->version,
					physicalGroup->ownerFontId,
					static_cast<UInt32>(
						physicalGroup->members.size()),
					static_cast<UInt32>(
						physicalGroup->uniqueSingleByteProfiles.size()),
					static_cast<UInt32>(physicalGroup
						->uniqueDoubleByteLayoutHashes.size()),
					static_cast<UInt32>(physicalGroupRoleSources.size()),
					static_cast<UInt32>(pages.size()),
					static_cast<unsigned long long>(
						physicalGroupSourceGpuBytes),
					static_cast<unsigned long long>(
						candidateGpuBytes),
					multiplePages ? "multi-page" : "gpu-growth",
					fallbackMarked ? 1u : 0u);
				return false;
			}
			originalGpuBytes = physicalGroupSourceGpuBytes;
		}
		const UInt32 pageCount = static_cast<UInt32>(pages.size());
		auto deleteTemporaryPages = [&]()
		{
			for (const SnapshotPageData& page : pages)
			{
				if (!page.path.empty())
					DeleteFileW((page.path + L".tmp").c_str());
			}
		};
		bool prepared = true;
		for (size_t index = 0; index < pages.size(); ++index)
		{
			SnapshotPageData& page = pages[index];
			page.key.pageIndex = static_cast<UInt16>(index);
			AtlasSnapshotStorage storageMode = UsesPlacedLevelZeroSnapshot(page.key)
				? AtlasSnapshotStorage::PlacedLevelZeroRects
				: AtlasSnapshotStorage::FullMipChain;
			const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'A', 'T', 'L', '9' };
			std::memcpy(page.header.magic, magic, sizeof(magic));
				page.header.version = kAtlasSnapshotVersion;
				page.header.headerSize = sizeof(page.header);
				page.header.snapshotHash = snapshotHash;
				page.header.maskContentHash = maskContentHash;
				page.header.atlasContentHash = page.key.atlasContentHash;
				page.header.flags = kAtlasSnapshotFlagGloballyRepacked;
				if (jointlyPackedRoles)
					page.header.flags |= kAtlasSnapshotFlagJointByteRoles;
				if (jointlyPackedFontGroup)
					page.header.flags |=
						kAtlasSnapshotFlagPhysicalFontGroup;
				if (jointlyPackedFontGroup && physicalGroup->version
					== kPhysicalAtlasPoolVersion)
				{
					page.header.flags |=
						kAtlasSnapshotFlagPhysicalFontPool;
				}
				if (g_bEnableFreeTypeDefaultPoolAtlas)
				{
					page.header.flags |= pages.size() == 1
						? kAtlasSnapshotFlagSingleAtlas
						: kAtlasSnapshotFlagSingleAtlasOverflow;
				}
				page.header.scaleMilli = page.key.scaleMilli;
				page.header.mipLevels = GetAtlasMipLevelCount(page.header.width,
					page.header.height, page.key.levelZeroOnly);
				page.header.pixelMode = static_cast<UInt8>(page.key.pixelMode);
				page.header.renderMode = static_cast<UInt8>(page.key.renderMode);
				page.header.byteClass = static_cast<UInt8>(page.key.byteClass);
				page.header.pageIndex = static_cast<UInt16>(index);
				page.header.pageCount = static_cast<UInt16>(pages.size());
				page.header.placementCount = static_cast<UInt32>(page.placements.size());
				size_t packedPixelBytes = page.pixels.size();
				if (!page.pixelSources.empty())
				{
					packedPixelBytes = 0;
					for (const SnapshotPixelSource& source :
						page.pixelSources)
					{
						if (source.destinationOffset != packedPixelBytes
							|| source.bytes
								> std::numeric_limits<size_t>::max()
									- packedPixelBytes)
						{
							prepared = false;
							break;
						}
						packedPixelBytes += source.bytes;
					}
				}
				if (!prepared)
					break;
				page.header.pixelBytes = packedPixelBytes;
				for (AtlasSnapshotPlacement& placement : page.placements)
				{
					if (!CacheAtlasSnapshotGlyphPlacement(placement,
						page.header.width, page.header.height, page.header.pageIndex))
					{
						prepared = false;
						break;
					}
				}
				if (!prepared)
					break;
				page.header.storageMode = static_cast<UInt8>(storageMode);
				page.header.storedPixelBytes = packedPixelBytes;
				page.header.payloadChecksum = 0;
				page.header.pageContentHash = 0;
				page.header.checksum = 0;

				UInt64 ignoredSnapshotHash = 0;
				UInt64 ignoredContentHash = 0;
				page.path = GetAtlasSnapshotPath(runtime, page.key,
					ignoredSnapshotHash, ignoredContentHash);
				if (page.path.empty() || ignoredSnapshotHash != snapshotHash
					|| ignoredContentHash != maskContentHash)
				{
					prepared = false;
					break;
				}
				const std::wstring temporary = page.path + L".tmp";
				HANDLE file = CreateFileW(temporary.c_str(),
					GENERIC_READ | GENERIC_WRITE, 0, nullptr,
					CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
				if (file == INVALID_HANDLE_VALUE)
				{
					prepared = false;
					break;
				}
				const bool sparse = TryEnableSparseFile(file);
				bool written = WriteSequentialFileBytes(file,
					&page.header, sizeof(page.header))
					&& WriteSequentialFileBytes(file, page.placements.data(),
						page.placements.size() * sizeof(page.placements[0]));
				if (written)
				{
					if (!page.pixelSources.empty())
					{
						written = WriteRepackedSnapshotPixels(file,
							page, page.header.payloadChecksum,
							page.header.pageContentHash);
					}
					else
					{
						UInt64 payloadHash = page.placements.empty()
							? 1469598103934665603ull
							: HashAtlasBytes(page.placements.data(),
								page.placements.size()
									* sizeof(page.placements[0]));
						page.header.payloadChecksum =
							HashAtlasBytes(page.pixels.data(),
								page.pixels.size(), payloadHash);
						page.header.pageContentHash =
							ComputeAtlasPageContentHash(page.header,
								page.placements, page.pixels);
						written = page.header.pageContentHash
							&& WriteSparseFileBytes(file,
								page.pixels.data(),
								page.pixels.size(), sparse);
					}
				}
				if (written)
				{
					page.header.checksum = HashAtlasBytes(
						&page.header,
						offsetof(AtlasSnapshotHeader, checksum));
					LARGE_INTEGER beginning = {};
					written = SetFilePointerEx(file, beginning,
						nullptr, FILE_BEGIN) != FALSE
						&& WriteSequentialFileBytes(file,
							&page.header, sizeof(page.header))
						&& FlushFileBuffers(file) != FALSE;
				}
				CloseHandle(file);
				if (!written || !page.header.payloadChecksum
					|| !page.header.pageContentHash)
				{
					prepared = false;
					break;
				}
				totalBytes += page.header.storedPixelBytes;
				totalPlacements += page.header.placementCount;
				snapshotGpuBytes += GetAtlasStorageBytes(page.header.width,
					page.header.height, page.key.pixelMode, page.header.mipLevels);
				std::vector<UInt8>().swap(page.pixels);
		}
		if (!prepared)
		{
			deleteTemporaryPages();
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(State().atlasMutex);
			// Committing any page changes the disk generation that the resident
			// CompactAtlasSnapshot objects describe. Invalidate before the first
			// replacement so a partial publication can never remain reusable.
			InvalidateCompleteAtlasProfileLocked(State(),
				MakeAtlasProfileKey(key));
		}
		for (const SnapshotPageData& page : pages)
		{
			const std::wstring temporary = page.path + L".tmp";
			if (!MoveFileExW(temporary.c_str(), page.path.c_str(),
				MOVEFILE_REPLACE_EXISTING))
			{
				deleteTemporaryPages();
				return false;
			}
		}
		for (UInt32 stalePage = pageCount; stalePage < sourcePageCount; ++stalePage)
		{
			AtlasCacheKey staleKey = key;
			staleKey.pageIndex = static_cast<UInt16>(stalePage);
			UInt64 ignoredSnapshotHash = 0;
			UInt64 ignoredContentHash = 0;
			const std::wstring stalePath = GetAtlasSnapshotPath(runtime, staleKey,
				ignoredSnapshotHash, ignoredContentHash);
			if (!stalePath.empty())
				DeleteFileW(stalePath.c_str());
		}
		if (jointlyPackedRoles
			&& byteClass == VectorFontByteClass::SingleByte)
		{
			struct SnapshotAliasTarget
			{
				RuntimeFont* runtime = nullptr;
				AtlasCacheKey key;
				UInt32 sourcePageCount = 0;
			};
			struct SnapshotAliasFile
			{
				std::wstring path;
				std::wstring temporary;
				AtlasProfileKey profile;
			};

			std::vector<SnapshotAliasTarget> targets;
			auto appendTarget = [&](RuntimeFont* targetRuntime,
				const AtlasCacheKey& targetKey, UInt32 knownPageCount = 0)
			{
				if (!targetRuntime)
					return false;
				SnapshotAliasTarget target;
				target.runtime = targetRuntime;
				target.key = targetKey;
				target.sourcePageCount = knownPageCount;
				targets.push_back(std::move(target));
				return true;
			};
			if (jointlyPackedFontGroup)
			{
				for (const PhysicalAtlasGroupMember& member :
					physicalGroup->members)
				{
					RuntimeFont* targetRuntime =
						EnsureRuntimeFont(member.config->fontId);
					if (!appendTarget(targetRuntime, member.singleByteKey)
						|| !appendTarget(targetRuntime, member.doubleByteKey))
					{
						return false;
					}
				}
			}
			else if (!appendTarget(&runtime, doubleByteKey,
				jointDoubleByteSourcePageCount))
			{
				return false;
			}

			{
				std::lock_guard<std::mutex> lock(State().atlasMutex);
				for (SnapshotAliasTarget& target : targets)
				{
					if (target.sourcePageCount)
						continue;
					const auto profile = State().atlasProfiles.find(
						MakeAtlasProfileKey(target.key));
					if (profile != State().atlasProfiles.end())
					{
						target.sourcePageCount = static_cast<UInt32>(
							profile->second.pages.size());
					}
				}
			}

			std::vector<SnapshotAliasFile> aliasFiles;
			aliasFiles.reserve(pages.size() * targets.size());
			std::unordered_set<std::wstring> preparedPaths;
			bool aliasesPrepared = true;
			for (const SnapshotAliasTarget& target : targets)
			{
				for (size_t index = 0; index < pages.size(); ++index)
				{
					AtlasCacheKey aliasKey = target.key;
					aliasKey.pageIndex = static_cast<UInt16>(index);
					UInt64 aliasSnapshotHash = 0;
					UInt64 aliasMaskContentHash = 0;
					const std::wstring aliasPath = GetAtlasSnapshotPath(
						*target.runtime, aliasKey, aliasSnapshotHash,
						aliasMaskContentHash);
					if (aliasPath.empty())
					{
						aliasesPrepared = false;
						break;
					}
					if (aliasPath == pages[index].path
						|| !preparedPaths.insert(aliasPath).second)
					{
						continue;
					}
					const std::wstring temporary = aliasPath + L".tmp";
					HANDLE file = CreateFileW(temporary.c_str(),
						GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
						FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
						nullptr);
					AtlasSnapshotHeader header = pages[index].header;
					header.snapshotHash = aliasSnapshotHash;
					header.maskContentHash = aliasMaskContentHash;
					header.atlasContentHash = aliasKey.atlasContentHash;
					header.byteClass = static_cast<UInt8>(
						aliasKey.byteClass);
					header.flags |=
						kAtlasSnapshotFlagPhysicalPayloadAlias;
					header.checksum = HashAtlasBytes(&header,
						offsetof(AtlasSnapshotHeader, checksum));

					AtlasSnapshotAliasRecord alias = {};
					const UInt8 aliasMagic[8] =
						{ 'T', 'N', 'V', 'F', 'A', 'L', 'I', '1' };
					std::memcpy(alias.magic, aliasMagic,
						sizeof(aliasMagic));
					alias.version = kAtlasSnapshotAliasVersion;
					alias.recordSize = sizeof(alias);
					alias.physicalSnapshotHash =
						pages[index].header.snapshotHash;
					alias.physicalPixelBytes =
						pages[index].header.pixelBytes;
					alias.physicalPayloadChecksum =
						pages[index].header.payloadChecksum;
					alias.physicalPageContentHash =
						pages[index].header.pageContentHash;
					alias.physicalStoredPixelBytes =
						pages[index].header.storedPixelBytes;
					alias.physicalPlacementCount =
						pages[index].header.placementCount;
					alias.physicalPageIndex =
						pages[index].header.pageIndex;
					alias.checksum = HashAtlasBytes(&alias,
						offsetof(AtlasSnapshotAliasRecord, checksum));
					const bool patched = file != INVALID_HANDLE_VALUE
						&& WriteSequentialFileBytes(file, &header,
							sizeof(header))
						&& WriteSequentialFileBytes(file, &alias,
							sizeof(alias))
						&& FlushFileBuffers(file) != FALSE;
					if (file != INVALID_HANDLE_VALUE)
						CloseHandle(file);
					if (!patched)
					{
						DeleteFileW(temporary.c_str());
						aliasesPrepared = false;
						break;
					}
					aliasFiles.push_back({ aliasPath, temporary,
						MakeAtlasProfileKey(aliasKey) });
					const UInt64 fullFileBytes = sizeof(AtlasSnapshotHeader)
						+ static_cast<UInt64>(
							pages[index].header.placementCount)
							* sizeof(AtlasSnapshotPlacement)
						+ pages[index].header.storedPixelBytes;
					const UInt64 aliasFileBytes =
						sizeof(AtlasSnapshotHeader)
							+ sizeof(AtlasSnapshotAliasRecord);
					if (fullFileBytes > aliasFileBytes)
						physicalAliasSavedBytes +=
							fullFileBytes - aliasFileBytes;
					++physicalAliasFiles;
				}
				if (!aliasesPrepared)
					break;
			}
			if (!aliasesPrepared)
			{
				for (const SnapshotAliasFile& file : aliasFiles)
					DeleteFileW(file.temporary.c_str());
				return false;
			}
			{
				std::lock_guard<std::mutex> lock(State().atlasMutex);
				// One raster profile can now have several member-local direct
				// tables. The mutable profile index owns only the most recently
				// published table, so invalidate every captured table before the
				// old texture generation is retired.
				if (jointlyPackedFontGroup)
				{
					for (const PhysicalGroupRoleSource& source :
						physicalGroupRoleSources)
					{
						if (source.table && source.table->validity)
						{
							source.table->validity->store(
								false, std::memory_order_release);
						}
					}
				}
				std::unordered_set<AtlasProfileKey, AtlasProfileKeyHash>
					invalidated;
				for (const SnapshotAliasFile& file : aliasFiles)
				{
					if (invalidated.insert(file.profile).second)
					{
						InvalidateCompleteAtlasProfileLocked(
							State(), file.profile);
					}
				}
			}
			for (const SnapshotAliasFile& file : aliasFiles)
			{
				if (!MoveFileExW(file.temporary.c_str(), file.path.c_str(),
					MOVEFILE_REPLACE_EXISTING))
				{
					for (const SnapshotAliasFile& cleanup : aliasFiles)
						DeleteFileW(cleanup.temporary.c_str());
					return false;
				}
			}

			std::unordered_set<std::wstring> stalePaths;
			for (const SnapshotAliasTarget& target : targets)
			{
				for (UInt32 stalePage = pageCount;
					stalePage < target.sourcePageCount; ++stalePage)
				{
					AtlasCacheKey staleKey = target.key;
					staleKey.pageIndex = static_cast<UInt16>(stalePage);
					UInt64 ignoredSnapshotHash = 0;
					UInt64 ignoredContentHash = 0;
					const std::wstring stalePath = GetAtlasSnapshotPath(
						*target.runtime, staleKey, ignoredSnapshotHash,
						ignoredContentHash);
					if (!stalePath.empty()
						&& stalePaths.insert(stalePath).second)
					{
						DeleteFileW(stalePath.c_str());
					}
				}
			}
			if (jointRolePublished)
				*jointRolePublished = true;
		}
		gLog.FormattedMessage(
			"tnvse_freetype_font: atlas snapshot saved font=%u role=%s pages=%u->%u placements=%llu rawBytes=%llu gpuBytes=%llu->%llu saved=%llu tail=%ux%u jointRoles=%u physicalAliasFiles=%u diskBytesSaved=%llu",
			key.fontId, key.byteClass == VectorFontByteClass::DoubleByte
				? "doubleByte" : "singleByte", sourcePageCount, pageCount,
			static_cast<unsigned long long>(totalPlacements),
			static_cast<unsigned long long>(totalBytes),
			static_cast<unsigned long long>(originalGpuBytes),
			static_cast<unsigned long long>(snapshotGpuBytes),
			static_cast<unsigned long long>(originalGpuBytes > snapshotGpuBytes
				? originalGpuBytes - snapshotGpuBytes : 0),
			pages.back().header.width, pages.back().header.height,
			jointlyPackedRoles ? 1u : 0u,
			physicalAliasFiles,
			static_cast<unsigned long long>(physicalAliasSavedBytes));
		if (jointlyPackedFontGroup)
		{
			gLog.FormattedMessage(
				physicalGroup->version == kPhysicalAtlasPoolVersion
					? "tnvse_freetype_font: physical atlas pool snapshot published version=%u owner=%u members=%u uniqueSingleProfiles=%u uniqueDoubleProfiles=%u doubleLayouts=%u logicalRoleSources=%u pageContentHash=%016llX size=%ux%u placements=%llu"
					: "tnvse_freetype_font: physical atlas group snapshot published version=%u owner=%u members=%u uniqueSingleProfiles=%u uniqueDoubleProfiles=%u doubleLayouts=%u logicalRoleSources=%u pageContentHash=%016llX size=%ux%u placements=%llu",
				physicalGroup->version,
				physicalGroup->ownerFontId,
				static_cast<UInt32>(physicalGroup->members.size()),
				static_cast<UInt32>(
					physicalGroup->uniqueSingleByteProfiles.size()),
				static_cast<UInt32>(
					physicalGroup->uniqueDoubleByteProfiles.size()),
				static_cast<UInt32>(physicalGroup
					->uniqueDoubleByteLayoutHashes.size()),
				static_cast<UInt32>(physicalGroupRoleSources.size()),
				static_cast<unsigned long long>(
					pages.front().header.pageContentHash),
				pages.front().header.width, pages.front().header.height,
				static_cast<unsigned long long>(totalPlacements));
		}
		return true;
	}
}

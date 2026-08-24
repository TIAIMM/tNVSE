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
		class ScopedSnapshotFile
		{
		public:
			explicit ScopedSnapshotFile(HANDLE file = INVALID_HANDLE_VALUE)
				: m_file(file)
			{
			}
			~ScopedSnapshotFile()
			{
				Close();
			}
			ScopedSnapshotFile(const ScopedSnapshotFile&) = delete;
			ScopedSnapshotFile& operator=(const ScopedSnapshotFile&) = delete;
			operator HANDLE() const { return m_file; }
			bool Valid() const { return m_file != INVALID_HANDLE_VALUE; }
			void Close()
			{
				if (m_file != INVALID_HANDLE_VALUE)
				{
					CloseHandle(m_file);
					m_file = INVALID_HANDLE_VALUE;
				}
			}

		private:
			HANDLE m_file = INVALID_HANDLE_VALUE;
		};

		bool MarkPhysicalAtlasGroupFallback(RuntimeFont& runtime,
			const AtlasCacheKey& baseKey, UInt32 pageCount)
		{
			if (!pageCount || pageCount > kMaximumAtlasSnapshotPages)
				return false;
			const UInt8 magic[8] =
				{ 'T', 'N', 'V', 'F', 'A', 'T', 'L', '9' };
			struct FallbackHeaderUpdate
			{
				std::wstring path;
				AtlasSnapshotHeader original = {};
				AtlasSnapshotHeader updated = {};
				bool changed = false;
			};
			std::vector<FallbackHeaderUpdate> updates;
			updates.reserve(pageCount);
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
				ScopedSnapshotFile file(CreateFileW(path.c_str(), GENERIC_READ,
					FILE_SHARE_READ | FILE_SHARE_WRITE
						| FILE_SHARE_DELETE,
					nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
				if (!file.Valid())
					return false;
				FallbackHeaderUpdate update;
				update.path = path;
				bool valid = ReadSnapshotBytesExact(
					file, &update.original, sizeof(update.original))
					&& std::memcmp(update.original.magic, magic,
						sizeof(magic)) == 0
					&& update.original.version == kAtlasSnapshotVersion
					&& update.original.headerSize == sizeof(update.original)
					&& update.original.snapshotHash == snapshotHash
					&& update.original.maskContentHash == maskContentHash
					&& update.original.atlasContentHash
						== pageKey.atlasContentHash
					&& update.original.pageIndex == pageIndex
					&& update.original.pageCount == pageCount
					&& !(update.original.flags & ~kAtlasSnapshotKnownFlags)
					&& update.original.checksum == HashAtlasBytes(&update.original,
						offsetof(AtlasSnapshotHeader, checksum));
				if (!valid)
					return false;
				update.updated = update.original;
				update.changed = !(update.updated.flags
					& kAtlasSnapshotFlagPhysicalFontGroupFallback);
				if (update.changed)
				{
					update.updated.flags |=
						kAtlasSnapshotFlagPhysicalFontGroupFallback;
					update.updated.checksum = HashAtlasBytes(&update.updated,
						offsetof(AtlasSnapshotHeader, checksum));
				}
				updates.push_back(std::move(update));
			}

			auto writeHeader = [](const std::wstring& path,
				const AtlasSnapshotHeader& header)
			{
				ScopedSnapshotFile file(CreateFileW(path.c_str(),
					GENERIC_READ | GENERIC_WRITE,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					nullptr, OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
				LARGE_INTEGER beginning = {};
				return file.Valid()
					&& SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN) != FALSE
					&& WriteSequentialFileBytes(file, &header, sizeof(header))
					&& FlushFileBuffers(file) != FALSE;
			};
			size_t applied = 0;
			for (; applied < updates.size(); ++applied)
			{
				const FallbackHeaderUpdate& update = updates[applied];
				if (!update.changed)
					continue;
				if (writeHeader(update.path, update.updated))
					continue;
				bool rolledBack = true;
				for (size_t rollback = 0; rollback <= applied; ++rollback)
				{
					const FallbackHeaderUpdate& prior = updates[rollback];
					if (prior.changed
						&& !writeHeader(prior.path, prior.original))
					{
						rolledBack = false;
					}
				}
				gLog.FormattedMessage(
					"tnvse_freetype_font: physical atlas group fallback marker write failed page=%u rollback=%u policy=retain-source-generation",
					static_cast<UInt32>(applied), rolledBack ? 1u : 0u);
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

		bool CollectRoleFilteredResources(
			const AtlasCacheKey& roleKey,
			const DirectAtlasGlyphTable& directTable,
			std::vector<std::pair<AtlasCacheKey,
				std::shared_ptr<AtlasResource>>>& destination,
			std::vector<std::shared_ptr<AtlasResource>>& physicalSources)
		{
			if (!directTable.validity
				|| !directTable.validity->load(std::memory_order_acquire)
				|| directTable.pages.empty()
				|| directTable.pages.size() > kMaximumAtlasSnapshotPages)
			{
				return false;
			}
			for (size_t pageSlot = 0;
				pageSlot < directTable.pages.size(); ++pageSlot)
			{
				AtlasCacheKey pageKey = roleKey;
				pageKey.pageIndex = static_cast<UInt16>(pageSlot);
				std::shared_ptr<AtlasResource> page =
					directTable.pages[pageSlot].lock();
				if (!page || !page->compactSnapshot)
				{
					return false;
				}
				physicalSources.push_back(page);
				std::shared_ptr<AtlasResource> view =
					CreateRoleFilteredAtlasView(
						*page,
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
		PhysicalAtlasGroupPreview* physicalGroupPreview,
		SnapshotSaveDiagnostics* saveDiagnostics,
		const PhysicalAtlasGroupSourceSnapshot* physicalGroupSources)
	{
		if (saveDiagnostics)
		{
			*saveDiagnostics = {};
			saveDiagnostics->stage = "initialize";
		}
		auto fail = [&](const char* stage, const char* reason,
			DWORD win32Error = ERROR_SUCCESS, size_t detailIndex = 0)
		{
			if (saveDiagnostics)
			{
				saveDiagnostics->stage = stage;
				saveDiagnostics->reason = reason;
				saveDiagnostics->win32Error = win32Error;
				saveDiagnostics->detailIndex = static_cast<UInt32>(
					std::min(detailIndex,
						static_cast<size_t>(std::numeric_limits<UInt32>::max())));
			}
			return false;
		};
		if (jointRolePublished)
			*jointRolePublished = false;
		if (physicalGroupFallback)
			*physicalGroupFallback = false;
		if (physicalGroupPreview)
			*physicalGroupPreview = {};
		const FontConfig& config = GetRuntimeConfig(runtime);
		AtlasCacheKey key;
		if (!ResolvePrewarmAtlasKey(config, byteClass, rasterScale, key))
			return fail("initialize", "resolve-prewarm-key");
		UInt64 snapshotHash = 0;
		UInt64 maskContentHash = 0;
		if (GetAtlasSnapshotPath(runtime, key, snapshotHash, maskContentHash).empty())
			return fail("initialize", "snapshot-path-empty");
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
				return fail("collect-role-sources", "physical-group-empty");
			std::unordered_set<const DirectAtlasGlyphTable*> uniqueTables;
			physicalGroupRoleSources.reserve(
				physicalGroup->members.size() * 2u);
			if (physicalGroupSources
				&& physicalGroupSources->members.size()
					!= physicalGroup->members.size())
			{
				return fail("collect-role-sources",
					"captured-source-count-mismatch");
			}
			for (size_t memberIndex = 0;
				memberIndex < physicalGroup->members.size(); ++memberIndex)
			{
				const PhysicalAtlasGroupMember& member =
					physicalGroup->members[memberIndex];
				RuntimeFont* memberRuntime = member.config
					? EnsureRuntimeFont(member.config->fontId) : nullptr;
				std::shared_ptr<const SealedDirectFontProfile> sealed;
				if (physicalGroupSources)
				{
					const PhysicalAtlasGroupMemberSource& source =
						physicalGroupSources->members[memberIndex];
					if (!member.config || source.fontId != member.config->fontId)
					{
						return fail("collect-role-sources",
							"captured-source-font-mismatch", ERROR_SUCCESS,
							memberIndex);
					}
					sealed = source.sealedProfile;
				}
				else if (memberRuntime)
				{
					sealed = LoadRuntimeSealedDirectProfile(*memberRuntime);
				}
				if (!memberRuntime || !sealed
					|| !IsSealedDirectFontProfileUsable(
						*memberRuntime, sealed, rasterScale))
				{
					return fail("collect-role-sources",
						"member-sealed-profile-unusable", ERROR_SUCCESS,
						memberIndex);
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
						return fail("collect-role-sources",
							"member-direct-table-invalid", ERROR_SUCCESS,
							physicalGroupRoleSources.size());
					}
					if (uniqueTables.insert(table.get()).second)
					{
						physicalGroupRoleSources.push_back(
							{ roleKey, table });
					}
					if (member.config
						&& member.config->fontId == config.fontId
						&& role == byteClass)
					{
						sourcePageCount = static_cast<UInt32>(
							table->pages.size());
					}
				}
			}
			if (physicalGroupRoleSources.empty())
				return fail("collect-role-sources", "no-unique-role-sources");
			if (saveDiagnostics)
				saveDiagnostics->roleSourceCount = static_cast<UInt32>(
					physicalGroupRoleSources.size());
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
		std::vector<std::pair<AtlasCacheKey,
			std::shared_ptr<AtlasResource>>> resources;
		std::vector<std::pair<AtlasCacheKey,
			std::shared_ptr<AtlasResource>>> baseRoleResources;
		{
			std::lock_guard<std::mutex> lock(State().atlasMutex);
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

			if (!jointlyPackedFontGroup
				&& !collectRole(key, resources, &sourcePageCount))
				return fail("collect-resources", "base-role-profile-unavailable");
			if (jointlyPackedFontGroup && !sourcePageCount)
				return fail("collect-resources", "captured-owner-role-empty");
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
						&& CollectRoleFilteredResources(
							source.key, *source.table, groupResources,
							groupPhysicalSources);
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
					return fail("collect-resources",
						!collected ? "physical-role-resource-collection"
							: groupResources.empty()
								? "no-physical-role-resources"
								: "physical-role-resource-limit");
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
					baseRoleResources = resources;
					resources.swap(jointResources);
					jointlyPackedRoles = true;
					packingByteClass = VectorFontByteClass::DoubleByte;
				}
			}
			if (resources.empty()
				|| (!jointlyPackedFontGroup
					&& resources.size()
						> static_cast<size_t>(kMaximumAtlasSnapshotPages) * 2u))
				return fail("collect-resources",
					resources.empty() ? "no-resources" : "resource-limit");
		}
		if (saveDiagnostics)
		{
			saveDiagnostics->resourceCount = static_cast<UInt32>(resources.size());
			saveDiagnostics->sourceGpuBytes = physicalGroupSourceGpuBytes;
			saveDiagnostics->stage = "repack";
		}
		// Resources are sealed and held by shared_ptr. Planning and pixel I/O can be
		// expensive, so never hold atlasMutex while evaluating an 8192-capable
		// physical layout or streaming its source snapshots.
		if (UsesPlacedLevelZeroSnapshot(key))
		{
			SnapshotSaveDiagnostics localRepackDiagnostics;
			SnapshotSaveDiagnostics* repackDiagnostics = saveDiagnostics
				? saveDiagnostics : &localRepackDiagnostics;
			bool repacked = BuildRepackedSnapshotPages(key, resources, pages,
				originalGpuBytes, packingByteClass,
				jointlyPackedFontGroup && physicalGroup->version
					== kPhysicalAtlasPoolVersion ? 1u : 0u,
				physicalGroupPreview == nullptr, repackDiagnostics);
			if (!repacked && jointlyPackedRoles && !jointlyPackedFontGroup
				&& !baseRoleResources.empty())
			{
				const char* jointFailure = repackDiagnostics->reason;
				const UInt32 jointDetail = repackDiagnostics->detailIndex;
				gLog.FormattedMessage(
					"tnvse_freetype_font: joint byte-role repack fallback font=%u role=%s reason=%s detailIndex=%u policy=save-roles-separately",
					config.fontId,
					byteClass == VectorFontByteClass::DoubleByte
						? "doubleByte" : "singleByte",
					jointFailure, jointDetail);
				resources.swap(baseRoleResources);
				pages.clear();
				originalGpuBytes = 0;
				jointlyPackedRoles = false;
				packingByteClass = byteClass;
				jointDoubleByteSourcePageCount = 0;
				if (saveDiagnostics)
				{
					*saveDiagnostics = {};
					saveDiagnostics->stage = "repack-role-fallback";
					saveDiagnostics->resourceCount =
						static_cast<UInt32>(resources.size());
				}
				else
				{
					localRepackDiagnostics = {};
					localRepackDiagnostics.stage = "repack-role-fallback";
					localRepackDiagnostics.resourceCount =
						static_cast<UInt32>(resources.size());
				}
				repacked = BuildRepackedSnapshotPages(key, resources, pages,
					originalGpuBytes, packingByteClass, 0,
					true, repackDiagnostics);
			}
			if (!repacked && jointlyPackedFontGroup
				&& !physicalGroupPreview && physicalGroup
				&& physicalGroup->version == kPhysicalAtlasGroupVersion)
			{
				const char* groupFailure = repackDiagnostics->reason;
				const UInt32 groupDetail = repackDiagnostics->detailIndex;
				const bool fallbackMarked = MarkPhysicalAtlasGroupFallback(
					runtime, key, sourcePageCount);
				if (physicalGroupFallback)
					*physicalGroupFallback = true;
				gLog.FormattedMessage(
					"tnvse_freetype_font: physical atlas group fallback version=%u owner=%u members=%u reason=repack-%s detailIndex=%u markerPersisted=%u policy=retain-per-font-atlases",
					physicalGroup->version, physicalGroup->ownerFontId,
					static_cast<UInt32>(physicalGroup->members.size()),
					groupFailure, groupDetail,
					fallbackMarked ? 1u : 0u);
				return false;
			}
			if (!repacked)
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
						return fail("copy-pages", "snapshot-placement",
							ERROR_SUCCESS, index);
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
					return fail("copy-pages", "snapshot-pixels",
						ERROR_SUCCESS, index);
				}
				pages.push_back(std::move(page));
			}
		}
		if (pages.empty() || pages.size() > kMaximumAtlasSnapshotPages)
			return fail("validate-pages",
				pages.empty() ? "no-pages" : "page-limit");
		if (saveDiagnostics)
			saveDiagnostics->pageCount = static_cast<UInt32>(pages.size());
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
					*physicalGroupFallback = physicalGroup->version
						== kPhysicalAtlasGroupVersion;
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
				return fail("validate-pages",
					multiplePages ? "physical-group-multi-page"
						: "physical-group-gpu-growth");
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
		const char* preparedStage = "prepare-page-files";
		const char* preparedReason = "none";
		DWORD preparedError = ERROR_SUCCESS;
		size_t preparedIndex = 0;
		auto failPreparation = [&](const char* reason, size_t index,
			DWORD error = ERROR_SUCCESS)
		{
			prepared = false;
			preparedStage = "prepare-page-files";
			preparedReason = reason;
			preparedError = error;
			preparedIndex = index;
		};
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
							failPreparation("pixel-source-layout", index);
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
						failPreparation("placement-cache", index);
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
					failPreparation("snapshot-path-identity", index);
					break;
				}
				const std::wstring temporary = page.path + L".tmp";
				ScopedSnapshotFile file(CreateFileW(temporary.c_str(),
					GENERIC_READ | GENERIC_WRITE, 0, nullptr,
					CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
				if (!file.Valid())
				{
					failPreparation("page-temp-create", index,
						GetLastError());
					break;
				}
				const bool sparse = TryEnableSparseFile(file);
				bool written = WriteSequentialFileBytes(file,
					&page.header, sizeof(page.header));
				if (!written)
					failPreparation("page-header-write", index, GetLastError());
				if (written)
				{
					written = WriteSequentialFileBytes(file,
						page.placements.data(), page.placements.size()
							* sizeof(page.placements[0]));
					if (!written)
						failPreparation("page-placement-write", index,
							GetLastError());
				}
				if (written)
				{
					if (!page.pixelSources.empty())
					{
						written = WriteRepackedSnapshotPixels(file,
							page, page.header.payloadChecksum,
							page.header.pageContentHash, saveDiagnostics);
						if (!written)
						{
							prepared = false;
							preparedStage = saveDiagnostics
								? saveDiagnostics->stage
								: "write-repacked-pixels";
							preparedReason = saveDiagnostics
								? saveDiagnostics->reason
								: "repacked-pixel-write";
							preparedError = saveDiagnostics
								? saveDiagnostics->win32Error
								: GetLastError();
							preparedIndex = saveDiagnostics
								? saveDiagnostics->detailIndex : index;
						}
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
						written = page.header.pageContentHash != 0;
						if (!written)
							failPreparation("page-content-hash", index);
						if (written)
						{
							written = WriteSparseFileBytes(file,
								page.pixels.data(), page.pixels.size(), sparse);
							if (!written)
								failPreparation("page-pixel-write", index,
									GetLastError());
						}
					}
				}
				if (written)
				{
					page.header.checksum = HashAtlasBytes(
						&page.header,
						offsetof(AtlasSnapshotHeader, checksum));
					LARGE_INTEGER beginning = {};
					written = SetFilePointerEx(file, beginning,
						nullptr, FILE_BEGIN) != FALSE;
					if (!written)
						failPreparation("page-header-seek", index,
							GetLastError());
					if (written)
					{
						written = WriteSequentialFileBytes(file,
							&page.header, sizeof(page.header));
						if (!written)
							failPreparation("page-header-rewrite", index,
								GetLastError());
					}
					if (written)
					{
						written = FlushFileBuffers(file) != FALSE;
						if (!written)
							failPreparation("page-flush", index,
								GetLastError());
					}
				}
				file.Close();
				if (!written || !page.header.payloadChecksum
					|| !page.header.pageContentHash)
				{
					if (prepared)
					{
						failPreparation(!page.header.payloadChecksum
							? "payload-checksum-zero"
							: "page-content-hash-zero", index);
					}
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
			return fail(preparedStage, preparedReason,
				preparedError, preparedIndex);
		}
		{
			std::lock_guard<std::mutex> lock(State().atlasMutex);
			// Committing any page changes the disk generation that the resident
			// CompactAtlasSnapshot objects describe. Invalidate before the first
			// replacement so a partial publication can never remain reusable.
			InvalidateCompleteAtlasProfileLocked(State(),
				MakeAtlasProfileKey(key));
		}
		for (size_t pageIndex = 0; pageIndex < pages.size(); ++pageIndex)
		{
			const SnapshotPageData& page = pages[pageIndex];
			const std::wstring temporary = page.path + L".tmp";
			if (!MoveFileExW(temporary.c_str(), page.path.c_str(),
				MOVEFILE_REPLACE_EXISTING))
			{
				const DWORD error = GetLastError();
				deleteTemporaryPages();
				return fail("commit-page-files", "page-replace", error,
					pageIndex);
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
				for (size_t memberIndex = 0;
					memberIndex < physicalGroup->members.size(); ++memberIndex)
				{
					const PhysicalAtlasGroupMember& member =
						physicalGroup->members[memberIndex];
					RuntimeFont* targetRuntime =
						EnsureRuntimeFont(member.config->fontId);
					UInt32 singleBytePages = 0;
					UInt32 doubleBytePages = 0;
					if (physicalGroupSources
						&& memberIndex < physicalGroupSources->members.size())
					{
						const auto& sealed = physicalGroupSources
							->members[memberIndex].sealedProfile;
						if (sealed)
						{
							singleBytePages = sealed->tables[0]
								? static_cast<UInt32>(
									sealed->tables[0]->pages.size()) : 0;
							doubleBytePages = sealed->tables[1]
								? static_cast<UInt32>(
									sealed->tables[1]->pages.size()) : 0;
						}
					}
					if (!appendTarget(targetRuntime, member.singleByteKey,
							singleBytePages)
						|| !appendTarget(targetRuntime, member.doubleByteKey,
							doubleBytePages))
					{
						return fail("prepare-alias-files",
							"alias-target-runtime-missing", ERROR_SUCCESS,
							targets.size());
					}
				}
			}
			else if (!appendTarget(&runtime, doubleByteKey,
				jointDoubleByteSourcePageCount))
			{
				return fail("prepare-alias-files",
					"double-byte-alias-target-missing");
			}
			if (saveDiagnostics)
				saveDiagnostics->aliasTargetCount = static_cast<UInt32>(
					targets.size());

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
			const char* aliasFailureReason = "none";
			DWORD aliasFailureError = ERROR_SUCCESS;
			size_t aliasFailureIndex = 0;
			for (size_t targetIndex = 0; targetIndex < targets.size();
				++targetIndex)
			{
				const SnapshotAliasTarget& target = targets[targetIndex];
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
						aliasFailureReason = "alias-path-empty";
						aliasFailureIndex = targetIndex;
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
					bool patched = file != INVALID_HANDLE_VALUE;
					if (!patched)
					{
						aliasFailureReason = "alias-temp-create";
						aliasFailureError = GetLastError();
					}
					if (patched)
					{
						patched = WriteSequentialFileBytes(file, &header,
							sizeof(header));
						if (!patched)
						{
							aliasFailureReason = "alias-header-write";
							aliasFailureError = GetLastError();
						}
					}
					if (patched)
					{
						patched = WriteSequentialFileBytes(file, &alias,
							sizeof(alias));
						if (!patched)
						{
							aliasFailureReason = "alias-record-write";
							aliasFailureError = GetLastError();
						}
					}
					if (patched)
					{
						patched = FlushFileBuffers(file) != FALSE;
						if (!patched)
						{
							aliasFailureReason = "alias-flush";
							aliasFailureError = GetLastError();
						}
					}
					if (file != INVALID_HANDLE_VALUE)
						CloseHandle(file);
					if (!patched)
					{
						DeleteFileW(temporary.c_str());
						aliasesPrepared = false;
						aliasFailureIndex = targetIndex;
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
					if (saveDiagnostics)
						saveDiagnostics->aliasFileCount = physicalAliasFiles;
				}
				if (!aliasesPrepared)
					break;
			}
			if (!aliasesPrepared)
			{
				for (const SnapshotAliasFile& file : aliasFiles)
					DeleteFileW(file.temporary.c_str());
				return fail("prepare-alias-files", aliasFailureReason,
					aliasFailureError, aliasFailureIndex);
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
			for (size_t aliasIndex = 0; aliasIndex < aliasFiles.size();
				++aliasIndex)
			{
				const SnapshotAliasFile& file = aliasFiles[aliasIndex];
				if (!MoveFileExW(file.temporary.c_str(), file.path.c_str(),
					MOVEFILE_REPLACE_EXISTING))
				{
					const DWORD error = GetLastError();
					for (const SnapshotAliasFile& cleanup : aliasFiles)
						DeleteFileW(cleanup.temporary.c_str());
					return fail("commit-alias-files", "alias-replace", error,
						aliasIndex);
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
			"tnvse_freetype_font: atlas snapshot saved font=%u role=%s pages=%u->%u placements=%llu rawBytes=%llu gpuBytes=%llu->%llu saved=%llu tail=%ux%u jointRoles=%u physicalAliasFiles=%u diskBytesSaved=%llu repackIoKiB=%u",
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
			static_cast<unsigned long long>(physicalAliasSavedBytes), 1024u);
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
		if (saveDiagnostics)
		{
			saveDiagnostics->stage = "complete";
			saveDiagnostics->reason = "none";
			saveDiagnostics->win32Error = ERROR_SUCCESS;
			saveDiagnostics->pageCount = pageCount;
			saveDiagnostics->aliasFileCount = physicalAliasFiles;
			saveDiagnostics->placementCount = totalPlacements;
			saveDiagnostics->sourceGpuBytes = originalGpuBytes;
			saveDiagnostics->candidateGpuBytes = snapshotGpuBytes;
		}
		return true;
	}
}

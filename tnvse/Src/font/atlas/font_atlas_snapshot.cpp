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

	UInt64 BuildAtlasSnapshotIdentityHash(const AtlasCacheKey& key,
		UInt64 maskContentHash, const FontConfig& config)
	{
		return ComputeAtlasSnapshotIdentityHash(
			key, maskContentHash, config);
	}

	bool StageGlyphAtlasSnapshotMetadata(RuntimeFont& runtime, float rasterScale)
	{
		if (!LoadGlyphAtlasSnapshotRole(runtime,
			VectorFontByteClass::SingleByte, rasterScale, true))
		{
			return false;
		}
		return !IsDbcsCodePage(GetFreeTypeTextCodePage())
			|| IsPrewarmAtlasAlias(GetRuntimeConfig(runtime),
				VectorFontByteClass::DoubleByte)
			|| LoadGlyphAtlasSnapshotRole(runtime,
				VectorFontByteClass::DoubleByte, rasterScale, true);
	}

	bool TryLoadGlyphAtlasSnapshot(RuntimeFont& runtime, float rasterScale)
	{
		const FontConfig& config = GetRuntimeConfig(runtime);
		const bool needsDoubleByte = IsDbcsCodePage(GetFreeTypeTextCodePage());
		AtlasCacheKey singleByteKey;
		AtlasCacheKey doubleByteKey;
		if (!ResolvePrewarmAtlasKey(config, VectorFontByteClass::SingleByte,
			rasterScale, singleByteKey)
			|| (needsDoubleByte && !ResolvePrewarmAtlasKey(config,
				VectorFontByteClass::DoubleByte, rasterScale, doubleByteKey)))
		{
			return false;
		}
		const bool singleByteResident =
			TryReuseCompleteAtlasProfile(singleByteKey);
		const bool doubleByteResident = !needsDoubleByte
			|| TryReuseCompleteAtlasProfile(doubleByteKey);
		if (singleByteResident && doubleByteResident)
		{
			return true;
		}

		size_t incomingStorageBytes = 0;
		size_t roleStorageBytes = 0;
		if (!singleByteResident)
		{
			if (!InspectSnapshotRoleStorage(
					runtime, singleByteKey, roleStorageBytes))
				return false;
			incomingStorageBytes = roleStorageBytes;
		}
		if (needsDoubleByte && !doubleByteResident)
		{
			if (!InspectSnapshotRoleStorage(runtime, doubleByteKey, roleStorageBytes)
				|| roleStorageBytes > std::numeric_limits<size_t>::max()
					- incomingStorageBytes)
			{
				return false;
			}
			incomingStorageBytes += roleStorageBytes;
		}
		size_t cacheBefore = 0;
		size_t cacheAfter = 0;
		{
			// Reserve both byte roles as one transaction. Reserving roles
			// independently can evict the just-restored single-byte role when a
			// large double-byte profile approaches the configured soft budget.
			AtlasState& state = State();
			std::lock_guard<std::mutex> lock(state.atlasMutex);
			cacheBefore = state.atlasCacheBytes;
			TrimAtlasCacheForIncomingBytes(state, incomingStorageBytes);
			cacheAfter = state.atlasCacheBytes;
		}
		if (IsGpuAtlasCacheUnlimited())
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: atlas restore reservation font=%u incomingMiB=%.2f cacheMiB=%.2f->%.2f budget=unlimited",
				config.fontId, incomingStorageBytes / (1024.0 * 1024.0),
				cacheBefore / (1024.0 * 1024.0),
				cacheAfter / (1024.0 * 1024.0));
		}
		else
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: atlas restore reservation font=%u incomingMiB=%.2f cacheMiB=%.2f->%.2f budgetMiB=%.2f",
				config.fontId, incomingStorageBytes / (1024.0 * 1024.0),
				cacheBefore / (1024.0 * 1024.0),
				cacheAfter / (1024.0 * 1024.0),
				GetAtlasCacheLimit() / (1024.0 * 1024.0));
		}

		const bool singleByteReady = singleByteResident
			|| LoadGlyphAtlasSnapshotRole(runtime,
				VectorFontByteClass::SingleByte, rasterScale, false);
		const bool doubleByteReady = !needsDoubleByte
			|| doubleByteResident
			|| LoadGlyphAtlasSnapshotRole(runtime,
				VectorFontByteClass::DoubleByte, rasterScale, false);
		bool profilesResident = false;
		{
			AtlasState& state = State();
			std::lock_guard<std::mutex> lock(state.atlasMutex);
			profilesResident = IsCompleteAtlasProfileResidentLocked(state, singleByteKey)
				&& (!needsDoubleByte
					|| IsCompleteAtlasProfileResidentLocked(state, doubleByteKey));
		}
		return singleByteReady && doubleByteReady && profilesResident;
	}

	bool HasGloballyRepackedGlyphAtlasSnapshot(RuntimeFont& runtime,
		float rasterScale)
	{
		const FontConfig& config = GetRuntimeConfig(runtime);
		const size_t roleCount = IsDbcsCodePage(GetFreeTypeTextCodePage()) ? 2 : 1;
		AtlasState& state = State();
		std::lock_guard<std::mutex> lock(state.atlasMutex);
		for (size_t roleIndex = 0; roleIndex < roleCount; ++roleIndex)
		{
			AtlasCacheKey key;
			if (!ResolvePrewarmAtlasKey(config,
				static_cast<VectorFontByteClass>(roleIndex), rasterScale, key)
				|| !IsGloballyRepackedAtlasProfileResidentLocked(state, key))
			{
				return false;
			}
		}
		return true;
	}

	bool TryLoadGloballyRepackedGlyphAtlasSnapshotRole(RuntimeFont& runtime,
		VectorFontByteClass byteClass, float rasterScale)
	{
		AtlasCacheKey key;
		if (!ResolvePrewarmAtlasKey(GetRuntimeConfig(runtime), byteClass,
			rasterScale, key))
		{
			return false;
		}
		if (TryReuseCompleteAtlasProfile(key))
		{
			AtlasState& state = State();
			std::lock_guard<std::mutex> lock(state.atlasMutex);
			if (IsGloballyRepackedAtlasProfileResidentLocked(state, key))
				return true;
			// A complete resident profile can still belong to the pre-repack
			// generation. Force the role loader to replace it from the validated
			// globally repacked snapshot instead of accepting the reuse marker.
			InvalidateCompleteAtlasProfileLocked(state,
				MakeAtlasProfileKey(key));
		}

		size_t incomingStorageBytes = 0;
		if (!InspectSnapshotRoleStorage(runtime, key, incomingStorageBytes))
			return false;
		size_t cacheBefore = 0;
		size_t cacheAfter = 0;
		{
			AtlasState& state = State();
			std::lock_guard<std::mutex> lock(state.atlasMutex);
			cacheBefore = state.atlasCacheBytes;
			TrimAtlasCacheForIncomingBytes(state, incomingStorageBytes);
			cacheAfter = state.atlasCacheBytes;
		}
		if (IsGpuAtlasCacheUnlimited())
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: shared atlas role restore reservation font=%u role=%s incomingMiB=%.2f cacheMiB=%.2f->%.2f budget=unlimited",
				GetRuntimeConfig(runtime).fontId,
				byteClass == VectorFontByteClass::DoubleByte
					? "doubleByte" : "singleByte",
				incomingStorageBytes / (1024.0 * 1024.0),
				cacheBefore / (1024.0 * 1024.0),
				cacheAfter / (1024.0 * 1024.0));
		}
		else
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: shared atlas role restore reservation font=%u role=%s incomingMiB=%.2f cacheMiB=%.2f->%.2f budgetMiB=%.2f",
				GetRuntimeConfig(runtime).fontId,
				byteClass == VectorFontByteClass::DoubleByte
					? "doubleByte" : "singleByte",
				incomingStorageBytes / (1024.0 * 1024.0),
				cacheBefore / (1024.0 * 1024.0),
				cacheAfter / (1024.0 * 1024.0),
				GetAtlasCacheLimit() / (1024.0 * 1024.0));
		}
		if (!LoadGlyphAtlasSnapshotRole(runtime, byteClass, rasterScale, false))
			return false;
		AtlasState& state = State();
		std::lock_guard<std::mutex> lock(state.atlasMutex);
		return IsGloballyRepackedAtlasProfileResidentLocked(state, key);
	}

	bool TryLoadGloballyRepackedGlyphAtlasSnapshot(RuntimeFont& runtime,
		float rasterScale)
	{
		if (!TryLoadGlyphAtlasSnapshot(runtime, rasterScale))
			return false;
		if (HasGloballyRepackedGlyphAtlasSnapshot(runtime, rasterScale))
			return true;
		gLog.FormattedMessage(
			"tnvse_freetype_font: atlas snapshot rejected font=%u reason=not-globally-repacked",
			GetRuntimeConfig(runtime).fontId);
		return false;
	}

	bool EnsureGloballyRepackedGlyphAtlasSnapshot(RuntimeFont& runtime,
		float rasterScale, bool* repacked)
	{
		if (repacked)
			*repacked = false;
		if (!TryLoadGlyphAtlasSnapshot(runtime, rasterScale))
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: global atlas repack unavailable font=%u stage=initial-restore",
				GetRuntimeConfig(runtime).fontId);
			return false;
		}
		if (HasGloballyRepackedGlyphAtlasSnapshot(runtime, rasterScale))
			return true;
		if (!SaveGlyphAtlasSnapshot(runtime, rasterScale))
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: global atlas repack unavailable font=%u stage=repack-write",
				GetRuntimeConfig(runtime).fontId);
			return false;
		}
		if (!RebuildGlyphAtlasFromSnapshot(runtime, rasterScale))
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: global atlas repack unavailable font=%u stage=post-repack-restore",
				GetRuntimeConfig(runtime).fontId);
			return false;
		}
		if (!HasGloballyRepackedGlyphAtlasSnapshot(runtime, rasterScale))
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: global atlas repack unavailable font=%u stage=repacked-flag-validation",
				GetRuntimeConfig(runtime).fontId);
			return false;
		}
		if (repacked)
			*repacked = true;
		return true;
	}

	bool DiscardGlyphAtlasSnapshot(RuntimeFont& runtime, float rasterScale)
	{
		const FontConfig& config = GetRuntimeConfig(runtime);
		const size_t roleCount = IsDbcsCodePage(GetFreeTypeTextCodePage()) ? 2 : 1;
		UInt32 discardedPages = 0;
		UInt64 discardedBytes = 0;
		{
			AtlasState& state = State();
			std::lock_guard<std::mutex> lock(state.atlasMutex);
			for (size_t roleIndex = 0; roleIndex < roleCount; ++roleIndex)
			{
				const VectorFontByteClass byteClass =
					static_cast<VectorFontByteClass>(roleIndex);
				if (IsPrewarmAtlasAlias(config, byteClass))
					continue;
				AtlasCacheKey baseKey;
				if (!ResolvePrewarmAtlasKey(config,
					byteClass, rasterScale, baseKey))
				{
					continue;
				}
				const AtlasProfileKey profileKey = MakeAtlasProfileKey(baseKey);
				std::vector<AtlasCacheKey> keys;
				for (const auto& entry : state.atlasCache)
				{
					if (MakeAtlasProfileKey(entry.first) == profileKey)
						keys.push_back(entry.first);
				}
				for (const AtlasCacheKey& key : keys)
				{
					auto page = state.atlasCache.find(key);
					if (page == state.atlasCache.end())
						continue;
					if (page->second.resource)
						RetireDefaultGeneration(*page->second.resource);
					UnindexAtlasPage(state, key);
					discardedBytes += page->second.bytes;
					state.atlasCacheBytes -= page->second.bytes;
					state.atlasLru.erase(page->second.lru);
					state.atlasCache.erase(page);
					++discardedPages;
				}
				InvalidateCompleteAtlasProfileLocked(state, profileKey);
			}
			RefreshAtlasCacheGpuAccountingLocked(state);
		}

		UInt32 deletedFiles = 0;
		UInt32 failedFiles = 0;
		DWORD firstDeleteError = ERROR_SUCCESS;
		for (size_t roleIndex = 0; roleIndex < roleCount; ++roleIndex)
		{
			const VectorFontByteClass byteClass =
				static_cast<VectorFontByteClass>(roleIndex);
			if (IsPrewarmAtlasAlias(config, byteClass))
				continue;
			AtlasCacheKey key;
			if (!ResolvePrewarmAtlasKey(config,
				byteClass, rasterScale, key))
			{
				continue;
			}
			for (UInt16 pageIndex = 0; pageIndex < kMaximumAtlasSnapshotPages;
				++pageIndex)
			{
				key.pageIndex = pageIndex;
				UInt64 ignoredSnapshotHash = 0;
				UInt64 ignoredMaskContentHash = 0;
				const std::wstring path = GetAtlasSnapshotPath(runtime, key,
					ignoredSnapshotHash, ignoredMaskContentHash);
				if (path.empty())
					continue;
				const std::array<std::wstring, 3> candidates = {
					path, path + L".tmp", path + L".stream.tmp"
				};
				for (const std::wstring& candidate : candidates)
				{
					if (DeleteFileW(candidate.c_str()))
					{
						++deletedFiles;
					}
					else
					{
						const DWORD error = GetLastError();
						if (!IsMissingFileError(error))
						{
							++failedFiles;
							if (firstDeleteError == ERROR_SUCCESS)
								firstDeleteError = error;
						}
					}
				}
			}
		}
		gLog.FormattedMessage(
			"tnvse_freetype_font: incomplete atlas cache reset font=%u residentPages=%u residentBytes=%llu filesDeleted=%u filesFailed=%u firstDeleteError=%lu",
			config.fontId, discardedPages,
			static_cast<unsigned long long>(discardedBytes), deletedFiles, failedFiles,
			static_cast<unsigned long>(firstDeleteError));
		return failedFiles == 0;
	}

	bool SaveGlyphAtlasSnapshot(RuntimeFont& runtime, float rasterScale)
	{
		bool jointRolePublished = false;
		if (!SaveGlyphAtlasSnapshotRole(runtime,
			VectorFontByteClass::SingleByte, rasterScale,
			&jointRolePublished))
		{
			return false;
		}
		return jointRolePublished
			|| !IsDbcsCodePage(GetFreeTypeTextCodePage())
			|| IsPrewarmAtlasAlias(GetRuntimeConfig(runtime),
				VectorFontByteClass::DoubleByte)
			|| SaveGlyphAtlasSnapshotRole(runtime,
				VectorFontByteClass::DoubleByte, rasterScale);
	}

	bool ConsolidatePhysicalFontAtlasGroups(float rasterScale,
		const FontAtlasPrewarmProgressReporter* progress)
	{
		if (!g_bEnableFreeTypeDefaultPoolAtlas
			|| !IsDbcsCodePage(GetFreeTypeTextCodePage())
			|| !std::isfinite(rasterScale) || rasterScale <= 0.0f)
		{
			return true;
		}
		const UInt32 scaleMilli = static_cast<UInt32>(
			std::lround(rasterScale * 1000.0f));
		if (!scaleMilli)
			return true;

		std::vector<const FontConfig*> configs;
		configs.reserve(g_configs.size());
		for (const auto& entry : g_configs)
			configs.push_back(&entry.second);
		std::sort(configs.begin(), configs.end(),
			[](const FontConfig* left, const FontConfig* right)
			{
				return left->fontId < right->fontId;
			});

		bool success = true;
		std::unordered_set<UInt64> processedGroups;
		for (const FontConfig* config : configs)
		{
			if (!config)
				continue;
			PhysicalAtlasGroup group;
			if (!BuildPhysicalAtlasGroup(*config, scaleMilli, group)
				|| !processedGroups.insert(group.identity).second)
			{
				continue;
			}

			std::shared_ptr<AtlasResource> residentGroup;
			{
				std::lock_guard<std::mutex> lock(State().atlasMutex);
				if (IsPhysicalAtlasGroupResidentLocked(
					group, &residentGroup))
				{
					const size_t bytes = GetAtlasStorageBytes(
						residentGroup->width, residentGroup->height,
						residentGroup->pixelMode, residentGroup->mipLevels);
					gLog.FormattedMessage(
						"tnvse_freetype_font: physical atlas group reused version=%u owner=%u members=%u uniqueSingleProfiles=%u doubleLayouts=%u size=%ux%u gpuBytes=%llu pageContentHash=%016llX",
						kPhysicalAtlasGroupVersion, group.ownerFontId,
						static_cast<UInt32>(group.members.size()),
						static_cast<UInt32>(
							group.uniqueSingleByteProfiles.size()),
						static_cast<UInt32>(
							group.uniqueDoubleByteLayoutHashes.size()),
						residentGroup->width, residentGroup->height,
						static_cast<unsigned long long>(bytes),
						static_cast<unsigned long long>(
							residentGroup->pageContentHash));
					continue;
				}
				if (IsPhysicalAtlasGroupFallbackMarkedLocked(group))
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: physical atlas group fallback reused version=%u owner=%u members=%u uniqueSingleProfiles=%u doubleLayouts=%u policy=retain-per-font-atlases",
						kPhysicalAtlasGroupVersion, group.ownerFontId,
						static_cast<UInt32>(group.members.size()),
						static_cast<UInt32>(
							group.uniqueSingleByteProfiles.size()),
						static_cast<UInt32>(
							group.uniqueDoubleByteLayoutHashes.size()));
					continue;
				}
			}

			RuntimeFont* ownerRuntime =
				EnsureRuntimeFont(group.ownerFontId);
			const auto validateMemberTables = [&](RuntimeFont& memberRuntime)
			{
				const std::shared_ptr<const SealedDirectFontProfile> sealed =
					LoadRuntimeSealedDirectProfile(memberRuntime);
				if (!sealed || !IsSealedDirectFontProfileUsable(
						memberRuntime, sealed, rasterScale))
				{
					return false;
				}
				for (size_t roleIndex = 0; roleIndex < 2; ++roleIndex)
				{
					const VectorFontByteClass role =
						static_cast<VectorFontByteClass>(roleIndex);
					const auto& table = sealed->tables[roleIndex];
					if (!table || table->byteClass != role
						|| table->layoutIdentity
							!= GetRuntimeDirectRoleLayoutIdentity(
								memberRuntime, role))
					{
						return false;
					}
				}
				return true;
			};
			bool sourceTablesReady = ownerRuntime != nullptr;
			if (sourceTablesReady)
			{
				for (const PhysicalAtlasGroupMember& member : group.members)
				{
					RuntimeFont* memberRuntime =
						EnsureRuntimeFont(member.config->fontId);
					if (!memberRuntime
						|| !BuildDirectGlyphAtlasTables(
							*memberRuntime, rasterScale)
						|| !validateMemberTables(*memberRuntime))
					{
						sourceTablesReady = false;
						break;
					}
				}
			}
			bool groupPublished = false;
			bool groupFallback = false;
			if (sourceTablesReady && progress)
			{
				progress->Report(
					FontAtlasPrewarmProgressStage::PublishPhysicalGroup,
					static_cast<UInt32>(processedGroups.size()), 0);
			}
			const bool groupSaved = sourceTablesReady
				&& SaveGlyphAtlasSnapshotRole(*ownerRuntime,
					VectorFontByteClass::SingleByte, rasterScale,
					&groupPublished, &group, &groupFallback);
			if (groupFallback)
				continue;
			if (!groupSaved || !groupPublished)
			{
				success = false;
				bool fallbackRestored = sourceTablesReady;
				if (sourceTablesReady)
				{
					for (const PhysicalAtlasGroupMember& member :
						group.members)
					{
						RuntimeFont* memberRuntime =
							EnsureRuntimeFont(member.config->fontId);
						if (!memberRuntime
							|| !RebuildGlyphAtlasFromSnapshot(
								*memberRuntime, rasterScale)
							|| !BuildDirectGlyphAtlasTables(
								*memberRuntime, rasterScale))
						{
							fallbackRestored = false;
						}
					}
				}
				gLog.FormattedMessage(
					"tnvse_freetype_font: physical atlas group consolidation skipped version=%u owner=%u members=%u uniqueSingleProfiles=%u doubleLayouts=%u fallbackRestored=%u reason=publish-failed",
					kPhysicalAtlasGroupVersion, group.ownerFontId,
					static_cast<UInt32>(group.members.size()),
					static_cast<UInt32>(
						group.uniqueSingleByteProfiles.size()),
					static_cast<UInt32>(
						group.uniqueDoubleByteLayoutHashes.size()),
					fallbackRestored ? 1u : 0u);
				continue;
			}

			bool rebuilt = true;
			std::vector<RuntimeFont*> memberRuntimes;
			memberRuntimes.reserve(group.members.size());
			if (progress)
			{
				progress->Report(
					FontAtlasPrewarmProgressStage::RestorePhysicalGroup,
					0, static_cast<UInt32>(group.members.size()));
			}
			for (const PhysicalAtlasGroupMember& member : group.members)
			{
				RuntimeFont* memberRuntime =
					EnsureRuntimeFont(member.config->fontId);
				if (!memberRuntime
					|| !RebuildGlyphAtlasFromSnapshot(
						*memberRuntime, rasterScale))
				{
					rebuilt = false;
					break;
				}
				memberRuntimes.push_back(memberRuntime);
			}
			if (rebuilt)
			{
				for (RuntimeFont* memberRuntime : memberRuntimes)
				{
					if (!BuildDirectGlyphAtlasTables(
							*memberRuntime, rasterScale)
						|| !validateMemberTables(*memberRuntime))
					{
						rebuilt = false;
						break;
					}
				}
			}

			std::shared_ptr<AtlasResource> shared;
			bool physicallyShared = false;
			bool logicalProfilesReady = rebuilt;
			const char* validationFailure = "not-rebuilt";
			if (rebuilt)
			{
				std::lock_guard<std::mutex> lock(State().atlasMutex);
				physicallyShared =
					IsPhysicalAtlasGroupResidentLocked(
						group, &shared, &validationFailure);
			}
			if (rebuilt && physicallyShared && shared)
			{
				for (RuntimeFont* memberRuntime : memberRuntimes)
				{
					const std::shared_ptr<const SealedDirectFontProfile> sealed =
						LoadRuntimeSealedDirectProfile(*memberRuntime);
					if (!sealed || !validateMemberTables(*memberRuntime))
					{
						logicalProfilesReady = false;
						break;
					}
					for (const auto& table : sealed->tables)
					{
						if (!table || table->pages.size() != 1)
						{
							logicalProfilesReady = false;
							break;
						}
						const std::shared_ptr<AtlasResource> page =
							table->pages.front().lock();
						if (!page || !AreAtlasResourcesBackedBySameTexture(
								*shared, *page))
						{
							logicalProfilesReady = false;
							break;
						}
					}
					if (!logicalProfilesReady)
						break;
				}
				if (!logicalProfilesReady)
					validationFailure = "logical-direct-profile";
			}
			if (!rebuilt || !physicallyShared || !shared
				|| !logicalProfilesReady)
			{
				success = false;
				gLog.FormattedMessage(
					"tnvse_freetype_font: physical atlas group restore validation failed version=%u owner=%u members=%u rebuilt=%u physicallyShared=%u logicalProfiles=%u reason=%s",
					kPhysicalAtlasGroupVersion, group.ownerFontId,
					static_cast<UInt32>(group.members.size()),
					rebuilt ? 1u : 0u, physicallyShared ? 1u : 0u,
					logicalProfilesReady ? 1u : 0u,
					validationFailure);
				continue;
			}

			const size_t bytes = GetAtlasStorageBytes(shared->width,
				shared->height, shared->pixelMode, shared->mipLevels);
			gLog.FormattedMessage(
				"tnvse_freetype_font: physical atlas group active version=%u owner=%u members=%u uniqueSingleProfiles=%u doubleLayouts=%u logicalProfiles=%u size=%ux%u gpuBytes=%llu pageContentHash=%016llX",
				kPhysicalAtlasGroupVersion, group.ownerFontId,
				static_cast<UInt32>(group.members.size()),
				static_cast<UInt32>(
					group.uniqueSingleByteProfiles.size()),
				static_cast<UInt32>(
					group.uniqueDoubleByteLayoutHashes.size()),
				static_cast<UInt32>(memberRuntimes.size()),
				shared->width, shared->height,
				static_cast<unsigned long long>(bytes),
				static_cast<unsigned long long>(shared->pageContentHash));
		}
		return success;
	}

	bool RebuildGlyphAtlasFromSnapshot(RuntimeFont& runtime, float rasterScale)
	{
		const FontConfig& config = GetRuntimeConfig(runtime);
		const bool needsDoubleByte = IsDbcsCodePage(GetFreeTypeTextCodePage());
		UInt32 discardedPages = 0;
		UInt64 discardedBytes = 0;
		{
			AtlasState& state = State();
			std::lock_guard<std::mutex> lock(state.atlasMutex);
			const size_t roleCount = needsDoubleByte ? 2 : 1;
			for (size_t roleIndex = 0; roleIndex < roleCount; ++roleIndex)
			{
				const VectorFontByteClass byteClass =
					static_cast<VectorFontByteClass>(roleIndex);
				if (IsPrewarmAtlasAlias(config, byteClass))
					continue;
				AtlasCacheKey baseKey;
				if (!ResolvePrewarmAtlasKey(config,
					byteClass, rasterScale, baseKey))
				{
					return false;
				}
				const auto profile = state.atlasProfiles.find(
					MakeAtlasProfileKey(baseKey));
				if (profile == state.atlasProfiles.end())
					continue;
				const std::vector<UInt16> pages = profile->second.pages;
				for (UInt16 pageIndex : pages)
				{
					AtlasCacheKey pageKey = baseKey;
					pageKey.pageIndex = pageIndex;
					auto page = state.atlasCache.find(pageKey);
					if (page == state.atlasCache.end())
						continue;
					const auto lru = page->second.lru;
					if (page->second.resource)
						RetireDefaultGeneration(*page->second.resource);
					UnindexAtlasPage(state, pageKey);
					discardedBytes += page->second.bytes;
					state.atlasCacheBytes -= page->second.bytes;
					state.atlasLru.erase(lru);
					state.atlasCache.erase(page);
					++discardedPages;
				}
			}
			RefreshAtlasCacheGpuAccountingLocked(state);
		}

		const bool restored = TryLoadGlyphAtlasSnapshot(runtime, rasterScale);
		gLog.FormattedMessage(
			"tnvse_freetype_font: atlas post-prewarm rebuild font=%u discardedPages=%u discardedBytes=%llu result=%s",
			config.fontId, discardedPages,
			static_cast<unsigned long long>(discardedBytes),
			restored ? "complete" : "failed");
		return restored;
	}

	bool PrewarmGlyphAtlas(RuntimeFont& runtime,
		VectorFontByteClass byteClass,
		const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
		float rasterScale)
	{
		if (bitmaps.empty())
			return true;
		const FontConfig& config = GetRuntimeConfig(runtime);
		AtlasCacheKey key;
		// Original-shader ARGB glyphs bake a per-range color into distinct bitmap
		// cache IDs at shape-build time. A raw ARGB prewarm atlas cannot be reused
		// and would only evict useful distance-field generations.
		if (!ResolvePrewarmAtlasKey(config, byteClass, rasterScale, key))
			return true;
		// Height-first shelves make a complete prewarm profile substantially denser
		// than appending scan-order batches, and let every page be uploaded once.
		std::vector<std::shared_ptr<const GlyphBitmap>> packedBitmaps = bitmaps;
		std::sort(packedBitmaps.begin(), packedBitmaps.end(),
			[](const auto& lhs, const auto& rhs)
			{
				if (lhs->height != rhs->height)
					return lhs->height > rhs->height;
				if (lhs->width != rhs->width)
					return lhs->width > rhs->width;
				return lhs->cacheId < rhs->cacheId;
			});
		return !GetAtlasResources(config, byteClass, rasterScale, packedBitmaps,
			key.pixelMode, key.renderMode, key.padding).empty();
	}

	PersistentCacheCleanupClass ClassifyAtlasSnapshotCacheForCleanup(
		const std::wstring& path)
	{
		AtlasSnapshotHeader header = {};
		SnapshotPayloadSource payload;
		if (!ReadSnapshotMetadata(path, header, nullptr, payload)
			|| payload.path.empty()
			|| header.renderMode
				> static_cast<UInt8>(AtlasRenderMode::ShaderEffects)
			|| header.pixelMode > static_cast<UInt8>(AtlasPixelMode::Mtsdf32)
			|| header.byteClass > static_cast<UInt8>(
				VectorFontByteClass::DoubleByte))
		{
			return PersistentCacheCleanupClass::Invalid;
		}
		const FontAtlasRoute route = GetPersistentFontCacheRoute();
		if (header.renderMode == static_cast<UInt8>(
			AtlasRenderMode::CpuEffects))
		{
			const bool aggressive =
				header.pixelMode == static_cast<UInt8>(AtlasPixelMode::Argb32);
			const bool fallback =
				header.pixelMode == static_cast<UInt8>(AtlasPixelMode::A8);
			if (!aggressive && !fallback)
				return PersistentCacheCleanupClass::Invalid;
			return (aggressive
						&& route == FontAtlasRoute::ShaderA8Coverage)
					|| (fallback
						&& route == FontAtlasRoute::ArgbFallback)
				? PersistentCacheCleanupClass::Neutral
				: PersistentCacheCleanupClass::InactiveDistanceField;
		}

		DistanceFieldMethod method;
		if (header.pixelMode == static_cast<UInt8>(AtlasPixelMode::A8))
			method = DistanceFieldMethod::TrueSdf;
		else if (header.pixelMode == static_cast<UInt8>(AtlasPixelMode::Mtsdf32))
			method = DistanceFieldMethod::Mtsdf;
		else
			return PersistentCacheCleanupClass::Invalid;
		return route == FontAtlasRoute::ShaderDistanceField
				&& method == GetConfiguredDistanceFieldMethod()
			? PersistentCacheCleanupClass::CurrentDistanceField
			: PersistentCacheCleanupClass::InactiveDistanceField;
	}
}

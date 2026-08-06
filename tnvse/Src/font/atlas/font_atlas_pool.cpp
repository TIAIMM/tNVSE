#include "font_atlas_snapshot_internal.h"

#include "font_manager.h"
#include "load_config.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace fonthook::vectorfont
{
	namespace implementation::font_atlas_snapshot {}
	using namespace implementation::font_atlas_snapshot;

	namespace
	{
		constexpr size_t kMaximumExactPhysicalPoolAtoms = 8;
		constexpr size_t kMaximumBoundedPhysicalPoolAtoms = 16;

		struct PhysicalPoolMemberState
		{
			PhysicalAtlasGroupMember member;
			std::shared_ptr<AtlasResource> page;
		};

		struct PhysicalPoolAtom
		{
			std::vector<PhysicalAtlasGroupMember> members;
			std::shared_ptr<AtlasResource> page;
			UInt64 gpuBytes = 0;
			bool restoredPoolV3 = false;
		};

		struct PhysicalPoolCandidate
		{
			UInt32 atomMask = 0;
			PhysicalAtlasGroup pool;
			PhysicalAtlasGroupPreview preview;
		};

		bool SamePhysicalPoolStorageContract(const AtlasCacheKey& left,
			const AtlasCacheKey& right)
		{
			return left.scaleMilli == right.scaleMilli
				&& left.pixelMode == right.pixelMode
				&& left.renderMode == right.renderMode
				&& left.padding == right.padding
				&& left.levelZeroOnly == right.levelZeroOnly;
		}

		UInt32 CountSetBits(UInt32 value)
		{
			UInt32 count = 0;
			while (value)
			{
				value &= value - 1u;
				++count;
			}
			return count;
		}

		UInt32 LowestSetBit(UInt32 value)
		{
			return value & (~value + 1u);
		}

		size_t FindAtomRoot(std::vector<size_t>& parents, size_t index)
		{
			size_t root = index;
			while (parents[root] != root)
				root = parents[root];
			while (parents[index] != index)
			{
				const size_t next = parents[index];
				parents[index] = root;
				index = next;
			}
			return root;
		}

		bool ValidatePhysicalPoolMember(RuntimeFont& runtime,
			const PhysicalAtlasGroupMember& member, float rasterScale,
			std::shared_ptr<AtlasResource>& sharedPage)
		{
			sharedPage.reset();
			const std::shared_ptr<const SealedDirectFontProfile> sealed =
				LoadRuntimeSealedDirectProfile(runtime);
			if (!sealed || !IsSealedDirectFontProfileUsable(
					runtime, sealed, rasterScale))
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
						!= GetRuntimeDirectRoleLayoutIdentity(runtime, role)
					|| table->pages.size() != 1)
				{
					return false;
				}
				const std::shared_ptr<AtlasResource> page =
					table->pages.front().lock();
				if (!page || !page->compactSnapshot || !page->pageContentHash)
					return false;
				if (!sharedPage)
					sharedPage = page;
				else if (!AreAtlasResourcesBackedBySameTexture(
					*sharedPage, *page))
				{
					return false;
				}
			}
			return sharedPage != nullptr;
		}

		bool CollectPhysicalPoolMemberStates(float rasterScale,
			std::vector<PhysicalPoolMemberState>& states,
			UInt32& skippedFonts)
		{
			states.clear();
			skippedFonts = 0;
			std::vector<const FontConfig*> configs;
			configs.reserve(g_configs.size());
			for (const auto& entry : g_configs)
				configs.push_back(&entry.second);
			std::sort(configs.begin(), configs.end(),
				[](const FontConfig* left, const FontConfig* right)
				{
					return left->fontId < right->fontId;
				});

			for (const FontConfig* config : configs)
			{
				PhysicalPoolMemberState state;
				state.member.config = config;
				RuntimeFont* runtime = config
					? EnsureRuntimeFont(config->fontId) : nullptr;
				if (!config || !runtime
					|| !ResolvePrewarmAtlasKey(*config,
						VectorFontByteClass::SingleByte, rasterScale,
						state.member.singleByteKey)
					|| !ResolvePrewarmAtlasKey(*config,
						VectorFontByteClass::DoubleByte, rasterScale,
						state.member.doubleByteKey)
					|| !UsesPlacedLevelZeroSnapshot(
						state.member.singleByteKey)
					|| !UsesPlacedLevelZeroSnapshot(
						state.member.doubleByteKey)
					|| !SamePhysicalPoolStorageContract(
						state.member.singleByteKey,
						state.member.doubleByteKey)
					|| !BuildDirectGlyphAtlasTables(*runtime, rasterScale)
					|| !ValidatePhysicalPoolMember(*runtime,
						state.member, rasterScale, state.page))
				{
					++skippedFonts;
					continue;
				}
				states.push_back(std::move(state));
			}
			return !states.empty();
		}

		bool BuildPhysicalPoolAtoms(
			const std::vector<PhysicalPoolMemberState>& states,
			std::vector<PhysicalPoolAtom>& atoms)
		{
			atoms.clear();
			if (states.empty())
				return false;
			std::vector<size_t> parents(states.size());
			for (size_t index = 0; index < parents.size(); ++index)
				parents[index] = index;
			for (size_t left = 0; left < states.size(); ++left)
			{
				for (size_t right = left + 1; right < states.size(); ++right)
				{
					if (!states[left].page || !states[right].page
						|| !AreAtlasResourcesBackedBySameTexture(
							*states[left].page, *states[right].page))
					{
						continue;
					}
					const size_t leftRoot = FindAtomRoot(parents, left);
					const size_t rightRoot = FindAtomRoot(parents, right);
					if (leftRoot != rightRoot)
						parents[rightRoot] = leftRoot;
				}
			}

			std::unordered_map<size_t, size_t> atomIndices;
			for (size_t index = 0; index < states.size(); ++index)
			{
				const size_t root = FindAtomRoot(parents, index);
				auto inserted = atomIndices.emplace(root, atoms.size());
				if (inserted.second)
					atoms.push_back({});
				PhysicalPoolAtom& atom = atoms[inserted.first->second];
				atom.members.push_back(states[index].member);
				if (!atom.page)
					atom.page = states[index].page;
			}
			for (PhysicalPoolAtom& atom : atoms)
			{
				if (!atom.page)
					return false;
				atom.gpuBytes = GetAtlasStorageBytes(atom.page->width,
					atom.page->height, atom.page->pixelMode,
					atom.page->mipLevels);
				atom.restoredPoolV3 = atom.page->compactSnapshot
					&& (atom.page->compactSnapshot->sourceHeader.flags
						& kAtlasSnapshotFlagPhysicalFontPool) != 0;
			}
			std::sort(atoms.begin(), atoms.end(),
				[](const PhysicalPoolAtom& left,
					const PhysicalPoolAtom& right)
				{
					return left.members.front().config->fontId
						< right.members.front().config->fontId;
				});
			return true;
		}

		bool RestorePhysicalPoolMembers(const PhysicalAtlasGroup& pool,
			float rasterScale)
		{
			bool restored = true;
			for (const PhysicalAtlasGroupMember& member : pool.members)
			{
				RuntimeFont* runtime = EnsureRuntimeFont(member.config->fontId);
				if (!runtime
					|| !RebuildGlyphAtlasFromSnapshot(*runtime, rasterScale)
					|| !BuildDirectGlyphAtlasTables(*runtime, rasterScale))
				{
					restored = false;
				}
			}
			return restored;
		}

		bool CanPhysicalPoolBeatSourceByArea(
			const std::vector<PhysicalPoolAtom>& atoms, UInt32 atomMask,
			const PhysicalAtlasGroup& pool, UInt64 sourceGpuBytes,
			UInt64& paddedGlyphBytes, UInt64& largestSavingPageBytes)
		{
			paddedGlyphBytes = 0;
			largestSavingPageBytes = 0;
			if (!atomMask || pool.members.empty() || !sourceGpuBytes)
				return false;
			const AtlasCacheKey& storageKey =
				pool.members.front().singleByteKey;
			const UInt64 bytesPerPixel = AtlasBytesPerPixel(
				storageKey.pixelMode);
			std::unordered_set<UInt64> cacheIds;
			for (size_t atomIndex = 0; atomIndex < atoms.size(); ++atomIndex)
			{
				if (!(atomMask & (1u << static_cast<UInt32>(atomIndex))))
					continue;
				const auto& page = atoms[atomIndex].page;
				if (!page || !page->compactSnapshot)
					return true;
				for (const AtlasSnapshotPlacement& placement :
					page->compactSnapshot->placements)
				{
					if (!cacheIds.insert(placement.cacheId).second)
						continue;
					const UInt64 paddedWidth = placement.rect.width
						+ static_cast<UInt64>(storageKey.padding) * 2u;
					const UInt64 paddedHeight = placement.rect.height
						+ static_cast<UInt64>(storageKey.padding) * 2u;
					if (!paddedWidth || !paddedHeight
						|| paddedWidth > std::numeric_limits<UInt64>::max()
							/ paddedHeight)
					{
						return true;
					}
					const UInt64 paddedPixels = paddedWidth * paddedHeight;
					if (!bytesPerPixel
						|| paddedPixels > std::numeric_limits<UInt64>::max()
							/ bytesPerPixel)
					{
						return true;
					}
					const UInt64 bytes = paddedPixels * bytesPerPixel;
					if (bytes > std::numeric_limits<UInt64>::max()
							- paddedGlyphBytes)
					{
						return true;
					}
					paddedGlyphBytes += bytes;
				}
			}

			const SnapshotPackingCaps caps = GetSnapshotPackingCaps();
			const UInt32 maximumSize = GetSnapshotMaximumSize(
				caps, VectorFontByteClass::DoubleByte,
				storageKey.pixelMode);
			for (UInt32 width = 64; width && width <= maximumSize;)
			{
				for (UInt32 height = 64; height && height <= maximumSize;)
				{
					if (IsSnapshotPageShapeValid(
							width, height, maximumSize, caps))
					{
						const UInt64 bytes = GetAtlasStorageBytes(
							width, height, storageKey.pixelMode,
							GetAtlasMipLevelCount(width, height,
								storageKey.levelZeroOnly));
						if (bytes < sourceGpuBytes)
							largestSavingPageBytes = std::max(
								largestSavingPageBytes, bytes);
					}
					if (height > maximumSize / 2u)
						break;
					height <<= 1;
				}
				if (width > maximumSize / 2u)
					break;
				width <<= 1;
			}
			return largestSavingPageBytes
				&& paddedGlyphBytes <= largestSavingPageBytes;
		}

		bool PublishPhysicalPool(const PhysicalPoolCandidate& candidate,
			float rasterScale)
		{
			const PhysicalAtlasGroup& pool = candidate.pool;
			RuntimeFont* ownerRuntime = EnsureRuntimeFont(pool.ownerFontId);
			bool sourcesReady = ownerRuntime != nullptr;
			for (const PhysicalAtlasGroupMember& member : pool.members)
			{
				RuntimeFont* runtime = EnsureRuntimeFont(member.config->fontId);
				std::shared_ptr<AtlasResource> ignoredPage;
				if (!runtime
					|| !BuildDirectGlyphAtlasTables(*runtime, rasterScale)
					|| !ValidatePhysicalPoolMember(*runtime,
						member, rasterScale, ignoredPage))
				{
					sourcesReady = false;
					break;
				}
			}

			bool published = false;
			bool fallback = false;
			const bool saved = sourcesReady
				&& SaveGlyphAtlasSnapshotRole(*ownerRuntime,
					VectorFontByteClass::SingleByte, rasterScale,
					&published, &pool, &fallback);
			if (!saved || !published || fallback)
			{
				const bool fallbackRestored = sourcesReady
					&& RestorePhysicalPoolMembers(pool, rasterScale);
				gLog.FormattedMessage(
					"tnvse_freetype_font: physical atlas pool publication skipped version=%u owner=%u members=%u sourceReady=%u saved=%u published=%u fallback=%u fallbackRestored=%u reason=publish-failed",
					kPhysicalAtlasPoolVersion, pool.ownerFontId,
					static_cast<UInt32>(pool.members.size()),
					sourcesReady ? 1u : 0u, saved ? 1u : 0u,
					published ? 1u : 0u, fallback ? 1u : 0u,
					fallbackRestored ? 1u : 0u);
				return false;
			}

			const bool rebuilt = RestorePhysicalPoolMembers(pool, rasterScale);
			std::shared_ptr<AtlasResource> shared;
			const char* failureReason = rebuilt ? "none" : "rebuild";
			bool physicallyShared = false;
			if (rebuilt)
			{
				std::lock_guard<std::mutex> lock(State().atlasMutex);
				physicallyShared = IsPhysicalAtlasGroupResidentLocked(
					pool, &shared, &failureReason);
			}
			bool logicalProfilesReady = rebuilt && physicallyShared && shared;
			if (logicalProfilesReady)
			{
				for (const PhysicalAtlasGroupMember& member : pool.members)
				{
					RuntimeFont* runtime = EnsureRuntimeFont(
						member.config->fontId);
					std::shared_ptr<AtlasResource> memberPage;
					if (!runtime || !ValidatePhysicalPoolMember(*runtime,
							member, rasterScale, memberPage)
						|| !memberPage
						|| !AreAtlasResourcesBackedBySameTexture(
							*shared, *memberPage))
					{
						logicalProfilesReady = false;
						failureReason = "logical-direct-profile";
						break;
					}
				}
			}
			if (!rebuilt || !physicallyShared || !shared
				|| !logicalProfilesReady)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: physical atlas pool restore validation failed version=%u owner=%u members=%u rebuilt=%u physicallyShared=%u logicalProfiles=%u reason=%s",
					kPhysicalAtlasPoolVersion, pool.ownerFontId,
					static_cast<UInt32>(pool.members.size()),
					rebuilt ? 1u : 0u, physicallyShared ? 1u : 0u,
					logicalProfilesReady ? 1u : 0u, failureReason);
				return false;
			}

			const UInt64 actualGpuBytes = GetAtlasStorageBytes(shared->width,
				shared->height, shared->pixelMode, shared->mipLevels);
			gLog.FormattedMessage(
				"tnvse_freetype_font: physical atlas pool active version=%u owner=%u members=%u singleProfiles=%u doubleProfiles=%u sourceGpuBytes=%llu targetGpuBytes=%llu savedGpuBytes=%llu logicalProfiles=%u size=%ux%u pageContentHash=%016llX",
				kPhysicalAtlasPoolVersion, pool.ownerFontId,
				static_cast<UInt32>(pool.members.size()),
				static_cast<UInt32>(pool.uniqueSingleByteProfiles.size()),
				static_cast<UInt32>(pool.uniqueDoubleByteProfiles.size()),
				static_cast<unsigned long long>(
					candidate.preview.sourceGpuBytes),
				static_cast<unsigned long long>(actualGpuBytes),
				static_cast<unsigned long long>(
					candidate.preview.sourceGpuBytes > actualGpuBytes
						? candidate.preview.sourceGpuBytes - actualGpuBytes : 0),
				static_cast<UInt32>(pool.members.size()),
				shared->width, shared->height,
				static_cast<unsigned long long>(shared->pageContentHash));
			return actualGpuBytes == candidate.preview.candidateGpuBytes;
		}

		void LogPhysicalPoolAccounting(float rasterScale)
		{
			UInt32 sealedFonts = 0;
			UInt32 logicalPages = 0;
			std::vector<std::shared_ptr<AtlasResource>> physicalPages;
			for (const auto& entry : g_configs)
			{
				RuntimeFont* runtime = FindRuntimeFont(entry.first);
				const auto sealed = runtime
					? LoadRuntimeSealedDirectProfile(*runtime) : nullptr;
				if (!runtime || !sealed
					|| !IsSealedDirectFontProfileUsable(
						*runtime, sealed, rasterScale))
				{
					continue;
				}
				++sealedFonts;
				logicalPages += static_cast<UInt32>(sealed->atlases.size());
				for (const auto& page : sealed->atlases)
				{
					if (!page)
						continue;
					const bool duplicate = std::any_of(
						physicalPages.begin(), physicalPages.end(),
						[&](const auto& existing)
						{
							return existing.get() == page.get()
								|| AreAtlasResourcesBackedBySameTexture(
									*existing, *page);
						});
					if (!duplicate)
						physicalPages.push_back(page);
				}
			}
			UInt64 activeGpuBytes = 0;
			UInt32 poolPages = 0;
			for (const auto& page : physicalPages)
			{
				activeGpuBytes += GetAtlasStorageBytes(page->width, page->height,
					page->pixelMode, page->mipLevels);
				poolPages += page->compactSnapshot
					&& (page->compactSnapshot->sourceHeader.flags
						& kAtlasSnapshotFlagPhysicalFontPool) != 0 ? 1u : 0u;
			}
			std::vector<std::shared_ptr<AtlasResource>> retiredPages;
			{
				std::lock_guard<std::mutex> lock(State().atlasMutex);
				retiredPages.reserve(State().retiredAtlases.size());
				for (const RetiredAtlasGeneration& retired :
					State().retiredAtlases)
				{
					if (retired.resource)
						retiredPages.push_back(retired.resource);
				}
			}
			UInt64 retiredGpuBytes = 0;
			UInt32 retiredPhysicalPages = 0;
			for (size_t index = 0; index < retiredPages.size(); ++index)
			{
				const auto& page = retiredPages[index];
				const bool duplicateActive = std::any_of(
					physicalPages.begin(), physicalPages.end(),
					[&](const auto& existing)
					{
						return existing.get() == page.get()
							|| AreAtlasResourcesBackedBySameTexture(
								*existing, *page);
					});
				const bool duplicateRetired = std::any_of(
					retiredPages.begin(), retiredPages.begin() + index,
					[&](const auto& existing)
					{
						return existing.get() == page.get()
							|| AreAtlasResourcesBackedBySameTexture(
								*existing, *page);
					});
				if (duplicateActive || duplicateRetired)
					continue;
				++retiredPhysicalPages;
				retiredGpuBytes += GetAtlasStorageBytes(page->width, page->height,
					page->pixelMode, page->mipLevels);
			}
			gLog.FormattedMessage(
				"tnvse_freetype_font: physical atlas pool accounting version=%u configuredFonts=%u sealedFonts=%u logicalPages=%u activePhysicalPages=%u poolPages=%u activeProfileGpuBytes=%llu retiredPhysicalPages=%u retiredGpuBytes=%llu trackedGpuBytes=%llu scope=sealed-profiles-plus-retired-generations",
				kPhysicalAtlasPoolVersion,
				static_cast<UInt32>(g_configs.size()), sealedFonts,
				logicalPages, static_cast<UInt32>(physicalPages.size()),
				poolPages, static_cast<unsigned long long>(activeGpuBytes),
				retiredPhysicalPages,
				static_cast<unsigned long long>(retiredGpuBytes),
				static_cast<unsigned long long>(
					activeGpuBytes + retiredGpuBytes));
		}
	}

	bool ConsolidatePhysicalFontAtlasPools(float rasterScale)
	{
		if (!g_bEnableFreeTypeDefaultPoolAtlas
			|| !IsDbcsCodePage(GetFreeTypeTextCodePage())
			|| !std::isfinite(rasterScale) || rasterScale <= 0.0f)
		{
			return true;
		}

		std::vector<PhysicalPoolMemberState> memberStates;
		UInt32 skippedFonts = 0;
		if (!CollectPhysicalPoolMemberStates(
			rasterScale, memberStates, skippedFonts))
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: physical atlas pool planning skipped version=%u configuredFonts=%u skippedFonts=%u reason=no-eligible-sealed-profile",
				kPhysicalAtlasPoolVersion,
				static_cast<UInt32>(g_configs.size()), skippedFonts);
			return false;
		}

		std::vector<PhysicalPoolAtom> atoms;
		if (!BuildPhysicalPoolAtoms(memberStates, atoms)
			|| atoms.empty()
			|| atoms.size() > kMaximumBoundedPhysicalPoolAtoms)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: physical atlas pool planning skipped version=%u eligibleFonts=%u atoms=%u maximumAtoms=%u reason=atom-limit-or-build",
				kPhysicalAtlasPoolVersion,
				static_cast<UInt32>(memberStates.size()),
				static_cast<UInt32>(atoms.size()),
				static_cast<UInt32>(kMaximumBoundedPhysicalPoolAtoms));
			return false;
		}

		for (const PhysicalPoolAtom& atom : atoms)
		{
			if (!atom.restoredPoolV3 || atom.members.size() < 2)
				continue;
			PhysicalAtlasGroup restoredPool;
			std::shared_ptr<AtlasResource> shared;
			bool resident = BuildPhysicalAtlasPool(atom.members, restoredPool);
			if (resident)
			{
				std::lock_guard<std::mutex> lock(State().atlasMutex);
				resident = IsPhysicalAtlasGroupResidentLocked(
					restoredPool, &shared);
			}
			if (resident && shared)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: physical atlas pool reused version=%u owner=%u members=%u size=%ux%u gpuBytes=%llu pageContentHash=%016llX",
					kPhysicalAtlasPoolVersion, restoredPool.ownerFontId,
					static_cast<UInt32>(restoredPool.members.size()),
					shared->width, shared->height,
					static_cast<unsigned long long>(atom.gpuBytes),
					static_cast<unsigned long long>(
						shared->pageContentHash));
			}
		}

		const bool exactPlanning = atoms.size()
			<= kMaximumExactPhysicalPoolAtoms;
		const UInt32 stateCount = 1u << static_cast<UInt32>(atoms.size());
		const UInt32 fullMask = stateCount - 1u;
		const UInt64 infinity = std::numeric_limits<UInt64>::max();
		std::vector<UInt64> subsetCosts(stateCount, infinity);
		std::vector<int> candidateIndices(stateCount, -1);
		UInt64 baselineGpuBytes = 0;
		for (size_t atomIndex = 0; atomIndex < atoms.size(); ++atomIndex)
		{
			const UInt32 mask = 1u << static_cast<UInt32>(atomIndex);
			subsetCosts[mask] = atoms[atomIndex].gpuBytes;
			if (atoms[atomIndex].gpuBytes > infinity - baselineGpuBytes)
				return false;
			baselineGpuBytes += atoms[atomIndex].gpuBytes;
		}

		UInt32 candidatesTried = 0;
		UInt32 candidatesAccepted = 0;
		UInt32 incompatibleCandidates = 0;
		UInt32 previewFailures = 0;
		UInt32 lowerBoundCandidates = 0;
		UInt32 multiPageCandidates = 0;
		UInt32 nonSavingCandidates = 0;
		UInt32 sourceMismatchCandidates = 0;
		std::vector<PhysicalPoolCandidate> candidates;
		for (UInt32 mask = 1; mask < stateCount; ++mask)
		{
			const UInt32 atomCount = CountSetBits(mask);
			if (atomCount < 2 || (!exactPlanning && atomCount != 2))
				continue;
			++candidatesTried;
			std::vector<PhysicalAtlasGroupMember> members;
			UInt64 subsetBaseline = 0;
			for (size_t atomIndex = 0; atomIndex < atoms.size(); ++atomIndex)
			{
				if (!(mask & (1u << static_cast<UInt32>(atomIndex))))
					continue;
				members.insert(members.end(), atoms[atomIndex].members.begin(),
					atoms[atomIndex].members.end());
				if (atoms[atomIndex].gpuBytes > infinity - subsetBaseline)
				{
					subsetBaseline = infinity;
					break;
				}
				subsetBaseline += atoms[atomIndex].gpuBytes;
			}

			PhysicalPoolCandidate candidate;
			candidate.atomMask = mask;
			if (subsetBaseline == infinity
				|| !BuildPhysicalAtlasPool(members, candidate.pool))
			{
				++incompatibleCandidates;
				continue;
			}
			UInt64 paddedGlyphBytes = 0;
			UInt64 largestSavingPageBytes = 0;
			if (!CanPhysicalPoolBeatSourceByArea(atoms, mask,
				candidate.pool, subsetBaseline, paddedGlyphBytes,
				largestSavingPageBytes))
			{
				++lowerBoundCandidates;
				continue;
			}
			RuntimeFont* ownerRuntime = EnsureRuntimeFont(
				candidate.pool.ownerFontId);
			bool ignoredPublished = false;
			bool ignoredFallback = false;
			if (!ownerRuntime
				|| !SaveGlyphAtlasSnapshotRole(*ownerRuntime,
					VectorFontByteClass::SingleByte, rasterScale,
					&ignoredPublished, &candidate.pool, &ignoredFallback,
					&candidate.preview)
				|| !candidate.preview.evaluated)
			{
				++previewFailures;
				continue;
			}
			if (candidate.preview.sourceGpuBytes != subsetBaseline)
			{
				++sourceMismatchCandidates;
				continue;
			}
			if (!candidate.preview.feasible
				|| candidate.preview.pageCount != 1)
			{
				++multiPageCandidates;
				continue;
			}
			if (candidate.preview.candidateGpuBytes
				>= candidate.preview.sourceGpuBytes)
			{
				++nonSavingCandidates;
				continue;
			}

			const int candidateIndex = static_cast<int>(candidates.size());
			candidateIndices[mask] = candidateIndex;
			subsetCosts[mask] = candidate.preview.candidateGpuBytes;
			++candidatesAccepted;
			gLog.FormattedMessage(
				"tnvse_freetype_font: physical atlas pool candidate accepted version=%u atomMask=%08X atoms=%u members=%u sourceGpuBytes=%llu targetGpuBytes=%llu savedGpuBytes=%llu size=%ux%u placements=%llu",
				kPhysicalAtlasPoolVersion, mask, atomCount,
				static_cast<UInt32>(candidate.pool.members.size()),
				static_cast<unsigned long long>(
					candidate.preview.sourceGpuBytes),
				static_cast<unsigned long long>(
					candidate.preview.candidateGpuBytes),
				static_cast<unsigned long long>(
					candidate.preview.sourceGpuBytes
						- candidate.preview.candidateGpuBytes),
				candidate.preview.width, candidate.preview.height,
				static_cast<unsigned long long>(
					candidate.preview.placementCount));
			candidates.push_back(std::move(candidate));
		}

		std::vector<UInt64> bestCosts(stateCount, infinity);
		std::vector<UInt32> bestParts(stateCount,
			std::numeric_limits<UInt32>::max());
		std::vector<UInt32> bestChoices(stateCount, 0);
		bestCosts[0] = 0;
		bestParts[0] = 0;
		for (UInt32 mask = 1; mask < stateCount; ++mask)
		{
			const UInt32 pivot = LowestSetBit(mask);
			const auto consider = [&](UInt32 subset)
			{
				if (!subset || !(subset & pivot)
					|| (subset & mask) != subset
					|| subsetCosts[subset] == infinity)
				{
					return;
				}
				const UInt32 remainder = mask ^ subset;
				if (bestCosts[remainder] == infinity
					|| subsetCosts[subset]
						> infinity - bestCosts[remainder])
				{
					return;
				}
				const UInt64 cost = subsetCosts[subset]
					+ bestCosts[remainder];
				const UInt32 parts = bestParts[remainder] + 1u;
				if (cost < bestCosts[mask]
					|| (cost == bestCosts[mask]
						&& parts < bestParts[mask]))
				{
					bestCosts[mask] = cost;
					bestParts[mask] = parts;
					bestChoices[mask] = subset;
				}
			};
			consider(pivot);
			for (const PhysicalPoolCandidate& candidate : candidates)
				consider(candidate.atomMask);
		}

		std::vector<int> selectedCandidates;
		UInt32 remaining = fullMask;
		while (remaining)
		{
			const UInt32 choice = bestChoices[remaining];
			if (!choice)
				break;
			const int candidateIndex = candidateIndices[choice];
			if (candidateIndex >= 0)
				selectedCandidates.push_back(candidateIndex);
			remaining ^= choice;
		}
		const UInt64 plannedGpuBytes = bestCosts[fullMask];
		gLog.FormattedMessage(
			"tnvse_freetype_font: physical atlas pool plan version=%u policy=%s eligibleFonts=%u skippedFonts=%u atoms=%u candidatesTried=%u candidatesAccepted=%u selectedPools=%u sourceGpuBytes=%llu plannedGpuBytes=%llu plannedSavedGpuBytes=%llu rejectedIncompatible=%u rejectedAreaBound=%u rejectedPreview=%u rejectedMultiPageOrGrowth=%u rejectedNoSaving=%u rejectedSourceMismatch=%u",
			kPhysicalAtlasPoolVersion,
			exactPlanning ? "exact-partition" : "bounded-pairs",
			static_cast<UInt32>(memberStates.size()), skippedFonts,
			static_cast<UInt32>(atoms.size()), candidatesTried,
			candidatesAccepted,
			static_cast<UInt32>(selectedCandidates.size()),
			static_cast<unsigned long long>(baselineGpuBytes),
			static_cast<unsigned long long>(plannedGpuBytes),
			static_cast<unsigned long long>(
				baselineGpuBytes > plannedGpuBytes
					? baselineGpuBytes - plannedGpuBytes : 0),
			incompatibleCandidates, lowerBoundCandidates, previewFailures,
			multiPageCandidates,
			nonSavingCandidates, sourceMismatchCandidates);

		// Candidate previews and atom discovery retain source pages only for the
		// duration of planning. Drop those extra owners before publication so the
		// normal sealed-profile handoff controls the transition lifetime.
		memberStates.clear();
		for (PhysicalPoolAtom& atom : atoms)
			atom.page.reset();

		bool success = remaining == 0 && plannedGpuBytes != infinity;
		for (int candidateIndex : selectedCandidates)
		{
			if (candidateIndex < 0
				|| static_cast<size_t>(candidateIndex) >= candidates.size()
				|| !PublishPhysicalPool(
					candidates[candidateIndex], rasterScale))
			{
				success = false;
			}
		}
		{
			std::lock_guard<std::mutex> lock(State().atlasMutex);
			PruneRetiredAtlasGenerations();
			RefreshAtlasCacheGpuAccountingLocked(State());
		}
		LogPhysicalPoolAccounting(rasterScale);
		return success;
	}
}

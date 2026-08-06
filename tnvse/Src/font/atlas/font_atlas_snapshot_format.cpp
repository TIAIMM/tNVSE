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
	namespace implementation::font_atlas_snapshot
	{
		UInt64 HashAtlasBytes(const void* data, size_t size,
			UInt64 hash)
		{
			const UInt8* bytes = static_cast<const UInt8*>(data);
			for (size_t index = 0; index < size; ++index)
			{
				hash ^= bytes[index];
				hash *= 1099511628211ull;
			}
			return hash;
		}

		bool UsesPlacedLevelZeroSnapshot(const AtlasCacheKey& key);


		bool SameAtlasStorageContract(const AtlasCacheKey& left,
			const AtlasCacheKey& right)
		{
			return left.scaleMilli == right.scaleMilli
				&& left.pixelMode == right.pixelMode
				&& left.renderMode == right.renderMode
				&& left.padding == right.padding
				&& left.levelZeroOnly == right.levelZeroOnly;
		}

		bool LessAtlasProfileKey(const AtlasProfileKey& left,
			const AtlasProfileKey& right)
		{
			if (left.atlasContentHash != right.atlasContentHash)
				return left.atlasContentHash < right.atlasContentHash;
			if (left.scaleMilli != right.scaleMilli)
				return left.scaleMilli < right.scaleMilli;
			if (left.pixelMode != right.pixelMode)
				return static_cast<UInt8>(left.pixelMode)
					< static_cast<UInt8>(right.pixelMode);
			if (left.renderMode != right.renderMode)
				return static_cast<UInt8>(left.renderMode)
					< static_cast<UInt8>(right.renderMode);
			if (left.padding != right.padding)
				return left.padding < right.padding;
			if (left.levelZeroOnly != right.levelZeroOnly)
				return left.levelZeroOnly < right.levelZeroOnly;
			return static_cast<UInt8>(left.byteClass)
				< static_cast<UInt8>(right.byteClass);
		}

		UInt64 HashAtlasProfileKey(const AtlasProfileKey& key, UInt64 hash)
		{
			hash = HashAtlasBytes(&key.atlasContentHash,
				sizeof(key.atlasContentHash), hash);
			hash = HashAtlasBytes(&key.scaleMilli, sizeof(key.scaleMilli), hash);
			hash = HashAtlasBytes(&key.pixelMode, sizeof(key.pixelMode), hash);
			hash = HashAtlasBytes(&key.renderMode, sizeof(key.renderMode), hash);
			hash = HashAtlasBytes(&key.padding, sizeof(key.padding), hash);
			hash = HashAtlasBytes(&key.levelZeroOnly,
				sizeof(key.levelZeroOnly), hash);
			return HashAtlasBytes(&key.byteClass, sizeof(key.byteClass), hash);
		}

		bool BuildPhysicalAtlasGroup(const FontConfig& anchor,
			UInt32 scaleMilli, PhysicalAtlasGroup& group)
		{
			group = {};
			group.version = kPhysicalAtlasGroupVersion;
			if (!g_bEnableFreeTypeDefaultPoolAtlas
				|| !IsDbcsCodePage(GetFreeTypeTextCodePage())
				|| !scaleMilli)
			{
				return false;
			}

			const float rasterScale = scaleMilli / 1000.0f;
			AtlasCacheKey anchorSingle;
			AtlasCacheKey anchorDouble;
			if (!ResolvePrewarmAtlasKey(anchor,
					VectorFontByteClass::SingleByte, rasterScale, anchorSingle)
				|| !ResolvePrewarmAtlasKey(anchor,
					VectorFontByteClass::DoubleByte, rasterScale, anchorDouble)
				|| !UsesPlacedLevelZeroSnapshot(anchorSingle)
				|| !UsesPlacedLevelZeroSnapshot(anchorDouble)
				|| !SameAtlasStorageContract(anchorSingle, anchorDouble))
			{
				return false;
			}

			const AtlasProfileKey doubleProfile =
				MakeAtlasProfileKey(anchorDouble);
			const size_t singleRoleIndex = static_cast<size_t>(
				VectorFontByteClass::SingleByte);
			const size_t doubleRoleIndex = static_cast<size_t>(
				VectorFontByteClass::DoubleByte);
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
				if (!config)
					continue;
				PhysicalAtlasGroupMember member;
				member.config = config;
				if (!ResolvePrewarmAtlasKey(*config,
						VectorFontByteClass::SingleByte, rasterScale,
						member.singleByteKey)
					|| !ResolvePrewarmAtlasKey(*config,
						VectorFontByteClass::DoubleByte, rasterScale,
						member.doubleByteKey)
					|| !(MakeAtlasProfileKey(member.doubleByteKey)
						== doubleProfile)
					|| !config->layoutRoleHashes[singleRoleIndex]
					|| !config->layoutRoleHashes[doubleRoleIndex]
					|| !UsesPlacedLevelZeroSnapshot(member.singleByteKey)
					|| !UsesPlacedLevelZeroSnapshot(member.doubleByteKey)
					|| !SameAtlasStorageContract(
						member.singleByteKey, anchorDouble)
					|| !SameAtlasStorageContract(
						member.doubleByteKey, anchorDouble))
				{
					continue;
				}
				group.members.push_back(std::move(member));
			}
			if (group.members.size() < 2)
				return false;

			for (const PhysicalAtlasGroupMember& member : group.members)
			{
				const AtlasProfileKey profile =
					MakeAtlasProfileKey(member.singleByteKey);
				if (std::find(group.uniqueSingleByteProfiles.begin(),
						group.uniqueSingleByteProfiles.end(), profile)
					== group.uniqueSingleByteProfiles.end())
				{
					group.uniqueSingleByteProfiles.push_back(profile);
				}
				const AtlasProfileKey doubleMemberProfile =
					MakeAtlasProfileKey(member.doubleByteKey);
				if (std::find(group.uniqueDoubleByteProfiles.begin(),
						group.uniqueDoubleByteProfiles.end(), doubleMemberProfile)
					== group.uniqueDoubleByteProfiles.end())
				{
					group.uniqueDoubleByteProfiles.push_back(
						doubleMemberProfile);
				}
				const UInt64 doubleLayoutHash =
					member.config->layoutRoleHashes[doubleRoleIndex];
				if (std::find(group.uniqueDoubleByteLayoutHashes.begin(),
						group.uniqueDoubleByteLayoutHashes.end(), doubleLayoutHash)
					== group.uniqueDoubleByteLayoutHashes.end())
				{
					group.uniqueDoubleByteLayoutHashes.push_back(doubleLayoutHash);
				}
			}
			// Identical single- and double-byte profiles already share a complete
			// page through the existing content-addressed cache. A physical font
			// group is useful only when at least two single-byte profiles differ.
			if (group.uniqueSingleByteProfiles.size() < 2)
				return false;
			std::sort(group.uniqueSingleByteProfiles.begin(),
				group.uniqueSingleByteProfiles.end(), LessAtlasProfileKey);
			std::sort(group.uniqueDoubleByteProfiles.begin(),
				group.uniqueDoubleByteProfiles.end(), LessAtlasProfileKey);
			std::sort(group.uniqueDoubleByteLayoutHashes.begin(),
				group.uniqueDoubleByteLayoutHashes.end());

			group.ownerFontId = group.members.front().config->fontId;
			UInt64 hash = HashAtlasBytes(&kPhysicalAtlasGroupVersion,
				sizeof(kPhysicalAtlasGroupVersion));
			hash = HashAtlasProfileKey(doubleProfile, hash);
			const UInt32 profileCount = static_cast<UInt32>(
				group.uniqueSingleByteProfiles.size());
			hash = HashAtlasBytes(&profileCount, sizeof(profileCount), hash);
			for (const AtlasProfileKey& profile :
				group.uniqueSingleByteProfiles)
			{
				hash = HashAtlasProfileKey(profile, hash);
			}
			const UInt32 doubleLayoutCount = static_cast<UInt32>(
				group.uniqueDoubleByteLayoutHashes.size());
			hash = HashAtlasBytes(&doubleLayoutCount,
				sizeof(doubleLayoutCount), hash);
			for (UInt64 layoutHash : group.uniqueDoubleByteLayoutHashes)
				hash = HashAtlasBytes(&layoutHash, sizeof(layoutHash), hash);

			// v2 separates immutable raster identity from logical layout identity.
			// Keep the latter in the group generation hash so membership/config
			// changes republish aliases and direct tables, but never require all
			// members to use the same advances, baselines, or logical pixel size.
			const UInt32 memberCount = static_cast<UInt32>(group.members.size());
			hash = HashAtlasBytes(&memberCount, sizeof(memberCount), hash);
			for (const PhysicalAtlasGroupMember& member : group.members)
			{
				const UInt32 fontId = member.config->fontId;
				hash = HashAtlasBytes(&fontId, sizeof(fontId), hash);
				hash = HashAtlasProfileKey(
					MakeAtlasProfileKey(member.singleByteKey), hash);
				hash = HashAtlasBytes(
					&member.config->layoutRoleHashes[singleRoleIndex],
					sizeof(member.config->layoutRoleHashes[singleRoleIndex]), hash);
				hash = HashAtlasBytes(
					&member.config->layoutRoleHashes[doubleRoleIndex],
					sizeof(member.config->layoutRoleHashes[doubleRoleIndex]), hash);
			}
			group.identity = hash;
			return group.identity != 0;
		}

		bool BuildPhysicalAtlasPool(
			const std::vector<PhysicalAtlasGroupMember>& members,
			PhysicalAtlasGroup& pool)
		{
			pool = {};
			pool.version = kPhysicalAtlasPoolVersion;
			if (!g_bEnableFreeTypeDefaultPoolAtlas
				|| !IsDbcsCodePage(GetFreeTypeTextCodePage())
				|| members.size() < 2)
			{
				return false;
			}

			pool.members = members;
			std::sort(pool.members.begin(), pool.members.end(),
				[](const PhysicalAtlasGroupMember& left,
					const PhysicalAtlasGroupMember& right)
				{
					return left.config && right.config
						? left.config->fontId < right.config->fontId
						: left.config != nullptr;
				});
			const size_t singleRoleIndex = static_cast<size_t>(
				VectorFontByteClass::SingleByte);
			const size_t doubleRoleIndex = static_cast<size_t>(
				VectorFontByteClass::DoubleByte);
			const AtlasCacheKey& storageKey = pool.members.front().singleByteKey;
			UInt32 previousFontId = 0;
			bool hasPreviousFontId = false;
			for (const PhysicalAtlasGroupMember& member : pool.members)
			{
				if (!member.config
					|| (hasPreviousFontId
						&& member.config->fontId == previousFontId)
					|| !member.config->layoutRoleHashes[singleRoleIndex]
					|| !member.config->layoutRoleHashes[doubleRoleIndex]
					|| !UsesPlacedLevelZeroSnapshot(member.singleByteKey)
					|| !UsesPlacedLevelZeroSnapshot(member.doubleByteKey)
					|| !SameAtlasStorageContract(
						member.singleByteKey, storageKey)
					|| !SameAtlasStorageContract(
						member.doubleByteKey, storageKey))
				{
					return false;
				}
				hasPreviousFontId = true;
				previousFontId = member.config->fontId;

				const AtlasProfileKey singleProfile =
					MakeAtlasProfileKey(member.singleByteKey);
				if (std::find(pool.uniqueSingleByteProfiles.begin(),
						pool.uniqueSingleByteProfiles.end(), singleProfile)
					== pool.uniqueSingleByteProfiles.end())
				{
					pool.uniqueSingleByteProfiles.push_back(singleProfile);
				}
				const AtlasProfileKey doubleProfile =
					MakeAtlasProfileKey(member.doubleByteKey);
				if (std::find(pool.uniqueDoubleByteProfiles.begin(),
						pool.uniqueDoubleByteProfiles.end(), doubleProfile)
					== pool.uniqueDoubleByteProfiles.end())
				{
					pool.uniqueDoubleByteProfiles.push_back(doubleProfile);
				}
				const UInt64 doubleLayoutHash =
					member.config->layoutRoleHashes[doubleRoleIndex];
				if (std::find(pool.uniqueDoubleByteLayoutHashes.begin(),
						pool.uniqueDoubleByteLayoutHashes.end(), doubleLayoutHash)
					== pool.uniqueDoubleByteLayoutHashes.end())
				{
					pool.uniqueDoubleByteLayoutHashes.push_back(
						doubleLayoutHash);
				}
			}

			std::sort(pool.uniqueSingleByteProfiles.begin(),
				pool.uniqueSingleByteProfiles.end(), LessAtlasProfileKey);
			std::sort(pool.uniqueDoubleByteProfiles.begin(),
				pool.uniqueDoubleByteProfiles.end(), LessAtlasProfileKey);
			std::sort(pool.uniqueDoubleByteLayoutHashes.begin(),
				pool.uniqueDoubleByteLayoutHashes.end());
			pool.ownerFontId = pool.members.front().config->fontId;

			UInt64 hash = HashAtlasBytes(&kPhysicalAtlasPoolVersion,
				sizeof(kPhysicalAtlasPoolVersion));
			const auto hashProfiles = [&](const auto& profiles, UInt64 value)
			{
				const UInt32 count = static_cast<UInt32>(profiles.size());
				value = HashAtlasBytes(&count, sizeof(count), value);
				for (const AtlasProfileKey& profile : profiles)
					value = HashAtlasProfileKey(profile, value);
				return value;
			};
			hash = hashProfiles(pool.uniqueSingleByteProfiles, hash);
			hash = hashProfiles(pool.uniqueDoubleByteProfiles, hash);
			const UInt32 doubleLayoutCount = static_cast<UInt32>(
				pool.uniqueDoubleByteLayoutHashes.size());
			hash = HashAtlasBytes(&doubleLayoutCount,
				sizeof(doubleLayoutCount), hash);
			for (UInt64 layoutHash : pool.uniqueDoubleByteLayoutHashes)
				hash = HashAtlasBytes(&layoutHash, sizeof(layoutHash), hash);

			const UInt32 memberCount = static_cast<UInt32>(pool.members.size());
			hash = HashAtlasBytes(&memberCount, sizeof(memberCount), hash);
			for (const PhysicalAtlasGroupMember& member : pool.members)
			{
				const UInt32 fontId = member.config->fontId;
				hash = HashAtlasBytes(&fontId, sizeof(fontId), hash);
				hash = HashAtlasProfileKey(
					MakeAtlasProfileKey(member.singleByteKey), hash);
				hash = HashAtlasProfileKey(
					MakeAtlasProfileKey(member.doubleByteKey), hash);
				hash = HashAtlasBytes(
					&member.config->layoutRoleHashes[singleRoleIndex],
					sizeof(member.config->layoutRoleHashes[singleRoleIndex]), hash);
				hash = HashAtlasBytes(
					&member.config->layoutRoleHashes[doubleRoleIndex],
					sizeof(member.config->layoutRoleHashes[doubleRoleIndex]), hash);
			}
			pool.identity = hash;
			return pool.identity != 0;
		}

		bool IsPhysicalAtlasGroupResidentLocked(
			const PhysicalAtlasGroup& group,
			std::shared_ptr<AtlasResource>* sharedResource,
			const char** failureReason)
		{
			if (sharedResource)
				sharedResource->reset();
			if (failureReason)
				*failureReason = "none";
			const auto fail = [&](const char* reason)
			{
				if (failureReason)
					*failureReason = reason;
				return false;
			};
			if (group.members.empty())
				return fail("empty-group");

			std::vector<AtlasCacheKey> roleKeys;
			std::unordered_set<AtlasProfileKey, AtlasProfileKeyHash>
				roleProfiles;
			for (const PhysicalAtlasGroupMember& member : group.members)
			{
				const std::array<const AtlasCacheKey*, 2> memberRoleKeys = {
					&member.singleByteKey, &member.doubleByteKey
				};
				for (const AtlasCacheKey* memberRoleKey : memberRoleKeys)
				{
					const AtlasProfileKey profile =
						MakeAtlasProfileKey(*memberRoleKey);
					if (roleProfiles.insert(profile).second)
						roleKeys.push_back(*memberRoleKey);
				}
			}

			std::shared_ptr<AtlasResource> first;
			UInt64 pageContentHash = 0;
			AtlasState& state = State();
			for (AtlasCacheKey roleKey : roleKeys)
			{
				const AtlasProfileKey profileKey =
					MakeAtlasProfileKey(roleKey);
				const auto profile = state.atlasProfiles.find(profileKey);
				if (state.completeAtlasProfiles.find(profileKey)
						== state.completeAtlasProfiles.end()
					|| profile == state.atlasProfiles.end()
					|| profile->second.pages.size() != 1
					|| profile->second.pages.front() != 0)
				{
					return fail("incomplete-profile");
				}
				roleKey.pageIndex = 0;
				const auto page = state.atlasCache.find(roleKey);
				const UInt32 requiredFlags = kAtlasSnapshotFlagPhysicalFontGroup
					| (group.version == kPhysicalAtlasPoolVersion
						? kAtlasSnapshotFlagPhysicalFontPool : 0u);
				if (page == state.atlasCache.end()
					|| !page->second.resource
					|| !page->second.resource->compactSnapshot
					|| (page->second.resource->compactSnapshot
						->sourceHeader.flags & requiredFlags) != requiredFlags
					|| !page->second.resource->pageContentHash)
				{
					return fail("missing-page-resource");
				}
				const AtlasResource& resource = *page->second.resource;
				if (resource.compactSnapshot->sourceHeader.pageContentHash
					!= resource.pageContentHash)
				{
					return fail("snapshot-content-mismatch");
				}
				if (!first)
				{
					first = page->second.resource;
					pageContentHash = first->pageContentHash;
				}
				else if (page->second.resource->pageContentHash
						!= pageContentHash)
				{
					return fail("page-content-mismatch");
				}
				else if (!AreAtlasResourcesBackedBySameTexture(
					*first, *page->second.resource))
				{
					return fail("physical-texture-mismatch");
				}
			}
			if (!first)
				return fail("missing-shared-resource");
			if (sharedResource)
				*sharedResource = first;
			return true;
		}

		bool IsPhysicalAtlasGroupFallbackMarkedLocked(
			const PhysicalAtlasGroup& group)
		{
			if (group.members.empty())
				return false;
			const AtlasCacheKey& baseKey =
				group.members.front().singleByteKey;
			AtlasState& state = State();
			const AtlasProfileKey profileKey =
				MakeAtlasProfileKey(baseKey);
			const auto profile = state.atlasProfiles.find(profileKey);
			if (state.completeAtlasProfiles.find(profileKey)
					== state.completeAtlasProfiles.end()
				|| profile == state.atlasProfiles.end()
				|| profile->second.pages.empty())
			{
				return false;
			}
			for (UInt16 pageIndex : profile->second.pages)
			{
				AtlasCacheKey pageKey = baseKey;
				pageKey.pageIndex = pageIndex;
				const auto page = state.atlasCache.find(pageKey);
				if (page == state.atlasCache.end()
					|| !page->second.resource
					|| !page->second.resource->compactSnapshot
					|| !(page->second.resource->compactSnapshot
						->sourceHeader.flags
						& kAtlasSnapshotFlagPhysicalFontGroupFallback))
				{
					return false;
				}
			}
			return true;
		}


		bool IsPowerOfTwo(UInt32 value)
		{
			return value && (value & (value - 1u)) == 0;
		}

		SnapshotPackingCaps GetSnapshotPackingCaps()
		{
			SnapshotPackingCaps caps;
			caps.singleByteMaximum =
				GetMaximumAtlasSize(VectorFontByteClass::SingleByte);
			caps.doubleByteMaximum =
				GetMaximumAtlasSize(VectorFontByteClass::DoubleByte);
			if (NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton())
			{
				caps.maximumAspectRatio =
					renderer->m_kD3DCaps9.MaxTextureAspectRatio;
			}
			return caps;
		}

		UInt32 GetSnapshotMaximumSize(const SnapshotPackingCaps& caps,
			VectorFontByteClass byteClass)
		{
			return byteClass == VectorFontByteClass::DoubleByte
				? caps.doubleByteMaximum : caps.singleByteMaximum;
		}

		bool IsSnapshotAspectRatioValid(UInt32 width, UInt32 height,
			const SnapshotPackingCaps& caps)
		{
			if (!width || !height || !caps.maximumAspectRatio)
				return width && height;
			const UInt32 longer = std::max(width, height);
			const UInt32 shorter = std::min(width, height);
			return static_cast<UInt64>(longer)
				<= static_cast<UInt64>(shorter) * caps.maximumAspectRatio;
		}

		bool IsSnapshotPageShapeValid(UInt32 width, UInt32 height,
			UInt32 maximumSize, const SnapshotPackingCaps& caps)
		{
			if (width < 64 || height < 64
				|| width > maximumSize || height > maximumSize
				|| !IsPowerOfTwo(width) || !IsPowerOfTwo(height)
				|| !IsSnapshotAspectRatioValid(width, height, caps))
			{
				return false;
			}
			return true;
		}

		bool TryEnableSparseFile(HANDLE file)
		{
			if (file == INVALID_HANDLE_VALUE)
				return false;
			DWORD bytesReturned = 0;
			return DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0,
				nullptr, 0, &bytesReturned, nullptr) != FALSE;
		}

		bool WriteSequentialFileBytes(HANDLE file, const void* data, size_t size)
		{
			if (!size)
				return true;
			DWORD written = 0;
			return size <= std::numeric_limits<DWORD>::max()
				&& WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr)
				&& written == size;
		}

		bool IsMissingFileError(DWORD error)
		{
			return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
		}

		bool WriteSparseFileBytes(HANDLE file, const UInt8* data, size_t size,
			bool sparse)
		{
			constexpr size_t kSparseChunkBytes = 64u * 1024u;
			for (size_t offset = 0; offset < size;)
			{
				const size_t runStart = offset;
				const size_t firstChunk = std::min(kSparseChunkBytes, size - offset);
				const bool zeroRun = sparse && std::all_of(data + offset,
					data + offset + firstChunk, [](UInt8 value) { return value == 0; });
				offset += firstChunk;
				while (offset < size)
				{
					const size_t chunk = std::min(kSparseChunkBytes, size - offset);
					const bool zero = sparse && std::all_of(data + offset,
						data + offset + chunk, [](UInt8 value) { return value == 0; });
					if (zero != zeroRun)
						break;
					offset += chunk;
				}
				const size_t runBytes = offset - runStart;
				if (zeroRun)
				{
					LARGE_INTEGER distance = {};
					distance.QuadPart = static_cast<LONGLONG>(runBytes);
					if (!SetFilePointerEx(file, distance, nullptr, FILE_CURRENT))
						return false;
				}
				else
				{
					size_t written = 0;
					while (written < runBytes)
					{
						const size_t chunk = std::min(runBytes - written,
							static_cast<size_t>(std::numeric_limits<DWORD>::max()));
						if (!WriteSequentialFileBytes(file, data + runStart + written, chunk))
							return false;
						written += chunk;
					}
				}
			}
			return SetEndOfFile(file) != FALSE;
		}

		UInt64 ComputeAtlasSnapshotIdentityHash(const AtlasCacheKey& key,
			UInt64 maskContentHash,
			const FontConfig& config)
		{
			UInt64 hash = HashAtlasBytes(
				&kAtlasSnapshotPayloadIdentityVersion,
				sizeof(kAtlasSnapshotPayloadIdentityVersion));
			hash = HashAtlasBytes(&maskContentHash, sizeof(maskContentHash), hash);
			hash = HashAtlasBytes(&key.atlasContentHash,
				sizeof(key.atlasContentHash), hash);
			hash = HashAtlasBytes(&key.scaleMilli, sizeof(key.scaleMilli), hash);
			hash = HashAtlasBytes(&key.pixelMode, sizeof(key.pixelMode), hash);
			hash = HashAtlasBytes(&key.renderMode, sizeof(key.renderMode), hash);
			hash = HashAtlasBytes(&key.padding, sizeof(key.padding), hash);
			hash = HashAtlasBytes(&key.levelZeroOnly,
				sizeof(key.levelZeroOnly), hash);
			hash = HashAtlasBytes(&key.byteClass, sizeof(key.byteClass), hash);
			const bool forceSingleAtlas =
				g_bEnableFreeTypeDefaultPoolAtlas;
			hash = HashAtlasBytes(&forceSingleAtlas,
				sizeof(forceSingleAtlas), hash);
			const SnapshotPackingCaps packingCaps = GetSnapshotPackingCaps();
			hash = HashAtlasBytes(&kAtlasPackingRevision,
				sizeof(kAtlasPackingRevision), hash);
			hash = HashAtlasBytes(&packingCaps.singleByteMaximum,
				sizeof(packingCaps.singleByteMaximum), hash);
			hash = HashAtlasBytes(&packingCaps.doubleByteMaximum,
				sizeof(packingCaps.doubleByteMaximum), hash);
			hash = HashAtlasBytes(&packingCaps.maximumAspectRatio,
				sizeof(packingCaps.maximumAspectRatio), hash);
			PhysicalAtlasGroup physicalGroup;
			const UInt64 physicalGroupIdentity =
				BuildPhysicalAtlasGroup(config, key.scaleMilli, physicalGroup)
					? physicalGroup.identity : 0;
			hash = HashAtlasBytes(&physicalGroupIdentity,
				sizeof(physicalGroupIdentity), hash);
			const UInt32 atlasHardLimit = GetAtlasHardLimit(key.byteClass);
			hash = HashAtlasBytes(&atlasHardLimit,
				sizeof(atlasHardLimit), hash);
			const UInt32 codePage = GetFreeTypeTextCodePage();
			hash = HashAtlasBytes(&codePage, sizeof(codePage), hash);
			hash = HashAtlasBytes(&kCompleteCodePagePrewarmIdentity,
				sizeof(kCompleteCodePagePrewarmIdentity), hash);
			hash = HashAtlasBytes(&kMaximumAtlasMipLevels,
				sizeof(kMaximumAtlasMipLevels), hash);
			hash = HashAtlasBytes(&A8ShapeColorContract::kTileUniformColorAbi,
				sizeof(A8ShapeColorContract::kTileUniformColorAbi), hash);
			if (key.renderMode == AtlasRenderMode::ShaderEffects)
			{
				const DistanceFieldMethod method =
					GetConfiguredDistanceFieldMethod();
				const UInt32 revision = DistanceFieldGeneratorRevision(method);
				hash = HashAtlasBytes(&method, sizeof(method), hash);
				hash = HashAtlasBytes(&revision, sizeof(revision), hash);
			}
			else if (key.renderMode == AtlasRenderMode::CpuEffects
				&& key.pixelMode == AtlasPixelMode::Argb32
				&& key.levelZeroOnly)
			{
				hash = HashAtlasBytes(&kCpuCompositeRasterRevision,
					sizeof(kCpuCompositeRasterRevision), hash);
			}
			// Atlas restore also requires the matching complete glyph manifest.
			hash = HashAtlasBytes(&kPersistentGlyphManifestVersion,
				sizeof(kPersistentGlyphManifestVersion), hash);
			hash = HashAtlasBytes(
				&kPersistentGlyphManifestCacheIdentityVersion,
				sizeof(kPersistentGlyphManifestCacheIdentityVersion), hash);
			return hash;
		}

		std::wstring BuildAtlasSnapshotFilePath(const std::wstring& directory,
			UInt64 snapshotHash, UInt16 pageIndex)
		{
			if (directory.empty() || !snapshotHash)
				return {};
			wchar_t fileName[256] = {};
			_snwprintf_s(fileName, _countof(fileName), _TRUNCATE,
				L"shared_%016llX_p%u.tnvfatlas",
				static_cast<unsigned long long>(snapshotHash), pageIndex);
			return directory + L"\\" + fileName;
		}

		std::wstring GetAtlasSnapshotPath(RuntimeFont& runtime,
			const AtlasCacheKey& key, UInt64& snapshotHash, UInt64& maskContentHash)
		{
			std::wstring directory;
			if (!GetFreeTypeFontCacheDirectory(directory))
				return {};
			RuntimeFont* atlasRuntime = GetPrewarmAtlasRuntime(runtime, key);
			if (!atlasRuntime)
				return {};
			maskContentHash = GetRuntimeMaskContentHash(
				*atlasRuntime, key.byteClass);
			snapshotHash = BuildAtlasSnapshotIdentityHash(key, maskContentHash,
				GetRuntimeConfig(*atlasRuntime));
			const std::wstring path = BuildAtlasSnapshotFilePath(
				directory, snapshotHash, key.pageIndex);
			MarkFreeTypeFontCacheFileUsed(path);
			return path;
		}

		bool GetPlacedLevelZeroSnapshotBytes(
			const std::vector<AtlasSnapshotPlacement>& placements,
			UInt32 atlasWidth, UInt32 atlasHeight, AtlasPixelMode pixelMode,
			size_t& bytes)
		{
			bytes = 0;
			const size_t bytesPerPixel = AtlasBytesPerPixel(pixelMode);
			for (const AtlasSnapshotPlacement& placement : placements)
			{
				const AtlasRect& rect = placement.rect;
				if (!placement.cacheId || !rect.width || !rect.height
					|| rect.x > atlasWidth || rect.width > atlasWidth - rect.x
					|| rect.y > atlasHeight || rect.height > atlasHeight - rect.y)
				{
					return false;
				}
				const size_t rowBytes = static_cast<size_t>(rect.width) * bytesPerPixel;
				if (rect.height > std::numeric_limits<size_t>::max() / rowBytes)
					return false;
				const size_t rectBytes = rowBytes * rect.height;
				if (rectBytes > std::numeric_limits<size_t>::max() - bytes)
					return false;
				bytes += rectBytes;
			}
			return true;
		}

		bool UsesPlacedLevelZeroSnapshot(const AtlasCacheKey& key)
		{
			return key.levelZeroOnly;
		}

		bool MakeSnapshotPlacement(const AtlasResource& resource, UInt64 cacheId,
			const AtlasRect& rect, AtlasSnapshotPlacement& placement)
		{
			const AtlasGlyphRecord* found = FindAtlasGlyph(resource, cacheId);
			if (!found)
				return false;
			if (!found->bitmap)
			{
				if (!resource.compactSnapshot
					|| found->snapshotPlacementIndex
						>= resource.compactSnapshot->placements.size())
				{
					return false;
				}
				placement = resource.compactSnapshot->placements[
					found->snapshotPlacementIndex];
				if (placement.cacheId != cacheId)
					return false;
				placement.rect = rect;
				return true;
			}
			const GlyphBitmap& bitmap = *found->bitmap;
			if (bitmap.cacheId != cacheId || bitmap.width != static_cast<int>(rect.width)
				|| bitmap.height != static_cast<int>(rect.height)
				|| (!bitmap.alpha.empty()
					&& bitmap.alpha.size() < ExpectedGlyphBitmapBytes(bitmap)))
			{
				return false;
			}
			placement.cacheId = cacheId;
			placement.rect = rect;
			placement.left = bitmap.left;
			placement.top = bitmap.top;
			placement.effectiveWidth = bitmap.effectiveWidth;
			placement.effectiveHeight = bitmap.effectiveHeight;
			placement.strokeWidth26Dot6 = bitmap.strokeWidth26Dot6;
			placement.atlasRgb = bitmap.atlasRgb;
			placement.bakedRgba = bitmap.bakedRgba;
			placement.maskType = static_cast<UInt8>(bitmap.maskType);
			placement.sdfSpread = bitmap.sdfSpread;
			placement.colorBaked = bitmap.colorBaked ? 1 : 0;
			placement.bakedLayer = bitmap.bakedLayer;
			return true;
		}

		bool IsValidSnapshotPlacement(const AtlasSnapshotPlacement& placement)
		{
			if (!placement.cacheId || !placement.rect.width || !placement.rect.height
				|| placement.rect.width > kAtlasHardLimit
				|| placement.rect.height > kAtlasHardLimit
				|| placement.maskType > static_cast<UInt8>(
					GlyphMaskType::Composite)
				|| placement.colorBaked > 1 || placement.bakedLayer > 3
				|| placement.effectiveWidth <= 0 || placement.effectiveWidth > 65535
				|| placement.effectiveHeight <= 0 || placement.effectiveHeight > 65535
				|| (placement.maskType == static_cast<UInt8>(GlyphMaskType::DistanceField)
					&& (placement.sdfSpread < 2 || placement.sdfSpread > 32))
				|| (placement.maskType != static_cast<UInt8>(GlyphMaskType::DistanceField)
					&& placement.sdfSpread != 0))
			{
				return false;
			}
			return true;
		}

		bool IsCompleteAtlasProfileResidentLocked(AtlasState& state,
			const AtlasCacheKey& baseKey, UInt16* pageCount,
			UInt64* placementCount)
		{
			const AtlasProfileKey profileKey = MakeAtlasProfileKey(baseKey);
			const auto profile = state.atlasProfiles.find(profileKey);
			if (profile == state.atlasProfiles.end() || profile->second.pages.empty())
				return false;
			const std::shared_ptr<const DirectAtlasGlyphTable>& direct =
				profile->second.directGlyphs;
			const bool sealedCompactProfile =
				profile->second.compactResidentIndexReleased
				&& direct && direct->validity
				&& direct->validity->load(std::memory_order_acquire)
				&& direct->byteClass == baseKey.byteClass
				&& direct->atlasIdentity == baseKey.atlasContentHash
				&& direct->pages.size() == profile->second.pages.size();
			UInt16 expectedPage = 0;
			UInt64 placements = 0;
			for (UInt16 pageIndex : profile->second.pages)
			{
				if (pageIndex != expectedPage++)
					return false;
				AtlasCacheKey pageKey = baseKey;
				pageKey.pageIndex = pageIndex;
				const auto page = state.atlasCache.find(pageKey);
				if (page == state.atlasCache.end() || !page->second.resource
					|| !page->second.resource->property)
				{
					return false;
				}
				if (page->second.resource->glyphs.empty())
				{
					const size_t pageSlot =
						static_cast<size_t>(expectedPage - 1u);
					const std::shared_ptr<AtlasResource> directPage =
						sealedCompactProfile
						? direct->pages[pageSlot].lock() : nullptr;
					if (!directPage
						|| directPage.get()
							!= page->second.resource.get()
						|| !page->second.resource->compactSnapshot
						|| page->second.resource->compactSnapshot
							->placements.empty())
					{
						return false;
					}
					placements += page->second.resource
						->compactSnapshot->placements.size();
					continue;
				}
				for (const AtlasGlyphRecord& glyph : page->second.resource->glyphs)
				{
					if (glyph.bitmap)
						continue;
					if (!page->second.resource->compactSnapshot
						|| glyph.snapshotPlacementIndex
							>= page->second.resource->compactSnapshot->placements.size()
						|| page->second.resource->compactSnapshot->placements[
							glyph.snapshotPlacementIndex].cacheId != glyph.cacheId)
					{
						return false;
					}
				}
				placements += page->second.resource->glyphs.size();
			}
			if (pageCount)
				*pageCount = expectedPage;
			if (placementCount)
				*placementCount = placements;
			return true;
		}

		bool MatchesDefaultPoolSnapshotLayout(
			const AtlasSnapshotHeader& header)
		{
			if (!g_bEnableFreeTypeDefaultPoolAtlas)
				return true;
			const bool single =
				(header.flags & kAtlasSnapshotFlagSingleAtlas) != 0;
			const bool overflow =
				(header.flags & kAtlasSnapshotFlagSingleAtlasOverflow) != 0;
			return header.pageCount == 1
				? single && !overflow
				: header.pageCount > 1 && !single && overflow;
		}

		bool IsGloballyRepackedAtlasProfileResidentLocked(AtlasState& state,
			const AtlasCacheKey& baseKey)
		{
			if (!IsCompleteAtlasProfileResidentLocked(state, baseKey))
				return false;
			const auto profile = state.atlasProfiles.find(
				MakeAtlasProfileKey(baseKey));
			if (profile == state.atlasProfiles.end() || profile->second.pages.empty())
				return false;
			for (UInt16 pageIndex : profile->second.pages)
			{
				AtlasCacheKey pageKey = baseKey;
				pageKey.pageIndex = pageIndex;
				const auto page = state.atlasCache.find(pageKey);
				if (page == state.atlasCache.end() || !page->second.resource
					|| !page->second.resource->compactSnapshot
					|| !(page->second.resource->compactSnapshot->sourceHeader.flags
						& kAtlasSnapshotFlagGloballyRepacked)
					|| !MatchesDefaultPoolSnapshotLayout(
						page->second.resource->compactSnapshot->sourceHeader))
				{
					return false;
				}
			}
			return true;
		}

		bool TryReuseCompleteAtlasProfile(const AtlasCacheKey& key)
		{
			AtlasState& state = State();
			std::lock_guard<std::mutex> lock(state.atlasMutex);
			const AtlasProfileKey profileKey = MakeAtlasProfileKey(key);
			if (state.completeAtlasProfiles.find(profileKey)
				== state.completeAtlasProfiles.end())
			{
				return false;
			}
			UInt16 pageCount = 0;
			UInt64 placementCount = 0;
			if (!IsCompleteAtlasProfileResidentLocked(state, key,
				&pageCount, &placementCount))
			{
				InvalidateCompleteAtlasProfileLocked(state, profileKey);
				return false;
			}
			const auto profile = state.atlasProfiles.find(profileKey);
			for (UInt16 pageIndex : profile->second.pages)
			{
				AtlasCacheKey pageKey = key;
				pageKey.pageIndex = pageIndex;
				auto page = state.atlasCache.find(pageKey);
				state.atlasLru.splice(state.atlasLru.begin(), state.atlasLru,
					page->second.lru);
				page->second.lru = state.atlasLru.begin();
			}
			RecordFreeTypePerf(FreeTypePerfCounter::AtlasSnapshotProfileReuse);
			gLog.FormattedMessage(
				"tnvse_freetype_font: atlas snapshot reused GPU-resident profile font=%u role=%s pages=%u placements=%llu",
				key.fontId, key.byteClass == VectorFontByteClass::DoubleByte
					? "doubleByte" : "singleByte", pageCount,
				static_cast<unsigned long long>(placementCount));
			return true;
		}


		bool DecodeAtlasSnapshotPixels(const AtlasSnapshotHeader& header,
			const std::vector<AtlasSnapshotPlacement>& placements,
			const UInt8* storedPixels, std::vector<UInt8>& pixels)
		{
			if ((!storedPixels && header.storedPixelBytes)
				|| header.pixelBytes > std::numeric_limits<size_t>::max()
				|| header.storedPixelBytes > std::numeric_limits<size_t>::max())
				return false;
			const AtlasPixelMode pixelMode = static_cast<AtlasPixelMode>(header.pixelMode);
			const AtlasSnapshotStorage storageMode =
				static_cast<AtlasSnapshotStorage>(header.storageMode);
			const size_t fullBytes = GetAtlasStorageBytes(header.width, header.height,
				pixelMode, header.mipLevels);
			if (storageMode == AtlasSnapshotStorage::FullMipChain)
			{
				if (header.pixelBytes != fullBytes
					|| header.storedPixelBytes != header.pixelBytes)
					return false;
				pixels.assign(storedPixels, storedPixels + fullBytes);
				return true;
			}
			if (storageMode != AtlasSnapshotStorage::PlacedLevelZeroRects)
				return false;
			size_t packedBytes = 0;
			if (!GetPlacedLevelZeroSnapshotBytes(placements,
				header.width, header.height, pixelMode, packedBytes)
				|| header.pixelBytes != packedBytes)
			{
				return false;
			}
			if (header.storedPixelBytes != header.pixelBytes)
				return false;
			const size_t bytesPerPixel = AtlasBytesPerPixel(pixelMode);
			const size_t levelZeroBytes = static_cast<size_t>(header.width)
				* header.height * bytesPerPixel;
			pixels.assign(levelZeroBytes, 0);
			const size_t destinationPitch = static_cast<size_t>(header.width)
				* bytesPerPixel;
			size_t sourceOffset = 0;
			for (const AtlasSnapshotPlacement& placement : placements)
			{
				const AtlasRect& rect = placement.rect;
				const size_t rowBytes = static_cast<size_t>(rect.width) * bytesPerPixel;
				for (UInt32 row = 0; row < rect.height; ++row)
				{
					UInt8* destination = pixels.data()
						+ static_cast<size_t>(rect.y + row) * destinationPitch
						+ static_cast<size_t>(rect.x) * bytesPerPixel;
					std::memcpy(destination, storedPixels + sourceOffset, rowBytes);
					sourceOffset += rowBytes;
				}
			}
			return sourceOffset == packedBytes;
		}

		bool ReadSnapshotBytesExact(HANDLE file, void* destination, size_t size)
		{
			UInt8* current = static_cast<UInt8*>(destination);
			while (size)
			{
				const DWORD requested = static_cast<DWORD>(std::min<size_t>(
					size, std::numeric_limits<DWORD>::max()));
				DWORD read = 0;
				if (!ReadFile(file, current, requested, &read, nullptr)
					|| read != requested)
				{
					return false;
				}
				current += read;
				size -= read;
			}
			return true;
		}

		bool WriteRepackedSnapshotPixels(HANDLE file,
			const SnapshotPageData& page, UInt64& payloadChecksum,
			UInt64& pageContentHash)
		{
			if (file == INVALID_HANDLE_VALUE
				|| page.pixelSources.size() != page.placements.size())
			{
				return false;
			}
			struct PageIdentity
			{
				UInt32 width;
				UInt32 height;
				UInt32 padding;
				UInt32 mipLevels;
				UInt8 pixelMode;
				UInt8 renderMode;
				UInt8 storageMode;
				UInt8 levelZeroOnly;
			};
			const PageIdentity identity = {
				page.header.width, page.header.height,
				page.header.padding, page.header.mipLevels,
				page.header.pixelMode, page.header.renderMode,
				page.header.storageMode,
				static_cast<UInt8>(page.header.mipLevels == 1 ? 1 : 0)
			};
			payloadChecksum = HashAtlasBytes(page.placements.data(),
				page.placements.size() * sizeof(page.placements[0]));
			pageContentHash = HashAtlasBytes(&identity, sizeof(identity));

			constexpr size_t kOutputBufferBytes = 1024u * 1024u;
			std::vector<UInt8> output;
			output.reserve(kOutputBufferBytes);
			auto flush = [&]()
			{
				if (output.empty())
					return true;
				const bool written = WriteSequentialFileBytes(
					file, output.data(), output.size());
				output.clear();
				return written;
			};

			const CompactAtlasSnapshot* loadedIdentity = nullptr;
			const std::vector<UInt8>* sourcePixels = nullptr;
			std::vector<UInt8> loadedPixels;
			size_t destinationOffset = 0;
			const size_t bytesPerPixel = AtlasBytesPerPixel(
				static_cast<AtlasPixelMode>(page.header.pixelMode));
			for (size_t index = 0; index < page.pixelSources.size(); ++index)
			{
				const SnapshotPixelSource& source =
					page.pixelSources[index];
				const AtlasSnapshotPlacement& placement =
					page.placements[index];
				if (!source.snapshot
					|| source.destinationOffset != destinationOffset)
				{
					return false;
				}
				const size_t expectedBytes =
					static_cast<size_t>(placement.rect.width)
						* placement.rect.height * bytesPerPixel;
				if (source.bytes != expectedBytes)
					return false;
				if (loadedIdentity != source.snapshot.get())
				{
					loadedPixels.clear();
					loadedIdentity = source.snapshot.get();
					if (!source.snapshot->pixels.empty())
					{
						sourcePixels = &source.snapshot->pixels;
					}
					else
					{
						if (!LoadCompactAtlasSnapshotPixels(
							*source.snapshot, loadedPixels))
						{
							return false;
						}
						sourcePixels = &loadedPixels;
					}
				}
				if (!sourcePixels || source.sourceOffset > sourcePixels->size()
					|| source.bytes
						> sourcePixels->size() - source.sourceOffset)
				{
					return false;
				}
				const UInt8* current =
					sourcePixels->data() + source.sourceOffset;
				payloadChecksum = HashAtlasBytes(
					current, source.bytes, payloadChecksum);
				pageContentHash = HashAtlasBytes(
					&placement.rect, sizeof(placement.rect),
					pageContentHash);
				pageContentHash = HashAtlasBytes(
					current, source.bytes, pageContentHash);

				size_t remaining = source.bytes;
				while (remaining)
				{
					const size_t available =
						kOutputBufferBytes - output.size();
					const size_t copied =
						std::min(remaining, available);
					output.insert(output.end(), current,
						current + copied);
					current += copied;
					remaining -= copied;
					if (output.size() == kOutputBufferBytes
						&& !flush())
					{
						return false;
					}
				}
				destinationOffset += source.bytes;
			}
			return destinationOffset == page.header.pixelBytes
				&& flush() && SetEndOfFile(file) != FALSE;
		}

		bool IsSnapshotHeaderEnvelopeValid(const AtlasSnapshotHeader& header)
		{
			const UInt8 magic[8] =
				{ 'T', 'N', 'V', 'F', 'A', 'T', 'L', '9' };
			return std::memcmp(header.magic, magic, sizeof(magic)) == 0
				&& header.version == kAtlasSnapshotVersion
				&& header.headerSize == sizeof(header)
				&& !(header.flags & ~kAtlasSnapshotKnownFlags)
				&& header.checksum == HashAtlasBytes(&header,
					offsetof(AtlasSnapshotHeader, checksum));
		}

		bool ReadPhysicalSnapshotMetadata(const std::wstring& path,
			AtlasSnapshotHeader& header,
			std::vector<AtlasSnapshotPlacement>* placements,
			UInt64& fileBytes)
		{
			if (placements)
				placements->clear();
			HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
				OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return false;
			LARGE_INTEGER size = {};
			const bool validSize = GetFileSizeEx(file, &size) && size.QuadPart > 0
				&& size.QuadPart <= 512ll * 1024ll * 1024ll;
			if (!validSize
				|| !ReadSnapshotBytesExact(file, &header, sizeof(header))
				|| !IsSnapshotHeaderEnvelopeValid(header)
				|| (header.flags & kAtlasSnapshotFlagPhysicalPayloadAlias))
			{
				CloseHandle(file);
				return false;
			}
			fileBytes = static_cast<UInt64>(size.QuadPart);
			const UInt64 placementBytes =
				static_cast<UInt64>(header.placementCount)
					* sizeof(AtlasSnapshotPlacement);
			const UInt64 payloadOffset = sizeof(header) + placementBytes;
			if (payloadOffset < sizeof(header)
				|| header.storedPixelBytes
					> std::numeric_limits<UInt64>::max() - payloadOffset
				|| payloadOffset + header.storedPixelBytes != fileBytes
				|| placementBytes > std::numeric_limits<size_t>::max())
			{
				CloseHandle(file);
				return false;
			}
			bool result = true;
			if (placements)
			{
				placements->resize(header.placementCount);
				result = ReadSnapshotBytesExact(file, placements->data(),
					static_cast<size_t>(placementBytes));
				if (!result)
					placements->clear();
			}
			CloseHandle(file);
			return result;
		}

		bool HasMatchingSnapshotPayloadContract(
			const AtlasSnapshotHeader& logical,
			const AtlasSnapshotHeader& physical)
		{
			constexpr UInt32 kPayloadFlags =
				kAtlasSnapshotFlagGloballyRepacked
				| kAtlasSnapshotFlagSingleAtlas
				| kAtlasSnapshotFlagSingleAtlasOverflow
				| kAtlasSnapshotFlagJointByteRoles
				| kAtlasSnapshotFlagPhysicalFontGroup
				| kAtlasSnapshotFlagPhysicalFontPool;
			return (logical.flags & kPayloadFlags)
					== (physical.flags & kPayloadFlags)
				&& logical.scaleMilli == physical.scaleMilli
				&& logical.width == physical.width
				&& logical.height == physical.height
				&& logical.cursorX == physical.cursorX
				&& logical.cursorY == physical.cursorY
				&& logical.shelfHeight == physical.shelfHeight
				&& logical.padding == physical.padding
				&& logical.mipLevels == physical.mipLevels
				&& logical.pixelMode == physical.pixelMode
				&& logical.renderMode == physical.renderMode
				&& logical.storageMode == physical.storageMode
				&& logical.pageIndex == physical.pageIndex
				&& logical.pageCount == physical.pageCount
				&& logical.placementCount == physical.placementCount
				&& logical.pixelBytes == physical.pixelBytes
				&& logical.storedPixelBytes == physical.storedPixelBytes
				&& logical.payloadChecksum == physical.payloadChecksum
				&& logical.pageContentHash == physical.pageContentHash;
		}

		bool ReadSnapshotMetadata(const std::wstring& path,
			AtlasSnapshotHeader& logicalHeader,
			std::vector<AtlasSnapshotPlacement>* placements,
			SnapshotPayloadSource& payload)
		{
			payload = {};
			if (ReadPhysicalSnapshotMetadata(path, logicalHeader,
				placements, payload.fileBytes))
			{
				payload.path = path;
				payload.header = logicalHeader;
				return true;
			}

			if (placements)
				placements->clear();
			HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
				OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return false;
			LARGE_INTEGER size = {};
			AtlasSnapshotAliasRecord alias = {};
			const bool aliasRead = GetFileSizeEx(file, &size)
				&& size.QuadPart == static_cast<LONGLONG>(
					sizeof(logicalHeader) + sizeof(alias))
				&& ReadSnapshotBytesExact(file, &logicalHeader,
					sizeof(logicalHeader))
				&& ReadSnapshotBytesExact(file, &alias, sizeof(alias));
			CloseHandle(file);

			const UInt8 aliasMagic[8] =
				{ 'T', 'N', 'V', 'F', 'A', 'L', 'I', '1' };
			if (!aliasRead || !IsSnapshotHeaderEnvelopeValid(logicalHeader)
				|| !(logicalHeader.flags
					& kAtlasSnapshotFlagPhysicalPayloadAlias)
				|| std::memcmp(alias.magic, aliasMagic,
					sizeof(aliasMagic)) != 0
				|| alias.version != kAtlasSnapshotAliasVersion
				|| alias.recordSize != sizeof(alias)
				|| !alias.physicalSnapshotHash
				|| alias.physicalPageIndex >= kMaximumAtlasSnapshotPages
				|| alias.reserved
				|| alias.checksum != HashAtlasBytes(&alias,
					offsetof(AtlasSnapshotAliasRecord, checksum)))
			{
				return false;
			}

			const size_t separator = path.find_last_of(L"\\/");
			if (separator == std::wstring::npos)
				return false;
			const std::wstring physicalPath = BuildAtlasSnapshotFilePath(
				path.substr(0, separator), alias.physicalSnapshotHash,
				alias.physicalPageIndex);
			if (physicalPath.empty()
				|| _wcsicmp(physicalPath.c_str(), path.c_str()) == 0)
			{
				return false;
			}

			AtlasSnapshotHeader physicalHeader = {};
			UInt64 physicalFileBytes = 0;
			if (!ReadPhysicalSnapshotMetadata(physicalPath, physicalHeader,
					placements, physicalFileBytes)
				|| physicalHeader.snapshotHash
					!= alias.physicalSnapshotHash
				|| physicalHeader.pageIndex != alias.physicalPageIndex
				|| physicalHeader.pixelBytes != alias.physicalPixelBytes
				|| physicalHeader.payloadChecksum
					!= alias.physicalPayloadChecksum
				|| physicalHeader.pageContentHash
					!= alias.physicalPageContentHash
				|| physicalHeader.storedPixelBytes
					!= alias.physicalStoredPixelBytes
				|| physicalHeader.placementCount
					!= alias.physicalPlacementCount
				|| !HasMatchingSnapshotPayloadContract(
					logicalHeader, physicalHeader))
			{
				if (placements)
					placements->clear();
				return false;
			}

			payload.path = physicalPath;
			payload.header = physicalHeader;
			payload.fileBytes = physicalFileBytes;
			return true;
		}

		bool InspectSnapshotRoleStorage(RuntimeFont& runtime,
			const AtlasCacheKey& baseKey, size_t& storageBytes)
		{
			storageBytes = 0;
			UInt16 pageCount = 0;
			UInt64 snapshotHash = 0;
			UInt64 maskContentHash = 0;
			for (UInt16 pageIndex = 0;
				pageIndex == 0 || pageIndex < pageCount; ++pageIndex)
			{
				AtlasCacheKey pageKey = baseKey;
				pageKey.pageIndex = pageIndex;
				UInt64 pageSnapshotHash = 0;
				UInt64 pageMaskContentHash = 0;
				const std::wstring path = GetAtlasSnapshotPath(runtime, pageKey,
					pageSnapshotHash, pageMaskContentHash);
				auto reject = [&](const char* reason,
					const AtlasSnapshotHeader& rejectedHeader,
					UInt64 fileBytes, DWORD fileError)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: atlas snapshot storage rejected font=%u role=%s page=%u reason=%s fileError=%lu fileBytes=%llu version=%u/%u expectedSnapshot=%016llX actualSnapshot=%016llX expectedMask=%016llX actualMask=%016llX expectedAtlas=%016llX actualAtlas=%016llX scale=%u/%u pixelMode=%u/%u renderMode=%u/%u byteClass=%u/%u padding=%u/%u flags=%08X size=%ux%u pageCount=%u placements=%u storedBytes=%llu",
						GetRuntimeConfig(runtime).fontId,
						pageKey.byteClass == VectorFontByteClass::DoubleByte
							? "doubleByte" : "singleByte",
						pageIndex, reason ? reason : "unknown", fileError,
						static_cast<unsigned long long>(fileBytes),
						kAtlasSnapshotVersion, rejectedHeader.version,
						static_cast<unsigned long long>(pageSnapshotHash),
						static_cast<unsigned long long>(
							rejectedHeader.snapshotHash),
						static_cast<unsigned long long>(pageMaskContentHash),
						static_cast<unsigned long long>(
							rejectedHeader.maskContentHash),
						static_cast<unsigned long long>(pageKey.atlasContentHash),
						static_cast<unsigned long long>(
							rejectedHeader.atlasContentHash),
						pageKey.scaleMilli, rejectedHeader.scaleMilli,
						static_cast<UInt32>(pageKey.pixelMode),
						static_cast<UInt32>(rejectedHeader.pixelMode),
						static_cast<UInt32>(pageKey.renderMode),
						static_cast<UInt32>(rejectedHeader.renderMode),
						static_cast<UInt32>(pageKey.byteClass),
						static_cast<UInt32>(rejectedHeader.byteClass),
						pageKey.padding, rejectedHeader.padding,
						rejectedHeader.flags, rejectedHeader.width,
						rejectedHeader.height, rejectedHeader.pageCount,
						rejectedHeader.placementCount,
						static_cast<unsigned long long>(
							rejectedHeader.storedPixelBytes));
					return false;
				};
				if (path.empty())
					return reject("path", {}, 0, GetLastError());
				AtlasSnapshotHeader header = {};
				SnapshotPayloadSource payload;
				if (!ReadSnapshotMetadata(path, header, nullptr, payload))
				{
					AtlasSnapshotHeader rawHeader = {};
					UInt64 rawFileBytes = 0;
					DWORD fileError = ERROR_SUCCESS;
					HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
						FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
						nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
					if (file == INVALID_HANDLE_VALUE)
						fileError = GetLastError();
					else
					{
						LARGE_INTEGER size = {};
						if (GetFileSizeEx(file, &size) && size.QuadPart >= 0)
							rawFileBytes = static_cast<UInt64>(size.QuadPart);
						if (!ReadSnapshotBytesExact(
								file, &rawHeader, sizeof(rawHeader)))
							fileError = GetLastError();
						CloseHandle(file);
					}
					return reject("metadata-or-alias", rawHeader,
						rawFileBytes, fileError);
				}
				MarkFreeTypeFontCacheFileUsed(payload.path);
				if (!pageIndex)
				{
					pageCount = header.pageCount;
					snapshotHash = pageSnapshotHash;
					maskContentHash = pageMaskContentHash;
				}
				const UInt64 placementBytes =
					static_cast<UInt64>(header.placementCount)
						* sizeof(AtlasSnapshotPlacement);
				const UInt64 expectedFileBytes = sizeof(header) + placementBytes
					+ header.storedPixelBytes;
				const SnapshotPackingCaps packingCaps =
					GetSnapshotPackingCaps();
				const VectorFontByteClass packingByteClass =
					(header.flags & kAtlasSnapshotFlagJointByteRoles)
						? VectorFontByteClass::DoubleByte : pageKey.byteClass;
				const bool shapeValid = IsSnapshotPageShapeValid(
					header.width, header.height,
					GetSnapshotMaximumSize(
						packingCaps, packingByteClass), packingCaps)
					&& header.mipLevels >= 1
					&& header.mipLevels <= kMaximumAtlasMipLevels;
				const char* rejection = nullptr;
				if (!pageCount || pageCount > kMaximumAtlasSnapshotPages)
					rejection = "page-count";
				else if (header.pageIndex != pageIndex)
					rejection = "page-index";
				else if (header.pageCount != pageCount)
					rejection = "page-count-consistency";
				else if (header.version != kAtlasSnapshotVersion)
					rejection = "version";
				else if ((header.flags & ~kAtlasSnapshotKnownFlags) != 0)
					rejection = "flags";
				else if (!MatchesDefaultPoolSnapshotLayout(header))
					rejection = "default-pool-layout";
				else if (pageSnapshotHash != snapshotHash
					|| header.snapshotHash != snapshotHash)
					rejection = "snapshot-identity";
				else if (pageMaskContentHash != maskContentHash
					|| header.maskContentHash != maskContentHash)
					rejection = "mask-identity";
				else if (header.atlasContentHash != pageKey.atlasContentHash)
					rejection = "atlas-identity";
				else if (header.scaleMilli != pageKey.scaleMilli)
					rejection = "scale";
				else if (header.pixelMode
					!= static_cast<UInt8>(pageKey.pixelMode))
					rejection = "pixel-mode";
				else if (header.renderMode
					!= static_cast<UInt8>(pageKey.renderMode))
					rejection = "render-mode";
				else if (header.byteClass
					!= static_cast<UInt8>(pageKey.byteClass))
					rejection = "byte-class";
				else if (header.padding != pageKey.padding)
					rejection = "padding";
				else if (!shapeValid)
					rejection = "shape";
				else if (header.mipLevels != GetAtlasMipLevelCount(
					header.width, header.height, pageKey.levelZeroOnly))
					rejection = "mip-levels";
				else if (expectedFileBytes != payload.fileBytes)
					rejection = "payload-size";
				else if (header.checksum != HashAtlasBytes(&header,
					offsetof(AtlasSnapshotHeader, checksum)))
					rejection = "header-checksum";
				if (rejection)
					return reject(rejection, header, payload.fileBytes, 0);
				const size_t pageBytes = GetAtlasStorageBytes(header.width,
					header.height, pageKey.pixelMode, header.mipLevels);
				if (pageBytes > std::numeric_limits<size_t>::max() - storageBytes)
					return reject("storage-overflow", header,
						payload.fileBytes, ERROR_ARITHMETIC_OVERFLOW);
				storageBytes += pageBytes;
			}
			return storageBytes != 0;
		}
	}
}

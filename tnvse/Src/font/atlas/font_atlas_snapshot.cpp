#include "font_atlas_internal.h"

#include "font_manager.h"
#include "load_config.h"
#include "native_calls.h"

#include "NiDX9Renderer.hpp"
#include "NiFixedString.hpp"
#include "NiGlobalStringTable.hpp"
#include "NiDX9TextureData.hpp"
#include "NiTriShapeData.hpp"
#include "Utils/SafeWrite.h"

#define STBRP_STATIC
#define STB_RECT_PACK_IMPLEMENTATION
#include "third_party/stb/stb_rect_pack.h"

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
	namespace
	{
		UInt64 HashAtlasBytes(const void* data, size_t size,
			UInt64 hash = 1469598103934665603ull)
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

		struct PhysicalAtlasGroupMember
		{
			const FontConfig* config = nullptr;
			AtlasCacheKey singleByteKey;
			AtlasCacheKey doubleByteKey;
		};

		struct PhysicalAtlasGroup
		{
			UInt64 identity = 0;
			UInt64 doubleByteLayoutHash = 0;
			UInt32 ownerFontId = 0;
			std::vector<PhysicalAtlasGroupMember> members;
			std::vector<AtlasProfileKey> uniqueSingleByteProfiles;
		};

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
			const size_t doubleRoleIndex = static_cast<size_t>(
				VectorFontByteClass::DoubleByte);
			const UInt64 doubleByteLayoutHash =
				anchor.layoutRoleHashes[doubleRoleIndex];
			if (!doubleByteLayoutHash)
				return false;
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
					|| config->layoutRoleHashes[doubleRoleIndex]
						!= doubleByteLayoutHash
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
			}
			// Identical single- and double-byte profiles already share a complete
			// page through the existing content-addressed cache. A physical font
			// group is useful only when at least two single-byte profiles differ.
			if (group.uniqueSingleByteProfiles.size() < 2)
				return false;
			std::sort(group.uniqueSingleByteProfiles.begin(),
				group.uniqueSingleByteProfiles.end(), LessAtlasProfileKey);

			group.ownerFontId = group.members.front().config->fontId;
			group.doubleByteLayoutHash = doubleByteLayoutHash;
			constexpr UInt32 kPhysicalAtlasGroupRevision = 1;
			UInt64 hash = HashAtlasBytes(&kPhysicalAtlasGroupRevision,
				sizeof(kPhysicalAtlasGroupRevision));
			hash = HashAtlasProfileKey(doubleProfile, hash);
			hash = HashAtlasBytes(&group.doubleByteLayoutHash,
				sizeof(group.doubleByteLayoutHash), hash);
			const UInt32 profileCount = static_cast<UInt32>(
				group.uniqueSingleByteProfiles.size());
			hash = HashAtlasBytes(&profileCount, sizeof(profileCount), hash);
			for (const AtlasProfileKey& profile :
				group.uniqueSingleByteProfiles)
			{
				hash = HashAtlasProfileKey(profile, hash);
			}
			group.identity = hash;
			return group.identity != 0;
		}

		bool IsPhysicalAtlasGroupResidentLocked(
			const PhysicalAtlasGroup& group,
			std::shared_ptr<AtlasResource>* sharedResource = nullptr)
		{
			if (sharedResource)
				sharedResource->reset();
			if (group.members.empty())
				return false;

			std::vector<AtlasCacheKey> roleKeys;
			std::unordered_set<AtlasProfileKey, AtlasProfileKeyHash>
				roleProfiles;
			for (const PhysicalAtlasGroupMember& member : group.members)
			{
				const AtlasProfileKey singleProfile =
					MakeAtlasProfileKey(member.singleByteKey);
				if (roleProfiles.insert(singleProfile).second)
					roleKeys.push_back(member.singleByteKey);
			}
			const AtlasCacheKey& doubleKey =
				group.members.front().doubleByteKey;
			if (roleProfiles.insert(MakeAtlasProfileKey(doubleKey)).second)
				roleKeys.push_back(doubleKey);

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
					return false;
				}
				roleKey.pageIndex = 0;
				const auto page = state.atlasCache.find(roleKey);
				if (page == state.atlasCache.end()
					|| !page->second.resource
					|| !page->second.resource->compactSnapshot
					|| !(page->second.resource->compactSnapshot
						->sourceHeader.flags
						& kAtlasSnapshotFlagPhysicalFontGroup)
					|| !page->second.resource->pageContentHash)
				{
					return false;
				}
				if (!first)
				{
					first = page->second.resource;
					pageContentHash = first->pageContentHash;
				}
				else if (page->second.resource->pageContentHash
						!= pageContentHash
					|| !AreAtlasResourcesBackedBySameTexture(
						*first, *page->second.resource))
				{
					return false;
				}
			}
			if (sharedResource)
				*sharedResource = std::move(first);
			return first != nullptr;
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

		struct SnapshotPackingCaps
		{
			UInt32 singleByteMaximum = kSingleByteAtlasHardLimit;
			UInt32 doubleByteMaximum = kDoubleByteAtlasHardLimit;
			UInt32 maximumAspectRatio = 0;
		};

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
			const AtlasCacheKey& baseKey, UInt16* pageCount = nullptr,
			UInt64* placementCount = nullptr)
		{
			const AtlasProfileKey profileKey = MakeAtlasProfileKey(baseKey);
			const auto profile = state.atlasProfiles.find(profileKey);
			if (profile == state.atlasProfiles.end() || profile->second.pages.empty())
				return false;
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
					|| !page->second.resource->property
					|| page->second.resource->glyphs.empty())
				{
					return false;
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

		bool BuildAtlasSnapshotPixels(const AtlasResource& resource,
			const std::vector<AtlasSnapshotPlacement>& placements,
			AtlasSnapshotStorage storageMode, std::vector<UInt8>& pixels)
		{
			const UInt32 bytesPerPixel = AtlasBytesPerPixel(resource.pixelMode);
			pixels.clear();
			if (storageMode == AtlasSnapshotStorage::PlacedLevelZeroRects)
			{
				size_t packedBytes = 0;
				if (!GetPlacedLevelZeroSnapshotBytes(placements,
					resource.width, resource.height, resource.pixelMode, packedBytes))
				{
					return false;
				}
				const UInt8* source = nullptr;
				size_t sourcePitch = static_cast<size_t>(resource.width) * bytesPerPixel;
				std::vector<UInt8> reconstructed;
				if (resource.pixelData && resource.pixelData->m_pucPixels
					&& resource.pixelData->m_puiWidth && resource.pixelData->m_puiHeight
					&& resource.pixelData->m_puiOffsetInBytes
					&& resource.pixelData->m_uiMipmapLevels
					&& resource.pixelData->m_puiWidth[0] == resource.width
					&& resource.pixelData->m_puiHeight[0] == resource.height)
				{
					source = resource.pixelData->m_pucPixels
						+ resource.pixelData->m_puiOffsetInBytes[0];
				}
				else
				{
					reconstructed.assign(static_cast<size_t>(resource.width)
						* resource.height * bytesPerPixel, 0);
					if (!WriteCompactSnapshotPixels(reconstructed.data(),
						static_cast<LONG>(sourcePitch), resource.pixelMode, resource))
					{
						return false;
					}
					for (const AtlasGlyphRecord& glyph : resource.glyphs)
					{
						const std::shared_ptr<const GlyphBitmap>& bitmap = glyph.bitmap;
						if (!bitmap || bitmap->alpha.empty())
							continue;
						WriteBitmapPixels(reconstructed.data(),
							static_cast<LONG>(sourcePitch), resource.pixelMode,
							*bitmap, glyph.rect,
							glyph.rect.x, glyph.rect.y);
					}
					source = reconstructed.data();
				}

				pixels.resize(packedBytes);
				size_t destinationOffset = 0;
				for (const AtlasSnapshotPlacement& placement : placements)
				{
					const AtlasRect& rect = placement.rect;
					const size_t rowBytes = static_cast<size_t>(rect.width) * bytesPerPixel;
					for (UInt32 row = 0; row < rect.height; ++row)
					{
						const UInt8* begin = source
							+ static_cast<size_t>(rect.y + row) * sourcePitch
							+ static_cast<size_t>(rect.x) * bytesPerPixel;
						std::memcpy(pixels.data() + destinationOffset, begin, rowBytes);
						destinationOffset += rowBytes;
					}
				}
				return destinationOffset == packedBytes;
			}
			if (storageMode != AtlasSnapshotStorage::FullMipChain)
				return false;
			pixels.reserve(GetAtlasStorageBytes(resource.width, resource.height,
				resource.pixelMode, resource.mipLevels));
			if (resource.pixelData && resource.pixelData->m_pucPixels
				&& resource.pixelData->m_puiOffsetInBytes)
			{
				for (UInt32 level = 0; level < resource.mipLevels; ++level)
				{
					const UInt32 width = resource.pixelData->m_puiWidth[level];
					const UInt32 height = resource.pixelData->m_puiHeight[level];
					const size_t bytes = static_cast<size_t>(width) * height * bytesPerPixel;
					const UInt8* source = resource.pixelData->m_pucPixels
						+ resource.pixelData->m_puiOffsetInBytes[level];
					pixels.insert(pixels.end(), source, source + bytes);
				}
				return pixels.size() == GetAtlasStorageBytes(resource.width,
					resource.height, resource.pixelMode, resource.mipLevels);
			}

			std::vector<UInt8> current(static_cast<size_t>(resource.width)
				* resource.height * bytesPerPixel, 0);
			for (const AtlasGlyphRecord& glyph : resource.glyphs)
			{
				const std::shared_ptr<const GlyphBitmap>& bitmap = glyph.bitmap;
				if (!bitmap || bitmap->alpha.empty())
					continue;
				WriteBitmapPixels(current.data(),
					static_cast<LONG>(resource.width * bytesPerPixel), resource.pixelMode,
					*bitmap, glyph.rect, glyph.rect.x, glyph.rect.y);
			}
			UInt32 width = resource.width;
			UInt32 height = resource.height;
			pixels.insert(pixels.end(), current.begin(), current.end());
			for (UInt32 level = 1; level < resource.mipLevels; ++level)
			{
				std::vector<UInt8> next;
				if (!BuildNextMipLevel(current.data(), width, height,
					static_cast<size_t>(width) * bytesPerPixel,
					resource.pixelMode, next))
					return false;
				pixels.insert(pixels.end(), next.begin(), next.end());
				current.swap(next);
				width = std::max<UInt32>(1, width / 2);
				height = std::max<UInt32>(1, height / 2);
			}
			return true;
		}

		struct SnapshotGlyphData
		{
			AtlasSnapshotPlacement placement;
			std::shared_ptr<const CompactAtlasSnapshot> sourceSnapshot;
			size_t sourcePageOrdinal = 0;
			size_t sourceOffset = 0;
			size_t sourceBytes = 0;
			VectorFontByteClass byteClass = VectorFontByteClass::SingleByte;
		};

		struct SnapshotPixelSource
		{
			std::shared_ptr<const CompactAtlasSnapshot> snapshot;
			size_t sourceOffset = 0;
			size_t destinationOffset = 0;
			size_t bytes = 0;
		};

		struct SnapshotPageData
		{
			AtlasCacheKey key;
			AtlasSnapshotHeader header;
			std::vector<AtlasSnapshotPlacement> placements;
			std::vector<UInt8> pixels;
			std::vector<SnapshotPixelSource> pixelSources;
			std::wstring path;
		};

		struct SnapshotPackedGlyph
		{
			size_t glyphIndex = 0;
			AtlasRect rect;
		};

		struct SnapshotPackingPage
		{
			UInt32 width = 0;
			UInt32 height = 0;
			std::vector<SnapshotPackedGlyph> glyphs;
		};

		struct SnapshotPackingPlan
		{
			std::vector<SnapshotPackingPage> pages;
			UInt64 gpuBytes = 0;
			UInt32 targetWidth = 0;
			int heuristic = STBRP_HEURISTIC_Skyline_BL_sortHeight;
		};

		UInt32 QuantizeSnapshotExtent(UInt32 value, UInt32 maximumSize)
		{
			value = std::max<UInt32>(64, value);
			UInt32 result = 64;
			while (result < value && result <= maximumSize / 2u)
				result <<= 1;
			return result >= value && result <= maximumSize ? result : 0;
		}

		bool ResolveSnapshotPageDimensions(UInt32 usedWidth, UInt32 usedHeight,
			UInt32 maximumSize, const SnapshotPackingCaps& caps,
			UInt32& width, UInt32& height)
		{
			width = QuantizeSnapshotExtent(usedWidth, maximumSize);
			height = QuantizeSnapshotExtent(usedHeight, maximumSize);
			if (!width || !height)
				return false;
			if (caps.maximumAspectRatio)
			{
				if (static_cast<UInt64>(width)
					> static_cast<UInt64>(height) * caps.maximumAspectRatio)
				{
					const UInt32 requiredHeight = static_cast<UInt32>(
						(static_cast<UInt64>(width)
							+ caps.maximumAspectRatio - 1u)
						/ caps.maximumAspectRatio);
					height = QuantizeSnapshotExtent(
						requiredHeight, maximumSize);
				}
				else if (static_cast<UInt64>(height)
					> static_cast<UInt64>(width) * caps.maximumAspectRatio)
				{
					const UInt32 requiredWidth = static_cast<UInt32>(
						(static_cast<UInt64>(height)
							+ caps.maximumAspectRatio - 1u)
						/ caps.maximumAspectRatio);
					width = QuantizeSnapshotExtent(
						requiredWidth, maximumSize);
				}
			}
			return width && height && IsSnapshotPageShapeValid(
				width, height, maximumSize, caps);
		}

		std::vector<UInt32> BuildSnapshotCandidateWidths(
			const std::vector<SnapshotGlyphData>& glyphs, UInt32 padding,
			UInt32 maximumSize)
		{
			UInt32 minimumWidth = 64;
			for (const SnapshotGlyphData& glyph : glyphs)
			{
				const UInt32 glyphWidth = glyph.placement.rect.width;
				if (padding > maximumSize / 2u
					|| glyphWidth > maximumSize - padding * 2u)
				{
					return {};
				}
				minimumWidth = std::max(
					minimumWidth, glyphWidth + padding * 2u);
			}

			std::vector<UInt32> widths;
			for (UInt32 width = 64; width <= maximumSize;)
			{
				if (width >= minimumWidth)
					widths.push_back(width);
				if (width > maximumSize / 2u)
					break;
				width <<= 1;
			}
			// Establish the likely minimum page count first. Once a wider plan
			// succeeds, narrower candidates can stop as soon as they exceed it.
			std::reverse(widths.begin(), widths.end());
			return widths;
		}

		bool BuildSnapshotPackingPlan(
			const std::vector<SnapshotGlyphData>& glyphs,
			UInt32 padding, UInt32 maximumSize,
			AtlasPixelMode pixelMode, bool levelZeroOnly,
			const SnapshotPackingCaps& caps, UInt32 targetWidth,
			int heuristic, size_t maximumAcceptedPages,
			SnapshotPackingPlan& plan)
		{
			plan = {};
			plan.targetWidth = targetWidth;
			plan.heuristic = heuristic;
			const size_t pageLimit = maximumAcceptedPages
				? std::min(maximumAcceptedPages,
					static_cast<size_t>(kMaximumAtlasSnapshotPages))
				: static_cast<size_t>(kMaximumAtlasSnapshotPages);
			std::vector<size_t> remaining;
			remaining.reserve(glyphs.size());
			for (size_t index = 0; index < glyphs.size(); ++index)
				remaining.push_back(index);

			while (!remaining.empty())
			{
				if (plan.pages.size() >= pageLimit
					|| targetWidth > static_cast<UInt32>(
						std::numeric_limits<int>::max())
					|| maximumSize > static_cast<UInt32>(
						std::numeric_limits<int>::max()))
				{
					return false;
				}

				std::vector<stbrp_node> nodes(targetWidth);
				stbrp_context context = {};
				stbrp_init_target(&context, static_cast<int>(targetWidth),
					static_cast<int>(maximumSize), nodes.data(),
					static_cast<int>(nodes.size()));
				stbrp_setup_heuristic(&context, heuristic);

				SnapshotPackingPage page;
				page.glyphs.reserve(remaining.size());
				std::vector<size_t> nextRemaining;
				nextRemaining.reserve(remaining.size());
				UInt32 usedWidth = 0;
				UInt32 usedHeight = 0;
				for (size_t glyphIndex : remaining)
				{
					const AtlasRect& sourceRect =
						glyphs[glyphIndex].placement.rect;
					if (!sourceRect.width || !sourceRect.height
						|| padding > maximumSize / 2u
						|| sourceRect.width > targetWidth - std::min(
							targetWidth, padding * 2u)
						|| sourceRect.height > maximumSize - padding * 2u)
					{
						nextRemaining.push_back(glyphIndex);
						continue;
					}

					stbrp_rect packedRect = {};
					packedRect.w = static_cast<stbrp_coord>(
						sourceRect.width + padding * 2u);
					packedRect.h = static_cast<stbrp_coord>(
						sourceRect.height + padding * 2u);
					// Feed the stable height/width/cacheId order one rectangle
					// at a time so equal rectangles never depend on stb's qsort.
					stbrp_pack_rects(&context, &packedRect, 1);
					if (!packedRect.was_packed)
					{
						nextRemaining.push_back(glyphIndex);
						continue;
					}

					AtlasRect destination = {
						static_cast<UInt32>(packedRect.x) + padding,
						static_cast<UInt32>(packedRect.y) + padding,
						sourceRect.width,
						sourceRect.height
					};
					page.glyphs.push_back({ glyphIndex, destination });
					usedWidth = std::max(usedWidth,
						static_cast<UInt32>(packedRect.x + packedRect.w));
					usedHeight = std::max(usedHeight,
						static_cast<UInt32>(packedRect.y + packedRect.h));
				}
				if (page.glyphs.empty()
					|| !ResolveSnapshotPageDimensions(
						usedWidth, usedHeight, maximumSize, caps,
						page.width, page.height))
				{
					return false;
				}

				const UInt32 mipLevels = GetAtlasMipLevelCount(
					page.width, page.height, levelZeroOnly);
				const UInt64 pageBytes = GetAtlasStorageBytes(
					page.width, page.height, pixelMode, mipLevels);
				if (pageBytes > std::numeric_limits<UInt64>::max()
					- plan.gpuBytes)
				{
					return false;
				}
				plan.gpuBytes += pageBytes;
				plan.pages.push_back(std::move(page));
				remaining.swap(nextRemaining);
			}
			if (plan.pages.empty())
				return false;

			std::vector<UInt8> seen(glyphs.size(), 0);
			size_t packedGlyphs = 0;
			UInt64 verifiedBytes = 0;
			for (const SnapshotPackingPage& page : plan.pages)
			{
				if (!IsSnapshotPageShapeValid(
					page.width, page.height, maximumSize, caps))
				{
					return false;
				}
				const UInt32 mipLevels = GetAtlasMipLevelCount(
					page.width, page.height, levelZeroOnly);
				const UInt64 pageBytes = GetAtlasStorageBytes(
					page.width, page.height, pixelMode, mipLevels);
				if (pageBytes > std::numeric_limits<UInt64>::max()
					- verifiedBytes)
				{
					return false;
				}
				verifiedBytes += pageBytes;
				for (const SnapshotPackedGlyph& packed : page.glyphs)
				{
					if (packed.glyphIndex >= glyphs.size()
						|| seen[packed.glyphIndex])
						return false;
					const AtlasRect& source =
						glyphs[packed.glyphIndex].placement.rect;
					const AtlasRect& destination = packed.rect;
					if (destination.width != source.width
						|| destination.height != source.height
						|| destination.x < padding
						|| destination.y < padding
						|| static_cast<UInt64>(destination.x)
							+ destination.width + padding > page.width
						|| static_cast<UInt64>(destination.y)
							+ destination.height + padding > page.height)
					{
						return false;
					}
					seen[packed.glyphIndex] = 1;
					++packedGlyphs;
				}
			}
			return packedGlyphs == glyphs.size()
				&& verifiedBytes == plan.gpuBytes;
		}

		UInt32 GetSnapshotPackingPlanMaximumEdge(
			const SnapshotPackingPlan& plan)
		{
			UInt32 edge = 0;
			for (const SnapshotPackingPage& page : plan.pages)
				edge = std::max(edge, std::max(page.width, page.height));
			return edge;
		}

		bool IsBetterSnapshotPackingPlan(const SnapshotPackingPlan& candidate,
			const SnapshotPackingPlan& current)
		{
			if (candidate.pages.empty())
				return false;
			if (current.pages.empty())
				return true;
			if (candidate.pages.size() != current.pages.size())
				return candidate.pages.size() < current.pages.size();
			if (candidate.gpuBytes != current.gpuBytes)
				return candidate.gpuBytes < current.gpuBytes;
			const UInt32 candidateEdge =
				GetSnapshotPackingPlanMaximumEdge(candidate);
			const UInt32 currentEdge =
				GetSnapshotPackingPlanMaximumEdge(current);
			if (candidateEdge != currentEdge)
				return candidateEdge < currentEdge;
			if (candidate.targetWidth != current.targetWidth)
				return candidate.targetWidth < current.targetWidth;
			return candidate.heuristic < current.heuristic;
		}

		UInt64 ComputeAtlasPageContentHash(const AtlasSnapshotHeader& header,
			const std::vector<AtlasSnapshotPlacement>& placements,
			const std::vector<UInt8>& pixels)
		{
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
			const PageIdentity identity = { header.width, header.height, header.padding,
				header.mipLevels, header.pixelMode, header.renderMode,
				header.storageMode,
				static_cast<UInt8>(header.mipLevels == 1 ? 1 : 0) };
			UInt64 hash = HashAtlasBytes(&identity, sizeof(identity));
			if (header.storageMode
				== static_cast<UInt8>(AtlasSnapshotStorage::FullMipChain))
			{
				return HashAtlasBytes(pixels.data(), pixels.size(), hash);
			}
			size_t offset = 0;
			const size_t bytesPerPixel = AtlasBytesPerPixel(
				static_cast<AtlasPixelMode>(header.pixelMode));
			for (const AtlasSnapshotPlacement& placement : placements)
			{
				const size_t bytes = static_cast<size_t>(placement.rect.width)
					* placement.rect.height * bytesPerPixel;
				if (offset > pixels.size() || bytes > pixels.size() - offset)
					return 0;
				// v18 placed snapshots are serialized in deterministic placement
				// order. Hash that same stream so a 200+ MiB single atlas can be
				// written and validated incrementally without materializing it.
				hash = HashAtlasBytes(&placement.rect,
					sizeof(placement.rect), hash);
				hash = HashAtlasBytes(pixels.data() + offset, bytes, hash);
				offset += bytes;
			}
			return offset == pixels.size() ? hash : 0;
		}

		bool BuildRepackedSnapshotPages(const AtlasCacheKey& baseKey,
			const std::vector<std::pair<AtlasCacheKey,
				std::shared_ptr<AtlasResource>>>& resources,
			std::vector<SnapshotPageData>& pages, UInt64& originalGpuBytes,
			VectorFontByteClass packingByteClass)
		{
			std::vector<SnapshotGlyphData> glyphs;
			std::unordered_set<UInt64> cacheIds;
			originalGpuBytes = 0;
			bool hasSingleByte = false;
			bool hasDoubleByte = false;
			for (size_t resourceIndex = 0; resourceIndex < resources.size();
				++resourceIndex)
			{
				const auto& item = resources[resourceIndex];
				hasSingleByte |= item.first.byteClass
					== VectorFontByteClass::SingleByte;
				hasDoubleByte |= item.first.byteClass
					== VectorFontByteClass::DoubleByte;
				const AtlasResource& resource = *item.second;
				if (resource.pixelMode != baseKey.pixelMode
					|| resource.renderMode != baseKey.renderMode
					|| !resource.levelZeroOnly)
					return false;
				originalGpuBytes += GetAtlasStorageBytes(resource.width, resource.height,
					resource.pixelMode, resource.mipLevels);
				const std::shared_ptr<const CompactAtlasSnapshot> sourceSnapshot =
					resource.compactSnapshot;
				if (!sourceSnapshot || sourceSnapshot->placements.empty())
					return false;
				std::vector<size_t> sourceOffsets(sourceSnapshot->placements.size());
				size_t sourceBytes = 0;
				for (size_t index = 0; index < sourceSnapshot->placements.size(); ++index)
				{
					sourceOffsets[index] = sourceBytes;
					const AtlasRect& rect = sourceSnapshot->placements[index].rect;
					const size_t bytes = static_cast<size_t>(rect.width) * rect.height
						* AtlasBytesPerPixel(resource.pixelMode);
					if (!rect.width || !rect.height
						|| bytes > std::numeric_limits<size_t>::max() - sourceBytes)
					{
						return false;
					}
					sourceBytes += bytes;
				}
				const size_t expectedSourceBytes = sourceSnapshot->pixels.empty()
					? static_cast<size_t>(sourceSnapshot->sourceHeader.pixelBytes)
					: sourceSnapshot->pixels.size();
				if (sourceBytes != expectedSourceBytes)
					return false;
				for (const AtlasGlyphRecord& record : resource.glyphs)
				{
					if (record.snapshotPlacementIndex >= sourceSnapshot->placements.size())
						return false;
					const size_t placementIndex = record.snapshotPlacementIndex;
					const AtlasSnapshotPlacement& placement =
						sourceSnapshot->placements[placementIndex];
					const size_t bytes = static_cast<size_t>(placement.rect.width)
						* placement.rect.height * AtlasBytesPerPixel(resource.pixelMode);
					if (placement.cacheId != record.cacheId
						|| std::memcmp(&placement.rect, &record.rect, sizeof(record.rect)) != 0
						|| !cacheIds.insert(record.cacheId).second)
						return false;
					SnapshotGlyphData glyph;
					glyph.placement = placement;
					glyph.sourceSnapshot = sourceSnapshot;
					glyph.sourcePageOrdinal = resourceIndex;
					glyph.sourceOffset = sourceOffsets[placementIndex];
					glyph.sourceBytes = bytes;
					glyph.byteClass = item.first.byteClass;
					glyphs.push_back(std::move(glyph));
				}
			}
			if (glyphs.empty())
				return false;
			const bool mixedByteRoles = hasSingleByte && hasDoubleByte;
			std::sort(glyphs.begin(), glyphs.end(), [mixedByteRoles](
				const auto& lhs, const auto& rhs)
			{
				if (mixedByteRoles && lhs.byteClass != rhs.byteClass)
					return lhs.byteClass == VectorFontByteClass::SingleByte;
				if (lhs.placement.rect.height != rhs.placement.rect.height)
					return lhs.placement.rect.height > rhs.placement.rect.height;
				if (lhs.placement.rect.width != rhs.placement.rect.width)
					return lhs.placement.rect.width > rhs.placement.rect.width;
				return lhs.placement.cacheId < rhs.placement.cacheId;
			});

			const bool preferSingleAtlas =
				g_bEnableFreeTypeDefaultPoolAtlas;
			const SnapshotPackingCaps packingCaps =
				GetSnapshotPackingCaps();
			const UInt32 roleMaximum =
				GetSnapshotMaximumSize(packingCaps, packingByteClass);
			const UInt32 maximumSize = preferSingleAtlas
				? roleMaximum
				: std::min(roleMaximum,
					kMaximumMtsdfPrewarmAtlasSize);
			const UInt32 padding = baseKey.padding;
			const std::vector<UInt32> candidateWidths =
				BuildSnapshotCandidateWidths(glyphs, padding, maximumSize);
			if (candidateWidths.empty())
				return false;

			const int heuristics[] = {
				STBRP_HEURISTIC_Skyline_BL_sortHeight,
				STBRP_HEURISTIC_Skyline_BF_sortHeight
			};
			SnapshotPackingPlan selectedPlan;
			UInt32 attemptedPlans = 0;
			for (UInt32 targetWidth : candidateWidths)
			{
				for (int heuristic : heuristics)
				{
					SnapshotPackingPlan candidate;
					++attemptedPlans;
					if (BuildSnapshotPackingPlan(glyphs, padding,
						maximumSize, baseKey.pixelMode,
						baseKey.levelZeroOnly, packingCaps,
						targetWidth, heuristic,
						selectedPlan.pages.size(), candidate)
						&& IsBetterSnapshotPackingPlan(
							candidate, selectedPlan))
					{
						selectedPlan = std::move(candidate);
					}
				}
			}
			if (selectedPlan.pages.empty())
				return false;

			pages.clear();
			pages.reserve(selectedPlan.pages.size());
			for (size_t pageIndex = 0;
				pageIndex < selectedPlan.pages.size(); ++pageIndex)
			{
				const SnapshotPackingPage& packedPage =
					selectedPlan.pages[pageIndex];
				SnapshotPageData page;
				page.key = baseKey;
				page.key.pageIndex = static_cast<UInt16>(pageIndex);
				page.header.width = packedPage.width;
				page.header.height = packedPage.height;
				page.header.padding = padding;
				// A skyline-packed snapshot cannot resume the runtime shelf cursor.
				// Close the restored page so later glyphs are placed on a fresh page.
				page.header.cursorX = padding;
				page.header.cursorY = page.header.height;
				page.header.shelfHeight = 0;

				std::vector<SnapshotPackedGlyph> ordered =
					packedPage.glyphs;
				std::sort(ordered.begin(), ordered.end(),
					[&glyphs](const auto& lhs, const auto& rhs)
				{
					const SnapshotGlyphData& left =
						glyphs[lhs.glyphIndex];
					const SnapshotGlyphData& right =
						glyphs[rhs.glyphIndex];
					if (left.sourcePageOrdinal != right.sourcePageOrdinal)
						return left.sourcePageOrdinal < right.sourcePageOrdinal;
					if (left.sourceOffset != right.sourceOffset)
						return left.sourceOffset < right.sourceOffset;
					return left.placement.cacheId < right.placement.cacheId;
				});
				size_t destinationBytes = 0;
				for (const SnapshotPackedGlyph& packedGlyph : ordered)
				{
					if (packedGlyph.glyphIndex >= glyphs.size())
						return false;
					const SnapshotGlyphData& glyph =
						glyphs[packedGlyph.glyphIndex];
					AtlasSnapshotPlacement placement = glyph.placement;
					placement.rect = packedGlyph.rect;
					page.placements.push_back(placement);
					page.pixelSources.push_back({ glyph.sourceSnapshot,
						glyph.sourceOffset, destinationBytes, glyph.sourceBytes });
					if (glyph.sourceBytes > std::numeric_limits<size_t>::max()
						- destinationBytes)
					{
						return false;
					}
					destinationBytes += glyph.sourceBytes;
				}
				pages.push_back(std::move(page));
			}
			gLog.FormattedMessage(
				"tnvse_freetype_font: atlas repack selected font=%u role=%s glyphs=%u plans=%u pages=%u targetWidth=%u heuristic=%s gpuBytes=%llu dimensions=power-of-two objective=page-count-then-bytes",
				baseKey.fontId,
				baseKey.byteClass == VectorFontByteClass::DoubleByte
					? "doubleByte" : "singleByte",
				static_cast<UInt32>(glyphs.size()), attemptedPlans,
				static_cast<UInt32>(pages.size()),
				selectedPlan.targetWidth,
				selectedPlan.heuristic
					== STBRP_HEURISTIC_Skyline_BF_sortHeight
					? "best-fit" : "bottom-left",
				static_cast<unsigned long long>(selectedPlan.gpuBytes));
			if (preferSingleAtlas && pages.size() > 1)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: single atlas physical overflow font=%u role=%s pages=%u limit=%ux%u policy=persist-minimum-pages",
					baseKey.fontId,
					baseKey.byteClass == VectorFontByteClass::DoubleByte
						? "doubleByte" : "singleByte",
					static_cast<UInt32>(pages.size()),
					maximumSize, maximumSize);
			}
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

		struct SnapshotPayloadSource
		{
			std::wstring path;
			AtlasSnapshotHeader header;
			UInt64 fileBytes = 0;
		};

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
				| kAtlasSnapshotFlagPhysicalFontGroup;
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
				if (path.empty())
					return false;
				AtlasSnapshotHeader header = {};
				SnapshotPayloadSource payload;
				if (!ReadSnapshotMetadata(path, header, nullptr, payload))
					return false;
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
				if (!pageCount || pageCount > kMaximumAtlasSnapshotPages
					|| header.pageIndex != pageIndex
					|| header.pageCount != pageCount
					|| header.version != kAtlasSnapshotVersion
					|| (header.flags & ~kAtlasSnapshotKnownFlags) != 0
					|| !MatchesDefaultPoolSnapshotLayout(header)
					|| pageSnapshotHash != snapshotHash
					|| pageMaskContentHash != maskContentHash
					|| header.snapshotHash != snapshotHash
					|| header.maskContentHash != maskContentHash
					|| header.atlasContentHash != pageKey.atlasContentHash
					|| header.scaleMilli != pageKey.scaleMilli
					|| header.pixelMode != static_cast<UInt8>(pageKey.pixelMode)
					|| header.renderMode != static_cast<UInt8>(pageKey.renderMode)
					|| header.byteClass != static_cast<UInt8>(pageKey.byteClass)
					|| header.padding != pageKey.padding
					|| !shapeValid
					|| header.mipLevels != GetAtlasMipLevelCount(
						header.width, header.height, pageKey.levelZeroOnly)
					|| expectedFileBytes != payload.fileBytes
					|| header.checksum != HashAtlasBytes(&header,
						offsetof(AtlasSnapshotHeader, checksum)))
				{
					return false;
				}
				const size_t pageBytes = GetAtlasStorageBytes(header.width,
					header.height, pageKey.pixelMode, header.mipLevels);
				if (pageBytes > std::numeric_limits<size_t>::max() - storageBytes)
					return false;
				storageBytes += pageBytes;
			}
			return storageBytes != 0;
		}
	}

	UInt64 BuildAtlasSnapshotIdentityHash(const AtlasCacheKey& key,
		UInt64 maskContentHash, const FontConfig& config)
	{
		return ComputeAtlasSnapshotIdentityHash(
			key, maskContentHash, config);
	}

	bool LoadGlyphAtlasSnapshotRole(RuntimeFont& runtime,
		VectorFontByteClass byteClass, float rasterScale, bool metadataOnly)
	{
		const FontConfig& config = GetRuntimeConfig(runtime);
		AtlasCacheKey key;
		if (!ResolvePrewarmAtlasKey(config, byteClass, rasterScale, key))
			return false;
		if (!metadataOnly && TryReuseCompleteAtlasProfile(key))
			return true;
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
		UInt64 directStreamedPixelBytes = 0;
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
							RegisterDefaultPoolAtlasPage(resource, header.pageContentHash);
						directStreamedPixelBytes += header.pixelBytes;
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
						return false;
					resource->property = property;
					directStreamedPixelBytes += storedPixelVector.size();
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
			const std::vector<UInt16> existingPages = existingProfile
				!= state.atlasProfiles.end() ? existingProfile->second.pages
				: std::vector<UInt16>();
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
				"tnvse_freetype_font: atlas snapshot restored font=%u role=%s pages=%u physicalAliasPages=%u replacedPages=%u defaultPoolPages=%u deduplicatedPages=%u deduplicatedGpuBytes=%llu placements=%llu bytes=%llu directStreamedPixelBytes=%llu",
				key.fontId, key.byteClass == VectorFontByteClass::DoubleByte
					? "doubleByte" : "singleByte", pageCount,
				physicalAliasPages, replacedPages,
				defaultPoolPages, deduplicatedPages,
				static_cast<unsigned long long>(deduplicatedGpuBytes),
				static_cast<unsigned long long>(totalPlacements),
				static_cast<unsigned long long>(totalBytes),
				static_cast<unsigned long long>(directStreamedPixelBytes));
		}
		return true;
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
		if (TryReuseCompleteAtlasProfile(singleByteKey)
			&& (!needsDoubleByte || TryReuseCompleteAtlasProfile(doubleByteKey)))
		{
			return true;
		}

		size_t incomingStorageBytes = 0;
		size_t roleStorageBytes = 0;
		if (!InspectSnapshotRoleStorage(runtime, singleByteKey, roleStorageBytes))
			return false;
		incomingStorageBytes = roleStorageBytes;
		if (needsDoubleByte)
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

		const bool singleByteReady = LoadGlyphAtlasSnapshotRole(runtime,
			VectorFontByteClass::SingleByte, rasterScale, false);
		const bool doubleByteReady = !needsDoubleByte
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

	namespace
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

		bool ResolveStockLetterAtlasRect(const FontLetter& letter,
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
				for (const FontLetter& letter : table.stockGlyphs)
				{
					if (letter.iTextureIndex != pageSlot)
						continue;
					AtlasRect rect;
					if (!ResolveStockLetterAtlasRect(
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
			std::vector<std::pair<AtlasCacheKey,
				std::shared_ptr<AtlasResource>>>& destination,
			std::vector<std::shared_ptr<AtlasResource>>& physicalSources)
		{
			AtlasState& state = State();
			const auto profile = state.atlasProfiles.find(
				MakeAtlasProfileKey(roleKey));
			if (profile == state.atlasProfiles.end()
				|| profile->second.pages.empty()
				|| !profile->second.directGlyphs
				|| profile->second.directGlyphs->pages.size()
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
						*profile->second.directGlyphs,
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
		bool* jointRolePublished = nullptr,
		const PhysicalAtlasGroup* physicalGroup = nullptr,
		bool* physicalGroupFallback = nullptr)
	{
		if (jointRolePublished)
			*jointRolePublished = false;
		if (physicalGroupFallback)
			*physicalGroupFallback = false;
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
				std::unordered_set<AtlasProfileKey, AtlasProfileKeyHash>
					collectedProfiles;
				const auto collectUniqueRole = [&](const AtlasCacheKey& roleKey)
				{
					const AtlasProfileKey profile =
						MakeAtlasProfileKey(roleKey);
					if (!collectedProfiles.insert(profile).second)
						return true;
					return CollectRoleFilteredResourcesLocked(
						roleKey, groupResources, groupPhysicalSources);
				};
				bool collected = physicalGroup
					&& !physicalGroup->members.empty();
				for (const PhysicalAtlasGroupMember& member :
					physicalGroup->members)
				{
					collected = collected
						&& collectUniqueRole(member.singleByteKey);
				}
				if (collected)
				{
					collected = collectUniqueRole(
						physicalGroup->members.front().doubleByteKey);
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
						* (physicalGroup->uniqueSingleByteProfiles.size() + 1u);
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
					originalGpuBytes, packingByteClass))
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
			if (multiplePages || gpuGrowth)
			{
				const bool fallbackMarked =
					MarkPhysicalAtlasGroupFallback(
						runtime, key, sourcePageCount);
				if (physicalGroupFallback)
					*physicalGroupFallback = fallbackMarked;
				gLog.FormattedMessage(
					"tnvse_freetype_font: physical atlas group fallback owner=%u members=%u uniqueSingleProfiles=%u pages=%u sourceGpuBytes=%llu candidateGpuBytes=%llu reason=%s markerPersisted=%u policy=retain-per-font-atlases",
					physicalGroup->ownerFontId,
					static_cast<UInt32>(
						physicalGroup->members.size()),
					static_cast<UInt32>(
						physicalGroup->uniqueSingleByteProfiles.size()),
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
				"tnvse_freetype_font: physical atlas group snapshot published owner=%u members=%u uniqueSingleProfiles=%u pageContentHash=%016llX size=%ux%u placements=%llu",
				physicalGroup->ownerFontId,
				static_cast<UInt32>(physicalGroup->members.size()),
				static_cast<UInt32>(
					physicalGroup->uniqueSingleByteProfiles.size()),
				static_cast<unsigned long long>(
					pages.front().header.pageContentHash),
				pages.front().header.width, pages.front().header.height,
				static_cast<unsigned long long>(totalPlacements));
		}
		return true;
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

	bool ConsolidatePhysicalFontAtlasGroups(float rasterScale)
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
						"tnvse_freetype_font: physical atlas group reused owner=%u members=%u uniqueSingleProfiles=%u size=%ux%u gpuBytes=%llu pageContentHash=%016llX",
						group.ownerFontId,
						static_cast<UInt32>(group.members.size()),
						static_cast<UInt32>(
							group.uniqueSingleByteProfiles.size()),
						residentGroup->width, residentGroup->height,
						static_cast<unsigned long long>(bytes),
						static_cast<unsigned long long>(
							residentGroup->pageContentHash));
					continue;
				}
				if (IsPhysicalAtlasGroupFallbackMarkedLocked(group))
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: physical atlas group fallback reused owner=%u members=%u uniqueSingleProfiles=%u policy=retain-per-font-atlases",
						group.ownerFontId,
						static_cast<UInt32>(group.members.size()),
						static_cast<UInt32>(
							group.uniqueSingleByteProfiles.size()));
					continue;
				}
			}

			RuntimeFont* ownerRuntime =
				EnsureRuntimeFont(group.ownerFontId);
			bool sourceTablesReady = ownerRuntime != nullptr;
			UInt64 doubleByteLayoutIdentity = 0;
			if (sourceTablesReady)
			{
				for (const PhysicalAtlasGroupMember& member : group.members)
				{
					RuntimeFont* memberRuntime =
						EnsureRuntimeFont(member.config->fontId);
					const UInt64 memberDoubleByteLayoutIdentity =
						memberRuntime
							? GetRuntimeDirectRoleLayoutIdentity(
								*memberRuntime,
								VectorFontByteClass::DoubleByte)
							: 0;
					if (!doubleByteLayoutIdentity)
					{
						doubleByteLayoutIdentity =
							memberDoubleByteLayoutIdentity;
					}
					if (!memberRuntime
						|| !memberDoubleByteLayoutIdentity
						|| memberDoubleByteLayoutIdentity
							!= doubleByteLayoutIdentity
						|| !BuildDirectGlyphAtlasTables(
							*memberRuntime, rasterScale))
					{
						sourceTablesReady = false;
						break;
					}
				}
			}
			bool groupPublished = false;
			bool groupFallback = false;
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
					"tnvse_freetype_font: physical atlas group consolidation skipped owner=%u members=%u uniqueSingleProfiles=%u fallbackRestored=%u reason=publish-failed",
					group.ownerFontId,
					static_cast<UInt32>(group.members.size()),
					static_cast<UInt32>(
						group.uniqueSingleByteProfiles.size()),
					fallbackRestored ? 1u : 0u);
				continue;
			}

			bool rebuilt = true;
			std::vector<RuntimeFont*> memberRuntimes;
			memberRuntimes.reserve(group.members.size());
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
						*memberRuntime, rasterScale))
					{
						rebuilt = false;
						break;
					}
				}
			}

			std::shared_ptr<AtlasResource> shared;
			bool physicallyShared = false;
			if (rebuilt)
			{
				std::lock_guard<std::mutex> lock(State().atlasMutex);
				physicallyShared =
					IsPhysicalAtlasGroupResidentLocked(group, &shared);
			}
			if (!rebuilt || !physicallyShared || !shared)
			{
				success = false;
				gLog.FormattedMessage(
					"tnvse_freetype_font: physical atlas group restore validation failed owner=%u members=%u rebuilt=%u physicallyShared=%u",
					group.ownerFontId,
					static_cast<UInt32>(group.members.size()),
					rebuilt ? 1u : 0u, physicallyShared ? 1u : 0u);
				continue;
			}

			const size_t bytes = GetAtlasStorageBytes(shared->width,
				shared->height, shared->pixelMode, shared->mipLevels);
			gLog.FormattedMessage(
				"tnvse_freetype_font: physical atlas group active owner=%u members=%u uniqueSingleProfiles=%u size=%ux%u gpuBytes=%llu pageContentHash=%016llX",
				group.ownerFontId,
				static_cast<UInt32>(group.members.size()),
				static_cast<UInt32>(
					group.uniqueSingleByteProfiles.size()),
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

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

		UInt64 GetAtlasSnapshotHash(const AtlasCacheKey& key, UInt64 maskContentHash,
			const FontConfig& config)
		{
			UInt64 hash = HashAtlasBytes(&kAtlasSnapshotVersion,
				sizeof(kAtlasSnapshotVersion));
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
			// Atlas restore also requires the matching complete glyph manifest.
			hash = HashAtlasBytes(&kPersistentGlyphManifestVersion,
				sizeof(kPersistentGlyphManifestVersion), hash);
			hash = HashAtlasBytes(
				&kPersistentGlyphManifestCacheIdentityVersion,
				sizeof(kPersistentGlyphManifestCacheIdentityVersion), hash);
			return hash;
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
			snapshotHash = GetAtlasSnapshotHash(key, maskContentHash,
				GetRuntimeConfig(*atlasRuntime));
			wchar_t fileName[256] = {};
			_snwprintf_s(fileName, _countof(fileName), _TRUNCATE,
				L"shared_%016llX_p%u.tnvfatlas",
				static_cast<unsigned long long>(snapshotHash), key.pageIndex);
			const std::wstring path = directory + L"\\" + fileName;
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
				|| placement.maskType > static_cast<UInt8>(GlyphMaskType::Shadow)
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
						& kAtlasSnapshotFlagGloballyRepacked))
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
				state.completeAtlasProfiles.erase(profileKey);
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
			size_t sourceOffset = 0;
			size_t sourceBytes = 0;
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

		bool MaterializeRepackedSnapshotPixels(SnapshotPageData& page)
		{
			if (page.pixelSources.empty())
				return true;
			size_t totalBytes = 0;
			for (const SnapshotPixelSource& source : page.pixelSources)
			{
				if (!source.snapshot || source.destinationOffset != totalBytes
					|| source.bytes > std::numeric_limits<size_t>::max() - totalBytes)
				{
					return false;
				}
				totalBytes += source.bytes;
			}
			page.pixels.assign(totalBytes, 0);
			std::unordered_set<const CompactAtlasSnapshot*> loaded;
			std::vector<UInt8> sourcePixels;
			for (const SnapshotPixelSource& source : page.pixelSources)
			{
				const CompactAtlasSnapshot* identity = source.snapshot.get();
				if (!loaded.insert(identity).second)
					continue;
				if (!LoadCompactAtlasSnapshotPixels(*source.snapshot, sourcePixels))
					return false;
				for (const SnapshotPixelSource& copy : page.pixelSources)
				{
					if (copy.snapshot.get() != identity
						|| copy.sourceOffset > sourcePixels.size()
						|| copy.bytes > sourcePixels.size() - copy.sourceOffset
						|| copy.destinationOffset > page.pixels.size()
						|| copy.bytes > page.pixels.size() - copy.destinationOffset)
					{
						if (copy.snapshot.get() == identity)
							return false;
						continue;
					}
					std::memcpy(page.pixels.data() + copy.destinationOffset,
						sourcePixels.data() + copy.sourceOffset, copy.bytes);
				}
				sourcePixels.clear();
			}
			return true;
		}

		UInt32 NextSnapshotPowerOfTwo(UInt32 value)
		{
			UInt32 result = 64;
			while (result < value && result < kAtlasHardLimit)
				result <<= 1;
			return result;
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
			struct Slice
			{
				AtlasRect rect;
				size_t offset;
				size_t bytes;
			};
			std::vector<Slice> slices;
			slices.reserve(placements.size());
			size_t offset = 0;
			const size_t bytesPerPixel = AtlasBytesPerPixel(
				static_cast<AtlasPixelMode>(header.pixelMode));
			for (const AtlasSnapshotPlacement& placement : placements)
			{
				const size_t bytes = static_cast<size_t>(placement.rect.width)
					* placement.rect.height * bytesPerPixel;
				if (offset > pixels.size() || bytes > pixels.size() - offset)
					return 0;
				slices.push_back({ placement.rect, offset, bytes });
				offset += bytes;
			}
			if (offset != pixels.size())
				return 0;
			std::sort(slices.begin(), slices.end(), [](const Slice& a, const Slice& b)
			{
				if (a.rect.y != b.rect.y) return a.rect.y < b.rect.y;
				if (a.rect.x != b.rect.x) return a.rect.x < b.rect.x;
				if (a.rect.height != b.rect.height) return a.rect.height < b.rect.height;
				return a.rect.width < b.rect.width;
			});
			for (const Slice& slice : slices)
			{
				hash = HashAtlasBytes(&slice.rect, sizeof(slice.rect), hash);
				hash = HashAtlasBytes(pixels.data() + slice.offset, slice.bytes, hash);
			}
			return hash;
		}

		bool BuildRepackedSnapshotPages(const AtlasCacheKey& baseKey,
			const std::vector<std::pair<AtlasCacheKey,
				std::shared_ptr<AtlasResource>>>& resources,
			std::vector<SnapshotPageData>& pages, UInt64& originalGpuBytes)
		{
			std::vector<SnapshotGlyphData> glyphs;
			std::unordered_set<UInt64> cacheIds;
			originalGpuBytes = 0;
			for (const auto& item : resources)
			{
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
					glyph.sourceOffset = sourceOffsets[placementIndex];
					glyph.sourceBytes = bytes;
					glyphs.push_back(std::move(glyph));
				}
			}
			if (glyphs.empty())
				return false;
			std::sort(glyphs.begin(), glyphs.end(), [](const auto& lhs, const auto& rhs)
			{
				if (lhs.placement.rect.height != rhs.placement.rect.height)
					return lhs.placement.rect.height > rhs.placement.rect.height;
				if (lhs.placement.rect.width != rhs.placement.rect.width)
					return lhs.placement.rect.width > rhs.placement.rect.width;
				return lhs.placement.cacheId < rhs.placement.cacheId;
			});

			const UInt32 maximumSize = std::min(
				std::min(GetMaximumAtlasSize(), kAtlasHardLimit),
				kMaximumMtsdfPrewarmAtlasSize);
			const UInt32 padding = baseKey.padding;
			std::vector<SnapshotGlyphData> remaining = std::move(glyphs);
			while (!remaining.empty())
			{
				if (pages.size() >= kMaximumAtlasSnapshotPages
					|| maximumSize > static_cast<UInt32>(std::numeric_limits<int>::max()))
					return false;

				std::vector<stbrp_node> nodes(maximumSize);
				stbrp_context context = {};
				stbrp_init_target(&context, static_cast<int>(maximumSize),
					static_cast<int>(maximumSize), nodes.data(),
					static_cast<int>(nodes.size()));

				std::vector<SnapshotGlyphData> packed;
				std::vector<SnapshotGlyphData> nextRemaining;
				packed.reserve(remaining.size());
				nextRemaining.reserve(remaining.size());
				UInt32 usedWidth = 0;
				UInt32 usedHeight = 0;
				for (SnapshotGlyphData& glyph : remaining)
				{
					const UInt32 glyphWidth = glyph.placement.rect.width;
					const UInt32 glyphHeight = glyph.placement.rect.height;
					if (!glyphWidth || !glyphHeight || padding > maximumSize / 2
						|| glyphWidth > maximumSize - padding * 2
						|| glyphHeight > maximumSize - padding * 2)
						return false;

					stbrp_rect rect = {};
					rect.w = static_cast<stbrp_coord>(glyphWidth + padding * 2);
					rect.h = static_cast<stbrp_coord>(glyphHeight + padding * 2);
					// Pack one already-sorted rectangle at a time. This preserves the
					// cacheId tie-break instead of relying on qsort stability for equal sizes.
					stbrp_pack_rects(&context, &rect, 1);
					if (!rect.was_packed)
					{
						nextRemaining.push_back(std::move(glyph));
						continue;
					}
					glyph.placement.rect.x = static_cast<UInt32>(rect.x) + padding;
					glyph.placement.rect.y = static_cast<UInt32>(rect.y) + padding;
					usedWidth = std::max(usedWidth,
						static_cast<UInt32>(rect.x + rect.w));
					usedHeight = std::max(usedHeight,
						static_cast<UInt32>(rect.y + rect.h));
					packed.push_back(std::move(glyph));
				}
				if (packed.empty())
					return false;

				SnapshotPageData page;
				page.key = baseKey;
				page.key.pageIndex = static_cast<UInt16>(pages.size());
				page.header.width = NextSnapshotPowerOfTwo(
					std::max<UInt32>(64, usedWidth));
				page.header.height = NextSnapshotPowerOfTwo(
					std::max<UInt32>(64, usedHeight));
				page.header.padding = padding;
				// A skyline-packed snapshot cannot resume the runtime shelf cursor.
				// Close the restored page so later glyphs are placed on a fresh page.
				page.header.cursorX = padding;
				page.header.cursorY = page.header.height;
				page.header.shelfHeight = 0;
				std::sort(packed.begin(), packed.end(), [](const auto& lhs, const auto& rhs)
				{
					return lhs.placement.cacheId < rhs.placement.cacheId;
				});
				size_t destinationBytes = 0;
				for (SnapshotGlyphData& glyph : packed)
				{
					page.placements.push_back(glyph.placement);
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
				remaining = std::move(nextRemaining);
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

		bool ReadSnapshotMetadata(const std::wstring& path,
			AtlasSnapshotHeader& header,
			std::vector<AtlasSnapshotPlacement>& placements,
			UInt64& fileBytes)
		{
			HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
				OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return false;
			LARGE_INTEGER size = {};
			const bool validSize = GetFileSizeEx(file, &size) && size.QuadPart > 0
				&& size.QuadPart <= 512ll * 1024ll * 1024ll;
			if (!validSize)
			{
				CloseHandle(file);
				return false;
			}
			fileBytes = static_cast<UInt64>(size.QuadPart);
			if (!ReadSnapshotBytesExact(file, &header, sizeof(header)))
			{
				CloseHandle(file);
				return false;
			}
			const UInt64 placementBytes = static_cast<UInt64>(header.placementCount)
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
			placements.resize(header.placementCount);
			const bool result = ReadSnapshotBytesExact(file, placements.data(),
				static_cast<size_t>(placementBytes));
			CloseHandle(file);
			return result;
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
				HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
					OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
				if (file == INVALID_HANDLE_VALUE)
					return false;
				LARGE_INTEGER fileSize = {};
				AtlasSnapshotHeader header = {};
				DWORD read = 0;
				const bool readHeader = GetFileSizeEx(file, &fileSize)
					&& fileSize.QuadPart >= static_cast<LONGLONG>(sizeof(header))
					&& ReadFile(file, &header, sizeof(header), &read, nullptr)
					&& read == sizeof(header);
				CloseHandle(file);
				if (!readHeader)
					return false;
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
				const bool shapeValid = header.width >= 64 && header.height >= 64
					&& header.width <= kAtlasHardLimit
					&& header.height <= kAtlasHardLimit
					&& header.mipLevels >= 1
					&& header.mipLevels <= kMaximumAtlasMipLevels;
				if (!pageCount || pageCount > kMaximumAtlasSnapshotPages
					|| header.pageIndex != pageIndex
					|| header.pageCount != pageCount
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
					|| expectedFileBytes != static_cast<UInt64>(fileSize.QuadPart)
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
			UInt64 serializedBytes = 0;
			if (pageSnapshotHash != snapshotHash
				|| pageContentHash != maskContentHash
				|| !ReadSnapshotMetadata(path, header, placementList,
					serializedBytes))
				return false;
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
				&& payloadOffset + header.storedPixelBytes == serializedBytes;
			const bool shapeValid = header.width >= 64 && header.height >= 64
				&& header.width <= kAtlasHardLimit && header.height <= kAtlasHardLimit
				&& header.mipLevels >= 1 && header.mipLevels <= kMaximumAtlasMipLevels;
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
			compactSnapshot->sourcePath = path;
			compactSnapshot->sourceHeader = header;
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
					&& IsDistanceFieldAtlasPixelMode(pageKey.pixelMode)
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
			// Blocking prewarm can encounter demand-created pages for the same profile
			// before the validated snapshot is restored. Keeping those entries would
			// discard the full resources built above, then incorrectly mark the partial
			// profile complete. No text shapes exist at this startup barrier, so replace
			// the whole profile generation atomically and retire any externally-held
			// DEFAULT wrappers through the normal lifetime path.
			const AtlasProfileKey profileKey = MakeAtlasProfileKey(key);
			if (metadataOnly)
				state.completeAtlasProfiles.erase(profileKey);
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
				"tnvse_freetype_font: atlas snapshot metadata staged font=%u role=%s pages=%u replacedPages=%u placements=%llu sourceBytes=%llu gpuBytes=0",
				key.fontId, key.byteClass == VectorFontByteClass::DoubleByte
					? "doubleByte" : "singleByte", pageCount, replacedPages,
				static_cast<unsigned long long>(totalPlacements),
				static_cast<unsigned long long>(totalBytes));
		}
		else
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: atlas snapshot restored font=%u role=%s pages=%u replacedPages=%u defaultPoolPages=%u deduplicatedPages=%u deduplicatedGpuBytes=%llu placements=%llu bytes=%llu directStreamedPixelBytes=%llu",
				key.fontId, key.byteClass == VectorFontByteClass::DoubleByte
					? "doubleByte" : "singleByte", pageCount, replacedPages,
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
			return HasCompleteGlyphManifest(runtime);
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
		return HasCompleteGlyphManifest(runtime)
			&& singleByteReady && doubleByteReady && profilesResident;
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
			state.completeAtlasProfiles.erase(MakeAtlasProfileKey(key));
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
				state.completeAtlasProfiles.erase(profileKey);
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

	bool SaveGlyphAtlasSnapshotRole(RuntimeFont& runtime,
		VectorFontByteClass byteClass, float rasterScale)
	{
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
		UInt32 sourcePageCount = 0;
		{
			std::lock_guard<std::mutex> lock(State().atlasMutex);
			std::vector<std::pair<AtlasCacheKey, std::shared_ptr<AtlasResource>>> resources;
			const auto profile = State().atlasProfiles.find(MakeAtlasProfileKey(key));
			if (profile == State().atlasProfiles.end())
				return false;
			resources.reserve(profile->second.pages.size());
			for (UInt16 pageIndex : profile->second.pages)
			{
				AtlasCacheKey pageKey = key;
				pageKey.pageIndex = pageIndex;
				const auto page = State().atlasCache.find(pageKey);
				if (page != State().atlasCache.end() && page->second.resource)
					resources.push_back({ pageKey, page->second.resource });
			}
			if (resources.empty() || resources.size() > kMaximumAtlasSnapshotPages)
				return false;
			sourcePageCount = static_cast<UInt32>(resources.size());
			for (size_t index = 0; index < resources.size(); ++index)
			{
				if (resources[index].first.pageIndex != index)
					return false;
			}
			if (UsesPlacedLevelZeroSnapshot(key))
			{
				if (!BuildRepackedSnapshotPages(key, resources, pages, originalGpuBytes))
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
			if (!MaterializeRepackedSnapshotPixels(page))
			{
				prepared = false;
				break;
			}
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
				page.header.scaleMilli = page.key.scaleMilli;
				page.header.mipLevels = GetAtlasMipLevelCount(page.header.width,
					page.header.height, page.key.levelZeroOnly);
				page.header.pixelMode = static_cast<UInt8>(page.key.pixelMode);
				page.header.renderMode = static_cast<UInt8>(page.key.renderMode);
				page.header.byteClass = static_cast<UInt8>(page.key.byteClass);
				page.header.pageIndex = static_cast<UInt16>(index);
				page.header.pageCount = static_cast<UInt16>(pages.size());
				page.header.placementCount = static_cast<UInt32>(page.placements.size());
				page.header.pixelBytes = page.pixels.size();
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
				page.header.storedPixelBytes = page.pixels.size();
				UInt64 payloadHash = page.placements.empty() ? 1469598103934665603ull
					: HashAtlasBytes(page.placements.data(),
						page.placements.size() * sizeof(page.placements[0]));
				page.header.payloadChecksum = HashAtlasBytes(page.pixels.data(),
					page.pixels.size(), payloadHash);
				page.header.pageContentHash = ComputeAtlasPageContentHash(page.header,
					page.placements, page.pixels);
				page.header.checksum = HashAtlasBytes(&page.header,
					offsetof(AtlasSnapshotHeader, checksum));

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
				HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
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
					written = WriteSparseFileBytes(file, page.pixels.data(),
						page.pixels.size(), sparse);
				CloseHandle(file);
				if (!written)
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
			State().completeAtlasProfiles.erase(MakeAtlasProfileKey(key));
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
		gLog.FormattedMessage(
			"tnvse_freetype_font: atlas snapshot saved font=%u role=%s pages=%u->%u placements=%llu rawBytes=%llu gpuBytes=%llu->%llu saved=%llu tail=%ux%u",
			key.fontId, key.byteClass == VectorFontByteClass::DoubleByte
				? "doubleByte" : "singleByte", sourcePageCount, pageCount,
			static_cast<unsigned long long>(totalPlacements),
			static_cast<unsigned long long>(totalBytes),
			static_cast<unsigned long long>(originalGpuBytes),
			static_cast<unsigned long long>(snapshotGpuBytes),
			static_cast<unsigned long long>(originalGpuBytes > snapshotGpuBytes
				? originalGpuBytes - snapshotGpuBytes : 0),
			pages.back().header.width, pages.back().header.height);
		return true;
	}

	bool SaveGlyphAtlasSnapshot(RuntimeFont& runtime, float rasterScale)
	{
		if (!SaveGlyphAtlasSnapshotRole(runtime,
			VectorFontByteClass::SingleByte, rasterScale))
		{
			return false;
		}
		return !IsDbcsCodePage(GetFreeTypeTextCodePage())
			|| IsPrewarmAtlasAlias(GetRuntimeConfig(runtime),
				VectorFontByteClass::DoubleByte)
			|| SaveGlyphAtlasSnapshotRole(runtime,
				VectorFontByteClass::DoubleByte, rasterScale);
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
		AtlasSnapshotHeader header;
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return PersistentCacheCleanupClass::Invalid;
		DWORD read = 0;
		const bool readHeader = ReadFile(file, &header, sizeof(header),
			&read, nullptr) && read == sizeof(header);
		CloseHandle(file);

		const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'A', 'T', 'L', '9' };
		if (!readHeader
			|| std::memcmp(header.magic, magic, sizeof(magic)) != 0
			|| header.version != kAtlasSnapshotVersion
			|| header.headerSize != sizeof(header)
			|| header.renderMode
				> static_cast<UInt8>(AtlasRenderMode::ShaderEffects)
			|| header.pixelMode > static_cast<UInt8>(AtlasPixelMode::Mtsdf32)
			|| header.checksum != HashAtlasBytes(&header,
				offsetof(AtlasSnapshotHeader, checksum)))
		{
			return PersistentCacheCleanupClass::Invalid;
		}
		if (header.renderMode != static_cast<UInt8>(
			AtlasRenderMode::ShaderEffects))
		{
			return PersistentCacheCleanupClass::Neutral;
		}

		DistanceFieldMethod method;
		if (header.pixelMode == static_cast<UInt8>(AtlasPixelMode::A8))
			method = DistanceFieldMethod::TrueSdf;
		else if (header.pixelMode == static_cast<UInt8>(AtlasPixelMode::Mtsdf32))
			method = DistanceFieldMethod::Mtsdf;
		else
			return PersistentCacheCleanupClass::Invalid;
		if (GetPersistentFontCacheDomain()
			== PersistentFontCacheDomain::CpuCoverage)
			return PersistentCacheCleanupClass::InactiveDistanceField;
		return method == GetConfiguredDistanceFieldMethod()
			? PersistentCacheCleanupClass::CurrentDistanceField
			: PersistentCacheCleanupClass::InactiveDistanceField;
	}
}

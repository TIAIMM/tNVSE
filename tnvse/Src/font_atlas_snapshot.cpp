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

		bool TryEnableFileCompression(HANDLE file)
		{
			if (file == INVALID_HANDLE_VALUE)
				return false;
			USHORT format = COMPRESSION_FORMAT_DEFAULT;
			DWORD bytesReturned = 0;
			return DeviceIoControl(file, FSCTL_SET_COMPRESSION,
				&format, sizeof(format), nullptr, 0, &bytesReturned, nullptr) != FALSE;
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

		bool WriteSparseFileBytes(HANDLE file, const UInt8* data, size_t size,
			bool sparse)
		{
			constexpr size_t kSparseChunkBytes = 64u * 1024u;
			for (size_t offset = 0; offset < size;)
			{
				const size_t chunk = std::min(kSparseChunkBytes, size - offset);
				const bool zero = sparse && std::all_of(data + offset,
					data + offset + chunk, [](UInt8 value) { return value == 0; });
				if (zero)
				{
					LARGE_INTEGER distance = {};
					distance.QuadPart = static_cast<LONGLONG>(chunk);
					if (!SetFilePointerEx(file, distance, nullptr, FILE_CURRENT))
						return false;
				}
				else if (!WriteSequentialFileBytes(file, data + offset, chunk))
					return false;
				offset += chunk;
			}
			return SetEndOfFile(file) != FALSE;
		}

		bool ResolvePrewarmAtlasKey(const FontConfig& config, float rasterScale,
			AtlasCacheKey& key)
		{
			if (!IsA8RendererAvailable())
				return false;
			EffectQuality resolved = config.effectQuality;
			bool shaderEffects = (config.shadow.enabled || config.glow.enabled
				|| config.outline.enabled || UsesSdfFill(config))
				&& ResolveA8EffectQuality(config.effectQuality, resolved);
			UInt32 sdfSpread = 0;
			if (shaderEffects && NeedsSdfMask(config)
				&& !ResolveSdfSpread(config, rasterScale, sdfSpread))
				shaderEffects = false;
			key = {
				BuildPrewarmAtlasContentHash(config, rasterScale, shaderEffects),
				config.fontId,
				static_cast<UInt32>(std::lround(rasterScale * 1000.0f)),
				AtlasPixelMode::A8,
				shaderEffects ? AtlasRenderMode::ShaderEffects
					: AtlasRenderMode::CpuEffects,
				kAtlasPadding,
				shaderEffects && UsesSdfFill(config)
			};
			return true;
		}

		UInt64 GetAtlasSnapshotHash(const AtlasCacheKey& key, UInt64 maskContentHash,
			FontPrewarmMode prewarmMode)
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
			hash = HashAtlasBytes(&g_usingWinEncoding,
				sizeof(g_usingWinEncoding), hash);
			hash = HashAtlasBytes(&prewarmMode, sizeof(prewarmMode), hash);
			hash = HashAtlasBytes(&kMaximumAtlasMipLevels,
				sizeof(kMaximumAtlasMipLevels), hash);
			return HashAtlasBytes(&A8ShapeColorContract::kTileUniformColorAbi,
				sizeof(A8ShapeColorContract::kTileUniformColorAbi), hash);
		}

		std::wstring GetAtlasSnapshotPath(RuntimeFont& runtime,
			const AtlasCacheKey& key, UInt64& snapshotHash, UInt64& maskContentHash)
		{
			std::wstring directory;
			if (!GetFreeTypeFontCacheDirectory(directory))
				return {};
			maskContentHash = GetRuntimeMaskContentHash(runtime);
			snapshotHash = GetAtlasSnapshotHash(key, maskContentHash,
				GetRuntimeConfig(runtime).prewarm);
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

		bool BuildAtlasSnapshotPixels(const AtlasResource& resource,
			const std::vector<AtlasSnapshotPlacement>& placements,
			AtlasSnapshotStorage storageMode, std::vector<UInt8>& pixels)
		{
			const UInt32 bytesPerPixel = AtlasBytesPerPixel(resource.pixelMode);
			pixels.clear();
			if (storageMode == AtlasSnapshotStorage::PlacedLevelZeroRects)
			{
				if (!resource.levelZeroOnly || resource.mipLevels != 1)
					return false;
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
					for (const auto& pair : resource.residentBitmaps)
					{
						auto placement = resource.placements.find(pair.first);
						if (!pair.second || placement == resource.placements.end())
							continue;
						WriteBitmapPixels(reconstructed.data(),
							static_cast<LONG>(sourcePitch), resource.pixelMode,
							*pair.second, placement->second,
							placement->second.x, placement->second.y);
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
			for (const auto& pair : resource.residentBitmaps)
			{
				auto placement = resource.placements.find(pair.first);
				if (!pair.second || placement == resource.placements.end())
					continue;
				WriteBitmapPixels(current.data(),
					static_cast<LONG>(resource.width * bytesPerPixel), resource.pixelMode,
					*pair.second, placement->second, placement->second.x, placement->second.y);
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

		bool DecodeAtlasSnapshotPixels(const AtlasSnapshotHeader& header,
			const std::vector<AtlasSnapshotPlacement>& placements,
			const UInt8* storedPixels, std::vector<UInt8>& pixels)
		{
			if (!storedPixels && header.pixelBytes)
				return false;
			const AtlasPixelMode pixelMode = static_cast<AtlasPixelMode>(header.pixelMode);
			const AtlasSnapshotStorage storageMode =
				static_cast<AtlasSnapshotStorage>(header.storageMode);
			const size_t fullBytes = GetAtlasStorageBytes(header.width, header.height,
				pixelMode, header.mipLevels);
			if (storageMode == AtlasSnapshotStorage::FullMipChain)
			{
				if (header.pixelBytes != fullBytes)
					return false;
				pixels.assign(storedPixels, storedPixels + fullBytes);
				return true;
			}
			if (storageMode != AtlasSnapshotStorage::PlacedLevelZeroRects
				|| header.mipLevels != 1)
			{
				return false;
			}
			size_t packedBytes = 0;
			if (!GetPlacedLevelZeroSnapshotBytes(placements,
				header.width, header.height, pixelMode, packedBytes)
				|| header.pixelBytes != packedBytes)
			{
				return false;
			}
			pixels.assign(fullBytes, 0);
			const size_t bytesPerPixel = AtlasBytesPerPixel(pixelMode);
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

		bool ReadSnapshotFile(const std::wstring& path, std::vector<UInt8>& bytes)
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
			bytes.resize(static_cast<size_t>(size.QuadPart));
			DWORD read = 0;
			const bool result = ReadFile(file, bytes.data(),
				static_cast<DWORD>(bytes.size()), &read, nullptr)
				&& read == bytes.size();
			CloseHandle(file);
			return result;
		}
	}

	bool TryLoadGlyphAtlasSnapshot(RuntimeFont& runtime, float rasterScale)
	{
		const FontConfig& config = GetRuntimeConfig(runtime);
		if (!HasCompleteGlyphManifest(runtime, config.prewarm))
			return false;
		AtlasCacheKey key;
		if (!ResolvePrewarmAtlasKey(config, rasterScale, key))
			return false;
		UInt64 snapshotHash = 0;
		UInt64 maskContentHash = 0;
		std::vector<std::pair<AtlasCacheKey, std::shared_ptr<AtlasResource>>> pages;
		UInt64 totalBytes = 0;
		UInt64 totalPlacements = 0;
		UInt16 pageCount = 0;
		UInt16 defaultPoolPages = 0;
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
			std::vector<UInt8> serialized;
			if (pageSnapshotHash != snapshotHash
				|| pageContentHash != maskContentHash
				|| !ReadSnapshotFile(path, serialized)
				|| serialized.size() < sizeof(AtlasSnapshotHeader))
				return false;
			AtlasSnapshotHeader header;
			std::memcpy(&header, serialized.data(), sizeof(header));
			const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'A', 'T', 'L', '5' };
			const AtlasSnapshotStorage expectedStorage = pageKey.levelZeroOnly
				? AtlasSnapshotStorage::PlacedLevelZeroRects
				: AtlasSnapshotStorage::FullMipChain;
			const UInt64 placementsBytes = static_cast<UInt64>(header.placementCount)
				* sizeof(AtlasSnapshotPlacement);
			const UInt64 payloadOffset = sizeof(header) + placementsBytes;
			const bool payloadSizeValid = payloadOffset >= sizeof(header)
				&& header.pixelBytes <= std::numeric_limits<UInt64>::max() - payloadOffset
				&& payloadOffset + header.pixelBytes == serialized.size();
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
				|| header.reservedFontId != 0
				|| header.scaleMilli != pageKey.scaleMilli
				|| header.pixelMode != static_cast<UInt8>(pageKey.pixelMode)
				|| header.renderMode != static_cast<UInt8>(pageKey.renderMode)
				|| header.storageMode != static_cast<UInt8>(expectedStorage)
				|| header.reserved != 0
				|| header.padding != pageKey.padding || !payloadSizeValid
				|| header.pageIndex != pageIndex || !header.pageCount
				|| header.pageCount > kMaximumAtlasSnapshotPages
				|| (pageIndex && header.pageCount != pageCount)
				|| !shapeValid
				|| header.mipLevels != GetAtlasMipLevelCount(header.width, header.height,
					pageKey.levelZeroOnly)
				|| header.pixelBytes > fullPixelBytes
				|| (expectedStorage == AtlasSnapshotStorage::FullMipChain
					&& header.pixelBytes != fullPixelBytes)
				|| header.checksum != HashAtlasBytes(&header,
					offsetof(AtlasSnapshotHeader, checksum)))
				return false;
			if (!pageIndex)
				pageCount = header.pageCount;
			const UInt8* payload = serialized.data() + sizeof(header);
			if (header.payloadChecksum != HashAtlasBytes(payload,
				static_cast<size_t>(placementsBytes + header.pixelBytes)))
				return false;
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
			const auto* placements = reinterpret_cast<const AtlasSnapshotPlacement*>(payload);
			std::vector<AtlasSnapshotPlacement> placementList(
				placements, placements + header.placementCount);
			for (UInt32 index = 0; index < header.placementCount; ++index)
			{
				const AtlasRect& rect = placementList[index].rect;
				if (!placementList[index].cacheId || !rect.width || !rect.height
					|| rect.x > header.width || rect.width > header.width - rect.x
					|| rect.y > header.height || rect.height > header.height - rect.y
					|| !resource->placements.emplace(placementList[index].cacheId, rect).second)
					return false;
			}
			const UInt8* pixelData = payload + placementsBytes;
			// v5 level-zero placement snapshots are pure SDF A8 pages. Keep their
			// packed payload as reset backing instead of expanding a full CPU atlas.
			const bool compactDefaultEligible = g_bEnableFreeTypeDefaultPoolAtlas
				&& !State().defaultPoolShutdown
				&& pageKey.pixelMode == AtlasPixelMode::A8
				&& pageKey.renderMode == AtlasRenderMode::ShaderEffects
				&& pageKey.levelZeroOnly
				&& header.mipLevels == 1
				&& expectedStorage == AtlasSnapshotStorage::PlacedLevelZeroRects;
			bool restoredToDefaultPool = false;
			if (compactDefaultEligible)
			{
				size_t packedBytes = 0;
				if (!GetPlacedLevelZeroSnapshotBytes(placementList,
					header.width, header.height, pageKey.pixelMode, packedBytes)
					|| packedBytes != header.pixelBytes)
				{
					return false;
				}
				auto compactSnapshot = std::make_shared<CompactAtlasSnapshot>();
				compactSnapshot->pixelMode = pageKey.pixelMode;
				compactSnapshot->placements = placementList;
				compactSnapshot->pixels.assign(pixelData,
					pixelData + static_cast<size_t>(header.pixelBytes));
				resource->compactSnapshot = compactSnapshot;
				resource->backend = AtlasBackend::DefaultPool;
				restoredToDefaultPool = CreateDefaultPoolAtlas(
					*resource, pageKey.pixelMode);
				if (restoredToDefaultPool)
					++defaultPoolPages;
			}
			if (!restoredToDefaultPool)
			{
				resource->compactSnapshot.reset();
				resource->backend = AtlasBackend::Managed;
				std::vector<UInt8> pixels;
				if (!DecodeAtlasSnapshotPixels(header, placementList, pixelData, pixels))
					return false;
				NiTexturingProperty* property = CreateManagedAtlasProperty(header.width,
					header.height, pageKey.pixelMode, header.mipLevels,
					pixels, resource->pixelData);
				if (!property)
					return false;
				resource->property = property;
			}
			resource->generation = 1;
			totalBytes += header.pixelBytes;
			totalPlacements += header.placementCount;
			pages.push_back({ pageKey, resource });
		}
		if (pages.empty() || pages.size() != pageCount)
			return false;
		{
			AtlasState& state = State();
			std::lock_guard<std::mutex> lock(state.atlasMutex);
			for (const auto& page : pages)
			{
				if (state.atlasCache.find(page.first) != state.atlasCache.end())
					continue;
				const size_t storageBytes = GetAtlasStorageBytes(page.second->width,
					page.second->height, page.second->pixelMode, page.second->mipLevels);
				state.atlasLru.push_front(page.first);
				state.atlasCache.emplace(page.first, AtlasCacheEntry{
					page.second, storageBytes, state.atlasLru.begin() });
				IndexAtlasPage(state, page.first, *page.second);
				state.atlasCacheBytes += storageBytes;
			}
			TrimAtlasCache(state);
		}
		gLog.FormattedMessage(
			"tnvse_freetype_font: atlas snapshot restored font=%u pages=%u defaultPoolPages=%u placements=%llu bytes=%llu",
			key.fontId, pageCount, defaultPoolPages,
			static_cast<unsigned long long>(totalPlacements),
			static_cast<unsigned long long>(totalBytes));
		return true;
	}

	bool SaveGlyphAtlasSnapshot(RuntimeFont& runtime, float rasterScale)
	{
		const FontConfig& config = GetRuntimeConfig(runtime);
		AtlasCacheKey key;
		if (!ResolvePrewarmAtlasKey(config, rasterScale, key))
			return false;
		UInt64 snapshotHash = 0;
		UInt64 maskContentHash = 0;
		if (GetAtlasSnapshotPath(runtime, key, snapshotHash, maskContentHash).empty())
			return false;
		struct SnapshotPage
		{
			AtlasCacheKey key;
			AtlasSnapshotHeader header;
			std::vector<AtlasSnapshotPlacement> placements;
			std::vector<UInt8> pixels;
			std::wstring path;
		};
		UInt64 totalBytes = 0;
		UInt64 totalPlacements = 0;
		UInt32 pageCount = 0;
		{
			std::lock_guard<std::mutex> lock(State().atlasMutex);
			std::vector<std::pair<AtlasCacheKey, const AtlasResource*>> resources;
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
					resources.push_back({ pageKey, page->second.resource.get() });
			}
			if (resources.empty() || resources.size() > kMaximumAtlasSnapshotPages)
				return false;
			pageCount = static_cast<UInt32>(resources.size());
			for (size_t index = 0; index < resources.size(); ++index)
			{
				if (resources[index].first.pageIndex != index)
					return false;
				SnapshotPage page;
				page.key = resources[index].first;
				const AtlasResource& resource = *resources[index].second;
				page.placements.reserve(resource.placements.size());
				for (const auto& pair : resource.placements)
					page.placements.push_back({ pair.first, pair.second });
				std::sort(page.placements.begin(), page.placements.end(),
					[](const AtlasSnapshotPlacement& lhs, const AtlasSnapshotPlacement& rhs)
					{
						return lhs.cacheId < rhs.cacheId;
					});
				const AtlasSnapshotStorage storageMode = resource.levelZeroOnly
					? AtlasSnapshotStorage::PlacedLevelZeroRects
					: AtlasSnapshotStorage::FullMipChain;
				if (!BuildAtlasSnapshotPixels(resource, page.placements,
					storageMode, page.pixels))
				{
					return false;
				}
				const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'A', 'T', 'L', '5' };
				std::memcpy(page.header.magic, magic, sizeof(magic));
				page.header.version = kAtlasSnapshotVersion;
				page.header.headerSize = sizeof(page.header);
				page.header.snapshotHash = snapshotHash;
				page.header.maskContentHash = maskContentHash;
				page.header.atlasContentHash = page.key.atlasContentHash;
				page.header.reservedFontId = 0;
				page.header.scaleMilli = page.key.scaleMilli;
				page.header.width = resource.width;
				page.header.height = resource.height;
				page.header.cursorX = resource.cursorX;
				page.header.cursorY = resource.cursorY;
				page.header.shelfHeight = resource.shelfHeight;
				page.header.padding = resource.padding;
				page.header.mipLevels = resource.mipLevels;
				page.header.pixelMode = static_cast<UInt8>(resource.pixelMode);
				page.header.renderMode = static_cast<UInt8>(resource.renderMode);
				page.header.storageMode = static_cast<UInt8>(storageMode);
				page.header.pageIndex = static_cast<UInt16>(index);
				page.header.pageCount = static_cast<UInt16>(resources.size());
				page.header.placementCount = static_cast<UInt32>(page.placements.size());
				page.header.pixelBytes = page.pixels.size();
				UInt64 payloadHash = page.placements.empty() ? 1469598103934665603ull
					: HashAtlasBytes(page.placements.data(),
						page.placements.size() * sizeof(page.placements[0]));
				page.header.payloadChecksum = HashAtlasBytes(page.pixels.data(),
					page.pixels.size(), payloadHash);
				page.header.checksum = HashAtlasBytes(&page.header,
					offsetof(AtlasSnapshotHeader, checksum));

				UInt64 ignoredSnapshotHash = 0;
				UInt64 ignoredContentHash = 0;
				page.path = GetAtlasSnapshotPath(runtime, page.key,
					ignoredSnapshotHash, ignoredContentHash);
				if (page.path.empty() || ignoredSnapshotHash != snapshotHash
					|| ignoredContentHash != maskContentHash)
					return false;
				const std::wstring temporary = page.path + L".tmp";
				HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
					CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
				if (file == INVALID_HANDLE_VALUE)
					return false;
				const bool sparse = TryEnableSparseFile(file);
				TryEnableFileCompression(file);
				const bool written = WriteSequentialFileBytes(file,
					&page.header, sizeof(page.header))
					&& WriteSequentialFileBytes(file, page.placements.data(),
						page.placements.size() * sizeof(page.placements[0]))
					&& WriteSparseFileBytes(file, page.pixels.data(),
						page.pixels.size(), sparse)
					&& FlushFileBuffers(file);
				CloseHandle(file);
				if (!written || !MoveFileExW(temporary.c_str(), page.path.c_str(),
					MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
				{
					DeleteFileW(temporary.c_str());
					return false;
				}
				totalBytes += page.header.pixelBytes;
				totalPlacements += page.header.placementCount;
			}
		}
		gLog.FormattedMessage(
			"tnvse_freetype_font: atlas snapshot saved font=%u pages=%u placements=%llu bytes=%llu",
			key.fontId, pageCount,
			static_cast<unsigned long long>(totalPlacements),
			static_cast<unsigned long long>(totalBytes));
		return true;
	}

	bool PrewarmGlyphAtlas(RuntimeFont& runtime,
		const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
		float rasterScale)
	{
		if (bitmaps.empty())
			return true;
		const FontConfig& config = GetRuntimeConfig(runtime);
		const bool useCustomA8Shader = IsA8RendererAvailable();
		// Original-shader ARGB glyphs bake a per-range color into distinct bitmap
		// cache IDs at shape-build time. A raw ARGB prewarm atlas cannot be reused
		// and would only evict useful A8 generations.
		if (!useCustomA8Shader)
			return true;
		const AtlasPixelMode pixelMode = AtlasPixelMode::A8;
		EffectQuality resolved = config.effectQuality;
		bool shaderEffects = pixelMode == AtlasPixelMode::A8
			&& (config.shadow.enabled || config.glow.enabled || config.outline.enabled
				|| UsesSdfFill(config))
			&& ResolveA8EffectQuality(config.effectQuality, resolved);
		UInt32 sdfSpread = 0;
		if (shaderEffects && NeedsSdfMask(config)
			&& !ResolveSdfSpread(config, rasterScale, sdfSpread))
			shaderEffects = false;
		const UInt32 padding = kAtlasPadding;
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
		return !GetAtlasResources(config, rasterScale, packedBitmaps, pixelMode,
			shaderEffects ? AtlasRenderMode::ShaderEffects : AtlasRenderMode::CpuEffects,
			padding).empty();
	}
}

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
			const UInt32 codePage = GetFreeTypeTextCodePage();
			hash = HashAtlasBytes(&codePage, sizeof(codePage), hash);
			hash = HashAtlasBytes(&config.prewarm, sizeof(config.prewarm), hash);
			hash = HashAtlasBytes(&kMaximumAtlasMipLevels,
				sizeof(kMaximumAtlasMipLevels), hash);
			hash = HashAtlasBytes(&A8ShapeColorContract::kTileUniformColorAbi,
				sizeof(A8ShapeColorContract::kTileUniformColorAbi), hash);
			const bool completeEffectOnlySdf =
				config.prewarm == FontPrewarmMode::CodePage
				&& key.renderMode == AtlasRenderMode::ShaderEffects
				&& !UsesSdfFill(config) && NeedsSdfMask(config);
			if (completeEffectOnlySdf)
			{
				hash = HashAtlasBytes(&kCodePageEffectOnlySdfCoverageRevision,
					sizeof(kCodePageEffectOnlySdfCoverageRevision), hash);
			}
			return hash;
		}

		std::wstring GetAtlasSnapshotPath(RuntimeFont& runtime,
			const AtlasCacheKey& key, UInt64& snapshotHash, UInt64& maskContentHash)
		{
			std::wstring directory;
			if (!GetFreeTypeFontCacheDirectory(directory))
				return {};
			maskContentHash = GetRuntimeMaskContentHash(runtime);
			snapshotHash = GetAtlasSnapshotHash(key, maskContentHash,
				GetRuntimeConfig(runtime));
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
			return key.pixelMode == AtlasPixelMode::A8
				&& key.renderMode == AtlasRenderMode::ShaderEffects;
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
				|| (!bitmap.alpha.empty() && bitmap.alpha.size()
					< static_cast<size_t>(rect.width) * rect.height))
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
				|| placement.maskType > static_cast<UInt8>(GlyphMaskType::DistanceField)
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
				"tnvse_freetype_font: atlas snapshot reused GPU-resident profile font=%u pages=%u placements=%llu",
				key.fontId, pageCount,
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
			std::vector<UInt8> pixels;
		};

		struct SnapshotPageData
		{
			AtlasCacheKey key;
			AtlasSnapshotHeader header;
			std::vector<AtlasSnapshotPlacement> placements;
			std::vector<UInt8> pixels;
			std::wstring path;
		};

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
			} identity = { header.width, header.height, header.padding,
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
				if (resource.pixelMode != AtlasPixelMode::A8
					|| resource.renderMode != AtlasRenderMode::ShaderEffects)
					return false;
				originalGpuBytes += GetAtlasStorageBytes(resource.width, resource.height,
					resource.pixelMode, resource.mipLevels);
				std::vector<AtlasSnapshotPlacement> placements;
				placements.reserve(resource.glyphs.size());
				for (const AtlasGlyphRecord& record : resource.glyphs)
				{
					AtlasSnapshotPlacement placement;
					if (!MakeSnapshotPlacement(resource, record.cacheId, record.rect, placement)
						|| !cacheIds.insert(record.cacheId).second)
						return false;
					placements.push_back(placement);
				}
				std::sort(placements.begin(), placements.end(),
					[](const auto& lhs, const auto& rhs) { return lhs.cacheId < rhs.cacheId; });
				std::vector<UInt8> packed;
				if (!BuildAtlasSnapshotPixels(resource, placements,
					AtlasSnapshotStorage::PlacedLevelZeroRects, packed))
					return false;
				size_t offset = 0;
				for (const AtlasSnapshotPlacement& placement : placements)
				{
					const size_t bytes = static_cast<size_t>(placement.rect.width)
						* placement.rect.height * AtlasBytesPerPixel(resource.pixelMode);
					if (offset > packed.size() || bytes > packed.size() - offset)
						return false;
					SnapshotGlyphData glyph;
					glyph.placement = placement;
					glyph.pixels.assign(packed.data() + offset, packed.data() + offset + bytes);
					glyphs.push_back(std::move(glyph));
					offset += bytes;
				}
				if (offset != packed.size())
					return false;
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

			const UInt32 maximumSize = std::min(GetMaximumAtlasSize(), kAtlasHardLimit);
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
				for (SnapshotGlyphData& glyph : packed)
				{
					page.placements.push_back(glyph.placement);
					page.pixels.insert(page.pixels.end(),
						glyph.pixels.begin(), glyph.pixels.end());
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
			if (storageMode != AtlasSnapshotStorage::PlacedLevelZeroRects)
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
		if (TryReuseCompleteAtlasProfile(key))
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
		UInt64 releasedResetPixelBytes = 0;
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
			const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'A', 'T', 'L', '8' };
			const AtlasSnapshotStorage expectedStorage = UsesPlacedLevelZeroSnapshot(pageKey)
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
				|| !header.pageContentHash
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
			const UInt8* pixelData = payload + placementsBytes;
			std::vector<UInt8> storedPixelVector(pixelData,
				pixelData + static_cast<size_t>(header.pixelBytes));
			if (header.pageContentHash != ComputeAtlasPageContentHash(header,
				placementList, storedPixelVector))
				return false;
			resource->glyphs.reserve(header.placementCount);
			for (UInt32 index = 0; index < header.placementCount; ++index)
			{
				const AtlasRect& rect = placementList[index].rect;
				if (!placementList[index].cacheId || !rect.width || !rect.height
					|| rect.x > header.width || rect.width > header.width - rect.x
					|| rect.y > header.height || rect.height > header.height - rect.y
					|| !IsValidSnapshotPlacement(placementList[index]))
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
			// v8 shader pages keep only placed level-zero texels. DEFAULT-pool restore
			// regenerates the mip chain directly; later resets stream these texels from
			// the validated snapshot instead of retaining a second CPU pixel copy.
			const bool compactDefaultEligible = g_bEnableFreeTypeDefaultPoolAtlas
				&& !State().defaultPoolShutdown
				&& pageKey.pixelMode == AtlasPixelMode::A8
				&& pageKey.renderMode == AtlasRenderMode::ShaderEffects
				&& expectedStorage == AtlasSnapshotStorage::PlacedLevelZeroRects;
			bool restoredToDefaultPool = false;
			if (compactDefaultEligible)
			{
				size_t packedBytes = 0;
				if (!GetPlacedLevelZeroSnapshotBytes(compactSnapshot->placements,
					header.width, header.height, pageKey.pixelMode, packedBytes)
					|| packedBytes != header.pixelBytes)
				{
					return false;
				}
				compactSnapshot->pixels = std::move(storedPixelVector);
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
					releasedResetPixelBytes += compactSnapshot->pixels.size();
					std::vector<UInt8>().swap(compactSnapshot->pixels);
				}
			}
			if (!restoredToDefaultPool)
			{
				resource->backend = AtlasBackend::Managed;
				std::vector<UInt8> pixels;
				if (!DecodeAtlasSnapshotPixels(header, compactSnapshot->placements,
					pixelData, pixels))
					return false;
				NiTexturingProperty* property = CreateManagedAtlasProperty(header.width,
					header.height, pageKey.pixelMode, header.mipLevels,
					pixels, resource->pixelData);
				if (!property)
					return false;
				resource->property = property;
				releasedResetPixelBytes += compactSnapshot->pixels.size();
				std::vector<UInt8>().swap(compactSnapshot->pixels);
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
				const size_t storageBytes = page.second->sharedGpuPage ? 0
					: GetAtlasStorageBytes(page.second->width,
						page.second->height, page.second->pixelMode, page.second->mipLevels);
				state.atlasLru.push_front(page.first);
				state.atlasCache.emplace(page.first, AtlasCacheEntry{
					page.second, storageBytes, state.atlasLru.begin() });
				IndexAtlasPage(state, page.first, *page.second);
				state.atlasCacheBytes += storageBytes;
			}
			TrimAtlasCache(state);
			if (IsCompleteAtlasProfileResidentLocked(state, key))
				state.completeAtlasProfiles.insert(MakeAtlasProfileKey(key));
		}
		gLog.FormattedMessage(
			"tnvse_freetype_font: atlas snapshot restored font=%u pages=%u defaultPoolPages=%u deduplicatedPages=%u deduplicatedGpuBytes=%llu placements=%llu bytes=%llu releasedResetPixelBytes=%llu",
			key.fontId, pageCount, defaultPoolPages, deduplicatedPages,
			static_cast<unsigned long long>(deduplicatedGpuBytes),
			static_cast<unsigned long long>(totalPlacements),
			static_cast<unsigned long long>(totalBytes),
			static_cast<unsigned long long>(releasedResetPixelBytes));
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
		for (size_t index = 0; index < pages.size(); ++index)
		{
			SnapshotPageData& page = pages[index];
			page.key.pageIndex = static_cast<UInt16>(index);
			const AtlasSnapshotStorage storageMode = UsesPlacedLevelZeroSnapshot(page.key)
				? AtlasSnapshotStorage::PlacedLevelZeroRects
				: AtlasSnapshotStorage::FullMipChain;
			const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'A', 'T', 'L', '8' };
			std::memcpy(page.header.magic, magic, sizeof(magic));
				page.header.version = kAtlasSnapshotVersion;
				page.header.headerSize = sizeof(page.header);
				page.header.snapshotHash = snapshotHash;
				page.header.maskContentHash = maskContentHash;
				page.header.atlasContentHash = page.key.atlasContentHash;
				page.header.reservedFontId = 0;
				page.header.scaleMilli = page.key.scaleMilli;
				page.header.mipLevels = GetAtlasMipLevelCount(page.header.width,
					page.header.height, page.key.levelZeroOnly);
				page.header.pixelMode = static_cast<UInt8>(page.key.pixelMode);
				page.header.renderMode = static_cast<UInt8>(page.key.renderMode);
				page.header.storageMode = static_cast<UInt8>(storageMode);
				page.header.pageIndex = static_cast<UInt16>(index);
				page.header.pageCount = static_cast<UInt16>(pages.size());
				page.header.placementCount = static_cast<UInt32>(page.placements.size());
				page.header.pixelBytes = page.pixels.size();
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
					return false;
				const std::wstring temporary = page.path + L".tmp";
				HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
					CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
				if (file == INVALID_HANDLE_VALUE)
					return false;
				const bool sparse = TryEnableSparseFile(file);
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
				snapshotGpuBytes += GetAtlasStorageBytes(page.header.width,
					page.header.height, page.key.pixelMode, page.header.mipLevels);
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
		{
			std::lock_guard<std::mutex> lock(State().atlasMutex);
			if (IsCompleteAtlasProfileResidentLocked(State(), key))
				State().completeAtlasProfiles.insert(MakeAtlasProfileKey(key));
		}
		gLog.FormattedMessage(
			"tnvse_freetype_font: atlas snapshot saved font=%u pages=%u->%u placements=%llu bytes=%llu gpuBytes=%llu->%llu saved=%llu tail=%ux%u",
			key.fontId, sourcePageCount, pageCount,
			static_cast<unsigned long long>(totalPlacements),
			static_cast<unsigned long long>(totalBytes),
			static_cast<unsigned long long>(originalGpuBytes),
			static_cast<unsigned long long>(snapshotGpuBytes),
			static_cast<unsigned long long>(originalGpuBytes > snapshotGpuBytes
				? originalGpuBytes - snapshotGpuBytes : 0),
			pages.back().header.width, pages.back().header.height);
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

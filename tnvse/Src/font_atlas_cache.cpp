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

namespace fonthook::vectorfont
{
	namespace
	{
		AtlasState s_atlasState;
	}

	AtlasState& State()
	{
		return s_atlasState;
	}

		UInt32 NextPowerOfTwo(UInt32 value)
		{
			if (value <= 1)
				return 1;
			--value;
			value |= value >> 1;
			value |= value >> 2;
			value |= value >> 4;
			value |= value >> 8;
			value |= value >> 16;
			return value + 1;
		}

		UInt32 GetMaximumAtlasSize()
		{
			UInt32 result = kAtlasHardLimit;
			if (NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton())
			{
				result = std::min(result, std::min(renderer->m_kD3DCaps9.MaxTextureWidth,
					renderer->m_kD3DCaps9.MaxTextureHeight));
			}
			return std::max<UInt32>(64, result);
		}

		UInt32 ResolveAutomaticGpuBudgetMB(UInt32& availableMB)
		{
			availableMB = 0;
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer ? renderer->GetD3DDevice() : nullptr;
			if (!device)
				return kAutomaticAtlasBudgetFallbackMB;
			availableMB = device->GetAvailableTextureMem() / (1024u * 1024u);
			if (availableMB < 64 || availableMB > 4096)
				return kAutomaticAtlasBudgetFallbackMB;
			UInt32 budget = availableMB / 8u;
			budget = (budget / kAutomaticAtlasBudgetQuantumMB)
				* kAutomaticAtlasBudgetQuantumMB;
			return std::clamp(budget, kAutomaticAtlasBudgetMinimumMB,
				kAutomaticAtlasBudgetMaximumMB);
		}

		void ResolveGpuAtlasBudget(bool force)
		{
			if (State().budgetResolved && !force)
				return;
			const size_t previous = State().resolvedGpuBudgetBytes;
			UInt32 availableMB = 0;
			UInt32 resolvedMB = g_uiFreeTypeFontGpuAtlasCacheMB;
			const bool automatic = resolvedMB == 0;
			if (automatic)
				resolvedMB = ResolveAutomaticGpuBudgetMB(availableMB);
			State().lastAvailableTextureMemoryMB = availableMB;
			State().resolvedGpuBudgetBytes = static_cast<size_t>(resolvedMB)
				* 1024u * 1024u;
			State().budgetResolved = true;
			if (!previous)
			{
				if (automatic)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: GPU atlas cache budget configured=0 resolved=%uMB source=automatic availableTextureMem=%uMB",
						resolvedMB, availableMB);
				}
				else
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: GPU atlas cache budget configured=%u resolved=%uMB source=configured",
						g_uiFreeTypeFontGpuAtlasCacheMB, resolvedMB);
				}
			}
			else if (previous != State().resolvedGpuBudgetBytes)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: GPU atlas cache budget changed old=%uMB new=%uMB availableTextureMem=%uMB",
					static_cast<UInt32>(previous / (1024u * 1024u)), resolvedMB,
					availableMB);
			}
		}

		size_t GetAtlasCacheLimit()
		{
			if (!g_bEnableFreeTypeDefaultPoolAtlas)
			{
				return static_cast<size_t>(g_uiFreeTypeFontMemoryCacheMB)
					* 1024u * 1024u / 2u;
			}
			ResolveGpuAtlasBudget(false);
			return State().resolvedGpuBudgetBytes
				? State().resolvedGpuBudgetBytes
				: static_cast<size_t>(kAutomaticAtlasBudgetFallbackMB) * 1024u * 1024u;
		}


		bool PackAtWidth(const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
			UInt32 atlasWidth, UInt32 maximumSize, UInt32& atlasHeight,
			std::unordered_map<UInt64, AtlasRect>& placements, UInt32 padding)
		{
			placements.clear();
			UInt32 x = padding;
			UInt32 y = padding;
			UInt32 shelfHeight = 0;
			for (const auto& bitmap : bitmaps)
			{
				const UInt32 width = static_cast<UInt32>(bitmap->width);
				const UInt32 height = static_cast<UInt32>(bitmap->height);
				if (width + padding * 2 > atlasWidth
					|| height + padding * 2 > maximumSize)
					return false;
				if (x + width + padding > atlasWidth)
				{
					x = padding;
					y += shelfHeight;
					shelfHeight = 0;
				}
				if (y + height + padding > maximumSize)
					return false;
				placements[bitmap->cacheId] = { x, y, width, height };
				x += width + padding * 2;
				shelfHeight = std::max(shelfHeight, height + padding * 2);
			}
			atlasHeight = NextPowerOfTwo(y + shelfHeight);
			return atlasHeight <= maximumSize;
		}

		bool PackAtlas(const std::vector<std::shared_ptr<const GlyphBitmap>>& source,
			UInt32& atlasWidth, UInt32& atlasHeight,
			std::unordered_map<UInt64, AtlasRect>& placements, UInt32 padding)
		{
			std::vector<std::shared_ptr<const GlyphBitmap>> bitmaps = source;
			std::sort(bitmaps.begin(), bitmaps.end(), [](const auto& lhs, const auto& rhs)
			{
				if (lhs->height != rhs->height)
					return lhs->height > rhs->height;
				if (lhs->width != rhs->width)
					return lhs->width > rhs->width;
				return lhs->cacheId < rhs->cacheId;
			});

			const UInt32 maximumSize = GetMaximumAtlasSize();
			UInt64 bestArea = UINT64_MAX;
			std::unordered_map<UInt64, AtlasRect> candidate;
			for (UInt32 width = 64; width <= maximumSize; width <<= 1)
			{
				UInt32 height = 0;
				if (!PackAtWidth(bitmaps, width, maximumSize, height, candidate, padding))
					continue;
				const UInt64 area = static_cast<UInt64>(width) * height;
				if (area < bestArea)
				{
					bestArea = area;
					atlasWidth = width;
					atlasHeight = height;
					placements = candidate;
				}
			}
			return bestArea != UINT64_MAX;
		}

		void TouchAtlasEntry(AtlasState& state, AtlasCacheEntry& entry,
			const AtlasCacheKey& key)
		{
			state.atlasLru.splice(state.atlasLru.begin(), state.atlasLru, entry.lru);
			entry.lru = state.atlasLru.begin();
		}

		AtlasProfileKey MakeAtlasProfileKey(const AtlasCacheKey& key)
		{
			return {
				key.atlasContentHash,
				key.scaleMilli,
				key.pixelMode,
				key.renderMode,
				key.padding,
				key.levelZeroOnly
			};
		}

		void IndexAtlasPage(AtlasState& state, const AtlasCacheKey& key,
			const AtlasResource& resource)
		{
			AtlasProfileIndex& profile = state.atlasProfiles[MakeAtlasProfileKey(key)];
			const auto page = std::lower_bound(profile.pages.begin(), profile.pages.end(),
				key.pageIndex);
			if (page == profile.pages.end() || *page != key.pageIndex)
				profile.pages.insert(page, key.pageIndex);

			// Re-indexing an appended or restored page used to scan every resident in
			// the profile. Track each page's own IDs so updates stay proportional to
			// that page while preserving the same cacheId -> page result.
			std::vector<UInt64>& pageResidents = profile.pageResidents[key.pageIndex];
			for (UInt64 cacheId : pageResidents)
			{
				const auto resident = profile.residentPages.find(cacheId);
				if (resident != profile.residentPages.end()
					&& resident->second == key.pageIndex)
				{
					profile.residentPages.erase(resident);
				}
			}
			pageResidents.clear();
			pageResidents.reserve(resource.placements.size());
			for (const auto& placement : resource.placements)
			{
				profile.residentPages[placement.first] = key.pageIndex;
				pageResidents.push_back(placement.first);
			}
		}

		void UnindexAtlasPage(AtlasState& state, const AtlasCacheKey& key)
		{
			const AtlasProfileKey profileKey = MakeAtlasProfileKey(key);
			auto found = state.atlasProfiles.find(profileKey);
			if (found == state.atlasProfiles.end())
				return;
			AtlasProfileIndex& profile = found->second;
			const auto page = std::lower_bound(profile.pages.begin(), profile.pages.end(),
				key.pageIndex);
			if (page != profile.pages.end() && *page == key.pageIndex)
				profile.pages.erase(page);
			const auto pageResidents = profile.pageResidents.find(key.pageIndex);
			if (pageResidents != profile.pageResidents.end())
			{
				for (UInt64 cacheId : pageResidents->second)
				{
					const auto resident = profile.residentPages.find(cacheId);
					if (resident != profile.residentPages.end()
						&& resident->second == key.pageIndex)
					{
						profile.residentPages.erase(resident);
					}
				}
				profile.pageResidents.erase(pageResidents);
			}
			if (profile.pages.empty())
				state.atlasProfiles.erase(found);
		}

		void TrimAtlasCache(AtlasState& state)
		{
			while (state.atlasCacheBytes > GetAtlasCacheLimit() && !state.atlasLru.empty())
			{
				const AtlasCacheKey key = state.atlasLru.back();
				auto it = state.atlasCache.find(key);
				if (it != state.atlasCache.end())
				{
					RetireDefaultGeneration(*it->second.resource);
					UnindexAtlasPage(state, key);
					state.atlasCacheBytes -= it->second.bytes;
					state.atlasCache.erase(it);
				}
				state.atlasLru.pop_back();
			}
		}

		bool PlaceBitmap(AtlasResource& resource, const GlyphBitmap& bitmap, AtlasRect& rect)
		{
			const UInt32 padding = resource.padding;
			const UInt32 width = static_cast<UInt32>(bitmap.width);
			const UInt32 height = static_cast<UInt32>(bitmap.height);
			if (width + padding * 2 > resource.width
				|| height + padding * 2 > resource.height)
				return false;
			if (resource.cursorX + width + padding > resource.width)
			{
				resource.cursorX = padding;
				resource.cursorY += resource.shelfHeight;
				resource.shelfHeight = 0;
			}
			if (resource.cursorY + height + padding > resource.height)
				return false;
			rect = { resource.cursorX, resource.cursorY, width, height };
			resource.cursorX += width + padding * 2;
			resource.shelfHeight = std::max(resource.shelfHeight,
				height + padding * 2);
			return true;
		}

		void TrimBatchCache(AtlasState& state)
		{
			const size_t limit = static_cast<size_t>(g_uiFreeTypeFontMemoryCacheMB)
				* 1024u * 1024u / 12u;
			while (state.batchCacheBytes > limit && !state.batchLru.empty())
			{
				const BatchTemplateKey key = state.batchLru.back();
				auto existing = state.batchCache.find(key);
				if (existing != state.batchCache.end())
				{
					state.batchCacheBytes -= existing->second.bytes;
					state.batchCache.erase(existing);
				}
				state.batchLru.pop_back();
			}
		}


		UInt64 BuildAtlasContentHash(UInt64 maskGenerationHash,
			UInt8 maskCombination, UInt8 sdfSpread,
			SInt32 outlineStroke, SInt32 glowStroke, UInt64 cpuCoverageHash,
			UInt64 bakedColorHash)
		{
			UInt64 hash = 1469598103934665603ull;
			auto add = [&](const void* data, size_t size)
			{
				const UInt8* bytes = static_cast<const UInt8*>(data);
				for (size_t index = 0; index < size; ++index)
				{
					hash ^= bytes[index];
					hash *= 1099511628211ull;
				}
			};
			add(&maskGenerationHash, sizeof(maskGenerationHash));
			add(&maskCombination, sizeof(maskCombination));
			add(&sdfSpread, sizeof(sdfSpread));
			add(&outlineStroke, sizeof(outlineStroke));
			add(&glowStroke, sizeof(glowStroke));
			add(&cpuCoverageHash, sizeof(cpuCoverageHash));
			add(&bakedColorHash, sizeof(bakedColorHash));
			return hash;
		}

		UInt64 BuildCpuCoverageHash(const FontConfig& config, float rasterScale)
		{
			UInt64 hash = 1469598103934665603ull;
			auto add = [&](const void* data, size_t size)
			{
				const UInt8* bytes = static_cast<const UInt8*>(data);
				for (size_t index = 0; index < size; ++index)
				{
					hash ^= bytes[index];
					hash *= 1099511628211ull;
				}
			};
			auto addEffect = [&](const EffectStyle& effect)
			{
				add(&effect.enabled, sizeof(effect.enabled));
				if (!effect.enabled)
					return;
				const SInt32 width = static_cast<SInt32>(std::lround(
					effect.width * rasterScale * 64.0f));
				const SInt32 blur = static_cast<SInt32>(std::lround(
					effect.blur * rasterScale * 64.0f));
				const SInt32 inner = static_cast<SInt32>(std::lround(
					effect.inner * rasterScale * 64.0f));
				const SInt32 outer = static_cast<SInt32>(std::lround(
					effect.outer * rasterScale * 64.0f));
				const SInt32 softness = static_cast<SInt32>(std::lround(
					effect.softness * rasterScale * 64.0f));
				add(&width, sizeof(width));
				add(&blur, sizeof(blur));
				add(&inner, sizeof(inner));
				add(&outer, sizeof(outer));
				add(&softness, sizeof(softness));
				add(&effect.power, sizeof(effect.power));
			};
			addEffect(config.glow);
			addEffect(config.outline);
			addEffect(config.shadow);
			return hash;
		}

		UInt64 BuildAtlasContentHash(const FontConfig& config,
			const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
			float rasterScale, AtlasRenderMode renderMode)
		{
			UInt8 combination = 0;
			UInt8 sdfSpread = 0;
			SInt32 outlineStroke = 0;
			SInt32 glowStroke = 0;
			std::vector<UInt64> bakedVariants;
			for (const auto& bitmap : bitmaps)
			{
				if (!bitmap)
					continue;
				combination |= static_cast<UInt8>(1u
					<< static_cast<UInt8>(bitmap->maskType));
				if (bitmap->maskType == GlyphMaskType::DistanceField)
					sdfSpread = std::max(sdfSpread, bitmap->sdfSpread);
				else if (bitmap->maskType == GlyphMaskType::Outline)
					outlineStroke = bitmap->strokeWidth26Dot6;
				else if (bitmap->maskType == GlyphMaskType::Glow)
					glowStroke = bitmap->strokeWidth26Dot6;
				if (bitmap->colorBaked)
				{
					bakedVariants.push_back((static_cast<UInt64>(bitmap->bakedLayer) << 32)
						| bitmap->bakedRgba);
				}
			}
			std::sort(bakedVariants.begin(), bakedVariants.end());
			bakedVariants.erase(std::unique(bakedVariants.begin(), bakedVariants.end()),
				bakedVariants.end());
			UInt64 bakedColorHash = 1469598103934665603ull;
			for (UInt64 variant : bakedVariants)
			{
				for (size_t index = 0; index < sizeof(variant); ++index)
				{
					bakedColorHash ^= reinterpret_cast<const UInt8*>(&variant)[index];
					bakedColorHash *= 1099511628211ull;
				}
			}
			return BuildAtlasContentHash(config.maskGenerationHash,
				combination, sdfSpread, outlineStroke, glowStroke,
				renderMode == AtlasRenderMode::CpuEffects
					? BuildCpuCoverageHash(config, rasterScale) : 0,
				bakedColorHash);
		}

		bool UsesLevelZeroOnly(const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps)
		{
			bool found = false;
			for (const auto& bitmap : bitmaps)
			{
				if (!bitmap)
					continue;
				found = true;
				if (bitmap->maskType != GlyphMaskType::DistanceField)
					return false;
			}
			return found;
		}

		UInt64 BuildPrewarmAtlasContentHash(const FontConfig& config,
			float rasterScale, bool shaderEffects)
		{
			UInt8 combination = 0;
			UInt8 sdfSpread = 0;
			SInt32 outlineStroke = 0;
			SInt32 glowStroke = 0;
			auto include = [&](GlyphMaskType type)
			{
				combination |= static_cast<UInt8>(1u << static_cast<UInt8>(type));
			};
			if (shaderEffects)
			{
				if (!UsesSdfFill(config))
					include(GlyphMaskType::Fill);
				if (NeedsSdfMask(config))
				{
					UInt32 resolvedSpread = 0;
					if (ResolveSdfSpread(config, rasterScale, resolvedSpread))
					{
						include(GlyphMaskType::DistanceField);
						sdfSpread = static_cast<UInt8>(resolvedSpread);
					}
				}
			}
			else
			{
				include(GlyphMaskType::Fill);
				if (config.glow.enabled)
				{
					include(GlyphMaskType::Glow);
					glowStroke = static_cast<SInt32>(std::lround(
						config.glow.width * rasterScale * 64.0f));
				}
				if (config.outline.enabled)
				{
					include(GlyphMaskType::Outline);
					outlineStroke = static_cast<SInt32>(std::lround(
						config.outline.width * rasterScale * 64.0f));
				}
			}
			return BuildAtlasContentHash(config.maskGenerationHash,
				combination, sdfSpread, outlineStroke, glowStroke,
				shaderEffects ? 0 : BuildCpuCoverageHash(config, rasterScale),
				1469598103934665603ull);
		}

		std::vector<std::shared_ptr<AtlasResource>> GetAtlasResources(
			const FontConfig& config, float rasterScale,
			const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
			AtlasPixelMode pixelMode, AtlasRenderMode renderMode,
			UInt32 padding)
		{
			AtlasState& state = State();
			const AtlasCacheKey baseKey = {
				BuildAtlasContentHash(config, bitmaps, rasterScale, renderMode),
				config.fontId,
				static_cast<UInt32>(std::lround(rasterScale * 1000.0f)),
				pixelMode,
				renderMode,
				padding,
				UsesLevelZeroOnly(bitmaps)
			};

			std::lock_guard<std::mutex> lock(state.atlasMutex);
			std::vector<std::pair<AtlasCacheKey, AtlasCacheEntry*>> entries;
			const AtlasProfileKey profileKey = MakeAtlasProfileKey(baseKey);
			auto profile = state.atlasProfiles.find(profileKey);
			if (profile != state.atlasProfiles.end())
			{
				entries.reserve(profile->second.pages.size());
				for (UInt16 pageIndex : profile->second.pages)
				{
					AtlasCacheKey pageKey = baseKey;
					pageKey.pageIndex = pageIndex;
					auto page = state.atlasCache.find(pageKey);
					if (page != state.atlasCache.end() && page->second.resource)
						entries.push_back({ pageKey, &page->second });
				}
			}
			std::vector<std::shared_ptr<AtlasResource>> pages;
			pages.reserve(entries.size() + 1);
			for (auto& entry : entries)
			{
				TouchAtlasEntry(state, *entry.second, entry.first);
				pages.push_back(entry.second->resource);
			}
			if (!pages.empty())
				RecordFreeTypePerf(FreeTypePerfCounter::AtlasHit);

			std::vector<std::shared_ptr<const GlyphBitmap>> missing;
			missing.reserve(bitmaps.size());
			for (const auto& bitmap : bitmaps)
			{
				if (!bitmap)
					continue;
				const bool resident = profile != state.atlasProfiles.end()
					&& profile->second.residentPages.count(bitmap->cacheId) != 0;
				if (!resident)
					missing.push_back(bitmap);
			}
			if (missing.empty() && !pages.empty())
				return pages;

			if (!pages.empty() && AddBitmapsToAtlas(*pages.back(), missing))
			{
				IndexAtlasPage(state, entries.back().first, *pages.back());
				AtlasCacheEntry* entry = entries.back().second;
				const size_t bytes = GetAtlasStorageBytes(pages.back()->width,
					pages.back()->height, pages.back()->pixelMode, pages.back()->mipLevels);
				if (bytes != entry->bytes)
				{
					state.atlasCacheBytes -= entry->bytes;
					entry->bytes = bytes;
					state.atlasCacheBytes += bytes;
					TrimAtlasCache(state);
				}
				return pages;
			}
			if (entries.size() >= kMaximumAtlasSnapshotPages)
				return {};

			auto resource = std::make_shared<AtlasResource>();
			RecordFreeTypePerf(FreeTypePerfCounter::AtlasCreated);
			resource->pixelMode = pixelMode;
			resource->backend = g_bEnableFreeTypeDefaultPoolAtlas
				? AtlasBackend::DefaultPool : AtlasBackend::Managed;
			resource->renderMode = renderMode;
			resource->levelZeroOnly = baseKey.levelZeroOnly;
			resource->padding = padding;
			resource->width = std::min<UInt32>(512, GetMaximumAtlasSize());
			resource->height = resource->width;
			resource->mipLevels = GetAtlasMipLevelCount(
				resource->width, resource->height, resource->levelZeroOnly);
			resource->cursorX = padding;
			resource->cursorY = padding;
			if (resource->backend == AtlasBackend::Managed)
			{
				resource->pixels.assign(static_cast<size_t>(resource->width)
					* resource->height * AtlasBytesPerPixel(resource->pixelMode), 0u);
			}
			if (!AddBitmapsToAtlas(*resource, missing))
				return {};
			const size_t bytes = GetAtlasStorageBytes(resource->width,
				resource->height, resource->pixelMode, resource->mipLevels);
			AtlasCacheKey pageKey = baseKey;
			pageKey.pageIndex = entries.empty() ? 0
				: static_cast<UInt16>(entries.back().first.pageIndex + 1);
			state.atlasLru.push_front(pageKey);
			state.atlasCache.emplace(pageKey,
				AtlasCacheEntry{ resource, bytes, state.atlasLru.begin() });
			IndexAtlasPage(state, pageKey, *resource);
			state.atlasCacheBytes += bytes;
			TrimAtlasCache(state);
			pages.push_back(resource);
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_font: atlas page created font=%u page=%u size=%ux%u pixelMode=%u renderMode=%u",
					pageKey.fontId, pageKey.pageIndex, resource->width, resource->height,
					static_cast<UInt32>(resource->pixelMode),
					static_cast<UInt32>(resource->renderMode));
			}
			return pages;
		}

		std::shared_ptr<AtlasResource> CreateTransientAtlas(
			const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
			AtlasPixelMode pixelMode, AtlasRenderMode renderMode,
			UInt32 padding)
		{
			auto resource = std::make_shared<AtlasResource>();
			resource->pixelMode = pixelMode;
			resource->backend = g_bEnableFreeTypeDefaultPoolAtlas
				? AtlasBackend::DefaultPool : AtlasBackend::Managed;
			resource->renderMode = renderMode;
			resource->levelZeroOnly = UsesLevelZeroOnly(bitmaps);
			resource->padding = padding;
			resource->transient = true;
			if (!PackAtlas(bitmaps, resource->width, resource->height,
				resource->placements, padding))
			{
				return nullptr;
			}
			resource->mipLevels = GetAtlasMipLevelCount(
				resource->width, resource->height, resource->levelZeroOnly);
			for (const auto& bitmap : bitmaps)
			{
				if (bitmap)
					resource->residentBitmaps[bitmap->cacheId] = bitmap;
			}
			if (resource->backend == AtlasBackend::DefaultPool
				&& CreateDefaultPoolAtlas(*resource, pixelMode))
			{
				return resource;
			}
			resource->backend = AtlasBackend::Managed;
			resource->pixelMode = AtlasPixelMode::Argb32;
			resource->mipLevels = GetAtlasMipLevelCount(
				resource->width, resource->height, resource->levelZeroOnly);
			resource->pixels.assign(static_cast<size_t>(resource->width)
				* resource->height * AtlasBytesPerPixel(resource->pixelMode), 0u);
			for (const auto& bitmap : bitmaps)
			{
				if (bitmap)
					CopyBitmapToAtlas(*resource, *bitmap,
						resource->placements.at(bitmap->cacheId));
			}
			return RecreateManagedAtlasProperty(*resource) ? resource : nullptr;
		}
}

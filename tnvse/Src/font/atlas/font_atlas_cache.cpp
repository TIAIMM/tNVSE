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

	bool InvalidateCompleteAtlasProfileLocked(AtlasState& state,
		const AtlasProfileKey& profileKey)
	{
		const bool wasComplete =
			state.completeAtlasProfiles.erase(profileKey) != 0;
		const auto profile = state.atlasProfiles.find(profileKey);
		if (profile != state.atlasProfiles.end())
		{
			if (profile->second.directGlyphs
				&& profile->second.directGlyphs->validity)
			{
				profile->second.directGlyphs->validity->store(
					false, std::memory_order_release);
			}
			profile->second.directGlyphs.reset();
		}
		return wasComplete;
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

		void ResolveGpuAtlasBudget(bool force)
		{
			if (State().budgetResolved && !force)
				return;
			const bool wasResolved = State().budgetResolved;
			const size_t previous = State().resolvedGpuBudgetBytes;
			const bool unlimited = g_uiFreeTypeFontGpuAtlasCacheMB == 0;
			State().resolvedGpuBudgetBytes = unlimited ? 0
				: static_cast<size_t>(g_uiFreeTypeFontGpuAtlasCacheMB)
					* 1024u * 1024u;
			State().budgetResolved = true;
			if (!wasResolved)
			{
				if (unlimited)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: GPU atlas cache budget configured=0 resolved=unlimited policy=full-resident");
				}
				else
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: GPU atlas cache budget configured=%u resolved=%uMB source=configured",
						g_uiFreeTypeFontGpuAtlasCacheMB,
						g_uiFreeTypeFontGpuAtlasCacheMB);
				}
			}
			else if (previous != State().resolvedGpuBudgetBytes)
			{
				if (unlimited)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: GPU atlas cache budget changed old=%uMB new=unlimited policy=full-resident",
						static_cast<UInt32>(previous / (1024u * 1024u)));
				}
				else
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: GPU atlas cache budget changed old=unlimited new=%uMB source=configured",
						g_uiFreeTypeFontGpuAtlasCacheMB);
				}
			}
		}

		bool IsGpuAtlasCacheUnlimited()
		{
			return g_bEnableFreeTypeDefaultPoolAtlas
				&& g_uiFreeTypeFontGpuAtlasCacheMB == 0;
		}

		size_t GetAtlasCacheLimit()
		{
			if (!g_bEnableFreeTypeDefaultPoolAtlas)
			{
				return static_cast<size_t>(g_uiFreeTypeFontMemoryCacheMB)
					* 1024u * 1024u / 2u;
			}
			ResolveGpuAtlasBudget(false);
			return IsGpuAtlasCacheUnlimited()
				? std::numeric_limits<size_t>::max()
				: State().resolvedGpuBudgetBytes;
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

		void PartitionBitmapsForAtlasPage(const AtlasResource& resource,
			const std::vector<std::shared_ptr<const GlyphBitmap>>& source,
			std::vector<std::shared_ptr<const GlyphBitmap>>& accepted,
			std::vector<std::shared_ptr<const GlyphBitmap>>& remaining)
		{
			accepted.clear();
			remaining.clear();
			accepted.reserve(source.size());
			remaining.reserve(source.size());
			const UInt32 maximum = GetMaximumAtlasSize();
			const UInt32 padding = resource.padding;
			UInt32 cursorX = resource.cursorX;
			UInt32 cursorY = resource.cursorY;
			UInt32 shelfHeight = resource.shelfHeight;
			for (const auto& bitmap : source)
			{
				if (!bitmap)
					continue;
				const UInt32 width = static_cast<UInt32>(bitmap->width);
				const UInt32 height = static_cast<UInt32>(bitmap->height);
				UInt32 nextX = cursorX;
				UInt32 nextY = cursorY;
				UInt32 nextShelf = shelfHeight;
				bool fits = width + padding * 2 <= maximum
					&& height + padding * 2 <= maximum;
				if (fits && nextX + width + padding > maximum)
				{
					nextX = padding;
					nextY += nextShelf;
					nextShelf = 0;
				}
				fits = fits && nextY + height + padding <= maximum;
				if (!fits)
				{
					remaining.push_back(bitmap);
					continue;
				}
				accepted.push_back(bitmap);
				cursorX = nextX + width + padding * 2;
				cursorY = nextY;
				shelfHeight = std::max(nextShelf, height + padding * 2);
			}
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
				key.levelZeroOnly,
				key.byteClass
			};
		}

		void RefreshAtlasProfileCpuMemory(AtlasProfileIndex& profile)
		{
			size_t bytes = sizeof(AtlasProfileIndex)
				+ profile.pages.capacity() * sizeof(UInt16)
				+ profile.residentPages.size()
					* (sizeof(std::pair<const UInt64, UInt16>) + 3u * sizeof(void*))
				+ profile.pageResidents.size()
					* (sizeof(std::pair<const UInt16, std::vector<UInt64>>)
						+ 3u * sizeof(void*))
				+ profile.duplicateResidents.size()
					* (sizeof(UInt64) + 2u * sizeof(void*));
			for (const auto& page : profile.pageResidents)
				bytes += page.second.capacity() * sizeof(UInt64);
			profile.cpuMemory.Reset(CpuMemoryCategory::AtlasMetadata, bytes);
		}

		bool EnsureAtlasProfileIndexLocked(AtlasState& state,
			const AtlasCacheKey& baseKey, AtlasProfileIndex& profile)
		{
			if (!profile.compactResidentIndexReleased)
				return true;
			profile.residentPages.clear();
			profile.pageResidents.clear();
			profile.duplicateResidents.clear();
			for (UInt16 pageIndex : profile.pages)
			{
				AtlasCacheKey pageKey = baseKey;
				pageKey.pageIndex = pageIndex;
				const auto page = state.atlasCache.find(pageKey);
				if (page == state.atlasCache.end()
					|| !page->second.resource
					|| !EnsureAtlasGlyphIndex(
						*page->second.resource))
				{
					return false;
				}
				std::vector<UInt64>& residents =
					profile.pageResidents[pageIndex];
				residents.reserve(
					page->second.resource->glyphs.size());
				for (const AtlasGlyphRecord& glyph :
					page->second.resource->glyphs)
				{
					const auto [resident, inserted] =
						profile.residentPages.emplace(
							glyph.cacheId, pageIndex);
					if (!inserted
						&& resident->second != pageIndex)
					{
						profile.duplicateResidents.insert(
							glyph.cacheId);
						resident->second = std::min(
							resident->second, pageIndex);
					}
					residents.push_back(glyph.cacheId);
				}
			}
			profile.compactResidentIndexReleased = false;
			RefreshAtlasProfileCpuMemory(profile);
			return true;
		}

		void ReleaseSealedAtlasCpuIndexesLocked(AtlasState& state,
			const SealedDirectFontProfile& sealed)
		{
			for (auto& [key, profile] : state.atlasProfiles)
			{
				bool ownedBySealed = false;
				for (const auto& table : sealed.tables)
				{
					if (table
						&& profile.directGlyphs.get()
							== table.get())
					{
						ownedBySealed = true;
						break;
					}
				}
				if (!ownedBySealed)
					continue;
				std::unordered_map<UInt64, UInt16>().swap(
					profile.residentPages);
				std::unordered_map<UInt16,
					std::vector<UInt64>>().swap(
						profile.pageResidents);
				std::unordered_set<UInt64>().swap(
					profile.duplicateResidents);
				profile.compactResidentIndexReleased = true;
				RefreshAtlasProfileCpuMemory(profile);
			}
			for (const auto& atlas : sealed.atlases)
			{
				if (!atlas || !atlas->compactSnapshot)
					continue;
				std::vector<AtlasGlyphRecord>().swap(
					atlas->glyphs);
				atlas->compactGlyphIndexReleased = true;
				if (atlas->backend == AtlasBackend::DefaultPool)
					std::vector<UInt8>().swap(atlas->pixels);
				RefreshAtlasResourceCpuMemory(*atlas);
			}
		}

		void IndexAtlasPage(AtlasState& state, const AtlasCacheKey& key,
			const AtlasResource& resource)
		{
			AtlasProfileIndex& profile = state.atlasProfiles[MakeAtlasProfileKey(key)];
			EnsureAtlasProfileIndexLocked(state, key, profile);
			// A direct table stores page slots and page-local glyph indices. Any page
			// insertion or replacement invalidates that immutable view; complete
			// prewarm republishes it after the profile generation is finalized.
			if (profile.directGlyphs
				&& profile.directGlyphs->validity)
			{
				profile.directGlyphs->validity->store(
					false, std::memory_order_release);
			}
			profile.directGlyphs.reset();
			const auto page = std::lower_bound(profile.pages.begin(), profile.pages.end(),
				key.pageIndex);
			if (page == profile.pages.end() || *page != key.pageIndex)
				profile.pages.insert(page, key.pageIndex);

			// Track each page's own IDs so re-indexing stays proportional to that page
			// and keeps the direct cacheId -> first page lookup synchronized.
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
			pageResidents.reserve(resource.glyphs.size());
			for (const auto& placement : resource.glyphs)
			{
				const auto resident = profile.residentPages.find(placement.cacheId);
				if (resident == profile.residentPages.end())
				{
					profile.residentPages.emplace(placement.cacheId, key.pageIndex);
				}
				else
				{
					if (resident->second != key.pageIndex)
						profile.duplicateResidents.insert(placement.cacheId);
					if (key.pageIndex < resident->second)
					{
						// Profile pages are consumed in ascending order. Preserve the
						// original placement scan's first-page-wins behavior even when a
						// restored or legacy profile happens to contain a duplicate ID.
						resident->second = key.pageIndex;
					}
				}
				pageResidents.push_back(placement.cacheId);
			}
			RefreshAtlasProfileCpuMemory(profile);
		}

		void UnindexAtlasPage(AtlasState& state, const AtlasCacheKey& key)
		{
			const AtlasProfileKey profileKey = MakeAtlasProfileKey(key);
			InvalidateCompleteAtlasProfileLocked(state, profileKey);
			auto found = state.atlasProfiles.find(profileKey);
			if (found == state.atlasProfiles.end())
				return;
			AtlasProfileIndex& profile = found->second;
			EnsureAtlasProfileIndexLocked(state, key, profile);
			const auto page = std::lower_bound(profile.pages.begin(), profile.pages.end(),
				key.pageIndex);
			if (page != profile.pages.end() && *page == key.pageIndex)
				profile.pages.erase(page);
			const auto pageResidents = profile.pageResidents.find(key.pageIndex);
			if (pageResidents != profile.pageResidents.end())
			{
				for (UInt64 cacheId : pageResidents->second)
				{
					auto resident = profile.residentPages.find(cacheId);
					if (resident != profile.residentPages.end()
						&& resident->second == key.pageIndex)
						profile.residentPages.erase(resident);

					if (profile.duplicateResidents.find(cacheId)
						== profile.duplicateResidents.end())
					{
						continue;
					}

					// Duplicate IDs are not produced by the current packer, but old
					// snapshots may contain them. Only those IDs pay for a successor
					// search when a page is retired; the common unique-ID path remains
					// linear in this page's resident count.
					UInt16 firstResidentPage = 0;
					UInt32 residentCount = 0;
					for (UInt16 candidatePageIndex : profile.pages)
					{
						AtlasCacheKey candidateKey = key;
						candidateKey.pageIndex = candidatePageIndex;
						const auto candidate = state.atlasCache.find(candidateKey);
						if (candidate == state.atlasCache.end()
							|| !candidate->second.resource
							|| !FindAtlasGlyph(*candidate->second.resource, cacheId))
						{
							continue;
						}
						if (residentCount++ == 0)
							firstResidentPage = candidatePageIndex;
						if (residentCount == 2)
							break;
					}
					if (residentCount != 0)
						profile.residentPages[cacheId] = firstResidentPage;
					else
						profile.residentPages.erase(cacheId);
					if (residentCount < 2)
						profile.duplicateResidents.erase(cacheId);
				}
				profile.pageResidents.erase(pageResidents);
			}
			if (profile.pages.empty())
				state.atlasProfiles.erase(found);
			else
				RefreshAtlasProfileCpuMemory(profile);
		}

		void TrimAtlasCacheToTarget(AtlasState& state, size_t targetBytes)
		{
			while (state.atlasCacheBytes > targetBytes && !state.atlasLru.empty())
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
			PruneRetiredAtlasGenerations();
		}

		void TrimAtlasCache(AtlasState& state)
		{
			if (IsGpuAtlasCacheUnlimited())
			{
				PruneRetiredAtlasGenerations();
				return;
			}
			TrimAtlasCacheToTarget(state, GetAtlasCacheLimit());
		}

		void TrimAtlasCacheForIncomingBytes(AtlasState& state, size_t incomingBytes)
		{
			if (IsGpuAtlasCacheUnlimited())
			{
				PruneRetiredAtlasGenerations();
				return;
			}
			const size_t limit = GetAtlasCacheLimit();
			TrimAtlasCacheToTarget(state,
				incomingBytes < limit ? limit - incomingBytes : 0);
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

		void TrimTextArtifactCache(AtlasState& state)
		{
			const size_t preferred = static_cast<size_t>(
				g_uiFreeTypeFontMemoryCacheMB) * 1024u * 1024u / 12u;
			const size_t limit = GetCpuMemoryCategoryHeadroom(
				CpuMemoryCategory::TextArtifact, preferred);
			while ((GetCpuMemoryUsage(CpuMemoryCategory::TextArtifact) > limit
					|| IsCpuMemoryBudgetExceeded())
				&& !state.textArtifactLru.empty())
			{
				const TextArtifactKey key = state.textArtifactLru.back();
				auto existing = state.textArtifactCache.find(key);
				if (existing != state.textArtifactCache.end())
				{
					state.textArtifactCacheBytes -= existing->second.bytes;
					state.textArtifactCache.erase(existing);
				}
				state.textArtifactLru.pop_back();
			}
		}

	void TrimAtlasCpuCachesForTotalBudget()
	{
		AtlasState& state = State();
		std::lock_guard<std::mutex> lock(state.textArtifactMutex);
		TrimTextArtifactCache(state);
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
			if (maskCombination & (1u
				<< static_cast<UInt8>(GlyphMaskType::DistanceField)))
			{
				const DistanceFieldMethod method =
					GetConfiguredDistanceFieldMethod();
				const UInt32 revision = DistanceFieldGeneratorRevision(method);
				add(&method, sizeof(method));
				add(&revision, sizeof(revision));
			}
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
				add(&effect.x, sizeof(effect.x));
				add(&effect.y, sizeof(effect.y));
				add(&effect.colorMode, sizeof(effect.colorMode));
				add(&effect.color, sizeof(effect.color));
			};
			add(&kCpuEffectCoverageVersion,
				sizeof(kCpuEffectCoverageVersion));
			add(&config.fontColor.configured,
				sizeof(config.fontColor.configured));
			add(&config.fontColor.color, sizeof(config.fontColor.color));
			addEffect(config.glow);
			addEffect(config.outline);
			addEffect(config.shadow);
			add(&config.shadow.includeGlow,
				sizeof(config.shadow.includeGlow));
			add(&config.shadow.includeOutline,
				sizeof(config.shadow.includeOutline));
			if (HardShadowIncludesGlow(config))
				add(&config.glow.color.a, sizeof(config.glow.color.a));
			if (HardShadowIncludesOutline(config))
				add(&config.outline.color.a, sizeof(config.outline.color.a));
			return hash;
		}

		UInt64 BuildAtlasContentHash(const FontConfig& config,
			VectorFontByteClass byteClass,
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
				else if (bitmap->maskType == GlyphMaskType::Composite)
					outlineStroke = bitmap->strokeWidth26Dot6;
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
			const FontConfig& rasterConfig =
				renderMode == AtlasRenderMode::ShaderEffects
				? GetMtsdfAtlasConfig(config, byteClass) : config;
			return BuildAtlasContentHash(
				rasterConfig.maskGenerationRoleHashes[
					static_cast<size_t>(byteClass)],
				combination, sdfSpread, outlineStroke, glowStroke,
				renderMode == AtlasRenderMode::CpuEffects
					? BuildCpuCoverageHash(config, rasterScale) : 0,
				bakedColorHash);
		}

		bool UsesLevelZeroOnly(
			const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
			AtlasPixelMode pixelMode, AtlasRenderMode renderMode)
		{
			// Native true-SDF A8 and aggressive BGRA composite profiles deliberately
			// sample level zero. Retaining or generating mips would only add upload,
			// disk and VRAM cost.
			if (pixelMode == AtlasPixelMode::A8
				&& renderMode == AtlasRenderMode::CpuEffects)
			{
				return true;
			}
			if (pixelMode == AtlasPixelMode::Argb32
				&& renderMode == AtlasRenderMode::CpuEffects)
			{
				bool foundComposite = false;
				for (const auto& bitmap : bitmaps)
				{
					if (!bitmap)
						continue;
					if (bitmap->maskType != GlyphMaskType::Composite)
						return false;
					foundComposite = true;
				}
				if (foundComposite)
					return true;
			}
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
			VectorFontByteClass byteClass, float rasterScale, bool shaderEffects)
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
				MtsdfSharedRasterProfile profile;
				if (ResolveMtsdfSharedRasterProfile(config, byteClass,
					rasterScale, true, profile))
				{
					include(GlyphMaskType::DistanceField);
					sdfSpread = static_cast<UInt8>(profile.sdfSpread);
				}
			}
			else if (g_bEnableFreeTypeFontAggressivePerformanceMode)
			{
				include(GlyphMaskType::Composite);
				outlineStroke = ResolveCpuEffectMaskIdentity(
					config, GlyphMaskType::Composite, rasterScale);
			}
			else
			{
				include(GlyphMaskType::Fill);
				if (config.glow.enabled)
				{
					include(GlyphMaskType::Glow);
					glowStroke = ResolveCpuEffectMaskIdentity(
						config, GlyphMaskType::Glow, rasterScale);
				}
				if (config.outline.enabled)
				{
					include(GlyphMaskType::Outline);
					outlineStroke = ResolveCpuEffectMaskIdentity(
						config, GlyphMaskType::Outline, rasterScale);
				}
				if (config.shadow.enabled)
					include(GlyphMaskType::Shadow);
			}
			const FontConfig& rasterConfig = shaderEffects
				? GetMtsdfAtlasConfig(config, byteClass) : config;
			return BuildAtlasContentHash(
				rasterConfig.maskGenerationRoleHashes[
					static_cast<size_t>(byteClass)],
				combination, sdfSpread, outlineStroke, glowStroke,
				shaderEffects ? 0 : BuildCpuCoverageHash(config, rasterScale),
				1469598103934665603ull);
		}

	bool ResolvePrewarmAtlasKey(const FontConfig& config,
		VectorFontByteClass byteClass, float rasterScale, AtlasCacheKey& key)
	{
		// The complete-code-page transaction follows the same route contract as
		// demand rendering. Aggressive mode persists one CPU-precomposed BGRA
		// rectangle per glyph; the ordinary CPU-effects route keeps its A8 layers.
		const FontAtlasRoute route = ResolveFontAtlasRoute(
			IsA8RendererAvailable(),
			g_bEnableFreeTypeFontAggressivePerformanceMode);
		if (route == FontAtlasRoute::ArgbFallback)
			return false;

		const bool shaderEffects =
			route == FontAtlasRoute::ShaderDistanceField;
		if (shaderEffects)
		{
			EffectQuality resolved = config.effectQuality;
			UInt32 sdfSpread = 0;
			if (!ResolveA8EffectQuality(config.effectQuality, resolved)
				|| !ResolveSdfSpread(config, rasterScale, sdfSpread))
			{
				return false;
			}
		}

		const bool aggressiveComposite =
			route == FontAtlasRoute::ShaderA8Coverage;
		key = {
			BuildPrewarmAtlasContentHash(config, byteClass, rasterScale,
				shaderEffects),
			config.fontId,
			static_cast<UInt32>(std::lround(rasterScale * 1000.0f)),
			shaderEffects
				? GetConfiguredDistanceFieldAtlasPixelMode()
				: aggressiveComposite
					? AtlasPixelMode::Argb32 : AtlasPixelMode::A8,
			shaderEffects
				? AtlasRenderMode::ShaderEffects
				: AtlasRenderMode::CpuEffects,
			aggressiveComposite ? kArgbAtlasPadding
				: kDistanceFieldAtlasPadding,
			true,
			byteClass
		};
		return true;
	}

	bool IsPrewarmAtlasAlias(const FontConfig& config,
		VectorFontByteClass byteClass)
	{
		return ResolveFontAtlasRoute(IsA8RendererAvailable(),
			g_bEnableFreeTypeFontAggressivePerformanceMode)
				== FontAtlasRoute::ShaderDistanceField
			&& IsMtsdfAtlasAlias(config, byteClass);
	}

	RuntimeFont* GetPrewarmAtlasRuntime(RuntimeFont& runtime,
		const AtlasCacheKey& key)
	{
		return key.renderMode == AtlasRenderMode::ShaderEffects
			? GetMtsdfAtlasRuntime(runtime, key.byteClass) : &runtime;
	}

		void GetAtlasBackedGlyphBitmaps(RuntimeFont& runtime,
			const std::vector<GlyphBitmapRequest>& requests, float rasterScale,
			AtlasPixelMode pixelMode, AtlasRenderMode renderMode, UInt32 padding,
			std::vector<std::shared_ptr<const GlyphBitmap>>& results)
		{
			results.assign(requests.size(), nullptr);
			if (requests.empty())
				return;

			std::vector<UInt64> cacheIds;
			ResolveGlyphBitmapCacheIds(runtime, requests, rasterScale, cacheIds);
			const FontConfig& config = GetRuntimeConfig(runtime);
			std::array<UInt8, 2> combinations = {};
			std::array<UInt8, 2> sdfSpreads = {};
			SInt32 outlineStroke = 0;
			SInt32 glowStroke = 0;
			bool found = false;
			bool levelZeroOnly = true;
			for (size_t index = 0; index < requests.size(); ++index)
			{
				if (!cacheIds[index])
					continue;
				found = true;
				const GlyphMaskType type = requests[index].maskType;
				const size_t roleIndex = requests[index].glyph
					? static_cast<size_t>(requests[index].glyph->byteClass) : 0;
				combinations[roleIndex] |= static_cast<UInt8>(
					1u << static_cast<UInt8>(type));
				levelZeroOnly = levelZeroOnly
					&& (type == GlyphMaskType::DistanceField
						|| type == GlyphMaskType::Composite);
				if (type == GlyphMaskType::DistanceField)
				{
					MtsdfSharedRasterProfile profile;
					if (requests[index].glyph
						&& ResolveMtsdfSharedRasterProfile(config,
							requests[index].glyph->byteClass, rasterScale,
							true, profile))
					{
						sdfSpreads[roleIndex] = std::max(
							sdfSpreads[roleIndex],
							static_cast<UInt8>(profile.sdfSpread));
					}
				}
				else if (type == GlyphMaskType::Outline)
					outlineStroke = ResolveCpuEffectMaskIdentity(
						config, GlyphMaskType::Outline, rasterScale);
				else if (type == GlyphMaskType::Glow)
					glowStroke = ResolveCpuEffectMaskIdentity(
						config, GlyphMaskType::Glow, rasterScale);
				else if (type == GlyphMaskType::Composite)
					outlineStroke = ResolveCpuEffectMaskIdentity(
						config, GlyphMaskType::Composite, rasterScale);
			}

			UInt64 residentHits = 0;
			if (found)
			{
				std::array<AtlasCacheKey, 2> baseKeys;
				for (size_t roleIndex = 0; roleIndex < baseKeys.size(); ++roleIndex)
				{
					const VectorFontByteClass byteClass =
						static_cast<VectorFontByteClass>(roleIndex);
					const FontConfig& rasterConfig =
						renderMode == AtlasRenderMode::ShaderEffects
						? GetMtsdfAtlasConfig(config, byteClass) : config;
					baseKeys[roleIndex] = {
						BuildAtlasContentHash(
							rasterConfig.maskGenerationRoleHashes[roleIndex],
							combinations[roleIndex], sdfSpreads[roleIndex],
							outlineStroke, glowStroke,
							renderMode == AtlasRenderMode::CpuEffects
								? BuildCpuCoverageHash(config, rasterScale) : 0,
							1469598103934665603ull),
						config.fontId,
						static_cast<UInt32>(std::lround(rasterScale * 1000.0f)),
						pixelMode,
						renderMode,
						padding,
						levelZeroOnly,
						byteClass
					};
				}
				AtlasState& state = State();
				std::lock_guard<std::mutex> lock(state.atlasMutex);
				for (size_t index = 0; index < cacheIds.size(); ++index)
				{
					if (!cacheIds[index] || !requests[index].glyph)
						continue;
					const size_t roleIndex = static_cast<size_t>(
						requests[index].glyph->byteClass);
					const AtlasCacheKey& baseKey = baseKeys[roleIndex];
					const auto profile = state.atlasProfiles.find(
						MakeAtlasProfileKey(baseKey));
					if (profile == state.atlasProfiles.end())
						continue;
					if (!EnsureAtlasProfileIndexLocked(
						state, baseKey, profile->second))
					{
						continue;
					}
					const auto resident = profile->second.residentPages.find(cacheIds[index]);
					if (resident == profile->second.residentPages.end())
						continue;
					AtlasCacheKey pageKey = baseKey;
					pageKey.pageIndex = resident->second;
					const auto page = state.atlasCache.find(pageKey);
					if (page == state.atlasCache.end() || !page->second.resource)
						continue;
					std::shared_ptr<const GlyphBitmap> bitmap =
						GetOrCreateAtlasGlyphBitmap(*page->second.resource, cacheIds[index]);
					if (!bitmap)
						continue;
					results[index] = std::move(bitmap);
					++residentHits;
				}
			}

			std::vector<GlyphBitmapRequest> misses;
			std::vector<size_t> missIndices;
			misses.reserve(requests.size() - static_cast<size_t>(residentHits));
			missIndices.reserve(misses.capacity());
			for (size_t index = 0; index < requests.size(); ++index)
			{
				if (results[index])
					continue;
				misses.push_back(requests[index]);
				missIndices.push_back(index);
			}
			if (residentHits)
				RecordFreeTypePerf(FreeTypePerfCounter::GpuResidentGlyphHit, residentHits);
			if (!misses.empty())
			{
				RecordFreeTypePerf(FreeTypePerfCounter::GpuResidentGlyphMiss,
					static_cast<UInt64>(misses.size()));
				std::vector<std::shared_ptr<const GlyphBitmap>> fallback;
				GetGlyphBitmaps(runtime, misses, rasterScale, fallback);
				for (size_t index = 0; index < fallback.size(); ++index)
					results[missIndices[index]] = std::move(fallback[index]);
			}
		}

		std::vector<std::shared_ptr<AtlasResource>> GetAtlasResources(
			const FontConfig& config, VectorFontByteClass byteClass, float rasterScale,
			const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
			AtlasPixelMode pixelMode, AtlasRenderMode renderMode,
			UInt32 padding, std::vector<UInt16>* outBitmapPageOrdinals)
		{
			constexpr UInt16 kUnavailablePage = std::numeric_limits<UInt16>::max();
			if (outBitmapPageOrdinals)
				outBitmapPageOrdinals->assign(bitmaps.size(), kUnavailablePage);
			const auto assignPageOrdinal = [&](const AtlasResource& resource, UInt16 ordinal)
			{
				if (!outBitmapPageOrdinals)
					return;
				for (size_t bitmapIndex = 0; bitmapIndex < bitmaps.size(); ++bitmapIndex)
				{
					const auto& bitmap = bitmaps[bitmapIndex];
					if (!bitmap || (*outBitmapPageOrdinals)[bitmapIndex] != kUnavailablePage)
						continue;
					if (FindAtlasGlyph(resource, bitmap->cacheId))
						(*outBitmapPageOrdinals)[bitmapIndex] = ordinal;
				}
			};

			AtlasState& state = State();
			const AtlasCacheKey baseKey = {
				BuildAtlasContentHash(config, byteClass, bitmaps, rasterScale, renderMode),
				config.fontId,
				static_cast<UInt32>(std::lround(rasterScale * 1000.0f)),
				pixelMode,
				renderMode,
				padding,
				UsesLevelZeroOnly(bitmaps, pixelMode, renderMode),
				byteClass
			};

			std::lock_guard<std::mutex> lock(state.atlasMutex);
			std::vector<std::pair<AtlasCacheKey, AtlasCacheEntry*>> entries;
			const AtlasProfileKey profileKey = MakeAtlasProfileKey(baseKey);
			auto profile = state.atlasProfiles.find(profileKey);
			if (profile != state.atlasProfiles.end())
			{
				EnsureAtlasProfileIndexLocked(
					state, baseKey, profile->second);
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
			thread_local std::unordered_map<UInt16, UInt16> availablePageOrdinals;
			if (outBitmapPageOrdinals)
			{
				availablePageOrdinals.clear();
				availablePageOrdinals.reserve(entries.size() + 1);
			}
			for (size_t ordinal = 0; ordinal < entries.size(); ++ordinal)
			{
				auto& entry = entries[ordinal];
				TouchAtlasEntry(state, *entry.second, entry.first);
				pages.push_back(entry.second->resource);
				if (outBitmapPageOrdinals)
				{
					availablePageOrdinals.emplace(entry.first.pageIndex,
						static_cast<UInt16>(ordinal));
				}
			}
			if (!pages.empty())
				RecordFreeTypePerf(FreeTypePerfCounter::AtlasHit);

			std::vector<std::shared_ptr<const GlyphBitmap>> missing;
			missing.reserve(bitmaps.size());
			for (size_t bitmapIndex = 0; bitmapIndex < bitmaps.size(); ++bitmapIndex)
			{
				const auto& bitmap = bitmaps[bitmapIndex];
				if (!bitmap)
					continue;
				UInt16 residentPage = 0;
				bool isResident = false;
				if (profile != state.atlasProfiles.end())
				{
					const auto resident = profile->second.residentPages.find(bitmap->cacheId);
					if (resident != profile->second.residentPages.end())
					{
						residentPage = resident->second;
						isResident = true;
					}
				}
				if (!isResident)
				{
					missing.push_back(bitmap);
					continue;
				}
				if (outBitmapPageOrdinals)
				{
					const auto ordinal = availablePageOrdinals.find(residentPage);
					if (ordinal != availablePageOrdinals.end())
						(*outBitmapPageOrdinals)[bitmapIndex] = ordinal->second;
				}
			}
			if (missing.empty() && !pages.empty())
				return pages;

			auto updateExistingPage = [&](AtlasCacheEntry& entry,
				const AtlasCacheKey& pageKey, const std::shared_ptr<AtlasResource>& resource)
			{
				IndexAtlasPage(state, pageKey, *resource);
				assignPageOrdinal(*resource, static_cast<UInt16>(pages.size() - 1));
				const size_t bytes = GetAtlasStorageBytes(resource->width,
					resource->height, resource->pixelMode, resource->mipLevels);
				if (bytes != entry.bytes)
				{
					state.atlasCacheBytes -= entry.bytes;
					entry.bytes = bytes;
					state.atlasCacheBytes += bytes;
				}
			};

			std::vector<std::shared_ptr<const GlyphBitmap>> accepted;
			std::vector<std::shared_ptr<const GlyphBitmap>> remaining;
			if (!pages.empty())
			{
				AtlasCacheEntry& entry = *entries.back().second;
				if (AddBitmapsToAtlas(*pages.back(), missing))
				{
					updateExistingPage(entry, entries.back().first, pages.back());
					missing.clear();
				}
				else
				{
					PartitionBitmapsForAtlasPage(*pages.back(), missing,
						accepted, remaining);
					if (!accepted.empty() && accepted.size() < missing.size()
						&& AddBitmapsToAtlas(*pages.back(), accepted))
					{
						updateExistingPage(entry, entries.back().first, pages.back());
						missing.swap(remaining);
					}
				}
			}

			UInt32 nextPageIndex = entries.empty() ? 0u
				: static_cast<UInt32>(entries.back().first.pageIndex) + 1u;
			while (!missing.empty())
			{
				if (nextPageIndex >= kMaximumAtlasSnapshotPages)
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
				PartitionBitmapsForAtlasPage(*resource, missing, accepted, remaining);
				if (accepted.empty() || !AddBitmapsToAtlas(*resource, accepted))
					return {};
				const size_t bytes = GetAtlasStorageBytes(resource->width,
					resource->height, resource->pixelMode, resource->mipLevels);
				AtlasCacheKey pageKey = baseKey;
				pageKey.pageIndex = static_cast<UInt16>(nextPageIndex++);
				state.atlasLru.push_front(pageKey);
				const auto inserted = state.atlasCache.emplace(pageKey,
					AtlasCacheEntry{ resource, bytes, state.atlasLru.begin() });
				if (!inserted.second)
				{
					state.atlasLru.pop_front();
					return {};
				}
				inserted.first->second.cpuMemory.Reset(CpuMemoryCategory::AtlasMetadata,
					sizeof(AtlasCacheEntry) + 2u * sizeof(AtlasCacheKey)
						+ 4u * sizeof(void*));
				IndexAtlasPage(state, pageKey, *resource);
				state.atlasCacheBytes += bytes;
				pages.push_back(resource);
				assignPageOrdinal(*resource, static_cast<UInt16>(pages.size() - 1));
				if (g_bEnableFreeTypeFontRenderingLog)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: atlas page created font=%u role=%s page=%u size=%ux%u pixelMode=%u renderMode=%u",
						pageKey.fontId,
						pageKey.byteClass == VectorFontByteClass::DoubleByte
							? "doubleByte" : "singleByte",
						pageKey.pageIndex, resource->width, resource->height,
						static_cast<UInt32>(resource->pixelMode),
						static_cast<UInt32>(resource->renderMode));
				}
				missing.swap(remaining);
			}
			TrimAtlasCache(state);
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
			resource->levelZeroOnly =
				UsesLevelZeroOnly(bitmaps, pixelMode, renderMode);
			resource->padding = padding;
			resource->transient = true;
			std::unordered_map<UInt64, AtlasRect> placements;
			if (!PackAtlas(bitmaps, resource->width, resource->height,
				placements, padding))
			{
				return nullptr;
			}
			resource->mipLevels = GetAtlasMipLevelCount(
				resource->width, resource->height, resource->levelZeroOnly);
			for (const auto& bitmap : bitmaps)
			{
				if (bitmap)
					resource->glyphs.push_back({ bitmap->cacheId,
						placements.at(bitmap->cacheId), bitmap, kNoSnapshotPlacement });
			}
			SortAtlasGlyphs(*resource);
			if (resource->backend == AtlasBackend::DefaultPool
				&& CreateDefaultPoolAtlas(*resource, pixelMode))
			{
				return resource;
			}
			if (g_bEnableFreeTypeDefaultPoolAtlas)
				return nullptr;
			resource->backend = AtlasBackend::Managed;
			if (resource->pixelMode == AtlasPixelMode::A8)
				resource->pixelMode = AtlasPixelMode::Argb32;
			resource->mipLevels = GetAtlasMipLevelCount(
				resource->width, resource->height, resource->levelZeroOnly);
			resource->pixels.assign(static_cast<size_t>(resource->width)
				* resource->height * AtlasBytesPerPixel(resource->pixelMode), 0u);
			for (const auto& bitmap : bitmaps)
			{
				if (bitmap)
				{
					const AtlasGlyphRecord* glyph = FindAtlasGlyph(*resource, bitmap->cacheId);
					if (!glyph)
						return nullptr;
					CopyBitmapToAtlas(*resource, *bitmap, glyph->rect);
				}
			}
			return RecreateManagedAtlasProperty(*resource) ? resource : nullptr;
		}
}

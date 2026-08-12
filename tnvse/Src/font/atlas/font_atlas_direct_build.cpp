#include "font_atlas_internal.h"

#include "encoding.h"
#include "globals.h"
#include "load_config.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "font_atlas_direct_internal.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_atlas_direct {}
	using namespace implementation::font_atlas_direct;

namespace implementation::font_atlas_direct
{
		bool IsSpaceCodePoint(UInt32 codePoint)
		{
			return codePoint == 0x20 || codePoint == 0xA0
				|| codePoint == 0x1680
				|| (codePoint >= 0x2000 && codePoint <= 0x200A)
				|| codePoint == 0x202F || codePoint == 0x205F
				|| codePoint == 0x3000;
		}

		bool ConvertCompositeTableToVanillaLetters(
			DirectAtlasGlyphTable& table,
			const std::vector<std::shared_ptr<AtlasResource>>& pages,
			float rasterScale)
		{
			if (table.recordKind
					!= DirectCachedLetterKind::VanillaFontLetter
				|| table.glyphs.empty()
				|| !std::isfinite(rasterScale)
				|| rasterScale <= 0.0f)
			{
				return false;
			}
			table.vanillaGlyphs.assign(table.glyphs.size(), {});
			for (FontLetter& letter : table.vanillaGlyphs)
				letter.iTextureIndex = -1;
			for (size_t slot = 0; slot < table.glyphs.size(); ++slot)
			{
				const DirectCachedLetter& source = table.glyphs[slot];
				if (!(source.flags & kDirectCachedLetterValid))
					continue;
				FontLetter& target = table.vanillaGlyphs[slot];
				if (source.flags & kDirectCachedLetterKnownEmpty)
				{
					target.iTextureIndex = -2;
					target.fWidth = source.width;
					target.fHeight = source.height;
					target.fLeadingEdge = source.leadingEdge;
					target.fSpacing = source.spacing;
					target.fTopEdge = source.topEdge;
					NormalizeKnownEmptyAdvance(target);
					continue;
				}
				const DirectAtlasGlyphLayer* layer =
					FindDirectLayer(source, GlyphMaskType::Composite);
				if (!layer || layer->pageSlot >= pages.size())
					return false;
				const auto& page = pages[layer->pageSlot];
				if (!page || !page->compactSnapshot
					|| layer->snapshotPlacementIndex
						>= page->compactSnapshot->placements.size())
				{
					return false;
				}
				const AtlasSnapshotPlacement& placement =
					page->compactSnapshot->placements[
						layer->snapshotPlacementIndex];
				if (placement.maskType != static_cast<UInt8>(
						GlyphMaskType::Composite)
					|| !IsValidAtlasSnapshotGlyphPlacement(placement,
						page->width, page->height,
						page->compactSnapshot->sourceHeader.pageIndex))
				{
					return false;
				}
				const float layoutAdvance = source.leadingEdge
					+ source.width
					+ (source.width > 0.0f ? source.spacing : 0.0f);
				target.iTextureIndex = layer->pageSlot;
				target.fWidth =
					static_cast<float>(placement.rect.width)
						/ rasterScale;
				target.fHeight =
					static_cast<float>(placement.rect.height)
						/ rasterScale;
				target.fLeadingEdge =
					static_cast<float>(placement.left) / rasterScale;
				target.fSpacing = layoutAdvance
					- target.fLeadingEdge - target.fWidth;
				target.fTopEdge =
					static_cast<float>(placement.top) / rasterScale;
				target.pMapping[0] = {
					placement.glyphPlacement.u0,
					placement.glyphPlacement.v0 };
				target.pMapping[1] = {
					placement.glyphPlacement.u1,
					placement.glyphPlacement.v0 };
				target.pMapping[2] = {
					placement.glyphPlacement.u1,
					placement.glyphPlacement.v1 };
				target.pMapping[3] = {
					placement.glyphPlacement.u0,
					placement.glyphPlacement.v1 };
			}
			table.glyphs.clear();
			table.glyphs.shrink_to_fit();
			return true;
		}

		size_t GetDirectGlyphSlotCount(VectorFontByteClass byteClass)
		{
			return byteClass == VectorFontByteClass::DoubleByte
				? kDoubleByteGlyphSlots : 256u;
		}

		bool ResolveDirectGlyphSlot(VectorFontByteClass byteClass,
			UInt32 encodedCode, size_t& slot)
		{
			if (byteClass == VectorFontByteClass::SingleByte)
			{
				if (encodedCode > 0xFFu)
					return false;
				slot = encodedCode;
				return true;
			}
			if (encodedCode & 0xFFFF0000u)
				return false;
			const UInt32 lead = (encodedCode >> 8) & 0xFFu;
			const UInt32 trail = encodedCode & 0xFFu;
			if (lead < kFirstLeadByte || lead > kLastLeadByte
				|| trail < kFirstTrailByte || trail > kLastTrailByte)
			{
				return false;
			}
			slot = static_cast<size_t>(lead - kFirstLeadByte)
				* kGlyphsPerDoubleByteRow + (trail - kFirstTrailByte);
			return true;
		}

		UInt32 EncodeDirectGlyphSlot(VectorFontByteClass byteClass, size_t slot)
		{
			if (byteClass == VectorFontByteClass::SingleByte)
				return static_cast<UInt32>(slot);
			const UInt32 lead = kFirstLeadByte
				+ static_cast<UInt32>(slot / kGlyphsPerDoubleByteRow);
			const UInt32 trail = kFirstTrailByte
				+ static_cast<UInt32>(slot % kGlyphsPerDoubleByteRow);
			return (lead << 8) | trail;
		}

		std::vector<GlyphMaskType> GetDirectProfileMasks(
			const FontConfig& config, const AtlasCacheKey& key)
		{
			std::vector<GlyphMaskType> masks;
			if (key.renderMode == AtlasRenderMode::ShaderEffects)
			{
				masks.push_back(GlyphMaskType::DistanceField);
				return masks;
			}
			if (UsesBakedEffectRoute())
			{
				masks.push_back(GlyphMaskType::Composite);
				return masks;
			}
			masks.push_back(GlyphMaskType::Fill);
			if (config.outline.enabled)
				masks.push_back(GlyphMaskType::Outline);
			if (config.glow.enabled)
				masks.push_back(GlyphMaskType::Glow);
			if (config.shadow.enabled)
				masks.push_back(GlyphMaskType::Shadow);
			return masks;
		}

		bool ResolveDirectLayerLocked(AtlasState& state,
			const AtlasCacheKey& baseKey, AtlasProfileIndex& profile,
			const std::vector<UInt16>& pageIndices, UInt64 cacheId,
			DirectAtlasGlyphLayer& layer)
		{
			if (!EnsureAtlasProfileIndexLocked(
				state, baseKey, profile))
			{
				return false;
			}
			const auto resident = profile.residentPages.find(cacheId);
			if (resident == profile.residentPages.end())
				return false;
			const auto pageSlot = std::lower_bound(pageIndices.begin(),
				pageIndices.end(), resident->second);
			if (pageSlot == pageIndices.end() || *pageSlot != resident->second)
				return false;

			AtlasCacheKey pageKey = baseKey;
			pageKey.pageIndex = resident->second;
			const auto page = state.atlasCache.find(pageKey);
			if (page == state.atlasCache.end() || !page->second.resource)
				return false;
			AtlasGlyphRecord* glyph =
				FindAtlasGlyph(*page->second.resource, cacheId);
			if (!glyph || !page->second.resource->compactSnapshot
				|| glyph->snapshotPlacementIndex == kNoSnapshotPlacement
				|| glyph->snapshotPlacementIndex
					>= page->second.resource->compactSnapshot->placements.size())
				return false;
			const AtlasSnapshotPlacement& snapshot =
				page->second.resource->compactSnapshot->placements[
					glyph->snapshotPlacementIndex];
			if (snapshot.cacheId != cacheId
				|| std::memcmp(&snapshot.rect, &glyph->rect,
					sizeof(snapshot.rect)) != 0)
			{
				return false;
			}
			if (!IsValidAtlasSnapshotGlyphPlacement(snapshot,
				page->second.resource->width,
				page->second.resource->height, resident->second))
			{
				return false;
			}

			layer.pageSlot = static_cast<UInt16>(
				pageSlot - pageIndices.begin());
			layer.maskType = snapshot.maskType;
			layer.reserved = 0;
			layer.snapshotPlacementIndex = glyph->snapshotPlacementIndex;
			return true;
		}

		bool ResolveDirectGlyphSource(
			const DirectAtlasGlyphTable& table,
			const VectorEncodedGlyph& glyph, GlyphMaskType maskType,
			PendingQuad::GlyphSource& result)
		{
			size_t glyphSlot = 0;
			if (!ResolveDirectGlyphSlot(glyph.byteClass,
				glyph.encodedCode, glyphSlot)
				|| glyphSlot >= table.glyphs.size())
				return false;
			const DirectCachedLetter& letter = table.glyphs[glyphSlot];
			if (!(letter.flags & kDirectCachedLetterValid)
				|| (letter.flags & kDirectCachedLetterKnownEmpty)
				|| letter.encodedCode != glyph.encodedCode
				|| letter.byteClass != static_cast<UInt8>(glyph.byteClass))
				return false;
			const DirectAtlasGlyphLayer* direct =
				FindDirectLayer(letter, maskType);
			if (!direct || direct->pageSlot >= table.pages.size())
				return false;
			std::shared_ptr<AtlasResource> page =
				table.pages[direct->pageSlot].lock();
			if (!page || !page->compactSnapshot
				|| direct->snapshotPlacementIndex
					>= page->compactSnapshot->placements.size())
				return false;
			const AtlasSnapshotPlacement& snapshot =
				page->compactSnapshot->placements[
					direct->snapshotPlacementIndex];
			if (snapshot.maskType != direct->maskType
				|| !page->pageContentHash
				|| page->compactSnapshot->sourceHeader.pageContentHash
					!= page->pageContentHash
				|| !IsValidAtlasSnapshotGlyphPlacement(snapshot,
					page->width, page->height,
					page->compactSnapshot->sourceHeader.pageIndex))
				return false;

			result = {};
			result.atlas = std::move(page);
			result.directCacheId = snapshot.cacheId;
			result.directWidth = static_cast<SInt32>(snapshot.rect.width);
			result.directHeight = static_cast<SInt32>(snapshot.rect.height);
			result.directLeft = snapshot.left;
			result.directTop = snapshot.top;
			result.directMaskType = snapshot.maskType;
			result.directSdfSpread = snapshot.sdfSpread;
			result.placement.atlasIdentity =
				reinterpret_cast<uintptr_t>(result.atlas.get());
			result.placement.atlasGeneration = result.atlas->generation;
			result.placement.atlasWidth = result.atlas->width;
			result.placement.atlasHeight = result.atlas->height;
			result.placement.pageIndex = direct->pageSlot;
			result.placement.inverseWidth =
				1.0f / static_cast<float>(result.atlas->width);
			result.placement.inverseHeight =
				1.0f / static_cast<float>(result.atlas->height);
			result.placement.u0 = snapshot.glyphPlacement.u0;
			result.placement.v0 = snapshot.glyphPlacement.v0;
			result.placement.u1 = snapshot.glyphPlacement.u1;
			result.placement.v1 = snapshot.glyphPlacement.v1;
			return true;
		}

		bool BuildDirectGlyphAtlasTableRole(RuntimeFont& runtime,
			VectorFontByteClass byteClass, float rasterScale)
		{
			const FontConfig& config = GetRuntimeConfig(runtime);
			AtlasCacheKey baseKey;
			if (!ResolvePrewarmAtlasKey(config, byteClass, rasterScale, baseKey))
				return false;
			const AtlasProfileKey profileKey = MakeAtlasProfileKey(baseKey);
			const UInt32 directCodePage = GetFreeTypeTextCodePage();
			const UInt64 directLayoutIdentity =
				GetRuntimeDirectRoleLayoutIdentity(runtime, byteClass);
			const UInt64 directEffectIdentity =
				GetRuntimeMaskContentHash(runtime, byteClass);

			std::vector<UInt16> pageIndices;
			std::vector<std::shared_ptr<AtlasResource>> pages;
			{
				AtlasState& state = State();
				std::lock_guard<std::mutex> lock(state.atlasMutex);
				const auto profile = state.atlasProfiles.find(profileKey);
				if (profile == state.atlasProfiles.end()
					|| state.completeAtlasProfiles.find(profileKey)
						== state.completeAtlasProfiles.end())
				{
					return false;
				}
				const std::shared_ptr<const DirectAtlasGlyphTable>& existing =
					profile->second.directGlyphs;
				if (existing && existing->validity
					&& existing->validity->load(
						std::memory_order_acquire)
					&& existing->byteClass == byteClass
					&& existing->codePage == directCodePage
					&& existing->layoutIdentity
						== directLayoutIdentity
					&& existing->effectIdentity
						== directEffectIdentity
					&& existing->atlasIdentity
						== baseKey.atlasContentHash)
				{
					return true;
				}
				pageIndices = profile->second.pages;
				pages.reserve(pageIndices.size());
				for (UInt16 pageIndex : pageIndices)
				{
					AtlasCacheKey pageKey = baseKey;
					pageKey.pageIndex = pageIndex;
					const auto page = state.atlasCache.find(pageKey);
					if (page == state.atlasCache.end() || !page->second.resource)
						return false;
					pages.push_back(page->second.resource);
				}
			}
			if (pages.empty())
				return false;

			const std::vector<GlyphMaskType> masks =
				GetDirectProfileMasks(config, baseKey);
			if (masks.empty())
				return false;
			auto table = std::make_shared<DirectAtlasGlyphTable>();
			table->byteClass = byteClass;
			table->recordKind =
				masks.size() == 1
					&& masks[0] == GlyphMaskType::Composite
				? DirectCachedLetterKind::VanillaFontLetter
				: DirectCachedLetterKind::EffectLayers;
			table->effectLayerMask =
				BuildDirectEffectLayerMask(masks);
			table->codePage = directCodePage;
			table->layoutIdentity = directLayoutIdentity;
			table->effectIdentity = directEffectIdentity;
			table->atlasIdentity = baseKey.atlasContentHash;
			table->pageIdentityChecksum =
				BuildDirectPageIdentityChecksum(pages);
			const size_t directSlotCount =
				GetDirectGlyphSlotCount(byteClass);
			table->glyphs.resize(directSlotCount);
			if (table->recordKind
				== DirectCachedLetterKind::VanillaFontLetter)
			{
				table->vanillaGlyphs.resize(directSlotCount);
			}
			table->faceIndices.resize(directSlotCount);
			table->pages.reserve(pages.size());
			for (const auto& page : pages)
				table->pages.push_back(page);

			const UInt64 directIdentity =
				BuildDirectProfileIdentity(baseKey, *table, pages);
			table->profileIdentity = directIdentity;
			const std::wstring directPath =
				GetDirectCachedLetterPath(directIdentity, byteClass);
			if (TryLoadDirectCachedLetters(directPath, directIdentity,
				byteClass, baseKey, pages, masks, *table))
			{
				table->cpuMemory.Reset(CpuMemoryCategory::AtlasMetadata,
					GetDirectTableStorageBytes(*table));
				AtlasState& state = State();
				std::lock_guard<std::mutex> lock(state.atlasMutex);
				const auto profile = state.atlasProfiles.find(profileKey);
				if (profile == state.atlasProfiles.end()
					|| profile->second.pages != pageIndices
					|| state.completeAtlasProfiles.find(profileKey)
						== state.completeAtlasProfiles.end())
					return false;
				profile->second.directGlyphs = table;
				gLog.FormattedMessage(
					"tnvse_freetype_font: direct cached-letter table restored font=%u role=%s glyphs=%u layers=%u pages=%u bytes=%llu",
					config.fontId,
					byteClass == VectorFontByteClass::DoubleByte
						? "doubleByte" : "singleByte",
					table->resolvedGlyphs, table->resolvedLayers,
					static_cast<UInt32>(table->pages.size()),
					static_cast<unsigned long long>(
						GetDirectTableStorageBytes(*table)));
				return true;
			}
			UInt32 distanceFieldSpread = 0;
			if (baseKey.renderMode == AtlasRenderMode::ShaderEffects)
			{
				DistanceFieldRasterProfile profile;
				if (!ResolveDistanceFieldRasterProfile(config, byteClass,
					rasterScale, true, profile))
				{
					return false;
				}
				distanceFieldSpread = profile.sdfSpread;
			}

			std::vector<VectorEncodedGlyph> glyphs;
			std::vector<GlyphBitmapRequest> requests;
			std::vector<UInt64> cacheIds;
			glyphs.reserve(kDirectBuildBatchGlyphs);
			requests.reserve(kDirectBuildBatchGlyphs * masks.size());
			const size_t slotCount = table->glyphs.size();
			for (size_t batchStart = 0; batchStart < slotCount;
				batchStart += kDirectBuildBatchGlyphs)
			{
				const size_t batchEnd = std::min(slotCount,
					batchStart + kDirectBuildBatchGlyphs);
				glyphs.clear();
				requests.clear();
				for (size_t slot = batchStart; slot < batchEnd; ++slot)
				{
					VectorEncodedGlyph glyph;
					const UInt32 encodedCode =
						EncodeDirectGlyphSlot(byteClass, slot);
					FontLetter metrics = {};
					if (!LoadGlyphManifest(runtime, encodedCode,
						byteClass, &glyph, &metrics))
					{
						continue;
					}
					DirectCachedLetter& direct = table->glyphs[slot];
					direct.encodedCode =
						static_cast<UInt16>(encodedCode);
					direct.flags = kDirectCachedLetterValid
						| (IsSpaceCodePoint(glyph.codePoint)
							? kDirectCachedLetterKnownEmpty : 0);
					direct.byteClass =
						static_cast<UInt8>(byteClass);
					direct.width = metrics.fWidth;
					direct.leadingEdge = metrics.fLeadingEdge;
					direct.height = metrics.fHeight;
					direct.topEdge = metrics.fTopEdge;
					direct.spacing = metrics.fSpacing;
					if (direct.flags & kDirectCachedLetterKnownEmpty)
						NormalizeKnownEmptyAdvance(direct);
					if (slot < table->faceIndices.size())
						table->faceIndices[slot] =
							static_cast<UInt8>(std::min<UInt16>(
								glyph.faceIndex,
								std::numeric_limits<UInt8>::max()));
					glyphs.push_back(glyph);
				}
				for (const VectorEncodedGlyph& glyph : glyphs)
				{
					for (GlyphMaskType mask : masks)
					{
						requests.push_back({ &glyph, mask,
							mask == GlyphMaskType::DistanceField
								? distanceFieldSpread : 0 });
					}
				}
				if (requests.empty())
					continue;
				ResolveGlyphBitmapCacheIds(runtime, requests, rasterScale, cacheIds);
				if (cacheIds.size() != requests.size())
					return false;

				AtlasState& state = State();
				std::lock_guard<std::mutex> lock(state.atlasMutex);
				const auto profile = state.atlasProfiles.find(profileKey);
				if (profile == state.atlasProfiles.end()
					|| profile->second.pages != pageIndices
					|| state.completeAtlasProfiles.find(profileKey)
						== state.completeAtlasProfiles.end())
				{
					return false;
				}
				for (size_t requestIndex = 0;
					requestIndex < requests.size(); ++requestIndex)
				{
					const GlyphBitmapRequest& request = requests[requestIndex];
					if (!request.glyph || !cacheIds[requestIndex])
						continue;
					size_t glyphSlot = 0;
					if (!ResolveDirectGlyphSlot(byteClass,
						request.glyph->encodedCode, glyphSlot)
						|| glyphSlot >= table->glyphs.size())
					{
						continue;
					}
					DirectAtlasGlyphLayer* layer =
						FindOrCreateDirectLayer(
							table->glyphs[glyphSlot], request.maskType);
					if (!layer)
						continue;
					ResolveDirectLayerLocked(state, baseKey, profile->second,
						pageIndices, cacheIds[requestIndex],
						*layer);
				}
			}

			for (const DirectAtlasGlyphRecord& glyph : table->glyphs)
			{
				if (!(glyph.flags & kDirectCachedLetterValid))
					continue;
				const bool knownEmpty =
					(glyph.flags & kDirectCachedLetterKnownEmpty) != 0;
				bool complete = true;
				UInt32 layers = 0;
				for (GlyphMaskType mask : masks)
				{
					if (knownEmpty)
						break;
					const DirectAtlasGlyphLayer* layer =
						FindDirectLayer(glyph, mask);
					if (!layer)
					{
						complete = false;
						break;
					}
					++layers;
				}
				if (complete)
				{
					++table->resolvedGlyphs;
					table->resolvedLayers += layers;
				}
				else
				return false;
			}
			if (!table->resolvedGlyphs)
				return false;
			if (table->recordKind
					== DirectCachedLetterKind::VanillaFontLetter
				&& !ConvertCompositeTableToVanillaLetters(
					*table, pages, rasterScale))
			{
				return false;
			}
			if (!SaveDirectCachedLetters(directPath,
				directIdentity, byteClass, baseKey,
				pages, masks, *table))
			{
				return false;
			}

			table->cpuMemory.Reset(CpuMemoryCategory::AtlasMetadata,
				GetDirectTableStorageBytes(*table));
			{
				AtlasState& state = State();
				std::lock_guard<std::mutex> lock(state.atlasMutex);
				const auto profile = state.atlasProfiles.find(profileKey);
				if (profile == state.atlasProfiles.end()
					|| profile->second.pages != pageIndices
					|| state.completeAtlasProfiles.find(profileKey)
						== state.completeAtlasProfiles.end())
				{
					return false;
				}
				profile->second.directGlyphs = table;
			}
			gLog.FormattedMessage(
				"tnvse_freetype_font: direct atlas glyph table font=%u role=%s glyphs=%u layers=%u pages=%u bytes=%llu",
				config.fontId,
				byteClass == VectorFontByteClass::DoubleByte
					? "doubleByte" : "singleByte",
				table->resolvedGlyphs, table->resolvedLayers,
				static_cast<UInt32>(table->pages.size()),
				static_cast<unsigned long long>(
					GetDirectTableStorageBytes(*table)));
			return true;
		}

		bool ValidateSealedDirectTable(
			const DirectAtlasGlyphTable& table,
			const SealedDirectFontProfile& sealed, size_t roleIndex)
		{
			if (!table.SlotCount()
				|| table.faceIndices.size() != table.SlotCount()
				|| roleIndex >= sealed.pageOrdinals.size()
				|| roleIndex >= sealed.faceBaselineOffsets.size()
				|| sealed.faceBaselineOffsets[roleIndex].empty()
				|| std::any_of(table.faceIndices.begin(),
					table.faceIndices.end(),
					[&](UInt8 faceIndex)
					{
						return faceIndex
							>= sealed.faceBaselineOffsets[roleIndex].size();
					}))
			{
				return false;
			}
			if (table.recordKind
				== DirectCachedLetterKind::VanillaFontLetter)
			{
				for (const FontLetter& letter : table.vanillaGlyphs)
				{
					if (letter.iTextureIndex < 0)
						continue;
					if (letter.iTextureIndex
							>= kMaximumAtlasSnapshotPages
						|| sealed.pageOrdinals[roleIndex][
							letter.iTextureIndex]
							== kInvalidDirectAtlasPageSlot)
					{
						return false;
					}
				}
			}
			else
			{
				for (const DirectCachedLetter& letter : table.glyphs)
				{
					if (!(letter.flags & kDirectCachedLetterValid)
						|| (letter.flags & kDirectCachedLetterKnownEmpty))
					{
						continue;
					}
					for (const DirectAtlasGlyphLayer& layer :
						letter.layers)
					{
						if (!layer.valid())
							continue;
						if (layer.pageSlot
								>= kMaximumAtlasSnapshotPages
							|| sealed.pageOrdinals[roleIndex][
								layer.pageSlot]
								== kInvalidDirectAtlasPageSlot)
						{
							return false;
						}
					}
				}
			}
			return true;
		}

		bool IsSealedDirectProfileValid(
			const SealedDirectFontProfile& sealed)
		{
			bool foundTable = false;
			for (const auto& table : sealed.tables)
			{
				if (!table)
					continue;
				foundTable = true;
				if (!table->validity
					|| !table->validity->load(
						std::memory_order_acquire))
				{
					return false;
				}
			}
			return foundTable;
		}

		bool PublishSealedDirectFontProfile(RuntimeFont& runtime,
			float rasterScale,
			std::shared_ptr<const SealedDirectFontProfile>& pinnedProfile)
		{
			pinnedProfile.reset();
			const FontConfig& config = GetRuntimeConfig(runtime);
			AtlasState& state = State();
			std::shared_ptr<const SealedDirectFontProfile> published;
			{
				std::lock_guard<std::mutex> lock(state.atlasMutex);
				auto sealed =
					std::make_shared<SealedDirectFontProfile>();
				sealed->validityEpoch = state.directProfileEpoch.load(
					std::memory_order_relaxed);
				sealed->layoutIdentity =
					GetRuntimeDirectLayoutIdentity(runtime);
				sealed->scaleMilli = static_cast<UInt32>(std::lround(
					rasterScale * 1000.0f));
				sealed->codePage = GetFreeTypeTextCodePage();
				const size_t roleCount = UsesDbcsTextLayout() ? 2u : 1u;
				bool commonProfileInitialized = false;
				for (size_t roleIndex = 0; roleIndex < roleCount;
					++roleIndex)
				{
					const VectorFontByteClass byteClass =
						static_cast<VectorFontByteClass>(roleIndex);
					AtlasCacheKey key;
					if (!ResolvePrewarmAtlasKey(config, byteClass,
						rasterScale, key))
					{
						return false;
					}
					const AtlasProfileKey profileKey =
						MakeAtlasProfileKey(key);
					const auto profile =
						state.atlasProfiles.find(profileKey);
					if (profile == state.atlasProfiles.end()
						|| state.completeAtlasProfiles.find(profileKey)
							== state.completeAtlasProfiles.end()
						|| !profile->second.directGlyphs)
					{
						return false;
					}
					const auto& table = profile->second.directGlyphs;
					if (table->codePage != sealed->codePage
						|| table->layoutIdentity
							!= GetRuntimeDirectRoleLayoutIdentity(
								runtime, byteClass)
						|| table->byteClass != byteClass
						|| table->pages.size()
							!= profile->second.pages.size()
						|| table->pages.size()
							> kMaximumAtlasSnapshotPages)
					{
						return false;
					}
					if (!commonProfileInitialized)
					{
						sealed->pixelMode = key.pixelMode;
						sealed->renderMode = key.renderMode;
						sealed->padding = key.padding;
						sealed->recordKind = table->recordKind;
						sealed->effectLayerMask =
							table->effectLayerMask;
						commonProfileInitialized = true;
					}
					else if (sealed->pixelMode != key.pixelMode
						|| sealed->renderMode != key.renderMode
						|| sealed->padding != key.padding
						|| sealed->recordKind != table->recordKind
						|| sealed->effectLayerMask
							!= table->effectLayerMask)
					{
						return false;
					}
					sealed->tables[roleIndex] = table;
					for (size_t pageSlot = 0;
						pageSlot < table->pages.size(); ++pageSlot)
					{
						std::shared_ptr<AtlasResource> page =
							table->pages[pageSlot].lock();
						if (!page || !page->property
							|| !GetAtlasTexture(*page)
							|| !page->compactSnapshot
							|| !page->pageContentHash
							|| page->pixelMode != key.pixelMode
							|| page->renderMode != key.renderMode
							|| page->padding != key.padding
							|| page->levelZeroOnly
								!= key.levelZeroOnly
							|| page->compactSnapshot->sourceHeader
								.pageContentHash
								!= page->pageContentHash)
						{
							return false;
						}
						sealed->tableAtlasOwners[roleIndex].push_back(page);
						UInt16 ordinal =
							kInvalidDirectAtlasPageSlot;
						for (UInt16 candidate = 0;
							candidate < sealed->atlases.size();
							++candidate)
						{
							if (sealed->atlases[candidate].get()
									== page.get()
								|| AreAtlasResourcesBackedBySameTexture(
									*sealed->atlases[candidate], *page))
							{
								ordinal = candidate;
								break;
							}
						}
						if (ordinal == kInvalidDirectAtlasPageSlot)
						{
							if (sealed->atlases.size()
								>= kMaximumAtlasSnapshotPages)
							{
								return false;
							}
							ordinal = static_cast<UInt16>(
								sealed->atlases.size());
							sealed->atlases.push_back(
								std::move(page));
						}
						sealed->pageOrdinals[roleIndex][pageSlot]
							= ordinal;
					}
					GetRuntimeDirectBaselineOffsets(runtime,
						byteClass,
						sealed->roleBaselineOffsets[roleIndex],
						sealed->faceBaselineOffsets[roleIndex]);
					if (!ValidateSealedDirectTable(
						*table, *sealed, roleIndex))
					{
						return false;
					}
				}
				UInt64 identity = HashDirectBytes(
					&sealed->validityEpoch,
					sizeof(sealed->validityEpoch));
				identity = HashDirectBytes(&sealed->layoutIdentity,
					sizeof(sealed->layoutIdentity), identity);
				identity = HashDirectBytes(&sealed->scaleMilli,
					sizeof(sealed->scaleMilli), identity);
				identity = HashDirectBytes(&sealed->codePage,
					sizeof(sealed->codePage), identity);
				for (const auto& table : sealed->tables)
				{
					if (!table)
						continue;
					identity = HashDirectBytes(
						&table->profileIdentity,
						sizeof(table->profileIdentity), identity);
				}
				sealed->identity = identity ? identity : 1;
				const auto shared =
					state.sealedDirectProfiles.find(
						sealed->identity);
				if (shared != state.sealedDirectProfiles.end())
				{
					published = shared->second.lock();
					if (published
						&& published->validityEpoch
							!= sealed->validityEpoch)
					{
						published.reset();
					}
					if (published
						&& !IsSealedDirectProfileValid(*published))
					{
						published.reset();
					}
				}
				if (!published)
				{
					size_t bytes = sizeof(SealedDirectFontProfile)
						+ sealed->atlases.capacity()
							* sizeof(std::shared_ptr<AtlasResource>);
					for (const auto& owners : sealed->tableAtlasOwners)
					{
						bytes += owners.capacity()
							* sizeof(std::shared_ptr<AtlasResource>);
					}
					for (const auto& baselines :
						sealed->faceBaselineOffsets)
					{
						bytes += baselines.capacity()
							* sizeof(float);
					}
					sealed->cpuMemory.Reset(
						CpuMemoryCategory::AtlasMetadata, bytes);
					published = sealed;
					state.sealedDirectProfiles[
						sealed->identity] = published;
				}
				for (const auto& atlas : published->atlases)
				{
					if (atlas)
						atlas->sealedImmutable = true;
				}
				for (const auto& owners : published->tableAtlasOwners)
				{
					for (const auto& atlas : owners)
					{
						if (atlas)
							atlas->sealedImmutable = true;
					}
				}
				ReleaseSealedAtlasCpuIndexesLocked(
					state, *published);
			}
			StoreRuntimeSealedDirectProfile(runtime, published);
			// Preserve the exact generation even if a loading-overlay draw revokes
			// the mutable runtime slot immediately after publication.
			pinnedProfile = published;
			state.directProfilesAvailable.store(
				true, std::memory_order_release);
			gLog.FormattedMessage(
				"tnvse_freetype_font: sealed direct profile font=%u identity=%016llX kind=%u glyphSlots=%u/%u pages=%u epoch=%u",
				config.fontId,
				static_cast<unsigned long long>(published->identity),
				static_cast<UInt32>(published->recordKind),
				published->tables[0]
					? static_cast<UInt32>(
						published->tables[0]->SlotCount()) : 0,
				published->tables[1]
					? static_cast<UInt32>(
						published->tables[1]->SlotCount()) : 0,
				static_cast<UInt32>(published->atlases.size()),
				published->validityEpoch);
			return true;
		}
	}
	bool BuildDirectGlyphAtlasTablesPinned(RuntimeFont& runtime,
		float rasterScale,
		std::shared_ptr<const SealedDirectFontProfile>& pinnedProfile)
	{
		pinnedProfile.reset();
		const bool single = BuildDirectGlyphAtlasTableRole(runtime,
			VectorFontByteClass::SingleByte, rasterScale);
		const bool doubleByte = !UsesDbcsTextLayout()
			|| BuildDirectGlyphAtlasTableRole(runtime,
				VectorFontByteClass::DoubleByte, rasterScale);
		const bool complete = single && doubleByte;
		if (!complete
			|| !PublishSealedDirectFontProfile(
				runtime, rasterScale, pinnedProfile))
		{
			pinnedProfile.reset();
			InvalidateSealedDirectFontProfile(runtime);
			return false;
		}
		ReleaseSealedRuntimeFreeTypeState(runtime);
		return true;
	}

	bool BuildDirectGlyphAtlasTables(RuntimeFont& runtime, float rasterScale)
	{
		std::shared_ptr<const SealedDirectFontProfile> pinnedProfile;
		return BuildDirectGlyphAtlasTablesPinned(
			runtime, rasterScale, pinnedProfile);
	}
}

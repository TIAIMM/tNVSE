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

	void InvalidateSealedDirectFontProfile(RuntimeFont& runtime)
	{
		StoreRuntimeSealedDirectProfile(runtime, {});
	}

	bool IsSealedDirectFontProfileUsable(RuntimeFont& runtime,
		const std::shared_ptr<const SealedDirectFontProfile>& sealed,
		float rasterScale)
	{
		if (!sealed || !std::isfinite(rasterScale) || rasterScale <= 0.0f)
			return false;
		const UInt32 scaleMilli = static_cast<UInt32>(std::lround(
			rasterScale * 1000.0f));
		AtlasState& state = State();
		return scaleMilli
			&& state.directProfilesAvailable.load(std::memory_order_acquire)
			&& sealed->validityEpoch == state.directProfileEpoch.load(
				std::memory_order_acquire)
			&& sealed->layoutIdentity == GetRuntimeDirectLayoutIdentity(runtime)
			&& sealed->scaleMilli == scaleMilli
			&& sealed->codePage == GetFreeTypeTextCodePage()
			&& IsSealedDirectProfileValid(*sealed);
	}

	std::shared_ptr<const SealedDirectFontProfile>
		AcquireSealedDirectLayoutProfile(RuntimeFont& runtime)
	{
		std::shared_ptr<const SealedDirectFontProfile> sealed =
			LoadRuntimeSealedDirectProfile(runtime);
		if (!sealed)
			return {};
		AtlasState& state = State();
		if (sealed->validityEpoch
				!= state.directProfileEpoch.load(
					std::memory_order_acquire)
			|| sealed->layoutIdentity
				!= GetRuntimeDirectLayoutIdentity(runtime)
			|| sealed->codePage != GetFreeTypeTextCodePage()
			|| !IsSealedDirectProfileValid(*sealed))
		{
			InvalidateSealedDirectFontProfile(runtime);
			return {};
		}
		return sealed;
	}

	std::shared_ptr<const SealedDirectFontProfile>
		AcquireSealedDirectFontProfile(RuntimeFont& runtime,
			float rasterScale)
	{
		std::shared_ptr<const SealedDirectFontProfile> sealed =
			AcquireSealedDirectLayoutProfile(runtime);
		if (!sealed)
			return {};
		const UInt32 scaleMilli =
			std::isfinite(rasterScale) && rasterScale > 0.0f
				? static_cast<UInt32>(std::lround(
					rasterScale * 1000.0f))
				: 0;
		return scaleMilli && sealed->scaleMilli == scaleMilli
			? sealed : std::shared_ptr<const SealedDirectFontProfile>();
	}

	SealedDirectGlyphLookup DecodeSealedDirectGlyph(
		const SealedDirectFontProfile& sealed,
		const char* text, VectorEncodedGlyph& glyph)
	{
		glyph = {};
		if (!text || !*text)
			return SealedDirectGlyphLookup::Invalid;
		UInt32 encodedCode = 0;
		if (text[1]
			&& TryDecodeFreeTypeDoubleByte(text, encodedCode))
		{
			glyph.byteLength = 2;
			glyph.byteClass =
				VectorFontByteClass::DoubleByte;
			glyph.codePoint = 0x10000u;
		}
		else
		{
			encodedCode = static_cast<UInt8>(text[0]);
			glyph.byteLength = 1;
			glyph.byteClass =
				VectorFontByteClass::SingleByte;
			glyph.codePoint = encodedCode;
		}
		glyph.encodedCode = encodedCode;
		const size_t roleIndex =
			static_cast<size_t>(glyph.byteClass);
		if (roleIndex >= sealed.tables.size()
			|| !sealed.tables[roleIndex])
		{
			return SealedDirectGlyphLookup::Invalid;
		}
		const DirectAtlasGlyphTable& table =
			*sealed.tables[roleIndex];
		size_t slot = 0;
		if (!ResolveDirectGlyphSlot(glyph.byteClass,
				encodedCode, slot)
			|| slot >= table.SlotCount()
			|| slot > std::numeric_limits<UInt16>::max())
		{
			return SealedDirectGlyphLookup::Invalid;
		}
		glyph.directSlot = static_cast<UInt16>(slot);
		if (slot >= table.faceIndices.size())
			return SealedDirectGlyphLookup::Invalid;
		glyph.faceIndex = table.faceIndices[slot];
		if (table.recordKind
			== DirectCachedLetterKind::StockFontLetter)
		{
			const FontLetter& letter = table.stockGlyphs[slot];
			if (letter.iTextureIndex == -1)
				return SealedDirectGlyphLookup::Unavailable;
			if (letter.iTextureIndex < -2)
				return SealedDirectGlyphLookup::Invalid;
			glyph.directWidth = letter.fWidth;
			glyph.directHeight = letter.fHeight;
			glyph.directLeadingEdge = letter.fLeadingEdge;
			glyph.directSpacing = letter.fSpacing;
			glyph.directTopEdge = letter.fTopEdge;
		}
		else
		{
			const DirectCachedLetter& letter =
				table.glyphs[slot];
			if (!letter.flags)
				return SealedDirectGlyphLookup::Unavailable;
			if (!(letter.flags & kDirectCachedLetterValid)
				|| letter.encodedCode != encodedCode
				|| letter.byteClass
					!= static_cast<UInt8>(glyph.byteClass))
			{
				return SealedDirectGlyphLookup::Invalid;
			}
			glyph.directWidth = letter.width;
			glyph.directHeight = letter.height;
			glyph.directLeadingEdge = letter.leadingEdge;
			glyph.directSpacing = letter.spacing;
			glyph.directTopEdge = letter.topEdge;
		}
		const auto& baselines =
			sealed.faceBaselineOffsets[roleIndex];
		glyph.directBaselineOffset =
			glyph.faceIndex < baselines.size()
				? baselines[glyph.faceIndex]
				: sealed.roleBaselineOffsets[roleIndex];
		glyph.hasDirectMetrics = true;
		return SealedDirectGlyphLookup::Resolved;
	}

	SealedDirectGlyphLookup DecodeSealedDirectGlyph(RuntimeFont& runtime,
		const char* text, VectorEncodedGlyph& glyph)
	{
		glyph = {};
		if (!text || !*text)
			return SealedDirectGlyphLookup::Invalid;
		std::shared_ptr<const SealedDirectFontProfile> sealed =
			LoadRuntimeSealedDirectProfile(runtime);
		if (!sealed)
			return SealedDirectGlyphLookup::Unavailable;
		AtlasState& state = State();
		if (!state.directProfilesAvailable.load(
			std::memory_order_acquire))
		{
			// During a DEFAULT-pool reset the immutable owner is deliberately
			// retained while GPU publication is unavailable. Do not reopen FT.
			return SealedDirectGlyphLookup::Invalid;
		}
		if (sealed->validityEpoch
				!= state.directProfileEpoch.load(
					std::memory_order_acquire)
			|| sealed->layoutIdentity
				!= GetRuntimeDirectLayoutIdentity(runtime)
			|| sealed->codePage != GetFreeTypeTextCodePage()
			|| !IsSealedDirectProfileValid(*sealed))
		{
			InvalidateSealedDirectFontProfile(runtime);
			return SealedDirectGlyphLookup::Unavailable;
		}
		const SealedDirectGlyphLookup result =
			DecodeSealedDirectGlyph(*sealed, text, glyph);
		if (result == SealedDirectGlyphLookup::Invalid)
		{
			InvalidateSealedDirectFontProfile(runtime);
		}
		return result;
	}

	bool GetSealedDirectAtlasGlyphBatch(RuntimeFont& runtime,
		const std::shared_ptr<const SealedDirectFontProfile>& sealed,
		const std::vector<DirectGlyphCommand>& glyphs,
		GlyphMaskType maskType, float rasterScale,
		AtlasPixelMode pixelMode, AtlasRenderMode renderMode,
		UInt32 padding, DirectAtlasGlyphBatch& result)
	{
		result.Clear();
		if (!sealed)
			return false;
		if (glyphs.empty())
		{
			result.sealed = sealed;
			result.tables = sealed->tables;
			return true;
		}
		const size_t maskIndex = static_cast<size_t>(maskType);
		if (maskIndex >= kDirectAtlasMaskCount)
			return false;

		const std::shared_ptr<const SealedDirectFontProfile> published =
			LoadRuntimeSealedDirectProfile(runtime);
		if (published.get() != sealed.get()
			|| !IsSealedDirectFontProfileUsable(runtime, sealed, rasterScale)
			|| sealed->pixelMode != pixelMode
			|| sealed->renderMode != renderMode
			|| sealed->padding != padding)
		{
			InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed);
			return false;
		}
		const UInt8 requestedMask = static_cast<UInt8>(
			1u << static_cast<UInt8>(maskType));
		if (!(sealed->effectLayerMask & requestedMask))
			return false;

		result.sealed = sealed;
		result.tables = sealed->tables;
		result.glyphs.resize(glyphs.size());
		for (size_t glyphIndex = 0;
			glyphIndex < glyphs.size(); ++glyphIndex)
		{
			const DirectGlyphCommand& glyph = glyphs[glyphIndex];
			const size_t roleIndex = glyph.byteClass;
			if (roleIndex >= sealed->tables.size()
				|| !sealed->tables[roleIndex]
				|| !glyph.byteLength
				|| glyph.directSlot
					== std::numeric_limits<UInt16>::max())
			{
				InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed);
				result.Clear();
				return false;
			}
			const DirectAtlasGlyphTable& table =
				*sealed->tables[roleIndex];
			size_t expectedSlot = 0;
			const VectorFontByteClass byteClass =
				static_cast<VectorFontByteClass>(roleIndex);
			if (!ResolveDirectGlyphSlot(byteClass,
					glyph.encodedCode, expectedSlot)
				|| expectedSlot != glyph.directSlot
				|| expectedSlot >= table.SlotCount())
			{
				InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed);
				result.Clear();
				return false;
			}

			DirectAtlasBatchGlyph& output =
				result.glyphs[glyphIndex];
			output.byteClass = glyph.byteClass;
			UInt16 localPage = kInvalidDirectAtlasPageSlot;
			if (table.recordKind
				== DirectCachedLetterKind::StockFontLetter)
			{
				if (maskType != GlyphMaskType::Composite)
				{
					InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed);
					result.Clear();
					return false;
				}
				const FontLetter& letter =
					table.stockGlyphs[expectedSlot];
				if (letter.iTextureIndex == -2)
				{
					output.knownEmpty = true;
					continue;
				}
				if (letter.iTextureIndex < 0
					|| letter.iTextureIndex
						>= kMaximumAtlasSnapshotPages)
				{
					InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed);
					result.Clear();
					return false;
				}
				localPage =
					static_cast<UInt16>(letter.iTextureIndex);
				output.stockLetter = &letter;
			}
			else
			{
				const DirectCachedLetter& letter =
					table.glyphs[expectedSlot];
				if (!(letter.flags & kDirectCachedLetterValid)
					|| letter.encodedCode != glyph.encodedCode
					|| letter.byteClass != glyph.byteClass)
				{
					InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed);
					result.Clear();
					return false;
				}
				if (letter.flags & kDirectCachedLetterKnownEmpty)
				{
					output.knownEmpty = true;
					continue;
				}
				const DirectAtlasGlyphLayer* layer =
					FindDirectLayer(letter, maskType);
				if (!layer || layer->pageSlot
						>= kMaximumAtlasSnapshotPages)
				{
					InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed);
					result.Clear();
					return false;
				}
				localPage = layer->pageSlot;
				output.snapshotPlacementIndex =
					layer->snapshotPlacementIndex;
			}
			const UInt16 ordinal =
				sealed->pageOrdinals[roleIndex][localPage];
			if (ordinal >= sealed->atlases.size())
			{
				InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed);
				result.Clear();
				return false;
			}
			output.atlasPage = ordinal;
			if (output.stockLetter)
				continue;
			const auto& page = sealed->atlases[ordinal];
			if (!page || !page->compactSnapshot
				|| output.snapshotPlacementIndex
					>= page->compactSnapshot->placements.size())
			{
				InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed);
				result.Clear();
				return false;
			}
			const AtlasSnapshotPlacement& placement =
				page->compactSnapshot->placements[
					output.snapshotPlacementIndex];
			if (placement.maskType
				!= static_cast<UInt8>(maskType))
			{
				InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed);
				result.Clear();
				return false;
			}
			output.placement = &placement;
		}
		RecordFreeTypePerf(
			FreeTypePerfCounter::GpuResidentGlyphHit,
			static_cast<UInt64>(glyphs.size()));
		return true;
	}

	bool GetDirectAtlasGlyphBatch(RuntimeFont& runtime,
		const std::vector<AtlasGlyphInstance>& glyphs,
		GlyphMaskType maskType, float rasterScale,
		AtlasPixelMode pixelMode, AtlasRenderMode renderMode,
		UInt32 padding, DirectAtlasGlyphBatch& result)
	{
		result.Clear();
		if (glyphs.empty())
			return true;
		const size_t maskIndex = static_cast<size_t>(maskType);
		if (maskIndex >= kDirectAtlasMaskCount)
			return false;

		std::shared_ptr<const SealedDirectFontProfile> sealed =
			LoadRuntimeSealedDirectProfile(runtime);
		AtlasState& state = State();
		if (sealed
			&& IsSealedDirectFontProfileUsable(
				runtime, sealed, rasterScale)
			&& sealed->pixelMode == pixelMode
			&& sealed->renderMode == renderMode
			&& sealed->padding == padding)
		{
			const UInt8 requestedMask =
				static_cast<UInt8>(1u << static_cast<UInt8>(
					maskType));
			if (!(sealed->effectLayerMask & requestedMask))
			{
				return false;
			}
			result.sealed = sealed;
			result.tables = sealed->tables;
			result.glyphs.resize(glyphs.size());
			for (size_t glyphIndex = 0;
				glyphIndex < glyphs.size(); ++glyphIndex)
			{
				const VectorEncodedGlyph& glyph =
					glyphs[glyphIndex].glyph;
				const size_t roleIndex =
					static_cast<size_t>(glyph.byteClass);
				if (roleIndex >= sealed->tables.size()
					|| !sealed->tables[roleIndex])
				{
					InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed);
					result.Clear();
					return false;
				}
				const DirectAtlasGlyphTable& table =
					*sealed->tables[roleIndex];
				// A generic decode deliberately lacks a sealed slot when a code point
				// was not part of the immutable prewarm table. That is an ordinary
				// direct-cache miss, not profile corruption.
				if (!glyph.hasDirectMetrics
					|| glyph.directSlot
						== std::numeric_limits<UInt16>::max())
				{
					result.Clear();
					return false;
				}
				const size_t glyphSlot = glyph.directSlot;
				if (glyphSlot >= table.SlotCount())
				{
					InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed);
					result.Clear();
					return false;
				}
				DirectAtlasBatchGlyph& output =
					result.glyphs[glyphIndex];
				output.byteClass =
					static_cast<UInt8>(glyph.byteClass);
				UInt16 localPage = kInvalidDirectAtlasPageSlot;
				if (table.recordKind
					== DirectCachedLetterKind::StockFontLetter)
				{
					if (maskType != GlyphMaskType::Composite)
					{
						InvalidateSealedDirectFontProfileIfCurrent(
							runtime, sealed);
						result.Clear();
						return false;
					}
					const FontLetter& letter =
						table.stockGlyphs[glyphSlot];
					if (letter.iTextureIndex == -2)
					{
						output.knownEmpty = true;
						continue;
					}
					if (letter.iTextureIndex < 0
						|| letter.iTextureIndex
							>= kMaximumAtlasSnapshotPages)
					{
						InvalidateSealedDirectFontProfileIfCurrent(
							runtime, sealed);
						result.Clear();
						return false;
					}
					localPage = static_cast<UInt16>(
						letter.iTextureIndex);
					output.stockLetter = &letter;
				}
				else
				{
					const DirectCachedLetter& letter =
						table.glyphs[glyphSlot];
					if (!(letter.flags & kDirectCachedLetterValid)
						|| letter.encodedCode != glyph.encodedCode
						|| letter.byteClass
							!= static_cast<UInt8>(glyph.byteClass))
					{
						InvalidateSealedDirectFontProfileIfCurrent(
							runtime, sealed);
						result.Clear();
						return false;
					}
					if (letter.flags & kDirectCachedLetterKnownEmpty)
					{
						output.knownEmpty = true;
						continue;
					}
					const DirectAtlasGlyphLayer* layer =
						FindDirectLayer(letter, maskType);
					if (!layer || layer->pageSlot
							>= kMaximumAtlasSnapshotPages)
					{
						InvalidateSealedDirectFontProfileIfCurrent(
							runtime, sealed);
						result.Clear();
						return false;
					}
					localPage = layer->pageSlot;
					output.snapshotPlacementIndex =
						layer->snapshotPlacementIndex;
				}
				const UInt16 ordinal =
					sealed->pageOrdinals[roleIndex][localPage];
				if (ordinal >= sealed->atlases.size())
				{
					InvalidateSealedDirectFontProfileIfCurrent(
						runtime, sealed);
					result.Clear();
					return false;
				}
				output.atlasPage = ordinal;
				if (output.stockLetter)
					continue;
				const auto& page = sealed->atlases[ordinal];
				if (!page || !page->compactSnapshot
					|| output.snapshotPlacementIndex
						>= page->compactSnapshot->placements.size())
				{
					InvalidateSealedDirectFontProfileIfCurrent(
						runtime, sealed);
					result.Clear();
					return false;
				}
				const AtlasSnapshotPlacement& placement =
					page->compactSnapshot->placements[
						output.snapshotPlacementIndex];
				if (placement.maskType
					!= static_cast<UInt8>(maskType))
				{
					InvalidateSealedDirectFontProfileIfCurrent(
						runtime, sealed);
					result.Clear();
					return false;
				}
				output.placement = &placement;
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::GpuResidentGlyphHit,
				static_cast<UInt64>(glyphs.size()));
			return true;
		}
		if (sealed)
		{
			InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed);
			return false;
		}

		std::array<bool, 2> roleUsed = {};
		for (const AtlasGlyphInstance& instance : glyphs)
		{
			const size_t roleIndex =
				static_cast<size_t>(instance.glyph.byteClass);
			if (roleIndex >= roleUsed.size())
				return false;
			roleUsed[roleIndex] = true;
		}

		const FontConfig& config = GetRuntimeConfig(runtime);
		std::array<AtlasCacheKey, 2> baseKeys;
		std::array<std::array<bool, kMaximumAtlasSnapshotPages>, 2>
			usedPages = {};
		std::array<std::array<UInt16, kMaximumAtlasSnapshotPages>, 2>
			pageOrdinals;
		for (auto& role : pageOrdinals)
			role.fill(kInvalidDirectAtlasPageSlot);

		std::lock_guard<std::mutex> lock(state.atlasMutex);
		auto fail = [&](const char* stage)
		{
			result.Clear();
			const UInt64 logKey = 0x4000000000000000ull
				^ (static_cast<UInt64>(config.fontId) << 32)
				^ (static_cast<UInt64>(std::lround(
					rasterScale * 1000.0f)) << 8)
				^ (static_cast<UInt64>(pixelMode) << 4)
				^ static_cast<UInt64>(renderMode);
			if (g_bEnableFreeTypeFontRenderingLog
				&& state.loggedDirectGlyphBatches.insert(logKey).second)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: direct atlas geometry batch unavailable font=%u sourceScale=%.3f mode=%u render=%u stage=%s",
					config.fontId, rasterScale,
					static_cast<UInt32>(pixelMode),
					static_cast<UInt32>(renderMode), stage);
			}
			return false;
		};

		for (size_t roleIndex = 0; roleIndex < roleUsed.size(); ++roleIndex)
		{
			if (!roleUsed[roleIndex])
				continue;
			const VectorFontByteClass byteClass =
				static_cast<VectorFontByteClass>(roleIndex);
			if (!ResolvePrewarmAtlasKey(config, byteClass,
					rasterScale, baseKeys[roleIndex])
				|| baseKeys[roleIndex].pixelMode != pixelMode
				|| baseKeys[roleIndex].renderMode != renderMode
				|| baseKeys[roleIndex].padding != padding)
			{
				return fail("profile-key");
			}
			const AtlasProfileKey profileKey =
				MakeAtlasProfileKey(baseKeys[roleIndex]);
			const auto profile = state.atlasProfiles.find(profileKey);
			if (profile == state.atlasProfiles.end()
				|| state.completeAtlasProfiles.find(profileKey)
					== state.completeAtlasProfiles.end()
				|| !profile->second.directGlyphs)
			{
				return fail("profile-table");
			}
			result.tables[roleIndex] = profile->second.directGlyphs;
		}

		result.glyphs.resize(glyphs.size());
		for (size_t glyphIndex = 0; glyphIndex < glyphs.size(); ++glyphIndex)
		{
			const VectorEncodedGlyph& glyph = glyphs[glyphIndex].glyph;
			const size_t roleIndex = static_cast<size_t>(glyph.byteClass);
			if (roleIndex >= result.tables.size()
				|| !result.tables[roleIndex])
			{
				return fail("table-role");
			}
			const DirectAtlasGlyphTable& table =
				*result.tables[roleIndex];
			size_t glyphSlot = 0;
			if (!ResolveDirectGlyphSlot(glyph.byteClass,
					glyph.encodedCode, glyphSlot)
				|| glyphSlot >= table.glyphs.size())
			{
				return fail("encoded-slot");
			}
			const DirectCachedLetter& letter = table.glyphs[glyphSlot];
			if (!(letter.flags & kDirectCachedLetterValid)
				|| letter.encodedCode != glyph.encodedCode
				|| letter.byteClass != static_cast<UInt8>(glyph.byteClass))
			{
				return fail("letter-identity");
			}

			DirectAtlasBatchGlyph& output = result.glyphs[glyphIndex];
			output.byteClass = static_cast<UInt8>(glyph.byteClass);
			if (letter.flags & kDirectCachedLetterKnownEmpty)
			{
				if (!IsSpaceCodePoint(glyph.codePoint))
					return fail("known-empty");
				output.knownEmpty = true;
				continue;
			}

			const DirectAtlasGlyphLayer* layer =
				FindDirectLayer(letter, maskType);
			if (!layer || layer->reserved
				|| layer->pageSlot >= table.pages.size()
				|| layer->pageSlot >= kMaximumAtlasSnapshotPages)
			{
				return fail("direct-layer");
			}
			usedPages[roleIndex][layer->pageSlot] = true;
			output.snapshotPlacementIndex =
				layer->snapshotPlacementIndex;
			// This is a role-local page until the deterministic page pass below.
			output.atlasPage = layer->pageSlot;
		}

		for (size_t roleIndex = 0; roleIndex < roleUsed.size(); ++roleIndex)
		{
			if (!roleUsed[roleIndex])
				continue;
			const DirectAtlasGlyphTable& table =
				*result.tables[roleIndex];
			for (UInt16 pageSlot = 0;
				pageSlot < kMaximumAtlasSnapshotPages; ++pageSlot)
			{
				if (!usedPages[roleIndex][pageSlot])
					continue;
				if (pageSlot >= table.pages.size())
					return fail("page-slot");
				std::shared_ptr<AtlasResource> page =
					table.pages[pageSlot].lock();
				const AtlasCacheKey& key = baseKeys[roleIndex];
				if (!page || !page->compactSnapshot
					|| !page->pageContentHash
					|| page->compactSnapshot->sourceHeader.pageContentHash
						!= page->pageContentHash
					|| page->pixelMode != key.pixelMode
					|| page->renderMode != key.renderMode
					|| page->padding != key.padding
					|| page->levelZeroOnly != key.levelZeroOnly
					|| !page->width || !page->height || !page->property
					|| !GetAtlasTexture(*page))
				{
					return fail("page-resource");
				}

				UInt16 ordinal = kInvalidDirectAtlasPageSlot;
				for (UInt16 candidate = 0;
					candidate < result.atlases.size(); ++candidate)
				{
					if (result.atlases[candidate].get() == page.get()
						|| AreAtlasResourcesBackedBySameTexture(
							*result.atlases[candidate], *page))
					{
						ordinal = candidate;
						break;
					}
				}
				if (ordinal == kInvalidDirectAtlasPageSlot)
				{
					if (result.atlases.size()
						>= kMaximumAtlasSnapshotPages)
					{
						return fail("page-limit");
					}
					ordinal = static_cast<UInt16>(
						result.atlases.size());
					result.atlases.push_back(std::move(page));
				}
				pageOrdinals[roleIndex][pageSlot] = ordinal;
			}
		}

		for (DirectAtlasBatchGlyph& glyph : result.glyphs)
		{
			if (glyph.knownEmpty)
				continue;
			const size_t roleIndex = glyph.byteClass;
			if (roleIndex >= pageOrdinals.size()
				|| glyph.atlasPage >= kMaximumAtlasSnapshotPages)
			{
				return fail("page-remap-source");
			}
			const UInt16 ordinal =
				pageOrdinals[roleIndex][glyph.atlasPage];
			if (ordinal >= result.atlases.size())
				return fail("page-remap-target");
			const std::shared_ptr<AtlasResource>& page =
				result.atlases[ordinal];
			if (!page || !page->compactSnapshot
				|| glyph.snapshotPlacementIndex
					>= page->compactSnapshot->placements.size())
			{
				return fail("placement-remap-source");
			}
			const AtlasSnapshotPlacement& placement =
				page->compactSnapshot->placements[
					glyph.snapshotPlacementIndex];
			if (placement.maskType != static_cast<UInt8>(maskType)
				|| !IsValidAtlasSnapshotGlyphPlacement(placement,
					page->width, page->height,
					page->compactSnapshot->sourceHeader.pageIndex))
			{
				return fail("placement-remap-target");
			}
			glyph.atlasPage = ordinal;
			glyph.placement = &placement;
		}

		RecordFreeTypePerf(FreeTypePerfCounter::GpuResidentGlyphHit,
			static_cast<UInt64>(glyphs.size()));
		const UInt64 logKey = 0x8000000000000000ull
			^ (static_cast<UInt64>(config.fontId) << 32)
			^ (static_cast<UInt64>(std::lround(
				rasterScale * 1000.0f)) << 8)
			^ (static_cast<UInt64>(pixelMode) << 4)
			^ static_cast<UInt64>(renderMode);
		if (!result.atlases.empty()
			&& state.loggedDirectGlyphBatches.insert(logKey).second)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: direct atlas geometry batch font=%u sourceScale=%.3f mode=%u render=%u glyphs=%u pages=%u source=dense-cached-letter",
				config.fontId, rasterScale,
				static_cast<UInt32>(pixelMode),
				static_cast<UInt32>(renderMode),
				static_cast<UInt32>(glyphs.size()),
				static_cast<UInt32>(result.atlases.size()));
		}
		return true;
	}

	bool GetDirectAtlasGlyphSources(RuntimeFont& runtime,
		const std::vector<GlyphBitmapRequest>& requests, float rasterScale,
		AtlasPixelMode pixelMode, AtlasRenderMode renderMode, UInt32 padding,
		std::vector<PendingQuad::GlyphSource>& results)
	{
		results.assign(requests.size(), {});
		if (requests.empty())
			return true;

		const FontConfig& config = GetRuntimeConfig(runtime);
		std::array<AtlasCacheKey, 2> baseKeys;
		std::array<std::shared_ptr<const DirectAtlasGlyphTable>, 2> tables;
		AtlasState& state = State();
		std::lock_guard<std::mutex> lock(state.atlasMutex);
		for (size_t roleIndex = 0; roleIndex < baseKeys.size(); ++roleIndex)
		{
			const VectorFontByteClass byteClass =
				static_cast<VectorFontByteClass>(roleIndex);
			if (!ResolvePrewarmAtlasKey(config, byteClass,
				rasterScale, baseKeys[roleIndex])
				|| baseKeys[roleIndex].pixelMode != pixelMode
				|| baseKeys[roleIndex].renderMode != renderMode
				|| baseKeys[roleIndex].padding != padding)
			{
				return false;
			}
			const auto profile = state.atlasProfiles.find(
				MakeAtlasProfileKey(baseKeys[roleIndex]));
			if (profile != state.atlasProfiles.end()
				&& state.completeAtlasProfiles.find(
					MakeAtlasProfileKey(baseKeys[roleIndex]))
					!= state.completeAtlasProfiles.end())
				tables[roleIndex] = profile->second.directGlyphs;
		}

		for (size_t requestIndex = 0;
			requestIndex < requests.size(); ++requestIndex)
		{
			const GlyphBitmapRequest& request = requests[requestIndex];
			if (!request.glyph)
				return false;
			const size_t roleIndex =
				static_cast<size_t>(request.glyph->byteClass);
			if (roleIndex >= tables.size() || !tables[roleIndex])
				return false;
			const std::shared_ptr<const DirectAtlasGlyphTable>& table =
				tables[roleIndex];
			size_t glyphSlot = 0;
			if (!ResolveDirectGlyphSlot(request.glyph->byteClass,
				request.glyph->encodedCode, glyphSlot)
				|| glyphSlot >= table->glyphs.size())
			{
				return false;
			}
			if (static_cast<size_t>(request.maskType)
				>= kDirectAtlasMaskCount)
				return false;
			const DirectCachedLetter& letter =
				table->glyphs[glyphSlot];
			if (!(letter.flags & kDirectCachedLetterValid)
				|| letter.encodedCode != request.glyph->encodedCode
				|| letter.byteClass
					!= static_cast<UInt8>(request.glyph->byteClass))
			{
				return false;
			}
			if (letter.flags & kDirectCachedLetterKnownEmpty)
			{
				if (!IsSpaceCodePoint(request.glyph->codePoint))
					return false;
				results[requestIndex].knownEmpty = true;
				continue;
			}
			if (!ResolveDirectGlyphSource(*table, *request.glyph,
				request.maskType, results[requestIndex]))
				return false;
		}
		RecordFreeTypePerf(FreeTypePerfCounter::GpuResidentGlyphHit,
			static_cast<UInt64>(requests.size()));
		const UInt64 logKey =
			(static_cast<UInt64>(config.fontId) << 32)
			| (static_cast<UInt64>(std::lround(rasterScale * 1000.0f))
				<< 8)
			| (static_cast<UInt64>(pixelMode) << 4)
			| static_cast<UInt64>(renderMode);
		if (state.loggedDirectGlyphBatches.insert(logKey).second)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: direct atlas glyph batch font=%u sourceScale=%.3f mode=%u render=%u glyphs=%u source=immutable-snapshot-placement",
				config.fontId, rasterScale,
				static_cast<UInt32>(pixelMode),
				static_cast<UInt32>(renderMode),
				static_cast<UInt32>(requests.size()));
		}
		return true;
	}

	bool GetDirectAtlasGlyphSources(RuntimeFont& runtime,
		const std::vector<AtlasGlyphInstance>& glyphs,
		GlyphMaskType maskType, float rasterScale,
		AtlasPixelMode pixelMode, AtlasRenderMode renderMode,
		UInt32 padding, std::vector<PendingQuad::GlyphSource>& results)
	{
		results.assign(glyphs.size(), {});
		if (glyphs.empty())
			return true;
		if (static_cast<size_t>(maskType) >= kDirectAtlasMaskCount)
			return false;

		const FontConfig& config = GetRuntimeConfig(runtime);
		std::array<AtlasCacheKey, 2> baseKeys;
		std::array<std::shared_ptr<const DirectAtlasGlyphTable>, 2> tables;
		AtlasState& state = State();
		std::lock_guard<std::mutex> lock(state.atlasMutex);
		for (size_t roleIndex = 0; roleIndex < baseKeys.size();
			++roleIndex)
		{
			const VectorFontByteClass byteClass =
				static_cast<VectorFontByteClass>(roleIndex);
			if (!ResolvePrewarmAtlasKey(config, byteClass,
				rasterScale, baseKeys[roleIndex])
				|| baseKeys[roleIndex].pixelMode != pixelMode
				|| baseKeys[roleIndex].renderMode != renderMode
				|| baseKeys[roleIndex].padding != padding)
			{
				return false;
			}
			const auto profile = state.atlasProfiles.find(
				MakeAtlasProfileKey(baseKeys[roleIndex]));
			if (profile != state.atlasProfiles.end()
				&& state.completeAtlasProfiles.find(
					MakeAtlasProfileKey(baseKeys[roleIndex]))
					!= state.completeAtlasProfiles.end())
				tables[roleIndex] = profile->second.directGlyphs;
		}

		for (size_t glyphIndex = 0; glyphIndex < glyphs.size();
			++glyphIndex)
		{
			const VectorEncodedGlyph& glyph =
				glyphs[glyphIndex].glyph;
			const size_t roleIndex =
				static_cast<size_t>(glyph.byteClass);
			if (roleIndex >= tables.size() || !tables[roleIndex])
				return false;
			size_t glyphSlot = 0;
			if (!ResolveDirectGlyphSlot(glyph.byteClass,
				glyph.encodedCode, glyphSlot)
				|| glyphSlot >= tables[roleIndex]->glyphs.size())
				return false;
			const DirectCachedLetter& letter =
				tables[roleIndex]->glyphs[glyphSlot];
			if ((letter.flags & kDirectCachedLetterKnownEmpty)
				&& (letter.flags & kDirectCachedLetterValid))
			{
				if (!IsSpaceCodePoint(glyph.codePoint))
					return false;
				results[glyphIndex].knownEmpty = true;
				continue;
			}
			if (ResolveDirectGlyphSource(*tables[roleIndex], glyph,
				maskType, results[glyphIndex]))
				continue;
			return false;
		}
		RecordFreeTypePerf(FreeTypePerfCounter::GpuResidentGlyphHit,
			static_cast<UInt64>(glyphs.size()));
		const UInt64 logKey =
			(static_cast<UInt64>(config.fontId) << 32)
			| (static_cast<UInt64>(std::lround(
				rasterScale * 1000.0f)) << 8)
			| (static_cast<UInt64>(pixelMode) << 4)
			| static_cast<UInt64>(renderMode);
		if (state.loggedDirectGlyphBatches.insert(logKey).second)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: direct atlas glyph batch font=%u sourceScale=%.3f mode=%u render=%u glyphs=%u source=persistent-direct-cached-letter",
				config.fontId, rasterScale,
				static_cast<UInt32>(pixelMode),
				static_cast<UInt32>(renderMode),
				static_cast<UInt32>(glyphs.size()));
		}
		return true;
	}
}

#include "font_atlas_snapshot_internal.h"

#define STBRP_STATIC
#define STB_RECT_PACK_IMPLEMENTATION
#include "third_party/stb/stb_rect_pack.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fonthook::vectorfont
{
	namespace implementation::font_atlas_snapshot
	{
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
			VectorFontByteClass packingByteClass,
			size_t maximumAcceptedPages,
			bool emitDiagnostics)
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
				GetSnapshotMaximumSize(packingCaps, packingByteClass,
					baseKey.pixelMode);
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
					size_t pageLimit = maximumAcceptedPages;
					if (!selectedPlan.pages.empty())
					{
						pageLimit = pageLimit
							? std::min(pageLimit, selectedPlan.pages.size())
							: selectedPlan.pages.size();
					}
					if (BuildSnapshotPackingPlan(glyphs, padding,
						maximumSize, baseKey.pixelMode,
						baseKey.levelZeroOnly, packingCaps,
						targetWidth, heuristic,
						pageLimit, candidate)
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
			if (emitDiagnostics)
			{
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
			}
			if (emitDiagnostics && preferSingleAtlas && pages.size() > 1)
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
	}
}

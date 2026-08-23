#include "font_atlas_shape_detail.h"

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
#include <memory>

namespace fonthook::vectorfont
{
		NiColorA UnpackNativeBaseColor(UInt32 color)
		{
			constexpr float inverse = 1.0f / 255.0f;
			return {
				static_cast<float>((color >> 16) & 0xFFu) * inverse,
				static_cast<float>((color >> 8) & 0xFFu) * inverse,
				static_cast<float>(color & 0xFFu) * inverse,
				static_cast<float>((color >> 24) & 0xFFu) * inverse
			};
		}

		const NiPoint3& GetDirectGlyphPen(
			const AtlasGlyphInstance& glyph)
		{
			return glyph.pen;
		}

		const NiPoint3& GetDirectGlyphPen(
			const DirectGlyphCommand& glyph)
		{
			return glyph.pen;
		}

		NiColorA GetDirectGlyphSourceColor(
			const AtlasGlyphInstance& glyph)
		{
			return glyph.color;
		}

		NiColorA GetDirectGlyphSourceColor(
			const DirectGlyphCommand& glyph)
		{
			return glyph.sourceColor;
		}

		VectorFontByteClass GetDirectGlyphByteClass(
			const AtlasGlyphInstance& instance)
		{
			return instance.glyph.byteClass;
		}

		VectorFontByteClass GetDirectGlyphByteClass(
			const DirectGlyphCommand& glyph)
		{
			return static_cast<VectorFontByteClass>(glyph.byteClass);
		}

		float GetDirectGlyphBaselineOffset(RuntimeFont& runtime,
			const DirectAtlasGlyphBatch&,
			const AtlasGlyphInstance& instance)
		{
			return GetGlyphBaselineOffset(runtime, instance.glyph);
		}

		float GetDirectGlyphBaselineOffset(RuntimeFont&,
			const DirectAtlasGlyphBatch& batch,
			const DirectGlyphCommand& glyph)
		{
			if (!batch.sealed)
				return 0.0f;
			const size_t roleIndex = glyph.byteClass;
			if (roleIndex >= batch.sealed->tables.size()
				|| !batch.sealed->tables[roleIndex])
			{
				return 0.0f;
			}
			const DirectAtlasGlyphTable& table =
				*batch.sealed->tables[roleIndex];
			if (glyph.directSlot >= table.faceIndices.size())
				return batch.sealed->roleBaselineOffsets[roleIndex];
			const UInt8 faceIndex =
				table.faceIndices[glyph.directSlot];
			const auto& offsets =
				batch.sealed->faceBaselineOffsets[roleIndex];
			return faceIndex < offsets.size()
				? offsets[faceIndex]
				: batch.sealed->roleBaselineOffsets[roleIndex];
		}

	DirectAtlasShapeBuildResult TryCreateSealedCpuEffectShape(
		Font& font, RuntimeFont& runtime,
		const std::shared_ptr<const SealedDirectFontProfile>& sealed,
		const std::vector<DirectGlyphCommand>& glyphs, float rasterScale,
		bool prepareObject, const NiColorA& tileColor,
		bool suppressEffects)
	{
		DirectAtlasShapeBuildResult result;
		if (!sealed)
		{
			result.outcome = DirectAtlasShapeOutcome::Failed;
			return result;
		}
		if (glyphs.empty())
		{
			result.outcome = DirectAtlasShapeOutcome::Empty;
			return result;
		}
		const std::shared_ptr<const SealedDirectFontProfile> published =
			LoadRuntimeSealedDirectProfile(runtime);
		if (published.get() != sealed.get()
			|| !IsSealedDirectFontProfileUsable(runtime, sealed, rasterScale)
			|| sealed->recordKind
				!= DirectCachedLetterKind::EffectLayers
			|| sealed->renderMode != AtlasRenderMode::CpuEffects
			|| sealed->pixelMode != AtlasPixelMode::A8
			|| sealed->padding != kDistanceFieldAtlasPadding
			|| sealed->atlases.empty()
			|| sealed->atlases.size() > kMaximumAtlasSnapshotPages)
		{
			result.outcome = DirectAtlasShapeOutcome::Failed;
			return result;
		}

		const FontConfig& config = GetRuntimeConfig(runtime);
		const std::array<GlyphMaskType, 4> masks = {{
			GlyphMaskType::Shadow,
			GlyphMaskType::Glow,
			GlyphMaskType::Outline,
			GlyphMaskType::Fill
		}};
		const std::array<bool, 4> enabled = {{
			!suppressEffects && config.shadow.enabled,
			!suppressEffects && config.glow.enabled,
			!suppressEffects && config.outline.enabled,
			true
		}};
		const std::array<float, 4> offsetsX = {{
			config.shadow.x, 0.0f, 0.0f, 0.0f
		}};
		const std::array<float, 4> offsetsY = {{
			config.shadow.y, 0.0f, 0.0f, 0.0f
		}};
		const std::array<NiColorA, 4> layerColors = {{
			ResolveEffectLayerColor(config.shadow, config.fontColor),
			ResolveEffectLayerColor(config.glow, config.fontColor),
			ResolveEffectLayerColor(config.outline, config.fontColor),
			ResolveFillLayerColor(config.fontColor)
		}};
		const std::array<bool, 4> usesLiveTileRgb = {{
			EffectUsesLiveTileRgb(config.shadow),
			EffectUsesLiveTileRgb(config.glow),
			EffectUsesLiveTileRgb(config.outline),
			true
		}};
		for (size_t layer = 0; layer < enabled.size(); ++layer)
		{
			const UInt8 maskBit = static_cast<UInt8>(
				1u << static_cast<UInt8>(masks[layer]));
			if (enabled[layer]
				&& !(sealed->effectLayerMask & maskBit))
			{
				result.outcome = DirectAtlasShapeOutcome::Failed;
				return result;
			}
		}

		struct CommonCommandResolution
		{
			const DirectCachedLetter* letter = nullptr;
			size_t roleIndex = 0;
			float baselineOffset = 0.0f;
			bool knownEmpty = false;
		};

		auto resolveCommand = [&](const DirectGlyphCommand& command,
			CommonCommandResolution& resolution)
		{
			resolution = {};
			const size_t roleIndex = command.byteClass;
			if (roleIndex >= sealed->tables.size()
				|| !sealed->tables[roleIndex]
				|| !command.byteLength)
			{
				return false;
			}
			const DirectAtlasGlyphTable& table =
				*sealed->tables[roleIndex];
			if (table.recordKind
					!= DirectCachedLetterKind::EffectLayers
				|| command.directSlot >= table.glyphs.size()
				|| command.directSlot >= table.faceIndices.size())
			{
				return false;
			}
			const DirectCachedLetter& letter =
				table.glyphs[command.directSlot];
			if ((letter.flags & ~kDirectCachedLetterKnownFlags)
				|| !(letter.flags & kDirectCachedLetterValid)
				|| letter.encodedCode != command.encodedCode
				|| letter.byteClass != command.byteClass)
			{
				return false;
			}
			resolution.letter = &letter;
			resolution.roleIndex = roleIndex;
			if (letter.flags & kDirectCachedLetterKnownEmpty)
			{
				resolution.knownEmpty = true;
				return true;
			}
			const UInt8 faceIndex =
				table.faceIndices[command.directSlot];
			const auto& baselines =
				sealed->faceBaselineOffsets[roleIndex];
			resolution.baselineOffset = faceIndex < baselines.size()
				? baselines[faceIndex]
				: sealed->roleBaselineOffsets[roleIndex];
			return true;
		};

		struct LayerPageResolution
		{
			const DirectAtlasGlyphLayer* direct = nullptr;
			const AtlasResource* atlas = nullptr;
			UInt16 pageOrdinal = kInvalidDirectAtlasPageSlot;
			UInt8 maskType = 0;
		};

		auto resolveLayerPage = [&](const CommonCommandResolution& resolution,
			size_t layer, LayerPageResolution& layerResolution)
		{
			layerResolution = {};
			if (!resolution.letter || resolution.knownEmpty
				|| layer >= masks.size())
			{
				return false;
			}
			layerResolution.maskType =
				static_cast<UInt8>(masks[layer]);
			const DirectAtlasGlyphLayer* direct = nullptr;
			for (const DirectAtlasGlyphLayer& candidate :
				resolution.letter->layers)
			{
				if (candidate.valid()
					&& candidate.maskType == layerResolution.maskType)
				{
					direct = &candidate;
					break;
				}
			}
			if (!direct || direct->reserved
				|| direct->pageSlot >= kMaximumAtlasSnapshotPages)
			{
				return false;
			}
			layerResolution.pageOrdinal =
				sealed->pageOrdinals[resolution.roleIndex][direct->pageSlot];
			if (layerResolution.pageOrdinal >= sealed->atlases.size()
				|| layerResolution.pageOrdinal >= kMaximumAtlasSnapshotPages)
			{
				return false;
			}
			const std::shared_ptr<AtlasResource>& atlas =
				sealed->atlases[layerResolution.pageOrdinal];
			if (!atlas)
			{
				return false;
			}
			layerResolution.direct = direct;
			layerResolution.atlas = atlas.get();
			return true;
		};

		auto resolveLayerPlacement = [&](const LayerPageResolution& layer,
			const AtlasSnapshotPlacement*& placement)
		{
			placement = nullptr;
			if (!layer.direct || !layer.atlas
				|| !layer.atlas->compactSnapshot
				|| !layer.atlas->pageContentHash
				|| layer.atlas->compactSnapshot->sourceHeader.pageContentHash
					!= layer.atlas->pageContentHash
				|| layer.direct->snapshotPlacementIndex
					>= layer.atlas->compactSnapshot->placements.size())
			{
				return false;
			}
			const AtlasSnapshotPlacement& source =
				layer.atlas->compactSnapshot->placements[
					layer.direct->snapshotPlacementIndex];
			if (!source.cacheId || source.maskType != layer.maskType
				|| !source.rect.width || !source.rect.height)
			{
				return false;
			}
			placement = &source;
			return true;
		};

		UInt64 rangeInitializationBytesAvoided = 0;
		std::array<std::array<UInt32,
			kMaximumAtlasSnapshotPages>, 4> counts;
		if (!InitializeDirectQuadCountPrefix(counts,
			sealed->atlases.size(), rangeInitializationBytesAvoided))
		{
			result.outcome = DirectAtlasShapeOutcome::Failed;
			return result;
		}
		NiPoint3 origin;
		bool originInitialized = false;
		for (const DirectGlyphCommand& command : glyphs)
		{
			CommonCommandResolution resolution;
			if (!resolveCommand(command, resolution))
			{
				result.outcome = DirectAtlasShapeOutcome::Failed;
				return result;
			}
			if (resolution.knownEmpty)
				continue;
			for (size_t layer = 0; layer < enabled.size(); ++layer)
			{
				if (!enabled[layer])
					continue;
				LayerPageResolution layerResolution;
				if (!resolveLayerPage(
					resolution, layer, layerResolution))
				{
					result.outcome =
						DirectAtlasShapeOutcome::Failed;
					return result;
				}
				if (layerResolution.pageOrdinal >= sealed->atlases.size())
				{
					result.outcome =
						DirectAtlasShapeOutcome::Failed;
					return result;
				}
				++counts[layer][layerResolution.pageOrdinal];
				if (layer
					== static_cast<size_t>(AtlasLayer::Fill))
				{
					++result.glyphCount;
					if (!originInitialized)
					{
						origin = command.pen;
						originInitialized = true;
					}
				}
			}
		}
		if (!result.glyphCount)
		{
			result.outcome = DirectAtlasShapeOutcome::Empty;
			return result;
		}

		std::array<std::array<UInt32,
			kMaximumAtlasSnapshotPages>, 4> offsets;
		std::array<std::array<UInt32,
			kMaximumAtlasSnapshotPages>, 4> cursors;
		// Every live layer/page entry is assigned below before it can be read;
		// the final cursor proof rejects any count/fill divergence.
		rangeInitializationBytesAvoided += sizeof(offsets) + sizeof(cursors);
		UInt32 quadCount = 0;
		for (size_t layer = 0; layer < counts.size(); ++layer)
		{
			for (size_t page = 0;
				page < sealed->atlases.size(); ++page)
			{
				offsets[layer][page] = quadCount;
				cursors[layer][page] = quadCount;
				if (counts[layer][page]
					> kNativeFontMaximumArtifactQuads - quadCount)
				{
					result.outcome =
						DirectAtlasShapeOutcome::Failed;
					return result;
				}
				quadCount += counts[layer][page];
			}
		}
		if (!quadCount)
		{
			result.outcome = DirectAtlasShapeOutcome::Failed;
			return result;
		}

		std::vector<NativeFontGpuVertex> vertices(
			static_cast<size_t>(quadCount) * 4u);
		RecordFreeTypePerf(
			FreeTypePerfCounter::DirectShapeVertexInitializationBytesAvoided,
			vertices.size() * sizeof(NativeFontGpuVertex));
		NiPoint3 boundMinimum(std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max());
		NiPoint3 boundMaximum(std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest());
		NativeFontShapeColorContract colorContract;
		bool colorContractInitialized = false;
		NiColorA facadeColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		std::array<NiColorA, 4> sanitizedLayerColors;
		for (size_t layer = 0; layer < enabled.size(); ++layer)
		{
			if (enabled[layer])
				sanitizedLayerColors[layer] =
					SanitizeColor(layerColors[layer]);
		}
		for (const DirectGlyphCommand& command : glyphs)
		{
			CommonCommandResolution resolution;
			if (!resolveCommand(command, resolution))
			{
				result.outcome = DirectAtlasShapeOutcome::Failed;
				return result;
			}
			if (resolution.knownEmpty)
				continue;
			const NiColorA baseColor = ResolveBaseColor(
				command.sourceColor, tileColor);
			for (size_t layer = 0; layer < enabled.size(); ++layer)
			{
				if (!enabled[layer])
					continue;
				LayerPageResolution layerResolution;
				if (!resolveLayerPage(
					resolution, layer, layerResolution))
				{
					result.outcome =
						DirectAtlasShapeOutcome::Failed;
					return result;
				}
				const AtlasSnapshotPlacement* placement = nullptr;
				if (!resolveLayerPlacement(layerResolution, placement))
				{
					result.outcome =
						DirectAtlasShapeOutcome::Failed;
					return result;
				}
				if (!placement
					|| layerResolution.pageOrdinal >= sealed->atlases.size())
				{
					result.outcome =
						DirectAtlasShapeOutcome::Failed;
					return result;
				}
				const UInt32 quadIndex =
					cursors[layer][layerResolution.pageOrdinal]++;
				if (quadIndex >= quadCount)
				{
					result.outcome =
						DirectAtlasShapeOutcome::Failed;
					return result;
				}
				const NiColorA& layerColor = sanitizedLayerColors[layer];
				const NiColorA bakedColor = usesLiveTileRgb[layer]
					? NiColorA{
						baseColor.r * layerColor.r,
						baseColor.g * layerColor.g,
						baseColor.b * layerColor.b,
						baseColor.a * layerColor.a }
					: NiColorA{
						layerColor.r, layerColor.g, layerColor.b,
						baseColor.a * layerColor.a };
				if (quadIndex == 0)
					facadeColor = bakedColor;
				ExtendColorContract(colorContract,
					colorContractInitialized, bakedColor);
				if (!WriteDirectQuadVertices(*placement,
					command.pen, origin, offsetsX[layer],
					offsetsY[layer], rasterScale,
					resolution.baselineOffset,
					1.0f, false,
					PackNativeBaseColor(bakedColor),
					static_cast<UInt8>(
						usesLiveTileRgb[layer] ? 1u : 0u),
					&vertices[quadIndex * 4u],
					boundMinimum, boundMaximum))
				{
					result.outcome =
						DirectAtlasShapeOutcome::Failed;
					return result;
				}
			}
		}
		if (!DirectQuadRangesCompletelyWritten(
			counts, offsets, cursors, sealed->atlases.size(), quadCount))
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::DirectShapeVertexCoverageFailure);
			result.outcome = DirectAtlasShapeOutcome::Failed;
			return result;
		}
		RecordFreeTypePerf(
			FreeTypePerfCounter::DirectShapeRangeInitializationBytesAvoided,
			rangeInitializationBytesAvoided);
		const UInt64 enabledLayerCount = static_cast<UInt64>(
			std::count(enabled.begin(), enabled.end(), true));
		if (enabledLayerCount > 1)
		{
			RecordFreeTypePerf(
				FreeTypePerfCounter::DirectShapeCommonResolutionsSaved,
				static_cast<UInt64>(glyphs.size()) * 2u
					* (enabledLayerCount - 1u));
		}
		RecordFreeTypePerf(
			FreeTypePerfCounter::DirectShapeSnapshotResolutionsSaved,
			quadCount);
		if (!colorContractInitialized)
		{
			result.outcome = DirectAtlasShapeOutcome::Failed;
			return result;
		}

		NativeFontEffectShapeConfig effects;
		effects.enabled = true;
		effects.bakedCoverage = true;
		effects.quality = config.effectQuality;
		for (size_t layer = 0; layer < counts.size(); ++layer)
		{
			if (!enabled[layer])
				continue;
			for (UInt16 page = 0;
				page < sealed->atlases.size(); ++page)
			{
				const UInt32 count = counts[layer][page];
				if (!count)
					continue;
				NativeFontDrawRange range;
				range.firstVertex = offsets[layer][page] * 4u;
				range.vertexCount = count * 4u;
				range.startIndex = offsets[layer][page] * 6u;
				range.primitiveCount = count * 2u;
				range.layer = static_cast<UInt32>(layer);
				range.atlasPage = page;
				range.usesSdf = false;
				range.usesLiveTileRgb =
					usesLiveTileRgb[layer];
				range.layerColorModifier =
					{ 1.0f, 1.0f, 1.0f, 1.0f };
				effects.ranges.push_back(range);
			}
		}
		result.geometryQuadCount = quadCount;
		result.drawQuadCount = quadCount;
		result.pageCount = CountUsedDirectAtlasPages(
			counts, sealed->atlases.size());
		if (!sealed->atlases.empty() && sealed->atlases[0])
		{
			result.firstAtlasWidth = sealed->atlases[0]->width;
			result.firstAtlasHeight = sealed->atlases[0]->height;
		}
		result.shape = CreateDirectNativeShape(font,
			sealed->atlases, std::move(vertices),
			result.glyphCount, quadCount, effects,
			colorContract, facadeColor, tileColor, origin,
			boundMinimum, boundMaximum, prepareObject,
			result.failureStage);
		result.outcome = result.shape
			? DirectAtlasShapeOutcome::Created
			: DirectAtlasShapeOutcome::Failed;
		return result;
	}
}

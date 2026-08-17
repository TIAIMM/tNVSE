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
		template <class GlyphInstance>
		NiTriShape* CreateDirectArgbPageShape(Font& font,
			RuntimeFont& runtime,
			const std::vector<GlyphInstance>& glyphs,
			const DirectAtlasGlyphBatch& batch, float rasterScale,
			const NiColorA& tileColor, bool prepareObject,
			UInt16 atlasPage, UInt32 drawableGlyphs)
		{
			const auto& atlases = batch.Atlases();
			if (!drawableGlyphs || atlasPage >= atlases.size()
				|| batch.glyphs.size() != glyphs.size())
			{
				return nullptr;
			}
			NiTriShape* shape = CreateFreeTypeTextShape(drawableGlyphs,
				tileColor, false, atlases[atlasPage]->property,
				GetAtlasTexture(*atlases[atlasPage]));
			NiTriShapeOwner shapeOwner(shape);
			if (!shape || !shape->GetModelData())
			{
				return nullptr;
			}
			NiTriShapeData* data = shape->GetModelData();
			const UInt32 vertexCount = drawableGlyphs * 4u;
			if (data->m_usVertices < vertexCount || !data->m_pkVertex
				|| !data->m_pkTexture || !data->m_pusTriList)
			{
				return nullptr;
			}
			if (!data->m_pkColor)
				data->m_pkColor = NiAlloc<NiColorA>(data->m_usVertices);
			if (!data->m_pkColor)
				return nullptr;

			size_t firstDrawable = 0;
			while (firstDrawable < batch.glyphs.size()
				&& (batch.glyphs[firstDrawable].knownEmpty
					|| batch.glyphs[firstDrawable].atlasPage != atlasPage))
			{
				++firstDrawable;
			}
			if (firstDrawable >= glyphs.size())
				return nullptr;
			const NiPoint3 origin =
				GetDirectGlyphPen(glyphs[firstDrawable]);
			NiPoint3 boundMinimum(std::numeric_limits<float>::max(),
				std::numeric_limits<float>::max(),
				std::numeric_limits<float>::max());
			NiPoint3 boundMaximum(std::numeric_limits<float>::lowest(),
				std::numeric_limits<float>::lowest(),
				std::numeric_limits<float>::lowest());
			static constexpr UInt16 kCanonicalQuad[6] =
				{ 0, 2, 1, 0, 3, 2 };
			UInt32 outputQuad = 0;
			for (size_t glyphIndex = 0;
				glyphIndex < glyphs.size(); ++glyphIndex)
			{
				const DirectAtlasBatchGlyph& source =
					batch.glyphs[glyphIndex];
				if (source.knownEmpty || source.atlasPage != atlasPage)
					continue;
				if ((!source.placement && !source.vanillaLetter)
					|| source.atlasPage != atlasPage)
					return nullptr;
				const GlyphInstance& instance = glyphs[glyphIndex];
				const NiColorA baseColor =
					ResolveBaseColor(
						GetDirectGlyphSourceColor(instance),
						tileColor);
				const NiPoint3& pen =
					GetDirectGlyphPen(instance);
				const float baselineOffset =
					GetDirectGlyphBaselineOffset(
						runtime, batch, instance);
				if (outputQuad >= drawableGlyphs)
					return nullptr;
				NiPoint3* outputPositions =
					&data->m_pkVertex[outputQuad * 4u];
				NiPoint2* outputTexture =
					&data->m_pkTexture[outputQuad * 4u];
				const bool written = source.vanillaLetter
					? WriteVanillaDirectQuadGeometry(
						*source.vanillaLetter, pen, origin,
						baselineOffset, outputPositions,
						outputTexture, boundMinimum, boundMaximum)
					: WriteDirectQuadGeometry(*source.placement,
						pen, origin, 0.0f, 0.0f,
						rasterScale, baselineOffset,
						1.0f, false, outputPositions,
						outputTexture, boundMinimum,
						boundMaximum);
				if (!written)
				{
					return nullptr;
				}
				const NiColorA safeBaseColor = SanitizeColor(baseColor);
				for (UInt32 ordinal = 0; ordinal < 4; ++ordinal)
				{
					const UInt32 output = outputQuad * 4u + ordinal;
					data->m_pkVertex[output].x += origin.x;
					data->m_pkVertex[output].y += origin.y;
					data->m_pkVertex[output].z += origin.z;
					data->m_pkColor[output] = safeBaseColor;
				}
				for (UInt32 ordinal = 0; ordinal < 6; ++ordinal)
				{
					data->m_pusTriList[outputQuad * 6u + ordinal] =
						static_cast<UInt16>(outputQuad * 4u
							+ kCanonicalQuad[ordinal]);
				}
				++outputQuad;
			}
			if (outputQuad != drawableGlyphs)
				return nullptr;
			RecordFreeTypePerf(
				FreeTypePerfCounter::DirectArgbTransientVertexBytesAvoided,
				static_cast<UInt64>(outputQuad) * 4u
					* sizeof(NativeFontGpuVertex));
			NiBound bound;
			if (!BuildDirectVertexBound(
				static_cast<size_t>(outputQuad) * 4u,
				boundMinimum, boundMaximum, bound))
			{
				return nullptr;
			}
			data->m_kBound = bound;
			data->m_kBound.m_kCenter.x += origin.x;
			data->m_kBound.m_kCenter.y += origin.y;
			data->m_kBound.m_kCenter.z += origin.z;
			if (prepareObject)
				shape->PrepareObject();
			data->m_kBound = bound;
			data->m_kBound.m_kCenter.x += origin.x;
			data->m_kBound.m_kCenter.y += origin.y;
			data->m_kBound.m_kCenter.z += origin.z;
			if (prepareObject && shape->m_pWorldBound)
				shape->UpdateWorldBound();
			return shapeOwner.release();
		}

		template <class GlyphInstance>
		DirectAtlasShapeBuildResult CreateDirectCachedLetterShapeFromBatch(
			Font& font, RuntimeFont& runtime,
			const std::vector<GlyphInstance>& glyphs,
			const DirectAtlasGlyphBatch& batch, float rasterScale,
			bool prepareObject, const NiColorA& tileColor,
			bool suppressEffects, GlyphMaskType maskType,
			EffectQuality quality)
		{
			DirectAtlasShapeBuildResult result;
			const bool precomposed =
				maskType == GlyphMaskType::Composite;
			const bool distanceField =
				maskType == GlyphMaskType::DistanceField;
			if (!precomposed && !distanceField)
				return result;

			const auto& atlases = batch.Atlases();
			if (!atlases.empty() && atlases[0])
			{
				result.firstAtlasWidth = atlases[0]->width;
				result.firstAtlasHeight = atlases[0]->height;
			}
			size_t firstDrawable = 0;
			while (firstDrawable < batch.glyphs.size()
				&& batch.glyphs[firstDrawable].knownEmpty)
			{
				++firstDrawable;
			}
			if (firstDrawable >= batch.glyphs.size())
			{
				result.outcome = DirectAtlasShapeOutcome::Empty;
				return result;
			}
			if (atlases.empty()
				|| atlases.size() > kMaximumAtlasSnapshotPages
				|| batch.glyphs.size() != glyphs.size())
			{
				result.outcome = DirectAtlasShapeOutcome::Failed;
				return result;
			}
			const size_t drawableGlyphCount = static_cast<size_t>(std::count_if(
				batch.glyphs.begin(), batch.glyphs.end(),
				[](const DirectAtlasBatchGlyph& glyph)
				{
					return !glyph.knownEmpty
						&& (glyph.placement || glyph.vanillaLetter);
				}));
			if (!drawableGlyphCount
				|| drawableGlyphCount > kNativeFontMaximumArtifactQuads)
			{
				result.outcome = DirectAtlasShapeOutcome::Failed;
				return result;
			}
			result.glyphCount = static_cast<UInt32>(drawableGlyphCount);

			UInt64 rangeInitializationBytesAvoided = 0;
			if (precomposed)
			{
				std::array<UInt32, kMaximumAtlasSnapshotPages>
					pageGlyphCounts;
				if (!InitializeDirectPagePrefix(pageGlyphCounts,
					atlases.size(), UInt32{ 0 },
					rangeInitializationBytesAvoided))
				{
					result.outcome = DirectAtlasShapeOutcome::Failed;
					return result;
				}
				UInt32 usedPageCount = 0;
				for (const DirectAtlasBatchGlyph& glyph : batch.glyphs)
				{
					if (glyph.knownEmpty)
						continue;
					if ((!glyph.placement && !glyph.vanillaLetter)
						|| glyph.atlasPage >= atlases.size()
						|| glyph.atlasPage >= pageGlyphCounts.size())
					{
						result.outcome =
							DirectAtlasShapeOutcome::Failed;
						return result;
					}
					if (pageGlyphCounts[glyph.atlasPage]++ == 0)
						++usedPageCount;
				}

				// A vanilla NiTriShape has one texturing property. Keep that route
				// only when the complete aggressive artifact fits on one page.
				// Multi-page ARGB falls through to the native packet payload so
				// the accumulator still receives exactly one facade.
				if (usedPageCount == 1
					&& result.glyphCount <= kMaximumQuads)
				{
					NiTriShape* pageShape = nullptr;
					for (UInt16 page = 0; page < atlases.size(); ++page)
					{
						const UInt32 pageGlyphCount =
							pageGlyphCounts[page];
						if (!pageGlyphCount)
							continue;
						pageShape =
							CreateDirectArgbPageShape(
								font, runtime, glyphs, batch,
								rasterScale, tileColor,
								prepareObject, page,
								pageGlyphCount);
						if (!pageShape)
						{
							result.outcome =
								DirectAtlasShapeOutcome::Failed;
							return result;
						}
						break;
					}
					if (!pageShape)
					{
						result.outcome =
							DirectAtlasShapeOutcome::Failed;
						return result;
					}

					result.shape = pageShape;
					result.geometryQuadCount = result.glyphCount;
					result.drawQuadCount = result.glyphCount;
					result.pageCount = usedPageCount;
					result.outcome =
						DirectAtlasShapeOutcome::Created;
					RecordFreeTypePerf(
						FreeTypePerfCounter::
							DirectShapeRangeInitializationBytesAvoided,
						rangeInitializationBytesAvoided);
					return result;
				}
			}

			const FontConfig& config = GetRuntimeConfig(runtime);
			NativeFontEffectShapeConfig effects;
			std::array<DistanceFieldRasterProfile, 2> rasterProfiles;
			std::array<bool, 2> rasterProfileReady = {};
			bool drawShadow = false;
			bool shadowHasOffset = false;
			UInt8 bodyLayerMask =
				1u << static_cast<UInt8>(AtlasLayer::Fill);
			if (distanceField)
			{
				ShaderEffectBuild shaderBuild;
				if (!ConfigureShaderEffectBuild(config, rasterScale,
					quality, suppressEffects, shaderBuild))
				{
					return result;
				}
				effects = std::move(shaderBuild.config);
				drawShadow =
					!suppressEffects && config.shadow.enabled;
				const bool drawGlow =
					!suppressEffects && config.glow.enabled;
				const bool drawOutline =
					!suppressEffects && config.outline.enabled;
				shadowHasOffset = drawShadow
					&& (config.shadow.x != 0.0f
						|| config.shadow.y != 0.0f);
				if (drawGlow)
					bodyLayerMask |=
						1u << static_cast<UInt8>(AtlasLayer::Glow);
				if (drawOutline)
					bodyLayerMask |=
						1u << static_cast<UInt8>(AtlasLayer::Outline);
				if (drawShadow && !shadowHasOffset)
					bodyLayerMask |=
						1u << static_cast<UInt8>(AtlasLayer::Shadow);
				for (const GlyphInstance& instance : glyphs)
				{
					const size_t roleIndex =
						static_cast<size_t>(
							GetDirectGlyphByteClass(instance));
					if (roleIndex >= rasterProfiles.size())
					{
						result.outcome =
							DirectAtlasShapeOutcome::Failed;
						return result;
					}
					if (rasterProfileReady[roleIndex])
						continue;
					if (!ResolveDistanceFieldRasterProfile(config,
						GetDirectGlyphByteClass(instance),
						rasterScale, true,
						rasterProfiles[roleIndex]))
					{
						return result;
					}
					rasterProfileReady[roleIndex] = true;
				}
				result.sdfSpreadPixels =
					effects.sdfSpreadPixels;
			}
			else
			{
				effects.enabled = true;
				effects.precomposedArgb = true;
			}

			std::array<std::array<UInt32,
				kMaximumAtlasSnapshotPages>, 2> counts;
			if (!InitializeDirectQuadCountPrefix(counts, atlases.size(),
				rangeInitializationBytesAvoided))
			{
				result.outcome = DirectAtlasShapeOutcome::Failed;
				return result;
			}
			for (const DirectAtlasBatchGlyph& glyph : batch.glyphs)
			{
				if (glyph.knownEmpty)
					continue;
				if ((!glyph.placement && !glyph.vanillaLetter)
					|| glyph.atlasPage >= atlases.size()
					|| glyph.atlasPage >= kMaximumAtlasSnapshotPages)
				{
					result.outcome = DirectAtlasShapeOutcome::Failed;
					return result;
				}
				if (distanceField && shadowHasOffset)
					++counts[0][glyph.atlasPage];
				++counts[1][glyph.atlasPage];
			}
			std::array<std::array<UInt32,
				kMaximumAtlasSnapshotPages>, 2> offsets;
			std::array<std::array<UInt32,
				kMaximumAtlasSnapshotPages>, 2> cursors;
			// The live prefix is assigned in full by the range builder below.
			rangeInitializationBytesAvoided += sizeof(offsets) + sizeof(cursors);
			UInt32 physicalQuads = 0;
			for (size_t kind = 0; kind < counts.size(); ++kind)
			{
				for (size_t page = 0; page < atlases.size(); ++page)
				{
					if (counts[kind][page]
						> kNativeFontMaximumArtifactQuads - physicalQuads)
					{
						result.outcome = DirectAtlasShapeOutcome::Failed;
						return result;
					}
					offsets[kind][page] = physicalQuads;
					cursors[kind][page] = physicalQuads;
					physicalQuads += counts[kind][page];
				}
			}
			if (!physicalQuads)
			{
				result.outcome = DirectAtlasShapeOutcome::Failed;
				return result;
			}
			result.geometryQuadCount = physicalQuads;
			result.pageCount = CountUsedDirectAtlasPages(
				counts, atlases.size());

			std::vector<NativeFontGpuVertex> vertices(
				static_cast<size_t>(physicalQuads) * 4u);
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					DirectShapeVertexInitializationBytesAvoided,
				vertices.size() * sizeof(NativeFontGpuVertex));
			const NiPoint3 origin =
				GetDirectGlyphPen(glyphs[firstDrawable]);
			NiPoint3 boundMinimum(std::numeric_limits<float>::max(),
				std::numeric_limits<float>::max(),
				std::numeric_limits<float>::max());
			NiPoint3 boundMaximum(std::numeric_limits<float>::lowest(),
				std::numeric_limits<float>::lowest(),
				std::numeric_limits<float>::lowest());
			NativeFontShapeColorContract colorContract;
			bool colorContractInitialized = false;
			NiColorA facadeColor = tileColor;
			bool facadeColorInitialized = false;
			std::array<float, kMaximumAtlasSnapshotPages>
				pageSourceScales;
			std::array<UInt8, kMaximumAtlasSnapshotPages>
				pageSdfSpreads;
			std::array<bool, kMaximumAtlasSnapshotPages>
				pageProfileReady;
			// Scale/spread are read only after the corresponding ready flag has
			// been set. Precomposed batches never read any of these arrays.
			rangeInitializationBytesAvoided +=
				sizeof(pageSourceScales) + sizeof(pageSdfSpreads);
			if (distanceField)
			{
				if (!InitializeDirectPagePrefix(pageProfileReady,
					atlases.size(), false,
					rangeInitializationBytesAvoided))
				{
					result.outcome = DirectAtlasShapeOutcome::Failed;
					return result;
				}
			}
			else
			{
				rangeInitializationBytesAvoided += sizeof(pageProfileReady);
			}
			const bool directFullSpanComposite = distanceField
				&& atlases.size() == 1u && !shadowHasOffset
				&& physicalQuads == result.glyphCount
				&& counts[0][0] == 0u
				&& counts[1][0] == result.glyphCount;
			const bool directShiftedShadowComposite = distanceField
				&& atlases.size() == 1u && shadowHasOffset
				&& static_cast<UInt64>(physicalQuads)
					== static_cast<UInt64>(result.glyphCount) * 2u
				&& counts[0][0] == result.glyphCount
				&& counts[1][0] == result.glyphCount;
			std::vector<CompositeGlyphQuadSource> compositeSources;
			if (distanceField && !directFullSpanComposite)
				compositeSources.reserve(result.glyphCount);

			for (size_t glyphIndex = 0;
				glyphIndex < glyphs.size(); ++glyphIndex)
			{
				const DirectAtlasBatchGlyph& source =
					batch.glyphs[glyphIndex];
				if (source.knownEmpty)
					continue;
				const GlyphInstance& instance = glyphs[glyphIndex];
				const UInt16 page = source.atlasPage;
				float sourceToLogicalScale = 1.0f;
				if (distanceField)
				{
					const size_t roleIndex =
						static_cast<size_t>(
							GetDirectGlyphByteClass(instance));
					if (roleIndex >= rasterProfiles.size()
						|| !rasterProfileReady[roleIndex])
					{
						result.outcome =
							DirectAtlasShapeOutcome::Failed;
						return result;
					}
					sourceToLogicalScale =
						rasterProfiles[roleIndex].sourceToLogicalScale;
					if (pageProfileReady[page]
						&& (pageSourceScales[page]
								!= sourceToLogicalScale
							|| pageSdfSpreads[page]
								!= source.placement->sdfSpread))
					{
						// Direct ranges are page-contiguous. A profile that mixes
						// distance parameters within one page remains on the
						// existing compatibility compiler.
						return result;
					}
					pageProfileReady[page] = true;
					pageSourceScales[page] = sourceToLogicalScale;
					pageSdfSpreads[page] = source.placement->sdfSpread;
				}
				const NiColorA baseColor =
					ResolveBaseColor(
						GetDirectGlyphSourceColor(instance),
						tileColor);
				ExtendColorContract(
					colorContract, colorContractInitialized, baseColor);
				const float baselineOffset =
					GetDirectGlyphBaselineOffset(
						runtime, batch, instance);
				const NiPoint3& pen =
					GetDirectGlyphPen(instance);
				auto writeQuad = [&](size_t kind, float offsetX,
					float offsetY, UInt8 layerMask)
				{
					const UInt32 quadIndex = cursors[kind][page]++;
					if (quadIndex >= physicalQuads)
						return false;
					if (quadIndex == 0)
					{
						facadeColor = baseColor;
						facadeColorInitialized = true;
					}
					if (source.vanillaLetter)
					{
						if (distanceField || offsetX != 0.0f
							|| offsetY != 0.0f)
						{
							return false;
						}
						return WriteVanillaDirectQuadVertices(
							*source.vanillaLetter, pen, origin,
							baselineOffset,
							PackNativeBaseColor(baseColor),
							layerMask,
							&vertices[quadIndex * 4u],
							boundMinimum, boundMaximum);
					}
					return WriteDirectQuadVertices(
						*source.placement, pen, origin,
						offsetX, offsetY, rasterScale,
						baselineOffset, sourceToLogicalScale,
						distanceField,
						PackNativeBaseColor(baseColor),
						layerMask, &vertices[quadIndex * 4u],
						boundMinimum, boundMaximum);
				};
				if (distanceField && shadowHasOffset
					&& !writeQuad(0, config.shadow.x, config.shadow.y,
						1u << static_cast<UInt8>(AtlasLayer::Shadow)))
				{
					result.outcome = DirectAtlasShapeOutcome::Failed;
					return result;
				}
				const UInt32 bodyQuadIndex = cursors[1][page];
				if (!writeQuad(1, 0.0f, 0.0f,
					distanceField ? bodyLayerMask
						: 1u << static_cast<UInt8>(AtlasLayer::Fill)))
				{
					result.outcome = DirectAtlasShapeOutcome::Failed;
					return result;
				}
				if (distanceField && !directFullSpanComposite)
				{
					compositeSources.push_back({
						bodyQuadIndex * 4u, page,
						static_cast<UInt8>(bodyLayerMask
							| (drawShadow
								? 1u << static_cast<UInt8>(AtlasLayer::Shadow)
								: 0u))
					});
				}
			}
			if (!DirectQuadRangesCompletelyWritten(
				counts, offsets, cursors, atlases.size(), physicalQuads))
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::DirectShapeVertexCoverageFailure);
				result.outcome = DirectAtlasShapeOutcome::Failed;
				return result;
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::
					DirectShapeRangeInitializationBytesAvoided,
				rangeInitializationBytesAvoided);
			if (!facadeColorInitialized || !colorContractInitialized)
			{
				result.outcome = DirectAtlasShapeOutcome::Failed;
				return result;
			}

			auto appendRanges = [&](size_t kind, UInt32 layer,
				bool enabled)
			{
				if (!enabled)
					return;
				for (UInt16 page = 0;
					page < atlases.size(); ++page)
				{
					const UInt32 quadCount = counts[kind][page];
					if (!quadCount)
						continue;
					NativeFontDrawRange range;
					range.firstVertex = offsets[kind][page] * 4u;
					range.vertexCount = quadCount * 4u;
					range.startIndex = offsets[kind][page] * 6u;
					range.primitiveCount = quadCount * 2u;
					range.layer = layer;
					range.atlasPage = page;
					range.usesSdf = distanceField;
					range.usesLiveTileRgb = distanceField
						? effects.layerUsesLiveTileRgb[layer] : true;
					range.sdfSpreadPixels = distanceField
						? static_cast<float>(pageSdfSpreads[page])
						: 0.0f;
					range.sourceToLogicalScale = distanceField
						? pageSourceScales[page] : 1.0f;
					range.layerColorModifier = distanceField
						? effects.layerColorModifiers[layer]
						: NiColorA{ 1.0f, 1.0f, 1.0f, 1.0f };
					effects.ranges.push_back(range);
					result.drawQuadCount += quadCount;
				}
			};
			if (distanceField)
			{
				appendRanges(shadowHasOffset ? 0u : 1u,
					static_cast<UInt32>(AtlasLayer::Shadow),
					drawShadow);
				appendRanges(1,
					static_cast<UInt32>(AtlasLayer::Glow),
					(bodyLayerMask
						& (1u << static_cast<UInt8>(AtlasLayer::Glow)))
						!= 0);
				appendRanges(1,
					static_cast<UInt32>(AtlasLayer::Outline),
					(bodyLayerMask
						& (1u << static_cast<UInt8>(AtlasLayer::Outline)))
						!= 0);
				appendRanges(1,
					static_cast<UInt32>(AtlasLayer::Fill), true);
			}
			else
			{
				appendRanges(1,
					static_cast<UInt32>(AtlasLayer::Fill), true);
			}
			effects.enabled = !effects.ranges.empty();
			if (!effects.enabled)
			{
				result.outcome = DirectAtlasShapeOutcome::Failed;
				return result;
			}
			std::vector<NativeFontCompositeSpan> compositeSpans;
			bool compositeReady = false;
			if (directFullSpanComposite
				&& vertices.size() == static_cast<size_t>(physicalQuads) * 4u
				&& pageProfileReady[0])
			{
				NativeFontCompositeSpan span;
				span.firstVertex = 0;
				span.vertexCount = static_cast<UInt32>(vertices.size());
				span.atlasPage = 0;
				span.fused = true;
				const float uniformDistanceParameterScale =
					1.0f / pageSourceScales[0];
				if (bodyLayerMask >= 1u && bodyLayerMask <= 15u
					&& pageSdfSpreads[0] > 0u
					&& std::isfinite(uniformDistanceParameterScale)
					&& uniformDistanceParameterScale >= 1.0f)
				{
					span.constructionWitness.uniformSdfSpread =
						static_cast<float>(pageSdfSpreads[0]);
					span.constructionWitness.uniformDistanceParameterScale =
						uniformDistanceParameterScale;
					span.constructionWitness.staticLayerMask = bodyLayerMask;
					span.constructionWitness.complete = true;
					span.constructionWitness.registrationVerticesValid = true;
					span.constructionWitness.vanillaLayoutVerticesValid = true;
					RecordFreeTypePerf(FreeTypePerfCounter::
						DirectCompositeConstructionWitness);
				}
				compositeSpans.push_back(span);
				RecordFreeTypePerf(FreeTypePerfCounter::
					DirectCompositeSourceProofElementScanSaved,
					result.glyphCount);
				compositeReady = true;
			}
			else if (distanceField)
			{
				CompositeConstructionProfile constructionProfile;
				const CompositeConstructionProfile* constructionProfilePointer =
					nullptr;
				if (directShiftedShadowComposite && pageProfileReady[0])
				{
					// The shifted union span is appended after the ordinary shadow/body
					// ranges. Prove only that span here; payload sealing still validates
					// every retained compatibility vertex below.
					constructionProfile.uniformSdfSpread =
						static_cast<float>(pageSdfSpreads[0]);
					constructionProfile.uniformDistanceParameterScale =
						1.0f / pageSourceScales[0];
					constructionProfile.staticLayerMask = static_cast<UInt8>(
						bodyLayerMask
						| (1u << static_cast<UInt8>(AtlasLayer::Shadow)));
					constructionProfilePointer = &constructionProfile;
				}
				compositeReady = AppendOneGlyphCompositeQuads(vertices,
					compositeSources, static_cast<UInt32>(atlases.size()),
					effects, boundMinimum, boundMaximum, compositeSpans,
					constructionProfilePointer);
				if (compositeReady && compositeSpans.size() == 1u
					&& compositeSpans[0].constructionWitness.complete)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						DirectCompositeConstructionWitness);
				}
			}
			if (distanceField && !compositeReady)
			{
				// Keep the ordinary layer packets as a functional fallback for
				// oversized or malformed batches. Normal composite-capable text
				// still takes the one-quad representation.
				compositeSpans.clear();
				RecordFreeTypePerf(
					FreeTypePerfCounter::CompositeOverlapFallback);
			}
			result.shape = CreateDirectNativeShape(font, atlases,
				std::move(vertices), result.glyphCount, physicalQuads,
				effects, colorContract, facadeColor, tileColor, origin,
				boundMinimum, boundMaximum, prepareObject,
				std::move(compositeSpans));
			result.outcome = result.shape
				? DirectAtlasShapeOutcome::Created
				: DirectAtlasShapeOutcome::Failed;
			return result;
		}

		DirectAtlasShapeBuildResult TryCreateDirectCachedLetterShape(
			Font& font, RuntimeFont& runtime,
			const std::vector<AtlasGlyphInstance>& glyphs, float rasterScale,
			bool prepareObject, const NiColorA& tileColor,
			bool suppressEffects, GlyphMaskType maskType,
			EffectQuality quality)
		{
			DirectAtlasShapeBuildResult result;
			const bool precomposed =
				maskType == GlyphMaskType::Composite;
			const bool distanceField =
				maskType == GlyphMaskType::DistanceField;
			if (!precomposed && !distanceField)
				return result;
			const AtlasPixelMode pixelMode = precomposed
				? AtlasPixelMode::Argb32
				: GetConfiguredDistanceFieldAtlasPixelMode();
			const AtlasRenderMode renderMode = precomposed
				? AtlasRenderMode::CpuEffects
				: AtlasRenderMode::ShaderEffects;
			const UInt32 padding = precomposed
				? kArgbAtlasPadding : kDistanceFieldAtlasPadding;
			const std::shared_ptr<const SealedDirectFontProfile>
				sealedBeforeBuild =
					LoadRuntimeSealedDirectProfile(runtime);
			const bool sealedBatchExpected = sealedBeforeBuild
				&& IsSealedDirectFontProfileUsable(
					runtime, sealedBeforeBuild, rasterScale)
				&& sealedBeforeBuild->pixelMode == pixelMode
				&& sealedBeforeBuild->renderMode == renderMode
				&& sealedBeforeBuild->padding == padding
				&& std::all_of(glyphs.begin(), glyphs.end(),
					[](const AtlasGlyphInstance& instance)
					{
						return instance.glyph.hasDirectMetrics
							&& instance.glyph.directSlot
								!= std::numeric_limits<UInt16>::max();
					});

			thread_local DirectAtlasGlyphBatch batch;
			batch.Clear();
			struct BatchReset
			{
				DirectAtlasGlyphBatch& batch;
				~BatchReset() { batch.Clear(); }
			} reset{ batch };
			if (!GetDirectAtlasGlyphBatch(runtime, glyphs, maskType,
				rasterScale, pixelMode, renderMode, padding, batch))
			{
				const bool sealedStillCurrent = sealedBatchExpected
					&& LoadRuntimeSealedDirectProfile(runtime).get()
						== sealedBeforeBuild.get();
				if (sealedStillCurrent)
				{
					InvalidateSealedDirectFontProfileIfCurrent(
						runtime, sealedBeforeBuild);
					result.outcome =
						DirectAtlasShapeOutcome::Failed;
				}
				return result;
			}
			result = CreateDirectCachedLetterShapeFromBatch(
				font, runtime, glyphs, batch, rasterScale,
				prepareObject, tileColor, suppressEffects,
				maskType, quality);
			const bool sealedStillCurrent = sealedBatchExpected
				&& LoadRuntimeSealedDirectProfile(runtime).get()
					== sealedBeforeBuild.get();
			if (sealedStillCurrent
				&& result.outcome
					== DirectAtlasShapeOutcome::Unavailable)
			{
				result.outcome = DirectAtlasShapeOutcome::Failed;
			}
			if (sealedStillCurrent
				&& result.outcome == DirectAtlasShapeOutcome::Failed)
			{
				InvalidateSealedDirectFontProfileIfCurrent(
					runtime, sealedBeforeBuild);
			}
			return result;
		}

		DirectAtlasShapeBuildResult TryCreateDirectCachedLetterShape(
			Font& font, RuntimeFont& runtime,
			const std::shared_ptr<const SealedDirectFontProfile>& sealed,
			const std::vector<DirectGlyphCommand>& glyphs,
			float rasterScale, bool prepareObject,
			const NiColorA& tileColor, bool suppressEffects,
			GlyphMaskType maskType, EffectQuality quality)
		{
			DirectAtlasShapeBuildResult result;
			const bool precomposed =
				maskType == GlyphMaskType::Composite;
			const bool distanceField =
				maskType == GlyphMaskType::DistanceField;
			if (!sealed || (!precomposed && !distanceField))
			{
				result.outcome = sealed
					? DirectAtlasShapeOutcome::Unavailable
					: DirectAtlasShapeOutcome::Failed;
				return result;
			}
			const AtlasPixelMode pixelMode = precomposed
				? AtlasPixelMode::Argb32
				: GetConfiguredDistanceFieldAtlasPixelMode();
			const AtlasRenderMode renderMode = precomposed
				? AtlasRenderMode::CpuEffects
				: AtlasRenderMode::ShaderEffects;
			const UInt32 padding = precomposed
				? kArgbAtlasPadding : kDistanceFieldAtlasPadding;

			thread_local DirectAtlasGlyphBatch batch;
			batch.Clear();
			struct BatchReset
			{
				DirectAtlasGlyphBatch& batch;
				~BatchReset() { batch.Clear(); }
			} reset{ batch };
			if (!GetSealedDirectAtlasGlyphBatch(runtime, sealed,
				glyphs, maskType, rasterScale, pixelMode,
				renderMode, padding, batch))
			{
				InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed);
				result.outcome = DirectAtlasShapeOutcome::Failed;
				return result;
			}
			result = CreateDirectCachedLetterShapeFromBatch(
				font, runtime, glyphs, batch, rasterScale,
				prepareObject, tileColor, suppressEffects,
				maskType, quality);
			if (result.outcome == DirectAtlasShapeOutcome::Unavailable)
				result.outcome = DirectAtlasShapeOutcome::Failed;
			if (result.outcome == DirectAtlasShapeOutcome::Failed)
				InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed);
			return result;
		}
}

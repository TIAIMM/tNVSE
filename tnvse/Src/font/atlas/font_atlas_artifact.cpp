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
		NativeFontPayloadTemplatePtr BuildNativeTextArtifact(Font& font,
			const std::vector<PendingQuad>& quads,
			const std::vector<std::shared_ptr<AtlasResource>>& atlases,
			const NiPoint3& origin, const NativeFontEffectShapeConfig& effects)
		{
			if (quads.empty()
				|| quads.size() > kNativeFontMaximumArtifactQuads)
			{
				return {};
			}
			AtlasState& state = State();
			RecordFreeTypePerf(FreeTypePerfCounter::TextArtifactCompile);
			FreeTypePerfScope artifactCompilePerf(
				FreeTypePerfPhase::TextArtifactCompile);

			std::vector<NativeFontGpuVertex> vertices;
			const size_t sourceVertexCount = quads.size() * 4u;
			vertices.reserve(sourceVertexCount);
			RecordFreeTypePerf(
				FreeTypePerfCounter::TextArtifactCompiledVertex,
				sourceVertexCount);
			RecordFreeTypePerf(
				FreeTypePerfCounter::TextArtifactVertexInitializationBytesAvoided,
				sourceVertexCount * sizeof(NativeFontGpuVertex));
			bool compositeCandidate = effects.shaderEffects;
			std::vector<CompositeGlyphQuadSource> compositeGlyphs;
			if (compositeCandidate)
				compositeGlyphs.resize(quads.size());
			UInt32 compositeGlyphCount = 0;
			UInt32 compositeGlyphsWithoutFill = 0;
			bool compositeGlyphsDense = true;
			NiPoint3 boundMinimum(std::numeric_limits<float>::max(),
				std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
			NiPoint3 boundMaximum(std::numeric_limits<float>::lowest(),
				std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());
			const UInt8 fillLayerBit =
				1u << static_cast<UInt8>(AtlasLayer::Fill);
			UInt32 glyphCount = 0;
			NativeFontShapeColorContract colorContract;
			bool colorContractInitialized = false;
			for (UInt32 index = 0; index < quads.size(); ++index)
			{
				const PendingQuad& quad = quads[index];
				if (quad.atlasPage >= atlases.size() || !atlases[quad.atlasPage])
					return nullptr;
				const AtlasResource& atlas = *atlases[quad.atlasPage];
				if (!IsAtlasGlyphPlacementCurrent(
					quad.atlasPlacement, atlas, quad.atlasPage))
				{
					return nullptr;
				}
				if (quad.layerMask & fillLayerBit)
					++glyphCount;
				ExtendColorContract(
					colorContract, colorContractInitialized, quad.baseColor);
				const float scale = quad.rasterScale;
				const float sourcePixelToLogical =
					quad.sourceToLogicalScale / scale;
				const float expansion = static_cast<float>(quad.expansionPixels);
				const float logicalX = quad.pen.x - origin.x + quad.offsetX;
				const float logicalZ = quad.pen.z - origin.z
					+ quad.baselineOffset - quad.offsetY;
				const float bitmapLeft =
					static_cast<float>(quad.source.Left()) - expansion;
				const float bitmapTop =
					static_cast<float>(quad.source.Top()) + expansion;
				// Coverage masks stay aligned to their source-pixel grid. SDF masks are
				// reconstructed analytically by the shader, so snapping their logical
				// origin would discard shaped fractional advances and effect offsets.
				const float x0 = quad.usesSdf
					? logicalX + bitmapLeft * sourcePixelToLogical
					: std::round(logicalX * scale + bitmapLeft) / scale;
				const float z0 = quad.usesSdf
					? logicalZ + bitmapTop * sourcePixelToLogical
					: std::round(logicalZ * scale + bitmapTop) / scale;
				const float x1 = x0 + (static_cast<float>(quad.source.Width())
					+ expansion * 2.0f) * (quad.usesSdf
						? sourcePixelToLogical : 1.0f / scale);
				const float z1 = z0 - (static_cast<float>(quad.source.Height())
					+ expansion * 2.0f) * (quad.usesSdf
						? sourcePixelToLogical : 1.0f / scale);
				if (compositeCandidate)
				{
					if (!quad.usesSdf
						|| quad.glyphOrdinal >= compositeGlyphs.size())
					{
						compositeCandidate = false;
					}
					else
					{
						CompositeGlyphQuadSource& glyph =
							compositeGlyphs[quad.glyphOrdinal];
						if (!glyph.layerMask)
						{
							compositeGlyphsDense = compositeGlyphsDense
								&& quad.glyphOrdinal == compositeGlyphCount;
							++compositeGlyphCount;
							++compositeGlyphsWithoutFill;
						}
						if (glyph.atlasPage
								!= std::numeric_limits<UInt16>::max()
							&& glyph.atlasPage != quad.atlasPage)
						{
							compositeCandidate = false;
						}
						glyph.atlasPage = quad.atlasPage;
						glyph.layerMask |= quad.layerMask;
						if (quad.layerMask
							& (1u << static_cast<UInt8>(AtlasLayer::Fill)))
						{
							if (glyph.firstVertex
								== std::numeric_limits<UInt32>::max()
								&& compositeGlyphsWithoutFill)
							{
								--compositeGlyphsWithoutFill;
							}
							glyph.firstVertex =
								static_cast<UInt32>(vertices.size());
						}
					}
				}
				if (g_bEnableFreeTypeFontRenderingLog
					&& (quad.layerMask & (1u << static_cast<UInt8>(AtlasLayer::Fill))))
				{
					bool shouldLog = false;
					{
						std::lock_guard<std::mutex> lock(state.atlasMutex);
						shouldLog = state.loggedVerticalMetricFonts.insert(font.iFontNum).second;
					}
					if (shouldLog)
					{
						const float bitmapTop =
							static_cast<float>(quad.source.Top())
							* (quad.usesSdf ? sourcePixelToLogical : 1.0f / scale)
							+ quad.baselineOffset;
						FreeTypeFontDebugLog(
							"tnvse_freetype_font: first atlas vertical metrics font=%u scale=%.3f bitmapTop=%.3f logicalTopEdge=%.3f delta=%.3f baselineOffset=%.3f penZ=%.3f quadTop=%.3f positioning=%s",
							font.iFontNum, scale, bitmapTop, quad.logicalTopEdge,
							bitmapTop - quad.logicalTopEdge, quad.baselineOffset,
							quad.pen.z, z0 + origin.z,
							quad.usesSdf ? "distance-field-fractional"
								: "source-pixel-snapped");
					}
				}
				// All layers belong to the same logical Tile text. Ordering is provided by
				// the contiguous draw ranges, not by depth offsets. Artificial per-layer
				// depth can make an effect occlude the fill (or another Tile) on Pip-Boy
				// render targets whose UI pass has depth testing enabled.
				const float depth = quad.pen.y - origin.y;
				const float u0 = quad.atlasPlacement.u0
					- expansion * quad.atlasPlacement.inverseWidth;
				const float v0 = quad.atlasPlacement.v0
					- expansion * quad.atlasPlacement.inverseHeight;
				const float u1 = quad.atlasPlacement.u1
					+ expansion * quad.atlasPlacement.inverseWidth;
				const float v1 = quad.atlasPlacement.v1
					+ expansion * quad.atlasPlacement.inverseHeight;
				const UInt32 packedColor = PackNativeBaseColor(
					effects.bakedCoverage
						? ComposeQuadColor(quad) : quad.baseColor);
				const std::array<NiPoint3, 4> positions = {{
					NiPoint3(x0, depth, z0), NiPoint3(x1, depth, z0),
					NiPoint3(x1, depth, z1), NiPoint3(x0, depth, z1)
				}};
				const std::array<NiPoint2, 4> texture = {{
					NiPoint2(u0, v0), NiPoint2(u1, v0),
					NiPoint2(u1, v1), NiPoint2(u0, v1)
				}};
				for (UInt32 ordinal = 0; ordinal < 4; ++ordinal)
				{
					const NiPoint3& position = positions[ordinal];
					const NiPoint2& uv = texture[ordinal];
					vertices.push_back({ position.x, position.y, position.z,
						uv.x, uv.y, packedColor,
						static_cast<float>(quad.source.SdfSpread()),
						quad.sourceToLogicalScale > 0.0f
							? 1.0f / quad.sourceToLogicalScale : 1.0f,
						effects.bakedCoverage
							? (quad.usesLiveTileRgb ? 1.0f : 0.0f)
							: static_cast<float>(quad.layerMask),
						SanitizeNativeUvBound(u0),
						SanitizeNativeUvBound(v0),
						SanitizeNativeUvBound(u1),
						SanitizeNativeUvBound(v1) });
					boundMinimum.x = std::min(boundMinimum.x, position.x);
					boundMinimum.y = std::min(boundMinimum.y, position.y);
					boundMinimum.z = std::min(boundMinimum.z, position.z);
					boundMaximum.x = std::max(boundMaximum.x, position.x);
					boundMaximum.y = std::max(boundMaximum.y, position.y);
					boundMaximum.z = std::max(boundMaximum.z, position.z);
				}
			}
			if (vertices.size() != sourceVertexCount)
				return nullptr;
			std::vector<NativeFontCompositeSpan> compositeSpans;
			std::vector<CompositeGlyphQuadSource> compositeSources;
			if (compositeCandidate && compositeGlyphsDense
				&& compositeGlyphCount && !compositeGlyphsWithoutFill)
			{
				// The shader compiler emits one dense glyph ordinal for the common
				// single-page MTSDF artifact. The mandatory quad loop above has
				// already proved every occupied entry and its Fill vertex, so transfer
				// the backing allocation instead of rescanning and copying it.
				compositeGlyphs.resize(compositeGlyphCount);
				compositeSources = std::move(compositeGlyphs);
				RecordFreeTypePerf(FreeTypePerfCounter::
					TextArtifactCompositeSourceCompactionScanSaved,
					quads.size());
			}
			else if (compositeCandidate)
			{
				compositeSources.reserve(compositeGlyphs.size());
				for (const CompositeGlyphQuadSource& glyph : compositeGlyphs)
				{
					if (!glyph.layerMask)
						continue;
					if (glyph.firstVertex
						== std::numeric_limits<UInt32>::max())
					{
						compositeCandidate = false;
						break;
					}
					compositeSources.push_back(glyph);
				}
			}
			if (compositeCandidate
				&& AppendOneGlyphCompositeQuads(vertices, compositeSources,
					static_cast<UInt32>(atlases.size()), effects,
					boundMinimum, boundMaximum, compositeSpans, nullptr))
			{
				RecordFreeTypePerf(atlases.size() == 1
					? FreeTypePerfCounter::CompositeFusedEligible
					: FreeTypePerfCounter::CompositeOrderedEligible);
			}
			else if (compositeCandidate)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::CompositeOverlapFallback);
			}
			NiBound bound;
			bound.m_kCenter = NiPoint3(
				(boundMinimum.x + boundMaximum.x) * 0.5f,
				(boundMinimum.y + boundMaximum.y) * 0.5f,
				(boundMinimum.z + boundMaximum.z) * 0.5f);
			float radiusSquared = 0.0f;
			for (const NativeFontGpuVertex& vertex : vertices)
			{
				const float dx = vertex.x - bound.m_kCenter.x;
				const float dy = vertex.y - bound.m_kCenter.y;
				const float dz = vertex.z - bound.m_kCenter.z;
				radiusSquared = std::max(radiusSquared,
					dx * dx + dy * dy + dz * dz);
			}
			bound.m_fRadius = std::sqrt(radiusSquared);
			NativeFontPayloadTemplatePtr result = BuildNativeFontPayloadTemplate(
				std::move(vertices), static_cast<UInt32>(quads.size()),
				glyphCount, colorContract, effects, bound,
				std::move(compositeSpans));
			if (!result)
				return {};
			return result;
		}

		NiTriShape* CreateAtlasShape(Font& font, const std::vector<PendingQuad>& quads,
			const std::vector<std::shared_ptr<AtlasResource>>& atlases, bool prepareObject,
			const NiColorA& tileColor, bool useNativeFontShader,
			const NativeFontEffectShapeConfig* effectConfig, const NiPoint3& origin)
		{
			if (atlases.empty() || !atlases[0] || quads.empty()
				|| quads.size() > kNativeFontMaximumArtifactQuads)
				return nullptr;
			const bool needsNativeRangeRouting = useNativeFontShader
				|| atlases.size() > 1 || quads.size() > kMaximumQuads;
			NativeFontEffectShapeConfig resolvedEffect = effectConfig
				? *effectConfig : NativeFontEffectShapeConfig{};
			const UInt32 profileKinds =
				(resolvedEffect.shaderEffects ? 1u : 0u)
				+ (resolvedEffect.bakedCoverage ? 1u : 0u)
				+ (resolvedEffect.precomposedArgb ? 1u : 0u);
			if (needsNativeRangeRouting && profileKinds == 0u)
			{
				const bool allArgb = std::all_of(atlases.begin(), atlases.end(),
					[](const std::shared_ptr<AtlasResource>& atlas)
					{
						return atlas
							&& atlas->pixelMode == AtlasPixelMode::Argb32;
					});
				if (!allArgb)
					return nullptr;
				resolvedEffect.precomposedArgb = true;
			}
			resolvedEffect.atlasProperties.clear();
			resolvedEffect.atlasTextures.clear();
			resolvedEffect.atlasInverseSizes.clear();
			for (const auto& atlas : atlases)
			{
				resolvedEffect.atlasProperties.push_back(
					atlas ? atlas->property : nullptr);
				resolvedEffect.atlasTextures.push_back(
					atlas ? GetAtlasTexture(*atlas) : nullptr);
				resolvedEffect.atlasInverseSizes.push_back(atlas
					? NiPoint2(1.0f / atlas->width, 1.0f / atlas->height)
					: NiPoint2());
			}
			BuildNativeFontDrawRanges(quads, resolvedEffect);
			const NativeFontPayloadTemplatePtr artifact = BuildNativeTextArtifact(
				font, quads, atlases, origin, resolvedEffect);
			if (!artifact || artifact->gpuVertices.size() < quads.size() * 4u)
				return nullptr;
			RecordFreeTypePerf(
				FreeTypePerfCounter::TextArtifactShapeMetadataElementScanSaved,
				static_cast<UInt64>(quads.size()) * 2u);
			const UInt32 glyphCount = artifact->glyphCount;
			const NativeFontShapeColorContract& colorContract =
				artifact->colorContract;
			if (needsNativeRangeRouting)
			{
				if (NiTriShape* vanillaLayout = TryCreateVanillaLayoutShape(
					font, atlases, artifact, glyphCount, resolvedEffect,
					colorContract, tileColor, origin, prepareObject))
				{
					return vanillaLayout;
				}
			}

			NiTriShape* shape = CreateFreeTypeTextShape(
				static_cast<UInt32>(needsNativeRangeRouting ? 1u : quads.size()),
				tileColor, false, atlases[0]->property,
				GetAtlasTexture(*atlases[0]));
			NiTriShapeOwner shapeOwner(shape);
			if (!shape || !shape->GetModelData())
				return nullptr;
			shape->m_kLocal.m_Translate = NiPoint3(0.0f, 0.0f, 0.0f);

			NiTriShapeData* data = shape->GetModelData();
			if (!data)
				return nullptr;
			if (!data->m_pkColor)
				data->m_pkColor = NiAlloc<NiColorA>(data->m_usVertices);
			if (!data->m_pkColor)
				return nullptr;
			std::fill_n(data->m_pkColor, data->m_usVertices,
				SanitizeColor(tileColor));
			data->m_kBound = artifact->bound;
			data->m_kBound.m_kCenter.x += origin.x;
			data->m_kBound.m_kCenter.y += origin.y;
			data->m_kBound.m_kCenter.z += origin.z;
			if (!needsNativeRangeRouting)
			{
				if (!data->m_pkVertex || !data->m_pkTexture || !data->m_pusTriList)
					return nullptr;
				static constexpr UInt16 kCanonicalQuad[6] = { 0, 2, 1, 0, 3, 2 };
				for (UInt32 quadIndex = 0; quadIndex < quads.size(); ++quadIndex)
				{
					const NiColorA baseColor = SanitizeColor(quads[quadIndex].baseColor);
					for (UInt32 ordinal = 0; ordinal < 4; ++ordinal)
					{
						const UInt32 index = quadIndex * 4u + ordinal;
						const NativeFontGpuVertex& vertex = artifact->gpuVertices[index];
						data->m_pkVertex[index] = NiPoint3(vertex.x + origin.x,
							vertex.y + origin.y, vertex.z + origin.z);
						data->m_pkTexture[index] = NiPoint2(vertex.u, vertex.v);
						data->m_pkColor[index] = baseColor;
					}
					for (UInt32 ordinal = 0; ordinal < 6; ++ordinal)
					{
						data->m_pusTriList[quadIndex * 6u + ordinal] =
							static_cast<UInt16>(quadIndex * 4u + kCanonicalQuad[ordinal]);
					}
				}
			}
			else
			{
				// The vanilla accumulator still validates and prepares this one-quad
				// facade before the sorted Tile hook substitutes the shared proxy.
				// Give it a complete, finite quad instead of leaving the newly
				// constructed geometry arrays unspecified.
				if (data->m_usVertices < 4 || !data->m_pkVertex
					|| !data->m_pkTexture || !data->m_pusTriList)
				{
					return nullptr;
				}
				static constexpr UInt16 kFacadeQuad[6] = { 0, 2, 1, 0, 3, 2 };
				const NiColorA baseColor = SanitizeColor(quads.front().baseColor);
				for (UInt32 ordinal = 0; ordinal < 4; ++ordinal)
				{
					const NativeFontGpuVertex& vertex = artifact->gpuVertices[ordinal];
					data->m_pkVertex[ordinal] = NiPoint3(vertex.x + origin.x,
						vertex.y + origin.y, vertex.z + origin.z);
					data->m_pkTexture[ordinal] = NiPoint2(vertex.u, vertex.v);
					data->m_pkColor[ordinal] = baseColor;
				}
				std::copy(std::begin(kFacadeQuad), std::end(kFacadeQuad),
					data->m_pusTriList);
			}
			if (needsNativeRangeRouting)
			{
				if (!PrepareNativeFontAtlasShape(font, shape, font.iFontNum,
					glyphCount,
					static_cast<UInt32>(quads.size()), &resolvedEffect, &colorContract,
					artifact, origin))
				{
					return nullptr;
				}
			}
			// PrepareObject establishes the engine-side submission state even when
			// native rendering later replaces this facade with a shared proxy.
			if (prepareObject)
			{
				if (needsNativeRangeRouting
					&& IsFreeTypeNoPrecacheRouteActive())
				{
					shape->PrepareObject(false, true);
				}
				else
				{
					shape->PrepareObject();
				}
			}
			// The facade carries only one dummy quad on the native path. Restore the
			// immutable artifact bound after vanilla preparation so culling never sees
			// the dummy geometry's extent.
			data->m_kBound = artifact->bound;
			data->m_kBound.m_kCenter.x += origin.x;
			data->m_kBound.m_kCenter.y += origin.y;
			data->m_kBound.m_kCenter.z += origin.z;
			// PrepareObject may have propagated the one-quad facade bound to the AV
			// object. Refresh the already-created world bound from the restored full
			// artifact bound so vanilla accumulator culling cannot discard the text
			// before the sorted Tile route gets a chance to substitute its proxy.
			if (prepareObject && shape->m_pWorldBound)
				shape->UpdateWorldBound();
			return shapeOwner.release();
		}
}

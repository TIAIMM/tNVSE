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
		float SanitizeColorChannel(float value, float fallback = 1.0f)
		{
			return std::isfinite(value) ? value : fallback;
		}

		NiColorA SanitizeColor(const NiColorA& color)
		{
			return {
				SanitizeColorChannel(color.r),
				SanitizeColorChannel(color.g),
				SanitizeColorChannel(color.b),
				SanitizeColorChannel(color.a)
			};
		}

		float ResolveModifierChannel(float source, float tile)
		{
			if (!std::isfinite(source) || !std::isfinite(tile))
				return 1.0f;
			if (std::fabs(tile) > 0.000001f)
			{
				const float result = source / tile;
				return std::isfinite(result) ? result : 1.0f;
			}
			// The original TILE1000 contract multiplies the texture by c0. A zero
			// c0 channel cannot be recovered by any finite texture/modifier value,
			// so preserve that dynamic game state instead of trying to replace it.
			return 1.0f;
		}

		NiColorA ResolveSourceModifier(const NiColorA& source, const NiColorA& tile)
		{
			return {
				ResolveModifierChannel(source.r, tile.r),
				ResolveModifierChannel(source.g, tile.g),
				ResolveModifierChannel(source.b, tile.b),
				ResolveModifierChannel(source.a, tile.a)
			};
		}

		NiColorA ResolveFillColor(const FontColorStyle& style, const NiColorA& source,
			const NiColorA& tile)
		{
			NiColorA result = ResolveSourceModifier(source, tile);
			if (style.configured)
			{
				result.r *= style.color.r;
				result.g *= style.color.g;
				result.b *= style.color.b;
				result.a *= style.color.a;
			}
			return SanitizeColor(result);
		}

		NiColorA ResolveEffectColor(const EffectStyle& effect, const NiColorA& source,
			const NiColorA& tile)
		{
			NiColorA result = SanitizeColor(effect.color);
			result.a *= ResolveModifierChannel(source.a, tile.a);
			return SanitizeColor(result);
		}

		NiColorA ResolveSafeTileColor(const std::vector<AtlasGlyphInstance>&,
			const NiColorA& requested)
		{
			// c0 is the live Tile color/alpha selected by the game (and may also be
			// changed by render extensions such as NVR). Never infer or replace zero
			// channels from glyph colors; only discard non-finite input.
			return SanitizeColor(requested);
		}

		A8ShapeColorContract BuildColorContract(const std::vector<PendingQuad>& quads)
		{
			A8ShapeColorContract result;
			if (quads.empty())
				return result;
			result.minimumModifier = SanitizeColor(quads.front().color);
			result.maximumModifier = result.minimumModifier;
			for (const PendingQuad& quad : quads)
			{
				const NiColorA color = SanitizeColor(quad.color);
				result.minimumModifier.r = std::min(result.minimumModifier.r, color.r);
				result.minimumModifier.g = std::min(result.minimumModifier.g, color.g);
				result.minimumModifier.b = std::min(result.minimumModifier.b, color.b);
				result.minimumModifier.a = std::min(result.minimumModifier.a, color.a);
				result.maximumModifier.r = std::max(result.maximumModifier.r, color.r);
				result.maximumModifier.g = std::max(result.maximumModifier.g, color.g);
				result.maximumModifier.b = std::max(result.maximumModifier.b, color.b);
				result.maximumModifier.a = std::max(result.maximumModifier.a, color.a);
			}
			return result;
		}

		bool SameColorModifier(const NiColorA& lhs, const NiColorA& rhs)
		{
			return lhs.r == rhs.r && lhs.g == rhs.g
				&& lhs.b == rhs.b && lhs.a == rhs.a;
		}

		void BuildA8DrawRanges(const std::vector<PendingQuad>& quads,
			A8EffectShapeConfig& config)
		{
			config.ranges.clear();
			for (UInt32 index = 0; index < quads.size(); ++index)
			{
				const PendingQuad& quad = quads[index];
				const NiColorA color = SanitizeColor(quad.color);
				const UInt32 layer = static_cast<UInt32>(quad.layer);
				if (config.ranges.empty()
					|| config.ranges.back().layer != layer
					|| config.ranges.back().atlasPage != quad.atlasPage
					|| config.ranges.back().usesSdf != quad.usesSdf
					|| !SameColorModifier(config.ranges.back().colorModifier, color))
				{
					A8DrawRange range;
					range.firstVertex = index * 4;
					range.startIndex = index * 6;
				range.layer = layer;
				range.atlasPage = quad.atlasPage;
					range.usesSdf = quad.usesSdf;
					range.colorModifier = color;
					config.ranges.push_back(range);
				}
				A8DrawRange& range = config.ranges.back();
				range.vertexCount += 4;
				range.primitiveCount += 2;
			}
			config.enabled = !config.ranges.empty();
		}

		UInt32 PackColorModifierRgba(const NiColorA& color)
		{
			const NiColorA safeColor = SanitizeColor(color);
			auto channel = [](float value)
			{
				return static_cast<UInt32>(std::lround(
					std::clamp(value, 0.0f, 1.0f) * 255.0f));
			};
			return (channel(safeColor.a) << 24) | (channel(safeColor.r) << 16)
				| (channel(safeColor.g) << 8) | channel(safeColor.b);
		}

		UInt64 BuildBakedBitmapId(UInt64 sourceId, UInt32 rgba)
		{
			UInt64 result = 1469598103934665603ull;
			auto add = [&](const void* data, size_t size)
			{
				const UInt8* bytes = static_cast<const UInt8*>(data);
				for (size_t index = 0; index < size; ++index)
				{
					result ^= bytes[index];
					result *= 1099511628211ull;
				}
			};
			constexpr UInt32 marker = 0x41524742; // ARGB
			add(&marker, sizeof(marker));
			add(&sourceId, sizeof(sourceId));
			add(&rgba, sizeof(rgba));
			return result;
		}

		void BuildBakedArgbFallback(const std::vector<PendingQuad>& source,
			std::vector<PendingQuad>& quads,
			std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps)
		{
			quads = source;
			std::unordered_map<UInt64, std::shared_ptr<const GlyphBitmap>> unique;
			for (PendingQuad& quad : quads)
			{
				if (!quad.bitmap)
					continue;
				quad.color = SanitizeColor(quad.color);
				const UInt32 rgba = PackColorModifierRgba(quad.color);
				UInt64 bakedId = BuildBakedBitmapId(quad.bitmap->cacheId, rgba);
				bakedId = BuildBakedBitmapId(bakedId, static_cast<UInt8>(quad.layer));
				auto found = unique.find(bakedId);
				if (found == unique.end())
				{
					auto baked = std::make_shared<GlyphBitmap>(*quad.bitmap);
					baked->cacheId = bakedId;
					baked->atlasRgb = rgba & 0x00FFFFFF;
					baked->colorBaked = true;
					baked->bakedRgba = rgba;
					baked->bakedLayer = static_cast<UInt8>(quad.layer);
					const float alphaModifier = std::clamp(quad.color.a, 0.0f, 1.0f);
					for (UInt8& alpha : baked->alpha)
					{
						alpha = static_cast<UInt8>(std::lround(
							static_cast<float>(alpha) * alphaModifier));
					}
					found = unique.emplace(bakedId, std::move(baked)).first;
				}
				quad.bitmap = found->second;
				quad.color = { 1.0f, 1.0f, 1.0f, 1.0f };
			}
			bitmaps.clear();
			bitmaps.reserve(unique.size());
			for (auto& [id, bitmap] : unique)
				bitmaps.push_back(std::move(bitmap));
			std::sort(bitmaps.begin(), bitmaps.end(), [](const auto& lhs, const auto& rhs)
			{
				return lhs->cacheId < rhs->cacheId;
			});
		}


		void AddPendingQuad(std::vector<PendingQuad>& quads,
			const std::shared_ptr<const GlyphBitmap>& bitmap,
			const AtlasGlyphInstance& instance, const NiColorA& color,
			float offsetX, float offsetY, float rasterScale, float baselineOffset,
			AtlasLayer layer,
			UInt32 expansionPixels = 0, bool usesSdf = false)
		{
			if (bitmap && bitmap->width > 0 && bitmap->height > 0)
				quads.push_back({ bitmap, instance.pen, color, offsetX, offsetY,
					rasterScale, instance.glyph.metrics ? instance.glyph.metrics->fTopEdge : 0.0f,
					baselineOffset, expansionPixels, layer, usesSdf });
		}

		bool BuildPendingQuads(RuntimeFont& runtime,
			const std::vector<AtlasGlyphInstance>& glyphs, float rasterScale,
			const std::array<bool, 4>& included, const NiColorA& tileColor,
			std::vector<PendingQuad>& quads, PendingQuadBuildFailure& failure)
		{
			struct PreparedGlyph
			{
				const AtlasGlyphInstance* instance = nullptr;
				std::shared_ptr<const GlyphBitmap> fill;
				std::shared_ptr<const GlyphBitmap> glow;
				std::shared_ptr<const GlyphBitmap> outline;
				float baselineOffset = 0.0f;
			};

			quads.clear();
			failure = PendingQuadBuildFailure::None;
			const FontConfig& config = GetRuntimeConfig(runtime);
			thread_local std::vector<PreparedGlyph> prepared;
			thread_local std::vector<GlyphBitmapRequest> bitmapRequests;
			thread_local std::vector<std::shared_ptr<const GlyphBitmap>> bitmapResults;
			prepared.clear();
			prepared.reserve(glyphs.size());
			bitmapRequests.clear();
			bitmapRequests.reserve(glyphs.size() * 3);
			for (const AtlasGlyphInstance& instance : glyphs)
			{
				PreparedGlyph glyph;
				glyph.instance = &instance;
				glyph.baselineOffset = GetGlyphBaselineOffset(
					runtime, instance.glyph.byteClass);
				bitmapRequests.push_back({ &instance.glyph, GlyphMaskType::Fill, 0 });
				if (included[static_cast<size_t>(AtlasLayer::Glow)] && config.glow.enabled)
					bitmapRequests.push_back({ &instance.glyph, GlyphMaskType::Glow, 0 });
				if (included[static_cast<size_t>(AtlasLayer::Outline)] && config.outline.enabled)
					bitmapRequests.push_back({ &instance.glyph, GlyphMaskType::Outline, 0 });
				prepared.push_back(std::move(glyph));
			}
			GetGlyphBitmaps(runtime, bitmapRequests, rasterScale, bitmapResults);
			size_t bitmapIndex = 0;
			for (PreparedGlyph& glyph : prepared)
			{
				glyph.fill = bitmapResults[bitmapIndex++];
				if (!glyph.fill)
				{
					failure = PendingQuadBuildFailure::Fill;
					return false;
				}
				if (included[static_cast<size_t>(AtlasLayer::Glow)] && config.glow.enabled)
				{
					glyph.glow = bitmapResults[bitmapIndex++];
					if (!glyph.glow)
					{
						failure = PendingQuadBuildFailure::Glow;
						return false;
					}
				}
				if (included[static_cast<size_t>(AtlasLayer::Outline)] && config.outline.enabled)
				{
					glyph.outline = bitmapResults[bitmapIndex++];
					if (!glyph.outline)
					{
						failure = PendingQuadBuildFailure::Outline;
						return false;
					}
				}
			}

			// Tile text does not consistently depth-test effect triangles. Submit each
			// complete layer before the next one so a later glyph's effect cannot cover
			// an earlier glyph's fill.
			if (included[static_cast<size_t>(AtlasLayer::Shadow)] && config.shadow.enabled)
			{
				for (const PreparedGlyph& glyph : prepared)
				{
					AddPendingQuad(quads, glyph.fill, *glyph.instance,
						ResolveEffectColor(config.shadow, glyph.instance->color, tileColor),
						config.shadow.x, config.shadow.y, rasterScale,
						glyph.baselineOffset, AtlasLayer::Shadow);
				}
			}
			if (included[static_cast<size_t>(AtlasLayer::Glow)] && config.glow.enabled)
			{
				for (const PreparedGlyph& glyph : prepared)
				{
					AddPendingQuad(quads, glyph.glow, *glyph.instance,
						ResolveEffectColor(config.glow, glyph.instance->color, tileColor),
						0.0f, 0.0f, rasterScale, glyph.baselineOffset, AtlasLayer::Glow);
				}
			}
			if (included[static_cast<size_t>(AtlasLayer::Outline)] && config.outline.enabled)
			{
				for (const PreparedGlyph& glyph : prepared)
				{
					AddPendingQuad(quads, glyph.outline, *glyph.instance,
						ResolveEffectColor(config.outline, glyph.instance->color, tileColor),
						0.0f, 0.0f, rasterScale, glyph.baselineOffset, AtlasLayer::Outline);
				}
			}
			if (included[static_cast<size_t>(AtlasLayer::Fill)])
			{
				for (const PreparedGlyph& glyph : prepared)
				{
					AddPendingQuad(quads, glyph.fill, *glyph.instance,
						ResolveFillColor(config.fontColor, glyph.instance->color, tileColor),
						0.0f, 0.0f, rasterScale, glyph.baselineOffset, AtlasLayer::Fill);
				}
			}
			return true;
		}

		bool BuildShaderEffectQuads(RuntimeFont& runtime,
			const std::vector<AtlasGlyphInstance>& glyphs, float rasterScale,
			EffectQuality quality, const NiColorA& tileColor, bool suppressEffects,
			std::vector<PendingQuad>& quads,
			ShaderEffectBuild& build)
		{
			quads.clear();
			build = {};
			build.config.enabled = true;
			build.config.shaderEffects = true;
			build.config.quality = quality;
			const FontConfig& config = GetRuntimeConfig(runtime);
			const bool fillUsesSdf = UsesSdfFill(config);
			const bool needsSdf = fillUsesSdf
				|| (!suppressEffects && HasSdfEffects(config));
			// An SDF body is also an exact hard-shadow source. Reusing it avoids
			// inserting a second grayscale copy of every glyph into the atlas when
			// SDF fill is active.
			const bool needsGrayFill = !fillUsesSdf;
			build.config.fillUsesSdf = fillUsesSdf;
			UInt32 sdfSpread = 0;
			if (needsSdf && !ResolveSdfSpread(
				config, rasterScale, sdfSpread, !suppressEffects))
			{
				if (g_bEnableFreeTypeFontRenderingLog)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: SDF spread unsupported font=%u scale=%.3f glowOuter=%.3f outline=%.3f softness=%.3f shadowBlur=%.3f; using CPU effects",
						config.fontId, rasterScale, config.glow.outer,
						config.outline.width, config.outline.softness,
						config.shadow.blur);
				}
				return false;
			}
			build.config.sdfSpreadPixels = static_cast<float>(sdfSpread);
			build.config.shadowBlurPixels = config.shadow.blur * rasterScale;
			build.config.shadowPower = config.shadow.power;
			build.config.glowInnerPixels = config.glow.inner * rasterScale;
			build.config.glowOuterPixels = config.glow.outer * rasterScale;
			build.config.glowPower = config.glow.power;
			build.config.outlineWidthPixels = config.outline.width * rasterScale;
			build.config.outlineSoftnessPixels = config.outline.softness * rasterScale;

			struct PreparedShaderGlyph
			{
				const AtlasGlyphInstance* instance = nullptr;
				std::shared_ptr<const GlyphBitmap> fill;
				std::shared_ptr<const GlyphBitmap> sdf;
				float baselineOffset = 0.0f;
			};
			thread_local std::vector<PreparedShaderGlyph> prepared;
			thread_local std::vector<GlyphBitmapRequest> bitmapRequests;
			thread_local std::vector<std::shared_ptr<const GlyphBitmap>> bitmapResults;
			prepared.clear();
			prepared.reserve(glyphs.size());
			bitmapRequests.clear();
			bitmapRequests.reserve(glyphs.size() * 2);
			for (const AtlasGlyphInstance& instance : glyphs)
			{
				PreparedShaderGlyph glyph;
				glyph.instance = &instance;
				glyph.baselineOffset = GetGlyphBaselineOffset(
					runtime, instance.glyph.byteClass);
				if (needsGrayFill)
					bitmapRequests.push_back({ &instance.glyph, GlyphMaskType::Fill, 0 });
				if (needsSdf)
					bitmapRequests.push_back({ &instance.glyph,
						GlyphMaskType::DistanceField, sdfSpread });
				prepared.push_back(std::move(glyph));
			}
			GetAtlasBackedGlyphBitmaps(runtime, bitmapRequests, rasterScale,
				AtlasPixelMode::A8, AtlasRenderMode::ShaderEffects,
				kAtlasPadding, bitmapResults);
			size_t bitmapIndex = 0;
			for (PreparedShaderGlyph& glyph : prepared)
			{
				if (needsGrayFill)
				{
					glyph.fill = bitmapResults[bitmapIndex++];
					if (!glyph.fill)
						return false;
				}
				if (needsSdf)
				{
					glyph.sdf = bitmapResults[bitmapIndex++];
					if (!glyph.sdf)
						return false;
				}
			}

			auto addRange = [&](AtlasLayer layer, bool enabled, bool useSdf,
				float offsetX, float offsetY)
			{
				if (!enabled)
					return;
				for (const PreparedShaderGlyph& entry : prepared)
				{
					NiColorA layerColor = entry.instance->color;
					if (layer == AtlasLayer::Shadow)
						layerColor = ResolveEffectColor(config.shadow, entry.instance->color, tileColor);
					else if (layer == AtlasLayer::Glow)
						layerColor = ResolveEffectColor(config.glow, entry.instance->color, tileColor);
					else if (layer == AtlasLayer::Outline)
						layerColor = ResolveEffectColor(config.outline, entry.instance->color, tileColor);
					else if (layer == AtlasLayer::Fill)
						layerColor = ResolveFillColor(config.fontColor, entry.instance->color, tileColor);
					AddPendingQuad(quads, useSdf ? entry.sdf : entry.fill,
						*entry.instance, layerColor, offsetX, offsetY, rasterScale,
						entry.baselineOffset, layer, 0, useSdf);
				}
			};

			addRange(AtlasLayer::Shadow, !suppressEffects && config.shadow.enabled,
				fillUsesSdf || config.shadow.blur > 0.0f,
				config.shadow.x, config.shadow.y);
			addRange(AtlasLayer::Glow, !suppressEffects && config.glow.enabled,
				true, 0.0f, 0.0f);
			addRange(AtlasLayer::Outline, !suppressEffects && config.outline.enabled,
				true, 0.0f, 0.0f);
			addRange(AtlasLayer::Fill, true, fillUsesSdf, 0.0f, 0.0f);
			return true;
		}


		QuadBatchFingerprint BuildQuadBatchFingerprint(
			const std::vector<PendingQuad>& quads, const NiPoint3& origin)
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
			for (const PendingQuad& quad : quads)
			{
				const NiPoint3 relativePen(
					quad.pen.x - origin.x,
					quad.pen.y - origin.y,
					quad.pen.z - origin.z);
				add(&quad.bitmap->cacheId, sizeof(quad.bitmap->cacheId));
				add(&relativePen, sizeof(relativePen));
				add(&quad.offsetX, sizeof(quad.offsetX));
				add(&quad.offsetY, sizeof(quad.offsetY));
				add(&quad.rasterScale, sizeof(quad.rasterScale));
				add(&quad.baselineOffset, sizeof(quad.baselineOffset));
				add(&quad.expansionPixels, sizeof(quad.expansionPixels));
				add(&quad.layer, sizeof(quad.layer));
				add(&quad.usesSdf, sizeof(quad.usesSdf));
				add(&quad.atlasPage, sizeof(quad.atlasPage));
			}
			return { hash, static_cast<UInt32>(quads.size()) };
		}

		BatchTemplateKey BuildBatchTemplateKey(const QuadBatchFingerprint& fingerprint,
			const std::vector<std::shared_ptr<AtlasResource>>& atlases)
		{
			UInt64 hash = fingerprint.contentHash;
			auto add = [&](const void* data, size_t size)
			{
				const UInt8* bytes = static_cast<const UInt8*>(data);
				for (size_t index = 0; index < size; ++index)
				{
					hash ^= bytes[index];
					hash *= 1099511628211ull;
				}
			};
			UInt32 generation = 0;
			for (const auto& atlas : atlases)
			{
				if (!atlas)
					continue;
				const uintptr_t identity = reinterpret_cast<uintptr_t>(atlas.get());
				add(&identity, sizeof(identity));
				add(&atlas->width, sizeof(atlas->width));
				add(&atlas->height, sizeof(atlas->height));
				add(&atlas->generation, sizeof(atlas->generation));
				generation ^= atlas->generation + 0x9E3779B9u
					+ (generation << 6) + (generation >> 2);
			}
			return { atlases.empty() ? 0 : reinterpret_cast<uintptr_t>(atlases[0].get()),
				hash, generation, fingerprint.quadCount };
		}

		std::shared_ptr<const BatchTemplate> GetBatchTemplate(Font& font,
			const std::vector<PendingQuad>& quads,
			const std::vector<std::shared_ptr<AtlasResource>>& atlases,
			const QuadBatchFingerprint& fingerprint, const NiPoint3& origin)
		{
			AtlasState& state = State();
			const BatchTemplateKey key = BuildBatchTemplateKey(fingerprint, atlases);
			{
				std::lock_guard<std::mutex> lock(state.batchMutex);
				auto existing = state.batchCache.find(key);
				if (existing != state.batchCache.end())
				{
					state.batchLru.splice(state.batchLru.begin(), state.batchLru,
						existing->second.lru);
					existing->second.lru = state.batchLru.begin();
					RecordFreeTypePerf(FreeTypePerfCounter::BatchHit);
					return existing->second.data;
				}
			}
			RecordFreeTypePerf(FreeTypePerfCounter::BatchMiss);

			auto result = std::make_shared<BatchTemplate>();
			result->vertices.resize(quads.size() * 4);
			result->texture.resize(quads.size() * 4);
			result->indices.resize(quads.size() * 6);
			for (UInt32 index = 0; index < quads.size(); ++index)
			{
				const PendingQuad& quad = quads[index];
				if (quad.atlasPage >= atlases.size() || !atlases[quad.atlasPage])
					return nullptr;
				const AtlasResource& atlas = *atlases[quad.atlasPage];
				const AtlasRect& rect = atlas.placements.at(quad.bitmap->cacheId);
				const float scale = quad.rasterScale;
				const float expansion = static_cast<float>(quad.expansionPixels);
				const float logicalX = quad.pen.x - origin.x + quad.offsetX;
				const float logicalZ = quad.pen.z - origin.z
					+ quad.baselineOffset - quad.offsetY;
				const float bitmapLeft = static_cast<float>(quad.bitmap->left) - expansion;
				const float bitmapTop = static_cast<float>(quad.bitmap->top) + expansion;
				// Coverage masks stay aligned to their source-pixel grid. SDF masks are
				// reconstructed analytically by the shader, so snapping their logical
				// origin would discard shaped fractional advances and effect offsets.
				const float x0 = quad.usesSdf
					? logicalX + bitmapLeft / scale
					: std::round(logicalX * scale + bitmapLeft) / scale;
				const float z0 = quad.usesSdf
					? logicalZ + bitmapTop / scale
					: std::round(logicalZ * scale + bitmapTop) / scale;
				const float x1 = x0 + (static_cast<float>(quad.bitmap->width)
					+ expansion * 2.0f) / scale;
				const float z1 = z0 - (static_cast<float>(quad.bitmap->height)
					+ expansion * 2.0f) / scale;
				if (g_bEnableFreeTypeFontRenderingLog && quad.layer == AtlasLayer::Fill)
				{
					bool shouldLog = false;
					{
						std::lock_guard<std::mutex> lock(state.atlasMutex);
						shouldLog = state.loggedVerticalMetricFonts.insert(font.iFontNum).second;
					}
					if (shouldLog)
					{
						const float bitmapTop = static_cast<float>(quad.bitmap->top) / scale
							+ quad.baselineOffset;
						FreeTypeFontDebugLog(
							"tnvse_freetype_font: first atlas vertical metrics font=%u scale=%.3f bitmapTop=%.3f logicalTopEdge=%.3f delta=%.3f baselineOffset=%.3f penZ=%.3f quadTop=%.3f positioning=%s",
							font.iFontNum, scale, bitmapTop, quad.logicalTopEdge,
							bitmapTop - quad.logicalTopEdge, quad.baselineOffset,
							quad.pen.z, z0 + origin.z,
							quad.usesSdf ? "sdf-subpixel" : "source-pixel-snapped");
					}
				}
				// All layers belong to the same logical Tile text. Ordering is provided by
				// the contiguous draw ranges, not by depth offsets. Artificial per-layer
				// depth can make an effect occlude the fill (or another Tile) on Pip-Boy
				// render targets whose UI pass has depth testing enabled.
				const float depth = quad.pen.y - origin.y;
				const float u0 = (static_cast<float>(rect.x) - expansion) / atlas.width;
				const float v0 = (static_cast<float>(rect.y) - expansion) / atlas.height;
				const float u1 = (static_cast<float>(rect.x + rect.width) + expansion)
					/ atlas.width;
				const float v1 = (static_cast<float>(rect.y + rect.height) + expansion)
					/ atlas.height;
				const UInt32 base = index * 4;
				result->vertices[base + 0] = NiPoint3(x0, depth, z0);
				result->vertices[base + 1] = NiPoint3(x1, depth, z0);
				result->vertices[base + 2] = NiPoint3(x1, depth, z1);
				result->vertices[base + 3] = NiPoint3(x0, depth, z1);
				result->texture[base + 0] = NiPoint2(u0, v0);
				result->texture[base + 1] = NiPoint2(u1, v0);
				result->texture[base + 2] = NiPoint2(u1, v1);
				result->texture[base + 3] = NiPoint2(u0, v1);
				const UInt32 triangle = index * 6;
				result->indices[triangle + 0] = static_cast<UInt16>(base + 0);
				result->indices[triangle + 1] = static_cast<UInt16>(base + 2);
				result->indices[triangle + 2] = static_cast<UInt16>(base + 1);
				result->indices[triangle + 3] = static_cast<UInt16>(base + 0);
				result->indices[triangle + 4] = static_cast<UInt16>(base + 3);
				result->indices[triangle + 5] = static_cast<UInt16>(base + 2);
			}


			const size_t bytes = result->vertices.size() * sizeof(NiPoint3)
				+ result->texture.size() * sizeof(NiPoint2)
				+ result->indices.size() * sizeof(UInt16);
			{
				std::lock_guard<std::mutex> lock(state.batchMutex);
				state.batchLru.push_front(key);
				state.batchCache.emplace(key,
					BatchTemplateEntry{ result, bytes, state.batchLru.begin() });
				state.batchCacheBytes += bytes;
				TrimBatchCache(state);
			}
			return result;
		}

		NiTriShape* CreateAtlasShape(Font& font, const std::vector<PendingQuad>& quads,
			const std::vector<std::shared_ptr<AtlasResource>>& atlases, bool prepareObject,
			const NiColorA& tileColor, bool useCustomA8Shader,
			const A8EffectShapeConfig* effectConfig,
			const QuadBatchFingerprint& fingerprint, const NiPoint3& origin)
		{
			if (atlases.empty() || !atlases[0] || quads.empty()
				|| quads.size() > kMaximumQuads)
				return nullptr;
			NiTriShape* shape = font.MakeTriShape(static_cast<int>(quads.size()),
				&tileColor, false);
			if (!shape || !shape->GetModelData())
				return nullptr;
			shape->m_kLocal.m_Translate = NiPoint3(0.0f, 0.0f, 0.0f);
			shape->RemoveProperty(NiProperty::TEXTURING);
			shape->AddProperty(atlases[0]->property);
			shape->UpdateProperties();
			if (NiShadeProperty* shade = shape->GetShadeProperty())
			{
				if (shade->m_eShaderType == NiShadeProperty::PROP_Tile)
				{
					if (NiTexture* texture = GetAtlasTexture(*atlases[0]))
						ThisStdCall(0xBB7A10, shade, texture);
				}
			}

			NiTriShapeData* data = shape->GetModelData();
			const std::shared_ptr<const BatchTemplate> batch =
				GetBatchTemplate(font, quads, atlases, fingerprint, origin);
			if (!batch)
				return nullptr;
			for (size_t index = 0; index < batch->vertices.size(); ++index)
			{
				const NiPoint3& vertex = batch->vertices[index];
				data->m_pkVertex[index] = NiPoint3(
					vertex.x + origin.x,
					vertex.y + origin.y,
					vertex.z + origin.z);
			}
			std::copy(batch->texture.begin(), batch->texture.end(), data->m_pkTexture);
			std::copy(batch->indices.begin(), batch->indices.end(), data->m_pusTriList);
			ThisStdCall(0xA7EE30, &data->m_kBound, data->m_usVertices, data->m_pkVertex);
			const bool hasEffectLayer = std::any_of(quads.begin(), quads.end(),
				[](const PendingQuad& quad) { return quad.layer != AtlasLayer::Fill; });
			const bool needsNativeRangeRouting = useCustomA8Shader || hasEffectLayer
				|| atlases.size() > 1;
			if (needsNativeRangeRouting)
			{
				A8EffectShapeConfig resolvedEffect = effectConfig
					? *effectConfig : A8EffectShapeConfig{};
				resolvedEffect.useOriginalShader = !useCustomA8Shader;
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
				BuildA8DrawRanges(quads, resolvedEffect);
				const A8ShapeColorContract colorContract = BuildColorContract(quads);
				if (!PrepareA8AtlasShape(font, shape, font.iFontNum,
					static_cast<UInt32>(std::count_if(quads.begin(), quads.end(),
						[](const PendingQuad& quad) { return quad.layer == AtlasLayer::Fill; })),
					static_cast<UInt32>(quads.size()), &resolvedEffect, &colorContract))
				{
					return nullptr;
				}
			}
			if (prepareObject)
				shape->PrepareObject();
			return shape;
		}

		NiTriShape* TryCreateAtlasShapeForMode(Font& font,
			const std::vector<PendingQuad>& quads,
			const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
			const FontConfig& config, float rasterScale, bool prepareObject,
			AtlasPixelMode pixelMode, AtlasRenderMode renderMode, UInt32 padding,
			std::vector<std::shared_ptr<AtlasResource>>& outAtlases,
			const NiColorA& tileColor, bool useCustomA8Shader,
			const A8EffectShapeConfig* effectConfig)
		{
			if (quads.empty())
				return nullptr;
			if (pixelMode == AtlasPixelMode::A8 && !useCustomA8Shader)
				return nullptr;
			const std::vector<PendingQuad>* activeQuads = &quads;
			const std::vector<std::shared_ptr<const GlyphBitmap>>* activeBitmaps = &bitmaps;
			thread_local std::vector<PendingQuad> bakedQuads;
			thread_local std::vector<std::shared_ptr<const GlyphBitmap>> bakedBitmaps;
			if (pixelMode == AtlasPixelMode::Argb32 && !useCustomA8Shader)
			{
				BuildBakedArgbFallback(quads, bakedQuads, bakedBitmaps);
				activeQuads = &bakedQuads;
				activeBitmaps = &bakedBitmaps;
			}

			thread_local std::vector<UInt16> bitmapPageOrdinals;
			std::vector<std::shared_ptr<AtlasResource>> availableAtlases =
				GetAtlasResources(config, rasterScale, *activeBitmaps, pixelMode,
				renderMode, padding, &bitmapPageOrdinals);
			if (availableAtlases.empty())
			{
				std::shared_ptr<AtlasResource> transient = CreateTransientAtlas(
					*activeBitmaps, pixelMode, renderMode, padding);
				if (transient)
				{
					availableAtlases.push_back(std::move(transient));
					bitmapPageOrdinals.assign(activeBitmaps->size(),
						std::numeric_limits<UInt16>::max());
					for (size_t bitmapIndex = 0; bitmapIndex < activeBitmaps->size();
						++bitmapIndex)
					{
						const auto& bitmap = (*activeBitmaps)[bitmapIndex];
						if (bitmap && availableAtlases[0]->placements.find(bitmap->cacheId)
							!= availableAtlases[0]->placements.end())
						{
							bitmapPageOrdinals[bitmapIndex] = 0;
						}
					}
				}
			}
			if (availableAtlases.empty())
				return nullptr;
			thread_local std::vector<PendingQuad> pagedQuads;
			pagedQuads = *activeQuads;
			const NiPoint3 batchOrigin = pagedQuads.front().pen;
			outAtlases.clear();
			std::vector<UInt16> compactPageIndices(availableAtlases.size(),
				std::numeric_limits<UInt16>::max());
			thread_local std::unordered_map<UInt64, UInt16> placementPages;
			placementPages.clear();
			placementPages.reserve(activeBitmaps->size());
			for (size_t bitmapIndex = 0; bitmapIndex < activeBitmaps->size(); ++bitmapIndex)
			{
				const auto& bitmap = (*activeBitmaps)[bitmapIndex];
				if (!bitmap || placementPages.find(bitmap->cacheId) != placementPages.end())
					continue;
				const UInt16 page = bitmapPageOrdinals[bitmapIndex];
				if (page < availableAtlases.size())
					placementPages.emplace(bitmap->cacheId, page);
			}
			for (PendingQuad& quad : pagedQuads)
			{
				const auto placement = placementPages.find(quad.bitmap->cacheId);
				if (placement == placementPages.end())
					return nullptr;
				const UInt16 page = placement->second;
				UInt16& compactPage = compactPageIndices[page];
				if (compactPage == std::numeric_limits<UInt16>::max())
				{
					compactPage = static_cast<UInt16>(outAtlases.size());
					outAtlases.push_back(availableAtlases[page]);
				}
				quad.atlasPage = compactPage;
			}
			const auto batchOrder = [](const PendingQuad& lhs, const PendingQuad& rhs)
			{
				if (lhs.layer != rhs.layer)
					return lhs.layer < rhs.layer;
				return lhs.atlasPage < rhs.atlasPage;
			};
			if (!std::is_sorted(pagedQuads.begin(), pagedQuads.end(), batchOrder))
			{
				std::stable_sort(pagedQuads.begin(), pagedQuads.end(), batchOrder);
			}
			const QuadBatchFingerprint fingerprint =
				BuildQuadBatchFingerprint(pagedQuads, batchOrigin);
			A8EffectShapeConfig resolvedEffect;
			const A8EffectShapeConfig* resolvedEffectPointer = nullptr;
			if (effectConfig)
			{
				resolvedEffect = *effectConfig;
				resolvedEffect.inverseAtlasWidth = 1.0f / outAtlases[0]->width;
				resolvedEffect.inverseAtlasHeight = 1.0f / outAtlases[0]->height;
				resolvedEffectPointer = &resolvedEffect;
			}
			NiTriShape* shape = CreateAtlasShape(font, pagedQuads, outAtlases, prepareObject,
				tileColor, useCustomA8Shader, resolvedEffectPointer, fingerprint,
				batchOrigin);
			if (shape && outAtlases.size() == 1 && outAtlases[0]->transient
				&& outAtlases[0]->backend == AtlasBackend::DefaultPool)
			{
				std::lock_guard<std::mutex> lock(State().atlasMutex);
				RetireDefaultGeneration(*outAtlases[0]);
			}
			return shape;
		}
}

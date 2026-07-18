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

		NiColorA ResolveBaseColor(const NiColorA& source, const NiColorA& tile)
		{
			return SanitizeColor(ResolveSourceModifier(source, tile));
		}

		NiColorA ResolveFillLayerColor(const FontColorStyle& style)
		{
			NiColorA result = { 1.0f, 1.0f, 1.0f, 1.0f };
			if (style.configured)
				result = style.color;
			return SanitizeColor(result);
		}

		NiColorA ResolveEffectLayerColor(const EffectStyle& effect,
			const FontColorStyle& fillStyle)
		{
			NiColorA result = effect.colorMode == EffectColorMode::Fill
				? ResolveFillLayerColor(fillStyle)
				: SanitizeColor(effect.color);
			// Effect opacity remains independent from fontAlpha in both color modes;
			// the per-glyph source alpha remains in the shared base vertex color.
			result.a = SanitizeColor(effect.color).a;
			return SanitizeColor(result);
		}

		NiColorA ComposeQuadColor(const PendingQuad& quad)
		{
			const NiColorA base = SanitizeColor(quad.baseColor);
			const NiColorA layer = SanitizeColor(quad.layerColorModifier);
			if (!quad.usesLiveTileRgb)
				return { layer.r, layer.g, layer.b, base.a * layer.a };
			return { base.r * layer.r, base.g * layer.g,
				base.b * layer.b, base.a * layer.a };
		}

		bool EffectUsesLiveTileRgb(const EffectStyle& effect)
		{
			return effect.colorMode == EffectColorMode::Fill;
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
			result.minimumModifier = SanitizeColor(quads.front().baseColor);
			result.maximumModifier = result.minimumModifier;
			for (const PendingQuad& quad : quads)
			{
				const NiColorA color = SanitizeColor(quad.baseColor);
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
			for (UInt32 layer = 0; layer < 4; ++layer)
			{
				const UInt8 layerBit = static_cast<UInt8>(1u << layer);
				for (UInt32 index = 0; index < quads.size(); ++index)
				{
					const PendingQuad& quad = quads[index];
					if (!(quad.layerMask & layerBit))
						continue;
					const NiColorA layerColor = SanitizeColor(config.shaderEffects
						? config.layerColorModifiers[layer] : quad.layerColorModifier);
					const bool usesLiveTileRgb = config.shaderEffects
						? config.layerUsesLiveTileRgb[layer] : quad.usesLiveTileRgb;
					const UInt32 firstVertex = index * 4;
					const UInt32 startIndex = index * 6;
					if (config.ranges.empty()
						|| config.ranges.back().firstVertex
							+ config.ranges.back().vertexCount != firstVertex
						|| config.ranges.back().startIndex
							+ config.ranges.back().primitiveCount * 3 != startIndex
						|| config.ranges.back().layer != layer
						|| config.ranges.back().atlasPage != quad.atlasPage
						|| config.ranges.back().usesSdf != quad.usesSdf
						|| config.ranges.back().usesLiveTileRgb != usesLiveTileRgb
						|| !SameColorModifier(
							config.ranges.back().layerColorModifier, layerColor))
					{
						A8DrawRange range;
						range.firstVertex = firstVertex;
						range.startIndex = startIndex;
						range.layer = layer;
						range.atlasPage = quad.atlasPage;
						range.usesSdf = quad.usesSdf;
						range.usesLiveTileRgb = usesLiveTileRgb;
						range.layerColorModifier = layerColor;
						config.ranges.push_back(range);
					}
					A8DrawRange& range = config.ranges.back();
					range.vertexCount += 4;
					range.primitiveCount += 2;
				}
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
				const NiColorA compositeColor = ComposeQuadColor(quad);
				const UInt32 rgba = PackColorModifierRgba(compositeColor);
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
					const float alphaModifier = std::clamp(compositeColor.a, 0.0f, 1.0f);
					for (UInt8& alpha : baked->alpha)
					{
						alpha = static_cast<UInt8>(std::lround(
							static_cast<float>(alpha) * alphaModifier));
					}
					found = unique.emplace(bakedId, std::move(baked)).first;
				}
				quad.bitmap = found->second;
				quad.baseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
				quad.layerColorModifier = { 1.0f, 1.0f, 1.0f, 1.0f };
				quad.usesLiveTileRgb = true;
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
			const AtlasGlyphInstance& instance, const NiColorA& baseColor,
			const NiColorA& layerColorModifier,
			float offsetX, float offsetY, float rasterScale, float baselineOffset,
			AtlasLayer layer, bool usesLiveTileRgb,
			UInt32 expansionPixels = 0, bool usesSdf = false, UInt8 layerMask = 0)
		{
			if (bitmap && bitmap->width > 0 && bitmap->height > 0)
			{
				PendingQuad quad;
				quad.bitmap = bitmap;
				quad.pen = instance.pen;
				quad.baseColor = SanitizeColor(baseColor);
				quad.layerColorModifier = SanitizeColor(layerColorModifier);
				quad.offsetX = offsetX;
				quad.offsetY = offsetY;
				quad.rasterScale = rasterScale;
				quad.logicalTopEdge = instance.glyph.metrics
					? instance.glyph.metrics->fTopEdge : 0.0f;
				quad.baselineOffset = baselineOffset;
				quad.expansionPixels = expansionPixels;
				quad.layer = layer;
				quad.layerMask = layerMask ? layerMask
					: static_cast<UInt8>(1u << static_cast<UInt8>(layer));
				quad.usesSdf = usesSdf;
				quad.usesLiveTileRgb = usesLiveTileRgb;
				quads.push_back(std::move(quad));
			}
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
			const bool visibleGlow = included[static_cast<size_t>(AtlasLayer::Glow)]
				&& config.glow.enabled;
			const bool visibleOutline = included[static_cast<size_t>(AtlasLayer::Outline)]
				&& config.outline.enabled;
			const bool shadowIncludesGlow =
				included[static_cast<size_t>(AtlasLayer::Shadow)]
				&& HardShadowIncludesGlow(config);
			const bool shadowIncludesOutline =
				included[static_cast<size_t>(AtlasLayer::Shadow)]
				&& HardShadowIncludesOutline(config);
			const bool needsGlowMask = visibleGlow || shadowIncludesGlow;
			const bool needsOutlineMask = visibleOutline || shadowIncludesOutline;
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
				glyph.baselineOffset = GetGlyphBaselineOffset(runtime, instance.glyph);
				bitmapRequests.push_back({ &instance.glyph, GlyphMaskType::Fill, 0 });
				if (needsGlowMask)
					bitmapRequests.push_back({ &instance.glyph, GlyphMaskType::Glow, 0 });
				if (needsOutlineMask)
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
				if (needsGlowMask)
				{
					glyph.glow = bitmapResults[bitmapIndex++];
					if (!glyph.glow && visibleGlow)
					{
						failure = PendingQuadBuildFailure::Glow;
						return false;
					}
				}
				if (needsOutlineMask)
				{
					glyph.outline = bitmapResults[bitmapIndex++];
					if (!glyph.outline && visibleOutline)
					{
						failure = PendingQuadBuildFailure::Outline;
						return false;
					}
				}
			}

			// Tile text does not consistently depth-test effect triangles. Submit each
			// complete layer before the next one, with the shadow behind the glow and
			// every effect behind the fill.
			if (included[static_cast<size_t>(AtlasLayer::Shadow)] && config.shadow.enabled)
			{
				for (const PreparedGlyph& glyph : prepared)
				{
					const NiColorA baseColor = ResolveBaseColor(
						glyph.instance->color, tileColor);
					const NiColorA shadowColor = ResolveEffectLayerColor(
						config.shadow, config.fontColor);
					if (shadowIncludesGlow && glyph.glow)
					{
						NiColorA componentColor = shadowColor;
						componentColor.a *= config.glow.color.a;
						AddPendingQuad(quads, glyph.glow, *glyph.instance,
							baseColor, componentColor,
							config.shadow.x, config.shadow.y,
							rasterScale, glyph.baselineOffset, AtlasLayer::Shadow,
							EffectUsesLiveTileRgb(config.shadow));
					}
					if (shadowIncludesOutline && glyph.outline)
					{
						NiColorA componentColor = shadowColor;
						componentColor.a *= config.outline.color.a;
						AddPendingQuad(quads, glyph.outline, *glyph.instance,
							baseColor, componentColor,
							config.shadow.x, config.shadow.y,
							rasterScale, glyph.baselineOffset, AtlasLayer::Shadow,
							EffectUsesLiveTileRgb(config.shadow));
					}
					AddPendingQuad(quads, glyph.fill, *glyph.instance,
						baseColor, shadowColor,
						config.shadow.x, config.shadow.y, rasterScale,
						glyph.baselineOffset, AtlasLayer::Shadow,
						EffectUsesLiveTileRgb(config.shadow));
				}
			}
			if (included[static_cast<size_t>(AtlasLayer::Glow)] && config.glow.enabled)
			{
				for (const PreparedGlyph& glyph : prepared)
				{
					AddPendingQuad(quads, glyph.glow, *glyph.instance,
						ResolveBaseColor(glyph.instance->color, tileColor),
						ResolveEffectLayerColor(config.glow, config.fontColor),
						0.0f, 0.0f, rasterScale, glyph.baselineOffset, AtlasLayer::Glow,
						EffectUsesLiveTileRgb(config.glow));
				}
			}
			if (included[static_cast<size_t>(AtlasLayer::Outline)] && config.outline.enabled)
			{
				for (const PreparedGlyph& glyph : prepared)
				{
					AddPendingQuad(quads, glyph.outline, *glyph.instance,
						ResolveBaseColor(glyph.instance->color, tileColor),
						ResolveEffectLayerColor(config.outline, config.fontColor),
						0.0f, 0.0f, rasterScale, glyph.baselineOffset, AtlasLayer::Outline,
						EffectUsesLiveTileRgb(config.outline));
				}
			}
			if (included[static_cast<size_t>(AtlasLayer::Fill)])
			{
				for (const PreparedGlyph& glyph : prepared)
				{
					AddPendingQuad(quads, glyph.fill, *glyph.instance,
						ResolveBaseColor(glyph.instance->color, tileColor),
						ResolveFillLayerColor(config.fontColor),
						0.0f, 0.0f, rasterScale, glyph.baselineOffset, AtlasLayer::Fill,
						true);
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
			build.config.shadowGlowAlpha = HardShadowIncludesGlow(config)
				? config.glow.color.a : 0.0f;
			build.config.shadowOutlineAlpha = HardShadowIncludesOutline(config)
				? config.outline.color.a : 0.0f;
			build.config.glowInnerPixels = config.glow.inner * rasterScale;
			build.config.glowOuterPixels = config.glow.outer * rasterScale;
			build.config.glowPower = config.glow.power;
			build.config.outlineWidthPixels = config.outline.width * rasterScale;
			build.config.outlineSoftnessPixels = config.outline.softness * rasterScale;
			build.config.layerColorModifiers[static_cast<size_t>(AtlasLayer::Shadow)] =
				ResolveEffectLayerColor(config.shadow, config.fontColor);
			build.config.layerColorModifiers[static_cast<size_t>(AtlasLayer::Glow)] =
				ResolveEffectLayerColor(config.glow, config.fontColor);
			build.config.layerColorModifiers[static_cast<size_t>(AtlasLayer::Outline)] =
				ResolveEffectLayerColor(config.outline, config.fontColor);
			build.config.layerColorModifiers[static_cast<size_t>(AtlasLayer::Fill)] =
				ResolveFillLayerColor(config.fontColor);
			build.config.layerUsesLiveTileRgb = {{
				EffectUsesLiveTileRgb(config.shadow),
				EffectUsesLiveTileRgb(config.glow),
				EffectUsesLiveTileRgb(config.outline),
				true
			}};

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
				glyph.baselineOffset = GetGlyphBaselineOffset(runtime, instance.glyph);
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

			const bool drawShadow = !suppressEffects && config.shadow.enabled;
			const bool drawGlow = !suppressEffects && config.glow.enabled;
			const bool drawOutline = !suppressEffects && config.outline.enabled;
			const bool shadowUsesSdf = fillUsesSdf || config.shadow.blur > 0.0f
				|| HardShadowIncludesGlow(config) || HardShadowIncludesOutline(config);
			const bool shadowHasOffset = config.shadow.x != 0.0f
				|| config.shadow.y != 0.0f;
			const NiColorA identity = { 1.0f, 1.0f, 1.0f, 1.0f };
			auto firstLayer = [](UInt8 mask)
			{
				for (UInt8 layer = 0; layer < 4; ++layer)
				{
					if (mask & static_cast<UInt8>(1u << layer))
						return static_cast<AtlasLayer>(layer);
				}
				return AtlasLayer::Fill;
			};

			for (const PreparedShaderGlyph& entry : prepared)
			{
				const NiColorA baseColor = ResolveBaseColor(
					entry.instance->color, tileColor);
				UInt8 sdfLayerMask = 0;
				UInt8 fillLayerMask = 0;
				if (drawGlow)
					sdfLayerMask |= 1u << static_cast<UInt8>(AtlasLayer::Glow);
				if (drawOutline)
					sdfLayerMask |= 1u << static_cast<UInt8>(AtlasLayer::Outline);
				if (fillUsesSdf)
					sdfLayerMask |= 1u << static_cast<UInt8>(AtlasLayer::Fill);
				else
					fillLayerMask |= 1u << static_cast<UInt8>(AtlasLayer::Fill);

				if (drawShadow)
				{
					if (shadowHasOffset)
					{
						AddPendingQuad(quads, shadowUsesSdf ? entry.sdf : entry.fill,
							*entry.instance, baseColor, identity,
							config.shadow.x, config.shadow.y, rasterScale,
							entry.baselineOffset, AtlasLayer::Shadow, true, 0,
							shadowUsesSdf,
							1u << static_cast<UInt8>(AtlasLayer::Shadow));
					}
					else if (shadowUsesSdf)
						sdfLayerMask |= 1u << static_cast<UInt8>(AtlasLayer::Shadow);
					else
						fillLayerMask |= 1u << static_cast<UInt8>(AtlasLayer::Shadow);
				}

				if (sdfLayerMask)
				{
					AddPendingQuad(quads, entry.sdf, *entry.instance,
						baseColor, identity, 0.0f, 0.0f, rasterScale,
						entry.baselineOffset, firstLayer(sdfLayerMask), true, 0,
						true, sdfLayerMask);
				}
				if (fillLayerMask)
				{
					AddPendingQuad(quads, entry.fill, *entry.instance,
						baseColor, identity, 0.0f, 0.0f, rasterScale,
						entry.baselineOffset, firstLayer(fillLayerMask), true, 0,
						false, fillLayerMask);
				}
			}
			for (const PendingQuad& quad : quads)
			{
				UInt8 mask = quad.layerMask;
				while (mask)
				{
					build.drawQuadCount += mask & 1u;
					mask >>= 1;
				}
			}
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

		UInt64 BuildPacketTemplateHash(const BatchTemplateKey& geometryKey,
			const std::vector<PendingQuad>& quads,
			const A8EffectShapeConfig& effect)
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
			add(&geometryKey.atlasIdentity, sizeof(geometryKey.atlasIdentity));
			add(&geometryKey.contentHash, sizeof(geometryKey.contentHash));
			add(&geometryKey.generation, sizeof(geometryKey.generation));
			add(&geometryKey.quadCount, sizeof(geometryKey.quadCount));
			add(&effect.enabled, sizeof(effect.enabled));
			add(&effect.shaderEffects, sizeof(effect.shaderEffects));
			add(&effect.useOriginalShader, sizeof(effect.useOriginalShader));
			add(&effect.fillUsesSdf, sizeof(effect.fillUsesSdf));
			add(&effect.quality, sizeof(effect.quality));
			for (const PendingQuad& quad : quads)
				add(&quad.baseColor, sizeof(quad.baseColor));
			const std::array<float, 12> scalars = {
				effect.inverseAtlasWidth, effect.inverseAtlasHeight,
				effect.sdfSpreadPixels, effect.shadowBlurPixels,
				effect.shadowPower, effect.shadowGlowAlpha,
				effect.shadowOutlineAlpha, effect.glowInnerPixels,
				effect.glowOuterPixels, effect.glowPower,
				effect.outlineWidthPixels, effect.outlineSoftnessPixels
			};
			add(scalars.data(), scalars.size() * sizeof(float));
			const size_t pageCount = effect.atlasInverseSizes.size();
			add(&pageCount, sizeof(pageCount));
			for (const NiPoint2& inverseSize : effect.atlasInverseSizes)
				add(&inverseSize, sizeof(inverseSize));
			const size_t rangeCount = effect.ranges.size();
			add(&rangeCount, sizeof(rangeCount));
			for (const A8DrawRange& range : effect.ranges)
			{
				add(&range.firstVertex, sizeof(range.firstVertex));
				add(&range.vertexCount, sizeof(range.vertexCount));
				add(&range.startIndex, sizeof(range.startIndex));
				add(&range.primitiveCount, sizeof(range.primitiveCount));
				add(&range.layer, sizeof(range.layer));
				add(&range.atlasPage, sizeof(range.atlasPage));
				add(&range.usesSdf, sizeof(range.usesSdf));
				add(&range.usesLiveTileRgb, sizeof(range.usesLiveTileRgb));
				add(&range.layerColorModifier, sizeof(range.layerColorModifier));
			}
			return hash;
		}

		std::shared_ptr<const BatchTemplate> GetBatchTemplate(Font& font,
			const std::vector<PendingQuad>& quads,
			const std::vector<std::shared_ptr<AtlasResource>>& atlases,
			const BatchTemplateKey& key, const NiPoint3& origin)
		{
			AtlasState& state = State();
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
			ThisStdCall(0xA7EE30, &result->bound,
				static_cast<UInt16>(result->vertices.size()), result->vertices.data());


			const size_t bytes = result->vertices.size() * sizeof(NiPoint3)
				+ result->texture.size() * sizeof(NiPoint2)
				+ result->indices.size() * sizeof(UInt16) + sizeof(result->bound);
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
			const BatchTemplateKey batchKey =
				BuildBatchTemplateKey(fingerprint, atlases);
			const std::shared_ptr<const BatchTemplate> batch =
				GetBatchTemplate(font, quads, atlases, batchKey, origin);
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
			if (!data->m_pkColor)
				data->m_pkColor = NiAlloc<NiColorA>(data->m_usVertices);
			if (!data->m_pkColor)
				return nullptr;
			for (size_t quadIndex = 0; quadIndex < quads.size(); ++quadIndex)
			{
				const NiColorA baseColor = SanitizeColor(quads[quadIndex].baseColor);
				std::fill_n(data->m_pkColor + quadIndex * 4, 4, baseColor);
			}
			data->m_kBound = batch->bound;
			data->m_kBound.m_kCenter.x += origin.x;
			data->m_kBound.m_kCenter.y += origin.y;
			data->m_kBound.m_kCenter.z += origin.z;
			const UInt8 fillLayerBit = 1u << static_cast<UInt8>(AtlasLayer::Fill);
			const bool hasEffectLayer = std::any_of(quads.begin(), quads.end(),
				[fillLayerBit](const PendingQuad& quad)
				{
					return (quad.layerMask & ~fillLayerBit) != 0;
				});
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
				const UInt64 packetTemplateHash =
					BuildPacketTemplateHash(batchKey, quads, resolvedEffect);
				if (!PrepareA8AtlasShape(font, shape, font.iFontNum,
					static_cast<UInt32>(std::count_if(quads.begin(), quads.end(),
						[fillLayerBit](const PendingQuad& quad)
						{
							return (quad.layerMask & fillLayerBit) != 0;
						})),
					static_cast<UInt32>(quads.size()), &resolvedEffect, &colorContract,
					packetTemplateHash, origin))
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
				const UInt32 lhsRank = GetA8LayerDrawRank(
					static_cast<UInt32>(lhs.layer));
				const UInt32 rhsRank = GetA8LayerDrawRank(
					static_cast<UInt32>(rhs.layer));
				if (lhsRank != rhsRank)
					return lhsRank < rhsRank;
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

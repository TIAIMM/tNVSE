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

		UInt32 PackNativeColorChannel(float value)
		{
			value = std::clamp(SanitizeColorChannel(value), 0.0f, 1.0f);
			return static_cast<UInt32>(value * 255.0f + 0.5f);
		}

		UInt32 PackNativeBaseColor(const NiColorA& color)
		{
			const NiColorA safe = SanitizeColor(color);
			return (PackNativeColorChannel(safe.a) << 24)
				| (PackNativeColorChannel(safe.r) << 16)
				| (PackNativeColorChannel(safe.g) << 8)
				| PackNativeColorChannel(safe.b);
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
						|| config.ranges.back().sdfSpreadPixels
							!= static_cast<float>(quad.bitmap->sdfSpread)
						|| config.ranges.back().sourceToLogicalScale
							!= quad.sourceToLogicalScale
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
						range.sdfSpreadPixels =
							static_cast<float>(quad.bitmap->sdfSpread);
						range.sourceToLogicalScale =
							quad.sourceToLogicalScale;
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
			std::vector<PendingQuad>& quads)
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
					auto baked = std::make_shared<GlyphBitmap>();
					baked->cacheId = quad.bitmap->cacheId;
					baked->atlasRgb = quad.bitmap->atlasRgb;
					baked->width = quad.bitmap->width;
					baked->height = quad.bitmap->height;
					baked->left = quad.bitmap->left;
					baked->top = quad.bitmap->top;
					baked->effectiveWidth = quad.bitmap->effectiveWidth;
					baked->effectiveHeight = quad.bitmap->effectiveHeight;
					baked->maskType = quad.bitmap->maskType;
					baked->distanceFieldMethod =
						quad.bitmap->distanceFieldMethod;
					baked->sdfSpread = quad.bitmap->sdfSpread;
					baked->strokeWidth26Dot6 = quad.bitmap->strokeWidth26Dot6;
					baked->colorBaked = quad.bitmap->colorBaked;
					baked->bakedRgba = quad.bitmap->bakedRgba;
					baked->bakedLayer = quad.bitmap->bakedLayer;
					baked->alpha = quad.bitmap->alpha;
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
					baked->cpuMemory.Reset(CpuMemoryCategory::GlyphBitmap,
						sizeof(GlyphBitmap) + baked->alpha.capacity());
					found = unique.emplace(bakedId, std::move(baked)).first;
				}
				quad.bitmap = found->second;
				quad.baseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
				quad.layerColorModifier = { 1.0f, 1.0f, 1.0f, 1.0f };
				quad.usesLiveTileRgb = true;
				quad.atlasPlacement = {};
			}
		}


		void AddPendingQuad(std::vector<PendingQuad>& quads,
			const std::shared_ptr<const GlyphBitmap>& bitmap,
			const AtlasGlyphInstance& instance, const NiColorA& baseColor,
			const NiColorA& layerColorModifier,
			float offsetX, float offsetY, float rasterScale, float baselineOffset,
			AtlasLayer layer, bool usesLiveTileRgb,
			UInt32 expansionPixels = 0, bool usesSdf = false, UInt8 layerMask = 0,
			float sourceToLogicalScale = 1.0f)
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
				quad.sourceToLogicalScale = sourceToLogicalScale;
				quad.logicalTopEdge = instance.glyph.metrics
					? instance.glyph.metrics->fTopEdge : 0.0f;
				quad.baselineOffset = baselineOffset;
				quad.expansionPixels = expansionPixels;
				quad.layer = layer;
				quad.layerMask = layerMask ? layerMask
					: static_cast<UInt8>(1u << static_cast<UInt8>(layer));
				quad.byteClass = instance.glyph.byteClass;
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
			build.config.distanceFieldMethod =
				GetConfiguredDistanceFieldMethod();
			build.config.quality = quality;
			const FontConfig& config = GetRuntimeConfig(runtime);
			UInt32 sdfSpread = 0;
			if (!ResolveSdfSpread(
				config, rasterScale, sdfSpread, !suppressEffects))
			{
				if (g_bEnableFreeTypeFontRenderingLog)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: %s spread unsupported font=%u scale=%.3f glowOuter=%.3f outline=%.3f softness=%.3f shadowBlur=%.3f; using CPU effects",
						GetConfiguredDistanceFieldMethodName(),
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
				std::shared_ptr<const GlyphBitmap> sdf;
				float baselineOffset = 0.0f;
				float sourceToLogicalScale = 1.0f;
			};
			thread_local std::vector<PreparedShaderGlyph> prepared;
			thread_local std::vector<GlyphBitmapRequest> bitmapRequests;
			thread_local std::vector<std::shared_ptr<const GlyphBitmap>> bitmapResults;
			prepared.clear();
			prepared.reserve(glyphs.size());
			bitmapRequests.clear();
			bitmapRequests.reserve(glyphs.size());
			for (const AtlasGlyphInstance& instance : glyphs)
			{
				PreparedShaderGlyph glyph;
				glyph.instance = &instance;
				glyph.baselineOffset = GetGlyphBaselineOffset(runtime, instance.glyph);
				MtsdfSharedRasterProfile profile;
				if (!ResolveMtsdfSharedRasterProfile(config,
					instance.glyph.byteClass, rasterScale, true, profile))
				{
					return false;
				}
				glyph.sourceToLogicalScale = profile.sourceToLogicalScale;
				bitmapRequests.push_back({ &instance.glyph,
					GlyphMaskType::DistanceField, profile.sdfSpread });
				prepared.push_back(std::move(glyph));
			}
			GetAtlasBackedGlyphBitmaps(runtime, bitmapRequests, rasterScale,
				GetConfiguredDistanceFieldAtlasPixelMode(),
				AtlasRenderMode::ShaderEffects,
				kDistanceFieldAtlasPadding, bitmapResults);
			size_t bitmapIndex = 0;
			for (PreparedShaderGlyph& glyph : prepared)
			{
				glyph.sdf = bitmapResults[bitmapIndex++];
				if (!glyph.sdf)
					return false;
			}

			const bool drawShadow = !suppressEffects && config.shadow.enabled;
			const bool drawGlow = !suppressEffects && config.glow.enabled;
			const bool drawOutline = !suppressEffects && config.outline.enabled;
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
				if (drawGlow)
					sdfLayerMask |= 1u << static_cast<UInt8>(AtlasLayer::Glow);
				if (drawOutline)
					sdfLayerMask |= 1u << static_cast<UInt8>(AtlasLayer::Outline);
				sdfLayerMask |= 1u << static_cast<UInt8>(AtlasLayer::Fill);

				if (drawShadow)
				{
					if (shadowHasOffset)
					{
						AddPendingQuad(quads, entry.sdf,
							*entry.instance, baseColor, identity,
							config.shadow.x, config.shadow.y, rasterScale,
							entry.baselineOffset, AtlasLayer::Shadow, true, 0,
							true,
							1u << static_cast<UInt8>(AtlasLayer::Shadow),
							entry.sourceToLogicalScale);
					}
					else
						sdfLayerMask |= 1u << static_cast<UInt8>(AtlasLayer::Shadow);
				}

				if (sdfLayerMask)
				{
					AddPendingQuad(quads, entry.sdf, *entry.instance,
						baseColor, identity, 0.0f, 0.0f, rasterScale,
						entry.baselineOffset, firstLayer(sdfLayerMask), true, 0,
						true, sdfLayerMask, entry.sourceToLogicalScale);
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
				add(&quad.sourceToLogicalScale,
					sizeof(quad.sourceToLogicalScale));
				add(&quad.baselineOffset, sizeof(quad.baselineOffset));
				add(&quad.expansionPixels, sizeof(quad.expansionPixels));
				add(&quad.usesSdf, sizeof(quad.usesSdf));
				add(&quad.atlasPage, sizeof(quad.atlasPage));
				add(&quad.atlasPlacement.atlasIdentity,
					sizeof(quad.atlasPlacement.atlasIdentity));
				add(&quad.atlasPlacement.atlasGeneration,
					sizeof(quad.atlasPlacement.atlasGeneration));
				add(&quad.atlasPlacement.u0, sizeof(quad.atlasPlacement.u0));
				add(&quad.atlasPlacement.v0, sizeof(quad.atlasPlacement.v0));
				add(&quad.atlasPlacement.u1, sizeof(quad.atlasPlacement.u1));
				add(&quad.atlasPlacement.v1, sizeof(quad.atlasPlacement.v1));
			}
			return { hash, static_cast<UInt32>(quads.size()) };
		}

		TextArtifactKey BuildTextArtifactKey(const QuadBatchFingerprint& fingerprint,
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

		UInt64 BuildTextArtifactContentHash(const TextArtifactKey& geometryKey,
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
			add(&effect.distanceFieldMethod,
				sizeof(effect.distanceFieldMethod));
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
				add(&range.sdfSpreadPixels, sizeof(range.sdfSpreadPixels));
				add(&range.sourceToLogicalScale,
					sizeof(range.sourceToLogicalScale));
				add(&range.layerColorModifier, sizeof(range.layerColorModifier));
			}
			return hash;
		}

		bool TextArtifactMatchesAtlases(const NativeA8PayloadTemplate& artifact,
			const std::vector<std::shared_ptr<AtlasResource>>& atlases)
		{
			if (artifact.pageCount != atlases.size()
				|| artifact.atlasProperties.size() != atlases.size()
				|| artifact.atlasTextures.size() != atlases.size())
				return false;
			for (size_t page = 0; page < atlases.size(); ++page)
			{
				if (!atlases[page]
					|| artifact.atlasProperties[page].m_pObject
						!= atlases[page]->property.m_pObject
					|| artifact.atlasTextures[page].m_pObject
						!= GetAtlasTexture(*atlases[page]))
					return false;
			}
			return true;
		}

		NativeA8PayloadTemplatePtr GetNativeTextArtifact(Font& font,
			const std::vector<PendingQuad>& quads,
			const std::vector<std::shared_ptr<AtlasResource>>& atlases,
			const TextArtifactKey& key, const NiPoint3& origin,
			const A8EffectShapeConfig& effects)
		{
			AtlasState& state = State();
			{
				std::lock_guard<std::mutex> lock(state.textArtifactMutex);
				auto existing = state.textArtifactCache.find(key);
				if (existing != state.textArtifactCache.end())
				{
					if (existing->second.data
						&& TextArtifactMatchesAtlases(*existing->second.data,
							atlases))
					{
						state.textArtifactLru.splice(state.textArtifactLru.begin(),
							state.textArtifactLru,
							existing->second.lru);
						existing->second.lru = state.textArtifactLru.begin();
						RecordFreeTypePerf(FreeTypePerfCounter::TextArtifactHit);
						return existing->second.data;
					}
					state.textArtifactCacheBytes -= std::min(
						state.textArtifactCacheBytes,
						existing->second.bytes);
					state.textArtifactLru.erase(existing->second.lru);
					state.textArtifactCache.erase(existing);
				}
			}
			RecordFreeTypePerf(FreeTypePerfCounter::TextArtifactMiss);

			std::vector<NativeA8GpuVertex> vertices(quads.size() * 4);
			NiPoint3 boundMinimum(std::numeric_limits<float>::max(),
				std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
			NiPoint3 boundMaximum(std::numeric_limits<float>::lowest(),
				std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());
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
				const float scale = quad.rasterScale;
				const float sourcePixelToLogical =
					quad.sourceToLogicalScale / scale;
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
					? logicalX + bitmapLeft * sourcePixelToLogical
					: std::round(logicalX * scale + bitmapLeft) / scale;
				const float z0 = quad.usesSdf
					? logicalZ + bitmapTop * sourcePixelToLogical
					: std::round(logicalZ * scale + bitmapTop) / scale;
				const float x1 = x0 + (static_cast<float>(quad.bitmap->width)
					+ expansion * 2.0f) * (quad.usesSdf
						? sourcePixelToLogical : 1.0f / scale);
				const float z1 = z0 - (static_cast<float>(quad.bitmap->height)
					+ expansion * 2.0f) * (quad.usesSdf
						? sourcePixelToLogical : 1.0f / scale);
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
						const float bitmapTop = static_cast<float>(quad.bitmap->top)
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
				const UInt32 base = index * 4;
				const UInt32 packedColor = PackNativeBaseColor(quad.baseColor);
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
					NativeA8GpuVertex& output = vertices[base + ordinal];
					output = { position.x, position.y, position.z,
						uv.x, uv.y, packedColor };
					boundMinimum.x = std::min(boundMinimum.x, position.x);
					boundMinimum.y = std::min(boundMinimum.y, position.y);
					boundMinimum.z = std::min(boundMinimum.z, position.z);
					boundMaximum.x = std::max(boundMaximum.x, position.x);
					boundMaximum.y = std::max(boundMaximum.y, position.y);
					boundMaximum.z = std::max(boundMaximum.z, position.z);
				}
			}
			NiBound bound;
			bound.m_kCenter = NiPoint3(
				(boundMinimum.x + boundMaximum.x) * 0.5f,
				(boundMinimum.y + boundMaximum.y) * 0.5f,
				(boundMinimum.z + boundMaximum.z) * 0.5f);
			float radiusSquared = 0.0f;
			for (const NativeA8GpuVertex& vertex : vertices)
			{
				const float dx = vertex.x - bound.m_kCenter.x;
				const float dy = vertex.y - bound.m_kCenter.y;
				const float dz = vertex.z - bound.m_kCenter.z;
				radiusSquared = std::max(radiusSquared,
					dx * dx + dy * dy + dz * dz);
			}
			bound.m_fRadius = std::sqrt(radiusSquared);
			NativeA8PayloadTemplatePtr result = BuildNativeA8PayloadTemplate(
				std::move(vertices), static_cast<UInt32>(quads.size()), effects,
				bound);
			if (!result)
				return {};
			const size_t bytes = GetNativeA8PayloadTemplateBytes(*result);
			{
				std::lock_guard<std::mutex> lock(state.textArtifactMutex);
				auto existing = state.textArtifactCache.find(key);
				if (existing != state.textArtifactCache.end())
				{
					if (existing->second.data
						&& TextArtifactMatchesAtlases(*existing->second.data,
							atlases))
					{
						state.textArtifactLru.splice(state.textArtifactLru.begin(),
							state.textArtifactLru,
							existing->second.lru);
						existing->second.lru = state.textArtifactLru.begin();
						return existing->second.data;
					}
					state.textArtifactCacheBytes -= std::min(
						state.textArtifactCacheBytes,
						existing->second.bytes);
					state.textArtifactLru.erase(existing->second.lru);
					state.textArtifactCache.erase(existing);
				}
				state.textArtifactLru.push_front(key);
				const auto [inserted, success] = state.textArtifactCache.emplace(key,
					TextArtifactEntry{ result, bytes,
						state.textArtifactLru.begin() });
				if (!success)
				{
					state.textArtifactLru.pop_front();
					state.textArtifactLru.splice(state.textArtifactLru.begin(),
						state.textArtifactLru,
						inserted->second.lru);
					inserted->second.lru = state.textArtifactLru.begin();
					return inserted->second.data;
				}
				inserted->second.cpuMemory.Reset(CpuMemoryCategory::TextArtifact,
					sizeof(TextArtifactEntry) + 2u * sizeof(TextArtifactKey)
						+ 4u * sizeof(void*));
				state.textArtifactCacheBytes += bytes;
				TrimTextArtifactCache(state);
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
			const bool needsNativeRangeRouting = useCustomA8Shader || atlases.size() > 1;
			A8EffectShapeConfig resolvedEffect = effectConfig
				? *effectConfig : A8EffectShapeConfig{};
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
			const TextArtifactKey geometryKey = BuildTextArtifactKey(
				fingerprint, atlases);
			const UInt64 artifactHash = BuildTextArtifactContentHash(
				geometryKey, quads, resolvedEffect);
			TextArtifactKey artifactKey = geometryKey;
			artifactKey.contentHash = artifactHash;
			const NativeA8PayloadTemplatePtr artifact = GetNativeTextArtifact(
				font, quads, atlases, artifactKey, origin, resolvedEffect);
			if (!artifact || artifact->gpuVertices.size() != quads.size() * 4u)
				return nullptr;

			NiTriShape* shape = font.MakeTriShape(
				static_cast<int>(needsNativeRangeRouting ? 1u : quads.size()),
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
						const NativeA8GpuVertex& vertex = artifact->gpuVertices[index];
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
				// The stock accumulator still validates and prepares this one-quad
				// facade before the sorted Tile hook substitutes the shared proxy.
				// Give it a complete, finite quad instead of leaving MakeTriShape's
				// transient arrays unspecified.
				if (data->m_usVertices < 4 || !data->m_pkVertex
					|| !data->m_pkTexture || !data->m_pusTriList)
				{
					return nullptr;
				}
				static constexpr UInt16 kFacadeQuad[6] = { 0, 2, 1, 0, 3, 2 };
				const NiColorA baseColor = SanitizeColor(quads.front().baseColor);
				for (UInt32 ordinal = 0; ordinal < 4; ++ordinal)
				{
					const NativeA8GpuVertex& vertex = artifact->gpuVertices[ordinal];
					data->m_pkVertex[ordinal] = NiPoint3(vertex.x + origin.x,
						vertex.y + origin.y, vertex.z + origin.z);
					data->m_pkTexture[ordinal] = NiPoint2(vertex.u, vertex.v);
					data->m_pkColor[ordinal] = baseColor;
				}
				std::copy(std::begin(kFacadeQuad), std::end(kFacadeQuad),
					data->m_pusTriList);
			}
			const UInt8 fillLayerBit = 1u << static_cast<UInt8>(AtlasLayer::Fill);
			if (needsNativeRangeRouting)
			{
				const A8ShapeColorContract colorContract = BuildColorContract(quads);
				if (!PrepareA8AtlasShape(font, shape, font.iFontNum,
					static_cast<UInt32>(std::count_if(quads.begin(), quads.end(),
						[fillLayerBit](const PendingQuad& quad)
						{
							return (quad.layerMask & fillLayerBit) != 0;
						})),
					static_cast<UInt32>(quads.size()), &resolvedEffect, &colorContract,
					artifact, origin))
				{
					return nullptr;
				}
			}
			// PrepareObject establishes the engine-side submission state even when
			// native rendering later replaces this facade with a shared proxy.
			if (prepareObject)
				shape->PrepareObject();
			// The facade carries only one dummy quad on the native path. Restore the
			// immutable artifact bound after stock preparation so culling never sees
			// the dummy geometry's extent.
			data->m_kBound = artifact->bound;
			data->m_kBound.m_kCenter.x += origin.x;
			data->m_kBound.m_kCenter.y += origin.y;
			data->m_kBound.m_kCenter.z += origin.z;
			// PrepareObject may have propagated the one-quad facade bound to the AV
			// object. Refresh the already-created world bound from the restored full
			// artifact bound so stock accumulator culling cannot discard the text
			// before the sorted Tile route gets a chance to substitute its proxy.
			if (prepareObject && shape->m_pWorldBound)
				shape->UpdateWorldBound();
			return shape;
		}

		NiTriShape* TryCreateAtlasShapeForMode(Font& font,
			const std::vector<PendingQuad>& quads,
			const FontConfig& config, float rasterScale, bool prepareObject,
			AtlasPixelMode pixelMode, AtlasRenderMode renderMode, UInt32 padding,
			std::vector<std::shared_ptr<AtlasResource>>& outAtlases,
			const NiColorA& tileColor, bool useCustomA8Shader,
			const A8EffectShapeConfig* effectConfig)
		{
			if (quads.empty())
				return nullptr;
			if ((pixelMode == AtlasPixelMode::A8
				|| pixelMode == AtlasPixelMode::Mtsdf32) && !useCustomA8Shader)
				return nullptr;
			const std::vector<PendingQuad>* activeQuads = &quads;
			thread_local std::vector<PendingQuad> bakedQuads;
			if (pixelMode == AtlasPixelMode::Argb32 && !useCustomA8Shader)
			{
				BuildBakedArgbFallback(quads, bakedQuads);
				activeQuads = &bakedQuads;
			}

			struct ResolvedPlacement
			{
				UInt16 page = std::numeric_limits<UInt16>::max();
				AtlasGlyphPlacement placement;
			};
			thread_local std::array<std::unordered_map<UInt64,
				std::shared_ptr<const GlyphBitmap>>, 2> roleUnique;
			thread_local std::array<std::vector<std::shared_ptr<const GlyphBitmap>>, 2>
				roleBitmaps;
			thread_local std::array<std::unordered_map<UInt64, ResolvedPlacement>, 2>
				placementRecords;
			for (size_t roleIndex = 0; roleIndex < roleUnique.size(); ++roleIndex)
			{
				roleUnique[roleIndex].clear();
				roleBitmaps[roleIndex].clear();
				placementRecords[roleIndex].clear();
			}
			for (const PendingQuad& quad : *activeQuads)
			{
				if (quad.bitmap)
				{
					roleUnique[static_cast<size_t>(quad.byteClass)].emplace(
						quad.bitmap->cacheId, quad.bitmap);
				}
			}
			std::vector<std::shared_ptr<AtlasResource>> availableAtlases;
			for (size_t roleIndex = 0; roleIndex < roleBitmaps.size(); ++roleIndex)
			{
				auto& bitmaps = roleBitmaps[roleIndex];
				bitmaps.reserve(roleUnique[roleIndex].size());
				for (auto& [id, bitmap] : roleUnique[roleIndex])
					bitmaps.push_back(bitmap);
				std::sort(bitmaps.begin(), bitmaps.end(), [](const auto& lhs, const auto& rhs)
				{
					return lhs->cacheId < rhs->cacheId;
				});
				if (bitmaps.empty())
					continue;
				thread_local std::vector<UInt16> bitmapPageOrdinals;
				const VectorFontByteClass byteClass =
					static_cast<VectorFontByteClass>(roleIndex);
				std::vector<std::shared_ptr<AtlasResource>> roleAtlases =
					GetAtlasResources(config, byteClass, rasterScale, bitmaps, pixelMode,
						renderMode, padding, &bitmapPageOrdinals);
				if (roleAtlases.empty())
				{
					std::shared_ptr<AtlasResource> transient = CreateTransientAtlas(
						bitmaps, pixelMode, renderMode, padding);
					if (transient)
					{
						roleAtlases.push_back(std::move(transient));
						bitmapPageOrdinals.assign(bitmaps.size(),
							std::numeric_limits<UInt16>::max());
						for (size_t bitmapIndex = 0; bitmapIndex < bitmaps.size(); ++bitmapIndex)
						{
							if (bitmaps[bitmapIndex]
								&& FindAtlasGlyph(*roleAtlases[0],
									bitmaps[bitmapIndex]->cacheId))
							{
								bitmapPageOrdinals[bitmapIndex] = 0;
							}
						}
					}
				}
				if (roleAtlases.empty())
					return nullptr;
				const UInt16 rolePageBase = static_cast<UInt16>(availableAtlases.size());
				availableAtlases.insert(availableAtlases.end(), roleAtlases.begin(),
					roleAtlases.end());
				for (size_t bitmapIndex = 0; bitmapIndex < bitmaps.size(); ++bitmapIndex)
				{
					const UInt16 page = bitmapPageOrdinals[bitmapIndex];
					if (page >= roleAtlases.size() || !bitmaps[bitmapIndex])
						continue;
					const UInt16 resolvedPage = static_cast<UInt16>(rolePageBase + page);
					AtlasGlyphRecord* glyph = FindAtlasGlyph(*roleAtlases[page],
						bitmaps[bitmapIndex]->cacheId);
					if (!glyph || !CacheAtlasGlyphPlacement(
						*glyph, *roleAtlases[page], resolvedPage))
					{
						return nullptr;
					}
					placementRecords[roleIndex].emplace(bitmaps[bitmapIndex]->cacheId,
						ResolvedPlacement{ resolvedPage, glyph->placement });
				}
			}
			if (availableAtlases.empty())
				return nullptr;
			if (!useCustomA8Shader && availableAtlases.size() > 1)
			{
				// Stock Tile geometry can bind only one texture. The no-loader ARGB
				// route therefore collapses just this text unit into one transient
				// atlas instead of accidentally entering the native multi-page route.
				// This path is both exceptional and potentially large. Keep its merge
				// list scoped to the collapse operation so a no-loader fallback cannot
				// leave an unbudgeted thread-local capacity behind.
				std::vector<std::shared_ptr<const GlyphBitmap>> fallbackBitmaps;
				fallbackBitmaps.reserve(roleBitmaps[0].size() + roleBitmaps[1].size());
				for (const auto& role : roleBitmaps)
					fallbackBitmaps.insert(fallbackBitmaps.end(), role.begin(), role.end());
				std::sort(fallbackBitmaps.begin(), fallbackBitmaps.end(),
					[](const auto& left, const auto& right)
					{
						return left->cacheId < right->cacheId;
					});
				fallbackBitmaps.erase(std::unique(fallbackBitmaps.begin(),
					fallbackBitmaps.end(), [](const auto& left, const auto& right)
					{
						return left->cacheId == right->cacheId;
					}), fallbackBitmaps.end());
				std::shared_ptr<AtlasResource> collapsed = CreateTransientAtlas(
					fallbackBitmaps, pixelMode, renderMode, padding);
				if (!collapsed)
					return nullptr;
				availableAtlases.assign(1, collapsed);
				for (size_t roleIndex = 0; roleIndex < roleBitmaps.size(); ++roleIndex)
				{
					placementRecords[roleIndex].clear();
					for (const auto& bitmap : roleBitmaps[roleIndex])
					{
						if (!bitmap)
							continue;
						AtlasGlyphRecord* glyph = FindAtlasGlyph(*collapsed, bitmap->cacheId);
						if (!glyph || !CacheAtlasGlyphPlacement(*glyph, *collapsed, 0))
							return nullptr;
						placementRecords[roleIndex][bitmap->cacheId] =
							ResolvedPlacement{ 0, glyph->placement };
					}
				}
			}
			thread_local std::vector<PendingQuad> pagedQuads;
			pagedQuads = *activeQuads;
			const NiPoint3 batchOrigin = pagedQuads.front().pen;
			outAtlases.clear();
			std::vector<UInt16> compactPageIndices(availableAtlases.size(),
				std::numeric_limits<UInt16>::max());
			for (PendingQuad& quad : pagedQuads)
			{
				const auto& rolePlacements = placementRecords[
					static_cast<size_t>(quad.byteClass)];
				const auto placement = rolePlacements.find(quad.bitmap->cacheId);
				if (placement == rolePlacements.end())
					return nullptr;
				const UInt16 page = placement->second.page;
				UInt16& compactPage = compactPageIndices[page];
				if (compactPage == std::numeric_limits<UInt16>::max())
				{
					compactPage = static_cast<UInt16>(outAtlases.size());
					outAtlases.push_back(availableAtlases[page]);
				}
				quad.atlasPage = compactPage;
				quad.atlasPlacement = placement->second.placement;
				quad.atlasPlacement.pageIndex = compactPage;
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

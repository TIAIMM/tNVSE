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
		void AddPendingQuad(std::vector<PendingQuad>& quads,
			const PendingQuad::GlyphSource& source,
			const AtlasGlyphInstance& instance, const NiColorA& baseColor,
			const NiColorA& layerColorModifier,
			float offsetX, float offsetY, float rasterScale, float baselineOffset,
			AtlasLayer layer, bool usesLiveTileRgb,
			UInt32 expansionPixels = 0, bool usesSdf = false, UInt8 layerMask = 0,
			float sourceToLogicalScale = 1.0f,
			UInt32 glyphOrdinal = std::numeric_limits<UInt32>::max())
		{
			if (source.IsDrawable())
			{
				PendingQuad quad;
				quad.source = source;
				quad.pen = instance.pen;
				quad.baseColor = SanitizeColor(baseColor);
				quad.layerColorModifier = SanitizeColor(layerColorModifier);
				quad.offsetX = offsetX;
				quad.offsetY = offsetY;
				quad.rasterScale = rasterScale;
				quad.sourceToLogicalScale = sourceToLogicalScale;
				quad.logicalTopEdge =
					GetVectorGlyphTopEdge(instance.glyph);
				quad.baselineOffset = baselineOffset;
				quad.expansionPixels = expansionPixels;
				quad.layer = layer;
				quad.layerMask = layerMask ? layerMask
					: static_cast<UInt8>(1u << static_cast<UInt8>(layer));
				quad.byteClass = instance.glyph.byteClass;
				quad.usesSdf = usesSdf;
				quad.usesLiveTileRgb = usesLiveTileRgb;
				quad.glyphOrdinal = glyphOrdinal;
				quads.push_back(std::move(quad));
			}
		}

		void AddPendingQuad(std::vector<PendingQuad>& quads,
			const std::shared_ptr<const GlyphBitmap>& bitmap,
			const AtlasGlyphInstance& instance, const NiColorA& baseColor,
			const NiColorA& layerColorModifier,
			float offsetX, float offsetY, float rasterScale, float baselineOffset,
			AtlasLayer layer, bool usesLiveTileRgb,
			UInt32 expansionPixels = 0, bool usesSdf = false, UInt8 layerMask = 0,
			float sourceToLogicalScale = 1.0f,
			UInt32 glyphOrdinal = std::numeric_limits<UInt32>::max())
		{
			PendingQuad::GlyphSource source;
			source.bitmap = bitmap;
			AddPendingQuad(quads, source, instance, baseColor,
				layerColorModifier, offsetX, offsetY, rasterScale,
				baselineOffset, layer, usesLiveTileRgb, expansionPixels,
				usesSdf, layerMask, sourceToLogicalScale, glyphOrdinal);
		}

		bool BuildPendingQuads(RuntimeFont& runtime,
			const std::vector<AtlasGlyphInstance>& glyphs, float rasterScale,
			const std::array<bool, 4>& included, const NiColorA& tileColor,
			bool preferDirectAtlas, std::vector<PendingQuad>& quads,
			PendingQuadBuildFailure& failure)
		{
			struct PreparedGlyph
			{
				const AtlasGlyphInstance* instance = nullptr;
				PendingQuad::GlyphSource fill;
				PendingQuad::GlyphSource shadow;
				PendingQuad::GlyphSource glow;
				PendingQuad::GlyphSource outline;
				float baselineOffset = 0.0f;
			};

			quads.clear();
			failure = PendingQuadBuildFailure::None;
			const FontConfig& config = GetRuntimeConfig(runtime);
			const bool visibleGlow =
				included[static_cast<size_t>(AtlasLayer::Glow)]
				&& config.glow.enabled;
			const bool visibleOutline =
				included[static_cast<size_t>(AtlasLayer::Outline)]
				&& config.outline.enabled;
			const bool visibleShadow =
				included[static_cast<size_t>(AtlasLayer::Shadow)]
				&& config.shadow.enabled;
			const bool visibleFill =
				included[static_cast<size_t>(AtlasLayer::Fill)];
			if (preferDirectAtlas
				&& UsesBakedEffectRoute())
			{
				thread_local std::vector<GlyphBitmapRequest> compositeRequests;
				thread_local std::vector<std::shared_ptr<const GlyphBitmap>>
					compositeBitmaps;
				thread_local std::vector<PendingQuad::GlyphSource>
					compositeSources;
				compositeRequests.clear();
				const bool direct = GetDirectAtlasGlyphSources(runtime,
					glyphs, GlyphMaskType::Composite, rasterScale,
					AtlasPixelMode::Argb32, AtlasRenderMode::CpuEffects,
					kArgbAtlasPadding, compositeSources);
				if (!direct)
				{
					compositeRequests.reserve(glyphs.size());
					for (const AtlasGlyphInstance& instance : glyphs)
					{
						compositeRequests.push_back({
							&instance.glyph, GlyphMaskType::Composite, 0
						});
					}
					GetGlyphBitmaps(runtime, compositeRequests, rasterScale,
						compositeBitmaps);
					compositeSources.assign(compositeBitmaps.size(), {});
					for (size_t index = 0;
						index < compositeBitmaps.size(); ++index)
					{
						compositeSources[index].bitmap =
							compositeBitmaps[index];
					}
				}
				if (compositeSources.size() != glyphs.size())
				{
					failure = PendingQuadBuildFailure::Fill;
					return false;
				}
				quads.reserve(glyphs.size());
				for (size_t index = 0; index < glyphs.size(); ++index)
				{
					const PendingQuad::GlyphSource& source =
						compositeSources[index];
					if (!source.IsAvailable()
						|| (!source.knownEmpty
							&& !source.IsPrecomposedArgb()))
					{
						failure = PendingQuadBuildFailure::Fill;
						return false;
					}
					const AtlasGlyphInstance& instance = glyphs[index];
					AddPendingQuad(quads, source, instance,
						ResolveBaseColor(instance.color, tileColor),
						NiColorA{ 1.0f, 1.0f, 1.0f, 1.0f },
						0.0f, 0.0f, rasterScale,
						GetGlyphBaselineOffset(runtime, instance.glyph),
						AtlasLayer::Fill, true, 0, false,
						1u << static_cast<UInt8>(AtlasLayer::Fill),
						1.0f, static_cast<UInt32>(index));
				}
				return true;
			}
			if (preferDirectAtlas
				&& !UsesBakedEffectRoute())
			{
				thread_local std::array<
					std::vector<PendingQuad::GlyphSource>, 4> directLayers;
				auto loadLayer = [&](AtlasLayer layer, GlyphMaskType mask,
					bool needed)
				{
					std::vector<PendingQuad::GlyphSource>& sources =
						directLayers[static_cast<size_t>(layer)];
					sources.clear();
					return !needed || GetDirectAtlasGlyphSources(runtime,
						glyphs, mask, rasterScale, AtlasPixelMode::A8,
						AtlasRenderMode::CpuEffects,
						kDistanceFieldAtlasPadding, sources);
				};
				const bool direct = loadLayer(AtlasLayer::Shadow,
						GlyphMaskType::Shadow, visibleShadow)
					&& loadLayer(AtlasLayer::Glow,
						GlyphMaskType::Glow, visibleGlow)
					&& loadLayer(AtlasLayer::Outline,
						GlyphMaskType::Outline, visibleOutline)
					&& loadLayer(AtlasLayer::Fill,
						GlyphMaskType::Fill, visibleFill);
				if (direct)
				{
					const UInt32 layerCount =
						static_cast<UInt32>(visibleShadow)
						+ static_cast<UInt32>(visibleGlow)
						+ static_cast<UInt32>(visibleOutline)
						+ static_cast<UInt32>(visibleFill);
					quads.reserve(glyphs.size() * layerCount);
					auto emitLayer = [&](AtlasLayer layer, bool needed,
						float offsetX, float offsetY,
						const NiColorA& layerColor, bool usesLiveTileRgb)
					{
						if (!needed)
							return;
						const auto& sources =
							directLayers[static_cast<size_t>(layer)];
						for (size_t index = 0; index < glyphs.size(); ++index)
						{
							const AtlasGlyphInstance& instance = glyphs[index];
							AddPendingQuad(quads, sources[index], instance,
								ResolveBaseColor(instance.color, tileColor),
								layerColor, offsetX, offsetY, rasterScale,
								GetGlyphBaselineOffset(runtime, instance.glyph),
								layer, usesLiveTileRgb);
						}
					};
					emitLayer(AtlasLayer::Shadow, visibleShadow,
						config.shadow.x, config.shadow.y,
						ResolveEffectLayerColor(config.shadow,
							config.fontColor),
						EffectUsesLiveTileRgb(config.shadow));
					emitLayer(AtlasLayer::Glow, visibleGlow, 0.0f, 0.0f,
						ResolveEffectLayerColor(config.glow,
							config.fontColor),
						EffectUsesLiveTileRgb(config.glow));
					emitLayer(AtlasLayer::Outline, visibleOutline,
						0.0f, 0.0f,
						ResolveEffectLayerColor(config.outline,
							config.fontColor),
						EffectUsesLiveTileRgb(config.outline));
					emitLayer(AtlasLayer::Fill, visibleFill, 0.0f, 0.0f,
						ResolveFillLayerColor(config.fontColor), true);
					return true;
				}
			}
			const bool needsGlowMask = visibleGlow;
			const bool needsOutlineMask = visibleOutline;
			thread_local std::vector<PreparedGlyph> prepared;
			thread_local std::vector<GlyphBitmapRequest> bitmapRequests;
			thread_local std::vector<std::shared_ptr<const GlyphBitmap>> bitmapResults;
			thread_local std::vector<PendingQuad::GlyphSource> sourceResults;
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
				if (visibleShadow)
					bitmapRequests.push_back({ &instance.glyph, GlyphMaskType::Shadow, 0 });
				if (needsGlowMask)
					bitmapRequests.push_back({ &instance.glyph, GlyphMaskType::Glow, 0 });
				if (needsOutlineMask)
					bitmapRequests.push_back({ &instance.glyph, GlyphMaskType::Outline, 0 });
				prepared.push_back(std::move(glyph));
			}
			bool direct = preferDirectAtlas
				&& GetDirectAtlasGlyphSources(runtime, bitmapRequests, rasterScale,
					AtlasPixelMode::A8, AtlasRenderMode::CpuEffects,
					kDistanceFieldAtlasPadding, sourceResults);
			if (!direct)
			{
				GetGlyphBitmaps(runtime, bitmapRequests, rasterScale, bitmapResults);
				sourceResults.assign(bitmapResults.size(), {});
				for (size_t index = 0; index < bitmapResults.size(); ++index)
					sourceResults[index].bitmap = bitmapResults[index];
			}
			size_t bitmapIndex = 0;
			for (PreparedGlyph& glyph : prepared)
			{
				glyph.fill = sourceResults[bitmapIndex++];
				if (!glyph.fill.IsAvailable())
				{
					failure = PendingQuadBuildFailure::Fill;
					return false;
				}
				if (visibleShadow)
				{
					glyph.shadow = sourceResults[bitmapIndex++];
					if (!glyph.shadow.IsAvailable())
					{
						failure = PendingQuadBuildFailure::Shadow;
						return false;
					}
				}
				if (needsGlowMask)
				{
					glyph.glow = sourceResults[bitmapIndex++];
					if (!glyph.glow.IsAvailable() && visibleGlow)
					{
						failure = PendingQuadBuildFailure::Glow;
						return false;
					}
				}
				if (needsOutlineMask)
				{
					glyph.outline = sourceResults[bitmapIndex++];
					if (!glyph.outline.IsAvailable() && visibleOutline)
					{
						failure = PendingQuadBuildFailure::Outline;
						return false;
					}
				}
			}

			// Tile text does not consistently depth-test effect triangles. Submit each
			// complete layer before the next one, with the shadow behind the glow and
			// every effect behind the fill.
			if (visibleShadow)
			{
				for (const PreparedGlyph& glyph : prepared)
				{
					const NiColorA baseColor = ResolveBaseColor(
						glyph.instance->color, tileColor);
					const NiColorA shadowColor = ResolveEffectLayerColor(
						config.shadow, config.fontColor);
					AddPendingQuad(quads, glyph.shadow, *glyph.instance,
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

		bool ConfigureShaderEffectBuild(const FontConfig& config,
			float rasterScale, EffectQuality quality, bool suppressEffects,
			ShaderEffectBuild& build)
		{
			build = {};
			build.config.enabled = true;
			build.config.shaderEffects = true;
			build.config.distanceFieldMethod =
				GetConfiguredDistanceFieldMethod();
			build.config.quality = quality;
			build.config.rasterScale = rasterScale;
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
			if (!suppressEffects && config.shadow.enabled)
			{
				build.config.shadowOffsetX = config.shadow.x;
				build.config.shadowOffsetY = config.shadow.y;
				build.config.shadowOffsetRasterScale = rasterScale;
			}
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
			return true;
		}

		bool BuildShaderEffectQuads(RuntimeFont& runtime,
			const std::vector<AtlasGlyphInstance>& glyphs, float rasterScale,
			EffectQuality quality, const NiColorA& tileColor, bool suppressEffects,
			std::vector<PendingQuad>& quads,
			ShaderEffectBuild& build)
		{
			quads.clear();
			const FontConfig& config = GetRuntimeConfig(runtime);
			if (!ConfigureShaderEffectBuild(config, rasterScale, quality,
				suppressEffects, build))
			{
				return false;
			}

			thread_local std::vector<GlyphBitmapRequest> bitmapRequests;
			thread_local std::vector<std::shared_ptr<const GlyphBitmap>> bitmapResults;
			thread_local std::vector<PendingQuad::GlyphSource> sourceResults;
			std::array<DistanceFieldRasterProfile, 2> rasterProfiles;
			std::array<bool, 2> rasterProfileReady = {};
			for (const AtlasGlyphInstance& instance : glyphs)
			{
				const size_t roleIndex =
					static_cast<size_t>(instance.glyph.byteClass);
				if (roleIndex >= rasterProfiles.size())
					return false;
				if (rasterProfileReady[roleIndex])
					continue;
				if (!ResolveDistanceFieldRasterProfile(config,
					instance.glyph.byteClass, rasterScale, true,
					rasterProfiles[roleIndex]))
				{
					return false;
				}
				rasterProfileReady[roleIndex] = true;
			}
			const bool direct = GetDirectAtlasGlyphSources(runtime,
				glyphs, GlyphMaskType::DistanceField, rasterScale,
				GetConfiguredDistanceFieldAtlasPixelMode(),
				AtlasRenderMode::ShaderEffects,
				kDistanceFieldAtlasPadding, sourceResults);
			if (!direct)
			{
				bitmapRequests.clear();
				bitmapRequests.reserve(glyphs.size());
				for (const AtlasGlyphInstance& instance : glyphs)
				{
					const size_t roleIndex =
						static_cast<size_t>(instance.glyph.byteClass);
					if (roleIndex >= rasterProfiles.size()
						|| !rasterProfileReady[roleIndex])
						return false;
					bitmapRequests.push_back({ &instance.glyph,
						GlyphMaskType::DistanceField,
						rasterProfiles[roleIndex].sdfSpread });
				}
				GetAtlasBackedGlyphBitmaps(runtime, bitmapRequests, rasterScale,
					GetConfiguredDistanceFieldAtlasPixelMode(),
					AtlasRenderMode::ShaderEffects,
					kDistanceFieldAtlasPadding, bitmapResults);
				sourceResults.assign(bitmapResults.size(), {});
				for (size_t index = 0; index < bitmapResults.size(); ++index)
					sourceResults[index].bitmap = bitmapResults[index];
			}
			if (sourceResults.size() != glyphs.size())
				return false;

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

			for (UInt32 glyphOrdinal = 0;
				glyphOrdinal < static_cast<UInt32>(glyphs.size());
				++glyphOrdinal)
			{
				const AtlasGlyphInstance& instance = glyphs[glyphOrdinal];
				const PendingQuad::GlyphSource& source =
					sourceResults[glyphOrdinal];
				if (!source.IsAvailable())
				{
					// Preserve the old all-or-nothing failure surface: callers and
					// diagnostics must never observe a partially built shader batch.
					quads.clear();
					return false;
				}
				const size_t roleIndex =
					static_cast<size_t>(instance.glyph.byteClass);
				if (roleIndex >= rasterProfiles.size()
					|| !rasterProfileReady[roleIndex])
					return false;
				const DistanceFieldRasterProfile& profile =
					rasterProfiles[roleIndex];
				const float baselineOffset =
					GetGlyphBaselineOffset(runtime, instance.glyph);
				const NiColorA baseColor = ResolveBaseColor(
					instance.color, tileColor);
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
						AddPendingQuad(quads, source,
							instance, baseColor, identity,
							config.shadow.x, config.shadow.y, rasterScale,
							baselineOffset, AtlasLayer::Shadow, true, 0,
							true,
							1u << static_cast<UInt8>(AtlasLayer::Shadow),
							profile.sourceToLogicalScale, glyphOrdinal);
					}
					else
						sdfLayerMask |= 1u << static_cast<UInt8>(AtlasLayer::Shadow);
				}

				if (sdfLayerMask)
				{
					AddPendingQuad(quads, source, instance,
						baseColor, identity, 0.0f, 0.0f, rasterScale,
						baselineOffset, firstLayer(sdfLayerMask), true, 0,
						true, sdfLayerMask, profile.sourceToLogicalScale,
						glyphOrdinal);
				}
				if (source.IsDrawable())
				{
					// The emitted masks contain Fill plus every enabled effect. An
					// offset shadow lives in its own quad, but contributes the same
					// one draw that its in-place mask would have contributed.
					build.drawQuadCount += 1u
						+ static_cast<UInt32>(drawShadow)
						+ static_cast<UInt32>(drawGlow)
						+ static_cast<UInt32>(drawOutline);
				}
			}
			RecordFreeTypePerf(FreeTypePerfCounter::
				ShaderEffectSourceValidationElementScanSaved,
				sourceResults.size());
			RecordFreeTypePerf(FreeTypePerfCounter::
				ShaderEffectDrawCountElementScanSaved, quads.size());
			return true;
		}
}

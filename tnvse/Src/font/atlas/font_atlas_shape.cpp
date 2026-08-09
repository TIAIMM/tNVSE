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
#include <memory>

namespace fonthook::vectorfont
{
		struct NiTriShapeDeleter
		{
			void operator()(NiTriShape* shape) const
			{
				if (shape)
					shape->DeleteThis();
			}
		};

		using NiTriShapeOwner = std::unique_ptr<NiTriShape, NiTriShapeDeleter>;

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

		float SanitizeNativeUvBound(float value)
		{
			return std::clamp(
				std::isfinite(value) ? value : 0.0f, 0.0f, 1.0f);
		}

		template <size_t LayerCount>
		UInt32 CountUsedDirectAtlasPages(
			const std::array<std::array<UInt32,
				kMaximumAtlasSnapshotPages>, LayerCount>& counts,
			size_t availablePageCount)
		{
			const size_t pageLimit = std::min(
				availablePageCount,
				static_cast<size_t>(kMaximumAtlasSnapshotPages));
			UInt32 usedPageCount = 0;
			for (size_t page = 0; page < pageLimit; ++page)
			{
				for (size_t layer = 0; layer < LayerCount; ++layer)
				{
					if (!counts[layer][page])
						continue;
					++usedPageCount;
					break;
				}
			}
			return usedPageCount;
		}

		template <class Value, size_t Capacity>
		bool InitializeDirectPagePrefix(
			std::array<Value, Capacity>& values, size_t availablePageCount,
			const Value& value, UInt64& initializationBytesAvoided)
		{
			if (!availablePageCount || availablePageCount > Capacity)
				return false;
			std::fill_n(values.begin(), availablePageCount, value);
			initializationBytesAvoided +=
				(Capacity - availablePageCount) * sizeof(Value);
			return true;
		}

		template <size_t RangeCount>
		bool InitializeDirectQuadCountPrefix(
			std::array<std::array<UInt32,
				kMaximumAtlasSnapshotPages>, RangeCount>& counts,
			size_t availablePageCount, UInt64& initializationBytesAvoided)
		{
			if (!availablePageCount
				|| availablePageCount > kMaximumAtlasSnapshotPages)
			{
				return false;
			}
			for (auto& range : counts)
			{
				std::fill_n(range.begin(), availablePageCount, 0u);
			}
			initializationBytesAvoided += RangeCount
				* (kMaximumAtlasSnapshotPages - availablePageCount)
				* sizeof(UInt32);
			return true;
		}

		template <size_t RangeCount>
		bool DirectQuadRangesCompletelyWritten(
			const std::array<std::array<UInt32,
				kMaximumAtlasSnapshotPages>, RangeCount>& counts,
			const std::array<std::array<UInt32,
				kMaximumAtlasSnapshotPages>, RangeCount>& offsets,
			const std::array<std::array<UInt32,
				kMaximumAtlasSnapshotPages>, RangeCount>& cursors,
			size_t availablePageCount, UInt32 expectedQuadCount)
		{
			const size_t pageLimit = std::min(
				availablePageCount,
				static_cast<size_t>(kMaximumAtlasSnapshotPages));
			// Count builders reject every page outside this live prefix before
			// incrementing; proving the prefix avoids scanning 64 empty slots.
			UInt64 contiguousEnd = 0;
			for (size_t range = 0; range < RangeCount; ++range)
			{
				for (size_t page = 0; page < pageLimit; ++page)
				{
					if (offsets[range][page] != contiguousEnd)
						return false;
					contiguousEnd += counts[range][page];
					if (contiguousEnd > expectedQuadCount
						|| cursors[range][page] != contiguousEnd)
					{
						return false;
					}
				}
			}
			return contiguousEnd == expectedQuadCount;
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
			// The vanilla TILE1000 contract multiplies the texture by c0. A zero
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

		NativeFontShapeColorContract BuildColorContract(const std::vector<PendingQuad>& quads)
		{
			NativeFontShapeColorContract result;
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

		void BuildNativeFontDrawRanges(const std::vector<PendingQuad>& quads,
			NativeFontEffectShapeConfig& config)
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
							!= static_cast<float>(quad.source.SdfSpread())
						|| config.ranges.back().sourceToLogicalScale
							!= quad.sourceToLogicalScale
						|| !SameColorModifier(
							config.ranges.back().layerColorModifier, layerColor))
					{
						NativeFontDrawRange range;
						range.firstVertex = firstVertex;
						range.startIndex = startIndex;
						range.layer = layer;
						range.atlasPage = quad.atlasPage;
						range.usesSdf = quad.usesSdf;
						range.usesLiveTileRgb = usesLiveTileRgb;
						range.sdfSpreadPixels =
							static_cast<float>(quad.source.SdfSpread());
						range.sourceToLogicalScale =
							quad.sourceToLogicalScale;
						range.layerColorModifier = layerColor;
						config.ranges.push_back(range);
					}
					NativeFontDrawRange& range = config.ranges.back();
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
			const NiColorA& tileColor, std::vector<PendingQuad>& quads)
		{
			quads = source;
			std::unordered_map<UInt64, std::shared_ptr<const GlyphBitmap>> unique;
			for (PendingQuad& quad : quads)
			{
				if (!quad.source.bitmap)
					continue;
				const std::shared_ptr<const GlyphBitmap>& sourceBitmap =
					quad.source.bitmap;
				NiColorA compositeColor = ComposeQuadColor(quad);
				if (!quad.usesLiveTileRgb)
				{
					// TILE1000 unconditionally multiplies the sampled texture by
					// live c0. Pre-compensate fixed effect RGB so the vanilla fallback
					// resolves to the same configured color as the native shader.
					compositeColor.r = ResolveModifierChannel(
						compositeColor.r, tileColor.r);
					compositeColor.g = ResolveModifierChannel(
						compositeColor.g, tileColor.g);
					compositeColor.b = ResolveModifierChannel(
						compositeColor.b, tileColor.b);
				}
				const UInt32 rgba = PackColorModifierRgba(compositeColor);
				UInt64 bakedId = BuildBakedBitmapId(sourceBitmap->cacheId, rgba);
				bakedId = BuildBakedBitmapId(bakedId, static_cast<UInt8>(quad.layer));
				auto found = unique.find(bakedId);
				if (found == unique.end())
				{
					auto baked = std::make_shared<GlyphBitmap>();
					baked->cacheId = sourceBitmap->cacheId;
					baked->atlasRgb = sourceBitmap->atlasRgb;
					baked->width = sourceBitmap->width;
					baked->height = sourceBitmap->height;
					baked->left = sourceBitmap->left;
					baked->top = sourceBitmap->top;
					baked->effectiveWidth = sourceBitmap->effectiveWidth;
					baked->effectiveHeight = sourceBitmap->effectiveHeight;
					baked->maskType = sourceBitmap->maskType;
					baked->distanceFieldMethod =
						sourceBitmap->distanceFieldMethod;
					baked->sdfSpread = sourceBitmap->sdfSpread;
					baked->strokeWidth26Dot6 = sourceBitmap->strokeWidth26Dot6;
					baked->colorBaked = sourceBitmap->colorBaked;
					baked->bakedRgba = sourceBitmap->bakedRgba;
					baked->bakedLayer = sourceBitmap->bakedLayer;
					baked->pixels = sourceBitmap->pixels;
					baked->cacheId = bakedId;
					baked->atlasRgb = rgba & 0x00FFFFFF;
					baked->colorBaked = true;
					baked->bakedRgba = rgba;
					baked->bakedLayer = static_cast<UInt8>(quad.layer);
					const float alphaModifier = std::clamp(compositeColor.a, 0.0f, 1.0f);
					for (UInt8& alpha : baked->pixels)
					{
						alpha = static_cast<UInt8>(std::lround(
							static_cast<float>(alpha) * alphaModifier));
					}
					baked->cpuMemory.Reset(CpuMemoryCategory::GlyphBitmap,
						sizeof(GlyphBitmap) + baked->pixels.capacity());
					found = unique.emplace(bakedId, std::move(baked)).first;
				}
				quad.source = {};
				quad.source.bitmap = found->second;
				quad.baseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
				quad.layerColorModifier = { 1.0f, 1.0f, 1.0f, 1.0f };
				quad.usesLiveTileRgb = true;
				quad.atlasPlacement = {};
			}
		}


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
			if (sourceResults.size() != glyphs.size()
				|| std::any_of(sourceResults.begin(), sourceResults.end(),
					[](const PendingQuad::GlyphSource& source)
					{
						return !source.IsAvailable();
					}))
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

		bool WriteDirectQuadVertices(const AtlasSnapshotPlacement& source,
			const NiPoint3& pen, const NiPoint3& origin,
			float offsetX, float offsetY, float rasterScale,
			float baselineOffset, float sourceToLogicalScale, bool usesSdf,
			UInt32 packedColor, UInt8 layerMask,
			NativeFontGpuVertex* output, NiPoint3& boundMinimum,
			NiPoint3& boundMaximum)
		{
			if (!output || !source.cacheId
				|| !source.rect.width || !source.rect.height
				|| !std::isfinite(rasterScale) || rasterScale <= 0.0f
				|| !std::isfinite(sourceToLogicalScale)
				|| sourceToLogicalScale <= 0.0f)
			{
				return false;
			}
			const float sourcePixelToLogical =
				sourceToLogicalScale / rasterScale;
			const float logicalX = pen.x - origin.x + offsetX;
			const float logicalZ = pen.z - origin.z
				+ baselineOffset - offsetY;
			const float bitmapLeft = static_cast<float>(source.left);
			const float bitmapTop = static_cast<float>(source.top);
			const float x0 = usesSdf
				? logicalX + bitmapLeft * sourcePixelToLogical
				: std::round(logicalX * rasterScale + bitmapLeft)
					/ rasterScale;
			const float z0 = usesSdf
				? logicalZ + bitmapTop * sourcePixelToLogical
				: std::round(logicalZ * rasterScale + bitmapTop)
					/ rasterScale;
			const float pixelScale = usesSdf
				? sourcePixelToLogical : 1.0f / rasterScale;
			const float x1 = x0
				+ static_cast<float>(source.rect.width) * pixelScale;
			const float z1 = z0
				- static_cast<float>(source.rect.height) * pixelScale;
			const float depth = pen.y - origin.y;
			const std::array<NiPoint3, 4> positions = {{
				NiPoint3(x0, depth, z0), NiPoint3(x1, depth, z0),
				NiPoint3(x1, depth, z1), NiPoint3(x0, depth, z1)
			}};
			const std::array<NiPoint2, 4> texture = {{
				NiPoint2(source.glyphPlacement.u0, source.glyphPlacement.v0),
				NiPoint2(source.glyphPlacement.u1, source.glyphPlacement.v0),
				NiPoint2(source.glyphPlacement.u1, source.glyphPlacement.v1),
				NiPoint2(source.glyphPlacement.u0, source.glyphPlacement.v1)
			}};
			for (UInt32 ordinal = 0; ordinal < 4; ++ordinal)
			{
				const NiPoint3& position = positions[ordinal];
				const NiPoint2& uv = texture[ordinal];
				if (!std::isfinite(position.x)
					|| !std::isfinite(position.y)
					|| !std::isfinite(position.z)
					|| !std::isfinite(uv.x) || !std::isfinite(uv.y))
				{
					return false;
				}
				output[ordinal] = {
					position.x, position.y, position.z, uv.x, uv.y,
					packedColor, static_cast<float>(source.sdfSpread),
					1.0f / sourceToLogicalScale,
					static_cast<float>(layerMask),
					SanitizeNativeUvBound(source.glyphPlacement.u0),
					SanitizeNativeUvBound(source.glyphPlacement.v0),
					SanitizeNativeUvBound(source.glyphPlacement.u1),
					SanitizeNativeUvBound(source.glyphPlacement.v1)
				};
				boundMinimum.x = std::min(boundMinimum.x, position.x);
				boundMinimum.y = std::min(boundMinimum.y, position.y);
				boundMinimum.z = std::min(boundMinimum.z, position.z);
				boundMaximum.x = std::max(boundMaximum.x, position.x);
				boundMaximum.y = std::max(boundMaximum.y, position.y);
				boundMaximum.z = std::max(boundMaximum.z, position.z);
			}
			return true;
		}

		bool WriteVanillaDirectQuadVertices(const FontLetter& letter,
			const NiPoint3& pen, const NiPoint3& origin,
			float baselineOffset, UInt32 packedColor, UInt8 layerMask,
			NativeFontGpuVertex* output, NiPoint3& boundMinimum,
			NiPoint3& boundMaximum)
		{
			if (!output || letter.iTextureIndex < 0
				|| !std::isfinite(letter.fWidth)
				|| !std::isfinite(letter.fHeight)
				|| !std::isfinite(letter.fLeadingEdge)
				|| !std::isfinite(letter.fTopEdge))
			{
				return false;
			}
			const float x0 = pen.x - origin.x
				+ letter.fLeadingEdge;
			const float z0 = pen.z - origin.z
				+ baselineOffset + letter.fTopEdge;
			const float x1 = x0 + letter.fWidth;
			const float z1 = z0 - letter.fHeight;
			const float depth = pen.y - origin.y;
			const std::array<NiPoint3, 4> positions = {{
				NiPoint3(x0, depth, z0), NiPoint3(x1, depth, z0),
				NiPoint3(x1, depth, z1), NiPoint3(x0, depth, z1)
			}};
			float glyphU0 = std::numeric_limits<float>::max();
			float glyphV0 = std::numeric_limits<float>::max();
			float glyphU1 = std::numeric_limits<float>::lowest();
			float glyphV1 = std::numeric_limits<float>::lowest();
			for (UInt32 ordinal = 0; ordinal < 4; ++ordinal)
			{
				glyphU0 = std::min(glyphU0, letter.pMapping[ordinal].fU);
				glyphV0 = std::min(glyphV0, letter.pMapping[ordinal].fV);
				glyphU1 = std::max(glyphU1, letter.pMapping[ordinal].fU);
				glyphV1 = std::max(glyphV1, letter.pMapping[ordinal].fV);
			}
			for (UInt32 ordinal = 0; ordinal < 4; ++ordinal)
			{
				const UVMap& uv = letter.pMapping[ordinal];
				const NiPoint3& position = positions[ordinal];
				if (!std::isfinite(position.x)
					|| !std::isfinite(position.y)
					|| !std::isfinite(position.z)
					|| !std::isfinite(uv.fU)
					|| !std::isfinite(uv.fV))
				{
					return false;
				}
				output[ordinal] = {
					position.x, position.y, position.z,
					uv.fU, uv.fV, packedColor, 0.0f, 1.0f,
					static_cast<float>(layerMask),
					SanitizeNativeUvBound(glyphU0),
					SanitizeNativeUvBound(glyphV0),
					SanitizeNativeUvBound(glyphU1),
					SanitizeNativeUvBound(glyphV1)
				};
				boundMinimum.x =
					std::min(boundMinimum.x, position.x);
				boundMinimum.y =
					std::min(boundMinimum.y, position.y);
				boundMinimum.z =
					std::min(boundMinimum.z, position.z);
				boundMaximum.x =
					std::max(boundMaximum.x, position.x);
				boundMaximum.y =
					std::max(boundMaximum.y, position.y);
				boundMaximum.z =
					std::max(boundMaximum.z, position.z);
			}
			return true;
		}

		struct CompositeGlyphQuadSource
		{
			UInt32 firstVertex = std::numeric_limits<UInt32>::max();
			UInt16 atlasPage = std::numeric_limits<UInt16>::max();
			UInt8 layerMask = 0;
		};

		bool AppendOneGlyphCompositeQuads(
			std::vector<NativeFontGpuVertex>& vertices,
			const std::vector<CompositeGlyphQuadSource>& sources,
			UInt32 pageCount, const NativeFontEffectShapeConfig& effects,
			NiPoint3& boundMinimum, NiPoint3& boundMaximum,
			std::vector<NativeFontCompositeSpan>& spans)
		{
			spans.clear();
			if (!effects.shaderEffects || sources.empty() || !pageCount)
				return false;
			const UInt32 sourceVertexCount =
				static_cast<UInt32>(vertices.size());
			const bool shiftedShadow =
				(effects.shadowOffsetX != 0.0f
					|| effects.shadowOffsetY != 0.0f);
			if (!std::isfinite(effects.shadowOffsetX)
				|| !std::isfinite(effects.shadowOffsetY))
			{
				return false;
			}
			for (const CompositeGlyphQuadSource& source : sources)
			{
				const UInt64 bodyEnd =
					static_cast<UInt64>(source.firstVertex) + 4u;
				if (!source.layerMask || source.atlasPage >= pageCount
					|| bodyEnd > sourceVertexCount)
				{
					return false;
				}
				const NativeFontGpuVertex& topLeft =
					vertices[source.firstVertex];
				const NativeFontGpuVertex& topRight =
					vertices[source.firstVertex + 1u];
				const NativeFontGpuVertex& bottomRight =
					vertices[source.firstVertex + 2u];
				if (!(topRight.x > topLeft.x)
					|| !(topLeft.z > bottomRight.z))
				{
					return false;
				}
			}

			// The common single-page/no-offset case already has exactly one body
			// quad per glyph. Reuse it without growing the 32-bit text artifact.
			bool canAliasSource = pageCount == 1 && !shiftedShadow
				&& sources.size() * 4u == vertices.size();
			for (size_t index = 0; canAliasSource && index < sources.size();
				++index)
			{
				const CompositeGlyphQuadSource& source = sources[index];
				canAliasSource = source.atlasPage == 0
					&& source.firstVertex == index * 4u
					&& source.layerMask != 0;
			}
			if (canAliasSource)
			{
				spans.push_back({
					0, sourceVertexCount, 0, true
				});
				return true;
			}

			const UInt64 totalVertexCount =
				static_cast<UInt64>(vertices.size())
				+ static_cast<UInt64>(sources.size()) * 4u;
			if (totalVertexCount
				> static_cast<UInt64>(kNativeFontMaximumQuads) * 4u)
			{
				return false;
			}
			vertices.reserve(static_cast<size_t>(totalVertexCount));
			for (UInt16 page = 0; page < pageCount; ++page)
			{
				const UInt32 firstVertex =
					static_cast<UInt32>(vertices.size());
				for (const CompositeGlyphQuadSource& source : sources)
				{
					if (source.atlasPage != page)
						continue;
					std::array<NativeFontGpuVertex, 4> quad = {{
						vertices[source.firstVertex + 0],
						vertices[source.firstVertex + 1],
						vertices[source.firstVertex + 2],
						vertices[source.firstVertex + 3]
					}};
					const float x0 = quad[0].x;
					const float x1 = quad[1].x;
					const float z0 = quad[0].z;
					const float z1 = quad[2].z;
					const float width = x1 - x0;
					const float height = z0 - z1;

					const bool hasShadow = (source.layerMask & 1u) != 0;
					const float shadowX = hasShadow
						? effects.shadowOffsetX : 0.0f;
					const float shadowZ = hasShadow
						? -effects.shadowOffsetY : 0.0f;
					const float unionX0 = std::min(x0, x0 + shadowX);
					const float unionX1 = std::max(x1, x1 + shadowX);
					const float unionZ0 = std::max(z0, z0 + shadowZ);
					const float unionZ1 = std::min(z1, z1 + shadowZ);
					const float uPerLogical =
						(quad[1].u - quad[0].u) / width;
					const float vPerLogical =
						(quad[2].v - quad[1].v) / height;
					const float unionU0 =
						quad[0].u + (unionX0 - x0) * uPerLogical;
					const float unionU1 =
						quad[0].u + (unionX1 - x0) * uPerLogical;
					const float unionV0 =
						quad[0].v + (z0 - unionZ0) * vPerLogical;
					const float unionV1 =
						quad[0].v + (z0 - unionZ1) * vPerLogical;
					const std::array<NiPoint3, 4> positions = {{
						NiPoint3(unionX0, quad[0].y, unionZ0),
						NiPoint3(unionX1, quad[1].y, unionZ0),
						NiPoint3(unionX1, quad[2].y, unionZ1),
						NiPoint3(unionX0, quad[3].y, unionZ1)
					}};
					const std::array<NiPoint2, 4> texture = {{
						NiPoint2(unionU0, unionV0),
						NiPoint2(unionU1, unionV0),
						NiPoint2(unionU1, unionV1),
						NiPoint2(unionU0, unionV1)
					}};
					for (UInt32 ordinal = 0; ordinal < 4; ++ordinal)
					{
						quad[ordinal].x = positions[ordinal].x;
						quad[ordinal].y = positions[ordinal].y;
						quad[ordinal].z = positions[ordinal].z;
						quad[ordinal].u = texture[ordinal].x;
						quad[ordinal].v = texture[ordinal].y;
						quad[ordinal].layerMask =
							static_cast<float>(source.layerMask);
						boundMinimum.x = std::min(
							boundMinimum.x, quad[ordinal].x);
						boundMinimum.y = std::min(
							boundMinimum.y, quad[ordinal].y);
						boundMinimum.z = std::min(
							boundMinimum.z, quad[ordinal].z);
						boundMaximum.x = std::max(
							boundMaximum.x, quad[ordinal].x);
						boundMaximum.y = std::max(
							boundMaximum.y, quad[ordinal].y);
						boundMaximum.z = std::max(
							boundMaximum.z, quad[ordinal].z);
						vertices.push_back(quad[ordinal]);
					}
				}
				const UInt32 vertexCount =
					static_cast<UInt32>(vertices.size()) - firstVertex;
				if (vertexCount)
				{
					spans.push_back({
						firstVertex, vertexCount, page, pageCount == 1
					});
				}
			}
			return !spans.empty();
		}

		void ExtendDirectColorContract(NativeFontShapeColorContract& contract,
			bool& initialized, const NiColorA& source)
		{
			const NiColorA color = SanitizeColor(source);
			if (!initialized)
			{
				contract.minimumModifier = color;
				contract.maximumModifier = color;
				initialized = true;
				return;
			}
			contract.minimumModifier.r =
				std::min(contract.minimumModifier.r, color.r);
			contract.minimumModifier.g =
				std::min(contract.minimumModifier.g, color.g);
			contract.minimumModifier.b =
				std::min(contract.minimumModifier.b, color.b);
			contract.minimumModifier.a =
				std::min(contract.minimumModifier.a, color.a);
			contract.maximumModifier.r =
				std::max(contract.maximumModifier.r, color.r);
			contract.maximumModifier.g =
				std::max(contract.maximumModifier.g, color.g);
			contract.maximumModifier.b =
				std::max(contract.maximumModifier.b, color.b);
			contract.maximumModifier.a =
				std::max(contract.maximumModifier.a, color.a);
		}

		bool BuildDirectVertexBound(
			size_t vertexCount,
			const NiPoint3& minimum, const NiPoint3& maximum,
			NiBound& bound)
		{
			if (!vertexCount
				|| !std::isfinite(minimum.x)
				|| !std::isfinite(minimum.y)
				|| !std::isfinite(minimum.z)
				|| !std::isfinite(maximum.x)
				|| !std::isfinite(maximum.y)
				|| !std::isfinite(maximum.z))
			{
				return false;
			}
			bound.m_kCenter = NiPoint3(
				(minimum.x + maximum.x) * 0.5f,
				(minimum.y + maximum.y) * 0.5f,
				(minimum.z + maximum.z) * 0.5f);
			const float dx = (maximum.x - minimum.x) * 0.5f;
			const float dy = (maximum.y - minimum.y) * 0.5f;
			const float dz = (maximum.z - minimum.z) * 0.5f;
			const float radiusSquared =
				dx * dx + dy * dy + dz * dz;
			bound.m_fRadius = std::sqrt(radiusSquared);
			return std::isfinite(bound.m_fRadius);
		}

		bool PopulateDirectAtlasEffectPages(
			const std::vector<std::shared_ptr<AtlasResource>>& atlases,
			NativeFontEffectShapeConfig& effects)
		{
			if (atlases.empty())
				return false;
			effects.atlasProperties.clear();
			effects.atlasTextures.clear();
			effects.atlasInverseSizes.clear();
			effects.atlasProperties.reserve(atlases.size());
			effects.atlasTextures.reserve(atlases.size());
			effects.atlasInverseSizes.reserve(atlases.size());
			for (const std::shared_ptr<AtlasResource>& atlas : atlases)
			{
				if (!atlas || !atlas->property || !atlas->width
					|| !atlas->height)
				{
					return false;
				}
				NiTexture* texture = GetAtlasTexture(*atlas);
				if (!texture)
					return false;
				effects.atlasProperties.push_back(atlas->property);
				effects.atlasTextures.push_back(texture);
				effects.atlasInverseSizes.push_back(NiPoint2(
					1.0f / static_cast<float>(atlas->width),
					1.0f / static_cast<float>(atlas->height)));
			}
			effects.inverseAtlasWidth = effects.atlasInverseSizes[0].x;
			effects.inverseAtlasHeight = effects.atlasInverseSizes[0].y;
			return true;
		}

		NiTriShape* CreateDirectNativePacketShell(
			const std::shared_ptr<AtlasResource>& atlas,
			const NativeFontPayloadTemplate& payload,
			const NativeFontPacketTemplate& packet,
			const NiColorA& facadeColor, const NiColorA& tileColor,
			const NiPoint3& origin, bool prepareObject)
		{
			const UInt64 vertexEnd = static_cast<UInt64>(packet.firstVertex)
				+ packet.vertexCount;
			if (!atlas || !packet.vertexCount
				|| vertexEnd > payload.gpuVertices.size())
			{
				return nullptr;
			}

			NiTriShape* shape = CreateFreeTypeTextShape(1, tileColor, false,
				atlas->property, GetAtlasTexture(*atlas));
			if (!shape || !shape->GetModelData())
			{
				if (shape)
					shape->DeleteThis();
				return nullptr;
			}
			NiTriShapeData* data = shape->GetModelData();
			if (data->m_usVertices < 4 || !data->m_pkVertex
				|| !data->m_pkTexture || !data->m_pusTriList)
			{
				shape->DeleteThis();
				return nullptr;
			}
			if (!data->m_pkColor)
				data->m_pkColor = NiAlloc<NiColorA>(data->m_usVertices);
			if (!data->m_pkColor)
			{
				shape->DeleteThis();
				return nullptr;
			}

			static constexpr UInt16 kFacadeQuad[6] =
				{ 0, 2, 1, 0, 3, 2 };
			const NiColorA safeFacadeColor = SanitizeColor(facadeColor);
			for (UInt32 ordinal = 0; ordinal < 4; ++ordinal)
			{
				const NativeFontGpuVertex& vertex =
					payload.gpuVertices[packet.firstVertex + ordinal];
				data->m_pkVertex[ordinal] = NiPoint3(
					vertex.x + origin.x,
					vertex.y + origin.y,
					vertex.z + origin.z);
				data->m_pkTexture[ordinal] = NiPoint2(vertex.u, vertex.v);
				data->m_pkColor[ordinal] = safeFacadeColor;
			}
			std::copy(std::begin(kFacadeQuad), std::end(kFacadeQuad),
				data->m_pusTriList);
			data->m_kBound = payload.bound;
			data->m_kBound.m_kCenter.x += origin.x;
			data->m_kBound.m_kCenter.y += origin.y;
			data->m_kBound.m_kCenter.z += origin.z;
			if (prepareObject)
				shape->PrepareObject();
			data->m_kBound = payload.bound;
			data->m_kBound.m_kCenter.x += origin.x;
			data->m_kBound.m_kCenter.y += origin.y;
			data->m_kBound.m_kCenter.z += origin.z;
			if (prepareObject && shape->m_pWorldBound)
				shape->UpdateWorldBound();
			return shape;
		}

		NiColorA UnpackNativeBaseColor(UInt32 color);

		bool IsVanillaLayoutPayloadEligible(
			const NativeFontPayloadTemplate& payload,
			const NativeFontPacketTemplate*& packet,
			NativeFontVanillaLayoutKind& layoutKind)
		{
			packet = nullptr;
			layoutKind = NativeFontVanillaLayoutKind::None;
			if (payload.pageCount != 1 || payload.compositePackets.size() != 1
				|| payload.gpuVertices.empty())
			{
				return false;
			}
			const NativeFontPacketTemplate& candidate =
				payload.compositePackets.front();
			const bool supportedDistanceField =
				candidate.distanceFieldMethod == DistanceFieldMethod::TrueSdf
				|| candidate.distanceFieldMethod == DistanceFieldMethod::Mtsdf;
			const UInt64 packetEnd = static_cast<UInt64>(
				candidate.firstVertex) + candidate.vertexCount;
			if (candidate.shaderClass != NativeFontShaderClass::Composite
				|| !supportedDistanceField
				|| candidate.atlasPage != 0 || !candidate.vertexCount
				|| (candidate.firstVertex & 3u)
				|| (candidate.vertexCount & 3u)
				|| packetEnd > payload.gpuVertices.size()
				|| candidate.staticCompositeLayerMask < 8u
				|| candidate.staticCompositeLayerMask > 15u
				|| !std::isfinite(candidate.uniformSdfSpread)
				|| candidate.uniformSdfSpread < 0.0f
				|| !std::isfinite(
					candidate.uniformDistanceParameterScale)
				|| candidate.uniformDistanceParameterScale < 0.0f
				|| (candidate.uniformDistanceParameterScale > 0.0f
					&& candidate.uniformDistanceParameterScale < 1.0f))
			{
				return false;
			}
			const bool uniformSpread = candidate.uniformSdfSpread > 0.0f;
			const bool uniformDistanceScale =
				candidate.uniformDistanceParameterScale >= 1.0f;
			const float layerMask = static_cast<float>(
				candidate.staticCompositeLayerMask);
			for (UInt32 relative = 0; relative < candidate.vertexCount;
				++relative)
			{
				const size_t index = static_cast<size_t>(candidate.firstVertex)
					+ relative;
				const NativeFontGpuVertex& vertex = payload.gpuVertices[index];
				if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y)
					|| !std::isfinite(vertex.z) || !std::isfinite(vertex.u)
					|| !std::isfinite(vertex.v)
					|| !std::isfinite(vertex.sdfSpread)
					|| vertex.sdfSpread <= 0.0f
					|| !std::isfinite(vertex.distanceParameterScale)
					|| vertex.distanceParameterScale < 1.0f
					|| (uniformSpread
						&& vertex.sdfSpread != candidate.uniformSdfSpread)
					|| (uniformDistanceScale
						&& vertex.distanceParameterScale
							!= candidate.uniformDistanceParameterScale)
					|| vertex.layerMask != layerMask
					|| !std::isfinite(vertex.glyphU0)
					|| !std::isfinite(vertex.glyphV0)
					|| !std::isfinite(vertex.glyphU1)
					|| !std::isfinite(vertex.glyphV1)
					|| vertex.glyphU0 > vertex.glyphU1
					|| vertex.glyphV0 > vertex.glyphV1)
				{
					return false;
				}
				const NativeFontGpuVertex& quadFirst =
					payload.gpuVertices[static_cast<size_t>(
						candidate.firstVertex) + (relative & ~UInt32(3u))];
				if (vertex.sdfSpread != quadFirst.sdfSpread
					|| vertex.distanceParameterScale
						!= quadFirst.distanceParameterScale
					|| vertex.glyphU0 != quadFirst.glyphU0
					|| vertex.glyphV0 != quadFirst.glyphV0
					|| vertex.glyphU1 != quadFirst.glyphU1
					|| vertex.glyphV1 != quadFirst.glyphV1)
				{
					return false;
				}
			}
			layoutKind = uniformSpread && uniformDistanceScale
				? NativeFontVanillaLayoutKind::Uniform40
				: NativeFontVanillaLayoutKind::Parametric48;
			packet = &candidate;
			return true;
		}

		NiTriShape* TryCreateVanillaLayoutShape(Font& font,
			const std::vector<std::shared_ptr<AtlasResource>>& atlases,
			const NativeFontPayloadTemplatePtr& payload, UInt32 glyphCount,
			const NativeFontEffectShapeConfig& effects,
			const NativeFontShapeColorContract& colorContract,
			const NiColorA& tileColor, const NiPoint3& origin,
			bool prepareObject)
		{
			if (!g_bEnableFreeTypeFontVanillaLayout)
				return nullptr;

			const NativeFontPacketTemplate* packet = nullptr;
			NativeFontVanillaLayoutKind layoutKind =
				NativeFontVanillaLayoutKind::None;
			if (!payload || atlases.size() != 1 || !atlases.front()
				|| !IsVanillaLayoutPayloadEligible(
					*payload, packet, layoutKind)
				|| !packet
				|| !UsesNativeFontVanillaLayout(layoutKind)
				|| !IsVanillaLayoutEnabled(packet->distanceFieldMethod))
			{
				return nullptr;
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutEligible);
			if (packet->compositeShiftedShadow)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::VanillaLayoutShiftedEligible);
			}
			const auto recordFallback = []()
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::VanillaLayoutFallback);
			};
			if (!prepareObject)
			{
				recordFallback();
				return nullptr;
			}
			const UInt32 targetQuadCount = packet->vertexCount / 4u;
			if (!targetQuadCount || targetQuadCount != glyphCount
				|| packet->vertexCount > std::numeric_limits<UInt16>::max())
			{
				recordFallback();
				return nullptr;
			}

			NiTriShape* shape = CreateFreeTypeTextShape(targetQuadCount,
				tileColor, false, atlases.front()->property,
				GetAtlasTexture(*atlases.front()));
			if (!shape || !shape->GetModelData())
			{
				if (shape)
					shape->DeleteThis();
				recordFallback();
				return nullptr;
			}
			NiTriShapeData* data = shape->GetModelData();
			if (data->m_pkBuffData || data->m_usVertices < packet->vertexCount
				|| !data->m_pkVertex || !data->m_pusTriList)
			{
				shape->DeleteThis();
				recordFallback();
				return nullptr;
			}
			if (!data->m_pkColor)
				data->m_pkColor = NiAlloc<NiColorA>(data->m_usVertices);
			if (!data->m_pkTexture)
				data->m_pkTexture = NiAlloc<NiPoint2>(data->m_usVertices);
			if (!data->m_pkColor || !data->m_pkTexture)
			{
				shape->DeleteThis();
				recordFallback();
				return nullptr;
			}

			static constexpr UInt16 kCanonicalQuad[6] =
				{ 0, 2, 1, 0, 3, 2 };
			for (UInt32 index = 0; index < packet->vertexCount; ++index)
			{
				const NativeFontGpuVertex& vertex = payload->gpuVertices[
					static_cast<size_t>(packet->firstVertex) + index];
				data->m_pkVertex[index] = NiPoint3(
					vertex.x + origin.x, vertex.y + origin.y,
					vertex.z + origin.z);
				data->m_pkColor[index] = UnpackNativeBaseColor(vertex.color);
				data->m_pkTexture[index] = NiPoint2(vertex.u, vertex.v);
			}
			for (UInt32 quad = 0; quad < targetQuadCount; ++quad)
			{
				for (UInt32 ordinal = 0; ordinal < 6; ++ordinal)
				{
					data->m_pusTriList[quad * 6u + ordinal] =
						static_cast<UInt16>(
							quad * 4u + kCanonicalQuad[ordinal]);
				}
			}
			// Retail FalloutNV exposes one UV-present bit and one m_pkTexture
			// source. The compact TEXCOORD1/2 or parameterized TEXCOORD1/2/3
			// stream is installed only by the certified direct VB upload after the
			// queued native pack has completed and retired this UV source.
			data->m_usDataFlags |= NiGeometryData::TEXTURE_SET_MASK;
			data->m_kBound = payload->bound;
			data->m_kBound.m_kCenter.x += origin.x;
			data->m_kBound.m_kCenter.y += origin.y;
			data->m_kBound.m_kCenter.z += origin.z;

			// Establish vanilla properties and bounds, but suppress its geometry
			// precache: that route selects the vanilla TileShader's 20-byte layout.
			shape->PrepareObject(false, true);
			if (!PrepareNativeFontVanillaLayoutShape(font, shape, font.iFontNum,
				glyphCount, payload->quadCount, layoutKind,
				&effects, &colorContract,
				payload, origin))
			{
				shape->DeleteThis();
				recordFallback();
				return nullptr;
			}
			TileShader* targetShader = ResolveNativeFontPacketShader(
				*packet, shape, false, layoutKind);
			if (!targetShader || !RequestNativeFontVanillaLayoutShapePrecache(
				shape, targetShader))
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::VanillaLayoutPrecacheUnavailable);
				shape->DeleteThis();
				recordFallback();
				return nullptr;
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::VanillaLayoutPrecacheAccepted);
			data->m_kBound = payload->bound;
			data->m_kBound.m_kCenter.x += origin.x;
			data->m_kBound.m_kCenter.y += origin.y;
			data->m_kBound.m_kCenter.z += origin.z;
			if (shape->m_pWorldBound)
				shape->UpdateWorldBound();
			RecordFreeTypePerf(FreeTypePerfCounter::VanillaLayoutCreated);
			if (packet->compositeShiftedShadow)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::VanillaLayoutShiftedCreated);
			}
			RecordFreeTypePerf(FreeTypePerfCounter::VanillaLayoutVertex,
				packet->vertexCount);
			return shape;
		}

		NiTriShape* CreateDirectNativeShape(Font& font,
			const std::vector<std::shared_ptr<AtlasResource>>& atlases,
			std::vector<NativeFontGpuVertex>&& vertices,
			UInt32 glyphCount, UInt32 quadCount,
			NativeFontEffectShapeConfig& effects,
			const NativeFontShapeColorContract& colorContract,
			const NiColorA& facadeColor, const NiColorA& tileColor,
			const NiPoint3& origin, const NiPoint3& boundMinimum,
			const NiPoint3& boundMaximum, bool prepareObject,
			std::vector<NativeFontCompositeSpan>&& compositeSpans = {})
		{
			if (!quadCount || vertices.size() < quadCount * 4u
				|| !PopulateDirectAtlasEffectPages(atlases, effects))
			{
				return nullptr;
			}
			NiBound bound;
			if (!BuildDirectVertexBound(
				vertices.size(), boundMinimum, boundMaximum, bound))
			{
				return nullptr;
			}
			NativeFontPayloadTemplatePtr payload =
				BuildNativeFontPayloadTemplate(std::move(vertices),
					quadCount, effects, bound, std::move(compositeSpans));
			if (!payload || payload->gpuVertices.size() < 4
				|| payload->packets.empty())
				return nullptr;
			if (NiTriShape* vanillaLayout = TryCreateVanillaLayoutShape(
				font, atlases, payload, glyphCount, effects, colorContract,
				tileColor, origin, prepareObject))
			{
				return vanillaLayout;
			}

			RecordFreeTypePerf(
				FreeTypePerfCounter::SingletonFacadeCandidate);
			RecordFreeTypePerf(
				FreeTypePerfCounter::SingletonFacadePayloadPacket,
				static_cast<UInt64>(payload->packets.size()));
			RecordFreeTypePerf(payload->packets.size() == 1
				? FreeTypePerfCounter::SingletonFacadeSinglePacketArtifact
				: FreeTypePerfCounter::SingletonFacadeMultiPacketArtifact);
			const NativeFontPacketTemplate& facadePacket =
				payload->packets.front();
			NiTriShape* shape =
				facadePacket.atlasPage < atlases.size()
					? CreateDirectNativePacketShell(
						atlases[facadePacket.atlasPage], *payload,
						facadePacket,
						facadeColor, tileColor, origin, false)
					: nullptr;
			if (!shape)
				return nullptr;
			if (!PrepareNativeFontSingletonFacadeShape(font, shape,
				font.iFontNum, glyphCount, quadCount, &effects,
				&colorContract, payload, origin))
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::SingletonFacadeFallback);
				if (!PrepareNativeFontAtlasShape(font, shape, font.iFontNum,
					glyphCount, quadCount, &effects, &colorContract,
					payload, origin))
				{
					shape->DeleteThis();
					return nullptr;
				}
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::SingletonFacadeCreated);
			if (prepareObject)
				shape->PrepareObject();
			NiTriShapeData* data = shape->GetModelData();
			data->m_kBound = bound;
			data->m_kBound.m_kCenter.x += origin.x;
			data->m_kBound.m_kCenter.y += origin.y;
			data->m_kBound.m_kCenter.z += origin.z;
			if (prepareObject && shape->m_pWorldBound)
				shape->UpdateWorldBound();
			return shape;
		}

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
			return UnpackNativeBaseColor(glyph.packedColor);
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

		auto resolve = [&](const DirectGlyphCommand& command,
			size_t layer, const AtlasSnapshotPlacement*& placement,
			UInt16& pageOrdinal, float& baselineOffset,
			bool& knownEmpty)
		{
			placement = nullptr;
			pageOrdinal = kInvalidDirectAtlasPageSlot;
			baselineOffset = 0.0f;
			knownEmpty = false;
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
			if (letter.flags & kDirectCachedLetterKnownEmpty)
			{
				knownEmpty = true;
				return true;
			}
			const UInt8 maskType =
				static_cast<UInt8>(masks[layer]);
			const DirectAtlasGlyphLayer* direct = nullptr;
			for (const DirectAtlasGlyphLayer& candidate :
				letter.layers)
			{
				if (candidate.valid()
					&& candidate.maskType == maskType)
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
			pageOrdinal =
				sealed->pageOrdinals[roleIndex][direct->pageSlot];
			if (pageOrdinal >= sealed->atlases.size()
				|| pageOrdinal >= kMaximumAtlasSnapshotPages)
			{
				return false;
			}
			const std::shared_ptr<AtlasResource>& atlas =
				sealed->atlases[pageOrdinal];
			if (!atlas || !atlas->compactSnapshot
				|| !atlas->pageContentHash
				|| atlas->compactSnapshot->sourceHeader.pageContentHash
					!= atlas->pageContentHash
				|| direct->snapshotPlacementIndex
					>= atlas->compactSnapshot->placements.size())
			{
				return false;
			}
			const AtlasSnapshotPlacement& source =
				atlas->compactSnapshot->placements[
					direct->snapshotPlacementIndex];
			if (!source.cacheId || source.maskType != maskType
				|| !source.rect.width || !source.rect.height)
			{
				return false;
			}
			placement = &source;
			const UInt8 faceIndex =
				table.faceIndices[command.directSlot];
			const auto& baselines =
				sealed->faceBaselineOffsets[roleIndex];
			baselineOffset = faceIndex < baselines.size()
				? baselines[faceIndex]
				: sealed->roleBaselineOffsets[roleIndex];
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
			for (size_t layer = 0; layer < enabled.size(); ++layer)
			{
				if (!enabled[layer])
					continue;
				const AtlasSnapshotPlacement* placement = nullptr;
				UInt16 page = kInvalidDirectAtlasPageSlot;
				float baselineOffset = 0.0f;
				bool knownEmpty = false;
				if (!resolve(command, layer, placement, page,
					baselineOffset, knownEmpty))
				{
					result.outcome =
						DirectAtlasShapeOutcome::Failed;
					return result;
				}
				if (knownEmpty)
					continue;
				if (!placement || page >= sealed->atlases.size())
				{
					result.outcome =
						DirectAtlasShapeOutcome::Failed;
					return result;
				}
				++counts[layer][page];
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
					> kMaximumQuads - quadCount)
				{
					result.outcome =
						DirectAtlasShapeOutcome::Failed;
					return result;
				}
				quadCount += counts[layer][page];
			}
		}
		if (!quadCount || quadCount > kMaximumQuads)
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
		for (size_t layer = 0; layer < enabled.size(); ++layer)
		{
			if (!enabled[layer])
				continue;
			const NiColorA layerColor =
				SanitizeColor(layerColors[layer]);
			for (const DirectGlyphCommand& command : glyphs)
			{
				const AtlasSnapshotPlacement* placement = nullptr;
				UInt16 page = kInvalidDirectAtlasPageSlot;
				float baselineOffset = 0.0f;
				bool knownEmpty = false;
				if (!resolve(command, layer, placement, page,
					baselineOffset, knownEmpty))
				{
					result.outcome =
						DirectAtlasShapeOutcome::Failed;
					return result;
				}
				if (knownEmpty)
					continue;
				if (!placement || page >= sealed->atlases.size())
				{
					result.outcome =
						DirectAtlasShapeOutcome::Failed;
					return result;
				}
				const UInt32 quadIndex = cursors[layer][page]++;
				if (quadIndex >= quadCount)
				{
					result.outcome =
						DirectAtlasShapeOutcome::Failed;
					return result;
				}
				const NiColorA baseColor = ResolveBaseColor(
					UnpackNativeBaseColor(command.packedColor),
					tileColor);
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
				ExtendDirectColorContract(colorContract,
					colorContractInitialized, bakedColor);
				if (!WriteDirectQuadVertices(*placement,
					command.pen, origin, offsetsX[layer],
					offsetsY[layer], rasterScale, baselineOffset,
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
			boundMinimum, boundMaximum, prepareObject);
		result.outcome = result.shape
			? DirectAtlasShapeOutcome::Created
			: DirectAtlasShapeOutcome::Failed;
		return result;
	}

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
				std::array<NativeFontGpuVertex, 4> vertices;
				const bool written = source.vanillaLetter
					? WriteVanillaDirectQuadVertices(
						*source.vanillaLetter, pen, origin,
						baselineOffset,
						PackNativeBaseColor(baseColor),
						1u << static_cast<UInt8>(
							AtlasLayer::Fill),
						vertices.data(), boundMinimum,
						boundMaximum)
					: WriteDirectQuadVertices(*source.placement,
						pen, origin, 0.0f, 0.0f,
						rasterScale, baselineOffset,
						1.0f, false,
						PackNativeBaseColor(baseColor),
						1u << static_cast<UInt8>(
							AtlasLayer::Fill),
						vertices.data(), boundMinimum,
						boundMaximum);
				if (!written)
				{
					return nullptr;
				}
				for (UInt32 ordinal = 0; ordinal < 4; ++ordinal)
				{
					const UInt32 output = outputQuad * 4u + ordinal;
					data->m_pkVertex[output] = NiPoint3(
						vertices[ordinal].x + origin.x,
						vertices[ordinal].y + origin.y,
						vertices[ordinal].z + origin.z);
					data->m_pkTexture[output] = NiPoint2(
						vertices[ordinal].u, vertices[ordinal].v);
					data->m_pkColor[output] = SanitizeColor(baseColor);
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
				FreeTypePerfCounter::
					DirectShapeVertexInitializationBytesAvoided,
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
			result.glyphCount = static_cast<UInt32>(std::count_if(
				batch.glyphs.begin(), batch.glyphs.end(),
				[](const DirectAtlasBatchGlyph& glyph)
				{
					return !glyph.knownEmpty
						&& (glyph.placement || glyph.vanillaLetter);
				}));
			if (!result.glyphCount || result.glyphCount > kMaximumQuads)
			{
				result.outcome = DirectAtlasShapeOutcome::Failed;
				return result;
			}

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
				if (usedPageCount == 1)
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
					offsets[kind][page] = physicalQuads;
					cursors[kind][page] = physicalQuads;
					physicalQuads += counts[kind][page];
				}
			}
			if (!physicalQuads || physicalQuads > kMaximumQuads)
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
			std::vector<CompositeGlyphQuadSource> compositeSources;
			if (distanceField)
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
				ExtendDirectColorContract(
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
				if (distanceField)
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
			if (distanceField
				&& !AppendOneGlyphCompositeQuads(vertices,
					compositeSources, static_cast<UInt32>(atlases.size()),
					effects, boundMinimum, boundMaximum, compositeSpans))
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


		QuadBatchFingerprint BuildQuadBatchFingerprint(
			const std::vector<PendingQuad>& quads, const NiPoint3& origin)
		{
			UInt64 hash = 1469598103934665603ull;
			UInt64 colorHash = 1469598103934665603ull;
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
				const UInt64 cacheId = quad.source.CacheId();
				const NiPoint3 relativePen(
					quad.pen.x - origin.x,
					quad.pen.y - origin.y,
					quad.pen.z - origin.z);
				add(&cacheId, sizeof(cacheId));
				add(&relativePen, sizeof(relativePen));
				add(&quad.offsetX, sizeof(quad.offsetX));
				add(&quad.offsetY, sizeof(quad.offsetY));
				add(&quad.rasterScale, sizeof(quad.rasterScale));
				add(&quad.sourceToLogicalScale,
					sizeof(quad.sourceToLogicalScale));
				add(&quad.baselineOffset, sizeof(quad.baselineOffset));
				add(&quad.expansionPixels, sizeof(quad.expansionPixels));
				add(&quad.usesSdf, sizeof(quad.usesSdf));
				add(&quad.glyphOrdinal, sizeof(quad.glyphOrdinal));
				add(&quad.atlasPage, sizeof(quad.atlasPage));
				add(&quad.atlasPlacement.atlasIdentity,
					sizeof(quad.atlasPlacement.atlasIdentity));
				add(&quad.atlasPlacement.atlasGeneration,
					sizeof(quad.atlasPlacement.atlasGeneration));
				add(&quad.atlasPlacement.u0, sizeof(quad.atlasPlacement.u0));
				add(&quad.atlasPlacement.v0, sizeof(quad.atlasPlacement.v0));
				add(&quad.atlasPlacement.u1, sizeof(quad.atlasPlacement.u1));
				add(&quad.atlasPlacement.v1, sizeof(quad.atlasPlacement.v1));
				const UInt8* colorBytes =
					reinterpret_cast<const UInt8*>(&quad.baseColor);
				for (size_t index = 0;
					index < sizeof(quad.baseColor); ++index)
				{
					colorHash ^= colorBytes[index];
					colorHash *= 1099511628211ull;
				}
			}
			return { hash, colorHash,
				static_cast<UInt32>(quads.size()) };
		}

		struct TextArtifactHotEntry
		{
			UInt64 admissionSignature = 0;
			TextArtifactKey key;
			std::weak_ptr<const NativeFontPayloadTemplate> data;
			bool occupied = false;
		};

		struct TextArtifactAdmissionSlot
		{
			UInt64 signature = 0;
			UInt8 observations = 0;
		};

		using TextArtifactHotBucket = std::array<TextArtifactHotEntry,
			kTextArtifactHotWays>;
		using TextArtifactAdmissionBucket = std::array<TextArtifactAdmissionSlot,
			kTextArtifactAdmissionWays>;

		static_assert((kTextArtifactHotBucketCount
			& (kTextArtifactHotBucketCount - 1u)) == 0u);
		static_assert((kTextArtifactHotWays
			& (kTextArtifactHotWays - 1u)) == 0u);
		static_assert((kTextArtifactAdmissionBucketCount
			& (kTextArtifactAdmissionBucketCount - 1u)) == 0u);
		static_assert((kTextArtifactAdmissionWays
			& (kTextArtifactAdmissionWays - 1u)) == 0u);

		// The hot front never extends artifact lifetime. A coarse-admission
		// collision can only cause an extra exact lookup/admission; the resident
		// key and atlas objects are still fully validated before reuse. Each
		// lookup examines one four-way bucket, independent of total capacity.
		thread_local std::array<TextArtifactHotBucket,
			kTextArtifactHotBucketCount> s_textArtifactHotBuckets;
		thread_local std::array<UInt8,
			kTextArtifactHotBucketCount> s_textArtifactHotNextWays;
		thread_local std::array<TextArtifactAdmissionBucket,
			kTextArtifactAdmissionBucketCount> s_textArtifactAdmissionBuckets;
		thread_local std::array<UInt8,
			kTextArtifactAdmissionBucketCount> s_textArtifactAdmissionNextWays;

		size_t FoldTextArtifactSignature(UInt64 signature, size_t bucketCount)
		{
			// FNV already distributes the input bytes. Folding the high half avoids
			// relying only on its low bits while keeping this lookup cheaper than
			// the exact per-quad fingerprint that admission is intended to avoid.
			const UInt64 folded = signature ^ (signature >> 32)
				^ (signature >> 17);
			return static_cast<size_t>(folded) & (bucketCount - 1u);
		}

		bool TouchTextArtifactAdmission(UInt64 signature)
		{
			if (!signature)
				signature = 1;
			const size_t bucketIndex = FoldTextArtifactSignature(signature,
				s_textArtifactAdmissionBuckets.size());
			TextArtifactAdmissionBucket& bucket =
				s_textArtifactAdmissionBuckets[bucketIndex];
			size_t emptyWay = bucket.size();
			size_t candidateWay = bucket.size();
			const size_t firstWay = static_cast<size_t>(
				s_textArtifactAdmissionNextWays[bucketIndex])
				& (bucket.size() - 1u);
			for (size_t offset = 0; offset < bucket.size(); ++offset)
			{
				const size_t way = (firstWay + offset)
					& (bucket.size() - 1u);
				TextArtifactAdmissionSlot& slot = bucket[way];
				if (slot.signature == signature)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						TextArtifactAdmissionHistoryHit);
					if (slot.observations < 2)
						++slot.observations;
					return slot.observations >= 2;
				}
				if (!slot.signature && emptyWay == bucket.size())
					emptyWay = way;
				else if (slot.observations < 2
					&& candidateWay == bucket.size())
				{
					candidateWay = way;
				}
			}

			size_t targetWay = emptyWay;
			if (targetWay == bucket.size())
				targetWay = candidateWay;
			if (targetWay == bucket.size())
				targetWay = firstWay;
			TextArtifactAdmissionSlot& target = bucket[targetWay];
			if (target.signature)
			{
				const FreeTypePerfCounter replacementCounter =
					target.observations < 2
						? FreeTypePerfCounter::
							TextArtifactAdmissionCandidateReplacement
						: FreeTypePerfCounter::
							TextArtifactAdmissionEstablishedReplacement;
				RecordFreeTypePerf(replacementCounter);
				s_textArtifactAdmissionNextWays[bucketIndex] =
					static_cast<UInt8>((targetWay + 1u)
						& (bucket.size() - 1u));
			}
			target.signature = signature;
			target.observations = 1;
			return false;
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
			return {
				atlases.empty() ? 0
					: reinterpret_cast<uintptr_t>(atlases[0].get()),
				hash, fingerprint.quadColorHash,
				generation, fingerprint.quadCount
			};
		}

		UInt64 BuildTextArtifactAdmissionSignature(
			const std::vector<PendingQuad>& quads,
			const std::vector<std::shared_ptr<AtlasResource>>& atlases,
			const NiPoint3& origin,
			const NativeFontEffectShapeConfig& effect)
		{
			// Most text artifacts in menu churn are never requested again. This
			// constant-cost signature is intentionally coarse: a collision can only
			// trigger a later exact fingerprint/cache lookup, never artifact reuse.
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
			const size_t quadCount = quads.size();
			const size_t atlasCount = atlases.size();
			add(&quadCount, sizeof(quadCount));
			add(&atlasCount, sizeof(atlasCount));
			auto addAtlas = [&](const std::shared_ptr<AtlasResource>& atlas)
			{
				const uintptr_t identity =
					reinterpret_cast<uintptr_t>(atlas.get());
				const UInt32 generation = atlas ? atlas->generation : 0;
				add(&identity, sizeof(identity));
				add(&generation, sizeof(generation));
			};
			if (!atlases.empty())
			{
				addAtlas(atlases.front());
				if (atlases.size() > 1)
					addAtlas(atlases.back());
			}
			auto addQuad = [&](const PendingQuad& quad)
			{
				const UInt64 cacheId = quad.source.CacheId();
				const NiPoint3 relativePen(
					quad.pen.x - origin.x,
					quad.pen.y - origin.y,
					quad.pen.z - origin.z);
				add(&cacheId, sizeof(cacheId));
				add(&relativePen, sizeof(relativePen));
				add(&quad.offsetX, sizeof(quad.offsetX));
				add(&quad.offsetY, sizeof(quad.offsetY));
				add(&quad.rasterScale, sizeof(quad.rasterScale));
				add(&quad.sourceToLogicalScale,
					sizeof(quad.sourceToLogicalScale));
				add(&quad.baselineOffset, sizeof(quad.baselineOffset));
				add(&quad.expansionPixels, sizeof(quad.expansionPixels));
				add(&quad.usesSdf, sizeof(quad.usesSdf));
				add(&quad.glyphOrdinal, sizeof(quad.glyphOrdinal));
				add(&quad.atlasPage, sizeof(quad.atlasPage));
				add(&quad.atlasPlacement.atlasIdentity,
					sizeof(quad.atlasPlacement.atlasIdentity));
				add(&quad.atlasPlacement.atlasGeneration,
					sizeof(quad.atlasPlacement.atlasGeneration));
				add(&quad.baseColor, sizeof(quad.baseColor));
			};
			if (!quads.empty())
			{
				addQuad(quads.front());
				if (quads.size() > 1)
					addQuad(quads.back());
			}
			add(&effect.enabled, sizeof(effect.enabled));
			add(&effect.shaderEffects, sizeof(effect.shaderEffects));
			add(&effect.bakedCoverage, sizeof(effect.bakedCoverage));
			add(&effect.precomposedArgb, sizeof(effect.precomposedArgb));
			add(&effect.distanceFieldMethod,
				sizeof(effect.distanceFieldMethod));
			add(&effect.quality, sizeof(effect.quality));
			const std::array<float, 16> scalars = {
				effect.rasterScale,
				effect.inverseAtlasWidth, effect.inverseAtlasHeight,
				effect.sdfSpreadPixels, effect.shadowBlurPixels,
				effect.shadowPower, effect.shadowGlowAlpha,
				effect.shadowOutlineAlpha, effect.shadowOffsetX,
				effect.shadowOffsetY, effect.shadowOffsetRasterScale,
				effect.glowInnerPixels,
				effect.glowOuterPixels, effect.glowPower,
				effect.outlineWidthPixels, effect.outlineSoftnessPixels
			};
			add(scalars.data(), scalars.size() * sizeof(float));
			const size_t pageCount = effect.atlasInverseSizes.size();
			const size_t rangeCount = effect.ranges.size();
			add(&pageCount, sizeof(pageCount));
			add(&rangeCount, sizeof(rangeCount));
			return hash ? hash : 1;
		}

		UInt64 BuildTextArtifactContentHash(
			const TextArtifactKey& geometryKey,
			const NativeFontEffectShapeConfig& effect)
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
			add(&geometryKey.quadColorHash,
				sizeof(geometryKey.quadColorHash));
			add(&geometryKey.generation, sizeof(geometryKey.generation));
			add(&geometryKey.quadCount, sizeof(geometryKey.quadCount));
			add(&effect.enabled, sizeof(effect.enabled));
			add(&effect.shaderEffects, sizeof(effect.shaderEffects));
			add(&effect.bakedCoverage, sizeof(effect.bakedCoverage));
			add(&effect.precomposedArgb,
				sizeof(effect.precomposedArgb));
			add(&effect.distanceFieldMethod,
				sizeof(effect.distanceFieldMethod));
			add(&effect.quality, sizeof(effect.quality));
			const std::array<float, 16> scalars = {
				effect.rasterScale,
				effect.inverseAtlasWidth, effect.inverseAtlasHeight,
				effect.sdfSpreadPixels, effect.shadowBlurPixels,
				effect.shadowPower, effect.shadowGlowAlpha,
				effect.shadowOutlineAlpha, effect.shadowOffsetX,
				effect.shadowOffsetY, effect.shadowOffsetRasterScale,
				effect.glowInnerPixels,
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
			for (const NativeFontDrawRange& range : effect.ranges)
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

		bool TextArtifactMatchesAtlases(const NativeFontPayloadTemplate& artifact,
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

		NativeFontPayloadTemplatePtr FindHotTextArtifact(
			UInt64 admissionSignature,
			const TextArtifactKey& geometryKey,
			const NativeFontEffectShapeConfig& effects,
			const std::vector<std::shared_ptr<AtlasResource>>& atlases,
			TextArtifactKey& resolvedKey, bool& keyResolved)
		{
			if (!admissionSignature)
				admissionSignature = 1;
			TextArtifactHotBucket& bucket = s_textArtifactHotBuckets[
				FoldTextArtifactSignature(admissionSignature,
					s_textArtifactHotBuckets.size())];
			bool hasCandidate = false;
			for (TextArtifactHotEntry& entry : bucket)
			{
				if (!entry.occupied
					|| entry.admissionSignature != admissionSignature)
					continue;
				if (entry.data.expired())
				{
					entry = {};
					RecordFreeTypePerf(FreeTypePerfCounter::
						TextArtifactHotEntryExpired);
					continue;
				}
				hasCandidate = true;
			}
			if (!hasCandidate)
				return {};
			if (!keyResolved)
			{
				resolvedKey = geometryKey;
				resolvedKey.contentHash = BuildTextArtifactContentHash(
					geometryKey, effects);
				keyResolved = true;
			}
			for (TextArtifactHotEntry& entry : bucket)
			{
				if (!entry.occupied
					|| entry.admissionSignature != admissionSignature
					|| !(entry.key == resolvedKey))
				{
					continue;
				}
				NativeFontPayloadTemplatePtr data = entry.data.lock();
				if (data && TextArtifactMatchesAtlases(*data, atlases))
					return data;
				if (!data)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::
						TextArtifactHotEntryExpired);
				}
				entry = {};
			}
			return {};
		}

		void PublishHotTextArtifact(UInt64 admissionSignature,
			const TextArtifactKey& key,
			const NativeFontPayloadTemplatePtr& data)
		{
			if (!data)
				return;
			if (!admissionSignature)
				admissionSignature = 1;
			const size_t bucketIndex = FoldTextArtifactSignature(
				admissionSignature, s_textArtifactHotBuckets.size());
			TextArtifactHotBucket& bucket =
				s_textArtifactHotBuckets[bucketIndex];
			size_t reusableWay = bucket.size();
			for (size_t way = 0; way < bucket.size(); ++way)
			{
				TextArtifactHotEntry& current = bucket[way];
				if (!current.occupied)
				{
					if (reusableWay == bucket.size())
						reusableWay = way;
					continue;
				}
				if (current.data.expired())
				{
					current = {};
					RecordFreeTypePerf(FreeTypePerfCounter::
						TextArtifactHotEntryExpired);
					if (reusableWay == bucket.size())
						reusableWay = way;
					continue;
				}
				if (current.admissionSignature == admissionSignature
					&& current.key == key)
				{
					current.data = data;
					return;
				}
			}
			const size_t targetWay = reusableWay != bucket.size()
				? reusableWay
				: static_cast<size_t>(
					s_textArtifactHotNextWays[bucketIndex])
					& (bucket.size() - 1u);
			if (reusableWay == bucket.size())
			{
				RecordFreeTypePerf(FreeTypePerfCounter::
					TextArtifactHotEntryReplacement);
				s_textArtifactHotNextWays[bucketIndex] =
					static_cast<UInt8>((targetWay + 1u)
						& (bucket.size() - 1u));
			}
			TextArtifactHotEntry& entry = bucket[targetWay];
			entry.admissionSignature = admissionSignature;
			entry.key = key;
			entry.data = data;
			entry.occupied = true;
		}

		NativeFontPayloadTemplatePtr GetNativeTextArtifact(Font& font,
			const std::vector<PendingQuad>& quads,
			const std::vector<std::shared_ptr<AtlasResource>>& atlases,
			const TextArtifactKey& geometryKey, UInt64 admissionSignature,
			const NiPoint3& origin, const NativeFontEffectShapeConfig& effects,
			bool allowCache)
		{
			AtlasState& state = State();
			TextArtifactKey key = geometryKey;
			bool keyResolved = false;
			if (allowCache)
			{
				if (NativeFontPayloadTemplatePtr hot =
					FindHotTextArtifact(admissionSignature, geometryKey,
						effects, atlases, key, keyResolved))
				{
					RecordFreeTypePerf(FreeTypePerfCounter::TextArtifactHit);
					RecordFreeTypePerf(
						FreeTypePerfCounter::TextArtifactHotHit);
					return hot;
				}
			}
			if (allowCache)
			{
				if (!keyResolved)
				{
					key.contentHash = BuildTextArtifactContentHash(
						geometryKey, effects);
					keyResolved = true;
				}
				{
					std::lock_guard<std::mutex> lock(
						state.textArtifactMutex);
					auto existing = state.textArtifactCache.find(key);
					if (existing != state.textArtifactCache.end())
					{
						if (existing->second.data
							&& TextArtifactMatchesAtlases(
								*existing->second.data, atlases))
						{
							state.textArtifactLru.splice(
								state.textArtifactLru.begin(),
								state.textArtifactLru,
								existing->second.lru);
							existing->second.lru =
								state.textArtifactLru.begin();
							PublishHotTextArtifact(
								admissionSignature, key,
								existing->second.data);
							RecordFreeTypePerf(
								FreeTypePerfCounter::TextArtifactHit);
							return existing->second.data;
						}
						state.textArtifactCacheBytes -= std::min(
							state.textArtifactCacheBytes,
							existing->second.bytes);
						state.textArtifactLru.erase(existing->second.lru);
						state.textArtifactCache.erase(existing);
						RecordFreeTypePerf(
							FreeTypePerfCounter::TextArtifactEviction);
					}
				}
			}
			else
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::TextArtifactAdmissionBypass);
			}
			RecordFreeTypePerf(FreeTypePerfCounter::TextArtifactMiss);
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
						glyph.firstVertex = static_cast<UInt32>(vertices.size());
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
			if (compositeCandidate)
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
					boundMinimum, boundMaximum, compositeSpans))
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
				std::move(vertices), static_cast<UInt32>(quads.size()), effects,
				bound, std::move(compositeSpans));
			if (!result)
				return {};
			if (!allowCache)
				return result;
			const size_t bytes = GetNativeFontPayloadTemplateBytes(*result);
			const size_t cacheOverhead = sizeof(TextArtifactEntry)
				+ 2u * sizeof(TextArtifactKey) + 4u * sizeof(void*);
			const size_t preferred = static_cast<size_t>(
				g_uiFreeTypeFontMemoryCacheMB) * 1024u * 1024u / 12u;
			const size_t limit = GetCpuMemoryCategoryHeadroom(
				CpuMemoryCategory::TextArtifact, preferred);
			if (!limit || bytes > limit
				|| cacheOverhead > limit - bytes)
			{
				return result;
			}
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
						PublishHotTextArtifact(admissionSignature, key,
							existing->second.data);
						return existing->second.data;
					}
					state.textArtifactCacheBytes -= std::min(
						state.textArtifactCacheBytes,
						existing->second.bytes);
					state.textArtifactLru.erase(existing->second.lru);
					state.textArtifactCache.erase(existing);
					RecordFreeTypePerf(
						FreeTypePerfCounter::TextArtifactEviction);
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
					PublishHotTextArtifact(admissionSignature, key,
						inserted->second.data);
					return inserted->second.data;
				}
				inserted->second.cpuMemory.Reset(
					CpuMemoryCategory::TextArtifact, cacheOverhead);
				state.textArtifactCacheBytes += bytes;
				RecordFreeTypePerf(
					FreeTypePerfCounter::TextArtifactAdmission);
				TrimTextArtifactCache(state);
				PublishHotTextArtifact(admissionSignature, key, result);
			}
			return result;
		}

		NiTriShape* CreateAtlasShape(Font& font, const std::vector<PendingQuad>& quads,
			const std::vector<std::shared_ptr<AtlasResource>>& atlases, bool prepareObject,
			const NiColorA& tileColor, bool useNativeFontShader,
			const NativeFontEffectShapeConfig* effectConfig, const NiPoint3& origin)
		{
			if (atlases.empty() || !atlases[0] || quads.empty()
				|| quads.size() > kMaximumQuads)
				return nullptr;
			const bool needsNativeRangeRouting = useNativeFontShader
				|| atlases.size() > 1;
			NativeFontEffectShapeConfig resolvedEffect = effectConfig
				? *effectConfig : NativeFontEffectShapeConfig{};
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
			const UInt64 artifactAdmissionSignature =
				BuildTextArtifactAdmissionSignature(
					quads, atlases, origin, resolvedEffect);
			const bool allowArtifactCache = TouchTextArtifactAdmission(
				artifactAdmissionSignature);
			TextArtifactKey geometryKey;
			if (allowArtifactCache)
			{
				const QuadBatchFingerprint fingerprint =
					BuildQuadBatchFingerprint(quads, origin);
				geometryKey = BuildTextArtifactKey(
					fingerprint, atlases);
			}
			const NativeFontPayloadTemplatePtr artifact = GetNativeTextArtifact(
				font, quads, atlases, geometryKey,
				artifactAdmissionSignature, origin,
				resolvedEffect, allowArtifactCache);
			if (!artifact || artifact->gpuVertices.size() < quads.size() * 4u)
				return nullptr;
			const UInt8 fillLayerBit =
				1u << static_cast<UInt8>(AtlasLayer::Fill);
			const UInt32 glyphCount = static_cast<UInt32>(std::count_if(
				quads.begin(), quads.end(),
				[fillLayerBit](const PendingQuad& quad)
				{
					return (quad.layerMask & fillLayerBit) != 0;
				}));
			const NativeFontShapeColorContract colorContract =
				BuildColorContract(quads);
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
				shape->PrepareObject();
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

		NiTriShape* TryCreateAtlasShapeForMode(Font& font,
			const std::vector<PendingQuad>& quads,
			const FontConfig& config, float rasterScale, bool prepareObject,
			AtlasPixelMode pixelMode, AtlasRenderMode renderMode, UInt32 padding,
			std::vector<std::shared_ptr<AtlasResource>>& outAtlases,
			const NiColorA& tileColor, bool useNativeFontShader,
			const NativeFontEffectShapeConfig* effectConfig)
		{
			if (quads.empty())
				return nullptr;
			if ((pixelMode == AtlasPixelMode::A8
				|| pixelMode == AtlasPixelMode::Mtsdf32) && !useNativeFontShader)
				return nullptr;
			const std::vector<PendingQuad>* activeQuads = &quads;
			thread_local std::vector<PendingQuad> bakedQuads;
			const bool precomposedArgb = pixelMode == AtlasPixelMode::Argb32
				&& std::all_of(quads.begin(), quads.end(),
					[](const PendingQuad& quad)
					{
						return quad.source.IsPrecomposedArgb();
					});
			if (pixelMode == AtlasPixelMode::Argb32 && !useNativeFontShader
				&& !precomposedArgb)
			{
				BuildBakedArgbFallback(quads, tileColor, bakedQuads);
				activeQuads = &bakedQuads;
				// Direct atlas sources deliberately carry no CPU pixel payload.
				// They cannot be color-baked for the vanilla ARGB TileShader route.
				// The caller must first rebuild this batch through the compatibility
				// GlyphBitmap path; otherwise an A8 page could be submitted as ARGB
				// and render as dark, missing, or unrelated glyph rectangles.
				if (std::any_of(activeQuads->begin(), activeQuads->end(),
					[](const PendingQuad& quad)
					{
						return !quad.source.bitmap;
					}))
				{
					return nullptr;
				}
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
			const bool directSources = std::all_of(activeQuads->begin(),
				activeQuads->end(), [](const PendingQuad& quad)
				{
					return quad.source.IsDirect();
				});
			std::array<std::array<std::shared_ptr<AtlasResource>,
				kMaximumAtlasSnapshotPages>, 2> directRolePages;
			std::array<std::array<UInt16, kMaximumAtlasSnapshotPages>, 2>
				directPageOrdinals;
			for (auto& role : directPageOrdinals)
				role.fill(std::numeric_limits<UInt16>::max());
			for (const PendingQuad& quad : *activeQuads)
			{
				if (quad.source.bitmap)
				{
					roleUnique[static_cast<size_t>(quad.byteClass)].emplace(
						quad.source.bitmap->cacheId, quad.source.bitmap);
				}
			}
			std::vector<std::shared_ptr<AtlasResource>> availableAtlases;
			if (directSources)
			{
				for (const PendingQuad& quad : *activeQuads)
				{
					if (!quad.source.atlas
						|| !IsAtlasGlyphPlacementForAtlas(
							quad.source.placement, *quad.source.atlas))
					{
						return nullptr;
					}
					const size_t roleIndex =
						static_cast<size_t>(quad.byteClass);
					const UInt16 rolePage = quad.source.placement.pageIndex;
					if (roleIndex >= directRolePages.size()
						|| rolePage >= kMaximumAtlasSnapshotPages)
					{
						return nullptr;
					}
					std::shared_ptr<AtlasResource>& known =
						directRolePages[roleIndex][rolePage];
					if (known && known.get() != quad.source.atlas.get())
						return nullptr;
					known = quad.source.atlas;
				}
				for (size_t roleIndex = 0;
					roleIndex < directRolePages.size(); ++roleIndex)
				{
					for (UInt16 rolePage = 0;
						rolePage < kMaximumAtlasSnapshotPages; ++rolePage)
					{
						const std::shared_ptr<AtlasResource>& page =
							directRolePages[roleIndex][rolePage];
						if (!page)
							continue;
						UInt16 ordinal =
							std::numeric_limits<UInt16>::max();
						for (UInt16 candidate = 0;
							candidate < availableAtlases.size(); ++candidate)
						{
							if (availableAtlases[candidate].get()
									== page.get()
								|| AreAtlasResourcesBackedBySameTexture(
									*availableAtlases[candidate], *page))
							{
								ordinal = candidate;
								break;
							}
						}
						if (ordinal == std::numeric_limits<UInt16>::max())
						{
							if (availableAtlases.size()
								>= kMaximumAtlasSnapshotPages)
							{
								return nullptr;
							}
							ordinal = static_cast<UInt16>(
								availableAtlases.size());
							availableAtlases.push_back(page);
						}
						directPageOrdinals[roleIndex][rolePage] =
							ordinal;
					}
				}
			}
			else for (size_t roleIndex = 0; roleIndex < roleBitmaps.size(); ++roleIndex)
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
						bitmaps, pixelMode, renderMode, padding, byteClass);
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
				std::vector<UInt16> rolePageOrdinals(roleAtlases.size(),
					std::numeric_limits<UInt16>::max());
				for (size_t rolePage = 0; rolePage < roleAtlases.size(); ++rolePage)
				{
					if (!roleAtlases[rolePage])
						return nullptr;
					for (UInt16 candidate = 0;
						candidate < availableAtlases.size(); ++candidate)
					{
						if (availableAtlases[candidate].get()
								== roleAtlases[rolePage].get()
							|| AreAtlasResourcesBackedBySameTexture(
								*availableAtlases[candidate],
								*roleAtlases[rolePage]))
						{
							rolePageOrdinals[rolePage] = candidate;
							break;
						}
					}
					if (rolePageOrdinals[rolePage]
						== std::numeric_limits<UInt16>::max())
					{
						if (availableAtlases.size()
							>= kMaximumAtlasSnapshotPages)
							return nullptr;
						rolePageOrdinals[rolePage] = static_cast<UInt16>(
							availableAtlases.size());
						availableAtlases.push_back(roleAtlases[rolePage]);
					}
				}
				for (size_t bitmapIndex = 0; bitmapIndex < bitmaps.size(); ++bitmapIndex)
				{
					const UInt16 page = bitmapPageOrdinals[bitmapIndex];
					if (page >= roleAtlases.size() || !bitmaps[bitmapIndex])
						continue;
					const UInt16 resolvedPage = rolePageOrdinals[page];
					AtlasGlyphRecord* glyph = FindAtlasGlyph(*roleAtlases[page],
						bitmaps[bitmapIndex]->cacheId);
					if (!glyph || !CacheAtlasGlyphPlacement(
						*glyph, *roleAtlases[page], resolvedPage))
					{
						return nullptr;
					}
					AtlasGlyphPlacement placement = glyph->placement;
					const std::shared_ptr<AtlasResource>& physical =
						availableAtlases[resolvedPage];
					placement.atlasIdentity =
						reinterpret_cast<uintptr_t>(physical.get());
					placement.atlasGeneration = physical->generation;
					placement.atlasWidth = physical->width;
					placement.atlasHeight = physical->height;
					placementRecords[roleIndex].emplace(bitmaps[bitmapIndex]->cacheId,
						ResolvedPlacement{ resolvedPage, placement });
				}
			}
			if (availableAtlases.empty())
				return nullptr;
			if (effectConfig && effectConfig->bakedCoverage
				&& std::any_of(availableAtlases.begin(), availableAtlases.end(),
					[](const std::shared_ptr<AtlasResource>& atlas)
					{
						return !atlas
							|| atlas->pixelMode != AtlasPixelMode::A8;
					}))
			{
				// The aggressive contract is specifically one-byte A8. If the
				// renderer cannot realize that format, let the caller rebuild the
				// same CPU masks through the vanilla ARGB32 TileShader fallback.
				return nullptr;
			}
			if (!useNativeFontShader && !precomposedArgb
				&& availableAtlases.size() > 1)
			{
				// Vanilla Tile geometry can bind only one texture. The no-loader ARGB
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
					fallbackBitmaps, pixelMode, renderMode, padding,
					roleBitmaps[1].empty() ? VectorFontByteClass::SingleByte
						: VectorFontByteClass::DoubleByte);
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
			if (directSources)
			{
				outAtlases = availableAtlases;
				std::array<std::array<UInt32,
					kMaximumAtlasSnapshotPages>, 4> counts = {};
				for (PendingQuad& quad : pagedQuads)
				{
					const size_t roleIndex =
						static_cast<size_t>(quad.byteClass);
					const UInt16 rolePage =
						quad.source.placement.pageIndex;
					if (roleIndex >= directPageOrdinals.size()
						|| rolePage >= kMaximumAtlasSnapshotPages)
						return nullptr;
					const UInt16 page =
						directPageOrdinals[roleIndex][rolePage];
					if (page >= outAtlases.size()
						|| !outAtlases[page])
						return nullptr;
					quad.atlasPage = page;
					quad.atlasPlacement = quad.source.placement;
					quad.atlasPlacement.atlasIdentity =
						reinterpret_cast<uintptr_t>(outAtlases[page].get());
					quad.atlasPlacement.atlasGeneration =
						outAtlases[page]->generation;
					quad.atlasPlacement.atlasWidth =
						outAtlases[page]->width;
					quad.atlasPlacement.atlasHeight =
						outAtlases[page]->height;
					quad.atlasPlacement.pageIndex = page;
					const UInt32 rank = GetNativeFontLayerDrawRank(
						static_cast<UInt32>(quad.layer));
					if (rank >= counts.size())
						return nullptr;
					++counts[rank][page];
				}
				std::array<std::array<UInt32,
					kMaximumAtlasSnapshotPages>, 4> cursors = {};
				UInt32 offset = 0;
				for (size_t layer = 0; layer < counts.size(); ++layer)
				{
					for (size_t page = 0; page < counts[layer].size();
						++page)
					{
						cursors[layer][page] = offset;
						offset += counts[layer][page];
					}
				}
				thread_local std::vector<PendingQuad> orderedDirectQuads;
				orderedDirectQuads.resize(pagedQuads.size());
				for (const PendingQuad& quad : pagedQuads)
				{
					const UInt32 rank = GetNativeFontLayerDrawRank(
						static_cast<UInt32>(quad.layer));
					orderedDirectQuads[
						cursors[rank][quad.atlasPage]++] = quad;
				}
				pagedQuads.swap(orderedDirectQuads);
			}
			else
			{
				std::vector<UInt16> compactPageIndices(
					availableAtlases.size(),
					std::numeric_limits<UInt16>::max());
				for (PendingQuad& quad : pagedQuads)
				{
					const auto& rolePlacements = placementRecords[
						static_cast<size_t>(quad.byteClass)];
					const auto placement =
						rolePlacements.find(quad.source.CacheId());
					if (placement == rolePlacements.end())
						return nullptr;
					const UInt16 page = placement->second.page;
					UInt16& compactPage = compactPageIndices[page];
					if (compactPage == std::numeric_limits<UInt16>::max())
					{
						compactPage = static_cast<UInt16>(
							outAtlases.size());
						outAtlases.push_back(availableAtlases[page]);
					}
					quad.atlasPage = compactPage;
					quad.atlasPlacement = placement->second.placement;
					quad.atlasPlacement.pageIndex = compactPage;
				}
				const auto batchOrder = [](const PendingQuad& lhs,
					const PendingQuad& rhs)
				{
					const UInt32 lhsRank = GetNativeFontLayerDrawRank(
						static_cast<UInt32>(lhs.layer));
					const UInt32 rhsRank = GetNativeFontLayerDrawRank(
						static_cast<UInt32>(rhs.layer));
					if (lhsRank != rhsRank)
						return lhsRank < rhsRank;
					return lhs.atlasPage < rhs.atlasPage;
				};
				if (!std::is_sorted(pagedQuads.begin(),
					pagedQuads.end(), batchOrder))
				{
					std::stable_sort(pagedQuads.begin(),
						pagedQuads.end(), batchOrder);
				}
			}
			NativeFontEffectShapeConfig resolvedEffect;
			const NativeFontEffectShapeConfig* resolvedEffectPointer = nullptr;
			if (effectConfig)
			{
				resolvedEffect = *effectConfig;
				resolvedEffect.inverseAtlasWidth = 1.0f / outAtlases[0]->width;
				resolvedEffect.inverseAtlasHeight = 1.0f / outAtlases[0]->height;
				resolvedEffectPointer = &resolvedEffect;
			}
			NiTriShape* shape = CreateAtlasShape(font, pagedQuads, outAtlases, prepareObject,
				tileColor, useNativeFontShader, resolvedEffectPointer, batchOrigin);
			if (shape && outAtlases.size() == 1 && outAtlases[0]->transient
				&& outAtlases[0]->backend == AtlasBackend::DefaultPool)
			{
				std::lock_guard<std::mutex> lock(State().atlasMutex);
				RetireDefaultGeneration(*outAtlases[0]);
			}
			return shape;
		}
}

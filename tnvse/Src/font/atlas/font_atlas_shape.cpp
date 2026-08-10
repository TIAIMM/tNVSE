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
			std::array<std::array<std::shared_ptr<AtlasResource>,
				kMaximumAtlasSnapshotPages>, 2> directRolePages;
			bool directSources = true;
			bool directSourcesValid = true;
			for (const PendingQuad& quad : *activeQuads)
			{
				if (quad.source.bitmap)
				{
					roleUnique[static_cast<size_t>(quad.byteClass)].emplace(
						quad.source.bitmap->cacheId, quad.source.bitmap);
				}
				if (!quad.source.IsDirect())
				{
					directSources = false;
					continue;
				}
				if (!quad.source.atlas
					|| !IsAtlasGlyphPlacementForAtlas(
						quad.source.placement, *quad.source.atlas))
				{
					directSourcesValid = false;
					continue;
				}
				const size_t roleIndex =
					static_cast<size_t>(quad.byteClass);
				const UInt16 rolePage = quad.source.placement.pageIndex;
				if (roleIndex >= directRolePages.size()
					|| rolePage >= kMaximumAtlasSnapshotPages)
				{
					directSourcesValid = false;
					continue;
				}
				std::shared_ptr<AtlasResource>& known =
					directRolePages[roleIndex][rolePage];
				if (known && known.get() != quad.source.atlas.get())
				{
					directSourcesValid = false;
					continue;
				}
				known = quad.source.atlas;
			}
			std::array<std::array<UInt16, kMaximumAtlasSnapshotPages>, 2>
				directPageOrdinals;
			for (auto& role : directPageOrdinals)
				role.fill(std::numeric_limits<UInt16>::max());
			std::vector<std::shared_ptr<AtlasResource>> availableAtlases;
			if (directSources)
			{
				if (!directSourcesValid)
					return nullptr;
				// This mandatory walk now classifies the batch, gathers bitmap
				// fallbacks and proves direct Atlas ownership. The old successful
				// direct path visited every quad three times here.
				RecordFreeTypePerf(FreeTypePerfCounter::
					DirectSourceClassificationElementScanSaved,
					static_cast<UInt64>(activeQuads->size()) * 2u);
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

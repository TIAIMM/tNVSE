#include "font_vector_internal.h"

#include "font_manager.h"
#include "load_config.h"
#include "native_calls.h"

#include "NiDX9Renderer.hpp"
#include "NiFixedString.hpp"
#include "NiGlobalStringTable.hpp"
#include "NiPixelData.hpp"
#include "NiDX9TextureData.hpp"
#include "NiTexturingProperty.hpp"
#include "NiTriShape.hpp"
#include "NiTriShapeData.hpp"
#include "Utils/SafeWrite.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace fonthook::vectorfont
{
	namespace
	{
		constexpr UInt32 kAtlasPadding = 2;
		constexpr UInt32 kAtlasHardLimit = 4096;
		constexpr UInt32 kMaximumQuads = 16383;
		constexpr float kLayerDepthStep = 0.01f;
		constexpr UInt32 kAutomaticAtlasBudgetFallbackMB = 128;
		constexpr UInt32 kAutomaticAtlasBudgetMinimumMB = 64;
		constexpr UInt32 kAutomaticAtlasBudgetMaximumMB = 256;
		constexpr UInt32 kAutomaticAtlasBudgetQuantumMB = 16;

		enum class AtlasLayer : UInt8
		{
			Shadow = 0,
			Glow = 1,
			Outline = 2,
			Fill = 3,
		};

		enum class AtlasPixelMode : UInt8
		{
			Argb32 = 0,
			A8 = 1,
		};

		enum class AtlasBackend : UInt8
		{
			Managed = 0,
			DefaultPool = 1,
		};

		enum class AtlasRenderMode : UInt8
		{
			CpuEffects = 0,
			ShaderEffects = 1,
		};

		struct AtlasRect
		{
			UInt32 x = 0;
			UInt32 y = 0;
			UInt32 width = 0;
			UInt32 height = 0;
		};

		struct AtlasResource
		{
			NiTexturingPropertyPtr property;
			NiPixelDataPtr pixelData;
			UInt32 width = 0;
			UInt32 height = 0;
			UInt32 cursorX = kAtlasPadding;
			UInt32 cursorY = kAtlasPadding;
			UInt32 shelfHeight = 0;
			UInt32 padding = kAtlasPadding;
			UInt32 generation = 0;
			AtlasPixelMode pixelMode = AtlasPixelMode::Argb32;
			AtlasBackend backend = AtlasBackend::Managed;
			AtlasRenderMode renderMode = AtlasRenderMode::CpuEffects;
			bool resetPending = false;
			bool transient = false;
			std::vector<UInt8> pixels;
			std::unordered_map<UInt64, AtlasRect> placements;
			std::unordered_map<UInt64, std::shared_ptr<const GlyphBitmap>> residentBitmaps;
		};

		struct RetiredAtlasGeneration
		{
			std::shared_ptr<AtlasResource> resource;
		};

		struct AtlasCacheKey
		{
			UInt64 styleHash = 0;
			UInt32 fontId = 0;
			UInt32 scaleMilli = 1000;
			AtlasPixelMode pixelMode = AtlasPixelMode::Argb32;
			AtlasRenderMode renderMode = AtlasRenderMode::CpuEffects;
			UInt32 padding = kAtlasPadding;

			bool operator==(const AtlasCacheKey& other) const
			{
				return styleHash == other.styleHash && fontId == other.fontId
					&& scaleMilli == other.scaleMilli && pixelMode == other.pixelMode
					&& renderMode == other.renderMode && padding == other.padding;
			}
		};

		struct AtlasCacheKeyHash
		{
			size_t operator()(const AtlasCacheKey& key) const
			{
				size_t result = static_cast<size_t>(key.styleHash ^ (key.styleHash >> 32));
				result ^= static_cast<size_t>(key.fontId) * 0x9E3779B1u;
				result ^= static_cast<size_t>(key.scaleMilli) * 0x85EBCA77u;
				result ^= static_cast<size_t>(key.pixelMode) << 4;
				result ^= static_cast<size_t>(key.renderMode) << 8;
				result ^= static_cast<size_t>(key.padding) * 0x27D4EB2Du;
				return result;
			}
		};

		struct AtlasCacheEntry
		{
			std::shared_ptr<AtlasResource> resource;
			size_t bytes = 0;
			std::list<AtlasCacheKey>::iterator lru;
		};

		struct PendingQuad
		{
			std::shared_ptr<const GlyphBitmap> bitmap;
			NiPoint3 pen;
			NiColorA color;
			float offsetX = 0.0f;
			float offsetY = 0.0f;
			float rasterScale = 1.0f;
			float logicalTopEdge = 0.0f;
			UInt32 expansionPixels = 0;
			AtlasLayer layer = AtlasLayer::Fill;
		};

		struct BatchTemplateKey
		{
			uintptr_t atlasIdentity = 0;
			UInt64 contentHash = 0;
			UInt32 generation = 0;
			UInt32 quadCount = 0;

			bool operator==(const BatchTemplateKey& other) const
			{
				return atlasIdentity == other.atlasIdentity && contentHash == other.contentHash
					&& generation == other.generation && quadCount == other.quadCount;
			}
		};

		struct BatchTemplateKeyHash
		{
			size_t operator()(const BatchTemplateKey& key) const
			{
				return static_cast<size_t>(key.contentHash ^ (key.contentHash >> 32))
					^ key.atlasIdentity ^ (static_cast<size_t>(key.generation) << 8)
					^ key.quadCount;
			}
		};

		struct BatchTemplate
		{
			std::vector<NiPoint3> vertices;
			std::vector<NiPoint2> texture;
			std::vector<UInt16> indices;
		};

		struct BatchTemplateEntry
		{
			std::shared_ptr<const BatchTemplate> data;
			size_t bytes = 0;
			std::list<BatchTemplateKey>::iterator lru;
		};

		std::unordered_map<AtlasCacheKey, AtlasCacheEntry, AtlasCacheKeyHash> s_atlasCache;
		std::list<AtlasCacheKey> s_atlasLru;
		size_t s_atlasCacheBytes = 0;
		std::mutex s_atlasMutex;
		std::vector<RetiredAtlasGeneration> s_retiredAtlases;
		bool s_defaultPoolResetRegistered = false;
		bool s_defaultPoolShutdown = false;
		bool s_budgetResolved = false;
		size_t s_resolvedGpuBudgetBytes = 0;
		UInt32 s_lastAvailableTextureMemoryMB = 0;
		UInt32 s_defaultPoolFailureLogCount = 0;
		UInt32 s_atlasFailureLogCount = 0;
		std::unordered_set<UInt64> s_loggedAtlasBatches;
		std::unordered_set<UInt32> s_loggedVerticalMetricFonts;
		std::unordered_set<UInt64> s_loggedQualityDowngrades;
		std::unordered_map<BatchTemplateKey, BatchTemplateEntry,
			BatchTemplateKeyHash> s_batchCache;
		std::list<BatchTemplateKey> s_batchLru;
		size_t s_batchCacheBytes = 0;
		std::mutex s_batchMutex;

		NiTexture* GetAtlasTexture(const AtlasResource& resource);
		void RetireDefaultGeneration(const AtlasResource& resource);
		NiTexturingProperty* CreateManagedAtlasProperty(UInt32 width, UInt32 height,
			AtlasPixelMode mode, const std::vector<UInt8>& source,
			NiPixelDataPtr& outPixelData);

		class DefaultAtlasTexture : public NiTexture
		{
		public:
			static DefaultAtlasTexture* Create(IDirect3DTexture9* texture,
				AtlasPixelMode mode);
			UInt32 GetWidthEx() const;
			UInt32 GetHeightEx() const;

		private:
			static void* s_vtable[41];
		};

		struct ShaderEffectBuild
		{
			A8EffectShapeConfig config;
			UInt32 padding = kAtlasPadding;
		};

		void* DefaultAtlasTexture::s_vtable[41] = {};

		float ResolveModifierChannel(float source, float tile)
		{
			if (std::fabs(tile) > 0.000001f)
				return source / tile;
			return std::fabs(source) <= 0.000001f ? 1.0f : source;
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
			return result;
		}

		NiColorA ResolveEffectColor(const EffectStyle& effect, const NiColorA& source,
			const NiColorA& tile)
		{
			NiColorA result = effect.color;
			result.a *= ResolveModifierChannel(source.a, tile.a);
			return result;
		}

		NiColorA ResolveSafeTileColor(const std::vector<AtlasGlyphInstance>& glyphs,
			const NiColorA& requested)
		{
			NiColorA result = requested;
			for (const AtlasGlyphInstance& glyph : glyphs)
			{
				if (std::fabs(result.r) <= 0.000001f && std::fabs(glyph.color.r) > 0.000001f)
					result.r = 1.0f;
				if (std::fabs(result.g) <= 0.000001f && std::fabs(glyph.color.g) > 0.000001f)
					result.g = 1.0f;
				if (std::fabs(result.b) <= 0.000001f && std::fabs(glyph.color.b) > 0.000001f)
					result.b = 1.0f;
				if (std::fabs(result.a) <= 0.000001f && std::fabs(glyph.color.a) > 0.000001f)
					result.a = 1.0f;
			}
			return result;
		}

		float LayerDepth(AtlasLayer layer)
		{
			return static_cast<float>(static_cast<UInt32>(AtlasLayer::Fill)
				- static_cast<UInt32>(layer)) * kLayerDepthStep;
		}

		UInt32 NextPowerOfTwo(UInt32 value)
		{
			if (value <= 1)
				return 1;
			--value;
			value |= value >> 1;
			value |= value >> 2;
			value |= value >> 4;
			value |= value >> 8;
			value |= value >> 16;
			return value + 1;
		}

		UInt32 GetMaximumAtlasSize()
		{
			UInt32 result = kAtlasHardLimit;
			if (NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton())
			{
				result = std::min(result, std::min(renderer->m_kD3DCaps9.MaxTextureWidth,
					renderer->m_kD3DCaps9.MaxTextureHeight));
			}
			return std::max<UInt32>(64, result);
		}

		UInt32 ResolveAutomaticGpuBudgetMB(UInt32& availableMB)
		{
			availableMB = 0;
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer ? renderer->GetD3DDevice() : nullptr;
			if (!device)
				return kAutomaticAtlasBudgetFallbackMB;
			availableMB = device->GetAvailableTextureMem() / (1024u * 1024u);
			if (availableMB < 64 || availableMB > 4096)
				return kAutomaticAtlasBudgetFallbackMB;
			UInt32 budget = availableMB / 8u;
			budget = (budget / kAutomaticAtlasBudgetQuantumMB)
				* kAutomaticAtlasBudgetQuantumMB;
			return std::clamp(budget, kAutomaticAtlasBudgetMinimumMB,
				kAutomaticAtlasBudgetMaximumMB);
		}

		void ResolveGpuAtlasBudget(bool force)
		{
			if (s_budgetResolved && !force)
				return;
			const size_t previous = s_resolvedGpuBudgetBytes;
			UInt32 availableMB = 0;
			UInt32 resolvedMB = g_uiFreeTypeFontGpuAtlasCacheMB;
			const bool automatic = resolvedMB == 0;
			if (automatic)
				resolvedMB = ResolveAutomaticGpuBudgetMB(availableMB);
			s_lastAvailableTextureMemoryMB = availableMB;
			s_resolvedGpuBudgetBytes = static_cast<size_t>(resolvedMB)
				* 1024u * 1024u;
			s_budgetResolved = true;
			if (!previous)
			{
				if (automatic)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: GPU atlas cache budget configured=0 resolved=%uMB source=automatic availableTextureMem=%uMB",
						resolvedMB, availableMB);
				}
				else
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: GPU atlas cache budget configured=%u resolved=%uMB source=configured",
						g_uiFreeTypeFontGpuAtlasCacheMB, resolvedMB);
				}
			}
			else if (previous != s_resolvedGpuBudgetBytes)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: GPU atlas cache budget changed old=%uMB new=%uMB availableTextureMem=%uMB",
					static_cast<UInt32>(previous / (1024u * 1024u)), resolvedMB,
					availableMB);
			}
		}

		size_t GetAtlasCacheLimit()
		{
			if (!g_bEnableFreeTypeDefaultPoolAtlas)
			{
				return static_cast<size_t>(g_uiFreeTypeFontMemoryCacheMB)
					* 1024u * 1024u / 2u;
			}
			ResolveGpuAtlasBudget(false);
			return s_resolvedGpuBudgetBytes
				? s_resolvedGpuBudgetBytes
				: static_cast<size_t>(kAutomaticAtlasBudgetFallbackMB) * 1024u * 1024u;
		}

		UInt32 DefaultAtlasTexture::GetWidthEx() const
		{
			return m_pkRendererData ? m_pkRendererData->m_uiWidth : 0;
		}

		UInt32 DefaultAtlasTexture::GetHeightEx() const
		{
			return m_pkRendererData ? m_pkRendererData->m_uiHeight : 0;
		}

		DefaultAtlasTexture* DefaultAtlasTexture::Create(IDirect3DTexture9* texture,
			AtlasPixelMode mode)
		{
			if (!texture)
				return nullptr;
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			if (!renderer)
				return nullptr;
			auto* result = static_cast<DefaultAtlasTexture*>(
				NiMemObject::operator new(sizeof(DefaultAtlasTexture)));
			if (!result)
				return nullptr;
			ThisStdCall(0xA5C200, result);
			*reinterpret_cast<void**>(result) = reinterpret_cast<void*>(0x109B944);
			if (!s_vtable[0])
			{
				std::copy_n(*reinterpret_cast<void***>(result),
					_countof(s_vtable), s_vtable);
				ReplaceVTableEntry(s_vtable, 37, &DefaultAtlasTexture::GetWidthEx);
				ReplaceVTableEntry(s_vtable, 38, &DefaultAtlasTexture::GetHeightEx);
			}
			*reinterpret_cast<void***>(result) = s_vtable;
			result->m_kFormatPrefs.m_ePixelLayout = mode == AtlasPixelMode::A8
				? NiTexture::FormatPrefs::SINGLE_COLOR_8
				: NiTexture::FormatPrefs::TRUE_COLOR_32;
			result->m_kFormatPrefs.m_eAlphaFmt = NiTexture::FormatPrefs::SMOOTH;
			result->m_kFormatPrefs.m_eMipMapped = NiTexture::FormatPrefs::NO;

			auto* data = static_cast<NiDX9TextureData*>(
				NiMemObject::operator new(sizeof(NiDX9TextureData)));
			if (!data)
			{
				result->DeleteThis();
				return nullptr;
			}
			data = ThisStdCall<NiDX9TextureData*>(0xE8A260, data, result, renderer);
			if (!data)
			{
				result->DeleteThis();
				return nullptr;
			}
			data->m_pkD3DTexture = texture;
			if (!data->InitializeFromD3DTexture(texture))
			{
				data->m_pkD3DTexture = nullptr;
				data->DeleteThis();
				result->DeleteThis();
				return nullptr;
			}
			result->m_pkRendererData = data;
			ThisStdCall(0xA5F7B0, result);
			return result;
		}

		A8ShapeColorContract BuildColorContract(const std::vector<PendingQuad>& quads)
		{
			A8ShapeColorContract result;
			if (quads.empty())
				return result;
			result.minimumModifier = quads.front().color;
			result.maximumModifier = quads.front().color;
			for (const PendingQuad& quad : quads)
			{
				result.minimumModifier.r = std::min(result.minimumModifier.r, quad.color.r);
				result.minimumModifier.g = std::min(result.minimumModifier.g, quad.color.g);
				result.minimumModifier.b = std::min(result.minimumModifier.b, quad.color.b);
				result.minimumModifier.a = std::min(result.minimumModifier.a, quad.color.a);
				result.maximumModifier.r = std::max(result.maximumModifier.r, quad.color.r);
				result.maximumModifier.g = std::max(result.maximumModifier.g, quad.color.g);
				result.maximumModifier.b = std::max(result.maximumModifier.b, quad.color.b);
				result.maximumModifier.a = std::max(result.maximumModifier.a, quad.color.a);
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
				const UInt32 layer = static_cast<UInt32>(quad.layer);
				if (config.ranges.empty()
					|| config.ranges.back().layer != layer
					|| !SameColorModifier(config.ranges.back().colorModifier, quad.color))
				{
					A8DrawRange range;
					range.firstVertex = index * 4;
					range.startIndex = index * 6;
					range.layer = layer;
					range.colorModifier = quad.color;
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
			auto channel = [](float value)
			{
				return static_cast<UInt32>(std::lround(
					std::clamp(value, 0.0f, 1.0f) * 255.0f));
			};
			return (channel(color.a) << 24) | (channel(color.r) << 16)
				| (channel(color.g) << 8) | channel(color.b);
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
				const UInt32 rgba = PackColorModifierRgba(quad.color);
				UInt64 bakedId = BuildBakedBitmapId(quad.bitmap->cacheId, rgba);
				bakedId = BuildBakedBitmapId(bakedId, static_cast<UInt8>(quad.layer));
				auto found = unique.find(bakedId);
				if (found == unique.end())
				{
					auto baked = std::make_shared<GlyphBitmap>(*quad.bitmap);
					baked->cacheId = bakedId;
					baked->atlasRgb = rgba & 0x00FFFFFF;
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

		bool PackAtWidth(const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
			UInt32 atlasWidth, UInt32 maximumSize, UInt32& atlasHeight,
			std::unordered_map<UInt64, AtlasRect>& placements, UInt32 padding)
		{
			placements.clear();
			UInt32 x = padding;
			UInt32 y = padding;
			UInt32 shelfHeight = 0;
			for (const auto& bitmap : bitmaps)
			{
				const UInt32 width = static_cast<UInt32>(bitmap->width);
				const UInt32 height = static_cast<UInt32>(bitmap->height);
				if (width + padding * 2 > atlasWidth
					|| height + padding * 2 > maximumSize)
					return false;
				if (x + width + padding > atlasWidth)
				{
					x = padding;
					y += shelfHeight;
					shelfHeight = 0;
				}
				if (y + height + padding > maximumSize)
					return false;
				placements[bitmap->cacheId] = { x, y, width, height };
				x += width + padding * 2;
				shelfHeight = std::max(shelfHeight, height + padding * 2);
			}
			atlasHeight = NextPowerOfTwo(y + shelfHeight);
			return atlasHeight <= maximumSize;
		}

		bool PackAtlas(const std::vector<std::shared_ptr<const GlyphBitmap>>& source,
			UInt32& atlasWidth, UInt32& atlasHeight,
			std::unordered_map<UInt64, AtlasRect>& placements, UInt32 padding)
		{
			std::vector<std::shared_ptr<const GlyphBitmap>> bitmaps = source;
			std::sort(bitmaps.begin(), bitmaps.end(), [](const auto& lhs, const auto& rhs)
			{
				if (lhs->height != rhs->height)
					return lhs->height > rhs->height;
				if (lhs->width != rhs->width)
					return lhs->width > rhs->width;
				return lhs->cacheId < rhs->cacheId;
			});

			const UInt32 maximumSize = GetMaximumAtlasSize();
			UInt64 bestArea = UINT64_MAX;
			std::unordered_map<UInt64, AtlasRect> candidate;
			for (UInt32 width = 64; width <= maximumSize; width <<= 1)
			{
				UInt32 height = 0;
				if (!PackAtWidth(bitmaps, width, maximumSize, height, candidate, padding))
					continue;
				const UInt64 area = static_cast<UInt64>(width) * height;
				if (area < bestArea)
				{
					bestArea = area;
					atlasWidth = width;
					atlasHeight = height;
					placements = candidate;
				}
			}
			return bestArea != UINT64_MAX;
		}

		void TouchAtlasEntry(AtlasCacheEntry& entry, const AtlasCacheKey& key)
		{
			s_atlasLru.splice(s_atlasLru.begin(), s_atlasLru, entry.lru);
			entry.lru = s_atlasLru.begin();
		}

		void TrimAtlasCache()
		{
			while (s_atlasCacheBytes > GetAtlasCacheLimit() && !s_atlasLru.empty())
			{
				const AtlasCacheKey key = s_atlasLru.back();
				auto it = s_atlasCache.find(key);
				if (it != s_atlasCache.end())
				{
					RetireDefaultGeneration(*it->second.resource);
					s_atlasCacheBytes -= it->second.bytes;
					s_atlasCache.erase(it);
				}
				s_atlasLru.pop_back();
			}
		}

		bool PlaceBitmap(AtlasResource& resource, const GlyphBitmap& bitmap, AtlasRect& rect)
		{
			const UInt32 padding = resource.padding;
			const UInt32 width = static_cast<UInt32>(bitmap.width);
			const UInt32 height = static_cast<UInt32>(bitmap.height);
			if (width + padding * 2 > resource.width
				|| height + padding * 2 > resource.height)
				return false;
			if (resource.cursorX + width + padding > resource.width)
			{
				resource.cursorX = padding;
				resource.cursorY += resource.shelfHeight;
				resource.shelfHeight = 0;
			}
			if (resource.cursorY + height + padding > resource.height)
				return false;
			rect = { resource.cursorX, resource.cursorY, width, height };
			resource.cursorX += width + padding * 2;
			resource.shelfHeight = std::max(resource.shelfHeight,
				height + padding * 2);
			return true;
		}

		void TrimBatchCache()
		{
			const size_t limit = static_cast<size_t>(g_uiFreeTypeFontMemoryCacheMB)
				* 1024u * 1024u / 12u;
			while (s_batchCacheBytes > limit && !s_batchLru.empty())
			{
				const BatchTemplateKey key = s_batchLru.back();
				auto existing = s_batchCache.find(key);
				if (existing != s_batchCache.end())
				{
					s_batchCacheBytes -= existing->second.bytes;
					s_batchCache.erase(existing);
				}
				s_batchLru.pop_back();
			}
		}

		UInt32 AtlasBytesPerPixel(AtlasPixelMode mode)
		{
			return mode == AtlasPixelMode::A8 ? 1u : 4u;
		}

		size_t GetResidentMaskBytes(const AtlasResource& resource)
		{
			size_t result = 0;
			for (const auto& [id, bitmap] : resource.residentBitmaps)
			{
				if (bitmap)
					result += bitmap->alpha.size();
			}
			return result;
		}

		const NiPixelFormat* FindA8PixelFormat()
		{
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			if (!renderer)
				return nullptr;
			for (const auto& formatsByDepth : renderer->m_aapkTextureFormats)
			{
				for (const NiPixelFormat* format : formatsByDepth)
				{
					if (format && format->m_uiRendererHint == D3DFMT_A8
						&& format->m_ucBitsPerPixel == 8)
					{
						return format;
					}
				}
			}
			return nullptr;
		}

		bool PropertyUsesA8(NiTexturingProperty* property)
		{
			if (!property || !property->m_kMaps.GetSize() || !property->m_kMaps[0])
				return false;
			NiTexture* texture = property->m_kMaps[0]->m_spTexture;
			if (!texture || !texture->GetDX9RendererData())
				return false;
			LPDIRECT3DBASETEXTURE9 base = texture->GetDX9RendererData()->GetD3DTexture();
			if (!base)
				return false;
			IDirect3DTexture9* d3dTexture = nullptr;
			if (FAILED(base->QueryInterface(IID_IDirect3DTexture9,
				reinterpret_cast<void**>(&d3dTexture))) || !d3dTexture)
			{
				return false;
			}
			D3DSURFACE_DESC description = {};
			const bool result = SUCCEEDED(d3dTexture->GetLevelDesc(0, &description))
				&& description.Format == D3DFMT_A8;
			d3dTexture->Release();
			return result;
		}

		NiTexturingProperty* CreateManagedAtlasProperty(UInt32 width, UInt32 height,
			AtlasPixelMode mode, const std::vector<UInt8>& source,
			NiPixelDataPtr& outPixelData)
		{
			const NiPixelFormat* pixelFormat = mode == AtlasPixelMode::A8
				? FindA8PixelFormat()
				: reinterpret_cast<const NiPixelFormat*>(0x11AA2A0);
			if (!pixelFormat)
				return nullptr;
			NiPixelData* pixelData = static_cast<NiPixelData*>(
				NiMemObject::operator new(sizeof(NiPixelData)));
			if (!pixelData)
				return nullptr;
			pixelData = ThisStdCall<NiPixelData*>(0xA7C190, pixelData, width, height,
				pixelFormat, 1, 1);
			if (!pixelData || !pixelData->m_pucPixels || !pixelData->m_puiOffsetInBytes)
				return nullptr;
			UInt8* pixels = pixelData->m_pucPixels + *pixelData->m_puiOffsetInBytes;
			const size_t byteCount = static_cast<size_t>(width) * height
				* AtlasBytesPerPixel(mode);
			if (source.size() == byteCount)
				std::copy(source.begin(), source.end(), pixels);
			else
				std::fill(pixels, pixels + byteCount, static_cast<UInt8>(0));
			pixelData->bNoConvert = 1;
			outPixelData = pixelData;

			NiTexture::FormatPrefs prefs;
			prefs.m_ePixelLayout = mode == AtlasPixelMode::A8
				? NiTexture::FormatPrefs::SINGLE_COLOR_8
				: NiTexture::FormatPrefs::PIX_DEFAULT;
			prefs.m_eAlphaFmt = mode == AtlasPixelMode::A8
				? NiTexture::FormatPrefs::SMOOTH
				: NiTexture::FormatPrefs::ALPHA_DEFAULT;
			prefs.m_eMipMapped = NiTexture::FormatPrefs::NO;
			NiTexturingProperty* property = static_cast<NiTexturingProperty*>(
				NiMemObject::operator new(sizeof(NiTexturingProperty)));
			if (!property)
				return nullptr;
			NiFixedString textureName;
			textureName.m_kHandle = static_cast<char*>(
				NiGlobalStringTable::AddString("tNVSE FreeType Atlas"));
			property = ThisStdCall<NiTexturingProperty*>(0xA6ABB0,
				property, pixelData, &textureName, &prefs);
			if (!property || !property->m_kMaps.GetSize())
				return nullptr;
			ThisStdCall(0x60AEB0, property, 1);
			if (mode == AtlasPixelMode::A8 && !PropertyUsesA8(property))
				return nullptr;
			if (NiTexturingProperty::Map* map = property->m_kMaps[0])
			{
				map->m_usflags = static_cast<UInt16>((map->m_usflags & ~0x1Fu)
					| (NiTexturingProperty::FILTER_NEAREST << 2)
					| NiTexturingProperty::CLAMP_S_CLAMP_T);
			}

			return property;
		}

		void WriteBitmapPixels(UInt8* destination, LONG pitch, AtlasPixelMode mode,
			const GlyphBitmap& bitmap, const AtlasRect& rect,
			UInt32 destinationX = 0, UInt32 destinationY = 0)
		{
			const UInt32 bytesPerPixel = AtlasBytesPerPixel(mode);
			for (UInt32 y = 0; y < rect.height; ++y)
			{
				UInt8* row = destination + static_cast<size_t>(destinationY + y) * pitch
					+ static_cast<size_t>(destinationX) * bytesPerPixel;
				const UInt8* alpha = bitmap.alpha.data()
					+ static_cast<size_t>(y) * rect.width;
				if (mode == AtlasPixelMode::A8)
				{
					std::memcpy(row, alpha, rect.width);
				}
				else
				{
					const UInt8 blue = static_cast<UInt8>(bitmap.atlasRgb & 0xFF);
					const UInt8 green = static_cast<UInt8>((bitmap.atlasRgb >> 8) & 0xFF);
					const UInt8 red = static_cast<UInt8>((bitmap.atlasRgb >> 16) & 0xFF);
					for (UInt32 x = 0; x < rect.width; ++x)
					{
						row[x * 4 + 0] = blue;
						row[x * 4 + 1] = green;
						row[x * 4 + 2] = red;
						row[x * 4 + 3] = alpha[x];
					}
				}
			}
		}

		IDirect3DTexture9* CreateDynamicAtlasTexture(UInt32 width, UInt32 height,
			AtlasPixelMode mode)
		{
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer ? renderer->GetD3DDevice() : nullptr;
			if (!device)
				return nullptr;
			IDirect3DTexture9* texture = nullptr;
			const D3DFORMAT format = mode == AtlasPixelMode::A8
				? D3DFMT_A8 : D3DFMT_A8R8G8B8;
			if (FAILED(device->CreateTexture(width, height, 1, D3DUSAGE_DYNAMIC,
				format, D3DPOOL_DEFAULT, &texture, nullptr)))
			{
				return nullptr;
			}
			D3DSURFACE_DESC description = {};
			if (FAILED(texture->GetLevelDesc(0, &description))
				|| description.Format != format || description.Pool != D3DPOOL_DEFAULT)
			{
				texture->Release();
				return nullptr;
			}
			return texture;
		}

		bool PopulateDefaultTexture(IDirect3DTexture9* texture,
			const AtlasResource& resource, AtlasPixelMode mode)
		{
			if (!texture)
				return false;
			D3DLOCKED_RECT locked = {};
			if (FAILED(texture->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD)))
				return false;
			for (UInt32 y = 0; y < resource.height; ++y)
			{
				std::memset(static_cast<UInt8*>(locked.pBits)
					+ static_cast<size_t>(y) * locked.Pitch, 0,
					static_cast<size_t>(resource.width) * AtlasBytesPerPixel(mode));
			}
			for (const auto& [id, bitmap] : resource.residentBitmaps)
			{
				auto placement = resource.placements.find(id);
				if (!bitmap || placement == resource.placements.end())
					continue;
				const AtlasRect& rect = placement->second;
				WriteBitmapPixels(static_cast<UInt8*>(locked.pBits), locked.Pitch,
					mode, *bitmap, rect, rect.x, rect.y);
			}
			return SUCCEEDED(texture->UnlockRect(0));
		}

		NiTexturingProperty* CreatePropertyForDefaultTexture(
			IDirect3DTexture9*& d3dTexture, AtlasPixelMode mode)
		{
			DefaultAtlasTexture* texture = DefaultAtlasTexture::Create(d3dTexture, mode);
			if (!texture)
				return nullptr;
			d3dTexture = nullptr;
			std::vector<UInt8> bootstrapPixels(4, 0u);
			NiPixelDataPtr bootstrapData;
			NiTexturingProperty* property = CreateManagedAtlasProperty(
				1, 1, AtlasPixelMode::Argb32, bootstrapPixels, bootstrapData);
			if (!property || !property->m_kMaps.GetSize() || !property->m_kMaps[0])
			{
				texture->DeleteThis();
				return nullptr;
			}
			NiTexturingProperty::Map* map = property->m_kMaps[0];
			texture->IncRefCount();
			NiTexture* oldTexture = map->m_spTexture;
			map->m_spTexture = texture;
			if (oldTexture)
				oldTexture->DecRefCount();
			map->m_usflags = static_cast<UInt16>((map->m_usflags & ~0x1Fu)
				| (NiTexturingProperty::FILTER_NEAREST << 2)
				| NiTexturingProperty::CLAMP_S_CLAMP_T);
			return property;
		}

		bool CreateDefaultPoolAtlas(AtlasResource& resource, AtlasPixelMode requestedMode)
		{
			if (!g_bEnableFreeTypeDefaultPoolAtlas || s_defaultPoolShutdown)
				return false;
			AtlasPixelMode mode = requestedMode;
			for (UInt32 attempt = 0; attempt < 2; ++attempt)
			{
				if (mode == AtlasPixelMode::A8 && !IsA8RendererAvailable())
				{
					mode = AtlasPixelMode::Argb32;
					continue;
				}
				IDirect3DTexture9* d3dTexture = CreateDynamicAtlasTexture(
					resource.width, resource.height, mode);
				if (d3dTexture && PopulateDefaultTexture(d3dTexture, resource, mode))
				{
					NiTexturingProperty* property = CreatePropertyForDefaultTexture(
						d3dTexture, mode);
					if (property)
					{
						resource.property = property;
						resource.pixelData = nullptr;
						std::vector<UInt8>().swap(resource.pixels);
						resource.pixelMode = mode;
						resource.backend = AtlasBackend::DefaultPool;
						resource.resetPending = false;
						++resource.generation;
						if (g_bEnableFreeTypeFontRenderingLog)
						{
							FreeTypeFontDebugLog(
								"tnvse_freetype_font: default atlas created size=%ux%u format=%s pool=default usage=dynamic gpuBytes=%llu residentMaskBytes=%llu cpuBacking=0 bytes",
								resource.width, resource.height,
								mode == AtlasPixelMode::A8 ? "a8" : "argb32",
								static_cast<unsigned long long>(resource.width)
									* resource.height * AtlasBytesPerPixel(mode),
								static_cast<unsigned long long>(GetResidentMaskBytes(resource)));
						}
						return true;
					}
				}
				if (d3dTexture)
					d3dTexture->Release();
				if (mode == AtlasPixelMode::Argb32)
					break;
				mode = AtlasPixelMode::Argb32;
			}
			if (s_defaultPoolFailureLogCount++ < 8)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: DEFAULT atlas creation failed; using engine-managed atlas fallback");
			}
			return false;
		}

		UInt8* GetAtlasBacking(AtlasResource& resource)
		{
			if (!resource.pixels.empty())
				return resource.pixels.data();
			if (!resource.pixelData || !resource.pixelData->m_pucPixels
				|| !resource.pixelData->m_puiOffsetInBytes)
			{
				return nullptr;
			}
			return resource.pixelData->m_pucPixels + *resource.pixelData->m_puiOffsetInBytes;
		}

		void CopyBitmapToAtlas(AtlasResource& resource, const GlyphBitmap& bitmap,
			const AtlasRect& rect)
		{
			UInt8* pixels = GetAtlasBacking(resource);
			if (!pixels)
				return;
			for (UInt32 y = 0; y < rect.height; ++y)
			{
				for (UInt32 x = 0; x < rect.width; ++x)
				{
					const UInt8 alpha = bitmap.alpha[static_cast<size_t>(y) * rect.width + x];
					const size_t pixelIndex = static_cast<size_t>(rect.y + y)
						* resource.width + rect.x + x;
					if (resource.pixelMode == AtlasPixelMode::A8)
					{
						pixels[pixelIndex] = alpha;
					}
					else
					{
						UInt8* destination = pixels + pixelIndex * 4;
						destination[0] = static_cast<UInt8>(bitmap.atlasRgb & 0xFF);
						destination[1] = static_cast<UInt8>((bitmap.atlasRgb >> 8) & 0xFF);
						destination[2] = static_cast<UInt8>((bitmap.atlasRgb >> 16) & 0xFF);
						destination[3] = alpha;
					}
				}
			}
		}

		bool RecreateManagedAtlasProperty(AtlasResource& resource)
		{
			const size_t byteCount = static_cast<size_t>(resource.width) * resource.height
				* AtlasBytesPerPixel(resource.pixelMode);
			std::vector<UInt8> source;
			const bool movedTemporaryBacking = resource.pixels.size() == byteCount;
			if (movedTemporaryBacking)
				source = std::move(resource.pixels);
			else if (UInt8* current = GetAtlasBacking(resource))
				source.assign(current, current + byteCount);
			else
				return false;
			NiPixelDataPtr pixelData;
			NiTexturingProperty* property = CreateManagedAtlasProperty(
				resource.width, resource.height, resource.pixelMode, source, pixelData);
			if (!property)
			{
				if (movedTemporaryBacking)
					resource.pixels = std::move(source);
				return false;
			}
			resource.property = property;
			resource.pixelData = pixelData;
			resource.backend = AtlasBackend::Managed;
			std::vector<UInt8>().swap(resource.pixels);
			++resource.generation;
			RecordFreeTypePerf(FreeTypePerfCounter::AtlasUpload);
			RecordFreeTypePerf(FreeTypePerfCounter::AtlasUploadBytes,
				static_cast<UInt64>(byteCount));
			return true;
		}

		bool GrowManagedAtlas(AtlasResource& resource)
		{
			const UInt32 maximum = GetMaximumAtlasSize();
			if (resource.width >= maximum || resource.height >= maximum)
				return false;
			const UInt32 newWidth = std::min(maximum, resource.width * 2);
			const UInt32 newHeight = std::min(maximum, resource.height * 2);
			const UInt32 bytesPerPixel = AtlasBytesPerPixel(resource.pixelMode);
			std::vector<UInt8> expanded(static_cast<size_t>(newWidth) * newHeight
				* bytesPerPixel, 0u);
			UInt8* current = GetAtlasBacking(resource);
			if (!current)
				return false;
			for (UInt32 y = 0; y < resource.height; ++y)
			{
				std::copy_n(current + static_cast<size_t>(y) * resource.width * bytesPerPixel,
					static_cast<size_t>(resource.width) * bytesPerPixel,
					expanded.data() + static_cast<size_t>(y) * newWidth * bytesPerPixel);
			}
			resource.width = newWidth;
			resource.height = newHeight;
			resource.pixels.swap(expanded);
			RecordFreeTypePerf(FreeTypePerfCounter::AtlasGrown);
			return RecreateManagedAtlasProperty(resource);
		}

		bool UploadManagedAtlasRegion(AtlasResource& resource, const AtlasRect& dirty)
		{
			if (!resource.pixelData || !resource.pixelData->m_pucPixels
				|| !resource.pixelData->m_puiOffsetInBytes)
				return false;
			UInt8* sourcePixels = GetAtlasBacking(resource);
			if (!sourcePixels)
				return false;

			NiTexture* texture = GetAtlasTexture(resource);
			if (!texture || !texture->GetDX9RendererData())
				return true;
			LPDIRECT3DBASETEXTURE9 baseTexture = texture->GetDX9RendererData()->GetD3DTexture();
			if (!baseTexture)
				return true;
			IDirect3DTexture9* d3dTexture = nullptr;
			if (FAILED(baseTexture->QueryInterface(IID_IDirect3DTexture9,
				reinterpret_cast<void**>(&d3dTexture))) || !d3dTexture)
				return false;
			D3DSURFACE_DESC description = {};
			const D3DFORMAT expectedFormat = resource.pixelMode == AtlasPixelMode::A8
				? D3DFMT_A8 : D3DFMT_A8R8G8B8;
			if (FAILED(d3dTexture->GetLevelDesc(0, &description))
				|| description.Format != expectedFormat)
			{
				d3dTexture->Release();
				return false;
			}
			RECT rect = {
				static_cast<LONG>(dirty.x), static_cast<LONG>(dirty.y),
				static_cast<LONG>(dirty.x + dirty.width),
				static_cast<LONG>(dirty.y + dirty.height)
			};
			D3DLOCKED_RECT locked = {};
			const HRESULT result = d3dTexture->LockRect(0, &locked, &rect, 0);
			if (SUCCEEDED(result))
			{
				const UInt32 bytesPerPixel = AtlasBytesPerPixel(resource.pixelMode);
				for (UInt32 y = 0; y < dirty.height; ++y)
				{
					const UInt8* source = sourcePixels
						+ (static_cast<size_t>(dirty.y + y) * resource.width + dirty.x)
							* bytesPerPixel;
					UInt8* destination = static_cast<UInt8*>(locked.pBits)
						+ static_cast<size_t>(y) * locked.Pitch;
					std::memcpy(destination, source,
						static_cast<size_t>(dirty.width) * bytesPerPixel);
				}
				d3dTexture->UnlockRect(0);
			}
			d3dTexture->Release();
			if (SUCCEEDED(result))
			{
				RecordFreeTypePerf(FreeTypePerfCounter::AtlasUpload);
				RecordFreeTypePerf(FreeTypePerfCounter::AtlasUploadBytes,
					static_cast<UInt64>(dirty.width) * dirty.height
						* AtlasBytesPerPixel(resource.pixelMode));
			}
			return SUCCEEDED(result);
		}

		std::shared_ptr<AtlasResource> MakeGenerationSnapshot(
			const AtlasResource& resource)
		{
			auto snapshot = std::make_shared<AtlasResource>();
			snapshot->property = resource.property;
			snapshot->width = resource.width;
			snapshot->height = resource.height;
			snapshot->generation = resource.generation;
			snapshot->pixelMode = resource.pixelMode;
			snapshot->backend = resource.backend;
			snapshot->resetPending = resource.resetPending;
			snapshot->placements = resource.placements;
			snapshot->residentBitmaps = resource.residentBitmaps;
			return snapshot;
		}

		void RetireDefaultGeneration(const AtlasResource& resource)
		{
			if (resource.backend != AtlasBackend::DefaultPool || !resource.property)
			{
				return;
			}
			s_retiredAtlases.push_back({ MakeGenerationSnapshot(resource) });
		}

		void PruneRetiredAtlases()
		{
			s_retiredAtlases.erase(std::remove_if(s_retiredAtlases.begin(),
				s_retiredAtlases.end(), [](const RetiredAtlasGeneration& retired)
			{
				return !retired.resource || !retired.resource->property
					|| retired.resource->property->m_uiRefCount <= 1;
			}), s_retiredAtlases.end());
		}

		IDirect3DTexture9* QueryAtlasD3DTexture(const AtlasResource& resource)
		{
			NiTexture* texture = GetAtlasTexture(resource);
			NiDX9TextureData* data = texture ? texture->GetDX9RendererData() : nullptr;
			LPDIRECT3DBASETEXTURE9 base = data ? data->GetD3DTexture() : nullptr;
			if (!base)
				return nullptr;
			IDirect3DTexture9* result = nullptr;
			if (FAILED(base->QueryInterface(IID_IDirect3DTexture9,
				reinterpret_cast<void**>(&result))))
			{
				return nullptr;
			}
			return result;
		}

		struct PendingAtlasPlacement
		{
			std::shared_ptr<const GlyphBitmap> bitmap;
			AtlasRect rect;
		};

		bool UploadDefaultAtlasRegions(AtlasResource& resource,
			const std::vector<PendingAtlasPlacement>& pending, const AtlasRect& dirty)
		{
			IDirect3DTexture9* texture = QueryAtlasD3DTexture(resource);
			if (!texture)
				return false;
			RECT lockRect = {
				static_cast<LONG>(dirty.x), static_cast<LONG>(dirty.y),
				static_cast<LONG>(dirty.x + dirty.width),
				static_cast<LONG>(dirty.y + dirty.height)
			};
			D3DLOCKED_RECT locked = {};
			HRESULT result = texture->LockRect(0, &locked, &lockRect, 0);
			if (SUCCEEDED(result))
			{
				for (const PendingAtlasPlacement& entry : pending)
				{
					if (!entry.bitmap)
						continue;
					WriteBitmapPixels(static_cast<UInt8*>(locked.pBits), locked.Pitch,
						resource.pixelMode, *entry.bitmap, entry.rect,
						entry.rect.x - dirty.x, entry.rect.y - dirty.y);
				}
				result = texture->UnlockRect(0);
			}
			texture->Release();
			if (FAILED(result))
				return false;
			RecordFreeTypePerf(FreeTypePerfCounter::AtlasUpload);
			RecordFreeTypePerf(FreeTypePerfCounter::AtlasUploadBytes,
				static_cast<UInt64>(dirty.width) * dirty.height
					* AtlasBytesPerPixel(resource.pixelMode));
			return true;
		}

		void CommitDefaultCandidate(AtlasResource& resource, AtlasResource& candidate)
		{
			RetireDefaultGeneration(resource);
			resource.property = candidate.property;
			resource.pixelData = nullptr;
			resource.width = candidate.width;
			resource.height = candidate.height;
			resource.cursorX = candidate.cursorX;
			resource.cursorY = candidate.cursorY;
			resource.shelfHeight = candidate.shelfHeight;
			resource.padding = candidate.padding;
			resource.generation = candidate.generation;
			resource.pixelMode = candidate.pixelMode;
			resource.backend = AtlasBackend::DefaultPool;
			resource.renderMode = candidate.renderMode;
			resource.resetPending = false;
			resource.placements = std::move(candidate.placements);
			resource.residentBitmaps = std::move(candidate.residentBitmaps);
			std::vector<UInt8>().swap(resource.pixels);
		}

		bool AddBitmapsToDefaultAtlas(AtlasResource& resource,
			const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps)
		{
			const UInt32 maximum = GetMaximumAtlasSize();
			UInt32 targetWidth = resource.width;
			UInt32 targetHeight = resource.height;
			AtlasResource layout;
			std::vector<PendingAtlasPlacement> pending;
			for (;;)
			{
				layout.width = targetWidth;
				layout.height = targetHeight;
				layout.cursorX = resource.cursorX;
				layout.cursorY = resource.cursorY;
				layout.shelfHeight = resource.shelfHeight;
				layout.padding = resource.padding;
				pending.clear();
				bool placedAll = true;
				for (const auto& bitmap : bitmaps)
				{
					if (!bitmap || resource.placements.count(bitmap->cacheId))
						continue;
					AtlasRect rect;
					if (!PlaceBitmap(layout, *bitmap, rect))
					{
						placedAll = false;
						break;
					}
					pending.push_back({ bitmap, rect });
				}
				if (placedAll)
					break;
				if (targetWidth >= maximum || targetHeight >= maximum)
					return false;
				targetWidth = std::min(maximum, targetWidth * 2);
				targetHeight = std::min(maximum, targetHeight * 2);
			}
			if (pending.empty())
				return true;

			AtlasResource candidate;
			candidate.width = layout.width;
			candidate.height = layout.height;
			candidate.cursorX = layout.cursorX;
			candidate.cursorY = layout.cursorY;
			candidate.shelfHeight = layout.shelfHeight;
			candidate.padding = resource.padding;
			candidate.pixelMode = resource.pixelMode;
			candidate.backend = AtlasBackend::DefaultPool;
			candidate.renderMode = resource.renderMode;
			candidate.generation = resource.generation;
			candidate.placements = resource.placements;
			candidate.residentBitmaps = resource.residentBitmaps;
			UInt32 minX = candidate.width;
			UInt32 minY = candidate.height;
			UInt32 maxX = 0;
			UInt32 maxY = 0;
			for (const PendingAtlasPlacement& entry : pending)
			{
				candidate.placements[entry.bitmap->cacheId] = entry.rect;
				candidate.residentBitmaps[entry.bitmap->cacheId] = entry.bitmap;
				minX = std::min(minX, entry.rect.x);
				minY = std::min(minY, entry.rect.y);
				maxX = std::max(maxX, entry.rect.x + entry.rect.width);
				maxY = std::max(maxY, entry.rect.y + entry.rect.height);
			}
			const bool needsRebuild = !resource.property
				|| resource.width != candidate.width || resource.height != candidate.height;
			if (needsRebuild)
			{
				if (!CreateDefaultPoolAtlas(candidate, resource.pixelMode))
					return false;
				if (resource.property)
					RecordFreeTypePerf(FreeTypePerfCounter::AtlasGrown);
				CommitDefaultCandidate(resource, candidate);
				return true;
			}
			const AtlasRect dirty = { minX, minY, maxX - minX, maxY - minY };
			if (UploadDefaultAtlasRegions(resource, pending, dirty))
			{
				resource.cursorX = candidate.cursorX;
				resource.cursorY = candidate.cursorY;
				resource.shelfHeight = candidate.shelfHeight;
				resource.placements = std::move(candidate.placements);
				resource.residentBitmaps = std::move(candidate.residentBitmaps);
				return true;
			}
			if (!CreateDefaultPoolAtlas(candidate, resource.pixelMode))
				return false;
			CommitDefaultCandidate(resource, candidate);
			return true;
		}

		bool AddBitmapsToManagedAtlas(AtlasResource& resource,
			const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps)
		{
			AtlasResource planned;
			planned.width = resource.width;
			planned.height = resource.height;
			planned.cursorX = resource.cursorX;
			planned.cursorY = resource.cursorY;
			planned.shelfHeight = resource.shelfHeight;
			planned.padding = resource.padding;
			const UInt32 maximum = GetMaximumAtlasSize();
			for (const auto& bitmap : bitmaps)
			{
				if (!bitmap || resource.placements.count(bitmap->cacheId))
					continue;
				AtlasRect ignored;
				while (!PlaceBitmap(planned, *bitmap, ignored))
				{
					if (planned.width >= maximum || planned.height >= maximum)
						return false;
					planned.width = std::min(maximum, planned.width * 2);
					planned.height = std::min(maximum, planned.height * 2);
				}
			}
			while (resource.width < planned.width || resource.height < planned.height)
			{
				if (!GrowManagedAtlas(resource))
					return false;
			}
			UInt32 minX = resource.width;
			UInt32 minY = resource.height;
			UInt32 maxX = 0;
			UInt32 maxY = 0;
			bool changed = false;
			for (const auto& bitmap : bitmaps)
			{
				if (!bitmap || resource.placements.count(bitmap->cacheId))
					continue;
				AtlasRect rect;
				if (!PlaceBitmap(resource, *bitmap, rect))
					return false;
				resource.placements[bitmap->cacheId] = rect;
				resource.residentBitmaps[bitmap->cacheId] = bitmap;
				CopyBitmapToAtlas(resource, *bitmap, rect);
				minX = std::min(minX, rect.x);
				minY = std::min(minY, rect.y);
				maxX = std::max(maxX, rect.x + rect.width);
				maxY = std::max(maxY, rect.y + rect.height);
				changed = true;
			}
			if (!changed)
				return true;
			if (!resource.property)
				return RecreateManagedAtlasProperty(resource);
			const AtlasRect dirty = { minX, minY, maxX - minX, maxY - minY };
			if (UploadManagedAtlasRegion(resource, dirty))
				return true;
			return RecreateManagedAtlasProperty(resource);
		}

		bool AddBitmapsToAtlas(AtlasResource& resource,
			const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps)
		{
			if (resource.backend == AtlasBackend::DefaultPool)
			{
				if (AddBitmapsToDefaultAtlas(resource, bitmaps))
					return true;
				// Keep an established DEFAULT generation intact. The caller can use a
				// transient atlas for this text without invalidating existing shapes.
				if (resource.property)
					return false;
				resource.backend = AtlasBackend::Managed;
				resource.pixelMode = AtlasPixelMode::Argb32;
				resource.property = nullptr;
				resource.pixelData = nullptr;
				resource.pixels.assign(static_cast<size_t>(resource.width)
					* resource.height * AtlasBytesPerPixel(resource.pixelMode), 0u);
				resource.placements.clear();
				resource.residentBitmaps.clear();
				resource.cursorX = resource.padding;
				resource.cursorY = resource.padding;
				resource.shelfHeight = 0;
			}
			return AddBitmapsToManagedAtlas(resource, bitmaps);
		}

		std::shared_ptr<AtlasResource> GetAtlasResource(
			const FontConfig& config, float rasterScale,
			const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
			AtlasPixelMode pixelMode, AtlasRenderMode renderMode,
			UInt32 padding)
		{
			AtlasCacheKey key = {
				config.styleHash,
				config.fontId,
				static_cast<UInt32>(std::lround(rasterScale * 1000.0f)),
				pixelMode,
				renderMode,
				padding
			};

			std::lock_guard<std::mutex> lock(s_atlasMutex);
			auto existing = s_atlasCache.find(key);
			if (existing != s_atlasCache.end())
			{
				RecordFreeTypePerf(FreeTypePerfCounter::AtlasHit);
				TouchAtlasEntry(existing->second, key);
				std::shared_ptr<AtlasResource> resource = existing->second.resource;
				if (AddBitmapsToAtlas(*resource, bitmaps))
				{
					const size_t bytes = static_cast<size_t>(resource->width)
						* resource->height * AtlasBytesPerPixel(resource->pixelMode);
					if (bytes != existing->second.bytes)
					{
						s_atlasCacheBytes -= existing->second.bytes;
						existing->second.bytes = bytes;
						s_atlasCacheBytes += bytes;
						TrimAtlasCache();
					}
					return resource;
				}
				return nullptr;
			}
			auto resource = std::make_shared<AtlasResource>();
			RecordFreeTypePerf(FreeTypePerfCounter::AtlasCreated);
			resource->pixelMode = pixelMode;
			resource->backend = g_bEnableFreeTypeDefaultPoolAtlas
				? AtlasBackend::DefaultPool : AtlasBackend::Managed;
			resource->renderMode = renderMode;
			resource->padding = padding;
			resource->width = std::min<UInt32>(512, GetMaximumAtlasSize());
			resource->height = resource->width;
			resource->cursorX = padding;
			resource->cursorY = padding;
			if (resource->backend == AtlasBackend::Managed)
			{
				resource->pixels.assign(static_cast<size_t>(resource->width)
					* resource->height * AtlasBytesPerPixel(resource->pixelMode), 0u);
			}
			if (!AddBitmapsToAtlas(*resource, bitmaps))
				return nullptr;
			const size_t bytes = static_cast<size_t>(resource->width) * resource->height
				* AtlasBytesPerPixel(resource->pixelMode);
			s_atlasLru.push_front(key);
			s_atlasCache.emplace(key,
				AtlasCacheEntry{ resource, bytes, s_atlasLru.begin() });
			s_atlasCacheBytes += bytes;
			TrimAtlasCache();
			return resource;
		}

		std::shared_ptr<AtlasResource> CreateTransientAtlas(
			const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
			AtlasPixelMode pixelMode, AtlasRenderMode renderMode,
			UInt32 padding)
		{
			auto resource = std::make_shared<AtlasResource>();
			resource->pixelMode = pixelMode;
			resource->backend = g_bEnableFreeTypeDefaultPoolAtlas
				? AtlasBackend::DefaultPool : AtlasBackend::Managed;
			resource->renderMode = renderMode;
			resource->padding = padding;
			resource->transient = true;
			if (!PackAtlas(bitmaps, resource->width, resource->height,
				resource->placements, padding))
			{
				return nullptr;
			}
			for (const auto& bitmap : bitmaps)
			{
				if (bitmap)
					resource->residentBitmaps[bitmap->cacheId] = bitmap;
			}
			if (resource->backend == AtlasBackend::DefaultPool
				&& CreateDefaultPoolAtlas(*resource, pixelMode))
			{
				return resource;
			}
			resource->backend = AtlasBackend::Managed;
			resource->pixelMode = AtlasPixelMode::Argb32;
			resource->pixels.assign(static_cast<size_t>(resource->width)
				* resource->height * AtlasBytesPerPixel(resource->pixelMode), 0u);
			for (const auto& bitmap : bitmaps)
			{
				if (bitmap)
					CopyBitmapToAtlas(*resource, *bitmap,
						resource->placements.at(bitmap->cacheId));
			}
			return RecreateManagedAtlasProperty(*resource) ? resource : nullptr;
		}

		void AddPendingQuad(std::vector<PendingQuad>& quads,
			const std::shared_ptr<const GlyphBitmap>& bitmap,
			const AtlasGlyphInstance& instance, const NiColorA& color,
			float offsetX, float offsetY, float rasterScale, AtlasLayer layer,
			UInt32 expansionPixels = 0)
		{
			if (bitmap && bitmap->width > 0 && bitmap->height > 0 && !bitmap->alpha.empty())
				quads.push_back({ bitmap, instance.pen, color, offsetX, offsetY,
					rasterScale, instance.glyph.metrics ? instance.glyph.metrics->fTopEdge : 0.0f,
					expansionPixels, layer });
		}

		bool BuildPendingQuads(RuntimeFont& runtime,
			const std::vector<AtlasGlyphInstance>& glyphs, float rasterScale,
			const std::array<bool, 4>& included, const NiColorA& tileColor,
			std::vector<PendingQuad>& quads)
		{
			struct PreparedGlyph
			{
				const AtlasGlyphInstance* instance = nullptr;
				std::shared_ptr<const GlyphBitmap> fill;
				std::shared_ptr<const GlyphBitmap> glow;
				std::shared_ptr<const GlyphBitmap> outline;
			};

			quads.clear();
			const FontConfig& config = GetRuntimeConfig(runtime);
			thread_local std::vector<PreparedGlyph> prepared;
			prepared.clear();
			prepared.reserve(glyphs.size());
			for (const AtlasGlyphInstance& instance : glyphs)
			{
				PreparedGlyph glyph;
				glyph.instance = &instance;
				glyph.fill = GetGlyphBitmap(runtime, instance.glyph,
					GlyphMaskType::Fill, rasterScale);
				if (!glyph.fill)
					return false;
				if (included[static_cast<size_t>(AtlasLayer::Glow)] && config.glow.enabled)
				{
					glyph.glow = GetGlyphBitmap(runtime, instance.glyph,
						GlyphMaskType::Glow, rasterScale);
					if (!glyph.glow)
						return false;
				}
				if (included[static_cast<size_t>(AtlasLayer::Outline)] && config.outline.enabled)
				{
					glyph.outline = GetGlyphBitmap(runtime, instance.glyph,
						GlyphMaskType::Outline, rasterScale);
					if (!glyph.outline)
						return false;
				}
				prepared.push_back(std::move(glyph));
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
						config.shadow.x, config.shadow.y, rasterScale, AtlasLayer::Shadow);
				}
			}
			if (included[static_cast<size_t>(AtlasLayer::Glow)] && config.glow.enabled)
			{
				for (const PreparedGlyph& glyph : prepared)
				{
					AddPendingQuad(quads, glyph.glow, *glyph.instance,
						ResolveEffectColor(config.glow, glyph.instance->color, tileColor),
						0.0f, 0.0f, rasterScale, AtlasLayer::Glow);
				}
			}
			if (included[static_cast<size_t>(AtlasLayer::Outline)] && config.outline.enabled)
			{
				for (const PreparedGlyph& glyph : prepared)
				{
					AddPendingQuad(quads, glyph.outline, *glyph.instance,
						ResolveEffectColor(config.outline, glyph.instance->color, tileColor),
						0.0f, 0.0f, rasterScale, AtlasLayer::Outline);
				}
			}
			if (included[static_cast<size_t>(AtlasLayer::Fill)])
			{
				for (const PreparedGlyph& glyph : prepared)
				{
					AddPendingQuad(quads, glyph.fill, *glyph.instance,
						ResolveFillColor(config.fontColor, glyph.instance->color, tileColor),
						0.0f, 0.0f, rasterScale, AtlasLayer::Fill);
				}
			}
			return true;
		}

		bool BuildShaderEffectQuads(RuntimeFont& runtime,
			const std::vector<AtlasGlyphInstance>& glyphs, float rasterScale,
			EffectQuality quality, const NiColorA& tileColor,
			std::vector<PendingQuad>& quads,
			ShaderEffectBuild& build)
		{
			quads.clear();
			build = {};
			build.config.enabled = true;
			build.config.shaderEffects = true;
			build.config.quality = quality;
			const FontConfig& config = GetRuntimeConfig(runtime);
			const bool needsSdf = HasSdfEffects(config);
			UInt32 sdfSpread = 0;
			if (needsSdf && !ResolveSdfSpread(config, rasterScale, sdfSpread))
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
			};
			thread_local std::vector<PreparedShaderGlyph> prepared;
			prepared.clear();
			prepared.reserve(glyphs.size());
			for (const AtlasGlyphInstance& instance : glyphs)
			{
				PreparedShaderGlyph glyph;
				glyph.instance = &instance;
				glyph.fill = GetGlyphBitmap(runtime, instance.glyph,
					GlyphMaskType::Fill, rasterScale);
				if (!glyph.fill)
					return false;
				if (needsSdf)
				{
					glyph.sdf = GetGlyphBitmap(runtime, instance.glyph,
						GlyphMaskType::DistanceField, rasterScale, sdfSpread);
					if (!glyph.sdf)
						return false;
				}
				prepared.push_back(std::move(glyph));
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
						*entry.instance, layerColor, offsetX, offsetY, rasterScale, layer);
				}
			};

			addRange(AtlasLayer::Shadow, config.shadow.enabled,
				config.shadow.blur > 0.0f, config.shadow.x, config.shadow.y);
			addRange(AtlasLayer::Glow, config.glow.enabled, true, 0.0f, 0.0f);
			addRange(AtlasLayer::Outline, config.outline.enabled, true, 0.0f, 0.0f);
			addRange(AtlasLayer::Fill, true, false, 0.0f, 0.0f);
			return true;
		}

		NiTexture* GetAtlasTexture(const AtlasResource& resource)
		{
			if (!resource.property || !resource.property->m_kMaps.GetSize())
				return nullptr;
			NiTexturingProperty::Map* map = resource.property->m_kMaps[0];
			return map ? map->m_spTexture : nullptr;
		}

		BatchTemplateKey BuildBatchTemplateKey(const std::vector<PendingQuad>& quads,
			const AtlasResource& atlas)
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
			add(&atlas.width, sizeof(atlas.width));
			add(&atlas.height, sizeof(atlas.height));
			for (const PendingQuad& quad : quads)
			{
				add(&quad.bitmap->cacheId, sizeof(quad.bitmap->cacheId));
				add(&quad.pen, sizeof(quad.pen));
				add(&quad.color, sizeof(quad.color));
				add(&quad.offsetX, sizeof(quad.offsetX));
				add(&quad.offsetY, sizeof(quad.offsetY));
				add(&quad.rasterScale, sizeof(quad.rasterScale));
				add(&quad.expansionPixels, sizeof(quad.expansionPixels));
				add(&quad.layer, sizeof(quad.layer));
			}
			return { reinterpret_cast<uintptr_t>(&atlas), hash, atlas.generation,
				static_cast<UInt32>(quads.size()) };
		}

		std::shared_ptr<const BatchTemplate> GetBatchTemplate(Font& font,
			const std::vector<PendingQuad>& quads, const std::shared_ptr<AtlasResource>& atlas)
		{
			const BatchTemplateKey key = BuildBatchTemplateKey(quads, *atlas);
			{
				std::lock_guard<std::mutex> lock(s_batchMutex);
				auto existing = s_batchCache.find(key);
				if (existing != s_batchCache.end())
				{
					s_batchLru.splice(s_batchLru.begin(), s_batchLru, existing->second.lru);
					existing->second.lru = s_batchLru.begin();
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
				const AtlasRect& rect = atlas->placements.at(quad.bitmap->cacheId);
				const float scale = quad.rasterScale;
				const float expansion = static_cast<float>(quad.expansionPixels);
				const float x0 = std::round((quad.pen.x + quad.offsetX) * scale
					+ static_cast<float>(quad.bitmap->left) - expansion) / scale;
				const float z0 = std::round((quad.pen.z + quad.bitmap->baselineOffset
					- quad.offsetY) * scale + static_cast<float>(quad.bitmap->top)
					+ expansion) / scale;
				const float x1 = x0 + (static_cast<float>(quad.bitmap->width)
					+ expansion * 2.0f) / scale;
				const float z1 = z0 - (static_cast<float>(quad.bitmap->height)
					+ expansion * 2.0f) / scale;
				if (g_bEnableFreeTypeFontRenderingLog && quad.layer == AtlasLayer::Fill)
				{
					bool shouldLog = false;
					{
						std::lock_guard<std::mutex> lock(s_atlasMutex);
						shouldLog = s_loggedVerticalMetricFonts.insert(font.iFontNum).second;
					}
					if (shouldLog)
					{
						const float bitmapTop = static_cast<float>(quad.bitmap->top) / scale
							+ quad.bitmap->baselineOffset;
						FreeTypeFontDebugLog(
							"tnvse_freetype_font: first atlas vertical metrics font=%u scale=%.3f bitmapTop=%.3f logicalTopEdge=%.3f delta=%.3f baselineOffset=%.3f penZ=%.3f quadTop=%.3f",
							font.iFontNum, scale, bitmapTop, quad.logicalTopEdge,
							bitmapTop - quad.logicalTopEdge, quad.bitmap->baselineOffset,
							quad.pen.z, z0);
					}
				}
				const float depth = quad.pen.y + LayerDepth(quad.layer);
				const float u0 = (static_cast<float>(rect.x) - expansion) / atlas->width;
				const float v0 = (static_cast<float>(rect.y) - expansion) / atlas->height;
				const float u1 = (static_cast<float>(rect.x + rect.width) + expansion)
					/ atlas->width;
				const float v1 = (static_cast<float>(rect.y + rect.height) + expansion)
					/ atlas->height;
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
				std::lock_guard<std::mutex> lock(s_batchMutex);
				s_batchLru.push_front(key);
				s_batchCache.emplace(key,
					BatchTemplateEntry{ result, bytes, s_batchLru.begin() });
				s_batchCacheBytes += bytes;
				TrimBatchCache();
			}
			return result;
		}

		NiTriShape* CreateAtlasShape(Font& font, const std::vector<PendingQuad>& quads,
			const std::shared_ptr<AtlasResource>& atlas, bool prepareObject,
			const NiColorA& tileColor, const A8EffectShapeConfig* effectConfig)
		{
			if (!atlas || quads.empty() || quads.size() > kMaximumQuads)
				return nullptr;
			NiTriShape* shape = font.MakeTriShape(static_cast<int>(quads.size()),
				&tileColor, false);
			if (!shape || !shape->GetModelData())
				return nullptr;
			shape->m_kLocal.m_Translate = NiPoint3(0.0f, 0.0f, 0.0f);
			shape->RemoveProperty(NiProperty::TEXTURING);
			shape->AddProperty(atlas->property);
			shape->UpdateProperties();
			if (NiShadeProperty* shade = shape->GetShadeProperty())
			{
				if (shade->m_eShaderType == NiShadeProperty::PROP_Tile)
				{
					if (NiTexture* texture = GetAtlasTexture(*atlas))
						ThisStdCall(0xBB7A10, shade, texture);
				}
			}

			NiTriShapeData* data = shape->GetModelData();
			const std::shared_ptr<const BatchTemplate> batch =
				GetBatchTemplate(font, quads, atlas);
			if (!batch)
				return nullptr;
			std::copy(batch->vertices.begin(), batch->vertices.end(), data->m_pkVertex);
			std::copy(batch->texture.begin(), batch->texture.end(), data->m_pkTexture);
			std::copy(batch->indices.begin(), batch->indices.end(), data->m_pusTriList);
			ThisStdCall(0xA7EE30, &data->m_kBound, data->m_usVertices, data->m_pkVertex);
			if (IsA8RendererAvailable())
			{
				A8EffectShapeConfig resolvedEffect = effectConfig
					? *effectConfig : A8EffectShapeConfig{};
				BuildA8DrawRanges(quads, resolvedEffect);
				const A8ShapeColorContract colorContract = BuildColorContract(quads);
				if (!PrepareA8AtlasShape(shape, font.iFontNum,
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
			std::shared_ptr<AtlasResource>& outAtlas,
			const NiColorA& tileColor,
			const A8EffectShapeConfig* effectConfig = nullptr)
		{
			const std::vector<PendingQuad>* activeQuads = &quads;
			const std::vector<std::shared_ptr<const GlyphBitmap>>* activeBitmaps = &bitmaps;
			thread_local std::vector<PendingQuad> bakedQuads;
			thread_local std::vector<std::shared_ptr<const GlyphBitmap>> bakedBitmaps;
			if (pixelMode == AtlasPixelMode::Argb32 && !IsA8RendererAvailable())
			{
				BuildBakedArgbFallback(quads, bakedQuads, bakedBitmaps);
				activeQuads = &bakedQuads;
				activeBitmaps = &bakedBitmaps;
			}

			outAtlas = GetAtlasResource(config, rasterScale, *activeBitmaps, pixelMode,
				renderMode, padding);
			if (!outAtlas)
				outAtlas = CreateTransientAtlas(*activeBitmaps, pixelMode, renderMode, padding);
			if (!outAtlas)
				return nullptr;
			A8EffectShapeConfig resolvedEffect;
			const A8EffectShapeConfig* resolvedEffectPointer = nullptr;
			if (effectConfig)
			{
				resolvedEffect = *effectConfig;
				resolvedEffect.inverseAtlasWidth = 1.0f / outAtlas->width;
				resolvedEffect.inverseAtlasHeight = 1.0f / outAtlas->height;
				resolvedEffectPointer = &resolvedEffect;
			}
			NiTriShape* shape = CreateAtlasShape(font, *activeQuads, outAtlas, prepareObject,
				tileColor, resolvedEffectPointer);
			if (shape && outAtlas->transient
				&& outAtlas->backend == AtlasBackend::DefaultPool)
			{
				std::lock_guard<std::mutex> lock(s_atlasMutex);
				RetireDefaultGeneration(*outAtlas);
			}
			return shape;
		}

		void ReleaseDefaultPoolTexture(AtlasResource& resource)
		{
			if (resource.backend != AtlasBackend::DefaultPool)
				return;
			NiTexture* texture = GetAtlasTexture(resource);
			NiDX9TextureData* data = texture ? texture->GetDX9RendererData() : nullptr;
			if (data && data->m_pkD3DTexture)
			{
				data->m_pkD3DTexture->Release();
				data->m_pkD3DTexture = nullptr;
				data->m_uiLevels = 0;
			}
			resource.resetPending = true;
		}

		bool RebuildDefaultPoolTexture(AtlasResource& resource)
		{
			if (resource.backend != AtlasBackend::DefaultPool || !resource.property)
				return true;
			NiTexture* texture = GetAtlasTexture(resource);
			NiDX9TextureData* data = texture ? texture->GetDX9RendererData() : nullptr;
			if (!texture || !data)
				return false;
			AtlasPixelMode mode = resource.pixelMode;
			for (UInt32 attempt = 0; attempt < 2; ++attempt)
			{
				IDirect3DTexture9* d3dTexture = CreateDynamicAtlasTexture(
					resource.width, resource.height, mode);
				if (d3dTexture && PopulateDefaultTexture(d3dTexture, resource, mode))
				{
					data->m_pkD3DTexture = d3dTexture;
					if (data->InitializeFromD3DTexture(d3dTexture))
					{
						texture->m_kFormatPrefs.m_ePixelLayout = mode == AtlasPixelMode::A8
							? NiTexture::FormatPrefs::SINGLE_COLOR_8
							: NiTexture::FormatPrefs::TRUE_COLOR_32;
						resource.pixelMode = mode;
						resource.resetPending = false;
						RecordFreeTypePerf(FreeTypePerfCounter::AtlasUpload);
						RecordFreeTypePerf(FreeTypePerfCounter::AtlasUploadBytes,
							static_cast<UInt64>(resource.width) * resource.height
								* AtlasBytesPerPixel(mode));
						return true;
					}
					data->m_pkD3DTexture = nullptr;
				}
				if (d3dTexture)
					d3dTexture->Release();
				if (mode == AtlasPixelMode::Argb32)
					break;
				mode = AtlasPixelMode::Argb32;
			}
			resource.resetPending = true;
			return false;
		}

		bool DefaultPoolResetCallback(bool beforeReset, void*)
		{
			if (s_defaultPoolShutdown)
				return true;
			const auto started = std::chrono::steady_clock::now();
			UInt32 processed = 0;
			UInt32 failed = 0;
			{
				std::lock_guard<std::mutex> lock(s_atlasMutex);
				if (!beforeReset)
					ResolveGpuAtlasBudget(true);
				auto process = [&](AtlasResource& resource)
				{
					if (resource.backend != AtlasBackend::DefaultPool)
						return;
					++processed;
					if (beforeReset)
						ReleaseDefaultPoolTexture(resource);
					else if (!RebuildDefaultPoolTexture(resource))
						++failed;
				};
				for (auto& [key, entry] : s_atlasCache)
				{
					if (entry.resource)
						process(*entry.resource);
				}
				for (RetiredAtlasGeneration& retired : s_retiredAtlases)
				{
					if (retired.resource)
						process(*retired.resource);
				}
				if (!beforeReset)
				{
					s_atlasCacheBytes = 0;
					for (auto& [key, entry] : s_atlasCache)
					{
						if (!entry.resource)
							continue;
						entry.bytes = static_cast<size_t>(entry.resource->width)
							* entry.resource->height
							* AtlasBytesPerPixel(entry.resource->pixelMode);
						s_atlasCacheBytes += entry.bytes;
					}
					PruneRetiredAtlases();
					TrimAtlasCache();
				}
			}
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - started).count();
				FreeTypeFontDebugLog(
					"tnvse_freetype_font: DEFAULT atlas reset phase=%s generations=%u failed=%u timeUs=%lld",
					beforeReset ? "release" : "rebuild", processed, failed,
					static_cast<long long>(elapsed));
			}
			return true;
		}
	}

	void InitializeDefaultPoolAtlasLifecycle()
	{
		if (!g_bEnableFreeTypeFontRendering || !g_bEnableFreeTypeDefaultPoolAtlas
			|| s_defaultPoolResetRegistered)
			return;
		s_defaultPoolShutdown = false;
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		if (!renderer || !renderer->GetD3DDevice())
			return;
		ResolveGpuAtlasBudget(true);
		ThisStdCall<UInt32>(0x86BAE0, renderer, DefaultPoolResetCallback, nullptr);
		s_defaultPoolResetRegistered = true;
		gLog.FormattedMessage(
			"tnvse_freetype_font: registered DEFAULT atlas device reset lifecycle");
	}

	void PumpDefaultPoolAtlasLifecycle()
	{
		if (!g_bEnableFreeTypeFontRendering || !g_bEnableFreeTypeDefaultPoolAtlas
			|| s_defaultPoolShutdown)
			return;
		InitializeDefaultPoolAtlasLifecycle();
		static UInt32 retryFrame = 0;
		const bool retry = (++retryFrame % 120u) == 0;
		std::lock_guard<std::mutex> lock(s_atlasMutex);
		PruneRetiredAtlases();
		if (retry)
		{
			for (auto& [key, entry] : s_atlasCache)
			{
				if (entry.resource && entry.resource->resetPending)
					RebuildDefaultPoolTexture(*entry.resource);
			}
			for (RetiredAtlasGeneration& retired : s_retiredAtlases)
			{
				if (retired.resource && retired.resource->resetPending)
					RebuildDefaultPoolTexture(*retired.resource);
			}
		}
		TrimAtlasCache();
	}

	void ShutdownDefaultPoolAtlasLifecycle()
	{
		s_defaultPoolShutdown = true;
		{
			std::lock_guard<std::mutex> lock(s_atlasMutex);
			s_atlasCache.clear();
			s_atlasLru.clear();
			s_atlasCacheBytes = 0;
			s_retiredAtlases.clear();
			s_loggedAtlasBatches.clear();
			s_loggedVerticalMetricFonts.clear();
			s_loggedQualityDowngrades.clear();
		}
		{
			std::lock_guard<std::mutex> lock(s_batchMutex);
			s_batchCache.clear();
			s_batchLru.clear();
			s_batchCacheBytes = 0;
		}
	}

	bool PrewarmGlyphAtlas(RuntimeFont& runtime,
		const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
		float rasterScale)
	{
		if (bitmaps.empty())
			return true;
		const FontConfig& config = GetRuntimeConfig(runtime);
		const AtlasPixelMode pixelMode = IsA8RendererAvailable()
			? AtlasPixelMode::A8 : AtlasPixelMode::Argb32;
		EffectQuality resolved = config.effectQuality;
		bool shaderEffects = pixelMode == AtlasPixelMode::A8
			&& (config.shadow.enabled || config.glow.enabled || config.outline.enabled)
			&& ResolveA8EffectQuality(config.effectQuality, resolved);
		UInt32 sdfSpread = 0;
		if (shaderEffects && HasSdfEffects(config)
			&& !ResolveSdfSpread(config, rasterScale, sdfSpread))
			shaderEffects = false;
		const UInt32 padding = kAtlasPadding;
		return GetAtlasResource(config, rasterScale, bitmaps, pixelMode,
			shaderEffects ? AtlasRenderMode::ShaderEffects : AtlasRenderMode::CpuEffects,
			padding) != nullptr;
	}

	NiTriShape* TryCreateGlyphAtlasShape(Font& font, RuntimeFont& runtime,
		const std::vector<AtlasGlyphInstance>& glyphs, float rasterScale,
		bool prepareObject, const NiColorA& requestedTileColor)
	{
		if (glyphs.empty())
			return nullptr;
		const FontConfig& config = GetRuntimeConfig(runtime);
		const NiColorA tileColor = ResolveSafeTileColor(glyphs, requestedTileColor);
		const bool hasEffects = config.shadow.enabled || config.glow.enabled
			|| config.outline.enabled;
		EffectQuality resolvedQuality = config.effectQuality;
		if (hasEffects && IsA8RendererAvailable()
			&& ResolveA8EffectQuality(config.effectQuality, resolvedQuality))
		{
			if (resolvedQuality != config.effectQuality && g_bEnableFreeTypeFontRenderingLog)
			{
				const UInt64 downgradeKey = (static_cast<UInt64>(font.iFontNum) << 32)
					| (static_cast<UInt32>(config.effectQuality) << 8)
					| static_cast<UInt32>(resolvedQuality);
				bool shouldLog = false;
				{
					std::lock_guard<std::mutex> lock(s_atlasMutex);
					shouldLog = s_loggedQualityDowngrades.insert(downgradeKey).second;
				}
				if (shouldLog)
				{
					FreeTypeFontDebugLog(
						"tnvse_freetype_font: effect shader quality downgraded font=%u requested=%u resolved=%u",
						font.iFontNum, static_cast<UInt32>(config.effectQuality),
						static_cast<UInt32>(resolvedQuality));
				}
			}
			thread_local std::vector<PendingQuad> shaderQuads;
			ShaderEffectBuild shaderBuild;
			if (BuildShaderEffectQuads(runtime, glyphs, rasterScale,
				resolvedQuality, tileColor, shaderQuads, shaderBuild)
				&& !shaderQuads.empty() && shaderQuads.size() <= kMaximumQuads)
			{
				thread_local std::unordered_map<UInt64,
					std::shared_ptr<const GlyphBitmap>> shaderUnique;
				shaderUnique.clear();
				for (const PendingQuad& quad : shaderQuads)
					shaderUnique.emplace(quad.bitmap->cacheId, quad.bitmap);
				thread_local std::vector<std::shared_ptr<const GlyphBitmap>> shaderBitmaps;
				shaderBitmaps.clear();
				shaderBitmaps.reserve(shaderUnique.size());
				for (auto& [id, bitmap] : shaderUnique)
					shaderBitmaps.push_back(std::move(bitmap));
				std::sort(shaderBitmaps.begin(), shaderBitmaps.end(),
					[](const auto& lhs, const auto& rhs)
					{
						return lhs->cacheId < rhs->cacheId;
					});
				std::shared_ptr<AtlasResource> shaderAtlas;
				NiTriShape* shaderShape = TryCreateAtlasShapeForMode(font,
					shaderQuads, shaderBitmaps, config, rasterScale, prepareObject,
					AtlasPixelMode::A8, AtlasRenderMode::ShaderEffects,
					shaderBuild.padding, shaderAtlas, tileColor, &shaderBuild.config);
				if (shaderShape)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::ShaderEffectBatch);
					const UInt64 avoided = static_cast<UInt64>(glyphs.size())
						* static_cast<UInt64>((config.glow.enabled ? 1 : 0)
							+ (config.outline.enabled ? 1 : 0));
					RecordFreeTypePerf(FreeTypePerfCounter::CpuEffectMasksAvoided, avoided);
					if (g_bEnableFreeTypeFontRenderingLog)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_font: shader effect batch font=%u quality=%u glyphs=%u quads=%u padding=%u texture=%ux%u",
							font.iFontNum, static_cast<UInt32>(resolvedQuality),
							static_cast<UInt32>(glyphs.size()),
							static_cast<UInt32>(shaderQuads.size()), shaderBuild.padding,
							shaderAtlas->width, shaderAtlas->height);
					}
					return shaderShape;
				}
			}
			if (g_bEnableFreeTypeFontRenderingLog)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_font: shader effect atlas failed font=%u; using CPU effect masks",
					font.iFontNum);
			}
		}
		std::array<bool, 4> included = { true, true, true, true };
		const std::array<AtlasLayer, 3> degradationOrder = {
			AtlasLayer::Glow, AtlasLayer::Shadow, AtlasLayer::Outline
		};
		for (size_t attempt = 0; attempt <= degradationOrder.size(); ++attempt)
		{
			thread_local std::vector<PendingQuad> quads;
			quads.clear();
			if (!BuildPendingQuads(runtime, glyphs, rasterScale, included,
				tileColor, quads))
				return nullptr;
			if (quads.empty())
				return nullptr;
			if (quads.size() <= kMaximumQuads)
			{
				thread_local std::unordered_map<UInt64,
					std::shared_ptr<const GlyphBitmap>> unique;
				unique.clear();
				for (const PendingQuad& quad : quads)
					unique.emplace(quad.bitmap->cacheId, quad.bitmap);
				thread_local std::vector<std::shared_ptr<const GlyphBitmap>> bitmaps;
				bitmaps.clear();
				bitmaps.reserve(unique.size());
				for (auto& [id, bitmap] : unique)
					bitmaps.push_back(std::move(bitmap));
				std::sort(bitmaps.begin(), bitmaps.end(), [](const auto& lhs, const auto& rhs)
				{
					return lhs->cacheId < rhs->cacheId;
				});
				AtlasPixelMode pixelMode = IsA8RendererAvailable()
					? AtlasPixelMode::A8 : AtlasPixelMode::Argb32;
				std::shared_ptr<AtlasResource> atlas;
				NiTriShape* shape = TryCreateAtlasShapeForMode(font, quads, bitmaps,
					config, rasterScale, prepareObject, pixelMode,
					AtlasRenderMode::CpuEffects, kAtlasPadding, atlas, tileColor);
				if (!shape && pixelMode == AtlasPixelMode::A8)
				{
					pixelMode = AtlasPixelMode::Argb32;
					shape = TryCreateAtlasShapeForMode(font, quads, bitmaps,
						config, rasterScale, prepareObject, pixelMode,
						AtlasRenderMode::CpuEffects, kAtlasPadding, atlas, tileColor);
				}
				if (shape)
				{
					pixelMode = atlas->pixelMode;
					const UInt64 logKey = (static_cast<UInt64>(font.iFontNum) << 32)
						| (static_cast<UInt32>(std::lround(rasterScale * 1000.0f)) << 1)
						| static_cast<UInt32>(pixelMode);
					bool shouldLog = false;
					if (g_bEnableFreeTypeFontRenderingLog)
					{
						std::lock_guard<std::mutex> lock(s_atlasMutex);
						shouldLog = s_loggedAtlasBatches.insert(logKey).second;
					}
					if (shouldLog)
					{
						FreeTypeFontDebugLog(
							"tnvse_freetype_font: atlas batch font=%u scale=%.3f mode=%s backend=%s glyphs=%u quads=%u texture=%ux%u generation=%u gpuBytes=%llu residentMaskBytes=%llu",
							font.iFontNum, rasterScale,
							pixelMode == AtlasPixelMode::A8 ? "a8" : "argb32",
							atlas->backend == AtlasBackend::DefaultPool
								? "default" : "managed",
							static_cast<UInt32>(glyphs.size()),
							static_cast<UInt32>(quads.size()),
							atlas->width, atlas->height, atlas->generation,
							static_cast<unsigned long long>(atlas->width)
								* atlas->height * AtlasBytesPerPixel(atlas->pixelMode),
							static_cast<unsigned long long>(GetResidentMaskBytes(*atlas)));
					}
					return shape;
				}
			}
			if (attempt < degradationOrder.size())
				included[static_cast<size_t>(degradationOrder[attempt])] = false;
		}

		if (s_atlasFailureLogCount++ < 32)
			gLog.FormattedMessage("tnvse_freetype_font: atlas batch failed font=%u; using vector fallback",
				font.iFontNum);
		return nullptr;
	}
}

namespace fonthook
{
	void InitializeFreeTypeDefaultPoolAtlas()
	{
		vectorfont::InitializeDefaultPoolAtlasLifecycle();
	}

	void HandleFreeTypeDefaultPoolAtlasMainLoop()
	{
		vectorfont::PumpDefaultPoolAtlasLifecycle();
	}

	void ShutdownFreeTypeDefaultPoolAtlas()
	{
		vectorfont::ShutdownDefaultPoolAtlasLifecycle();
	}
}

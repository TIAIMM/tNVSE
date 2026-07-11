#include "font_vector_internal.h"

#include "font_manager.h"
#include "load_config.h"
#include "native_calls.h"

#include "NiDX9Renderer.hpp"
#include "NiFixedString.hpp"
#include "NiGlobalStringTable.hpp"
#include "NiPixelData.hpp"
#include "NiTexturingProperty.hpp"
#include "NiTriShape.hpp"
#include "NiTriShapeData.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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
		constexpr size_t kAtlasCacheLimit = 128u * 1024u * 1024u;
		constexpr UInt32 kMaximumQuads = 16383;
		constexpr float kLayerDepthStep = 0.01f;

		enum class AtlasLayer : UInt8
		{
			Shadow = 0,
			Glow = 1,
			Outline = 2,
			Fill = 3,
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
			UInt32 width = 0;
			UInt32 height = 0;
			std::unordered_map<UInt64, AtlasRect> placements;
		};

		struct AtlasCacheKey
		{
			std::vector<UInt64> ids;

			bool operator==(const AtlasCacheKey& other) const
			{
				return ids == other.ids;
			}
		};

		struct AtlasCacheKeyHash
		{
			size_t operator()(const AtlasCacheKey& key) const
			{
				size_t hash = static_cast<size_t>(2166136261u);
				for (UInt64 id : key.ids)
				{
					hash ^= static_cast<size_t>(id ^ (id >> 32));
					hash *= static_cast<size_t>(16777619u);
				}
				return hash;
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
			AtlasLayer layer = AtlasLayer::Fill;
		};

		std::unordered_map<AtlasCacheKey, AtlasCacheEntry, AtlasCacheKeyHash> s_atlasCache;
		std::list<AtlasCacheKey> s_atlasLru;
		size_t s_atlasCacheBytes = 0;
		std::mutex s_atlasMutex;
		UInt32 s_atlasFailureLogCount = 0;
		std::unordered_set<UInt64> s_loggedAtlasBatches;
		std::unordered_set<UInt32> s_loggedVerticalMetricFonts;

		NiColorA ResolveFillColor(const FontColorStyle& style, const NiColorA& source)
		{
			if (!style.configured)
				return source;
			NiColorA result = style.color;
			result.a *= source.a;
			return result;
		}

		NiColorA ResolveEffectColor(const EffectStyle& effect, const NiColorA& source)
		{
			NiColorA result = effect.color;
			result.a *= source.a;
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

		bool PackAtWidth(const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
			UInt32 atlasWidth, UInt32 maximumSize, UInt32& atlasHeight,
			std::unordered_map<UInt64, AtlasRect>& placements)
		{
			placements.clear();
			UInt32 x = kAtlasPadding;
			UInt32 y = kAtlasPadding;
			UInt32 shelfHeight = 0;
			for (const auto& bitmap : bitmaps)
			{
				const UInt32 width = static_cast<UInt32>(bitmap->width);
				const UInt32 height = static_cast<UInt32>(bitmap->height);
				if (width + kAtlasPadding * 2 > atlasWidth
					|| height + kAtlasPadding * 2 > maximumSize)
					return false;
				if (x + width + kAtlasPadding > atlasWidth)
				{
					x = kAtlasPadding;
					y += shelfHeight;
					shelfHeight = 0;
				}
				if (y + height + kAtlasPadding > maximumSize)
					return false;
				placements[bitmap->cacheId] = { x, y, width, height };
				x += width + kAtlasPadding * 2;
				shelfHeight = std::max(shelfHeight, height + kAtlasPadding * 2);
			}
			atlasHeight = NextPowerOfTwo(y + shelfHeight);
			return atlasHeight <= maximumSize;
		}

		bool PackAtlas(const std::vector<std::shared_ptr<const GlyphBitmap>>& source,
			UInt32& atlasWidth, UInt32& atlasHeight,
			std::unordered_map<UInt64, AtlasRect>& placements)
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
				if (!PackAtWidth(bitmaps, width, maximumSize, height, candidate))
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
			s_atlasLru.erase(entry.lru);
			s_atlasLru.push_front(key);
			entry.lru = s_atlasLru.begin();
		}

		void TrimAtlasCache()
		{
			while (s_atlasCacheBytes > kAtlasCacheLimit && !s_atlasLru.empty())
			{
				const AtlasCacheKey key = s_atlasLru.back();
				auto it = s_atlasCache.find(key);
				if (it != s_atlasCache.end())
				{
					s_atlasCacheBytes -= it->second.bytes;
					s_atlasCache.erase(it);
				}
				s_atlasLru.pop_back();
			}
		}

		std::shared_ptr<AtlasResource> CreateAtlasResource(
			const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps)
		{
			UInt32 width = 0;
			UInt32 height = 0;
			std::unordered_map<UInt64, AtlasRect> placements;
			if (!PackAtlas(bitmaps, width, height, placements) || !width || !height)
				return nullptr;

			NiPixelData* pixelData = static_cast<NiPixelData*>(
				NiMemObject::operator new(sizeof(NiPixelData)));
			if (!pixelData)
				return nullptr;
			pixelData = ThisStdCall<NiPixelData*>(0xA7C190, pixelData, width, height,
				reinterpret_cast<const NiPixelFormat*>(0x11AA2A0), 1, 1);
			if (!pixelData || !pixelData->m_pucPixels || !pixelData->m_puiOffsetInBytes)
				return nullptr;
			UInt32* pixels = reinterpret_cast<UInt32*>(
				pixelData->m_pucPixels + *pixelData->m_puiOffsetInBytes);
			std::fill(pixels, pixels + static_cast<size_t>(width) * height, 0u);
			for (const auto& bitmap : bitmaps)
			{
				const AtlasRect& rect = placements.at(bitmap->cacheId);
				for (UInt32 y = 0; y < rect.height; ++y)
				{
					for (UInt32 x = 0; x < rect.width; ++x)
					{
						const UInt8 alpha = bitmap->alpha[static_cast<size_t>(y) * rect.width + x];
						pixels[static_cast<size_t>(rect.y + y) * width + rect.x + x]
							= (static_cast<UInt32>(alpha) << 24) | 0x00FFFFFFu;
					}
				}
			}
			pixelData->bNoConvert = 1;

			NiTexture::FormatPrefs prefs;
			prefs.m_ePixelLayout = static_cast<NiTexture::FormatPrefs::PixelLayout>(0x6);
			prefs.m_eAlphaFmt = static_cast<NiTexture::FormatPrefs::AlphaFormat>(0x3);
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
			if (NiTexturingProperty::Map* map = property->m_kMaps[0])
			{
				map->m_usflags = static_cast<UInt16>((map->m_usflags & ~0x1Fu)
					| (NiTexturingProperty::FILTER_NEAREST << 2)
					| NiTexturingProperty::CLAMP_S_CLAMP_T);
			}

			auto resource = std::make_shared<AtlasResource>();
			resource->property = property;
			resource->width = width;
			resource->height = height;
			resource->placements = std::move(placements);
			return resource;
		}

		std::shared_ptr<AtlasResource> GetAtlasResource(
			const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps)
		{
			AtlasCacheKey key;
			key.ids.reserve(bitmaps.size());
			for (const auto& bitmap : bitmaps)
				key.ids.push_back(bitmap->cacheId);
			std::sort(key.ids.begin(), key.ids.end());
			key.ids.erase(std::unique(key.ids.begin(), key.ids.end()), key.ids.end());

			std::lock_guard<std::mutex> lock(s_atlasMutex);
			auto existing = s_atlasCache.find(key);
			if (existing != s_atlasCache.end())
			{
				TouchAtlasEntry(existing->second, key);
				return existing->second.resource;
			}
			std::shared_ptr<AtlasResource> resource = CreateAtlasResource(bitmaps);
			if (!resource)
				return nullptr;
			const size_t bytes = static_cast<size_t>(resource->width) * resource->height * 4;
			s_atlasLru.push_front(key);
			s_atlasCache.emplace(key,
				AtlasCacheEntry{ resource, bytes, s_atlasLru.begin() });
			s_atlasCacheBytes += bytes;
			TrimAtlasCache();
			return resource;
		}

		void AddPendingQuad(std::vector<PendingQuad>& quads,
			const std::shared_ptr<const GlyphBitmap>& bitmap,
			const AtlasGlyphInstance& instance, const NiColorA& color,
			float offsetX, float offsetY, float rasterScale, AtlasLayer layer)
		{
			if (bitmap && bitmap->width > 0 && bitmap->height > 0 && !bitmap->alpha.empty())
				quads.push_back({ bitmap, instance.pen, color, offsetX, offsetY,
					rasterScale, instance.glyph.metrics ? instance.glyph.metrics->fTopEdge : 0.0f,
					layer });
		}

		bool BuildPendingQuads(RuntimeFont& runtime,
			const std::vector<AtlasGlyphInstance>& glyphs, float rasterScale,
			const std::array<bool, 4>& included, std::vector<PendingQuad>& quads)
		{
			quads.clear();
			const FontConfig& config = GetRuntimeConfig(runtime);
			for (const AtlasGlyphInstance& instance : glyphs)
			{
				const auto fill = GetGlyphBitmap(runtime, instance.glyph,
					GlyphMaskType::Fill, rasterScale);
				if (!fill)
					return false;
				if (included[static_cast<size_t>(AtlasLayer::Shadow)] && config.shadow.enabled)
				{
					AddPendingQuad(quads, fill, instance,
						ResolveEffectColor(config.shadow, instance.color),
						config.shadow.x, config.shadow.y, rasterScale, AtlasLayer::Shadow);
				}
				if (included[static_cast<size_t>(AtlasLayer::Glow)] && config.glow.enabled)
				{
					const auto glow = GetGlyphBitmap(runtime, instance.glyph,
						GlyphMaskType::Glow, rasterScale);
					if (!glow)
						return false;
					AddPendingQuad(quads, glow, instance,
						ResolveEffectColor(config.glow, instance.color),
						0.0f, 0.0f, rasterScale, AtlasLayer::Glow);
				}
				if (included[static_cast<size_t>(AtlasLayer::Outline)] && config.outline.enabled)
				{
					const auto outline = GetGlyphBitmap(runtime, instance.glyph,
						GlyphMaskType::Outline, rasterScale);
					if (!outline)
						return false;
					AddPendingQuad(quads, outline, instance,
						ResolveEffectColor(config.outline, instance.color),
						0.0f, 0.0f, rasterScale, AtlasLayer::Outline);
				}
				if (included[static_cast<size_t>(AtlasLayer::Fill)])
				{
					AddPendingQuad(quads, fill, instance,
						ResolveFillColor(config.fontColor, instance.color),
						0.0f, 0.0f, rasterScale, AtlasLayer::Fill);
				}
			}
			return true;
		}

		NiTexture* GetAtlasTexture(const AtlasResource& resource)
		{
			if (!resource.property || !resource.property->m_kMaps.GetSize())
				return nullptr;
			NiTexturingProperty::Map* map = resource.property->m_kMaps[0];
			return map ? map->m_spTexture : nullptr;
		}

		NiTriShape* CreateAtlasShape(Font& font, const std::vector<PendingQuad>& quads,
			const std::shared_ptr<AtlasResource>& atlas, bool prepareObject)
		{
			if (!atlas || quads.empty() || quads.size() > kMaximumQuads)
				return nullptr;
			const NiColorA white = { 1.0f, 1.0f, 1.0f, 1.0f };
			NiTriShape* shape = font.MakeTriShape(static_cast<int>(quads.size()), &white, false);
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
					*reinterpret_cast<NiColorA*>(reinterpret_cast<UInt8*>(shade) + 0x68) = white;
				}
			}

			NiTriShapeData* data = shape->GetModelData();
			const UInt32 vertexCount = static_cast<UInt32>(quads.size()) * 4;
			NiColorA* colors = static_cast<NiColorA*>(
				MemoryManager_s_Instance->Allocate(sizeof(NiColorA) * vertexCount));
			if (!colors)
				return nullptr;
			data->m_pkColor = colors;
			data->m_ucKeepFlags |= NiGeometryData::KEEP_COLOR;

			for (UInt32 index = 0; index < quads.size(); ++index)
			{
				const PendingQuad& quad = quads[index];
				const AtlasRect& rect = atlas->placements.at(quad.bitmap->cacheId);
				const float scale = quad.rasterScale;
				const float x0 = std::round((quad.pen.x + quad.offsetX) * scale
					+ static_cast<float>(quad.bitmap->left)) / scale;
				const float z0 = std::round((quad.pen.z + quad.bitmap->baselineOffset
					- quad.offsetY) * scale + static_cast<float>(quad.bitmap->top)) / scale;
				const float x1 = x0 + static_cast<float>(quad.bitmap->width) / scale;
				const float z1 = z0 - static_cast<float>(quad.bitmap->height) / scale;
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
				const float u0 = static_cast<float>(rect.x) / atlas->width;
				const float v0 = static_cast<float>(rect.y) / atlas->height;
				const float u1 = static_cast<float>(rect.x + rect.width) / atlas->width;
				const float v1 = static_cast<float>(rect.y + rect.height) / atlas->height;
				const UInt32 base = index * 4;
				data->m_pkVertex[base + 0] = NiPoint3(x0, depth, z0);
				data->m_pkVertex[base + 1] = NiPoint3(x1, depth, z0);
				data->m_pkVertex[base + 2] = NiPoint3(x1, depth, z1);
				data->m_pkVertex[base + 3] = NiPoint3(x0, depth, z1);
				data->m_pkTexture[base + 0] = NiPoint2(u0, v0);
				data->m_pkTexture[base + 1] = NiPoint2(u1, v0);
				data->m_pkTexture[base + 2] = NiPoint2(u1, v1);
				data->m_pkTexture[base + 3] = NiPoint2(u0, v1);
				for (UInt32 colorIndex = 0; colorIndex < 4; ++colorIndex)
					data->m_pkColor[base + colorIndex] = quad.color;
				const UInt32 triangle = index * 6;
				data->m_pusTriList[triangle + 0] = static_cast<UInt16>(base + 0);
				data->m_pusTriList[triangle + 1] = static_cast<UInt16>(base + 2);
				data->m_pusTriList[triangle + 2] = static_cast<UInt16>(base + 1);
				data->m_pusTriList[triangle + 3] = static_cast<UInt16>(base + 0);
				data->m_pusTriList[triangle + 4] = static_cast<UInt16>(base + 3);
				data->m_pusTriList[triangle + 5] = static_cast<UInt16>(base + 2);
			}
			ThisStdCall(0xA7EE30, &data->m_kBound, data->m_usVertices, data->m_pkVertex);
			if (prepareObject)
				shape->PrepareObject();
			return shape;
		}
	}

	NiTriShape* TryCreateGlyphAtlasShape(Font& font, RuntimeFont& runtime,
		const std::vector<AtlasGlyphInstance>& glyphs, float rasterScale,
		bool prepareObject)
	{
		if (glyphs.empty())
			return nullptr;
		std::array<bool, 4> included = { true, true, true, true };
		const std::array<AtlasLayer, 3> degradationOrder = {
			AtlasLayer::Glow, AtlasLayer::Shadow, AtlasLayer::Outline
		};
		for (size_t attempt = 0; attempt <= degradationOrder.size(); ++attempt)
		{
			std::vector<PendingQuad> quads;
			if (!BuildPendingQuads(runtime, glyphs, rasterScale, included, quads))
				return nullptr;
			if (quads.empty())
				return nullptr;
			if (quads.size() <= kMaximumQuads)
			{
				std::unordered_map<UInt64, std::shared_ptr<const GlyphBitmap>> unique;
				for (const PendingQuad& quad : quads)
					unique.emplace(quad.bitmap->cacheId, quad.bitmap);
				std::vector<std::shared_ptr<const GlyphBitmap>> bitmaps;
				bitmaps.reserve(unique.size());
				for (auto& [id, bitmap] : unique)
					bitmaps.push_back(std::move(bitmap));
				std::sort(bitmaps.begin(), bitmaps.end(), [](const auto& lhs, const auto& rhs)
				{
					return lhs->cacheId < rhs->cacheId;
				});
				if (std::shared_ptr<AtlasResource> atlas = GetAtlasResource(bitmaps))
				{
					if (NiTriShape* shape = CreateAtlasShape(font, quads, atlas, prepareObject))
					{
						const UInt64 logKey = (static_cast<UInt64>(font.iFontNum) << 32)
							| static_cast<UInt32>(std::lround(rasterScale * 1000.0f));
						bool shouldLog = false;
						if (g_bEnableFreeTypeFontRenderingLog)
						{
							std::lock_guard<std::mutex> lock(s_atlasMutex);
							shouldLog = s_loggedAtlasBatches.insert(logKey).second;
						}
						if (shouldLog)
						{
							FreeTypeFontDebugLog(
								"tnvse_freetype_font: atlas batch font=%u scale=%.3f glyphs=%u quads=%u texture=%ux%u",
								font.iFontNum, rasterScale,
								static_cast<UInt32>(glyphs.size()),
								static_cast<UInt32>(quads.size()),
								atlas->width, atlas->height);
						}
						return shape;
					}
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

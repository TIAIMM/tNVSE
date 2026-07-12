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

#include <algorithm>
#include <array>
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

		size_t GetAtlasCacheLimit()
		{
			return static_cast<size_t>(g_uiFreeTypeFontMemoryCacheMB) * 1024u * 1024u / 2u;
		}

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
			UInt32 generation = 0;
			AtlasPixelMode pixelMode = AtlasPixelMode::Argb32;
			std::vector<UInt8> pixels;
			std::unordered_map<UInt64, AtlasRect> placements;
		};

		struct AtlasCacheKey
		{
			UInt64 styleHash = 0;
			UInt32 fontId = 0;
			UInt32 scaleMilli = 1000;
			AtlasPixelMode pixelMode = AtlasPixelMode::Argb32;

			bool operator==(const AtlasCacheKey& other) const
			{
				return styleHash == other.styleHash && fontId == other.fontId
					&& scaleMilli == other.scaleMilli && pixelMode == other.pixelMode;
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
			std::vector<NiColorA> colors;
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
		UInt32 s_atlasFailureLogCount = 0;
		std::unordered_set<UInt64> s_loggedAtlasBatches;
		std::unordered_set<UInt32> s_loggedVerticalMetricFonts;
		std::unordered_map<BatchTemplateKey, BatchTemplateEntry,
			BatchTemplateKeyHash> s_batchCache;
		std::list<BatchTemplateKey> s_batchLru;
		size_t s_batchCacheBytes = 0;
		std::mutex s_batchMutex;

		NiTexture* GetAtlasTexture(const AtlasResource& resource);

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
					s_atlasCacheBytes -= it->second.bytes;
					s_atlasCache.erase(it);
				}
				s_atlasLru.pop_back();
			}
		}

		bool PlaceBitmap(AtlasResource& resource, const GlyphBitmap& bitmap, AtlasRect& rect)
		{
			const UInt32 width = static_cast<UInt32>(bitmap.width);
			const UInt32 height = static_cast<UInt32>(bitmap.height);
			if (width + kAtlasPadding * 2 > resource.width
				|| height + kAtlasPadding * 2 > resource.height)
				return false;
			if (resource.cursorX + width + kAtlasPadding > resource.width)
			{
				resource.cursorX = kAtlasPadding;
				resource.cursorY += resource.shelfHeight;
				resource.shelfHeight = 0;
			}
			if (resource.cursorY + height + kAtlasPadding > resource.height)
				return false;
			rect = { resource.cursorX, resource.cursorY, width, height };
			resource.cursorX += width + kAtlasPadding * 2;
			resource.shelfHeight = std::max(resource.shelfHeight,
				height + kAtlasPadding * 2);
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

		NiTexturingProperty* CreateAtlasProperty(UInt32 width, UInt32 height,
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
						destination[0] = 0xFF;
						destination[1] = 0xFF;
						destination[2] = 0xFF;
						destination[3] = alpha;
					}
				}
			}
		}

		bool RecreateAtlasProperty(AtlasResource& resource)
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
			NiTexturingProperty* property = CreateAtlasProperty(
				resource.width, resource.height, resource.pixelMode, source, pixelData);
			if (!property)
			{
				if (movedTemporaryBacking)
					resource.pixels = std::move(source);
				return false;
			}
			resource.property = property;
			resource.pixelData = pixelData;
			std::vector<UInt8>().swap(resource.pixels);
			++resource.generation;
			RecordFreeTypePerf(FreeTypePerfCounter::AtlasUpload);
			RecordFreeTypePerf(FreeTypePerfCounter::AtlasUploadBytes,
				static_cast<UInt64>(byteCount));
			return true;
		}

		bool GrowAtlas(AtlasResource& resource)
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
			return RecreateAtlasProperty(resource);
		}

		bool UploadAtlasRegion(AtlasResource& resource, const AtlasRect& dirty)
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

		bool AddBitmapsToAtlas(AtlasResource& resource,
			const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps)
		{
			AtlasResource planned;
			planned.width = resource.width;
			planned.height = resource.height;
			planned.cursorX = resource.cursorX;
			planned.cursorY = resource.cursorY;
			planned.shelfHeight = resource.shelfHeight;
			const UInt32 maximum = GetMaximumAtlasSize();
			for (const auto& bitmap : bitmaps)
			{
				if (!bitmap || resource.placements.find(bitmap->cacheId)
					!= resource.placements.end())
				{
					continue;
				}
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
				if (!GrowAtlas(resource))
					return false;
			}

			bool changed = false;
			UInt32 minX = resource.width;
			UInt32 minY = resource.height;
			UInt32 maxX = 0;
			UInt32 maxY = 0;
			for (const auto& bitmap : bitmaps)
			{
				if (!bitmap || resource.placements.find(bitmap->cacheId) != resource.placements.end())
					continue;
				AtlasRect rect;
				if (!PlaceBitmap(resource, *bitmap, rect))
					return false;
				resource.placements.emplace(bitmap->cacheId, rect);
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
				return RecreateAtlasProperty(resource);
			const AtlasRect dirty = { minX, minY, maxX - minX, maxY - minY };
			if (UploadAtlasRegion(resource, dirty))
				return true;
			// Some D3D9 wrappers reject managed-texture partial locks. Rebuilding the
			// current generation preserves correctness and remains a rare fallback.
			return RecreateAtlasProperty(resource);
		}

		std::shared_ptr<AtlasResource> GetAtlasResource(
			const FontConfig& config, float rasterScale,
			const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
			AtlasPixelMode pixelMode)
		{
			AtlasCacheKey key = {
				config.styleHash,
				config.fontId,
				static_cast<UInt32>(std::lround(rasterScale * 1000.0f)),
				pixelMode
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
			resource->width = std::min<UInt32>(512, GetMaximumAtlasSize());
			resource->height = resource->width;
			resource->pixels.assign(static_cast<size_t>(resource->width) * resource->height
				* AtlasBytesPerPixel(resource->pixelMode), 0u);
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
			AtlasPixelMode pixelMode)
		{
			auto resource = std::make_shared<AtlasResource>();
			resource->pixelMode = pixelMode;
			if (!PackAtlas(bitmaps, resource->width, resource->height,
				resource->placements))
			{
				return nullptr;
			}
			resource->pixels.assign(static_cast<size_t>(resource->width)
				* resource->height * AtlasBytesPerPixel(resource->pixelMode), 0u);
			for (const auto& bitmap : bitmaps)
			{
				if (bitmap)
					CopyBitmapToAtlas(*resource, *bitmap,
						resource->placements.at(bitmap->cacheId));
			}
			return RecreateAtlasProperty(*resource) ? resource : nullptr;
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
			result->colors.resize(quads.size() * 4);
			result->indices.resize(quads.size() * 6);
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
				result->vertices[base + 0] = NiPoint3(x0, depth, z0);
				result->vertices[base + 1] = NiPoint3(x1, depth, z0);
				result->vertices[base + 2] = NiPoint3(x1, depth, z1);
				result->vertices[base + 3] = NiPoint3(x0, depth, z1);
				result->texture[base + 0] = NiPoint2(u0, v0);
				result->texture[base + 1] = NiPoint2(u1, v0);
				result->texture[base + 2] = NiPoint2(u1, v1);
				result->texture[base + 3] = NiPoint2(u0, v1);
				for (UInt32 colorIndex = 0; colorIndex < 4; ++colorIndex)
					result->colors[base + colorIndex] = quad.color;
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
				+ result->colors.size() * sizeof(NiColorA)
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
			const std::shared_ptr<const BatchTemplate> batch =
				GetBatchTemplate(font, quads, atlas);
			if (!batch)
				return nullptr;
			std::copy(batch->vertices.begin(), batch->vertices.end(), data->m_pkVertex);
			std::copy(batch->texture.begin(), batch->texture.end(), data->m_pkTexture);
			std::copy(batch->colors.begin(), batch->colors.end(), data->m_pkColor);
			std::copy(batch->indices.begin(), batch->indices.end(), data->m_pusTriList);
			ThisStdCall(0xA7EE30, &data->m_kBound, data->m_usVertices, data->m_pkVertex);
			if (atlas->pixelMode == AtlasPixelMode::A8
				&& !PrepareA8AtlasShape(shape, font.iFontNum,
					static_cast<UInt32>(std::count_if(quads.begin(), quads.end(),
						[](const PendingQuad& quad) { return quad.layer == AtlasLayer::Fill; })),
					static_cast<UInt32>(quads.size())))
				return nullptr;
			if (prepareObject)
				shape->PrepareObject();
			return shape;
		}

		NiTriShape* TryCreateAtlasShapeForMode(Font& font,
			const std::vector<PendingQuad>& quads,
			const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
			const FontConfig& config, float rasterScale, bool prepareObject,
			AtlasPixelMode pixelMode, std::shared_ptr<AtlasResource>& outAtlas)
		{
			outAtlas = GetAtlasResource(config, rasterScale, bitmaps, pixelMode);
			if (!outAtlas)
				outAtlas = CreateTransientAtlas(bitmaps, pixelMode);
			if (!outAtlas)
				return nullptr;
			return CreateAtlasShape(font, quads, outAtlas, prepareObject);
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
		return GetAtlasResource(config, rasterScale, bitmaps, pixelMode) != nullptr;
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
			thread_local std::vector<PendingQuad> quads;
			quads.clear();
			if (!BuildPendingQuads(runtime, glyphs, rasterScale, included, quads))
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
				const FontConfig& config = GetRuntimeConfig(runtime);
				AtlasPixelMode pixelMode = IsA8RendererAvailable()
					? AtlasPixelMode::A8 : AtlasPixelMode::Argb32;
				std::shared_ptr<AtlasResource> atlas;
				NiTriShape* shape = TryCreateAtlasShapeForMode(font, quads, bitmaps,
					config, rasterScale, prepareObject, pixelMode, atlas);
				if (!shape && pixelMode == AtlasPixelMode::A8)
				{
					pixelMode = AtlasPixelMode::Argb32;
					shape = TryCreateAtlasShapeForMode(font, quads, bitmaps,
						config, rasterScale, prepareObject, pixelMode, atlas);
				}
				if (shape)
				{
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
							"tnvse_freetype_font: atlas batch font=%u scale=%.3f mode=%s glyphs=%u quads=%u texture=%ux%u",
							font.iFontNum, rasterScale,
							pixelMode == AtlasPixelMode::A8 ? "a8" : "argb32",
							static_cast<UInt32>(glyphs.size()),
							static_cast<UInt32>(quads.size()),
							atlas->width, atlas->height);
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

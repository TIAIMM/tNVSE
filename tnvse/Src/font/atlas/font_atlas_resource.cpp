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

#include "font_atlas_resource_internal.h"

namespace fonthook::vectorfont
{
	using namespace implementation::font_atlas_resource;

	void* DefaultAtlasTexture::s_vtable[41] = {};

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
			result->m_kFormatPrefs.m_eMipMapped = texture->GetLevelCount() > 1
				? NiTexture::FormatPrefs::YES : NiTexture::FormatPrefs::NO;

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


		IDirect3DTexture9* CreateDynamicAtlasTexture(UInt32 width, UInt32 height,
			AtlasPixelMode mode, UInt32 mipLevels)
		{
			NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer ? renderer->GetD3DDevice() : nullptr;
			if (!device)
				return nullptr;
			IDirect3DTexture9* texture = nullptr;
			const D3DFORMAT format = mode == AtlasPixelMode::A8
				? D3DFMT_A8 : D3DFMT_A8R8G8B8;
			if (!mipLevels || mipLevels > GetAtlasMipLevelCount(width, height))
				return nullptr;
			if (FAILED(device->CreateTexture(width, height, mipLevels, D3DUSAGE_DYNAMIC,
				format, D3DPOOL_DEFAULT, &texture, nullptr)))
			{
				return nullptr;
			}
			D3DSURFACE_DESC description = {};
			if (FAILED(texture->GetLevelDesc(0, &description))
				|| description.Format != format || description.Pool != D3DPOOL_DEFAULT
				|| texture->GetLevelCount() != mipLevels)
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
			const UInt32 mipLevels = texture->GetLevelCount();
			if (mipLevels != resource.mipLevels)
				return false;
			const UInt32 bytesPerPixel = AtlasBytesPerPixel(mode);
			D3DLOCKED_RECT locked = {};
			if (FAILED(texture->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD)))
				return false;
			for (UInt32 y = 0; y < resource.height; ++y)
			{
				std::memset(static_cast<UInt8*>(locked.pBits)
					+ static_cast<size_t>(y) * locked.Pitch, 0,
					static_cast<size_t>(resource.width) * bytesPerPixel);
			}
			if (!WriteCompactSnapshotPixels(static_cast<UInt8*>(locked.pBits),
				locked.Pitch, mode, resource))
			{
				texture->UnlockRect(0);
				return false;
			}
			for (const AtlasGlyphRecord& glyph : resource.glyphs)
			{
				const std::shared_ptr<const GlyphBitmap>& bitmap = glyph.bitmap;
				if (!bitmap || bitmap->alpha.empty())
					continue;
				const AtlasRect& rect = glyph.rect;
				WriteBitmapPixels(static_cast<UInt8*>(locked.pBits), locked.Pitch,
					mode, *bitmap, rect, rect.x, rect.y);
			}
			std::vector<UInt8> current;
			if (mipLevels > 1)
			{
				current.resize(static_cast<size_t>(resource.width)
					* resource.height * bytesPerPixel);
				for (UInt32 y = 0; y < resource.height; ++y)
				{
					std::memcpy(current.data() + static_cast<size_t>(y)
						* resource.width * bytesPerPixel,
						static_cast<const UInt8*>(locked.pBits)
							+ static_cast<size_t>(y) * locked.Pitch,
						static_cast<size_t>(resource.width) * bytesPerPixel);
				}
			}
			if (FAILED(texture->UnlockRect(0)))
				return false;
			if (mipLevels == 1)
				return true;

			UInt32 currentWidth = resource.width;
			UInt32 currentHeight = resource.height;
			for (UInt32 level = 1; level < mipLevels; ++level)
			{
				std::vector<UInt8> next;
				if (!BuildNextMipLevel(current.data(), currentWidth, currentHeight,
					static_cast<size_t>(currentWidth) * bytesPerPixel, mode, next))
				{
					return false;
				}
				currentWidth = std::max<UInt32>(1, currentWidth / 2);
				currentHeight = std::max<UInt32>(1, currentHeight / 2);
				locked = {};
				if (FAILED(texture->LockRect(level, &locked, nullptr, 0)))
					return false;
				for (UInt32 y = 0; y < currentHeight; ++y)
				{
					std::memcpy(static_cast<UInt8*>(locked.pBits)
						+ static_cast<size_t>(y) * locked.Pitch,
						next.data() + static_cast<size_t>(y)
							* currentWidth * bytesPerPixel,
						static_cast<size_t>(currentWidth) * bytesPerPixel);
				}
				if (FAILED(texture->UnlockRect(level)))
					return false;
				current.swap(next);
			}
			return true;
		}

		NiTexturingProperty* CreateDefaultTextureProperty(
			IDirect3DTexture9*& d3dTexture, AtlasPixelMode mode)
		{
			DefaultAtlasTexture* texture = DefaultAtlasTexture::Create(d3dTexture, mode);
			if (!texture)
				return nullptr;
			d3dTexture = nullptr;
			std::vector<UInt8> bootstrapPixels(4, 0u);
			NiPixelDataPtr bootstrapData;
			NiTexturingProperty* property = CreateManagedAtlasProperty(
				1, 1, AtlasPixelMode::Argb32, 1, bootstrapPixels, bootstrapData);
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
				| (NiTexturingProperty::FILTER_TRILERP << 2)
				| NiTexturingProperty::CLAMP_S_CLAMP_T);
			return property;
		}

		bool CreateDefaultPoolAtlas(AtlasResource& resource, AtlasPixelMode requestedMode)
		{
			if (!g_bEnableFreeTypeDefaultPoolAtlas || State().defaultPoolShutdown)
				return false;
			AtlasPixelMode mode = requestedMode;
			for (UInt32 attempt = 0; attempt < 2; ++attempt)
			{
				IDirect3DTexture9* d3dTexture = CreateDynamicAtlasTexture(
					resource.width, resource.height, mode, resource.mipLevels);
				if (d3dTexture && PopulateDefaultTexture(d3dTexture, resource, mode))
				{
					NiTexturingProperty* property = CreateDefaultTextureProperty(
						d3dTexture, mode);
					if (property)
					{
						resource.property = property;
						resource.pixelData = nullptr;
						std::vector<UInt8>().swap(resource.pixels);
						resource.pixelMode = mode;
						resource.backend = AtlasBackend::DefaultPool;
						resource.mipLevels = d3dTexture
							? d3dTexture->GetLevelCount()
							: GetAtlasMipLevelCount(resource.width, resource.height,
								resource.levelZeroOnly);
						resource.resetPending = false;
						++resource.generation;
						RefreshAtlasResourceCpuMemory(resource);
						if (g_bEnableFreeTypeFontRenderingLog)
						{
							FreeTypeFontDebugLog(
								"tnvse_freetype_font: default atlas created size=%ux%u levels=%u format=%s pool=default usage=dynamic gpuBytes=%llu residentMaskBytes=%llu compactSnapshotBytes=%llu fullCpuBacking=0 bytes",
								resource.width, resource.height,
								resource.mipLevels,
								mode == AtlasPixelMode::A8 ? "a8"
									: mode == AtlasPixelMode::Mtsdf32
										? "mtsdf32" : "argb32",
								static_cast<unsigned long long>(GetAtlasStorageBytes(
									resource.width, resource.height, mode, resource.mipLevels)),
								static_cast<unsigned long long>(GetResidentMaskBytes(resource)),
								static_cast<unsigned long long>(GetCompactSnapshotBytes(resource)));
						}
						return true;
					}
				}
				if (d3dTexture)
					d3dTexture->Release();
				if (mode != AtlasPixelMode::A8)
					break;
				mode = AtlasPixelMode::Argb32;
			}
			if (State().defaultPoolFailureLogCount++ < 8)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: DEFAULT atlas creation failed; managed fallback is disabled while bEnableFreeTypeDefaultPoolAtlas=1");
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
			const UInt32 bitmapBytesPerPixel =
				GlyphBitmapBytesPerPixel(
					bitmap.maskType, bitmap.distanceFieldMethod);
			if (!pixels || bitmap.alpha.size() < static_cast<size_t>(rect.width)
				* rect.height * bitmapBytesPerPixel
				|| !IsCompatibleDistanceFieldBitmap(resource.pixelMode, bitmap))
				return;
			for (UInt32 y = 0; y < rect.height; ++y)
			{
				for (UInt32 x = 0; x < rect.width; ++x)
				{
					const size_t sourcePixel =
						static_cast<size_t>(y) * rect.width + x;
					const size_t pixelIndex = static_cast<size_t>(rect.y + y)
						* resource.width + rect.x + x;
					if (resource.pixelMode == AtlasPixelMode::Argb32
						&& bitmap.maskType == GlyphMaskType::Composite)
					{
						std::memcpy(pixels + pixelIndex * 4u,
							bitmap.alpha.data() + sourcePixel * 4u, 4u);
					}
					else if (resource.pixelMode == AtlasPixelMode::Mtsdf32)
					{
						std::memcpy(pixels + pixelIndex * 4u,
							bitmap.alpha.data() + sourcePixel * 4u, 4u);
					}
					else if (resource.pixelMode == AtlasPixelMode::A8)
					{
						pixels[pixelIndex] = bitmap.alpha[sourcePixel];
					}
					else
					{
						UInt8* destination = pixels + pixelIndex * 4;
						destination[0] = static_cast<UInt8>(bitmap.atlasRgb & 0xFF);
						destination[1] = static_cast<UInt8>((bitmap.atlasRgb >> 8) & 0xFF);
						destination[2] = static_cast<UInt8>((bitmap.atlasRgb >> 16) & 0xFF);
						destination[3] = bitmap.alpha[sourcePixel];
					}
				}
			}
		}

		bool RecreateManagedAtlasProperty(AtlasResource& resource)
		{
			// A managed 1x1 bootstrap property is still used transiently while
			// wrapping a native DEFAULT texture, but a live atlas generation must
			// never acquire engine-managed backing when the DEFAULT policy is on.
			if (g_bEnableFreeTypeDefaultPoolAtlas)
				return false;
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
				resource.width, resource.height, resource.pixelMode,
				resource.mipLevels, source, pixelData);
			if (!property)
			{
				if (movedTemporaryBacking)
					resource.pixels = std::move(source);
				return false;
			}
			resource.property = property;
			resource.pixelData = pixelData;
			resource.backend = AtlasBackend::Managed;
			resource.mipLevels = pixelData
				? std::min(pixelData->m_uiMipmapLevels, kMaximumAtlasMipLevels)
				: 1;
			std::vector<UInt8>().swap(resource.pixels);
			++resource.generation;
			RefreshAtlasResourceCpuMemory(resource);
			RecordFreeTypePerf(FreeTypePerfCounter::AtlasUpload);
			RecordFreeTypePerf(FreeTypePerfCounter::AtlasUploadBytes,
				static_cast<UInt64>(GetAtlasStorageBytes(resource.width,
					resource.height, resource.pixelMode, resource.mipLevels)));
			return true;
		}

		bool GrowManagedAtlas(AtlasResource& resource)
		{
			const UInt32 maximum = GetMaximumAtlasSize(resource.byteClass);
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
			resource.mipLevels = GetAtlasMipLevelCount(
				resource.width, resource.height, resource.levelZeroOnly);
			resource.pixels.swap(expanded);
			RecordFreeTypePerf(FreeTypePerfCounter::AtlasGrown);
			return RecreateManagedAtlasProperty(resource);
		}

		bool UploadManagedAtlasRegions(AtlasResource& resource,
			const std::vector<AtlasRect>& dirtyRects)
		{
			if (!resource.pixelData || !resource.pixelData->m_pucPixels
				|| !resource.pixelData->m_puiOffsetInBytes)
				return false;
			if (dirtyRects.empty())
				return true;
			resource.mipLevels = std::min(resource.pixelData->m_uiMipmapLevels,
				kMaximumAtlasMipLevels);
			if (!resource.mipLevels
				|| !GeneratePixelDataMipRegions(*resource.pixelData,
					resource.pixelMode, dirtyRects))
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
			const D3DFORMAT expectedFormat = resource.pixelMode == AtlasPixelMode::A8
				? D3DFMT_A8 : D3DFMT_A8R8G8B8;
			if (d3dTexture->GetLevelCount() < resource.mipLevels)
			{
				d3dTexture->Release();
				return false;
			}
			for (UInt32 level = 0; level < resource.mipLevels; ++level)
			{
				D3DSURFACE_DESC description = {};
				if (FAILED(d3dTexture->GetLevelDesc(level, &description))
					|| description.Format != expectedFormat)
				{
					d3dTexture->Release();
					return false;
				}
			}

			const UInt32 bytesPerPixel = AtlasBytesPerPixel(resource.pixelMode);
			UInt64 uploadedBytes = 0;
			HRESULT result = D3D_OK;
			for (const AtlasRect& dirty : dirtyRects)
			{
				AtlasRect levelRect = dirty;
				for (UInt32 level = 0; level < resource.mipLevels; ++level)
				{
					RECT rect = {
						static_cast<LONG>(levelRect.x), static_cast<LONG>(levelRect.y),
						static_cast<LONG>(levelRect.x + levelRect.width),
						static_cast<LONG>(levelRect.y + levelRect.height)
					};
					D3DLOCKED_RECT locked = {};
					result = d3dTexture->LockRect(level, &locked, &rect, 0);
					if (FAILED(result))
						break;
					const UInt32 sourceWidth = resource.pixelData->m_puiWidth[level];
					const UInt8* sourcePixels = resource.pixelData->m_pucPixels
						+ resource.pixelData->m_puiOffsetInBytes[level];
					for (UInt32 y = 0; y < levelRect.height; ++y)
					{
						const UInt8* source = sourcePixels
							+ (static_cast<size_t>(levelRect.y + y) * sourceWidth
								+ levelRect.x) * bytesPerPixel;
						UInt8* destination = static_cast<UInt8*>(locked.pBits)
							+ static_cast<size_t>(y) * locked.Pitch;
						std::memcpy(destination, source,
							static_cast<size_t>(levelRect.width) * bytesPerPixel);
					}
					result = d3dTexture->UnlockRect(level);
					if (FAILED(result))
						break;
					uploadedBytes += static_cast<UInt64>(levelRect.width)
						* levelRect.height * bytesPerPixel;
					levelRect = { levelRect.x / 2, levelRect.y / 2,
						std::max<UInt32>(1, levelRect.width / 2),
						std::max<UInt32>(1, levelRect.height / 2) };
				}
				if (FAILED(result))
					break;
			}
			d3dTexture->Release();
			if (SUCCEEDED(result))
			{
				RecordFreeTypePerf(FreeTypePerfCounter::AtlasUpload);
				RecordFreeTypePerf(FreeTypePerfCounter::AtlasUploadBytes,
					uploadedBytes);
				RecordFreeTypePerf(FreeTypePerfCounter::AtlasUploadRect,
					static_cast<UInt64>(dirtyRects.size()));
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
			snapshot->mipLevels = resource.mipLevels;
			snapshot->pixelMode = resource.pixelMode;
			snapshot->backend = resource.backend;
			snapshot->renderMode = resource.renderMode;
			snapshot->byteClass = resource.byteClass;
			snapshot->levelZeroOnly = resource.levelZeroOnly;
			snapshot->padding = resource.padding;
			snapshot->resetPending = resource.resetPending;
			snapshot->sharedGpuPage = resource.sharedGpuPage;
			snapshot->pageContentHash = resource.pageContentHash;
			snapshot->glyphs = resource.glyphs;
			snapshot->compactSnapshot = resource.compactSnapshot;
			RefreshAtlasResourceCpuMemory(*snapshot);
			return snapshot;
		}

		void RetireDefaultGeneration(const AtlasResource& resource)
		{
			if (resource.backend != AtlasBackend::DefaultPool || !resource.property)
			{
				return;
			}
			State().retiredAtlases.push_back({ MakeGenerationSnapshot(resource) });
			State().defaultPoolMaintenancePending.store(true, std::memory_order_release);
		}

		void PruneRetiredAtlases()
		{
			State().retiredAtlases.erase(std::remove_if(State().retiredAtlases.begin(),
				State().retiredAtlases.end(), [](const RetiredAtlasGeneration& retired)
			{
				return !retired.resource || !retired.resource->property
					|| retired.resource->property->m_uiRefCount <= 1;
			}), State().retiredAtlases.end());
		}

		bool HasDefaultPoolMaintenanceLocked()
		{
			if (!State().retiredAtlases.empty())
				return true;
			return std::any_of(State().atlasCache.begin(), State().atlasCache.end(),
				[](const auto& entry)
				{
					return entry.second.resource
						&& entry.second.resource->resetPending;
				});
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

	bool AreAtlasResourcesBackedBySameTexture(const AtlasResource& left,
		const AtlasResource& right)
	{
		if (&left == &right)
			return true;
		if (!left.property || !right.property || left.resetPending
			|| right.resetPending || left.width != right.width
			|| left.height != right.height || left.pixelMode != right.pixelMode
			|| left.renderMode != right.renderMode
			|| left.levelZeroOnly != right.levelZeroOnly)
		{
			return false;
		}
		NiTexture* leftTexture = GetAtlasTexture(left);
		NiTexture* rightTexture = GetAtlasTexture(right);
		NiDX9TextureData* leftData =
			leftTexture ? leftTexture->GetDX9RendererData() : nullptr;
		NiDX9TextureData* rightData =
			rightTexture ? rightTexture->GetDX9RendererData() : nullptr;
		LPDIRECT3DBASETEXTURE9 leftD3D =
			leftData ? leftData->GetD3DTexture() : nullptr;
		LPDIRECT3DBASETEXTURE9 rightD3D =
			rightData ? rightData->GetD3DTexture() : nullptr;
		return leftD3D && leftD3D == rightD3D;
	}

	void RefreshAtlasCacheGpuAccountingLocked(AtlasState& state)
	{
		state.atlasCacheBytes = 0;
		std::unordered_set<const void*> defaultPoolTextures;
		std::unordered_set<const void*> managedTextures;
		for (auto& [key, entry] : state.atlasCache)
		{
			AtlasResource* resource = entry.resource.get();
			if (!resource || !resource->property || resource->resetPending)
			{
				entry.bytes = 0;
				continue;
			}

			size_t bytes = GetAtlasStorageBytes(resource->width,
				resource->height, resource->pixelMode,
				resource->mipLevels);
			NiTexture* texture = GetAtlasTexture(*resource);
			if (resource->backend == AtlasBackend::DefaultPool)
			{
				NiDX9TextureData* data =
					texture ? texture->GetDX9RendererData() : nullptr;
				LPDIRECT3DBASETEXTURE9 d3dTexture =
					data ? data->GetD3DTexture() : nullptr;
				if (!d3dTexture
					|| !defaultPoolTextures.insert(d3dTexture).second)
				{
					bytes = 0;
				}
			}
			else if (texture
				&& !managedTextures.insert(texture).second)
			{
				bytes = 0;
			}
			entry.bytes = bytes;
			if (bytes <= std::numeric_limits<size_t>::max()
					- state.atlasCacheBytes)
			{
				state.atlasCacheBytes += bytes;
			}
			else
			{
				state.atlasCacheBytes =
					std::numeric_limits<size_t>::max();
			}
		}
	}

		bool CompactSnapshotsEqual(const AtlasResource& lhs,
			const AtlasResource& rhs)
		{
			if (!lhs.compactSnapshot || !rhs.compactSnapshot
				|| lhs.width != rhs.width || lhs.height != rhs.height
				|| lhs.mipLevels != rhs.mipLevels || lhs.pixelMode != rhs.pixelMode
				|| lhs.renderMode != rhs.renderMode || lhs.padding != rhs.padding
				|| lhs.levelZeroOnly != rhs.levelZeroOnly)
			{
				return false;
			}
			const CompactAtlasSnapshot& left = *lhs.compactSnapshot;
			const CompactAtlasSnapshot& right = *rhs.compactSnapshot;
			if (left.pixelMode != right.pixelMode
				|| left.placements.size() != right.placements.size())
			{
				return false;
			}
			std::vector<UInt8> leftLoadedPixels;
			std::vector<UInt8> rightLoadedPixels;
			if ((left.pixels.empty()
					&& !LoadCompactSnapshotPixels(left, leftLoadedPixels))
				|| (right.pixels.empty()
					&& !LoadCompactSnapshotPixels(right, rightLoadedPixels)))
			{
				return false;
			}
			const std::vector<UInt8>& leftPixels = left.pixels.empty()
				? leftLoadedPixels : left.pixels;
			const std::vector<UInt8>& rightPixels = right.pixels.empty()
				? rightLoadedPixels : right.pixels;
			struct Slice
			{
				AtlasRect rect;
				size_t offset;
				size_t bytes;
			};
			auto buildSlices = [](const CompactAtlasSnapshot& snapshot,
				const std::vector<UInt8>& pixels, std::vector<Slice>& slices)
			{
				size_t offset = 0;
				const size_t bytesPerPixel = AtlasBytesPerPixel(snapshot.pixelMode);
				for (const AtlasSnapshotPlacement& placement : snapshot.placements)
				{
					const size_t bytes = static_cast<size_t>(placement.rect.width)
						* placement.rect.height * bytesPerPixel;
					if (offset > pixels.size()
						|| bytes > pixels.size() - offset)
						return false;
					slices.push_back({ placement.rect, offset, bytes });
					offset += bytes;
				}
				std::sort(slices.begin(), slices.end(), [](const Slice& a, const Slice& b)
				{
					if (a.rect.y != b.rect.y) return a.rect.y < b.rect.y;
					if (a.rect.x != b.rect.x) return a.rect.x < b.rect.x;
					if (a.rect.height != b.rect.height) return a.rect.height < b.rect.height;
					return a.rect.width < b.rect.width;
				});
				return offset == pixels.size();
			};
			std::vector<Slice> leftSlices;
			std::vector<Slice> rightSlices;
			leftSlices.reserve(left.placements.size());
			rightSlices.reserve(right.placements.size());
			if (!buildSlices(left, leftPixels, leftSlices)
				|| !buildSlices(right, rightPixels, rightSlices))
				return false;
			for (size_t index = 0; index < leftSlices.size(); ++index)
			{
				const Slice& a = leftSlices[index];
				const Slice& b = rightSlices[index];
				if (std::memcmp(&a.rect, &b.rect, sizeof(a.rect)) != 0
					|| a.bytes != b.bytes
					|| (a.bytes && std::memcmp(leftPixels.data() + a.offset,
						rightPixels.data() + b.offset, a.bytes) != 0))
					return false;
			}
			return true;
		}

	bool EnsureAtlasGlyphIndex(AtlasResource& resource)
	{
		if (!resource.compactGlyphIndexReleased)
			return true;
		if (!resource.compactSnapshot)
			return false;
		std::vector<AtlasGlyphRecord> restored;
		restored.reserve(
			resource.compactSnapshot->placements.size());
		for (UInt32 index = 0;
			index < resource.compactSnapshot->placements.size();
			++index)
		{
			const AtlasSnapshotPlacement& snapshot =
				resource.compactSnapshot->placements[index];
			const UInt16 pageIndex =
				snapshot.glyphPlacement.pageIndex;
			AtlasGlyphRecord glyph;
			glyph.cacheId = snapshot.cacheId;
			glyph.rect = snapshot.rect;
			glyph.snapshotPlacementIndex = index;
			if (!glyph.cacheId
				|| !RestoreAtlasSnapshotGlyphPlacement(
					snapshot, resource, pageIndex,
					pageIndex, glyph.placement))
			{
				return false;
			}
			restored.push_back(std::move(glyph));
		}
		std::sort(restored.begin(), restored.end(),
			[](const AtlasGlyphRecord& left,
				const AtlasGlyphRecord& right)
			{
				return left.cacheId < right.cacheId;
			});
		resource.glyphs.swap(restored);
		resource.compactGlyphIndexReleased = false;
		RefreshAtlasResourceCpuMemory(resource);
		return true;
	}

	AtlasGlyphRecord* FindAtlasGlyph(AtlasResource& resource, UInt64 cacheId)
	{
		if (!EnsureAtlasGlyphIndex(resource))
			return nullptr;
		auto found = std::lower_bound(resource.glyphs.begin(), resource.glyphs.end(),
			cacheId, [](const AtlasGlyphRecord& glyph, UInt64 id)
			{
				return glyph.cacheId < id;
			});
		return found != resource.glyphs.end() && found->cacheId == cacheId
			? &*found : nullptr;
	}

	const AtlasGlyphRecord* FindAtlasGlyph(const AtlasResource& resource, UInt64 cacheId)
	{
		AtlasResource& mutableResource =
			const_cast<AtlasResource&>(resource);
		if (!EnsureAtlasGlyphIndex(mutableResource))
			return nullptr;
		auto found = std::lower_bound(resource.glyphs.begin(), resource.glyphs.end(),
			cacheId, [](const AtlasGlyphRecord& glyph, UInt64 id)
			{
				return glyph.cacheId < id;
			});
		return found != resource.glyphs.end() && found->cacheId == cacheId
			? &*found : nullptr;
	}

	void SortAtlasGlyphs(AtlasResource& resource)
	{
		std::sort(resource.glyphs.begin(), resource.glyphs.end(),
			[](const AtlasGlyphRecord& lhs, const AtlasGlyphRecord& rhs)
			{
				return lhs.cacheId < rhs.cacheId;
			});
		RefreshAtlasResourceCpuMemory(resource);
	}

	void RefreshAtlasResourceCpuMemory(AtlasResource& resource)
	{
		const size_t managedBacking = resource.backend == AtlasBackend::Managed
			&& resource.pixelData
			? GetAtlasStorageBytes(resource.width, resource.height,
				resource.pixelMode, resource.mipLevels)
			: resource.pixels.capacity();
		resource.cpuMemory.Reset(CpuMemoryCategory::AtlasMetadata,
			sizeof(AtlasResource)
				+ resource.glyphs.capacity() * sizeof(AtlasGlyphRecord)
				+ managedBacking);
	}

	std::shared_ptr<const GlyphBitmap> GetOrCreateAtlasGlyphBitmap(
		AtlasResource& resource, UInt64 cacheId)
	{
		AtlasGlyphRecord* found = FindAtlasGlyph(resource, cacheId);
		if (!found)
			return nullptr;
		AtlasGlyphRecord& glyph = *found;
		if (glyph.bitmap)
			return glyph.bitmap;
		if (!resource.compactSnapshot
			|| glyph.snapshotPlacementIndex
				>= resource.compactSnapshot->placements.size())
		{
			return nullptr;
		}
		const AtlasSnapshotPlacement& placement =
			resource.compactSnapshot->placements[glyph.snapshotPlacementIndex];
		if (placement.cacheId != cacheId
			|| std::memcmp(&placement.rect, &glyph.rect, sizeof(glyph.rect)) != 0)
		{
			return nullptr;
		}
		// Snapshot restore leaves this null for the complete page. Materialize only
		// glyph metadata that live text actually consumes; alpha stays disk/GPU-backed.
		auto bitmap = std::make_shared<GlyphBitmap>();
		bitmap->cacheId = placement.cacheId;
		bitmap->atlasRgb = placement.atlasRgb;
		bitmap->width = static_cast<int>(placement.rect.width);
		bitmap->height = static_cast<int>(placement.rect.height);
		bitmap->left = placement.left;
		bitmap->top = placement.top;
		bitmap->effectiveWidth = placement.effectiveWidth;
		bitmap->effectiveHeight = placement.effectiveHeight;
		bitmap->maskType = static_cast<GlyphMaskType>(placement.maskType);
		bitmap->distanceFieldMethod = resource.pixelMode == AtlasPixelMode::Mtsdf32
			? DistanceFieldMethod::Mtsdf : DistanceFieldMethod::TrueSdf;
		bitmap->sdfSpread = placement.sdfSpread;
		bitmap->strokeWidth26Dot6 = placement.strokeWidth26Dot6;
		bitmap->colorBaked = placement.colorBaked != 0;
		bitmap->bakedRgba = placement.bakedRgba;
		bitmap->bakedLayer = placement.bakedLayer;
		bitmap->cpuMemory.Reset(CpuMemoryCategory::GlyphBitmap,
			sizeof(GlyphBitmap));
		glyph.bitmap = bitmap;
		return bitmap;
	}

	bool TryReuseDefaultPoolAtlasPage(const std::shared_ptr<AtlasResource>& resource,
		UInt64 pageContentHash)
	{
		if (!resource || !pageContentHash || !resource->compactSnapshot
			|| State().defaultPoolShutdown)
			return false;
		AtlasState& state = State();
		std::lock_guard<std::mutex> lock(state.atlasMutex);
		auto range = state.atlasPageDedup.equal_range(pageContentHash);
		for (auto it = range.first; it != range.second;)
		{
			const std::shared_ptr<AtlasResource> existing = it->second.lock();
			if (!existing)
			{
				it = state.atlasPageDedup.erase(it);
				continue;
			}
			++it;
			if (existing.get() == resource.get()
				|| existing->backend != AtlasBackend::DefaultPool
				|| existing->resetPending || !existing->property
				|| existing->pageContentHash != pageContentHash
				|| !CompactSnapshotsEqual(*existing, *resource))
			{
				continue;
			}
			IDirect3DTexture9* d3dTexture = QueryAtlasD3DTexture(*existing);
			if (!d3dTexture)
				continue;
			NiTexturingProperty* property = CreateDefaultTextureProperty(
				d3dTexture, resource->pixelMode);
			if (d3dTexture)
				d3dTexture->Release();
			if (!property)
				continue;
			resource->property = property;
			resource->pixelData = nullptr;
			resource->backend = AtlasBackend::DefaultPool;
			resource->resetPending = false;
			resource->sharedGpuPage = true;
			resource->pageContentHash = pageContentHash;
			++resource->generation;
			RefreshAtlasResourceCpuMemory(*resource);
			existing->sharedGpuPage = true;
			state.atlasPageDedup.emplace(pageContentHash, resource);
			return true;
		}
		return false;
	}

	void PruneRetiredAtlasGenerations()
	{
		PruneRetiredAtlases();
	}

	void RegisterDefaultPoolAtlasPage(const std::shared_ptr<AtlasResource>& resource,
		UInt64 pageContentHash)
	{
		if (!resource || !pageContentHash
			|| resource->backend != AtlasBackend::DefaultPool || !resource->property)
			return;
		resource->pageContentHash = pageContentHash;
		std::lock_guard<std::mutex> lock(State().atlasMutex);
		State().atlasPageDedup.emplace(pageContentHash, resource);
	}

		struct PendingAtlasPlacement
		{
			std::shared_ptr<const GlyphBitmap> bitmap;
			AtlasRect rect;
		};

		bool UploadDefaultAtlasRegions(AtlasResource& resource,
			const std::vector<PendingAtlasPlacement>& pending,
			const std::vector<AtlasRect>& dirtyRects)
		{
			if (dirtyRects.empty())
				return true;
			IDirect3DTexture9* texture = QueryAtlasD3DTexture(resource);
			if (!texture)
				return false;
			resource.mipLevels = texture->GetLevelCount();
			if (resource.mipLevels != GetAtlasMipLevelCount(
				resource.width, resource.height, resource.levelZeroOnly))
			{
				texture->Release();
				return false;
			}
			const UInt32 bytesPerPixel = AtlasBytesPerPixel(resource.pixelMode);
			std::vector<UInt8> current;
			std::vector<UInt8> next;
			UInt64 uploadedBytes = 0;
			for (const AtlasRect& dirty : dirtyRects)
			{
				AtlasRect levelRect = dirty;
				RECT lockRect = {
					static_cast<LONG>(levelRect.x), static_cast<LONG>(levelRect.y),
					static_cast<LONG>(levelRect.x + levelRect.width),
					static_cast<LONG>(levelRect.y + levelRect.height)
				};
				D3DLOCKED_RECT locked = {};
				HRESULT result = texture->LockRect(0, &locked, &lockRect, 0);
				if (FAILED(result))
				{
					texture->Release();
					return false;
				}
				for (const PendingAtlasPlacement& entry : pending)
				{
					if (!entry.bitmap || !AtlasRectContains(dirty, entry.rect))
						continue;
					WriteBitmapPixels(static_cast<UInt8*>(locked.pBits), locked.Pitch,
						resource.pixelMode, *entry.bitmap, entry.rect,
						entry.rect.x - levelRect.x, entry.rect.y - levelRect.y);
				}
				current.resize(static_cast<size_t>(levelRect.width)
					* levelRect.height * bytesPerPixel);
				for (UInt32 y = 0; y < levelRect.height; ++y)
				{
					std::memcpy(current.data() + static_cast<size_t>(y)
						* levelRect.width * bytesPerPixel,
						static_cast<const UInt8*>(locked.pBits)
							+ static_cast<size_t>(y) * locked.Pitch,
						static_cast<size_t>(levelRect.width) * bytesPerPixel);
				}
				result = texture->UnlockRect(0);
				if (FAILED(result))
				{
					texture->Release();
					return false;
				}
				uploadedBytes += static_cast<UInt64>(levelRect.width)
					* levelRect.height * bytesPerPixel;
				for (UInt32 level = 1; level < resource.mipLevels; ++level)
				{
					if (!BuildNextMipLevel(current.data(), levelRect.width,
						levelRect.height,
						static_cast<size_t>(levelRect.width) * bytesPerPixel,
						resource.pixelMode, next))
					{
						texture->Release();
						return false;
					}
					levelRect = { levelRect.x / 2, levelRect.y / 2,
						std::max<UInt32>(1, levelRect.width / 2),
						std::max<UInt32>(1, levelRect.height / 2) };
					lockRect = {
						static_cast<LONG>(levelRect.x), static_cast<LONG>(levelRect.y),
						static_cast<LONG>(levelRect.x + levelRect.width),
						static_cast<LONG>(levelRect.y + levelRect.height)
					};
					locked = {};
					result = texture->LockRect(level, &locked, &lockRect, 0);
					if (FAILED(result))
					{
						texture->Release();
						return false;
					}
					for (UInt32 y = 0; y < levelRect.height; ++y)
					{
						std::memcpy(static_cast<UInt8*>(locked.pBits)
							+ static_cast<size_t>(y) * locked.Pitch,
							next.data() + static_cast<size_t>(y)
								* levelRect.width * bytesPerPixel,
							static_cast<size_t>(levelRect.width) * bytesPerPixel);
					}
					result = texture->UnlockRect(level);
					if (FAILED(result))
					{
						texture->Release();
						return false;
					}
					uploadedBytes += static_cast<UInt64>(levelRect.width)
						* levelRect.height * bytesPerPixel;
					current.swap(next);
				}
			}
			texture->Release();
			RecordFreeTypePerf(FreeTypePerfCounter::AtlasUpload);
			RecordFreeTypePerf(FreeTypePerfCounter::AtlasUploadBytes,
				uploadedBytes);
			RecordFreeTypePerf(FreeTypePerfCounter::AtlasUploadRect,
				static_cast<UInt64>(dirtyRects.size()));
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
			resource.mipLevels = candidate.mipLevels;
			resource.pixelMode = candidate.pixelMode;
			resource.backend = AtlasBackend::DefaultPool;
			resource.renderMode = candidate.renderMode;
			resource.byteClass = candidate.byteClass;
			resource.levelZeroOnly = candidate.levelZeroOnly;
			resource.resetPending = false;
			resource.glyphs = std::move(candidate.glyphs);
			resource.compactSnapshot = std::move(candidate.compactSnapshot);
			resource.sharedGpuPage = candidate.sharedGpuPage;
			resource.pageContentHash = candidate.pageContentHash;
			std::vector<UInt8>().swap(resource.pixels);
			RefreshAtlasResourceCpuMemory(resource);
		}

		bool AddBitmapsToDefaultAtlas(AtlasResource& resource,
			const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps)
		{
			const UInt32 maximum = GetMaximumAtlasSize(resource.byteClass);
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
				layout.byteClass = resource.byteClass;
				pending.clear();
				bool placedAll = true;
				for (const auto& bitmap : bitmaps)
				{
					if (!bitmap || FindAtlasGlyph(resource, bitmap->cacheId))
						continue;
					if (std::any_of(pending.begin(), pending.end(), [&](const auto& entry)
					{
						return entry.bitmap->cacheId == bitmap->cacheId;
					}))
					{
						continue;
					}
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
			if (resource.sharedGpuPage)
			{
				AtlasResource detached;
				detached.width = resource.width;
				detached.height = resource.height;
				detached.cursorX = resource.cursorX;
				detached.cursorY = resource.cursorY;
				detached.shelfHeight = resource.shelfHeight;
				detached.padding = resource.padding;
				detached.generation = resource.generation;
				detached.mipLevels = resource.mipLevels;
				detached.pixelMode = resource.pixelMode;
				detached.backend = AtlasBackend::DefaultPool;
				detached.renderMode = resource.renderMode;
				detached.byteClass = resource.byteClass;
				detached.levelZeroOnly = resource.levelZeroOnly;
				detached.glyphs = resource.glyphs;
				detached.compactSnapshot = resource.compactSnapshot;
				if (!CreateDefaultPoolAtlas(detached, resource.pixelMode))
					return false;
				CommitDefaultCandidate(resource, detached);
				resource.sharedGpuPage = false;
			}

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
			candidate.byteClass = resource.byteClass;
			candidate.levelZeroOnly = resource.levelZeroOnly;
			candidate.generation = resource.generation;
			candidate.mipLevels = GetAtlasMipLevelCount(
				candidate.width, candidate.height, candidate.levelZeroOnly);
			candidate.glyphs = resource.glyphs;
			candidate.compactSnapshot = resource.compactSnapshot;
			candidate.pageContentHash = 0;
			candidate.sharedGpuPage = false;
			const size_t existingGlyphCount = candidate.glyphs.size();
			thread_local std::vector<AtlasRect> rawDirtyRects;
			thread_local std::vector<AtlasRect> dirtyRects;
			rawDirtyRects.clear();
			rawDirtyRects.reserve(pending.size());
			for (const PendingAtlasPlacement& entry : pending)
			{
				candidate.glyphs.push_back({ entry.bitmap->cacheId,
					entry.rect, entry.bitmap, kNoSnapshotPlacement });
				rawDirtyRects.push_back(entry.rect);
			}
			const auto glyphLess = [](const AtlasGlyphRecord& lhs,
				const AtlasGlyphRecord& rhs) { return lhs.cacheId < rhs.cacheId; };
			std::sort(candidate.glyphs.begin() + existingGlyphCount,
				candidate.glyphs.end(), glyphLess);
			std::inplace_merge(candidate.glyphs.begin(),
				candidate.glyphs.begin() + existingGlyphCount,
				candidate.glyphs.end(), glyphLess);
			const bool needsRebuild = !resource.property
				|| resource.width != candidate.width || resource.height != candidate.height;
			if (needsRebuild)
			{
				if (!CreateDefaultPoolAtlas(candidate, resource.pixelMode))
					return false;
				if (resource.property)
					RecordFreeTypePerf(FreeTypePerfCounter::AtlasGrown);
				CommitDefaultCandidate(resource, candidate);
				resource.pageContentHash = 0;
				return true;
			}
			BuildMergedAtlasDirtyRects(rawDirtyRects, resource.width, resource.height,
				resource.mipLevels, resource.pixelMode, dirtyRects);
			if (UploadDefaultAtlasRegions(resource, pending, dirtyRects))
			{
				resource.cursorX = candidate.cursorX;
				resource.cursorY = candidate.cursorY;
				resource.shelfHeight = candidate.shelfHeight;
				resource.glyphs = std::move(candidate.glyphs);
				resource.pageContentHash = 0;
				RefreshAtlasResourceCpuMemory(resource);
				return true;
			}
			if (!CreateDefaultPoolAtlas(candidate, resource.pixelMode))
				return false;
			CommitDefaultCandidate(resource, candidate);
			resource.pageContentHash = 0;
			return true;
		}

		bool AddBitmapsToManagedAtlas(AtlasResource& resource,
			const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps)
		{
			if (g_bEnableFreeTypeDefaultPoolAtlas)
				return false;
			AtlasResource planned;
			planned.width = resource.width;
			planned.height = resource.height;
			planned.cursorX = resource.cursorX;
			planned.cursorY = resource.cursorY;
			planned.shelfHeight = resource.shelfHeight;
			planned.padding = resource.padding;
			planned.byteClass = resource.byteClass;
			const UInt32 maximum = GetMaximumAtlasSize(resource.byteClass);
			for (const auto& bitmap : bitmaps)
			{
				if (!bitmap || FindAtlasGlyph(resource, bitmap->cacheId))
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
			thread_local std::vector<AtlasRect> rawDirtyRects;
			thread_local std::vector<AtlasRect> dirtyRects;
			thread_local std::vector<PendingAtlasPlacement> pending;
			rawDirtyRects.clear();
			rawDirtyRects.reserve(bitmaps.size());
			pending.clear();
			pending.reserve(bitmaps.size());
			for (const auto& bitmap : bitmaps)
			{
				if (!bitmap || FindAtlasGlyph(resource, bitmap->cacheId))
					continue;
				if (std::any_of(pending.begin(), pending.end(), [&](const auto& entry)
				{
					return entry.bitmap->cacheId == bitmap->cacheId;
				}))
				{
					continue;
				}
				AtlasRect rect;
				if (!PlaceBitmap(resource, *bitmap, rect))
					return false;
				pending.push_back({ bitmap, rect });
				CopyBitmapToAtlas(resource, *bitmap, rect);
				rawDirtyRects.push_back(rect);
			}
			const size_t existingGlyphCount = resource.glyphs.size();
			for (const PendingAtlasPlacement& entry : pending)
			{
				resource.glyphs.push_back({ entry.bitmap->cacheId,
					entry.rect, entry.bitmap, kNoSnapshotPlacement });
			}
			const auto glyphLess = [](const AtlasGlyphRecord& lhs,
				const AtlasGlyphRecord& rhs) { return lhs.cacheId < rhs.cacheId; };
			std::sort(resource.glyphs.begin() + existingGlyphCount,
				resource.glyphs.end(), glyphLess);
			std::inplace_merge(resource.glyphs.begin(),
				resource.glyphs.begin() + existingGlyphCount,
				resource.glyphs.end(), glyphLess);
			RefreshAtlasResourceCpuMemory(resource);
			if (rawDirtyRects.empty())
				return true;
			if (!resource.property)
				return RecreateManagedAtlasProperty(resource);
			BuildMergedAtlasDirtyRects(rawDirtyRects, resource.width, resource.height,
				resource.mipLevels, resource.pixelMode, dirtyRects);
			if (UploadManagedAtlasRegions(resource, dirtyRects))
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
				if (g_bEnableFreeTypeDefaultPoolAtlas)
					return false;
				resource.backend = AtlasBackend::Managed;
				if (resource.pixelMode == AtlasPixelMode::A8)
					resource.pixelMode = AtlasPixelMode::Argb32;
				resource.property = nullptr;
				resource.pixelData = nullptr;
				resource.mipLevels = GetAtlasMipLevelCount(
					resource.width, resource.height, resource.levelZeroOnly);
				resource.pixels.assign(static_cast<size_t>(resource.width)
					* resource.height * AtlasBytesPerPixel(resource.pixelMode), 0u);
				resource.glyphs.clear();
				resource.cursorX = resource.padding;
				resource.cursorY = resource.padding;
				resource.shelfHeight = 0;
			}
			return AddBitmapsToManagedAtlas(resource, bitmaps);
		}


		NiTexture* GetAtlasTexture(const AtlasResource& resource)
		{
			if (!resource.property || !resource.property->m_kMaps.GetSize())
				return nullptr;
			NiTexturingProperty::Map* map = resource.property->m_kMaps[0];
			return map ? map->m_spTexture : nullptr;
		}

}

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


		UInt32 AtlasBytesPerPixel(AtlasPixelMode mode)
		{
			return mode == AtlasPixelMode::A8 ? 1u : 4u;
		}

		UInt32 GetAtlasMipLevelCount(UInt32 width, UInt32 height,
			bool levelZeroOnly)
		{
			if (levelZeroOnly)
				return 1;
			UInt32 levels = 1;
			while (levels < kMaximumAtlasMipLevels && (width > 1 || height > 1))
			{
				width = std::max<UInt32>(1, width / 2);
				height = std::max<UInt32>(1, height / 2);
				++levels;
			}
			return levels;
		}

		size_t GetAtlasStorageBytes(UInt32 width, UInt32 height,
			AtlasPixelMode mode, UInt32 mipLevels)
		{
			size_t bytes = 0;
			const UInt32 bytesPerPixel = AtlasBytesPerPixel(mode);
			for (UInt32 level = 0; level < std::max<UInt32>(1, mipLevels); ++level)
			{
				bytes += static_cast<size_t>(width) * height * bytesPerPixel;
				width = std::max<UInt32>(1, width / 2);
				height = std::max<UInt32>(1, height / 2);
			}
			return bytes;
		}

		AtlasRect AlignDirtyRectForMipChain(const AtlasRect& dirty,
			UInt32 width, UInt32 height, UInt32 mipLevels)
		{
			const UInt32 alignment = 1u << (std::max<UInt32>(1, mipLevels) - 1);
			const UInt32 x0 = dirty.x & ~(alignment - 1);
			const UInt32 y0 = dirty.y & ~(alignment - 1);
			const UInt32 x1 = std::min(width,
				(dirty.x + dirty.width + alignment - 1) & ~(alignment - 1));
			const UInt32 y1 = std::min(height,
				(dirty.y + dirty.height + alignment - 1) & ~(alignment - 1));
			return { x0, y0, x1 - x0, y1 - y0 };
		}

		static AtlasRect UnionAtlasRects(const AtlasRect& lhs, const AtlasRect& rhs)
		{
			const UInt32 x0 = std::min(lhs.x, rhs.x);
			const UInt32 y0 = std::min(lhs.y, rhs.y);
			const UInt32 x1 = std::max(lhs.x + lhs.width, rhs.x + rhs.width);
			const UInt32 y1 = std::max(lhs.y + lhs.height, rhs.y + rhs.height);
			return { x0, y0, x1 - x0, y1 - y0 };
		}

		static bool AtlasRectsTouchOrOverlap(const AtlasRect& lhs, const AtlasRect& rhs)
		{
			return lhs.x <= rhs.x + rhs.width && rhs.x <= lhs.x + lhs.width
				&& lhs.y <= rhs.y + rhs.height && rhs.y <= lhs.y + lhs.height;
		}

		static bool AtlasRectContains(const AtlasRect& outer, const AtlasRect& inner)
		{
			return inner.x >= outer.x && inner.y >= outer.y
				&& inner.x + inner.width <= outer.x + outer.width
				&& inner.y + inner.height <= outer.y + outer.height;
		}

		static UInt64 EstimateDirtyRectUploadBytes(AtlasRect rect, UInt32 mipLevels,
			AtlasPixelMode mode)
		{
			UInt64 result = 0;
			const UInt32 bytesPerPixel = AtlasBytesPerPixel(mode);
			for (UInt32 level = 0; level < std::max<UInt32>(1, mipLevels); ++level)
			{
				result += static_cast<UInt64>(rect.width) * rect.height * bytesPerPixel;
				rect.x /= 2;
				rect.y /= 2;
				rect.width = std::max<UInt32>(1, rect.width / 2);
				rect.height = std::max<UInt32>(1, rect.height / 2);
			}
			return result;
		}

		static void BuildMergedAtlasDirtyRects(const std::vector<AtlasRect>& source,
			UInt32 width, UInt32 height, UInt32 mipLevels, AtlasPixelMode mode,
			std::vector<AtlasRect>& result)
		{
			constexpr size_t kMaximumDirtyRectUploads = 16;
			constexpr UInt64 kEstimatedLockPairCostBytes = 4096;
			const UInt64 mergeAllowance = kEstimatedLockPairCostBytes
				* std::max<UInt32>(1, mipLevels);
			result.clear();
			result.reserve(source.size());
			for (const AtlasRect& raw : source)
			{
				AtlasRect current = AlignDirtyRectForMipChain(raw,
					width, height, mipLevels);
				if (!current.width || !current.height)
					continue;
				for (;;)
				{
					size_t best = result.size();
					UInt64 bestExtra = std::numeric_limits<UInt64>::max();
					for (size_t index = 0; index < result.size(); ++index)
					{
						const AtlasRect combined = UnionAtlasRects(current, result[index]);
						const UInt64 separateCost = EstimateDirtyRectUploadBytes(
							current, mipLevels, mode) + EstimateDirtyRectUploadBytes(
								result[index], mipLevels, mode);
						const UInt64 combinedCost = EstimateDirtyRectUploadBytes(
							combined, mipLevels, mode);
						const UInt64 extra = combinedCost > separateCost
							? combinedCost - separateCost : 0;
						if ((AtlasRectsTouchOrOverlap(current, result[index])
							|| extra <= mergeAllowance) && extra < bestExtra)
						{
							best = index;
							bestExtra = extra;
						}
					}
					if (best == result.size())
						break;
					current = UnionAtlasRects(current, result[best]);
					result.erase(result.begin() + best);
				}
				result.push_back(current);
			}

			// Bound D3D9 LockRect traffic for unusually fragmented batches. Merge the
			// least expensive pair until the batch fits the fixed submission budget.
			while (result.size() > kMaximumDirtyRectUploads)
			{
				size_t bestLeft = 0;
				size_t bestRight = 1;
				UInt64 bestExtra = std::numeric_limits<UInt64>::max();
				for (size_t left = 0; left + 1 < result.size(); ++left)
				{
					for (size_t right = left + 1; right < result.size(); ++right)
					{
						const AtlasRect combined = UnionAtlasRects(
							result[left], result[right]);
						const UInt64 separateCost = EstimateDirtyRectUploadBytes(
							result[left], mipLevels, mode) + EstimateDirtyRectUploadBytes(
								result[right], mipLevels, mode);
						const UInt64 combinedCost = EstimateDirtyRectUploadBytes(
							combined, mipLevels, mode);
						const UInt64 extra = combinedCost > separateCost
							? combinedCost - separateCost : 0;
						if (extra < bestExtra)
						{
							bestLeft = left;
							bestRight = right;
							bestExtra = extra;
						}
					}
				}
				result[bestLeft] = UnionAtlasRects(result[bestLeft], result[bestRight]);
				result.erase(result.begin() + bestRight);
			}
			// A forced merge can span a third rectangle. Coalesce any resulting
			// overlap so no glyph or mip region is submitted twice.
			bool coalesced = true;
			while (coalesced)
			{
				coalesced = false;
				for (size_t left = 0; left + 1 < result.size() && !coalesced; ++left)
				{
					for (size_t right = left + 1; right < result.size(); ++right)
					{
						if (!AtlasRectsTouchOrOverlap(result[left], result[right]))
							continue;
						result[left] = UnionAtlasRects(result[left], result[right]);
						result.erase(result.begin() + right);
						coalesced = true;
						break;
					}
				}
			}
			std::sort(result.begin(), result.end(), [](const AtlasRect& lhs,
				const AtlasRect& rhs)
			{
				return lhs.y != rhs.y ? lhs.y < rhs.y : lhs.x < rhs.x;
			});
		}

		bool BuildNextMipLevel(const UInt8* source, UInt32 sourceWidth,
			UInt32 sourceHeight, size_t sourcePitch, AtlasPixelMode mode,
			std::vector<UInt8>& destination)
		{
			if (!source || !sourceWidth || !sourceHeight)
				return false;
			const UInt32 targetWidth = std::max<UInt32>(1, sourceWidth / 2);
			const UInt32 targetHeight = std::max<UInt32>(1, sourceHeight / 2);
			const UInt32 bytesPerPixel = AtlasBytesPerPixel(mode);
			destination.resize(static_cast<size_t>(targetWidth) * targetHeight
				* bytesPerPixel);
			for (UInt32 y = 0; y < targetHeight; ++y)
			{
				const UInt32 sourceY0 = std::min(sourceHeight - 1, y * 2);
				const UInt32 sourceY1 = std::min(sourceHeight - 1, sourceY0 + 1);
				for (UInt32 x = 0; x < targetWidth; ++x)
				{
					const UInt32 sourceX0 = std::min(sourceWidth - 1, x * 2);
					const UInt32 sourceX1 = std::min(sourceWidth - 1, sourceX0 + 1);
					const UInt8* samples[4] = {
						source + static_cast<size_t>(sourceY0) * sourcePitch
							+ static_cast<size_t>(sourceX0) * bytesPerPixel,
						source + static_cast<size_t>(sourceY0) * sourcePitch
							+ static_cast<size_t>(sourceX1) * bytesPerPixel,
						source + static_cast<size_t>(sourceY1) * sourcePitch
							+ static_cast<size_t>(sourceX0) * bytesPerPixel,
						source + static_cast<size_t>(sourceY1) * sourcePitch
							+ static_cast<size_t>(sourceX1) * bytesPerPixel
					};
					UInt8* target = destination.data()
						+ (static_cast<size_t>(y) * targetWidth + x) * bytesPerPixel;
					if (mode == AtlasPixelMode::A8)
					{
						const UInt32 sum = static_cast<UInt32>(*samples[0]) + *samples[1]
							+ *samples[2] + *samples[3];
						*target = static_cast<UInt8>((sum + 2) / 4);
						continue;
					}

					const UInt32 alphaSum = static_cast<UInt32>(samples[0][3])
						+ samples[1][3] + samples[2][3] + samples[3][3];
					target[3] = static_cast<UInt8>((alphaSum + 2) / 4);
					for (UInt32 channel = 0; channel < 3; ++channel)
					{
						if (!alphaSum)
						{
							target[channel] = 0;
							continue;
						}
						UInt32 weighted = 0;
						for (const UInt8* sample : samples)
							weighted += static_cast<UInt32>(sample[channel]) * sample[3];
						target[channel] = static_cast<UInt8>(
							(weighted + alphaSum / 2) / alphaSum);
					}
				}
			}
			return true;
		}

		bool GeneratePixelDataMipChain(NiPixelData& pixelData, AtlasPixelMode mode)
		{
			if (!pixelData.m_pucPixels || !pixelData.m_puiWidth
				|| !pixelData.m_puiHeight || !pixelData.m_puiOffsetInBytes
				|| !pixelData.m_uiMipmapLevels)
			{
				return false;
			}
			const UInt32 levels = std::min(pixelData.m_uiMipmapLevels,
				kMaximumAtlasMipLevels);
			for (UInt32 level = 1; level < levels; ++level)
			{
				const UInt32 sourceWidth = pixelData.m_puiWidth[level - 1];
				const UInt32 sourceHeight = pixelData.m_puiHeight[level - 1];
				const UInt8* source = pixelData.m_pucPixels
					+ pixelData.m_puiOffsetInBytes[level - 1];
				std::vector<UInt8> mip;
				if (!BuildNextMipLevel(source, sourceWidth, sourceHeight,
					static_cast<size_t>(sourceWidth) * AtlasBytesPerPixel(mode), mode, mip))
				{
					return false;
				}
				const UInt32 targetWidth = pixelData.m_puiWidth[level];
				const UInt32 targetHeight = pixelData.m_puiHeight[level];
				if (targetWidth != std::max<UInt32>(1, sourceWidth / 2)
					|| targetHeight != std::max<UInt32>(1, sourceHeight / 2)
					|| mip.size() != static_cast<size_t>(targetWidth) * targetHeight
						* AtlasBytesPerPixel(mode))
				{
					return false;
				}
				std::memcpy(pixelData.m_pucPixels + pixelData.m_puiOffsetInBytes[level],
					mip.data(), mip.size());
			}
			return true;
		}

		static bool GeneratePixelDataMipRegions(NiPixelData& pixelData,
			AtlasPixelMode mode, const std::vector<AtlasRect>& dirtyRects)
		{
			if (!pixelData.m_pucPixels || !pixelData.m_puiWidth
				|| !pixelData.m_puiHeight || !pixelData.m_puiOffsetInBytes
				|| !pixelData.m_uiMipmapLevels)
			{
				return false;
			}
			const UInt32 levels = std::min(pixelData.m_uiMipmapLevels,
				kMaximumAtlasMipLevels);
			if (levels <= 1)
				return true;
			const UInt32 bytesPerPixel = AtlasBytesPerPixel(mode);
			std::vector<UInt8> mip;
			for (const AtlasRect& dirty : dirtyRects)
			{
				AtlasRect levelRect = dirty;
				for (UInt32 level = 1; level < levels; ++level)
				{
					const UInt32 sourceWidth = pixelData.m_puiWidth[level - 1];
					const UInt32 sourceHeight = pixelData.m_puiHeight[level - 1];
					if (levelRect.x > sourceWidth
						|| levelRect.width > sourceWidth - levelRect.x
						|| levelRect.y > sourceHeight
						|| levelRect.height > sourceHeight - levelRect.y)
					{
						return false;
					}
					const UInt8* source = pixelData.m_pucPixels
						+ pixelData.m_puiOffsetInBytes[level - 1]
						+ (static_cast<size_t>(levelRect.y) * sourceWidth
							+ levelRect.x) * bytesPerPixel;
					if (!BuildNextMipLevel(source, levelRect.width, levelRect.height,
						static_cast<size_t>(sourceWidth) * bytesPerPixel, mode, mip))
					{
						return false;
					}
					AtlasRect targetRect = { levelRect.x / 2, levelRect.y / 2,
						std::max<UInt32>(1, levelRect.width / 2),
						std::max<UInt32>(1, levelRect.height / 2) };
					const UInt32 targetWidth = pixelData.m_puiWidth[level];
					const UInt32 targetHeight = pixelData.m_puiHeight[level];
					if (targetRect.x > targetWidth
						|| targetRect.width > targetWidth - targetRect.x
						|| targetRect.y > targetHeight
						|| targetRect.height > targetHeight - targetRect.y
						|| mip.size() != static_cast<size_t>(targetRect.width)
							* targetRect.height * bytesPerPixel)
					{
						return false;
					}
					UInt8* destination = pixelData.m_pucPixels
						+ pixelData.m_puiOffsetInBytes[level];
					for (UInt32 y = 0; y < targetRect.height; ++y)
					{
						std::memcpy(destination
							+ (static_cast<size_t>(targetRect.y + y) * targetWidth
								+ targetRect.x) * bytesPerPixel,
							mip.data() + static_cast<size_t>(y)
								* targetRect.width * bytesPerPixel,
							static_cast<size_t>(targetRect.width) * bytesPerPixel);
					}
					levelRect = targetRect;
				}
			}
			return true;
		}

		size_t GetResidentMaskBytes(const AtlasResource& resource)
		{
			size_t result = 0;
			for (const AtlasGlyphRecord& glyph : resource.glyphs)
			{
				if (glyph.bitmap)
					result += glyph.bitmap->alpha.size();
			}
			return result;
		}

		size_t GetCompactSnapshotBytes(const AtlasResource& resource)
		{
			if (!resource.compactSnapshot)
				return 0;
			return resource.compactSnapshot->pixels.size()
				+ resource.compactSnapshot->placements.size()
					* sizeof(AtlasSnapshotPlacement);
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
			AtlasPixelMode mode, UInt32 mipLevels, const std::vector<UInt8>& source,
			NiPixelDataPtr& outPixelData)
		{
			const NiPixelFormat* pixelFormat = mode == AtlasPixelMode::A8
				? FindA8PixelFormat()
				: reinterpret_cast<const NiPixelFormat*>(0x11AA2A0);
			const UInt32 maximumMipLevels = GetAtlasMipLevelCount(width, height);
			if (!pixelFormat || !mipLevels || mipLevels > maximumMipLevels)
				return nullptr;
			NiPixelData* pixelData = static_cast<NiPixelData*>(
				NiMemObject::operator new(sizeof(NiPixelData)));
			if (!pixelData)
				return nullptr;
			pixelData = ThisStdCall<NiPixelData*>(0xA7C190, pixelData, width, height,
				pixelFormat, mipLevels, 1);
			if (!pixelData || !pixelData->m_pucPixels || !pixelData->m_puiOffsetInBytes)
				return nullptr;
			UInt8* pixels = pixelData->m_pucPixels + *pixelData->m_puiOffsetInBytes;
			const size_t byteCount = static_cast<size_t>(width) * height
				* AtlasBytesPerPixel(mode);
			const size_t mipByteCount = GetAtlasStorageBytes(width, height, mode,
				mipLevels);
			const bool completeMipSnapshot = source.size() == mipByteCount;
			if (completeMipSnapshot)
			{
				size_t sourceOffset = 0;
				for (UInt32 level = 0; level < mipLevels; ++level)
				{
					const size_t levelBytes = static_cast<size_t>(pixelData->m_puiWidth[level])
						* pixelData->m_puiHeight[level] * AtlasBytesPerPixel(mode);
					std::memcpy(pixelData->m_pucPixels
						+ pixelData->m_puiOffsetInBytes[level],
						source.data() + sourceOffset, levelBytes);
					sourceOffset += levelBytes;
				}
			}
			else if (source.size() == byteCount)
				std::copy(source.begin(), source.end(), pixels);
			else
				std::fill(pixels, pixels + byteCount, static_cast<UInt8>(0));
			if (pixelData->m_uiMipmapLevels < mipLevels
				|| (!completeMipSnapshot
					&& !GeneratePixelDataMipChain(*pixelData, mode)))
			{
				return nullptr;
			}
			pixelData->bNoConvert = 1;
			outPixelData = pixelData;

			NiTexture::FormatPrefs prefs;
			prefs.m_ePixelLayout = mode == AtlasPixelMode::A8
				? NiTexture::FormatPrefs::SINGLE_COLOR_8
				: NiTexture::FormatPrefs::PIX_DEFAULT;
			prefs.m_eAlphaFmt = mode == AtlasPixelMode::A8
				? NiTexture::FormatPrefs::SMOOTH
				: NiTexture::FormatPrefs::ALPHA_DEFAULT;
			prefs.m_eMipMapped = mipLevels > 1
				? NiTexture::FormatPrefs::YES : NiTexture::FormatPrefs::NO;
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
					| (NiTexturingProperty::FILTER_TRILERP << 2)
					| NiTexturingProperty::CLAMP_S_CLAMP_T);
			}

			return property;
		}

		void WriteBitmapPixels(UInt8* destination, LONG pitch, AtlasPixelMode mode,
			const GlyphBitmap& bitmap, const AtlasRect& rect,
			UInt32 destinationX, UInt32 destinationY)
		{
			const UInt32 bitmapBytesPerPixel =
				GlyphBitmapBytesPerPixel(
					bitmap.maskType, bitmap.distanceFieldMethod);
			const size_t requiredBitmapBytes = static_cast<size_t>(rect.width)
				* rect.height * bitmapBytesPerPixel;
			if (!destination || bitmap.alpha.size() < requiredBitmapBytes
				|| !IsCompatibleDistanceFieldBitmap(mode, bitmap))
				return;
			const UInt32 bytesPerPixel = AtlasBytesPerPixel(mode);
			for (UInt32 y = 0; y < rect.height; ++y)
			{
				UInt8* row = destination + static_cast<size_t>(destinationY + y) * pitch
					+ static_cast<size_t>(destinationX) * bytesPerPixel;
				const UInt8* source = bitmap.alpha.data()
					+ static_cast<size_t>(y) * rect.width * bitmapBytesPerPixel;
				if (mode == AtlasPixelMode::Argb32
					&& bitmap.maskType == GlyphMaskType::Composite)
				{
					std::memcpy(row, source,
						static_cast<size_t>(rect.width) * 4u);
				}
				else if (mode == AtlasPixelMode::Mtsdf32)
				{
					std::memcpy(row, source, static_cast<size_t>(rect.width) * 4u);
				}
				else if (mode == AtlasPixelMode::A8)
				{
					std::memcpy(row, source, rect.width);
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
						row[x * 4 + 3] = source[x];
					}
				}
			}
		}

		namespace
		{
			constexpr UInt64 kAtlasByteHashOffset = 1469598103934665603ull;

			UInt64 HashCompactAtlasBytes(const void* data, size_t size,
				UInt64 hash = kAtlasByteHashOffset)
			{
				const UInt8* bytes = static_cast<const UInt8*>(data);
				for (size_t index = 0; index < size; ++index)
				{
					hash ^= bytes[index];
					hash *= 1099511628211ull;
				}
				return hash;
			}

			bool ReadSnapshotBytesExact(HANDLE file, void* destination, size_t size)
			{
				UInt8* output = static_cast<UInt8*>(destination);
				while (size)
				{
					const DWORD requested = static_cast<DWORD>(std::min<size_t>(
						size, std::numeric_limits<DWORD>::max()));
					DWORD read = 0;
					if (!ReadFile(file, output, requested, &read, nullptr)
						|| read != requested)
					{
						return false;
					}
					output += read;
					size -= read;
				}
				return true;
			}

			struct CompactSnapshotFile
			{
				HANDLE handle = INVALID_HANDLE_VALUE;
				~CompactSnapshotFile()
				{
					if (handle != INVALID_HANDLE_VALUE)
						CloseHandle(handle);
				}
			};

			bool OpenCompactSnapshotPixels(const CompactAtlasSnapshot& snapshot,
				CompactSnapshotFile& file)
			{
				if (snapshot.sourcePath.empty()
					|| snapshot.sourceHeader.placementCount != snapshot.placements.size()
					|| snapshot.sourceHeader.pixelMode != static_cast<UInt8>(snapshot.pixelMode)
					|| snapshot.sourceHeader.headerSize != sizeof(AtlasSnapshotHeader)
					|| snapshot.sourceHeader.pixelBytes > std::numeric_limits<size_t>::max()
					|| snapshot.sourceHeader.storedPixelBytes
						> std::numeric_limits<size_t>::max())
				{
					return false;
				}
				const UInt64 placementBytes = static_cast<UInt64>(snapshot.placements.size())
					* sizeof(AtlasSnapshotPlacement);
				const UInt64 payloadOffset = sizeof(AtlasSnapshotHeader) + placementBytes;
				if (payloadOffset < sizeof(AtlasSnapshotHeader)
					|| snapshot.sourceHeader.storedPixelBytes
						> std::numeric_limits<UInt64>::max() - payloadOffset)
				{
					return false;
				}
				file.handle = CreateFileW(snapshot.sourcePath.c_str(), GENERIC_READ,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
					OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
				if (file.handle == INVALID_HANDLE_VALUE)
					return false;
				LARGE_INTEGER size = {};
				if (!GetFileSizeEx(file.handle, &size) || size.QuadPart < 0
					|| static_cast<UInt64>(size.QuadPart)
						!= payloadOffset + snapshot.sourceHeader.storedPixelBytes)
				{
					return false;
				}
				AtlasSnapshotHeader currentHeader = {};
				if (!ReadSnapshotBytesExact(file.handle, &currentHeader, sizeof(currentHeader))
					|| std::memcmp(&currentHeader, &snapshot.sourceHeader,
						sizeof(currentHeader)) != 0)
				{
					return false;
				}
				LARGE_INTEGER position = {};
				position.QuadPart = payloadOffset;
				return SetFilePointerEx(file.handle, position, nullptr, FILE_BEGIN) != FALSE;
			}

			bool LoadCompactSnapshotPixels(const CompactAtlasSnapshot& snapshot,
				std::vector<UInt8>& pixels)
			{
				if (!snapshot.pixels.empty())
				{
					pixels = snapshot.pixels;
					return true;
				}
				if (snapshot.sourceHeader.pixelBytes > std::numeric_limits<size_t>::max()
					|| snapshot.sourceHeader.storedPixelBytes
						> std::numeric_limits<size_t>::max())
					return false;
				CompactSnapshotFile file;
				if (!OpenCompactSnapshotPixels(snapshot, file))
					return false;
				std::vector<UInt8> storedPixels(
					static_cast<size_t>(snapshot.sourceHeader.storedPixelBytes));
				if (!ReadSnapshotBytesExact(file.handle,
					storedPixels.data(), storedPixels.size()))
					return false;
				const AtlasSnapshotStorage storageMode =
					static_cast<AtlasSnapshotStorage>(snapshot.sourceHeader.storageMode);
				if (storageMode == AtlasSnapshotStorage::PlacedLevelZeroRects
					&& snapshot.sourceHeader.storedPixelBytes
						== snapshot.sourceHeader.pixelBytes)
				{
					pixels = std::move(storedPixels);
				}
				else
				{
					return false;
				}
				UInt64 hash = HashCompactAtlasBytes(snapshot.placements.data(),
					snapshot.placements.size() * sizeof(AtlasSnapshotPlacement));
				hash = HashCompactAtlasBytes(pixels.data(), pixels.size(), hash);
				return hash == snapshot.sourceHeader.payloadChecksum;
			}
		}

		bool LoadCompactAtlasSnapshotPixels(const CompactAtlasSnapshot& snapshot,
			std::vector<UInt8>& pixels)
		{
			return LoadCompactSnapshotPixels(snapshot, pixels);
		}

		bool WriteCompactSnapshotPixels(UInt8* destination, LONG pitch,
			AtlasPixelMode destinationMode, const AtlasResource& resource)
		{
			if (!resource.compactSnapshot)
				return true;
			const CompactAtlasSnapshot& snapshot = *resource.compactSnapshot;
			const UInt32 sourceBytesPerPixel = AtlasBytesPerPixel(snapshot.pixelMode);
			const UInt32 destinationBytesPerPixel = AtlasBytesPerPixel(destinationMode);
			if (snapshot.pixelMode != destinationMode
				&& (snapshot.pixelMode == AtlasPixelMode::Mtsdf32
					|| destinationMode == AtlasPixelMode::Mtsdf32))
			{
				return false;
			}
			// Snapshot pages are capped at 16 MiB. Read and checksum one complete
			// page at a time instead of issuing one ReadFile call per glyph row
			// (millions of kernel calls for a full CJK profile). The bounded page
			// buffer is released before the next D3D9 texture is created.
			std::vector<UInt8> loadedPixels;
			if (snapshot.pixels.empty()
				&& !LoadCompactSnapshotPixels(snapshot, loadedPixels))
			{
				return false;
			}
			const std::vector<UInt8>& memoryPixels = snapshot.pixels.empty()
				? loadedPixels : snapshot.pixels;
			const size_t expectedPixelBytes = memoryPixels.size();
			size_t sourceOffset = 0;
			for (const AtlasSnapshotPlacement& placement : snapshot.placements)
			{
				const AtlasRect& rect = placement.rect;
				if (!placement.cacheId || !rect.width || !rect.height
					|| rect.x > resource.width || rect.width > resource.width - rect.x
					|| rect.y > resource.height || rect.height > resource.height - rect.y)
				{
					return false;
				}
				const size_t sourceRowBytes = static_cast<size_t>(rect.width)
					* sourceBytesPerPixel;
				const size_t rectBytes = sourceRowBytes * rect.height;
				if (sourceOffset > expectedPixelBytes
					|| rectBytes > expectedPixelBytes - sourceOffset)
				{
					return false;
				}
				for (UInt32 row = 0; row < rect.height; ++row)
				{
					const UInt8* source = memoryPixels.data() + sourceOffset
						+ static_cast<size_t>(row) * sourceRowBytes;
					UInt8* target = destination
						+ static_cast<size_t>(rect.y + row) * pitch
						+ static_cast<size_t>(rect.x) * destinationBytesPerPixel;
					if (snapshot.pixelMode == destinationMode)
					{
						std::memcpy(target, source, sourceRowBytes);
					}
					else if (snapshot.pixelMode == AtlasPixelMode::A8)
					{
						for (UInt32 x = 0; x < rect.width; ++x)
						{
							target[x * 4 + 0] = 0xFF;
							target[x * 4 + 1] = 0xFF;
							target[x * 4 + 2] = 0xFF;
							target[x * 4 + 3] = source[x];
						}
					}
					else
					{
						for (UInt32 x = 0; x < rect.width; ++x)
							target[x] = source[x * 4 + 3];
					}
				}
				sourceOffset += rectBytes;
			}
			return sourceOffset == expectedPixelBytes;
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

	AtlasGlyphRecord* FindAtlasGlyph(AtlasResource& resource, UInt64 cacheId)
	{
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

	namespace
	{
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
				NotifyNativeA8AtlasTextureMutation();
			}
			resource.resetPending = true;
			State().defaultPoolMaintenancePending.store(true, std::memory_order_release);
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
					resource.width, resource.height, mode, resource.mipLevels);
				if (d3dTexture && PopulateDefaultTexture(d3dTexture, resource, mode))
				{
					data->m_pkD3DTexture = d3dTexture;
					if (data->InitializeFromD3DTexture(d3dTexture))
					{
						texture->m_kFormatPrefs.m_ePixelLayout = mode == AtlasPixelMode::A8
							? NiTexture::FormatPrefs::SINGLE_COLOR_8
							: NiTexture::FormatPrefs::TRUE_COLOR_32;
						texture->m_kFormatPrefs.m_eMipMapped = d3dTexture->GetLevelCount() > 1
							? NiTexture::FormatPrefs::YES : NiTexture::FormatPrefs::NO;
						resource.pixelMode = mode;
						resource.mipLevels = d3dTexture->GetLevelCount();
						resource.resetPending = false;
						// Each wrapper owns a separate rebuilt D3D texture after reset.
						resource.sharedGpuPage = false;
						NotifyNativeA8AtlasTextureMutation();
						RecordFreeTypePerf(FreeTypePerfCounter::AtlasUpload);
						RecordFreeTypePerf(FreeTypePerfCounter::AtlasUploadBytes,
							static_cast<UInt64>(GetAtlasStorageBytes(resource.width,
								resource.height, mode, resource.mipLevels)));
						return true;
					}
					data->m_pkD3DTexture = nullptr;
				}
				if (d3dTexture)
					d3dTexture->Release();
				if (mode != AtlasPixelMode::A8)
					break;
				mode = AtlasPixelMode::Argb32;
			}
			resource.resetPending = true;
			State().defaultPoolMaintenancePending.store(true, std::memory_order_release);
			return false;
		}

		bool DefaultPoolResetCallback(bool beforeReset, void*)
		{
			AtlasState& state = State();
			if (state.defaultPoolShutdown)
				return true;
			const bool logResetTiming = g_bEnableFreeTypeFontRenderingLog;
			const auto started = logResetTiming
				? std::chrono::steady_clock::now()
				: std::chrono::steady_clock::time_point{};
			UInt32 processed = 0;
			UInt32 failed = 0;
			{
				std::lock_guard<std::mutex> lock(state.atlasMutex);
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
				for (auto& [key, entry] : state.atlasCache)
				{
					if (entry.resource)
						process(*entry.resource);
				}
				for (RetiredAtlasGeneration& retired : state.retiredAtlases)
				{
					if (retired.resource)
						process(*retired.resource);
				}
				if (!beforeReset)
				{
					state.atlasCacheBytes = 0;
					for (auto& [key, entry] : state.atlasCache)
					{
						if (!entry.resource)
							continue;
						entry.bytes = GetAtlasStorageBytes(entry.resource->width,
							entry.resource->height, entry.resource->pixelMode,
							entry.resource->mipLevels);
						state.atlasCacheBytes += entry.bytes;
					}
					PruneRetiredAtlases();
					TrimAtlasCache(state);
				}
				state.defaultPoolMaintenancePending.store(
					HasDefaultPoolMaintenanceLocked(), std::memory_order_release);
			}
			if (logResetTiming)
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
			|| State().defaultPoolResetRegistered)
			return;
		State().defaultPoolShutdown = false;
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		if (!renderer || !renderer->GetD3DDevice())
			return;
		ResolveGpuAtlasBudget(true);
		ThisStdCall<UInt32>(0x86BAE0, renderer, DefaultPoolResetCallback, nullptr);
		State().defaultPoolResetRegistered = true;
		gLog.FormattedMessage(
			"tnvse_freetype_font: registered DEFAULT atlas device reset lifecycle");
	}

	void PumpDefaultPoolAtlasLifecycle()
	{
		if (!g_bEnableFreeTypeFontRendering || !g_bEnableFreeTypeDefaultPoolAtlas
			|| State().defaultPoolShutdown)
			return;
		InitializeDefaultPoolAtlasLifecycle();
		if (!State().defaultPoolMaintenancePending.load(std::memory_order_acquire))
			return;
		static UInt32 retryFrame = 0;
		const bool retry = (++retryFrame % 120u) == 0;
		if (!retry)
			return;
		std::lock_guard<std::mutex> lock(State().atlasMutex);
		PruneRetiredAtlases();
		for (auto& [key, entry] : State().atlasCache)
		{
			if (entry.resource && entry.resource->resetPending)
				RebuildDefaultPoolTexture(*entry.resource);
		}
		for (RetiredAtlasGeneration& retired : State().retiredAtlases)
		{
			if (retired.resource && retired.resource->resetPending)
				RebuildDefaultPoolTexture(*retired.resource);
		}
		State().defaultPoolMaintenancePending.store(
			HasDefaultPoolMaintenanceLocked(), std::memory_order_release);
	}

	void ShutdownDefaultPoolAtlasLifecycle()
	{
		// NVSE broadcasts ExitGame while the active StartMenu can still own and
		// render text shapes during its final fade.  A menu overhaul may keep many
		// retired atlas generations alive; synchronously clearing them here walks
		// and destroys large pixel buffers and glyph maps before the window closes,
		// leaving the game visibly stuck on that transition frame.  Quiesce reset
		// handling and new DEFAULT-pool allocations, but keep all referenced atlas
		// generations valid.  ExitProcess reclaims them with the rest of the address
		// space, while normal device-reset and runtime eviction paths are unchanged.
		State().defaultPoolShutdown = true;
		if (g_bEnableFreeTypeFontRenderingLog)
			gLog.FormattedMessage(
				"tnvse_freetype_font: atlas lifecycle quiesced for process exit; resource reclamation deferred");
	}
}

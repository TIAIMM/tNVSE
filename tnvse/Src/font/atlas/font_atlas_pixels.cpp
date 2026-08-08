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

		bool AtlasRectContains(const AtlasRect& outer, const AtlasRect& inner)
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

		void BuildMergedAtlasDirtyRects(const std::vector<AtlasRect>& source,
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

		bool GeneratePixelDataMipRegions(NiPixelData& pixelData,
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
					result += glyph.bitmap->pixels.size();
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
				: &NiPixelFormat_RGBA32;
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
			if (!destination || bitmap.pixels.size() < requiredBitmapBytes
				|| !IsCompatibleDistanceFieldBitmap(mode, bitmap))
				return;
			const UInt32 bytesPerPixel = AtlasBytesPerPixel(mode);
			for (UInt32 y = 0; y < rect.height; ++y)
			{
				UInt8* row = destination + static_cast<size_t>(destinationY + y) * pitch
					+ static_cast<size_t>(destinationX) * bytesPerPixel;
				const UInt8* source = bitmap.pixels.data()
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

		namespace implementation::font_atlas_resource {}
		using namespace implementation::font_atlas_resource;

		namespace implementation::font_atlas_resource
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

			constexpr size_t kCompactSnapshotCompareBufferBytes =
				1024u * 1024u;

			struct CompactSnapshotSlice
			{
				AtlasRect rect = {};
				size_t offset = 0;
				size_t bytes = 0;
			};

			class CompactSnapshotRangeReader
			{
			public:
				explicit CompactSnapshotRangeReader(
					const CompactAtlasSnapshot& snapshot)
					: m_snapshot(snapshot)
				{
				}

				bool Open()
				{
					if (m_snapshot.sourceHeader.placementCount
							!= m_snapshot.placements.size()
						|| m_snapshot.sourceHeader.pixelMode
							!= static_cast<UInt8>(m_snapshot.pixelMode)
						|| static_cast<AtlasSnapshotStorage>(
							m_snapshot.sourceHeader.storageMode)
							!= AtlasSnapshotStorage::PlacedLevelZeroRects
						|| m_snapshot.sourceHeader.pixelBytes
							!= m_snapshot.sourceHeader.storedPixelBytes
						|| m_snapshot.sourceHeader.pixelBytes
							> std::numeric_limits<size_t>::max())
					{
						return false;
					}
					m_size = static_cast<size_t>(
						m_snapshot.sourceHeader.pixelBytes);
					if (!m_snapshot.pixels.empty())
						return m_snapshot.pixels.size() == m_size;
					if (!OpenCompactSnapshotPixels(m_snapshot, m_file))
						return false;
					const UInt64 placementBytes = static_cast<UInt64>(
						m_snapshot.placements.size())
						* sizeof(AtlasSnapshotPlacement);
					m_payloadOffset = sizeof(AtlasSnapshotHeader)
						+ placementBytes;
					return m_payloadOffset >= sizeof(AtlasSnapshotHeader);
				}

				size_t Size() const { return m_size; }

				bool ReadAt(size_t offset, void* destination, size_t size)
				{
					if (offset > m_size || size > m_size - offset)
						return false;
					if (!m_snapshot.pixels.empty())
					{
						std::memcpy(destination,
							m_snapshot.pixels.data() + offset, size);
						return true;
					}
					if (m_file.handle == INVALID_HANDLE_VALUE
						|| offset > std::numeric_limits<UInt64>::max()
							- m_payloadOffset)
					{
						return false;
					}
					LARGE_INTEGER position = {};
					position.QuadPart = m_payloadOffset + offset;
					return SetFilePointerEx(m_file.handle, position,
							nullptr, FILE_BEGIN) != FALSE
						&& ReadSnapshotBytesExact(
							m_file.handle, destination, size);
				}

			private:
				const CompactAtlasSnapshot& m_snapshot;
				CompactSnapshotFile m_file;
				UInt64 m_payloadOffset = 0;
				size_t m_size = 0;
			};

			bool BuildCompactSnapshotSlices(
				const CompactAtlasSnapshot& snapshot,
				size_t expectedBytes,
				std::vector<CompactSnapshotSlice>& slices)
			{
				slices.clear();
				slices.reserve(snapshot.placements.size());
				size_t offset = 0;
				const size_t bytesPerPixel = AtlasBytesPerPixel(
					snapshot.pixelMode);
				for (const AtlasSnapshotPlacement& placement :
					snapshot.placements)
				{
					if (!placement.rect.width || !placement.rect.height
						|| placement.rect.width
							> std::numeric_limits<size_t>::max()
								/ placement.rect.height
						|| static_cast<size_t>(placement.rect.width)
							* placement.rect.height
							> std::numeric_limits<size_t>::max()
								/ bytesPerPixel)
					{
						return false;
					}
					const size_t bytes = static_cast<size_t>(
						placement.rect.width) * placement.rect.height
						* bytesPerPixel;
					if (offset > expectedBytes
						|| bytes > expectedBytes - offset)
					{
						return false;
					}
					slices.push_back({ placement.rect, offset, bytes });
					offset += bytes;
				}
				std::sort(slices.begin(), slices.end(),
					[](const CompactSnapshotSlice& left,
						const CompactSnapshotSlice& right)
					{
						if (left.rect.y != right.rect.y)
							return left.rect.y < right.rect.y;
						if (left.rect.x != right.rect.x)
							return left.rect.x < right.rect.x;
						if (left.rect.height != right.rect.height)
							return left.rect.height < right.rect.height;
						return left.rect.width < right.rect.width;
					});
				return offset == expectedBytes;
			}

			bool ValidateCompactSnapshotPayloadBounded(
				const CompactAtlasSnapshot& snapshot,
				CompactSnapshotRangeReader& reader,
				std::vector<UInt8>& buffer)
			{
				UInt64 hash = HashCompactAtlasBytes(
					snapshot.placements.data(),
					snapshot.placements.size()
						* sizeof(AtlasSnapshotPlacement));
				for (size_t offset = 0; offset < reader.Size();)
				{
					const size_t chunk = std::min(
						reader.Size() - offset, buffer.size());
					if (!reader.ReadAt(offset, buffer.data(), chunk))
						return false;
					hash = HashCompactAtlasBytes(
						buffer.data(), chunk, hash);
					offset += chunk;
					ServiceFontPrewarmHostMessages();
				}
				return hash == snapshot.sourceHeader.payloadChecksum;
			}

			bool CompareCompactSnapshotPixelsBounded(
				const CompactAtlasSnapshot& left,
				const CompactAtlasSnapshot& right)
			{
				CompactSnapshotRangeReader leftReader(left);
				CompactSnapshotRangeReader rightReader(right);
				if (!leftReader.Open() || !rightReader.Open()
					|| leftReader.Size() != rightReader.Size())
				{
					return false;
				}
				std::vector<CompactSnapshotSlice> leftSlices;
				std::vector<CompactSnapshotSlice> rightSlices;
				if (!BuildCompactSnapshotSlices(
						left, leftReader.Size(), leftSlices)
					|| !BuildCompactSnapshotSlices(
						right, rightReader.Size(), rightSlices)
					|| leftSlices.size() != rightSlices.size())
				{
					return false;
				}
				std::vector<UInt8> leftBuffer(
					kCompactSnapshotCompareBufferBytes);
				std::vector<UInt8> rightBuffer(
					kCompactSnapshotCompareBufferBytes);
				if (!ValidateCompactSnapshotPayloadBounded(
						left, leftReader, leftBuffer))
				{
					return false;
				}
				const bool samePhysicalSource = !left.sourcePath.empty()
					&& !right.sourcePath.empty()
					&& _wcsicmp(left.sourcePath.c_str(),
						right.sourcePath.c_str()) == 0
					&& std::memcmp(&left.sourceHeader,
						&right.sourceHeader,
						sizeof(left.sourceHeader)) == 0
					&& left.placements.size() == right.placements.size()
					&& (left.placements.empty()
						|| std::memcmp(left.placements.data(),
							right.placements.data(),
							left.placements.size()
								* sizeof(AtlasSnapshotPlacement)) == 0);
				if (samePhysicalSource)
					return true;
				if (!ValidateCompactSnapshotPayloadBounded(
						right, rightReader, leftBuffer))
				{
					return false;
				}
				for (size_t index = 0; index < leftSlices.size(); ++index)
				{
					const CompactSnapshotSlice& a = leftSlices[index];
					const CompactSnapshotSlice& b = rightSlices[index];
					if (std::memcmp(&a.rect, &b.rect,
							sizeof(a.rect)) != 0 || a.bytes != b.bytes)
					{
						return false;
					}
					for (size_t compared = 0; compared < a.bytes;)
					{
						const size_t chunk = std::min(
							a.bytes - compared, leftBuffer.size());
						if (!leftReader.ReadAt(a.offset + compared,
								leftBuffer.data(), chunk)
							|| !rightReader.ReadAt(b.offset + compared,
								rightBuffer.data(), chunk)
							|| std::memcmp(leftBuffer.data(),
								rightBuffer.data(), chunk) != 0)
						{
							return false;
						}
						compared += chunk;
						ServiceFontPrewarmHostMessages();
					}
				}
				return true;
			}

			class BufferedSnapshotReader
			{
			public:
				explicit BufferedSnapshotReader(HANDLE file)
					: m_file(file), m_buffer(1024u * 1024u)
				{
				}

				bool ReadExact(void* destination, size_t size)
				{
					UInt8* output = static_cast<UInt8*>(destination);
					while (size)
					{
						if (m_offset == m_size)
						{
							DWORD read = 0;
							if (!ReadFile(m_file, m_buffer.data(),
								static_cast<DWORD>(m_buffer.size()),
								&read, nullptr)
								|| !read)
							{
								return false;
							}
							m_offset = 0;
							m_size = read;
						}
						const size_t copied = std::min(
							size, m_size - m_offset);
						std::memcpy(output,
							m_buffer.data() + m_offset, copied);
						output += copied;
						size -= copied;
						m_offset += copied;
					}
					return true;
				}

			private:
				HANDLE m_file = INVALID_HANDLE_VALUE;
				std::vector<UInt8> m_buffer;
				size_t m_offset = 0;
				size_t m_size = 0;
			};

			bool StreamCompactSnapshotPixels(UInt8* destination,
				LONG pitch, AtlasPixelMode destinationMode,
				const AtlasResource& resource,
				const CompactAtlasSnapshot& snapshot)
			{
				if (!destination || pitch <= 0
					|| static_cast<AtlasSnapshotStorage>(
						snapshot.sourceHeader.storageMode)
						!= AtlasSnapshotStorage::PlacedLevelZeroRects
					|| snapshot.sourceHeader.storedPixelBytes
						!= snapshot.sourceHeader.pixelBytes)
				{
					return false;
				}
				CompactSnapshotFile file;
				if (!OpenCompactSnapshotPixels(snapshot, file))
					return false;
				BufferedSnapshotReader reader(file.handle);
				const UInt32 sourceBytesPerPixel =
					AtlasBytesPerPixel(snapshot.pixelMode);
				const UInt32 destinationBytesPerPixel =
					AtlasBytesPerPixel(destinationMode);
				UInt64 payloadHash = HashCompactAtlasBytes(
					snapshot.placements.data(),
					snapshot.placements.size()
						* sizeof(AtlasSnapshotPlacement));
				size_t sourceBytes = 0;
				std::vector<UInt8> sourceRow;
				for (const AtlasSnapshotPlacement& placement :
					snapshot.placements)
				{
					const AtlasRect& rect = placement.rect;
					if (!placement.cacheId || !rect.width
						|| !rect.height || rect.x > resource.width
						|| rect.width > resource.width - rect.x
						|| rect.y > resource.height
						|| rect.height > resource.height - rect.y)
					{
						return false;
					}
					const size_t sourceRowBytes =
						static_cast<size_t>(rect.width)
							* sourceBytesPerPixel;
					if (sourceRowBytes
						> std::numeric_limits<size_t>::max()
							- sourceBytes)
					{
						return false;
					}
					sourceRow.resize(sourceRowBytes);
					for (UInt32 row = 0; row < rect.height; ++row)
					{
						UInt8* target = destination
							+ static_cast<size_t>(rect.y + row)
								* pitch
							+ static_cast<size_t>(rect.x)
								* destinationBytesPerPixel;
						UInt8* source = sourceRow.data();
						if (!reader.ReadExact(source,
							sourceRowBytes))
						{
							return false;
						}
						payloadHash = HashCompactAtlasBytes(
							source, sourceRowBytes, payloadHash);
						if (snapshot.pixelMode
							== destinationMode)
						{
							std::memcpy(target, source,
								sourceRowBytes);
						}
						else if (snapshot.pixelMode
							== AtlasPixelMode::A8)
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
						sourceBytes += sourceRowBytes;
					}
				}
				return sourceBytes
						== snapshot.sourceHeader.storedPixelBytes
					&& payloadHash
						== snapshot.sourceHeader.payloadChecksum;
			}
		}

		bool LoadCompactAtlasSnapshotPixels(const CompactAtlasSnapshot& snapshot,
			std::vector<UInt8>& pixels)
		{
			return LoadCompactSnapshotPixels(snapshot, pixels);
		}

		bool CompactAtlasSnapshotPixelsEqualBounded(
			const CompactAtlasSnapshot& left,
			const CompactAtlasSnapshot& right)
		{
			return CompareCompactSnapshotPixelsBounded(left, right);
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
			// A forced complete-code-page atlas can exceed 200 MiB. Stream the
			// compact payload through a bounded reader directly into the locked
			// DEFAULT texture instead of recreating a full CPU texture copy.
			if (snapshot.pixels.empty())
			{
				return StreamCompactSnapshotPixels(destination,
					pitch, destinationMode, resource, snapshot);
			}
			const std::vector<UInt8>& memoryPixels = snapshot.pixels;
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

}

#include "font_atlas_internal.h"

#include "encoding.h"
#include "globals.h"
#include "load_config.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "font_atlas_direct_internal.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_atlas_direct {}
	using namespace implementation::font_atlas_direct;

	namespace implementation::font_atlas_direct
	{
		constexpr UInt32 kDirectCachedLetterVersion = 4;

		UInt32 EncodeDirectGlyphSlot(VectorFontByteClass byteClass,
			size_t slot);
		size_t GetDirectGlyphSlotCount(VectorFontByteClass byteClass);
		std::vector<GlyphMaskType> GetDirectProfileMasks(
			const FontConfig& config, const AtlasCacheKey& key);

		const DirectAtlasGlyphLayer* FindDirectLayer(
			const DirectCachedLetter& letter, GlyphMaskType mask)
		{
			const UInt8 maskValue = static_cast<UInt8>(mask);
			for (const DirectAtlasGlyphLayer& layer : letter.layers)
			{
				if (layer.valid() && layer.maskType == maskValue)
					return &layer;
			}
			return nullptr;
		}

		DirectAtlasGlyphLayer* FindOrCreateDirectLayer(
			DirectCachedLetter& letter, GlyphMaskType mask)
		{
			if (const DirectAtlasGlyphLayer* existing =
					FindDirectLayer(letter, mask))
			{
				return const_cast<DirectAtlasGlyphLayer*>(existing);
			}
			for (DirectAtlasGlyphLayer& layer : letter.layers)
			{
				if (!layer.valid())
				{
					layer = {};
					layer.maskType = static_cast<UInt8>(mask);
					return &layer;
				}
			}
			return nullptr;
		}

#pragma pack(push, 1)
		struct DirectCachedLetterFileHeader
		{
			UInt8 magic[8] = {};
			UInt32 version = 0;
			UInt32 headerSize = 0;
			UInt32 atlasSnapshotVersion = 0;
			UInt32 manifestVersion = 0;
			UInt8 manifestIdentityVersion = 0;
			UInt8 byteClass = 0;
			UInt8 pixelMode = 0;
			UInt8 renderMode = 0;
			UInt8 recordKind = 0;
			UInt8 effectLayerMask = 0;
			UInt16 reserved = 0;
			UInt32 codePage = 0;
			UInt32 scaleMilli = 0;
			UInt32 padding = 0;
			UInt32 slotCount = 0;
			UInt32 recordSize = 0;
			UInt32 auxiliaryRecordSize = 0;
			UInt32 pageCount = 0;
			UInt32 resolvedGlyphs = 0;
			UInt32 resolvedLayers = 0;
			UInt64 profileIdentity = 0;
			UInt64 layoutIdentity = 0;
			UInt64 effectIdentity = 0;
			UInt64 atlasIdentity = 0;
			UInt64 pageIdentityChecksum = 0;
			UInt64 recordsChecksum = 0;
			UInt64 auxiliaryChecksum = 0;
			UInt64 checksum = 0;
		};
#pragma pack(pop)
		static_assert(sizeof(DirectCachedLetterFileHeader) == 132);

		UInt64 HashDirectBytes(const void* data, size_t size, UInt64 hash)
		{
			const UInt8* bytes = static_cast<const UInt8*>(data);
			for (size_t index = 0; index < size; ++index)
			{
				hash ^= bytes[index];
				hash *= 1099511628211ull;
			}
			return hash;
		}

		UInt64 BuildDirectProfileIdentity(const AtlasCacheKey& key,
			const DirectAtlasGlyphTable& table,
			const std::vector<std::shared_ptr<AtlasResource>>& pages)
		{
			UInt64 hash = HashDirectBytes(&kDirectCachedLetterVersion,
				sizeof(kDirectCachedLetterVersion));
			hash = HashDirectBytes(
				&kAtlasSnapshotPayloadIdentityVersion,
				sizeof(kAtlasSnapshotPayloadIdentityVersion), hash);
			hash = HashDirectBytes(&kPersistentGlyphManifestVersion,
				sizeof(kPersistentGlyphManifestVersion), hash);
			hash = HashDirectBytes(
				&kPersistentGlyphManifestCacheIdentityVersion,
				sizeof(kPersistentGlyphManifestCacheIdentityVersion), hash);
			hash = HashDirectBytes(&key.atlasContentHash,
				sizeof(key.atlasContentHash), hash);
			hash = HashDirectBytes(&key.scaleMilli,
				sizeof(key.scaleMilli), hash);
			hash = HashDirectBytes(&key.pixelMode,
				sizeof(key.pixelMode), hash);
			hash = HashDirectBytes(&key.renderMode,
				sizeof(key.renderMode), hash);
			hash = HashDirectBytes(&key.padding, sizeof(key.padding), hash);
			hash = HashDirectBytes(&key.levelZeroOnly,
				sizeof(key.levelZeroOnly), hash);
			hash = HashDirectBytes(&key.byteClass,
				sizeof(key.byteClass), hash);
			hash = HashDirectBytes(&table.recordKind,
				sizeof(table.recordKind), hash);
			hash = HashDirectBytes(&table.effectLayerMask,
				sizeof(table.effectLayerMask), hash);
			hash = HashDirectBytes(&table.codePage,
				sizeof(table.codePage), hash);
			hash = HashDirectBytes(&table.layoutIdentity,
				sizeof(table.layoutIdentity), hash);
			hash = HashDirectBytes(&table.effectIdentity,
				sizeof(table.effectIdentity), hash);
			hash = HashDirectBytes(&table.atlasIdentity,
				sizeof(table.atlasIdentity), hash);
			hash = HashDirectBytes(&table.pageIdentityChecksum,
				sizeof(table.pageIdentityChecksum), hash);
			const UInt32 pageCount = static_cast<UInt32>(pages.size());
			hash = HashDirectBytes(&pageCount, sizeof(pageCount), hash);
			for (const auto& page : pages)
			{
				if (!page || !page->compactSnapshot)
					return 0;
				hash = HashDirectBytes(&page->pageContentHash,
					sizeof(page->pageContentHash), hash);
				const AtlasSnapshotHeader& source =
					page->compactSnapshot->sourceHeader;
				hash = HashDirectBytes(&source.snapshotHash,
					sizeof(source.snapshotHash), hash);
				hash = HashDirectBytes(&source.payloadChecksum,
					sizeof(source.payloadChecksum), hash);
			}
			return hash;
		}

		UInt64 BuildDirectPageIdentityChecksum(
			const std::vector<std::shared_ptr<AtlasResource>>& pages)
		{
			UInt64 hash = 1469598103934665603ull;
			for (const auto& page : pages)
			{
				if (!page || !page->compactSnapshot)
					return 0;
				hash = HashDirectBytes(&page->pageContentHash,
					sizeof(page->pageContentHash), hash);
				const AtlasSnapshotHeader& source =
					page->compactSnapshot->sourceHeader;
				hash = HashDirectBytes(&source.snapshotHash,
					sizeof(source.snapshotHash), hash);
				hash = HashDirectBytes(&source.payloadChecksum,
					sizeof(source.payloadChecksum), hash);
			}
			return hash;
		}

		UInt8 BuildDirectEffectLayerMask(
			const std::vector<GlyphMaskType>& masks)
		{
			UInt8 result = 0;
			for (GlyphMaskType mask : masks)
			{
				const UInt8 bit = static_cast<UInt8>(mask);
				if (bit < 8)
					result |= static_cast<UInt8>(1u << bit);
			}
			return result;
		}

		size_t GetDirectTableStorageBytes(
			const DirectAtlasGlyphTable& table)
		{
			return sizeof(DirectAtlasGlyphTable)
				+ table.pages.capacity()
					* sizeof(std::weak_ptr<AtlasResource>)
				+ table.glyphs.capacity()
					* sizeof(DirectAtlasGlyphRecord)
				+ table.stockGlyphs.capacity() * sizeof(FontLetter)
				+ table.faceIndices.capacity() * sizeof(UInt8);
		}

		std::wstring GetDirectCachedLetterPath(UInt64 identity,
			VectorFontByteClass byteClass)
		{
			std::wstring directory;
			if (!identity || !GetFreeTypeFontCacheDirectory(directory))
				return {};
			wchar_t fileName[128] = {};
			_snwprintf_s(fileName, _countof(fileName), _TRUNCATE,
				L"shared_%016llX_%c.tnvfdirect",
				static_cast<unsigned long long>(identity),
				byteClass == VectorFontByteClass::DoubleByte ? L'd' : L's');
			std::wstring path = directory + L"\\" + fileName;
			MarkFreeTypeFontCacheFileUsed(path);
			return path;
		}

		bool ReadDirectFile(HANDLE file, void* destination, size_t size)
		{
			UInt8* output = static_cast<UInt8*>(destination);
			while (size)
			{
				const DWORD chunk = static_cast<DWORD>(std::min<size_t>(
					size, std::numeric_limits<DWORD>::max()));
				DWORD read = 0;
				if (!ReadFile(file, output, chunk, &read, nullptr)
					|| read != chunk)
					return false;
				output += read;
				size -= read;
			}
			return true;
		}

		bool WriteDirectFile(HANDLE file, const void* source, size_t size)
		{
			const UInt8* input = static_cast<const UInt8*>(source);
			while (size)
			{
				const DWORD chunk = static_cast<DWORD>(std::min<size_t>(
					size, std::numeric_limits<DWORD>::max()));
				DWORD written = 0;
				if (!WriteFile(file, input, chunk, &written, nullptr)
					|| written != chunk)
					return false;
				input += written;
				size -= written;
			}
			return true;
		}

		bool ValidateDirectLayer(const DirectAtlasGlyphLayer& layer,
			GlyphMaskType mask,
			const std::vector<std::shared_ptr<AtlasResource>>& pages)
		{
			if (!layer.valid() || layer.maskType != static_cast<UInt8>(mask)
				|| layer.reserved || layer.pageSlot >= pages.size())
				return false;
			const std::shared_ptr<AtlasResource>& page =
				pages[layer.pageSlot];
			if (!page || !page->pageContentHash || !page->compactSnapshot
				|| layer.snapshotPlacementIndex
					>= page->compactSnapshot->placements.size())
				return false;
			const AtlasSnapshotPlacement& snapshot =
				page->compactSnapshot->placements[
					layer.snapshotPlacementIndex];
			return snapshot.cacheId
				&& snapshot.maskType == layer.maskType
				&& page->compactSnapshot->sourceHeader.pageContentHash
					== page->pageContentHash
				&& IsValidAtlasSnapshotGlyphPlacement(snapshot,
					page->width, page->height,
					page->compactSnapshot->sourceHeader.pageIndex);
		}

		bool IsFiniteDirectMetrics(float width, float height,
			float leadingEdge, float spacing, float topEdge)
		{
			return std::isfinite(width) && std::isfinite(height)
				&& std::isfinite(leadingEdge) && std::isfinite(spacing)
				&& std::isfinite(topEdge);
		}

		void NormalizeKnownEmptyAdvance(float& width, float& spacing)
		{
			// FreeType serializes an empty glyph such as a space as
			// width=0, spacing=advance.  The stock FontLetter contract only
			// adds spacing when width is positive, so preserve the same total
			// advance in the width field for direct immutable records.
			const float combinedWidth = width + spacing;
			if (std::isfinite(combinedWidth) && combinedWidth > 0.0f
				&& (width <= 0.0f || spacing != 0.0f))
			{
				width = combinedWidth;
				spacing = 0.0f;
			}
		}

		void NormalizeKnownEmptyAdvance(FontLetter& letter)
		{
			NormalizeKnownEmptyAdvance(letter.fWidth, letter.fSpacing);
		}

		void NormalizeKnownEmptyAdvance(DirectCachedLetter& letter)
		{
			NormalizeKnownEmptyAdvance(letter.width, letter.spacing);
		}

		bool ValidateStockDirectLetter(const FontLetter& letter,
			const std::vector<std::shared_ptr<AtlasResource>>& pages)
		{
			if (!IsFiniteDirectMetrics(letter.fWidth, letter.fHeight,
				letter.fLeadingEdge, letter.fSpacing, letter.fTopEdge))
			{
				return false;
			}
			if (letter.iTextureIndex == -1 || letter.iTextureIndex == -2)
				return true;
			if (letter.iTextureIndex < 0
				|| static_cast<size_t>(letter.iTextureIndex) >= pages.size())
			{
				return false;
			}
			const auto& page = pages[letter.iTextureIndex];
			if (!page || !page->compactSnapshot || !page->pageContentHash)
				return false;
			for (const UVMap& uv : letter.pMapping)
			{
				if (!std::isfinite(uv.fU) || !std::isfinite(uv.fV)
					|| uv.fU < 0.0f || uv.fU > 1.0f
					|| uv.fV < 0.0f || uv.fV > 1.0f)
				{
					return false;
				}
			}
			return true;
		}

		bool TryLoadDirectCachedLetters(const std::wstring& path,
			UInt64 identity, VectorFontByteClass byteClass,
			const AtlasCacheKey& key,
			const std::vector<std::shared_ptr<AtlasResource>>& pages,
			const std::vector<GlyphMaskType>& masks,
			DirectAtlasGlyphTable& table)
		{
			if (path.empty())
				return false;
			HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return false;
			DirectCachedLetterFileHeader header;
			const UInt8 magic[8] =
				{ 'T', 'N', 'V', 'F', 'D', 'I', 'R', '1' };
			const size_t slotCount = table.SlotCount();
			const size_t recordSize =
				table.recordKind == DirectCachedLetterKind::StockFontLetter
					? sizeof(FontLetter) : sizeof(DirectCachedLetter);
			bool valid = ReadDirectFile(file, &header, sizeof(header))
				&& std::memcmp(header.magic, magic, sizeof(magic)) == 0
				&& header.version == kDirectCachedLetterVersion
				&& header.headerSize == sizeof(header)
				&& header.atlasSnapshotVersion == kAtlasSnapshotVersion
				&& header.manifestVersion == kPersistentGlyphManifestVersion
				&& header.manifestIdentityVersion
					== kPersistentGlyphManifestCacheIdentityVersion
				&& header.byteClass == static_cast<UInt8>(byteClass)
				&& header.pixelMode == static_cast<UInt8>(key.pixelMode)
				&& header.renderMode == static_cast<UInt8>(key.renderMode)
				&& header.recordKind
					== static_cast<UInt8>(table.recordKind)
				&& header.effectLayerMask == table.effectLayerMask
				&& !header.reserved
				&& header.codePage == table.codePage
				&& header.scaleMilli == key.scaleMilli
				&& header.padding == key.padding
				&& header.slotCount == slotCount
				&& header.recordSize == recordSize
				&& header.auxiliaryRecordSize == sizeof(UInt8)
				&& header.pageCount == pages.size()
				&& header.profileIdentity == identity
				&& header.layoutIdentity == table.layoutIdentity
				&& header.effectIdentity == table.effectIdentity
				&& header.atlasIdentity == key.atlasContentHash
				&& header.pageIdentityChecksum
					== table.pageIdentityChecksum
				&& header.checksum == HashDirectBytes(&header,
					offsetof(DirectCachedLetterFileHeader, checksum));
			std::vector<UInt8> recordBytes;
			std::vector<UInt8> faceIndices;
			if (valid)
			{
				recordBytes.resize(slotCount * recordSize);
				faceIndices.resize(slotCount);
				valid = ReadDirectFile(file, recordBytes.data(),
						recordBytes.size())
					&& header.recordsChecksum == HashDirectBytes(
						recordBytes.data(), recordBytes.size())
					&& ReadDirectFile(file, faceIndices.data(),
						faceIndices.size())
					&& header.auxiliaryChecksum == HashDirectBytes(
						faceIndices.data(), faceIndices.size());
			}
			LARGE_INTEGER fileSize = {};
			if (valid)
			{
				const UInt64 expected = sizeof(header)
					+ static_cast<UInt64>(recordBytes.size())
					+ static_cast<UInt64>(faceIndices.size());
				valid = GetFileSizeEx(file, &fileSize)
					&& static_cast<UInt64>(fileSize.QuadPart) == expected;
			}
			CloseHandle(file);
			if (!valid)
				return false;

			UInt32 resolvedGlyphs = 0;
			UInt32 resolvedLayers = 0;
			if (table.recordKind
				== DirectCachedLetterKind::StockFontLetter)
			{
				std::vector<FontLetter> records(slotCount);
				std::memcpy(records.data(), recordBytes.data(),
					recordBytes.size());
				for (FontLetter& record : records)
				{
					if (!ValidateStockDirectLetter(record, pages))
						return false;
					if (record.iTextureIndex == -1)
						continue;
					if (record.iTextureIndex < -2)
						return false;
					if (record.iTextureIndex == -2)
						NormalizeKnownEmptyAdvance(record);
					++resolvedGlyphs;
					if (record.iTextureIndex >= 0)
						++resolvedLayers;
				}
				table.stockGlyphs = std::move(records);
				table.glyphs.clear();
				table.glyphs.shrink_to_fit();
			}
			else
			{
				std::vector<DirectCachedLetter> records(slotCount);
				std::memcpy(records.data(), recordBytes.data(),
					recordBytes.size());
				for (size_t slot = 0; slot < records.size(); ++slot)
				{
					DirectCachedLetter& record = records[slot];
					if (!record.flags)
						continue;
					if ((record.flags & ~kDirectCachedLetterKnownFlags)
						|| !(record.flags & kDirectCachedLetterValid)
						|| record.encodedCode != EncodeDirectGlyphSlot(
							byteClass, slot)
						|| record.byteClass
							!= static_cast<UInt8>(byteClass)
						|| !IsFiniteDirectMetrics(record.width,
							record.height, record.leadingEdge,
							record.spacing, record.topEdge))
					{
						return false;
					}
					const bool knownEmpty =
						(record.flags & kDirectCachedLetterKnownEmpty) != 0;
					if (knownEmpty)
						NormalizeKnownEmptyAdvance(record);
					bool complete = true;
					UInt32 layers = 0;
					std::array<bool, kDirectAtlasMaskCount> seenMasks = {};
					for (const DirectAtlasGlyphLayer& layer : record.layers)
					{
						if (!layer.valid())
							continue;
						const size_t maskIndex =
							static_cast<size_t>(layer.maskType);
						if (layer.reserved
							|| maskIndex >= seenMasks.size()
							|| seenMasks[maskIndex]
							|| std::find(masks.begin(), masks.end(),
								static_cast<GlyphMaskType>(
									layer.maskType)) == masks.end()
							|| !ValidateDirectLayer(layer,
								static_cast<GlyphMaskType>(
									layer.maskType), pages))
						{
							complete = false;
							break;
						}
						seenMasks[maskIndex] = true;
						++layers;
					}
					for (GlyphMaskType mask : masks)
					{
						if (knownEmpty)
							break;
						if (!FindDirectLayer(record, mask))
						{
							complete = false;
							break;
						}
					}
					if (knownEmpty && layers)
						complete = false;
					if (!complete)
						return false;
					++resolvedGlyphs;
					resolvedLayers += layers;
				}
				table.glyphs = std::move(records);
			}
			if (resolvedGlyphs != header.resolvedGlyphs
				|| resolvedLayers != header.resolvedLayers
				|| !resolvedGlyphs)
			{
				return false;
			}
			table.faceIndices = std::move(faceIndices);
			table.resolvedGlyphs = resolvedGlyphs;
			table.resolvedLayers = resolvedLayers;
			return true;
		}

		struct DirectLayoutRoleRecords
		{
			std::vector<FontLetter> metrics;
			std::vector<UInt8> valid;
			UInt64 profileIdentity = 0;
		};

		bool ValidateStockDirectLayoutLetter(const FontLetter& letter,
			UInt32 pageCount)
		{
			if (!IsFiniteDirectMetrics(letter.fWidth, letter.fHeight,
				letter.fLeadingEdge, letter.fSpacing, letter.fTopEdge))
			{
				return false;
			}
			if (letter.iTextureIndex == -1 || letter.iTextureIndex == -2)
				return true;
			if (letter.iTextureIndex < 0
				|| static_cast<UInt32>(letter.iTextureIndex) >= pageCount)
			{
				return false;
			}
			for (const UVMap& uv : letter.pMapping)
			{
				if (!std::isfinite(uv.fU) || !std::isfinite(uv.fV)
					|| uv.fU < 0.0f || uv.fU > 1.0f
					|| uv.fV < 0.0f || uv.fV > 1.0f)
				{
					return false;
				}
			}
			return true;
		}

		bool ReadDirectLayoutRoleFile(const std::wstring& path,
			const wchar_t* fileName, RuntimeFont& runtime,
			VectorFontByteClass byteClass, const AtlasCacheKey& key,
			const std::vector<GlyphMaskType>& masks,
			DirectLayoutRoleRecords& result)
		{
			HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return false;

			DirectCachedLetterFileHeader header;
			const UInt8 magic[8] =
				{ 'T', 'N', 'V', 'F', 'D', 'I', 'R', '1' };
			const size_t slotCount = GetDirectGlyphSlotCount(byteClass);
			const DirectCachedLetterKind recordKind =
				masks.size() == 1 && masks[0] == GlyphMaskType::Composite
				? DirectCachedLetterKind::StockFontLetter
				: DirectCachedLetterKind::EffectLayers;
			const size_t recordSize =
				recordKind == DirectCachedLetterKind::StockFontLetter
				? sizeof(FontLetter) : sizeof(DirectCachedLetter);
			const UInt64 layoutIdentity =
				GetRuntimeDirectRoleLayoutIdentity(runtime, byteClass);
			const UInt64 effectIdentity =
				GetRuntimeMaskContentHash(runtime, byteClass);
			const UInt8 effectLayerMask =
				BuildDirectEffectLayerMask(masks);
			bool valid = ReadDirectFile(file, &header, sizeof(header))
				&& std::memcmp(header.magic, magic, sizeof(magic)) == 0
				&& header.version == kDirectCachedLetterVersion
				&& header.headerSize == sizeof(header)
				&& header.atlasSnapshotVersion == kAtlasSnapshotVersion
				&& header.manifestVersion == kPersistentGlyphManifestVersion
				&& header.manifestIdentityVersion
					== kPersistentGlyphManifestCacheIdentityVersion
				&& header.byteClass == static_cast<UInt8>(byteClass)
				&& header.pixelMode == static_cast<UInt8>(key.pixelMode)
				&& header.renderMode == static_cast<UInt8>(key.renderMode)
				&& header.recordKind == static_cast<UInt8>(recordKind)
				&& header.effectLayerMask == effectLayerMask
				&& !header.reserved
				&& header.codePage == GetFreeTypeTextCodePage()
				&& header.scaleMilli == key.scaleMilli
				&& header.padding == key.padding
				&& header.slotCount == slotCount
				&& header.recordSize == recordSize
				&& header.auxiliaryRecordSize == sizeof(UInt8)
				&& header.pageCount
				&& header.pageCount <= kMaximumAtlasSnapshotPages
				&& header.resolvedGlyphs
				&& header.profileIdentity
				&& header.layoutIdentity == layoutIdentity
				&& header.effectIdentity == effectIdentity
				&& header.atlasIdentity == key.atlasContentHash
				&& header.pageIdentityChecksum
				&& header.checksum == HashDirectBytes(&header,
					offsetof(DirectCachedLetterFileHeader, checksum));
			wchar_t expectedName[128] = {};
			if (valid)
			{
				_snwprintf_s(expectedName, _countof(expectedName), _TRUNCATE,
					L"shared_%016llX_%c.tnvfdirect",
					static_cast<unsigned long long>(header.profileIdentity),
					byteClass == VectorFontByteClass::DoubleByte ? L'd' : L's');
				valid = fileName && _wcsicmp(fileName, expectedName) == 0;
			}

			std::vector<UInt8> recordBytes;
			std::vector<UInt8> faceIndices;
			if (valid)
			{
				try
				{
					recordBytes.resize(slotCount * recordSize);
					faceIndices.resize(slotCount);
				}
				catch (const std::bad_alloc&)
				{
					valid = false;
				}
			}
			if (valid)
			{
				valid = ReadDirectFile(file, recordBytes.data(),
						recordBytes.size())
					&& header.recordsChecksum == HashDirectBytes(
						recordBytes.data(), recordBytes.size())
					&& ReadDirectFile(file, faceIndices.data(),
						faceIndices.size())
					&& header.auxiliaryChecksum == HashDirectBytes(
						faceIndices.data(), faceIndices.size());
			}
			LARGE_INTEGER fileSize = {};
			if (valid)
			{
				const UInt64 expected = sizeof(header)
					+ static_cast<UInt64>(recordBytes.size())
					+ static_cast<UInt64>(faceIndices.size());
				valid = GetFileSizeEx(file, &fileSize)
					&& static_cast<UInt64>(fileSize.QuadPart) == expected;
			}
			CloseHandle(file);
			if (!valid)
				return false;

			const size_t faceCount =
				GetRuntimeDirectFaceCount(runtime, byteClass);
			if (!faceCount)
				return false;
			DirectLayoutRoleRecords loaded;
			try
			{
				loaded.metrics.resize(slotCount);
				loaded.valid.assign(slotCount, 0);
			}
			catch (const std::bad_alloc&)
			{
				return false;
			}

			UInt32 resolvedGlyphs = 0;
			UInt32 resolvedLayers = 0;
			if (recordKind == DirectCachedLetterKind::StockFontLetter)
			{
				for (size_t slot = 0; slot < slotCount; ++slot)
				{
					FontLetter record;
					std::memcpy(&record,
						recordBytes.data() + slot * sizeof(record),
						sizeof(record));
					if (!ValidateStockDirectLayoutLetter(
							record, header.pageCount))
					{
						return false;
					}
					if (record.iTextureIndex == -1)
						continue;
					if (record.iTextureIndex == -2)
						NormalizeKnownEmptyAdvance(record);
					if (faceIndices[slot] >= faceCount)
					{
						return false;
					}
					loaded.metrics[slot] = record;
					// This table is a provisional layout source. The sealed profile
					// owns the real atlas page and UV identity.
					loaded.metrics[slot].iTextureIndex = 0;
					loaded.valid[slot] = 1;
					++resolvedGlyphs;
					if (record.iTextureIndex >= 0)
						++resolvedLayers;
				}
			}
			else
			{
				for (size_t slot = 0; slot < slotCount; ++slot)
				{
					DirectCachedLetter record;
					std::memcpy(&record,
						recordBytes.data() + slot * sizeof(record),
						sizeof(record));
					if (!record.flags)
						continue;
					if ((record.flags & ~kDirectCachedLetterKnownFlags)
						|| !(record.flags & kDirectCachedLetterValid)
						|| record.encodedCode
							!= EncodeDirectGlyphSlot(byteClass, slot)
						|| record.byteClass != static_cast<UInt8>(byteClass)
						|| !IsFiniteDirectMetrics(record.width,
							record.height, record.leadingEdge,
							record.spacing, record.topEdge)
						|| faceIndices[slot] >= faceCount)
					{
						return false;
					}
					const bool knownEmpty =
						(record.flags & kDirectCachedLetterKnownEmpty) != 0;
					if (knownEmpty)
						NormalizeKnownEmptyAdvance(record);
					std::array<bool, kDirectAtlasMaskCount> seenMasks = {};
					UInt32 layers = 0;
					for (const DirectAtlasGlyphLayer& layer : record.layers)
					{
						if (!layer.valid())
							continue;
						const size_t maskIndex =
							static_cast<size_t>(layer.maskType);
						if (layer.reserved
							|| layer.pageSlot >= header.pageCount
							|| maskIndex >= seenMasks.size()
							|| seenMasks[maskIndex]
							|| std::find(masks.begin(), masks.end(),
								static_cast<GlyphMaskType>(
									layer.maskType)) == masks.end())
						{
							return false;
						}
						seenMasks[maskIndex] = true;
						++layers;
					}
					if ((knownEmpty && layers)
						|| (!knownEmpty
							&& std::any_of(masks.begin(), masks.end(),
								[&](GlyphMaskType mask)
								{
									return !seenMasks[
										static_cast<size_t>(mask)];
								})))
					{
						return false;
					}
					FontLetter metrics = {};
					metrics.iTextureIndex = 0;
					metrics.fWidth = record.width;
					metrics.fLeadingEdge = record.leadingEdge;
					metrics.fHeight = record.height;
					metrics.fTopEdge = record.topEdge;
					metrics.fSpacing = record.spacing;
					loaded.metrics[slot] = metrics;
					loaded.valid[slot] = 1;
					++resolvedGlyphs;
					resolvedLayers += layers;
				}
			}
			if (resolvedGlyphs != header.resolvedGlyphs
				|| resolvedLayers != header.resolvedLayers)
			{
				return false;
			}
			loaded.profileIdentity = header.profileIdentity;
			result = std::move(loaded);
			MarkFreeTypeFontCacheFileUsed(path);
			return true;
		}

		bool TryLoadDirectLayoutRole(RuntimeFont& runtime,
			VectorFontByteClass byteClass, float rasterScale,
			DirectLayoutRoleRecords& result)
		{
			const FontConfig& config = GetRuntimeConfig(runtime);
			AtlasCacheKey key;
			if (!ResolvePrewarmAtlasKey(config, byteClass,
					rasterScale, key))
			{
				return false;
			}
			const std::vector<GlyphMaskType> masks =
				GetDirectProfileMasks(config, key);
			if (masks.empty())
				return false;
			std::wstring directory;
			if (!GetFreeTypeFontCacheDirectory(directory))
				return false;
			const wchar_t suffix =
				byteClass == VectorFontByteClass::DoubleByte ? L'd' : L's';
			std::wstring pattern = directory
				+ L"\\shared_*_" + suffix + L".tnvfdirect";
			WIN32_FIND_DATAW found = {};
			HANDLE search = FindFirstFileW(pattern.c_str(), &found);
			if (search == INVALID_HANDLE_VALUE)
				return false;
			bool loaded = false;
			do
			{
				if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					continue;
				const std::wstring path =
					directory + L"\\" + found.cFileName;
				if (ReadDirectLayoutRoleFile(path, found.cFileName,
					runtime, byteClass, key, masks, result))
				{
					loaded = true;
					break;
				}
			} while (FindNextFileW(search, &found));
			FindClose(search);
			return loaded;
		}

		bool SaveDirectCachedLetters(const std::wstring& path,
			UInt64 identity, VectorFontByteClass byteClass,
			const AtlasCacheKey& key,
			const std::vector<std::shared_ptr<AtlasResource>>& pages,
			const std::vector<GlyphMaskType>& masks,
			const DirectAtlasGlyphTable& table)
		{
			const size_t slotCount = table.SlotCount();
			if (path.empty() || !slotCount
				|| table.faceIndices.size() != slotCount)
				return false;
			const void* records = table.recordKind
				== DirectCachedLetterKind::StockFontLetter
				? static_cast<const void*>(table.stockGlyphs.data())
				: static_cast<const void*>(table.glyphs.data());
			const size_t recordSize = table.recordKind
				== DirectCachedLetterKind::StockFontLetter
				? sizeof(FontLetter) : sizeof(DirectCachedLetter);
			const size_t recordsBytes = slotCount * recordSize;
			DirectCachedLetterFileHeader header;
			const UInt8 magic[8] =
				{ 'T', 'N', 'V', 'F', 'D', 'I', 'R', '1' };
			std::memcpy(header.magic, magic, sizeof(magic));
			header.version = kDirectCachedLetterVersion;
			header.headerSize = sizeof(header);
			header.atlasSnapshotVersion = kAtlasSnapshotVersion;
			header.manifestVersion = kPersistentGlyphManifestVersion;
			header.manifestIdentityVersion =
				kPersistentGlyphManifestCacheIdentityVersion;
			header.byteClass = static_cast<UInt8>(byteClass);
			header.pixelMode = static_cast<UInt8>(key.pixelMode);
			header.renderMode = static_cast<UInt8>(key.renderMode);
			header.recordKind = static_cast<UInt8>(table.recordKind);
			header.effectLayerMask = table.effectLayerMask;
			header.codePage = table.codePage;
			header.scaleMilli = key.scaleMilli;
			header.padding = key.padding;
			header.slotCount = static_cast<UInt32>(slotCount);
			header.recordSize = static_cast<UInt32>(recordSize);
			header.auxiliaryRecordSize = sizeof(UInt8);
			header.pageCount = static_cast<UInt32>(pages.size());
			header.resolvedGlyphs = table.resolvedGlyphs;
			header.resolvedLayers = table.resolvedLayers;
			header.profileIdentity = identity;
			header.layoutIdentity = table.layoutIdentity;
			header.effectIdentity = table.effectIdentity;
			header.atlasIdentity = key.atlasContentHash;
			header.pageIdentityChecksum = table.pageIdentityChecksum;
			header.recordsChecksum = HashDirectBytes(records, recordsBytes);
			header.auxiliaryChecksum =
				HashDirectBytes(table.faceIndices.data(),
					table.faceIndices.size());
			header.checksum = HashDirectBytes(&header,
				offsetof(DirectCachedLetterFileHeader, checksum));
			const std::wstring temporary = path + L".tmp";
			HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0,
				nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return false;
			const bool written = WriteDirectFile(file, &header, sizeof(header))
				&& WriteDirectFile(file, records, recordsBytes)
				&& WriteDirectFile(file, table.faceIndices.data(),
					table.faceIndices.size())
				&& FlushFileBuffers(file);
			CloseHandle(file);
			DirectAtlasGlyphTable verified;
			if (written)
			{
				verified.byteClass = table.byteClass;
				verified.recordKind = table.recordKind;
				verified.effectLayerMask = table.effectLayerMask;
				verified.codePage = table.codePage;
				verified.layoutIdentity = table.layoutIdentity;
				verified.effectIdentity = table.effectIdentity;
				verified.atlasIdentity = table.atlasIdentity;
				verified.pageIdentityChecksum =
					table.pageIdentityChecksum;
				if (table.recordKind
					== DirectCachedLetterKind::StockFontLetter)
				{
					verified.stockGlyphs.resize(slotCount);
				}
				else
					verified.glyphs.resize(slotCount);
				verified.faceIndices.resize(slotCount);
			}
			const bool verifiedWrite = written
				&& TryLoadDirectCachedLetters(temporary, identity,
					byteClass, key, pages, masks, verified);
			if (!verifiedWrite
				|| !MoveFileExW(temporary.c_str(), path.c_str(),
					MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				DeleteFileW(temporary.c_str());
				return false;
			}
			return true;
		}

	}
	bool TryApplyDirectCachedLayoutMetrics(RuntimeFont& runtime, Font& font,
		float rasterScale,
		std::shared_ptr<DirectExtraGlyphTable>& codePageMetrics)
	{
		codePageMetrics.reset();
		if (!font.pFontData || !std::isfinite(rasterScale)
			|| rasterScale <= 0.0f)
		{
			return false;
		}

		DirectLayoutRoleRecords singleByte;
		if (!TryLoadDirectLayoutRole(runtime,
			VectorFontByteClass::SingleByte, rasterScale, singleByte)
			|| singleByte.metrics.size() != 256
			|| singleByte.valid.size() != 256)
		{
			return false;
		}
		for (UInt32 value = 0x20; value <= 0xFF; ++value)
		{
			if (value != 0x7F && !singleByte.valid[value])
				return false;
		}

		DirectLayoutRoleRecords doubleByte;
		std::shared_ptr<DirectExtraGlyphTable> loadedCodePageMetrics;
		if (UsesDbcsTextLayout())
		{
			if (!TryLoadDirectLayoutRole(runtime,
				VectorFontByteClass::DoubleByte, rasterScale, doubleByte)
				|| doubleByte.metrics.size()
					!= DirectExtraGlyphTable::kIndexCount
				|| doubleByte.valid.size()
					!= DirectExtraGlyphTable::kIndexCount)
			{
				return false;
			}
			try
			{
				loadedCodePageMetrics =
					std::make_shared<DirectExtraGlyphTable>();
			}
			catch (const std::bad_alloc&)
			{
				return false;
			}
			const size_t validCount = static_cast<size_t>(std::count(
				doubleByte.valid.begin(), doubleByte.valid.end(), UInt8{ 1 }));
			if (!validCount
				|| !loadedCodePageMetrics->Initialize(validCount))
				return false;
			for (size_t slot = 0; slot < doubleByte.valid.size(); ++slot)
			{
				if (!doubleByte.valid[slot])
					continue;
				if (!loadedCodePageMetrics->Insert(
					EncodeDirectGlyphSlot(
						VectorFontByteClass::DoubleByte, slot),
					doubleByte.metrics[slot]))
				{
					return false;
				}
			}
		}

		for (UInt32 value = 0x20; value <= 0xFF; ++value)
		{
			if (value == 0x7F)
				continue;
			font.pFontData->pFontLetters[value] =
				singleByte.metrics[value];
		}
		codePageMetrics = std::move(loadedCodePageMetrics);
		if (g_bEnableFreeTypeFontRenderingLog)
		{
			FreeTypeFontDebugLog(
				"tnvse_freetype_font: direct v4 layout restored font=%u scale=%.3f profiles=%016llX/%016llX dbcsGlyphs=%u manifest=unopened",
				GetRuntimeConfig(runtime).fontId, rasterScale,
				static_cast<unsigned long long>(
					singleByte.profileIdentity),
				static_cast<unsigned long long>(
					doubleByte.profileIdentity),
				codePageMetrics
					? static_cast<UInt32>(
						codePageMetrics->metrics.size()) : 0);
		}
		return true;
	}

	PersistentCacheCleanupClass ClassifyDirectCachedLetterCacheForCleanup(
		const std::wstring& path)
	{
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return PersistentCacheCleanupClass::Invalid;
		DirectCachedLetterFileHeader header;
		const bool read = ReadDirectFile(file, &header, sizeof(header));
		CloseHandle(file);
		const UInt8 magic[8] =
			{ 'T', 'N', 'V', 'F', 'D', 'I', 'R', '1' };
		if (!read
			|| std::memcmp(header.magic, magic, sizeof(magic)) != 0
			|| header.version != kDirectCachedLetterVersion
			|| header.headerSize != sizeof(header)
			|| header.atlasSnapshotVersion != kAtlasSnapshotVersion
			|| header.manifestVersion != kPersistentGlyphManifestVersion
			|| header.manifestIdentityVersion
				!= kPersistentGlyphManifestCacheIdentityVersion
			|| header.pixelMode
				> static_cast<UInt8>(AtlasPixelMode::Mtsdf32)
			|| header.renderMode
				> static_cast<UInt8>(AtlasRenderMode::ShaderEffects)
			|| header.checksum != HashDirectBytes(&header,
				offsetof(DirectCachedLetterFileHeader, checksum)))
		{
			return PersistentCacheCleanupClass::Invalid;
		}

		const FontAtlasRoute route = GetPersistentFontCacheRoute();
		if (header.renderMode == static_cast<UInt8>(
			AtlasRenderMode::CpuEffects))
		{
			const bool aggressive = header.pixelMode
				== static_cast<UInt8>(AtlasPixelMode::Argb32);
			const bool fallback = header.pixelMode
				== static_cast<UInt8>(AtlasPixelMode::A8);
			if (!aggressive && !fallback)
				return PersistentCacheCleanupClass::Invalid;
			return (aggressive
						&& route == FontAtlasRoute::ShaderA8Coverage)
					|| (fallback
						&& route == FontAtlasRoute::ArgbFallback)
				? PersistentCacheCleanupClass::Neutral
				: PersistentCacheCleanupClass::InactiveDistanceField;
		}

		DistanceFieldMethod method;
		if (header.pixelMode == static_cast<UInt8>(AtlasPixelMode::A8))
			method = DistanceFieldMethod::TrueSdf;
		else if (header.pixelMode
			== static_cast<UInt8>(AtlasPixelMode::Mtsdf32))
			method = DistanceFieldMethod::Mtsdf;
		else
			return PersistentCacheCleanupClass::Invalid;
		return route == FontAtlasRoute::ShaderDistanceField
				&& method == GetConfiguredDistanceFieldMethod()
			? PersistentCacheCleanupClass::CurrentDistanceField
			: PersistentCacheCleanupClass::InactiveDistanceField;
	}
}

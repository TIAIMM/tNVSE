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

namespace fonthook::vectorfont
{
	namespace
	{
		constexpr UInt32 kFirstLeadByte = 0x81;
		constexpr UInt32 kLastLeadByte = 0xFE;
		constexpr UInt32 kFirstTrailByte = 0x40;
		constexpr UInt32 kLastTrailByte = 0xFE;
		constexpr UInt32 kGlyphsPerDoubleByteRow =
			kLastTrailByte - kFirstTrailByte + 1;
		constexpr UInt32 kDoubleByteGlyphSlots =
			(kLastLeadByte - kFirstLeadByte + 1) * kGlyphsPerDoubleByteRow;
		constexpr size_t kDirectBuildBatchGlyphs = 512;
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

		UInt64 HashDirectBytes(const void* data, size_t size,
			UInt64 hash = 1469598103934665603ull)
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
			hash = HashDirectBytes(&kAtlasSnapshotVersion,
				sizeof(kAtlasSnapshotVersion), hash);
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
				GetRuntimeDirectLayoutIdentity(runtime);
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

		bool IsSpaceCodePoint(UInt32 codePoint)
		{
			return codePoint == 0x20 || codePoint == 0xA0
				|| codePoint == 0x1680
				|| (codePoint >= 0x2000 && codePoint <= 0x200A)
				|| codePoint == 0x202F || codePoint == 0x205F
				|| codePoint == 0x3000;
		}

		bool ConvertCompositeTableToStockLetters(
			DirectAtlasGlyphTable& table,
			const std::vector<std::shared_ptr<AtlasResource>>& pages,
			float rasterScale)
		{
			if (table.recordKind
					!= DirectCachedLetterKind::StockFontLetter
				|| table.glyphs.empty()
				|| !std::isfinite(rasterScale)
				|| rasterScale <= 0.0f)
			{
				return false;
			}
			table.stockGlyphs.assign(table.glyphs.size(), {});
			for (FontLetter& letter : table.stockGlyphs)
				letter.iTextureIndex = -1;
			for (size_t slot = 0; slot < table.glyphs.size(); ++slot)
			{
				const DirectCachedLetter& source = table.glyphs[slot];
				if (!(source.flags & kDirectCachedLetterValid))
					continue;
				FontLetter& target = table.stockGlyphs[slot];
				if (source.flags & kDirectCachedLetterKnownEmpty)
				{
					target.iTextureIndex = -2;
					target.fWidth = source.width;
					target.fHeight = source.height;
					target.fLeadingEdge = source.leadingEdge;
					target.fSpacing = source.spacing;
					target.fTopEdge = source.topEdge;
					NormalizeKnownEmptyAdvance(target);
					continue;
				}
				const DirectAtlasGlyphLayer* layer =
					FindDirectLayer(source, GlyphMaskType::Composite);
				if (!layer || layer->pageSlot >= pages.size())
					return false;
				const auto& page = pages[layer->pageSlot];
				if (!page || !page->compactSnapshot
					|| layer->snapshotPlacementIndex
						>= page->compactSnapshot->placements.size())
				{
					return false;
				}
				const AtlasSnapshotPlacement& placement =
					page->compactSnapshot->placements[
						layer->snapshotPlacementIndex];
				if (placement.maskType != static_cast<UInt8>(
						GlyphMaskType::Composite)
					|| !IsValidAtlasSnapshotGlyphPlacement(placement,
						page->width, page->height,
						page->compactSnapshot->sourceHeader.pageIndex))
				{
					return false;
				}
				const float layoutAdvance = source.leadingEdge
					+ source.width
					+ (source.width > 0.0f ? source.spacing : 0.0f);
				target.iTextureIndex = layer->pageSlot;
				target.fWidth =
					static_cast<float>(placement.rect.width)
						/ rasterScale;
				target.fHeight =
					static_cast<float>(placement.rect.height)
						/ rasterScale;
				target.fLeadingEdge =
					static_cast<float>(placement.left) / rasterScale;
				target.fSpacing = layoutAdvance
					- target.fLeadingEdge - target.fWidth;
				target.fTopEdge =
					static_cast<float>(placement.top) / rasterScale;
				target.pMapping[0] = {
					placement.glyphPlacement.u0,
					placement.glyphPlacement.v0 };
				target.pMapping[1] = {
					placement.glyphPlacement.u1,
					placement.glyphPlacement.v0 };
				target.pMapping[2] = {
					placement.glyphPlacement.u1,
					placement.glyphPlacement.v1 };
				target.pMapping[3] = {
					placement.glyphPlacement.u0,
					placement.glyphPlacement.v1 };
			}
			table.glyphs.clear();
			table.glyphs.shrink_to_fit();
			return true;
		}

		size_t GetDirectGlyphSlotCount(VectorFontByteClass byteClass)
		{
			return byteClass == VectorFontByteClass::DoubleByte
				? kDoubleByteGlyphSlots : 256u;
		}

		bool ResolveDirectGlyphSlot(VectorFontByteClass byteClass,
			UInt32 encodedCode, size_t& slot)
		{
			if (byteClass == VectorFontByteClass::SingleByte)
			{
				if (encodedCode > 0xFFu)
					return false;
				slot = encodedCode;
				return true;
			}
			if (encodedCode & 0xFFFF0000u)
				return false;
			const UInt32 lead = (encodedCode >> 8) & 0xFFu;
			const UInt32 trail = encodedCode & 0xFFu;
			if (lead < kFirstLeadByte || lead > kLastLeadByte
				|| trail < kFirstTrailByte || trail > kLastTrailByte)
			{
				return false;
			}
			slot = static_cast<size_t>(lead - kFirstLeadByte)
				* kGlyphsPerDoubleByteRow + (trail - kFirstTrailByte);
			return true;
		}

		UInt32 EncodeDirectGlyphSlot(VectorFontByteClass byteClass, size_t slot)
		{
			if (byteClass == VectorFontByteClass::SingleByte)
				return static_cast<UInt32>(slot);
			const UInt32 lead = kFirstLeadByte
				+ static_cast<UInt32>(slot / kGlyphsPerDoubleByteRow);
			const UInt32 trail = kFirstTrailByte
				+ static_cast<UInt32>(slot % kGlyphsPerDoubleByteRow);
			return (lead << 8) | trail;
		}

		std::vector<GlyphMaskType> GetDirectProfileMasks(
			const FontConfig& config, const AtlasCacheKey& key)
		{
			std::vector<GlyphMaskType> masks;
			if (key.renderMode == AtlasRenderMode::ShaderEffects)
			{
				masks.push_back(GlyphMaskType::DistanceField);
				return masks;
			}
			if (g_bEnableFreeTypeFontAggressivePerformanceMode)
			{
				masks.push_back(GlyphMaskType::Composite);
				return masks;
			}
			masks.push_back(GlyphMaskType::Fill);
			if (config.outline.enabled)
				masks.push_back(GlyphMaskType::Outline);
			if (config.glow.enabled)
				masks.push_back(GlyphMaskType::Glow);
			if (config.shadow.enabled)
				masks.push_back(GlyphMaskType::Shadow);
			return masks;
		}

		bool ResolveDirectLayerLocked(AtlasState& state,
			const AtlasCacheKey& baseKey, AtlasProfileIndex& profile,
			const std::vector<UInt16>& pageIndices, UInt64 cacheId,
			DirectAtlasGlyphLayer& layer)
		{
			if (!EnsureAtlasProfileIndexLocked(
				state, baseKey, profile))
			{
				return false;
			}
			const auto resident = profile.residentPages.find(cacheId);
			if (resident == profile.residentPages.end())
				return false;
			const auto pageSlot = std::lower_bound(pageIndices.begin(),
				pageIndices.end(), resident->second);
			if (pageSlot == pageIndices.end() || *pageSlot != resident->second)
				return false;

			AtlasCacheKey pageKey = baseKey;
			pageKey.pageIndex = resident->second;
			const auto page = state.atlasCache.find(pageKey);
			if (page == state.atlasCache.end() || !page->second.resource)
				return false;
			AtlasGlyphRecord* glyph =
				FindAtlasGlyph(*page->second.resource, cacheId);
			if (!glyph || !page->second.resource->compactSnapshot
				|| glyph->snapshotPlacementIndex == kNoSnapshotPlacement
				|| glyph->snapshotPlacementIndex
					>= page->second.resource->compactSnapshot->placements.size())
				return false;
			const AtlasSnapshotPlacement& snapshot =
				page->second.resource->compactSnapshot->placements[
					glyph->snapshotPlacementIndex];
			if (snapshot.cacheId != cacheId
				|| std::memcmp(&snapshot.rect, &glyph->rect,
					sizeof(snapshot.rect)) != 0)
			{
				return false;
			}
			if (!IsValidAtlasSnapshotGlyphPlacement(snapshot,
				page->second.resource->width,
				page->second.resource->height, resident->second))
			{
				return false;
			}

			layer.pageSlot = static_cast<UInt16>(
				pageSlot - pageIndices.begin());
			layer.maskType = snapshot.maskType;
			layer.reserved = 0;
			layer.snapshotPlacementIndex = glyph->snapshotPlacementIndex;
			return true;
		}

		bool ResolveDirectGlyphSource(
			const DirectAtlasGlyphTable& table,
			const VectorEncodedGlyph& glyph, GlyphMaskType maskType,
			PendingQuad::GlyphSource& result)
		{
			size_t glyphSlot = 0;
			if (!ResolveDirectGlyphSlot(glyph.byteClass,
				glyph.encodedCode, glyphSlot)
				|| glyphSlot >= table.glyphs.size())
				return false;
			const DirectCachedLetter& letter = table.glyphs[glyphSlot];
			if (!(letter.flags & kDirectCachedLetterValid)
				|| (letter.flags & kDirectCachedLetterKnownEmpty)
				|| letter.encodedCode != glyph.encodedCode
				|| letter.byteClass != static_cast<UInt8>(glyph.byteClass))
				return false;
			const DirectAtlasGlyphLayer* direct =
				FindDirectLayer(letter, maskType);
			if (!direct || direct->pageSlot >= table.pages.size())
				return false;
			std::shared_ptr<AtlasResource> page =
				table.pages[direct->pageSlot].lock();
			if (!page || !page->compactSnapshot
				|| direct->snapshotPlacementIndex
					>= page->compactSnapshot->placements.size())
				return false;
			const AtlasSnapshotPlacement& snapshot =
				page->compactSnapshot->placements[
					direct->snapshotPlacementIndex];
			if (snapshot.maskType != direct->maskType
				|| !page->pageContentHash
				|| page->compactSnapshot->sourceHeader.pageContentHash
					!= page->pageContentHash
				|| !IsValidAtlasSnapshotGlyphPlacement(snapshot,
					page->width, page->height,
					page->compactSnapshot->sourceHeader.pageIndex))
				return false;

			result = {};
			result.atlas = std::move(page);
			result.directCacheId = snapshot.cacheId;
			result.directWidth = static_cast<SInt32>(snapshot.rect.width);
			result.directHeight = static_cast<SInt32>(snapshot.rect.height);
			result.directLeft = snapshot.left;
			result.directTop = snapshot.top;
			result.directMaskType = snapshot.maskType;
			result.directSdfSpread = snapshot.sdfSpread;
			result.placement.atlasIdentity =
				reinterpret_cast<uintptr_t>(result.atlas.get());
			result.placement.atlasGeneration = result.atlas->generation;
			result.placement.atlasWidth = result.atlas->width;
			result.placement.atlasHeight = result.atlas->height;
			result.placement.pageIndex = direct->pageSlot;
			result.placement.inverseWidth =
				1.0f / static_cast<float>(result.atlas->width);
			result.placement.inverseHeight =
				1.0f / static_cast<float>(result.atlas->height);
			result.placement.u0 = snapshot.glyphPlacement.u0;
			result.placement.v0 = snapshot.glyphPlacement.v0;
			result.placement.u1 = snapshot.glyphPlacement.u1;
			result.placement.v1 = snapshot.glyphPlacement.v1;
			return true;
		}

		bool BuildDirectGlyphAtlasTableRole(RuntimeFont& runtime,
			VectorFontByteClass byteClass, float rasterScale)
		{
			const FontConfig& config = GetRuntimeConfig(runtime);
			AtlasCacheKey baseKey;
			if (!ResolvePrewarmAtlasKey(config, byteClass, rasterScale, baseKey))
				return false;
			const AtlasProfileKey profileKey = MakeAtlasProfileKey(baseKey);

			std::vector<UInt16> pageIndices;
			std::vector<std::shared_ptr<AtlasResource>> pages;
			{
				AtlasState& state = State();
				std::lock_guard<std::mutex> lock(state.atlasMutex);
				const auto profile = state.atlasProfiles.find(profileKey);
				if (profile == state.atlasProfiles.end()
					|| state.completeAtlasProfiles.find(profileKey)
						== state.completeAtlasProfiles.end())
				{
					return false;
				}
				if (profile->second.directGlyphs)
					return true;
				pageIndices = profile->second.pages;
				pages.reserve(pageIndices.size());
				for (UInt16 pageIndex : pageIndices)
				{
					AtlasCacheKey pageKey = baseKey;
					pageKey.pageIndex = pageIndex;
					const auto page = state.atlasCache.find(pageKey);
					if (page == state.atlasCache.end() || !page->second.resource)
						return false;
					pages.push_back(page->second.resource);
				}
			}
			if (pages.empty())
				return false;

			const std::vector<GlyphMaskType> masks =
				GetDirectProfileMasks(config, baseKey);
			if (masks.empty())
				return false;
			auto table = std::make_shared<DirectAtlasGlyphTable>();
			table->byteClass = byteClass;
			table->recordKind =
				masks.size() == 1
					&& masks[0] == GlyphMaskType::Composite
				? DirectCachedLetterKind::StockFontLetter
				: DirectCachedLetterKind::EffectLayers;
			table->effectLayerMask =
				BuildDirectEffectLayerMask(masks);
			table->codePage = GetFreeTypeTextCodePage();
			table->layoutIdentity =
				GetRuntimeDirectLayoutIdentity(runtime);
			table->effectIdentity =
				GetRuntimeMaskContentHash(runtime, byteClass);
			table->atlasIdentity = baseKey.atlasContentHash;
			table->pageIdentityChecksum =
				BuildDirectPageIdentityChecksum(pages);
			const size_t directSlotCount =
				GetDirectGlyphSlotCount(byteClass);
			table->glyphs.resize(directSlotCount);
			if (table->recordKind
				== DirectCachedLetterKind::StockFontLetter)
			{
				table->stockGlyphs.resize(directSlotCount);
			}
			table->faceIndices.resize(directSlotCount);
			table->pages.reserve(pages.size());
			for (const auto& page : pages)
				table->pages.push_back(page);

			const UInt64 directIdentity =
				BuildDirectProfileIdentity(baseKey, *table, pages);
			table->profileIdentity = directIdentity;
			const std::wstring directPath =
				GetDirectCachedLetterPath(directIdentity, byteClass);
			if (TryLoadDirectCachedLetters(directPath, directIdentity,
				byteClass, baseKey, pages, masks, *table))
			{
				table->cpuMemory.Reset(CpuMemoryCategory::AtlasMetadata,
					GetDirectTableStorageBytes(*table));
				AtlasState& state = State();
				std::lock_guard<std::mutex> lock(state.atlasMutex);
				const auto profile = state.atlasProfiles.find(profileKey);
				if (profile == state.atlasProfiles.end()
					|| profile->second.pages != pageIndices
					|| state.completeAtlasProfiles.find(profileKey)
						== state.completeAtlasProfiles.end())
					return false;
				profile->second.directGlyphs = table;
				gLog.FormattedMessage(
					"tnvse_freetype_font: direct cached-letter table restored font=%u role=%s glyphs=%u layers=%u pages=%u bytes=%llu",
					config.fontId,
					byteClass == VectorFontByteClass::DoubleByte
						? "doubleByte" : "singleByte",
					table->resolvedGlyphs, table->resolvedLayers,
					static_cast<UInt32>(table->pages.size()),
					static_cast<unsigned long long>(
						GetDirectTableStorageBytes(*table)));
				return true;
			}
			UInt32 distanceFieldSpread = 0;
			if (baseKey.renderMode == AtlasRenderMode::ShaderEffects)
			{
				MtsdfSharedRasterProfile profile;
				if (!ResolveMtsdfSharedRasterProfile(config, byteClass,
					rasterScale, true, profile))
				{
					return false;
				}
				distanceFieldSpread = profile.sdfSpread;
			}

			std::vector<VectorEncodedGlyph> glyphs;
			std::vector<GlyphBitmapRequest> requests;
			std::vector<UInt64> cacheIds;
			glyphs.reserve(kDirectBuildBatchGlyphs);
			requests.reserve(kDirectBuildBatchGlyphs * masks.size());
			const size_t slotCount = table->glyphs.size();
			for (size_t batchStart = 0; batchStart < slotCount;
				batchStart += kDirectBuildBatchGlyphs)
			{
				const size_t batchEnd = std::min(slotCount,
					batchStart + kDirectBuildBatchGlyphs);
				glyphs.clear();
				requests.clear();
				for (size_t slot = batchStart; slot < batchEnd; ++slot)
				{
					VectorEncodedGlyph glyph;
					const UInt32 encodedCode =
						EncodeDirectGlyphSlot(byteClass, slot);
					FontLetter metrics = {};
					if (!LoadGlyphManifest(runtime, encodedCode,
						byteClass, &glyph, &metrics))
					{
						continue;
					}
					DirectCachedLetter& direct = table->glyphs[slot];
					direct.encodedCode =
						static_cast<UInt16>(encodedCode);
					direct.flags = kDirectCachedLetterValid
						| (IsSpaceCodePoint(glyph.codePoint)
							? kDirectCachedLetterKnownEmpty : 0);
					direct.byteClass =
						static_cast<UInt8>(byteClass);
					direct.width = metrics.fWidth;
					direct.leadingEdge = metrics.fLeadingEdge;
					direct.height = metrics.fHeight;
					direct.topEdge = metrics.fTopEdge;
					direct.spacing = metrics.fSpacing;
					if (direct.flags & kDirectCachedLetterKnownEmpty)
						NormalizeKnownEmptyAdvance(direct);
					if (slot < table->faceIndices.size())
						table->faceIndices[slot] =
							static_cast<UInt8>(std::min<UInt16>(
								glyph.faceIndex,
								std::numeric_limits<UInt8>::max()));
					glyphs.push_back(glyph);
				}
				for (const VectorEncodedGlyph& glyph : glyphs)
				{
					for (GlyphMaskType mask : masks)
					{
						requests.push_back({ &glyph, mask,
							mask == GlyphMaskType::DistanceField
								? distanceFieldSpread : 0 });
					}
				}
				if (requests.empty())
					continue;
				ResolveGlyphBitmapCacheIds(runtime, requests, rasterScale, cacheIds);
				if (cacheIds.size() != requests.size())
					return false;

				AtlasState& state = State();
				std::lock_guard<std::mutex> lock(state.atlasMutex);
				const auto profile = state.atlasProfiles.find(profileKey);
				if (profile == state.atlasProfiles.end()
					|| profile->second.pages != pageIndices
					|| state.completeAtlasProfiles.find(profileKey)
						== state.completeAtlasProfiles.end())
				{
					return false;
				}
				for (size_t requestIndex = 0;
					requestIndex < requests.size(); ++requestIndex)
				{
					const GlyphBitmapRequest& request = requests[requestIndex];
					if (!request.glyph || !cacheIds[requestIndex])
						continue;
					size_t glyphSlot = 0;
					if (!ResolveDirectGlyphSlot(byteClass,
						request.glyph->encodedCode, glyphSlot)
						|| glyphSlot >= table->glyphs.size())
					{
						continue;
					}
					DirectAtlasGlyphLayer* layer =
						FindOrCreateDirectLayer(
							table->glyphs[glyphSlot], request.maskType);
					if (!layer)
						continue;
					ResolveDirectLayerLocked(state, baseKey, profile->second,
						pageIndices, cacheIds[requestIndex],
						*layer);
				}
			}

			for (const DirectAtlasGlyphRecord& glyph : table->glyphs)
			{
				if (!(glyph.flags & kDirectCachedLetterValid))
					continue;
				const bool knownEmpty =
					(glyph.flags & kDirectCachedLetterKnownEmpty) != 0;
				bool complete = true;
				UInt32 layers = 0;
				for (GlyphMaskType mask : masks)
				{
					if (knownEmpty)
						break;
					const DirectAtlasGlyphLayer* layer =
						FindDirectLayer(glyph, mask);
					if (!layer)
					{
						complete = false;
						break;
					}
					++layers;
				}
				if (complete)
				{
					++table->resolvedGlyphs;
					table->resolvedLayers += layers;
				}
				else
				return false;
			}
			if (!table->resolvedGlyphs)
				return false;
			if (table->recordKind
					== DirectCachedLetterKind::StockFontLetter
				&& !ConvertCompositeTableToStockLetters(
					*table, pages, rasterScale))
			{
				return false;
			}
			if (!SaveDirectCachedLetters(directPath,
				directIdentity, byteClass, baseKey,
				pages, masks, *table))
			{
				return false;
			}

			table->cpuMemory.Reset(CpuMemoryCategory::AtlasMetadata,
				GetDirectTableStorageBytes(*table));
			{
				AtlasState& state = State();
				std::lock_guard<std::mutex> lock(state.atlasMutex);
				const auto profile = state.atlasProfiles.find(profileKey);
				if (profile == state.atlasProfiles.end()
					|| profile->second.pages != pageIndices
					|| state.completeAtlasProfiles.find(profileKey)
						== state.completeAtlasProfiles.end())
				{
					return false;
				}
				profile->second.directGlyphs = table;
			}
			gLog.FormattedMessage(
				"tnvse_freetype_font: direct atlas glyph table font=%u role=%s glyphs=%u layers=%u pages=%u bytes=%llu",
				config.fontId,
				byteClass == VectorFontByteClass::DoubleByte
					? "doubleByte" : "singleByte",
				table->resolvedGlyphs, table->resolvedLayers,
				static_cast<UInt32>(table->pages.size()),
				static_cast<unsigned long long>(
					GetDirectTableStorageBytes(*table)));
			return true;
		}

		bool ValidateSealedDirectTable(
			const DirectAtlasGlyphTable& table,
			const SealedDirectFontProfile& sealed, size_t roleIndex)
		{
			if (!table.SlotCount()
				|| table.faceIndices.size() != table.SlotCount()
				|| roleIndex >= sealed.pageOrdinals.size()
				|| roleIndex >= sealed.faceBaselineOffsets.size()
				|| sealed.faceBaselineOffsets[roleIndex].empty()
				|| std::any_of(table.faceIndices.begin(),
					table.faceIndices.end(),
					[&](UInt8 faceIndex)
					{
						return faceIndex
							>= sealed.faceBaselineOffsets[roleIndex].size();
					}))
			{
				return false;
			}
			if (table.recordKind
				== DirectCachedLetterKind::StockFontLetter)
			{
				for (const FontLetter& letter : table.stockGlyphs)
				{
					if (letter.iTextureIndex < 0)
						continue;
					if (letter.iTextureIndex
							>= kMaximumAtlasSnapshotPages
						|| sealed.pageOrdinals[roleIndex][
							letter.iTextureIndex]
							== kInvalidDirectAtlasPageSlot)
					{
						return false;
					}
				}
			}
			else
			{
				for (const DirectCachedLetter& letter : table.glyphs)
				{
					if (!(letter.flags & kDirectCachedLetterValid)
						|| (letter.flags & kDirectCachedLetterKnownEmpty))
					{
						continue;
					}
					for (const DirectAtlasGlyphLayer& layer :
						letter.layers)
					{
						if (!layer.valid())
							continue;
						if (layer.pageSlot
								>= kMaximumAtlasSnapshotPages
							|| sealed.pageOrdinals[roleIndex][
								layer.pageSlot]
								== kInvalidDirectAtlasPageSlot)
						{
							return false;
						}
					}
				}
			}
			return true;
		}

		bool IsSealedDirectProfileValid(
			const SealedDirectFontProfile& sealed)
		{
			bool foundTable = false;
			for (const auto& table : sealed.tables)
			{
				if (!table)
					continue;
				foundTable = true;
				if (!table->validity
					|| !table->validity->load(
						std::memory_order_acquire))
				{
					return false;
				}
			}
			return foundTable;
		}

		bool PublishSealedDirectFontProfile(RuntimeFont& runtime,
			float rasterScale)
		{
			const FontConfig& config = GetRuntimeConfig(runtime);
			AtlasState& state = State();
			std::shared_ptr<const SealedDirectFontProfile> published;
			{
				std::lock_guard<std::mutex> lock(state.atlasMutex);
				auto sealed =
					std::make_shared<SealedDirectFontProfile>();
				sealed->validityEpoch = state.directProfileEpoch.load(
					std::memory_order_relaxed);
				sealed->layoutIdentity =
					GetRuntimeDirectLayoutIdentity(runtime);
				sealed->scaleMilli = static_cast<UInt32>(std::lround(
					rasterScale * 1000.0f));
				sealed->codePage = GetFreeTypeTextCodePage();
				const size_t roleCount = UsesDbcsTextLayout() ? 2u : 1u;
				bool commonProfileInitialized = false;
				for (size_t roleIndex = 0; roleIndex < roleCount;
					++roleIndex)
				{
					const VectorFontByteClass byteClass =
						static_cast<VectorFontByteClass>(roleIndex);
					AtlasCacheKey key;
					if (!ResolvePrewarmAtlasKey(config, byteClass,
						rasterScale, key))
					{
						return false;
					}
					const AtlasProfileKey profileKey =
						MakeAtlasProfileKey(key);
					const auto profile =
						state.atlasProfiles.find(profileKey);
					if (profile == state.atlasProfiles.end()
						|| state.completeAtlasProfiles.find(profileKey)
							== state.completeAtlasProfiles.end()
						|| !profile->second.directGlyphs)
					{
						return false;
					}
					const auto& table = profile->second.directGlyphs;
					if (table->codePage != sealed->codePage
						|| table->layoutIdentity
							!= sealed->layoutIdentity
						|| table->byteClass != byteClass
						|| table->pages.size()
							!= profile->second.pages.size()
						|| table->pages.size()
							> kMaximumAtlasSnapshotPages)
					{
						return false;
					}
					if (!commonProfileInitialized)
					{
						sealed->pixelMode = key.pixelMode;
						sealed->renderMode = key.renderMode;
						sealed->padding = key.padding;
						sealed->recordKind = table->recordKind;
						sealed->effectLayerMask =
							table->effectLayerMask;
						commonProfileInitialized = true;
					}
					else if (sealed->pixelMode != key.pixelMode
						|| sealed->renderMode != key.renderMode
						|| sealed->padding != key.padding
						|| sealed->recordKind != table->recordKind
						|| sealed->effectLayerMask
							!= table->effectLayerMask)
					{
						return false;
					}
					sealed->tables[roleIndex] = table;
					for (size_t pageSlot = 0;
						pageSlot < table->pages.size(); ++pageSlot)
					{
						std::shared_ptr<AtlasResource> page =
							table->pages[pageSlot].lock();
						if (!page || !page->property
							|| !GetAtlasTexture(*page)
							|| !page->compactSnapshot
							|| !page->pageContentHash
							|| page->pixelMode != key.pixelMode
							|| page->renderMode != key.renderMode
							|| page->padding != key.padding
							|| page->levelZeroOnly
								!= key.levelZeroOnly
							|| page->compactSnapshot->sourceHeader
								.pageContentHash
								!= page->pageContentHash)
						{
							return false;
						}
						UInt16 ordinal =
							kInvalidDirectAtlasPageSlot;
						for (UInt16 candidate = 0;
							candidate < sealed->atlases.size();
							++candidate)
						{
							if (sealed->atlases[candidate].get()
									== page.get()
								|| AreAtlasResourcesBackedBySameTexture(
									*sealed->atlases[candidate], *page))
							{
								ordinal = candidate;
								break;
							}
						}
						if (ordinal == kInvalidDirectAtlasPageSlot)
						{
							if (sealed->atlases.size()
								>= kMaximumAtlasSnapshotPages)
							{
								return false;
							}
							ordinal = static_cast<UInt16>(
								sealed->atlases.size());
							sealed->atlases.push_back(
								std::move(page));
						}
						sealed->pageOrdinals[roleIndex][pageSlot]
							= ordinal;
					}
					GetRuntimeDirectBaselineOffsets(runtime,
						byteClass,
						sealed->roleBaselineOffsets[roleIndex],
						sealed->faceBaselineOffsets[roleIndex]);
					if (!ValidateSealedDirectTable(
						*table, *sealed, roleIndex))
					{
						return false;
					}
				}
				UInt64 identity = HashDirectBytes(
					&sealed->validityEpoch,
					sizeof(sealed->validityEpoch));
				identity = HashDirectBytes(&sealed->layoutIdentity,
					sizeof(sealed->layoutIdentity), identity);
				identity = HashDirectBytes(&sealed->scaleMilli,
					sizeof(sealed->scaleMilli), identity);
				identity = HashDirectBytes(&sealed->codePage,
					sizeof(sealed->codePage), identity);
				for (const auto& table : sealed->tables)
				{
					if (!table)
						continue;
					identity = HashDirectBytes(
						&table->profileIdentity,
						sizeof(table->profileIdentity), identity);
				}
				sealed->identity = identity ? identity : 1;
				const auto shared =
					state.sealedDirectProfiles.find(
						sealed->identity);
				if (shared != state.sealedDirectProfiles.end())
				{
					published = shared->second.lock();
					if (published
						&& published->validityEpoch
							!= sealed->validityEpoch)
					{
						published.reset();
					}
					if (published
						&& !IsSealedDirectProfileValid(*published))
					{
						published.reset();
					}
				}
				if (!published)
				{
					size_t bytes = sizeof(SealedDirectFontProfile)
						+ sealed->atlases.capacity()
							* sizeof(std::shared_ptr<AtlasResource>);
					for (const auto& baselines :
						sealed->faceBaselineOffsets)
					{
						bytes += baselines.capacity()
							* sizeof(float);
					}
					sealed->cpuMemory.Reset(
						CpuMemoryCategory::AtlasMetadata, bytes);
					published = sealed;
					state.sealedDirectProfiles[
						sealed->identity] = published;
				}
				ReleaseSealedAtlasCpuIndexesLocked(
					state, *published);
			}
			StoreRuntimeSealedDirectProfile(runtime, published);
			state.directProfilesAvailable.store(
				true, std::memory_order_release);
			gLog.FormattedMessage(
				"tnvse_freetype_font: sealed direct profile font=%u identity=%016llX kind=%u glyphSlots=%u/%u pages=%u epoch=%u",
				config.fontId,
				static_cast<unsigned long long>(published->identity),
				static_cast<UInt32>(published->recordKind),
				published->tables[0]
					? static_cast<UInt32>(
						published->tables[0]->SlotCount()) : 0,
				published->tables[1]
					? static_cast<UInt32>(
						published->tables[1]->SlotCount()) : 0,
				static_cast<UInt32>(published->atlases.size()),
				published->validityEpoch);
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

	bool BuildDirectGlyphAtlasTables(RuntimeFont& runtime, float rasterScale)
	{
		const bool single = BuildDirectGlyphAtlasTableRole(runtime,
			VectorFontByteClass::SingleByte, rasterScale);
		const bool doubleByte = !UsesDbcsTextLayout()
			|| BuildDirectGlyphAtlasTableRole(runtime,
				VectorFontByteClass::DoubleByte, rasterScale);
		const bool complete = single && doubleByte;
		if (!complete
			|| !PublishSealedDirectFontProfile(runtime, rasterScale))
		{
			InvalidateSealedDirectFontProfile(runtime);
			return false;
		}
		ReleaseSealedRuntimeFreeTypeState(runtime);
		return true;
	}

	void InvalidateSealedDirectFontProfile(RuntimeFont& runtime)
	{
		StoreRuntimeSealedDirectProfile(runtime, {});
	}

	std::shared_ptr<const SealedDirectFontProfile>
		AcquireSealedDirectLayoutProfile(RuntimeFont& runtime)
	{
		std::shared_ptr<const SealedDirectFontProfile> sealed =
			LoadRuntimeSealedDirectProfile(runtime);
		if (!sealed)
			return {};
		AtlasState& state = State();
		if (sealed->validityEpoch
				!= state.directProfileEpoch.load(
					std::memory_order_acquire)
			|| sealed->layoutIdentity
				!= GetRuntimeDirectLayoutIdentity(runtime)
			|| sealed->codePage != GetFreeTypeTextCodePage()
			|| !IsSealedDirectProfileValid(*sealed))
		{
			InvalidateSealedDirectFontProfile(runtime);
			return {};
		}
		return sealed;
	}

	std::shared_ptr<const SealedDirectFontProfile>
		AcquireSealedDirectFontProfile(RuntimeFont& runtime,
			float rasterScale)
	{
		std::shared_ptr<const SealedDirectFontProfile> sealed =
			AcquireSealedDirectLayoutProfile(runtime);
		if (!sealed)
			return {};
		const UInt32 scaleMilli =
			std::isfinite(rasterScale) && rasterScale > 0.0f
				? static_cast<UInt32>(std::lround(
					rasterScale * 1000.0f))
				: 0;
		return scaleMilli && sealed->scaleMilli == scaleMilli
			? sealed : std::shared_ptr<const SealedDirectFontProfile>();
	}

	SealedDirectGlyphLookup DecodeSealedDirectGlyph(
		const SealedDirectFontProfile& sealed,
		const char* text, VectorEncodedGlyph& glyph)
	{
		glyph = {};
		if (!text || !*text)
			return SealedDirectGlyphLookup::Invalid;
		UInt32 encodedCode = 0;
		if (text[1]
			&& TryDecodeFreeTypeDoubleByte(text, encodedCode))
		{
			glyph.byteLength = 2;
			glyph.byteClass =
				VectorFontByteClass::DoubleByte;
			glyph.codePoint = 0x10000u;
		}
		else
		{
			encodedCode = static_cast<UInt8>(text[0]);
			glyph.byteLength = 1;
			glyph.byteClass =
				VectorFontByteClass::SingleByte;
			glyph.codePoint = encodedCode;
		}
		glyph.encodedCode = encodedCode;
		const size_t roleIndex =
			static_cast<size_t>(glyph.byteClass);
		if (roleIndex >= sealed.tables.size()
			|| !sealed.tables[roleIndex])
		{
			return SealedDirectGlyphLookup::Invalid;
		}
		const DirectAtlasGlyphTable& table =
			*sealed.tables[roleIndex];
		size_t slot = 0;
		if (!ResolveDirectGlyphSlot(glyph.byteClass,
				encodedCode, slot)
			|| slot >= table.SlotCount()
			|| slot > std::numeric_limits<UInt16>::max())
		{
			return SealedDirectGlyphLookup::Invalid;
		}
		glyph.directSlot = static_cast<UInt16>(slot);
		if (slot >= table.faceIndices.size())
			return SealedDirectGlyphLookup::Invalid;
		glyph.faceIndex = table.faceIndices[slot];
		if (table.recordKind
			== DirectCachedLetterKind::StockFontLetter)
		{
			const FontLetter& letter = table.stockGlyphs[slot];
			if (letter.iTextureIndex == -1)
				return SealedDirectGlyphLookup::Unavailable;
			if (letter.iTextureIndex < -2)
				return SealedDirectGlyphLookup::Invalid;
			glyph.directWidth = letter.fWidth;
			glyph.directHeight = letter.fHeight;
			glyph.directLeadingEdge = letter.fLeadingEdge;
			glyph.directSpacing = letter.fSpacing;
			glyph.directTopEdge = letter.fTopEdge;
		}
		else
		{
			const DirectCachedLetter& letter =
				table.glyphs[slot];
			if (!letter.flags)
				return SealedDirectGlyphLookup::Unavailable;
			if (!(letter.flags & kDirectCachedLetterValid)
				|| letter.encodedCode != encodedCode
				|| letter.byteClass
					!= static_cast<UInt8>(glyph.byteClass))
			{
				return SealedDirectGlyphLookup::Invalid;
			}
			glyph.directWidth = letter.width;
			glyph.directHeight = letter.height;
			glyph.directLeadingEdge = letter.leadingEdge;
			glyph.directSpacing = letter.spacing;
			glyph.directTopEdge = letter.topEdge;
		}
		const auto& baselines =
			sealed.faceBaselineOffsets[roleIndex];
		glyph.directBaselineOffset =
			glyph.faceIndex < baselines.size()
				? baselines[glyph.faceIndex]
				: sealed.roleBaselineOffsets[roleIndex];
		glyph.hasDirectMetrics = true;
		return SealedDirectGlyphLookup::Resolved;
	}

	SealedDirectGlyphLookup DecodeSealedDirectGlyph(RuntimeFont& runtime,
		const char* text, VectorEncodedGlyph& glyph)
	{
		glyph = {};
		if (!text || !*text)
			return SealedDirectGlyphLookup::Invalid;
		std::shared_ptr<const SealedDirectFontProfile> sealed =
			LoadRuntimeSealedDirectProfile(runtime);
		if (!sealed)
			return SealedDirectGlyphLookup::Unavailable;
		AtlasState& state = State();
		if (!state.directProfilesAvailable.load(
			std::memory_order_acquire))
		{
			// During a DEFAULT-pool reset the immutable owner is deliberately
			// retained while GPU publication is unavailable. Do not reopen FT.
			return SealedDirectGlyphLookup::Invalid;
		}
		if (sealed->validityEpoch
				!= state.directProfileEpoch.load(
					std::memory_order_acquire)
			|| sealed->layoutIdentity
				!= GetRuntimeDirectLayoutIdentity(runtime)
			|| sealed->codePage != GetFreeTypeTextCodePage()
			|| !IsSealedDirectProfileValid(*sealed))
		{
			InvalidateSealedDirectFontProfile(runtime);
			return SealedDirectGlyphLookup::Unavailable;
		}
		const SealedDirectGlyphLookup result =
			DecodeSealedDirectGlyph(*sealed, text, glyph);
		if (result == SealedDirectGlyphLookup::Invalid)
		{
			InvalidateSealedDirectFontProfile(runtime);
		}
		return result;
	}

	bool GetSealedDirectAtlasGlyphBatch(RuntimeFont& runtime,
		const std::shared_ptr<const SealedDirectFontProfile>& sealed,
		const std::vector<DirectGlyphCommand>& glyphs,
		GlyphMaskType maskType, float rasterScale,
		AtlasPixelMode pixelMode, AtlasRenderMode renderMode,
		UInt32 padding, DirectAtlasGlyphBatch& result)
	{
		result.Clear();
		if (!sealed)
			return false;
		if (glyphs.empty())
		{
			result.sealed = sealed;
			result.tables = sealed->tables;
			return true;
		}
		const size_t maskIndex = static_cast<size_t>(maskType);
		if (maskIndex >= kDirectAtlasMaskCount)
			return false;

		AtlasState& state = State();
		const std::shared_ptr<const SealedDirectFontProfile> published =
			LoadRuntimeSealedDirectProfile(runtime);
		const UInt32 scaleMilli =
			std::isfinite(rasterScale) && rasterScale > 0.0f
				? static_cast<UInt32>(std::lround(
					rasterScale * 1000.0f))
				: 0;
		if (published.get() != sealed.get()
			|| sealed->validityEpoch
				!= state.directProfileEpoch.load(
					std::memory_order_acquire)
			|| !state.directProfilesAvailable.load(
				std::memory_order_acquire)
			|| !IsSealedDirectProfileValid(*sealed)
			|| sealed->layoutIdentity
				!= GetRuntimeDirectLayoutIdentity(runtime)
			|| sealed->scaleMilli != scaleMilli
			|| sealed->pixelMode != pixelMode
			|| sealed->renderMode != renderMode
			|| sealed->padding != padding
			|| sealed->codePage != GetFreeTypeTextCodePage())
		{
			if (published.get() == sealed.get())
				InvalidateSealedDirectFontProfile(runtime);
			return false;
		}
		const UInt8 requestedMask = static_cast<UInt8>(
			1u << static_cast<UInt8>(maskType));
		if (!(sealed->effectLayerMask & requestedMask))
			return false;

		result.sealed = sealed;
		result.tables = sealed->tables;
		result.glyphs.resize(glyphs.size());
		for (size_t glyphIndex = 0;
			glyphIndex < glyphs.size(); ++glyphIndex)
		{
			const DirectGlyphCommand& glyph = glyphs[glyphIndex];
			const size_t roleIndex = glyph.byteClass;
			if (roleIndex >= sealed->tables.size()
				|| !sealed->tables[roleIndex]
				|| !glyph.byteLength
				|| glyph.directSlot
					== std::numeric_limits<UInt16>::max())
			{
				InvalidateSealedDirectFontProfile(runtime);
				result.Clear();
				return false;
			}
			const DirectAtlasGlyphTable& table =
				*sealed->tables[roleIndex];
			size_t expectedSlot = 0;
			const VectorFontByteClass byteClass =
				static_cast<VectorFontByteClass>(roleIndex);
			if (!ResolveDirectGlyphSlot(byteClass,
					glyph.encodedCode, expectedSlot)
				|| expectedSlot != glyph.directSlot
				|| expectedSlot >= table.SlotCount())
			{
				InvalidateSealedDirectFontProfile(runtime);
				result.Clear();
				return false;
			}

			DirectAtlasBatchGlyph& output =
				result.glyphs[glyphIndex];
			output.byteClass = glyph.byteClass;
			UInt16 localPage = kInvalidDirectAtlasPageSlot;
			if (table.recordKind
				== DirectCachedLetterKind::StockFontLetter)
			{
				if (maskType != GlyphMaskType::Composite)
				{
					InvalidateSealedDirectFontProfile(runtime);
					result.Clear();
					return false;
				}
				const FontLetter& letter =
					table.stockGlyphs[expectedSlot];
				if (letter.iTextureIndex == -2)
				{
					output.knownEmpty = true;
					continue;
				}
				if (letter.iTextureIndex < 0
					|| letter.iTextureIndex
						>= kMaximumAtlasSnapshotPages)
				{
					InvalidateSealedDirectFontProfile(runtime);
					result.Clear();
					return false;
				}
				localPage =
					static_cast<UInt16>(letter.iTextureIndex);
				output.stockLetter = &letter;
			}
			else
			{
				const DirectCachedLetter& letter =
					table.glyphs[expectedSlot];
				if (!(letter.flags & kDirectCachedLetterValid)
					|| letter.encodedCode != glyph.encodedCode
					|| letter.byteClass != glyph.byteClass)
				{
					InvalidateSealedDirectFontProfile(runtime);
					result.Clear();
					return false;
				}
				if (letter.flags & kDirectCachedLetterKnownEmpty)
				{
					output.knownEmpty = true;
					continue;
				}
				const DirectAtlasGlyphLayer* layer =
					FindDirectLayer(letter, maskType);
				if (!layer || layer->pageSlot
						>= kMaximumAtlasSnapshotPages)
				{
					InvalidateSealedDirectFontProfile(runtime);
					result.Clear();
					return false;
				}
				localPage = layer->pageSlot;
				output.snapshotPlacementIndex =
					layer->snapshotPlacementIndex;
			}
			const UInt16 ordinal =
				sealed->pageOrdinals[roleIndex][localPage];
			if (ordinal >= sealed->atlases.size())
			{
				InvalidateSealedDirectFontProfile(runtime);
				result.Clear();
				return false;
			}
			output.atlasPage = ordinal;
			if (output.stockLetter)
				continue;
			const auto& page = sealed->atlases[ordinal];
			if (!page || !page->compactSnapshot
				|| output.snapshotPlacementIndex
					>= page->compactSnapshot->placements.size())
			{
				InvalidateSealedDirectFontProfile(runtime);
				result.Clear();
				return false;
			}
			const AtlasSnapshotPlacement& placement =
				page->compactSnapshot->placements[
					output.snapshotPlacementIndex];
			if (placement.maskType
				!= static_cast<UInt8>(maskType))
			{
				InvalidateSealedDirectFontProfile(runtime);
				result.Clear();
				return false;
			}
			output.placement = &placement;
		}
		RecordFreeTypePerf(
			FreeTypePerfCounter::GpuResidentGlyphHit,
			static_cast<UInt64>(glyphs.size()));
		return true;
	}

	bool GetDirectAtlasGlyphBatch(RuntimeFont& runtime,
		const std::vector<AtlasGlyphInstance>& glyphs,
		GlyphMaskType maskType, float rasterScale,
		AtlasPixelMode pixelMode, AtlasRenderMode renderMode,
		UInt32 padding, DirectAtlasGlyphBatch& result)
	{
		result.Clear();
		if (glyphs.empty())
			return true;
		const size_t maskIndex = static_cast<size_t>(maskType);
		if (maskIndex >= kDirectAtlasMaskCount)
			return false;

		std::shared_ptr<const SealedDirectFontProfile> sealed =
			LoadRuntimeSealedDirectProfile(runtime);
		AtlasState& state = State();
		const UInt32 currentEpoch = state.directProfileEpoch.load(
			std::memory_order_acquire);
		if (sealed
			&& sealed->validityEpoch == currentEpoch
			&& state.directProfilesAvailable.load(
				std::memory_order_acquire)
			&& IsSealedDirectProfileValid(*sealed)
			&& sealed->scaleMilli == static_cast<UInt32>(std::lround(
				rasterScale * 1000.0f))
			&& sealed->pixelMode == pixelMode
			&& sealed->renderMode == renderMode
			&& sealed->padding == padding
			&& sealed->codePage == GetFreeTypeTextCodePage())
		{
			const UInt8 requestedMask =
				static_cast<UInt8>(1u << static_cast<UInt8>(
					maskType));
			if (!(sealed->effectLayerMask & requestedMask))
			{
				return false;
			}
			result.sealed = sealed;
			result.tables = sealed->tables;
			result.glyphs.resize(glyphs.size());
			for (size_t glyphIndex = 0;
				glyphIndex < glyphs.size(); ++glyphIndex)
			{
				const VectorEncodedGlyph& glyph =
					glyphs[glyphIndex].glyph;
				const size_t roleIndex =
					static_cast<size_t>(glyph.byteClass);
				if (roleIndex >= sealed->tables.size()
					|| !sealed->tables[roleIndex])
				{
					InvalidateSealedDirectFontProfile(runtime);
					result.Clear();
					return false;
				}
				const DirectAtlasGlyphTable& table =
					*sealed->tables[roleIndex];
				size_t glyphSlot = glyph.directSlot;
				if (glyphSlot
						== std::numeric_limits<UInt16>::max()
					&& !ResolveDirectGlyphSlot(glyph.byteClass,
						glyph.encodedCode, glyphSlot))
				{
					InvalidateSealedDirectFontProfile(runtime);
					result.Clear();
					return false;
				}
				if (glyphSlot >= table.SlotCount())
				{
					InvalidateSealedDirectFontProfile(runtime);
					result.Clear();
					return false;
				}
				DirectAtlasBatchGlyph& output =
					result.glyphs[glyphIndex];
				output.byteClass =
					static_cast<UInt8>(glyph.byteClass);
				UInt16 localPage = kInvalidDirectAtlasPageSlot;
				if (table.recordKind
					== DirectCachedLetterKind::StockFontLetter)
				{
					if (maskType != GlyphMaskType::Composite)
					{
						InvalidateSealedDirectFontProfile(runtime);
						result.Clear();
						return false;
					}
					const FontLetter& letter =
						table.stockGlyphs[glyphSlot];
					if (letter.iTextureIndex == -2)
					{
						output.knownEmpty = true;
						continue;
					}
					if (letter.iTextureIndex < 0
						|| letter.iTextureIndex
							>= kMaximumAtlasSnapshotPages)
					{
						InvalidateSealedDirectFontProfile(runtime);
						result.Clear();
						return false;
					}
					localPage = static_cast<UInt16>(
						letter.iTextureIndex);
					output.stockLetter = &letter;
				}
				else
				{
					const DirectCachedLetter& letter =
						table.glyphs[glyphSlot];
					if (!(letter.flags & kDirectCachedLetterValid)
						|| letter.encodedCode != glyph.encodedCode
						|| letter.byteClass
							!= static_cast<UInt8>(glyph.byteClass))
					{
						InvalidateSealedDirectFontProfile(runtime);
						result.Clear();
						return false;
					}
					if (letter.flags & kDirectCachedLetterKnownEmpty)
					{
						output.knownEmpty = true;
						continue;
					}
					const DirectAtlasGlyphLayer* layer =
						FindDirectLayer(letter, maskType);
					if (!layer || layer->pageSlot
							>= kMaximumAtlasSnapshotPages)
					{
						InvalidateSealedDirectFontProfile(runtime);
						result.Clear();
						return false;
					}
					localPage = layer->pageSlot;
					output.snapshotPlacementIndex =
						layer->snapshotPlacementIndex;
				}
				const UInt16 ordinal =
					sealed->pageOrdinals[roleIndex][localPage];
				if (ordinal >= sealed->atlases.size())
				{
					InvalidateSealedDirectFontProfile(runtime);
					result.Clear();
					return false;
				}
				output.atlasPage = ordinal;
				if (output.stockLetter)
					continue;
				const auto& page = sealed->atlases[ordinal];
				if (!page || !page->compactSnapshot
					|| output.snapshotPlacementIndex
						>= page->compactSnapshot->placements.size())
				{
					InvalidateSealedDirectFontProfile(runtime);
					result.Clear();
					return false;
				}
				const AtlasSnapshotPlacement& placement =
					page->compactSnapshot->placements[
						output.snapshotPlacementIndex];
				if (placement.maskType
					!= static_cast<UInt8>(maskType))
				{
					InvalidateSealedDirectFontProfile(runtime);
					result.Clear();
					return false;
				}
				output.placement = &placement;
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::GpuResidentGlyphHit,
				static_cast<UInt64>(glyphs.size()));
			return true;
		}
		if (sealed)
		{
			InvalidateSealedDirectFontProfile(runtime);
			return false;
		}

		std::array<bool, 2> roleUsed = {};
		for (const AtlasGlyphInstance& instance : glyphs)
		{
			const size_t roleIndex =
				static_cast<size_t>(instance.glyph.byteClass);
			if (roleIndex >= roleUsed.size())
				return false;
			roleUsed[roleIndex] = true;
		}

		const FontConfig& config = GetRuntimeConfig(runtime);
		std::array<AtlasCacheKey, 2> baseKeys;
		std::array<std::array<bool, kMaximumAtlasSnapshotPages>, 2>
			usedPages = {};
		std::array<std::array<UInt16, kMaximumAtlasSnapshotPages>, 2>
			pageOrdinals;
		for (auto& role : pageOrdinals)
			role.fill(kInvalidDirectAtlasPageSlot);

		std::lock_guard<std::mutex> lock(state.atlasMutex);
		auto fail = [&](const char* stage)
		{
			result.Clear();
			const UInt64 logKey = 0x4000000000000000ull
				^ (static_cast<UInt64>(config.fontId) << 32)
				^ (static_cast<UInt64>(std::lround(
					rasterScale * 1000.0f)) << 8)
				^ (static_cast<UInt64>(pixelMode) << 4)
				^ static_cast<UInt64>(renderMode);
			if (g_bEnableFreeTypeFontRenderingLog
				&& state.loggedDirectGlyphBatches.insert(logKey).second)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: direct atlas geometry batch unavailable font=%u sourceScale=%.3f mode=%u render=%u stage=%s",
					config.fontId, rasterScale,
					static_cast<UInt32>(pixelMode),
					static_cast<UInt32>(renderMode), stage);
			}
			return false;
		};

		for (size_t roleIndex = 0; roleIndex < roleUsed.size(); ++roleIndex)
		{
			if (!roleUsed[roleIndex])
				continue;
			const VectorFontByteClass byteClass =
				static_cast<VectorFontByteClass>(roleIndex);
			if (!ResolvePrewarmAtlasKey(config, byteClass,
					rasterScale, baseKeys[roleIndex])
				|| baseKeys[roleIndex].pixelMode != pixelMode
				|| baseKeys[roleIndex].renderMode != renderMode
				|| baseKeys[roleIndex].padding != padding)
			{
				return fail("profile-key");
			}
			const AtlasProfileKey profileKey =
				MakeAtlasProfileKey(baseKeys[roleIndex]);
			const auto profile = state.atlasProfiles.find(profileKey);
			if (profile == state.atlasProfiles.end()
				|| state.completeAtlasProfiles.find(profileKey)
					== state.completeAtlasProfiles.end()
				|| !profile->second.directGlyphs)
			{
				return fail("profile-table");
			}
			result.tables[roleIndex] = profile->second.directGlyphs;
		}

		result.glyphs.resize(glyphs.size());
		for (size_t glyphIndex = 0; glyphIndex < glyphs.size(); ++glyphIndex)
		{
			const VectorEncodedGlyph& glyph = glyphs[glyphIndex].glyph;
			const size_t roleIndex = static_cast<size_t>(glyph.byteClass);
			if (roleIndex >= result.tables.size()
				|| !result.tables[roleIndex])
			{
				return fail("table-role");
			}
			const DirectAtlasGlyphTable& table =
				*result.tables[roleIndex];
			size_t glyphSlot = 0;
			if (!ResolveDirectGlyphSlot(glyph.byteClass,
					glyph.encodedCode, glyphSlot)
				|| glyphSlot >= table.glyphs.size())
			{
				return fail("encoded-slot");
			}
			const DirectCachedLetter& letter = table.glyphs[glyphSlot];
			if (!(letter.flags & kDirectCachedLetterValid)
				|| letter.encodedCode != glyph.encodedCode
				|| letter.byteClass != static_cast<UInt8>(glyph.byteClass))
			{
				return fail("letter-identity");
			}

			DirectAtlasBatchGlyph& output = result.glyphs[glyphIndex];
			output.byteClass = static_cast<UInt8>(glyph.byteClass);
			if (letter.flags & kDirectCachedLetterKnownEmpty)
			{
				if (!IsSpaceCodePoint(glyph.codePoint))
					return fail("known-empty");
				output.knownEmpty = true;
				continue;
			}

			const DirectAtlasGlyphLayer* layer =
				FindDirectLayer(letter, maskType);
			if (!layer || layer->reserved
				|| layer->pageSlot >= table.pages.size()
				|| layer->pageSlot >= kMaximumAtlasSnapshotPages)
			{
				return fail("direct-layer");
			}
			usedPages[roleIndex][layer->pageSlot] = true;
			output.snapshotPlacementIndex =
				layer->snapshotPlacementIndex;
			// This is a role-local page until the deterministic page pass below.
			output.atlasPage = layer->pageSlot;
		}

		for (size_t roleIndex = 0; roleIndex < roleUsed.size(); ++roleIndex)
		{
			if (!roleUsed[roleIndex])
				continue;
			const DirectAtlasGlyphTable& table =
				*result.tables[roleIndex];
			for (UInt16 pageSlot = 0;
				pageSlot < kMaximumAtlasSnapshotPages; ++pageSlot)
			{
				if (!usedPages[roleIndex][pageSlot])
					continue;
				if (pageSlot >= table.pages.size())
					return fail("page-slot");
				std::shared_ptr<AtlasResource> page =
					table.pages[pageSlot].lock();
				const AtlasCacheKey& key = baseKeys[roleIndex];
				if (!page || !page->compactSnapshot
					|| !page->pageContentHash
					|| page->compactSnapshot->sourceHeader.pageContentHash
						!= page->pageContentHash
					|| page->pixelMode != key.pixelMode
					|| page->renderMode != key.renderMode
					|| page->padding != key.padding
					|| page->levelZeroOnly != key.levelZeroOnly
					|| !page->width || !page->height || !page->property
					|| !GetAtlasTexture(*page))
				{
					return fail("page-resource");
				}

				UInt16 ordinal = kInvalidDirectAtlasPageSlot;
				for (UInt16 candidate = 0;
					candidate < result.atlases.size(); ++candidate)
				{
					if (result.atlases[candidate].get() == page.get())
					{
						ordinal = candidate;
						break;
					}
				}
				if (ordinal == kInvalidDirectAtlasPageSlot)
				{
					if (result.atlases.size()
						>= kMaximumAtlasSnapshotPages)
					{
						return fail("page-limit");
					}
					ordinal = static_cast<UInt16>(
						result.atlases.size());
					result.atlases.push_back(std::move(page));
				}
				pageOrdinals[roleIndex][pageSlot] = ordinal;
			}
		}

		for (DirectAtlasBatchGlyph& glyph : result.glyphs)
		{
			if (glyph.knownEmpty)
				continue;
			const size_t roleIndex = glyph.byteClass;
			if (roleIndex >= pageOrdinals.size()
				|| glyph.atlasPage >= kMaximumAtlasSnapshotPages)
			{
				return fail("page-remap-source");
			}
			const UInt16 ordinal =
				pageOrdinals[roleIndex][glyph.atlasPage];
			if (ordinal >= result.atlases.size())
				return fail("page-remap-target");
			const std::shared_ptr<AtlasResource>& page =
				result.atlases[ordinal];
			if (!page || !page->compactSnapshot
				|| glyph.snapshotPlacementIndex
					>= page->compactSnapshot->placements.size())
			{
				return fail("placement-remap-source");
			}
			const AtlasSnapshotPlacement& placement =
				page->compactSnapshot->placements[
					glyph.snapshotPlacementIndex];
			if (placement.maskType != static_cast<UInt8>(maskType)
				|| !IsValidAtlasSnapshotGlyphPlacement(placement,
					page->width, page->height,
					page->compactSnapshot->sourceHeader.pageIndex))
			{
				return fail("placement-remap-target");
			}
			glyph.atlasPage = ordinal;
			glyph.placement = &placement;
		}

		RecordFreeTypePerf(FreeTypePerfCounter::GpuResidentGlyphHit,
			static_cast<UInt64>(glyphs.size()));
		const UInt64 logKey = 0x8000000000000000ull
			^ (static_cast<UInt64>(config.fontId) << 32)
			^ (static_cast<UInt64>(std::lround(
				rasterScale * 1000.0f)) << 8)
			^ (static_cast<UInt64>(pixelMode) << 4)
			^ static_cast<UInt64>(renderMode);
		if (!result.atlases.empty()
			&& state.loggedDirectGlyphBatches.insert(logKey).second)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: direct atlas geometry batch font=%u sourceScale=%.3f mode=%u render=%u glyphs=%u pages=%u source=dense-cached-letter",
				config.fontId, rasterScale,
				static_cast<UInt32>(pixelMode),
				static_cast<UInt32>(renderMode),
				static_cast<UInt32>(glyphs.size()),
				static_cast<UInt32>(result.atlases.size()));
		}
		return true;
	}

	bool GetDirectAtlasGlyphSources(RuntimeFont& runtime,
		const std::vector<GlyphBitmapRequest>& requests, float rasterScale,
		AtlasPixelMode pixelMode, AtlasRenderMode renderMode, UInt32 padding,
		std::vector<PendingQuad::GlyphSource>& results)
	{
		results.assign(requests.size(), {});
		if (requests.empty())
			return true;

		const FontConfig& config = GetRuntimeConfig(runtime);
		std::array<AtlasCacheKey, 2> baseKeys;
		std::array<std::shared_ptr<const DirectAtlasGlyphTable>, 2> tables;
		AtlasState& state = State();
		std::lock_guard<std::mutex> lock(state.atlasMutex);
		for (size_t roleIndex = 0; roleIndex < baseKeys.size(); ++roleIndex)
		{
			const VectorFontByteClass byteClass =
				static_cast<VectorFontByteClass>(roleIndex);
			if (!ResolvePrewarmAtlasKey(config, byteClass,
				rasterScale, baseKeys[roleIndex])
				|| baseKeys[roleIndex].pixelMode != pixelMode
				|| baseKeys[roleIndex].renderMode != renderMode
				|| baseKeys[roleIndex].padding != padding)
			{
				return false;
			}
			const auto profile = state.atlasProfiles.find(
				MakeAtlasProfileKey(baseKeys[roleIndex]));
			if (profile != state.atlasProfiles.end()
				&& state.completeAtlasProfiles.find(
					MakeAtlasProfileKey(baseKeys[roleIndex]))
					!= state.completeAtlasProfiles.end())
				tables[roleIndex] = profile->second.directGlyphs;
		}

		for (size_t requestIndex = 0;
			requestIndex < requests.size(); ++requestIndex)
		{
			const GlyphBitmapRequest& request = requests[requestIndex];
			if (!request.glyph)
				return false;
			const size_t roleIndex =
				static_cast<size_t>(request.glyph->byteClass);
			if (roleIndex >= tables.size() || !tables[roleIndex])
				return false;
			const std::shared_ptr<const DirectAtlasGlyphTable>& table =
				tables[roleIndex];
			size_t glyphSlot = 0;
			if (!ResolveDirectGlyphSlot(request.glyph->byteClass,
				request.glyph->encodedCode, glyphSlot)
				|| glyphSlot >= table->glyphs.size())
			{
				return false;
			}
			if (static_cast<size_t>(request.maskType)
				>= kDirectAtlasMaskCount)
				return false;
			const DirectCachedLetter& letter =
				table->glyphs[glyphSlot];
			if (!(letter.flags & kDirectCachedLetterValid)
				|| letter.encodedCode != request.glyph->encodedCode
				|| letter.byteClass
					!= static_cast<UInt8>(request.glyph->byteClass))
			{
				return false;
			}
			if (letter.flags & kDirectCachedLetterKnownEmpty)
			{
				if (!IsSpaceCodePoint(request.glyph->codePoint))
					return false;
				results[requestIndex].knownEmpty = true;
				continue;
			}
			if (!ResolveDirectGlyphSource(*table, *request.glyph,
				request.maskType, results[requestIndex]))
				return false;
		}
		RecordFreeTypePerf(FreeTypePerfCounter::GpuResidentGlyphHit,
			static_cast<UInt64>(requests.size()));
		const UInt64 logKey =
			(static_cast<UInt64>(config.fontId) << 32)
			| (static_cast<UInt64>(std::lround(rasterScale * 1000.0f))
				<< 8)
			| (static_cast<UInt64>(pixelMode) << 4)
			| static_cast<UInt64>(renderMode);
		if (state.loggedDirectGlyphBatches.insert(logKey).second)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: direct atlas glyph batch font=%u sourceScale=%.3f mode=%u render=%u glyphs=%u source=immutable-snapshot-placement",
				config.fontId, rasterScale,
				static_cast<UInt32>(pixelMode),
				static_cast<UInt32>(renderMode),
				static_cast<UInt32>(requests.size()));
		}
		return true;
	}

	bool GetDirectAtlasGlyphSources(RuntimeFont& runtime,
		const std::vector<AtlasGlyphInstance>& glyphs,
		GlyphMaskType maskType, float rasterScale,
		AtlasPixelMode pixelMode, AtlasRenderMode renderMode,
		UInt32 padding, std::vector<PendingQuad::GlyphSource>& results)
	{
		results.assign(glyphs.size(), {});
		if (glyphs.empty())
			return true;
		if (static_cast<size_t>(maskType) >= kDirectAtlasMaskCount)
			return false;

		const FontConfig& config = GetRuntimeConfig(runtime);
		std::array<AtlasCacheKey, 2> baseKeys;
		std::array<std::shared_ptr<const DirectAtlasGlyphTable>, 2> tables;
		AtlasState& state = State();
		std::lock_guard<std::mutex> lock(state.atlasMutex);
		for (size_t roleIndex = 0; roleIndex < baseKeys.size();
			++roleIndex)
		{
			const VectorFontByteClass byteClass =
				static_cast<VectorFontByteClass>(roleIndex);
			if (!ResolvePrewarmAtlasKey(config, byteClass,
				rasterScale, baseKeys[roleIndex])
				|| baseKeys[roleIndex].pixelMode != pixelMode
				|| baseKeys[roleIndex].renderMode != renderMode
				|| baseKeys[roleIndex].padding != padding)
			{
				return false;
			}
			const auto profile = state.atlasProfiles.find(
				MakeAtlasProfileKey(baseKeys[roleIndex]));
			if (profile != state.atlasProfiles.end()
				&& state.completeAtlasProfiles.find(
					MakeAtlasProfileKey(baseKeys[roleIndex]))
					!= state.completeAtlasProfiles.end())
				tables[roleIndex] = profile->second.directGlyphs;
		}

		for (size_t glyphIndex = 0; glyphIndex < glyphs.size();
			++glyphIndex)
		{
			const VectorEncodedGlyph& glyph =
				glyphs[glyphIndex].glyph;
			const size_t roleIndex =
				static_cast<size_t>(glyph.byteClass);
			if (roleIndex >= tables.size() || !tables[roleIndex])
				return false;
			size_t glyphSlot = 0;
			if (!ResolveDirectGlyphSlot(glyph.byteClass,
				glyph.encodedCode, glyphSlot)
				|| glyphSlot >= tables[roleIndex]->glyphs.size())
				return false;
			const DirectCachedLetter& letter =
				tables[roleIndex]->glyphs[glyphSlot];
			if ((letter.flags & kDirectCachedLetterKnownEmpty)
				&& (letter.flags & kDirectCachedLetterValid))
			{
				if (!IsSpaceCodePoint(glyph.codePoint))
					return false;
				results[glyphIndex].knownEmpty = true;
				continue;
			}
			if (ResolveDirectGlyphSource(*tables[roleIndex], glyph,
				maskType, results[glyphIndex]))
				continue;
			return false;
		}
		RecordFreeTypePerf(FreeTypePerfCounter::GpuResidentGlyphHit,
			static_cast<UInt64>(glyphs.size()));
		const UInt64 logKey =
			(static_cast<UInt64>(config.fontId) << 32)
			| (static_cast<UInt64>(std::lround(
				rasterScale * 1000.0f)) << 8)
			| (static_cast<UInt64>(pixelMode) << 4)
			| static_cast<UInt64>(renderMode);
		if (state.loggedDirectGlyphBatches.insert(logKey).second)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: direct atlas glyph batch font=%u sourceScale=%.3f mode=%u render=%u glyphs=%u source=persistent-direct-cached-letter",
				config.fontId, rasterScale,
				static_cast<UInt32>(pixelMode),
				static_cast<UInt32>(renderMode),
				static_cast<UInt32>(glyphs.size()));
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

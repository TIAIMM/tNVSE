#include "font_atlas_internal.h"

#include "encoding.h"
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
		constexpr UInt32 kDirectCachedLetterVersion = 2;
		constexpr UInt8 kDirectLetterValid = 1u << 0;
		constexpr UInt8 kDirectLetterKnownEmpty = 1u << 1;
		constexpr UInt8 kDirectLetterKnownFlags =
			kDirectLetterValid | kDirectLetterKnownEmpty;

		UInt32 EncodeDirectGlyphSlot(VectorFontByteClass byteClass,
			size_t slot);

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
			UInt32 scaleMilli = 0;
			UInt32 padding = 0;
			UInt32 slotCount = 0;
			UInt32 recordSize = 0;
			UInt32 pageCount = 0;
			UInt32 resolvedGlyphs = 0;
			UInt32 resolvedLayers = 0;
			UInt64 profileIdentity = 0;
			UInt64 recordsChecksum = 0;
			UInt64 checksum = 0;
		};
#pragma pack(pop)
		static_assert(sizeof(DirectCachedLetterFileHeader) == 80);

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
				|| layer.pageSlot >= pages.size())
				return false;
			const std::shared_ptr<AtlasResource>& page =
				pages[layer.pageSlot];
			if (!page || page->pageContentHash != layer.pageContentHash
				|| !page->compactSnapshot
				|| layer.snapshotPlacementIndex
					>= page->compactSnapshot->placements.size())
				return false;
			const AtlasSnapshotPlacement& snapshot =
				page->compactSnapshot->placements[
					layer.snapshotPlacementIndex];
			constexpr float epsilon = 1.0e-6f;
			auto same = [epsilon](float left, float right)
			{
				return std::fabs(left - right) <= epsilon;
			};
			return snapshot.cacheId == layer.cacheId
				&& snapshot.maskType == layer.maskType
				&& snapshot.sdfSpread == layer.sdfSpread
				&& static_cast<SInt32>(snapshot.rect.width) == layer.width
				&& static_cast<SInt32>(snapshot.rect.height) == layer.height
				&& snapshot.left == layer.left && snapshot.top == layer.top
				&& same(snapshot.glyphPlacement.u0, layer.u0)
				&& same(snapshot.glyphPlacement.v0, layer.v0)
				&& same(snapshot.glyphPlacement.u1, layer.u1)
				&& same(snapshot.glyphPlacement.v1, layer.v1);
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
				&& header.scaleMilli == key.scaleMilli
				&& header.padding == key.padding
				&& header.slotCount == table.glyphs.size()
				&& header.recordSize == sizeof(DirectCachedLetter)
				&& header.pageCount == pages.size()
				&& header.profileIdentity == identity
				&& header.checksum == HashDirectBytes(&header,
					offsetof(DirectCachedLetterFileHeader, checksum));
			std::vector<DirectCachedLetter> records;
			if (valid)
			{
				records.resize(header.slotCount);
				valid = ReadDirectFile(file, records.data(),
					records.size() * sizeof(DirectCachedLetter))
					&& header.recordsChecksum == HashDirectBytes(
						records.data(),
						records.size() * sizeof(DirectCachedLetter));
			}
			LARGE_INTEGER fileSize = {};
			if (valid)
			{
				const UInt64 expected = sizeof(header)
					+ static_cast<UInt64>(records.size())
						* sizeof(DirectCachedLetter);
				valid = GetFileSizeEx(file, &fileSize)
					&& static_cast<UInt64>(fileSize.QuadPart) == expected;
			}
			CloseHandle(file);
			if (!valid)
				return false;

			UInt32 resolvedGlyphs = 0;
			UInt32 resolvedLayers = 0;
			for (size_t slot = 0; slot < records.size(); ++slot)
			{
				const DirectCachedLetter& record = records[slot];
				if (!record.flags)
					continue;
				if ((record.flags & ~kDirectLetterKnownFlags)
					|| !(record.flags & kDirectLetterValid)
					|| record.encodedCode != EncodeDirectGlyphSlot(
						byteClass, slot)
					|| record.byteClass != static_cast<UInt8>(byteClass))
					return false;
				const bool knownEmpty =
					(record.flags & kDirectLetterKnownEmpty) != 0;
				bool complete = true;
				UInt32 layers = 0;
				for (GlyphMaskType mask : masks)
				{
					if (knownEmpty)
						break;
					if (!ValidateDirectLayer(record.layers[
							static_cast<size_t>(mask)], mask, pages))
					{
						complete = false;
						break;
					}
					++layers;
				}
				if (complete)
				{
					++resolvedGlyphs;
					resolvedLayers += layers;
				}
			}
			if (resolvedGlyphs != header.resolvedGlyphs
				|| resolvedLayers != header.resolvedLayers
				|| !resolvedGlyphs)
				return false;
			table.glyphs = std::move(records);
			table.resolvedGlyphs = resolvedGlyphs;
			table.resolvedLayers = resolvedLayers;
			return true;
		}

		bool SaveDirectCachedLetters(const std::wstring& path,
			UInt64 identity, VectorFontByteClass byteClass,
			const AtlasCacheKey& key,
			const std::vector<std::shared_ptr<AtlasResource>>& pages,
			const DirectAtlasGlyphTable& table)
		{
			if (path.empty() || table.glyphs.empty())
				return false;
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
			header.scaleMilli = key.scaleMilli;
			header.padding = key.padding;
			header.slotCount = static_cast<UInt32>(table.glyphs.size());
			header.recordSize = sizeof(DirectCachedLetter);
			header.pageCount = static_cast<UInt32>(pages.size());
			header.resolvedGlyphs = table.resolvedGlyphs;
			header.resolvedLayers = table.resolvedLayers;
			header.profileIdentity = identity;
			header.recordsChecksum = HashDirectBytes(table.glyphs.data(),
				table.glyphs.size() * sizeof(DirectCachedLetter));
			header.checksum = HashDirectBytes(&header,
				offsetof(DirectCachedLetterFileHeader, checksum));
			const std::wstring temporary = path + L".tmp";
			HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0,
				nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return false;
			const bool written = WriteDirectFile(file, &header, sizeof(header))
				&& WriteDirectFile(file, table.glyphs.data(),
					table.glyphs.size() * sizeof(DirectCachedLetter))
				&& FlushFileBuffers(file);
			CloseHandle(file);
			if (!written || !MoveFileExW(temporary.c_str(), path.c_str(),
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
			const AtlasCacheKey& baseKey, const AtlasProfileIndex& profile,
			const std::vector<UInt16>& pageIndices, UInt64 cacheId,
			DirectAtlasGlyphLayer& layer)
		{
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
			layer.sdfSpread = snapshot.sdfSpread;
			layer.snapshotPlacementIndex = glyph->snapshotPlacementIndex;
			layer.cacheId = snapshot.cacheId;
			layer.pageContentHash =
				page->second.resource->pageContentHash;
			layer.width = static_cast<SInt32>(snapshot.rect.width);
			layer.height = static_cast<SInt32>(snapshot.rect.height);
			layer.left = snapshot.left;
			layer.top = snapshot.top;
			layer.u0 = snapshot.glyphPlacement.u0;
			layer.v0 = snapshot.glyphPlacement.v0;
			layer.u1 = snapshot.glyphPlacement.u1;
			layer.v1 = snapshot.glyphPlacement.v1;
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
			const size_t maskIndex = static_cast<size_t>(maskType);
			if (maskIndex >= kDirectAtlasMaskCount)
				return false;
			const DirectCachedLetter& letter = table.glyphs[glyphSlot];
			if (!(letter.flags & kDirectLetterValid)
				|| (letter.flags & kDirectLetterKnownEmpty)
				|| letter.encodedCode != glyph.encodedCode
				|| letter.byteClass != static_cast<UInt8>(glyph.byteClass))
				return false;
			const DirectAtlasGlyphLayer& direct =
				letter.layers[maskIndex];
			if (!direct.valid() || direct.pageSlot >= table.pages.size())
				return false;
			std::shared_ptr<AtlasResource> page =
				table.pages[direct.pageSlot].lock();
			if (!page || !page->compactSnapshot
				|| direct.snapshotPlacementIndex
					>= page->compactSnapshot->placements.size())
				return false;
			const AtlasSnapshotPlacement& snapshot =
				page->compactSnapshot->placements[
					direct.snapshotPlacementIndex];
			if (direct.maskType != static_cast<UInt8>(maskType)
				|| snapshot.maskType != direct.maskType
				|| snapshot.cacheId != direct.cacheId
				|| page->pageContentHash != direct.pageContentHash
				|| static_cast<SInt32>(snapshot.rect.width) != direct.width
				|| static_cast<SInt32>(snapshot.rect.height) != direct.height
				|| snapshot.left != direct.left || snapshot.top != direct.top
				|| !snapshot.rect.width || !snapshot.rect.height
				|| !page->width || !page->height)
				return false;

			result = {};
			result.atlas = std::move(page);
			result.directCacheId = direct.cacheId;
			result.directWidth = direct.width;
			result.directHeight = direct.height;
			result.directLeft = direct.left;
			result.directTop = direct.top;
			result.directMaskType = direct.maskType;
			result.directSdfSpread = direct.sdfSpread;
			result.placement.atlasIdentity =
				reinterpret_cast<uintptr_t>(result.atlas.get());
			result.placement.atlasGeneration = result.atlas->generation;
			result.placement.atlasWidth = result.atlas->width;
			result.placement.atlasHeight = result.atlas->height;
			result.placement.pageIndex = direct.pageSlot;
			result.placement.inverseWidth =
				1.0f / static_cast<float>(result.atlas->width);
			result.placement.inverseHeight =
				1.0f / static_cast<float>(result.atlas->height);
			result.placement.u0 = direct.u0;
			result.placement.v0 = direct.v0;
			result.placement.u1 = direct.u1;
			result.placement.v1 = direct.v1;
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

			auto table = std::make_shared<DirectAtlasGlyphTable>();
			table->byteClass = byteClass;
			table->glyphs.resize(GetDirectGlyphSlotCount(byteClass));
			table->pages.reserve(pages.size());
			for (const auto& page : pages)
				table->pages.push_back(page);

			const std::vector<GlyphMaskType> masks =
				GetDirectProfileMasks(config, baseKey);
			if (masks.empty())
				return false;
			const UInt64 directIdentity =
				BuildDirectProfileIdentity(baseKey, pages);
			const std::wstring directPath =
				GetDirectCachedLetterPath(directIdentity, byteClass);
			if (TryLoadDirectCachedLetters(directPath, directIdentity,
				byteClass, baseKey, pages, masks, *table))
			{
				table->cpuMemory.Reset(CpuMemoryCategory::AtlasMetadata,
					sizeof(DirectAtlasGlyphTable)
						+ table->pages.capacity()
							* sizeof(std::weak_ptr<AtlasResource>)
						+ table->glyphs.capacity()
							* sizeof(DirectAtlasGlyphRecord));
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
						table->glyphs.capacity()
							* sizeof(DirectAtlasGlyphRecord)));
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
					direct.flags = kDirectLetterValid
						| (IsSpaceCodePoint(glyph.codePoint)
							? kDirectLetterKnownEmpty : 0);
					direct.byteClass =
						static_cast<UInt8>(byteClass);
					direct.width = metrics.fWidth;
					direct.leadingEdge = metrics.fLeadingEdge;
					direct.height = metrics.fHeight;
					direct.topEdge = metrics.fTopEdge;
					direct.spacing = metrics.fSpacing;
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
					const size_t maskIndex =
						static_cast<size_t>(request.maskType);
					if (maskIndex >= kDirectAtlasMaskCount)
						continue;
					ResolveDirectLayerLocked(state, baseKey, profile->second,
						pageIndices, cacheIds[requestIndex],
						table->glyphs[glyphSlot].layers[maskIndex]);
				}
			}

			for (const DirectAtlasGlyphRecord& glyph : table->glyphs)
			{
				if (!(glyph.flags & kDirectLetterValid))
					continue;
				const bool knownEmpty =
					(glyph.flags & kDirectLetterKnownEmpty) != 0;
				bool complete = true;
				UInt32 layers = 0;
				for (GlyphMaskType mask : masks)
				{
					if (knownEmpty)
						break;
					const DirectAtlasGlyphLayer& layer =
						glyph.layers[static_cast<size_t>(mask)];
					if (!layer.valid())
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
			SaveDirectCachedLetters(directPath, directIdentity,
				byteClass, baseKey, pages, *table);

			table->cpuMemory.Reset(CpuMemoryCategory::AtlasMetadata,
				sizeof(DirectAtlasGlyphTable)
				+ table->pages.capacity()
					* sizeof(std::weak_ptr<AtlasResource>)
				+ table->glyphs.capacity()
					* sizeof(DirectAtlasGlyphRecord));
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
					sizeof(DirectAtlasGlyphTable)
					+ table->pages.capacity()
						* sizeof(std::weak_ptr<AtlasResource>)
					+ table->glyphs.capacity()
						* sizeof(DirectAtlasGlyphRecord)));
			return true;
		}
	}

	bool BuildDirectGlyphAtlasTables(RuntimeFont& runtime, float rasterScale)
	{
		const bool single = BuildDirectGlyphAtlasTableRole(runtime,
			VectorFontByteClass::SingleByte, rasterScale);
		const bool doubleByte = !UsesDbcsTextLayout()
			|| BuildDirectGlyphAtlasTableRole(runtime,
				VectorFontByteClass::DoubleByte, rasterScale);
		return single && doubleByte;
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
		std::array<std::array<UInt64, kMaximumAtlasSnapshotPages>, 2>
			pageContentHashes = {};
		std::array<std::array<UInt16, kMaximumAtlasSnapshotPages>, 2>
			pageOrdinals;
		for (auto& role : pageOrdinals)
			role.fill(kInvalidDirectAtlasPageSlot);

		AtlasState& state = State();
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
			if (!(letter.flags & kDirectLetterValid)
				|| letter.encodedCode != glyph.encodedCode
				|| letter.byteClass != static_cast<UInt8>(glyph.byteClass))
			{
				return fail("letter-identity");
			}

			DirectAtlasBatchGlyph& output = result.glyphs[glyphIndex];
			output.byteClass = static_cast<UInt8>(glyph.byteClass);
			if (letter.flags & kDirectLetterKnownEmpty)
			{
				if (!IsSpaceCodePoint(glyph.codePoint))
					return fail("known-empty");
				output.knownEmpty = true;
				continue;
			}

			const DirectAtlasGlyphLayer& layer =
				letter.layers[maskIndex];
			if (!layer.valid() || layer.maskType != static_cast<UInt8>(maskType)
				|| layer.pageSlot >= table.pages.size()
				|| layer.pageSlot >= kMaximumAtlasSnapshotPages
				|| !layer.pageContentHash
				|| !std::isfinite(layer.u0) || !std::isfinite(layer.v0)
				|| !std::isfinite(layer.u1) || !std::isfinite(layer.v1))
			{
				return fail("direct-layer");
			}
			bool& pageUsed = usedPages[roleIndex][layer.pageSlot];
			UInt64& pageHash =
				pageContentHashes[roleIndex][layer.pageSlot];
			if (pageUsed && pageHash != layer.pageContentHash)
				return fail("page-content-alias");
			pageUsed = true;
			pageHash = layer.pageContentHash;
			output.layer = &layer;
			// This is a role-local page until the deterministic page pass below.
			output.atlasPage = layer.pageSlot;
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
					|| page->pageContentHash
						!= pageContentHashes[roleIndex][pageSlot]
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
			if (!glyph.layer || roleIndex >= pageOrdinals.size()
				|| glyph.atlasPage >= kMaximumAtlasSnapshotPages)
			{
				return fail("page-remap-source");
			}
			const UInt16 ordinal =
				pageOrdinals[roleIndex][glyph.atlasPage];
			if (ordinal >= result.atlases.size())
				return fail("page-remap-target");
			glyph.atlasPage = ordinal;
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
			const size_t maskIndex = static_cast<size_t>(request.maskType);
			if (maskIndex >= kDirectAtlasMaskCount)
				return false;
			const DirectCachedLetter& letter =
				table->glyphs[glyphSlot];
			if (!(letter.flags & kDirectLetterValid)
				|| letter.encodedCode != request.glyph->encodedCode
				|| letter.byteClass
					!= static_cast<UInt8>(request.glyph->byteClass))
			{
				return false;
			}
			if (letter.flags & kDirectLetterKnownEmpty)
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
			if ((letter.flags & kDirectLetterKnownEmpty)
				&& (letter.flags & kDirectLetterValid))
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
}

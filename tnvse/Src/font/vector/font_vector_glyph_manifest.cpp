#include "font_freetype_internal.h"

#include "encoding.h"
#include "font_glyphs.h"
#include "globals.h"

#include <winioctl.h>

#include "font_vector_persistent_cache_internal.h"

namespace fonthook::vectorfont
{
	void RefreshGlyphManifestCpuMemory(PersistentGlyphManifest& manifest)
	{
		const size_t mappedBytes = manifest.mappedData
			? sizeof(PersistentGlyphManifestHeader)
				+ static_cast<size_t>(manifest.recordCount)
					* sizeof(PersistentGlyphManifestRecord)
			: 0;
		manifest.cpuMemory.Reset(CpuMemoryCategory::PersistentMapping,
			sizeof(PersistentGlyphManifest)
				+ manifest.path.capacity() * sizeof(wchar_t)
				+ manifest.validatedRecordIndex.capacity() * sizeof(UInt16)
				+ mappedBytes);
	}
		UInt64 HashRuntimeFontFaces(RuntimeFont& runtime, UInt64 hash)
		{
			for (const RuntimeRole& role : runtime.roles)
			{
				const UInt32 count = static_cast<UInt32>(role.faces.size());
				hash = HashBytes64(&count, sizeof(count), hash);
				for (const RuntimeFace& face : role.faces)
				{
					const UInt64 contentHash = face.file ? face.file->contentHash : 0;
					const SInt32 faceIndex = face.ftFace
						? static_cast<SInt32>(face.ftFace->face_index) : 0;
					hash = HashBytes64(&contentHash, sizeof(contentHash), hash);
					hash = HashBytes64(&faceIndex, sizeof(faceIndex), hash);
				}
			}
			return hash ? hash : 1;
		}

		UInt64 ComputeRuntimeLayoutContentHash(RuntimeFont& runtime)
		{
			if (runtime.layoutContentHash)
				return runtime.layoutContentHash;
			UInt64 hash = HashBytes64(&kPersistentGlyphManifestVersion,
				sizeof(kPersistentGlyphManifestVersion));
			hash = HashBytes64(&runtime.config->layoutHash,
				sizeof(runtime.config->layoutHash), hash);
			hash = HashBytes64(&runtime.verticalAlignmentRasterScale,
				sizeof(runtime.verticalAlignmentRasterScale), hash);
			runtime.layoutContentHash = HashRuntimeFontFaces(runtime, hash);
			return runtime.layoutContentHash;
		}

		UInt64 ComputeRuntimeMaskContentHash(RuntimeFont& runtime,
			VectorFontByteClass byteClass)
		{
			const size_t roleIndex = static_cast<size_t>(byteClass);
			if (runtime.maskContentRoleHashes[roleIndex])
				return runtime.maskContentRoleHashes[roleIndex];
			UInt64 hash = HashBytes64(&kPersistentBitmapVersion,
				sizeof(kPersistentBitmapVersion));
			const UInt64 roleMaskHash = runtime.config->maskGenerationRoleHashes[roleIndex];
			hash = HashBytes64(&roleMaskHash, sizeof(roleMaskHash), hash);
			const RuntimeRole& role = runtime.roles[roleIndex];
			const UInt32 count = static_cast<UInt32>(role.faces.size());
			hash = HashBytes64(&count, sizeof(count), hash);
			for (const RuntimeFace& face : role.faces)
			{
				const UInt64 contentHash = face.file ? face.file->contentHash : 0;
				const SInt32 faceIndex = face.ftFace
					? static_cast<SInt32>(face.ftFace->face_index) : 0;
				hash = HashBytes64(&contentHash, sizeof(contentHash), hash);
				hash = HashBytes64(&faceIndex, sizeof(faceIndex), hash);
			}
			runtime.maskContentRoleHashes[roleIndex] = hash ? hash : 1;
			return runtime.maskContentRoleHashes[roleIndex];
		}

		bool IsGb2312RoundTrip(const char bytes[2])
		{
			constexpr UInt32 kGb2312CodePage = 20936;
			wchar_t decoded[2] = {};
			int decodedCount = MultiByteToWideChar(kGb2312CodePage,
				MB_ERR_INVALID_CHARS, bytes, 2, decoded,
				static_cast<int>(std::size(decoded)));
			if (!decodedCount && GetLastError() == ERROR_INVALID_FLAGS)
			{
				decodedCount = MultiByteToWideChar(kGb2312CodePage, 0,
					bytes, 2, decoded, static_cast<int>(std::size(decoded)));
			}
			if (decodedCount != 1)
				return false;

			char encoded[2] = {};
			BOOL usedDefault = FALSE;
			int encodedCount = WideCharToMultiByte(kGb2312CodePage,
				WC_NO_BEST_FIT_CHARS, decoded, decodedCount, encoded,
				static_cast<int>(std::size(encoded)), nullptr, &usedDefault);
			if (!encodedCount && GetLastError() == ERROR_INVALID_FLAGS)
			{
				usedDefault = FALSE;
				encodedCount = WideCharToMultiByte(kGb2312CodePage, 0,
					decoded, decodedCount, encoded,
					static_cast<int>(std::size(encoded)), nullptr, &usedDefault);
			}
			return encodedCount == 2 && !usedDefault
				&& encoded[0] == bytes[0] && encoded[1] == bytes[1];
		}

		void BuildGlyphManifestCodeTable(UInt32 codePage,
			FontPrewarmRange prewarmRange,
			std::vector<UInt16>& encodedCodes)
		{
			encodedCodes.clear();
			const bool gb2312 = codePage == 936
				&& prewarmRange == FontPrewarmRange::GB2312;
			encodedCodes.reserve(gb2312 ? 8448 : 24576);
			for (UInt32 value = 0; value <= 0xFF; ++value)
				encodedCodes.push_back(static_cast<UInt16>(value));
			if (!IsDbcsCodePage(codePage))
				return;
			const UInt32 firstLead = gb2312 ? 0xA1 : 0x80;
			const UInt32 lastLead = gb2312 ? 0xF7 : 0xFF;
			const UInt32 firstTrail = gb2312 ? 0xA1 : 1;
			const UInt32 lastTrail = gb2312 ? 0xFE : 0xFF;
			const bool validateGb2312Assignments =
				gb2312 && IsValidCodePage(20936);
			for (UInt32 lead = firstLead; lead <= lastLead; ++lead)
			{
				for (UInt32 trail = firstTrail; trail <= lastTrail; ++trail)
				{
					const char bytes[2] = {
						static_cast<char>(lead), static_cast<char>(trail)
					};
					if (validateGb2312Assignments
						&& !IsGb2312RoundTrip(bytes))
					{
						continue;
					}
					UInt32 encoded = 0;
					if (!TryDecodeDoubleByteForCodePage(bytes, codePage, encoded))
						continue;
					wchar_t decoded[2] = {};
					int count = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
						bytes, 2, decoded, static_cast<int>(std::size(decoded)));
					if (!count && GetLastError() == ERROR_INVALID_FLAGS)
					{
						count = MultiByteToWideChar(codePage, 0,
							bytes, 2, decoded, static_cast<int>(std::size(decoded)));
					}
					if (count == 1 || (count == 2
						&& decoded[0] >= 0xD800 && decoded[0] <= 0xDBFF
						&& decoded[1] >= 0xDC00 && decoded[1] <= 0xDFFF))
						encodedCodes.push_back(static_cast<UInt16>(encoded));
				}
			}
		}

	const std::vector<UInt16>& GetCompleteCodePageEncodedUnits()
		{
			std::lock_guard<std::recursive_mutex> lock(State().mutex);
			const UInt32 codePage = GetFreeTypeTextCodePage();
			FreeTypeState& state = State();
			if (state.persistentGlyphManifestCodePage != codePage)
			{
				BuildGlyphManifestCodeTable(codePage,
					FontPrewarmRange::CompleteCodePage,
					state.persistentGlyphManifestCodes);
				state.persistentGlyphManifestCodePage = codePage;
			}
			return state.persistentGlyphManifestCodes;
		}

	const std::vector<UInt16>& GetFontPrewarmEncodedUnits(
		const FontConfig& config)
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		const UInt32 codePage = GetFreeTypeTextCodePage();
		FreeTypeState& state = State();
		const FontPrewarmRange range = ResolveFontPrewarmRange(config);
		if (range == FontPrewarmRange::GB2312)
		{
			if (state.persistentGlyphManifestGb2312Codes.empty())
			{
				BuildGlyphManifestCodeTable(936, FontPrewarmRange::GB2312,
					state.persistentGlyphManifestGb2312Codes);
			}
			return state.persistentGlyphManifestGb2312Codes;
		}
		if (state.persistentGlyphManifestCodePage != codePage)
		{
			BuildGlyphManifestCodeTable(codePage,
				FontPrewarmRange::CompleteCodePage,
				state.persistentGlyphManifestCodes);
			state.persistentGlyphManifestCodePage = codePage;
		}
		return state.persistentGlyphManifestCodes;
	}

		PersistentGlyphManifestHeader MakeGlyphManifestHeader(
			const RuntimeFont& runtime, UInt64 manifestHash, UInt64 layoutContentHash,
			UInt32 recordCount)
		{
			PersistentGlyphManifestHeader header;
			const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'G', 'L', 'Y', '1' };
			std::memcpy(header.magic, magic, sizeof(magic));
			header.version = kPersistentGlyphManifestVersion;
			header.headerSize = sizeof(header);
			header.manifestHash = manifestHash;
			header.layoutContentHash = layoutContentHash;
			header.layoutHash = runtime.config->layoutHash;
			header.reservedFontId = 0;
			header.codePage = GetFreeTypeTextCodePage();
			header.entryCount = recordCount;
			header.entrySize = sizeof(PersistentGlyphManifestRecord);
			const PersistentFontCacheDomain cacheDomain =
				GetPersistentFontCacheDomain();
			header.distanceFieldMethod = cacheDomain
				== PersistentFontCacheDomain::DistanceField
				? static_cast<UInt8>(GetConfiguredDistanceFieldMethod()) : 0;
			header.cacheIdentityVersion =
				kPersistentGlyphManifestCacheIdentityVersion;
			header.cacheDomain = static_cast<UInt8>(cacheDomain);
			header.checksum = HashBytes64(&header,
				offsetof(PersistentGlyphManifestHeader, checksum));
			return header;
		}

		bool MatchesGlyphManifestHeader(const PersistentGlyphManifestHeader& header,
			const RuntimeFont& runtime, UInt64 manifestHash, UInt64 layoutContentHash,
			UInt32 recordCount)
		{
			const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'G', 'L', 'Y', '1' };
			const PersistentFontCacheDomain cacheDomain =
				GetPersistentFontCacheDomain();
			const UInt8 distanceFieldMethod = cacheDomain
				== PersistentFontCacheDomain::DistanceField
				? static_cast<UInt8>(GetConfiguredDistanceFieldMethod()) : 0;
			return std::memcmp(header.magic, magic, sizeof(magic)) == 0
				&& header.version == kPersistentGlyphManifestVersion
				&& header.headerSize == sizeof(header)
				&& header.manifestHash == manifestHash
				&& header.layoutContentHash == layoutContentHash
				&& header.layoutHash == runtime.config->layoutHash
				&& header.reservedFontId == 0
				&& header.codePage == GetFreeTypeTextCodePage()
				&& header.entryCount == recordCount
				&& header.entrySize == sizeof(PersistentGlyphManifestRecord)
				&& header.distanceFieldMethod == distanceFieldMethod
				&& header.cacheIdentityVersion
					== kPersistentGlyphManifestCacheIdentityVersion
				&& header.cacheDomain == static_cast<UInt8>(cacheDomain)
				&& header.checksum == HashBytes64(&header,
					offsetof(PersistentGlyphManifestHeader, checksum));
		}

		void UnmapGlyphManifest(PersistentGlyphManifest& manifest)
		{
			manifest.validatedRecordIndexReady = false;
			manifest.validatedRecordIndex.clear();
			if (manifest.mappedData)
				UnmapViewOfFile(manifest.mappedData);
			if (manifest.mapping)
				CloseHandle(manifest.mapping);
			manifest.mappedData = nullptr;
			manifest.mapping = nullptr;
			RefreshGlyphManifestCpuMemory(manifest);
		}

		bool MapGlyphManifest(PersistentGlyphManifest& manifest)
		{
			UnmapGlyphManifest(manifest);
			manifest.mapping = CreateFileMappingW(manifest.file, nullptr,
				manifest.writable ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
			if (!manifest.mapping)
				return false;
			manifest.mappedData = static_cast<UInt8*>(MapViewOfFile(manifest.mapping,
				manifest.writable ? FILE_MAP_WRITE | FILE_MAP_READ : FILE_MAP_READ,
				0, 0, 0));
			if (!manifest.mappedData)
			{
				CloseHandle(manifest.mapping);
				manifest.mapping = nullptr;
			}
			RefreshGlyphManifestCpuMemory(manifest);
			return manifest.mappedData != nullptr;
		}

		bool WriteGlyphManifestFile(PersistentGlyphManifest& manifest,
			const PersistentGlyphManifestHeader& header,
			const std::vector<UInt16>& encodedCodes)
		{
			if (!manifest.writable || encodedCodes.size() != header.entryCount)
				return false;
			std::vector<PersistentGlyphManifestRecord> records(encodedCodes.size());
			for (size_t index = 0; index < encodedCodes.size(); ++index)
				records[index].encodedCode = encodedCodes[index];
			const UInt64 expectedSize = sizeof(header)
				+ static_cast<UInt64>(records.size()) * sizeof(records[0]);
			const size_t recordBytes = records.size() * sizeof(records[0]);
			return recordBytes <= std::numeric_limits<UInt32>::max()
				&& SetFileSize64(manifest.file, 0)
				&& SetFileSize64(manifest.file, expectedSize)
				&& WriteFileAt(manifest.file, 0, &header, sizeof(header))
				&& WriteFileAt(manifest.file, sizeof(header), records.data(),
					static_cast<UInt32>(recordBytes));
		}

		bool ManifestCodeTableMatches(const PersistentGlyphManifest& manifest,
			const std::vector<UInt16>& encodedCodes)
		{
			if (!manifest.mappedData || encodedCodes.size() != manifest.recordCount)
				return false;
			const auto* records = reinterpret_cast<const PersistentGlyphManifestRecord*>(
				manifest.mappedData + sizeof(PersistentGlyphManifestHeader));
			for (size_t index = 0; index < encodedCodes.size(); ++index)
			{
				if (records[index].encodedCode != encodedCodes[index]
					|| records[index].reserved != 0)
				{
					return false;
				}
			}
			return true;
		}

		bool BuildValidatedGlyphManifestIndex(PersistentGlyphManifest& manifest,
			const RuntimeFont& runtime)
		{
			manifest.validatedRecordIndexReady = false;
			manifest.validatedRecordIndex.clear();
			if (!manifest.mappedData || !manifest.recordCount
				|| manifest.recordCount > std::numeric_limits<UInt16>::max())
			{
				RefreshGlyphManifestCpuMemory(manifest);
				return false;
			}

			const auto* header = reinterpret_cast<const PersistentGlyphManifestHeader*>(
				manifest.mappedData);
			if (header->completeMode < kCompleteCodePagePrewarmIdentity)
			{
				RefreshGlyphManifestCpuMemory(manifest);
				return false;
			}

			constexpr UInt16 invalidRecordIndex =
				std::numeric_limits<UInt16>::max();
			std::vector<UInt16> validatedIndex;
			try
			{
				validatedIndex.assign(0x10000u, invalidRecordIndex);
			}
			catch (const std::bad_alloc&)
			{
				RefreshGlyphManifestCpuMemory(manifest);
				return false;
			}

			const auto* records = reinterpret_cast<const PersistentGlyphManifestRecord*>(
				manifest.mappedData + sizeof(PersistentGlyphManifestHeader));
			for (UInt32 index = 0; index < manifest.recordCount; ++index)
			{
				const PersistentGlyphManifestRecord& record = records[index];
				const PersistentGlyphManifestEntry& entry = record.entry;
				if (!entry.valid)
					continue;
				const VectorFontByteClass byteClass = record.encodedCode > 0xFF
					? VectorFontByteClass::DoubleByte
					: VectorFontByteClass::SingleByte;
				const RuntimeRole& role = runtime.roles[static_cast<size_t>(byteClass)];
				if (entry.byteClass != static_cast<UInt8>(byteClass)
					|| entry.faceIndex >= role.faces.size()
					|| entry.checksum != HashBytes64(&entry,
						offsetof(PersistentGlyphManifestEntry, checksum)))
				{
					continue;
				}
				validatedIndex[record.encodedCode] = static_cast<UInt16>(index);
			}

			manifest.validatedRecordIndex.swap(validatedIndex);
			manifest.validatedRecordIndexReady = true;
			RefreshGlyphManifestCpuMemory(manifest);
			return true;
		}

		bool InitializeGlyphManifest(PersistentGlyphManifest& manifest,
			RuntimeFont& runtime, const std::vector<UInt16>& encodedCodes)
		{
			manifest.recordCount = static_cast<UInt32>(encodedCodes.size());
			manifest.file = CreateFileW(manifest.path.c_str(),
				GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			manifest.writable = manifest.file != INVALID_HANDLE_VALUE;
			if (!manifest.writable)
			{
				manifest.file = CreateFileW(manifest.path.c_str(), GENERIC_READ,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
					OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			}
			if (manifest.file == INVALID_HANDLE_VALUE)
				return false;

			const UInt64 expectedSize = sizeof(PersistentGlyphManifestHeader)
				+ static_cast<UInt64>(manifest.recordCount)
					* sizeof(PersistentGlyphManifestRecord);
			UInt64 fileSize = 0;
			PersistentGlyphManifestHeader header;
			bool valid = GetFileSize64(manifest.file, fileSize)
				&& fileSize == expectedSize
				&& ReadFileAt(manifest.file, 0, &header, sizeof(header))
				&& MatchesGlyphManifestHeader(header, runtime, manifest.manifestHash,
					manifest.layoutContentHash, manifest.recordCount);
			if (!valid)
			{
				if (!manifest.writable)
					return false;
				header = MakeGlyphManifestHeader(runtime, manifest.manifestHash,
					manifest.layoutContentHash, manifest.recordCount);
				if (!WriteGlyphManifestFile(manifest, header, encodedCodes))
					return false;
			}
			if (!MapGlyphManifest(manifest))
				return false;
			if (ManifestCodeTableMatches(manifest, encodedCodes))
			{
				BuildValidatedGlyphManifestIndex(manifest, runtime);
				return true;
			}
			if (!manifest.writable)
			{
				UnmapGlyphManifest(manifest);
				return false;
			}
			UnmapGlyphManifest(manifest);
			header = MakeGlyphManifestHeader(runtime, manifest.manifestHash,
				manifest.layoutContentHash, manifest.recordCount);
			const bool initialized = WriteGlyphManifestFile(manifest, header, encodedCodes)
				&& MapGlyphManifest(manifest)
				&& ManifestCodeTableMatches(manifest, encodedCodes);
			if (initialized)
				BuildValidatedGlyphManifestIndex(manifest, runtime);
			return initialized;
		}

		PersistentGlyphManifest* GetGlyphManifest(RuntimeFont& runtime)
		{
			if (runtime.manifest)
			{
				if (runtime.manifest->mappedData)
					return runtime.manifest.get();
				// Do not let a previously failed or explicitly unmapped object suppress
				// reconstruction. Dropping this reference also releases any partially
				// opened handles when it is the last owner.
				runtime.manifest.reset();
			}
			const UInt64 layoutContentHash = ComputeRuntimeLayoutContentHash(runtime);
			UInt64 manifestHash = HashBytes64(&layoutContentHash,
				sizeof(layoutContentHash));
			const UInt32 codePage = GetFreeTypeTextCodePage();
			manifestHash = HashBytes64(&codePage,
				sizeof(codePage), manifestHash);
			const FontPrewarmRange prewarmRange =
				ResolveFontPrewarmRange(GetRuntimeConfig(runtime));
			manifestHash = HashBytes64(&prewarmRange,
				sizeof(prewarmRange), manifestHash);
			const PersistentFontCacheDomain cacheDomain =
				GetPersistentFontCacheDomain();
			manifestHash = HashBytes64(&cacheDomain,
				sizeof(cacheDomain), manifestHash);
			manifestHash = HashBytes64(
				&kPersistentGlyphManifestCacheIdentityVersion,
				sizeof(kPersistentGlyphManifestCacheIdentityVersion),
				manifestHash);
			if (cacheDomain == PersistentFontCacheDomain::DistanceField)
			{
				const DistanceFieldMethod distanceFieldMethod =
					GetConfiguredDistanceFieldMethod();
				const UInt32 distanceFieldRevision =
					DistanceFieldGeneratorRevision(distanceFieldMethod);
				manifestHash = HashBytes64(&distanceFieldMethod,
					sizeof(distanceFieldMethod), manifestHash);
				manifestHash = HashBytes64(&distanceFieldRevision,
					sizeof(distanceFieldRevision), manifestHash);
			}
			else
			{
				manifestHash = HashBytes64(&kCpuEffectCoverageVersion,
					sizeof(kCpuEffectCoverageVersion), manifestHash);
			}
			auto pooled = State().persistentGlyphManifests.find(manifestHash);
			if (pooled != State().persistentGlyphManifests.end())
			{
				if (std::shared_ptr<PersistentGlyphManifest> shared = pooled->second.lock())
				{
					if (shared->layoutContentHash != layoutContentHash)
						return nullptr;
					if (shared->mappedData)
					{
						runtime.manifest = std::move(shared);
						return runtime.manifest.get();
					}
				}
				State().persistentGlyphManifests.erase(pooled);
			}
			auto manifest = std::make_shared<PersistentGlyphManifest>();
			manifest->manifestHash = manifestHash;
			manifest->layoutContentHash = layoutContentHash;
			std::wstring directory;
			if (!EnsurePersistentBitmapDirectory(directory))
				return nullptr;
			wchar_t fileName[256] = {};
			_snwprintf_s(fileName, _countof(fileName), _TRUNCATE,
				L"shared_%016llX.tnvfmanifest",
				static_cast<unsigned long long>(manifestHash));
			manifest->path = directory + L"\\" + fileName;
			const std::vector<UInt16>& encodedCodes =
				GetFontPrewarmEncodedUnits(GetRuntimeConfig(runtime));
			if (!InitializeGlyphManifest(*manifest, runtime, encodedCodes)
				|| !manifest->mappedData)
			{
				// The local shared_ptr owns every partially created mapping and handle.
				// Nothing is published, so the next request remains eligible to retry.
				return nullptr;
			}

			// Publish only after the on-disk header, code table and mapping have all
			// passed initialization. The weak pool never contains failed manifests.
			State().persistentGlyphManifests[manifestHash] = manifest;
			runtime.manifest = std::move(manifest);
			State().usedPersistentCachePaths.insert(
				NormalizePathKey(runtime.manifest->path));
			return runtime.manifest.get();
		}

		PersistentGlyphManifestRecord* GetGlyphManifestRecord(
			PersistentGlyphManifest& manifest, UInt32 encodedCode)
		{
			if (!manifest.mappedData || encodedCode > 0xFFFF || !manifest.recordCount)
				return nullptr;
			auto* records = reinterpret_cast<PersistentGlyphManifestRecord*>(
				manifest.mappedData + sizeof(PersistentGlyphManifestHeader));
			size_t first = 0;
			size_t last = manifest.recordCount;
			while (first < last)
			{
				const size_t middle = first + (last - first) / 2;
				if (records[middle].encodedCode < encodedCode)
					first = middle + 1;
				else
					last = middle;
			}
			return first < manifest.recordCount
				&& records[first].encodedCode == encodedCode
				? &records[first] : nullptr;
		}

		PersistentGlyphManifestRecord* GetIndexedGlyphManifestRecord(
			PersistentGlyphManifest& manifest, UInt32 encodedCode)
		{
			if (!manifest.mappedData || !manifest.validatedRecordIndexReady
				|| encodedCode > 0xFFFF
				|| manifest.validatedRecordIndex.size() != 0x10000u)
			{
				return nullptr;
			}
			const UInt16 recordIndex = manifest.validatedRecordIndex[encodedCode];
			if (recordIndex == std::numeric_limits<UInt16>::max()
				|| recordIndex >= manifest.recordCount)
			{
				return nullptr;
			}
			auto* records = reinterpret_cast<PersistentGlyphManifestRecord*>(
				manifest.mappedData + sizeof(PersistentGlyphManifestHeader));
			return &records[recordIndex];
		}

	bool EnsureCompleteCodePageMetricTable(RuntimeFont& runtime)
	{
		if (runtime.codePageMetrics)
			return true;
		PersistentGlyphManifest* manifest = GetGlyphManifest(runtime);
		if (!manifest || !manifest->mappedData
			|| !manifest->validatedRecordIndexReady)
		{
			return false;
		}
		const auto* header = reinterpret_cast<const PersistentGlyphManifestHeader*>(
			manifest->mappedData);
		if (header->completeMode < kCompleteCodePagePrewarmIdentity)
			return false;

		std::shared_ptr<DirectExtraGlyphTable> table;
		try
		{
			table = std::make_shared<DirectExtraGlyphTable>();
		}
		catch (const std::bad_alloc&)
		{
			return false;
		}
		if (!table->Initialize(manifest->recordCount))
			return false;
		for (UInt32 lead = DirectExtraGlyphTable::kFirstLeadByte;
			lead <= DirectExtraGlyphTable::kLastLeadByte; ++lead)
		{
			for (UInt32 trail = DirectExtraGlyphTable::kFirstTrailByte;
				trail <= DirectExtraGlyphTable::kLastTrailByte; ++trail)
			{
				const UInt32 encodedCode = (lead << 8) | trail;
				PersistentGlyphManifestRecord* record =
					GetIndexedGlyphManifestRecord(*manifest, encodedCode);
				if (!record || record->entry.byteClass
					!= static_cast<UInt8>(VectorFontByteClass::DoubleByte))
				{
					continue;
				}
				const PersistentGlyphManifestEntry& entry = record->entry;
				FontLetter metrics = {};
				metrics.iTextureIndex = entry.textureIndex;
				metrics.fWidth = entry.width;
				metrics.fLeadingEdge = entry.leadingEdge;
				metrics.fHeight = entry.height;
				metrics.fTopEdge = entry.topEdge;
				metrics.fSpacing = entry.spacing;
				ApplyEffectExtentsToMetrics(*runtime.config,
					entry.codePoint, metrics);
				if (!table->Insert(encodedCode, metrics))
					return false;
			}
		}
		if (table->metrics.empty())
			return false;

		const size_t allocatedBytes = table->GetAllocatedBytes();
		runtime.codePageMetrics = std::move(table);
		runtime.cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
			runtime.cpuMemory.GetBytes() + allocatedBytes);
		if (g_bEnableFreeTypeFontRenderingLog)
		{
			FreeTypeFontDebugLog(
				"tnvse_freetype_font: materialized codepage metrics font=%u glyphs=%u bytes=%llu",
				runtime.config->fontId,
				static_cast<UInt32>(runtime.codePageMetrics->metrics.size()),
				static_cast<unsigned long long>(allocatedBytes));
		}
		return true;
	}

		bool LoadGlyphManifest(RuntimeFont& runtime, UInt32 encodedCode,
			VectorFontByteClass byteClass, VectorEncodedGlyph* glyph, FontLetter* metrics)
		{
			PersistentGlyphManifest* manifest = GetGlyphManifest(runtime);
			const bool validatedIndex = manifest
				&& manifest->validatedRecordIndexReady;
			PersistentGlyphManifestRecord* record = manifest
				? (validatedIndex
					? GetIndexedGlyphManifestRecord(*manifest, encodedCode)
					: GetGlyphManifestRecord(*manifest, encodedCode))
				: nullptr;
			PersistentGlyphManifestEntry* entry = record ? &record->entry : nullptr;
			if (!entry || !entry->valid
				|| entry->byteClass != static_cast<UInt8>(byteClass)
				|| (!validatedIndex && entry->checksum != HashBytes64(entry,
					offsetof(PersistentGlyphManifestEntry, checksum))))
				return false;
			RuntimeRole& role = runtime.roles[static_cast<size_t>(byteClass)];
			if (entry->faceIndex >= role.faces.size())
				return false;
			if (glyph)
			{
				glyph->encodedCode = encodedCode;
				glyph->byteClass = byteClass;
				glyph->byteLength = byteClass == VectorFontByteClass::DoubleByte ? 2 : 1;
				glyph->codePoint = entry->codePoint;
				glyph->faceIndex = entry->faceIndex;
				glyph->glyphIndex = entry->glyphIndex;
				glyph->hasGlyphIdentity = true;
			}
			if (metrics)
			{
				metrics->iTextureIndex = entry->textureIndex;
				metrics->fWidth = entry->width;
				metrics->fLeadingEdge = entry->leadingEdge;
				metrics->fHeight = entry->height;
				metrics->fTopEdge = entry->topEdge;
				metrics->fSpacing = entry->spacing;
				ApplyEffectExtentsToMetrics(*runtime.config,
					entry->codePoint, *metrics);
			}
			const auto inserted = role.glyphIdentities.emplace(
				entry->codePoint,
				CachedGlyphIdentity{ entry->faceIndex, entry->glyphIndex,
					entry->renderedCodePoint });
			if (inserted.second)
			{
				runtime.cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
					runtime.cpuMemory.GetBytes()
						+ sizeof(std::pair<const UInt32, CachedGlyphIdentity>)
						+ 3u * sizeof(void*));
			}
			return true;
		}

		bool LoadGlyphManifestIdentity(RuntimeFont& runtime, UInt32 encodedCode,
			VectorFontByteClass byteClass, VectorEncodedGlyph& glyph)
		{
			PersistentGlyphManifest* manifest = GetGlyphManifest(runtime);
			const bool validatedIndex = manifest
				&& manifest->validatedRecordIndexReady;
			PersistentGlyphManifestRecord* record = manifest
				? (validatedIndex
					? GetIndexedGlyphManifestRecord(*manifest, encodedCode)
					: GetGlyphManifestRecord(*manifest, encodedCode))
				: nullptr;
			const PersistentGlyphManifestEntry* entry = record ? &record->entry : nullptr;
			if (!entry || !entry->valid
				|| entry->byteClass != static_cast<UInt8>(byteClass)
				|| entry->faceIndex
					>= runtime.roles[static_cast<size_t>(byteClass)].faces.size()
				|| (!validatedIndex && entry->checksum != HashBytes64(entry,
					offsetof(PersistentGlyphManifestEntry, checksum))))
			{
				return false;
			}

			glyph = {};
			glyph.encodedCode = encodedCode;
			glyph.byteClass = byteClass;
			glyph.byteLength =
				byteClass == VectorFontByteClass::DoubleByte ? 2 : 1;
			glyph.codePoint = entry->codePoint;
			glyph.faceIndex = entry->faceIndex;
			glyph.glyphIndex = entry->glyphIndex;
			glyph.hasGlyphIdentity = true;
			return true;
		}

		void StoreGlyphManifest(RuntimeFont& runtime, const VectorEncodedGlyph& glyph,
			const ResolvedGlyph& resolved, const FontLetter& metrics)
		{
			PersistentGlyphManifest* manifest = GetGlyphManifest(runtime);
			PersistentGlyphManifestRecord* record = manifest
				? GetGlyphManifestRecord(*manifest, glyph.encodedCode) : nullptr;
			PersistentGlyphManifestEntry* destination = record ? &record->entry : nullptr;
			if (!destination || !manifest->writable || destination->valid)
				return;
			PersistentGlyphManifestEntry entry;
			entry.valid = 1;
			entry.byteClass = static_cast<UInt8>(glyph.byteClass);
			entry.faceIndex = static_cast<UInt16>(resolved.faceIndex);
			entry.glyphIndex = resolved.glyphIndex;
			entry.codePoint = glyph.codePoint;
			entry.renderedCodePoint = resolved.renderedCodePoint;
			FontLetter bodyMetrics = metrics;
			RemoveEffectExtentsFromMetrics(*runtime.config,
				glyph.codePoint, bodyMetrics);
			entry.textureIndex = bodyMetrics.iTextureIndex;
			entry.width = bodyMetrics.fWidth;
			entry.leadingEdge = bodyMetrics.fLeadingEdge;
			entry.height = bodyMetrics.fHeight;
			entry.topEdge = bodyMetrics.fTopEdge;
			entry.spacing = bodyMetrics.fSpacing;
			entry.checksum = HashBytes64(&entry,
				offsetof(PersistentGlyphManifestEntry, checksum));
			std::memcpy(destination, &entry, sizeof(entry));
			if (manifest->validatedRecordIndexReady
				&& glyph.encodedCode <= 0xFFFF
				&& manifest->validatedRecordIndex.size() == 0x10000u)
			{
				auto* records = reinterpret_cast<PersistentGlyphManifestRecord*>(
					manifest->mappedData + sizeof(PersistentGlyphManifestHeader));
				const size_t recordIndex = static_cast<size_t>(record - records);
				if (recordIndex < std::numeric_limits<UInt16>::max())
				{
					manifest->validatedRecordIndex[glyph.encodedCode] =
						static_cast<UInt16>(recordIndex);
				}
			}
		}

}

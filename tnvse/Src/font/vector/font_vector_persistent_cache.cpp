#include "font_freetype_internal.h"

#include "encoding.h"
#include "font_glyphs.h"
#include "globals.h"

#include <winioctl.h>

#include "font_vector_persistent_cache_internal.h"

namespace fonthook::vectorfont
{
	static bool IsMissingPersistentCacheFileError(DWORD error)
	{
		return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
	}

	PersistentFontCacheDomain GetPersistentFontCacheDomain()
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		if (State().persistentCacheRouteSynchronized)
			return State().persistentCacheDomain;
		return ResolvePersistentFontCacheDomain(ResolveFontAtlasRoute(
			IsNativeFontRendererAvailable(),
			UsesBakedEffectRoute()));
	}

	FontAtlasRoute GetPersistentFontCacheRoute()
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		if (State().persistentCacheRouteSynchronized)
			return State().persistentCacheRoute;
		return ResolveFontAtlasRoute(IsNativeFontRendererAvailable(),
			UsesBakedEffectRoute());
	}

	bool GetFreeTypeFontCacheDirectory(std::wstring& directory)
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		return EnsurePersistentBitmapDirectory(directory);
	}

	void MarkFreeTypeFontCacheFileUsed(const std::wstring& path)
	{
		if (path.empty())
			return;
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		State().usedPersistentCachePaths.insert(NormalizePathKey(path));
	}

	template <class Header>
	static bool ReadPersistentCacheHeader(const std::wstring& path, Header& header)
	{
		header = {};
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return false;
		DWORD read = 0;
		const bool result = ReadFile(file, &header, sizeof(header), &read, nullptr)
			&& read == sizeof(header);
		CloseHandle(file);
		return result;
	}

	static bool PersistentManifestBelongsToDomain(
		const PersistentGlyphManifest& manifest,
		PersistentFontCacheDomain cacheDomain)
	{
		if (!manifest.mappedData)
			return false;
		const auto* header = reinterpret_cast<
			const PersistentGlyphManifestHeader*>(manifest.mappedData);
		const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'G', 'L', 'Y', '1' };
		return std::memcmp(header->magic, magic, sizeof(magic)) == 0
			&& header->version == kPersistentGlyphManifestVersion
			&& header->headerSize == sizeof(*header)
			&& header->cacheIdentityVersion
				== kPersistentGlyphManifestCacheIdentityVersion
			&& header->cacheDomain == static_cast<UInt8>(cacheDomain)
			&& header->checksum == HashBytes64(header,
				offsetof(PersistentGlyphManifestHeader, checksum));
	}

	static void DetachPersistentManifestsOutsideDomain(
		PersistentFontCacheDomain cacheDomain)
	{
		FreeTypeState& state = State();
		std::vector<std::shared_ptr<PersistentGlyphManifest>> detached;
		std::unordered_set<PersistentGlyphManifest*> seen;
		UInt32 detachedRuntimes = 0;
		for (auto& pair : state.runtimeFonts)
		{
			RuntimeFont& runtime = *pair.second;
			if (!runtime.manifest
				|| PersistentManifestBelongsToDomain(
					*runtime.manifest, cacheDomain))
			{
				continue;
			}
			if (seen.insert(runtime.manifest.get()).second)
				detached.push_back(runtime.manifest);
			runtime.manifest.reset();
			runtime.codePageMetrics.reset();
			size_t runtimeBytes = sizeof(RuntimeFont);
			for (RuntimeRole& role : runtime.roles)
			{
				role.glyphIdentities.clear();
				runtimeBytes += role.faces.capacity() * sizeof(RuntimeFace);
			}
			runtime.cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
				runtimeBytes);
			auto extra = gNumberedExtraLetters.find(pair.first);
			if (extra != gNumberedExtraLetters.end())
				extra->second.generatedCodePage.reset();
			++detachedRuntimes;
		}

		for (auto pooled = state.persistentGlyphManifests.begin();
			pooled != state.persistentGlyphManifests.end();)
		{
			std::shared_ptr<PersistentGlyphManifest> manifest =
				pooled->second.lock();
			if (!manifest
				|| !PersistentManifestBelongsToDomain(*manifest, cacheDomain))
			{
				pooled = state.persistentGlyphManifests.erase(pooled);
			}
			else
			{
				++pooled;
			}
		}

		for (const std::shared_ptr<PersistentGlyphManifest>& manifest : detached)
		{
			UnmapGlyphManifest(*manifest);
			if (manifest->file != INVALID_HANDLE_VALUE)
			{
				CloseHandle(manifest->file);
				manifest->file = INVALID_HANDLE_VALUE;
			}
			manifest->writable = false;
			if (!manifest->path.empty())
			{
				state.usedPersistentCachePaths.erase(
					NormalizePathKey(manifest->path));
			}
		}
		if (detachedRuntimes)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: persistent manifest route detach domain=%s runtimes=%u mappings=%u",
				cacheDomain == PersistentFontCacheDomain::DistanceField
					? "distance-field" : "cpu-coverage",
				detachedRuntimes, static_cast<UInt32>(detached.size()));
		}
	}

	static bool BitmapCacheKeyBelongsToRoute(UInt8 maskType,
		UInt8 distanceFieldMethod, FontAtlasRoute route)
	{
		const UInt8 distance = static_cast<UInt8>(
			GlyphMaskType::DistanceField);
		const UInt8 composite = static_cast<UInt8>(
			GlyphMaskType::Composite);
		if (route == FontAtlasRoute::ShaderDistanceField)
		{
			return maskType == distance
				&& distanceFieldMethod == static_cast<UInt8>(
					GetConfiguredDistanceFieldMethod());
		}
		if (route == FontAtlasRoute::BakedArgbComposite)
			return maskType == composite;
		return maskType != distance && maskType != composite;
	}

	static void ReleaseBitmapCachesOutsideRoute(FontAtlasRoute route)
	{
		FreeTypeState& state = State();
		for (auto entry = state.bitmapCache.begin();
			entry != state.bitmapCache.end();)
		{
			if (BitmapCacheKeyBelongsToRoute(entry->first.maskType,
				entry->first.distanceFieldMethod, route))
			{
				++entry;
				continue;
			}
			state.bitmapCacheBytes -= std::min(
				state.bitmapCacheBytes, entry->second.bytes);
			state.bitmapLru.erase(entry->second.lru);
			entry = state.bitmapCache.erase(entry);
		}
		for (auto profile = state.persistentBitmapProfiles.begin();
			profile != state.persistentBitmapProfiles.end();)
		{
			if (BitmapCacheKeyBelongsToRoute(profile->first.maskType,
				profile->first.distanceFieldMethod, route))
			{
				++profile;
				continue;
			}
			if (!profile->second->path.empty())
			{
				state.usedPersistentCachePaths.erase(
					NormalizePathKey(profile->second->path));
			}
			profile = state.persistentBitmapProfiles.erase(profile);
		}
	}

	static PersistentCacheCleanupClass ClassifyPersistentBitmapCache(
		const std::wstring& path)
	{
		PersistentBitmapFileHeader header;
		const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'M', 'S', 'K', '1' };
		if (!ReadPersistentCacheHeader(path, header)
			|| std::memcmp(header.magic, magic, sizeof(magic)) != 0
			|| header.version != kPersistentBitmapVersion
			|| header.headerSize != sizeof(header)
			|| header.checksum != HashBytes64(&header,
				offsetof(PersistentBitmapFileHeader, checksum)))
		{
			return PersistentCacheCleanupClass::Invalid;
		}
		const FontAtlasRoute route = GetPersistentFontCacheRoute();
		const UInt8 maskType = header.maskType;
		if (maskType == static_cast<UInt8>(GlyphMaskType::DistanceField))
		{
			if (header.distanceFieldMethod
				> static_cast<UInt8>(DistanceFieldMethod::Mtsdf))
			{
				return PersistentCacheCleanupClass::Invalid;
			}
			return route == FontAtlasRoute::ShaderDistanceField
					&& header.distanceFieldMethod == static_cast<UInt8>(
						GetConfiguredDistanceFieldMethod())
				? PersistentCacheCleanupClass::CurrentDistanceField
				: PersistentCacheCleanupClass::InactiveDistanceField;
		}
		if (maskType == static_cast<UInt8>(GlyphMaskType::Composite))
		{
			return route == FontAtlasRoute::BakedArgbComposite
				? PersistentCacheCleanupClass::Neutral
				: PersistentCacheCleanupClass::InactiveDistanceField;
		}
		if (maskType > static_cast<UInt8>(GlyphMaskType::Composite))
			return PersistentCacheCleanupClass::Invalid;
		return route == FontAtlasRoute::ArgbFallback
			? PersistentCacheCleanupClass::Neutral
			: PersistentCacheCleanupClass::InactiveDistanceField;
	}

	static PersistentCacheCleanupClass ClassifyPersistentGlyphManifestCache(
		const std::wstring& path)
	{
		PersistentGlyphManifestHeader header;
		const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'G', 'L', 'Y', '1' };
		if (!ReadPersistentCacheHeader(path, header)
			|| std::memcmp(header.magic, magic, sizeof(magic)) != 0
			|| header.version != kPersistentGlyphManifestVersion
			|| header.headerSize != sizeof(header)
			|| header.cacheIdentityVersion
				!= kPersistentGlyphManifestCacheIdentityVersion
			|| header.cacheDomain
				> static_cast<UInt8>(PersistentFontCacheDomain::CpuCoverage)
			|| header.checksum != HashBytes64(&header,
				offsetof(PersistentGlyphManifestHeader, checksum)))
		{
			return PersistentCacheCleanupClass::Invalid;
		}
		const PersistentFontCacheDomain cacheDomain =
			static_cast<PersistentFontCacheDomain>(header.cacheDomain);
		if (cacheDomain == PersistentFontCacheDomain::CpuCoverage)
			return PersistentCacheCleanupClass::Neutral;
		if (header.distanceFieldMethod
			> static_cast<UInt8>(DistanceFieldMethod::Mtsdf))
			return PersistentCacheCleanupClass::Invalid;
		if (GetPersistentFontCacheDomain()
			== PersistentFontCacheDomain::CpuCoverage)
			return PersistentCacheCleanupClass::InactiveDistanceField;
		return header.distanceFieldMethod == static_cast<UInt8>(
			GetConfiguredDistanceFieldMethod())
			? PersistentCacheCleanupClass::CurrentDistanceField
			: PersistentCacheCleanupClass::InactiveDistanceField;
	}

	void SynchronizePersistentFontCacheRoute(FontAtlasRoute route)
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		const PersistentFontCacheDomain cacheDomain =
			ResolvePersistentFontCacheDomain(route);
		FreeTypeState& state = State();
		if (state.persistentCacheRouteSynchronized
			&& state.persistentCacheRoute == route)
		{
			return;
		}

		const bool wasSynchronized = state.persistentCacheRouteSynchronized;
		const FontAtlasRoute previousRoute = state.persistentCacheRoute;
		state.persistentCacheRouteSynchronized = true;
		state.persistentCacheRoute = route;
		state.persistentCacheDomain = cacheDomain;
		ReleaseBitmapCachesOutsideRoute(route);
		DetachPersistentManifestsOutsideDomain(cacheDomain);

		if (cacheDomain != PersistentFontCacheDomain::CpuCoverage)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: persistent cache route synchronized previous=%s current=distance-field invalidation=none",
				wasSynchronized
					? (previousRoute == FontAtlasRoute::ShaderDistanceField
						? "distance-field"
						: previousRoute == FontAtlasRoute::BakedArgbComposite
							? "argb-composite" : "argb-fallback")
					: "unresolved");
			return;
		}

		std::wstring directory;
		if (!EnsurePersistentBitmapDirectory(directory))
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: distance-field cache invalidation unavailable reason=cache-directory");
			return;
		}

		UInt32 deletedMasks = 0;
		UInt32 deletedManifests = 0;
		UInt32 deletedAtlases = 0;
		UInt32 deletedDirect = 0;
		UInt32 deletedTemporary = 0;
		UInt32 failed = 0;
		UInt64 deletedBytes = 0;
		auto hasSuffix = [](const std::wstring& value, const wchar_t* suffix)
		{
			const size_t length = std::wcslen(suffix);
			return value.size() >= length
				&& value.compare(value.size() - length, length, suffix) == 0;
		};
		const std::wstring pattern = directory + L"\\*";
		WIN32_FIND_DATAW found = {};
		HANDLE search = FindFirstFileW(pattern.c_str(), &found);
		if (search != INVALID_HANDLE_VALUE)
		{
			do
			{
				if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					continue;
				const std::wstring path = directory + L"\\" + found.cFileName;
				const std::wstring normalized = NormalizePathKey(path);
				const bool bitmap = hasSuffix(normalized, L".tnvfmask");
				const bool manifest = hasSuffix(normalized, L".tnvfmanifest");
				const bool atlas = hasSuffix(normalized, L".tnvfatlas");
				const bool direct = hasSuffix(normalized, L".tnvfdirect");
				const bool temporary =
					hasSuffix(normalized, L".tnvfmask.tmp")
					|| hasSuffix(normalized, L".tnvfmanifest.tmp")
					|| hasSuffix(normalized, L".tnvfatlas.tmp")
					|| hasSuffix(normalized, L".tnvfatlas.stream.tmp")
					|| hasSuffix(normalized, L".tnvfdirect.tmp");
				bool remove = temporary;
				if (bitmap)
				{
					const PersistentCacheCleanupClass cleanupClass =
						ClassifyPersistentBitmapCache(path);
					remove = cleanupClass
							== PersistentCacheCleanupClass::InactiveDistanceField
						|| cleanupClass == PersistentCacheCleanupClass::Invalid;
				}
				else if (manifest)
				{
					const PersistentCacheCleanupClass cleanupClass =
						ClassifyPersistentGlyphManifestCache(path);
					remove = cleanupClass
							== PersistentCacheCleanupClass::InactiveDistanceField
						|| cleanupClass == PersistentCacheCleanupClass::Invalid;
				}
				else if (atlas)
				{
					const PersistentCacheCleanupClass cleanupClass =
						ClassifyAtlasSnapshotCacheForCleanup(path);
					remove = cleanupClass
							== PersistentCacheCleanupClass::InactiveDistanceField
						|| cleanupClass == PersistentCacheCleanupClass::Invalid;
				}
				else if (direct)
				{
					const PersistentCacheCleanupClass cleanupClass =
						ClassifyDirectCachedLetterCacheForCleanup(path);
					remove = cleanupClass
							== PersistentCacheCleanupClass::InactiveDistanceField
						|| cleanupClass == PersistentCacheCleanupClass::Invalid;
				}
				if (!remove)
					continue;

				const UInt64 bytes =
					(static_cast<UInt64>(found.nFileSizeHigh) << 32)
					| found.nFileSizeLow;
				if (DeleteFileW(path.c_str())
					|| IsMissingPersistentCacheFileError(GetLastError()))
				{
					deletedBytes += bytes;
					state.usedPersistentCachePaths.erase(normalized);
					if (temporary)
						++deletedTemporary;
					else if (bitmap)
						++deletedMasks;
					else if (manifest)
						++deletedManifests;
					else if (atlas)
						++deletedAtlases;
					else if (direct)
						++deletedDirect;
				}
				else
				{
					// A failed unlink must not leave a reusable distance-field
					// header. Truncation preserves the logical invalidation even
					// when antivirus or another reader briefly holds the name.
					HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
						FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
						nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
					const bool invalidated = file != INVALID_HANDLE_VALUE
						&& SetFileSize64(file, 0) && FlushFileBuffers(file);
					if (file != INVALID_HANDLE_VALUE)
						CloseHandle(file);
					if (!invalidated)
						++failed;
				}
			} while (FindNextFileW(search, &found));
			FindClose(search);
		}

		gLog.FormattedMessage(
			"tnvse_freetype_font: inactive cache invalidated route=%s previous=%s masks=%u manifests=%u atlases=%u direct=%u temporary=%u bytes=%llu failed=%u",
			route == FontAtlasRoute::BakedArgbComposite
				? "argb-composite" : "argb-fallback",
			wasSynchronized
				? (previousRoute == FontAtlasRoute::ShaderDistanceField
					? "distance-field"
					: previousRoute == FontAtlasRoute::BakedArgbComposite
						? "argb-composite" : "argb-fallback")
				: "unresolved",
			deletedMasks, deletedManifests, deletedAtlases, deletedDirect,
			deletedTemporary,
			static_cast<unsigned long long>(deletedBytes), failed);
	}

	void DeleteUnusedFreeTypeFontCacheFiles(bool deleteAllUnused)
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		std::wstring directory;
		if (!EnsurePersistentBitmapDirectory(directory))
			return;
		const std::wstring pattern = directory + L"\\*";
		WIN32_FIND_DATAW found = {};
		HANDLE search = FindFirstFileW(pattern.c_str(), &found);
		if (search == INVALID_HANDLE_VALUE)
			return;
		UInt32 deleted = 0;
		UInt32 failed = 0;
		UInt32 staleMode = 0;
		UInt32 invalid = 0;
		UInt32 temporary = 0;
		UInt32 deferred = 0;
		UInt64 deletedBytes = 0;
		auto hasSuffix = [](const std::wstring& value, const wchar_t* suffix)
		{
			const size_t length = std::wcslen(suffix);
			return value.size() >= length
				&& value.compare(value.size() - length, length, suffix) == 0;
		};
		do
		{
			if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			const std::wstring path = directory + L"\\" + found.cFileName;
			const std::wstring normalized = NormalizePathKey(path);
			const bool bitmap = hasSuffix(normalized, L".tnvfmask");
			const bool fontHash = hasSuffix(normalized, L".tnvfhash");
			const bool manifest = hasSuffix(normalized, L".tnvfmanifest");
			const bool atlas = hasSuffix(normalized, L".tnvfatlas");
			const bool direct = hasSuffix(normalized, L".tnvfdirect");
			const bool temporaryFile =
				hasSuffix(normalized, L".tnvfmask.tmp")
				|| hasSuffix(normalized, L".tnvfhash.tmp")
				|| hasSuffix(normalized, L".tnvfmanifest.tmp")
				|| hasSuffix(normalized, L".tnvfatlas.tmp")
				|| hasSuffix(normalized, L".tnvfatlas.stream.tmp")
				|| hasSuffix(normalized, L".tnvfdirect.tmp");
			const bool managed = bitmap || fontHash || manifest || atlas || direct
				|| temporaryFile;
			if (!managed)
				continue;
			PersistentCacheCleanupClass cleanupClass =
				PersistentCacheCleanupClass::Neutral;
			if (!temporaryFile)
			{
				if (bitmap)
					cleanupClass = ClassifyPersistentBitmapCache(path);
				else if (manifest)
					cleanupClass = ClassifyPersistentGlyphManifestCache(path);
				else if (atlas)
					cleanupClass =
						ClassifyAtlasSnapshotCacheForCleanup(path);
				else if (direct)
					cleanupClass =
						ClassifyDirectCachedLetterCacheForCleanup(path);
			}
			const bool used =
				State().usedPersistentCachePaths.count(normalized) != 0;
			if (used
				&& cleanupClass !=
					PersistentCacheCleanupClass::InactiveDistanceField
				&& cleanupClass != PersistentCacheCleanupClass::Invalid
				&& !temporaryFile)
			{
				continue;
			}
			if (!deleteAllUnused && !temporaryFile)
			{
				if (cleanupClass == PersistentCacheCleanupClass::Neutral
					|| cleanupClass
						== PersistentCacheCleanupClass::CurrentDistanceField)
				{
					++deferred;
					continue;
				}
			}
			if (temporaryFile)
				++temporary;
			else if (cleanupClass
				== PersistentCacheCleanupClass::InactiveDistanceField)
				++staleMode;
			else if (cleanupClass == PersistentCacheCleanupClass::Invalid)
				++invalid;
			const UInt64 size = (static_cast<UInt64>(found.nFileSizeHigh) << 32)
				| found.nFileSizeLow;
			if (DeleteFileW(path.c_str()))
			{
				++deleted;
				deletedBytes += size;
				State().usedPersistentCachePaths.erase(normalized);
			}
			else
			{
				++failed;
			}
		} while (FindNextFileW(search, &found));
		FindClose(search);
		gLog.FormattedMessage(
			"tnvse_freetype_font: unused persistent cache cleanup scope=%s deleted=%u bytes=%llu failed=%u staleMode=%u invalid=%u temporary=%u deferred=%u used=%llu",
			deleteAllUnused ? "full" : "inactive-only",
			deleted, static_cast<unsigned long long>(deletedBytes), failed,
			staleMode, invalid, temporary, deferred,
			static_cast<unsigned long long>(State().usedPersistentCachePaths.size()));
	}

	void MarkGlyphManifestComplete(RuntimeFont& runtime)
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		PersistentGlyphManifest* manifest = GetGlyphManifest(runtime);
		if (!manifest || !manifest->mappedData || !manifest->writable)
			return;
		auto* header = reinterpret_cast<PersistentGlyphManifestHeader*>(
			manifest->mappedData);
		if (header->completeMode >= kCompleteCodePagePrewarmIdentity)
		{
			if (!manifest->validatedRecordIndexReady)
				BuildValidatedGlyphManifestIndex(*manifest, runtime);
			return;
		}
		header->completeMode = kCompleteCodePagePrewarmIdentity;
		header->checksum = HashBytes64(header,
			offsetof(PersistentGlyphManifestHeader, checksum));
		FlushViewOfFile(header, sizeof(*header));
		BuildValidatedGlyphManifestIndex(*manifest, runtime);
	}

	bool MarkCurrentFallbackBitmapProfilesUsed(RuntimeFont& runtime,
		float rasterScale)
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		if (GetPersistentFontCacheRoute() != FontAtlasRoute::ArgbFallback)
			return true;
		PersistentGlyphManifest* manifest = GetGlyphManifest(runtime);
		if (!manifest || !manifest->mappedData
			|| !manifest->validatedRecordIndexReady)
		{
			return false;
		}
		const auto* header = reinterpret_cast<
			const PersistentGlyphManifestHeader*>(manifest->mappedData);
		if (header->completeMode < kCompleteCodePagePrewarmIdentity)
			return false;

		std::array<std::vector<bool>, 2> usedFaces;
		for (size_t roleIndex = 0; roleIndex < usedFaces.size(); ++roleIndex)
			usedFaces[roleIndex].assign(runtime.roles[roleIndex].faces.size(), false);
		const auto* records = reinterpret_cast<
			const PersistentGlyphManifestRecord*>(manifest->mappedData
				+ sizeof(PersistentGlyphManifestHeader));
		for (UInt32 index = 0; index < manifest->recordCount; ++index)
		{
			const PersistentGlyphManifestEntry& entry = records[index].entry;
			const size_t roleIndex = entry.byteClass;
			if (!entry.valid || roleIndex >= usedFaces.size()
				|| entry.faceIndex >= usedFaces[roleIndex].size())
			{
				continue;
			}
			usedFaces[roleIndex][entry.faceIndex] = true;
		}

		const FontConfig& config = GetRuntimeConfig(runtime);
		std::array<GlyphMaskType, 4> masks = {
			GlyphMaskType::Fill,
			GlyphMaskType::Shadow,
			GlyphMaskType::Glow,
			GlyphMaskType::Outline
		};
		const float safeScale = std::isfinite(rasterScale)
				&& rasterScale >= 0.1f && rasterScale <= 10.0f
			? rasterScale : 1.0f;
		UInt32 requiredProfiles = 0;
		for (size_t roleIndex = 0; roleIndex < runtime.roles.size(); ++roleIndex)
		{
			RuntimeRole& role = runtime.roles[roleIndex];
			if (!role.style)
				return false;
			const ByteStyle& style = *role.style;
			const UInt16 effectiveWidth = static_cast<UInt16>(std::clamp(
				static_cast<int>(std::lround(
					style.pixelSize * style.scaleX * safeScale)), 1, 65535));
			const UInt16 effectiveHeight = static_cast<UInt16>(std::clamp(
				static_cast<int>(std::lround(
					style.pixelSize * style.scaleY * safeScale)), 1, 65535));
			const SInt32 embolden = static_cast<SInt32>(std::lround(
				style.embolden * safeScale * 64.0f));
			const float slant = std::tan(style.slantDegrees
				* 3.14159265358979323846f / 180.0f);
			const SInt32 slant16Dot16 = static_cast<SInt32>(std::lround(
				slant * 65536.0f));
			for (size_t faceIndex = 0; faceIndex < role.faces.size(); ++faceIndex)
			{
				if (!usedFaces[roleIndex][faceIndex])
					continue;
				RuntimeFace& face = role.faces[faceIndex];
				if (!face.file || !face.ftFace)
					return false;
				for (GlyphMaskType mask : masks)
				{
					if ((mask == GlyphMaskType::Shadow
							&& !config.shadow.enabled)
						|| (mask == GlyphMaskType::Glow
							&& !config.glow.enabled)
						|| (mask == GlyphMaskType::Outline
							&& !config.outline.enabled))
					{
						continue;
					}
					const PersistentBitmapProfileKey key = {
						face.file->contentHash,
						static_cast<SInt32>(face.ftFace->face_index),
						GetFreeTypeTextCodePage(),
						effectiveWidth,
						effectiveHeight,
						embolden,
						ResolveCpuEffectMaskIdentity(config, mask, safeScale),
						slant16Dot16,
						0,
						static_cast<UInt8>(mask),
						0
					};
					PersistentBitmapProfile* profile =
						GetPersistentBitmapProfile(key, face.file->path,
							config.fontId, static_cast<UInt32>(
								std::max<FT_Long>(1, face.ftFace->num_glyphs)));
					if (!profile || !profile->initialized
						|| !profile->recordCount || profile->path.empty())
					{
						return false;
					}
					MarkFreeTypeFontCacheFileUsed(profile->path);
					++requiredProfiles;
				}
			}
		}
		if (!requiredProfiles)
			return false;
		gLog.FormattedMessage(
			"tnvse_freetype_font: fallback persistent masks retained font=%u profiles=%u",
			config.fontId, requiredProfiles);
		return true;
	}

	void BeginCompleteCodePageAtlasOnlyPrewarm()
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		if (State().completeCodePageAtlasOnlyPrewarm)
			return;
		State().completeCodePageAtlasOnlyPrewarm = true;
		gLog.FormattedMessage(
			"tnvse_freetype_font: complete codepage atlas-only transaction begin persistentMasks=disabled");
	}

	void EndCompleteCodePageAtlasOnlyPrewarm()
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		if (!State().completeCodePageAtlasOnlyPrewarm)
			return;
		State().completeCodePageAtlasOnlyPrewarm = false;
		gLog.FormattedMessage(
			"tnvse_freetype_font: complete codepage atlas-only transaction end persistentMasks=runtime-policy");
	}

	void FlushGlyphBitmapDiskCache()
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		UInt32 profileCount = 0;
		UInt64 recordCount = 0;
		UInt64 byteCount = 0;
		for (auto& pair : State().persistentBitmapProfiles)
		{
			PersistentBitmapProfile& profile = *pair.second;
			if (!profile.initialized || profile.file == INVALID_HANDLE_VALUE)
				continue;
			if (profile.writable)
				FlushFileBuffers(profile.file);
			++profileCount;
			recordCount += profile.recordCount;
			byteCount += profile.validSize;
		}
		std::unordered_set<PersistentGlyphManifest*> flushedManifests;
		for (auto& pair : State().runtimeFonts)
		{
			RuntimeFont& runtime = *pair.second;
			if (!runtime.manifest || !runtime.manifest->mappedData)
				continue;
			if (!flushedManifests.insert(runtime.manifest.get()).second)
				continue;
			FlushViewOfFile(runtime.manifest->mappedData, 0);
			if (runtime.manifest->writable)
				FlushFileBuffers(runtime.manifest->file);
		}
		if (profileCount)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: persistent bitmap cache flushed profiles=%u records=%llu bytes=%llu",
				profileCount, static_cast<unsigned long long>(recordCount),
				static_cast<unsigned long long>(byteCount));
		}
	}

	UInt64 ReleaseGlyphBitmapDiskCacheMappings()
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		State().persistentBitmapMappingsEnabled = false;
		UInt32 profileCount = 0;
		UInt64 byteCount = 0;
		for (auto& pair : State().persistentBitmapProfiles)
		{
			PersistentBitmapProfile& profile = *pair.second;
			if (!profile.mappedData)
				continue;
			++profileCount;
			byteCount += profile.mappedSize;
			UnmapPersistentBitmapProfile(profile);
		}
		gLog.FormattedMessage(
			"tnvse_freetype_font: persistent bitmap mappings released profiles=%u bytes=%llu",
			profileCount, static_cast<unsigned long long>(byteCount));
		ReportCpuMemoryBudget("persistent-mappings-released", true);
		return byteCount;
	}

	bool ResetPersistentFontCachesForRegeneration(RuntimeFont& runtime)
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		std::shared_ptr<PersistentGlyphManifest> manifest = runtime.manifest;
		if (!manifest)
		{
			GetGlyphManifest(runtime);
			manifest = runtime.manifest;
		}

		std::wstring manifestPath;
		UInt64 manifestHash = 0;
		bool manifestInvalidated = true;
		UInt32 detachedRuntimes = 0;
		if (manifest)
		{
			manifestPath = manifest->path;
			manifestHash = manifest->manifestHash;
			manifestInvalidated = manifestPath.empty();
			for (auto& pair : State().runtimeFonts)
			{
				RuntimeFont& candidate = *pair.second;
				if (candidate.manifest != manifest)
					continue;
				candidate.manifest.reset();
				candidate.codePageMetrics.reset();
				size_t runtimeBytes = sizeof(RuntimeFont);
				for (RuntimeRole& role : candidate.roles)
				{
					role.glyphIdentities.clear();
					runtimeBytes += role.faces.capacity() * sizeof(RuntimeFace);
				}
				candidate.cpuMemory.Reset(CpuMemoryCategory::RuntimeMetadata,
					runtimeBytes);
				auto extra = gNumberedExtraLetters.find(pair.first);
				if (extra != gNumberedExtraLetters.end())
					extra->second.generatedCodePage.reset();
				++detachedRuntimes;
			}
			State().persistentGlyphManifests.erase(manifestHash);
			UnmapGlyphManifest(*manifest);
			if (manifest->file != INVALID_HANDLE_VALUE)
			{
				// Truncate before closing so a failed DeleteFile cannot leave a valid
				// incomplete manifest available to the next GetGlyphManifest call.
				manifestInvalidated = manifestInvalidated || (manifest->writable
					&& SetFileSize64(manifest->file, 0)
					&& FlushFileBuffers(manifest->file));
				CloseHandle(manifest->file);
				manifest->file = INVALID_HANDLE_VALUE;
			}
			manifest->writable = false;
			if (!manifestPath.empty())
			{
				const bool deleted = DeleteFileW(manifestPath.c_str())
					|| IsMissingPersistentCacheFileError(GetLastError());
				manifestInvalidated = manifestInvalidated || deleted;
				State().usedPersistentCachePaths.erase(
					NormalizePathKey(manifestPath));
			}
		}

		// A code-page rebuild is an all-or-nothing transaction. Mask profiles are
		// content-addressed and can be shared by font aliases, so clear the complete
		// construction tier instead of risking a mixture of old and fresh records.
		State().bitmapCache.clear();
		State().bitmapLru.clear();
		State().bitmapCacheBytes = 0;
		State().persistentBitmapProfiles.clear();
		State().atlasOnlyCodePageFontIds.clear();
		State().persistentBitmapMappingsEnabled = true;
		State().bitmapCacheReducedAfterPrewarm = false;

		UInt32 deletedMasks = 0;
		UInt32 failedMasks = 0;
		UInt64 deletedMaskBytes = 0;
		std::wstring directory;
		if (EnsurePersistentBitmapDirectory(directory))
		{
			const std::wstring pattern = directory + L"\\*.tnvfmask";
			WIN32_FIND_DATAW found = {};
			HANDLE search = FindFirstFileW(pattern.c_str(), &found);
			if (search != INVALID_HANDLE_VALUE)
			{
				do
				{
					if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
						continue;
					const std::wstring path = directory + L"\\" + found.cFileName;
					const UInt64 bytes = (static_cast<UInt64>(found.nFileSizeHigh) << 32)
						| found.nFileSizeLow;
					if (DeleteFileW(path.c_str())
						|| IsMissingPersistentCacheFileError(GetLastError()))
					{
						++deletedMasks;
						deletedMaskBytes += bytes;
						State().usedPersistentCachePaths.erase(NormalizePathKey(path));
					}
					else
					{
						++failedMasks;
					}
				} while (FindNextFileW(search, &found));
				FindClose(search);
			}
		}
		if (failedMasks)
		{
			// Do not reopen a residual mask that Windows refused to remove. The
			// current rebuild can still rasterize directly into the streamed atlas.
			State().persistentBitmapUnavailable = true;
		}
		else
		{
			State().persistentBitmapUnavailable = false;
		}
		gLog.FormattedMessage(
			"tnvse_freetype_font: incomplete persistent cache reset font=%u detachedRuntimes=%u manifest=%s masksDeleted=%u maskBytes=%llu masksFailed=%u diskMasks=%s",
			GetRuntimeConfig(runtime).fontId, detachedRuntimes,
			manifestInvalidated ? "discarded" : "failed",
			deletedMasks, static_cast<unsigned long long>(deletedMaskBytes),
			failedMasks, failedMasks ? "disabled" : "fresh");
		return manifestInvalidated;
	}

	bool DeleteCompleteCodePageGlyphBitmapDiskCaches(
		const std::vector<UInt32>& fontIds)
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		if (fontIds.empty())
			return false;
		const std::unordered_set<UInt32> completed(fontIds.begin(), fontIds.end());
		State().atlasOnlyCodePageFontIds.insert(completed.begin(), completed.end());

		std::unordered_map<std::wstring, std::wstring> candidates;
		for (auto profile = State().persistentBitmapProfiles.begin();
			profile != State().persistentBitmapProfiles.end();)
		{
			PersistentBitmapProfile& value = *profile->second;
			if (!value.path.empty())
				candidates.emplace(NormalizePathKey(value.path), value.path);
			UnmapPersistentBitmapProfile(value);
			if (value.file != INVALID_HANDLE_VALUE)
			{
				CloseHandle(value.file);
				value.file = INVALID_HANDLE_VALUE;
			}
			profile = State().persistentBitmapProfiles.erase(profile);
		}

		std::wstring directory;
		if (EnsurePersistentBitmapDirectory(directory))
		{
			const std::wstring pattern = directory + L"\\*.tnvfmask";
			WIN32_FIND_DATAW found = {};
			HANDLE search = FindFirstFileW(pattern.c_str(), &found);
			if (search != INVALID_HANDLE_VALUE)
				{
					do
					{
						if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
							continue;
						const std::wstring path = directory + L"\\" + found.cFileName;
					candidates.emplace(NormalizePathKey(path), path);
				} while (FindNextFileW(search, &found));
				FindClose(search);
			}
		}

		UInt32 deleted = 0;
		UInt32 failed = 0;
		UInt64 deletedBytes = 0;
		for (const auto& [normalized, path] : candidates)
		{
			WIN32_FILE_ATTRIBUTE_DATA attributes = {};
			UInt64 bytes = 0;
			if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
			{
				bytes = (static_cast<UInt64>(attributes.nFileSizeHigh) << 32)
					| attributes.nFileSizeLow;
			}
			if (DeleteFileW(path.c_str())
				|| IsMissingPersistentCacheFileError(GetLastError()))
			{
				++deleted;
				deletedBytes += bytes;
				State().usedPersistentCachePaths.erase(normalized);
			}
			else
			{
				++failed;
			}
		}
		gLog.FormattedMessage(
			"tnvse_freetype_font: complete codepage atlas-only mask cleanup fonts=%u deleted=%u bytes=%llu failed=%u",
			static_cast<UInt32>(completed.size()), deleted,
			static_cast<unsigned long long>(deletedBytes), failed);
		return failed == 0;
	}
}

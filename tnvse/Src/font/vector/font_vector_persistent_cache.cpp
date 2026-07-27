#include "font_freetype_internal.h"

#include "encoding.h"
#include "font_glyphs.h"
#include "globals.h"

#include <winioctl.h>

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
			IsA8RendererAvailable(),
			g_bEnableFreeTypeFontAggressivePerformanceMode));
	}

	FontAtlasRoute GetPersistentFontCacheRoute()
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		if (State().persistentCacheRouteSynchronized)
			return State().persistentCacheRoute;
		return ResolveFontAtlasRoute(IsA8RendererAvailable(),
			g_bEnableFreeTypeFontAggressivePerformanceMode);
	}

	static UInt32 MaximumPersistentBitmapBytes(GlyphMaskType maskType,
		DistanceFieldMethod distanceFieldMethod)
	{
		return maskType == GlyphMaskType::Composite
			? kMaximumPersistentMtsdfBitmapBytes
			: maskType == GlyphMaskType::DistanceField
			? (distanceFieldMethod == DistanceFieldMethod::Mtsdf
				? kMaximumPersistentMtsdfBitmapBytes
				: kMaximumPersistentSingleChannelBitmapBytes)
			: kMaximumPersistentSingleChannelBitmapBytes;
	}

	void RefreshPersistentBitmapProfileCpuMemory(
		PersistentBitmapProfile& profile)
	{
		const size_t mappedBytes = profile.mappedSize
			<= std::numeric_limits<size_t>::max()
			? static_cast<size_t>(profile.mappedSize)
			: std::numeric_limits<size_t>::max();
		profile.cpuMemory.Reset(CpuMemoryCategory::PersistentMapping,
			sizeof(PersistentBitmapProfile)
				+ (profile.fontFileName.capacity() + profile.path.capacity())
					* sizeof(wchar_t)
				+ profile.indexEntries.capacity()
					* sizeof(PersistentBitmapIndexEntry)
				+ mappedBytes);
	}

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

		UInt64 HashBitmapKey(const BitmapCacheKey& key)
		{
			UInt64 hash = 1469598103934665603ull;
			auto add = [&](const void* data, size_t size)
			{
				const UInt8* bytes = static_cast<const UInt8*>(data);
				for (size_t i = 0; i < size; ++i)
				{
					hash ^= bytes[i];
					hash *= 1099511628211ull;
				}
			};
			add(&key.fontContentHash, sizeof(key.fontContentHash));
			add(&key.fontFaceIndex, sizeof(key.fontFaceIndex));
			add(&key.glyphIndex, sizeof(key.glyphIndex));
			add(&key.codePage, sizeof(key.codePage));
			add(&key.effectiveWidth, sizeof(key.effectiveWidth));
			add(&key.effectiveHeight, sizeof(key.effectiveHeight));
			add(&key.embolden26Dot6, sizeof(key.embolden26Dot6));
			add(&key.strokeWidth26Dot6, sizeof(key.strokeWidth26Dot6));
			add(&key.slant16Dot16, sizeof(key.slant16Dot16));
			add(&key.sdfSpread, sizeof(key.sdfSpread));
			add(&key.maskType, sizeof(key.maskType));
			if (key.maskType == static_cast<UInt8>(GlyphMaskType::DistanceField))
			{
				add(&key.distanceFieldMethod, sizeof(key.distanceFieldMethod));
				const UInt32 revision = DistanceFieldGeneratorRevision(
					static_cast<DistanceFieldMethod>(key.distanceFieldMethod));
				add(&revision, sizeof(revision));
			}
			else if (key.maskType
				== static_cast<UInt8>(GlyphMaskType::Composite))
			{
				add(&kCpuCompositeRasterRevision,
					sizeof(kCpuCompositeRasterRevision));
			}
			return hash;
		}

		UInt64 HashPersistentBitmapProfileKey(
			const PersistentBitmapProfileKey& key)
		{
			UInt64 hash = HashBytes64(&kPersistentBitmapVersion,
				sizeof(kPersistentBitmapVersion));
			hash = HashBytes64(&key.fontContentHash, sizeof(key.fontContentHash), hash);
			hash = HashBytes64(&key.fontFaceIndex, sizeof(key.fontFaceIndex), hash);
			hash = HashBytes64(&key.codePage, sizeof(key.codePage), hash);
			hash = HashBytes64(&key.effectiveWidth, sizeof(key.effectiveWidth), hash);
			hash = HashBytes64(&key.effectiveHeight, sizeof(key.effectiveHeight), hash);
			hash = HashBytes64(&key.embolden26Dot6, sizeof(key.embolden26Dot6), hash);
			hash = HashBytes64(&key.strokeWidth26Dot6, sizeof(key.strokeWidth26Dot6), hash);
			hash = HashBytes64(&key.slant16Dot16, sizeof(key.slant16Dot16), hash);
			hash = HashBytes64(&key.sdfSpread, sizeof(key.sdfSpread), hash);
			hash = HashBytes64(&key.maskType, sizeof(key.maskType), hash);
			if (key.maskType == static_cast<UInt8>(GlyphMaskType::DistanceField))
			{
				hash = HashBytes64(&key.distanceFieldMethod,
					sizeof(key.distanceFieldMethod), hash);
				const UInt32 revision = DistanceFieldGeneratorRevision(
					static_cast<DistanceFieldMethod>(key.distanceFieldMethod));
				hash = HashBytes64(&revision, sizeof(revision), hash);
			}
			else if (key.maskType
				== static_cast<UInt8>(GlyphMaskType::Composite))
			{
				hash = HashBytes64(&kCpuCompositeRasterRevision,
					sizeof(kCpuCompositeRasterRevision), hash);
			}
			return hash;
		}

		PersistentBitmapProfileKey MakePersistentBitmapProfileKey(
			const BitmapCacheKey& key, UInt64 fontContentHash)
		{
			return {
				fontContentHash,
				key.fontFaceIndex,
				key.codePage,
				key.effectiveWidth,
				key.effectiveHeight,
				key.embolden26Dot6,
				key.strokeWidth26Dot6,
				key.slant16Dot16,
				key.sdfSpread,
				key.maskType,
				key.distanceFieldMethod
			};
		}

		PersistentBitmapFileHeader MakePersistentBitmapFileHeader(
			const PersistentBitmapProfileKey& key, UInt64 profileHash,
			UInt32 glyphCapacity)
		{
			PersistentBitmapFileHeader header;
			const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'M', 'S', 'K', '1' };
			std::memcpy(header.magic, magic, sizeof(magic));
			header.version = kPersistentBitmapVersion;
			header.headerSize = sizeof(header);
			header.profileHash = profileHash;
			header.fontContentHash = key.fontContentHash;
			header.fontFaceIndex = key.fontFaceIndex;
			header.codePage = key.codePage;
			header.effectiveWidth = key.effectiveWidth;
			header.effectiveHeight = key.effectiveHeight;
			header.embolden26Dot6 = key.embolden26Dot6;
			header.strokeWidth26Dot6 = key.strokeWidth26Dot6;
			header.slant16Dot16 = key.slant16Dot16;
			header.sdfSpread = key.sdfSpread;
			header.maskType = key.maskType;
			header.distanceFieldMethod = key.distanceFieldMethod;
			header.glyphCapacity = glyphCapacity;
			header.indexEntrySize = sizeof(PersistentBitmapIndexEntry);
			header.dataOffset = sizeof(PersistentBitmapFileHeader)
				+ static_cast<UInt64>(glyphCapacity) * sizeof(PersistentBitmapIndexEntry);
			header.checksum = HashBytes64(&header,
				offsetof(PersistentBitmapFileHeader, checksum));
			return header;
		}

		bool MatchesPersistentBitmapFileHeader(
			const PersistentBitmapFileHeader& header,
			const PersistentBitmapProfileKey& key, UInt64 profileHash,
			UInt32 glyphCapacity)
		{
			const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'M', 'S', 'K', '1' };
			return std::memcmp(header.magic, magic, sizeof(magic)) == 0
				&& header.version == kPersistentBitmapVersion
				&& header.headerSize == sizeof(header)
				&& header.profileHash == profileHash
				&& header.fontContentHash == key.fontContentHash
				&& header.fontFaceIndex == key.fontFaceIndex
				&& header.codePage == key.codePage
				&& header.effectiveWidth == key.effectiveWidth
				&& header.effectiveHeight == key.effectiveHeight
				&& header.embolden26Dot6 == key.embolden26Dot6
				&& header.strokeWidth26Dot6 == key.strokeWidth26Dot6
				&& header.slant16Dot16 == key.slant16Dot16
				&& header.sdfSpread == key.sdfSpread
				&& header.maskType == key.maskType
				&& header.distanceFieldMethod == key.distanceFieldMethod
				&& header.glyphCapacity == glyphCapacity
				&& header.indexEntrySize == sizeof(PersistentBitmapIndexEntry)
				&& header.dataOffset == sizeof(PersistentBitmapFileHeader)
					+ static_cast<UInt64>(glyphCapacity) * sizeof(PersistentBitmapIndexEntry)
				&& header.checksum == HashBytes64(&header,
					offsetof(PersistentBitmapFileHeader, checksum));
		}

		std::wstring GetPersistentBitmapDirectory()
		{
			std::array<wchar_t, MAX_PATH> modulePath = {};
			const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(),
				static_cast<DWORD>(modulePath.size()));
			if (!length || length >= modulePath.size())
				return {};
			std::wstring gameDirectory(modulePath.data(), length);
			const size_t slash = gameDirectory.find_last_of(L"\\/");
			if (slash == std::wstring::npos)
				return {};
			gameDirectory.resize(slash);
			return gameDirectory + L"\\Data\\NVSE\\plugins\\tnvse\\fontdata";
		}

		std::wstring SanitizePersistentBitmapFontName(const std::wstring& path)
		{
			const size_t slash = path.find_last_of(L"\\/");
			std::wstring name = slash == std::wstring::npos
				? path : path.substr(slash + 1);
			if (name.empty())
				name = L"font";
			for (wchar_t& character : name)
			{
				if (character < 0x20 || std::wcschr(L"<>:\"/\\|?*", character))
					character = L'_';
			}
			constexpr size_t kMaximumPersistentFontNameLength = 80;
			if (name.size() > kMaximumPersistentFontNameLength)
				name.resize(kMaximumPersistentFontNameLength);
			return name;
		}

		bool IsPersistentBitmapFontIdName(const wchar_t* fileName)
		{
			if (!fileName)
				return false;
			wchar_t* end = nullptr;
			const unsigned long fontId = std::wcstoul(fileName, &end, 10);
			return end != fileName && end && *end == L'_'
				&& fontId <= std::numeric_limits<UInt32>::max();
		}

		std::wstring FindPersistentBitmapByHash(const std::wstring& directory,
			UInt64 profileHash)
		{
			wchar_t pattern[160] = {};
			_snwprintf_s(pattern, _countof(pattern), _TRUNCATE,
				L"%ls\\*_%016llX.tnvfmask", directory.c_str(),
				static_cast<unsigned long long>(profileHash));
			WIN32_FIND_DATAW found = {};
			HANDLE search = FindFirstFileW(pattern, &found);
			if (search == INVALID_HANDLE_VALUE)
				return {};
			std::wstring path;
			do
			{
				if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					&& IsPersistentBitmapFontIdName(found.cFileName))
				{
					path = directory + L"\\" + found.cFileName;
					break;
				}
			} while (FindNextFileW(search, &found));
			FindClose(search);
			return path;
		}

		std::wstring FormatPersistentBitmapPath(const std::wstring& directory,
			const PersistentBitmapProfile& profile)
		{
			wchar_t fileName[256] = {};
			_snwprintf_s(fileName, _countof(fileName), _TRUNCATE,
				L"%u_%ls_%016llX.tnvfmask", profile.fontId,
				profile.fontFileName.c_str(),
				static_cast<unsigned long long>(profile.profileHash));
			return directory + L"\\" + fileName;
		}

		bool DirectoryExists(const std::wstring& path)
		{
			const DWORD attributes = GetFileAttributesW(path.c_str());
			return attributes != INVALID_FILE_ATTRIBUTES
				&& (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		}

		bool EnsurePersistentBitmapDirectory(std::wstring& directory)
		{
			directory = GetPersistentBitmapDirectory();
			if (directory.empty())
				return false;
			const size_t suffixLength = std::wcslen(
				L"\\Data\\NVSE\\plugins\\tnvse\\fontdata");
			if (directory.size() <= suffixLength)
				return false;
			const std::wstring gameDirectory = directory.substr(
				0, directory.size() - suffixLength);
			const std::array<std::wstring, 5> paths = {
				gameDirectory + L"\\Data",
				gameDirectory + L"\\Data\\NVSE",
				gameDirectory + L"\\Data\\NVSE\\plugins",
				gameDirectory + L"\\Data\\NVSE\\plugins\\tnvse",
				directory
			};
			for (const std::wstring& path : paths)
			{
				if (!DirectoryExists(path)
					&& !CreateDirectoryW(path.c_str(), nullptr)
					&& GetLastError() != ERROR_ALREADY_EXISTS)
				{
					return false;
				}
			}
			if (!State().loggedPersistentBitmapDirectory)
			{
				State().loggedPersistentBitmapDirectory = true;
				gLog.FormattedMessage(
					"tnvse_freetype_font: persistent bitmap cache directory=%ls version=%u",
					directory.c_str(), kPersistentBitmapVersion);
			}
			return true;
		}

		bool GetFileSize64(HANDLE file, UInt64& size)
		{
			LARGE_INTEGER value = {};
			if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &value)
				|| value.QuadPart < 0)
			{
				return false;
			}
			size = static_cast<UInt64>(value.QuadPart);
			return true;
		}

		bool SetFileSize64(HANDLE file, UInt64 size)
		{
			if (file == INVALID_HANDLE_VALUE
				|| size > static_cast<UInt64>(std::numeric_limits<LONGLONG>::max()))
			{
				return false;
			}
			FILE_END_OF_FILE_INFO endOfFile = {};
			endOfFile.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
			return SetFileInformationByHandle(file, FileEndOfFileInfo,
				&endOfFile, sizeof(endOfFile)) != FALSE;
		}

		bool TryEnableSparseFile(HANDLE file)
		{
			if (file == INVALID_HANDLE_VALUE)
				return false;
			DWORD bytesReturned = 0;
			return DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0,
				nullptr, 0, &bytesReturned, nullptr) != FALSE;
		}

		bool ReadFileAt(HANDLE file, UInt64 offset, void* data, UInt32 size)
		{
			if (!size)
				return true;
			if (file == INVALID_HANDLE_VALUE)
				return false;
			// All callers use synchronous file handles. Supplying an OVERLAPPED
			// offset keeps the read synchronous while avoiding a separate seek and
			// any dependency on the handle's current file pointer.
			OVERLAPPED operation = {};
			operation.Offset = static_cast<DWORD>(offset);
			operation.OffsetHigh = static_cast<DWORD>(offset >> 32);
			DWORD read = 0;
			return ReadFile(file, data, size, &read, &operation)
				&& read == size;
		}

		bool WriteFileAt(HANDLE file, UInt64 offset, const void* data, UInt32 size)
		{
			if (!size)
				return true;
			if (file == INVALID_HANDLE_VALUE)
				return false;
			// Match ReadFileAt: all callers use synchronous handles, so an explicit
			// OVERLAPPED offset avoids a separate seek without making the write
			// asynchronous.
			OVERLAPPED operation = {};
			operation.Offset = static_cast<DWORD>(offset);
			operation.OffsetHigh = static_cast<DWORD>(offset >> 32);
			DWORD written = 0;
			return WriteFile(file, data, size, &written, &operation)
				&& written == size;
		}

		bool ResolvePersistentFontContentHash(MappedFontFile& mapped,
			const std::wstring& normalizedPath)
		{
			BY_HANDLE_FILE_INFORMATION information = {};
			if (mapped.file == INVALID_HANDLE_VALUE
				|| !GetFileInformationByHandle(mapped.file, &information))
				return false;
			const UInt64 pathHash = HashBytes64(normalizedPath.data(),
				normalizedPath.size() * sizeof(wchar_t));
			std::wstring directory;
			if (!EnsurePersistentBitmapDirectory(directory))
				return false;
			wchar_t fileName[256] = {};
			const std::wstring fontName = SanitizePersistentBitmapFontName(mapped.path);
			_snwprintf_s(fileName, _countof(fileName), _TRUNCATE,
				L"hash_%ls_%016llX.tnvfhash", fontName.c_str(),
				static_cast<unsigned long long>(pathHash));
			const std::wstring cachePath = directory + L"\\" + fileName;
			State().usedPersistentCachePaths.insert(NormalizePathKey(cachePath));
			PersistentFontHashRecord record;
			HANDLE cache = CreateFileW(cachePath.c_str(), GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (cache != INVALID_HANDLE_VALUE)
			{
				const bool read = ReadFileAt(cache, 0, &record, sizeof(record));
				CloseHandle(cache);
				const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'H', 'S', 'H', '1' };
				if (read && std::memcmp(record.magic, magic, sizeof(magic)) == 0
					&& record.version == 1 && record.recordSize == sizeof(record)
					&& record.normalizedPathHash == pathHash
					&& record.fileSize == static_cast<UInt64>(mapped.size)
					&& record.volumeSerial == information.dwVolumeSerialNumber
					&& record.fileIndexHigh == information.nFileIndexHigh
					&& record.fileIndexLow == information.nFileIndexLow
					&& record.lastWriteHigh == information.ftLastWriteTime.dwHighDateTime
					&& record.lastWriteLow == information.ftLastWriteTime.dwLowDateTime
					&& record.checksum == HashBytes64(&record,
						offsetof(PersistentFontHashRecord, checksum)))
				{
					mapped.contentHash = record.contentHash;
					return true;
				}
			}

			mapped.contentHash = HashBytes64(mapped.data,
				static_cast<size_t>(mapped.size));
			record = {};
			const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'H', 'S', 'H', '1' };
			std::memcpy(record.magic, magic, sizeof(magic));
			record.version = 1;
			record.recordSize = sizeof(record);
			record.normalizedPathHash = pathHash;
			record.fileSize = static_cast<UInt64>(mapped.size);
			record.volumeSerial = information.dwVolumeSerialNumber;
			record.fileIndexHigh = information.nFileIndexHigh;
			record.fileIndexLow = information.nFileIndexLow;
			record.lastWriteHigh = information.ftLastWriteTime.dwHighDateTime;
			record.lastWriteLow = information.ftLastWriteTime.dwLowDateTime;
			record.contentHash = mapped.contentHash;
			record.checksum = HashBytes64(&record,
				offsetof(PersistentFontHashRecord, checksum));
			cache = CreateFileW(cachePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
				nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (cache != INVALID_HANDLE_VALUE)
			{
				WriteFileAt(cache, 0, &record, sizeof(record));
				CloseHandle(cache);
			}
			return true;
		}

		void UnmapPersistentBitmapProfile(PersistentBitmapProfile& profile)
		{
			if (profile.mappedData)
				UnmapViewOfFile(profile.mappedData);
			if (profile.mapping)
				CloseHandle(profile.mapping);
			profile.mappedData = nullptr;
			profile.mapping = nullptr;
			profile.mappedSize = 0;
			RefreshPersistentBitmapProfileCpuMemory(profile);
		}

		void MapPersistentBitmapProfile(PersistentBitmapProfile& profile)
		{
			UnmapPersistentBitmapProfile(profile);
			if (!State().persistentBitmapMappingsEnabled)
				return;
			UInt64 size = 0;
			if (!GetFileSize64(profile.file, size) || !size
				|| size > kMaximumPersistentProfileBytes)
			{
				return;
			}
			const size_t totalBytes = GetCpuMemoryUsage();
			const size_t budgetBytes = GetCpuMemoryBudget();
			if (size > std::numeric_limits<size_t>::max()
				|| totalBytes >= budgetBytes
				|| static_cast<size_t>(size) > budgetBytes - totalBytes)
			{
				static UInt32 skippedMappingLogs = 0;
				if (skippedMappingLogs++ < 8)
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: persistent bitmap mapping skipped by CPU budget path=%ls bytes=%llu totalMiB=%.2f limitMiB=%.2f",
						profile.path.c_str(),
						static_cast<unsigned long long>(size),
						totalBytes / (1024.0 * 1024.0),
						budgetBytes / (1024.0 * 1024.0));
				}
				return;
			}
			profile.mapping = CreateFileMappingW(
				profile.file, nullptr, PAGE_READONLY, 0, 0, nullptr);
			if (!profile.mapping)
				return;
			profile.mappedData = static_cast<const UInt8*>(MapViewOfFile(
				profile.mapping, FILE_MAP_READ, 0, 0, 0));
			if (!profile.mappedData)
			{
				CloseHandle(profile.mapping);
				profile.mapping = nullptr;
				return;
			}
			profile.mappedSize = size;
			RefreshPersistentBitmapProfileCpuMemory(profile);
		}

		bool ReadPersistentProfileBytes(const PersistentBitmapProfile& profile,
			UInt64 offset, void* data, UInt32 size)
		{
			if (offset > profile.validSize
				|| static_cast<UInt64>(size) > profile.validSize - offset)
			{
				return false;
			}
			if (profile.mappedData && offset + size <= profile.mappedSize)
			{
				std::memcpy(data, profile.mappedData + static_cast<size_t>(offset), size);
				return true;
			}
			return ReadFileAt(profile.file, offset, data, size);
		}

		bool ResetPersistentBitmapProfile(PersistentBitmapProfile& profile)
		{
			if (!profile.writable)
				return false;
			UnmapPersistentBitmapProfile(profile);
			const PersistentBitmapFileHeader header =
				MakePersistentBitmapFileHeader(profile.key, profile.profileHash,
					profile.glyphCapacity);
			const UInt64 indexBytes = static_cast<UInt64>(profile.glyphCapacity)
				* sizeof(PersistentBitmapIndexEntry);
			if (header.dataOffset != sizeof(header) + indexBytes
				|| !SetFileSize64(profile.file, 0))
			{
				return false;
			}
			TryEnableSparseFile(profile.file);
			if (!SetFileSize64(profile.file, header.dataOffset)
				|| !WriteFileAt(profile.file, 0, &header, sizeof(header)))
			{
				return false;
			}
			profile.recordCount = 0;
			profile.validSize = header.dataOffset;
			profile.indexEntries.assign(profile.glyphCapacity, {});
			MapPersistentBitmapProfile(profile);
			return true;
		}

		bool IsValidPersistentRecordHeader(
			const PersistentBitmapRecordHeader& record, GlyphMaskType maskType,
			DistanceFieldMethod distanceFieldMethod)
		{
			if (record.magic != kPersistentBitmapRecordMagic
				|| record.headerSize != sizeof(record)
				|| record.width <= 0 || record.height <= 0
				|| !record.alphaSize
				|| record.alphaSize > MaximumPersistentBitmapBytes(
					maskType, distanceFieldMethod))
			{
				return false;
			}
			return static_cast<UInt64>(record.width)
				* static_cast<UInt64>(record.height)
				* GlyphBitmapBytesPerPixel(
					maskType, distanceFieldMethod) == record.alphaSize;
		}

		bool InitializePersistentBitmapProfile(PersistentBitmapProfile& profile)
		{
			std::wstring directory;
			if (State().persistentBitmapUnavailable
				|| !EnsurePersistentBitmapDirectory(directory))
			{
				State().persistentBitmapUnavailable = true;
				return false;
			}
			profile.path = FindPersistentBitmapByHash(directory,
				profile.profileHash);
			if (profile.path.empty())
				profile.path = FormatPersistentBitmapPath(directory, profile);
			State().usedPersistentCachePaths.insert(NormalizePathKey(profile.path));
			profile.file = CreateFileW(profile.path.c_str(),
				GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
				OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			profile.writable = profile.file != INVALID_HANDLE_VALUE;
			if (profile.writable)
			{
				TryEnableSparseFile(profile.file);
			}
			if (!profile.writable)
			{
				profile.file = CreateFileW(profile.path.c_str(), GENERIC_READ,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
					OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			}
			if (profile.file == INVALID_HANDLE_VALUE)
				return false;

			UInt64 fileSize = 0;
			if (!GetFileSize64(profile.file, fileSize))
				return false;
			profile.validSize = fileSize;
			if (!fileSize || fileSize > kMaximumPersistentProfileBytes)
			{
				if (!ResetPersistentBitmapProfile(profile))
					return false;
				fileSize = profile.validSize;
			}
			else
			{
				MapPersistentBitmapProfile(profile);
				PersistentBitmapFileHeader header;
				if (!ReadPersistentProfileBytes(profile, 0, &header, sizeof(header))
					|| !MatchesPersistentBitmapFileHeader(
						header, profile.key, profile.profileHash,
						profile.glyphCapacity))
				{
					if (!ResetPersistentBitmapProfile(profile))
						return false;
					fileSize = profile.validSize;
				}
			}

			const UInt64 dataOffset = sizeof(PersistentBitmapFileHeader)
				+ static_cast<UInt64>(profile.glyphCapacity)
					* sizeof(PersistentBitmapIndexEntry);
			if (fileSize < dataOffset)
			{
				if (!ResetPersistentBitmapProfile(profile))
					return false;
				fileSize = profile.validSize;
			}
			profile.recordCount = 0;
			profile.indexEntries.assign(profile.glyphCapacity, {});
			auto inspectEntry = [&](UInt32 glyphIndex,
				const PersistentBitmapIndexEntry& entry)
			{
				if (!entry.offset && !entry.size)
					return;
				if (entry.offset < dataOffset || entry.size < sizeof(PersistentBitmapRecordHeader)
					|| entry.offset > fileSize || entry.size > fileSize - entry.offset)
				{
					if (profile.writable)
					{
						const PersistentBitmapIndexEntry empty;
						const UInt64 entryOffset = sizeof(PersistentBitmapFileHeader)
							+ static_cast<UInt64>(glyphIndex) * sizeof(entry);
						WriteFileAt(profile.file, entryOffset, &empty, sizeof(empty));
					}
					return;
				}
				profile.indexEntries[glyphIndex] = entry;
				++profile.recordCount;
			};
			const UInt64 indexBytes = static_cast<UInt64>(profile.glyphCapacity)
				* sizeof(PersistentBitmapIndexEntry);
			if (profile.mappedData
				&& sizeof(PersistentBitmapFileHeader) + indexBytes <= profile.mappedSize)
			{
				const auto* entries = reinterpret_cast<const PersistentBitmapIndexEntry*>(
					profile.mappedData + sizeof(PersistentBitmapFileHeader));
				for (UInt32 glyphIndex = 0; glyphIndex < profile.glyphCapacity; ++glyphIndex)
					inspectEntry(glyphIndex, entries[glyphIndex]);
			}
			else
			{
				constexpr UInt32 kIndexEntriesPerChunk = 4096;
				std::vector<PersistentBitmapIndexEntry> entries(kIndexEntriesPerChunk);
				for (UInt32 first = 0; first < profile.glyphCapacity;)
				{
					const UInt32 count = std::min<UInt32>(kIndexEntriesPerChunk,
						profile.glyphCapacity - first);
					const UInt64 offset = sizeof(PersistentBitmapFileHeader)
						+ static_cast<UInt64>(first) * sizeof(PersistentBitmapIndexEntry);
					const UInt32 bytes = count * sizeof(PersistentBitmapIndexEntry);
					if (!ReadFileAt(profile.file, offset, entries.data(), bytes))
						return false;
					for (UInt32 index = 0; index < count; ++index)
						inspectEntry(first + index, entries[index]);
					first += count;
				}
			}
			profile.validSize = fileSize;
			profile.initialized = true;
			return true;
		}

		PersistentBitmapProfile* GetPersistentBitmapProfile(
			const PersistentBitmapProfileKey& key,
			const std::wstring& fontPath, UInt32 fontId, UInt32 glyphCapacity)
		{
			if (State().completeCodePageAtlasOnlyPrewarm
				|| State().atlasOnlyCodePageFontIds.count(fontId))
				return nullptr;
			auto existing = State().persistentBitmapProfiles.find(key);
			if (existing != State().persistentBitmapProfiles.end())
				return existing->second->initialized ? existing->second.get() : nullptr;
			auto profile = std::make_unique<PersistentBitmapProfile>();
			profile->key = key;
			profile->profileHash = HashPersistentBitmapProfileKey(key);
			profile->fontId = fontId;
			profile->glyphCapacity = std::max<UInt32>(1, glyphCapacity);
			profile->fontFileName = SanitizePersistentBitmapFontName(fontPath);
			PersistentBitmapProfile* result = profile.get();
			State().persistentBitmapProfiles.emplace(key, std::move(profile));
			if (InitializePersistentBitmapProfile(*result))
				return result;
			if (State().persistentBitmapFailureLogCount++ < 8)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: persistent bitmap profile unavailable hash=%016llX path=%ls",
					static_cast<unsigned long long>(result->profileHash),
					result->path.empty() ? L"<unresolved>" : result->path.c_str());
			}
			return nullptr;
		}

		UInt64 HashPersistentBitmapRecord(
			const PersistentBitmapRecordHeader& record, const UInt8* alpha)
		{
			UInt64 hash = HashBytes64(&record,
				offsetof(PersistentBitmapRecordHeader, checksum));
			return HashBytes64(alpha, record.alphaSize, hash);
		}

		std::shared_ptr<GlyphBitmap> LoadPersistentGlyphBitmap(
			PersistentBitmapProfile& profile, const BitmapCacheKey& key)
		{
			if (key.glyphIndex >= profile.glyphCapacity
				|| key.glyphIndex >= profile.indexEntries.size())
				return nullptr;
			const PersistentBitmapIndexEntry entry =
				profile.indexEntries[key.glyphIndex];
			if (!entry.offset || !entry.size)
				return nullptr;
			PersistentBitmapRecordHeader record;
			if (!ReadPersistentProfileBytes(profile, entry.offset,
					&record, sizeof(record))
				|| !IsValidPersistentRecordHeader(record,
					static_cast<GlyphMaskType>(key.maskType),
					static_cast<DistanceFieldMethod>(key.distanceFieldMethod))
				|| record.glyphIndex != key.glyphIndex
				|| entry.size != sizeof(record) + record.alphaSize)
				return nullptr;
			auto bitmap = std::make_shared<GlyphBitmap>();
			bitmap->cacheId = HashBitmapKey(key);
			bitmap->width = record.width;
			bitmap->height = record.height;
			bitmap->left = record.left;
			bitmap->top = record.top;
			bitmap->effectiveWidth = key.effectiveWidth;
			bitmap->effectiveHeight = key.effectiveHeight;
			bitmap->maskType = static_cast<GlyphMaskType>(key.maskType);
			bitmap->distanceFieldMethod =
				static_cast<DistanceFieldMethod>(key.distanceFieldMethod);
			bitmap->sdfSpread = key.sdfSpread;
			bitmap->strokeWidth26Dot6 = key.strokeWidth26Dot6;
			bitmap->alpha.resize(record.alphaSize);
			const UInt64 alphaOffset = entry.offset + sizeof(record);
			if (!ReadPersistentProfileBytes(profile, alphaOffset,
					bitmap->alpha.data(), record.alphaSize)
				|| record.checksum != HashPersistentBitmapRecord(
					record, bitmap->alpha.data()))
			{
				return nullptr;
			}
			bitmap->cpuMemory.Reset(CpuMemoryCategory::GlyphBitmap,
				sizeof(GlyphBitmap) + bitmap->alpha.capacity());
			return bitmap;
		}

		static UInt32 StorePersistentGlyphBitmapsImpl(PersistentBitmapProfile& profile,
			const PersistentBitmapStoreRequest* requests, size_t requestCount,
			UInt64& storedAlphaBytes)
		{
			storedAlphaBytes = 0;
			if (!profile.writable || !requests || !requestCount
				|| profile.indexEntries.size() != profile.glyphCapacity)
			{
				return 0;
			}

			struct PendingIndex
			{
				UInt32 glyphIndex = 0;
				PersistentBitmapIndexEntry entry;
			};
			std::vector<PendingIndex> pending;
			pending.reserve(requestCount);
			std::vector<UInt8> serialized;
			UInt64 estimatedBytes = 0;
			for (size_t requestIndex = 0; requestIndex < requestCount; ++requestIndex)
			{
				const PersistentBitmapStoreRequest& request = requests[requestIndex];
				if (!request.key || !request.bitmap)
					continue;
				const BitmapCacheKey& key = *request.key;
				const GlyphBitmap& bitmap = *request.bitmap;
				if (key.glyphIndex >= profile.glyphCapacity
					|| profile.indexEntries[key.glyphIndex].offset
					|| profile.indexEntries[key.glyphIndex].size
					|| bitmap.width <= 0 || bitmap.height <= 0
					|| bitmap.alpha.empty()
					|| bitmap.alpha.size() > MaximumPersistentBitmapBytes(
						bitmap.maskType, bitmap.distanceFieldMethod)
					|| key.distanceFieldMethod
						!= static_cast<UInt8>(bitmap.distanceFieldMethod)
					|| ExpectedGlyphBitmapBytes(bitmap) != bitmap.alpha.size())
				{
					continue;
				}
				estimatedBytes += sizeof(PersistentBitmapRecordHeader)
					+ bitmap.alpha.size();
			}
			if (!estimatedBytes
				|| estimatedBytes > kMaximumPersistentProfileBytes - profile.validSize
				|| estimatedBytes > std::numeric_limits<UInt32>::max())
			{
				return 0;
			}
			serialized.reserve(static_cast<size_t>(estimatedBytes));
			for (size_t requestIndex = 0; requestIndex < requestCount; ++requestIndex)
			{
				const PersistentBitmapStoreRequest& request = requests[requestIndex];
				if (!request.key || !request.bitmap)
					continue;
				const BitmapCacheKey& key = *request.key;
				const GlyphBitmap& bitmap = *request.bitmap;
				if (key.glyphIndex >= profile.glyphCapacity
					|| profile.indexEntries[key.glyphIndex].offset
					|| profile.indexEntries[key.glyphIndex].size
					|| bitmap.width <= 0 || bitmap.height <= 0
					|| bitmap.alpha.empty()
					|| bitmap.alpha.size() > MaximumPersistentBitmapBytes(
						bitmap.maskType, bitmap.distanceFieldMethod)
					|| key.distanceFieldMethod
						!= static_cast<UInt8>(bitmap.distanceFieldMethod)
					|| ExpectedGlyphBitmapBytes(bitmap) != bitmap.alpha.size())
				{
					continue;
				}
				PersistentBitmapRecordHeader record;
				record.magic = kPersistentBitmapRecordMagic;
				record.headerSize = sizeof(record);
				record.glyphIndex = key.glyphIndex;
				record.width = bitmap.width;
				record.height = bitmap.height;
				record.left = bitmap.left;
				record.top = bitmap.top;
				record.alphaSize = static_cast<UInt32>(bitmap.alpha.size());
				record.checksum = HashPersistentBitmapRecord(record,
					bitmap.alpha.data());
				const UInt64 offset = profile.validSize + serialized.size();
				const UInt32 recordSize = static_cast<UInt32>(
					sizeof(record) + bitmap.alpha.size());
				const size_t oldSize = serialized.size();
				serialized.resize(oldSize + recordSize);
				std::memcpy(serialized.data() + oldSize, &record, sizeof(record));
				std::memcpy(serialized.data() + oldSize + sizeof(record),
					bitmap.alpha.data(), bitmap.alpha.size());
				pending.push_back({ key.glyphIndex, { offset, recordSize } });
			}
			if (pending.empty())
				return 0;

			// Extending a file while an older, shorter view is still mapped has
			// platform-dependent failure modes. Existing records remain readable
			// through ReadFileAt after the view is released.
			UnmapPersistentBitmapProfile(profile);
			const UInt64 dataOffset = profile.validSize;
			if (!WriteFileAt(profile.file, dataOffset, serialized.data(),
					static_cast<UInt32>(serialized.size())))
			{
				return 0;
			}
			profile.validSize += serialized.size();

			std::sort(pending.begin(), pending.end(),
				[](const PendingIndex& lhs, const PendingIndex& rhs)
				{
					return lhs.glyphIndex < rhs.glyphIndex;
				});
			UInt32 stored = 0;
			std::vector<PersistentBitmapIndexEntry> entries;
			for (size_t first = 0; first < pending.size();)
			{
				size_t last = first + 1;
				while (last < pending.size()
					&& pending[last].glyphIndex == pending[last - 1].glyphIndex + 1)
				{
					++last;
				}
				entries.clear();
				entries.reserve(last - first);
				for (size_t index = first; index < last; ++index)
					entries.push_back(pending[index].entry);
				const UInt64 indexOffset = sizeof(PersistentBitmapFileHeader)
					+ static_cast<UInt64>(pending[first].glyphIndex)
						* sizeof(PersistentBitmapIndexEntry);
				const UInt32 indexBytes = static_cast<UInt32>(entries.size()
					* sizeof(PersistentBitmapIndexEntry));
				if (WriteFileAt(profile.file, indexOffset, entries.data(), indexBytes))
				{
					for (size_t index = first; index < last; ++index)
					{
						const PendingIndex& value = pending[index];
						profile.indexEntries[value.glyphIndex] = value.entry;
						storedAlphaBytes += value.entry.size
							- sizeof(PersistentBitmapRecordHeader);
					}
					stored += static_cast<UInt32>(last - first);
				}
				first = last;
			}
			profile.recordCount += stored;
			return stored;
		}

		UInt32 StorePersistentGlyphBitmaps(PersistentBitmapProfile& profile,
			const std::vector<PersistentBitmapStoreRequest>& requests,
			UInt64& storedAlphaBytes)
		{
			return StorePersistentGlyphBitmapsImpl(profile, requests.data(),
				requests.size(), storedAlphaBytes);
		}

		bool StorePersistentGlyphBitmap(PersistentBitmapProfile& profile,
			const BitmapCacheKey& key, const GlyphBitmap& bitmap)
		{
			const PersistentBitmapStoreRequest request = { &key, &bitmap };
			UInt64 storedAlphaBytes = 0;
			return StorePersistentGlyphBitmapsImpl(profile, &request, 1,
				storedAlphaBytes) == 1;
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
					const SInt32 faceIndex = face.face
						? static_cast<SInt32>(face.face->face_index) : 0;
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
				const SInt32 faceIndex = face.face
					? static_cast<SInt32>(face.face->face_index) : 0;
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
				return runtime.manifest->mappedData ? runtime.manifest.get() : nullptr;
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
					runtime.manifest = std::move(shared);
					return runtime.manifest->mappedData ? runtime.manifest.get() : nullptr;
				}
				State().persistentGlyphManifests.erase(pooled);
			}
			auto manifest = std::make_shared<PersistentGlyphManifest>();
			manifest->manifestHash = manifestHash;
			manifest->layoutContentHash = layoutContentHash;
			std::wstring directory;
			if (!EnsurePersistentBitmapDirectory(directory))
			{
				runtime.manifest = manifest;
				State().persistentGlyphManifests[manifestHash] = manifest;
				return nullptr;
			}
			wchar_t fileName[256] = {};
			_snwprintf_s(fileName, _countof(fileName), _TRUNCATE,
				L"shared_%016llX.tnvfmanifest",
				static_cast<unsigned long long>(manifestHash));
			manifest->path = directory + L"\\" + fileName;
			State().usedPersistentCachePaths.insert(NormalizePathKey(manifest->path));
			const std::vector<UInt16>& encodedCodes =
				GetFontPrewarmEncodedUnits(GetRuntimeConfig(runtime));
			InitializeGlyphManifest(*manifest, runtime, encodedCodes);
			State().persistentGlyphManifests[manifestHash] = manifest;
			runtime.manifest = std::move(manifest);
			return runtime.manifest->mappedData ? runtime.manifest.get() : nullptr;
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
			if (!g_bDisableFreeTypeExtendedCaches)
			{
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
		if (route == FontAtlasRoute::ShaderA8Coverage)
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
			return route == FontAtlasRoute::ShaderA8Coverage
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
						: previousRoute == FontAtlasRoute::ShaderA8Coverage
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
			route == FontAtlasRoute::ShaderA8Coverage
				? "argb-composite" : "argb-fallback",
			wasSynchronized
				? (previousRoute == FontAtlasRoute::ShaderDistanceField
					? "distance-field"
					: previousRoute == FontAtlasRoute::ShaderA8Coverage
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
				if (!face.file || !face.face)
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
						static_cast<SInt32>(face.face->face_index),
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
								std::max<FT_Long>(1, face.face->num_glyphs)));
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

#include "font_freetype_internal.h"

#include "encoding.h"
#include "font_glyphs.h"
#include "globals.h"

#include <winioctl.h>

#include "font_vector_persistent_cache_internal.h"

namespace fonthook::vectorfont
{
	static UInt32 MaximumPersistentBitmapBytes(GlyphMaskType maskType,
		DistanceFieldMethod distanceFieldMethod)
	{
		return maskType == GlyphMaskType::Composite
			? kMaximumPersistentFourChannelBitmapBytes
			: maskType == GlyphMaskType::DistanceField
			? (distanceFieldMethod == DistanceFieldMethod::Mtsdf
				? kMaximumPersistentFourChannelBitmapBytes
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
		UInt64 HashBitmapKey(const BitmapCacheKey& key)
		{
			if (key.stableHash)
				return key.stableHash;
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
				|| !record.payloadSize
				|| record.payloadSize > MaximumPersistentBitmapBytes(
					maskType, distanceFieldMethod))
			{
				return false;
			}
			return static_cast<UInt64>(record.width)
				* static_cast<UInt64>(record.height)
				* GlyphBitmapBytesPerPixel(
					maskType, distanceFieldMethod) == record.payloadSize;
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
			const PersistentBitmapRecordHeader& record, const UInt8* payload)
		{
			UInt64 hash = HashBytes64(&record,
				offsetof(PersistentBitmapRecordHeader, checksum));
			return HashBytes64(payload, record.payloadSize, hash);
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
				|| entry.size != sizeof(record) + record.payloadSize)
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
			bitmap->pixels.resize(record.payloadSize);
			const UInt64 payloadOffset = entry.offset + sizeof(record);
			if (!ReadPersistentProfileBytes(profile, payloadOffset,
					bitmap->pixels.data(), record.payloadSize)
				|| record.checksum != HashPersistentBitmapRecord(
					record, bitmap->pixels.data()))
			{
				return nullptr;
			}
			bitmap->cpuMemory.Reset(CpuMemoryCategory::GlyphBitmap,
				sizeof(GlyphBitmap) + bitmap->pixels.capacity());
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
					|| bitmap.pixels.empty()
					|| bitmap.pixels.size() > MaximumPersistentBitmapBytes(
						bitmap.maskType, bitmap.distanceFieldMethod)
					|| key.distanceFieldMethod
						!= static_cast<UInt8>(bitmap.distanceFieldMethod)
					|| ExpectedGlyphBitmapBytes(bitmap) != bitmap.pixels.size())
				{
					continue;
				}
				estimatedBytes += sizeof(PersistentBitmapRecordHeader)
					+ bitmap.pixels.size();
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
					|| bitmap.pixels.empty()
					|| bitmap.pixels.size() > MaximumPersistentBitmapBytes(
						bitmap.maskType, bitmap.distanceFieldMethod)
					|| key.distanceFieldMethod
						!= static_cast<UInt8>(bitmap.distanceFieldMethod)
					|| ExpectedGlyphBitmapBytes(bitmap) != bitmap.pixels.size())
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
				record.payloadSize = static_cast<UInt32>(bitmap.pixels.size());
				record.checksum = HashPersistentBitmapRecord(record,
					bitmap.pixels.data());
				const UInt64 offset = profile.validSize + serialized.size();
				const UInt32 recordSize = static_cast<UInt32>(
					sizeof(record) + bitmap.pixels.size());
				const size_t oldSize = serialized.size();
				serialized.resize(oldSize + recordSize);
				std::memcpy(serialized.data() + oldSize, &record, sizeof(record));
				std::memcpy(serialized.data() + oldSize + sizeof(record),
					bitmap.pixels.data(), bitmap.pixels.size());
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


}

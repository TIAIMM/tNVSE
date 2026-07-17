#include "font_freetype_internal.h"

#include "encoding.h"
#include "font_glyphs.h"
#include "globals.h"

#include <winioctl.h>

namespace fonthook::vectorfont
{
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
			return HashBytes64(&key.maskType, sizeof(key.maskType), hash);
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
				key.maskType
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
			LARGE_INTEGER position = {};
			position.QuadPart = static_cast<LONGLONG>(size);
			return file != INVALID_HANDLE_VALUE
				&& SetFilePointerEx(file, position, nullptr, FILE_BEGIN)
				&& SetEndOfFile(file);
		}

		bool TryEnableSparseFile(HANDLE file)
		{
			if (file == INVALID_HANDLE_VALUE)
				return false;
			DWORD bytesReturned = 0;
			return DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0,
				nullptr, 0, &bytesReturned, nullptr) != FALSE;
		}

		bool TryEnableFileCompression(HANDLE file)
		{
			if (file == INVALID_HANDLE_VALUE)
				return false;
			USHORT format = COMPRESSION_FORMAT_DEFAULT;
			DWORD bytesReturned = 0;
			return DeviceIoControl(file, FSCTL_SET_COMPRESSION,
				&format, sizeof(format), nullptr, 0, &bytesReturned, nullptr) != FALSE;
		}

		bool ReadFileAt(HANDLE file, UInt64 offset, void* data, UInt32 size)
		{
			if (!size)
				return true;
			LARGE_INTEGER position = {};
			position.QuadPart = static_cast<LONGLONG>(offset);
			DWORD read = 0;
			return file != INVALID_HANDLE_VALUE
				&& SetFilePointerEx(file, position, nullptr, FILE_BEGIN)
				&& ReadFile(file, data, size, &read, nullptr)
				&& read == size;
		}

		bool WriteFileAt(HANDLE file, UInt64 offset, const void* data, UInt32 size)
		{
			if (!size)
				return true;
			LARGE_INTEGER position = {};
			position.QuadPart = static_cast<LONGLONG>(offset);
			DWORD written = 0;
			return file != INVALID_HANDLE_VALUE
				&& SetFilePointerEx(file, position, nullptr, FILE_BEGIN)
				&& WriteFile(file, data, size, &written, nullptr)
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
		}

		void MapPersistentBitmapProfile(PersistentBitmapProfile& profile)
		{
			UnmapPersistentBitmapProfile(profile);
			UInt64 size = 0;
			if (!GetFileSize64(profile.file, size) || !size
				|| size > kMaximumPersistentProfileBytes)
			{
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
			TryEnableFileCompression(profile.file);
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
			const PersistentBitmapRecordHeader& record)
		{
			if (record.magic != kPersistentBitmapRecordMagic
				|| record.headerSize != sizeof(record)
				|| record.width <= 0 || record.height <= 0
				|| !record.alphaSize
				|| record.alphaSize > kMaximumPersistentBitmapBytes)
			{
				return false;
			}
			return static_cast<UInt64>(record.width)
				* static_cast<UInt64>(record.height) == record.alphaSize;
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
				TryEnableFileCompression(profile.file);
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
				|| !IsValidPersistentRecordHeader(record)
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
			return bitmap;
		}

		UInt32 StorePersistentGlyphBitmaps(PersistentBitmapProfile& profile,
			const std::vector<PersistentBitmapStoreRequest>& requests,
			UInt64& storedAlphaBytes)
		{
			storedAlphaBytes = 0;
			if (!profile.writable || requests.empty()
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
			pending.reserve(requests.size());
			std::vector<UInt8> serialized;
			UInt64 estimatedBytes = 0;
			for (const PersistentBitmapStoreRequest& request : requests)
			{
				if (!request.key || !request.bitmap)
					continue;
				const BitmapCacheKey& key = *request.key;
				const GlyphBitmap& bitmap = *request.bitmap;
				if (key.glyphIndex >= profile.glyphCapacity
					|| profile.indexEntries[key.glyphIndex].offset
					|| profile.indexEntries[key.glyphIndex].size
					|| bitmap.width <= 0 || bitmap.height <= 0
					|| bitmap.alpha.empty()
					|| bitmap.alpha.size() > kMaximumPersistentBitmapBytes
					|| static_cast<UInt64>(bitmap.width) * bitmap.height
						!= bitmap.alpha.size())
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
			for (const PersistentBitmapStoreRequest& request : requests)
			{
				if (!request.key || !request.bitmap)
					continue;
				const BitmapCacheKey& key = *request.key;
				const GlyphBitmap& bitmap = *request.bitmap;
				if (key.glyphIndex >= profile.glyphCapacity
					|| profile.indexEntries[key.glyphIndex].offset
					|| profile.indexEntries[key.glyphIndex].size
					|| bitmap.width <= 0 || bitmap.height <= 0
					|| bitmap.alpha.empty()
					|| bitmap.alpha.size() > kMaximumPersistentBitmapBytes
					|| static_cast<UInt64>(bitmap.width) * bitmap.height
						!= bitmap.alpha.size())
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
			for (size_t first = 0; first < pending.size();)
			{
				size_t last = first + 1;
				while (last < pending.size()
					&& pending[last].glyphIndex == pending[last - 1].glyphIndex + 1)
				{
					++last;
				}
				std::vector<PersistentBitmapIndexEntry> entries;
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

		bool StorePersistentGlyphBitmap(PersistentBitmapProfile& profile,
			const BitmapCacheKey& key, const GlyphBitmap& bitmap)
		{
			const std::vector<PersistentBitmapStoreRequest> requests = {
				{ &key, &bitmap }
			};
			UInt64 storedAlphaBytes = 0;
			return StorePersistentGlyphBitmaps(profile, requests,
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

		UInt64 ComputeRuntimeMaskContentHash(RuntimeFont& runtime)
		{
			if (runtime.maskContentHash)
				return runtime.maskContentHash;
			UInt64 hash = HashBytes64(&kPersistentBitmapVersion,
				sizeof(kPersistentBitmapVersion));
			hash = HashBytes64(&runtime.config->maskGenerationHash,
				sizeof(runtime.config->maskGenerationHash));
			runtime.maskContentHash = HashRuntimeFontFaces(runtime, hash);
			return runtime.maskContentHash;
		}

		PersistentGlyphManifestHeader MakeGlyphManifestHeader(
			const RuntimeFont& runtime, UInt64 manifestHash, UInt64 layoutContentHash)
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
			header.entryCount = kPersistentGlyphManifestEntries;
			header.entrySize = sizeof(PersistentGlyphManifestEntry);
			header.checksum = HashBytes64(&header,
				offsetof(PersistentGlyphManifestHeader, checksum));
			return header;
		}

		bool MatchesGlyphManifestHeader(const PersistentGlyphManifestHeader& header,
			const RuntimeFont& runtime, UInt64 manifestHash, UInt64 layoutContentHash)
		{
			const UInt8 magic[8] = { 'T', 'N', 'V', 'F', 'G', 'L', 'Y', '1' };
			return std::memcmp(header.magic, magic, sizeof(magic)) == 0
				&& header.version == kPersistentGlyphManifestVersion
				&& header.headerSize == sizeof(header)
				&& header.manifestHash == manifestHash
				&& header.layoutContentHash == layoutContentHash
				&& header.layoutHash == runtime.config->layoutHash
				&& header.reservedFontId == 0
				&& header.codePage == GetFreeTypeTextCodePage()
				&& header.entryCount == kPersistentGlyphManifestEntries
				&& header.entrySize == sizeof(PersistentGlyphManifestEntry)
				&& header.checksum == HashBytes64(&header,
					offsetof(PersistentGlyphManifestHeader, checksum));
		}

		PersistentGlyphManifest* GetGlyphManifest(RuntimeFont& runtime)
		{
			if (runtime.manifest)
				return runtime.manifest->mappedData ? runtime.manifest.get() : nullptr;
			auto manifest = std::make_unique<PersistentGlyphManifest>();
			const UInt64 layoutContentHash = ComputeRuntimeLayoutContentHash(runtime);
			UInt64 manifestHash = HashBytes64(&layoutContentHash,
				sizeof(layoutContentHash));
			const UInt32 codePage = GetFreeTypeTextCodePage();
			manifestHash = HashBytes64(&codePage,
				sizeof(codePage), manifestHash);
			manifest->manifestHash = manifestHash;
			manifest->layoutContentHash = layoutContentHash;
			std::wstring directory;
			if (!EnsurePersistentBitmapDirectory(directory))
			{
				runtime.manifest = std::move(manifest);
				return nullptr;
			}
			wchar_t fileName[256] = {};
			_snwprintf_s(fileName, _countof(fileName), _TRUNCATE,
				L"shared_%016llX.tnvfmanifest",
				static_cast<unsigned long long>(manifestHash));
			manifest->path = directory + L"\\" + fileName;
			State().usedPersistentCachePaths.insert(NormalizePathKey(manifest->path));
			manifest->file = CreateFileW(manifest->path.c_str(),
				GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			manifest->writable = manifest->file != INVALID_HANDLE_VALUE;
			if (manifest->writable)
			{
				// Packed entries rarely leave full sparse clusters. NTFS compression
				// complements sparse allocation without changing mapped-file access.
				TryEnableSparseFile(manifest->file);
				TryEnableFileCompression(manifest->file);
			}
			if (!manifest->writable)
			{
				manifest->file = CreateFileW(manifest->path.c_str(), GENERIC_READ,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
					OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			}
			if (manifest->file == INVALID_HANDLE_VALUE)
			{
				runtime.manifest = std::move(manifest);
				return nullptr;
			}
			const UInt64 expectedSize = sizeof(PersistentGlyphManifestHeader)
				+ static_cast<UInt64>(kPersistentGlyphManifestEntries)
					* sizeof(PersistentGlyphManifestEntry);
			UInt64 fileSize = 0;
			PersistentGlyphManifestHeader header;
			bool valid = GetFileSize64(manifest->file, fileSize)
				&& fileSize == expectedSize
				&& ReadFileAt(manifest->file, 0, &header, sizeof(header))
				&& MatchesGlyphManifestHeader(header, runtime, manifestHash,
					layoutContentHash);
			if (!valid)
			{
				if (!manifest->writable)
				{
					runtime.manifest = std::move(manifest);
					return nullptr;
				}
				header = MakeGlyphManifestHeader(runtime, manifestHash,
					layoutContentHash);
				if (!SetFileSize64(manifest->file, 0))
				{
					runtime.manifest = std::move(manifest);
					return nullptr;
				}
				TryEnableSparseFile(manifest->file);
				TryEnableFileCompression(manifest->file);
				if (!SetFileSize64(manifest->file, expectedSize)
					|| !WriteFileAt(manifest->file, 0, &header, sizeof(header)))
				{
					runtime.manifest = std::move(manifest);
					return nullptr;
				}
			}
			manifest->mapping = CreateFileMappingW(manifest->file, nullptr,
				manifest->writable ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
			if (manifest->mapping)
			{
				manifest->mappedData = static_cast<UInt8*>(MapViewOfFile(manifest->mapping,
					manifest->writable ? FILE_MAP_WRITE | FILE_MAP_READ : FILE_MAP_READ,
					0, 0, 0));
			}
			runtime.manifest = std::move(manifest);
			return runtime.manifest->mappedData ? runtime.manifest.get() : nullptr;
		}

		PersistentGlyphManifestEntry* GetGlyphManifestEntry(
			PersistentGlyphManifest& manifest, UInt32 encodedCode)
		{
			if (!manifest.mappedData || encodedCode >= kPersistentGlyphManifestEntries)
				return nullptr;
			return reinterpret_cast<PersistentGlyphManifestEntry*>(
				manifest.mappedData + sizeof(PersistentGlyphManifestHeader)) + encodedCode;
		}

		bool LoadGlyphManifest(RuntimeFont& runtime, UInt32 encodedCode,
			VectorFontByteClass byteClass, VectorEncodedGlyph* glyph, FontLetter* metrics)
		{
			PersistentGlyphManifest* manifest = GetGlyphManifest(runtime);
			PersistentGlyphManifestEntry* entry = manifest
				? GetGlyphManifestEntry(*manifest, encodedCode) : nullptr;
			if (!entry || !entry->valid
				|| entry->byteClass != static_cast<UInt8>(byteClass)
				|| entry->checksum != HashBytes64(entry,
					offsetof(PersistentGlyphManifestEntry, checksum)))
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
			role.glyphIdentities.emplace(entry->codePoint, CachedGlyphIdentity{
				entry->faceIndex, entry->glyphIndex, entry->renderedCodePoint });
			return true;
		}

		void StoreGlyphManifest(RuntimeFont& runtime, const VectorEncodedGlyph& glyph,
			const ResolvedGlyph& resolved, const FontLetter& metrics)
		{
			PersistentGlyphManifest* manifest = GetGlyphManifest(runtime);
			PersistentGlyphManifestEntry* destination = manifest
				? GetGlyphManifestEntry(*manifest, glyph.encodedCode) : nullptr;
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
		}

		SInt16 QuantizeCollisionCoordinate(float value)
		{
			const long quantized = std::lround(value * 64.0f);
			return static_cast<SInt16>(std::clamp<long>(quantized,
				std::numeric_limits<SInt16>::min(),
				std::numeric_limits<SInt16>::max()));
		}

		bool LoadGlyphCollisionProfile(RuntimeFont& runtime,
			const VectorEncodedGlyph& glyph, GlyphCollisionProfile& profile)
		{
			profile = {};
			PersistentGlyphManifest* manifest = GetGlyphManifest(runtime);
			PersistentGlyphManifestEntry* entry = manifest
				? GetGlyphManifestEntry(*manifest, glyph.encodedCode) : nullptr;
			if (!entry || !entry->valid || !entry->collisionValid
				|| entry->byteClass != static_cast<UInt8>(glyph.byteClass)
				|| entry->faceIndex != glyph.faceIndex
				|| entry->glyphIndex != glyph.glyphIndex
				|| entry->checksum != HashBytes64(entry,
					offsetof(PersistentGlyphManifestEntry, checksum)))
			{
				return false;
			}
			profile.top = static_cast<float>(entry->collisionTop26Dot6) / 64.0f;
			profile.bottom = static_cast<float>(entry->collisionBottom26Dot6) / 64.0f;
			profile.bandMask = entry->collisionBandMask;
			for (size_t index = 0; index < kGlyphCollisionBandCount; ++index)
			{
				profile.left[index] = static_cast<float>(
					entry->collisionLeft26Dot6[index]) / 64.0f;
				profile.right[index] = static_cast<float>(
					entry->collisionRight26Dot6[index]) / 64.0f;
			}
			return true;
		}

		void StoreGlyphCollisionProfile(RuntimeFont& runtime,
			const VectorEncodedGlyph& glyph, const GlyphBitmap& bitmap, float rasterScale)
		{
			if (bitmap.maskType != GlyphMaskType::Fill
				&& bitmap.maskType != GlyphMaskType::DistanceField)
			{
				return;
			}
			if (!std::isfinite(rasterScale) || rasterScale < 0.1f
				|| bitmap.width < 0 || bitmap.height < 0
				|| bitmap.alpha.size() != static_cast<size_t>(bitmap.width)
					* static_cast<size_t>(bitmap.height))
			{
				return;
			}

			std::lock_guard<std::recursive_mutex> lock(State().mutex);
			PersistentGlyphManifest* manifest = GetGlyphManifest(runtime);
			PersistentGlyphManifestEntry* destination = manifest
				? GetGlyphManifestEntry(*manifest, glyph.encodedCode) : nullptr;
			if (!destination || !manifest->writable || !destination->valid
				|| destination->collisionValid
				|| destination->byteClass != static_cast<UInt8>(glyph.byteClass)
				|| destination->faceIndex != glyph.faceIndex
				|| destination->glyphIndex != glyph.glyphIndex
				|| destination->checksum != HashBytes64(destination,
					offsetof(PersistentGlyphManifestEntry, checksum)))
			{
				return;
			}

			constexpr UInt8 kBodyThreshold = 128;
			int firstRow = bitmap.height;
			int lastRow = -1;
			for (int y = 0; y < bitmap.height; ++y)
			{
				const UInt8* row = bitmap.alpha.data()
					+ static_cast<size_t>(y) * bitmap.width;
				if (std::find_if(row, row + bitmap.width,
					[](UInt8 value) { return value >= kBodyThreshold; })
					!= row + bitmap.width)
				{
					firstRow = std::min(firstRow, y);
					lastRow = y;
				}
			}

			PersistentGlyphManifestEntry entry = *destination;
			entry.collisionValid = 1;
			entry.collisionBandMask = 0;
			if (lastRow >= firstRow)
			{
				entry.collisionTop26Dot6 = QuantizeCollisionCoordinate(
					(static_cast<float>(bitmap.top - firstRow)) / rasterScale);
				entry.collisionBottom26Dot6 = QuantizeCollisionCoordinate(
					(static_cast<float>(bitmap.top - lastRow - 1)) / rasterScale);
				std::array<float, kGlyphCollisionBandCount> left;
				std::array<float, kGlyphCollisionBandCount> right;
				left.fill(std::numeric_limits<float>::infinity());
				right.fill(-std::numeric_limits<float>::infinity());
				const int visibleRows = lastRow - firstRow + 1;
				for (int y = firstRow; y <= lastRow; ++y)
				{
					const UInt8* row = bitmap.alpha.data()
						+ static_cast<size_t>(y) * bitmap.width;
					int first = 0;
					while (first < bitmap.width && row[first] < kBodyThreshold)
						++first;
					if (first == bitmap.width)
						continue;
					int last = bitmap.width - 1;
					while (last > first && row[last] < kBodyThreshold)
						--last;

					const float previous = first > 0 ? row[first - 1] : 0.0f;
					const float currentLeft = row[first];
					const float leftMix = currentLeft > previous
						? (kBodyThreshold - previous) / (currentLeft - previous) : 1.0f;
					const float leftPixel = static_cast<float>(first) - 0.5f + leftMix;
					const float currentRight = row[last];
					const float next = last + 1 < bitmap.width ? row[last + 1] : 0.0f;
					const float rightMix = currentRight > next
						? (currentRight - kBodyThreshold) / (currentRight - next) : 0.0f;
					const float rightPixel = static_cast<float>(last) + 0.5f + rightMix;
					const size_t band = std::min<size_t>(kGlyphCollisionBandCount - 1,
						static_cast<size_t>(y - firstRow) * kGlyphCollisionBandCount
							/ static_cast<size_t>(visibleRows));
					left[band] = std::min(left[band],
						(static_cast<float>(bitmap.left) + leftPixel) / rasterScale);
					right[band] = std::max(right[band],
						(static_cast<float>(bitmap.left) + rightPixel) / rasterScale);
				}
				for (size_t band = 0; band < kGlyphCollisionBandCount; ++band)
				{
					if (!std::isfinite(left[band]) || !std::isfinite(right[band]))
						continue;
					entry.collisionBandMask |= static_cast<UInt16>(1u << band);
					entry.collisionLeft26Dot6[band] = QuantizeCollisionCoordinate(left[band]);
					entry.collisionRight26Dot6[band] = QuantizeCollisionCoordinate(right[band]);
				}
			}
			entry.checksum = HashBytes64(&entry,
				offsetof(PersistentGlyphManifestEntry, checksum));
			std::memcpy(destination, &entry, sizeof(entry));
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

	void DeleteUnusedFreeTypeFontCacheFiles()
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
			const bool managed = hasSuffix(normalized, L".tnvfmask")
				|| hasSuffix(normalized, L".tnvfhash")
				|| hasSuffix(normalized, L".tnvfmanifest")
				|| hasSuffix(normalized, L".tnvfatlas")
				|| hasSuffix(normalized, L".tnvfatlas.tmp");
			if (!managed || State().usedPersistentCachePaths.count(normalized))
				continue;
			const UInt64 size = (static_cast<UInt64>(found.nFileSizeHigh) << 32)
				| found.nFileSizeLow;
			if (DeleteFileW(path.c_str()))
			{
				++deleted;
				deletedBytes += size;
			}
			else
			{
				++failed;
			}
		} while (FindNextFileW(search, &found));
		FindClose(search);
		gLog.FormattedMessage(
			"tnvse_freetype_font: unused persistent cache cleanup deleted=%u bytes=%llu failed=%u retained=%llu",
			deleted, static_cast<unsigned long long>(deletedBytes), failed,
			static_cast<unsigned long long>(State().usedPersistentCachePaths.size()));
	}

	bool HasCompleteGlyphManifest(RuntimeFont& runtime, FontPrewarmMode mode)
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		PersistentGlyphManifest* manifest = GetGlyphManifest(runtime);
		if (!manifest || !manifest->mappedData)
			return false;
		const auto* header = reinterpret_cast<const PersistentGlyphManifestHeader*>(
			manifest->mappedData);
		return header->completeMode >= static_cast<UInt8>(mode);
	}

	void MarkGlyphManifestComplete(RuntimeFont& runtime, FontPrewarmMode mode)
	{
		std::lock_guard<std::recursive_mutex> lock(State().mutex);
		PersistentGlyphManifest* manifest = GetGlyphManifest(runtime);
		if (!manifest || !manifest->mappedData || !manifest->writable)
			return;
		auto* header = reinterpret_cast<PersistentGlyphManifestHeader*>(
			manifest->mappedData);
		if (header->completeMode >= static_cast<UInt8>(mode))
			return;
		header->completeMode = static_cast<UInt8>(mode);
		header->checksum = HashBytes64(header,
			offsetof(PersistentGlyphManifestHeader, checksum));
		FlushViewOfFile(header, sizeof(*header));
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
		for (auto& pair : State().runtimeFonts)
		{
			RuntimeFont& runtime = *pair.second;
			if (!runtime.manifest || !runtime.manifest->mappedData)
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
}

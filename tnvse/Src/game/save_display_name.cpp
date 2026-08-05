#include "save_display_name.h"

#include "encoding.h"
#include "hook_identity.h"
#include "load_config.h"
#include "SafeWrite.h"
#include "tnvse.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <Windows.h>

namespace fonthook
{
	void CaptureSaveDisplayName(const char* originalName, const char* actualName);

	namespace implementation::save_display_name {}
	using namespace implementation::save_display_name;

	namespace implementation::save_display_name
	{
		using hook_identity::Rel32Opcode;

		constexpr SIZE_T kSavePathProcessCallSite = 0x8518BB;
		constexpr SIZE_T kStockSavePathProcess = 0x8518D0;
		constexpr SIZE_T kManualSaveNameCheckCallSite = 0x851AAE;
		constexpr SIZE_T kStockManualSaveNameCheck = 0x851980;
		// SaveGameManager::Save has already copied the supplied name into the
		// save header before this call, but 0x850030 has not yet built the
		// physical .fos path.  Replacing only this call preserves display text.
		constexpr SIZE_T kCustomSaveFileBuildCallSite = 0x850545;

		constexpr UInt32 kStoreMagic = 'SVDN';
		constexpr UInt32 kStoreVersion = 1;
		constexpr UInt32 kMaxStoreFileSize = 16 * 1024 * 1024;
		constexpr UInt32 kMaxStoreRecords = 8192;
		constexpr UInt32 kMaxActualKeyLen = 260;
		constexpr UInt32 kMaxSavePathKeyLen = MAX_PATH * 2;
		constexpr UInt32 kMaxDisplayNameLen = 1024;

		struct StoreHeader
		{
			UInt32 magic;
			UInt32 version;
			UInt32 recordCount;
		};

		struct StoreRecordHeader
		{
			UInt32 actualKeyLen;
			UInt32 savePathKeyLen;
			UInt32 displayNameLen;
			UInt32 uiEncoding;
			UInt32 codePage;
		};

		struct FileStamp
		{
			bool exists = false;
			UInt32 size = 0;
			FILETIME writeTime = {};
		};

		struct DisplayRecord
		{
			std::string actualKey;
			std::string savePathKey;
			std::string displayNameMb;
			UInt32 uiEncoding = 0;
			UInt32 codePage = 0;
		};

		struct CacheEntry
		{
			FileStamp stamp;
			bool hasRecord = false;
			std::string displayNameMb;
		};

		std::unordered_map<std::string, DisplayRecord> s_pendingByActualKey;
		std::unordered_map<std::string, CacheEntry> s_displayCache;
		std::string s_pendingRenameOldPath;
		SIZE_T s_nextCustomSaveFileBuild = 0;
		SIZE_T s_nextManualSaveNameCheck = 0;

		std::string ToLowerAscii(std::string value)
		{
			for (char& c : value)
			{
				if (c >= 'A' && c <= 'Z')
					c = static_cast<char>(c - 'A' + 'a');
			}
			return value;
		}

		bool EndsWithInsensitive(const std::string& value, const char* suffix)
		{
			const std::string lowerValue = ToLowerAscii(value);
			const std::string lowerSuffix = ToLowerAscii(suffix);
			return lowerValue.size() >= lowerSuffix.size()
				&& lowerValue.compare(lowerValue.size() - lowerSuffix.size(), lowerSuffix.size(), lowerSuffix) == 0;
		}

		std::string StripPath(std::string path)
		{
			const size_t slash = path.find_last_of("\\/");
			if (slash != std::string::npos)
				path.erase(0, slash + 1);
			return path;
		}

		std::string StripFosExtension(std::string name)
		{
			if (EndsWithInsensitive(name, ".fos"))
				name.resize(name.size() - 4);
			return name;
		}

		bool CopyBoundedLocalCString(const char* value, size_t maxLength, std::string& outValue)
		{
			outValue.clear();
			if (!value || !maxLength)
				return false;

			size_t length = 0;
			while (length < maxLength && value[length])
				++length;

			if (length == maxLength)
				return false;

			outValue.assign(value, length);
			return true;
		}

		std::string ExtractActualKey(const char* pathOrName)
		{
			if (!pathOrName || !*pathOrName)
				return {};
			return StripFosExtension(StripPath(pathOrName));
		}

		std::string GetGameDirectory()
		{
			char path[MAX_PATH] = {};
			GetModuleFileNameA(nullptr, path, MAX_PATH);
			path[MAX_PATH - 1] = '\0';

			char* slash = std::strrchr(path, '\\');
			if (!slash)
				return {};
			*slash = '\0';
			return path;
		}

		bool DirectoryExists(const std::string& path)
		{
			const DWORD attributes = GetFileAttributesA(path.c_str());
			return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
		}

		std::string GetStoreDirectory()
		{
			const std::string gameDirectory = GetGameDirectory();
			if (gameDirectory.empty())
				return {};
			return gameDirectory + "\\Data\\NVSE\\plugins\\tnvse";
		}

		std::string GetStorePath()
		{
			const std::string directory = GetStoreDirectory();
			if (directory.empty())
				return {};
			return directory + "\\save_display_names.dat";
		}

		bool EnsureStoreDirectory()
		{
			const std::string gameDirectory = GetGameDirectory();
			if (gameDirectory.empty())
				return false;

			const std::string dataDirectory = gameDirectory + "\\Data";
			const std::string nvseDirectory = dataDirectory + "\\NVSE";
			const std::string pluginsDirectory = nvseDirectory + "\\plugins";
			const std::string storeDirectory = pluginsDirectory + "\\tnvse";

			CreateDirectoryA(dataDirectory.c_str(), nullptr);
			CreateDirectoryA(nvseDirectory.c_str(), nullptr);
			CreateDirectoryA(pluginsDirectory.c_str(), nullptr);
			CreateDirectoryA(storeDirectory.c_str(), nullptr);
			return DirectoryExists(storeDirectory);
		}

		std::string NormalizeSavePathKey(std::string path)
		{
			std::replace(path.begin(), path.end(), '/', '\\');
			return ToLowerAscii(path);
		}

		std::string GetSaveGamePath()
		{
			char savePath[MAX_PATH] = {};
			StdCall<BOOL>(0x84FF30, savePath);
			savePath[MAX_PATH - 1] = '\0';

			std::string result;
			if (!CopyBoundedLocalCString(savePath, MAX_PATH, result))
				return {};
			return result;
		}

		std::string BuildSavePathFromActualKey(const std::string& actualKey)
		{
			if (actualKey.empty() || actualKey.size() >= MAX_PATH)
				return {};

			char savePath[MAX_PATH] = {};
			StdCall<void>(0x84FF90, actualKey.c_str(), savePath, static_cast<UInt8>(0));
			savePath[MAX_PATH - 1] = '\0';

			std::string result;
			if (!CopyBoundedLocalCString(savePath, MAX_PATH, result))
				result.clear();
			if (!result.empty())
				return result;

			result = GetSaveGamePath();
			if (result.empty() || result.size() >= MAX_PATH)
				return {};

			if (!result.empty() && result.back() != '\\' && result.back() != '/')
				result.push_back('\\');
			result += actualKey;
			result += ".fos";
			return result;
		}

		std::string ResolveSavePathKey(const std::string& savePath, const std::string& actualKey)
		{
			std::string resolvedPath = savePath;
			if (resolvedPath.empty() || resolvedPath.find_last_of("\\/") == std::string::npos)
				resolvedPath = BuildSavePathFromActualKey(actualKey);
			if (resolvedPath.empty() || resolvedPath.size() > kMaxSavePathKeyLen)
				return {};
			return NormalizeSavePathKey(resolvedPath);
		}

		std::string MakeCacheKey(const std::string& savePathKey, const std::string& actualKey)
		{
			return savePathKey + "\n" + actualKey;
		}

		bool SameStamp(const FileStamp& lhs, const FileStamp& rhs)
		{
			return lhs.exists == rhs.exists
				&& lhs.size == rhs.size
				&& lhs.writeTime.dwLowDateTime == rhs.writeTime.dwLowDateTime
				&& lhs.writeTime.dwHighDateTime == rhs.writeTime.dwHighDateTime;
		}

		FileStamp GetFileStamp(const std::string& path)
		{
			FileStamp stamp;
			WIN32_FILE_ATTRIBUTE_DATA data = {};
			if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
				return stamp;

			stamp.exists = true;
			stamp.size = data.nFileSizeLow;
			stamp.writeTime = data.ftLastWriteTime;
			return stamp;
		}

		bool ReadWholeFile(const std::string& path, std::vector<UInt8>& outData)
		{
			HANDLE file = CreateFileA(
				path.c_str(),
				GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return false;

			LARGE_INTEGER size = {};
			if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > kMaxStoreFileSize)
			{
				CloseHandle(file);
				return false;
			}

			outData.resize(static_cast<size_t>(size.QuadPart));
			DWORD bytesRead = 0;
			const BOOL ok = ReadFile(file, outData.data(), static_cast<DWORD>(outData.size()), &bytesRead, nullptr);
			CloseHandle(file);
			return ok && bytesRead == outData.size();
		}

		bool IsReadableProtection(DWORD protect)
		{
			if (protect & (PAGE_GUARD | PAGE_NOACCESS))
				return false;

			protect &= 0xFF;
			return protect == PAGE_READONLY
				|| protect == PAGE_READWRITE
				|| protect == PAGE_WRITECOPY
				|| protect == PAGE_EXECUTE_READ
				|| protect == PAGE_EXECUTE_READWRITE
				|| protect == PAGE_EXECUTE_WRITECOPY;
		}

		bool TryCopyCString(const char* value, size_t maxLength, std::string& outValue)
		{
			outValue.clear();
			if (!value || !maxLength)
				return false;

			const UInt8* current = reinterpret_cast<const UInt8*>(value);
			size_t remaining = maxLength;
			while (remaining)
			{
				MEMORY_BASIC_INFORMATION memoryInfo = {};
				if (!VirtualQuery(current, &memoryInfo, sizeof(memoryInfo))
					|| memoryInfo.State != MEM_COMMIT
					|| !IsReadableProtection(memoryInfo.Protect))
				{
					return false;
				}

				const UInt8* regionEnd = static_cast<const UInt8*>(memoryInfo.BaseAddress) + memoryInfo.RegionSize;
				if (current >= regionEnd)
					return false;

				const size_t chunkSize = std::min<size_t>(remaining, regionEnd - current);
				for (size_t i = 0; i < chunkSize; ++i)
				{
					if (!current[i])
						return true;
					outValue.push_back(static_cast<char>(current[i]));
				}

				current += chunkSize;
				remaining -= chunkSize;
			}

			outValue.clear();
			return false;
		}

		std::string TrimAsciiWhitespace(std::string value)
		{
			const char* whitespace = " \t\r\n";
			const size_t begin = value.find_first_not_of(whitespace);
			if (begin == std::string::npos)
				return {};

			const size_t end = value.find_last_not_of(whitespace);
			return value.substr(begin, end - begin + 1);
		}

		void CollectAsciiDelimiterPositionsOutsideDbcs(
			const std::string& text,
			char delimiter,
			std::vector<size_t>& outPositions)
		{
			outPositions.clear();
			for (size_t i = 0; i < text.size(); ++i)
			{
				UInt32 dbcsCode = 0;
				if (i + 1 < text.size() && TryDecodeDoubleByte(&text[i], dbcsCode))
				{
					++i;
					continue;
				}

				if (text[i] == delimiter)
					outPositions.push_back(i);
			}
		}

		size_t FindAsciiTokenOutsideDbcs(const std::string& text, const char* token)
		{
			if (!token || !*token)
				return std::string::npos;

			const size_t tokenLen = std::strlen(token);
			for (size_t i = 0; i < text.size(); ++i)
			{
				UInt32 dbcsCode = 0;
				if (i + 1 < text.size() && TryDecodeDoubleByte(&text[i], dbcsCode))
				{
					++i;
					continue;
				}

				if (i + tokenLen <= text.size() && std::memcmp(text.data() + i, token, tokenLen) == 0)
					return i;
			}

			return std::string::npos;
		}

		bool TryExtractManualSaveLocation(
			const std::string& displayNameMb,
			std::string& outLocation)
		{
			outLocation.clear();

			std::vector<size_t> commaPositions;
			CollectAsciiDelimiterPositionsOutsideDbcs(displayNameMb, ',', commaPositions);
			if (commaPositions.size() < 2)
				return false;

			const size_t lastComma = commaPositions.back();
			const size_t previousComma = commaPositions[commaPositions.size() - 2];

			const size_t headerSep = FindAsciiTokenOutsideDbcs(displayNameMb, " - ");
			if (headerSep == std::string::npos || headerSep > previousComma)
				return false;

			outLocation = TrimAsciiWhitespace(displayNameMb.substr(previousComma + 1, lastComma - previousComma - 1));
			return !outLocation.empty();
		}

		bool LooksLikeManualSaveDisplayName(const std::string& displayNameMb)
		{
			std::string location;
			return TryExtractManualSaveLocation(displayNameMb, location);
		}

		bool StartsWithAsciiInsensitive(const std::string& value, const char* prefix)
		{
			if (!prefix)
				return false;

			for (size_t i = 0; prefix[i]; ++i)
			{
				if (i >= value.size())
					return false;

				char lhs = value[i];
				char rhs = prefix[i];
				if (lhs >= 'A' && lhs <= 'Z')
					lhs = static_cast<char>(lhs - 'A' + 'a');
				if (rhs >= 'A' && rhs <= 'Z')
					rhs = static_cast<char>(rhs - 'A' + 'a');
				if (lhs != rhs)
					return false;
			}
			return true;
		}

		bool CharInList(const char* list, char c)
		{
			while (*list)
			{
				if (*list == c)
					return true;
				++list;
			}
			return false;
		}

		bool ContainsHighByte(const std::string& value)
		{
			return std::any_of(value.begin(), value.end(), [](char c)
			{
				return static_cast<UInt8>(c) >= 0x80;
			});
		}

		void SanitizeSaveFileName(char* fileName, size_t length)
		{
			if (!fileName)
				return;

			for (size_t i = 0; i < length; ++i)
			{
				const UInt8 c = static_cast<UInt8>(fileName[i]);
				if (c < '0' || c > 'z')
				{
					fileName[i] = ' ';
				}
				else if (CharInList("\t\\/:*<>?|\"+=@^[]`;", static_cast<char>(c)))
				{
					fileName[i] = c == '"' ? '\'' : ' ';
				}
			}
		}

		void __fastcall SavePathProcess(void*, UInt32, char* fileName)
		{
			if (!fileName)
				return;

			std::string originalName;
			if (!TryCopyCString(fileName, MAX_PATH, originalName))
				return;

			SanitizeSaveFileName(fileName, originalName.size());
			CaptureSaveDisplayName(originalName.c_str(), fileName);
		}

		void* __fastcall BuildCustomSaveFileWithSafeName(
			void* saveManager,
			void*,
			const char* fileName,
			UInt32 createFile,
			UInt32 bufferMode,
			SInt32 saveIndex)
		{
			// The fastcall shim consumes ECX/EDX and leaves the original four
			// thiscall stack arguments intact.  The target captured from the CALL
			// site owns the same ABI and the 0x10-byte stack cleanup.
			const SIZE_T nextTarget = s_nextCustomSaveFileBuild;
			if (!hook_identity::IsExecutableTarget(nextTarget)
				|| nextTarget == reinterpret_cast<SIZE_T>(
					&BuildCustomSaveFileWithSafeName))
			{
				return nullptr;
			}

			std::string originalName;
			if (!TryCopyCString(fileName, MAX_PATH, originalName)
				|| originalName.empty()
				|| !ContainsHighByte(originalName))
			{
				return ThisStdCall<void*>(nextTarget, saveManager,
					fileName, createFile, bufferMode, saveIndex);
			}

			char safeName[MAX_PATH] = {};
			std::memcpy(safeName, originalName.data(), originalName.size());
			SanitizeSaveFileName(safeName, originalName.size());
			CaptureSaveDisplayName(originalName.c_str(), safeName);
			gLog.FormattedMessage(
				"tnvse_save_display_name: sanitized custom multibyte save name bytes=%u",
				static_cast<UInt32>(originalName.size()));

			return ThisStdCall<void*>(nextTarget, saveManager,
				safeName, createFile, bufferMode, saveIndex);
		}

		template <class T>
		bool ReadValue(const std::vector<UInt8>& data, size_t& offset, T& outValue)
		{
			if (offset + sizeof(T) > data.size())
				return false;
			std::memcpy(&outValue, data.data() + offset, sizeof(T));
			offset += sizeof(T);
			return true;
		}

		bool ReadBytes(const std::vector<UInt8>& data, size_t& offset, std::string& outValue, UInt32 length)
		{
			if (offset + length > data.size())
				return false;
			outValue.assign(reinterpret_cast<const char*>(data.data() + offset), length);
			offset += length;
			return true;
		}

		void AppendBytes(std::vector<UInt8>& outData, const void* data, size_t length)
		{
			if (!length)
				return;

			const UInt8* bytes = static_cast<const UInt8*>(data);
			outData.insert(outData.end(), bytes, bytes + length);
		}

		template <class T>
		void AppendValue(std::vector<UInt8>& outData, const T& value)
		{
			AppendBytes(outData, &value, sizeof(value));
		}

		bool WriteWholeFileAtomic(const std::string& path, const std::vector<UInt8>& data)
		{
			if (path.empty() || data.empty())
				return false;

			const std::string tempPath = path + ".tnvse.tmp";
			HANDLE file = CreateFileA(
				tempPath.c_str(),
				GENERIC_WRITE,
				0,
				nullptr,
				CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return false;

			DWORD bytesWritten = 0;
			const BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &bytesWritten, nullptr);
			if (ok)
				FlushFileBuffers(file);
			CloseHandle(file);

			if (!ok || bytesWritten != data.size())
			{
				DeleteFileA(tempPath.c_str());
				return false;
			}

			if (!MoveFileExA(
				tempPath.c_str(),
				path.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				DeleteFileA(tempPath.c_str());
				return false;
			}

			return true;
		}

		bool IsValidRecord(const DisplayRecord& record)
		{
			return !record.actualKey.empty()
				&& !record.savePathKey.empty()
				&& !record.displayNameMb.empty()
				&& record.actualKey.size() <= kMaxActualKeyLen
				&& record.savePathKey.size() <= kMaxSavePathKeyLen
				&& record.displayNameMb.size() <= kMaxDisplayNameLen;
		}

		bool ReadStoreRecords(std::vector<DisplayRecord>& outRecords)
		{
			outRecords.clear();

			const std::string storePath = GetStorePath();
			if (storePath.empty())
				return false;
			if (!GetFileStamp(storePath).exists)
				return true;

			std::vector<UInt8> data;
			if (!ReadWholeFile(storePath, data))
				return false;

			size_t offset = 0;
			StoreHeader header = {};
			if (!ReadValue(data, offset, header)
				|| header.magic != kStoreMagic
				|| header.version != kStoreVersion
				|| header.recordCount > kMaxStoreRecords)
			{
				return false;
			}

			outRecords.reserve(header.recordCount);
			for (UInt32 i = 0; i < header.recordCount; ++i)
			{
				StoreRecordHeader recordHeader = {};
				if (!ReadValue(data, offset, recordHeader))
					return false;
				if (!recordHeader.actualKeyLen
					|| !recordHeader.savePathKeyLen
					|| !recordHeader.displayNameLen
					|| recordHeader.actualKeyLen > kMaxActualKeyLen
					|| recordHeader.savePathKeyLen > kMaxSavePathKeyLen
					|| recordHeader.displayNameLen > kMaxDisplayNameLen)
				{
					return false;
				}

				DisplayRecord record;
				record.uiEncoding = recordHeader.uiEncoding;
				record.codePage = recordHeader.codePage;
				if (!ReadBytes(data, offset, record.actualKey, recordHeader.actualKeyLen)
					|| !ReadBytes(data, offset, record.savePathKey, recordHeader.savePathKeyLen)
					|| !ReadBytes(data, offset, record.displayNameMb, recordHeader.displayNameLen)
					|| !IsValidRecord(record))
				{
					return false;
				}

				outRecords.push_back(std::move(record));
			}

			return offset == data.size();
		}

		bool WriteStoreRecords(const std::vector<DisplayRecord>& records)
		{
			if (!EnsureStoreDirectory())
				return false;

			std::vector<DisplayRecord> validRecords;
			validRecords.reserve(records.size());
			for (const DisplayRecord& record : records)
			{
				if (IsValidRecord(record))
					validRecords.push_back(record);
			}
			if (validRecords.size() > kMaxStoreRecords)
				return false;

			StoreHeader header = {};
			header.magic = kStoreMagic;
			header.version = kStoreVersion;
			header.recordCount = static_cast<UInt32>(validRecords.size());

			std::vector<UInt8> data;
			data.reserve(sizeof(header) + validRecords.size() * 128);
			AppendValue(data, header);
			for (const DisplayRecord& record : validRecords)
			{
				StoreRecordHeader recordHeader = {};
				recordHeader.actualKeyLen = static_cast<UInt32>(record.actualKey.size());
				recordHeader.savePathKeyLen = static_cast<UInt32>(record.savePathKey.size());
				recordHeader.displayNameLen = static_cast<UInt32>(record.displayNameMb.size());
				recordHeader.uiEncoding = record.uiEncoding;
				recordHeader.codePage = record.codePage;

				AppendValue(data, recordHeader);
				AppendBytes(data, record.actualKey.data(), record.actualKey.size());
				AppendBytes(data, record.savePathKey.data(), record.savePathKey.size());
				AppendBytes(data, record.displayNameMb.data(), record.displayNameMb.size());
			}

			return WriteWholeFileAtomic(GetStorePath(), data);
		}

		bool FindDisplayRecord(
			const std::string& savePathKey,
			const std::string& actualKey,
			DisplayRecord& outRecord)
		{
			if (savePathKey.empty() || actualKey.empty())
				return false;

			std::vector<DisplayRecord> records;
			if (!ReadStoreRecords(records))
				return false;

			for (auto it = records.rbegin(); it != records.rend(); ++it)
			{
				if (it->actualKey == actualKey && it->savePathKey == savePathKey)
				{
					outRecord = *it;
					return true;
				}
			}
			return false;
		}

		bool UpsertDisplayRecord(const DisplayRecord& record)
		{
			if (!IsValidRecord(record))
				return false;

			std::vector<DisplayRecord> records;
			if (!ReadStoreRecords(records))
				return false;

			records.erase(
				std::remove_if(
					records.begin(),
					records.end(),
					[&](const DisplayRecord& entry)
					{
						return entry.actualKey == record.actualKey && entry.savePathKey == record.savePathKey;
					}),
				records.end());
			records.push_back(record);
			return WriteStoreRecords(records);
		}

		bool DeleteDisplayRecord(const std::string& savePathKey, const std::string& actualKey)
		{
			if (savePathKey.empty() || actualKey.empty())
				return false;

			std::vector<DisplayRecord> records;
			if (!ReadStoreRecords(records))
				return false;

			const size_t oldSize = records.size();
			records.erase(
				std::remove_if(
					records.begin(),
					records.end(),
					[&](const DisplayRecord& entry)
					{
						return entry.actualKey == actualKey && entry.savePathKey == savePathKey;
					}),
				records.end());

			if (records.size() == oldSize)
				return true;
			return WriteStoreRecords(records);
		}

		bool MoveDisplayRecord(const std::string& oldSavePath, const std::string& newSavePath)
		{
			const std::string oldActualKey = ExtractActualKey(oldSavePath.c_str());
			const std::string newActualKey = ExtractActualKey(newSavePath.c_str());
			const std::string oldSavePathKey = ResolveSavePathKey(oldSavePath, oldActualKey);
			const std::string newSavePathKey = ResolveSavePathKey(newSavePath, newActualKey);
			if (oldActualKey.empty() || newActualKey.empty() || oldSavePathKey.empty() || newSavePathKey.empty())
				return false;

			std::vector<DisplayRecord> records;
			if (!ReadStoreRecords(records))
				return false;

			DisplayRecord movedRecord;
			bool found = false;
			records.erase(
				std::remove_if(
					records.begin(),
					records.end(),
					[&](const DisplayRecord& entry)
					{
						if (entry.actualKey == oldActualKey && entry.savePathKey == oldSavePathKey)
						{
							movedRecord = entry;
							found = true;
							return true;
						}
						return entry.actualKey == newActualKey && entry.savePathKey == newSavePathKey;
					}),
				records.end());

			if (!found)
				return true;

			movedRecord.actualKey = newActualKey;
			movedRecord.savePathKey = newSavePathKey;
			if (IsValidRecord(movedRecord))
				records.push_back(movedRecord);

			return WriteStoreRecords(records);
		}

		bool ConvertCodePage(const std::string& src, UInt32 fromCodePage, UInt32 toCodePage, std::string& out)
		{
			if (fromCodePage == toCodePage)
			{
				out = src;
				return true;
			}
			if (!fromCodePage || !toCodePage)
				return false;

			const int wideLen = MultiByteToWideChar(fromCodePage, 0, src.data(), static_cast<int>(src.size()), nullptr, 0);
			if (wideLen <= 0)
				return false;

			std::wstring wide(static_cast<size_t>(wideLen), L'\0');
			MultiByteToWideChar(fromCodePage, 0, src.data(), static_cast<int>(src.size()), &wide[0], wideLen);

			const int mbLen = WideCharToMultiByte(toCodePage, 0, wide.data(), wideLen, nullptr, 0, nullptr, nullptr);
			if (mbLen <= 0)
				return false;

			out.assign(static_cast<size_t>(mbLen), '\0');
			WideCharToMultiByte(toCodePage, 0, wide.data(), wideLen, &out[0], mbLen, nullptr, nullptr);
			return true;
		}

		std::string PrepareDisplayNameMb(const std::string& originalName)
		{
			if (g_bEnableUTF8 && IsEastAsianUiMode()
				&& IsValidUTF8With3ByteMin(originalName.c_str()))
			{
				const std::string converted = UTF8ToMultiByteStr(originalName, g_usingWinEncoding);
				if (!converted.empty())
					return converted;
			}
			return originalName;
		}

		bool ResolveDisplayNameForSavePath(
			const std::string& savePath,
			const std::string& actualKey,
			std::string& outDisplayNameMb)
		{
			if (actualKey.empty() || actualKey.size() >= MAX_PATH)
				return false;

			const std::string savePathKey = ResolveSavePathKey(savePath, actualKey);
			if (savePathKey.empty())
				return false;

			DisplayRecord record;
			if (!FindDisplayRecord(savePathKey, actualKey, record))
				return false;

			const UInt32 storedCodePage = record.codePage
				? record.codePage : kWindows1252CodePage;
			if (storedCodePage == g_usingWinEncoding)
			{
				outDisplayNameMb = record.displayNameMb;
				return true;
			}

			return ConvertCodePage(record.displayNameMb, storedCodePage,
				g_usingWinEncoding, outDisplayNameMb);
		}

		bool LoadCachedDisplayName(const std::string& actualKey, std::string& outDisplayNameMb)
		{
			if (actualKey.empty() || actualKey.size() >= MAX_PATH)
				return false;

			const std::string savePath = BuildSavePathFromActualKey(actualKey);
			if (savePath.empty())
				return false;

			const std::string savePathKey = ResolveSavePathKey(savePath, actualKey);
			if (savePathKey.empty())
				return false;

			const FileStamp stamp = GetFileStamp(GetStorePath());
			const std::string cacheKey = MakeCacheKey(savePathKey, actualKey);
			auto cached = s_displayCache.find(cacheKey);
			if (cached != s_displayCache.end() && SameStamp(cached->second.stamp, stamp))
			{
				if (!cached->second.hasRecord)
					return false;
				outDisplayNameMb = cached->second.displayNameMb;
				return true;
			}

			CacheEntry entry;
			entry.stamp = stamp;
			entry.hasRecord = ResolveDisplayNameForSavePath(savePath, actualKey, entry.displayNameMb);
			s_displayCache[cacheKey] = entry;

			if (!entry.hasRecord)
				return false;
			outDisplayNameMb = entry.displayNameMb;
			return true;
		}

		bool __fastcall SaveDisplayNameIsManualSave(
			void* saveManager, void*, const char* actualName)
		{
			const SIZE_T nextTarget = s_nextManualSaveNameCheck;
			if (hook_identity::IsExecutableTarget(nextTarget)
				&& nextTarget != reinterpret_cast<SIZE_T>(
					&SaveDisplayNameIsManualSave)
				&& ThisStdCall<bool>(nextTarget, saveManager, actualName))
				return true;

			std::string actualKey;
			if (!TryCopyCString(actualName, MAX_PATH, actualKey) || actualKey.empty())
				return false;

			if (StartsWithAsciiInsensitive(actualKey, "Save "))
				return true;

			std::string displayNameMb;
			return LoadCachedDisplayName(ExtractActualKey(actualKey.c_str()), displayNameMb)
				&& LooksLikeManualSaveDisplayName(displayNameMb);
		}

		bool ShouldStoreDisplayRecord(const DisplayRecord& record)
		{
			return IsValidRecord(record)
				&& record.displayNameMb != record.actualKey
				&& !StartsWithAsciiInsensitive(record.actualKey, "Save ");
		}

		void UpdateDisplayRecordForSavePath(const std::string& savePath)
		{
			const std::string actualKey = ExtractActualKey(savePath.c_str());
			if (actualKey.empty())
				return;

			const std::string savePathKey = ResolveSavePathKey(savePath, actualKey);
			if (savePathKey.empty())
				return;

			s_displayCache.erase(MakeCacheKey(savePathKey, actualKey));

			DisplayRecord record;
			bool hasRecord = false;

			auto pending = s_pendingByActualKey.find(actualKey);
			if (pending != s_pendingByActualKey.end())
			{
				if (pending->second.displayNameMb == actualKey)
				{
					if (FindDisplayRecord(savePathKey, actualKey, record))
					{
						record.uiEncoding = g_uiEncoding;
						record.codePage = g_usingWinEncoding;
					}
					else
					{
						record = pending->second;
						record.savePathKey = savePathKey;
					}
				}
				else
				{
					record = pending->second;
					record.savePathKey = savePathKey;
				}

				hasRecord = true;
				s_pendingByActualKey.erase(pending);
			}

			if (!hasRecord)
				return;

			if (ShouldStoreDisplayRecord(record))
				UpsertDisplayRecord(record);
			else
				DeleteDisplayRecord(savePathKey, actualKey);

			s_displayCache.clear();
		}

		void DeleteDisplayRecordForSavePath(const std::string& savePath)
		{
			const std::string actualKey = ExtractActualKey(savePath.c_str());
			const std::string savePathKey = ResolveSavePathKey(savePath, actualKey);
			if (actualKey.empty() || savePathKey.empty())
				return;

			DeleteDisplayRecord(savePathKey, actualKey);
			s_displayCache.clear();
		}

		void RenameDisplayRecordForSavePath(const std::string& oldSavePath, const std::string& newSavePath)
		{
			if (oldSavePath.empty() || newSavePath.empty())
				return;

			MoveDisplayRecord(oldSavePath, newSavePath);
			s_displayCache.clear();
		}

		std::string MessageDataToString(NVSEMessagingInterface::Message* message)
		{
			if (!message || !message->data || !message->dataLen)
				return {};

			std::string value(static_cast<const char*>(message->data), message->dataLen);
			const size_t terminator = value.find('\0');
			if (terminator != std::string::npos)
				value.resize(terminator);
			return value;
		}
	}

	void HandleSaveDisplayNameMessage(NVSEMessagingInterface::Message* message)
	{
		if (!g_bSaveDisplayNameMap || !message)
			return;

		if (message->type == NVSEMessagingInterface::kMessage_SaveGame)
		{
			UpdateDisplayRecordForSavePath(MessageDataToString(message));
		}
		else if (message->type == NVSEMessagingInterface::kMessage_DeleteGame)
		{
			DeleteDisplayRecordForSavePath(MessageDataToString(message));
		}
		else if (message->type == NVSEMessagingInterface::kMessage_RenameGame)
		{
			s_pendingRenameOldPath = MessageDataToString(message);
		}
		else if (message->type == NVSEMessagingInterface::kMessage_RenameNewGame)
		{
			RenameDisplayRecordForSavePath(s_pendingRenameOldPath, MessageDataToString(message));
			s_pendingRenameOldPath.clear();
		}
	}

	void CaptureSaveDisplayName(const char* originalName, const char* actualName)
	{
		if (!g_bSaveDisplayNameMap || !originalName || !actualName || !*actualName)
			return;

		const std::string actualKey = ExtractActualKey(actualName);
		if (actualKey.empty())
			return;

		DisplayRecord record;
		record.actualKey = actualKey;
		record.displayNameMb = PrepareDisplayNameMb(originalName);
		record.uiEncoding = g_uiEncoding;
		record.codePage = g_usingWinEncoding;
		if (record.displayNameMb.empty())
			return;

		s_pendingByActualKey[actualKey] = record;
		s_displayCache.clear();
	}

	void InitSaveDisplayNameHook()
	{
		const SIZE_T customSaveFileHook =
			reinterpret_cast<SIZE_T>(&BuildCustomSaveFileWithSafeName);
		SIZE_T customSaveFileTarget = 0;
		if (!hook_identity::ReadRel32Target(
				kCustomSaveFileBuildCallSite,
				Rel32Opcode::Call,
				customSaveFileTarget)
			|| !hook_identity::IsExecutableTarget(customSaveFileTarget)
			|| customSaveFileTarget == customSaveFileHook)
		{
			gLog.FormattedMessage(
				"tnvse_save_display_name: custom save hook target invalid target=%08X; disabled",
				static_cast<UInt32>(customSaveFileTarget));
		}
		else
		{
			// Preserve whichever compatible hook currently owns the CALL.  In a
			// Stewie Tweaks setup this is SaveGameManager__CreateSaveLoadFile,
			// which in turn calls the stock 0x850030 implementation.
			s_nextCustomSaveFileBuild = customSaveFileTarget;
			WriteRelCall(kCustomSaveFileBuildCallSite,
				&BuildCustomSaveFileWithSafeName);
			if (!hook_identity::MatchesRel32Target(
					kCustomSaveFileBuildCallSite,
					Rel32Opcode::Call,
					customSaveFileHook))
			{
				WriteRelCall(kCustomSaveFileBuildCallSite,
					customSaveFileTarget);
				const bool restored = hook_identity::MatchesRel32Target(
					kCustomSaveFileBuildCallSite,
					Rel32Opcode::Call,
					customSaveFileTarget);
				gLog.FormattedMessage(
					"tnvse_save_display_name: custom save hook write verification failed; restore target=%08X restored=%u",
					static_cast<UInt32>(customSaveFileTarget),
					restored ? 1u : 0u);
			}
			else
			{
				gLog.FormattedMessage(
					"tnvse_save_display_name: custom multibyte save sanitizer installed next=%08X",
					static_cast<UInt32>(customSaveFileTarget));
			}
		}

		if (!g_bSaveDisplayNameMap)
			return;

		SIZE_T savePathTarget = 0;
		SIZE_T manualSaveTarget = 0;
		const SIZE_T manualSaveHook = reinterpret_cast<SIZE_T>(
			&SaveDisplayNameIsManualSave);
		if (!hook_identity::ReadRel32Target(
				kSavePathProcessCallSite, Rel32Opcode::Call, savePathTarget)
			|| savePathTarget != kStockSavePathProcess
			|| !hook_identity::ReadRel32Target(
				kManualSaveNameCheckCallSite,
				Rel32Opcode::Call,
				manualSaveTarget)
			|| !hook_identity::IsExecutableTarget(manualSaveTarget)
			|| manualSaveTarget == manualSaveHook)
		{
			gLog.FormattedMessage(
				"tnvse_save_display_name: hook identity mismatch path=%08X manual=%08X; disabled",
				static_cast<UInt32>(savePathTarget),
				static_cast<UInt32>(manualSaveTarget));
			return;
		}

		s_nextManualSaveNameCheck = manualSaveTarget;
		WriteRelCall(kSavePathProcessCallSite, &SavePathProcess);
		WriteRelCall(kManualSaveNameCheckCallSite,
			&SaveDisplayNameIsManualSave);

		const bool savePathInstalled = hook_identity::MatchesRel32Target(
			kSavePathProcessCallSite,
			Rel32Opcode::Call,
			reinterpret_cast<SIZE_T>(&SavePathProcess));
		const bool manualSaveInstalled = hook_identity::MatchesRel32Target(
				kManualSaveNameCheckCallSite,
				Rel32Opcode::Call,
				manualSaveHook);
		if (!savePathInstalled || !manualSaveInstalled)
		{
			if (savePathInstalled)
			{
				WriteRelCall(kSavePathProcessCallSite, savePathTarget);
			}

			SIZE_T observedManualTarget = 0;
			const bool observedManualCall = hook_identity::ReadRel32Target(
				kManualSaveNameCheckCallSite,
				Rel32Opcode::Call,
				observedManualTarget);
			if (manualSaveInstalled)
			{
				WriteRelCall(kManualSaveNameCheckCallSite,
					manualSaveTarget);
				s_nextManualSaveNameCheck = 0;
			}
			else if (!observedManualCall
				|| observedManualTarget == manualSaveTarget)
			{
				// tNVSE was never published at this site.
				s_nextManualSaveNameCheck = 0;
			}
			else
			{
				// A successor may have captured tNVSE between the write and readback.
				// Retain its predecessor and leave the successor in place.
				gLog.FormattedMessage(
					"tnvse_save_display_name: manual-save hook changed during publication successor=%08X predecessor=%08X; tNVSE left below successor",
					static_cast<UInt32>(observedManualTarget),
					static_cast<UInt32>(manualSaveTarget));
			}
			gLog.FormattedMessage(
				"tnvse_save_display_name: hook write verification failed; restored captured targets where still owned");
			return;
		}

		gLog.FormattedMessage(
			"tnvse_save_display_name: display-name hooks installed manualNext=%08X manualStock=%u",
			static_cast<UInt32>(manualSaveTarget),
			manualSaveTarget == kStockManualSaveNameCheck ? 1u : 0u);
	}
}

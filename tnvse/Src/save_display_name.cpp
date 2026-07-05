#include "save_display_name.h"

#include "encoding.h"
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
	void SaveDisplayName_SaveCallback(void* reserved);
	void SaveDisplayName_LoadCallback(void* reserved);

	namespace
	{
		constexpr UInt32 kRecordType = 'SVDN';
		constexpr UInt32 kRecordVersion = 1;
		constexpr UInt32 kNvseSignature = MACRO_SWAP32('NVSE');

		struct NvseHeader
		{
			UInt32 signature;
			UInt32 formatVersion;
			UInt16 nvseVersion;
			UInt16 nvseMinorVersion;
			UInt32 falloutVersion;
			UInt32 numPlugins;
		};

		struct PluginHeader
		{
			UInt32 opcodeBase;
			UInt32 numChunks;
			UInt32 length;
		};

		struct ChunkHeader
		{
			UInt32 type;
			UInt32 version;
			UInt32 length;
		};

		struct SaveDisplayNameRecordHeader
		{
			UInt32 actualKeyLen;
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

		NVSESerializationInterface* s_serialization = nullptr;
		std::unordered_map<std::string, DisplayRecord> s_pendingByActualKey;
		std::unordered_map<std::string, CacheEntry> s_displayCache;
		std::string s_lastSavePath;

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

		std::string ConvertSaveFileNameToCosave(std::string name)
		{
			if (name.empty() || name.size() >= MAX_PATH * 2)
				return {};

			std::string bakSuffix;
			const size_t firstDotBak = name.find(".bak");
			if (firstDotBak != std::string::npos)
			{
				bakSuffix = name.substr(firstDotBak);
				name = name.substr(0, firstDotBak);
			}

			std::string result;
			const size_t lastPeriod = name.rfind('.');
			if (lastPeriod == std::string::npos)
				result = name;
			else
				result = name.substr(0, lastPeriod);

			result += ".nvse";
			result += bakSuffix;

			if (result.find_last_of("\\/") == std::string::npos)
			{
				std::string savePath = GetSaveGamePath();
				if (savePath.empty() || savePath.size() >= MAX_PATH)
					return {};

				if (!savePath.empty() && savePath.back() != '\\' && savePath.back() != '/')
					savePath.push_back('\\');
				result = savePath + result;
			}

			return result;
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
			if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024)
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

		bool TryExtractManualSaveLocation(
			const std::string& displayNameMb,
			std::string& outLocation)
		{
			outLocation.clear();

			const size_t lastComma = displayNameMb.rfind(',');
			if (lastComma == std::string::npos)
				return false;

			const size_t previousComma = displayNameMb.rfind(',', lastComma - 1);
			if (previousComma == std::string::npos)
				return false;

			const size_t headerSep = displayNameMb.find(" - ");
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

		void __fastcall SavePathProcess(void*, UInt32, char* fileName)
		{
			if (!fileName)
				return;

			const std::string originalName = fileName;
			const size_t length = CdeclCall<UInt32>(0xEC6130, fileName);
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

			CaptureSaveDisplayName(originalName.c_str(), fileName);
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

		bool ParseDisplayRecord(const std::vector<UInt8>& data, size_t chunkStart, UInt32 chunkLength, DisplayRecord& outRecord)
		{
			size_t offset = chunkStart;
			const size_t chunkEnd = chunkStart + chunkLength;
			if (chunkEnd > data.size())
				return false;

			SaveDisplayNameRecordHeader header = {};
			if (!ReadValue(data, offset, header))
				return false;
			if (header.actualKeyLen > 260 || header.displayNameLen > 1024)
				return false;
			if (offset + header.actualKeyLen + header.displayNameLen > chunkEnd)
				return false;

			DisplayRecord record;
			record.uiEncoding = header.uiEncoding;
			record.codePage = header.codePage;
			if (!ReadBytes(data, offset, record.actualKey, header.actualKeyLen))
				return false;
			if (!ReadBytes(data, offset, record.displayNameMb, header.displayNameLen))
				return false;
			if (record.actualKey.empty() || record.displayNameMb.empty())
				return false;

			outRecord = record;
			return true;
		}

		bool ReadDisplayRecordFromCosave(const std::string& cosavePath, const std::string& expectedActualKey, DisplayRecord& outRecord)
		{
			std::vector<UInt8> data;
			if (!ReadWholeFile(cosavePath, data))
				return false;

			size_t offset = 0;
			NvseHeader header = {};
			if (!ReadValue(data, offset, header))
				return false;
			if (header.signature != kNvseSignature || header.formatVersion == 0)
				return false;

			for (UInt32 pluginIndex = 0; pluginIndex < header.numPlugins && offset + sizeof(PluginHeader) <= data.size(); ++pluginIndex)
			{
				PluginHeader plugin = {};
				if (!ReadValue(data, offset, plugin) || !plugin.length)
					return false;

				const size_t pluginEnd = offset + plugin.length;
				if (pluginEnd > data.size())
					return false;

				if (plugin.opcodeBase != kTNVSEOpcodeBase)
				{
					offset = pluginEnd;
					continue;
				}

				for (UInt32 chunkIndex = 0; chunkIndex < plugin.numChunks && offset + sizeof(ChunkHeader) <= pluginEnd; ++chunkIndex)
				{
					ChunkHeader chunk = {};
					if (!ReadValue(data, offset, chunk))
						return false;
					if (offset + chunk.length > pluginEnd)
						return false;

					if (chunk.type == kRecordType && chunk.version == kRecordVersion)
					{
						DisplayRecord record;
						if (ParseDisplayRecord(data, offset, chunk.length, record)
							&& record.actualKey == expectedActualKey)
						{
							outRecord = record;
							return true;
						}
					}

					offset += chunk.length;
				}

				offset = pluginEnd;
			}

			return false;
		}

		std::string PrepareDisplayNameMb(const std::string& originalName)
		{
			if (g_bEnableUTF8 && g_usingWinEncoding && IsValidUTF8With3ByteMin(originalName.c_str()))
				return UTF8ToMultiByteStr(originalName, g_usingWinEncoding);
			return originalName;
		}

		bool ResolveDisplayNameForSavePath(
			const std::string& savePath,
			const std::string& actualKey,
			std::string& outDisplayNameMb)
		{
			if (savePath.empty() || actualKey.empty() || actualKey.size() >= MAX_PATH)
				return false;

			const std::string cosavePath = ConvertSaveFileNameToCosave(savePath);
			if (cosavePath.empty())
				return false;

			DisplayRecord record;
			if (!ReadDisplayRecordFromCosave(cosavePath, actualKey, record))
				return false;

			if (record.codePage == g_usingWinEncoding || !record.codePage || !g_usingWinEncoding)
			{
				outDisplayNameMb = record.displayNameMb;
				return true;
			}

			return ConvertCodePage(record.displayNameMb, record.codePage, g_usingWinEncoding, outDisplayNameMb);
		}

		bool LoadCachedDisplayName(const std::string& actualKey, std::string& outDisplayNameMb)
		{
			if (actualKey.empty() || actualKey.size() >= MAX_PATH)
				return false;

			const std::string savePath = BuildSavePathFromActualKey(actualKey);
			if (savePath.empty())
				return false;

			const std::string cosavePath = ConvertSaveFileNameToCosave(savePath);
			if (cosavePath.empty())
				return false;

			const FileStamp stamp = GetFileStamp(cosavePath);

			auto cached = s_displayCache.find(actualKey);
			if (cached != s_displayCache.end()
				&& (stamp.exists || cached->second.hasRecord)
				&& SameStamp(cached->second.stamp, stamp))
			{
				if (!cached->second.hasRecord)
					return false;
				outDisplayNameMb = cached->second.displayNameMb;
				return true;
			}

			CacheEntry entry;
			entry.stamp = stamp;
			entry.hasRecord = ResolveDisplayNameForSavePath(savePath, actualKey, entry.displayNameMb);
			s_displayCache[actualKey] = entry;

			if (!entry.hasRecord)
				return false;
			outDisplayNameMb = entry.displayNameMb;
			return true;
		}

		bool __stdcall SaveDisplayNameIsManualSave(const char* actualName)
		{
			if (StdCall<bool>(0x851980, actualName))
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

		void WriteRecord(const DisplayRecord& record)
		{
			if (!s_serialization || record.actualKey.empty() || record.displayNameMb.empty())
				return;

			SaveDisplayNameRecordHeader header = {};
			header.actualKeyLen = static_cast<UInt32>(record.actualKey.size());
			header.displayNameLen = static_cast<UInt32>(record.displayNameMb.size());
			header.uiEncoding = record.uiEncoding;
			header.codePage = record.codePage;

			if (!s_serialization->OpenRecord(kRecordType, kRecordVersion))
				return;

			s_serialization->WriteRecordData(&header, sizeof(header));
			s_serialization->WriteRecordData(record.actualKey.data(), header.actualKeyLen);
			s_serialization->WriteRecordData(record.displayNameMb.data(), header.displayNameLen);
		}
	}

	void InitSaveDisplayName(NVSESerializationInterface* serialization)
	{
		s_serialization = serialization;
		if (!s_serialization)
			return;

		s_serialization->SetLoadCallback(g_pluginHandle, SaveDisplayName_LoadCallback);
		s_serialization->SetPreLoadCallback(g_pluginHandle, SaveDisplayName_LoadCallback);

		if (g_bSaveDisplayNameMap)
			s_serialization->SetSaveCallback(g_pluginHandle, SaveDisplayName_SaveCallback);
	}

	void HandleSaveDisplayNameMessage(NVSEMessagingInterface::Message* message)
	{
		if (!g_bSaveDisplayNameMap || !message)
			return;

		if (message->type == NVSEMessagingInterface::kMessage_SaveGame && message->data)
		{
			s_lastSavePath.assign(static_cast<const char*>(message->data), message->dataLen);
			const std::string actualKey = ExtractActualKey(s_lastSavePath.c_str());
			if (!actualKey.empty())
				s_displayCache.erase(actualKey);
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
		s_displayCache.erase(actualKey);
	}

	void SaveDisplayName_SaveCallback(void*)
	{
		if (!g_bSaveDisplayNameMap || !s_serialization || s_lastSavePath.empty())
			return;

		const std::string actualKey = ExtractActualKey(s_lastSavePath.c_str());
		if (actualKey.empty())
			return;

		auto pending = s_pendingByActualKey.find(actualKey);
		if (pending != s_pendingByActualKey.end())
		{
			std::string existingDisplayName;
			if (pending->second.displayNameMb == actualKey
				&& ResolveDisplayNameForSavePath(s_lastSavePath, actualKey, existingDisplayName))
			{
				DisplayRecord record;
				record.actualKey = actualKey;
				record.displayNameMb = existingDisplayName;
				record.uiEncoding = g_uiEncoding;
				record.codePage = g_usingWinEncoding;
				WriteRecord(record);
			}
			else
			{
				WriteRecord(pending->second);
			}

			s_pendingByActualKey.erase(pending);
			s_displayCache.erase(actualKey);
			return;
		}

		std::string existingDisplayName;
		if (ResolveDisplayNameForSavePath(s_lastSavePath, actualKey, existingDisplayName))
		{
			DisplayRecord record;
			record.actualKey = actualKey;
			record.displayNameMb = existingDisplayName;
			record.uiEncoding = g_uiEncoding;
			record.codePage = g_usingWinEncoding;
			WriteRecord(record);
			s_displayCache.erase(actualKey);
		}
	}

	void SaveDisplayName_LoadCallback(void*)
	{
		if (!s_serialization)
			return;

		UInt32 type = 0;
		UInt32 version = 0;
		UInt32 length = 0;
		while (s_serialization->GetNextRecordInfo(&type, &version, &length))
		{
			if (length)
				s_serialization->SkipNBytes(length);
		}
	}

	void InitSaveDisplayNameHook()
	{
		if (!g_bSaveDisplayNameMap)
			return;

		WriteRelCall(0x8518BB, &SavePathProcess);
		WriteRelCall(0x851AAE, &SaveDisplayNameIsManualSave);
	}
}

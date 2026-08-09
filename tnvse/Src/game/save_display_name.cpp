#include "save_display_name.h"

#include "encoding.h"
#include "hook_identity.h"
#include "hook_site.h"
#include "load_config.h"
#include "plugin_dependencies.h"
#include "SafeWrite.h"
#include "tnvse.h"

#include <algorithm>
#include <array>
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

		constexpr SIZE_T kScrubFileNameCallSite = 0x8518BB;
		constexpr SIZE_T kVanillaScrubFileName = 0x8518D0;
		constexpr SIZE_T kIsSaveFileNameGeneratedCallSite = 0x851AAE;
		constexpr SIZE_T kVanillaIsSaveFileNameGenerated = 0x851980;
		// BGSSaveLoadManager::SaveGame has already copied the supplied name into
		// the save header before this call, but OpenSaveFile (0x850030) has not
		// yet built the physical .fos path. Replacing only this call preserves
		// display text.
		constexpr SIZE_T kOpenSaveFileCallSite = 0x850545;
		constexpr SIZE_T kVanillaOpenSaveFile = 0x850030;
		constexpr const char* kStewieTweaksModuleName =
			"nvse_stewie_tweaks.dll";
		constexpr UInt32 kStewieSaveHookVersion990 = 990;
		constexpr UInt32 kStewieSaveHookVersion995 = 995;
		// Release 9.90 and the audited 9.95 source both forward the same four
		// stack arguments to vanilla 0x850030. The 9.90 binary emits this exact
		// prefix; a different code generator/version fails closed at PostLoad.
		constexpr std::array<UInt8, 24>
			kStewieOpenSaveFileForwardPrefix = {
				0x56,
				0xFF, 0x74, 0x24, 0x14,
				0xB8, 0x30, 0x00, 0x85, 0x00,
				0xFF, 0x74, 0x24, 0x14,
				0xFF, 0x74, 0x24, 0x14,
				0xFF, 0x74, 0x24, 0x14,
				0xFF, 0xD0,
			};
		constexpr SIZE_T kSaveLoadManagerSingleton = 0x11DE134;

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
		SIZE_T s_nextOpenSaveFile = 0;
		SIZE_T s_nextIsSaveFileNameGenerated = 0;

		struct OpenSaveFileForwardFrame
		{
			OpenSaveFileForwardFrame* previous = nullptr;
			SIZE_T target = 0;
			void* saveManager = nullptr;
			UInt32 createFile = 0;
			UInt32 bufferMode = 0;
			SInt32 saveIndex = 0;
		};

		thread_local OpenSaveFileForwardFrame*
			s_activeOpenSaveFileForward = nullptr;
		volatile LONG s_loggedOpenSaveFileForwardCycle = 0;

		class ScopedOpenSaveFileForward
		{
		public:
			ScopedOpenSaveFileForward(SIZE_T target, void* saveManager,
				UInt32 createFile, UInt32 bufferMode, SInt32 saveIndex)
				: m_frame{ s_activeOpenSaveFileForward, target, saveManager,
					createFile, bufferMode, saveIndex }
			{
				s_activeOpenSaveFileForward = &m_frame;
			}

			~ScopedOpenSaveFileForward()
			{
				s_activeOpenSaveFileForward = m_frame.previous;
			}

			ScopedOpenSaveFileForward(const ScopedOpenSaveFileForward&) = delete;
			ScopedOpenSaveFileForward& operator=(
				const ScopedOpenSaveFileForward&) = delete;

		private:
			OpenSaveFileForwardFrame m_frame;
		};

		enum class OpenSaveFileHookPhase
		{
			PluginLoad,
			PostLoad,
		};

		struct StewieOpenSaveFileContract
		{
			bool moduleOwned = false;
			bool pluginInfoValid = false;
			bool versionSupported = false;
			bool forwardingPrefixMatches = false;
			UInt32 version = 0;
		};

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

		void* GetSaveLoadManager()
		{
			if (!hook_identity::IsAccessibleRegion(
					kSaveLoadManagerSingleton, sizeof(void*), false))
			{
				return nullptr;
			}
			return *reinterpret_cast<void**>(kSaveLoadManagerSingleton);
		}

		std::string GetSaveGamePath()
		{
			void* saveManager = GetSaveLoadManager();
			if (!saveManager)
				return {};

			char savePath[MAX_PATH] = {};
			ThisStdCall<BOOL>(0x84FF30, saveManager, savePath);
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
			void* saveManager = GetSaveLoadManager();
			if (saveManager)
			{
				// Retail PC adds the temporary-file flag as a third stack
				// argument, but still retains the BGSSaveLoadManager thiscall
				// contract in ECX.
				ThisStdCall<void>(0x84FF90, saveManager,
					actualKey.c_str(), savePath, static_cast<UInt8>(0));
			}
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

		void* ForwardOpenSaveFile(SIZE_T target, void* saveManager,
			const char* fileName, UInt32 createFile, UInt32 bufferMode,
			SInt32 saveIndex)
		{
			for (const OpenSaveFileForwardFrame* frame =
					s_activeOpenSaveFileForward;
				frame; frame = frame->previous)
			{
				if (frame->target != target
					|| frame->saveManager != saveManager
					|| frame->createFile != createFile
					|| frame->bufferMode != bufferMode
					|| frame->saveIndex != saveIndex)
				{
					continue;
				}

				if (target != kVanillaOpenSaveFile
					&& hook_identity::IsExecutableTarget(
						kVanillaOpenSaveFile))
				{
					if (InterlockedCompareExchange(
							&s_loggedOpenSaveFileForwardCycle, 1, 0) == 0)
					{
						gLog.FormattedMessage(
							"tnvse_save_display_name: recursive predecessor edge detected target=%08X manager=%08X saveIndex=%d; forwarding directly to vanilla=%08X",
							static_cast<UInt32>(target),
							reinterpret_cast<UInt32>(saveManager),
							saveIndex,
							static_cast<UInt32>(kVanillaOpenSaveFile));
					}
					return ThisStdCall<void*>(kVanillaOpenSaveFile,
						saveManager, fileName, createFile, bufferMode, saveIndex);
				}
				break;
			}

			ScopedOpenSaveFileForward activeForward(target, saveManager,
				createFile, bufferMode, saveIndex);
			return ThisStdCall<void*>(target, saveManager,
				fileName, createFile, bufferMode, saveIndex);
		}

		void __fastcall ScrubFileNameAndCaptureDisplayName(
			void*, UInt32, char* fileName)
		{
			if (!fileName)
				return;

			std::string originalName;
			if (!TryCopyCString(fileName, MAX_PATH, originalName))
				return;

			SanitizeSaveFileName(fileName, originalName.size());
			CaptureSaveDisplayName(originalName.c_str(), fileName);
		}

		void* __fastcall OpenSaveFileWithSafeName(
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
			const SIZE_T nextTarget = s_nextOpenSaveFile;
			if (!hook_identity::IsExecutableTarget(nextTarget)
				|| nextTarget == reinterpret_cast<SIZE_T>(
					&OpenSaveFileWithSafeName))
			{
				return nullptr;
			}

			std::string originalName;
			if (!TryCopyCString(fileName, MAX_PATH, originalName)
				|| originalName.empty()
				|| !ContainsHighByte(originalName))
			{
				return ForwardOpenSaveFile(nextTarget, saveManager,
					fileName, createFile, bufferMode, saveIndex);
			}

			char safeName[MAX_PATH] = {};
			std::memcpy(safeName, originalName.data(), originalName.size());
			SanitizeSaveFileName(safeName, originalName.size());
			CaptureSaveDisplayName(originalName.c_str(), safeName);
			gLog.FormattedMessage(
				"tnvse_save_display_name: sanitized custom multibyte save name bytes=%u",
				static_cast<UInt32>(originalName.size()));

			return ForwardOpenSaveFile(nextTarget, saveManager,
				safeName, createFile, bufferMode, saveIndex);
		}

		bool IsAddressOwnedByModule(SIZE_T address, HMODULE module)
		{
			MEMORY_BASIC_INFORMATION region = {};
			return address && module
				&& VirtualQuery(reinterpret_cast<const void*>(address),
					&region, sizeof(region)) == sizeof(region)
				&& region.AllocationBase == module;
		}

		StewieOpenSaveFileContract InspectStewieOpenSaveFileContract(
			SIZE_T target)
		{
			StewieOpenSaveFileContract contract;
			contract.moduleOwned = IsAddressOwnedByModule(target,
				GetModuleHandleA(kStewieTweaksModuleName));

			const PluginInfo* const info = g_cmdTableInterface
				&& g_cmdTableInterface->GetPluginInfoByName
				? g_cmdTableInterface->GetPluginInfoByName(
					dependencies::kStewieTweaksPluginName)
				: nullptr;
			contract.pluginInfoValid = dependencies::IsPluginInfoValid(info);
			contract.version = contract.pluginInfoValid ? info->version : 0;
			contract.versionSupported = contract.pluginInfoValid
				&& (contract.version == kStewieSaveHookVersion990
					|| contract.version == kStewieSaveHookVersion995);
			contract.forwardingPrefixMatches = contract.moduleOwned
				&& hook_identity::IsAccessibleRegion(target,
					kStewieOpenSaveFileForwardPrefix.size(), true)
				&& std::memcmp(reinterpret_cast<const void*>(target),
					kStewieOpenSaveFileForwardPrefix.data(),
					kStewieOpenSaveFileForwardPrefix.size()) == 0;
			return contract;
		}

		bool IsKnownPostLoadOpenSaveFilePredecessor(SIZE_T target,
			StewieOpenSaveFileContract& stewieContract)
		{
			// Vanilla has no predecessor state and therefore cannot recurse
			// through tNVSE. Pointer equality with a previously captured wrapper is
			// not sufficient: that wrapper may have been reinstalled later and may
			// now retain tNVSE as its own predecessor.
			if (target == kVanillaOpenSaveFile)
			{
				return true;
			}

			// Stewie is accepted only when both its reported version and the live
			// machine-code forwarding contract are recognized. Module ownership or
			// equality with a saved address alone is insufficient: a wrapper which
			// retained tNVSE would recurse if placed below it again.
			stewieContract = InspectStewieOpenSaveFileContract(target);
			return stewieContract.moduleOwned
				&& stewieContract.versionSupported
				&& stewieContract.forwardingPrefixMatches;
		}

		const char* OpenSaveFileHookPhaseName(OpenSaveFileHookPhase phase)
		{
			return phase == OpenSaveFileHookPhase::PostLoad
				? "post-load" : "plugin-load";
		}

		bool ReconcileOpenSaveFileHook(OpenSaveFileHookPhase phase)
		{
			hook_site::RelCallSite site{
				"BGSSaveLoadManager::SaveGame -> OpenSaveFile "
				"(__thiscall via __fastcall shim)",
				kOpenSaveFileCallSite,
				kVanillaOpenSaveFile,
				&OpenSaveFileWithSafeName
			};
			const char* const phaseName = OpenSaveFileHookPhaseName(phase);

			SIZE_T currentTarget = 0;
			if (!site.ReadTarget(currentTarget)
				|| !hook_identity::IsExecutableTarget(currentTarget))
			{
				gLog.FormattedMessage(
					"tnvse_save_display_name: %s save sanitizer identity invalid site=%08X instruction=CALL rel32 current=%08X abi=__thiscall-via-__fastcall; fail-closed",
					phaseName,
					static_cast<UInt32>(site.callAddress),
					static_cast<UInt32>(currentTarget));
				return false;
			}

			if (currentTarget == site.replacementTarget)
			{
				const bool predecessorValid =
					s_nextOpenSaveFile != site.replacementTarget
					&& hook_identity::IsExecutableTarget(
						s_nextOpenSaveFile);
				gLog.FormattedMessage(
					"tnvse_save_display_name: %s save sanitizer %s site=%08X instruction=CALL rel32 current=%08X next=%08X abi=__thiscall-via-__fastcall",
					phaseName,
					predecessorValid ? "verified" : "predecessor invalid; fail-closed",
					static_cast<UInt32>(site.callAddress),
					static_cast<UInt32>(currentTarget),
					static_cast<UInt32>(s_nextOpenSaveFile));
				return predecessorValid;
			}

			StewieOpenSaveFileContract stewieContract;
			if (phase == OpenSaveFileHookPhase::PostLoad
				&& !IsKnownPostLoadOpenSaveFilePredecessor(
					currentTarget, stewieContract))
			{
				// An unknown owner may already retain tNVSE as its predecessor.
				// Re-publishing above it could create tNVSE -> owner -> tNVSE.
				gLog.FormattedMessage(
					"tnvse_save_display_name: post-load save sanitizer left unknown owner unchanged site=%08X instruction=CALL rel32 current=%08X savedNext=%08X abi=__thiscall-via-__fastcall stewieOwned=%u stewieInfo=%u stewieVersion=%u stewieVersionSupported=%u vanillaForwardPrefix=%u; fail-closed",
					static_cast<UInt32>(site.callAddress),
					static_cast<UInt32>(currentTarget),
					static_cast<UInt32>(s_nextOpenSaveFile),
					stewieContract.moduleOwned ? 1u : 0u,
					stewieContract.pluginInfoValid ? 1u : 0u,
					stewieContract.version,
					stewieContract.versionSupported ? 1u : 0u,
					stewieContract.forwardingPrefixMatches ? 1u : 0u);
				return false;
			}

			const SIZE_T previousPredecessor = s_nextOpenSaveFile;
			s_nextOpenSaveFile = currentTarget;
			const bool writeCompleted = WriteRelCall(
				site.callAddress, &OpenSaveFileWithSafeName);

			SIZE_T observedTarget = 0;
			const bool observedCall = site.ReadTarget(observedTarget);
			if (observedCall && observedTarget == site.replacementTarget)
			{
				gLog.FormattedMessage(
					"tnvse_save_display_name: %s custom multibyte save sanitizer installed site=%08X instruction=CALL rel32 target=%08X next=%08X abi=__thiscall-via-__fastcall writeComplete=%u",
					phaseName,
					static_cast<UInt32>(site.callAddress),
					static_cast<UInt32>(site.replacementTarget),
					static_cast<UInt32>(currentTarget),
					writeCompleted ? 1u : 0u);
				return true;
			}

			const bool predecessorRetained = !observedCall
				|| observedTarget != currentTarget;
			if (!predecessorRetained)
			{
				// The wrapper is not reachable, so its predecessor must not be
				// changed as though publication had succeeded.
				s_nextOpenSaveFile = previousPredecessor;
			}
			gLog.FormattedMessage(
				"tnvse_save_display_name: %s save sanitizer publication unverified site=%08X instruction=CALL rel32 predecessor=%08X observed=%08X readable=%u executable=%u writeComplete=%u predecessorRetained=%u; fail-closed",
				phaseName,
				static_cast<UInt32>(site.callAddress),
				static_cast<UInt32>(currentTarget),
				static_cast<UInt32>(observedTarget),
				observedCall ? 1u : 0u,
				observedCall && hook_identity::IsExecutableTarget(
					observedTarget) ? 1u : 0u,
				writeCompleted ? 1u : 0u,
				predecessorRetained ? 1u : 0u);
			return false;
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
			const BOOL writeOk = WriteFile(
				file,
				data.data(),
				static_cast<DWORD>(data.size()),
				&bytesWritten,
				nullptr);
			const BOOL flushOk = writeOk && bytesWritten == data.size()
				? FlushFileBuffers(file)
				: FALSE;
			CloseHandle(file);

			if (!writeOk || bytesWritten != data.size() || !flushOk)
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

		bool __fastcall IsSaveFileNameGeneratedWithDisplayName(
			void* saveManager, void*, const char* actualName)
		{
			const SIZE_T nextTarget = s_nextIsSaveFileNameGenerated;
			if (hook_identity::IsExecutableTarget(nextTarget)
				&& nextTarget != reinterpret_cast<SIZE_T>(
					&IsSaveFileNameGeneratedWithDisplayName)
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

	void ReconcileSaveDisplayNameHookPostLoad()
	{
		ReconcileOpenSaveFileHook(OpenSaveFileHookPhase::PostLoad);
	}

	void InitSaveDisplayNameHook()
	{
		hook_site::RelCallSite scrubFileNameCallSite{
			"SaveGameManager filename scrub (__fastcall)",
			kScrubFileNameCallSite,
			kVanillaScrubFileName,
			&ScrubFileNameAndCaptureDisplayName
		};
		hook_site::RelCallSite generatedFileNameCallSite{
			"SaveGameManager generated-name test (__fastcall)",
			kIsSaveFileNameGeneratedCallSite,
			0,
			&IsSaveFileNameGeneratedWithDisplayName
		};
		ReconcileOpenSaveFileHook(OpenSaveFileHookPhase::PluginLoad);

		if (!g_bSaveDisplayNameMap)
			return;

		SIZE_T scrubFileNameTarget = 0;
		SIZE_T generatedFileNameTarget = 0;
		if (!hook_identity::ReadRel32Target(
				kScrubFileNameCallSite, Rel32Opcode::Call, scrubFileNameTarget)
			|| scrubFileNameTarget != kVanillaScrubFileName
			|| !hook_identity::ReadRel32Target(
				kIsSaveFileNameGeneratedCallSite,
				Rel32Opcode::Call,
				generatedFileNameTarget)
			|| !hook_identity::IsExecutableTarget(generatedFileNameTarget)
			|| generatedFileNameTarget
				== generatedFileNameCallSite.replacementTarget)
		{
			gLog.FormattedMessage(
				"tnvse_save_display_name: hook identity mismatch scrub=%08X generated=%08X; disabled",
				static_cast<UInt32>(scrubFileNameTarget),
				static_cast<UInt32>(generatedFileNameTarget));
			return;
		}

		s_nextIsSaveFileNameGenerated = generatedFileNameTarget;
		// SaveGameManager filename scrub (__fastcall).
		WriteRelCall(kScrubFileNameCallSite,
			&ScrubFileNameAndCaptureDisplayName);
		// SaveGameManager generated-name test (__fastcall), chained to the
		// currently installed predecessor when one is present.
		WriteRelCall(kIsSaveFileNameGeneratedCallSite,
			&IsSaveFileNameGeneratedWithDisplayName);

		const bool scrubFileNameInstalled = scrubFileNameCallSite.IsInstalled();
		const bool generatedFileNameInstalled =
			generatedFileNameCallSite.IsInstalled();
		if (!scrubFileNameInstalled || !generatedFileNameInstalled)
		{
			SIZE_T observedScrubTarget = 0;
			SIZE_T observedGeneratedTarget = 0;
			const bool scrubRestored = scrubFileNameCallSite.RollbackOwned(
				scrubFileNameTarget, &observedScrubTarget);
			const bool generatedRestored = generatedFileNameCallSite.RollbackOwned(
				generatedFileNameTarget, &observedGeneratedTarget);
			if (generatedRestored)
				s_nextIsSaveFileNameGenerated = 0;
			else
			{
				gLog.FormattedMessage(
					"tnvse_save_display_name: generated-name rollback retained observed=%08X predecessor=%08X",
					static_cast<UInt32>(observedGeneratedTarget),
					static_cast<UInt32>(generatedFileNameTarget));
			}
			gLog.FormattedMessage(
				"tnvse_save_display_name: hook write verification failed scrubInstalled=%u generatedInstalled=%u scrubObserved=%08X generatedObserved=%08X rollbackScrub=%u rollbackGenerated=%u",
				scrubFileNameInstalled ? 1u : 0u,
				generatedFileNameInstalled ? 1u : 0u,
				static_cast<UInt32>(observedScrubTarget),
				static_cast<UInt32>(observedGeneratedTarget),
				scrubRestored ? 1u : 0u,
				generatedRestored ? 1u : 0u);
			return;
		}

		gLog.FormattedMessage(
			"tnvse_save_display_name: display-name hooks installed generatedNext=%08X generatedVanilla=%u",
			static_cast<UInt32>(generatedFileNameTarget),
			generatedFileNameTarget == kVanillaIsSaveFileNameGenerated ? 1u : 0u);
	}
}

#include "tianmiao_conflict.h"

#include "hook_identity.h"
#include "tnvse.h"

#include <Windows.h>

#include <array>
#include <cwchar>
#include <string>
#include <utility>

namespace fonthook::compatibility
{
	namespace implementation::tianmiao_conflict {}
	using namespace implementation::tianmiao_conflict;

	namespace implementation::tianmiao_conflict
	{
		// FalloutNV 1.4.0.525 image base is 0x00400000.  Tianmiao 1.11
		// replaces the instructions at these RVAs with rel32 jumps into its
		// root binkw32.dll proxy.  Use RVAs so the probe remains correct if the
		// executable image is relocated.
		struct TianmiaoPatchSite
		{
			SIZE_T rva = 0;
			bool coreFontPatch = false;
		};

		constexpr std::array<TianmiaoPatchSite, 5> kTianmiaoPatchSites = {{
			{ 0x612DF2u, true },  // Font::CreateText glyph lookup
			{ 0x613853u, true },  // Font::PrepText glyph lookup
			{ 0x61B110u, true },  // CalculateStringDimensions glyph lookup
			{ 0x613CAAu, false }, // Font::PrepText DBCS wrap boundary
			{ 0x37AEDEu, false }, // HUDMainMenu::UpdateQuestText DBCS unit
		}};

		struct ModuleEvidence
		{
			HMODULE module = nullptr;
			UInt32 coreMatches = 0;
			UInt32 auxiliaryMatches = 0;
		};

		struct TianmiaoDetection
		{
			HMODULE proxyModule = nullptr;
			UInt32 apiMatches = 0;
			UInt32 coreMatches = 0;
			UInt32 auxiliaryMatches = 0;
			std::wstring proxyPath;
			std::wstring backupPath;
			bool backupExists = false;
		};

		bool TryGetModuleImageSize(HMODULE module, SIZE_T& imageSize)
		{
			imageSize = 0;
			if (!module)
				return false;

			const auto* base = reinterpret_cast<const UInt8*>(module);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
				return false;

			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
				base + static_cast<SIZE_T>(dos->e_lfanew));
			if (nt->Signature != IMAGE_NT_SIGNATURE)
				return false;

			imageSize = nt->OptionalHeader.SizeOfImage;
			return imageSize != 0;
		}

		std::wstring GetLoadedModulePath(HMODULE module)
		{
			if (!module)
				return {};

			std::wstring path(512, L'\0');
			for (;;)
			{
				SetLastError(ERROR_SUCCESS);
				const DWORD length = GetModuleFileNameW(
					module, path.data(), static_cast<DWORD>(path.size()));
				if (!length)
					return {};
				if (length < path.size() - 1u)
				{
					path.resize(length);
					return path;
				}
				if (path.size() >= 32768u)
					return {};
				path.resize(path.size() * 2u);
			}
		}

		const wchar_t* FileNamePart(const std::wstring& path)
		{
			const wchar_t* fileName = path.c_str();
			for (const wchar_t* cursor = fileName; *cursor; ++cursor)
			{
				if (*cursor == L'\\' || *cursor == L'/')
					fileName = cursor + 1;
			}
			return fileName;
		}

		bool IsBinkProxyModulePath(const std::wstring& path)
		{
			if (path.empty()
				|| _wcsicmp(FileNamePart(path), L"binkw32.dll") != 0)
			{
				return false;
			}

			// Tianmiao's proxy is the root binkw32.dll.  The module below usub is
			// the original Bink library loaded by the proxy, not the injector.
			constexpr wchar_t kOriginalLibrarySuffix[] = L"\\usub\\binkw32.dll";
			const SIZE_T suffixLength = std::size(kOriginalLibrarySuffix) - 1u;
			return path.size() < suffixLength
				|| _wcsicmp(path.c_str() + path.size() - suffixLength,
					kOriginalLibrarySuffix) != 0;
		}

		std::wstring BuildTianmiaoBackupPath(const std::wstring& proxyPath)
		{
			const SIZE_T separator = proxyPath.find_last_of(L"\\/");
			if (separator == std::wstring::npos)
				return L"usub\\binkw32.dll";
			return proxyPath.substr(0, separator + 1u)
				+ L"usub\\binkw32.dll";
		}

		bool IsOrdinaryFile(const std::wstring& path)
		{
			if (path.empty())
				return false;
			const DWORD attributes = GetFileAttributesW(path.c_str());
			return attributes != INVALID_FILE_ATTRIBUTES
				&& (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
		}

		HMODULE ModuleContainingExecutableAddress(SIZE_T address)
		{
			if (!hook_identity::IsExecutableTarget(address))
				return nullptr;

			HMODULE module = nullptr;
			if (!GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
					| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(address), &module))
			{
				return nullptr;
			}
			return module;
		}

		bool PopulateDetection(
			TianmiaoDetection& detection, HMODULE proxyModule,
			UInt32 apiMatches, UInt32 coreMatches,
			UInt32 auxiliaryMatches)
		{
			std::wstring path = GetLoadedModulePath(proxyModule);
			if (!IsBinkProxyModulePath(path))
				return false;

			detection.proxyModule = proxyModule;
			detection.apiMatches = apiMatches;
			detection.coreMatches = coreMatches;
			detection.auxiliaryMatches = auxiliaryMatches;
			detection.proxyPath = std::move(path);
			detection.backupPath = BuildTianmiaoBackupPath(
				detection.proxyPath);
			detection.backupExists = IsOrdinaryFile(detection.backupPath);
			return true;
		}

		bool DetectTianmiaoProfileApiHook(TianmiaoDetection& detection)
		{
			// Tianmiao's root bink proxy installs this detour from DLL_PROCESS_ATTACH,
			// before its later Direct3D callback scans and patches FalloutNV.exe.
			// Inspect the live API entry point and the owner of the jump destination;
			// do not identify the DLL by file bytes, size, timestamp or hash.
			HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
			const FARPROC profileString = kernel32
				? GetProcAddress(kernel32, "GetPrivateProfileStringA")
				: nullptr;
			SIZE_T target = 0;
			if (!profileString
				|| !hook_identity::ReadRel32Target(
					reinterpret_cast<SIZE_T>(profileString),
					hook_identity::Rel32Opcode::Jump, target))
			{
				return false;
			}

			HMODULE owner = ModuleContainingExecutableAddress(target);
			return owner && PopulateDetection(detection, owner, 1, 0, 0);
		}

		TianmiaoDetection DetectTianmiaoFontPatch()
		{
			TianmiaoDetection detection;
			if (DetectTianmiaoProfileApiHook(detection))
				return detection;

			HMODULE gameModule = GetModuleHandleW(nullptr);
			SIZE_T gameImageSize = 0;
			if (!TryGetModuleImageSize(gameModule, gameImageSize))
				return detection;

			std::array<ModuleEvidence, kTianmiaoPatchSites.size()> evidence = {};
			SIZE_T evidenceCount = 0;
			const SIZE_T gameBase = reinterpret_cast<SIZE_T>(gameModule);
			for (const TianmiaoPatchSite& site : kTianmiaoPatchSites)
			{
				if (site.rva >= gameImageSize
					|| 5u > gameImageSize - site.rva)
				{
					continue;
				}

				SIZE_T target = 0;
				if (!hook_identity::ReadRel32Target(
					gameBase + site.rva,
					hook_identity::Rel32Opcode::Jump, target))
				{
					continue;
				}

				HMODULE owner = ModuleContainingExecutableAddress(target);
				if (!owner || owner == gameModule)
					continue;

				SIZE_T index = 0;
				for (; index < evidenceCount; ++index)
				{
					if (evidence[index].module == owner)
						break;
				}
				if (index == evidenceCount)
				{
					if (evidenceCount >= evidence.size())
						continue;
					evidence[index].module = owner;
					++evidenceCount;
				}

				if (site.coreFontPatch)
					++evidence[index].coreMatches;
				else
					++evidence[index].auxiliaryMatches;
			}

			constexpr UInt32 kRequiredCoreFontPatches = 3;
			for (SIZE_T index = 0; index < evidenceCount; ++index)
			{
				if (evidence[index].coreMatches != kRequiredCoreFontPatches)
					continue;

				if (PopulateDetection(
					detection, evidence[index].module, 0,
					evidence[index].coreMatches,
					evidence[index].auxiliaryMatches))
				{
					return detection;
				}
			}

			return detection;
		}

		std::wstring BuildConflictMessage(const TianmiaoDetection& detection)
		{
			std::wstring message =
				L"tNVSE 检测到当前游戏正在使用天邈汉化核心文件 binkw32.dll\n\n"
				L"天邈汉化核心与 tNVSE 不兼容，且tNVSE不需要其他汉化核心即可实现中文支持\n"
				L"请将游戏根目录中的天邈版 binkw32.dll 恢复为原版\n"
				L"天邈汉化保存的原版 DLL 备份位置为：\n";
			message += detection.backupPath.empty()
				? L"<游戏目录>\\usub\\binkw32.dll"
				: detection.backupPath;
			if (!detection.backupExists)
			{
				message += L"\n（当前未在该位置找到备份）";
			}
			message +=
				L"\n\n操作方法：\n"
				L"1. 删除或移走游戏根目录中的天邈版 binkw32.dll\n"
				L"2. 将上述 usub 目录中的备份复制到游戏根目录，并确保其命名为 binkw32.dll\n"
				L"3. 如果备份不存在，请从纯净原版备份恢复";
			return message;
		}
	}

	bool BlockTianmiaoFontPatchIfPresent(const char* phase)
	{
		const TianmiaoDetection detection = DetectTianmiaoFontPatch();
		if (!detection.proxyModule)
			return false;

		IDebugLog::SetAutoFlush(true);
		gLog.FormattedMessage(
			"tnvse_tianmiao_conflict: detected=1 phase=%s proxy=%p apiPatches=%u corePatches=%u auxiliaryPatches=%u backupExists=%u",
			phase ? phase : "unknown", detection.proxyModule,
			detection.apiMatches, detection.coreMatches,
			detection.auxiliaryMatches, detection.backupExists ? 1u : 0u);

		const std::wstring message = BuildConflictMessage(detection);
		MessageBoxW(nullptr, message.c_str(),
			L"tNVSE - 检测到不兼容的天邈汉化核心",
			MB_OK | MB_ICONERROR | MB_TASKMODAL | MB_SETFOREGROUND | MB_TOPMOST);
		ExitProcess(ERROR_BAD_ENVIRONMENT);
	}
}

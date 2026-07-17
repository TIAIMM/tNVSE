#include "plugin_dependencies.h"
#include "encoding.h"
#include "game_hooks.h"

#include "load_config.h"
#include "tnvse.h"

#include <cstdio>
#include <string>
#include <vector>
#include <Windows.h>

namespace fonthook::dependencies
{
	namespace
	{
		std::string FormatVersion(UInt32 version)
		{
			char buffer[32] = { 0 };
			sprintf_s(buffer, "%u.%02u", version / 100, version % 100);
			return buffer;
		}

		void AddMissingIssue(std::vector<std::string>& issues,
			const char* setting, const char* plugin, const char* requirement)
		{
			issues.emplace_back(std::string("- ") + setting + ": " + plugin
				+ " is not installed or was not detected. Required: " + requirement + ".");
		}

		void AddVersionIssue(std::vector<std::string>& issues,
			const char* setting, const char* plugin, UInt32 installed,
			const char* requirement)
		{
			issues.emplace_back(std::string("- ") + setting + ": " + plugin
				+ " " + FormatVersion(installed) + " is incompatible. Required: "
				+ requirement + ".");
		}
	}

	void ShowExternalPluginDependencyWarnings()
	{
		static bool shown = false;
		if (shown)
			return;
		shown = true;

		if (!g_cmdTableInterface)
			return;

		std::vector<std::string> issues;
		if (g_bChangeJIPBigGunDesc)
		{
			const PluginInfo* info = g_cmdTableInterface->GetPluginInfoByName
				? g_cmdTableInterface->GetPluginInfoByName(kJipPluginName) : nullptr;
			if (!IsPluginInfoValid(info))
			{
				AddMissingIssue(issues, "bChangeJIPBigGunDesc", kJipPluginName,
					"version 57.30");
			}
			else if (info->version != kJipBigGunsVersion)
			{
				AddVersionIssue(issues, "bChangeJIPBigGunDesc", kJipPluginName,
					info->version, "version 57.30");
			}
		}

		if (g_bMultibyteInput && AreMultibyteFontHooksInstalled()
			&& IsEastAsianUiMode() && g_bMultibyteInputStewieTweaks)
		{
			const PluginInfo* info = g_cmdTableInterface->GetPluginInfoByName
				? g_cmdTableInterface->GetPluginInfoByName(kStewieTweaksPluginName) : nullptr;
			if (!IsPluginInfoValid(info))
			{
				AddMissingIssue(issues, "bMultibyteInputStewieTweaks",
					kStewieTweaksPluginName, "version 9.90 or newer");
			}
			else if (info->version < kStewieTweaksMinVersion)
			{
				AddVersionIssue(issues, "bMultibyteInputStewieTweaks",
					kStewieTweaksPluginName, info->version,
					"version 9.90 or newer");
			}
		}

		if (AreFreeTypeFontHooksInstalled() && g_bEnableFreeTypeA8Atlas)
		{
			const PluginInfo* info = g_cmdTableInterface->GetPluginInfoByDLLName
				? g_cmdTableInterface->GetPluginInfoByDLLName(kShaderLoaderDllName) : nullptr;
			if (!IsPluginInfoValid(info))
			{
				AddMissingIssue(issues, "bEnableFreeTypeA8Atlas",
					"Fallout Shader Loader", "version 1.40 or newer");
			}
			else if (info->version < kShaderLoaderMinVersion)
			{
				AddVersionIssue(issues, "bEnableFreeTypeA8Atlas",
					"Fallout Shader Loader", info->version,
					"version 1.40 or newer");
			}
		}

		if (issues.empty())
			return;

		std::string message =
			"Some enabled tNVSE features require NVSE plugins that are missing or incompatible:\n\n";
		for (const std::string& issue : issues)
		{
			message += issue;
			message += '\n';
			gLog.FormattedMessage("tnvse_dependency: %s", issue.c_str());
		}
		message += "\nThe affected features will remain disabled or use their built-in fallback. "
			"The game will continue to start.";

		MessageBoxA(nullptr, message.c_str(), "tNVSE dependency warning",
			MB_OK | MB_ICONWARNING | MB_TASKMODAL);
	}
}

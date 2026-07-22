#pragma once

#include "nvse/PluginAPI.h"

namespace fonthook::dependencies
{
	inline constexpr const char* kJipPluginName = "JIP LN NVSE";
	inline constexpr UInt32 kJipBigGunsVersion = 5730;
	inline constexpr UInt32 kJipKeyEventFilterVersion = 5730;
	inline constexpr const char* kStewieTweaksPluginName = "lStewieAl's Tweaks";
	inline constexpr UInt32 kStewieTweaksMinVersion = 990;
	inline constexpr const char* kShaderLoaderDllName = "Fallout Shader Loader.dll";
	inline constexpr UInt32 kShaderLoaderMinVersion = 140;

	inline bool IsPluginInfoValid(const PluginInfo* info)
	{
		return info && info->infoVersion == PluginInfo::kInfoVersion;
	}

	void ShowExternalPluginDependencyWarnings();
}

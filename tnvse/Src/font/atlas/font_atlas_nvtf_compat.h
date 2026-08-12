#pragma once

namespace fonthook::vectorfont::implementation::font_atlas_snapshot
{
	inline constexpr UInt32 kNvtfMode2LargeAtlasDimension = 8192u;
	inline constexpr UInt64 kNvtfMode2LargeAtlasGpuBytes =
		128ull * 1024ull * 1024ull;

	struct NvtfTextureLockCompatibilityState
	{
		bool active = false;
		bool exactMode2Pattern = false;
		UInt32 readableSites = 0;
		UInt32 noppedSites = 0;
	};

	// NVTF mode 2 removes both sides of three renderer/texture-manager
	// critical-section families.  Inspect the live instruction images instead
	// of trusting a DLL name, version string, or INI path.
	NvtfTextureLockCompatibilityState
		GetNvtfTextureLockCompatibilityState(
			bool refreshIfInactive = false);

	bool IsNvtfMode2LargeAtlasPublication(
		UInt32 width, UInt32 height, UInt64 candidateGpuBytes);
	void LogNvtfMode2DirectAtlasPublication(UInt32 width, UInt32 height,
		UInt64 candidateGpuBytes, bool deviceMultithreaded,
		UInt64 deviceEpoch);
}

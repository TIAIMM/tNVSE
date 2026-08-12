#include "font_atlas_nvtf_compat.h"

#include "hook_identity.h"

#include <array>
#include <atomic>
#include <limits>
#include <mutex>

namespace fonthook::vectorfont::implementation::font_atlas_snapshot
{
	namespace
	{
		// FalloutNV 1.4.0.525 image base is 0x00400000. NVTF mode 2 writes
		// seven NOPs at every site below. Use RVAs so the probe still describes
		// the live executable image if its module base is relocated.
		constexpr std::array<SIZE_T, 8> kNvtfMode2TextureLockSiteRvas = {{
			0xA6DC4Bu, // NiDX9Renderer::CreateSourceTextureRendererData enter
			0xA6DC69u, // NiDX9Renderer::CreateSourceTextureRendererData leave
			0xA90B45u, // NiDX9TextureManager::PrepareTextureForRendering enter
			0xA90B79u, // NiDX9TextureManager::PrepareTextureForRendering leave
			0xA90BAAu, // NiDX9TextureManager::PrepareTextureForRendering leave
			0xA90C90u, // NiDX9TextureManager::PrecacheTexture enter
			0xA90CBDu, // NiDX9TextureManager::PrecacheTexture leave
			0xA90CFCu, // NiDX9TextureManager::PrecacheTexture leave
		}};

		constexpr std::array<UInt8, 7> kSevenNops = {{
			0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u
		}};
		std::atomic<UInt32> s_directPublicationLogCount = 0;
		std::mutex s_compatibilityMutex;
		NvtfTextureLockCompatibilityState s_compatibilityState;
		bool s_compatibilityInitialized = false;
		ULONGLONG s_lastCompatibilityProbeTick = 0;

		NvtfTextureLockCompatibilityState DetectNvtfTextureLockState()
		{
			NvtfTextureLockCompatibilityState state;
			const HMODULE gameModule = GetModuleHandleW(nullptr);
			const SIZE_T gameBase = reinterpret_cast<SIZE_T>(gameModule);
			if (!gameBase)
				return state;

			for (const SIZE_T siteRva : kNvtfMode2TextureLockSiteRvas)
			{
				if (siteRva > std::numeric_limits<SIZE_T>::max() - gameBase)
					continue;
				const SIZE_T address = gameBase + siteRva;
				if (!hook_identity::IsAccessibleRegion(
					address, kSevenNops.size(), true))
				{
					continue;
				}
				++state.readableSites;
				if (hook_identity::MatchesBytesUnchecked(address,
					kSevenNops.data(), kSevenNops.size()))
				{
					++state.noppedSites;
				}
			}

			state.active = state.noppedSites != 0;
			state.exactMode2Pattern = state.readableSites
					== kNvtfMode2TextureLockSiteRvas.size()
				&& state.noppedSites
					== kNvtfMode2TextureLockSiteRvas.size();
			if (state.active)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: aggressive texture-lock bypass detected pattern=%s readableSites=%u noppedSites=%u totalSites=%u policy=direct-shadow-publication-reset-barrier-deferred-retirement largeDimensionThreshold=%u largeGpuBytesThreshold=%llu",
					state.exactMode2Pattern
						? "nvtf-mode-2" : "partial-or-foreign",
					state.readableSites, state.noppedSites,
					static_cast<UInt32>(
						kNvtfMode2TextureLockSiteRvas.size()),
					kNvtfMode2LargeAtlasDimension,
					static_cast<unsigned long long>(
						kNvtfMode2LargeAtlasGpuBytes));
			}
			return state;
		}
	}

	NvtfTextureLockCompatibilityState
		GetNvtfTextureLockCompatibilityState(bool refreshIfInactive)
	{
		// Demand text can reach the atlas before a later-loading NVSE plugin has
		// installed its patches. Deferred snapshot/group publication therefore
		// requests one refresh if the early probe was inactive. Once any bypass is
		// observed the live pattern is immutable for the process lifetime.
		std::lock_guard<std::mutex> lock(s_compatibilityMutex);
		const ULONGLONG now = GetTickCount64();
		const bool periodicInactiveRefresh = s_compatibilityInitialized
			&& !s_compatibilityState.active
			&& now - s_lastCompatibilityProbeTick >= 1000ull;
		if (!s_compatibilityInitialized
			|| (refreshIfInactive && !s_compatibilityState.active)
			|| periodicInactiveRefresh)
		{
			s_compatibilityState = DetectNvtfTextureLockState();
			s_compatibilityInitialized = true;
			s_lastCompatibilityProbeTick = now;
		}
		return s_compatibilityState;
	}

	bool IsNvtfMode2LargeAtlasPublication(
		UInt32 width, UInt32 height, UInt64 candidateGpuBytes)
	{
		if (!GetNvtfTextureLockCompatibilityState().active)
			return false;
		return width >= kNvtfMode2LargeAtlasDimension
			|| height >= kNvtfMode2LargeAtlasDimension
			|| candidateGpuBytes >= kNvtfMode2LargeAtlasGpuBytes;
	}

	void LogNvtfMode2DirectAtlasPublication(UInt32 width, UInt32 height,
		UInt64 candidateGpuBytes, bool deviceMultithreaded,
		UInt64 deviceEpoch)
	{
		const NvtfTextureLockCompatibilityState compatibility =
			GetNvtfTextureLockCompatibilityState();
		if (!compatibility.active
			|| !IsNvtfMode2LargeAtlasPublication(
				width, height, candidateGpuBytes)
			|| s_directPublicationLogCount.fetch_add(
				1, std::memory_order_relaxed) >= 16)
		{
			return;
		}
		gLog.FormattedMessage(
			"tnvse_freetype_font: NVTF-compatible large atlas direct publication pattern=%s size=%ux%u gpuBytes=%llu deviceMultithreaded=%u deviceEpoch=%llu transaction=shadow-build-then-profile-swap oldGeneration=deferred-by-property-refcount resetBarrier=active",
			compatibility.exactMode2Pattern
				? "nvtf-mode-2" : "partial-or-foreign",
			width, height,
			static_cast<unsigned long long>(candidateGpuBytes),
			deviceMultithreaded ? 1u : 0u,
			static_cast<unsigned long long>(deviceEpoch));
	}
}

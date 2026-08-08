#pragma once

#include "ITypes.h"

#include <string>
#include <string_view>
#include <vector>

namespace fonthook
{
	struct NativeTileOverlayLine
	{
		std::wstring text;
		bool highlighted = false;
	};

	bool EnsureNativeImeOverlayHost();
	bool InstallNativePrewarmOverlayLoadingThreadHook();
	bool IsNativeImeOverlayHostReady();
	bool IsNativePrewarmOverlayHostReady();
	bool IsNativeImeOverlayVisible();
	UInt32 GetNativeImeOverlayHostGeneration();

	void UpdateNativeImeOverlay(
		const std::vector<NativeTileOverlayLine>& lines,
		bool forceTextGeometryRefresh = false);
	void HideNativeImeOverlay();

	void ShowNativePrewarmOverlay();
	void UpdateNativePrewarmOverlay(
		std::wstring_view detail,
		std::wstring_view stage,
		float progress);
	bool RefreshNativePrewarmOverlayTextGeometry(UInt32 timeoutMs = 0);
	void HideNativePrewarmOverlay();
	bool QuiesceNativePrewarmOverlay(UInt32 timeoutMs);
	bool IsNativePrewarmOverlayActive();

	void ShutdownNativeTileOverlayHost();
}

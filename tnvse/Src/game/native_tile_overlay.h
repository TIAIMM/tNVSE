#pragma once

#include "ITypes.h"

#include <string>
#include <vector>

namespace fonthook
{
	struct NativeTileOverlayLine
	{
		std::wstring text;
		bool highlighted = false;
	};

	bool EnsureNativeImeOverlayHost();
	bool EnsureNativePrewarmOverlayHost();
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
		const std::wstring& detail,
		const std::wstring& stage,
		float progress);
	void RefreshNativePrewarmOverlayTextGeometry();
	void HideNativePrewarmOverlay();
	bool IsNativePrewarmOverlayActive();

	void ShutdownNativeTileOverlayHost();
}

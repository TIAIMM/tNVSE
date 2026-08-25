#pragma once

#include "ITypes.h"
#include "prewarm_overlay_mailbox.h"

#include <string>
#include <string_view>
#include <vector>

namespace fonthook
{
	using PrewarmOverlayCloseReason =
		implementation::native_tile_overlay::PrewarmOverlayCloseReason;

	struct NativeTileOverlayLine
	{
		std::wstring text;
		bool highlighted = false;
	};

	bool EnsureNativeImeOverlayHost();
	bool InstallNativePrewarmOverlayLoadingMenuUpdateHook();
	bool IsNativeImeOverlayHostReady();
	bool IsNativeImeOverlayVisible();
	UInt32 GetNativeImeOverlayHostGeneration();

	void UpdateNativeImeOverlay(
		const std::vector<NativeTileOverlayLine>& lines,
		bool forceTextGeometryRefresh = false);
	void HideNativeImeOverlay();

	void PublishNativePrewarmOverlayProgress(
		UInt64 runToken, float progress);
	void SuspendNativePrewarmOverlay(UInt64 runToken);
	void CloseNativePrewarmOverlay(
		UInt64 runToken, PrewarmOverlayCloseReason reason);
	bool IsNativePrewarmOverlayPresentationRequested();
	void LogNativePrewarmOverlayBarrierState(
		UInt64 runToken, const char* barrier);

	// LoadingMenu text diagnostics are independent of the graphical prewarm
	// mailbox. They continue to cover vanilla and third-party TileText paths.
	void LogNativeLoadingMenuDiagnostic(const char* event);
	void PumpNativeLoadingMenuDiagnostics();
	void BeginNativeLoadingMenuTextGeometryDiagnostic(
		const void* tile, const char* tileName, float fontTrait);
	void EndNativeLoadingMenuTextGeometryDiagnostic(
		const void* tile, bool producedNode);

	void ShutdownNativeTileOverlayHost();
}

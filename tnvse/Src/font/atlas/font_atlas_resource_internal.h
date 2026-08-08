#pragma once

// Private declarations shared by atlas pixel/storage and DEFAULT-pool
// lifecycle implementation units. Public atlas APIs remain in
// font_atlas_internal.h.

namespace fonthook::vectorfont
{
	bool AtlasRectContains(const AtlasRect& outer, const AtlasRect& inner);
	void BuildMergedAtlasDirtyRects(const std::vector<AtlasRect>& source,
		UInt32 width, UInt32 height, UInt32 mipLevels, AtlasPixelMode mode,
		std::vector<AtlasRect>& result);
	bool GeneratePixelDataMipRegions(NiPixelData& pixelData,
		AtlasPixelMode mode, const std::vector<AtlasRect>& dirtyRects);
	IDirect3DTexture9* CreateDynamicAtlasTexture(
		UInt32 width, UInt32 height, AtlasPixelMode mode, UInt32 mipLevels);
	bool PopulateDefaultTexture(IDirect3DTexture9* texture,
		const AtlasResource& resource, AtlasPixelMode mode);
	bool HasDefaultPoolMaintenanceLocked();
	IDirect3DTexture9* QueryAtlasD3DTexture(const AtlasResource& resource);

	namespace implementation::font_atlas_resource
	{
		bool LoadCompactSnapshotPixels(
			const CompactAtlasSnapshot& snapshot, std::vector<UInt8>& pixels);
		bool RebuildDefaultPoolTexture(AtlasResource& resource);
		bool DefaultPoolResetCallback(bool beforeReset, void*);
	}
}

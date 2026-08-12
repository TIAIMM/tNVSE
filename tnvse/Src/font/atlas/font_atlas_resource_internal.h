#pragma once

// Private declarations shared by atlas pixel/storage and DEFAULT-pool
// lifecycle implementation units. Public atlas APIs remain in
// font_atlas_internal.h.

namespace fonthook::vectorfont
{
	// Owns one participant in the DEFAULT-pool publication/reset barrier.  The
	// outermost scope on a thread keeps Reset blocked until every private D3D9
	// texture created by that transaction has either entered atlasCache or been
	// destroyed. Nested role restores share the outer transaction.
	class DefaultPoolPublicationScope
	{
	public:
		explicit DefaultPoolPublicationScope(bool enabled = true);
		~DefaultPoolPublicationScope();

		DefaultPoolPublicationScope(const DefaultPoolPublicationScope&) = delete;
		DefaultPoolPublicationScope& operator=(
			const DefaultPoolPublicationScope&) = delete;

		bool Ready() const { return m_ready; }
		bool IsCurrent() const;
		bool DeviceIsMultithreaded() const { return m_deviceMultithreaded; }
		UInt64 DeviceEpoch() const { return m_deviceEpoch; }
		const char* FailureReason() const { return m_failureReason; }

	private:
		bool m_enabled = false;
		bool m_ready = false;
		bool m_outermost = false;
		bool m_deviceMultithreaded = false;
		UInt64 m_deviceEpoch = 0;
		IDirect3DDevice9* m_device = nullptr;
		const char* m_failureReason = "disabled";
	};

	// The synchronous renderer reset callback calls these around its normal
	// atlas walk. The before phase prevents new publishers and waits without
	// holding atlasMutex; the after phase reopens publication only after every
	// registered DEFAULT generation has been rebuilt.
	bool EnterDefaultPoolResetBarrier(bool beforeReset,
		UInt32& waitedPublications, UInt64& deviceEpoch);
	void LeaveDefaultPoolResetBarrier(bool beforeReset);
	void SetDefaultPoolPublicationShutdown(bool shutdown);

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

#pragma once

#include "font_vector_internal.h"

namespace fonthook::vectorfont
{
	struct StreamingPrewarmFinalization
	{
		UInt32 fontId = 0;
		UInt64 pages = 0;
		UInt64 placements = 0;
		UInt64 bytes = 0;
		ULONGLONG finalizeStarted = 0;
		ULONGLONG publishedAt = 0;
		ULONGLONG stagedAt = 0;
		ULONGLONG repackedAt = 0;
		bool restoreRequired = false;
		bool staged = false;
		bool repacked = false;
	};

	// Raster prewarm is intentionally bounded to one CPU atlas page plus the
	// current glyph batch. Completed pages are written directly to the normal
	// .tnvfatlas snapshot format instead of retaining every GlyphBitmap until the
	// complete code page has been scanned.
	bool AppendStreamingPrewarmAtlas(RuntimeFont& arRuntime,
		const std::vector<GlyphBitmapRequest>& arRequests,
		const std::vector<std::shared_ptr<const GlyphBitmap>>& arResults,
		float afRasterScale, bool* apAllocationFailed = nullptr);

	// Publish the completed snapshot transaction without entering D3D9. The
	// blocked DeferredInit main-thread service restores DEFAULT-pool pages
	// afterwards, then the worker coordinator records and validates the transaction.
	bool PrepareStreamingPrewarmAtlasFinalization(RuntimeFont& arRuntime,
		float afRasterScale, StreamingPrewarmFinalization& arFinalization);
	bool CompleteStreamingPrewarmAtlasFinalization(RuntimeFont& arRuntime,
		float afRasterScale,
		const StreamingPrewarmFinalization& arFinalization,
		bool abRestoreSucceeded, ULONGLONG auiRestoreMs);

	// Removes temporary page files for a cancelled or failed job.
	void CancelStreamingPrewarmAtlas(RuntimeFont& arRuntime);
}

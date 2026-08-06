#pragma once

#include "font_vector_internal.h"

namespace fonthook::vectorfont
{
	// Raster prewarm is intentionally bounded to one CPU atlas page plus the
	// current glyph batch. Completed pages are written directly to the normal
	// .tnvfatlas snapshot format instead of retaining every GlyphBitmap until the
	// complete code page has been scanned.
	bool AppendStreamingPrewarmAtlas(RuntimeFont& arRuntime,
		const std::vector<GlyphBitmapRequest>& arRequests,
		const std::vector<std::shared_ptr<const GlyphBitmap>>& arResults,
		float afRasterScale, bool* apAllocationFailed = nullptr);

	// Finalizes page counts, publishes the snapshot files atomically, and restores
	// the completed page set through the normal validated snapshot loader.
	bool FinalizeStreamingPrewarmAtlas(RuntimeFont& arRuntime,
		float afRasterScale);

	// Removes temporary page files for a cancelled or failed job.
	void CancelStreamingPrewarmAtlas(RuntimeFont& arRuntime);
}

#pragma once

// Private cross-translation-unit declarations for the persistent FreeType
// cache implementation. Public cache APIs remain in the existing font
// internal headers; this file only connects the bitmap, manifest, and
// lifecycle implementation units.

namespace fonthook::vectorfont
{
	bool EnsurePersistentBitmapDirectory(std::wstring& directory);
	bool GetFileSize64(HANDLE file, UInt64& size);
	bool SetFileSize64(HANDLE file, UInt64 size);
	bool ReadFileAt(HANDLE file, UInt64 offset, void* data, UInt32 size);
	bool WriteFileAt(HANDLE file, UInt64 offset, const void* data, UInt32 size);
	void UnmapPersistentBitmapProfile(PersistentBitmapProfile& profile);

	void UnmapGlyphManifest(PersistentGlyphManifest& manifest);
	bool BuildValidatedGlyphManifestIndex(
		PersistentGlyphManifest& manifest, const RuntimeFont& runtime);
	PersistentGlyphManifest* GetGlyphManifest(RuntimeFont& runtime);
}

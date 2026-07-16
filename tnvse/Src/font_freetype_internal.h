#pragma once

// Private FreeType runtime model shared only by sibling implementation units.

#include "font_vector_internal.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_MODULE_H
#include FT_OUTLINE_H
#include FT_STROKER_H

#include <hb-ft.h>
#include <hb.h>

#include <array>
#include <deque>
#include <list>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <Windows.h>

namespace fonthook::vectorfont
{
	inline constexpr size_t kMeshCacheLimit = 8u * 1024u * 1024u;
	inline constexpr FT_Int32 kGlyphLoadFlags =
		FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP | FT_LOAD_NO_SVG;
	inline constexpr float kFixedScale = 65536.0f;

	struct MappedFontFile
	{
		std::wstring path;
		HANDLE file = INVALID_HANDLE_VALUE;
		HANDLE mapping = nullptr;
		const FT_Byte* data = nullptr;
		FT_Long size = 0;
		UInt64 contentHash = 0;

		~MappedFontFile()
		{
			if (data)
				UnmapViewOfFile(data);
			if (mapping)
				CloseHandle(mapping);
			if (file != INVALID_HANDLE_VALUE)
				CloseHandle(file);
		}
	};

	struct RuntimeFace
	{
		std::shared_ptr<MappedFontFile> file;
		FT_Face face = nullptr;
		hb_font_t* hbFont = nullptr;
		bool configured = false;
		bool configuredRaster = false;
		FT_UInt configuredWidth = 0;
		FT_UInt configuredHeight = 0;

		RuntimeFace() = default;
		RuntimeFace(const RuntimeFace&) = delete;
		RuntimeFace& operator=(const RuntimeFace&) = delete;
		RuntimeFace(RuntimeFace&& other) noexcept
			: file(std::move(other.file)), face(other.face), hbFont(other.hbFont),
			configured(other.configured), configuredRaster(other.configuredRaster),
			configuredWidth(other.configuredWidth), configuredHeight(other.configuredHeight)
		{
			other.face = nullptr;
			other.hbFont = nullptr;
		}
		RuntimeFace& operator=(RuntimeFace&& other) noexcept
		{
			if (this != &other)
			{
				if (hbFont)
					hb_font_destroy(hbFont);
				if (face)
					FT_Done_Face(face);
				file = std::move(other.file);
				face = other.face;
				hbFont = other.hbFont;
				configured = other.configured;
				configuredRaster = other.configuredRaster;
				configuredWidth = other.configuredWidth;
				configuredHeight = other.configuredHeight;
				other.face = nullptr;
				other.hbFont = nullptr;
			}
			return *this;
		}
		~RuntimeFace()
		{
			if (hbFont)
				hb_font_destroy(hbFont);
			if (face)
				FT_Done_Face(face);
		}
	};

#pragma pack(push, 1)
	struct PersistentFontHashRecord
	{
		UInt8 magic[8] = {};
		UInt32 version = 1;
		UInt32 recordSize = 0;
		UInt64 normalizedPathHash = 0;
		UInt64 fileSize = 0;
		UInt32 volumeSerial = 0;
		UInt32 fileIndexHigh = 0;
		UInt32 fileIndexLow = 0;
		UInt32 lastWriteHigh = 0;
		UInt32 lastWriteLow = 0;
		UInt64 contentHash = 0;
		UInt64 checksum = 0;
	};
#pragma pack(pop)

	struct CachedGlyphIdentity
	{
		UInt16 faceIndex = 0;
		FT_UInt glyphIndex = 0;
		UInt32 renderedCodePoint = 0;
	};

	struct RuntimeRole
	{
		const ByteStyle* style = nullptr;
		std::vector<RuntimeFace> faces;
		std::unordered_map<UInt32, CachedGlyphIdentity> glyphIdentities;
		float resolvedBaselineOffset = 0.0f;
		float visualCenterCorrection = 0.0f;
		float ascender = 0.0f;
		float descender = 0.0f;
	};

	struct VerticalEffectExtents
	{
		float top = 0.0f;
		float bottom = 0.0f;
	};

	struct ResolvedGlyph
	{
		RuntimeRole* role = nullptr;
		RuntimeFace* runtimeFace = nullptr;
		UInt32 faceIndex = 0;
		FT_UInt glyphIndex = 0;
		UInt32 renderedCodePoint = 0;
	};

	struct MeshCacheKey
	{
		UInt64 generationHash = 0;
		UInt32 fontId = 0;
		UInt32 glyphIndex = 0;
		UInt16 faceIndex = 0;
		UInt8 byteClass = 0;
		UInt8 meshType = 0;

		bool operator==(const MeshCacheKey& other) const
		{
			return generationHash == other.generationHash
				&& fontId == other.fontId
				&& glyphIndex == other.glyphIndex
				&& faceIndex == other.faceIndex
				&& byteClass == other.byteClass
				&& meshType == other.meshType;
		}
	};

	struct MeshCacheKeyHash
	{
		size_t operator()(const MeshCacheKey& key) const
		{
			size_t result = static_cast<size_t>(
				key.generationHash ^ (key.generationHash >> 32));
			result ^= static_cast<size_t>(key.fontId) * 0x9E3779B1u;
			result ^= static_cast<size_t>(key.glyphIndex) * 0x85EBCA77u;
			result ^= static_cast<size_t>(key.faceIndex) * 0xC2B2AE3Du;
			result ^= static_cast<size_t>(key.byteClass) << 8;
			result ^= key.meshType;
			return result;
		}
	};

	struct MeshCacheEntry
	{
		std::shared_ptr<GlyphMesh> mesh;
		size_t bytes = 0;
		std::list<MeshCacheKey>::iterator lru;
	};

	struct BitmapCacheKey
	{
		UInt64 fontContentHash = 0;
		SInt32 fontFaceIndex = 0;
		UInt32 glyphIndex = 0;
		UInt16 effectiveWidth = 0;
		UInt16 effectiveHeight = 0;
		SInt32 embolden26Dot6 = 0;
		SInt32 strokeWidth26Dot6 = 0;
		SInt32 slant16Dot16 = 0;
		UInt8 sdfSpread = 0;
		UInt8 maskType = 0;

		bool operator==(const BitmapCacheKey& other) const
		{
			return fontContentHash == other.fontContentHash
				&& fontFaceIndex == other.fontFaceIndex
				&& glyphIndex == other.glyphIndex
				&& effectiveWidth == other.effectiveWidth
				&& effectiveHeight == other.effectiveHeight
				&& embolden26Dot6 == other.embolden26Dot6
				&& strokeWidth26Dot6 == other.strokeWidth26Dot6
				&& slant16Dot16 == other.slant16Dot16
				&& sdfSpread == other.sdfSpread
				&& maskType == other.maskType;
		}
	};

	struct BitmapCacheKeyHash
	{
		size_t operator()(const BitmapCacheKey& key) const
		{
			size_t result = static_cast<size_t>(
				key.fontContentHash ^ (key.fontContentHash >> 32));
			result ^= static_cast<size_t>(key.fontFaceIndex) * 0x9E3779B1u;
			result ^= static_cast<size_t>(key.glyphIndex) * 0x85EBCA77u;
			result ^= static_cast<size_t>(key.effectiveWidth) << 16;
			result ^= static_cast<size_t>(key.effectiveHeight);
			result ^= static_cast<size_t>(key.embolden26Dot6) * 0x27D4EB2Du;
			result ^= static_cast<size_t>(key.strokeWidth26Dot6) * 0xC2B2AE3Du;
			result ^= static_cast<size_t>(key.slant16Dot16) * 0x165667B1u;
			result ^= static_cast<size_t>(key.sdfSpread) * 0x165667B1u;
			result ^= key.maskType;
			return result;
		}
	};

	struct BitmapCacheEntry
	{
		std::shared_ptr<GlyphBitmap> bitmap;
		size_t bytes = 0;
		std::list<BitmapCacheKey>::iterator lru;
		UInt32 sourceFontId = 0;
	};

	// Version 7 validates overlap-SDF output and falls back to coverage BSDF
	// when FreeType cannot resolve a glyph's contour geometry. Older masks are
	// left untouched and are retired through normal cleanup.
	constexpr UInt32 kPersistentBitmapVersion = 7;
	constexpr UInt32 kPersistentBitmapRecordMagic = 0x4B534D47u; // GMSK
	constexpr UInt64 kMaximumPersistentProfileBytes = 512ull * 1024ull * 1024ull;
	constexpr UInt32 kMaximumPersistentBitmapBytes = 16u * 1024u * 1024u;

	struct PersistentBitmapProfileKey
	{
		UInt64 fontContentHash = 0;
		SInt32 fontFaceIndex = 0;
		UInt16 effectiveWidth = 0;
		UInt16 effectiveHeight = 0;
		SInt32 embolden26Dot6 = 0;
		SInt32 strokeWidth26Dot6 = 0;
		SInt32 slant16Dot16 = 0;
		UInt8 sdfSpread = 0;
		UInt8 maskType = 0;

		bool operator==(const PersistentBitmapProfileKey& other) const
		{
			return fontContentHash == other.fontContentHash
				&& fontFaceIndex == other.fontFaceIndex
				&& effectiveWidth == other.effectiveWidth
				&& effectiveHeight == other.effectiveHeight
				&& embolden26Dot6 == other.embolden26Dot6
				&& strokeWidth26Dot6 == other.strokeWidth26Dot6
				&& slant16Dot16 == other.slant16Dot16
				&& sdfSpread == other.sdfSpread
				&& maskType == other.maskType;
		}
	};

	struct PersistentBitmapProfileKeyHash
	{
		size_t operator()(const PersistentBitmapProfileKey& key) const
		{
			size_t result = static_cast<size_t>(
				key.fontContentHash ^ (key.fontContentHash >> 32));
			result ^= static_cast<size_t>(key.fontFaceIndex) * 0x9E3779B1u;
			result ^= static_cast<size_t>(key.effectiveWidth) << 16;
			result ^= static_cast<size_t>(key.effectiveHeight);
			result ^= static_cast<size_t>(key.embolden26Dot6) * 0x85EBCA77u;
			result ^= static_cast<size_t>(key.strokeWidth26Dot6) * 0xC2B2AE3Du;
			result ^= static_cast<size_t>(key.slant16Dot16) * 0x27D4EB2Du;
			result ^= static_cast<size_t>(key.sdfSpread) << 8;
			result ^= key.maskType;
			return result;
		}
	};

#pragma pack(push, 1)
	struct PersistentBitmapFileHeader
	{
		UInt8 magic[8] = {};
		UInt32 version = 0;
		UInt32 headerSize = 0;
		UInt64 profileHash = 0;
		UInt64 fontContentHash = 0;
		SInt32 fontFaceIndex = 0;
		UInt16 effectiveWidth = 0;
		UInt16 effectiveHeight = 0;
		SInt32 embolden26Dot6 = 0;
		SInt32 strokeWidth26Dot6 = 0;
		SInt32 slant16Dot16 = 0;
		UInt8 sdfSpread = 0;
		UInt8 maskType = 0;
		UInt16 reserved = 0;
		UInt32 glyphCapacity = 0;
		UInt32 indexEntrySize = 0;
		UInt64 dataOffset = 0;
		UInt64 checksum = 0;
	};

	struct PersistentBitmapIndexEntry
	{
		UInt64 offset = 0;
		UInt32 size = 0;
		UInt32 reserved = 0;
	};

	struct PersistentBitmapRecordHeader
	{
		UInt32 magic = 0;
		UInt32 headerSize = 0;
		UInt32 glyphIndex = 0;
		SInt32 width = 0;
		SInt32 height = 0;
		SInt32 left = 0;
		SInt32 top = 0;
		UInt32 alphaSize = 0;
		UInt64 checksum = 0;
	};
#pragma pack(pop)

	struct PersistentBitmapProfile
	{
		PersistentBitmapProfileKey key;
		UInt64 profileHash = 0;
		UInt32 fontId = 0;
		std::wstring fontFileName;
		std::wstring path;
		HANDLE file = INVALID_HANDLE_VALUE;
		HANDLE mapping = nullptr;
		const UInt8* mappedData = nullptr;
		UInt64 mappedSize = 0;
		UInt64 validSize = 0;
		UInt32 glyphCapacity = 0;
		UInt32 recordCount = 0;
		bool writable = false;
		bool initialized = false;
		~PersistentBitmapProfile()
		{
			if (mappedData)
				UnmapViewOfFile(mappedData);
			if (mapping)
				CloseHandle(mapping);
			if (file != INVALID_HANDLE_VALUE)
				CloseHandle(file);
		}
	};

	// Version 5 stores effect-independent body metrics in a sparse, compressed,
	// content-addressed file so identical configurations share it across font IDs.
	constexpr UInt32 kPersistentGlyphManifestVersion = 5;
	constexpr UInt32 kPersistentGlyphManifestEntries = 65536;

#pragma pack(push, 1)
	struct PersistentGlyphManifestHeader
	{
		UInt8 magic[8] = {};
		UInt32 version = 0;
		UInt32 headerSize = 0;
		UInt64 manifestHash = 0;
		UInt64 layoutContentHash = 0;
		UInt64 layoutHash = 0;
		UInt32 reservedFontId = 0;
		UInt32 codePage = 0;
		UInt32 entryCount = 0;
		UInt32 entrySize = 0;
		UInt8 completeMode = 0;
		UInt8 reserved[7] = {};
		UInt64 checksum = 0;
	};

	struct PersistentGlyphManifestEntry
	{
		UInt8 valid = 0;
		UInt8 byteClass = 0;
		UInt16 faceIndex = 0;
		UInt32 glyphIndex = 0;
		UInt32 codePoint = 0;
		UInt32 renderedCodePoint = 0;
		SInt32 textureIndex = 0;
		float width = 0.0f;
		float leadingEdge = 0.0f;
		float height = 0.0f;
		float topEdge = 0.0f;
		float spacing = 0.0f;
		UInt64 checksum = 0;
	};
#pragma pack(pop)

	struct PersistentGlyphManifest
	{
		UInt64 manifestHash = 0;
		UInt64 layoutContentHash = 0;
		std::wstring path;
		HANDLE file = INVALID_HANDLE_VALUE;
		HANDLE mapping = nullptr;
		UInt8* mappedData = nullptr;
		bool writable = false;

		~PersistentGlyphManifest()
		{
			if (mappedData)
				UnmapViewOfFile(mappedData);
			if (mapping)
				CloseHandle(mapping);
			if (file != INVALID_HANDLE_VALUE)
				CloseHandle(file);
		}
	};

	struct LayoutCacheKey
	{
		UInt64 layoutHash = 0;
		UInt32 codePage = 0;
		bool allowShaping = false;
		std::string text;

		bool operator==(const LayoutCacheKey& other) const
		{
			return layoutHash == other.layoutHash && codePage == other.codePage
				&& allowShaping == other.allowShaping
				&& text == other.text;
		}
	};

	struct LayoutCacheLookupKey
	{
		UInt64 layoutHash = 0;
		UInt32 codePage = 0;
		bool allowShaping = false;
		std::string_view text;
	};

	struct LayoutCacheKeyHash
	{
		using is_transparent = void;

		size_t operator()(const LayoutCacheKey& key) const
		{
			return Hash(key.layoutHash, key.codePage,
				key.allowShaping, key.text);
		}

		size_t operator()(const LayoutCacheLookupKey& key) const
		{
			return Hash(key.layoutHash, key.codePage,
				key.allowShaping, key.text);
		}

	private:
		static size_t Hash(UInt64 layoutHash, UInt32 codePage,
			bool allowShaping, std::string_view text)
		{
			size_t result = static_cast<size_t>(layoutHash ^ (layoutHash >> 32));
			result ^= static_cast<size_t>(codePage) * 0x85EBCA77u;
			result ^= static_cast<size_t>(allowShaping) << 7;
			for (char value : text)
				result = (result ^ static_cast<UInt8>(value))
					* static_cast<size_t>(16777619u);
			return result;
		}
	};

	struct LayoutCacheKeyEqual
	{
		using is_transparent = void;

		bool operator()(const LayoutCacheKey& lhs,
			const LayoutCacheKey& rhs) const
		{
			return lhs == rhs;
		}

		bool operator()(const LayoutCacheKey& lhs,
			const LayoutCacheLookupKey& rhs) const
		{
			return lhs.layoutHash == rhs.layoutHash && lhs.codePage == rhs.codePage
				&& lhs.allowShaping == rhs.allowShaping && lhs.text == rhs.text;
		}

		bool operator()(const LayoutCacheLookupKey& lhs,
			const LayoutCacheKey& rhs) const
		{
			return (*this)(rhs, lhs);
		}
	};

	struct LayoutCacheEntry
	{
		FreeTypeLayoutRun layout;
		size_t bytes = 0;
		std::list<LayoutCacheKey>::iterator lru;
	};

	struct KerningCacheKey
	{
		UInt64 layoutHash = 0;
		UInt32 codePage = 0;
		UInt16 leftCode = 0;
		UInt16 rightCode = 0;
		UInt8 leftLength = 0;
		UInt8 rightLength = 0;

		bool operator==(const KerningCacheKey& other) const
		{
			return layoutHash == other.layoutHash && codePage == other.codePage
				&& leftCode == other.leftCode
				&& rightCode == other.rightCode && leftLength == other.leftLength
				&& rightLength == other.rightLength;
		}
	};

	struct KerningCacheKeyHash
	{
		size_t operator()(const KerningCacheKey& key) const
		{
			size_t result = static_cast<size_t>(key.layoutHash ^ (key.layoutHash >> 32));
			result ^= static_cast<size_t>(key.codePage) * 0x85EBCA77u;
			result ^= static_cast<size_t>(key.leftCode) * 0xC2B2AE3Du;
			result ^= static_cast<size_t>(key.rightCode) * 0x27D4EB2Du;
			result ^= static_cast<size_t>(key.leftLength) << 8;
			result ^= key.rightLength;
			return result;
		}
	};

	struct ActiveFontState
	{
		struct OriginalVerticalMetrics
		{
			float baseLine = 0.0f;
			float fontHeight = 0.0f;
			float maxDrop = 0.0f;
			float spaceHeight = 0.0f;
			float spaceTopEdge = 0.0f;
			bool valid = false;
		};

		const FontData* data = nullptr;
		UInt32 fontId = 0;
		OriginalVerticalMetrics originalMetrics;
		bool originalMetricsCaptured = false;
		bool originalFallbackLogged = false;
	};

	struct RuntimeFont
	{
		const FontConfig* config = nullptr;
		std::array<RuntimeRole, 2> roles;
		std::vector<hb_feature_t> hbFeatures;
		float baseLine = 0.0f;
		float glyphTop = 0.0f;
		float minBottom = 0.0f;
		float glyphHeight = 0.0f;
		float fontHeight = 0.0f;
		bool manualBaseline = false;
		bool initialized = false;
		UInt64 layoutContentHash = 0;
		UInt64 maskContentHash = 0;
		std::unique_ptr<PersistentGlyphManifest> manifest;
	};

	static_assert(sizeof(PersistentFontHashRecord) == 68);
	static_assert(sizeof(PersistentBitmapFileHeader) == 80);
	static_assert(sizeof(PersistentBitmapIndexEntry) == 16);
	static_assert(sizeof(PersistentBitmapRecordHeader) == 40);
	static_assert(sizeof(PersistentGlyphManifestHeader) == 72);
	static_assert(sizeof(PersistentGlyphManifestEntry) == 48);

	struct ActiveRuntimeCache
	{
		const Font* font = nullptr;
		const void* data = nullptr;
		UInt32 fontId = 0;
		RuntimeFont* runtime = nullptr;
	};

	struct FreeTypeThreadState
	{
		std::array<ActiveRuntimeCache, 4> activeRuntimes;
	};

	struct FreeTypeState
	{
		FT_Library library = nullptr;
		std::unordered_map<std::wstring, std::weak_ptr<MappedFontFile>> mappedFiles;
		std::unordered_map<UInt32, std::unique_ptr<RuntimeFont>> runtimeFonts;
		std::unordered_map<const Font*, ActiveFontState> activeFonts;
		std::unordered_map<MeshCacheKey, MeshCacheEntry, MeshCacheKeyHash> meshCache;
		std::list<MeshCacheKey> meshLru;
		std::unordered_map<BitmapCacheKey, BitmapCacheEntry, BitmapCacheKeyHash> bitmapCache;
		std::list<BitmapCacheKey> bitmapLru;
		std::unordered_map<PersistentBitmapProfileKey,
			std::unique_ptr<PersistentBitmapProfile>,
			PersistentBitmapProfileKeyHash> persistentBitmapProfiles;
		std::unordered_set<std::wstring> usedPersistentCachePaths;
		std::unordered_map<LayoutCacheKey, LayoutCacheEntry, LayoutCacheKeyHash,
			LayoutCacheKeyEqual> layoutCache;
		std::list<LayoutCacheKey> layoutLru;
		std::unordered_map<KerningCacheKey, float, KerningCacheKeyHash> kerningCache;
		std::deque<KerningCacheKey> kerningCacheOrder;
		std::array<UInt32, 256> singleByteCodePoints = {};
		std::array<UInt32, 65536> doubleByteCodePoints = {};
		UInt32 codePointCacheCodePage = UINT32_MAX;
		std::unordered_set<UInt32> loggedUnconfiguredFontIds;
		std::unordered_set<UInt64> loggedVerticalMetricRoles;
		std::unordered_set<UInt64> loggedHarfBuzzVerticalRoles;
		UInt32 shapingFallbackLogCount = 0;
		UInt32 overlapSdfFallbackLogCount = 0;
		bool loggedCrossFontBitmapShare = false;
		bool loggedPersistentBitmapDirectory = false;
		bool loggedPersistentBitmapHit = false;
		bool persistentBitmapUnavailable = false;
		UInt32 persistentBitmapFailureLogCount = 0;
		size_t meshCacheBytes = 0;
		size_t bitmapCacheBytes = 0;
		size_t layoutCacheBytes = 0;
		std::recursive_mutex mutex;
	};

	FreeTypeState& State();
	FreeTypeThreadState& ThreadState();

	size_t GetBitmapCacheLimit();
	size_t GetLayoutCacheLimit();
	std::wstring NormalizePathKey(std::wstring path);
	UInt64 HashBytes64(const void* data, size_t size,
		UInt64 hash = 1469598103934665603ull);
	bool ResolvePersistentFontContentHash(MappedFontFile& mapped,
		const std::wstring& normalizedPath);
	bool InitializeLibrary();
	bool ConfigureRuntimeFace(RuntimeFace& runtimeFace, const ByteStyle& style,
		float rasterScale, bool raster);
	bool LoadGlyph(RuntimeRole& role, RuntimeFace& face, FT_UInt glyphIndex);
	bool ResolveGlyph(RuntimeRole& role, UInt32 codePoint, ResolvedGlyph& result);
	bool DecodeCodePoint(const char* bytes, int length, UInt32& codePoint);
	float GetFixedCellAdvance(const ByteStyle& style);
	float GetFixedCellGlyphOffset(const ByteStyle& style, const FT_GlyphSlot slot);
	void ApplyResolvedIdentity(VectorEncodedGlyph& glyph, const ResolvedGlyph& resolved);
	VerticalEffectExtents GetVerticalEffectExtents(const FontConfig& config);
	void RemoveEffectExtentsFromMetrics(const FontConfig& config,
		UInt32 codePoint, FontLetter& metrics);
	void ApplyEffectExtentsToMetrics(const FontConfig& config,
		UInt32 codePoint, FontLetter& metrics);
	FontLetter BuildFontLetter(RuntimeRole& role, const FontConfig& config,
		VectorFontByteClass byteClass, UInt32 codePoint);
	bool ResolveVectorGlyph(RuntimeFont& runtime, const VectorEncodedGlyph& glyph,
		ResolvedGlyph& result);
	std::unique_ptr<RuntimeFont> CreateRuntimeFont(const FontConfig& config);
	bool DecodeEncodedGlyphIdentity(RuntimeFont& runtime, const char* text,
		VectorEncodedGlyph& glyph);
	RuntimeFont* FindActiveRuntime(const Font* font);
	bool LayoutRuntimeRun(RuntimeFont& runtime, const char* text,
		size_t length, bool allowShaping, FreeTypeLayoutRun& layout);

	UInt64 ComputeRuntimeMaskContentHash(RuntimeFont& runtime);
	bool LoadGlyphManifest(RuntimeFont& runtime, UInt32 encodedCode,
		VectorFontByteClass byteClass, VectorEncodedGlyph* glyph,
		FontLetter* metrics);
	void StoreGlyphManifest(RuntimeFont& runtime, const VectorEncodedGlyph& glyph,
		const ResolvedGlyph& resolved, const FontLetter& metrics);
	PersistentBitmapProfileKey MakePersistentBitmapProfileKey(
		const BitmapCacheKey& key, UInt64 fontContentHash);
	UInt64 HashBitmapKey(const BitmapCacheKey& key);
	PersistentBitmapProfile* GetPersistentBitmapProfile(
		const PersistentBitmapProfileKey& key, const std::wstring& fontFileName,
		UInt32 fontId, UInt32 glyphCapacity);
	std::shared_ptr<GlyphBitmap> LoadPersistentGlyphBitmap(
		PersistentBitmapProfile& profile, const BitmapCacheKey& key);
	bool StorePersistentGlyphBitmap(PersistentBitmapProfile& profile,
		const BitmapCacheKey& key, const GlyphBitmap& bitmap);

	std::shared_ptr<GlyphMesh> BuildGlyphMesh(RuntimeFont& runtime,
		const VectorEncodedGlyph& glyph, GlyphMeshType meshType);
	std::shared_ptr<GlyphBitmap> BuildGlyphBitmap(RuntimeFont& runtime,
		const VectorEncodedGlyph& glyph, GlyphMaskType maskType,
		float rasterScale, const BitmapCacheKey& key);
}

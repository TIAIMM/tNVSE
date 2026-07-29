#pragma once

// Private FreeType runtime model shared only by sibling implementation units.

#include "font_vector_internal.h"
#include "font_sparse_codepoint_table.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_MODULE_H
#include FT_OUTLINE_H
#include FT_STROKER_H

#include <array>
#include <atomic>
#include <list>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <Windows.h>

namespace fonthook
{
	struct DirectExtraGlyphTable;
}

namespace fonthook::vectorfont
{
	struct SealedDirectFontProfile;

	inline constexpr FT_Int32 kGlyphLoadFlags =
		FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP | FT_LOAD_NO_SVG;
	inline constexpr float kFixedScale = 65536.0f;

	struct MappedFontFile
	{
		CpuMemoryLease cpuMemory;
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

	struct DirectLayoutGlyphMetric
	{
		float advance = 0.0f;
		float fixedOffset = 0.0f;
		bool valid = false;
	};

	class SparseDirectLayoutMetricTable
	{
	public:
		static constexpr size_t kPageEntryCount = 256;
		using Page = std::array<DirectLayoutGlyphMetric, kPageEntryCount>;

		SparseDirectLayoutMetricTable() = default;
		SparseDirectLayoutMetricTable(const SparseDirectLayoutMetricTable&) = delete;
		SparseDirectLayoutMetricTable& operator=(
			const SparseDirectLayoutMetricTable&) = delete;
		SparseDirectLayoutMetricTable(SparseDirectLayoutMetricTable&& other) noexcept
			: pages_(std::move(other.pages_)),
			allocatedPageCount_(std::exchange(other.allocatedPageCount_, 0))
		{
		}
		SparseDirectLayoutMetricTable& operator=(
			SparseDirectLayoutMetricTable&& other) noexcept
		{
			if (this != &other)
			{
				pages_ = std::move(other.pages_);
				allocatedPageCount_ = std::exchange(other.allocatedPageCount_, 0);
			}
			return *this;
		}

		DirectLayoutGlyphMetric* Find(FT_UInt glyphIndex) noexcept
		{
			if (glyphIndex > std::numeric_limits<UInt16>::max())
				return nullptr;
			const std::unique_ptr<Page>& page = pages_[glyphIndex >> 8];
			return page ? &(*page)[glyphIndex & 0xFFu] : nullptr;
		}

		DirectLayoutGlyphMetric* GetOrCreate(FT_UInt glyphIndex) noexcept
		{
			if (glyphIndex > std::numeric_limits<UInt16>::max())
				return nullptr;
			std::unique_ptr<Page>& page = pages_[glyphIndex >> 8];
			if (!page)
			{
				std::unique_ptr<Page> allocated(new (std::nothrow) Page{});
				if (!allocated)
					return nullptr;
				page = std::move(allocated);
				++allocatedPageCount_;
			}
			return &(*page)[glyphIndex & 0xFFu];
		}

		size_t GetAllocatedBytes() const noexcept
		{
			return allocatedPageCount_ * sizeof(Page);
		}

		void Clear() noexcept
		{
			for (std::unique_ptr<Page>& page : pages_)
				page.reset();
			allocatedPageCount_ = 0;
		}

	private:
		std::array<std::unique_ptr<Page>, kPageEntryCount> pages_ = {};
		size_t allocatedPageCount_ = 0;
	};

	struct RuntimeFace
	{
		std::shared_ptr<MappedFontFile> file;
		FT_Face face = nullptr;
		bool configured = false;
		bool configuredRaster = false;
		FT_UInt configuredWidth = 0;
		FT_UInt configuredHeight = 0;
		UInt16 sourceConfigIndex = 0;
		float resolvedBaselineOffset = 0.0f;
		float visualCenterCorrection = 0.0f;
		SparseDirectLayoutMetricTable directLayoutMetrics;
		CpuMemoryLease directLayoutMetricMemory;

		RuntimeFace() = default;
		RuntimeFace(const RuntimeFace&) = delete;
		RuntimeFace& operator=(const RuntimeFace&) = delete;
		RuntimeFace(RuntimeFace&& other) noexcept
			: file(std::move(other.file)), face(other.face),
			configured(other.configured), configuredRaster(other.configuredRaster),
			configuredWidth(other.configuredWidth), configuredHeight(other.configuredHeight),
			sourceConfigIndex(other.sourceConfigIndex),
			resolvedBaselineOffset(other.resolvedBaselineOffset),
			visualCenterCorrection(other.visualCenterCorrection),
			directLayoutMetrics(std::move(other.directLayoutMetrics)),
			directLayoutMetricMemory(std::move(other.directLayoutMetricMemory))
		{
			other.face = nullptr;
		}
		RuntimeFace& operator=(RuntimeFace&& other) noexcept
		{
			if (this != &other)
			{
				if (face)
					FT_Done_Face(face);
				file = std::move(other.file);
				face = other.face;
				configured = other.configured;
				configuredRaster = other.configuredRaster;
				configuredWidth = other.configuredWidth;
				configuredHeight = other.configuredHeight;
				sourceConfigIndex = other.sourceConfigIndex;
				resolvedBaselineOffset = other.resolvedBaselineOffset;
				visualCenterCorrection = other.visualCenterCorrection;
				directLayoutMetrics = std::move(other.directLayoutMetrics);
				directLayoutMetricMemory = std::move(other.directLayoutMetricMemory);
				other.face = nullptr;
			}
			return *this;
		}
		~RuntimeFace()
		{
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

	struct RuntimeFont;

	struct RuntimeRole
	{
		RuntimeFont* owner = nullptr;
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

	struct BitmapCacheKey
	{
		UInt64 fontContentHash = 0;
		SInt32 fontFaceIndex = 0;
		UInt32 glyphIndex = 0;
		UInt32 codePage = 0;
		UInt16 effectiveWidth = 0;
		UInt16 effectiveHeight = 0;
		SInt32 embolden26Dot6 = 0;
		SInt32 strokeWidth26Dot6 = 0;
		SInt32 slant16Dot16 = 0;
		UInt8 sdfSpread = 0;
		UInt8 maskType = 0;
		UInt8 distanceFieldMethod = 0;
		// Derived once after the semantic key fields are resolved. It does not
		// participate in equality; the folded value also serves as the in-memory
		// unordered-map hash so all consumers share one calculation.
		UInt64 stableHash = 0;

		bool operator==(const BitmapCacheKey& other) const
		{
			return fontContentHash == other.fontContentHash
				&& fontFaceIndex == other.fontFaceIndex
				&& glyphIndex == other.glyphIndex
				&& codePage == other.codePage
				&& effectiveWidth == other.effectiveWidth
				&& effectiveHeight == other.effectiveHeight
				&& embolden26Dot6 == other.embolden26Dot6
				&& strokeWidth26Dot6 == other.strokeWidth26Dot6
				&& slant16Dot16 == other.slant16Dot16
				&& sdfSpread == other.sdfSpread
				&& maskType == other.maskType
				&& distanceFieldMethod == other.distanceFieldMethod;
		}
	};

	struct BitmapCacheKeyHash
	{
		size_t operator()(const BitmapCacheKey& key) const
		{
			if (key.stableHash)
			{
				return static_cast<size_t>(
					key.stableHash ^ (key.stableHash >> 32));
			}
			size_t result = static_cast<size_t>(
				key.fontContentHash ^ (key.fontContentHash >> 32));
			result ^= static_cast<size_t>(key.fontFaceIndex) * 0x9E3779B1u;
			result ^= static_cast<size_t>(key.glyphIndex) * 0x85EBCA77u;
			result ^= static_cast<size_t>(key.codePage) * 0xC2B2AE3Du;
			result ^= static_cast<size_t>(key.effectiveWidth) << 16;
			result ^= static_cast<size_t>(key.effectiveHeight);
			result ^= static_cast<size_t>(key.embolden26Dot6) * 0x27D4EB2Du;
			result ^= static_cast<size_t>(key.strokeWidth26Dot6) * 0xC2B2AE3Du;
			result ^= static_cast<size_t>(key.slant16Dot16) * 0x165667B1u;
			result ^= static_cast<size_t>(key.sdfSpread) * 0x165667B1u;
			result ^= key.maskType;
			result ^= static_cast<size_t>(key.distanceFieldMethod) << 24;
			return result;
		}
	};

	struct BitmapCacheEntry
	{
		std::shared_ptr<GlyphBitmap> bitmap;
		size_t bytes = 0;
		std::list<BitmapCacheKey>::iterator lru;
		UInt32 sourceFontId = 0;
		CpuMemoryLease cpuMemory;
	};

	// Version 14 adds persistent BGRA composite glyph payloads for the
	// deterministic aggressive single-quad route.
	// CPU-effect revisions are scoped through their mask identity so unchanged
	// Fill and distance-field caches do not require regeneration.
	constexpr UInt32 kPersistentBitmapVersion = 14;
	constexpr UInt32 kPersistentBitmapRecordMagic = 0x4B534D47u; // GMSK
	constexpr UInt64 kMaximumPersistentProfileBytes = 512ull * 1024ull * 1024ull;
	constexpr UInt32 kMaximumPersistentSingleChannelBitmapBytes =
		16u * 1024u * 1024u;
	constexpr UInt32 kMaximumPersistentMtsdfBitmapBytes =
		4u * kMaximumPersistentSingleChannelBitmapBytes;

	struct PersistentBitmapProfileKey
	{
		UInt64 fontContentHash = 0;
		SInt32 fontFaceIndex = 0;
		UInt32 codePage = 0;
		UInt16 effectiveWidth = 0;
		UInt16 effectiveHeight = 0;
		SInt32 embolden26Dot6 = 0;
		SInt32 strokeWidth26Dot6 = 0;
		SInt32 slant16Dot16 = 0;
		UInt8 sdfSpread = 0;
		UInt8 maskType = 0;
		UInt8 distanceFieldMethod = 0;

		bool operator==(const PersistentBitmapProfileKey& other) const
		{
			return fontContentHash == other.fontContentHash
				&& fontFaceIndex == other.fontFaceIndex
				&& codePage == other.codePage
				&& effectiveWidth == other.effectiveWidth
				&& effectiveHeight == other.effectiveHeight
				&& embolden26Dot6 == other.embolden26Dot6
				&& strokeWidth26Dot6 == other.strokeWidth26Dot6
				&& slant16Dot16 == other.slant16Dot16
				&& sdfSpread == other.sdfSpread
				&& maskType == other.maskType
				&& distanceFieldMethod == other.distanceFieldMethod;
		}
	};

	struct PersistentBitmapProfileKeyHash
	{
		size_t operator()(const PersistentBitmapProfileKey& key) const
		{
			size_t result = static_cast<size_t>(
				key.fontContentHash ^ (key.fontContentHash >> 32));
			result ^= static_cast<size_t>(key.fontFaceIndex) * 0x9E3779B1u;
			result ^= static_cast<size_t>(key.codePage) * 0xC2B2AE3Du;
			result ^= static_cast<size_t>(key.effectiveWidth) << 16;
			result ^= static_cast<size_t>(key.effectiveHeight);
			result ^= static_cast<size_t>(key.embolden26Dot6) * 0x85EBCA77u;
			result ^= static_cast<size_t>(key.strokeWidth26Dot6) * 0xC2B2AE3Du;
			result ^= static_cast<size_t>(key.slant16Dot16) * 0x27D4EB2Du;
			result ^= static_cast<size_t>(key.sdfSpread) << 8;
			result ^= key.maskType;
			result ^= static_cast<size_t>(key.distanceFieldMethod) << 24;
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
		UInt32 codePage = 0;
		UInt16 effectiveWidth = 0;
		UInt16 effectiveHeight = 0;
		SInt32 embolden26Dot6 = 0;
		SInt32 strokeWidth26Dot6 = 0;
		SInt32 slant16Dot16 = 0;
		UInt8 sdfSpread = 0;
		UInt8 maskType = 0;
		UInt8 distanceFieldMethod = 0;
		UInt8 reserved = 0;
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
		CpuMemoryLease cpuMemory;
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
		std::vector<PersistentBitmapIndexEntry> indexEntries;
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
		UInt8 distanceFieldMethod = 0;
		UInt8 cacheIdentityVersion = 0;
		UInt8 cacheDomain = 0;
		UInt8 reserved[4] = {};
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

	struct PersistentGlyphManifestRecord
	{
		UInt16 encodedCode = 0;
		UInt16 reserved = 0;
		PersistentGlyphManifestEntry entry;
	};
#pragma pack(pop)

	struct PersistentGlyphManifest
	{
		CpuMemoryLease cpuMemory;
		UInt64 manifestHash = 0;
		UInt64 layoutContentHash = 0;
		std::wstring path;
		HANDLE file = INVALID_HANDLE_VALUE;
		HANDLE mapping = nullptr;
		UInt8* mappedData = nullptr;
		UInt32 recordCount = 0;
		std::vector<UInt16> validatedRecordIndex;
		bool validatedRecordIndexReady = false;
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
		CpuMemoryLease cpuMemory;
		const FontConfig* config = nullptr;
		std::array<RuntimeRole, 2> roles;
		float baseLine = 0.0f;
		float glyphTop = 0.0f;
		float minBottom = 0.0f;
		float glyphHeight = 0.0f;
		float fontHeight = 0.0f;
		float verticalAlignmentRasterScale = 1.0f;
		bool manualBaseline = false;
		bool initialized = false;
		UInt64 layoutContentHash = 0;
		std::array<UInt64, 2> layoutContentRoleHashes = {};
		std::array<UInt64, 2> maskContentRoleHashes = {};
		std::shared_ptr<PersistentGlyphManifest> manifest;
		std::shared_ptr<DirectExtraGlyphTable> codePageMetrics;
		// Published only after both fixed encoded-slot tables and all referenced
		// atlas pages have been validated as one immutable generation.
		std::atomic<std::shared_ptr<const SealedDirectFontProfile>>
			sealedDirectProfile;
	};

	static_assert(sizeof(PersistentFontHashRecord) == 68);
	static_assert(sizeof(PersistentBitmapFileHeader) == 84);
	static_assert(sizeof(PersistentBitmapIndexEntry) == 16);
	static_assert(sizeof(PersistentBitmapRecordHeader) == 40);
	static_assert(sizeof(PersistentGlyphManifestHeader) == 72);
	static_assert(sizeof(PersistentGlyphManifestEntry) == 48);
	static_assert(sizeof(PersistentGlyphManifestRecord) == 52);

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
		std::unordered_map<BitmapCacheKey, BitmapCacheEntry, BitmapCacheKeyHash> bitmapCache;
		std::list<BitmapCacheKey> bitmapLru;
		std::unordered_map<PersistentBitmapProfileKey,
			std::unique_ptr<PersistentBitmapProfile>,
			PersistentBitmapProfileKeyHash> persistentBitmapProfiles;
		std::unordered_set<UInt32> atlasOnlyCodePageFontIds;
		std::unordered_map<UInt64, std::weak_ptr<PersistentGlyphManifest>>
			persistentGlyphManifests;
		std::vector<UInt16> persistentGlyphManifestCodes;
		std::vector<UInt16> persistentGlyphManifestGb2312Codes;
		UInt32 persistentGlyphManifestCodePage = UINT32_MAX;
		std::unordered_set<std::wstring> usedPersistentCachePaths;
		std::array<UInt32, 256> singleByteCodePoints = {};
		SparseCodePointTable<UInt32> doubleByteCodePoints;
		CpuMemoryLease codePointCacheMemory;
		UInt32 codePointCacheCodePage = UINT32_MAX;
		std::unordered_set<UInt32> loggedUnconfiguredFontIds;
		std::unordered_set<UInt64> loggedVerticalMetricRoles;
		bool loggedCrossFontBitmapShare = false;
		bool loggedPersistentBitmapDirectory = false;
		bool loggedPersistentBitmapHit = false;
		bool persistentBitmapUnavailable = false;
		bool persistentBitmapMappingsEnabled = true;
		bool completeCodePageAtlasOnlyPrewarm = false;
		bool persistentCacheRouteSynchronized = false;
		FontAtlasRoute persistentCacheRoute =
			FontAtlasRoute::ShaderDistanceField;
		PersistentFontCacheDomain persistentCacheDomain =
			PersistentFontCacheDomain::DistanceField;
		UInt32 persistentBitmapFailureLogCount = 0;
		size_t bitmapCacheBytes = 0;
		bool bitmapCacheReducedAfterPrewarm = false;
		std::recursive_mutex mutex;
	};

	FreeTypeState& State();
	FreeTypeThreadState& ThreadState();

	size_t GetBitmapCacheLimit();
	void TrimBitmapCache(FreeTypeState& state);
	void TrimFreeTypeCpuCachesForTotalBudget();
	std::wstring NormalizePathKey(std::wstring path);
	UInt64 HashBytes64(const void* data, size_t size,
		UInt64 hash = 1469598103934665603ull);
	bool ResolvePersistentFontContentHash(MappedFontFile& mapped,
		const std::wstring& normalizedPath);
	bool InitializeLibrary();
	bool ConfigureRuntimeFace(RuntimeFace& runtimeFace, const ByteStyle& style,
		float rasterScale, bool raster);
	void ReleaseSealedRuntimeFreeTypeState(RuntimeFont& runtime);
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

	UInt64 ComputeRuntimeMaskContentHash(RuntimeFont& runtime,
		VectorFontByteClass byteClass);
	UInt64 ComputeRuntimeLayoutContentHash(RuntimeFont& runtime);
	bool LoadGlyphManifest(RuntimeFont& runtime, UInt32 encodedCode,
		VectorFontByteClass byteClass, VectorEncodedGlyph* glyph,
		FontLetter* metrics);
	bool LoadGlyphManifestIdentity(RuntimeFont& runtime, UInt32 encodedCode,
		VectorFontByteClass byteClass, VectorEncodedGlyph& glyph);
	bool EnsureCompleteCodePageMetricTable(RuntimeFont& runtime);
	bool TryApplyDirectCachedLayoutMetrics(RuntimeFont& runtime, Font& font,
		float rasterScale,
		std::shared_ptr<DirectExtraGlyphTable>& codePageMetrics);
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
	struct PersistentBitmapStoreRequest
	{
		const BitmapCacheKey* key = nullptr;
		const GlyphBitmap* bitmap = nullptr;
	};
	UInt32 StorePersistentGlyphBitmaps(PersistentBitmapProfile& profile,
		const std::vector<PersistentBitmapStoreRequest>& requests,
		UInt64& storedAlphaBytes);

	std::shared_ptr<GlyphBitmap> BuildGlyphBitmap(FreeTypeState& state,
		RuntimeFont& runtime, const ResolvedGlyph& resolved, GlyphMaskType maskType,
		float rasterScale, const BitmapCacheKey& key);
}

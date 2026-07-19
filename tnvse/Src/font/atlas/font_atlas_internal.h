#pragma once

// Private atlas model and sibling-module services.

#include "font_vector_internal.h"

#include "NiPixelData.hpp"
#include "NiTexturingProperty.hpp"
#include "NiTexture.hpp"
#include "NiTriShape.hpp"

#include <atomic>
#include <list>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <Windows.h>
#include <d3d9.h>

namespace fonthook::vectorfont
{
	// The coarsest 1/4 mip needs four transparent texels on each edge.
	inline constexpr UInt32 kAtlasPadding = 4;
	inline constexpr UInt32 kMaximumAtlasMipLevels = 3;
	inline constexpr UInt32 kAtlasHardLimit = 4096;
	inline constexpr UInt32 kMaximumQuads = 16383;
	inline constexpr UInt32 kAutomaticAtlasBudgetFallbackMB = 128;
	inline constexpr UInt32 kAutomaticAtlasBudgetMinimumMB = 64;
	inline constexpr UInt32 kAutomaticAtlasBudgetMaximumMB = 256;
	inline constexpr UInt32 kAutomaticAtlasBudgetQuantumMB = 16;

	enum class AtlasLayer : UInt8
	{
		Shadow = 0,
		Glow = 1,
		Outline = 2,
		Fill = 3,
	};

	enum class PendingQuadBuildFailure : UInt8
	{
		None = 0,
		Fill,
		Glow,
		Outline,
	};

	enum class AtlasPixelMode : UInt8
	{
		Argb32 = 0,
		A8 = 1,
	};

	enum class AtlasBackend : UInt8
	{
		Managed = 0,
		DefaultPool = 1,
	};

	enum class AtlasRenderMode : UInt8
	{
		CpuEffects = 0,
		ShaderEffects = 1,
	};

	enum class AtlasSnapshotStorage : UInt8
	{
		FullMipChain = 0,
		PlacedLevelZeroRects = 1,
	};

	struct AtlasRect
	{
		UInt32 x = 0;
		UInt32 y = 0;
		UInt32 width = 0;
		UInt32 height = 0;
	};

	struct CompactAtlasSnapshot;
	inline constexpr UInt32 kNoSnapshotPlacement = std::numeric_limits<UInt32>::max();

	struct AtlasGlyphRecord
	{
		UInt64 cacheId = 0;
		AtlasRect rect;
		std::shared_ptr<const GlyphBitmap> bitmap;
		UInt32 snapshotPlacementIndex = kNoSnapshotPlacement;
	};

	struct AtlasResource
	{
		CpuMemoryLease cpuMemory;
		NiTexturingPropertyPtr property;
		NiPixelDataPtr pixelData;
		UInt32 width = 0;
		UInt32 height = 0;
		UInt32 cursorX = kAtlasPadding;
		UInt32 cursorY = kAtlasPadding;
		UInt32 shelfHeight = 0;
		UInt32 padding = kAtlasPadding;
		UInt32 generation = 0;
		UInt32 mipLevels = 1;
		AtlasPixelMode pixelMode = AtlasPixelMode::Argb32;
		AtlasBackend backend = AtlasBackend::Managed;
		AtlasRenderMode renderMode = AtlasRenderMode::CpuEffects;
		bool levelZeroOnly = false;
		bool resetPending = false;
		bool transient = false;
		bool sharedGpuPage = false;
		UInt64 pageContentHash = 0;
		std::vector<UInt8> pixels;
		// Sorted by cacheId. One contiguous allocation replaces the old placement
		// and resident-bitmap node maps, including restored placeholder objects.
		std::vector<AtlasGlyphRecord> glyphs;
		std::shared_ptr<const CompactAtlasSnapshot> compactSnapshot;
	};

	constexpr UInt32 kAtlasSnapshotVersion = 9;
	// This identity-only revision invalidates the old partial codepage snapshot
	// without forcing complete SDF-fill or unrelated atlas profiles to rebuild.
	constexpr UInt32 kCodePageEffectOnlySdfCoverageRevision = 1;
	constexpr UInt16 kMaximumAtlasSnapshotPages = 64;
#pragma pack(push, 1)
	struct AtlasSnapshotHeader
	{
		UInt8 magic[8] = {};
		UInt32 version = 0;
		UInt32 headerSize = 0;
		UInt64 snapshotHash = 0;
		UInt64 maskContentHash = 0;
		UInt64 atlasContentHash = 0;
		UInt32 reservedFontId = 0;
		UInt32 scaleMilli = 0;
		UInt32 width = 0;
		UInt32 height = 0;
		UInt32 cursorX = 0;
		UInt32 cursorY = 0;
		UInt32 shelfHeight = 0;
		UInt32 padding = 0;
		UInt32 mipLevels = 0;
		UInt8 pixelMode = 0;
		UInt8 renderMode = 0;
		UInt8 storageMode = 0;
		UInt8 byteClass = 0;
		UInt16 pageIndex = 0;
		UInt16 pageCount = 0;
		UInt32 placementCount = 0;
		UInt64 pixelBytes = 0;
		UInt64 payloadChecksum = 0;
		UInt64 pageContentHash = 0;
		UInt64 checksum = 0;
	};

	struct AtlasSnapshotPlacement
	{
		UInt64 cacheId = 0;
		AtlasRect rect;
		SInt32 left = 0;
		SInt32 top = 0;
		SInt32 effectiveWidth = 0;
		SInt32 effectiveHeight = 0;
		SInt32 strokeWidth26Dot6 = 0;
		UInt32 atlasRgb = 0x00FFFFFF;
		UInt32 bakedRgba = 0;
		UInt8 maskType = 0;
		UInt8 sdfSpread = 0;
		UInt8 colorBaked = 0;
		UInt8 bakedLayer = 0;
	};
#pragma pack(pop)

	struct CompactAtlasSnapshot
	{
		CpuMemoryLease cpuMemory;
		AtlasPixelMode pixelMode = AtlasPixelMode::A8;
		std::vector<AtlasSnapshotPlacement> placements;
		std::vector<UInt8> pixels;
		std::wstring sourcePath;
		AtlasSnapshotHeader sourceHeader;
	};

	struct RetiredAtlasGeneration
	{
		std::shared_ptr<AtlasResource> resource;
	};

	struct AtlasCacheKey
	{
		UInt64 atlasContentHash = 0;
		UInt32 fontId = 0;
		UInt32 scaleMilli = 1000;
		AtlasPixelMode pixelMode = AtlasPixelMode::Argb32;
		AtlasRenderMode renderMode = AtlasRenderMode::CpuEffects;
		UInt32 padding = kAtlasPadding;
		bool levelZeroOnly = false;
		VectorFontByteClass byteClass = VectorFontByteClass::SingleByte;
		UInt16 pageIndex = 0;

		bool operator==(const AtlasCacheKey& other) const
		{
			return atlasContentHash == other.atlasContentHash
				&& scaleMilli == other.scaleMilli && pixelMode == other.pixelMode
				&& renderMode == other.renderMode && padding == other.padding
				&& levelZeroOnly == other.levelZeroOnly
				&& byteClass == other.byteClass
				&& pageIndex == other.pageIndex;
		}
	};

	struct AtlasCacheKeyHash
	{
		size_t operator()(const AtlasCacheKey& key) const
		{
			size_t result = static_cast<size_t>(
				key.atlasContentHash ^ (key.atlasContentHash >> 32));
			result ^= static_cast<size_t>(key.scaleMilli) * 0x85EBCA77u;
			result ^= static_cast<size_t>(key.pixelMode) << 4;
			result ^= static_cast<size_t>(key.renderMode) << 6;
			result ^= static_cast<size_t>(key.padding) * 0x27D4EB2Du;
			result ^= static_cast<size_t>(key.levelZeroOnly) << 11;
			result ^= static_cast<size_t>(key.byteClass) << 12;
			result ^= static_cast<size_t>(key.pageIndex) * 0x165667B1u;
			return result;
		}
	};

	struct AtlasCacheEntry
	{
		std::shared_ptr<AtlasResource> resource;
		size_t bytes = 0;
		std::list<AtlasCacheKey>::iterator lru;
		CpuMemoryLease cpuMemory;
	};

	struct PendingQuad
	{
		std::shared_ptr<const GlyphBitmap> bitmap;
		NiPoint3 pen;
		NiColorA baseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		NiColorA layerColorModifier = { 1.0f, 1.0f, 1.0f, 1.0f };
		float offsetX = 0.0f;
		float offsetY = 0.0f;
		float rasterScale = 1.0f;
		float logicalTopEdge = 0.0f;
		float baselineOffset = 0.0f;
		UInt32 expansionPixels = 0;
		AtlasLayer layer = AtlasLayer::Fill;
		UInt8 layerMask = 1u << static_cast<UInt8>(AtlasLayer::Fill);
		VectorFontByteClass byteClass = VectorFontByteClass::SingleByte;
		bool usesSdf = false;
		bool usesLiveTileRgb = true;
		UInt16 atlasPage = 0;
	};

	struct AtlasProfileKey
	{
		UInt64 atlasContentHash = 0;
		UInt32 scaleMilli = 1000;
		AtlasPixelMode pixelMode = AtlasPixelMode::Argb32;
		AtlasRenderMode renderMode = AtlasRenderMode::CpuEffects;
		UInt32 padding = kAtlasPadding;
		bool levelZeroOnly = false;
		VectorFontByteClass byteClass = VectorFontByteClass::SingleByte;

		bool operator==(const AtlasProfileKey& other) const
		{
			return atlasContentHash == other.atlasContentHash
				&& scaleMilli == other.scaleMilli && pixelMode == other.pixelMode
				&& renderMode == other.renderMode && padding == other.padding
				&& levelZeroOnly == other.levelZeroOnly
				&& byteClass == other.byteClass;
		}
	};

	struct AtlasProfileKeyHash
	{
		size_t operator()(const AtlasProfileKey& key) const
		{
			size_t result = static_cast<size_t>(
				key.atlasContentHash ^ (key.atlasContentHash >> 32));
			result ^= static_cast<size_t>(key.scaleMilli) * 0x85EBCA77u;
			result ^= static_cast<size_t>(key.pixelMode) << 4;
			result ^= static_cast<size_t>(key.renderMode) << 6;
			result ^= static_cast<size_t>(key.padding) * 0x27D4EB2Du;
			result ^= static_cast<size_t>(key.levelZeroOnly) << 11;
			result ^= static_cast<size_t>(key.byteClass) << 12;
			return result;
		}
	};

	struct AtlasProfileIndex
	{
		CpuMemoryLease cpuMemory;
		std::vector<UInt16> pages;
		std::unordered_map<UInt64, UInt16> residentPages;
		std::unordered_map<UInt16, std::vector<UInt64>> pageResidents;
		std::unordered_set<UInt64> duplicateResidents;
	};

	struct BatchTemplateKey
	{
		uintptr_t atlasIdentity = 0;
		UInt64 contentHash = 0;
		UInt32 generation = 0;
		UInt32 quadCount = 0;

		bool operator==(const BatchTemplateKey& other) const
		{
			return atlasIdentity == other.atlasIdentity && contentHash == other.contentHash
				&& generation == other.generation && quadCount == other.quadCount;
		}
	};

	struct BatchTemplateKeyHash
	{
		size_t operator()(const BatchTemplateKey& key) const
		{
			return static_cast<size_t>(key.contentHash ^ (key.contentHash >> 32))
				^ key.atlasIdentity ^ (static_cast<size_t>(key.generation) << 8)
				^ key.quadCount;
		}
	};

	struct BatchTemplate
	{
		CpuMemoryLease cpuMemory;
		std::vector<NiPoint3> vertices;
		std::vector<NiPoint2> texture;
		std::vector<UInt16> indices;
		NiBound bound;
	};

	struct QuadBatchFingerprint
	{
		UInt64 contentHash = 0;
		UInt32 quadCount = 0;
	};

	struct BatchTemplateEntry
	{
		std::shared_ptr<const BatchTemplate> data;
		size_t bytes = 0;
		std::list<BatchTemplateKey>::iterator lru;
		CpuMemoryLease cpuMemory;
	};

	class DefaultAtlasTexture : public NiTexture
	{
	public:
		static DefaultAtlasTexture* Create(IDirect3DTexture9* texture,
			AtlasPixelMode mode);
		UInt32 GetWidthEx() const;
		UInt32 GetHeightEx() const;

	private:
		static void* s_vtable[41];
	};

	struct ShaderEffectBuild
	{
		A8EffectShapeConfig config;
		UInt32 padding = kAtlasPadding;
		UInt32 drawQuadCount = 0;
	};

	static_assert(sizeof(AtlasSnapshotHeader) == 120);
	static_assert(sizeof(AtlasSnapshotPlacement) == 56);

	struct AtlasState
	{
		std::unordered_map<AtlasCacheKey, AtlasCacheEntry, AtlasCacheKeyHash> atlasCache;
		std::unordered_map<AtlasProfileKey, AtlasProfileIndex,
			AtlasProfileKeyHash> atlasProfiles;
		std::unordered_set<AtlasProfileKey, AtlasProfileKeyHash>
			completeAtlasProfiles;
		std::unordered_multimap<UInt64, std::weak_ptr<AtlasResource>> atlasPageDedup;
		std::list<AtlasCacheKey> atlasLru;
		size_t atlasCacheBytes = 0;
		std::mutex atlasMutex;
		std::vector<RetiredAtlasGeneration> retiredAtlases;
		bool defaultPoolResetRegistered = false;
		bool defaultPoolShutdown = false;
		std::atomic<bool> defaultPoolMaintenancePending = false;
		bool budgetResolved = false;
		size_t resolvedGpuBudgetBytes = 0;
		UInt32 lastAvailableTextureMemoryMB = 0;
		UInt32 defaultPoolFailureLogCount = 0;
		UInt32 atlasFailureLogCount = 0;
		UInt32 shaderBatchFailureLogCount = 0;
		UInt32 cpuMaskFailureLogCount = 0;
		std::unordered_set<UInt64> loggedAtlasBatches;
		std::unordered_set<UInt32> loggedVerticalMetricFonts;
		std::unordered_set<UInt64> loggedQualityDowngrades;
		std::unordered_map<BatchTemplateKey, BatchTemplateEntry,
			BatchTemplateKeyHash> batchCache;
		std::list<BatchTemplateKey> batchLru;
		size_t batchCacheBytes = 0;
		std::mutex batchMutex;
	};

	AtlasState& State();

	NiColorA ResolveSafeTileColor(const std::vector<AtlasGlyphInstance>& glyphs,
		const NiColorA& requested);
	bool BuildPendingQuads(RuntimeFont& runtime,
		const std::vector<AtlasGlyphInstance>& glyphs, float rasterScale,
		const std::array<bool, 4>& included, const NiColorA& tileColor,
		std::vector<PendingQuad>& quads, PendingQuadBuildFailure& failure);
	bool BuildShaderEffectQuads(RuntimeFont& runtime,
		const std::vector<AtlasGlyphInstance>& glyphs, float rasterScale,
		EffectQuality quality, const NiColorA& tileColor, bool suppressEffects,
		std::vector<PendingQuad>& quads, ShaderEffectBuild& build);
	NiTriShape* TryCreateAtlasShapeForMode(Font& font,
		const std::vector<PendingQuad>& quads,
		const FontConfig& config, float rasterScale, bool prepareObject,
		AtlasPixelMode pixelMode, AtlasRenderMode renderMode, UInt32 padding,
		std::vector<std::shared_ptr<AtlasResource>>& atlases,
		const NiColorA& tileColor, bool useCustomA8Shader,
		const A8EffectShapeConfig* effectConfig = nullptr);

	UInt32 AtlasBytesPerPixel(AtlasPixelMode mode);
	UInt32 GetAtlasMipLevelCount(UInt32 width, UInt32 height,
		bool levelZeroOnly = false);
	size_t GetAtlasStorageBytes(UInt32 width, UInt32 height,
		AtlasPixelMode mode, UInt32 mipLevels);
	size_t GetResidentMaskBytes(const AtlasResource& resource);
	size_t GetCompactSnapshotBytes(const AtlasResource& resource);
	NiTexture* GetAtlasTexture(const AtlasResource& resource);
	NiTexturingProperty* CreateManagedAtlasProperty(UInt32 width, UInt32 height,
		AtlasPixelMode mode, UInt32 mipLevels, const std::vector<UInt8>& source,
		NiPixelDataPtr& outPixelData);
	void RetireDefaultGeneration(const AtlasResource& resource);
	bool AddBitmapsToAtlas(AtlasResource& resource,
		const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps);
	bool RecreateManagedAtlasProperty(AtlasResource& resource);
	void IndexAtlasPage(AtlasState& state, const AtlasCacheKey& key,
		const AtlasResource& resource);
	void UnindexAtlasPage(AtlasState& state, const AtlasCacheKey& key);
	AtlasProfileKey MakeAtlasProfileKey(const AtlasCacheKey& key);
	void TrimAtlasCache(AtlasState& state);
	void TrimBatchCache(AtlasState& state);
	void TrimAtlasCpuCachesForTotalBudget();
	void ResolveGpuAtlasBudget(bool force);
	size_t GetAtlasCacheLimit();
	UInt32 GetMaximumAtlasSize();
	bool PlaceBitmap(AtlasResource& resource, const GlyphBitmap& bitmap,
		AtlasRect& rect);
	bool BuildNextMipLevel(const UInt8* source, UInt32 sourceWidth,
		UInt32 sourceHeight, size_t sourcePitch, AtlasPixelMode mode,
		std::vector<UInt8>& destination);
	void WriteBitmapPixels(UInt8* destination, LONG pitch, AtlasPixelMode mode,
		const GlyphBitmap& bitmap, const AtlasRect& rect,
		UInt32 destinationX = 0, UInt32 destinationY = 0);
	bool WriteCompactSnapshotPixels(UInt8* destination, LONG pitch,
		AtlasPixelMode destinationMode, const AtlasResource& resource);
	bool CreateDefaultPoolAtlas(AtlasResource& resource,
		AtlasPixelMode requestedMode);
	bool TryReuseDefaultPoolAtlasPage(const std::shared_ptr<AtlasResource>& resource,
		UInt64 pageContentHash);
	std::shared_ptr<const GlyphBitmap> GetOrCreateAtlasGlyphBitmap(
		AtlasResource& resource, UInt64 cacheId);
	AtlasGlyphRecord* FindAtlasGlyph(AtlasResource& resource, UInt64 cacheId);
	const AtlasGlyphRecord* FindAtlasGlyph(const AtlasResource& resource, UInt64 cacheId);
	void SortAtlasGlyphs(AtlasResource& resource);
	void RefreshAtlasResourceCpuMemory(AtlasResource& resource);
	void RegisterDefaultPoolAtlasPage(const std::shared_ptr<AtlasResource>& resource,
		UInt64 pageContentHash);
	void CopyBitmapToAtlas(AtlasResource& resource, const GlyphBitmap& bitmap,
		const AtlasRect& rect);

	bool PackAtlas(const std::vector<std::shared_ptr<const GlyphBitmap>>& source,
		UInt32& width, UInt32& height,
		std::unordered_map<UInt64, AtlasRect>& placements, UInt32 padding);
	std::vector<std::shared_ptr<AtlasResource>> GetAtlasResources(
		const FontConfig& config, VectorFontByteClass byteClass, float rasterScale,
		const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
		AtlasPixelMode pixelMode, AtlasRenderMode renderMode, UInt32 padding,
		// When requested, entries align with bitmaps and index the returned pages.
		std::vector<UInt16>* outBitmapPageOrdinals = nullptr);
	void GetAtlasBackedGlyphBitmaps(RuntimeFont& runtime,
		const std::vector<GlyphBitmapRequest>& requests, float rasterScale,
		AtlasPixelMode pixelMode, AtlasRenderMode renderMode, UInt32 padding,
		std::vector<std::shared_ptr<const GlyphBitmap>>& results);
	std::shared_ptr<AtlasResource> CreateTransientAtlas(
		const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
		AtlasPixelMode pixelMode, AtlasRenderMode renderMode, UInt32 padding);
	UInt64 BuildPrewarmAtlasContentHash(const FontConfig& config,
		VectorFontByteClass byteClass, float rasterScale, bool shaderEffects);

	void InitializeDefaultPoolAtlasLifecycle();
	void PumpDefaultPoolAtlasLifecycle();
	void ShutdownDefaultPoolAtlasLifecycle();
}

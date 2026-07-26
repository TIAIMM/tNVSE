#pragma once

// Private atlas model and sibling-module services.

#include "font_vector_internal.h"
#include "font_native_internal.h"

#include "NiPixelData.hpp"
#include "NiTexturingProperty.hpp"
#include "NiTexture.hpp"
#include "NiTriShape.hpp"

#include <atomic>
#include <cmath>
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
	// Shader distance-field atlases are level-zero-only. One outside-distance texel on
	// each edge isolates bilinear samples; the field already contains its spread.
	inline constexpr UInt32 kDistanceFieldAtlasPadding = 1;
	inline constexpr UInt32 kMtsdfAtlasPadding = kDistanceFieldAtlasPadding;
	// Compatibility alias for internal structures whose field name is still SDF.
	// CPU-baked ARGB atlases retain up to three mip levels. Four transparent
	// level-zero texels leave one transparent texel at the coarsest 1/4 mip.
	inline constexpr UInt32 kArgbAtlasPadding = 4;
	inline constexpr UInt32 kMaximumAtlasMipLevels = 3;
	inline constexpr UInt32 kAtlasHardLimit = 4096;
	// Keep a streamed/repacked distance-field or aggressive coverage page within
	// the old 4096x4096 A8 storage envelope. In MTSDF mode a 4096x4096 BGRA page
	// needs 64 MiB before vector growth and repack scratch space, which can fail
	// late in the 32-bit prewarm.
	inline constexpr UInt32 kMaximumMtsdfPrewarmAtlasSize = 2048;
	inline constexpr size_t kMaximumMtsdfPrewarmPageBytes =
		static_cast<size_t>(kMaximumMtsdfPrewarmAtlasSize)
			* kMaximumMtsdfPrewarmAtlasSize * 4u;
	static_assert(kMaximumMtsdfPrewarmPageBytes == 16u * 1024u * 1024u);
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
		Shadow,
		Glow,
		Outline,
	};

	enum class AtlasPixelMode : UInt8
	{
		Argb32 = 0,
		A8 = 1,
		Mtsdf32 = 2,
	};

	inline AtlasPixelMode GetConfiguredDistanceFieldAtlasPixelMode()
	{
		return UsesMtsdfDistanceField()
			? AtlasPixelMode::Mtsdf32 : AtlasPixelMode::A8;
	}

	inline bool IsDistanceFieldAtlasPixelMode(AtlasPixelMode mode)
	{
		return mode == AtlasPixelMode::A8 || mode == AtlasPixelMode::Mtsdf32;
	}

	inline bool IsCompatibleDistanceFieldBitmap(
		AtlasPixelMode mode, const GlyphBitmap& bitmap)
	{
		if (bitmap.maskType != GlyphMaskType::DistanceField)
			return mode != AtlasPixelMode::Mtsdf32;
		return (mode == AtlasPixelMode::Mtsdf32
				&& bitmap.distanceFieldMethod == DistanceFieldMethod::Mtsdf)
			|| (mode != AtlasPixelMode::Mtsdf32
				&& bitmap.distanceFieldMethod == DistanceFieldMethod::TrueSdf);
	}

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

	struct AtlasGlyphPlacement
	{
		uintptr_t atlasIdentity = 0;
		UInt32 atlasGeneration = 0;
		UInt32 atlasWidth = 0;
		UInt32 atlasHeight = 0;
		UInt16 pageIndex = std::numeric_limits<UInt16>::max();
		UInt16 reserved = 0;
		float inverseWidth = 0.0f;
		float inverseHeight = 0.0f;
		float u0 = 0.0f;
		float v0 = 0.0f;
		float u1 = 0.0f;
		float v1 = 0.0f;
	};

	struct CompactAtlasSnapshot;
	inline constexpr UInt32 kNoSnapshotPlacement = std::numeric_limits<UInt32>::max();

	struct AtlasGlyphRecord
	{
		UInt64 cacheId = 0;
		AtlasRect rect;
		std::shared_ptr<const GlyphBitmap> bitmap;
		UInt32 snapshotPlacementIndex = kNoSnapshotPlacement;
		// Cache the page and normalized texture coordinates beside the glyph rect.
		// Text compilation copies this immutable value into PendingQuad, eliminating
		// its former per-quad page search, rect lookup, and UV division.
		AtlasGlyphPlacement placement;
	};

	struct AtlasResource
	{
		CpuMemoryLease cpuMemory;
		NiTexturingPropertyPtr property;
		NiPixelDataPtr pixelData;
		UInt32 width = 0;
		UInt32 height = 0;
		UInt32 cursorX = kArgbAtlasPadding;
		UInt32 cursorY = kArgbAtlasPadding;
		UInt32 shelfHeight = 0;
		UInt32 padding = kArgbAtlasPadding;
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

	inline bool IsAtlasGlyphPlacementForAtlas(
		const AtlasGlyphPlacement& placement, const AtlasResource& atlas)
	{
		return placement.atlasIdentity == reinterpret_cast<uintptr_t>(&atlas)
			&& placement.atlasGeneration == atlas.generation
			&& placement.atlasWidth == atlas.width
			&& placement.atlasHeight == atlas.height
			&& placement.inverseWidth > 0.0f
			&& placement.inverseHeight > 0.0f;
	}

	inline bool IsAtlasGlyphPlacementCurrent(const AtlasGlyphPlacement& placement,
		const AtlasResource& atlas, UInt16 pageIndex)
	{
		return IsAtlasGlyphPlacementForAtlas(placement, atlas)
			&& placement.pageIndex == pageIndex;
	}

	inline bool CacheAtlasGlyphPlacement(AtlasGlyphRecord& glyph,
		const AtlasResource& atlas, UInt16 pageIndex)
	{
		if (!atlas.width || !atlas.height || !glyph.rect.width || !glyph.rect.height)
			return false;
		if (IsAtlasGlyphPlacementCurrent(glyph.placement, atlas, pageIndex))
			return true;
		// Snapshot records store profile-local page numbers. Text batches compact the
		// pages from both byte roles into one list, so only the runtime page ordinal
		// needs rebinding when the same atlas object and generation are still active.
		if (IsAtlasGlyphPlacementForAtlas(glyph.placement, atlas))
		{
			glyph.placement.pageIndex = pageIndex;
			return true;
		}
		AtlasGlyphPlacement placement;
		placement.atlasIdentity = reinterpret_cast<uintptr_t>(&atlas);
		placement.atlasGeneration = atlas.generation;
		placement.atlasWidth = atlas.width;
		placement.atlasHeight = atlas.height;
		placement.pageIndex = pageIndex;
		placement.inverseWidth = 1.0f / static_cast<float>(atlas.width);
		placement.inverseHeight = 1.0f / static_cast<float>(atlas.height);
		placement.u0 = static_cast<float>(glyph.rect.x) * placement.inverseWidth;
		placement.v0 = static_cast<float>(glyph.rect.y) * placement.inverseHeight;
		placement.u1 = static_cast<float>(glyph.rect.x + glyph.rect.width)
			* placement.inverseWidth;
		placement.v1 = static_cast<float>(glyph.rect.y + glyph.rect.height)
			* placement.inverseHeight;
		glyph.placement = placement;
		return true;
	}

	constexpr UInt32 kAtlasSnapshotVersion = 16;
	constexpr UInt32 kAtlasSnapshotFlagGloballyRepacked = 1u << 0;
	constexpr UInt32 kAtlasSnapshotKnownFlags =
		kAtlasSnapshotFlagGloballyRepacked;
	// CPU-effect revisions are part of the atlas content identity. Keeping the
	// container version stable preserves unrelated true-SDF/MTSDF snapshots.
	// Version 16 identifies selectable A8 true-SDF and BGRA MTSDF pages.
	// Version 15 adds largest-compatible-size double-byte MTSDF atlas sharing.
	// Version 14 replaced single-channel shader pages with BGRA MTSDF.
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
		UInt32 flags = 0;
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
		UInt64 storedPixelBytes = 0;
		UInt64 payloadChecksum = 0;
		UInt64 pageContentHash = 0;
		UInt64 checksum = 0;
	};

	struct AtlasSnapshotGlyphPlacement
	{
		UInt32 atlasWidth = 0;
		UInt32 atlasHeight = 0;
		UInt16 pageIndex = std::numeric_limits<UInt16>::max();
		UInt16 reserved = 0;
		float inverseWidth = 0.0f;
		float inverseHeight = 0.0f;
		float u0 = 0.0f;
		float v0 = 0.0f;
		float u1 = 0.0f;
		float v1 = 0.0f;
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
		AtlasSnapshotGlyphPlacement glyphPlacement;
	};
#pragma pack(pop)

	inline bool CacheAtlasSnapshotGlyphPlacement(
		AtlasSnapshotPlacement& snapshot, UInt32 atlasWidth, UInt32 atlasHeight,
		UInt16 pageIndex)
	{
		const AtlasRect& rect = snapshot.rect;
		if (!atlasWidth || !atlasHeight || !rect.width || !rect.height
			|| rect.x > atlasWidth || rect.width > atlasWidth - rect.x
			|| rect.y > atlasHeight || rect.height > atlasHeight - rect.y)
			return false;
		AtlasSnapshotGlyphPlacement placement;
		placement.atlasWidth = atlasWidth;
		placement.atlasHeight = atlasHeight;
		placement.pageIndex = pageIndex;
		placement.inverseWidth = 1.0f / static_cast<float>(atlasWidth);
		placement.inverseHeight = 1.0f / static_cast<float>(atlasHeight);
		placement.u0 = static_cast<float>(rect.x) * placement.inverseWidth;
		placement.v0 = static_cast<float>(rect.y) * placement.inverseHeight;
		placement.u1 = static_cast<float>(rect.x + rect.width)
			* placement.inverseWidth;
		placement.v1 = static_cast<float>(rect.y + rect.height)
			* placement.inverseHeight;
		snapshot.glyphPlacement = placement;
		return true;
	}

	inline bool IsValidAtlasSnapshotGlyphPlacement(
		const AtlasSnapshotPlacement& snapshot, UInt32 atlasWidth,
		UInt32 atlasHeight, UInt16 pageIndex)
	{
		const AtlasSnapshotGlyphPlacement& cached = snapshot.glyphPlacement;
		if (cached.atlasWidth != atlasWidth || cached.atlasHeight != atlasHeight
			|| cached.pageIndex != pageIndex || !std::isfinite(cached.inverseWidth)
			|| !std::isfinite(cached.inverseHeight) || !std::isfinite(cached.u0)
			|| !std::isfinite(cached.v0) || !std::isfinite(cached.u1)
			|| !std::isfinite(cached.v1) || cached.inverseWidth <= 0.0f
			|| cached.inverseHeight <= 0.0f || cached.u0 < 0.0f
			|| cached.v0 < 0.0f || cached.u1 <= cached.u0
			|| cached.v1 <= cached.v0 || cached.u1 > 1.0f || cached.v1 > 1.0f)
			return false;
		const float inverseWidth = 1.0f / static_cast<float>(atlasWidth);
		const float inverseHeight = 1.0f / static_cast<float>(atlasHeight);
		const AtlasRect& rect = snapshot.rect;
		constexpr float epsilon = 1.0e-6f;
		auto matches = [epsilon](float left, float right)
		{
			return std::fabs(left - right) <= epsilon;
		};
		return matches(cached.inverseWidth, inverseWidth)
			&& matches(cached.inverseHeight, inverseHeight)
			&& matches(cached.u0, static_cast<float>(rect.x) * inverseWidth)
			&& matches(cached.v0, static_cast<float>(rect.y) * inverseHeight)
			&& matches(cached.u1,
				static_cast<float>(rect.x + rect.width) * inverseWidth)
			&& matches(cached.v1,
				static_cast<float>(rect.y + rect.height) * inverseHeight);
	}

	inline bool RestoreAtlasSnapshotGlyphPlacement(
		const AtlasSnapshotPlacement& snapshot, const AtlasResource& atlas,
		UInt16 snapshotPageIndex, UInt16 runtimePageIndex,
		AtlasGlyphPlacement& placement)
	{
		if (!IsValidAtlasSnapshotGlyphPlacement(snapshot, atlas.width, atlas.height,
			snapshotPageIndex))
			return false;
		const AtlasSnapshotGlyphPlacement& cached = snapshot.glyphPlacement;
		placement.atlasIdentity = reinterpret_cast<uintptr_t>(&atlas);
		placement.atlasGeneration = atlas.generation;
		placement.atlasWidth = atlas.width;
		placement.atlasHeight = atlas.height;
		placement.pageIndex = runtimePageIndex;
		placement.inverseWidth = cached.inverseWidth;
		placement.inverseHeight = cached.inverseHeight;
		placement.u0 = cached.u0;
		placement.v0 = cached.v0;
		placement.u1 = cached.u1;
		placement.v1 = cached.v1;
		return true;
	}

	struct CompactAtlasSnapshot
	{
		CpuMemoryLease cpuMemory;
		AtlasPixelMode pixelMode = GetConfiguredDistanceFieldAtlasPixelMode();
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
		UInt32 padding = kArgbAtlasPadding;
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
		float sourceToLogicalScale = 1.0f;
		float logicalTopEdge = 0.0f;
		float baselineOffset = 0.0f;
		UInt32 expansionPixels = 0;
		AtlasLayer layer = AtlasLayer::Fill;
		UInt8 layerMask = 1u << static_cast<UInt8>(AtlasLayer::Fill);
		VectorFontByteClass byteClass = VectorFontByteClass::SingleByte;
		bool usesSdf = false;
		bool usesLiveTileRgb = true;
		// Shader-effect quads that originate from the same shaped glyph share this
		// ordinal (for example, an offset shadow quad and its body quad).  It lets
		// the composite route distinguish intentional intra-glyph overlap from
		// overlap whose global layer order must remain on the stock multi-pass path.
		UInt32 glyphOrdinal = std::numeric_limits<UInt32>::max();
		UInt16 atlasPage = 0;
		AtlasGlyphPlacement atlasPlacement;
	};

	struct AtlasProfileKey
	{
		UInt64 atlasContentHash = 0;
		UInt32 scaleMilli = 1000;
		AtlasPixelMode pixelMode = AtlasPixelMode::Argb32;
		AtlasRenderMode renderMode = AtlasRenderMode::CpuEffects;
		UInt32 padding = kArgbAtlasPadding;
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

	struct TextArtifactKey
	{
		uintptr_t atlasIdentity = 0;
		UInt64 contentHash = 0;
		UInt32 generation = 0;
		UInt32 quadCount = 0;

		bool operator==(const TextArtifactKey& other) const
		{
			return atlasIdentity == other.atlasIdentity && contentHash == other.contentHash
				&& generation == other.generation && quadCount == other.quadCount;
		}
	};

	struct TextArtifactKeyHash
	{
		size_t operator()(const TextArtifactKey& key) const
		{
			return static_cast<size_t>(key.contentHash ^ (key.contentHash >> 32))
				^ key.atlasIdentity ^ (static_cast<size_t>(key.generation) << 8)
				^ key.quadCount;
		}
	};

	struct QuadBatchFingerprint
	{
		UInt64 contentHash = 0;
		UInt32 quadCount = 0;
	};

	struct TextArtifactEntry
	{
		NativeA8PayloadTemplatePtr data;
		size_t bytes = 0;
		std::list<TextArtifactKey>::iterator lru;
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
		UInt32 padding = kDistanceFieldAtlasPadding;
		UInt32 drawQuadCount = 0;
	};

	static_assert(sizeof(AtlasSnapshotHeader) == 128);
	static_assert(sizeof(AtlasSnapshotGlyphPlacement) == 36);
	static_assert(sizeof(AtlasSnapshotPlacement) == 92);

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
		std::unordered_map<TextArtifactKey, TextArtifactEntry,
			TextArtifactKeyHash> textArtifactCache;
		std::list<TextArtifactKey> textArtifactLru;
		size_t textArtifactCacheBytes = 0;
		std::mutex textArtifactMutex;
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
	bool LoadCompactAtlasSnapshotPixels(const CompactAtlasSnapshot& snapshot,
		std::vector<UInt8>& pixels);
	NiTexture* GetAtlasTexture(const AtlasResource& resource);
	NiTexturingProperty* CreateManagedAtlasProperty(UInt32 width, UInt32 height,
		AtlasPixelMode mode, UInt32 mipLevels, const std::vector<UInt8>& source,
		NiPixelDataPtr& outPixelData);
	// Takes ownership of d3dTexture on success and sets it to null.  Composite
	// cache RTTs use the same renderer-data wrapper and Tile property contract as
	// DEFAULT-pool glyph atlas pages.
	NiTexturingProperty* CreateDefaultTextureProperty(
		IDirect3DTexture9*& d3dTexture, AtlasPixelMode mode);
	void RetireDefaultGeneration(const AtlasResource& resource);
	bool AddBitmapsToAtlas(AtlasResource& resource,
		const std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps);
	bool RecreateManagedAtlasProperty(AtlasResource& resource);
	void IndexAtlasPage(AtlasState& state, const AtlasCacheKey& key,
		const AtlasResource& resource);
	void UnindexAtlasPage(AtlasState& state, const AtlasCacheKey& key);
	AtlasProfileKey MakeAtlasProfileKey(const AtlasCacheKey& key);
	void TrimAtlasCache(AtlasState& state);
	void TrimAtlasCacheForIncomingBytes(AtlasState& state, size_t incomingBytes);
	void PruneRetiredAtlasGenerations();
	void TrimTextArtifactCache(AtlasState& state);
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
	bool ResolvePrewarmAtlasKey(const FontConfig& config,
		VectorFontByteClass byteClass, float rasterScale, AtlasCacheKey& key);
	bool IsPrewarmAtlasAlias(const FontConfig& config,
		VectorFontByteClass byteClass);
	RuntimeFont* GetPrewarmAtlasRuntime(RuntimeFont& runtime,
		const AtlasCacheKey& key);

	void InitializeDefaultPoolAtlasLifecycle();
	void PumpDefaultPoolAtlasLifecycle();
	void ShutdownDefaultPoolAtlasLifecycle();
}

#pragma once

#include "font_vector.h"

#include <array>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fonthook::vectorfont
{
	struct NativeA8PayloadTemplate;
	enum class FreeTypePerfCounter : UInt8
	{
		BitmapMemoryHit,
		BitmapCrossFontHit,
		BitmapDiskHit,
		BitmapDiskMiss,
		BitmapDiskWrite,
		BitmapDiskReadBytes,
		BitmapDiskWriteBytes,
		BitmapRasterized,
		BitmapBatchRequest,
		BitmapBatchDedupe,
		PreparedTextHit,
		PreparedTextMiss,
		AtlasHit,
		AtlasCreated,
		AtlasGrown,
		AtlasUpload,
		AtlasUploadBytes,
		AtlasUploadRect,
		TextArtifactHit,
		TextArtifactMiss,
		ShaderEffectBatch,
		CpuEffectMasksAvoided,
		GpuResidentGlyphHit,
		GpuResidentGlyphMiss,
		AtlasSnapshotProfileReuse,
		DynamicVertexUpload,
		DynamicVertexUploadBytes,
		DynamicVertexReuse,
		DynamicVertexDiscard,
		StaticVertexUpload,
		StaticVertexUploadBytes,
		StaticVertexHit,
		StaticVertexPromotionFailed,
		SortedStaticBatch,
		SortedStaticPayload,
		SortedStaticBytes,
		MergedPacketRange,
		Count,
	};

	void RecordFreeTypePerf(FreeTypePerfCounter aeCounter, UInt64 auiAmount = 1);
	void ReportFreeTypePerf();
	struct FaceConfig
	{
		// Separator-normalized XML value used for portable configuration hashes.
		std::wstring configuredPath;
		// Resolved filesystem path used only to open the font.
		std::wstring path;
		long faceIndex = 0;
	};

	struct ByteStyle
	{
		float pixelSize = 0.0f;
		float tracking = 0.0f;
		float scaleX = 1.0f;
		float scaleY = 1.0f;
		float embolden = 0.0f;
		float slantDegrees = 0.0f;
		float baselineOffset = 0.0f;
		float fixedWidth = 0.0f;
		std::vector<FaceConfig> faces;
	};

	enum class EffectColorMode : UInt8
	{
		Fixed = 0,
		Fill = 1,
	};

	struct EffectStyle
	{
		bool enabled = false;
		bool includeGlow = false;
		bool includeOutline = false;
		float width = 0.0f;
		float blur = 0.0f;
		float inner = 0.0f;
		float outer = 0.0f;
		float power = 2.0f;
		float softness = 0.5f;
		float x = 0.0f;
		float y = 0.0f;
		NiColorA color = { 0.0f, 0.0f, 0.0f, 1.0f };
		EffectColorMode colorMode = EffectColorMode::Fixed;
	};

	struct FontColorStyle
	{
		bool configured = false;
		NiColorA color = { 1.0f, 1.0f, 1.0f, 1.0f };
	};

	enum class EffectQuality : UInt8
	{
		Fast = 0,
		Balanced = 1,
		High = 2,
	};

	// Coverage revision 3 means every Windows-decodable unit in the active code
	// page. The former codepage mode used value 2 and covered only DCFGCF ranges,
	// so it must not satisfy the new completion contract. Zero remains the
	// in-progress manifest state.
	inline constexpr UInt8 kCompleteCodePagePrewarmIdentity = 3;

	enum class VerticalMetricsMode : UInt8
	{
		FreeType = 0,
		Original = 1,
	};

	enum class GlyphMaskType : UInt8
	{
		Fill = 0,
		Outline = 1,
		Glow = 2,
		DistanceField = 3,
	};

	struct FontConfig
	{
		UInt32 fontId = 0;
		std::array<ByteStyle, 2> styles;
		VerticalMetricsMode verticalMetrics = VerticalMetricsMode::FreeType;
		float baseline = 0.0f;
		FontColorStyle fontColor;
		EffectQuality effectQuality = EffectQuality::Balanced;
		EffectStyle glow;
		EffectStyle outline;
		EffectStyle shadow;
		UInt64 layoutHash = 0;
		UInt64 maskGenerationHash = 0;
		std::array<UInt64, 2> maskGenerationRoleHashes = {};
		UInt64 shaderEffectHash = 0;
	};

	struct GlyphBitmap
	{
		CpuMemoryLease cpuMemory;
		UInt64 cacheId = 0;
		UInt32 atlasRgb = 0x00FFFFFF;
		int width = 0;
		int height = 0;
		int left = 0;
		int top = 0;
		int effectiveWidth = 0;
		int effectiveHeight = 0;
		GlyphMaskType maskType = GlyphMaskType::Fill;
		UInt8 sdfSpread = 0;
		SInt32 strokeWidth26Dot6 = 0;
		bool colorBaked = false;
		UInt32 bakedRgba = 0;
		UInt8 bakedLayer = 0;
		std::vector<UInt8> alpha;
	};

	struct GlyphBitmapRequest
	{
		const VectorEncodedGlyph* glyph = nullptr;
		GlyphMaskType maskType = GlyphMaskType::Fill;
		UInt32 sdfSpread = 0;
	};

	// Layer IDs are part of the shader/native-packet ABI. Keep those IDs stable
	// while defining composition order independently: Shadow, Glow, Outline, Fill.
	inline constexpr UInt32 GetA8LayerDrawRank(UInt32 layer)
	{
		switch (layer)
		{
		case 0: return 0; // Shadow
		case 1: return 1; // Glow
		case 2: return 2; // Outline
		case 3: return 3; // Fill
		default: return 4;
		}
	}

	struct A8DrawRange
	{
		UInt32 firstVertex = 0;
		UInt32 vertexCount = 0;
		UInt32 startIndex = 0;
		UInt32 primitiveCount = 0;
		UInt32 layer = 3;
		UInt16 atlasPage = 0;
		bool usesSdf = false;
		bool usesLiveTileRgb = true;
		NiColorA layerColorModifier = { 1.0f, 1.0f, 1.0f, 1.0f };
	};

	struct A8EffectShapeConfig
	{
		bool enabled = false;
		bool shaderEffects = false;
		EffectQuality quality = EffectQuality::Balanced;
		float inverseAtlasWidth = 0.0f;
		float inverseAtlasHeight = 0.0f;
		float sdfSpreadPixels = 0.0f;
		float shadowBlurPixels = 0.0f;
		float shadowPower = 2.0f;
		float shadowGlowAlpha = 0.0f;
		float shadowOutlineAlpha = 0.0f;
		float glowInnerPixels = 0.0f;
		float glowOuterPixels = 0.0f;
		float glowPower = 2.0f;
		float outlineWidthPixels = 0.0f;
		float outlineSoftnessPixels = 0.5f;
		std::array<NiColorA, 4> layerColorModifiers = {{
			{ 1.0f, 1.0f, 1.0f, 1.0f },
			{ 1.0f, 1.0f, 1.0f, 1.0f },
			{ 1.0f, 1.0f, 1.0f, 1.0f },
			{ 1.0f, 1.0f, 1.0f, 1.0f }
		}};
		std::array<bool, 4> layerUsesLiveTileRgb = {{ true, true, true, true }};
		// Retain every page property for the lifetime of the shape. This is
		// required by the DEFAULT-pool reset/retirement path; page 0 is already a
		// shape property, but secondary page textures otherwise have no property
		// reference that the reset tracker can observe.
		std::vector<NiTexturingPropertyPtr> atlasProperties;
		std::vector<NiTexturePtr> atlasTextures;
		std::vector<NiPoint2> atlasInverseSizes;
		std::vector<A8DrawRange> ranges;
	};

	struct A8ShapeColorContract
	{
		// COLOR0 carries only the per-glyph base modifier. Packet c1 carries the
		// layer modifier, while c2.z selects whether fixed effects ignore both the
		// base and live Tile RGB. Every path continues to inherit live Tile alpha.
		static constexpr UInt32 kTileUniformColorAbi = 9;

		UInt32 abiVersion = kTileUniformColorAbi;
		NiColorA minimumModifier = { 1.0f, 1.0f, 1.0f, 1.0f };
		NiColorA maximumModifier = { 1.0f, 1.0f, 1.0f, 1.0f };
	};

	struct AtlasGlyphInstance
	{
		VectorEncodedGlyph glyph;
		NiPoint3 pen;
		NiColorA color;
	};

	enum class GlyphAtlasBuildOutcome : UInt8
	{
		Unknown = 0,
		Created,
		EmptyInput,
		NoDrawableShaderQuads,
		NoDrawableCpuQuads,
		CpuMaskBuildFailure,
		QuadLimit,
		AtlasOrShapeFailure,
	};

	enum class GlyphAtlasMaskFailure : UInt8
	{
		None = 0,
		Fill,
		Glow,
		Outline,
	};

	struct GlyphAtlasBuildDiagnostics
	{
		GlyphAtlasBuildOutcome outcome = GlyphAtlasBuildOutcome::Unknown;
		GlyphAtlasMaskFailure cpuMaskFailure = GlyphAtlasMaskFailure::None;
		UInt32 inputGlyphCount = 0;
		UInt32 missingMetricsCount = 0;
		UInt32 zeroByteLengthCount = 0;
		UInt32 controlGlyphCount = 0;
		UInt32 spaceGlyphCount = 0;
		UInt32 shaderQuadCount = 0;
		UInt32 cpuQuadCount = 0;
		UInt32 cpuAttempts = 0;
		UInt32 degradedLayerCount = 0;
		UInt32 shaderShapeAttempts = 0;
		UInt32 cpuShapeAttempts = 0;
		UInt32 firstEncodedCode = 0;
		UInt32 firstCodePoint = 0;
		UInt32 firstGlyphIndex = 0;
		UInt8 firstByteLength = 0;
		UInt8 firstByteClass = 0;
		UInt8 requestedQuality = 0;
		UInt8 resolvedQuality = 0;
		bool expectedEmpty = false;
		bool wantsShaderPath = false;
		bool hasEffects = false;
		bool requestsSdfFill = false;
		bool a8RendererAvailable = false;
		bool shaderQuadsBuilt = false;
		bool shaderAtlasOrShapeFailed = false;
		bool cpuQuadsBuilt = false;
	};

	struct RuntimeFont;

	extern std::unordered_map<UInt32, FontConfig> g_configs;

	const FontConfig* FindConfig(UInt32 auiFontId);
	RuntimeFont* FindRuntimeFont(UInt32 auiFontId);
	RuntimeFont* FindActiveRuntime(const Font* apFont);
	RuntimeFont* EnsureRuntimeFont(UInt32 auiFontId);
	bool ApplyRuntimeMetrics(RuntimeFont& arRuntime, Font& arFont);
	FontLetter* EnsureDoubleByteMetrics(RuntimeFont& arRuntime, Font& arFont, UInt32 auiEncodedCode);
	bool DecodeEncodedGlyph(RuntimeFont& arRuntime, Font& arFont, const char* apText, VectorEncodedGlyph& arGlyph);
	const FontConfig& GetRuntimeConfig(const RuntimeFont& arRuntime);
	UInt64 GetRuntimeMaskContentHash(RuntimeFont& arRuntime,
		VectorFontByteClass aeByteClass);
	bool GetFreeTypeFontCacheDirectory(std::wstring& arDirectory);
	void MarkFreeTypeFontCacheFileUsed(const std::wstring& arPath);
	void DeleteUnusedFreeTypeFontCacheFiles();
	bool HasCompleteGlyphManifest(RuntimeFont& arRuntime);
	void MarkGlyphManifestComplete(RuntimeFont& arRuntime);
	const std::vector<UInt16>& GetCompleteCodePageEncodedUnits();
	float GetGlyphBaselineOffset(const RuntimeFont& arRuntime,
		const VectorEncodedGlyph& arGlyph);
	void GetGlyphBitmaps(RuntimeFont& arRuntime,
		const std::vector<GlyphBitmapRequest>& arRequests, float afRasterScale,
		std::vector<std::shared_ptr<const GlyphBitmap>>& arResults);
	void ResolveGlyphBitmapCacheIds(RuntimeFont& arRuntime,
		const std::vector<GlyphBitmapRequest>& arRequests, float afRasterScale,
		std::vector<UInt64>& arCacheIds);
	void GetPrewarmGlyphBitmaps(RuntimeFont& arRuntime,
		const std::vector<GlyphBitmapRequest>& arRequests, float afRasterScale,
		std::vector<std::shared_ptr<const GlyphBitmap>>& arResults);
	void StoreGlyphCollisionProfile(RuntimeFont& arRuntime,
		const VectorEncodedGlyph& arGlyph, const GlyphBitmap& arBitmap,
		float afRasterScale);
	void FlushGlyphBitmapDiskCache();
	UInt64 ReleaseGlyphBitmapDiskCacheMappings();
	bool ResetPersistentFontCachesForRegeneration(RuntimeFont& arRuntime);
	bool DeleteCompleteCodePageGlyphBitmapDiskCaches(
		const std::vector<UInt32>& arFontIds);
	void SetBitmapCacheReducedAfterPrewarm(bool abReduced);
	bool HardShadowIncludesGlow(const FontConfig& arConfig);
	bool HardShadowIncludesOutline(const FontConfig& arConfig);
	bool HasSdfEffects(const FontConfig& arConfig);
	bool ResolveSdfSpread(const FontConfig& arConfig, float afRasterScale, UInt32& arSpread,
		bool abIncludeEffects = true);
	bool ResolvePrewarmGlyph(RuntimeFont& arRuntime, const char* apBytes,
		size_t auiLength, VectorEncodedGlyph& arGlyph);
	bool PrewarmGlyphAtlas(RuntimeFont& arRuntime,
		VectorFontByteClass aeByteClass,
		const std::vector<std::shared_ptr<const GlyphBitmap>>& arBitmaps,
		float afRasterScale);
	bool TryLoadGlyphAtlasSnapshot(RuntimeFont& arRuntime, float afRasterScale);
	bool TryLoadGloballyRepackedGlyphAtlasSnapshot(RuntimeFont& arRuntime,
		float afRasterScale);
	bool EnsureGloballyRepackedGlyphAtlasSnapshot(RuntimeFont& arRuntime,
		float afRasterScale, bool* apRepacked = nullptr);
	bool DiscardGlyphAtlasSnapshot(RuntimeFont& arRuntime, float afRasterScale);
	bool SaveGlyphAtlasSnapshot(RuntimeFont& arRuntime, float afRasterScale);
	bool RebuildGlyphAtlasFromSnapshot(RuntimeFont& arRuntime, float afRasterScale);
	void QueueFontPrewarm(UInt32 auiFontId);
	void PumpFontPrewarm();
	NiTriShape* TryCreateGlyphAtlasShape(Font& arFont, RuntimeFont& arRuntime,
		const std::vector<AtlasGlyphInstance>& arGlyphs, float afRasterScale,
		bool abPrepareObject, const NiColorA& arTileColor, bool abSuppressEffects,
		GlyphAtlasBuildDiagnostics* apDiagnostics = nullptr);
	bool IsA8RendererAvailable();
	bool ResolveA8EffectQuality(EffectQuality aeRequested, EffectQuality& arResolved);
	bool PrepareA8AtlasShape(Font& arFont, NiTriShape* apShape, UInt32 auiFontId,
		UInt32 auiGlyphCount, UInt32 auiQuadCount,
		const A8EffectShapeConfig* apEffectConfig = nullptr,
		const A8ShapeColorContract* apColorContract = nullptr,
		std::shared_ptr<const NativeA8PayloadTemplate> apPayloadTemplate = {},
		const NiPoint3& arGeometryOrigin = NiPoint3());
}

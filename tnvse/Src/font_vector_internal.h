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
	enum class FreeTypePerfCounter : UInt8
	{
		LayoutHit,
		LayoutMiss,
		HarfBuzzShape,
		KerningHit,
		KerningMiss,
		BitmapMemoryHit,
		BitmapRasterized,
		AtlasHit,
		AtlasCreated,
		AtlasGrown,
		AtlasUpload,
		AtlasUploadBytes,
		BatchHit,
		BatchMiss,
		ShaderEffectBatch,
		ShaderEffectPass,
		ShaderEffectSamples,
		CpuEffectMasksAvoided,
		Count,
	};

	void RecordFreeTypePerf(FreeTypePerfCounter aeCounter, UInt64 auiAmount = 1);
	void ReportFreeTypePerf();
	struct FaceConfig
	{
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

	struct EffectStyle
	{
		bool enabled = false;
		float width = 0.0f;
		float blur = 0.0f;
		float inner = 0.0f;
		float outer = 0.0f;
		float power = 2.0f;
		float softness = 0.5f;
		float x = 0.0f;
		float y = 0.0f;
		NiColorA color = { 0.0f, 0.0f, 0.0f, 1.0f };
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

	enum class FontPrewarmMode : UInt8
	{
		None = 0,
		Common = 1,
		CodePage = 2,
	};

	enum class VerticalMetricsMode : UInt8
	{
		FreeType = 0,
		Original = 1,
	};

	enum class GlyphMeshType : UInt8
	{
		Fill = 0,
		Outline = 1,
		Glow = 2,
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
		FontPrewarmMode prewarm = FontPrewarmMode::None;
		VerticalMetricsMode verticalMetrics = VerticalMetricsMode::FreeType;
		bool shaping = false;
		std::vector<std::string> shapingFeatures;
		float baseline = 0.0f;
		float curveTolerance = 0.35f;
		FontColorStyle fontColor;
		EffectQuality effectQuality = EffectQuality::Balanced;
		EffectStyle glow;
		EffectStyle outline;
		EffectStyle shadow;
		UInt64 styleHash = 0;
	};

	struct MeshPoint
	{
		float x = 0.0f;
		float y = 0.0f;
	};

	struct GlyphMesh
	{
		std::vector<MeshPoint> vertices;
		std::vector<UInt32> indices;
	};

	struct GlyphBitmap
	{
		UInt64 cacheId = 0;
		UInt32 atlasRgb = 0x00FFFFFF;
		int width = 0;
		int height = 0;
		int left = 0;
		int top = 0;
		int effectiveWidth = 0;
		int effectiveHeight = 0;
		float baselineOffset = 0.0f;
		std::vector<UInt8> alpha;
	};

	struct A8DrawRange
	{
		UInt32 firstVertex = 0;
		UInt32 vertexCount = 0;
		UInt32 startIndex = 0;
		UInt32 primitiveCount = 0;
		UInt32 layer = 3;
		NiColorA colorModifier = { 1.0f, 1.0f, 1.0f, 1.0f };
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
		float glowInnerPixels = 0.0f;
		float glowOuterPixels = 0.0f;
		float glowPower = 2.0f;
		float outlineWidthPixels = 0.0f;
		float outlineSoftnessPixels = 0.5f;
		std::vector<A8DrawRange> ranges;
	};

	struct A8ShapeColorContract
	{
		static constexpr UInt32 kTileUniformColorAbi = 4;

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

	struct RuntimeFont;

	extern std::unordered_map<UInt32, FontConfig> g_configs;

	const FontConfig* FindConfig(UInt32 auiFontId);
	RuntimeFont* FindRuntimeFont(UInt32 auiFontId);
	RuntimeFont* EnsureRuntimeFont(UInt32 auiFontId);
	bool ApplyRuntimeMetrics(RuntimeFont& arRuntime, Font& arFont);
	FontLetter* EnsureDoubleByteMetrics(RuntimeFont& arRuntime, Font& arFont, UInt32 auiEncodedCode);
	bool DecodeEncodedGlyph(RuntimeFont& arRuntime, Font& arFont, const char* apText, VectorEncodedGlyph& arGlyph);
	const FontConfig& GetRuntimeConfig(const RuntimeFont& arRuntime);
	std::shared_ptr<const GlyphMesh> GetGlyphMesh(RuntimeFont& arRuntime,
		const VectorEncodedGlyph& arGlyph, GlyphMeshType aeMeshType);
	std::shared_ptr<const GlyphBitmap> GetGlyphBitmap(RuntimeFont& arRuntime,
		const VectorEncodedGlyph& arGlyph, GlyphMaskType aeMaskType, float afRasterScale,
		UInt32 auiSdfSpread = 0);
	bool HasSdfEffects(const FontConfig& arConfig);
	bool ResolveSdfSpread(const FontConfig& arConfig, float afRasterScale, UInt32& arSpread);
	bool ResolvePrewarmGlyph(RuntimeFont& arRuntime, const char* apBytes,
		size_t auiLength, VectorEncodedGlyph& arGlyph);
	bool PrewarmGlyphAtlas(RuntimeFont& arRuntime,
		const std::vector<std::shared_ptr<const GlyphBitmap>>& arBitmaps,
		float afRasterScale);
	void QueueFontPrewarm(UInt32 auiFontId);
	void PumpFontPrewarm();
	NiTriShape* TryCreateGlyphAtlasShape(Font& arFont, RuntimeFont& arRuntime,
		const std::vector<AtlasGlyphInstance>& arGlyphs, float afRasterScale,
		bool abPrepareObject, const NiColorA& arTileColor);
	bool IsA8RendererAvailable();
	bool IsA8EffectRendererAvailable(EffectQuality aeQuality);
	bool ResolveA8EffectQuality(EffectQuality aeRequested, EffectQuality& arResolved);
	bool PrepareA8AtlasShape(NiTriShape* apShape, UInt32 auiFontId,
		UInt32 auiGlyphCount, UInt32 auiQuadCount,
		const A8EffectShapeConfig* apEffectConfig = nullptr,
		const A8ShapeColorContract* apColorContract = nullptr);
}

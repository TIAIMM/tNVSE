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
		std::vector<FaceConfig> faces;
	};

	struct EffectStyle
	{
		bool enabled = false;
		float width = 0.0f;
		float x = 0.0f;
		float y = 0.0f;
		NiColorA color = { 0.0f, 0.0f, 0.0f, 1.0f };
	};

	struct FontColorStyle
	{
		bool configured = false;
		NiColorA color = { 1.0f, 1.0f, 1.0f, 1.0f };
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
	};

	struct FontConfig
	{
		UInt32 fontId = 0;
		std::array<ByteStyle, 2> styles;
		bool shaping = false;
		std::vector<std::string> shapingFeatures;
		float baseline = 0.0f;
		float curveTolerance = 0.35f;
		FontColorStyle fontColor;
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
		int width = 0;
		int height = 0;
		int left = 0;
		int top = 0;
		int effectiveWidth = 0;
		int effectiveHeight = 0;
		float baselineOffset = 0.0f;
		std::vector<UInt8> alpha;
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
		const VectorEncodedGlyph& arGlyph, GlyphMaskType aeMaskType, float afRasterScale);
	NiTriShape* TryCreateGlyphAtlasShape(Font& arFont, RuntimeFont& arRuntime,
		const std::vector<AtlasGlyphInstance>& arGlyphs, float afRasterScale,
		bool abPrepareObject);
}

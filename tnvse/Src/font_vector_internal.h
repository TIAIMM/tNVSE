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

	struct FontConfig
	{
		UInt32 fontId = 0;
		std::array<ByteStyle, 2> styles;
		float lineHeight = 0.0f;
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
}

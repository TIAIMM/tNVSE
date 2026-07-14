sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);
float4 LayerColor : register(c1);
float4 AtlasPass : register(c2); // invWidth, invHeight, layer, SDF spread
float4 SdfFlags : register(c4); // x: current range uses an SDF mask

#include "freetype_tile_compat.hlsli"
#include "freetype_sdf_compat.hlsli"

struct PixelInput
{
	float2 uv : TEXCOORD0;
};

float4 Main(PixelInput input) : COLOR0
{
	const float coverage = ResolveFreeTypeBodyCoverage(FontAtlas, input.uv,
		AtlasPass.w, SdfFlags.x);
	return ComposeFreeTypeTileColor(coverage, TileColor, LayerColor);
}

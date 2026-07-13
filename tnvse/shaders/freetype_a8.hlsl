sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);
float4 LayerColor : register(c1);

#include "freetype_tile_compat.hlsli"

struct PixelInput
{
	float2 uv : TEXCOORD0;
};

float4 Main(PixelInput input) : COLOR0
{
	const float coverage = tex2D(FontAtlas, input.uv).a;
	return ComposeFreeTypeTileColor(coverage, TileColor, LayerColor);
}

sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);

#include "freetype_native_common.hlsli"

float4 Main(NativeFontPixelInput input) : COLOR0
{
	// ARGB and palette atlases already contain the final layer color. Applying
	// the packet vertex color again would square it and diverge from stock Tile.
	return tex2D(FontAtlas, input.atlasUv) * TileColor;
}

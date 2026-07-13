#ifndef TNVSE_FREETYPE_TILE_COMPAT_HLSLI
#define TNVSE_FREETYPE_TILE_COMPAT_HLSLI

// Uniform color ABI shared by every tNVSE FreeType shader.
// c0 is the original Tile color installed by the game. c1 is the font/effect
// layer modifier supplied by tNVSE. RGB remains straight (not premultiplied).
float4 ComposeFreeTypeTileColor(float coverage, float4 tileColor,
	float4 layerColor)
{
	return float4(tileColor.rgb * layerColor.rgb,
		saturate(coverage * tileColor.a * layerColor.a));
}

#endif

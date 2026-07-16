sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);

#include "freetype_native_common.hlsli"

float4 Main(NativeFontPixelInput input) : COLOR0
{
	return ComposeNativeFontCoverage(
		SampleNativeFontMask(FontAtlas, input.atlasUv), TileColor,
		NativeFontLayerColor(input));
}

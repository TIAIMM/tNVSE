sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);
float4 AtlasPass : register(c2); // invWidth, invHeight, layer, MTSDF spread

#include "freetype_native_common.hlsli"

float4 Main(NativeFontPixelInput input) : COLOR0
{
	const float4 mtsdf = SampleNativeFontMtsdf(FontAtlas, input.atlasUv);
	const float rgbDistance = DecodeNativeFontMtsdfDistance(
		MedianNativeFontMtsdf(mtsdf.rgb), AtlasPass.w);
	const float alphaDistance = DecodeNativeFontMtsdfDistance(
		mtsdf.a, AtlasPass.w);
	const float antialiasWidth =
		NativeFontMtsdfAntialiasWidth(alphaDistance);
	const float coverage = NativeFontMtsdfBody(rgbDistance, antialiasWidth);
	return ComposeNativeFontCoverage(coverage, TileColor, input.baseColor,
		NativeFontUsesLiveTileRgb(AtlasPass.z));
}

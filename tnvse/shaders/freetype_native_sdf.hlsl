sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);
float4 AtlasPass : register(c2); // invWidth, invHeight, layer, SDF spread

#include "freetype_native_common.hlsli"

float4 Main(NativeFontPixelInput input) : COLOR0
{
	const float sampled = SampleNativeFontMask(FontAtlas, input.atlasUv);
	const float distance = DecodeNativeFontSdf(sampled, AtlasPass.w);
	const float coverage = NativeFontSdfBody(distance,
		NativeFontSdfAntialiasWidth(distance));
	return ComposeNativeFontCoverage(coverage, TileColor, input.baseColor,
		NativeFontUsesLiveTileRgb(AtlasPass.z));
}

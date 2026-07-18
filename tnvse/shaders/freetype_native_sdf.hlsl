sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);
float4 AtlasPass : register(c2); // invWidth, invHeight, layer, SDF spread
float4 SdfFlags : register(c4);  // x: packet uses an SDF mask

#include "freetype_native_common.hlsli"

float4 Main(NativeFontPixelInput input) : COLOR0
{
	const float sampled = SampleNativeFontMask(FontAtlas, input.atlasUv);
	float coverage = sampled;
	if (SdfFlags.x >= 0.5)
	{
		const float distance = DecodeNativeFontSdf(sampled, AtlasPass.w);
		coverage = NativeFontSdfBody(distance,
			NativeFontSdfAntialiasWidth(distance));
	}
	return ComposeNativeFontCoverage(coverage, TileColor, input.baseColor,
		NativeFontUsesBaseRgb(AtlasPass.z));
}

#ifndef TNVSE_FREETYPE_NATIVE_COMMON_HLSLI
#define TNVSE_FREETYPE_NATIVE_COMMON_HLSLI

float4 LayerColor : register(c1);

struct NativeFontPixelInput
{
	float2 atlasUv : TEXCOORD0;
	float4 baseColor : COLOR0;
};

float NativeFontUsesLiveTileRgb(float layerAndFlags)
{
	return frac(layerAndFlags) < 0.125 ? 1.0 : 0.0;
}

float4 ComposeNativeFontCoverage(float coverage, float4 tileColor,
	float4 baseColor, float usesLiveTileRgb)
{
	// c0 is owned by the stock TileShader constant map. Fixed-color effects
	// select RGB identity here instead of rewriting c0 behind Gamebryo's back.
	const float3 identityRgb = float3(1.0, 1.0, 1.0);
	const float3 resolvedTileRgb = lerp(identityRgb, tileColor.rgb,
		usesLiveTileRgb);
	const float3 resolvedBaseRgb = lerp(identityRgb, baseColor.rgb,
		usesLiveTileRgb);
	return float4(resolvedTileRgb * resolvedBaseRgb * LayerColor.rgb,
		saturate(coverage * tileColor.a * baseColor.a * LayerColor.a));
}

float SampleNativeFontMask(sampler2D atlas, float2 uv)
{
	return tex2D(atlas, uv).a;
}

float DecodeNativeFontSdf(float encodedDistance, float spread)
{
	// FreeType's zero-distance contour is byte 128, rather than exactly 0.5.
	return (encodedDistance * (255.0 / 128.0) - 1.0) * spread;
}

float NativeFontSdfAntialiasWidth(float distance)
{
	return max(0.35, 0.5 * (abs(ddx(distance)) + abs(ddy(distance))));
}

float NativeFontSdfBody(float distance, float antialiasWidth)
{
	return smoothstep(-antialiasWidth, antialiasWidth, distance);
}

#endif

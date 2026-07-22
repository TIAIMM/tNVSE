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

float4 SampleNativeFontMtsdf(sampler2D atlas, float2 uv)
{
	return tex2D(atlas, uv);
}

float MedianNativeFontMtsdf(float3 value)
{
	return max(min(value.r, value.g), min(max(value.r, value.g), value.b));
}

float DecodeNativeFontMtsdfDistance(float encodedDistance, float spread)
{
	// The generator maps the symmetric [-spread, +spread] range to [0, 1].
	return (encodedDistance - 0.5) * (2.0 * spread);
}

float NativeFontMtsdfAntialiasWidth(float alphaDistance)
{
	// Alpha is the continuous true SDF. Its Euclidean derivative remains stable
	// where the RGB median changes channel at sharp corners.
	const float2 gradient = float2(ddx(alphaDistance), ddy(alphaDistance));
	return max(0.0001, 0.5 * length(gradient));
}

float NativeFontMtsdfBody(float rgbDistance, float antialiasWidth)
{
	// msdfgen-style one-screen-pixel reconstruction. RGB median owns body
	// topology; Alpha contributes only the stable screen-space footprint.
	return saturate(0.5 + rgbDistance
		/ max(2.0 * antialiasWidth, 0.0002));
}

#endif

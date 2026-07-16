#ifndef TNVSE_FREETYPE_NATIVE_COMMON_HLSLI
#define TNVSE_FREETYPE_NATIVE_COMMON_HLSLI

struct NativeFontPixelInput
{
	float2 atlasUv : TEXCOORD0;
	float4 layerColor : COLOR0;
};

float4 NativeFontLayerColor(NativeFontPixelInput input)
{
	return input.layerColor;
}

float4 ComposeNativeFontCoverage(float coverage, float4 tileColor,
	float4 layerColor)
{
	return float4(tileColor.rgb * layerColor.rgb,
		saturate(coverage * tileColor.a * layerColor.a));
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

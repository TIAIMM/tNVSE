#ifndef FILL_QUALITY
#define FILL_QUALITY 1
#endif

sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);
float4 AtlasPass : register(c2); // invWidth, invHeight, layer, MTSDF spread

#include "freetype_native_common.hlsli"

float EvaluateNativeFontMtsdfFillAt(float2 uv, float antialiasWidth)
{
	const float4 distanceSample = SampleNativeFontMtsdf(FontAtlas, uv);
	const float distance = DecodeNativeFontSelectedDistance(
		NativeFontBodyEncodedDistance(distanceSample), AtlasPass.w);
	return NativeFontMtsdfBody(distance, antialiasWidth);
}

float ResolveNativeFontMtsdfAntialiasWidth(float2 derivativeUv)
{
	const float screenPxRange = NativeFontMtsdfScreenPxRange(
		derivativeUv, AtlasPass.xy, AtlasPass.w);
	return NativeFontMtsdfAntialiasWidth(
		screenPxRange, AtlasPass.w);
}

float SupersampledNativeFontMtsdfFill(float2 uv, float antialiasWidth)
{
#if FILL_QUALITY == 0
	return EvaluateNativeFontMtsdfFillAt(uv, antialiasWidth);
#elif FILL_QUALITY == 1
	const float2 quarter = AtlasPass.xy * 0.25;
	float sum = 0.0;
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + float2(-quarter.x, -quarter.y), antialiasWidth);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + float2( quarter.x, -quarter.y), antialiasWidth);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + float2(-quarter.x,  quarter.y), antialiasWidth);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + float2( quarter.x,  quarter.y), antialiasWidth);
	return sum * 0.25;
#else
	const float2 texel = AtlasPass.xy;
	float sum = 0.0;
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + texel * float2(-0.375, -0.125), antialiasWidth);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + texel * float2(-0.125,  0.375), antialiasWidth);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + texel * float2( 0.125, -0.375), antialiasWidth);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + texel * float2( 0.375,  0.125), antialiasWidth);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + texel * float2(-0.375,  0.375), antialiasWidth);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + texel * float2( 0.375, -0.375), antialiasWidth);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + texel * float2(-0.125, -0.125), antialiasWidth);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + texel * float2( 0.125,  0.125), antialiasWidth);
	return sum * 0.125;
#endif
}

float4 Main(NativeFontPixelInput input) : COLOR0
{
	const float antialiasWidth =
		ResolveNativeFontMtsdfAntialiasWidth(input.atlasUv);
	const float coverage =
		SupersampledNativeFontMtsdfFill(input.atlasUv, antialiasWidth);
	return ComposeNativeFontCoverage(coverage, TileColor, input.baseColor,
		NativeFontUsesLiveTileRgb(AtlasPass.z));
}

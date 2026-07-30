#ifndef FILL_QUALITY
#define FILL_QUALITY 1
#endif

sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);
float4 AtlasPass : register(c177); // invWidth, invHeight, layer, raster scale

#include "freetype_native_common.hlsli"

float EvaluateNativeFontMtsdfFillAt(float2 uv, float antialiasWidth,
	float spread)
{
	const float4 distanceSample = SampleNativeFontMtsdf(FontAtlas, uv);
	const float distance = DecodeNativeFontSelectedDistance(
		NativeFontBodyEncodedDistance(distanceSample), spread);
	return NativeFontMtsdfBody(distance, antialiasWidth);
}

float SupersampledNativeFontMtsdfFill(float2 uv, float antialiasWidth,
	float spread)
{
#if FILL_QUALITY == 0
	return EvaluateNativeFontMtsdfFillAt(uv, antialiasWidth, spread);
#elif FILL_QUALITY == 1
	const float2 quarter = AtlasPass.xy * 0.25;
	float sum = 0.0;
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + float2(-quarter.x, -quarter.y), antialiasWidth, spread);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + float2( quarter.x, -quarter.y), antialiasWidth, spread);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + float2(-quarter.x,  quarter.y), antialiasWidth, spread);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + float2( quarter.x,  quarter.y), antialiasWidth, spread);
	return sum * 0.25;
#else
	const float2 texel = AtlasPass.xy;
	float sum = 0.0;
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + texel * float2(-0.375, -0.125), antialiasWidth, spread);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + texel * float2(-0.125,  0.375), antialiasWidth, spread);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + texel * float2( 0.125, -0.375), antialiasWidth, spread);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + texel * float2( 0.375,  0.125), antialiasWidth, spread);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + texel * float2(-0.375,  0.375), antialiasWidth, spread);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + texel * float2( 0.375, -0.375), antialiasWidth, spread);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + texel * float2(-0.125, -0.125), antialiasWidth, spread);
	sum += EvaluateNativeFontMtsdfFillAt(
		uv + texel * float2( 0.125,  0.125), antialiasWidth, spread);
	return sum * 0.125;
#endif
}

float4 Main(NativeFontPixelInput input) : COLOR0
{
	const float spread = max(input.glyphParams.x, 0.0001);
	const float antialiasWidth =
		ResolveNativeFontMtsdfAntialiasWidth(input, spread);
	const float coverage =
		SupersampledNativeFontMtsdfFill(
			input.atlasUv, antialiasWidth, spread);
	return ComposeNativeFontCoverage(coverage, TileColor, input.baseColor,
		NativeFontUsesLiveTileRgb(AtlasPass.z));
}

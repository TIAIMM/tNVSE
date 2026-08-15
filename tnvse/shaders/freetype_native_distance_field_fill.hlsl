#ifndef FILL_QUALITY
#define FILL_QUALITY 1
#endif

sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);
float4 AtlasPass : register(c177); // invWidth, invHeight, layer, raster scale

#include "freetype_native_common.hlsli"

float EvaluateNativeFontDistanceFieldFillAt(float2 uv, float antialiasWidth,
	float spread)
{
	const float4 distanceSample = SampleNativeFontDistanceField(FontAtlas, uv);
	const float distance = DecodeNativeFontSelectedDistance(
		NativeFontBodyEncodedDistance(distanceSample), spread);
	return NativeFontDistanceFieldBody(distance, antialiasWidth);
}

float SupersampledNativeFontDistanceFieldFill(float2 uv, float antialiasWidth,
	float spread)
{
#if FILL_QUALITY == 0
	return EvaluateNativeFontDistanceFieldFillAt(uv, antialiasWidth, spread);
#elif FILL_QUALITY == 1
	const float2 uvDx = ddx(uv);
	const float2 uvDy = ddy(uv);
	float sum = 0.0;
	sum += EvaluateNativeFontDistanceFieldFillAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2(-0.25, -0.25)), antialiasWidth, spread);
	sum += EvaluateNativeFontDistanceFieldFillAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2( 0.25, -0.25)), antialiasWidth, spread);
	sum += EvaluateNativeFontDistanceFieldFillAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2(-0.25,  0.25)), antialiasWidth, spread);
	sum += EvaluateNativeFontDistanceFieldFillAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2( 0.25,  0.25)), antialiasWidth, spread);
	return sum * 0.25;
#else
	const float2 uvDx = ddx(uv);
	const float2 uvDy = ddy(uv);
	float sum = 0.0;
	sum += EvaluateNativeFontDistanceFieldFillAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2(-0.375, -0.125)), antialiasWidth, spread);
	sum += EvaluateNativeFontDistanceFieldFillAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2(-0.125,  0.375)), antialiasWidth, spread);
	sum += EvaluateNativeFontDistanceFieldFillAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2( 0.125, -0.375)), antialiasWidth, spread);
	sum += EvaluateNativeFontDistanceFieldFillAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2( 0.375,  0.125)), antialiasWidth, spread);
	sum += EvaluateNativeFontDistanceFieldFillAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2(-0.375,  0.375)), antialiasWidth, spread);
	sum += EvaluateNativeFontDistanceFieldFillAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2( 0.375, -0.375)), antialiasWidth, spread);
	sum += EvaluateNativeFontDistanceFieldFillAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2(-0.125, -0.125)), antialiasWidth, spread);
	sum += EvaluateNativeFontDistanceFieldFillAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2( 0.125,  0.125)), antialiasWidth, spread);
	return sum * 0.125;
#endif
}

float4 Main(NativeFontPixelInput input) : COLOR0
{
	const float spread = max(input.glyphParams.x, 0.0001);
	const float antialiasWidth =
		ResolveNativeFontDistanceFieldAntialiasWidth(input, spread);
	const float coverage =
		SupersampledNativeFontDistanceFieldFill(
			input.atlasUv, antialiasWidth, spread);
	return ComposeNativeFontCoverage(coverage, TileColor, input.baseColor,
		NativeFontUsesLiveTileRgb(AtlasPass.z));
}

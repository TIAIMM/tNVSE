#ifndef FILL_QUALITY
#define FILL_QUALITY 1
#endif

sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);
float4 AtlasPass : register(c177); // invWidth, invHeight, layer, raster scale

#include "freetype_native_common.hlsli"

float EvaluateNativeFontDistanceFieldFillSample(float4 distanceSample,
	float antialiasWidth, float spread)
{
	const float distance = DecodeNativeFontSelectedDistance(
		NativeFontBodyEncodedDistance(distanceSample), spread);
	return NativeFontDistanceFieldBody(distance, antialiasWidth);
}

float EvaluateNativeFontDistanceFieldFillAt(float2 uv, float antialiasWidth,
	float spread)
{
	return EvaluateNativeFontDistanceFieldFillSample(
		SampleNativeFontDistanceField(FontAtlas, uv), antialiasWidth, spread);
}

float SupersampledNativeFontDistanceFieldFill(float2 uv,
	float analyticAntialiasWidth, float spread)
{
#if FILL_QUALITY == 0
	const float4 primarySample =
		SampleNativeFontDistanceField(FontAtlas, uv);
	const float antialiasWidth =
		ResolveNativeFontDistanceGradientAntialiasWidth(
			NativeFontDistanceFieldGradientDistance(primarySample, spread),
			analyticAntialiasWidth, spread);
	return EvaluateNativeFontDistanceFieldFillSample(
		primarySample, antialiasWidth, spread);
#elif FILL_QUALITY == 1
	const float2 uvDx = ddx(uv);
	const float2 uvDy = ddy(uv);
	const float2 primaryUv = uv + NativeFontScreenSubpixelOffset(
		uvDx, uvDy, float2(-0.25, -0.25));
	const float2 oppositeUv = uv + NativeFontScreenSubpixelOffset(
		uvDx, uvDy, float2( 0.25,  0.25));
	const float4 primarySample =
		SampleNativeFontDistanceField(FontAtlas, primaryUv);
	const float4 oppositeSample =
		SampleNativeFontDistanceField(FontAtlas, oppositeUv);
	// The symmetric pair reconstructs the center distance for a locally linear
	// field, so the derivative remains centered without adding another tap.
	const float gradientDistance = 0.5 * (
		NativeFontDistanceFieldGradientDistance(primarySample, spread)
		+ NativeFontDistanceFieldGradientDistance(oppositeSample, spread));
	const float antialiasWidth =
		ResolveNativeFontDistanceGradientAntialiasWidth(
			gradientDistance, analyticAntialiasWidth, spread);
	float sum = EvaluateNativeFontDistanceFieldFillSample(
		primarySample, antialiasWidth, spread)
		+ EvaluateNativeFontDistanceFieldFillSample(
			oppositeSample, antialiasWidth, spread);
	sum += EvaluateNativeFontDistanceFieldFillAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2( 0.25, -0.25)), antialiasWidth, spread);
	sum += EvaluateNativeFontDistanceFieldFillAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2(-0.25,  0.25)), antialiasWidth, spread);
	return sum * 0.25;
#else
	const float2 uvDx = ddx(uv);
	const float2 uvDy = ddy(uv);
	const float2 primaryUv = uv + NativeFontScreenSubpixelOffset(
		uvDx, uvDy, float2(-0.125, -0.125));
	const float2 oppositeUv = uv + NativeFontScreenSubpixelOffset(
		uvDx, uvDy, float2( 0.125,  0.125));
	const float4 primarySample =
		SampleNativeFontDistanceField(FontAtlas, primaryUv);
	const float4 oppositeSample =
		SampleNativeFontDistanceField(FontAtlas, oppositeUv);
	const float gradientDistance = 0.5 * (
		NativeFontDistanceFieldGradientDistance(primarySample, spread)
		+ NativeFontDistanceFieldGradientDistance(oppositeSample, spread));
	const float antialiasWidth =
		ResolveNativeFontDistanceGradientAntialiasWidth(
			gradientDistance, analyticAntialiasWidth, spread);
	float sum = EvaluateNativeFontDistanceFieldFillSample(
		primarySample, antialiasWidth, spread)
		+ EvaluateNativeFontDistanceFieldFillSample(
			oppositeSample, antialiasWidth, spread);
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
	return sum * 0.125;
#endif
}

float4 Main(NativeFontPixelInput input) : COLOR0
{
	const float spread = max(input.glyphParams.x, 0.0001);
	const float analyticAntialiasWidth =
		ResolveNativeFontDistanceFieldAntialiasWidth(input, spread);
	const float coverage =
		SupersampledNativeFontDistanceFieldFill(
			input.atlasUv, analyticAntialiasWidth, spread);
	return ComposeNativeFontCoverage(coverage, TileColor, input.baseColor,
		NativeFontUsesLiveTileRgb(AtlasPass.z));
}

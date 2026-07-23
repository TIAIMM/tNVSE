#ifndef FILL_QUALITY
#define FILL_QUALITY 1
#endif

#ifndef FILL_SUBPIXEL
#define FILL_SUBPIXEL 0
#endif

sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);
float4 AtlasPass : register(c2); // invWidth, invHeight, layer, MTSDF spread
#if FILL_SUBPIXEL
float4 SubpixelPass : register(c4); // x: channel offset, y: chroma strength
#endif

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
#if FILL_SUBPIXEL
	// ddx maps one horizontal pixel of the active render target into atlas UV.
	// Red and Blue evaluate the physical +/-1/3 channel position. Green uses
	// the ordinary center Fill shader. One extra center sample gives each side
	// pass a stable luminance reference without repeating its quality sample
	// set. A symmetric, edge-aware delta limit suppresses colored outer halos
	// and MTSDF corner outliers while retaining most vertical-stem resolution.
	const float2 sampleUv =
		input.atlasUv + ddx(input.atlasUv) * SubpixelPass.x;
	const float shiftedCoverage =
		SupersampledNativeFontMtsdfFill(sampleUv, antialiasWidth);
	const float centerCoverage =
		EvaluateNativeFontMtsdfFillAt(input.atlasUv, antialiasWidth);
	const float edgeProximity =
		1.0 - abs(centerCoverage * 2.0 - 1.0);
	const float maxChromaDelta =
		lerp(0.125, 0.25, edgeProximity);
	const float limitedDelta = clamp(
		shiftedCoverage - centerCoverage,
		-maxChromaDelta, maxChromaDelta);
	const float coverage = saturate(centerCoverage
		+ limitedDelta * saturate(SubpixelPass.y));
#else
	const float coverage =
		SupersampledNativeFontMtsdfFill(input.atlasUv, antialiasWidth);
#endif
	return ComposeNativeFontCoverage(coverage, TileColor, input.baseColor,
		NativeFontUsesLiveTileRgb(AtlasPass.z));
}

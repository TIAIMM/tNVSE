#ifndef TNVSE_FREETYPE_NATIVE_COMMON_HLSLI
#define TNVSE_FREETYPE_NATIVE_COMMON_HLSLI

float4 LayerColor : register(c176);

struct NativeFontPixelInput
{
	float2 atlasUv : TEXCOORD0;
	float4 baseColor : COLOR0;
	// x: source SDF spread, y: 1/sourceToLogicalScale,
	// z: exact Shadow/Glow/Outline/Fill bit mask.
	float3 glyphParams : TEXCOORD1;
	// Exact u0/v0/u1/v1 source rectangle. Composite geometry may extrapolate
	// atlasUv to include an offset shadow, but samples remain glyph-local.
	float4 glyphBounds : TEXCOORD2;
	// Analytic source-distance footprint computed once per vertex from the
	// vanilla Tile WVP matrix, current viewport and packet raster scale.
	float antialiasWidth : TEXCOORD3;
};

float NativeFontUsesLiveTileRgb(float layerAndFlags)
{
	return frac(layerAndFlags) < 0.125 ? 1.0 : 0.0;
}

float4 ComposeNativeFontCoverage(float coverage, float4 tileColor,
	float4 baseColor, float usesLiveTileRgb)
{
	// c0 is owned by the vanilla TileShader constant map. Fixed-color effects
	// select RGB identity here instead of rewriting c0 behind Gamebryo's back.
	const float3 identityRgb = float3(1.0, 1.0, 1.0);
	const float3 resolvedTileRgb = lerp(identityRgb, tileColor.rgb,
		usesLiveTileRgb);
	const float3 resolvedBaseRgb = lerp(identityRgb, baseColor.rgb,
		usesLiveTileRgb);
	return float4(resolvedTileRgb * resolvedBaseRgb * LayerColor.rgb,
		saturate(coverage * tileColor.a * baseColor.a * LayerColor.a));
}

#ifndef NATIVE_FONT_EXPLICIT_LOD
#define NATIVE_FONT_EXPLICIT_LOD 0
#endif

float4 SampleNativeFontDistanceField(sampler2D atlas, float2 uv)
{
#if NATIVE_FONT_EXPLICIT_LOD
	// Native distance-field pages are sealed level-zero-only atlases. Explicit
	// LOD permits the composite shader to put every fetch behind its
	// transparent-union branch.
	return tex2Dlod(atlas, float4(uv, 0.0, 0.0));
#else
	return tex2D(atlas, uv);
#endif
}

float2 NativeFontScreenSubpixelOffset(float2 uvDx, float2 uvDy,
	float2 screenOffset)
{
	// uvDx/uvDy are the atlas-UV footprint of one screen pixel. Combining the
	// complete vectors preserves rotation, shear and non-uniform scaling; using
	// AtlasPass.xy component-wise only describes an axis-aligned atlas texel.
	return uvDx * screenOffset.x + uvDy * screenOffset.y;
}

float MedianNativeFontMtsdf(float3 value)
{
	return max(min(value.r, value.g), min(max(value.r, value.g), value.b));
}

float DecodeNativeFontMtsdfDistance(float encodedDistance, float spread)
{
	return (encodedDistance - 0.5) * (2.0 * spread);
}

#ifndef DISTANCE_FIELD_TRUE_SDF
#define DISTANCE_FIELD_TRUE_SDF 0
#endif

float NativeFontBodyEncodedDistance(float4 distanceSample)
{
#if DISTANCE_FIELD_TRUE_SDF
	return distanceSample.a;
#else
	return MedianNativeFontMtsdf(distanceSample.rgb);
#endif
}

float DecodeNativeFontSelectedDistance(float encodedDistance, float spread)
{
#if DISTANCE_FIELD_TRUE_SDF
	// The quantized true-SDF contour is byte 128. Preserve that exact zero.
	return (encodedDistance * (255.0 / 128.0) - 1.0) * spread;
#else
	return DecodeNativeFontMtsdfDistance(encodedDistance, spread);
#endif
}

float NativeFontDistanceFieldGradientDistance(float4 distanceSample,
	float spread)
{
	// MTSDF stores the smooth true signed distance in alpha while RGB retains
	// the multi-channel contour used for sharp body corners. True-SDF pages also
	// store their only distance in alpha, so this is the stable derivative source
	// for both native distance-field modes.
	return DecodeNativeFontSelectedDistance(distanceSample.a, spread);
}

float ResolveNativeFontDistanceFieldAntialiasWidth(
	NativeFontPixelInput input, float spread)
{
	// All native layouts receive an affine screen footprint from the vertex
	// shader. It remains the fallback when a sampled distance derivative is
	// degenerate, including fully saturated distance-field regions.
	return min(max(input.antialiasWidth, 0.0001), spread);
}

float ResolveNativeFontDistanceGradientAntialiasWidth(
	float gradientDistance, float analyticAntialiasWidth, float spread)
{
	// fwidth is the complete source-distance footprint of a square screen pixel.
	// NativeFontDistanceFieldBody consumes a half-width, hence the 0.5 factor.
	// This makes diagonal contours wider by their actual projected footprint
	// without another atlas fetch. The analytic vertex width is retained only as
	// a fail-safe for a zero derivative away from (or at a saturated edge of) the
	// representable distance range.
	const float derivativeAntialiasWidth = 0.5 * fwidth(gradientDistance);
	const float useDerivative = step(0.0001, derivativeAntialiasWidth);
	const float resolvedAntialiasWidth = lerp(analyticAntialiasWidth,
		derivativeAntialiasWidth, useDerivative);
	return min(max(resolvedAntialiasWidth, 0.0001), spread);
}

float NativeFontDistanceFieldBody(float bodyDistance, float antialiasWidth)
{
	// Distance-field coverage reconstruction is linear across one screen pixel.
	return saturate(0.5 + bodyDistance
		/ max(2.0 * antialiasWidth, 0.0002));
}

#endif

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

float4 SampleNativeFontMtsdf(sampler2D atlas, float2 uv)
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

float ResolveNativeFontMtsdfAntialiasWidth(
	NativeFontPixelInput input, float spread)
{
	// All native layouts receive the affine screen footprint from the vertex
	// shader. Keeping derivatives out of the pixel path makes AA cost independent
	// of covered pixel count while preserving the same clamped source distance.
	return min(max(input.antialiasWidth, 0.0001), spread);
}

float NativeFontMtsdfBody(float rgbDistance, float antialiasWidth)
{
	// The official median reconstruction is linear across one screen pixel.
	return saturate(0.5 + rgbDistance
		/ max(2.0 * antialiasWidth, 0.0002));
}

#endif

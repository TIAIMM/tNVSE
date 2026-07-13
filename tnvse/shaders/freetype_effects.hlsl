sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);
float4 LayerColor : register(c1);
float4 AtlasPass : register(c2); // invWidth, invHeight, pass, unused
float4 EffectParams : register(c3); // radius in device pixels

#include "freetype_tile_compat.hlsli"

struct PixelInput
{
	float2 uv : TEXCOORD0;
};

float Coverage(float2 uv)
{
	return tex2D(FontAtlas, uv).a;
}

float SampleOffset(float2 uv, float2 offset)
{
	return Coverage(uv + offset * EffectParams.x * AtlasPass.xy);
}

void Ring8(float2 uv, float scale, inout float maximum, inout float sum)
{
	const float d = 0.70710678 * scale;
	const float s = scale;
	float value = SampleOffset(uv, float2( s, 0)); maximum = max(maximum, value); sum += value;
	value = SampleOffset(uv, float2(-s, 0)); maximum = max(maximum, value); sum += value;
	value = SampleOffset(uv, float2(0,  s)); maximum = max(maximum, value); sum += value;
	value = SampleOffset(uv, float2(0, -s)); maximum = max(maximum, value); sum += value;
	value = SampleOffset(uv, float2( d,  d)); maximum = max(maximum, value); sum += value;
	value = SampleOffset(uv, float2(-d,  d)); maximum = max(maximum, value); sum += value;
	value = SampleOffset(uv, float2( d, -d)); maximum = max(maximum, value); sum += value;
	value = SampleOffset(uv, float2(-d, -d)); maximum = max(maximum, value); sum += value;
}

void Ring8Rotated(float2 uv, float scale, inout float maximum, inout float sum)
{
	const float a = 0.92387953 * scale;
	const float b = 0.38268343 * scale;
	float value = SampleOffset(uv, float2( a,  b)); maximum = max(maximum, value); sum += value;
	value = SampleOffset(uv, float2(-a,  b)); maximum = max(maximum, value); sum += value;
	value = SampleOffset(uv, float2( a, -b)); maximum = max(maximum, value); sum += value;
	value = SampleOffset(uv, float2(-a, -b)); maximum = max(maximum, value); sum += value;
	value = SampleOffset(uv, float2( b,  a)); maximum = max(maximum, value); sum += value;
	value = SampleOffset(uv, float2(-b,  a)); maximum = max(maximum, value); sum += value;
	value = SampleOffset(uv, float2( b, -a)); maximum = max(maximum, value); sum += value;
	value = SampleOffset(uv, float2(-b, -a)); maximum = max(maximum, value); sum += value;
}

float OutlineCoverage(float2 uv, float center)
{
	float maximum = center;
	float unused = 0.0;
	Ring8(uv, 1.0, maximum, unused);
#if EFFECT_QUALITY >= 1
	Ring8(uv, 0.5, maximum, unused);
#endif
#if EFFECT_QUALITY >= 2
	Ring8Rotated(uv, 1.0, maximum, unused);
#endif
	return saturate(maximum - center);
}

float GlowCoverage(float2 uv, float center)
{
	float maximum = center;
	float sum = 0.0;
	Ring8(uv, 1.0, maximum, sum);
#if EFFECT_QUALITY == 0
	float blurred = (center * 4.0 + sum) / 12.0;
#elif EFFECT_QUALITY == 1
	float outer = 0.0;
	Ring8Rotated(uv, 1.0, maximum, outer);
	float blurred = (center * 6.0 + sum + outer) / 22.0;
#else
	float outer = 0.0;
	float inner = 0.0;
	Ring8Rotated(uv, 1.0, maximum, outer);
	Ring8(uv, 0.5, maximum, inner);
	float blurred = (center * 8.0 + sum + outer + inner * 2.0) / 40.0;
#endif
	return saturate(max(blurred, maximum * 0.35) - center);
}

float ShadowCoverage(float2 uv, float center)
{
	if (EffectParams.x <= 0.001)
		return center;
	float sum = 0.0;
	float value = SampleOffset(uv, float2( 1, 0)); sum += value;
	value = SampleOffset(uv, float2(-1, 0)); sum += value;
	value = SampleOffset(uv, float2(0,  1)); sum += value;
	value = SampleOffset(uv, float2(0, -1)); sum += value;
#if EFFECT_QUALITY >= 1
	const float d = 0.70710678;
	value = SampleOffset(uv, float2( d,  d)); sum += value;
	value = SampleOffset(uv, float2(-d,  d)); sum += value;
	value = SampleOffset(uv, float2( d, -d)); sum += value;
	value = SampleOffset(uv, float2(-d, -d)); sum += value;
#endif
#if EFFECT_QUALITY >= 2
	value = SampleOffset(uv, float2( 0.5, 0)); sum += value;
	value = SampleOffset(uv, float2(-0.5, 0)); sum += value;
	value = SampleOffset(uv, float2(0,  0.5)); sum += value;
	value = SampleOffset(uv, float2(0, -0.5)); sum += value;
#endif
#if EFFECT_QUALITY == 0
	return saturate((center * 2.0 + sum) / 6.0);
#elif EFFECT_QUALITY == 1
	return saturate((center * 4.0 + sum) / 12.0);
#else
	return saturate((center * 6.0 + sum) / 18.0);
#endif
}

float4 Main(PixelInput input) : COLOR0
{
	const float center = Coverage(input.uv);
	const int layer = (int)(AtlasPass.z + 0.5);
	float coverage = center;
	if (layer == 0)
		coverage = ShadowCoverage(input.uv, center);
	else if (layer == 1)
		coverage = GlowCoverage(input.uv, center);
	else if (layer == 2)
		coverage = OutlineCoverage(input.uv, center);
	return ComposeFreeTypeTileColor(coverage, TileColor, LayerColor);
}

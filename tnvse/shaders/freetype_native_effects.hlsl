#ifndef EFFECT_QUALITY
#define EFFECT_QUALITY 1
#endif

sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);
float4 AtlasPass : register(c2);    // invWidth, invHeight, layer, SDF spread
float4 EffectParams : register(c3); // layer-specific device-pixel parameters
float4 SdfFlags : register(c4);     // x: packet uses an SDF mask

#include "freetype_native_common.hlsli"

float EvaluateNativeFontEffect(float distance, float antialiasWidth, int layer)
{
	const float body = NativeFontSdfBody(distance, antialiasWidth);
	if (layer == 0)
	{
		const float blur = max(EffectParams.x, 0.0);
		if (blur <= 0.001)
			return body;
		const float power = max(EffectParams.y, 0.0001);
		const float blurred = smoothstep(-blur - antialiasWidth,
			blur + antialiasWidth, distance);
		return pow(saturate(blurred), power);
	}
	if (layer == 1)
	{
		const float inner = max(EffectParams.x, 0.0);
		const float outer = max(EffectParams.y, inner + 0.0001);
		const float power = max(EffectParams.z, 0.0001);
		const float outsideDistance = max(-distance, 0.0);
		const float fade = outsideDistance <= inner
			? 1.0
			: pow(saturate((outer - outsideDistance) / (outer - inner)), power);
		return saturate((1.0 - body) * fade);
	}
	if (layer == 2)
	{
		const float width = max(EffectParams.x, 0.0);
		const float softness = max(EffectParams.y, 0.0);
		const float expanded = smoothstep(-width - softness - antialiasWidth,
			-width + antialiasWidth, distance);
		return saturate(expanded - body);
	}
	return body;
}

float EvaluateNativeFontEffectAt(float2 uv, float antialiasWidth, int layer)
{
	return EvaluateNativeFontEffect(DecodeNativeFontSdf(
		SampleNativeFontMask(FontAtlas, uv), AtlasPass.w), antialiasWidth, layer);
}

float SupersampledNativeFontEffect(float2 uv, int layer)
{
	const float centerDistance = DecodeNativeFontSdf(
		SampleNativeFontMask(FontAtlas, uv), AtlasPass.w);
	const float antialiasWidth = NativeFontSdfAntialiasWidth(centerDistance);
#if EFFECT_QUALITY == 0
	return EvaluateNativeFontEffect(centerDistance, antialiasWidth, layer);
#elif EFFECT_QUALITY == 1
	const float2 quarter = AtlasPass.xy * 0.25;
	float sum = 0.0;
	sum += EvaluateNativeFontEffectAt(uv + float2(-quarter.x, -quarter.y), antialiasWidth, layer);
	sum += EvaluateNativeFontEffectAt(uv + float2( quarter.x, -quarter.y), antialiasWidth, layer);
	sum += EvaluateNativeFontEffectAt(uv + float2(-quarter.x,  quarter.y), antialiasWidth, layer);
	sum += EvaluateNativeFontEffectAt(uv + float2( quarter.x,  quarter.y), antialiasWidth, layer);
	return sum * 0.25;
#else
	const float2 texel = AtlasPass.xy;
	float sum = 0.0;
	sum += EvaluateNativeFontEffectAt(uv + texel * float2(-0.375, -0.125), antialiasWidth, layer);
	sum += EvaluateNativeFontEffectAt(uv + texel * float2(-0.125,  0.375), antialiasWidth, layer);
	sum += EvaluateNativeFontEffectAt(uv + texel * float2( 0.125, -0.375), antialiasWidth, layer);
	sum += EvaluateNativeFontEffectAt(uv + texel * float2( 0.375,  0.125), antialiasWidth, layer);
	sum += EvaluateNativeFontEffectAt(uv + texel * float2(-0.375,  0.375), antialiasWidth, layer);
	sum += EvaluateNativeFontEffectAt(uv + texel * float2( 0.375, -0.375), antialiasWidth, layer);
	sum += EvaluateNativeFontEffectAt(uv + texel * float2(-0.125, -0.125), antialiasWidth, layer);
	sum += EvaluateNativeFontEffectAt(uv + texel * float2( 0.125,  0.125), antialiasWidth, layer);
	return sum * 0.125;
#endif
}

float4 Main(NativeFontPixelInput input) : COLOR0
{
	const int layer = (int)(AtlasPass.z + 0.5);
	float coverage;
	if (SdfFlags.x >= 0.5)
		coverage = SupersampledNativeFontEffect(input.atlasUv, layer);
	else
		coverage = SampleNativeFontMask(FontAtlas, input.atlasUv);
	return ComposeNativeFontCoverage(coverage, TileColor,
		NativeFontLayerColor(input));
}

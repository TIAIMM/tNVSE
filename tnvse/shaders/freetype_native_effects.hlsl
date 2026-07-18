#ifndef EFFECT_QUALITY
#define EFFECT_QUALITY 1
#endif

sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);
float4 AtlasPass : register(c2);    // invWidth, invHeight, layer, SDF spread
float4 EffectParams : register(c3); // layer-specific device-pixel parameters
float4 SdfFlags : register(c4);     // x: SDF; hard shadow y: outline softness,
                                    // z/w: copied glow/outline alpha

#include "freetype_native_common.hlsli"

float NativeFontVanillaGlowFalloff(float normalizedDistance, float power)
{
	// The baked vanilla glow atlases use an approximately exponential alpha
	// tail.  With the default power of 2 this preserves the old shader's 0.25
	// midpoint while retaining a low-intensity tail near the configured radius.
	return exp2(-2.0 * power * saturate(normalizedDistance));
}

float EvaluateNativeFontGlowMask(float distance, float antialiasWidth,
	float body, float inner, float outer, float power)
{
	const float outsideDistance = max(-distance, 0.0);
	const float normalizedDistance = (outsideDistance - inner) / (outer - inner);
	const float fade = outsideDistance <= inner
		? 1.0
		: NativeFontVanillaGlowFalloff(normalizedDistance, power);
	return saturate((1.0 - body) * fade);
}

float EvaluateNativeFontOutlineMask(float distance, float antialiasWidth,
	float body, float width, float softness)
{
	const float expanded = smoothstep(-width - softness - antialiasWidth,
		-width + antialiasWidth, distance);
	return saturate(expanded - body);
}

float NativeFontGlowOuterFeather(float centerDistance, float antialiasWidth,
	float outer)
{
	const float outsideDistance = max(-centerDistance, 0.0);
	return 1.0 - smoothstep(
		outer - antialiasWidth, outer + antialiasWidth, outsideDistance);
}

float ApplyNativeFontHardShadowComposite(float distance, float antialiasWidth,
	float body)
{
	float glow = 0.0;
	if (SdfFlags.z > 0.0)
	{
		const float inner = max(EffectParams.x, 0.0);
		const float outer = max(EffectParams.y, inner + 0.0001);
		const float power = max(EffectParams.z, 0.0001);
		glow = EvaluateNativeFontGlowMask(distance, antialiasWidth,
			body, inner, outer, power);
		glow *= NativeFontGlowOuterFeather(distance, antialiasWidth, outer)
			* saturate(SdfFlags.z);
	}

	float outline = 0.0;
	if (SdfFlags.w > 0.0)
	{
		const float width = max(EffectParams.w, 0.0);
		const float softness = max(SdfFlags.y, 0.0);
		outline = EvaluateNativeFontOutlineMask(distance, antialiasWidth,
			body, width, softness) * saturate(SdfFlags.w);
	}

	// Reproduce the source text stack in one shadow-colored mask. Source-over
	// union avoids over-darkening where the copied glow and outline overlap.
	const float outside = outline + (1.0 - outline) * glow;
	return saturate(body + (1.0 - body) * outside);
}

float EvaluateNativeFontEffect(float distance, float antialiasWidth, int layer)
{
	const float body = NativeFontSdfBody(distance, antialiasWidth);
	if (layer == 0)
	{
		if (SdfFlags.z > 0.0 || SdfFlags.w > 0.0)
			return body;
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
		return EvaluateNativeFontGlowMask(distance, antialiasWidth,
			body, inner, outer, power);
	}
	if (layer == 2)
	{
		const float width = max(EffectParams.x, 0.0);
		const float softness = max(EffectParams.y, 0.0);
		return EvaluateNativeFontOutlineMask(distance, antialiasWidth,
			body, width, softness);
	}
	return body;
}

float ApplyNativeFontGlowOuterFeather(float coverage, float centerDistance,
	float antialiasWidth, int layer)
{
	if (layer != 1)
		return coverage;
	const float outer = max(EffectParams.y, EffectParams.x + 0.0001);
	return coverage * NativeFontGlowOuterFeather(
		centerDistance, antialiasWidth, outer);
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
	float coverage;
#if EFFECT_QUALITY == 0
	coverage = EvaluateNativeFontEffect(centerDistance, antialiasWidth, layer);
#elif EFFECT_QUALITY == 1
	const float2 quarter = AtlasPass.xy * 0.25;
	float sum = 0.0;
	sum += EvaluateNativeFontEffectAt(uv + float2(-quarter.x, -quarter.y), antialiasWidth, layer);
	sum += EvaluateNativeFontEffectAt(uv + float2( quarter.x, -quarter.y), antialiasWidth, layer);
	sum += EvaluateNativeFontEffectAt(uv + float2(-quarter.x,  quarter.y), antialiasWidth, layer);
	sum += EvaluateNativeFontEffectAt(uv + float2( quarter.x,  quarter.y), antialiasWidth, layer);
	coverage = sum * 0.25;
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
	coverage = sum * 0.125;
#endif
	if (layer == 0 && (SdfFlags.z > 0.0 || SdfFlags.w > 0.0))
	{
		coverage = ApplyNativeFontHardShadowComposite(
			centerDistance, antialiasWidth, coverage);
	}
	return ApplyNativeFontGlowOuterFeather(
		coverage, centerDistance, antialiasWidth, layer);
}

float4 Main(NativeFontPixelInput input) : COLOR0
{
	const int layer = (int)floor(AtlasPass.z);
	float coverage;
	if (SdfFlags.x >= 0.5)
		coverage = SupersampledNativeFontEffect(input.atlasUv, layer);
	else
		coverage = SampleNativeFontMask(FontAtlas, input.atlasUv);
	return ComposeNativeFontCoverage(coverage, TileColor, input.baseColor,
		NativeFontUsesBaseRgb(AtlasPass.z));
}

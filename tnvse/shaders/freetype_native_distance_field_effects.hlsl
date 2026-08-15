#ifndef EFFECT_QUALITY
#define EFFECT_QUALITY 1
#endif

sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);
float4 AtlasPass : register(c177);    // invWidth, invHeight, layer, raster scale
float4 EffectParams : register(c178); // layer-specific source-pixel parameters
float4 EffectFlags : register(c179);  // hard shadow y: outline softness,
                                      // z/w: copied glow/outline alpha

#include "freetype_native_common.hlsli"

float NativeFontVanillaGlowFalloff(float normalizedDistance, float power)
{
	return exp2(-2.0 * power * saturate(normalizedDistance));
}

float EvaluateNativeFontGlowMask(float alphaDistance, float antialiasWidth,
	float bodyCoverage, float inner, float outer, float power)
{
	const float outsideDistance = max(-alphaDistance, 0.0);
	const float normalizedDistance = (outsideDistance - inner) / (outer - inner);
	const float fade = outsideDistance <= inner
		? 1.0
		: NativeFontVanillaGlowFalloff(normalizedDistance, power);
	return saturate((1.0 - bodyCoverage) * fade);
}

float EvaluateNativeFontOutlineMask(float alphaDistance, float antialiasWidth,
	float bodyCoverage, float width, float softness)
{
	const float proxyAntialiasWidth = antialiasWidth + max(softness, 0.0);
	const float vuiProxy = smoothstep(-max(width, 0.0) - proxyAntialiasWidth,
		proxyAntialiasWidth, alphaDistance);
	return saturate(max(bodyCoverage, vuiProxy));
}

float NativeFontGlowOuterFeather(float alphaDistance, float antialiasWidth,
	float outer)
{
	const float outsideDistance = max(-alphaDistance, 0.0);
	return 1.0 - smoothstep(
		outer - antialiasWidth, outer + antialiasWidth, outsideDistance);
}

float ApplyNativeFontHardShadowComposite(float alphaDistance,
	float antialiasWidth, float bodyCoverage, float distanceScale)
{
	float glow = 0.0;
	if (EffectFlags.z > 0.0)
	{
		const float inner = max(EffectParams.x * distanceScale, 0.0);
		const float outer = max(EffectParams.y * distanceScale,
			inner + 0.0001);
		const float power = max(EffectParams.z, 0.0001);
		glow = EvaluateNativeFontGlowMask(alphaDistance, antialiasWidth,
			bodyCoverage, inner, outer, power);
		glow *= NativeFontGlowOuterFeather(alphaDistance, antialiasWidth, outer)
			* saturate(EffectFlags.z);
	}

	float outline = 0.0;
	if (EffectFlags.w > 0.0)
	{
		const float width = max(EffectParams.w * distanceScale, 0.0);
		const float softness = max(EffectFlags.y * distanceScale, 0.0);
		outline = EvaluateNativeFontOutlineMask(alphaDistance, antialiasWidth,
			bodyCoverage, width, softness) * saturate(EffectFlags.w);
	}
	const float outside = outline + (1.0 - outline) * glow;
	return saturate(bodyCoverage + (1.0 - bodyCoverage) * outside);
}

float EvaluateNativeFontEffect(float alphaDistance, float antialiasWidth,
	float bodyCoverage, int layer, float distanceScale)
{
	if (layer == 0)
	{
		if (EffectFlags.z > 0.0 || EffectFlags.w > 0.0)
			return bodyCoverage;
		const float blur = max(EffectParams.x * distanceScale, 0.0);
		if (blur <= 0.001)
			return bodyCoverage;
		const float power = max(EffectParams.y, 0.0001);
		const float blurred = smoothstep(-blur - antialiasWidth,
			blur + antialiasWidth, alphaDistance);
		return pow(saturate(blurred), power);
	}
	if (layer == 1)
	{
		const float inner = max(EffectParams.x * distanceScale, 0.0);
		const float outer = max(EffectParams.y * distanceScale,
			inner + 0.0001);
		const float power = max(EffectParams.z, 0.0001);
		return EvaluateNativeFontGlowMask(alphaDistance, antialiasWidth,
			bodyCoverage, inner, outer, power);
	}
	if (layer == 2)
	{
		const float width = max(EffectParams.x * distanceScale, 0.0);
		const float softness = max(EffectParams.y * distanceScale, 0.0);
		return EvaluateNativeFontOutlineMask(alphaDistance, antialiasWidth,
			bodyCoverage, width, softness);
	}
	return bodyCoverage;
}

float EvaluateNativeFontEffectAt(float2 uv, float antialiasWidth, int layer,
	float distanceScale, float spread)
{
	const float4 distanceSample = SampleNativeFontDistanceField(FontAtlas, uv);
	const float alphaDistance = DecodeNativeFontSelectedDistance(
		distanceSample.a, spread);
	const float bodyDistance = DecodeNativeFontSelectedDistance(
		NativeFontBodyEncodedDistance(distanceSample), spread);
	const float bodyCoverage = NativeFontDistanceFieldBody(
		bodyDistance, antialiasWidth);
	return EvaluateNativeFontEffect(
		alphaDistance, antialiasWidth, bodyCoverage, layer, distanceScale);
}

float SupersampledNativeFontEffect(float2 uv, int layer,
	float distanceScale, float spread, float antialiasWidth)
{
	const float4 center = SampleNativeFontDistanceField(FontAtlas, uv);
	const float centerAlphaDistance = DecodeNativeFontSelectedDistance(
		center.a, spread);
	const float centerBodyDistance = DecodeNativeFontSelectedDistance(
		NativeFontBodyEncodedDistance(center), spread);
	float coverage;
#if EFFECT_QUALITY == 0
	coverage = EvaluateNativeFontEffect(centerAlphaDistance, antialiasWidth,
		NativeFontDistanceFieldBody(centerBodyDistance, antialiasWidth), layer,
		distanceScale);
#elif EFFECT_QUALITY == 1
	const float2 uvDx = ddx(uv);
	const float2 uvDy = ddy(uv);
	float sum = 0.0;
	sum += EvaluateNativeFontEffectAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2(-0.25, -0.25)), antialiasWidth, layer,
		distanceScale, spread);
	sum += EvaluateNativeFontEffectAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2( 0.25, -0.25)), antialiasWidth, layer,
		distanceScale, spread);
	sum += EvaluateNativeFontEffectAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2(-0.25,  0.25)), antialiasWidth, layer,
		distanceScale, spread);
	sum += EvaluateNativeFontEffectAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2( 0.25,  0.25)), antialiasWidth, layer,
		distanceScale, spread);
	coverage = sum * 0.25;
#else
	const float2 uvDx = ddx(uv);
	const float2 uvDy = ddy(uv);
	float sum = 0.0;
	sum += EvaluateNativeFontEffectAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2(-0.375, -0.125)), antialiasWidth, layer,
		distanceScale, spread);
	sum += EvaluateNativeFontEffectAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2(-0.125,  0.375)), antialiasWidth, layer,
		distanceScale, spread);
	sum += EvaluateNativeFontEffectAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2( 0.125, -0.375)), antialiasWidth, layer,
		distanceScale, spread);
	sum += EvaluateNativeFontEffectAt(
		uv + NativeFontScreenSubpixelOffset(
			uvDx, uvDy, float2( 0.375,  0.125)), antialiasWidth, layer,
		distanceScale, spread);
	coverage = sum * 0.25;
#endif
	if (layer == 0 && (EffectFlags.z > 0.0 || EffectFlags.w > 0.0))
	{
		coverage = ApplyNativeFontHardShadowComposite(
			centerAlphaDistance, antialiasWidth, coverage, distanceScale);
	}
	if (layer == 1)
	{
		const float inner = EffectParams.x * distanceScale;
		const float outer = max(EffectParams.y * distanceScale,
			inner + 0.0001);
		coverage *= NativeFontGlowOuterFeather(
			centerAlphaDistance, antialiasWidth, outer);
	}
	return coverage;
}

float4 Main(NativeFontPixelInput input) : COLOR0
{
	const float distanceScale = max(input.glyphParams.y, 0.0001);
	const float spread = max(input.glyphParams.x, 0.0001);
	const int layer = (int)floor(AtlasPass.z);
	const float antialiasWidth =
		ResolveNativeFontDistanceFieldAntialiasWidth(input, spread);
	const float coverage = SupersampledNativeFontEffect(
		input.atlasUv, layer, distanceScale, spread, antialiasWidth);
	return ComposeNativeFontCoverage(coverage, TileColor, input.baseColor,
		NativeFontUsesLiveTileRgb(AtlasPass.z));
}

#ifndef COMPOSITE_QUALITY
#define COMPOSITE_QUALITY 1
#endif

sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);
float4 ShadowColor : register(c1);
float4 AtlasPass : register(c2);      // invWidth, invHeight, reserved
float4 EffectPrimary : register(c3);  // shadow blur/power, glow inner/outer
float4 EffectSecondary : register(c4);// glow power, outline width/softness,
                                      // hard-shadow glow alpha
float4 GlowColor : register(c5);
float4 OutlineColor : register(c6);
float4 FillColor : register(c7);
float4 CompositeFlags : register(c8);// hard-shadow outline alpha,
                                     // live-Tile-RGB layer mask,
                                     // shadow x/y in source pixels

#include "freetype_native_common.hlsli"

struct NativeCompositeSample
{
	float alphaDistance;
	float rgbBody;
	float valid;
};

NativeCompositeSample SampleNativeComposite(float2 uv, float4 glyphBounds,
	float spread, float antialiasWidth)
{
	const float2 boundedUv = clamp(uv, glyphBounds.xy, glyphBounds.zw);
	const float inside = step(glyphBounds.x, uv.x)
		* step(glyphBounds.y, uv.y)
		* step(uv.x, glyphBounds.z)
		* step(uv.y, glyphBounds.w);
	const float4 value = SampleNativeFontMtsdf(FontAtlas, boundedUv);
	NativeCompositeSample result;
	result.alphaDistance = lerp(-spread,
		DecodeNativeFontSelectedDistance(value.a, spread), inside);
	const float rgbDistance = DecodeNativeFontSelectedDistance(
		NativeFontBodyEncodedDistance(value), spread);
	result.rgbBody = NativeFontMtsdfBody(rgbDistance, antialiasWidth) * inside;
	result.valid = inside;
	return result;
}

float NativeCompositeGlowMask(float alphaDistance, float rgbBody,
	float antialiasWidth, float distanceScale)
{
	const float inner = max(EffectPrimary.z * distanceScale, 0.0);
	const float outer = max(EffectPrimary.w * distanceScale,
		inner + 0.0001);
	const float outsideDistance = max(-alphaDistance, 0.0);
	const float normalizedDistance =
		(outsideDistance - inner) / (outer - inner);
	const float fade = outsideDistance <= inner
		? 1.0
		: exp2(-2.0 * max(EffectSecondary.x, 0.0001)
			* saturate(normalizedDistance));
	return saturate((1.0 - rgbBody) * fade);
}

float NativeCompositeGlowFeather(float alphaDistance,
	float antialiasWidth, float distanceScale)
{
	const float inner = max(EffectPrimary.z * distanceScale, 0.0);
	const float outer = max(EffectPrimary.w * distanceScale,
		inner + 0.0001);
	const float outsideDistance = max(-alphaDistance, 0.0);
	return 1.0 - smoothstep(
		outer - antialiasWidth, outer + antialiasWidth, outsideDistance);
}

float NativeCompositeOutline(float alphaDistance, float rgbBody,
	float antialiasWidth, float distanceScale)
{
	const float width = max(EffectSecondary.y * distanceScale, 0.0);
	const float softness = max(EffectSecondary.z * distanceScale, 0.0);
	const float proxyAntialiasWidth = antialiasWidth + softness;
	const float proxy = smoothstep(-width - proxyAntialiasWidth,
		proxyAntialiasWidth, alphaDistance);
	return saturate(max(rgbBody, proxy));
}

float NativeCompositeShadowSample(float alphaDistance, float rgbBody,
	float antialiasWidth, float distanceScale)
{
	const bool hardComposite = EffectPrimary.x <= 0.001
		&& (EffectSecondary.w > 0.0 || CompositeFlags.x > 0.0);
	if (hardComposite)
		return rgbBody;
	const float blur = max(EffectPrimary.x * distanceScale, 0.0);
	if (blur <= 0.001)
		return rgbBody;
	const float blurred = smoothstep(-blur - antialiasWidth,
		blur + antialiasWidth, alphaDistance);
	return pow(saturate(blurred), max(EffectPrimary.y, 0.0001));
}

float ApplyNativeCompositeHardShadow(float alphaDistance, float rgbBody,
	float antialiasWidth, float distanceScale)
{
	const float glow = NativeCompositeGlowMask(alphaDistance, rgbBody,
		antialiasWidth, distanceScale)
		* NativeCompositeGlowFeather(alphaDistance, antialiasWidth,
			distanceScale)
		* saturate(EffectSecondary.w);
	const float outline = NativeCompositeOutline(alphaDistance, rgbBody,
		antialiasWidth, distanceScale) * saturate(CompositeFlags.x);
	const float outside = outline + (1.0 - outline) * glow;
	return saturate(rgbBody + (1.0 - rgbBody) * outside);
}

float4 ResolveNativeCompositeSource(float coverage, float4 layerColor,
	float4 baseColor, float usesLiveTileRgb)
{
	const float3 identityRgb = float3(1.0, 1.0, 1.0);
	const float3 tileRgb = lerp(identityRgb, TileColor.rgb, usesLiveTileRgb);
	const float3 baseRgb = lerp(identityRgb, baseColor.rgb, usesLiveTileRgb);
	return float4(tileRgb * baseRgb * layerColor.rgb,
		saturate(coverage * TileColor.a * baseColor.a * layerColor.a));
}

float NativeCompositeLayerUsesLiveRgb(int layer)
{
	const float divisor = exp2((float)layer);
	return fmod(floor(CompositeFlags.y / divisor), 2.0);
}

bool NativeCompositeHasLayer(int layerMask, int layerBit)
{
	return fmod(floor((float)layerMask / (float)layerBit), 2.0) > 0.5;
}

void ApplyNativeCompositeSource(inout float3 premultiplied,
	inout float alpha, float4 source)
{
	const float inverseSourceAlpha = 1.0 - source.a;
	premultiplied = source.rgb * source.a
		+ premultiplied * inverseSourceAlpha;
	alpha = source.a + alpha * inverseSourceAlpha;
}

float4 Main(NativeFontPixelInput input) : COLOR0
{
	const float spread = max(input.glyphParams.x, 0.0001);
	const float distanceScale = max(input.glyphParams.y, 0.0001);
	const int layerMask = (int)floor(input.glyphParams.z + 0.5);
	const bool hasShadow = NativeCompositeHasLayer(layerMask, 1);
	const float2 shadowSourceOffset =
		CompositeFlags.zw * input.glyphParams.y;
	const bool shiftedShadow = hasShadow
		&& (abs(shadowSourceOffset.x) + abs(shadowSourceOffset.y) > 0.0001);
	const float2 shadowUv = input.atlasUv
		- shadowSourceOffset * AtlasPass.xy;
	const float screenPxRange = NativeFontMtsdfScreenPxRange(
		input.atlasUv, AtlasPass.xy, spread);
	const float antialiasWidth = NativeFontMtsdfAntialiasWidth(
		screenPxRange, spread);
	const NativeCompositeSample center = SampleNativeComposite(
		input.atlasUv, input.glyphBounds, spread, antialiasWidth);
	NativeCompositeSample shadowCenter = center;
	if (shiftedShadow)
	{
		shadowCenter = SampleNativeComposite(shadowUv, input.glyphBounds,
			spread, antialiasWidth);
	}

	float fillCoverage = center.rgbBody;
	float shadowCoverage = NativeCompositeShadowSample(
		shadowCenter.alphaDistance, shadowCenter.rgbBody,
		antialiasWidth, distanceScale);
	float glowCoverage = NativeCompositeGlowMask(center.alphaDistance,
		center.rgbBody, antialiasWidth, distanceScale);
	float outlineCoverage = NativeCompositeOutline(center.alphaDistance,
		center.rgbBody, antialiasWidth, distanceScale);

#if COMPOSITE_QUALITY > 0
#if COMPOSITE_QUALITY == 1
	const float2 quarter = AtlasPass.xy * 0.25;
	const float2 effectOffsets[4] = {
		float2(-quarter.x, -quarter.y),
		float2( quarter.x, -quarter.y),
		float2(-quarter.x,  quarter.y),
		float2( quarter.x,  quarter.y)
	};
#else
	const float2 effectTexel = AtlasPass.xy;
	const float2 effectOffsets[4] = {
		effectTexel * float2(-0.375, -0.125),
		effectTexel * float2(-0.125,  0.375),
		effectTexel * float2( 0.125, -0.375),
		effectTexel * float2( 0.375,  0.125)
	};
#endif
	shadowCoverage = 0.0;
	glowCoverage = 0.0;
	outlineCoverage = 0.0;
	fillCoverage = 0.0;
	for (int tap = 0; tap < 4; ++tap)
	{
		const NativeCompositeSample sample = SampleNativeComposite(
			input.atlasUv + effectOffsets[tap], input.glyphBounds,
			spread, antialiasWidth);
		NativeCompositeSample shadowSample = sample;
		if (shiftedShadow)
		{
			shadowSample = SampleNativeComposite(
				shadowUv + effectOffsets[tap], input.glyphBounds,
				spread, antialiasWidth);
		}
		shadowCoverage += NativeCompositeShadowSample(
			shadowSample.alphaDistance, shadowSample.rgbBody,
			antialiasWidth, distanceScale);
		glowCoverage += NativeCompositeGlowMask(sample.alphaDistance,
			sample.rgbBody, antialiasWidth, distanceScale);
		outlineCoverage += NativeCompositeOutline(sample.alphaDistance,
			sample.rgbBody, antialiasWidth, distanceScale);
		fillCoverage += sample.rgbBody;
	}
	shadowCoverage *= 0.25;
	glowCoverage *= 0.25;
	outlineCoverage *= 0.25;
	fillCoverage *= 0.25;
#endif

	glowCoverage *= NativeCompositeGlowFeather(center.alphaDistance,
		antialiasWidth, distanceScale);
	if (EffectPrimary.x <= 0.001
		&& (EffectSecondary.w > 0.0 || CompositeFlags.x > 0.0))
	{
		shadowCoverage = ApplyNativeCompositeHardShadow(
			shadowCenter.alphaDistance, shadowCoverage,
			antialiasWidth, distanceScale);
	}

#if COMPOSITE_QUALITY > 1
	const float2 texel = AtlasPass.xy;
	const float2 extraOffsets[4] = {
		texel * float2(-0.375,  0.375),
		texel * float2( 0.375, -0.375),
		texel * float2(-0.125, -0.125),
		texel * float2( 0.125,  0.125)
	};
	float extraFill = 0.0;
	for (int extraTap = 0; extraTap < 4; ++extraTap)
	{
		extraFill += SampleNativeComposite(
			input.atlasUv + extraOffsets[extraTap],
			input.glyphBounds, spread, antialiasWidth).rgbBody;
	}
	fillCoverage = fillCoverage * 0.5 + extraFill * 0.125;
#endif

	float3 premultiplied = 0.0;
	float alpha = 0.0;
	if (NativeCompositeHasLayer(layerMask, 1))
	{
		ApplyNativeCompositeSource(premultiplied, alpha,
			ResolveNativeCompositeSource(shadowCoverage, ShadowColor,
				input.baseColor, NativeCompositeLayerUsesLiveRgb(0)));
	}
	if (NativeCompositeHasLayer(layerMask, 2))
	{
		ApplyNativeCompositeSource(premultiplied, alpha,
			ResolveNativeCompositeSource(glowCoverage, GlowColor,
				input.baseColor, NativeCompositeLayerUsesLiveRgb(1)));
	}
	if (NativeCompositeHasLayer(layerMask, 4))
	{
		ApplyNativeCompositeSource(premultiplied, alpha,
			ResolveNativeCompositeSource(outlineCoverage, OutlineColor,
				input.baseColor, NativeCompositeLayerUsesLiveRgb(2)));
	}
	if (NativeCompositeHasLayer(layerMask, 8))
	{
		ApplyNativeCompositeSource(premultiplied, alpha,
			ResolveNativeCompositeSource(fillCoverage, FillColor,
				input.baseColor, NativeCompositeLayerUsesLiveRgb(3)));
	}
	return alpha > 0.000001
		? float4(premultiplied / alpha, alpha)
		: float4(0.0, 0.0, 0.0, 0.0);
}

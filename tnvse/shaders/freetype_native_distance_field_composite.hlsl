#ifndef COMPOSITE_QUALITY
#define COMPOSITE_QUALITY 1
#endif
#ifndef COMPOSITE_STATIC_LAYER_MASK
// Zero retains the compatibility profile and reads the mask per glyph.
#define COMPOSITE_STATIC_LAYER_MASK 0
#endif
#ifndef COMPOSITE_STATIC_SHIFTED_SHADOW
// -1 retains the compatibility profile's uniform runtime decision.
#define COMPOSITE_STATIC_SHIFTED_SHADOW -1
#endif

sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);
float4 ShadowColor : register(c176);
float4 AtlasPass : register(c177);      // invWidth, invHeight, reserved, raster
float4 EffectPrimary : register(c178);  // shadow blur/power, glow inner/outer
float4 EffectSecondary : register(c179);// glow power, outline width/softness,
                                        // hard-shadow glow alpha
float4 GlowColor : register(c180);
float4 OutlineColor : register(c181);
float4 FillColor : register(c182);
float4 CompositeFlags : register(c183);// hard-shadow outline alpha,
                                       // live-Tile-RGB layer mask,
                                       // shadow x/y in source pixels

#if COMPOSITE_STATIC_SHIFTED_SHADOW == 0
#define NATIVE_FONT_EXPLICIT_LOD 0
#else
#define NATIVE_FONT_EXPLICIT_LOD 1
#endif
#include "freetype_native_common.hlsli"

struct NativeCompositeSample
{
	float alphaDistance;
	float bodyCoverage;
};

float NativeCompositeInsideGlyph(float2 uv, float4 glyphBounds)
{
	return step(glyphBounds.x, uv.x)
		* step(glyphBounds.y, uv.y)
		* step(uv.x, glyphBounds.z)
		* step(uv.y, glyphBounds.w);
}

NativeCompositeSample SampleNativeCompositeKnownInside(float2 uv,
	float4 glyphBounds, float spread, float antialiasWidth, float inside)
{
	const float2 boundedUv = clamp(uv, glyphBounds.xy, glyphBounds.zw);
	const float4 value = SampleNativeFontDistanceField(FontAtlas, boundedUv);
	NativeCompositeSample result;
	result.alphaDistance = lerp(-spread,
		DecodeNativeFontSelectedDistance(value.a, spread), inside);
	const float bodyDistance = DecodeNativeFontSelectedDistance(
		NativeFontBodyEncodedDistance(value), spread);
	result.bodyCoverage = NativeFontDistanceFieldBody(bodyDistance, antialiasWidth) * inside;
	return result;
}

NativeCompositeSample SampleNativeComposite(float2 uv, float4 glyphBounds,
	float spread, float antialiasWidth)
{
	return SampleNativeCompositeKnownInside(uv, glyphBounds, spread,
		antialiasWidth, NativeCompositeInsideGlyph(uv, glyphBounds));
}

float NativeCompositeGlowMask(float alphaDistance, float bodyCoverage,
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
	return saturate((1.0 - bodyCoverage) * fade);
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

float NativeCompositeOutline(float alphaDistance, float bodyCoverage,
	float antialiasWidth, float distanceScale)
{
	const float width = max(EffectSecondary.y * distanceScale, 0.0);
	const float softness = max(EffectSecondary.z * distanceScale, 0.0);
	const float proxyAntialiasWidth = antialiasWidth + softness;
	const float proxy = smoothstep(-width - proxyAntialiasWidth,
		proxyAntialiasWidth, alphaDistance);
	return saturate(max(bodyCoverage, proxy));
}

float NativeCompositeShadowSample(float alphaDistance, float bodyCoverage,
	float antialiasWidth, float distanceScale)
{
	const bool hardComposite = EffectPrimary.x <= 0.001
		&& (EffectSecondary.w > 0.0 || CompositeFlags.x > 0.0);
	if (hardComposite)
		return bodyCoverage;
	const float blur = max(EffectPrimary.x * distanceScale, 0.0);
	if (blur <= 0.001)
		return bodyCoverage;
	const float blurred = smoothstep(-blur - antialiasWidth,
		blur + antialiasWidth, alphaDistance);
	return pow(saturate(blurred), max(EffectPrimary.y, 0.0001));
}

float ApplyNativeCompositeHardShadow(float alphaDistance, float bodyCoverage,
	float antialiasWidth, float distanceScale)
{
	const float glow = NativeCompositeGlowMask(alphaDistance, bodyCoverage,
		antialiasWidth, distanceScale)
		* NativeCompositeGlowFeather(alphaDistance, antialiasWidth,
			distanceScale)
		* saturate(EffectSecondary.w);
	const float outline = NativeCompositeOutline(alphaDistance, bodyCoverage,
		antialiasWidth, distanceScale) * saturate(CompositeFlags.x);
	const float outside = outline + (1.0 - outline) * glow;
	return saturate(bodyCoverage + (1.0 - bodyCoverage) * outside);
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

float NativeCompositeDrawableInside(NativeFontPixelInput input)
{
#if COMPOSITE_STATIC_LAYER_MASK > 0
#if (COMPOSITE_STATIC_LAYER_MASK & 1)
	const bool hasShadow = true;
#else
	const bool hasShadow = false;
#endif
#else
	const int layerMask = (int)floor(input.glyphParams.z + 0.5);
	const bool hasShadow = NativeCompositeHasLayer(layerMask, 1);
#endif
	const float2 shadowSourceOffset =
		CompositeFlags.zw * input.glyphParams.y;
#if COMPOSITE_STATIC_SHIFTED_SHADOW > 0
	const bool shiftedShadow = true;
#elif COMPOSITE_STATIC_SHIFTED_SHADOW == 0
	const bool shiftedShadow = false;
#else
	const bool shiftedShadow = hasShadow
		&& (abs(shadowSourceOffset.x) + abs(shadowSourceOffset.y) > 0.0001);
#endif
	const float2 shadowUv = input.atlasUv
		- shadowSourceOffset * AtlasPass.xy;
	const float centerInside =
		NativeCompositeInsideGlyph(input.atlasUv, input.glyphBounds);
#if COMPOSITE_STATIC_LAYER_MASK > 0
#if (COMPOSITE_STATIC_LAYER_MASK & 14) \
	|| ((COMPOSITE_STATIC_LAYER_MASK & 1) \
		&& COMPOSITE_STATIC_SHIFTED_SHADOW == 0)
	float drawableInside = centerInside;
#else
	float drawableInside = 0.0;
#endif
#if (COMPOSITE_STATIC_LAYER_MASK & 1) \
	&& COMPOSITE_STATIC_SHIFTED_SHADOW > 0
	drawableInside = max(drawableInside,
		NativeCompositeInsideGlyph(shadowUv, input.glyphBounds));
#endif
#else
	const bool hasCenterLayer =
		NativeCompositeHasLayer(layerMask, 2)
		|| NativeCompositeHasLayer(layerMask, 4)
		|| NativeCompositeHasLayer(layerMask, 8)
		|| (hasShadow && !shiftedShadow);
	float drawableInside = hasCenterLayer ? centerInside : 0.0;
	if (hasShadow && shiftedShadow)
	{
		drawableInside = max(drawableInside,
			NativeCompositeInsideGlyph(shadowUv, input.glyphBounds));
	}
#endif
	return drawableInside;
}

float4 EvaluateNativeComposite(NativeFontPixelInput input)
{
	const float spread = max(input.glyphParams.x, 0.0001);
	const float distanceScale = max(input.glyphParams.y, 0.0001);
#if COMPOSITE_QUALITY > 0
	// Resolve the complete screen-space UV basis once, before any per-layer
	// dynamic flow. Every quality tap below is expressed in screen pixels.
	const float2 atlasUvDx = ddx(input.atlasUv);
	const float2 atlasUvDy = ddy(input.atlasUv);
#endif
#if COMPOSITE_STATIC_LAYER_MASK > 0
	const int layerMask = COMPOSITE_STATIC_LAYER_MASK;
#if (COMPOSITE_STATIC_LAYER_MASK & 1)
	const bool hasShadow = true;
#else
	const bool hasShadow = false;
#endif
#else
	const int layerMask = (int)floor(input.glyphParams.z + 0.5);
	const bool hasShadow = NativeCompositeHasLayer(layerMask, 1);
#endif
	const float2 shadowSourceOffset =
		CompositeFlags.zw * input.glyphParams.y;
#if COMPOSITE_STATIC_SHIFTED_SHADOW > 0
	const bool shiftedShadow = true;
#elif COMPOSITE_STATIC_SHIFTED_SHADOW == 0
	const bool shiftedShadow = false;
#else
	const bool shiftedShadow = hasShadow
		&& (abs(shadowSourceOffset.x) + abs(shadowSourceOffset.y) > 0.0001);
#endif
	const float2 shadowUv = input.atlasUv
		- shadowSourceOffset * AtlasPass.xy;
	const float antialiasWidth =
		ResolveNativeFontDistanceFieldAntialiasWidth(input, spread);

	// Composite shadow-union geometry can contain large regions that are outside
	// both the center glyph and its shifted shadow. Reject those pixels before
	// the first atlas lookup or any effect reconstruction.
	const float centerInside =
		NativeCompositeInsideGlyph(input.atlasUv, input.glyphBounds);
	float shadowInside = centerInside;
#if COMPOSITE_STATIC_LAYER_MASK > 0
#if (COMPOSITE_STATIC_LAYER_MASK & 14) \
	|| ((COMPOSITE_STATIC_LAYER_MASK & 1) \
		&& COMPOSITE_STATIC_SHIFTED_SHADOW == 0)
	float drawableInside = centerInside;
#else
	float drawableInside = 0.0;
#endif
#if (COMPOSITE_STATIC_LAYER_MASK & 1) \
	&& COMPOSITE_STATIC_SHIFTED_SHADOW > 0
	shadowInside = NativeCompositeInsideGlyph(shadowUv, input.glyphBounds);
	drawableInside = max(drawableInside, shadowInside);
#endif
#else
	const bool hasCenterLayer =
		NativeCompositeHasLayer(layerMask, 2)
		|| NativeCompositeHasLayer(layerMask, 4)
		|| NativeCompositeHasLayer(layerMask, 8)
		|| (hasShadow && !shiftedShadow);
	float drawableInside = hasCenterLayer ? centerInside : 0.0;
	if (hasShadow && shiftedShadow)
	{
		shadowInside =
			NativeCompositeInsideGlyph(shadowUv, input.glyphBounds);
		drawableInside = max(drawableInside, shadowInside);
	}
#endif
	const NativeCompositeSample center =
		SampleNativeCompositeKnownInside(input.atlasUv, input.glyphBounds,
			spread, antialiasWidth, centerInside);
	NativeCompositeSample shadowCenter = center;
	if (shiftedShadow)
	{
		shadowCenter = SampleNativeCompositeKnownInside(
			shadowUv, input.glyphBounds, spread, antialiasWidth,
			shadowInside);
	}

	float fillCoverage = 0.0;
	float shadowCoverage = 0.0;
	float glowCoverage = 0.0;
	float outlineCoverage = 0.0;
#if COMPOSITE_STATIC_LAYER_MASK == 0 \
	|| (COMPOSITE_STATIC_LAYER_MASK & 8)
	fillCoverage = center.bodyCoverage;
#endif
#if COMPOSITE_STATIC_LAYER_MASK == 0 \
	|| (COMPOSITE_STATIC_LAYER_MASK & 1)
	shadowCoverage = NativeCompositeShadowSample(
		shadowCenter.alphaDistance, shadowCenter.bodyCoverage,
		antialiasWidth, distanceScale);
#endif
#if COMPOSITE_STATIC_LAYER_MASK == 0 \
	|| (COMPOSITE_STATIC_LAYER_MASK & 2)
	glowCoverage = NativeCompositeGlowMask(center.alphaDistance,
		center.bodyCoverage, antialiasWidth, distanceScale);
#endif
#if COMPOSITE_STATIC_LAYER_MASK == 0 \
	|| (COMPOSITE_STATIC_LAYER_MASK & 4)
	outlineCoverage = NativeCompositeOutline(center.alphaDistance,
		center.bodyCoverage, antialiasWidth, distanceScale);
#endif

#if COMPOSITE_QUALITY > 0
#if COMPOSITE_QUALITY == 1
	const float2 effectOffsets[4] = {
		NativeFontScreenSubpixelOffset(
			atlasUvDx, atlasUvDy, float2(-0.25, -0.25)),
		NativeFontScreenSubpixelOffset(
			atlasUvDx, atlasUvDy, float2( 0.25, -0.25)),
		NativeFontScreenSubpixelOffset(
			atlasUvDx, atlasUvDy, float2(-0.25,  0.25)),
		NativeFontScreenSubpixelOffset(
			atlasUvDx, atlasUvDy, float2( 0.25,  0.25))
	};
#else
	const float2 effectOffsets[4] = {
		NativeFontScreenSubpixelOffset(
			atlasUvDx, atlasUvDy, float2(-0.375, -0.125)),
		NativeFontScreenSubpixelOffset(
			atlasUvDx, atlasUvDy, float2(-0.125,  0.375)),
		NativeFontScreenSubpixelOffset(
			atlasUvDx, atlasUvDy, float2( 0.125, -0.375)),
		NativeFontScreenSubpixelOffset(
			atlasUvDx, atlasUvDy, float2( 0.375,  0.125))
	};
#endif
	shadowCoverage = glowCoverage = outlineCoverage = fillCoverage = 0.0;
#if COMPOSITE_STATIC_LAYER_MASK > 0
	[unroll]
#endif
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
#if COMPOSITE_STATIC_LAYER_MASK == 0 \
	|| (COMPOSITE_STATIC_LAYER_MASK & 1)
		shadowCoverage += NativeCompositeShadowSample(
			shadowSample.alphaDistance, shadowSample.bodyCoverage,
			antialiasWidth, distanceScale);
#endif
#if COMPOSITE_STATIC_LAYER_MASK == 0 \
	|| (COMPOSITE_STATIC_LAYER_MASK & 2)
		glowCoverage += NativeCompositeGlowMask(sample.alphaDistance,
			sample.bodyCoverage, antialiasWidth, distanceScale);
#endif
#if COMPOSITE_STATIC_LAYER_MASK == 0 \
	|| (COMPOSITE_STATIC_LAYER_MASK & 4)
		outlineCoverage += NativeCompositeOutline(sample.alphaDistance,
			sample.bodyCoverage, antialiasWidth, distanceScale);
#endif
#if COMPOSITE_STATIC_LAYER_MASK == 0 \
	|| (COMPOSITE_STATIC_LAYER_MASK & 8)
		fillCoverage += sample.bodyCoverage;
#endif
	}
	shadowCoverage *= 0.25;
	glowCoverage *= 0.25;
	outlineCoverage *= 0.25;
	fillCoverage *= 0.25;
#endif

#if COMPOSITE_STATIC_LAYER_MASK == 0 \
	|| (COMPOSITE_STATIC_LAYER_MASK & 2)
	glowCoverage *= NativeCompositeGlowFeather(center.alphaDistance,
		antialiasWidth, distanceScale);
#endif
#if COMPOSITE_STATIC_LAYER_MASK == 0 \
	|| (COMPOSITE_STATIC_LAYER_MASK & 1)
	if (EffectPrimary.x <= 0.001
		&& (EffectSecondary.w > 0.0 || CompositeFlags.x > 0.0))
	{
		shadowCoverage = ApplyNativeCompositeHardShadow(
			shadowCenter.alphaDistance, shadowCoverage,
			antialiasWidth, distanceScale);
	}
#endif

#if COMPOSITE_QUALITY > 1
#if COMPOSITE_STATIC_LAYER_MASK == 0 \
	|| (COMPOSITE_STATIC_LAYER_MASK & 8)
	const float2 extraOffsets[4] = {
		NativeFontScreenSubpixelOffset(
			atlasUvDx, atlasUvDy, float2(-0.375,  0.375)),
		NativeFontScreenSubpixelOffset(
			atlasUvDx, atlasUvDy, float2( 0.375, -0.375)),
		NativeFontScreenSubpixelOffset(
			atlasUvDx, atlasUvDy, float2(-0.125, -0.125)),
		NativeFontScreenSubpixelOffset(
			atlasUvDx, atlasUvDy, float2( 0.125,  0.125))
	};
	float extraFill = 0.0;
#if COMPOSITE_STATIC_LAYER_MASK > 0
	[unroll]
#endif
	for (int extraTap = 0; extraTap < 4; ++extraTap)
	{
		extraFill += SampleNativeComposite(
			input.atlasUv + extraOffsets[extraTap],
			input.glyphBounds, spread, antialiasWidth).bodyCoverage;
	}
	fillCoverage = fillCoverage * 0.5 + extraFill * 0.125;
#endif
#endif

	float3 premultiplied = 0.0;
	float alpha = 0.0;
#if COMPOSITE_STATIC_LAYER_MASK > 0
#if (COMPOSITE_STATIC_LAYER_MASK & 1)
	ApplyNativeCompositeSource(premultiplied, alpha,
		ResolveNativeCompositeSource(shadowCoverage, ShadowColor,
			input.baseColor, NativeCompositeLayerUsesLiveRgb(0)));
#endif
#if (COMPOSITE_STATIC_LAYER_MASK & 2)
	ApplyNativeCompositeSource(premultiplied, alpha,
		ResolveNativeCompositeSource(glowCoverage, GlowColor,
			input.baseColor, NativeCompositeLayerUsesLiveRgb(1)));
#endif
#if (COMPOSITE_STATIC_LAYER_MASK & 4)
	ApplyNativeCompositeSource(premultiplied, alpha,
		ResolveNativeCompositeSource(outlineCoverage, OutlineColor,
			input.baseColor, NativeCompositeLayerUsesLiveRgb(2)));
#endif
#if (COMPOSITE_STATIC_LAYER_MASK & 8)
	ApplyNativeCompositeSource(premultiplied, alpha,
		ResolveNativeCompositeSource(fillCoverage, FillColor,
			input.baseColor, NativeCompositeLayerUsesLiveRgb(3)));
#endif
#else
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
#endif
	return alpha > 0.000001
		? float4(premultiplied / alpha, alpha)
		: float4(0.0, 0.0, 0.0, 0.0);
}

float4 Main(NativeFontPixelInput input) : COLOR0
{
#if COMPOSITE_STATIC_SHIFTED_SHADOW == 0
	return EvaluateNativeComposite(input);
#else
	[branch]
	if (NativeCompositeDrawableInside(input) >= 0.5)
		return EvaluateNativeComposite(input);
	clip(-1.0);
	return float4(0.0, 0.0, 0.0, 0.0);
#endif
}

sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);
float4 LayerColor : register(c1);
float4 AtlasPass : register(c2); // invWidth, invHeight, layer, SDF spread
float4 EffectParams : register(c3); // layer-specific parameters in device pixels
float4 SdfFlags : register(c4); // x: current range uses an SDF mask

#include "freetype_tile_compat.hlsli"
#include "freetype_sdf_compat.hlsli"

struct PixelInput
{
	float2 uv : TEXCOORD0;
};

float Coverage(float2 uv)
{
	return SampleFreeTypeMask(FontAtlas, uv);
}

float EvaluateSdfEffect(float distance, float antialiasWidth, int layer)
{
	const float body = FreeTypeSdfBodyCoverage(distance, antialiasWidth);
	if (layer == 0)
	{
		const float blur = max(EffectParams.x, 0.0);
		// A zero-blur shadow is the translated glyph body, not a powered copy of
		// its antialiased edge. Keep this threshold aligned with the CPU/runtime
		// decision that selects the hard-shadow mask.
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

float EvaluateAt(float2 uv, float antialiasWidth, int layer)
{
	return EvaluateSdfEffect(
		DecodeFreeTypeSdfDistance(Coverage(uv), AtlasPass.w),
		antialiasWidth, layer);
}

float SupersampledSdfCoverage(float2 uv, int layer)
{
	const float centerDistance = DecodeFreeTypeSdfDistance(
		Coverage(uv), AtlasPass.w);
	const float antialiasWidth = ResolveFreeTypeSdfAntialiasWidth(centerDistance);
#if EFFECT_QUALITY == 0
	return EvaluateSdfEffect(centerDistance, antialiasWidth, layer);
#elif EFFECT_QUALITY == 1
	const float2 quarter = AtlasPass.xy * 0.25;
	float sum = 0.0;
	sum += EvaluateAt(uv + float2(-quarter.x, -quarter.y), antialiasWidth, layer);
	sum += EvaluateAt(uv + float2( quarter.x, -quarter.y), antialiasWidth, layer);
	sum += EvaluateAt(uv + float2(-quarter.x,  quarter.y), antialiasWidth, layer);
	sum += EvaluateAt(uv + float2( quarter.x,  quarter.y), antialiasWidth, layer);
	return sum * 0.25;
#else
	const float2 texel = AtlasPass.xy;
	float sum = 0.0;
	sum += EvaluateAt(uv + texel * float2(-0.375, -0.125), antialiasWidth, layer);
	sum += EvaluateAt(uv + texel * float2(-0.125,  0.375), antialiasWidth, layer);
	sum += EvaluateAt(uv + texel * float2( 0.125, -0.375), antialiasWidth, layer);
	sum += EvaluateAt(uv + texel * float2( 0.375,  0.125), antialiasWidth, layer);
	sum += EvaluateAt(uv + texel * float2(-0.375,  0.375), antialiasWidth, layer);
	sum += EvaluateAt(uv + texel * float2( 0.375, -0.375), antialiasWidth, layer);
	sum += EvaluateAt(uv + texel * float2(-0.125, -0.125), antialiasWidth, layer);
	sum += EvaluateAt(uv + texel * float2( 0.125,  0.125), antialiasWidth, layer);
	return sum * 0.125;
#endif
}

float4 Main(PixelInput input) : COLOR0
{
	const int layer = (int)(AtlasPass.z + 0.5);
	float coverage;
	if (SdfFlags.x >= 0.5)
		coverage = SupersampledSdfCoverage(input.uv, layer);
	else
		coverage = Coverage(input.uv);
	return ComposeFreeTypeTileColor(coverage, TileColor, LayerColor);
}

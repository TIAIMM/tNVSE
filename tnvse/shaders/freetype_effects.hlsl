sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);
float4 LayerColor : register(c1);
float4 AtlasPass : register(c2); // invWidth, invHeight, layer, SDF spread
float4 EffectParams : register(c3); // layer-specific parameters in device pixels
float4 EffectReserved : register(c4);

#include "freetype_tile_compat.hlsli"

struct PixelInput
{
	float2 uv : TEXCOORD0;
};

float Coverage(float2 uv)
{
	return tex2D(FontAtlas, uv).a;
}

float DecodeDistance(float encodedDistance)
{
	// FreeType packs zero at byte 128 and one normalized distance unit into
	// 128 levels.  Texture sampling divides the stored byte by 255.
	return (encodedDistance * (255.0 / 128.0) - 1.0) * AtlasPass.w;
}

float ResolveAntialiasWidth(float distance)
{
	return max(0.35, 0.5 * (abs(ddx(distance)) + abs(ddy(distance))));
}

float BodyCoverage(float distance, float antialiasWidth)
{
	return smoothstep(-antialiasWidth, antialiasWidth, distance);
}

float EvaluateSdfEffect(float distance, float antialiasWidth, int layer)
{
	const float body = BodyCoverage(distance, antialiasWidth);
	if (layer == 0)
	{
		const float blur = max(EffectParams.x, 0.0);
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
	return EvaluateSdfEffect(DecodeDistance(Coverage(uv)), antialiasWidth, layer);
}

float SupersampledSdfCoverage(float2 uv, int layer)
{
	const float centerDistance = DecodeDistance(Coverage(uv));
	const float antialiasWidth = ResolveAntialiasWidth(centerDistance);
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
	if (layer == 3 || (layer == 0 && EffectParams.x <= 0.001))
		coverage = Coverage(input.uv);
	else
		coverage = SupersampledSdfCoverage(input.uv, layer);
	return ComposeFreeTypeTileColor(coverage, TileColor, LayerColor);
}

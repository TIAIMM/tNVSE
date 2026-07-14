#ifndef TNVSE_FREETYPE_SDF_COMPAT_HLSLI
#define TNVSE_FREETYPE_SDF_COMPAT_HLSLI

float SampleFreeTypeMask(sampler2D atlas, float2 uv)
{
	return tex2D(atlas, uv).a;
}

float DecodeFreeTypeSdfDistance(float encodedDistance, float spread)
{
	// FreeType stores the zero-distance contour at byte 128. Texture sampling
	// normalizes that byte to 0..1, so restore its signed distance here.
	return (encodedDistance * (255.0 / 128.0) - 1.0) * spread;
}

float ResolveFreeTypeSdfAntialiasWidth(float distance)
{
	return max(0.35, 0.5 * (abs(ddx(distance)) + abs(ddy(distance))));
}

float FreeTypeSdfBodyCoverage(float distance, float antialiasWidth)
{
	return smoothstep(-antialiasWidth, antialiasWidth, distance);
}

float ResolveFreeTypeBodyCoverage(sampler2D atlas, float2 uv,
	float spread, float usesSdf)
{
	const float sampled = SampleFreeTypeMask(atlas, uv);
	if (usesSdf < 0.5)
		return sampled;
	const float distance = DecodeFreeTypeSdfDistance(sampled, spread);
	return FreeTypeSdfBodyCoverage(distance,
		ResolveFreeTypeSdfAntialiasWidth(distance));
}

#endif

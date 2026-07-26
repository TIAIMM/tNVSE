sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);

struct NativeFontPixelInput
{
	float2 atlasUv : TEXCOORD0;
	float4 bakedColor : COLOR0;
	// The aggressive coverage route stores 0 for fixed effect RGB and 1 for
	// Fill/live effect RGB. The other two values are intentionally unused.
	float3 glyphParams : TEXCOORD1;
};

float4 Main(NativeFontPixelInput input) : COLOR0
{
	const float coverage = tex2D(FontAtlas, input.atlasUv).a;
	const float usesLiveTileRgb = step(0.5, input.glyphParams.z);
	const float3 tileRgb = lerp(
		float3(1.0, 1.0, 1.0), TileColor.rgb, usesLiveTileRgb);
	return float4(tileRgb * input.bakedColor.rgb,
		saturate(coverage * TileColor.a * input.bakedColor.a));
}

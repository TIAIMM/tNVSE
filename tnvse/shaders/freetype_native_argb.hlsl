sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);

struct NativeFontPixelInput
{
	float2 atlasUv : TEXCOORD0;
	float4 baseColor : COLOR0;
	float3 glyphParams : TEXCOORD1;
};

float4 Main(NativeFontPixelInput input) : COLOR0
{
	const float4 glyph = tex2D(FontAtlas, input.atlasUv);
	return float4(
		glyph.rgb * TileColor.rgb * input.baseColor.rgb,
		saturate(glyph.a * TileColor.a * input.baseColor.a));
}

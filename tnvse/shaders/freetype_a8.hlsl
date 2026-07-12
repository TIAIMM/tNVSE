sampler2D FontAtlas : register(s0);
float4 TileColor : register(c0);

struct PixelInput
{
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
};

float4 Main(PixelInput input) : COLOR0
{
	const float coverage = tex2D(FontAtlas, input.uv).a;
	const float4 color = input.color * TileColor;
	return float4(color.rgb, saturate(color.a * coverage));
}

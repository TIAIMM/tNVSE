sampler2D FontAtlas : register(s0);

struct PixelInput
{
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
};

float4 Main(PixelInput input) : COLOR0
{
	const float coverage = tex2D(FontAtlas, input.uv).a;
	return float4(input.color.rgb, saturate(input.color.a * coverage));
}

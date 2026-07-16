float4x4 WorldViewProjection : register(c0);

struct NativeFontVertexInput
{
	float3 position : POSITION0;
	float2 atlasUv : TEXCOORD0;
	float4 layerColor : COLOR0;
};

struct NativeFontVertexOutput
{
	float4 position : POSITION0;
	float2 atlasUv : TEXCOORD0;
	float4 layerColor : COLOR0;
};

NativeFontVertexOutput Main(NativeFontVertexInput input)
{
	NativeFontVertexOutput output;
	// This is the stock TILE1000 transform contract: c0-c3 are populated by
	// TileShader::UpdateConstants and contain the row-major WVP matrix.
	output.position = mul(float4(input.position, 1.0), WorldViewProjection);
	output.atlasUv = input.atlasUv;
	output.layerColor = input.layerColor;
	return output;
}

float4x4 WorldViewProjection : register(c0);
// x: packet-wide source SDF spread, y: distance scale,
// z: immutable composite layer mask.
float4 StockLayoutGlyphParams : register(c209);

struct StockLayoutVertexInput
{
	float3 position : POSITION0;
	float2 atlasUv : TEXCOORD0;
	float4 baseColor : COLOR0;
	float2 glyphMinimum : TEXCOORD1;
	float2 glyphMaximum : TEXCOORD2;
};

struct StockLayoutVertexOutput
{
	float4 position : POSITION0;
	float2 atlasUv : TEXCOORD0;
	float4 baseColor : COLOR0;
	float3 glyphParams : TEXCOORD1;
	float4 glyphBounds : TEXCOORD2;
	float antialiasWidth : TEXCOORD3;
};

StockLayoutVertexOutput Main(StockLayoutVertexInput input)
{
	StockLayoutVertexOutput output;
	output.position = mul(float4(input.position, 1.0), WorldViewProjection);
	output.atlasUv = input.atlasUv;
	output.baseColor = input.baseColor;
	output.glyphParams = StockLayoutGlyphParams.xyz;
	output.glyphBounds = float4(input.glyphMinimum, input.glyphMaximum);
	// The matching pixel shader reconstructs the exact screen footprint from
	// derivatives.  Keep the semantic present so its ABI matches the native
	// composite input without requiring the native c208 vertex-AA path.
	output.antialiasWidth = 0.0;
	return output;
}

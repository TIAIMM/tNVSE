struct NativeCacheVertexInput
{
	float3 position : POSITION0;
	float2 textureUv : TEXCOORD0;
	float4 color : COLOR0;
	float3 unused : TEXCOORD1;
};

struct NativeCacheVertexOutput
{
	float4 position : POSITION0;
	float2 textureUv : TEXCOORD0;
};

NativeCacheVertexOutput Main(NativeCacheVertexInput input)
{
	NativeCacheVertexOutput output;
	// Cache geometry is generated for one exact render-target/viewport state and
	// already stores clip coordinates. The stock Tile draw still owns scissor and
	// ordering, while its WVP constants intentionally do not transform this quad.
	output.position = float4(input.position.x, input.position.z, 0.0, 1.0);
	output.textureUv = input.textureUv;
	return output;
}

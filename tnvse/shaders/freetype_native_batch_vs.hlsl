float4x4 WorldViewProjection : register(c0);
float4 NativeAaProfile : register(c4); // viewport half-size, raster scale, valid

struct NativeFontBatchVertexInput
{
	float3 position : POSITION0;
	float2 atlasUv : TEXCOORD0;
	float4 baseColor : COLOR0;
	float3 glyphParams : TEXCOORD1;
	float4 glyphBounds : TEXCOORD2;
	float4 tileMultiplier : TEXCOORD3;
};

struct NativeFontVertexOutput
{
	float4 position : POSITION0;
	float2 atlasUv : TEXCOORD0;
	float4 baseColor : COLOR0;
	float3 glyphParams : TEXCOORD1;
	float4 glyphBounds : TEXCOORD2;
	float antialiasWidth : TEXCOORD3;
};

float2 NativeFontProjectedAxis(float4 clipPosition, float4 clipAxis)
{
	const float inverseW = 1.0 / clipPosition.w;
	return (clipAxis.xy * clipPosition.w
		- clipPosition.xy * clipAxis.w)
		* (inverseW * inverseW) * NativeAaProfile.xy;
}

float NativeFontVertexAntialiasWidth(float4 clipPosition,
	float3 glyphParams)
{
	const float4 clipX =
		mul(float4(1.0, 0.0, 0.0, 0.0), WorldViewProjection);
	const float4 clipZ =
		mul(float4(0.0, 0.0, 1.0, 0.0), WorldViewProjection);
	const float2 screenX = NativeFontProjectedAxis(clipPosition, clipX);
	const float2 screenZ = NativeFontProjectedAxis(clipPosition, clipZ);
	const float lengthX = length(screenX);
	const float lengthZ = length(screenZ);
	const float determinant = abs(
		screenX.x * screenZ.y - screenX.y * screenZ.x);
	const float sourcePixelsPerLogical =
		max(glyphParams.y * NativeAaProfile.z, 0.0001);
	const float denominator = max(
		determinant * (lengthX + lengthZ), 0.000001);
	const float antialiasWidth = sourcePixelsPerLogical
		* lengthX * lengthZ / denominator;
	return min(max(glyphParams.x, 0.0001),
		max(antialiasWidth, 0.0001))
		* step(0.5, NativeAaProfile.w);
}

NativeFontVertexOutput Main(NativeFontBatchVertexInput input)
{
	NativeFontVertexOutput output;
	output.position = mul(float4(input.position, 1.0), WorldViewProjection);
	output.atlasUv = input.atlasUv;
	// tileMultiplier is constant over every source quad. Multiplication in the
	// vertex shader therefore preserves the ordinary pixel-shader result without
	// consuming another interpolator or quantizing the live Tile color.
	output.baseColor = input.baseColor * input.tileMultiplier;
	output.glyphParams = input.glyphParams;
	output.glyphBounds = input.glyphBounds;
	output.antialiasWidth = NativeFontVertexAntialiasWidth(
		output.position, input.glyphParams);
	return output;
}

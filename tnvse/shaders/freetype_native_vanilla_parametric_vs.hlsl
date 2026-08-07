float4x4 WorldViewProjection : register(c0);
float4 NativeAaProfile : register(c208); // viewport half-size, raster scale, valid
// z: immutable composite layer mask. Per-glyph spread and distance scale are
// carried by TEXCOORD1 so mixed source sizes remain in one static packet.
float4 VanillaLayoutGlyphParams : register(c209);

struct VanillaParametricVertexInput
{
	float3 position : POSITION0;
	float2 atlasUv : TEXCOORD0;
	float4 baseColor : COLOR0;
	float2 glyphReconstruction : TEXCOORD1;
	float2 glyphMinimum : TEXCOORD2;
	float2 glyphMaximum : TEXCOORD3;
};

struct VanillaParametricVertexOutput
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

VanillaParametricVertexOutput Main(VanillaParametricVertexInput input)
{
	VanillaParametricVertexOutput output;
	output.position = mul(float4(input.position, 1.0), WorldViewProjection);
	output.atlasUv = input.atlasUv;
	output.baseColor = input.baseColor;
	output.glyphParams = float3(
		input.glyphReconstruction, VanillaLayoutGlyphParams.z);
	output.glyphBounds = float4(input.glyphMinimum, input.glyphMaximum);
	output.antialiasWidth = NativeFontVertexAntialiasWidth(
		output.position, output.glyphParams);
	return output;
}

float4x4 WorldViewProjection : register(c0);
float4 NativeAaProfile : register(c4); // viewport half-size, raster scale, valid

struct NativeFontVertexInput
{
	float3 position : POSITION0;
	float2 atlasUv : TEXCOORD0;
	float4 baseColor : COLOR0;
	float3 glyphParams : TEXCOORD1;
	float4 glyphBounds : TEXCOORD2;
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
	// Differentiate clip.xy/clip.w analytically, then convert NDC to pixels.
	// Tile geometry uses local X/Z as its two screen-plane axes.
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
	// This is algebraically equivalent to the canonical msdfgen
	// screenPxRange for an affine transform, including anisotropic scaling.
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

NativeFontVertexOutput Main(NativeFontVertexInput input)
{
	NativeFontVertexOutput output;
	// This is the stock TILE1000 transform contract: c0-c3 are populated by
	// TileShader::UpdateConstants and contain the row-major WVP matrix.
	output.position = mul(float4(input.position, 1.0), WorldViewProjection);
	output.atlasUv = input.atlasUv;
	output.baseColor = input.baseColor;
	output.glyphParams = input.glyphParams;
	output.glyphBounds = input.glyphBounds;
	output.antialiasWidth = NativeFontVertexAntialiasWidth(
		output.position, input.glyphParams);
	return output;
}

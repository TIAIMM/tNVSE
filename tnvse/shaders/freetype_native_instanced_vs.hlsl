float4 NativeAaProfile : register(c208); // viewport half-size, raster scale, valid

struct NativeFontInstancedVertexInput
{
	float4 corner : POSITION0;
	float4 localRect : TEXCOORD4;
	float4 uvRect : TEXCOORD5;
	float4 baseColor : COLOR0;
	float4 glyphParamsAndSelector : TEXCOORD6;
	float4 glyphBounds : TEXCOORD7;
	float localDepth : POSITION1;
	// Keep arbitrary vertex-input usage indices inside TEXCOORD0-TEXCOORD7.
	// D3D9 permits UsageIndex 0-15, but the shipped Tile declarations and the
	// Microsoft indexed-instancing layout stay in the first eight slots. This
	// avoids wrapper/driver paths which still size TEXCOORD lookup tables from
	// the fixed-function eight-coordinate limit.
	float4 wvpColumn0 : TEXCOORD0;
	float4 wvpColumn1 : TEXCOORD1;
	float4 wvpColumn2 : TEXCOORD2;
	float4 wvpColumn3 : TEXCOORD3;
	float4 tileColor : COLOR1;
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
	float4 clipX, float4 clipZ, float3 glyphParams)
{
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

NativeFontVertexOutput Main(NativeFontInstancedVertexInput input)
{
	NativeFontVertexOutput output;
	// stream 0 stores exact endpoint weights instead of 0/1 interpolation
	// coordinates.  This avoids the subtraction/add sequence emitted for lerp
	// and reconstructs the original TL/TR/BR/BL endpoints without introducing
	// a second geometry rounding path.
	const float localX = dot(input.localRect.xz, input.corner.xy);
	const float localZ = dot(input.localRect.yw, input.corner.zw);
	const float4 localPosition = float4(
		localX, input.localDepth, localZ, 1.0);
	output.position = float4(
		dot(localPosition, input.wvpColumn0),
		dot(localPosition, input.wvpColumn1),
		dot(localPosition, input.wvpColumn2),
		dot(localPosition, input.wvpColumn3));
	output.atlasUv = float2(
		dot(input.uvRect.xz, input.corner.xy),
		dot(input.uvRect.yw, input.corner.zw));
	const float tileRgbSelector =
		saturate(input.glyphParamsAndSelector.w);
	const float3 tileRgb = lerp(
		float3(1.0, 1.0, 1.0), input.tileColor.rgb,
		tileRgbSelector);
	output.baseColor = float4(
		input.baseColor.rgb * tileRgb,
		input.baseColor.a * input.tileColor.a);
	output.glyphParams = input.glyphParamsAndSelector.xyz;
	output.glyphBounds = input.glyphBounds;
	const float4 clipX = float4(
		input.wvpColumn0.x, input.wvpColumn1.x,
		input.wvpColumn2.x, input.wvpColumn3.x);
	const float4 clipZ = float4(
		input.wvpColumn0.z, input.wvpColumn1.z,
		input.wvpColumn2.z, input.wvpColumn3.z);
	output.antialiasWidth = NativeFontVertexAntialiasWidth(
		output.position, clipX, clipZ, output.glyphParams);
	return output;
}

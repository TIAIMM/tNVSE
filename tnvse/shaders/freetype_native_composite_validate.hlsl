sampler2D CompositeImage : register(s0);
sampler2D ReferenceImage : register(s1);

float4 Main(float2 textureUv : TEXCOORD0) : COLOR0
{
	const float4 composite = tex2D(CompositeImage, textureUv);
	const float4 reference = tex2D(ReferenceImage, textureUv);
	const float4 difference = abs(composite - reference);
	const float maximumDifference = max(
		max(difference.r, difference.g),
		max(difference.b, difference.a));
	// Both inputs are A8R8G8B8. Reject pixels whose integer channel values
	// differ by more than two; a small epsilon keeps exactly 2/255 accepted.
	clip(maximumDifference - (2.0001 / 255.0));
	return 1.0;
}

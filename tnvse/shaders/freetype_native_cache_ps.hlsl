sampler2D CompositeTexture : register(s0);

float4 Main(float2 textureUv : TEXCOORD0) : COLOR0
{
	// The RTT stores premultiplied RGBA. The cache Tile profile uses
	// ONE/INVSRCALPHA, so this path is exactly one texture sample.
	return tex2D(CompositeTexture, textureUv);
}

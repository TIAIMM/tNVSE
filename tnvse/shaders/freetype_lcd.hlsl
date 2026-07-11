sampler2D FontAtlas : register(s0);

struct PixelInput
{
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

float4 Main(PixelInput input) : COLOR0
{
    const float4 atlas = tex2D(FontAtlas, input.uv);
    const float lcdMarker = max(atlas.r, max(atlas.g, atlas.b));
    const float isLcd = step(0.5 / 255.0, lcdMarker);

#if LCD_CHANNEL == 0
    const float lcdCoverage = atlas.r;
#elif LCD_CHANNEL == 1
    const float lcdCoverage = atlas.g;
#else
    const float lcdCoverage = atlas.b;
#endif

    const float coverage = lerp(atlas.a, lcdCoverage, isLcd) * input.color.a;
    return float4(input.color.rgb, saturate(coverage));
}

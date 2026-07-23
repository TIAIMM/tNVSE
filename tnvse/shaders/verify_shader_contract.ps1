param(
    [Parameter(Mandatory = $true)]
    [string]$Fxc,
    [Parameter(Mandatory = $true)]
    [string]$ShaderDirectory
)

$ErrorActionPreference = 'Stop'

$pixelSources = @(
    'freetype_native_mtsdf_fill.hlsl',
    'freetype_native_mtsdf_effects.hlsl'
)
foreach ($sourceName in $pixelSources) {
    $source = Get-Content -LiteralPath (Join-Path $ShaderDirectory $sourceName) -Raw
    if ($source -notmatch '#include\s+"freetype_native_common\.hlsli"') {
        throw "$sourceName does not include freetype_native_common.hlsli"
    }
}

$common = Get-Content -LiteralPath (
    Join-Path $ShaderDirectory 'freetype_native_common.hlsli') -Raw
if ($common -notmatch 'resolvedTileRgb\s*\*\s*resolvedBaseRgb\s*\*\s*LayerColor\.rgb') {
    throw 'Native FreeType shader ABI does not combine resolved Tile, base-vertex, and packet-layer RGB'
}
if ($common -notmatch
    'resolvedTileRgb\s*=\s*lerp\([\s\S]*?tileColor\.rgb\s*,\s*usesLiveTileRgb\s*\)') {
    throw 'Native FreeType shader ABI does not select identity Tile RGB for fixed effects'
}
if ($common -notmatch
    'resolvedBaseRgb\s*=\s*lerp\([\s\S]*?baseColor\.rgb\s*,\s*usesLiveTileRgb\s*\)') {
    throw 'Native FreeType shader ABI does not select identity base RGB for fixed effects'
}
if ($common -notmatch 'coverage\s*\*\s*tileColor\.a\s*\*\s*baseColor\.a\s*\*\s*LayerColor\.a') {
    throw 'Native FreeType shader ABI does not multiply coverage by all three alpha sources'
}
if ($common -notmatch 'LayerColor\s*:\s*register\(c1\)') {
    throw 'Native FreeType shader ABI does not reserve c1 for the packet layer modifier'
}
if ($common -notmatch 'NativeFontUsesLiveTileRgb[\s\S]*?frac\(layerAndFlags\)\s*<\s*0\.125') {
    throw 'Native FreeType shader ABI does not decode the fixed-effect live-RGB flag'
}
if ($common -match 'float4\s*\([^,]+\*[^,]*coverage') {
    throw 'Native FreeType shader ABI appears to premultiply RGB by coverage'
}
if ($common -notmatch 'MedianNativeFontMtsdf' -or
    $common -notmatch
    '\(encodedDistance\s*-\s*0\.5\)\s*\*\s*\(2\.0\s*\*\s*spread\)') {
    throw 'Native FreeType shader ABI does not decode msdfgen MTSDF distances'
}
if ($common -notmatch 'NativeFontMtsdfScreenPxRange' -or
    $common -notmatch 'const\s+float2\s+dx\s*=\s*ddx\(uv\)' -or
    $common -notmatch 'const\s+float2\s+dy\s*=\s*ddy\(uv\)' -or
    $common -notmatch
    '\(2\.0\s*\*\s*spread\)\s*\*\s*inverseAtlasSize' -or
    $common -notmatch
    '0\.5\s*\*\s*dot\(unitRange,\s*screenTextureSize\)' -or
    $common -notmatch 'screenTextureSize\),\s*1\.0\)') {
    throw 'Native MTSDF AA does not follow msdfgen screenPxRange'
}

$fillSource = Get-Content -LiteralPath (
    Join-Path $ShaderDirectory 'freetype_native_mtsdf_fill.hlsl') -Raw
if ($common -notmatch 'NativeFontBodyEncodedDistance' -or
    $common -notmatch '#if\s+DISTANCE_FIELD_TRUE_SDF[\s\S]*?distanceSample\.a[\s\S]*?#else[\s\S]*?MedianNativeFontMtsdf' -or
    $common -notmatch 'DecodeNativeFontSelectedDistance' -or
    $common -notmatch '255\.0\s*/\s*128\.0' -or
    $fillSource -notmatch 'NativeFontBodyEncodedDistance' -or
    $fillSource -notmatch 'DecodeNativeFontSelectedDistance' -or
    $fillSource -notmatch 'FILL_QUALITY\s*==\s*1' -or
    $fillSource -notmatch 'FILL_QUALITY\s*==\s*0') {
    throw 'Native Fill does not select true-SDF Alpha or MTSDF RGB reconstruction correctly'
}
if ($fillSource -notmatch 'FILL_SUBPIXEL' -or
    $fillSource -notmatch 'SubpixelPass\s*:\s*register\(c4\)' -or
    $fillSource -notmatch
    'input\.atlasUv\s*\+\s*ddx\(input\.atlasUv\)\s*\*\s*SubpixelPass\.x' -or
    $fillSource -notmatch
    'centerCoverage\s*=\s*EvaluateNativeFontMtsdfFillAt\(input\.atlasUv,\s*antialiasWidth\)' -or
    $fillSource -notmatch
    'maxChromaDelta\s*=\s*lerp\(0\.125,\s*0\.25,\s*edgeProximity\)' -or
    $fillSource -notmatch
    'limitedDelta\s*=\s*clamp\(' -or
    $fillSource -notmatch 'limitedDelta\s*\*\s*saturate\(SubpixelPass\.y\)') {
    throw 'Native MTSDF side-channel Fill lacks center-referenced chroma limiting'
}

# The HLSL contract is only valid if the native TileShader profile preserves
# the packet's c1 layer modifier. Keep this check beside shader verification so
# the C++/HLSL ABI cannot drift independently again.
$resolvedShaderDirectory = (Resolve-Path -LiteralPath $ShaderDirectory).Path
$nativeShaderSourcePath = Join-Path (
    Split-Path -Parent $resolvedShaderDirectory) 'Src\font\native\font_native_shader.cpp'
$nativeShaderSource = Get-Content -LiteralPath $nativeShaderSourcePath -Raw
if ($nativeShaderSource -notmatch 'std::array<UInt32,\s*16>\s+constantBits') {
    throw 'Native shader profile key does not cover the complete c1-c4 ABI'
}
if ($nativeShaderSource -notmatch
    'const\s+std::array<float,\s*16>\s+constants\s*;') {
    throw 'Native TileShader profile c1-c4 block is not immutable'
}
if ($nativeShaderSource -notmatch
    'new\s+NativeShaderProfile\(generation,\s*key,\s*packet\.constants\)') {
    throw 'Native TileShader profile does not construct from packet c1-c4 constants'
}
if ($nativeShaderSource -match 'profile->constants\s*\[\s*[0-3]\s*\]\s*=') {
    throw 'Native TileShader profile overwrites the packet c1 layer modifier'
}
if ($nativeShaderSource -notmatch
    'key\.constantBits\.data\(\)\s*,\s*packet\.constants\.data\(\)') {
    throw 'Native shader profile identity does not include packet c1'
}
if ($nativeShaderSource -notmatch
    'key\.subpixelChannel\s*=\s*packet\.subpixelChannel' -or
    $nativeShaderSource -notmatch
    'sideSubpixelChannel[\s\S]*?NativeA8SubpixelChannel::Red[\s\S]*?NativeA8SubpixelChannel::Blue' -or
    $nativeShaderSource -notmatch
    'case\s+1:[\s\S]*?NativeA8SubpixelOrder::RGB' -or
    $nativeShaderSource -notmatch
    'case\s+2:[\s\S]*?NativeA8SubpixelOrder::BGR' -or
    $nativeShaderSource -notmatch
    'NativeA8SubpixelChannel::Red[\s\S]*?D3DCOLORWRITEENABLE_RED' -or
    $nativeShaderSource -notmatch
    'NativeA8SubpixelChannel::Green[\s\S]*?D3DCOLORWRITEENABLE_GREEN[\s\S]*?D3DCOLORWRITEENABLE_ALPHA' -or
    $nativeShaderSource -notmatch
    'NativeA8SubpixelChannel::Blue[\s\S]*?D3DCOLORWRITEENABLE_BLUE') {
    throw 'Native TileShader profiles do not isolate RGB subpixel packets and single-write target alpha'
}
$nativePacketSourcePath = Join-Path (
    Split-Path -Parent $resolvedShaderDirectory) 'Src\font\native\font_native_packets.cpp'
$nativePacketSource = Get-Content -LiteralPath $nativePacketSourcePath -Raw
if ($nativePacketSource -notmatch
    'subpixelRendering\s*&&\s*span\.layer\s*==\s*3' -or
    $nativePacketSource -notmatch
    'subpixelOrder\s*==\s*NativeA8SubpixelOrder::BGR' -or
    $nativePacketSource -notmatch
    'redOffset\s*=\s*bgr\s*\?\s*1\.0f\s*/\s*3\.0f\s*:\s*-1\.0f\s*/\s*3\.0f' -or
    $nativePacketSource -notmatch
    'NativeA8SubpixelChannel::Green,\s*0\.0f' -or
    $nativePacketSource -notmatch
    'blueOffset\s*=\s*bgr\s*\?\s*-1\.0f\s*/\s*3\.0f\s*:\s*1\.0f\s*/\s*3\.0f' -or
    $nativePacketSource -notmatch
    'NativeA8SubpixelChannel::Red,\s*redOffset' -or
    $nativePacketSource -notmatch
    'NativeA8SubpixelChannel::Blue,\s*blueOffset' -or
    $nativePacketSource -notmatch
    'packet\.constants\[12\]\s*=\s*horizontalPixelOffset' -or
    $nativePacketSource -notmatch
    'packet\.constants\[13\]\s*=\s*subpixelStrength') {
    throw 'Native packet builder does not emit RGB/BGR offsets and chroma strength through c4'
}
$nativeAtlasSourcePath = Join-Path (
    Split-Path -Parent $resolvedShaderDirectory) 'Src\font\atlas\font_atlas_shape.cpp'
$nativeAtlasSource = Get-Content -LiteralPath $nativeAtlasSourcePath -Raw
if ($nativeAtlasSource -notmatch
    'add\(&subpixelOrder,\s*sizeof\(subpixelOrder\)\)' -or
    $nativeAtlasSource -notmatch
    'add\(&subpixelStrength,\s*sizeof\(subpixelStrength\)\)' -or
    $nativeAtlasSource -notmatch
    'artifact\.subpixelOrder\s*!=\s*subpixelOrder' -or
    $nativeAtlasSource -notmatch
    'artifact\.subpixelStrength\s*!=\s*subpixelStrength') {
    throw 'Native text-artifact identity does not isolate subpixel order and strength'
}
$loadConfigSourcePath = Join-Path (
    Split-Path -Parent $resolvedShaderDirectory) 'Src\load_config.cpp'
$loadConfigSource = Get-Content -LiteralPath $loadConfigSourcePath -Raw
if ($loadConfigSource -notmatch
    '"uiFreeTypeFontDistanceFieldMode"' -or
    $loadConfigSource -notmatch
    'g_uiFreeTypeFontDistanceFieldMode\s*>\s*1[\s\S]*?g_uiFreeTypeFontDistanceFieldMode\s*=\s*1') {
    throw 'Native distance-field configuration does not validate true-SDF/MTSDF mode'
}
if ($loadConfigSource -notmatch
    '"uiFreeTypeFontSubpixelRendering"' -or
    $loadConfigSource -notmatch
    'g_uiFreeTypeFontSubpixelRendering\s*>\s*2[\s\S]*?g_uiFreeTypeFontSubpixelRendering\s*=\s*0' -or
    $loadConfigSource -notmatch
    '"fFreeTypeFontSubpixelStrength"[\s\S]*?0\.0f,\s*1\.0f') {
    throw 'Native subpixel configuration does not validate mode and chroma strength'
}
if ($nativeShaderSource -match '0x1202188') {
    throw 'Native TileShader update reads the retail global c0 scratch address directly'
}
if ($nativeShaderSource -notmatch
    'ResolveStockTilePixelConstant[\s\S]*?output\[0\]\s*=\s*tile->overlayColor\.r[\s\S]*?output\[1\]\s*=\s*tile->overlayColor\.g[\s\S]*?output\[2\]\s*=\s*tile->overlayColor\.b[\s\S]*?output\[3\]\s*=\s*tile->tileAlpha\s*\*\s*materialAlpha') {
    throw 'Native TileShader update does not mirror the retail c0 RGB/alpha definition'
}
if ($nativeShaderSource -notmatch
    'std::array<float,\s*20>\s+constants[\s\S]*?constants\.begin\(\)\s*\+\s*4[\s\S]*?SetPixelShaderConstantF\(0,\s*constants\.data\(\),\s*5\)') {
    throw 'Native TileShader update does not atomically upload the complete c0-c4 packet'
}
if ($nativeShaderSource -notmatch
    'stockUpdate\(shader,\s*properties\);[\s\S]*?ResolveStockTilePixelConstant\(properties,[\s\S]*?SetPixelShaderConstantF\(0,') {
    throw 'Native TileShader update does not refresh stock maps before publishing c0-c4'
}
if ($nativeShaderSource -notmatch 'D3DSAMP_SRGBTEXTURE,\s*FALSE' -or
    $nativeShaderSource -notmatch 'D3DSAMP_MIPFILTER,\s*D3DTEXF_NONE') {
    throw 'Native MTSDF sampler must force linear-space level-zero sampling'
}

$nativeHookSourcePath = Join-Path (
    Split-Path -Parent $resolvedShaderDirectory) 'Src\font\a8\font_a8_hooks.cpp'
$nativeHookSource = Get-Content -LiteralPath $nativeHookSourcePath -Raw
if ($nativeHookSource -notmatch 'kFirstRegister\s*=\s*1\s*;' -or
    $nativeHookSource -notmatch 'kRegisterCount\s*=\s*4\s*;') {
    throw 'Native packet isolation must preserve only tNVSE-owned pixel c1-c4'
}
if ($nativeHookSource -notmatch
    'packetScope\.Select\(proxyShape\);[\s\S]*?originalTileRenderPass\(pass,\s*currentPass,\s*false,\s*true,\s*setupDrawmode\)') {
    throw 'Native MTSDF packets must disable stock Alpha Test and retain Alpha Blend'
}

$nativeRingSourcePath = Join-Path (
    Split-Path -Parent $resolvedShaderDirectory) 'Src\font\native\font_native_ring.cpp'
$nativeRingSource = Get-Content -LiteralPath $nativeRingSourcePath -Raw
if ($nativeRingSource -notmatch
    'offsetof\(TileShaderPropertyView,\s*overlayColor\)\s*==\s*0x68' -or
    $nativeRingSource -notmatch
    'offsetof\(TileShaderPropertyView,\s*tileAlpha\)\s*==\s*0x78') {
    throw 'Native proxy does not pin the retail Tile RGB/alpha property offsets'
}
if ($nativeRingSource -notmatch
    'destination\.overlayColor\s*=\s*source\.overlayColor' -or
    $nativeRingSource -notmatch
    'destination\.tileAlpha\s*=\s*source\.tileAlpha') {
    throw 'Native proxy does not copy the live Tile RGB/alpha for each submission'
}
if ($nativeRingSource -notmatch
    'm_spMaterialProperty\s*=\s*\r?\n?\s*facade\.m_kProperties\.m_spMaterialProperty') {
    throw 'Native proxy does not preserve the retail material-alpha source for c0.a'
}
if ($nativeRingSource -notmatch 'NiAlphaPropertyPtr\s+alphaProperty' -or
    $nativeRingSource -notmatch
    'proxyAlpha->m_usFlags\s*=\s*sourceAlpha->m_usFlags' -or
    $nativeRingSource -notmatch 'proxyAlpha->SetAlphaTesting\(false\)') {
    throw 'Native proxy does not preserve blending in an owned no-test alpha property'
}

$effectsSource = Get-Content -LiteralPath (
    Join-Path $ShaderDirectory 'freetype_native_mtsdf_effects.hlsl') -Raw
if ($fillSource -match 'MtsdfFlags\.x' -or
    $effectsSource -match 'MtsdfFlags\.x') {
    throw 'Native Shader Loader path still contains a grayscale mask branch'
}
if ($effectsSource -notmatch
    'blur\s*<=\s*0\.001[\s\S]*?return\s+rgbBody\s*;') {
    throw 'Native hard MTSDF shadow does not bypass blur/power shaping'
}
if ($effectsSource -notmatch 'NativeFontVanillaGlowFalloff[\s\S]*?exp2\s*\(') {
    throw 'Native MTSDF glow does not use the vanilla-style exponential falloff'
}
if ($effectsSource -notmatch 'outer\s*-\s*antialiasWidth[\s\S]*?outer\s*\+\s*antialiasWidth') {
    throw 'Native MTSDF glow does not feather its outer cutoff by the pixel footprint'
}
if ($effectsSource -notmatch
    'MtsdfFlags\.z\s*>\s*0\.0[\s\S]*?MtsdfFlags\.w\s*>\s*0\.0') {
    throw 'Native hard shadow does not consume the copied glow and outline switches'
}
if ($effectsSource -notmatch
    'proxyAntialiasWidth[\s\S]*?vuiProxy\s*=\s*smoothstep[\s\S]*?max\s*\(\s*rgbBody\s*,\s*vuiProxy\s*\)') {
    throw 'Native MTSDF outline does not retain VUI-style dark-proxy overlap beneath the fill'
}
if ($effectsSource -match 'expanded\s*-\s*body') {
    throw 'Native MTSDF outline regressed to a hard hollow-ring mask'
}
if ($effectsSource -notmatch 'outline\s*\+\s*\(1\.0\s*-\s*outline\)\s*\*\s*glow') {
    throw 'Native hard shadow does not source-over the copied outline and glow masks'
}
if ($effectsSource -notmatch
    'alphaDistance\s*=\s*DecodeNativeFontSelectedDistance\([\s\S]*?mtsdf\.a' -or
    $effectsSource -notmatch
    'rgbDistance\s*=\s*DecodeNativeFontSelectedDistance\([\s\S]*?NativeFontBodyEncodedDistance\(mtsdf\)') {
    throw 'Native effects do not separate Alpha TSDF geometry from RGB body topology'
}

$shaderInputs = @(
    'freetype_native_common.hlsli',
    'freetype_native_vs.hlsl',
    'freetype_native_mtsdf_fill.hlsl',
    'freetype_native_mtsdf_effects.hlsl'
) | ForEach-Object { Get-Item -LiteralPath (Join-Path $ShaderDirectory $_) }
$newestShaderSource = ($shaderInputs |
    Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 1).LastWriteTimeUtc

$compiledDirectory = Join-Path $ShaderDirectory 'compiled'
$vertexShader = 'tnvse_freetype_native_vs.vso'
$pixelShaders = @(
    'tnvse_freetype_native_mtsdf_fill_fast.pso',
    'tnvse_freetype_native_mtsdf_fill_balanced.pso',
    'tnvse_freetype_native_mtsdf_fill_high.pso',
    'tnvse_freetype_native_mtsdf_fill_subpixel_fast.pso',
    'tnvse_freetype_native_mtsdf_fill_subpixel_balanced.pso',
    'tnvse_freetype_native_mtsdf_fill_subpixel_high.pso',
    'tnvse_freetype_native_mtsdf_effects_fast.pso',
    'tnvse_freetype_native_mtsdf_effects_balanced.pso',
    'tnvse_freetype_native_mtsdf_effects_high.pso',
    'tnvse_freetype_native_sdf_fill_fast.pso',
    'tnvse_freetype_native_sdf_fill_balanced.pso',
    'tnvse_freetype_native_sdf_fill_high.pso',
    'tnvse_freetype_native_sdf_fill_subpixel_fast.pso',
    'tnvse_freetype_native_sdf_fill_subpixel_balanced.pso',
    'tnvse_freetype_native_sdf_fill_subpixel_high.pso',
    'tnvse_freetype_native_sdf_effects_fast.pso',
    'tnvse_freetype_native_sdf_effects_balanced.pso',
    'tnvse_freetype_native_sdf_effects_high.pso'
)
$subpixelTextureSampleBudgets = @{
    'tnvse_freetype_native_mtsdf_fill_subpixel_fast.pso' = 2
    'tnvse_freetype_native_mtsdf_fill_subpixel_balanced.pso' = 5
    'tnvse_freetype_native_mtsdf_fill_subpixel_high.pso' = 9
    'tnvse_freetype_native_sdf_fill_subpixel_fast.pso' = 2
    'tnvse_freetype_native_sdf_fill_subpixel_balanced.pso' = 5
    'tnvse_freetype_native_sdf_fill_subpixel_high.pso' = 9
}

$vertexPath = Join-Path $compiledDirectory $vertexShader
if ((Get-Item -LiteralPath $vertexPath).LastWriteTimeUtc -lt $newestShaderSource) {
    throw "$vertexShader is older than its HLSL/include inputs"
}
$vertexDump = & $Fxc /nologo /dumpbin $vertexPath 2>&1
if ($LASTEXITCODE -ne 0 -or -not ($vertexDump -match '^\s+vs_3_0\s*$')) {
    throw "$vertexShader is not a valid vs_3_0 shader"
}
$vertexInstructions = @($vertexDump | Where-Object { $_ -match '^\s+[a-z]' })
if (-not ($vertexInstructions -match '\bdcl_position')) {
    throw "$vertexShader does not declare POSITION0"
}
if (-not ($vertexInstructions -match '\bdcl_texcoord')) {
    throw "$vertexShader does not forward TEXCOORD0"
}
if (-not ($vertexInstructions -match '\bdcl_color')) {
    throw "$vertexShader does not forward the per-glyph base COLOR0 modifier"
}
foreach ($matrixRegister in 0..3) {
    if (-not ($vertexInstructions -cmatch "\bc$matrixRegister\b")) {
        throw "$vertexShader does not consume stock Tile WVP register c$matrixRegister"
    }
}

foreach ($shaderName in $pixelShaders) {
    $shaderPath = Join-Path $compiledDirectory $shaderName
    if ((Get-Item -LiteralPath $shaderPath).LastWriteTimeUtc -lt $newestShaderSource) {
        throw "$shaderName is older than its HLSL/include inputs"
    }
    $dump = & $Fxc /nologo /dumpbin $shaderPath 2>&1
    if ($LASTEXITCODE -ne 0 -or -not ($dump -match '^\s+ps_3_0\s*$')) {
        throw "$shaderName is not a valid ps_3_0 shader"
    }
    $instructions = @($dump | Where-Object { $_ -match '^\s+[a-z]' })
    if (-not ($instructions -match '\bdcl_color')) {
        throw "$shaderName does not consume the per-glyph base COLOR0 modifier"
    }
    if (-not ($instructions -cmatch '\bc0\b')) {
        throw "$shaderName does not read the Tile color c0"
    }
    if (-not ($instructions -cmatch '\bc1\b')) {
        throw "$shaderName does not read the packet layer color c1"
    }
    if ($shaderName -like 'tnvse_freetype_native_*_fill_subpixel_*.pso' -and
        -not ($instructions -cmatch '\bc4\b')) {
        throw "$shaderName does not read the screen-space subpixel offset from c4"
    }
    if ($subpixelTextureSampleBudgets.ContainsKey($shaderName)) {
        $textureSamples = @($instructions |
            Where-Object { $_ -match '^\s+texld' }).Count
        if ($textureSamples -ne $subpixelTextureSampleBudgets[$shaderName]) {
            throw "$shaderName uses $textureSamples texture samples; expected $($subpixelTextureSampleBudgets[$shaderName])"
        }
    }
    if ($shaderName -like 'tnvse_freetype_native_*_effects_*.pso' -and
        -not ($dump -match '\b0\.001(?:0+\d*)?\b')) {
        throw "$shaderName does not contain the hard-shadow epsilon"
    }
    if ($shaderName -like 'tnvse_freetype_native_mtsdf_*.pso' -or
        $shaderName -like 'tnvse_freetype_native_sdf_*.pso') {
        if (-not ($dump -match 'approximately\s+(\d+)\s+instruction slots used')) {
            throw "$shaderName does not report its instruction-slot count"
        }
        if ([int]$Matches[1] -gt 512) {
            throw "$shaderName exceeds the ps_3_0 512-instruction-slot limit"
        }
    }
}

Write-Host 'FreeType native shader contract verification succeeded.'

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
if ($common -notmatch
    'float2\s+gradient\s*=\s*float2\s*\(ddx\(alphaDistance\),\s*ddy\(alphaDistance\)\)' -or
    $common -notmatch '0\.5\s*\*\s*length\(gradient\)' -or
    $common -match 'max\s*\(\s*0\.35') {
    throw 'Native MTSDF AA must use the true-SDF Euclidean half-pixel width without a fixed logical-width floor'
}
if ($common -notmatch
    'saturate\s*\(0\.5\s*\+\s*rgbDistance[\s\S]*?2\.0\s*\*\s*antialiasWidth') {
    throw 'Native MTSDF body must use linear RGB-median reconstruction'
}

$fillSource = Get-Content -LiteralPath (
    Join-Path $ShaderDirectory 'freetype_native_mtsdf_fill.hlsl') -Raw
if ($fillSource -notmatch
    'NativeFontMtsdfAntialiasWidth\(alphaDistance\)' -or
    $fillSource -notmatch
    'NativeFontMtsdfBody\(rgbDistance,\s*antialiasWidth\)' -or
    $fillSource -match
    'NativeFontMtsdfAntialiasWidth\(rgbDistance\)') {
    throw 'Native MTSDF fill must keep RGB median topology and derive AA only from true-SDF Alpha'
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
if ($nativeShaderSource -notmatch
    'D3DSAMP_SRGBTEXTURE,\s*FALSE' -or
    $nativeShaderSource -notmatch
    'D3DSAMP_MIPFILTER,\s*D3DTEXF_NONE') {
    throw 'Native MTSDF sampler must force linear-space level-zero sampling after the stock update'
}

$nativeHookSourcePath = Join-Path (
    Split-Path -Parent $resolvedShaderDirectory) 'Src\font\a8\font_a8_hooks.cpp'
$nativeHookSource = Get-Content -LiteralPath $nativeHookSourcePath -Raw
if ($nativeHookSource -notmatch 'kFirstRegister\s*=\s*1\s*;' -or
    $nativeHookSource -notmatch 'kRegisterCount\s*=\s*4\s*;') {
    throw 'Native packet isolation must preserve only tNVSE-owned pixel c1-c4'
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

$effectsSource = Get-Content -LiteralPath (
    Join-Path $ShaderDirectory 'freetype_native_mtsdf_effects.hlsl') -Raw
if ($fillSource -match 'MtsdfFlags\.x' -or $effectsSource -match 'MtsdfFlags\.x') {
    throw 'Native Shader Loader path still contains a grayscale mask branch'
}
if ($effectsSource -notmatch 'blur\s*<=\s*0\.001[\s\S]*?return\s+body\s*;') {
    if ($effectsSource -notmatch 'blur\s*<=\s*0\.001[\s\S]*?return\s+rgbBody\s*;') {
        throw 'Native hard MTSDF shadow does not bypass blur/power shaping at the runtime epsilon'
    }
}
if ($effectsSource -notmatch 'NativeFontVanillaGlowFalloff[\s\S]*?exp2\s*\(') {
    throw 'Native MTSDF glow does not use the vanilla-style exponential falloff'
}
if ($effectsSource -notmatch 'outer\s*-\s*antialiasWidth[\s\S]*?outer\s*\+\s*antialiasWidth') {
    throw 'Native MTSDF glow does not feather its outer cutoff by the pixel footprint'
}
if ($effectsSource -notmatch 'MtsdfFlags\.z\s*>\s*0\.0[\s\S]*?MtsdfFlags\.w\s*>\s*0\.0') {
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
    'tnvse_freetype_native_mtsdf_fill.pso',
    'tnvse_freetype_native_mtsdf_effects_fast.pso',
    'tnvse_freetype_native_mtsdf_effects_balanced.pso',
    'tnvse_freetype_native_mtsdf_effects_high.pso'
)

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
    if ($shaderName -like 'tnvse_freetype_native_mtsdf_effects_*.pso' -and
        -not ($dump -match '\b0\.001(?:0+\d*)?\b')) {
        throw "$shaderName does not contain the hard-shadow epsilon"
    }
    if ($shaderName -like 'tnvse_freetype_native_mtsdf_effects_*.pso') {
        if (-not ($dump -match 'approximately\s+(\d+)\s+instruction slots used')) {
            throw "$shaderName does not report its instruction-slot count"
        }
        if ([int]$Matches[1] -gt 512) {
            throw "$shaderName exceeds the ps_3_0 512-instruction-slot limit"
        }
    }
}

Write-Host 'FreeType native MTSDF shader contract verification succeeded.'

param(
    [Parameter(Mandatory = $true)]
    [string]$Fxc,
    [Parameter(Mandatory = $true)]
    [string]$ShaderDirectory
)

$ErrorActionPreference = 'Stop'

$pixelSources = @(
    'freetype_native_sdf.hlsl',
    'freetype_native_effects.hlsl'
)
foreach ($sourceName in $pixelSources) {
    $source = Get-Content -LiteralPath (Join-Path $ShaderDirectory $sourceName) -Raw
    if ($source -notmatch '#include\s+"freetype_native_common\.hlsli"') {
        throw "$sourceName does not include freetype_native_common.hlsli"
    }
}

$common = Get-Content -LiteralPath (
    Join-Path $ShaderDirectory 'freetype_native_common.hlsli') -Raw
if ($common -notmatch 'tileColor\.rgb\s*\*\s*resolvedBaseRgb\s*\*\s*LayerColor\.rgb') {
    throw 'Native FreeType shader ABI does not combine Tile, base-vertex, and packet-layer RGB'
}
if ($common -notmatch 'coverage\s*\*\s*tileColor\.a\s*\*\s*baseColor\.a\s*\*\s*LayerColor\.a') {
    throw 'Native FreeType shader ABI does not multiply coverage by all three alpha sources'
}
if ($common -notmatch 'LayerColor\s*:\s*register\(c1\)') {
    throw 'Native FreeType shader ABI does not reserve c1 for the packet layer modifier'
}
if ($common -notmatch 'frac\(layerAndFlags\)\s*<\s*0\.125') {
    throw 'Native FreeType shader ABI does not decode the fixed-effect base-RGB flag'
}
if ($common -match 'float4\s*\([^,]+\*[^,]*coverage') {
    throw 'Native FreeType shader ABI appears to premultiply RGB by coverage'
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
    'SetPixelShaderConstantF\(1,\s*profile->constants\.data\(\),\s*4\)') {
    throw 'Native TileShader update does not upload packet c1-c4 after stock constants'
}

$effectsSource = Get-Content -LiteralPath (
    Join-Path $ShaderDirectory 'freetype_native_effects.hlsl') -Raw
$sdfSource = Get-Content -LiteralPath (
    Join-Path $ShaderDirectory 'freetype_native_sdf.hlsl') -Raw
if ($sdfSource -match 'SdfFlags\.x' -or $effectsSource -match 'SdfFlags\.x') {
    throw 'Native Shader Loader path still contains a grayscale mask branch'
}
if ($effectsSource -notmatch 'blur\s*<=\s*0\.001[\s\S]*?return\s+body\s*;') {
    throw 'Native hard SDF shadow does not bypass blur/power shaping at the runtime epsilon'
}
if ($effectsSource -notmatch 'NativeFontVanillaGlowFalloff[\s\S]*?exp2\s*\(') {
    throw 'Native SDF glow does not use the vanilla-style exponential falloff'
}
if ($effectsSource -notmatch 'outer\s*-\s*antialiasWidth[\s\S]*?outer\s*\+\s*antialiasWidth') {
    throw 'Native SDF glow does not feather its outer cutoff by the pixel footprint'
}
if ($effectsSource -notmatch 'SdfFlags\.z\s*>\s*0\.0[\s\S]*?SdfFlags\.w\s*>\s*0\.0') {
    throw 'Native hard shadow does not consume the copied glow and outline switches'
}
if ($effectsSource -notmatch 'outline\s*\+\s*\(1\.0\s*-\s*outline\)\s*\*\s*glow') {
    throw 'Native hard shadow does not source-over the copied outline and glow masks'
}

$shaderInputs = @(
    'freetype_native_common.hlsli',
    'freetype_native_vs.hlsl',
    'freetype_native_sdf.hlsl',
    'freetype_native_effects.hlsl'
) | ForEach-Object { Get-Item -LiteralPath (Join-Path $ShaderDirectory $_) }
$newestShaderSource = ($shaderInputs |
    Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 1).LastWriteTimeUtc

$compiledDirectory = Join-Path $ShaderDirectory 'compiled'
$vertexShader = 'tnvse_freetype_native_vs.vso'
$pixelShaders = @(
    'tnvse_freetype_native_sdf.pso',
    'tnvse_freetype_native_effects_fast.pso',
    'tnvse_freetype_native_effects_balanced.pso',
    'tnvse_freetype_native_effects_high.pso'
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
    if ($shaderName -like 'tnvse_freetype_native_effects_*.pso' -and
        -not ($dump -match '\b0\.001(?:0+\d*)?\b')) {
        throw "$shaderName does not contain the hard-shadow epsilon"
    }
    if ($shaderName -like 'tnvse_freetype_native_effects_*.pso') {
        if (-not ($dump -match 'approximately\s+(\d+)\s+instruction slots used')) {
            throw "$shaderName does not report its instruction-slot count"
        }
        if ([int]$Matches[1] -gt 512) {
            throw "$shaderName exceeds the ps_3_0 512-instruction-slot limit"
        }
    }
}

Write-Host 'FreeType native shader contract verification succeeded.'

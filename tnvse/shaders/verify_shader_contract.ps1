param(
    [Parameter(Mandatory = $true)]
    [string]$Fxc,
    [Parameter(Mandatory = $true)]
    [string]$ShaderDirectory
)

$ErrorActionPreference = 'Stop'

$pixelSources = @(
    'freetype_native_original.hlsl',
    'freetype_native_coverage.hlsl',
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
if ($common -notmatch 'tileColor\.rgb\s*\*\s*layerColor\.rgb') {
    throw 'Native FreeType shader ABI does not multiply Tile RGB by layer RGB'
}
if ($common -notmatch 'coverage\s*\*\s*tileColor\.a\s*\*\s*layerColor\.a') {
    throw 'Native FreeType shader ABI does not multiply coverage by both alpha sources'
}
if ($common -match 'float4\s*\([^,]+\*[^,]*coverage') {
    throw 'Native FreeType shader ABI appears to premultiply RGB by coverage'
}

$effectsSource = Get-Content -LiteralPath (
    Join-Path $ShaderDirectory 'freetype_native_effects.hlsl') -Raw
if ($effectsSource -notmatch 'blur\s*<=\s*0\.001[\s\S]*?return\s+body\s*;') {
    throw 'Native hard SDF shadow does not bypass blur/power shaping at the runtime epsilon'
}
if ($effectsSource -notmatch 'NativeFontVanillaGlowFalloff[\s\S]*?exp2\s*\(') {
    throw 'Native SDF glow does not use the vanilla-style exponential falloff'
}
if ($effectsSource -notmatch 'outer\s*-\s*antialiasWidth[\s\S]*?outer\s*\+\s*antialiasWidth') {
    throw 'Native SDF glow does not feather its outer cutoff by the pixel footprint'
}

$shaderInputs = @(
    'freetype_native_common.hlsli',
    'freetype_native_vs.hlsl',
    'freetype_native_original.hlsl',
    'freetype_native_coverage.hlsl',
    'freetype_native_sdf.hlsl',
    'freetype_native_effects.hlsl'
) | ForEach-Object { Get-Item -LiteralPath (Join-Path $ShaderDirectory $_) }
$newestShaderSource = ($shaderInputs |
    Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 1).LastWriteTimeUtc

$compiledDirectory = Join-Path $ShaderDirectory 'compiled'
$vertexShader = 'tnvse_freetype_native_vs.vso'
$pixelShaders = @(
    'tnvse_freetype_native_original.pso',
    'tnvse_freetype_native_coverage.pso',
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
    throw "$vertexShader does not forward the per-layer COLOR0 modifier"
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
    if ($shaderName -ne 'tnvse_freetype_native_original.pso' -and
        -not ($instructions -match '\bdcl_color')) {
        throw "$shaderName does not consume the native packet COLOR0 modifier"
    }
    if (-not ($instructions -cmatch '\bc0\b')) {
        throw "$shaderName does not read the Tile color c0"
    }
    if ($shaderName -like 'tnvse_freetype_native_effects_*.pso' -and
        -not ($dump -match '\b0\.001(?:0+\d*)?\b')) {
        throw "$shaderName does not contain the hard-shadow epsilon"
    }
    if ($shaderName -eq 'tnvse_freetype_native_coverage.pso') {
        $textureSamples = @($instructions | Where-Object {
            $_ -match '^\s+texld(?:\s|_)'
        }).Count
        if ($textureSamples -ne 1) {
            throw "$shaderName is not a single-sample coverage shader"
        }
    }
}

Write-Host 'FreeType native shader contract verification succeeded.'

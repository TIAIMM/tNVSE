param(
    [Parameter(Mandatory = $true)]
    [string]$Fxc,
    [Parameter(Mandatory = $true)]
    [string]$ShaderDirectory
)

$ErrorActionPreference = 'Stop'

$tileSources = @(
    'freetype_a8.hlsl',
    'freetype_effects.hlsl',
    'freetype_coverage.hlsl'
)
foreach ($sourceName in $tileSources) {
    $sourcePath = Join-Path $ShaderDirectory $sourceName
    $source = Get-Content -LiteralPath $sourcePath -Raw
    if ($source -notmatch '#include\s+"freetype_tile_compat\.hlsli"') {
        throw "$sourceName does not include freetype_tile_compat.hlsli"
    }
}
$sdfSources = @(
    'freetype_a8.hlsl',
    'freetype_effects.hlsl'
)
foreach ($sourceName in $sdfSources) {
    $sourcePath = Join-Path $ShaderDirectory $sourceName
    $source = Get-Content -LiteralPath $sourcePath -Raw
    if ($source -notmatch '#include\s+"freetype_sdf_compat\.hlsli"') {
        throw "$sourceName does not include freetype_sdf_compat.hlsli"
    }
}

$effectsSource = Get-Content -LiteralPath (
    Join-Path $ShaderDirectory 'freetype_effects.hlsl') -Raw
if ($effectsSource -notmatch 'blur\s*<=\s*0\.001[\s\S]*?return\s+body\s*;') {
    throw 'Hard SDF shadow does not bypass blur/power shaping at the runtime epsilon'
}

$compatPath = Join-Path $ShaderDirectory 'freetype_tile_compat.hlsli'
$compat = Get-Content -LiteralPath $compatPath -Raw
if ($compat -notmatch 'tileColor\.rgb\s*\*\s*layerColor\.rgb') {
    throw 'Shared FreeType shader ABI does not multiply c0.rgb by c1.rgb'
}
if ($compat -notmatch 'coverage\s*\*\s*tileColor\.a\s*\*\s*layerColor\.a') {
    throw 'Shared FreeType shader ABI does not multiply coverage by c0.a and c1.a'
}
if ($compat -match 'float4\s*\([^,]+\*[^,]*coverage') {
    throw 'Shared FreeType shader ABI appears to premultiply RGB by coverage'
}
foreach ($sourceName in $tileSources) {
    $sourcePath = Join-Path $ShaderDirectory $sourceName
    $source = Get-Content -LiteralPath $sourcePath -Raw
    if ($source -match 'float4\s+\w+\s*:\s*COLOR0\s*;') {
        throw "$sourceName reads an unreliable COLOR0 vertex input"
    }
    if ($source -notmatch 'LayerColor\s*:\s*register\(c1\)') {
        throw "$sourceName does not bind the layer modifier to c1"
    }
}

$compiledDirectory = Join-Path $ShaderDirectory 'compiled'
$shaderInputs = @(
    'freetype_a8.hlsl',
    'freetype_effects.hlsl',
    'freetype_coverage.hlsl',
    'freetype_tile_compat.hlsli',
    'freetype_sdf_compat.hlsli'
) | ForEach-Object { Get-Item -LiteralPath (Join-Path $ShaderDirectory $_) }
$newestShaderSource = ($shaderInputs |
    Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 1).LastWriteTimeUtc
$shaders = @(
    'tnvse_freetype_a8.pso',
    'tnvse_freetype_coverage.pso',
    'tnvse_freetype_effects_fast.pso',
    'tnvse_freetype_effects_balanced.pso',
    'tnvse_freetype_effects_high.pso'
)
foreach ($shaderName in $shaders) {
    $shaderPath = Join-Path $compiledDirectory $shaderName
    if ((Get-Item -LiteralPath $shaderPath).LastWriteTimeUtc -lt $newestShaderSource) {
        throw "$shaderName is older than its HLSL/include inputs"
    }
    $dump = & $Fxc /nologo /dumpbin $shaderPath 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "fxc /dumpbin failed for $shaderName"
    }
    if (-not ($dump -match '^\s+ps_3_0\s*$')) {
        throw "$shaderName is not ps_3_0"
    }

    $instructions = @($dump | Where-Object { $_ -match '^\s+[a-z]' })
    if ($instructions -match '\bdcl_color') {
        throw "$shaderName declares a COLOR0 vertex input"
    }
    if (-not ($instructions -cmatch '\bc0\b')) {
        throw "$shaderName does not read the original Tile color c0"
    }
    if (-not ($instructions -cmatch '\bc1\b')) {
        throw "$shaderName does not read the per-layer color c1"
    }
    if (-not ($instructions -cmatch '\bc0\.w\b') -or
        -not ($instructions -cmatch '\bc1\.w\b')) {
        throw "$shaderName output alpha is not derived from both c0.a and c1.a"
    }
    if ($shaderName -like 'tnvse_freetype_effects_*.pso' -and
        -not ($dump -match '\b0\.001(?:0+\d*)?\b')) {
        throw "$shaderName does not contain the hard-shadow epsilon"
    }
    if ($shaderName -eq 'tnvse_freetype_coverage.pso') {
        $textureSamples = @($instructions | Where-Object {
            $_ -match '^\s+texld(?:\s|_)'
        }).Count
        if ($textureSamples -ne 1) {
            throw "$shaderName is not a single-sample coverage shader"
        }
        if ($instructions -match '\b(?:dsx|dsy|ifc|loop)\b') {
            throw "$shaderName contains SDF/effect control-flow instructions"
        }
    }
}

Write-Host 'FreeType shader contract tile-fill-effect-rgb-v7 verification succeeded.'

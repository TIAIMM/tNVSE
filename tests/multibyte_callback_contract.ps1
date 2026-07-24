param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'

function Get-SourceSection {
    param(
        [string]$Text,
        [string]$StartMarker,
        [string]$EndMarker,
        [string]$Label
    )

    $start = $Text.IndexOf($StartMarker, [StringComparison]::Ordinal)
    if ($start -lt 0) {
        throw "Missing start marker for $Label`: $StartMarker"
    }

    $end = $Text.IndexOf($EndMarker, $start, [StringComparison]::Ordinal)
    if ($end -lt 0) {
        throw "Missing end marker for $Label`: $EndMarker"
    }

    return $Text.Substring($start, $end - $start)
}

function Remove-CxxComments {
    param([string]$Text)

    $withoutBlocks = [regex]::Replace(
        $Text,
        '/\*.*?\*/',
        '',
        [Text.RegularExpressions.RegexOptions]::Singleline)
    return [regex]::Replace(
        $withoutBlocks,
        '//.*$',
        '',
        [Text.RegularExpressions.RegexOptions]::Multiline)
}

function Assert-NoForbiddenCall {
    param(
        [string]$Text,
        [string[]]$Names,
        [string]$Label
    )

    foreach ($name in $Names) {
        if ($Text -match "\b$([regex]::Escape($name))\b") {
            throw "$Label contains forbidden callback operation: $name"
        }
    }
}

$inputRoot = Join-Path $SourceRoot 'tnvse\Src\input'
$wndProcPath = Join-Path $inputRoot 'multibyte_input_wndproc.cpp'
$tsfPath = Join-Path $inputRoot 'multibyte_input_tsf.cpp'
$menuSearchPath = Join-Path $inputRoot 'multibyte_input_menu_search.cpp'
$stewiePath = Join-Path $inputRoot 'multibyte_input_stewie.cpp'
$mainPath = Join-Path $inputRoot 'multibyte_input.cpp'

$wndProcSource = Get-Content -LiteralPath $wndProcPath -Raw
$wndProcCallback = Get-SourceSection `
    $wndProcSource `
    'LRESULT CALLBACK MultibyteInputWndProc(' `
    'void PumpCapturedInputEvents()' `
    'MultibyteInputWndProc'
$wndProcCode = Remove-CxxComments $wndProcCallback

$mouseBranch = $wndProcCode.IndexOf(
    'if (IsMouseRoutingMessage(msg))',
    [StringComparison]::Ordinal)
$captureAllowlist = $wndProcCode.IndexOf(
    'if (!IsPotentialInputCaptureMessage(msg))',
    [StringComparison]::Ordinal)
$stateRead = $wndProcCode.IndexOf(
    'ImeState& state = State();',
    [StringComparison]::Ordinal)
if ($mouseBranch -lt 0 -or $captureAllowlist -lt 0 -or $stateRead -lt 0) {
    throw 'WndProc is missing the mouse-first, capture-allowlist, or cached-state boundary.'
}
if ($mouseBranch -gt $captureAllowlist -or $captureAllowlist -gt $stateRead) {
    throw 'WndProc must forward mouse and non-capture traffic before reading input state.'
}
if ($wndProcCode -notmatch
    'if\s*\(IsMouseRoutingMessage\(msg\)\)\s*return\s+ForwardWindowMessage') {
    throw 'Mouse routing must return directly to the existing WndProc chain.'
}

Assert-NoForbiddenCall $wndProcCode @(
    'GetOpenMenu',
    'GetActiveTextEditMenu',
    'GetActiveJipTextInputMenu',
    'GetActiveStewieInputTarget',
    'GetOverlayTextInputMenu',
    'GetOverlayStewieInputTarget',
    'GetOverlayMcmExtenderInputTarget',
    'GetOverlayDialogueHistoryInputTarget',
    'GetAnyActiveTextInputMenu',
    'HasOverlayInputTarget',
    'FindTileByID',
    'TileTreeContains',
    'TryInstallJipTextInputHook',
    'TryInstallStewieTweaksInputHooks',
    'ProcessStewieTweaksInputTargetState',
    'ProcessStewieMenuSearchPendingStateSync',
    'UpdateCandidateOverlay',
    'DrawCandidateOverlay',
    'HideCandidateOverlay',
    'SetGameImeEnabled',
    'HideSystemImeWindows',
    'SafeWrite32',
    'WriteRelCall',
    'WriteRelJump',
    'DebugLog',
    'DebugLogState',
    'gLog'
) 'MultibyteInputWndProc'

$tsfSource = Get-Content -LiteralPath $tsfPath -Raw
$null = Get-SourceSection `
    $tsfSource `
    'STDMETHODIMP BeginUIElement' `
    'bool Initialize()' `
    'TSF UI element callbacks'
$tsfCode = Remove-CxxComments $tsfSource
Assert-NoForbiddenCall $tsfCode @(
    'GetOpenMenu',
    'GetActiveTextEditMenu',
    'GetOverlayTextInputMenu',
    'GetOverlayStewieInputTarget',
    'GetOverlayMcmExtenderInputTarget',
    'GetOverlayDialogueHistoryInputTarget',
    'HasOverlayInputTarget',
    'FindTileByID',
    'TileTreeContains',
    'TryInstallJipTextInputHook',
    'TryInstallStewieTweaksInputHooks',
    'UpdateCandidateOverlay',
    'DrawCandidateOverlay',
    'HideCandidateOverlay',
    'SafeWrite32',
    'WriteRelCall',
    'WriteRelJump',
    'DebugLog',
    'gLog'
) 'TSF UI element callback'

$menuSearchSource = Get-Content -LiteralPath $menuSearchPath -Raw
if ($menuSearchSource -match
    '\b(?:TileReadXML|TryInstallTileReadXMLHook|WriteRelJump|VirtualAlloc)\b') {
    throw 'MenuSearch must not install a Tile::ReadXML/global jump hook.'
}
$menuWrites = [regex]::Matches($menuSearchSource, 'SafeWrite32\s*\(([^;]+)\);')
$menuWriteValid = $menuWrites.Count -eq 1
if ($menuWriteValid) {
    $menuWriteValid = $menuWrites[0].Groups[1].Value -match
        'hook\.entry\s*,\s*hook\.hook'
}
if (-not $menuWriteValid) {
    throw 'MenuSearch writes must be limited to its HandleKeyboardInput slot.'
}

$stewieSource = Get-Content -LiteralPath $stewiePath -Raw
$stewieWrites = [regex]::Matches($stewieSource, 'SafeWrite32\s*\(([^;]+)\);')
$stewieWriteValid = $stewieWrites.Count -eq 1
if ($stewieWriteValid) {
    $stewieWriteValid = $stewieWrites[0].Groups[1].Value -match
        'entry\s*,\s*hook'
}
if (-not $stewieWriteValid) {
    throw 'StewMenu writes must be limited to its instance HandleKeyboardInput slot.'
}

$mainSource = Get-Content -LiteralPath $mainPath -Raw
foreach ($requiredMainLoopCall in @(
    'TryInstallJipTextInputHook();',
    'TryInstallStewieTweaksInputHooks();',
    'ProcessStewieTweaksInputTargetState();',
    'ProcessStewieMenuSearchPendingStateSync();',
    'ProcessMcmExtenderInputTargetState();',
    'ProcessDialogueHistoryInputTargetState();',
    'PumpCapturedInputEvents();'
)) {
    if ($mainSource.IndexOf(
            $requiredMainLoopCall,
            [StringComparison]::Ordinal) -lt 0) {
        throw "Main-loop ownership is missing: $requiredMainLoopCall"
    }
}

Write-Host 'Multibyte input callback contract verification succeeded.'

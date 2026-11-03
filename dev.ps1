param([switch]$BuildOnly,[switch]$VerifyOnly)
$ErrorActionPreference = 'Stop'
if ($BuildOnly -and $VerifyOnly) { throw 'Choose either BuildOnly or VerifyOnly.' }
$devRoot = Join-Path $PSScriptRoot 'Build Output\dev'
New-Item -ItemType Directory -Path $devRoot -Force | Out-Null
$buildLock = $null
try {
    $buildLock = [IO.File]::Open((Join-Path $devRoot 'build.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $helper = Join-Path $PSScriptRoot 'Source Code\tools\dev-run.js'
    if (-not $BuildOnly) {
        & node $helper check
        if ($LASTEXITCODE -ne 0) { throw 'Close the previous development VM before launching another.' }
    }
    $buildId = 'dev-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
    $runRoot = Join-Path $devRoot ('runs\' + $buildId)
    & node $helper prepare $runRoot $buildId
    if ($LASTEXITCODE -ne 0) { throw 'Build preparation failed.' }
    & (Join-Path $PSScriptRoot 'Source Code\build-release.ps1') -OutputDirectory $runRoot -BuildId $buildId
    if ($LASTEXITCODE -ne 0) { throw 'Build failed. No VM was started.' }
    if ($BuildOnly) {
        & node $helper record $runRoot
    } elseif ($VerifyOnly) {
        & node $helper run $runRoot --verify-only
    } else {
        & node $helper run $runRoot
    }
    if ($LASTEXITCODE -ne 0) { throw 'Development build or boot verification failed. See the run directory above.' }
} finally {
    if ($buildLock) { $buildLock.Dispose() }
}

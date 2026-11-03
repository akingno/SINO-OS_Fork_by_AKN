param(
    [string]$Command,
    [int]$ExpectedExitCode = 0,
    [switch]$SelfTest,
    [ValidateSet('transport', 'commands')][string]$Suite = 'transport',
    [ValidateRange(1, 120)][int]$TimeoutSeconds = 15
)
$ErrorActionPreference = 'Stop'
if ($SelfTest -and $PSBoundParameters.ContainsKey('Command')) { throw 'Choose Command or SelfTest.' }
if (-not $SelfTest -and -not $PSBoundParameters.ContainsKey('Command')) { throw 'Provide -Command or -SelfTest.' }
if (-not $SelfTest -and ([Text.Encoding]::UTF8.GetByteCount($Command) -gt 255 -or $Command -match "[\x00\r\n]")) {
    throw 'Command must be one line of at most 255 UTF-8 bytes.'
}
$devRoot = Join-Path $PSScriptRoot 'Build Output\dev'
New-Item -ItemType Directory -Path $devRoot -Force | Out-Null
$buildLock = $null
try {
    # Keep this checkout's shared intermediates locked until verification ends.
    $buildLock = [IO.File]::Open((Join-Path $devRoot 'build.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $buildId = 'shell-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
    $runRoot = Join-Path $devRoot ('runs\' + $buildId)
    $helper = Join-Path $PSScriptRoot 'Source Code\tools\dev-run.js'
    & node $helper prepare $runRoot $buildId
    if ($LASTEXITCODE -ne 0) { throw 'Build preparation failed.' }
    & (Join-Path $PSScriptRoot 'Source Code\build-release.ps1') -OutputDirectory $runRoot -BuildId $buildId -ShellBridge
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    & node $helper record $runRoot
    if ($LASTEXITCODE -ne 0) { throw 'Build identity verification failed.' }
    $job = @{ command=$Command; expectedExitCode=$ExpectedExitCode; selfTest=[bool]$SelfTest; suite=$Suite; timeoutMs=$TimeoutSeconds*1000 }
    $job | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runRoot 'shell-job.json') -Encoding UTF8
    & node (Join-Path $PSScriptRoot 'Source Code\tools\shell-test.js') $runRoot
    $testExit = $LASTEXITCODE
    if ($testExit -ne 0) { exit $testExit }
} finally {
    if ($buildLock) { $buildLock.Dispose() }
}

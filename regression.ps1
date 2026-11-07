param([string]$Suite = 'core', [switch]$List, [switch]$Full)
$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot 'Source Code\tests\host\test-core.js'
$planArguments = @('--validate', $Suite)
if ($List) { $planArguments[0] = '--list' }
if ($Full) { $planArguments += '--full' }
& node $runner @planArguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($List) { exit 0 }
$devRoot = Join-Path $PSScriptRoot 'Build Output\dev'
New-Item -ItemType Directory -Path $devRoot -Force | Out-Null
$buildLock = $null
$runRoot = $null
$result = 1
try {
    # Keep the shared build and matching symbols stable for the entire gate.
    $buildLock = [IO.File]::Open((Join-Path $devRoot 'build.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $buildId = 'regression-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
    $runRoot = Join-Path $devRoot ('runs\' + $buildId)
    $helper = Join-Path $PSScriptRoot 'Source Code\tools\dev-run.js'
    & node $helper prepare $runRoot $buildId
    if ($LASTEXITCODE -ne 0) { throw 'Regression build preparation failed.' }
    & (Join-Path $PSScriptRoot 'Source Code\build-release.ps1') -OutputDirectory $runRoot -BuildId $buildId -ShellBridge
    if ($LASTEXITCODE -ne 0) { throw 'Regression build failed; no stale image will be tested.' }
    & node $helper record $runRoot
    if ($LASTEXITCODE -ne 0) { throw 'Regression build identity verification failed.' }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Source Code\build32\kernel-normal.exe') -Destination (Join-Path $runRoot 'kernel-normal.exe')
    $testArguments = @($runRoot, $Suite)
    if ($Full) { $testArguments += '--full' }
    & node $runner @testArguments
    $result = $LASTEXITCODE
} catch {
    if ($runRoot) {
        New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
        $reportFile = Join-Path $runRoot 'regression-report.json'
        if (-not (Test-Path -LiteralPath $reportFile)) {
            @{version=4; suite=$Suite; result='FAIL'; stage='build'; error=$_.Exception.Message; suites=@(); testsExecuted=$false} |
                ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 -LiteralPath $reportFile
        }
        Write-Host ('Report: ' + $reportFile)
    }
    Write-Host $_.Exception.Message
    $result = 1
} finally {
    if ($buildLock) { $buildLock.Dispose() }
}
exit $result

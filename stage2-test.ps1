param()
$ErrorActionPreference = 'Stop'
$devRoot = Join-Path $PSScriptRoot 'Build Output\dev'
New-Item -ItemType Directory -Path $devRoot -Force | Out-Null
$buildLock = $null
$runRoot = $null
$result = 1
try {
    # Keep the shared build and matching symbols stable for the entire gate.
    $buildLock = [IO.File]::Open((Join-Path $devRoot 'build.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $buildId = 'stage2-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
    $runRoot = Join-Path $devRoot ('runs\' + $buildId)
    $helper = Join-Path $PSScriptRoot 'Source Code\tools\dev-run.js'
    & node $helper prepare $runRoot $buildId
    if ($LASTEXITCODE -ne 0) { throw 'Stage 2 build preparation failed.' }
    & (Join-Path $PSScriptRoot 'Source Code\build-release.ps1') -OutputDirectory $runRoot -BuildId $buildId -ShellBridge
    if ($LASTEXITCODE -ne 0) { throw 'Stage 2 build failed; no stale image will be tested.' }
    & node $helper record $runRoot
    if ($LASTEXITCODE -ne 0) { throw 'Stage 2 build identity verification failed.' }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Source Code\build32\kernel-normal.exe') -Destination (Join-Path $runRoot 'kernel-normal.exe')
    & node (Join-Path $PSScriptRoot 'Source Code\tools\stage2-test.js') $runRoot
    $result = $LASTEXITCODE
} catch {
    if ($runRoot) {
        New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
        $reportFile = Join-Path $runRoot 'stage2-report.json'
        if (-not (Test-Path -LiteralPath $reportFile)) {
            @{version=1; result='FAIL'; stage='build'; error=$_.Exception.Message; suites=@(); testsExecuted=$false} |
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

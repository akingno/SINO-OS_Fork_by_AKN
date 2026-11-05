param(
    [Parameter(Mandatory=$true)][string]$Program,
    [string[]]$Arguments = @(),
    [string]$InputFile,
    [int]$ExpectedExitCode = 0,
    [ValidateRange(1,600)][int]$TimeoutSeconds = 5
)
$ErrorActionPreference = 'Stop'
$utf8 = [Text.Encoding]::UTF8
if (-not $Program.StartsWith('/') -or $Program.Contains([char]0) -or $utf8.GetByteCount($Program) -ge 128) { throw 'Program must be an absolute guest path shorter than 128 UTF-8 bytes.' }
if ($Arguments.Count -gt 16) { throw 'At most 16 arguments are supported.' }
$argumentBytes = 0
foreach ($arg in $Arguments) {
    if ($null -eq $arg -or $arg.Contains([char]0)) { throw 'Arguments must be strings without embedded NUL.' }
    $argumentBytes += $utf8.GetByteCount($arg) + 1
}
if ($argumentBytes -gt 1024) { throw 'Arguments exceed the 1024-byte packed limit.' }
$inputBytes = [byte[]]@()
if ($InputFile) {
    $stream = [IO.File]::OpenRead((Resolve-Path -LiteralPath $InputFile).Path)
    try {
        if ($stream.Length -gt 4096) { throw 'Input exceeds the 4096-byte limit.' }
        $buffer = New-Object byte[] 4097
        $used = 0
        while ($used -lt $buffer.Length) {
            $count = $stream.Read($buffer, $used, $buffer.Length - $used)
            if (-not $count) { break }
            $used += $count
        }
        if ($used -gt 4096) { throw 'Input exceeds the 4096-byte limit.' }
        $inputBytes = New-Object byte[] $used
        [Array]::Copy($buffer, $inputBytes, $used)
    } finally { $stream.Dispose() }
}
if ($inputBytes.Length -gt 4096) { throw 'Input exceeds the 4096-byte limit.' }
$devRoot = Join-Path $PSScriptRoot 'Build Output\dev'
New-Item -ItemType Directory -Force -Path $devRoot | Out-Null
$buildLock = $null
$runRoot = $null
$result = 1
try {
    $buildLock = [IO.File]::Open((Join-Path $devRoot 'build.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $buildId = 'cli-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
    $runRoot = Join-Path $devRoot ('runs\' + $buildId)
    $helper = Join-Path $PSScriptRoot 'Source Code\tools\dev-run.js'
    & node $helper prepare $runRoot $buildId
    if ($LASTEXITCODE -ne 0) { throw 'CLI build preparation failed.' }
    & (Join-Path $PSScriptRoot 'Source Code\build-release.ps1') -OutputDirectory $runRoot -BuildId $buildId -ShellBridge
    if ($LASTEXITCODE -ne 0) { throw 'CLI build failed; no stale image is tested.' }
    & node $helper record $runRoot
    if ($LASTEXITCODE -ne 0) { throw 'CLI build identity verification failed.' }
    @{program=$Program; arguments=@($Arguments); inputBase64=[Convert]::ToBase64String($inputBytes); expectedExitCode=$ExpectedExitCode; timeoutTicks=$TimeoutSeconds*100} |
        ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $runRoot 'run-job.json')
    @{selfTest=$true; suite='managed-cli'; timeoutMs=15000} | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $runRoot 'shell-job.json')
    & node (Join-Path $PSScriptRoot 'Source Code\tools\run-test.js') $runRoot
    $result = $LASTEXITCODE
} catch {
    if ($runRoot) {
        New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
        $report = Join-Path $runRoot 'cli-report.json'
        if (-not (Test-Path -LiteralPath $report)) {
            @{result='FAIL'; stage='build'; error=$_.Exception.Message; testsExecuted=$false} |
                ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath $report
        }
        Write-Host ('Report: ' + $report)
    }
    Write-Host $_.Exception.Message
    $result = 1
} finally { if ($buildLock) { $buildLock.Dispose() } }
exit $result

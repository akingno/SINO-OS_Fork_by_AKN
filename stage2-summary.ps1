param([Parameter(Mandatory=$true)][string]$RunDirectory)
$ErrorActionPreference = 'Stop'
& node (Join-Path $PSScriptRoot 'Source Code/tests/host/stage2-summary.js') $RunDirectory
exit $LASTEXITCODE

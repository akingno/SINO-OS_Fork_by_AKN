param([Parameter(Mandatory=$true)][string]$RunDirectory)
$ErrorActionPreference = 'Stop'
& node (Join-Path $PSScriptRoot 'Source Code/tests/host/test-summary.js') $RunDirectory
exit $LASTEXITCODE

param([Parameter(Mandatory=$true)][string]$RunDirectory)
$ErrorActionPreference = 'Stop'
& node (Join-Path $PSScriptRoot 'Source Code/tools/stage2-summary.js') $RunDirectory
exit $LASTEXITCODE

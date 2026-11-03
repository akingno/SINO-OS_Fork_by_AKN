param([switch]$TestImage,[string]$BuildId)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build32.ps1') -TestImage:$TestImage -BuildId $BuildId

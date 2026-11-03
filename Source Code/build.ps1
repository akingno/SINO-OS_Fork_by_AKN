param([switch]$TestImage,[string]$BuildId,[switch]$ShellBridge)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build32.ps1') -TestImage:$TestImage -BuildId $BuildId -ShellBridge:$ShellBridge

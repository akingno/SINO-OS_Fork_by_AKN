param([switch]$TestImage,[string]$BuildId,[switch]$ShellBridge)
$ErrorActionPreference = 'Stop'
if ($BuildId -and $BuildId -notmatch '^[A-Za-z0-9._-]{1,64}$') { throw 'BuildId must contain 1-64 ASCII letters, digits, dots, underscores or hyphens.' }
$taskRoot = $PSScriptRoot
$localePython = if ($env:SINO_PYTHON) { $env:SINO_PYTHON } else { 'python' }
& $localePython (Join-Path $taskRoot 'tools/build-locale.py')
if ($LASTEXITCODE -ne 0) { throw 'Locale resource validation failed.' }
$nasmPath = Join-Path $taskRoot 'tools\nasm-3.02\nasm.exe'
$toolRoot = $env:SINO_TOOLCHAIN
if (-not $toolRoot) {
    $gccCommand = Get-Command gcc -ErrorAction SilentlyContinue
    if ($gccCommand) { $toolRoot = Split-Path -Parent $gccCommand.Source }
    elseif (Test-Path -LiteralPath 'Z:\LunaMakerDebug\w64devkit\bin\gcc.exe') { $toolRoot = 'Z:\LunaMakerDebug\w64devkit\bin' }
    else { throw 'Set SINO_TOOLCHAIN to the bin folder of w64devkit / MinGW GCC with 32-bit code generation.' }
}
$gcc = Join-Path $toolRoot 'g++.exe'
$ld = Join-Path $toolRoot 'ld.exe'
$objcopy = Join-Path $toolRoot 'objcopy.exe'
$env:SINO_TOOLCHAIN = $toolRoot
$buildPath = Join-Path $taskRoot 'build32'
New-Item -ItemType Directory -Path $buildPath -Force | Out-Null
if ($BuildId) {
    Set-Content -LiteralPath (Join-Path $buildPath 'build-id.h') -Encoding ASCII -Value ('#define SINO_BUILD_ID "' + $BuildId + '"')
}
$sourcePath = Join-Path $taskRoot 'src32'
$bootDefines = @(Get-Content (Join-Path $sourcePath 'boot-layout.h') | ForEach-Object { if ($_ -match '^#define (BOOT_\w+) (\d+)$') { '-D' + $Matches[1] + '=' + $Matches[2] } })
$bootSources = @('boot','loader','mbr')
if ($TestImage -or $ShellBridge) { $bootSources += 'hello' }
foreach ($source in $bootSources) {
    & $nasmPath @bootDefines -f bin -Wall -Werror -w-reloc-abs-word -w-reloc-abs-dword (Join-Path $sourcePath "$source.asm") -o (Join-Path $buildPath "$source.bin") -l (Join-Path $buildPath "$source.lst")
    if ($LASTEXITCODE -ne 0) { throw "Assembly failed: $source" }
}
# COFF relocations are resolved by the linker below.
& $nasmPath -f win32 -Wall -Werror -w-reloc-abs-dword -w-reloc-rel-dword (Join-Path $sourcePath 'entry.asm') -o (Join-Path $buildPath 'entry.o')
if ($LASTEXITCODE -ne 0) { throw '32-bit entry assembly failed' }
& node (Join-Path $taskRoot 'tools/build-user.js')
if ($LASTEXITCODE -ne 0) { throw 'User application build failed' }
$variants = @('normal')
if ($TestImage) { $variants += 'test' }
$kernelSources = @('kernel','lib/kstring','panic','fs/boot_fat12','mm/page','mm/test_fault','mm/paging','mm/kmap','mm/uaccess','klog','locale','rtc','keyboard','object','process','filesys','pipe','terminal','run_bridge','net','console','vfs','shell_bridge','storage','disk')
foreach ($variant in $variants) {
    # The kernel owns its mapped stack; Windows stack-guard probing is inapplicable.
    $cflags = @('-m32','-march=i386','-std=c++17','-Os','-ffreestanding','-fno-builtin','-fno-exceptions','-fno-rtti','-fno-threadsafe-statics','-fno-pic','-fno-pie','-fno-stack-protector','-fno-asynchronous-unwind-tables','-mno-stack-arg-probe','-mno-sse','-mno-mmx','-msoft-float','-mpreferred-stack-boundary=2','-mincoming-stack-boundary=2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-register','-Wno-missing-field-initializers')
    if ($variant -eq 'test') { $cflags += '-DSELFTEST=1' }
    if ($ShellBridge) { $cflags += '-DSINO_SHELL_BRIDGE=1' }
    if ($variant -eq 'test' -and $ShellBridge) { $cflags += '-DSINO_KMAP_FORCE_SLOT=1' }
    if ($BuildId) { $cflags += @('-include',(Join-Path $buildPath 'build-id.h')) }
    $kernelObjects = @()
    foreach ($source in $kernelSources) {
        $object = ($source -replace '/', '-')
        $objectPath = Join-Path $buildPath "$object-$variant.o"
        & $gcc @cflags -c (Join-Path $sourcePath "$source.cpp") -o $objectPath
        if ($LASTEXITCODE -ne 0) { throw "C compilation failed: $source ($variant)" }
        $kernelObjects += $objectPath
    }
    & $ld -m i386pe --image-base 0x10000 --file-alignment 0x200 --section-alignment 0x1000 --disable-reloc-section --disable-dynamicbase --no-insert-timestamp -T (Join-Path $sourcePath 'linker.ld') (Join-Path $buildPath 'entry.o') @kernelObjects -Map (Join-Path $buildPath "kernel-$variant.map") -o (Join-Path $buildPath "kernel-$variant.exe")
    if ($LASTEXITCODE -ne 0) { throw '32-bit kernel link failed' }
    & $objcopy -O binary (Join-Path $buildPath "kernel-$variant.exe") (Join-Path $buildPath "kernel-$variant.bin")
    if ($LASTEXITCODE -ne 0) { throw 'Flat kernel extraction failed' }
    & node (Join-Path $taskRoot 'build-image32.js') $variant $ShellBridge.IsPresent
    if ($LASTEXITCODE -ne 0) { throw '32-bit floppy image creation failed' }
}

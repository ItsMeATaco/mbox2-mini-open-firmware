<#
.SYNOPSIS
    Builds the open firmware (firmware\) on this PC with SDCC and writes the EEPROM image to
    build\firmware\mbox2-open.bin. With -Install, also installs it on the Mbox over USB (usbeeprom.py,
    through the firmware's WinUSB maintenance interface).
.DESCRIPTION
    The same steps as firmware\build.sh on the Pi: compile each firmware\src\*.c, link with main.rel
    first, wrap with firmware\mkimage.py. Needs SDCC 4.5 (the Pi's version, so both build the same
    image), found on PATH or in C:\Program Files\SDCC\bin, and Python. The header's version byte comes
    from DEVICE_RELEASE in descriptors.c (0x0022 -> 22) unless -Version is given.
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\firmware\build.ps1
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\firmware\build.ps1 -Install
#>
param(
    [int]$Version = -1,
    [switch]$Install,
    [string]$BootCpt,          # bring-up: 8 hex bytes as for fwdebug.py cpt, baked in as the boot default
    [int]$DmaMode = -1         # 0-3: DMA buffer mode at boot (1 playback, 2 capture wrap-around); default: audio.c's
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\.."
$src = Join-Path $root 'firmware\src'
$out = Join-Path $root 'build\firmware\out'
$image = Join-Path $root 'build\firmware\mbox2-open.bin'

$sdcc = (Get-Command sdcc -ErrorAction SilentlyContinue).Source
if (-not $sdcc) { $sdcc = Join-Path $env:ProgramFiles 'SDCC\bin\sdcc.exe' }
if (-not (Test-Path $sdcc)) { throw "SDCC not found (PATH or $sdcc). Install SDCC 4.5.0 for Windows (sdcc.sourceforge.net)." }
$sdccBin = Split-Path $sdcc
$env:PATH = "$sdccBin;$env:PATH"                     # its DLLs, for the helper below too
# SDCC 4.5.0's Windows installer ships the preprocessor's cc1 without .exe, so sdcpp can't start it
# ("cannot execute 'cc1'"). Keep a correctly named copy in build\ and point sdcpp at it, rather than
# changing the installation.
$cc1 = Join-Path $sdccBin 'cc1'
if ((Test-Path $cc1) -and -not (Test-Path "$cc1.exe")) {
    $fix = Join-Path $root 'build\firmware\sdcc-cc1'
    $copy = Join-Path $fix 'cc1.exe'
    if (-not (Test-Path $copy) -or (Get-Item $copy).Length -ne (Get-Item $cc1).Length) {
        New-Item -ItemType Directory -Force $fix | Out-Null
        Copy-Item $cc1 $copy -Force
    }
    $env:COMPILER_PATH = $fix
}
$sdccVersion = (& $sdcc --version 2>&1 | Select-Object -First 1)
if ($sdccVersion -notmatch '4\.5\.') { Write-Warning "SDCC is not 4.5 ($sdccVersion): the image may differ from the Pi's build." }

if ($Version -lt 0) {
    $m = Select-String -Path (Join-Path $src 'descriptors.c') -Pattern '#define\s+DEVICE_RELEASE\s+0x([0-9A-Fa-f]{4})'
    if (-not $m) { throw 'DEVICE_RELEASE not found in descriptors.c; pass -Version.' }
    $Version = [int]$m.Matches[0].Groups[1].Value.TrimStart('0')       # BCD 0x0022 -> 22
}

$cflags = @('-mmcs51', '--model-small', '--opt-code-size')
if ($BootCpt) { $cflags += '-DBOOT_CPT=' + (($BootCpt.Trim() -split '\s+' | ForEach-Object { "0x$_" }) -join ',') }
if ($DmaMode -ge 0) { $cflags += "-DBOOT_DMA_MODE=$DmaMode" }

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
# Link in build.sh's order (main.rel first: it holds main()), so this PC and the Pi build the same
# image byte for byte; any newer source file goes after them.
$linkOrder = 'main.c', 'descriptors.c', 'audio.c', 'diag.c'
$sources = Get-ChildItem (Join-Path $src '*.c') | Sort-Object { $i = [array]::IndexOf($linkOrder, $_.Name); if ($i -lt 0) { 99 } else { $i } }, Name
foreach ($f in $sources) {
    & $sdcc @cflags -c $f.FullName -o "$out/"           # not "$out\": the path has spaces, and \" would escape the quote
    if ($LASTEXITCODE -ne 0) { throw "Compiling $($f.Name) failed." }
}
$rels = $sources | ForEach-Object { Join-Path $out ($_.BaseName + '.rel') }
# No external RAM exists outside the chip's 2 KB register/buffer window.
& $sdcc @cflags --code-size 0x2000 --iram-size 256 --xram-size 0 @rels -o (Join-Path $out 'mbox2fw.ihx')
if ($LASTEXITCODE -ne 0) { throw 'Linking failed.' }
$mem = Join-Path $out 'mbox2fw.mem'
if (Test-Path $mem) {
    $lines = Get-Content $mem
    $i = ($lines | Select-String 'Internal RAM layout' | Select-Object -First 1).LineNumber
    if ($i) { $lines[($i - 1)..($lines.Count - 1)] | Select-Object -Last 12 }
}
& python (Join-Path $root 'firmware\mkimage.py') (Join-Path $out 'mbox2fw.ihx') $image --version $Version
if ($LASTEXITCODE -ne 0) { throw 'Making the image failed.' }
Write-Host "Built $image ($sdccVersion)" -ForegroundColor Green

if ($Install) {
    & python (Join-Path $root 'tools\firmware\usbeeprom.py') write $image
    exit $LASTEXITCODE
}

<#
.SYNOPSIS
    Installs the Mbox 2 Mini ASIO driver (mbox2asio.dll) so DAWs such as Ableton Live list it as
    "Mbox 2 Mini (open firmware)". Asks Windows for administrator rights for the last step.

.DESCRIPTION
    1. Builds mbox2asio.dll in build\ (cmake --build, Release), unless -NoBuild.
    2. Elevated (Windows shows its usual Yes/No prompt):
       - copies it to C:\Program Files\Mbox2Asio\, so rebuilding never touches the copy a DAW has loaded;
       - registers it with regsvr32, which writes two keys:
           HKCR\CLSID\{5FE5CF79-6F37-4AE6-8241-93CD35CD733F}  (the COM class: the DLL's path)
           HKLM\SOFTWARE\ASIO\Mbox 2 Mini (open firmware)      (the list DAWs read)
       - reads the keys back and prints them.
    The driver is an ordinary user-mode DLL: no kernel driver, no test signing, and Secure Boot,
    driver signature enforcement and Memory Integrity stay on. Close your DAW before reinstalling.
    Undo with tools\asio\uninstall.ps1.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\asio\install.ps1
#>
[CmdletBinding()]
param(
    [switch]$NoBuild,
    # Internal: the elevated half, given the built DLL.
    [string]$Elevated
)

$ErrorActionPreference = 'Stop'
$installDir = Join-Path $env:ProgramFiles 'Mbox2Asio'
$clsid = '{5FE5CF79-6F37-4AE6-8241-93CD35CD733F}'
$asioKey = 'HKLM:\SOFTWARE\ASIO\Mbox 2 Mini (open firmware)'

if ($Elevated) {
    try {
        New-Item -ItemType Directory -Force $installDir | Out-Null
        $target = Join-Path $installDir 'mbox2asio.dll'
        # Earlier copies a DAW still had loaded were renamed aside; delete the ones that are free now.
        Get-ChildItem $installDir -Filter 'mbox2asio.dll.old-*' | ForEach-Object {
            try { Remove-Item $_.FullName -Force } catch { }
        }
        try {
            Copy-Item $Elevated $target -Force
        } catch {
            # A loaded DLL can't be overwritten but can be renamed; the DAW keeps using the old copy until it restarts.
            Rename-Item $target ("mbox2asio.dll.old-{0:yyyyMMddHHmmss}" -f (Get-Date))
            Copy-Item $Elevated $target -Force
            Write-Host 'A program had the old driver loaded; restart it to use the new one.' -ForegroundColor Yellow
        }
        Write-Host "Copied to $target"

        $regsvr32 = Join-Path $env:SystemRoot 'System32\regsvr32.exe'
        $p = Start-Process $regsvr32 -ArgumentList '/s', "`"$target`"" -Wait -PassThru
        if ($p.ExitCode -ne 0) { throw "regsvr32 failed with exit code $($p.ExitCode)" }

        $server = (Get-ItemProperty "Registry::HKEY_CLASSES_ROOT\CLSID\$clsid\InprocServer32").'(default)'
        $asio = Get-ItemProperty $asioKey
        Write-Host "HKCR\CLSID\$clsid\InprocServer32 = $server"
        Write-Host "HKLM\SOFTWARE\ASIO\Mbox 2 Mini (open firmware): CLSID $($asio.CLSID), Description '$($asio.Description)'"
        if ($server -ne $target -or $asio.CLSID -ne $clsid) { throw 'The registry does not match what was installed.' }
        Write-Host "`nInstalled. In Ableton Live: Preferences > Audio > Driver Type ASIO, Audio Device 'Mbox 2 Mini (open firmware)'." -ForegroundColor Green
    } catch {
        Write-Host "Install failed: $_" -ForegroundColor Red
    }
    return
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$built = Join-Path $root 'build\src\asio\Release\mbox2asio.dll'
if (-not $NoBuild) {
    Write-Host 'Building mbox2asio.dll...'
    & cmake --build (Join-Path $root 'build') --config Release --target mbox2asio | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Build failed (configure first: cmake -S . -B build -G "Visual Studio 17 2022" -A x64).' }
}
if (-not (Test-Path $built)) { throw "$built not found; build it first." }

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    & $PSCommandPath -Elevated $built
} else {
    Write-Host "`nAsking Windows for administrator rights to install it..."
    Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-NoExit',
        '-File', "`"$PSCommandPath`"", '-Elevated', "`"$built`""
}

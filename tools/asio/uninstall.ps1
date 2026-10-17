<#
.SYNOPSIS
    Removes the Mbox 2 Mini ASIO driver installed by install.ps1: unregisters it (both registry keys)
    and deletes C:\Program Files\Mbox2Asio. Asks Windows for administrator rights. Close your DAW first.

    Left in place: the per-user log %LOCALAPPDATA%\Mbox2Asio\driver.log and any settings under
    HKCU\Software\Mbox2Asio (delete them by hand if you like).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\asio\uninstall.ps1
#>
[CmdletBinding()]
param(
    # Internal: the elevated half.
    [switch]$Elevated
)

$ErrorActionPreference = 'Stop'
$installDir = Join-Path $env:ProgramFiles 'Mbox2Asio'
$clsid = '{5FE5CF79-6F37-4AE6-8241-93CD35CD733F}'
$clsidKey = "Registry::HKEY_CLASSES_ROOT\CLSID\$clsid"
$asioKey = 'HKLM:\SOFTWARE\ASIO\Mbox 2 Mini (open firmware)'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'Asking Windows for administrator rights...'
    Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-NoExit',
        '-File', "`"$PSCommandPath`"", '-Elevated'
    return
}

try {
    $dll = Join-Path $installDir 'mbox2asio.dll'
    if (Test-Path $dll) {
        $regsvr32 = Join-Path $env:SystemRoot 'System32\regsvr32.exe'
        Start-Process $regsvr32 -ArgumentList '/u', '/s', "`"$dll`"" -Wait | Out-Null
    }
    # Whatever regsvr32 left (or if the DLL was already gone).
    foreach ($key in $clsidKey, $asioKey) {
        if (Test-Path $key) { Remove-Item $key -Recurse -Force }
    }
    Write-Host 'Registry keys removed.'
    if (Test-Path $installDir) {
        try {
            Remove-Item $installDir -Recurse -Force
            Write-Host "Deleted $installDir."
        } catch {
            Write-Host "Couldn't delete $installDir (a program still has the driver loaded). Close it and run this again." -ForegroundColor Yellow
        }
    }
    Write-Host 'Uninstalled.' -ForegroundColor Green
} catch {
    Write-Host "Uninstall failed: $_" -ForegroundColor Red
}

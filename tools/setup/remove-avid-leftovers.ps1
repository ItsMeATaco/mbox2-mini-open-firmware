<#
.SYNOPSIS
  Removes what Avid's Mbox 2 driver (8.0.4.354) leaves behind after "Avid Mbox 2 USB Drivers (x64)" is
  uninstalled, so Memory Integrity stops flagging dgmbx2fu.sys. Only for a Mbox running the open
  firmware (1209:0001), which uses Windows' own drivers.

.DESCRIPTION
  Run from PowerShell; it asks Windows for administrator rights, lists what it found and waits for
  Enter before removing anything:
    powershell -ExecutionPolicy Bypass -File tools\setup\remove-avid-leftovers.ps1

  Found on this PC on 2026-10-17, after the uninstall:
    - kernel services DGUSBAP (dgmbx2.sys) and MBX2DFU (dgmbx2fu.sys), stopped;
    - their files in System32\drivers, and System32\dgmbx2co80.dll;
    - Avid's ASIO driver "ASIO Digidesign Driver Mbox2" (Common Files\Digidesign\DAE, 64- and 32-bit),
      still registered, so DAWs list it;
    - device entries for the Mbox's old identity (USB\VID_0DBA&PID_3000), not present.

  To go back to Avid's firmware and driver later: write your firmware backup back (README, "Going
  back to Avid's firmware"), then reinstall Avid's driver.
#>
$ErrorActionPreference = 'Stop'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  # Relaunch elevated (Windows shows its usual Yes/No prompt); -NoExit keeps the results on screen.
  Write-Host 'Asking Windows for administrator rights...'
  Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-NoExit', '-File', "`"$PSCommandPath`""
  return
}

$sys32 = Join-Path $env:SystemRoot 'System32'
$asioClsid = '{CE3B9FA0-DD94-4960-B3B1-97AF976F92FA}'
$asioName = 'ASIO Digidesign Driver Mbox2'
$daeDirs = @("$env:ProgramFiles\Common Files\Digidesign", "${env:ProgramFiles(x86)}\Common Files\Digidesign")
$expectedDaeFiles = 'dgmbx2asio.dll', 'dgmbx2pasio.dll', 'mbox2.xml', 'le01.png'

# What is there
$services = @(foreach ($name in 'DGUSBAP', 'MBX2DFU') {
  $key = "HKLM:\SYSTEM\CurrentControlSet\Services\$name"
  if (Test-Path $key) {
    $image = (Get-ItemProperty $key).ImagePath
    if ($image -match 'dgmbx2(fu)?\.sys') { [pscustomobject]@{ Name = $name; Image = $image } }
    else { Write-Warning "Service $name points to $image, not Avid's driver: leaving it alone." }
  }
})
$files = @(@("$sys32\drivers\dgmbx2.sys", "$sys32\drivers\dgmbx2fu.sys", "$sys32\dgmbx2co80.dll") | Where-Object { Test-Path $_ })
$devices = @(Get-PnpDevice | Where-Object { $_.InstanceId -match '^USB\\VID_0DBA&PID_3000' -and $_.Status -ne 'OK' })
$present = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match '^USB\\VID_0DBA&PID_3000' })
$asioKeys = @(@('HKLM:\SOFTWARE\ASIO', 'HKLM:\SOFTWARE\WOW6432Node\ASIO') | ForEach-Object { "$_\$asioName" } | Where-Object { Test-Path $_ })
$clsidKeys = @(@('HKLM:\SOFTWARE\Classes\CLSID', 'HKLM:\SOFTWARE\WOW6432Node\Classes\CLSID') | ForEach-Object { "$_\$asioClsid" } | Where-Object { Test-Path $_ })
$dae = @(foreach ($d in $daeDirs) {
  if (Test-Path $d) {
    $other = @(Get-ChildItem $d -Recurse -File | Where-Object { $expectedDaeFiles -notcontains $_.Name })
    if ($other) { Write-Warning "$d holds other files too ($($other[0].Name) ...): leaving that folder alone." }
    else { $d }
  }
})

if ($present) {
  Write-Host "A Mbox with Avid's firmware is plugged in ($($present[0].InstanceId)); it still needs these. Nothing removed." -ForegroundColor Red
  return
}
if (-not ($services -or $files -or $devices -or $asioKeys -or $clsidKeys -or $dae)) {
  Write-Host "Nothing of Avid's Mbox 2 driver is left." -ForegroundColor Green
  return
}

Write-Host "Found these leftovers of Avid's Mbox 2 driver:" -ForegroundColor Cyan
$services | ForEach-Object { Write-Host "  service  $($_.Name)  ($($_.Image))" }
$files | ForEach-Object { Write-Host "  file     $_" }
$devices | ForEach-Object { Write-Host "  device   $($_.FriendlyName)  ($($_.InstanceId), not present)" }
$asioKeys + $clsidKeys | ForEach-Object { Write-Host "  ASIO     $_" }
$dae | ForEach-Object { Write-Host "  folder   $_" }
Read-Host "`nPress Enter to remove them all (Ctrl+C to cancel)" | Out-Null

# 1. Avid's ASIO driver: unregister through its own DLLs, then remove whatever registration is left.
foreach ($d in $dae) {
  $dll = Join-Path $d 'DAE\Devices\dgmbx2asio.dll'
  if (Test-Path $dll) {
    $regsvr = if ($d -like "${env:ProgramFiles(x86)}*") { "$env:SystemRoot\SysWOW64\regsvr32.exe" } else { "$sys32\regsvr32.exe" }
    & $regsvr /u /s $dll
    Write-Host "Unregistered $dll"
  }
}
foreach ($k in $asioKeys + $clsidKeys) { if (Test-Path $k) { Remove-Item $k -Recurse -Force; Write-Host "Removed $k" } }

# 2. Old device entries
foreach ($dev in $devices) {
  pnputil /remove-device "$($dev.InstanceId)" | Out-Null
  Write-Host "Removed device $($dev.FriendlyName)"
}

# 3. Kernel services (stopped; nothing loads them any more)
foreach ($s in $services) {
  sc.exe delete $s.Name | Out-Null
  Write-Host "Deleted service $($s.Name)"
}

# 4. Files
foreach ($f in $files) { Remove-Item $f -Force; Write-Host "Deleted $f" }
foreach ($d in $dae) { Remove-Item $d -Recurse -Force; Write-Host "Deleted $d" }

Write-Host "`nDone. Restart Windows, then turn on Windows Security > Device security > Core isolation >" -ForegroundColor Green
Write-Host "Memory integrity (it asks for one more restart)." -ForegroundColor Green

<#
.SYNOPSIS
    Builds everything in Release and collects the files of a GitHub release in dist\v<version>\.
.DESCRIPTION
    The version comes from project(... VERSION x.y.z) in CMakeLists.txt; the release's tag is v<version>, which
    Mbox Hub compares with its own version. The files:
      mbox2hub.exe, mbox2asio.dll, mbox2-open.bin   single files, which Mbox Hub's updater and the Raspberry Pi
                                                    installer (pi/flash.sh) download by these names
      Mbox2Mini-Windows.zip                         the same three plus remove-avid-leftovers.ps1 and a short
                                                    README.txt, for a first install
    With -Publish, also creates the release on GitHub (gh CLI) from those files and -NotesFile.
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\release\package.ps1
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\release\package.ps1 -Publish -NotesFile notes.md
#>
param(
    [switch]$Publish,
    [string]$NotesFile
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path

$cmakeLists = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmakeLists -notmatch 'project\(mbox2 VERSION (\d+\.\d+\.\d+)') { throw 'No project(mbox2 VERSION x.y.z) in CMakeLists.txt.' }
$version = $Matches[1]
$tag = "v$version"
$repository = 'ItsMeATaco/mbox2-mini-open-firmware'
if ($cmakeLists -match 'set\(MBOX2_REPOSITORY "([^"]+)"') { $repository = $Matches[1] }
if ($Publish -and -not (Test-Path $NotesFile)) { throw 'Publishing needs -NotesFile with the release text.' }

Write-Host "Building $tag ..."
$build = Join-Path $root 'build\release'   # its own tree: the everyday build's hub may be running
& cmake -S $root -B $build -G 'Visual Studio 17 2022' -A x64 "-DMBOX2_REPOSITORY=$repository" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& cmake --build $build --config Release --target mbox2hub mbox2asio | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\firmware\build.ps1') | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Firmware build failed.' }

$dist = Join-Path $root "dist\$tag"
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
$stage = Join-Path $dist 'zip'
New-Item -ItemType Directory -Force $stage | Out-Null
$files = @{
    'mbox2hub.exe'   = Join-Path $build 'src\hub\Release\mbox2hub.exe'
    'mbox2asio.dll'  = Join-Path $build 'src\asio\Release\mbox2asio.dll'
    'mbox2-open.bin' = Join-Path $root 'build\firmware\mbox2-open.bin'   # tools\firmware\build.ps1's output
}
foreach ($name in $files.Keys) {
    Copy-Item $files[$name] (Join-Path $dist $name)
    Copy-Item $files[$name] (Join-Path $stage $name)
}
Copy-Item (Join-Path $root 'tools\setup\remove-avid-leftovers.ps1') $stage
@"
Mbox 2 Mini open firmware $tag

mbox2hub.exe                Mbox Hub: meters, analysis, health, firmware updates over USB, and the ASIO
                            driver's install (cog > DAW (ASIO) > Install). Keep these files together in a
                            folder of your own; the hub updates them from GitHub.
mbox2asio.dll               The ASIO driver. Mbox Hub installs it; you don't need to do anything with it.
mbox2-open.bin              The firmware image. Mbox Hub installs it over USB once the Mbox runs the open
                            firmware; the first install needs a Raspberry Pi (see the README online).
remove-avid-leftovers.ps1   Removes what Avid's driver uninstaller leaves behind (right-click > Run with
                            PowerShell).

Instructions: https://github.com/$repository
"@ | Set-Content -Encoding ASCII (Join-Path $stage 'README.txt')
$zip = Join-Path $dist 'Mbox2Mini-Windows.zip'
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Remove-Item -Recurse -Force $stage

$assets = Get-ChildItem $dist -File
$assets | ForEach-Object { '{0,-24} {1,9:N0} bytes  SHA-256 {2}' -f $_.Name, $_.Length, (Get-FileHash $_.FullName).Hash.ToLower() }
Write-Host "Release files in $dist" -ForegroundColor Green

if ($Publish) {
    & gh release create $tag @($assets.FullName) --repo $repository --title "Mbox 2 Mini open firmware $tag" --notes-file $NotesFile
    if ($LASTEXITCODE -ne 0) { throw 'gh release create failed.' }
}

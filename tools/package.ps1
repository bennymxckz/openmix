# Builds a release zip.
#
# openmix is portable: unzip and run. The one thing it cannot carry is
# usbip-win2, which is a signed kernel driver with its own installer, so the
# package documents it rather than bundling it.

param([string]$Version = "0.3.0")

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

& (Join-Path $PSScriptRoot 'build.ps1')

$stage = Join-Path $root "dist\openmix-$Version"
Remove-Item (Join-Path $root 'dist') -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $stage -Force | Out-Null

Copy-Item (Join-Path $root 'build\openmix.exe')     $stage
Copy-Item (Join-Path $root 'build\openmix-cli.exe') $stage
Copy-Item (Join-Path $root 'README.md')             $stage
Copy-Item (Join-Path $root 'LICENSE')               $stage
New-Item -ItemType Directory -Path (Join-Path $stage 'tools') -Force | Out-Null
Copy-Item (Join-Path $root 'tools\cleanup-devices.ps1') (Join-Path $stage 'tools')

@"
openmix $Version

Requires usbip-win2 v.0.9.7.7, which provides the signed USB transport that
openmix publishes its audio devices through:

    https://github.com/vadimgrn/usbip-win2/releases/tag/v.0.9.7.7

Do not use v.0.9.7.8 -- its own release notes warn of memory corruption.

Then run openmix.exe. Four devices appear in Windows sound settings. Point
applications at them, and pick your headphones and microphone in the window.

To have the devices show as "Openmix - Game" rather than "Speakers (Openmix -
Game)", run this once as administrator while openmix is running:

    openmix-cli.exe --fix-names

Windows composes USB audio endpoint names itself and a device cannot override
them, so setting the endpoint's own name is the only thing that sticks.
"@ | Set-Content (Join-Path $stage 'INSTALL.txt') -Encoding utf8

$zip = Join-Path $root "dist\openmix-$Version-windows-x64.zip"
Compress-Archive -Path "$stage\*" -DestinationPath $zip -Force

Write-Host "`nPackaged:" -ForegroundColor Green
Write-Host ("  {0}  {1:N0} bytes" -f (Split-Path $zip -Leaf), (Get-Item $zip).Length)

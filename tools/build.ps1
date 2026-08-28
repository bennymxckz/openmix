# Builds openmix.
#
# CMake and Ninja ship inside Visual Studio Build Tools and are not on PATH,
# so this locates them via vswhere rather than expecting a developer prompt.
#
#   .\tools\build.ps1              build
#   .\tools\build.ps1 -Run         build, then start the mixer
#   .\tools\build.ps1 -Clean       reconfigure from scratch

param(
    [switch]$Run,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "Visual Studio Build Tools not found. Install the 'Desktop development with C++' workload."
}
$vs = & $vswhere -latest -products * -property installationPath
if (-not $vs) { throw "No Visual Studio installation found." }

$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninja = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vs" }

# A running instance holds openmix.exe open and the link fails, which reads as
# a confusing build error rather than "close the app".
$running = Get-Process openmix -ErrorAction SilentlyContinue
if ($running) {
    throw "openmix is running (pid $($running.Id)). Quit it from the tray, then build again."
}

$env:PATH = "$cmake;$ninja;$env:PATH"
$build = Join-Path $root 'build'

if ($Clean -and (Test-Path $build)) {
    Remove-Item $build -Recurse -Force
}

$configure = ''
if (-not (Test-Path (Join-Path $build 'build.ninja'))) {
    $configure = "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && "
}

cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$root`" && $configure cmake --build build"
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

Write-Host "`nBuilt:" -ForegroundColor Green
Get-ChildItem (Join-Path $build 'openmix*.exe') |
    ForEach-Object { "  {0,-20} {1,9:N0} bytes" -f $_.Name, $_.Length }

if ($Run) {
    Start-Process (Join-Path $build 'openmix.exe')
    Write-Host "`nopenmix started." -ForegroundColor Green
}

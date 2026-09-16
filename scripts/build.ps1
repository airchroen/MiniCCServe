# Build helper: configures and builds MiniCCServe with either toolchain.
#
#   pwsh scripts/build.ps1              # mingw (uses CMakeUserPresets.json)
#   pwsh scripts/build.ps1 msvc         # MSVC (auto-locates VsDevCmd)
#
# The mingw path expects a local CMakeUserPresets.json pointing at your
# llvm-mingw toolchain (see README). The msvc path finds Visual Studio via
# vswhere and runs the build inside the VS developer environment.

param(
    [ValidateSet('mingw', 'msvc')][string]$Toolchain = 'mingw',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

if ($Toolchain -eq 'mingw') {
    if (-not (Test-Path "$repoRoot/CMakeUserPresets.json")) {
        Write-Error 'CMakeUserPresets.json not found. Create it with the paths to your llvm-mingw toolchain (see README.md).'
    }
    cmake --preset mingw-local
    cmake --build --preset mingw-local
}
else {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { Write-Error 'vswhere.exe not found; is Visual Studio installed?' }
    $installPath = & $vswhere -Latest -Products * -Requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -Property installationPath
    if (-not $installPath) { Write-Error 'No Visual Studio with C++ tools found.' }
    $vsdevcmd = Join-Path $installPath 'Common7\Tools\VsDevCmd.bat'
    cmd /s /c "`"$vsdevcmd`" -arch=x64 -no_logo && cmake --preset msvc-release && cmake --build --preset msvc-release"
}

Write-Host ''
Write-Host "Build OK: build\$Toolchain" -ForegroundColor Green

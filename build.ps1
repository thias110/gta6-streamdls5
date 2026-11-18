# Compile rpenhance avec le CMake fourni par Visual Studio (pas besoin de cmake dans le PATH).
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Config = 'Release',
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio avec la charge 'Développement Desktop en C++' introuvable." }

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' }

$root  = $PSScriptRoot
$build = Join-Path $root 'build'
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

& $cmake -S $root -B $build -A x64
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $cmake --build $build --config $Config --parallel
exit $LASTEXITCODE

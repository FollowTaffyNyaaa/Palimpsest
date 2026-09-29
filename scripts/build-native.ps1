param(
    [ValidateSet('Debug','Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
$WindowsRoot = Split-Path -Parent $PSScriptRoot
$RepoRoot = Split-Path -Parent $WindowsRoot
$NativeRoot = Join-Path $WindowsRoot 'native'
$BuildRoot = Join-Path $WindowsRoot 'build\native'

$pdfiumDll = Join-Path $WindowsRoot 'third_party\pdfium\bin\pdfium.dll'
if (-not (Test-Path $pdfiumDll)) { throw "Pinned PDFium runtime not found: $pdfiumDll" }

cmake -S $NativeRoot -B $BuildRoot -G Ninja "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE" }
cmake --build $BuildRoot --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "Native build failed with exit code $LASTEXITCODE" }

$bin = Join-Path $BuildRoot 'bin'
Copy-Item $pdfiumDll (Join-Path $bin 'pdfium.dll') -Force

$compiler = Get-Command g++ -ErrorAction Stop
$compilerDir = Split-Path -Parent $compiler.Source
foreach ($runtime in @('libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
    $runtimePath = Join-Path $compilerDir $runtime
    if (Test-Path $runtimePath) { Copy-Item $runtimePath (Join-Path $bin $runtime) -Force }
}
Write-Host "Native build ready: $bin"

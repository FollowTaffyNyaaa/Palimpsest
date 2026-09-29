param(
    [ValidateSet('Debug','Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
$WindowsRoot = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'build-native.ps1') -Configuration $Configuration
dotnet build (Join-Path $WindowsRoot 'src\Palimpsest.App\Palimpsest.App.csproj') -c $Configuration -p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw "WinUI build failed with exit code $LASTEXITCODE" }

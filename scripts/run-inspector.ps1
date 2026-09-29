param(
    [Parameter(Mandatory=$true)]
    [string]$Pdf,
    [string]$Output
)

$ErrorActionPreference = 'Stop'
$WindowsRoot = Split-Path -Parent $PSScriptRoot
$Inspector = Join-Path $WindowsRoot 'build\native\bin\paper_inspector.exe'
if (-not (Test-Path $Inspector)) { & (Join-Path $PSScriptRoot 'build-native.ps1') }
if (-not $Output) { $Output = Join-Path (Split-Path -Parent $Pdf) 'debug' }
& $Inspector $Pdf $Output
exit $LASTEXITCODE

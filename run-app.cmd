@echo off
setlocal
set "APP=%~dp0src\Palimpsest.App\bin\x64\Debug\net8.0-windows10.0.26100.0\win-x64\Palimpsest.App.exe"

if not exist "%APP%" (
  echo Palimpsest.App.exe was not found.
  echo Building Debug x64 first...
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1" -Configuration Debug
  if errorlevel 1 (
    echo Build failed.
    pause
    exit /b 1
  )
)

start "Palimpsest" "%APP%"
endlocal

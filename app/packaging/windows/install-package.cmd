@echo off
rem Registers this folder as the packaged AirLink (needed for call audio over Bluetooth).
rem Needs Windows Developer Mode: Settings - System - For developers.
cd /d "%~dp0"
taskkill /IM airlink.exe /F >nul 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -Command "Add-AppxPackage -Register \"%~dp0AppxManifest.xml\" -ForceUpdateFromAnyVersion; if ($?) { Write-Host \"AirLink paketi kuruldu. Baslat menusunden AirLink ile acin.\" }"
pause

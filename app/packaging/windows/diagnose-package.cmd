@echo off
rem Starts the packaged AirLink (package identity) with a log, for call and player audio tests.
cd /d "%~dp0"
set "AIRLINK_DIAG_LOG=%~dp0AirLink-package.log"
set "KOMPOZ_GPU=gles"
echo Log: "%AIRLINK_DIAG_LOG%"
taskkill /IM airlink.exe /F >nul 2>&1
airlink-pkg.exe
echo AirLink acildi. Testten sonra bu pencereye donun.
pause
type "%AIRLINK_DIAG_LOG%"
pause

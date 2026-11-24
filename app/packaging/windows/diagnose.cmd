@echo off
cd /d "%~dp0"
set "AIRLINK_DIAG_LOG=AirLink-diagnostic.log"
set "KOMPOZ_GPU=gles"
echo AirLink diagnostic launch. Log: "%~dp0AirLink-diagnostic.log"
echo Existing processes:
tasklist /FI "IMAGENAME eq airlink.exe"
start "" /wait "%~dp0airlink.exe"
echo Exit code: %ERRORLEVEL%
if exist "%AIRLINK_DIAG_LOG%" type "%AIRLINK_DIAG_LOG%"
echo.
pause

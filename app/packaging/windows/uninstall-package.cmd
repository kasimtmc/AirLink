@echo off
powershell -NoProfile -Command "Get-AppxPackage com.plyg.AirLink | Remove-AppxPackage; Write-Host \"AirLink paketi kaldirildi.\""
pause

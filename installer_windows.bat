@echo off
setlocal
cd /d "%~dp0"

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer_windows_bootstrap.ps1"
set "RFF_EXIT=%ERRORLEVEL%"

if not "%RFF_EXIT%"=="0" pause
exit /b %RFF_EXIT%

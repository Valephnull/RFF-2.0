@echo off
setlocal
cd /d "%~dp0bin"
RFF.exe
if errorlevel 1 pause

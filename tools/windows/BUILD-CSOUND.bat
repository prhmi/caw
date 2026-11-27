@echo off
rem CAW - build Csound 7 for Windows (see build-csound.ps1)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-csound.ps1" %*
if errorlevel 1 (echo. & echo [FAILED] & pause & exit /b 1)
pause

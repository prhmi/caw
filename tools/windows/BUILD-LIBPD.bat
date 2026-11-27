@echo off
rem CAW - build libpd.dll (MULTI=true) with MSYS2 (see build-libpd.ps1)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-libpd.ps1" %*
if errorlevel 1 (echo. & echo [FAILED] & pause & exit /b 1)
pause

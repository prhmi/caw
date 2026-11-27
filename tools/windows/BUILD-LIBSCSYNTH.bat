@echo off
rem CAW - build CAW's SuperCollider runtime (see build-libscsynth.ps1)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-libscsynth.ps1" %*
if errorlevel 1 (echo. & echo [FAILED] & pause & exit /b 1)
pause

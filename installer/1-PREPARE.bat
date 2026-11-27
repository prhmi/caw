@echo off
rem Step 1 of 2: collect everything the setup contains (see 1-PREPARE.ps1)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp01-PREPARE.ps1" %*
set RC=%ERRORLEVEL%
pause
exit /b %RC%

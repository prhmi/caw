@echo off
setlocal EnableExtensions
rem Step 2 of 2: compiles CAW-Setup.iss with Inno Setup into installer\Output\CAW-Setup-<version>.exe
cd /d "%~dp0"

if not exist "stage\caw.exe" (
  echo [ERROR] stage\caw.exe is missing. Run 1-PREPARE.bat first.
  goto :failed
)
if not exist "redist\vc_redist.x64.exe" (
  echo [ERROR] redist\vc_redist.x64.exe is missing. Run 1-PREPARE.bat first.
  goto :failed
)

set "ISCC="
for %%P in ("%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" "%ProgramFiles%\Inno Setup 6\ISCC.exe" "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe") do if exist %%P set "ISCC=%%~P"

if not defined ISCC (
  echo Inno Setup 6 was not found.
  choice /c YN /n /m "Install it now with winget? [Y/N] "
  if errorlevel 2 goto :manual
  winget install --id JRSoftware.InnoSetup --exact --accept-package-agreements --accept-source-agreements
  for %%P in ("%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" "%ProgramFiles%\Inno Setup 6\ISCC.exe" "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe") do if exist %%P set "ISCC=%%~P"
)
if not defined ISCC goto :manual

echo Using %ISCC%
"%ISCC%" "CAW-Setup.iss"
if errorlevel 1 goto :failed

echo.
echo [SUCCESS] The setup is in: %~dp0Output
start "" explorer.exe "%~dp0Output"
pause
exit /b 0

:manual
echo Install Inno Setup 6.3 or newer from https://jrsoftware.org/isdl.php and run this file again.
:failed
echo.
pause
exit /b 1

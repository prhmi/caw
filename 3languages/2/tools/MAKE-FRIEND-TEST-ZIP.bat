@echo off
setlocal EnableExtensions

rem Build a portable Windows x64 folder for a friend to smoke-test CAW.
cd /d "%~dp0.."
set "CAW_ROOT=%CD%"
set "CAW_EXE=%CAW_ROOT%\build\Release\caw.exe"
set "SC_RUNTIME=%USERPROFILE%\.caw_deps\supercollider\runtime-x64\SuperCollider"
set "PACKAGE=%CAW_ROOT%\friend-test"
set "ZIP=%CAW_ROOT%\friend-test.zip"

if not exist "%CAW_EXE%" (
  echo [ERROR] CAW was not built. Run build.bat first.
  goto :failed
)
if not exist "%SC_RUNTIME%\sclang.exe" (
  echo [ERROR] SuperCollider runtime is missing. Run tools\BUILD-LIBSCSYNTH.bat first.
  goto :failed
)
if not exist "%SC_RUNTIME%\libscsynth.dll" (
  echo [ERROR] libscsynth.dll is missing from the installed SuperCollider runtime.
  goto :failed
)

if exist "%PACKAGE%" rmdir /s /q "%PACKAGE%"
if exist "%ZIP%" del /q "%ZIP%"
mkdir "%PACKAGE%"
if errorlevel 1 goto :failed
copy /Y "%CAW_EXE%" "%PACKAGE%\caw.exe" >nul
if errorlevel 1 goto :failed

rem These assets are normally copied beside caw.exe by CMake.
if exist "%CAW_ROOT%\build\Release\ffmpeg.exe" copy /Y "%CAW_ROOT%\build\Release\ffmpeg.exe" "%PACKAGE%\" >nul
if exist "%CAW_ROOT%\build\Release\ffmpeg-licenses" (
  xcopy "%CAW_ROOT%\build\Release\ffmpeg-licenses" "%PACKAGE%\ffmpeg-licenses\" /E /I /Y >nul
)

echo Copying the complete SuperCollider runtime...
xcopy "%SC_RUNTIME%\*" "%PACKAGE%\SuperCollider\" /E /I /Y >nul
if errorlevel 1 goto :failed
if exist "%USERPROFILE%\.caw_deps\supercollider\source\LICENSE" (
  copy /Y "%USERPROFILE%\.caw_deps\supercollider\source\LICENSE" "%PACKAGE%\SuperCollider-LICENSE.txt" >nul
)

rem Optional: include the currently selected libpd library and its adjacent DLLs.
set "PD_DLL="
if exist "%USERPROFILE%\.caw_libpd_path" set /p PD_DLL=<"%USERPROFILE%\.caw_libpd_path"
if defined PD_DLL if exist "%PD_DLL%" (
  for %%I in ("%PD_DLL%") do set "PD_DIR=%%~dpI"
  mkdir "%PACKAGE%\PureData"
  for %%F in ("%PD_DIR%*.dll") do copy /Y "%%~fF" "%PACKAGE%\PureData\" >nul
)

rem Optional project file path can be passed as the first argument.
if not "%~1"=="" (
  if not exist "%~1" (
    echo [ERROR] Project file not found: %~1
    goto :failed
  )
  copy /Y "%~f1" "%PACKAGE%\" >nul
  if errorlevel 1 goto :failed
)

copy /Y "%~dp0FRIEND-TEST-README.txt" "%PACKAGE%\README-TESTER.txt" >nul
if errorlevel 1 goto :failed

echo Creating %ZIP% ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%PACKAGE%\*' -DestinationPath '%ZIP%' -Force"
if errorlevel 1 goto :failed
echo.
echo [SUCCESS] Send this zip to your friend:
echo %ZIP%
echo.
echo The friend needs Windows x64. Csound is required only for Csound tests.
pause
exit /b 0

:failed
echo.
echo [FAILED] The test package was not created.
pause
exit /b 1

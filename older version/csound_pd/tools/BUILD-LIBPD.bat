@echo off
setlocal EnableExtensions
 title CAW - Build libpd
 echo.
 echo  CAW - Pure Data library builder
 echo  ===============================
 echo.
 echo  This needs Windows 10/11 64-bit and an internet connection.
 echo  It downloads build tools and libpd source, then builds a DLL.
 echo  It does NOT change your installed Pure Data or CAW project.
 echo  The first run can take a while and needs free disk space.
 echo.
 choice /c YN /n /m "Continue? [Y/N] "
 if errorlevel 2 goto :cancel
 set "MSYS_ROOT=C:\msys64"
 if exist "%MSYS_ROOT%\usr\bin\bash.exe" goto :have_msys
 echo.
 echo MSYS2 was not found at C:\msys64.
 echo If it is installed elsewhere, type its folder now.
 echo Otherwise press Enter to install it with Windows Package Manager.
 set /p "CUSTOM_MSYS=MSYS2 folder (or Enter): "
 if defined CUSTOM_MSYS set "MSYS_ROOT=%CUSTOM_MSYS%"
 if exist "%MSYS_ROOT%\usr\bin\bash.exe" goto :have_msys
 if defined CUSTOM_MSYS goto :bad_msys
 where winget >nul 2>&1
 if errorlevel 1 goto :no_winget
 echo.
 echo Windows may ask you to approve the MSYS2 installation.
 choice /c YN /n /m "Install MSYS2 now? [Y/N] "
 if errorlevel 2 goto :cancel
 winget install --id MSYS2.MSYS2 --exact --source winget --accept-package-agreements --accept-source-agreements
 if errorlevel 1 goto :install_failed
 if not exist "%MSYS_ROOT%\usr\bin\bash.exe" goto :bad_msys
:have_msys
 if not exist "%~dp0build_libpd.sh" goto :missing_helper
 if not exist "%~dp0OPEN-CAW.bat" goto :missing_helper
 echo.
 echo Building libpd. This window will stay open if something fails.
 echo Downloading and installing the needed MSYS2 packages...
 set "MSYSTEM=MINGW64"
 set "MSYS2_PATH_TYPE=inherit"
 set "CAW_HELPER_DIR=%~dp0"
 if not defined CAW_RESULT_FILE set "CAW_RESULT_FILE=%TEMP%\caw_libpd_result_%RANDOM%_%RANDOM%.txt"
 if exist "%CAW_RESULT_FILE%" goto :collision
 "%MSYS_ROOT%\usr\bin\bash.exe" --login "%~dp0build_libpd.sh"
 if errorlevel 1 goto :build_failed
 if not exist "%CAW_RESULT_FILE%" goto :build_failed
 set "CAW_RESULT_DIR="
 for /f "usebackq delims=" %%D in ("%CAW_RESULT_FILE%") do set "CAW_RESULT_DIR=%%D"
 if not defined CAW_RESULT_DIR goto :build_failed
 if "%CAW_INTEGRATED%"=="1" exit /b 0
 echo.
 echo SUCCESS. Your libpd folder is:
 echo %CAW_RESULT_DIR%
 echo.
 echo The folder will open. To start CAW, double-click OPEN-CAW.bat
 echo in that folder and select your caw.exe.
 echo In CAW's PD Preview, choose libpd.dll from the same folder.
 start "" explorer.exe "%CAW_RESULT_DIR%"
 pause
 exit /b 0
:bad_msys
 echo.
 echo Could not find MSYS2 in "%MSYS_ROOT%".
 echo Install it from https://www.msys2.org/ and run this file again.
 goto :failed
:no_winget
 echo.
 echo Windows Package Manager was not found.
 echo Install MSYS2 from https://www.msys2.org/ then run this file again.
 goto :failed
:install_failed
 echo.
 echo MSYS2 installation did not finish. Install it from https://www.msys2.org/
 echo and run this file again.
 goto :failed
:missing_helper
 echo.
 echo A helper file is missing. Extract ALL files from the ZIP first.
 goto :failed
:collision
 echo.
 echo A temporary result name already exists. Please run this file again.
 goto :failed
:build_failed
 echo.
 echo The build did not finish. Scroll up to see the error.
 echo If a CAW-libpd folder was created in your user folder,
 echo its build.log has the details. Send that log for help.
 goto :failed
:cancel
 echo.
 echo Cancelled. Nothing more will be done.
 pause
 exit /b 1
:failed
 echo.
 pause
 exit /b 1

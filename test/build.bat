@echo off
setlocal

rem CAW build script for WINDOWS.  macOS / Linux: use build.sh
rem Go to the folder where this bat file lives
cd /d "%~dp0"

rem --- git is needed once, to download Dear ImGui ---
where git >nul 2>nul
if errorlevel 1 (
    echo [ERROR] git was not found. Install Git for Windows from git-scm.com, then run this again.
    pause
    exit /b 1
)

rem --- Find Visual Studio and load the C++ compiler environment ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found. Install Visual Studio or Build Tools with "Desktop development with C++".
    pause
    exit /b 1
)

set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"

if "%VSPATH%"=="" (
    echo [ERROR] No Visual Studio with C++ tools found.
    pause
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [ERROR] Could not load the Visual Studio environment.
    pause
    exit /b 1
)

echo.
echo === Configuring (downloads are cached in %USERPROFILE%\.caw_deps, only the very first build downloads) ===
echo This can take several minutes. Writing caw_configure.log...
cmake -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > caw_configure.log 2>&1
set "CAW_CONFIG_RESULT=%ERRORLEVEL%"
type caw_configure.log
if not "%CAW_CONFIG_RESULT%"=="0" goto :fail_config

echo.
echo === Building (writing caw_build.log) ===
cmake --build build --config Release > caw_build.log 2>&1
set "CAW_BUILD_RESULT=%ERRORLEVEL%"
type caw_build.log
if not "%CAW_BUILD_RESULT%"=="0" goto :fail_build

rem Reuse the existing libpd build. Do not download or build libpd here.
rem CAW loads it in place, along with DLL dependencies in the same folder.
set "CAW_PD_DIR=C:\Users\Parham\CAW-libpd\build-J5ruj3Sp"
if exist "%CAW_PD_DIR%\libpd.dll" (
    echo Pure Data library: %CAW_PD_DIR%\libpd.dll
) else (
    echo [WARNING] Existing libpd.dll not found at %CAW_PD_DIR%\libpd.dll
    echo Set its path in CAW Settings ^> Programs. No libpd download will be attempted.
)
echo.
if /i "%~1"=="nostart" goto :done
echo === Starting CAW ===
pushd build\Release
start "" caw.exe
popd
:done

echo.
echo Done.
exit /b 0

:fail_config
echo.
echo [FAILED] Configuration failed. Share caw_configure.log, including the first error above.
pause
exit /b 1

:fail_build
echo.
echo [FAILED] Build failed. Share caw_build.log, including the first error above.
pause
exit /b 1

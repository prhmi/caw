@echo off
setlocal

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
echo === Configuring (first time downloads Dear ImGui) ===
cmake -B build
if errorlevel 1 goto :fail

echo.
echo === Building ===
cmake --build build --config Release
if errorlevel 1 goto :fail

echo.
echo === Starting CAW ===
pushd build\Release
start "" caw.exe
popd

echo.
echo Done.
exit /b 0

:fail
echo.
echo [FAILED] See the messages above.
pause
exit /b 1

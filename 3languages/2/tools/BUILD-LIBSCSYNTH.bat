@echo off
setlocal EnableExtensions

rem Build the pinned SuperCollider x64 runtime for CAW (sclang, scsynth, plugins, and libscsynth).
rem Source, vcpkg, build products, and installed runtime stay in a shared user cache.
cd /d "%~dp0.."
set "SC_COMMIT=6df38879ddf08a21cb183e67dcbf800f9d5489cf"
set "SC_CACHE=%USERPROFILE%\.caw_deps\supercollider"
set "SC_SOURCE=%SC_CACHE%\source"
set "SC_BUILD=%SC_CACHE%\build-x64"
set "SC_INSTALL=%SC_CACHE%\runtime-x64"
set "SC_CONFIG_MARKER=%SC_BUILD%\caw-external-driver-v6.configured"

where git >nul 2>nul
if errorlevel 1 (
  echo [ERROR] Git for Windows is required. Install Git, then run this again.
  goto :failed
)
where cmake >nul 2>nul
if errorlevel 1 (
  echo [ERROR] CMake is required. Install CMake, then run this again.
  goto :failed
)

rem Match CAW's existing Visual Studio discovery and initialize the x64 compiler.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERROR] Visual Studio Installer was not found.
  echo Install Visual Studio or Build Tools with Desktop development with C++.
  goto :failed
)
set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo [ERROR] No Visual Studio installation with the x64 C++ tools was found.
  goto :failed
)
if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" (
  echo [ERROR] The Visual Studio x64 compiler setup script is missing.
  goto :failed
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [ERROR] Could not initialize the Visual Studio x64 compiler.
  goto :failed
)

rem Do not trust VCPKG_ROOT inherited from vcvars64.bat: VS injects its
rem manifest-only vcpkg, which cannot install these packages in classic mode.
rem Use CAW's cached standalone vcpkg, or an explicit standalone override.
set "CAW_VCPKG_ROOT=%CAW_SCSYNTH_VCPKG_ROOT%"
set "CAW_BOOTSTRAP_VCPKG=0"
if not defined CAW_VCPKG_ROOT (
  set "CAW_VCPKG_ROOT=%USERPROFILE%\.caw_deps\vcpkg"
  set "CAW_BOOTSTRAP_VCPKG=1"
)
set "VCPKG_ROOT=%CAW_VCPKG_ROOT%"
if exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" goto :vcpkg_ready
if not "%CAW_BOOTSTRAP_VCPKG%"=="1" (
  echo [ERROR] CAW_SCSYNTH_VCPKG_ROOT must point to a standalone vcpkg checkout:
  echo         %VCPKG_ROOT%
  goto :failed
)
if exist "%VCPKG_ROOT%" (
  echo [ERROR] The cached vcpkg folder exists but is incomplete:
  echo         %VCPKG_ROOT%
  echo Fix or remove that folder, then run this script again.
  goto :failed
)
echo.
echo === Downloading standalone vcpkg once into the shared cache ===
if not exist "%USERPROFILE%\.caw_deps" mkdir "%USERPROFILE%\.caw_deps"
git clone --depth 1 https://github.com/microsoft/vcpkg.git "%VCPKG_ROOT%"
if errorlevel 1 goto :download_failed
call "%VCPKG_ROOT%\bootstrap-vcpkg.bat" -disableMetrics
if errorlevel 1 goto :download_failed
:vcpkg_ready
if exist "%VCPKG_ROOT%\vcpkg.exe" goto :vcpkg_exe_ready
if not exist "%VCPKG_ROOT%\bootstrap-vcpkg.bat" (
  echo [ERROR] vcpkg.exe and bootstrap-vcpkg.bat are missing from:
  echo         %VCPKG_ROOT%
  goto :failed
)
call "%VCPKG_ROOT%\bootstrap-vcpkg.bat" -disableMetrics
if errorlevel 1 goto :download_failed
:vcpkg_exe_ready
if exist "%SC_SOURCE%\CMakeLists.txt" goto :source_ready
if exist "%SC_SOURCE%" (
  echo [ERROR] The cached SuperCollider source folder is incomplete:
  echo         %SC_SOURCE%
  echo Fix or remove that folder, then run this script again.
  goto :failed
)
if not exist "%SC_CACHE%" mkdir "%SC_CACHE%"
echo.
echo === Downloading the pinned SuperCollider source once into the shared cache ===
git clone --depth 1 --no-checkout --branch develop https://github.com/supercollider/supercollider.git "%SC_SOURCE%"
if errorlevel 1 goto :download_failed
git -C "%SC_SOURCE%" fetch --depth 1 origin %SC_COMMIT%
if errorlevel 1 goto :download_failed
git -C "%SC_SOURCE%" checkout --detach %SC_COMMIT%
if errorlevel 1 goto :download_failed
:source_ready
set "SC_CACHED_COMMIT="
for /f "usebackq delims=" %%C in (`git -C "%SC_SOURCE%" rev-parse HEAD 2^>nul`) do set "SC_CACHED_COMMIT=%%C"
if /i not "%SC_CACHED_COMMIT%"=="%SC_COMMIT%" (
  echo [ERROR] The cached SuperCollider source is not the pinned commit %SC_COMMIT%.
  echo Remove only %SC_SOURCE% and %SC_BUILD%, then run this script again.
  goto :failed
)
git -C "%SC_SOURCE%" submodule update --init --recursive
if errorlevel 1 goto :download_failed
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0FIX-SUPERCOLLIDER-CMAKE.ps1" -SourceRoot "%SC_SOURCE%"
if errorlevel 1 goto :patch_failed

rem Classic-mode vcpkg packages are installed once into the shared vcpkg cache.
rem Running from the CAW root keeps vcpkg out of SuperCollider's manifest mode.
echo.
echo === Preparing cached audio dependencies (no repeat download after first install) ===
"%VCPKG_ROOT%\vcpkg.exe" install fftw3 libsndfile --triplet x64-windows --disable-metrics
if errorlevel 1 goto :download_failed

if not exist "%SC_CONFIG_MARKER%" (
  echo === Replacing the previous standalone/static SC build cache once ===
  if exist "%SC_BUILD%" rmdir /s /q "%SC_BUILD%"
  if exist "%SC_INSTALL%" rmdir /s /q "%SC_INSTALL%"
)
if not exist "%SC_BUILD%" mkdir "%SC_BUILD%"
if not exist "%SC_INSTALL%" mkdir "%SC_INSTALL%"
echo.
echo === Configuring 64-bit SuperCollider shared-audio runtime ===
cmake -S "%SC_SOURCE%" -B "%SC_BUILD%" -A x64 -DCMAKE_INSTALL_PREFIX="%SC_INSTALL%" -DLIBSCSYNTH=ON -DSUPERNOVA=OFF -DSC_QT=OFF -DSC_IDE=OFF -DSC_DOC_RENDER=OFF -DINSTALL_HELP=OFF -DENABLE_TESTSUITE=OFF -DSC_ABLETON_LINK=OFF -DAUDIOAPI=external -DSTATIC_PLUGINS=ON -DUSE_VCPKG_FFTW=ON -DUSE_VCPKG_LIBSNDFILE=ON -DUSE_VCPKG_READLINE=OFF -DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_MANIFEST_MODE=OFF
if errorlevel 1 goto :configure_failed
echo CAW external audio driver v6>"%SC_CONFIG_MARKER%"

echo.
echo === Building SuperCollider runtime ===
cmake --build "%SC_BUILD%" --config Release --parallel
if errorlevel 1 goto :build_failed

echo.
echo === Installing the runtime payload ===
set "SC_INSTALL_STEP=CMake install/fixup_bundle"
cmake --install "%SC_BUILD%" --config Release
if errorlevel 1 goto :install_failed

rem libsndfile is a DLL so its optional MP3/LAME symbols are not pulled into
rem libscsynth.lib at link time. Bundle all vcpkg runtime DLLs beside sclang.
set "SC_RUNTIME_DIR=%SC_INSTALL%\SuperCollider"
set "SC_INSTALL_STEP=creating the runtime folder"
if not exist "%SC_RUNTIME_DIR%" mkdir "%SC_RUNTIME_DIR%"
if errorlevel 1 goto :install_failed
set "SC_INSTALL_STEP=copying vcpkg runtime DLLs"
if exist "%VCPKG_ROOT%\installed\x64-windows\bin\*.dll" (
  for %%F in ("%VCPKG_ROOT%\installed\x64-windows\bin\*.dll") do (
    copy /Y "%%~fF" "%SC_RUNTIME_DIR%"
    if errorlevel 1 goto :install_failed
  )
)

set "SC_RUNTIME_DLL=%SC_RUNTIME_DIR%\libscsynth.dll"
echo.
echo === Staging CAW host-pumped library ===
echo Destination: "%SC_RUNTIME_DLL%"
if exist "%SC_RUNTIME_DLL%" goto :verify_runtime_dll
set "SC_DLL="
for /r "%SC_BUILD%" %%F in (libscsynth.dll) do if not defined SC_DLL set "SC_DLL=%%~fF"
if not defined SC_DLL for /r "%SC_INSTALL%" %%F in (libscsynth.dll) do if not defined SC_DLL set "SC_DLL=%%~fF"
if not defined SC_DLL goto :missing_libsc
echo Source:      "%SC_DLL%"
set "SC_INSTALL_STEP=checking the CMake-built libscsynth.dll"
if not exist "%SC_DLL%" goto :missing_libsc
set "SC_INSTALL_STEP=creating the installed runtime folder"
if not exist "%SC_RUNTIME_DIR%" mkdir "%SC_RUNTIME_DIR%"
if errorlevel 1 goto :install_failed
if /i not "%SC_DLL%"=="%SC_RUNTIME_DLL%" (
  set "SC_INSTALL_STEP=copying libscsynth.dll into the installed runtime"
  copy /B /Y "%SC_DLL%" "%SC_RUNTIME_DLL%"
  if errorlevel 1 goto :install_failed
)
:verify_runtime_dll
set "SC_INSTALL_STEP=verifying the installed libscsynth.dll"
if not exist "%SC_RUNTIME_DLL%" (
  echo [ERROR] libscsynth.dll is still missing from the installed runtime.
  goto :install_failed
)
set "SC_INSTALL_STEP=verifying CAW host-pump exports in libscsynth.dll"
dumpbin /exports "%SC_RUNTIME_DLL%" > "%SC_BUILD%\caw_sc_exports.txt"
if errorlevel 1 goto :abi_failed
for %%S in (caw_sc_abi_version caw_sc_world_new caw_sc_world_process caw_sc_world_free) do (
  findstr /c:"%%S" "%SC_BUILD%\caw_sc_exports.txt" >nul
  if errorlevel 1 goto :abi_failed
)
set "SC_LANG="
for /r "%SC_INSTALL%" %%F in (sclang.exe) do if not defined SC_LANG set "SC_LANG=%%~fF"
if not defined SC_LANG (
  echo [ERROR] The runtime was installed but sclang.exe was not found. CAW needs sclang.exe to run SC clips.
  goto :failed
)
set "SC_SYNTH="
for /r "%SC_INSTALL%" %%F in (scsynth.exe) do if not defined SC_SYNTH set "SC_SYNTH=%%~fF"
if not defined SC_SYNTH (
  echo [ERROR] The runtime was installed but scsynth.exe was not found.
  goto :failed
)

echo.
echo === SUCCESS ===
echo SuperCollider language: %SC_LANG%
echo Audio server:           %SC_SYNTH%
echo Library:                %SC_RUNTIME_DLL%
echo Runtime package: %SC_INSTALL%
echo Cache:           %SC_CACHE%
echo.
echo Re-running this script reuses the source, vcpkg packages, and build outputs.
echo CAW uses sclang.exe plus the embedded libscsynth host driver for shared live audio.
echo.
pause
exit /b 0

:download_failed
echo.
echo [FAILED] A one-time source or vcpkg download/bootstrap failed.
echo Check your internet connection and try again. Completed downloads remain cached.
goto :failed
:patch_failed
echo.
echo [FAILED] Could not apply the SuperCollider CMake compatibility fix.
goto :failed
:configure_failed
echo.
echo [FAILED] CMake configuration failed. Review the error above.
goto :failed
:build_failed
echo.
echo [FAILED] SuperCollider build failed. Review the first compiler error above.
goto :failed
:install_failed
echo.
echo [FAILED] Runtime installation failed while %SC_INSTALL_STEP%.
echo Use the copy/CMake error above to identify what blocked it. Close CAW and retry if the DLL is in use.
goto :failed
:missing_libsc
echo [ERROR] libscsynth.dll was not found in the CMake build or installed runtime.
echo Check the CMake build log above; the external driver must produce this DLL.
set "SC_INSTALL_STEP=locating libscsynth.dll"
goto :install_failed
:abi_failed
echo [ERROR] The installed libscsynth.dll is missing one or more CAW host-pump exports.
echo Recheck the source patch and the CMake build log. The required ABI is version 3.
goto :install_failed
:failed
echo.
pause
exit /b 1

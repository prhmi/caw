@echo off
setlocal
rem ============================================================================
rem clean_build.bat  -  ONE-SHOT fix for LNK1181 "cannot open sdk_hosting.lib"
rem
rem Symptom: the build fails at LINK with
rem     LNK1181: cannot open input file 'lib\Release\sdk_hosting.lib'
rem This happens when the build\ tree (or the VST3 SDK fetch cache) is STALE -
rem typically after the project was moved, copied, or built from a scratch/temp
rem folder, so caw.vcxproj references SDK .lib paths that no longer exist where
rem the linker looks for them.
rem
rem This script wipes the stale build tree and the SDK build cache, then runs a
rem full clean configure+build. Run it ONCE to recover; afterwards use build.bat
rem for normal incremental builds.
rem ============================================================================

cd /d "%~dp0"

echo.
echo === Wiping the stale build tree (build\) ===
if exist build rmdir /s /q build
if exist build (
    echo [ERROR] could not remove build\ - close any program holding it (Visual Studio, Explorer) and retry.
    pause
    exit /b 1
)

echo === Wiping the stale VST3 SDK build cache (keeps the source, so it stays offline) ===
if exist "%USERPROFILE%\.caw_deps\fetch\vst3sdk-build" rmdir /s /q "%USERPROFILE%\.caw_deps\fetch\vst3sdk-build"

echo.
echo === Running a full clean build (reconfigures + rebuilds the SDK into build\lib\Release) ===
call build.bat
set "CAW_CLEAN_RESULT=%ERRORLEVEL%"
if not "%CAW_CLEAN_RESULT%"=="0" (
    echo.
    echo [ERROR] clean build failed. See caw_build.log / caw_configure.log.
    exit /b %CAW_CLEAN_RESULT%
)

echo.
echo === Clean build OK. Use build.bat for subsequent incremental builds. ===
endlocal

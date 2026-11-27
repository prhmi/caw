# CAW - build CAW's own SuperCollider runtime on Windows (libscsynth.dll with the external driver).
# Result: %USERPROFILE%\.caw_deps\supercollider\runtime-x64\SuperCollider\  (CAW and 1-PREPARE look there)
# Needs: Visual Studio 2022 (C++), Git, CMake, and vcpkg with libsndfile + fftw3 (set VCPKG_ROOT).
param([string]$Tag = $(if ($env:SC_TAG) { $env:SC_TAG } else { 'Version-3.14.0' }))
$ErrorActionPreference = 'Stop'
$here  = $PSScriptRoot
$scDir = Join-Path $here '..\supercollider'
$deps  = Join-Path $env:USERPROFILE '.caw_deps\supercollider'
$src   = Join-Path $deps 'source'
$build = Join-Path $deps 'build-x64'
$out   = Join-Path $deps 'runtime-x64\SuperCollider'
function Step($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }

Step "SuperCollider source ($Tag)"
New-Item -ItemType Directory -Force $deps | Out-Null
if (-not (Test-Path (Join-Path $src '.git'))) { git clone --recurse-submodules https://github.com/supercollider/supercollider.git $src }
Push-Location $src
git fetch --tags --quiet
git reset --hard --quiet
git clean -fdq server/scsynth
git checkout --quiet $Tag
git submodule update --init --recursive --quiet

Step 'CAW changes'
Copy-Item (Join-Path $scDir 'SC_ExternalDriver.cpp') 'server\scsynth\SC_ExternalDriver.cpp' -Force
git apply (Join-Path $scDir 'caw_sc.patch')
if ($LASTEXITCODE -ne 0) { throw "caw_sc.patch does not apply to $Tag - use the same tag as before" }
Pop-Location

Step 'Configure'
$tc = @()
if ($env:VCPKG_ROOT) { $tc = @("-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake", '-DVCPKG_TARGET_TRIPLET=x64-windows') }
else { Write-Host '  [warn] VCPKG_ROOT not set: libsndfile / fftw must be found some other way' -ForegroundColor Yellow }
cmake -S $src -B $build -G 'Visual Studio 17 2022' -A x64 `
  -DSC_QT=OFF -DSC_IDE=OFF -DSUPERNOVA=OFF -DSC_HIDAPI=OFF -DLIBSCSYNTH=ON -DAUDIOAPI=external `
  "-DCMAKE_INSTALL_PREFIX=$out" @tc
if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }

Step 'Build'
cmake --build $build --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

Step 'Collect'
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $out | Out-Null
$rel = Get-ChildItem $build -Recurse -File | Where-Object { $_.DirectoryName -match '\\Release$' }
$rel | Where-Object { $_.Name -in 'libscsynth.dll','libscsynth.lib','sclang.exe','scsynth.exe' -or $_.Extension -eq '.dll' } | Copy-Item -Destination $out -Force
$plug = Join-Path $out 'plugins'; New-Item -ItemType Directory -Force $plug | Out-Null
Get-ChildItem $build -Recurse -File -Filter '*.scx' | Copy-Item -Destination $plug -Force
Copy-Item (Join-Path $src 'SCClassLibrary') (Join-Path $out 'SCClassLibrary') -Recurse -Force
Copy-Item (Join-Path $src 'COPYING') (Join-Path $out 'SuperCollider-LICENSE.txt') -ErrorAction SilentlyContinue

Step 'Check'
$dll = Join-Path $out 'libscsynth.dll'
if (-not (Test-Path $dll)) { throw 'libscsynth.dll was not built' }
$bytes = [IO.File]::ReadAllBytes($dll); $text = [Text.Encoding]::ASCII.GetString($bytes)
$bad = 0
foreach ($s in 'caw_sc_abi_version','caw_sc_world_new','caw_sc_world_process','caw_sc_world_process_ex','caw_sc_world_free','caw_sc_last_error') {
  if ($text.Contains($s)) { Write-Host "  [ok]   $s" -ForegroundColor Green } else { Write-Host "  [MISS] $s" -ForegroundColor Red; $bad++ }
}
if ($bad) { throw 'libscsynth.dll has no CAW entry points - the external driver was not compiled in' }
Write-Host "`nDone: $out" -ForegroundColor Green

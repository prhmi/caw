# CAW - build libpd.dll with MULTI=true on Windows (MSYS2 MinGW-w64).
# Result: %USERPROFILE%\CAW-libpd\build-multi\libpd.dll (+ libwinpthread-1.dll); ~/.caw_libpd_path points CAW to it.
# Needs: MSYS2 (https://www.msys2.org) in C:\msys64, or set MSYS2_ROOT.
param([string]$Ref = $(if ($env:LIBPD_REF) { $env:LIBPD_REF } else { 'master' }))
$ErrorActionPreference = 'Stop'
$msys = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { 'C:\msys64' }
$bash = Join-Path $msys 'usr\bin\bash.exe'
if (-not (Test-Path $bash)) { throw "MSYS2 not found at $msys. Install it from https://www.msys2.org (or set MSYS2_ROOT)." }
$root = Join-Path $env:USERPROFILE 'CAW-libpd'
$src  = Join-Path $root 'source'
$out  = Join-Path $root 'build-multi'
New-Item -ItemType Directory -Force $root, $out | Out-Null
$srcU = ($src -replace '\\','/') -replace '^([A-Za-z]):','/$1'
$script = @"
set -e
pacman -S --needed --noconfirm git make mingw-w64-x86_64-gcc
export PATH=/mingw64/bin:`$PATH
[ -d '$srcU/.git' ] || git clone --recurse-submodules https://github.com/libpd/libpd.git '$srcU'
cd '$srcU'
git fetch --tags --quiet || true
git checkout --quiet $Ref
git submodule update --init --recursive --quiet
make clean >/dev/null 2>&1 || true
make MULTI=true -j`$(nproc)
"@
& $bash -lc $script
if ($LASTEXITCODE -ne 0) { throw 'libpd build failed' }
$dll = Get-ChildItem (Join-Path $src 'libs') -Filter 'libpd.dll' | Select-Object -First 1
if (-not $dll) { throw 'libpd.dll not found in libs\' }
Copy-Item $dll.FullName $out -Force
foreach ($d in 'libwinpthread-1.dll','libgcc_s_seh-1.dll','libstdc++-6.dll') {
  $f = Join-Path $msys "mingw64\bin\$d"; if (Test-Path $f) { Copy-Item $f $out -Force }
}
Set-Content (Join-Path $env:USERPROFILE '.caw_libpd_path') (Join-Path $out 'libpd.dll') -Encoding ASCII
Write-Host "`nDone: $out\libpd.dll  (CAW setting written to ~\.caw_libpd_path)" -ForegroundColor Green

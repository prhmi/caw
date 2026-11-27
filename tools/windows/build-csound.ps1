# CAW - build and install Csound 7 on Windows.
# Result: %LOCALAPPDATA%\Programs\Csound7\  (bin\csound.exe, bin\csound64.dll, plugins64\...)
#   installer\1-PREPARE.ps1 finds it there; its bin folder is added to your user PATH so CAW finds it too.
# Needs: Visual Studio 2022 (C++), Git, CMake. vcpkg is fetched by this script (Csound's own manifest).
param(
  [string]$Tag    = $(if ($env:CSOUND_TAG) { $env:CSOUND_TAG } else { '7.0.0-beta.18' }),
  [string]$Prefix = (Join-Path $env:LOCALAPPDATA 'Programs\Csound7')
)
$ErrorActionPreference = 'Stop'
function Step($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }
foreach ($t in 'git','cmake') { if (-not (Get-Command $t -ErrorAction SilentlyContinue)) { throw "'$t' not found - install it first" } }
$deps  = Join-Path $env:USERPROFILE '.caw_deps\csound'
$src   = Join-Path $deps 'source'
$build = Join-Path $deps 'build-x64'
$vcpkg = Join-Path $deps 'vcpkg'

Step "Csound source ($Tag)"
New-Item -ItemType Directory -Force $deps | Out-Null
if (-not (Test-Path (Join-Path $src '.git'))) { git clone https://github.com/csound/csound.git $src }
Push-Location $src
git fetch --tags --quiet
git checkout --quiet $Tag
if ($LASTEXITCODE -ne 0) { throw "tag $Tag not found (git tag | findstr 7.0)" }
git submodule update --init --recursive --quiet
Pop-Location

Step 'vcpkg (libsndfile, portaudio, portmidi ... from Csound''s vcpkg.json)'
if ($env:VCPKG_ROOT -and (Test-Path (Join-Path $env:VCPKG_ROOT 'vcpkg.exe'))) { $vcpkg = $env:VCPKG_ROOT }
elseif (-not (Test-Path (Join-Path $vcpkg 'vcpkg.exe'))) {
  if (-not (Test-Path $vcpkg)) { git clone https://github.com/microsoft/vcpkg.git $vcpkg }
  & (Join-Path $vcpkg 'bootstrap-vcpkg.bat') -disableMetrics
  if ($LASTEXITCODE -ne 0) { throw 'vcpkg bootstrap failed' }
}

Step 'Configure'
cmake -S $src -B $build -G 'Visual Studio 17 2022' -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$vcpkg\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DBUILD_TESTS=OFF -DBUILD_STATIC_LIBRARY=OFF -DUSE_DOUBLE=ON `
  "-DCMAKE_INSTALL_PREFIX=$Prefix"
if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed (see above)' }

Step 'Build (takes a while the first time: vcpkg builds the libraries)'
cmake --build $build --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

Step "Install to $Prefix"
cmake --install $build --config Release
if ($LASTEXITCODE -ne 0) { throw 'install failed' }
$bin = Join-Path $Prefix 'bin'
# the vcpkg DLLs (sndfile, portaudio ...) must sit beside csound64.dll
Get-ChildItem $build -Recurse -File -Filter '*.dll' | Where-Object { $_.DirectoryName -match '\\Release$' -and $_.Name -notmatch '^csound' } |
  ForEach-Object { if (-not (Test-Path (Join-Path $bin $_.Name))) { Copy-Item $_.FullName $bin } }

Step 'Check'
$api = Get-ChildItem $Prefix -Recurse -File -Include 'csound64*.dll','csound7*.dll' | Select-Object -First 1
if (-not $api) { throw 'no csound64.dll / csound7_64.dll was installed' }
Write-Host "  [ok]   API library: $($api.FullName)" -ForegroundColor Green
$exe = Join-Path $bin 'csound.exe'
if (Test-Path $exe) { & $exe --version 2>&1 | Select-Object -First 2 }
if (-not (Get-ChildItem $Prefix -Directory -Recurse -Filter 'plugins*' -ErrorAction SilentlyContinue)) { Write-Host '  [warn] no plugins* folder - some opcodes may be missing' -ForegroundColor Yellow }

Step 'User PATH'
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if (($userPath -split ';') -notcontains $bin) {
  [Environment]::SetEnvironmentVariable('Path', ($userPath.TrimEnd(';') + ';' + $bin), 'User')
  Write-Host "  [ok]   added $bin (open a new window / restart CAW)" -ForegroundColor Green
} else { Write-Host '  [ok]   already on PATH' -ForegroundColor Green }
Write-Host "`nDone. Restart CAW. If it still cannot find Csound, pick $($api.FullName) in Settings > Programs." -ForegroundColor Green

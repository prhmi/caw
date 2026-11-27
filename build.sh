#!/usr/bin/env bash
# CAW build script for macOS and Linux.   Windows: build.bat
set -e
cd "$(dirname "$0")"
OS="$(uname -s)"
JOBS="$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

need() { command -v "$1" >/dev/null 2>&1 || { echo "[ERROR] '$1' not found. $2"; exit 1; }; }
need git   "Install git first."
need cmake "Install CMake 3.20 or newer."

if [ "$OS" = "Darwin" ]; then
  xcode-select -p >/dev/null 2>&1 || { echo "[ERROR] Run: xcode-select --install"; exit 1; }
  GEN=()
  command -v ninja >/dev/null && GEN=(-G Ninja)
  EXTRA=(-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0)
  # The VST3 SDK asks `xcodebuild -version`, which fails with only the Command Line Tools (no full Xcode).
  # Then tell it a version ourselves (it reads the env var to skip its own check, and the cmake variable for the value).
  if ! xcodebuild -version 2>/dev/null | grep -q "^Xcode"; then
    export XCODE_VERSION=15.0
    EXTRA+=(-DXCODE_VERSION=15.0)
  fi
  [ -n "$CAW_UNIVERSAL" ] && EXTRA+=("-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64")
else
  GEN=(); command -v ninja >/dev/null && GEN=(-G Ninja)
  EXTRA=()
fi

CMAKE_ARGS=(); for a in "$@"; do [ "$a" = "nostart" ] || CMAKE_ARGS+=("$a"); done
echo "=== Configuring (dependencies are cached in ~/.caw_deps) ==="
cmake -B build "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "${EXTRA[@]}" "${CMAKE_ARGS[@]}" 2>&1 | tee caw_configure.log
[ "${PIPESTATUS[0]}" = "0" ] || {
  echo; echo "[FAILED] CMake configure failed, so nothing was built. The real error is in caw_configure.log:"
  grep -n -m 15 -E "CMake Error|FATAL_ERROR|fatal:|Could not|failed" caw_configure.log
  exit 1; }
echo "=== Building ==="
cmake --build build --config Release -j "$JOBS" 2>&1 | tee caw_build.log
[ "${PIPESTATUS[0]}" = "0" ] || { echo "[FAILED] see caw_build.log (first error)"; exit 1; }

if [ "$OS" = "Darwin" ]; then
  APP="build/caw.app"; [ -d "$APP" ] || APP="build/Release/caw.app"
  echo "Built: $APP"
  codesign --force --deep -s - "$APP" 2>/dev/null || true   # ad-hoc sign: Apple Silicon refuses unsigned binaries
  [ "$1" = "nostart" ] || open "$APP"
else
  echo "Built: build/caw"
  [ "$1" = "nostart" ] || ./build/caw &
fi

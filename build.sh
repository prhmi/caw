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
  [ -n "$CAW_UNIVERSAL" ] && EXTRA+=("-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64")
else
  GEN=(); command -v ninja >/dev/null && GEN=(-G Ninja)
  EXTRA=()
fi

echo "=== Configuring (dependencies are cached in ~/.caw_deps) ==="
cmake -B build "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "${EXTRA[@]}" "$@" 2>&1 | tee caw_configure.log
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

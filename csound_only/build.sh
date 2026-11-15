#!/usr/bin/env bash
# CAW build script for macOS and Linux.   Usage: ./build.sh [nostart]
set -eo pipefail
cd "$(dirname "$0")"

command -v git   >/dev/null || { echo "[ERROR] git was not found (needed once, to download Dear ImGui)."; exit 1; }
command -v cmake >/dev/null || { echo "[ERROR] cmake was not found."; exit 1; }

OS="$(uname -s)"
if [ "$OS" = "Linux" ]; then
  echo "On Debian/Ubuntu the build needs:  sudo apt install build-essential cmake git libgl1-mesa-dev libasound2-dev libjack-jackd2-dev xorg-dev"
  echo "For file dialogs install zenity:   sudo apt install zenity"
elif [ "$OS" = "Darwin" ]; then
  command -v xcode-select >/dev/null && xcode-select -p >/dev/null 2>&1 || { echo "[ERROR] Install the Xcode command line tools:  xcode-select --install"; exit 1; }
fi

echo "=== Configuring (first build downloads Dear ImGui, GLFW and PortAudio; this can take a few minutes) ==="
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 2>&1 | tee caw_configure.log
echo "=== Building ==="
cmake --build build --config Release -j 2>&1 | tee caw_build.log

if [ "$1" != "nostart" ]; then
  echo "=== Starting CAW ==="
  if [ "$OS" = "Darwin" ] && [ -d build/caw.app ]; then open build/caw.app; else ./build/caw & fi
fi
echo "Done."

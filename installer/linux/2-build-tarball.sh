#!/usr/bin/env bash
# CAW Linux installer - alternative step 2: stage/CAW as a .tar.gz (unpack anywhere, run ./caw).
# Smaller and simpler than the AppImage, but uses the system's libsndfile / fftw / jack / GL:
# the target PC needs the same packages (see HOW to Build.md, Linux 2.1).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
[ -x "$here/stage/CAW/caw" ] || { echo "[ERROR] run 1-prepare.sh first"; exit 1; }
VERSION="${CAW_VERSION:-$(date +%Y.%m.%d)}"
mkdir -p "$here/Output"
out="$here/Output/CAW-$VERSION-linux-$(uname -m).tar.gz"
tar -C "$here/stage" -czf "$out" CAW
echo "[SUCCESS] $out"
echo "Users: tar xzf it, then ./CAW/caw"

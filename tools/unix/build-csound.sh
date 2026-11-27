#!/usr/bin/env bash
# CAW - build and install Csound 7 (Linux / macOS).
# Result: libcsound64.so / .dylib + csound in $PREFIX (default /usr/local). CAW finds it there.
#   ./build-csound.sh
#   CSOUND_TAG=7.0.0-beta.18 PREFIX=/usr/local ./build-csound.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; . "$here/_common.sh"
CSOUND_TAG="${CSOUND_TAG:-7.0.0-beta.18}"
PREFIX="${PREFIX:-/usr/local}"
SRC="$DEPS/csound/source"; BUILD="$DEPS/csound/build"

step "Packages"
need git "Install git first."
if is_apt; then
  sudo apt-get install -y build-essential cmake ninja-build git flex bison \
    libsndfile1-dev libasound2-dev libjack-jackd2-dev portaudio19-dev \
    libportmidi-dev liblo-dev libfluidsynth-dev zlib1g-dev
elif is_mac; then
  mac_brew cmake ninja flex bison libsndfile portaudio portmidi liblo fluid-synth
  export PATH="$(brew --prefix bison)/bin:$(brew --prefix flex)/bin:$PATH"   # Apple's bison is too old
else warn "Unknown system: install cmake, flex, bison, libsndfile, portaudio yourself."; fi
need cmake "Install CMake 3.20+."

step "Source ($CSOUND_TAG)"
mkdir -p "$DEPS/csound"
[ -d "$SRC/.git" ] || git clone https://github.com/csound/csound.git "$SRC"
cd "$SRC"; git fetch --tags --quiet || true
git checkout --quiet "$CSOUND_TAG" || die "tag $CSOUND_TAG not found (git tag | grep '^7')"

step "Build"
GEN=(); command -v ninja >/dev/null && GEN=(-G Ninja)
cmake -S . -B "$BUILD" "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DBUILD_TESTS=OFF -DBUILD_STATIC_LIBRARY=OFF
cmake --build "$BUILD" -j "$JOBS"

step "Install to $PREFIX"
if [ -w "$PREFIX" ]; then cmake --install "$BUILD"; else sudo cmake --install "$BUILD"; fi
if [ "$OS" = "Linux" ]; then sudo ldconfig; fi

step "Check"
if [ "$OS" = "Linux" ]; then ldconfig -p | grep -i libcsound64 || warn "libcsound64 not in the linker cache"
else ls "$PREFIX/lib" | grep -i csound || warn "no csound library in $PREFIX/lib"; fi
"$PREFIX/bin/csound" --version 2>&1 | head -2 || true
echo; echo "Done. Restart CAW. If it still says 'Csound API library not found',"
echo "pick $PREFIX/lib/libcsound64.* in Settings > Programs."

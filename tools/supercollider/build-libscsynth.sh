#!/usr/bin/env bash
# CAW - build CAW's own SuperCollider runtime for Linux / macOS.
#   libscsynth with CAW's host-pumped driver (caw_sc_world_new / _process / _process_ex, ABI 5),
#   plus sclang, scsynth, the UGen plugins and SCClassLibrary.
# Result: ~/.caw_deps/supercollider/runtime-x64/   (CAW finds it there by itself)
#
# Usage:   ./build-libscsynth.sh                  (SuperCollider version: see SC_TAG below)
#          SC_TAG=Version-3.13.0 ./build-libscsynth.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
SC_TAG="${SC_TAG:-Version-3.14.0}"          # use the SAME version as your Windows build (git describe --tags)
DEPS="$HOME/.caw_deps/supercollider"
SRC="$DEPS/source"
BUILD="$DEPS/build-x64"
OUT="$DEPS/runtime-x64"
OS="$(uname -s)"
JOBS="$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

step() { printf '\n=== %s ===\n' "$1"; }
die()  { printf '[ERROR] %s\n' "$1" >&2; exit 1; }

step "Tools"
for t in git cmake; do command -v $t >/dev/null || die "'$t' not found"; done
if [ "$OS" = "Linux" ] && command -v apt-get >/dev/null; then
  echo "Installing build packages (sudo)..."
  sudo apt-get install -y build-essential cmake git libsndfile1-dev libasound2-dev \
    libjack-jackd2-dev libfftw3-dev libxt-dev libudev-dev libreadline-dev \
    libavahi-client-dev libncurses-dev
elif [ "$OS" = "Darwin" ]; then
  command -v brew >/dev/null || die "Homebrew is needed (https://brew.sh)"
  brew install libsndfile fftw readline >/dev/null || true
fi

step "SuperCollider source ($SC_TAG)"
mkdir -p "$DEPS"
if [ ! -d "$SRC/.git" ]; then
  git clone --recurse-submodules https://github.com/supercollider/supercollider.git "$SRC"
fi
cd "$SRC"
git fetch --tags --quiet || true
git reset --hard --quiet
git clean -fdq server/scsynth
git checkout --quiet "$SC_TAG" || die "tag $SC_TAG not found (git tag | grep Version)"
git submodule update --init --recursive --quiet

step "CAW changes"
cp "$here/SC_ExternalDriver.cpp" server/scsynth/SC_ExternalDriver.cpp
if git apply --check "$here/caw_sc.patch" 2>/dev/null; then
  git apply "$here/caw_sc.patch"
else
  echo "  patch does not apply cleanly to $SC_TAG - applying the 3 edits by text"
  for f in lang/CMakeLists.txt server/scsynth/CMakeLists.txt; do
    # add "external" to the allowed audio APIs, whatever list this SuperCollider version has
    grep -q 'AUDIOAPI MATCHES "^(external|' "$f" || \
      sed -i.bak 's/AUDIOAPI MATCHES "^(/AUDIOAPI MATCHES "^(external|/' "$f"
  done
  # insert_before FILE REGEX TEXT : put TEXT on its own lines before the FIRST line matching REGEX
  insert_before() {
    awk -v re="$2" -v txt="$3" 'BEGIN{done=0} { if (!done && $0 ~ re) { print txt; done=1 } print } END{ if (!done) exit 3 }' "$1" > "$1.new" \
      && mv "$1.new" "$1" || { rm -f "$1.new"; return 1; }
  }
  # insert_after_last FILE REGEX TEXT : put TEXT after the LAST line matching REGEX
  insert_after_last() {
    awk -v re="$2" -v txt="$3" '{ l[NR]=$0; if ($0 ~ re) last=NR } END{ if (!last) exit 3; for (i=1;i<=NR;i++){ print l[i]; if (i==last) print txt } }' "$1" > "$1.new" \
      && mv "$1.new" "$1" || { rm -f "$1.new"; return 1; }
  }
  F=server/scsynth/CMakeLists.txt
  grep -q 'SC_ExternalDriver.cpp' "$F" || \
    insert_before "$F" '^[[:space:]]*set[[:space:]]*[(][[:space:]]*FINAL_BUILD' $'if(AUDIOAPI STREQUAL external)\n list(APPEND scsynth_sources SC_ExternalDriver.cpp)\nendif()\n' || \
    insert_before "$F" '^[[:space:]]*if[[:space:]]*[(][[:space:]]*LIBSCSYNTH' $'if(AUDIOAPI STREQUAL external)\n list(APPEND scsynth_sources SC_ExternalDriver.cpp)\nendif()\n' || \
    die "could not find where to add SC_ExternalDriver.cpp in $F"
  grep -q 'SC_AUDIO_API=SC_AUDIO_API_EXTERNAL' "$F" || \
    insert_before "$F" 'target_compile_definitions[[:space:]]*[(][[:space:]]*libscsynth[[:space:]]+PRIVATE[[:space:]]+BUILDING_SCSYNTH' $'if(AUDIOAPI STREQUAL external)\n target_compile_definitions(libscsynth PRIVATE "SC_AUDIO_API=SC_AUDIO_API_EXTERNAL")\nendif()' || \
    die "could not find BUILDING_SCSYNTH in $F"
  H=server/scsynth/SC_CoreAudio.h
  # after the LAST "#define SC_AUDIO_API_<NAME> <number>" (3.13 ends at BELA 6, 3.14 at WEBAUDIO 7)
  grep -q 'SC_AUDIO_API_EXTERNAL 8' "$H" || \
    insert_after_last "$H" '^#define[[:space:]]+SC_AUDIO_API_[A-Z_]+[[:space:]]+[0-9]+' '#define SC_AUDIO_API_EXTERNAL 8' || \
    die "could not find the SC_AUDIO_API_ defines in $H"
  find . -name '*.bak' -delete
fi
for f in lang/CMakeLists.txt server/scsynth/CMakeLists.txt; do
  grep -Eq 'AUDIOAPI MATCHES "\^\(.*external' "$f" || die "CAW edit missing in $f (the audio API list)"
done
grep -q 'SC_ExternalDriver.cpp' server/scsynth/CMakeLists.txt || die "CAW edit missing: SC_ExternalDriver.cpp is not in server/scsynth/CMakeLists.txt"
grep -q 'SC_AUDIO_API_EXTERNAL 8' server/scsynth/SC_CoreAudio.h || die "CAW edit missing in SC_CoreAudio.h"
echo "  [ok]   CAW edits applied"

step "Build libscsynth (external driver)"
# libscsynth gets the external driver; sclang / scsynth use the normal audio API (they are not
# the audio path in CAW - sclang only talks to the embedded server over OSC).
EXTRA=()
[ "$OS" = "Darwin" ] && EXTRA+=(-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DSC_ABLETON_LINK=OFF)
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DSC_QT=OFF -DSC_IDE=OFF -DSC_ED=OFF -DSC_EL=OFF -DSC_VIM=OFF -DSUPERNOVA=OFF \
  -DLIBSCSYNTH=ON -DAUDIOAPI=external -DNO_X11=ON -DSC_HIDAPI=OFF \
  -DCMAKE_INSTALL_PREFIX="$OUT" "${EXTRA[@]}"
cmake --build "$BUILD" -j "$JOBS"

step "Collect the runtime"
rm -rf "$OUT"; mkdir -p "$OUT/SuperCollider"
R="$OUT/SuperCollider"
lib=$(find "$BUILD" -name 'libscsynth.so*' -o -name 'libscsynth*.dylib' | grep -v '\.a$' | head -1)
[ -n "$lib" ] || die "libscsynth was not built"
cp -L "$lib" "$R/$( [ "$OS" = "Darwin" ] && echo libscsynth.dylib || echo libscsynth.so )"
printf '%s\n' "$R/$( [ "$OS" = "Darwin" ] && echo libscsynth.dylib || echo libscsynth.so )" > "$HOME/.caw_scsynth_path"
for b in sclang scsynth; do
  f=$(find "$BUILD" -type f -name "$b" -perm -u+x | head -1)
  [ -n "$f" ] && cp "$f" "$R/" || echo "  [warn] $b not found"
done
cp -R SCClassLibrary "$R/"
mkdir -p "$R/plugins"
find "$BUILD/server/plugins" \( -name '*.so' -o -name '*.scx' \) -exec cp {} "$R/plugins/" \; 2>/dev/null || true
cp COPYING "$R/SuperCollider-LICENSE.txt" 2>/dev/null || true

step "Check"
LIB="$R/$( [ "$OS" = "Darwin" ] && echo libscsynth.dylib || echo libscsynth.so )"
if [ "$OS" = "Darwin" ]; then syms=$(nm -gU "$LIB"); else syms=$(nm -D --defined-only "$LIB"); fi
ok=1
for s in caw_sc_abi_version caw_sc_world_new caw_sc_world_process caw_sc_world_process_ex caw_sc_world_free caw_sc_last_error; do
  if echo "$syms" | grep -q "$s"; then echo "  [ok]   $s"; else echo "  [MISS] $s"; ok=0; fi
done
[ "$ok" = 1 ] || die "libscsynth has no CAW entry points - the external driver was not compiled in"
ls -la "$R"
echo
[ -x "$R/sclang" ] && printf '%s\n' "$R/sclang" > "$HOME/.caw_sclang_path"
echo "Done. CAW finds it in $R (settings written: ~/.caw_sclang_path, ~/.caw_scsynth_path)"
echo "(or set it by hand in Settings > Programs: sclang + $(basename "$LIB"))"

#!/usr/bin/env bash
# CAW macOS installer - step 1: put the engines INSIDE CAW.app
#   stage/CAW.app/Contents/Resources/{Csound,PureData,SuperCollider}
# Then run 2-build-dmg.sh.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
deps="$HOME/.caw_deps"
warns=0
step() { printf '\n=== %s ===\n' "$1"; }
ok()   { printf '  [ok]   %s\n' "$1"; }
warn() { printf '  [warn] %s\n' "$1"; warns=$((warns+1)); }
size() { du -sh "$1" 2>/dev/null | cut -f1; }

src=""; for c in "$root/build/caw.app" "$root/build/Release/caw.app"; do [ -d "$c" ] && { src="$c"; break; }; done
[ -n "$src" ] || { echo "[ERROR] caw.app not found in build/. Run ./build.sh first."; exit 1; }
rm -rf "$here/stage"; mkdir -p "$here/stage"
app="$here/stage/CAW.app"
step "CAW"
cp -R "$src" "$app"
res="$app/Contents/Resources"; mkdir -p "$res"
ok "CAW.app ($(size "$app"))"
newest=$(find "$root/src" -type f \( -name '*.cpp' -o -name '*.h' \) -newer "$src/Contents/MacOS/caw" | head -1)
[ -n "$newest" ] && warn "caw.app is OLDER than $(basename "$newest") - run ./build.sh again first."

step "Csound 7"
csexe=""; for c in /usr/local/bin/csound /opt/homebrew/bin/csound "$(command -v csound 2>/dev/null)"; do [ -n "$c" ] && [ -x "$c" ] && { csexe="$c"; break; }; done
cslib=$(ls /usr/local/lib/libcsound64*.dylib 2>/dev/null | head -1)
if [ -n "$csexe" ] && [ -n "$cslib" ]; then
  cs="$res/Csound"; mkdir -p "$cs/bin" "$cs/lib/csound"
  cp -L "$csexe" "$cs/bin/csound"
  cp -a "$(dirname "$cslib")"/libcsound64*.dylib "$cs/bin/"
  plug=$(ls -d "$(dirname "$cslib")"/csound/plugins64* 2>/dev/null | head -1)
  [ -n "$plug" ] && cp -a "$plug" "$cs/lib/csound/" || warn "no opcode plugin folder next to $cslib"
  ok "Csound ($(size "$cs"))"
else warn "Csound 7 not found. Run tools/unix/build-csound.sh first."; fi

step "Pure Data (libpd)"
pd=""; [ -f "$HOME/.caw_libpd_path" ] && pd="$(head -1 "$HOME/.caw_libpd_path")"
[ -f "$pd" ] || pd="$deps/libpd/libpd.dylib"
if [ -f "$pd" ]; then mkdir -p "$res/PureData"; cp -L "$pd" "$res/PureData/libpd.dylib"; ok "libpd"
else warn "libpd not found. Run tools/unix/build-libpd.sh first."; fi

step "SuperCollider (CAW's runtime)"
sc="$deps/supercollider/runtime-x64/SuperCollider"
if [ -x "$sc/sclang" ] && [ -f "$sc/libscsynth.dylib" ]; then cp -a "$sc" "$res/SuperCollider"; ok "SuperCollider ($(size "$res/SuperCollider"))"
else warn "SuperCollider runtime not found. Run tools/unix/build-supercollider.sh first."; fi

step "Homebrew libraries the engines use"
# The engines link against Homebrew's libsndfile / fftw / portaudio. Copy them into
# Contents/Frameworks and point the engines at them, so the app runs on a Mac without Homebrew.
fw="$app/Contents/Frameworks"; mkdir -p "$fw"
copy_deps() {   # $1 = a Mach-O file
  otool -L "$1" 2>/dev/null | awk 'NR>1{print $1}' | grep -E '^(/opt/homebrew|/usr/local)/' | while read -r dep; do
    base="$(basename "$dep")"
    if [ ! -f "$fw/$base" ]; then cp -L "$dep" "$fw/$base"; chmod u+w "$fw/$base"; copy_deps "$fw/$base"; fi
    install_name_tool -change "$dep" "@executable_path/../Frameworks/$base" "$1" 2>/dev/null || true
  done
}
find "$res" "$app/Contents/MacOS" -type f \( -perm -u+x -o -name '*.dylib' \) | while read -r f; do
  file "$f" | grep -q Mach-O && copy_deps "$f"
done
ok "bundled $(ls "$fw" | wc -l | tr -d ' ') librar(y/ies) into Contents/Frameworks"

step "Sign (ad-hoc)"
codesign --force --deep -s - "$app" && ok "signed" || warn "codesign failed"

step "Summary"
echo "  $app ($(size "$app"))"
if [ "$warns" -gt 0 ]; then printf '\n  %d warning(s) above. Fix them first, or that part will be missing.\n' "$warns"; exit 2; fi
echo; echo "  Ready. Now run 2-build-dmg.sh"

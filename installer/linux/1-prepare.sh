#!/usr/bin/env bash
# CAW Linux installer - step 1: collect everything the package will contain.
#   stage/CAW/            caw + Csound/ + PureData/ + SuperCollider/ + examples/ + licences
# Safe to run again: stage/ is rebuilt each time. Then run 2-build-appimage.sh (or 2-build-tarball.sh).
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
stage="$here/stage/CAW"
deps="$HOME/.caw_deps"
warns=0
step() { printf '\n=== %s ===\n' "$1"; }
ok()   { printf '  [ok]   %s\n' "$1"; }
warn() { printf '  [warn] %s\n' "$1"; warns=$((warns+1)); }
size() { du -sh "$1" 2>/dev/null | cut -f1; }

rm -rf "$here/stage"; mkdir -p "$stage"

step "CAW"
exe="$root/build/caw"
[ -x "$exe" ] || { echo "[ERROR] $exe not found. Run ./build.sh first."; exit 1; }
cp "$exe" "$stage/caw"; strip "$stage/caw" 2>/dev/null || true
ok "caw ($(size "$stage/caw"))"
newest=$(find "$root/src" -type f \( -name '*.cpp' -o -name '*.h' \) -newer "$exe" | head -1)
[ -n "$newest" ] && warn "caw is OLDER than $(basename "$newest") - run ./build.sh again first, or the package ships old code."

step "Csound 7"
csexe=""; for c in /usr/local/bin/csound "$(command -v csound 2>/dev/null)"; do [ -n "$c" ] && [ -x "$c" ] && { csexe="$c"; break; }; done
cslib=$(ls /usr/local/lib/libcsound64.so* 2>/dev/null | head -1)
[ -z "$cslib" ] && cslib=$(ldconfig -p 2>/dev/null | awk '/libcsound64\.so/{print $NF; exit}')
if [ -n "$csexe" ] && [ -n "$cslib" ]; then
  cs="$stage/Csound"; mkdir -p "$cs/bin" "$cs/lib/csound"
  cp -L "$csexe" "$cs/bin/csound"
  libdir="$(dirname "$cslib")"
  cp -a "$libdir"/libcsound64.so* "$cs/bin/"   # beside csound: CAW loads the API library next to the program
  plug=$(ls -d "$libdir"/csound/plugins64* 2>/dev/null | head -1)
  if [ -n "$plug" ]; then cp -a "$plug" "$cs/lib/csound/"; ok "opcode plugins: $(basename "$plug") ($(ls "$plug" | wc -l) files)"
  else warn "no opcode plugin folder (plugins64*) next to $cslib - many opcodes would be missing"; fi
  "$cs/bin/csound" --version 2>&1 | head -1 | sed 's/^/         /'
  ok "Csound ($(size "$cs"))"
else
  warn "Csound 7 not found (/usr/local). Run tools/unix/build-csound.sh first."
fi

step "Pure Data (libpd)"
pd=""; [ -f "$HOME/.caw_libpd_path" ] && pd="$(head -1 "$HOME/.caw_libpd_path" | tr -d '\r' | sed 's/[[:space:]]*$//')"
for c in "$deps/libpd/libpd.so" "$deps/libpd/source/libs/libpd.so" "$HOME/libpd/libs/libpd.so"; do
  [ -f "$pd" ] && break; pd="$c"
done
if [ -f "$pd" ]; then
  mkdir -p "$stage/PureData"; cp -L "$pd" "$stage/PureData/libpd.so"
  nm -D --defined-only "$stage/PureData/libpd.so" | grep -q libpd_new_instance || warn "this libpd is not MULTI=true - CAW refuses it"
  ok "libpd from $pd"
else warn "libpd not found. Run tools/unix/build-libpd.sh first."; fi

step "SuperCollider (CAW's runtime)"
sc="$deps/supercollider/runtime-x64/SuperCollider"
if [ -x "$sc/sclang" ] && [ -f "$sc/libscsynth.so" ]; then
  cp -a "$sc" "$stage/SuperCollider"
  nm -D --defined-only "$stage/SuperCollider/libscsynth.so" | grep -q caw_sc_world_process_ex || warn "libscsynth has no CAW driver - rebuild with tools/unix/build-supercollider.sh"
  ok "SuperCollider ($(size "$stage/SuperCollider"))"
else warn "SuperCollider runtime not found in $sc. Run tools/unix/build-supercollider.sh first."; fi

step "Examples and licences"
[ -d "$root/examples" ] && { cp -a "$root/examples" "$stage/"; ok "examples"; }
cp "$root/README.md" "$stage/" 2>/dev/null || true
cat > "$stage/LICENSES-READ-ME.txt" <<'EOF'
CAW - Code Audio Workstation

This package contains CAW together with third-party software. Each part keeps its own licence:
  SuperCollider (SuperCollider/)  GNU GPL v3   - github.com/supercollider/supercollider
  libpd / Pure Data (PureData/)   BSD-style    - github.com/libpd/libpd
  Csound (Csound/)                GNU LGPL 2.1 - github.com/csound/csound
  PortAudio, Dear ImGui, GLFW     MIT / zlib (linked into caw)
  VST3 SDK                        Steinberg VST3 licence / GPLv3 (linked into caw)
EOF
ok "LICENSES-READ-ME.txt"

step "Libraries the engines need from the system"
# Csound / SuperCollider link against libsndfile, fftw, jack...: list what each needs, so the
# AppImage step can bundle them (it bundles everything except the base system libraries).
for f in "$stage/caw" "$stage"/Csound/bin/libcsound64.so* "$stage/PureData/libpd.so" "$stage/SuperCollider/libscsynth.so" "$stage/SuperCollider/sclang"; do
  [ -e "$f" ] || continue
  miss=$(ldd "$f" 2>/dev/null | awk '/not found/{print $1}')
  [ -n "$miss" ] && warn "$(basename "$f") needs missing libraries: $miss"
done
ok "checked with ldd"

step "Summary"
echo "  stage: $(size "$here/stage")  ($stage)"
if [ "$warns" -gt 0 ]; then
  printf '\n  %d warning(s) above. Fix them first, or that part will be missing from the package.\n' "$warns"; exit 2
fi
echo; echo "  Ready. Now run 2-build-appimage.sh (one file) or 2-build-tarball.sh (a .tar.gz)."

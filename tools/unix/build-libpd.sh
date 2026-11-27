#!/usr/bin/env bash
# CAW - build libpd with MULTI=true (Linux / macOS).
# Result: ~/.caw_deps/libpd/libpd.so (.dylib), and ~/.caw_libpd_path points CAW to it.
#   ./build-libpd.sh
#   LIBPD_REF=0.14.1 ./build-libpd.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; . "$here/_common.sh"
LIBPD_REF="${LIBPD_REF:-master}"
SRC="$DEPS/libpd/source"; OUT="$DEPS/libpd"

step "Packages"
need git "Install git first."
if is_apt; then sudo apt-get install -y build-essential git
elif is_mac; then xcode-select -p >/dev/null 2>&1 || die "Run: xcode-select --install"; fi
need make "Install make / build tools."

step "Source ($LIBPD_REF)"
mkdir -p "$OUT"
[ -d "$SRC/.git" ] || git clone --recurse-submodules https://github.com/libpd/libpd.git "$SRC"
cd "$SRC"; git fetch --tags --quiet || true
git checkout --quiet "$LIBPD_REF"
git submodule update --init --recursive --quiet

step "Build (MULTI=true - CAW runs one Pd instance per clip)"
make clean >/dev/null 2>&1 || true
make MULTI=true -j "$JOBS"
LIB=$(ls libs/libpd.so libs/libpd.dylib 2>/dev/null | head -1)
[ -n "$LIB" ] || die "libpd was not built (see the output above)"
cp -L "$LIB" "$OUT/"
DEST="$OUT/$(basename "$LIB")"

step "Check"
if [ "$OS" = "Darwin" ]; then syms=$(nm -gU "$DEST"); else syms=$(nm -D --defined-only "$DEST"); fi
for s in libpd_init libpd_new_instance libpd_set_instance libpd_process_float; do
  echo "$syms" | grep -q "$s" && ok "$s" || die "$s missing - not a MULTI=true build"
done
printf '%s\n' "$DEST" > "$HOME/.caw_libpd_path"
ok "CAW setting written: ~/.caw_libpd_path -> $DEST"
echo; echo "Done. Restart CAW (or pick $DEST in Settings > Programs > Pure Data)."

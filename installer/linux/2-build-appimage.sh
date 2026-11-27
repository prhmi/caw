#!/usr/bin/env bash
# CAW Linux installer - step 2: pack stage/CAW into CAW-<version>-x86_64.AppImage
#   one file: chmod +x it and double-click / run it. Nothing to install, nothing else needed.
# Uses linuxdeploy (downloaded into tools/ once) to bundle the libraries caw and the engines need.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
stage="$here/stage/CAW"
VERSION="${CAW_VERSION:-$(date +%Y.%m.%d)}"
arch="$(uname -m)"
[ -x "$stage/caw" ] || { echo "[ERROR] stage/CAW/caw is missing. Run 1-prepare.sh first."; exit 1; }

tools="$here/tools"; mkdir -p "$tools"
ld="$tools/linuxdeploy-$arch.AppImage"
if [ ! -x "$ld" ]; then
  echo "Downloading linuxdeploy..."
  url="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$arch.AppImage"
  if command -v curl >/dev/null; then curl -fL -o "$ld.part" "$url"
  elif command -v wget >/dev/null; then wget -O "$ld.part" "$url"
  else echo "[ERROR] Neither curl nor wget is installed: sudo apt install curl"; exit 1; fi
  mv -f "$ld.part" "$ld"; chmod +x "$ld"
fi

app="$here/AppDir"; rm -rf "$app"
mkdir -p "$app/usr/bin" "$app/usr/lib/caw" "$app/usr/share/applications"
cp "$stage/caw" "$app/usr/bin/caw"
for d in Csound PureData SuperCollider examples; do [ -d "$stage/$d" ] && cp -a "$stage/$d" "$app/usr/lib/caw/"; done
cp "$stage"/*.txt "$stage"/*.md "$app/usr/lib/caw/" 2>/dev/null || true

cat > "$app/usr/share/applications/caw.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=CAW
GenericName=Code Audio Workstation
Comment=A timeline DAW where every clip is code
Exec=caw %f
Icon=caw
Categories=AudioVideo;Audio;Music;
MimeType=application/x-caw-project;
Terminal=false
EOF
icon="$root/res/caw.png"
[ -f "$icon" ] || { echo "[ERROR] $icon is missing (the AppImage needs an icon)."; exit 1; }
# linuxdeploy checks that the icon's pixel size matches its folder: read it from the PNG header
px=$(od -An -tu1 -j16 -N4 "$icon" | awk '{print $1*16777216+$2*65536+$3*256+$4}')
icondir="$app/usr/share/icons/hicolor/${px}x${px}/apps"; mkdir -p "$icondir"
cp "$icon" "$icondir/caw.png"

# Engine libraries are loaded at run time (dlopen), so linuxdeploy cannot see them: pass them
# explicitly so their own dependencies (libsndfile, fftw, ...) are bundled too.
extra=()
while IFS= read -r f; do extra+=(--library "$f"); done < <(find "$app/usr/lib/caw" -type f \( -name '*.so' -o -name '*.so.*' \) 2>/dev/null)
for x in "$app/usr/lib/caw/Csound/bin/csound" "$app/usr/lib/caw/SuperCollider/sclang" "$app/usr/lib/caw/SuperCollider/scsynth"; do
  [ -x "$x" ] && extra+=(--executable "$x")
done

export VERSION APPIMAGE_EXTRACT_AND_RUN=1
cd "$here"
"$ld" --appdir "$app" --executable "$app/usr/bin/caw" \
      --desktop-file "$app/usr/share/applications/caw.desktop" \
      --icon-file "$icondir/caw.png" \
      "${extra[@]}" --output appimage
mkdir -p "$here/Output"
mv -f "$here"/CAW*.AppImage "$here/Output/" 2>/dev/null || mv -f "$here"/*.AppImage "$here/Output/"
echo; echo "[SUCCESS] $(ls "$here"/Output/*.AppImage)"
echo "Users: chmod +x the file, then run it. CAW finds the engines inside it."
echo "If it says 'dlopen(): error loading libfuse.so.2': sudo apt install libfuse2t64  (Ubuntu 24.04+; libfuse2 on older)"
echo "  or run it without FUSE:  ./CAW-*.AppImage --appimage-extract-and-run"

#!/usr/bin/env bash
# CAW macOS installer - step 2: stage/CAW.app -> Output/CAW-<version>.dmg (drag to Applications).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
app="$here/stage/CAW.app"
[ -d "$app" ] || { echo "[ERROR] stage/CAW.app is missing. Run 1-prepare.sh first."; exit 1; }
VERSION="${CAW_VERSION:-$(date +%Y.%m.%d)}"
mkdir -p "$here/Output"
dmgsrc="$here/dmg"; rm -rf "$dmgsrc"; mkdir -p "$dmgsrc"
cp -R "$app" "$dmgsrc/"
ln -s /Applications "$dmgsrc/Applications"
out="$here/Output/CAW-$VERSION.dmg"
rm -f "$out"
hdiutil create -volname "CAW $VERSION" -srcfolder "$dmgsrc" -ov -format UDZO "$out"
rm -rf "$dmgsrc"
echo; echo "[SUCCESS] $out"
echo "Users: open the .dmg, drag CAW to Applications. First start: right-click CAW > Open."
echo "For distribution without that warning: sign with a Developer ID and notarize (see README.md)."

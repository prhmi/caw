# Installers

| System | Folder | Steps | Result |
|---|---|---|---|
| Windows | `installer\` | `1-PREPARE.bat` → `2-BUILD-INSTALLER.bat` (Inno Setup) | `Output\CAW-Setup-<version>.exe` |
| Linux | `installer/linux/` | `1-prepare.sh` → `2-build-appimage.sh` (or `2-build-tarball.sh`) | `Output/CAW-<version>-x86_64.AppImage` |
| macOS | `installer/macos/` | `1-prepare.sh` → `2-build-dmg.sh` | `Output/CAW-<version>.dmg` |

Each one bundles CAW together with **Csound 7**, **libpd** and **CAW's SuperCollider**, so users install nothing else. Build CAW and the engines first (see `HOW to Build.md`). Step 1 warns about anything missing.

**Windows:** copy your Inno Setup script `CAW-Setup.iss` into `installer\`. It isn't in this repository yet.

CAW looks for the bundled engines next to itself first:

| System | Folders |
|---|---|
| Windows | `Csound\`, `PureData\`, `SuperCollider\` beside `caw.exe` |
| Linux (AppImage) | `usr/lib/caw/` |
| macOS | `CAW.app/Contents/Resources/` |

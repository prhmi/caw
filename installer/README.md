# Windows installer

1. `build.bat` (project root) builds `build\Release\caw.exe`.
2. `1-PREPARE.bat` collects caw.exe, the SuperCollider runtime, libpd and a Csound copy into `stage\`, and downloads the VC++ runtime into `redist\`.
3. `2-BUILD-INSTALLER.bat` packs them with Inno Setup into `Output\CAW-Setup-<version>.exe`.

**Needs:** `CAW-Setup.iss`, the Inno Setup script. It isn't in this repository yet: copy yours into this folder.

The engines come from `tools\windows\` (SuperCollider, libpd) and an installed Csound 7.

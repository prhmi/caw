# How to build CAW

The short version. Every step, and what each script does inside, is in **[HOW to Build detailed.md](HOW%20to%20Build%20detailed.md)**.

CAW is one program. The engines are **separate** libraries it loads at run time. Build CAW first, then only the engines you use, one at a time, in any order. Without an engine CAW still runs; VST and sample tracks need none.

> **Status:** Windows is tested. The Linux build of CAW runs (Ubuntu 24.10). These parts are new and not fully tested yet: the macOS build, the VST3 editor windows on Linux (X11) and macOS (Cocoa), the engine scripts, and the Linux/macOS packages.

---

## Windows

**Tools (once):** Visual Studio 2022 with *Desktop development with C++*, Git, CMake.

**1. CAW:** run `build.bat`. The result is `build\Release\caw.exe`.

**2. Engines:** each one separately, from `tools\windows\`:

| Engine | Run | Also needs |
|---|---|---|
| Csound 7 | `BUILD-CSOUND.bat` (or the official Csound 7 installer) | — |
| Pure Data | `BUILD-LIBPD.bat` | MSYS2 in `C:\msys64` |
| SuperCollider | `BUILD-LIBSCSYNTH.bat` | vcpkg with `libsndfile` + `fftw3`, `VCPKG_ROOT` set |

**3. Installer (optional):** copy `CAW-Setup.iss` into `installer\`, run `1-PREPARE.bat`, then `2-BUILD-INSTALLER.bat`. The result is `installer\Output\CAW-Setup-<version>.exe`.

---

## Linux

**Tools (once, Ubuntu):**
```bash
sudo apt install -y build-essential cmake ninja-build git unzip \
  libasound2-dev libjack-jackd2-dev libgl1-mesa-dev \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
  libxkbcommon-dev libwayland-dev wayland-protocols fonts-dejavu
```

**1. CAW:**
```bash
chmod +x build.sh && ./build.sh nostart
./build/caw
```

**2. Engines:** each one separately, from `tools/unix/`:
```bash
cd tools/unix && chmod +x *.sh ../supercollider/*.sh
./build-csound.sh                                # Csound 7
./build-libpd.sh                                 # Pure Data (MULTI=true)
SC_TAG=Version-3.13.0 ./build-supercollider.sh   # CAW's SuperCollider
```
Or all three in one go: `./build-engines.sh`. Restart CAW afterwards.

**3. Package (optional):** in `installer/linux/`, run `./1-prepare.sh`, then `./2-build-appimage.sh`. The result is `Output/CAW-<version>-x86_64.AppImage`.

**VST3:** put Linux `.vst3` plugins in `~/.vst3`. **Open** shows the plugin's editor in an X11 window (needs X11 or XWayland).

---

## macOS

**Tools (once):** `xcode-select --install`, then `brew install cmake ninja git`.

**1. CAW:** `./build.sh nostart` (or `CAW_UNIVERSAL=1 ./build.sh nostart` for Intel + Apple Silicon). The result is `build/caw.app`. The first time, right‑click → **Open**.

**2. Engines:** the same scripts as Linux, in `tools/unix/`. They use Homebrew.

**3. Package (optional):** in `installer/macos/`, run `./1-prepare.sh`, then `./2-build-dmg.sh`. The result is `Output/CAW-<version>.dmg`.

**VST3:** put macOS `.vst3` plugins in `~/Library/Audio/Plug-Ins/VST3`. **Open** shows the plugin's editor in a Cocoa window.

---

## SuperCollider version

Use the **same** version on every system. On Windows, `git describe --tags` in `%USERPROFILE%\.caw_deps\supercollider\source` shows it. Pass it as `SC_TAG`. A normal SuperCollider install doesn't work with CAW; it needs CAW's driver from `tools/supercollider/`.

---

## Something failed?

- Find the **first** error: `grep -n -m 20 "error" caw_build.log`, or the last lines of the engine script.
- The common errors and their fixes are listed in **[HOW to Build detailed.md → When something fails](HOW%20to%20Build%20detailed.md#when-something-fails)**.

# How to build CAW

This guide builds CAW and the sound engines it uses, on **Windows**, **Linux** and **macOS**.

CAW is one program. The engines are libraries it loads **at run time**, so you can build CAW first and add the engines later:

| Part | Needed for | Built by |
|---|---|---|
| **CAW** (+ VST3 host) | everything | `build.bat` / `build.sh` |
| **Csound 7** | Csound tracks, offline render of Csound clips | `BUILD-CSOUND` / `build-csound.sh` |
| **Pure Data** (libpd, `MULTI=true`) | PD tracks | `BUILD-LIBPD` / `build-libpd.sh` |
| **SuperCollider** (CAW's own build) | SC tracks | `BUILD-LIBSCSYNTH` / `build-supercollider.sh` |

Without an engine, CAW still runs; its tracks just say the engine wasn't found. VST and sample tracks need no engine.

> **Status:** the Windows build of CAW is the tested one. The Linux build of CAW has been built and run on Ubuntu 24.10. The engine scripts for Windows, the Linux SuperCollider script and everything on macOS are new and not fully tested yet. If a step fails, the log tells you why (see [When something fails](#when-something-fails)).

---

## Contents

1. [Windows](#1-windows)
2. [Linux](#2-linux)
3. [macOS](#3-macos)
4. [SuperCollider version](#4-supercollider-version)
5. [Where things go](#5-where-things-go)
6. [When something fails](#when-something-fails)

---

## 1. Windows

### 1.1 Install the tools (once)

- **Visual Studio 2022** (Community is free) with the workload **Desktop development with C++**
- **Git** for Windows: <https://git-scm.com>
- **CMake** 3.20 or newer (Visual Studio already includes one)

### 1.2 Build CAW

Double-click **`build.bat`** in the project folder, or run it in a terminal.

- The first build downloads Dear ImGui, PortAudio, GLFW, the Steinberg ASIO SDK, the **VST3 SDK** and FFmpeg into `%USERPROFILE%\.caw_deps`. Later builds work offline.
- Result: **`build\Release\caw.exe`**. The script starts it at the end; run `build.bat nostart` to skip that.
- Logs: `caw_configure.log` and `caw_build.log` in the project folder.

### 1.3 Engines

Run each script once. They are all in `tools\windows\`.

**Csound 7:** `BUILD-CSOUND.bat`
- Needs Visual Studio, Git, CMake. It downloads vcpkg for Csound's libraries.
- Installs to `%LOCALAPPDATA%\Programs\Csound7\` and adds its `bin` folder to your user PATH.
- Or install the official Csound 7 release instead. CAW finds either one.

**Pure Data:** `BUILD-LIBPD.bat`
- Needs **MSYS2** (<https://www.msys2.org>) in `C:\msys64`.
- Builds `libpd.dll` with `MULTI=true` into `%USERPROFILE%\CAW-libpd\build-multi\`, together with `libwinpthread-1.dll`.

**SuperCollider:** `BUILD-LIBSCSYNTH.bat`
- Needs Visual Studio, Git, CMake and **vcpkg** with `libsndfile` and `fftw3`. Set `VCPKG_ROOT` to your vcpkg folder.
- Builds CAW's own `libscsynth.dll`, `sclang.exe`, `scsynth.exe` into `%USERPROFILE%\.caw_deps\supercollider\runtime-x64\SuperCollider\`.
- Use the same SuperCollider version on every system: see [section 4](#4-supercollider-version).

Restart CAW after building an engine. If it isn't found, pick its file in **Settings → Programs**.

### 1.4 Windows installer (optional)

Makes `CAW-Setup-<version>.exe` that contains CAW and all the engines, so users install nothing else.

1. Build CAW (1.2) and the engines (1.3).
2. Copy your Inno Setup script **`CAW-Setup.iss`** into `installer\`.
3. Run **`installer\1-PREPARE.bat`**. It collects everything into `installer\stage\` and downloads the VC++ runtime.
4. Run **`installer\2-BUILD-INSTALLER.bat`**. It offers to install Inno Setup 6 if it's missing.
5. The setup is in **`installer\Output\`**.

`1-PREPARE` stops with warnings if something is missing. Fix them first, or that part won't be in the setup.

---

## 2. Linux

Tested on Ubuntu. Other distributions work with the same steps and their own package names.

### 2.1 Install the tools (once)

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build git unzip \
  libasound2-dev libjack-jackd2-dev libgl1-mesa-dev \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
  libxkbcommon-dev libwayland-dev wayland-protocols fonts-dejavu
```

<details>
<summary>Fedora</summary>

```bash
sudo dnf install gcc-c++ cmake ninja-build git alsa-lib-devel \
  jack-audio-connection-kit-devel mesa-libGL-devel libX11-devel \
  libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel \
  libxkbcommon-devel wayland-devel dejavu-sans-fonts
```
</details>

<details>
<summary>Ubuntu 24.10 ("apt update" fails with "no longer has a Release file")</summary>

24.10 is no longer supported, so its servers have moved to `old-releases`:

```bash
sudo sed -i -e 's|http://[a-z.]*archive.ubuntu.com|http://old-releases.ubuntu.com|g' \
            -e 's|http://security.ubuntu.com|http://old-releases.ubuntu.com|g' \
            /etc/apt/sources.list.d/ubuntu.sources
sudo apt update
```
Ubuntu 24.04 LTS avoids this.
</details>

### 2.2 Build CAW

```bash
cd caw
chmod +x build.sh
./build.sh nostart
./build/caw
```

- The result is **`build/caw`**.
- Build inside your home folder (`~`), not on a shared Windows drive (`/mnt/c`, `/media/sf_...`). It's much faster.
- Logs: `caw_configure.log`, `caw_build.log`. Show the first errors with `grep -n -m 20 "error" caw_build.log`.

### 2.3 Engines

```bash
cd tools/unix
chmod +x *.sh ../supercollider/*.sh
./build-engines.sh                 # all three
./build-engines.sh csound pd       # or only some
```

Each script installs the packages it needs, downloads the source into `~/.caw_deps`, builds, checks the result and tells CAW where it is:

| Script | Result | CAW finds it through |
|---|---|---|
| `build-csound.sh` | Csound 7 in `/usr/local` (`libcsound64.so`) | the system library path |
| `build-libpd.sh` | `~/.caw_deps/libpd/libpd.so` | `~/.caw_libpd_path` |
| `build-supercollider.sh` | `~/.caw_deps/supercollider/runtime-x64/SuperCollider/` | `~/.caw_sclang_path`, `~/.caw_scsynth_path` |

At the end, `build-engines.sh` prints `ok` or `FAILED` for each engine. **Close CAW completely and start it again** afterwards; it only looks for the engines when it starts.

### 2.4 VST3 plugins

Nothing to build: the VST3 SDK is inside `caw`. You need **Linux** `.vst3` plugins (Windows plugins don't load). CAW scans:
- `~/.vst3`
- `/usr/lib/vst3`
- `/usr/local/lib/vst3`

To test, install `sudo apt install lsp-plugins-vst3`.

### 2.5 Running in VirtualBox

- **No window or an OpenGL error:** shut the VM down, then **Settings → Display → Enable 3D Acceleration**. Or start CAW with `LIBGL_ALWAYS_SOFTWARE=1 ./build/caw`.
- **No sound:** **Settings → Audio → Enable Audio Output**, then choose the device in CAW.

---

## 3. macOS

### 3.1 Install the tools (once)

```bash
xcode-select --install          # Apple's compiler
# Homebrew: see https://brew.sh
brew install cmake ninja git
```

### 3.2 Build CAW

```bash
cd caw
chmod +x build.sh
./build.sh nostart                       # this Mac's processor
CAW_UNIVERSAL=1 ./build.sh nostart       # Apple Silicon + Intel in one app
```

- Result: **`build/caw.app`**. The script signs it locally, so Apple Silicon Macs run it.
- **First start:** right-click `caw.app` → **Open** → **Open**. macOS blocks apps not signed by a registered Apple developer.
- If VST blocks the build, try `./build.sh nostart -DCAW_VST3=OFF` first.

### 3.3 Engines

The same scripts as Linux. They use Homebrew instead of apt:

```bash
cd tools/unix
chmod +x *.sh ../supercollider/*.sh
./build-engines.sh
```

### 3.4 VST3 plugins

CAW scans these folders for **macOS** `.vst3` plugins:
- `~/Library/Audio/Plug-Ins/VST3`
- `/Library/Audio/Plug-Ins/VST3`

### 3.5 Permissions

**System Settings → Privacy & Security → Microphone:** allow CAW if you use audio input.

---

## 4. SuperCollider version

CAW's SuperCollider is the normal SuperCollider source plus two things in `tools/supercollider/`:
- **`SC_ExternalDriver.cpp`**: CAW's driver. CAW drives `scsynth` from its own audio callback instead of SuperCollider opening a sound card. It provides `caw_sc_world_new`, `caw_sc_world_process`, `caw_sc_world_process_ex`, `caw_sc_world_free`, `caw_sc_last_error` and `caw_sc_abi_version` (5).
- **`caw_sc.patch`**: switches it on with `-DAUDIOAPI=external`.

A normal SuperCollider install doesn't work with CAW.

**Use the same SuperCollider version on every system.** Find the one you use on Windows:

```powershell
cd $env:USERPROFILE\.caw_deps\supercollider\source
git describe --tags --always
```

Then build with that version:

```bash
SC_TAG=Version-3.13.0 ./build-supercollider.sh      # Linux / macOS
```
```bat
set SC_TAG=Version-3.13.0
tools\windows\BUILD-LIBSCSYNTH.bat
```

On Linux/macOS, if the patch doesn't fit the version exactly, the script makes the same edits line by line instead. After the "CAW changes" step it prints `[ok] CAW edits applied`. At the end, the **Check** step lists the six `caw_sc_*` functions, and all six must say `[ok]`.

Other versions (Linux / macOS):

```bash
CSOUND_TAG=7.0.0-beta.18 ./build-csound.sh
LIBPD_REF=master ./build-libpd.sh
```

---

## 5. Where things go

| What | Windows | Linux / macOS |
|---|---|---|
| downloaded sources | `%USERPROFILE%\.caw_deps\` | `~/.caw_deps/` |
| CAW | `build\Release\caw.exe` | `build/caw` · `build/caw.app` |
| Csound 7 | `%LOCALAPPDATA%\Programs\Csound7\` | `/usr/local/lib`, `/usr/local/bin` |
| libpd | `%USERPROFILE%\CAW-libpd\build-multi\` | `~/.caw_deps/libpd/` |
| SuperCollider | `%USERPROFILE%\.caw_deps\supercollider\runtime-x64\SuperCollider\` | `~/.caw_deps/supercollider/runtime-x64/SuperCollider/` |
| saved engine paths | `%USERPROFILE%\.caw_libpd_path` etc. | `~/.caw_libpd_path`, `~/.caw_sclang_path`, `~/.caw_scsynth_path` |
| extra VST3 folders | `~\.caw_vst3_dirs` | `~/.caw_vst3_dirs` |

Deleting `.caw_deps` makes the next build download everything again.

---

## When something fails

**Find the first error.** Later errors are usually caused by the first one.
- CAW: `grep -n -m 20 "error" caw_build.log` (Linux/macOS), or search `caw_build.log` for `error` (Windows).
- An engine script: the last 20–30 lines of the terminal.

| Message | Cause | Fix |
|---|---|---|
| `Csound API library not found` / `cannot compile abs or sqrt` | no Csound library found | build or install Csound 7, **restart CAW**, or pick `libcsound64.so` / `csound64.dll` in Settings → Programs |
| `This libpd was not built with MULTI=true` | an ordinary libpd | build it with the script (`make MULTI=true`) |
| `missing CAW's host-pump entry points` / `old CAW audio ABI` | a normal or old SuperCollider | build CAW's SuperCollider (section 4) |
| `Unrecognised audio API: external` | the CAW patch wasn't applied | use the current `build-libscsynth.sh`; it checks each edit |
| `CMakeCache.txt directory ... does not match the source` | the build folder came from another path | delete `build/`, or let `build.bat` reconfigure it |
| `apt update`: `no longer has a Release file` | an old Ubuntu version | see Ubuntu 24.10 in 2.1 |
| `LoadLibraryW failed ... module could not be found` (a plugin) | a DLL the plugin needs is missing | put the plugin's DLLs (for example Csound's) on PATH or beside it |
| no window on Linux in a VM | no OpenGL 3 | VirtualBox 3D acceleration, or `LIBGL_ALWAYS_SOFTWARE=1` |

**Current limits on Linux / macOS:**
- Plugin windows don't open (plugins still play and are controlled from CAW's panel).
- A crashing plugin crashes CAW (on Windows CAW catches it).
- There's no installer yet (AppImage / `.dmg` are planned).

When you report a problem, include the system, the step, and the log lines above.

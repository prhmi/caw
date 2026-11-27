# CAW engine builds (Linux / macOS)

CAW loads its sound engines at run time. These scripts build the versions CAW needs.

| Script | Builds | Result | Notes |
|---|---|---|---|
| `build-engines.sh` | all three | — | `./build-engines.sh csound pd` builds only some |
| `build-csound.sh` | **Csound 7** | `libcsound64.so/.dylib` in `/usr/local` | default tag `7.0.0-beta.18` |
| `build-libpd.sh` | **libpd**, `MULTI=true` | `~/.caw_deps/libpd/libpd.so/.dylib` | writes `~/.caw_libpd_path`, so CAW finds it |
| `build-supercollider.sh` | **CAW's SuperCollider** | `~/.caw_deps/supercollider/runtime-x64/SuperCollider/` | stock SuperCollider + CAW's host-pumped driver |

## Use

```bash
cd tools/unix
chmod +x *.sh ../supercollider/*.sh
./build-engines.sh
```

Then restart CAW. If an engine isn't found, pick its file in **Settings > Programs**.

Every script downloads its source into `~/.caw_deps`, installs the build packages it needs (apt on Ubuntu/Debian, Homebrew on macOS), builds, then checks the result.

## Versions

Set them before the command, for example:

```bash
CSOUND_TAG=7.0.0-beta.18 LIBPD_REF=master SC_TAG=Version-3.13.0 ./build-engines.sh
```

Use the **same SuperCollider version as your Windows build**. On Windows: `git describe --tags` inside `%USERPROFILE%\.caw_deps\supercollider\source`.

## What is special

- **libpd:** CAW runs one Pure Data instance per clip, so libpd must be built with `MULTI=true`. A normal libpd is refused.
- **SuperCollider:** CAW doesn't use a sound card for SuperCollider. It pumps `scsynth` from its own audio callback. `../supercollider/SC_ExternalDriver.cpp` adds that driver (`caw_sc_world_new`, `caw_sc_world_process`, `caw_sc_world_process_ex`, ABI 5). `../supercollider/caw_sc.patch` adds the build option for it (`-DAUDIOAPI=external`). A normal SuperCollider install doesn't have these functions.
- **Csound:** stock Csound 7, nothing changed.

## Licences

Csound: LGPL 2.1. libpd / Pure Data: BSD-style. SuperCollider: GPL 3, and `SC_ExternalDriver.cpp` is GPL 3 as well. These scripts only download and build the sources; they ship no binaries.

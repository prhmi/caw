# Linux package

| Step | Script | Result |
|---|---|---|
| 0 | `./build.sh` and `tools/unix/build-engines.sh` | `build/caw`, plus the engines in `~/.caw_deps` and `/usr/local` |
| 1 | `./1-prepare.sh` | `stage/CAW/`: caw, `Csound/`, `PureData/`, `SuperCollider/`, licences |
| 2a | `./2-build-appimage.sh` | `Output/CAW-<version>-x86_64.AppImage`: **one file, everything inside** |
| 2b | `./2-build-tarball.sh` | `Output/CAW-<version>-linux-x86_64.tar.gz`: a folder; the target needs the system packages |

```bash
cd installer/linux
chmod +x *.sh
./1-prepare.sh
./2-build-appimage.sh
```

`1-prepare.sh` stops with warnings if an engine is missing or `caw` is older than the source, the same as `1-PREPARE` on Windows.

**AppImage:** users download it, run `chmod +x CAW-*.AppImage`, and start it. CAW finds the bundled engines in `usr/lib/caw/` inside the image. `2-build-appimage.sh` uses **linuxdeploy**, which it downloads once. linuxdeploy also bundles the libraries the engines need (libsndfile, fftw, …).

**Build on the oldest distribution you want to support:** an AppImage runs on systems with the same or a newer glibc than the one it was built on.

Set the version with `CAW_VERSION=1.0 ./2-build-appimage.sh`.

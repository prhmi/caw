# macOS package

| Step | Script | Result |
|---|---|---|
| 0 | `./build.sh` and `tools/unix/build-engines.sh` | `build/caw.app`, plus the engines |
| 1 | `./1-prepare.sh` | `stage/CAW.app` with `Contents/Resources/{Csound,PureData,SuperCollider}` and the Homebrew libraries in `Contents/Frameworks` |
| 2 | `./2-build-dmg.sh` | `Output/CAW-<version>.dmg`: open it and drag CAW to Applications |

```bash
cd installer/macos
chmod +x *.sh
./1-prepare.sh
./2-build-dmg.sh
```

**What `1-prepare.sh` does:**
- copies the engines into the app;
- copies the Homebrew libraries the engines need (libsndfile, fftw, …) into `Contents/Frameworks`, and repoints the engines to them, so the app runs on a Mac **without Homebrew**;
- signs the app ad hoc.

**Gatekeeper:** with an ad‑hoc signature, users must right‑click **CAW** → **Open** the first time. To avoid that, you need an Apple Developer ID ($99 a year):

```bash
codesign --force --deep --options runtime -s "Developer ID Application: <you>" stage/CAW.app
./2-build-dmg.sh
xcrun notarytool submit Output/CAW-*.dmg --apple-id <id> --team-id <team> --password <app-password> --wait
xcrun stapler staple Output/CAW-*.dmg
```

**Intel + Apple Silicon:** build CAW with `CAW_UNIVERSAL=1 ./build.sh`. The engines also have to be built for both, or built on each kind of Mac.

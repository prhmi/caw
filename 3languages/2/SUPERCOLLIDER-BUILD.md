# Building SuperCollider for CAW (Windows x64)

Run `tools/BUILD-LIBSCSYNTH.bat` from the extracted project. It builds a pinned SuperCollider `develop` snapshot with MSVC, CMake, and vcpkg, with the IDE and Qt disabled. The build includes `libscsynth.dll`, `sclang.exe`, static SuperCollider plugins, and the supporting runtime files. It patches in CAW's host-pumped audio backend so the server renders blocks directly into CAW's PortAudio callback. This pinned development snapshot includes the upstream Windows DLL fix (commit `0c5a96d`), but it is not a numbered stable release.

## First run

You need Git for Windows, CMake, and Visual Studio/Build Tools with **Desktop development with C++**. The script bootstraps its own standalone vcpkg checkout if it is not already cached, then installs dynamic `fftw3` and `libsndfile` packages. Dynamic libsndfile avoids the unresolved LAME symbols caused by linking its static archive without its transitive MP3 libraries; the script copies the vcpkg runtime DLLs beside `sclang.exe`. Switching triplets means vcpkg may compile the dependencies once more, then reuse them on later runs. It deliberately ignores Visual Studio's injected `VCPKG_ROOT`, which points to a manifest-only copy. The first run downloads SuperCollider's source/submodules and required dependency sources/packages. It can take a while. If you already have a standalone vcpkg checkout, set `CAW_SCSYNTH_VCPKG_ROOT` to that folder before running the batch file.

Everything is cached outside the project in `%USERPROFILE%\.caw_deps\supercollider` (and `%USERPROFILE%\.caw_deps\vcpkg`). Run the same `.bat` again to reuse the source, dependencies, and incremental build. It does not `git pull` or re-clone on later runs. The installed runtime is placed in `%USERPROFILE%\.caw_deps\supercollider\runtime-x64`; CAW searches there for `sclang.exe` and `libscsynth.dll`, and the builder copies the library beside `sclang.exe`. If you installed them elsewhere, set both paths under **Settings > Programs**. The builder also applies an idempotent compatibility fix for SuperCollider's `install(CODE)` block, which passes unsupported `COMMENT`/`VERBATIM` arguments to CMake. The `Readline not found, building without readline support` message is expected: the CAW headless runtime disables the optional interactive Readline support.

The source is pinned to commit `6df38879ddf08a21cb183e67dcbf800f9d5489cf` and the script does not pull updates. To intentionally change versions, edit `SC_COMMIT` in the batch file and remove only the cached `source` and `build-x64` folders. The vcpkg cache can remain.

## Run SC clips in CAW

1. Build the runtime with the batch file above, then rebuild CAW.
2. Enable **SuperCollider** in the language picker, or in Settings > Languages.
3. Add a SuperCollider track, double-click the lane to add a clip, and leave the starter code or replace it.
4. Press **Run SuperCollider**, wait for the ready message, then press **Play**. When Csound or PD is enabled, the combined **Run Csound + SC**, **Run Csound + PD + SC**, or **Run PD + SC** button starts both engines on CAW's shared output.

CAW launches `sclang.exe` as a language client and loads `libscsynth.dll` in-process using the host-pumped audio backend. The SC server generates audio in real time inside CAW's existing PortAudio callback, so it shares the soundcard stream, master gain, and monitor with Csound and Pure Data. SC lane meters are isolated by track for the common `play(target: ~cawGroup)` and `Out.ar(0, ...)` patterns, which CAW routes to each track's private output bus. Code that sends directly to a custom bus is outside that meter routing. In clip code, target the clip group with `~cawGroup` so CAW can stop that clip cleanly, for example:

```supercollider
{ SinOsc.ar(440, 0, 0.1) ! 2 }.play(target: ~cawGroup);
```

The clip group is freed when its duration ends. Enable MIDI on an SC clip and open its MIDI editor to trigger the clip code per note. The code receives `~cawP4` (pitch), `~cawP5` (velocity), and `~cawNoteDuration` (seconds); an example is `{ SinOsc.ar(~cawP4.midicps) * (~cawP5 / 127) * 0.1 ! 2 }.play(target: ~cawGroup);`. SC automation lanes and offline audio export are not wired to arbitrary SynthDef controls yet. Review the SuperCollider license and third-party notices before redistributing its runtime files.

## Sources

- [Pinned SuperCollider source snapshot](https://github.com/supercollider/supercollider/commit/6df38879ddf08a21cb183e67dcbf800f9d5489cf)
- [Upstream `libscsynth` Windows build fix](https://github.com/supercollider/supercollider/pull/7847)
- [Official Windows build instructions](https://github.com/supercollider/supercollider/blob/develop/README_WINDOWS.md)

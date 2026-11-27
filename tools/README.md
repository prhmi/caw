# CAW tools

```
tools/
├── supercollider/   CAW's SuperCollider driver + patch (shared by all systems)
├── unix/            Linux / macOS: build-csound.sh, build-libpd.sh, build-supercollider.sh, build-engines.sh (all three)
└── windows/         Windows: BUILD-CSOUND.bat, BUILD-LIBPD.bat, BUILD-LIBSCSYNTH.bat
```

Each engine is a separate library with its own script. See **HOW to Build detailed.md** for every step.

The VST3 SDK and the plugin editor windows aren't here. CAW's own CMake build downloads and compiles them on every system.

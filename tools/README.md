# CAW tools

```
tools/
├── supercollider/   CAW's SuperCollider driver + patch (shared by all systems)
├── unix/            Linux / macOS: build-engines.sh (Csound 7, libpd, SuperCollider)
└── windows/         Windows: BUILD-LIBSCSYNTH.bat, BUILD-LIBPD.bat
```

The VST3 SDK isn't here. CAW's own CMake build downloads and compiles it on every system.

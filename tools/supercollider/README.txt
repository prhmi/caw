CAW SuperCollider runtime for Linux / macOS
===========================================
Files:
  build-libscsynth.sh     the build script
  SC_ExternalDriver.cpp   CAW's host-pumped driver (caw_sc_world_new / _process / _process_ex)
  caw_sc.patch            the 3 CMake / header edits that switch the driver on

Run (Linux or macOS):
  chmod +x build-libscsynth.sh
  ./build-libscsynth.sh

The SuperCollider version must match your Windows build. Find it on Windows:
  cd %USERPROFILE%\.caw_deps\supercollider\source
  git describe --tags --always
then for example:
  SC_TAG=Version-3.13.0 ./build-libscsynth.sh

Result: ~/.caw_deps/supercollider/runtime-x64/SuperCollider/
  libscsynth.so (or .dylib), sclang, scsynth, plugins/, SCClassLibrary/
CAW looks there by itself. The "Check" step lists the 6 caw_sc_* functions - all must say [ok].

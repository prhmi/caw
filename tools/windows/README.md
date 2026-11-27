# CAW engines on Windows

| Script | Builds | Result | Needs |
|---|---|---|---|
| `BUILD-LIBSCSYNTH.bat` | CAW's SuperCollider (`libscsynth.dll` with the external driver) | `%USERPROFILE%\.caw_deps\supercollider\runtime-x64\SuperCollider\` | Visual Studio 2022, Git, CMake, vcpkg with `libsndfile` and `fftw3` (`VCPKG_ROOT`) |
| `BUILD-CSOUND.bat` | Csound 7 (`csound64.dll`, `csound.exe`, opcode plugins) | `%LOCALAPPDATA%\\Programs\\Csound7\\` (its `bin` folder is added to your user PATH) | Visual Studio 2022, Git, CMake (vcpkg is fetched automatically) |
| `BUILD-LIBPD.bat` | `libpd.dll` with `MULTI=true` | `%USERPROFILE%\CAW-libpd\build-multi\` | MSYS2 in `C:\msys64` |

**Csound:** install **Csound 7** from the official release (github.com/csound/csound/releases). CAW finds it in `Program Files\Csound*`, and `installer\1-PREPARE.ps1` copies it into the setup.

**VST3:** nothing to do. CMake downloads and builds the Steinberg VST3 SDK inside `build.bat`.

**Version:** use the same SuperCollider tag on every system: `set SC_TAG=Version-3.13.0` before `BUILD-LIBSCSYNTH.bat`.

CAW FRIEND TEST BUILD

1. Extract the whole folder.
2. Run caw.exe.
3. If Windows says a runtime DLL is missing, install the Microsoft Visual C++ 2015-2022 Redistributable (x64).
4. In Settings > Programs, set SuperCollider language to:
   SuperCollider\sclang.exe
5. Set SuperCollider synthesis library to:
   SuperCollider\libscsynth.dll
6. Enable SuperCollider and press Run SuperCollider.

This package includes the SuperCollider runtime. Install Csound separately
only if you want to test Csound. If a PureData folder is included, select
PureData\libpd.dll under Settings > Programs to test PD.

Send the project file and any external samples or PD patches separately if
they are not embedded in the .caw project.

For redistribution beyond a private test, check the included third-party
licenses and provide the corresponding source for the GPL-licensed runtime.

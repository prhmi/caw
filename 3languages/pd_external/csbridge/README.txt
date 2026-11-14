csbridge~  (Pd external, hosts Csound, audio via chnget/chnset)

[csbridge~ file.csd nIn nOut]
  Pd signal inlet k   -> Csound  aIn chnget "in<k+1>"
  Csound chnset aSig, "out<k+1>"  -> Pd signal outlet k
Messages: chn <name> <value> (control channel), event i 1 0 2 440, read file.csd, reset.
CSD needs the same sr as Pd. Use "-n" (added for you).

Build (Windows): install Pd + Csound 6 (with SDK/include+lib), then
  cmake -S . -B build -DPD_INCLUDE=... -DPD_LIB=... -DCSOUND_INCLUDE=... -DCSOUND_LIB=...
  cmake --build build --config Release
Copy csbridge~.dll next to your patch (or into Pd's extra folder), put the Csound bin folder in PATH.
Open csbridge-help.pd.

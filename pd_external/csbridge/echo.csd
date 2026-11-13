<CsoundSynthesizer>
<CsOptions>
-n
</CsOptions>
<CsInstruments>
sr = 44100
ksmps = 64
nchnls = 2
0dbfs = 1
instr 1                       ; audio from Pd (in1) -> lowpass -> back to Pd (out1, out2)
  aIn  chnget "in1"
  kCut chnget "cutoff"
  aOut butterlp aIn, limit(kCut, 50, 15000)
  chnset aOut, "out1"
  chnset aOut, "out2"
endin
</CsInstruments>
<CsScore>
i1 0 86400
</CsScore>
</CsoundSynthesizer>

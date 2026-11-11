<CsoundSynthesizer>
<CsOptions>
-odac -+rtaudio=portaudio -b1024 -B4096 -d
</CsOptions>
<CsInstruments>
sr = 44100
ksmps = 32
nchnls = 2
0dbfs = 1
A4 = 440

gaCawT_Lead1_1 init 0
gaCawT_Lead1_2 init 0
gaCawT_Lead2_1 init 0
gaCawT_Lead2_2 init 0
gaCawT_Lead3_1 init 0
gaCawT_Lead3_2 init 0
gaCawT_Bass1_1 init 0
gaCawT_Bass1_2 init 0
gaCawT_Bass2_1 init 0
gaCawT_Bass2_2 init 0
gaCawT_Pad1_1 init 0
gaCawT_Pad1_2 init 0
gaCawT_Unison1_1 init 0
gaCawT_Unison1_2 init 0
gaCawT_Pd1_1 init 0
gaCawT_Pd1_2 init 0
gaCawT_Voice1_1 init 0
gaCawT_Voice1_2 init 0
gaCawT_PdSeq_1 init 0
gaCawT_PdSeq_2 init 0
gaCawT_ScPad_1 init 0
gaCawT_ScPad_2 init 0
chn_k "Lead1_input1", 3
chnset 60, "Lead1_input1"

instr Lead1
; Lead: plain sine, MIDI note or input1 pitch




kMidi init p4
iVel = p5
if (iVel <= 0) then
  iVel = 127
endif
kNote chnget "Lead1_input1"
if (kMidi > 0) then
  kNote = kMidi
endif
aEnv  linen 1, 0.02, p3, 0.1
aOut  oscili 0.2 * (iVel / 127) * aEnv, cpsmidinn(kNote)
outs aOut, aOut
gaCawT_Lead1_1 = gaCawT_Lead1_1 + (aOut)
gaCawT_Lead1_2 = gaCawT_Lead1_2 + (aOut)

endin

chn_k "Lead2_input1", 3
chnset 67, "Lead2_input1"
chn_k "Lead2_detune", 3
chnset 0.035, "Lead2_detune"

instr Lead2
; Wide lead: two detuned sines, stereo





kMidi init p4
iVel = p5
if (iVel <= 0) then
  iVel = 127
endif
kNote chnget "Lead2_input1"
kDetune chnget "Lead2_detune"
if (kMidi > 0) then
  kNote = kMidi
endif
aEnv linen 1, 0.04, p3, 0.16
aLeft oscili 0.12 * (iVel / 127) * aEnv, cpsmidinn(kNote - kDetune)
aRight oscili 0.12 * (iVel / 127) * aEnv, cpsmidinn(kNote + kDetune)
outs aLeft, aRight
gaCawT_Lead2_1 = gaCawT_Lead2_1 + (aLeft)
gaCawT_Lead2_2 = gaCawT_Lead2_2 + (aRight)

endin

chn_k "Lead3_input1", 3
chnset 72, "Lead3_input1"
chn_k "Lead3_bright", 3
chnset 3800, "Lead3_bright"

instr Lead3
; Pluck lead: saw with decay and low-pass





kMidi init p4
iVel = p5
if (iVel <= 0) then
  iVel = 127
endif
kNote chnget "Lead3_input1"
kBright chnget "Lead3_bright"
if (kMidi > 0) then
  kNote = kMidi
endif
aEnv expseg 1, p3, 0.001
aRaw vco2 0.18 * (iVel / 127), cpsmidinn(kNote), 2, 0.3
aOut tone aRaw * aEnv, kBright
outs aOut, aOut
gaCawT_Lead3_1 = gaCawT_Lead3_1 + (aOut)
gaCawT_Lead3_2 = gaCawT_Lead3_2 + (aOut)

endin

chn_k "Bass1_volume", 3
chnset 0.55, "Bass1_volume"
chn_k "Bass1_cutoff", 3
chnset 520, "Bass1_cutoff"

instr Bass1
; Bass: saw + sub through a low-pass





kVol chnget "Bass1_volume"
kCut chnget "Bass1_cutoff"
iMidi = p4
if (iMidi <= 0) then
  iMidi = 36
endif
iVel = p5
if (iVel <= 0) then
  iVel = 127
endif
aEnv linen 1, 0.005, p3, 0.08
aSaw vco2 kVol * 0.25 * (iVel / 127), cpsmidinn(iMidi)
aSub oscili kVol * 0.12 * (iVel / 127), cpsmidinn(iMidi - 12)
aRaw = (aSaw + aSub) * aEnv
aOut tone aRaw, kCut
outs aOut, aOut
gaCawT_Bass1_1 = gaCawT_Bass1_1 + (aOut)
gaCawT_Bass1_2 = gaCawT_Bass1_2 + (aOut)

endin

chn_k "Bass2_volume", 3
chnset 0.6, "Bass2_volume"
chn_k "Bass2_drive", 3
chnset 2.2, "Bass2_drive"

instr Bass2
; Deep bass: driven sine





kVol chnget "Bass2_volume"
kDrive chnget "Bass2_drive"
iMidi = p4
if (iMidi <= 0) then
  iMidi = 31
endif
iVel = p5
if (iVel <= 0) then
  iVel = 127
endif
aEnv linen 1, 0.01, p3, 0.12
aSin oscili 1, cpsmidinn(iMidi)
aOut = tanh(aSin * kDrive) * kVol * 0.3 * (iVel / 127) * aEnv
outs aOut, aOut
gaCawT_Bass2_1 = gaCawT_Bass2_1 + (aOut)
gaCawT_Bass2_2 = gaCawT_Bass2_2 + (aOut)

endin

chn_k "Pad1_volume", 3
chnset 0.42, "Pad1_volume"
chn_k "Pad1_detune", 3
chnset 0.025, "Pad1_detune"

instr Pad1
; Pad: slow detuned stereo sines





kVol chnget "Pad1_volume"
kDetune chnget "Pad1_detune"
iMidi = p4
if (iMidi <= 0) then
  iMidi = 60
endif
iVel = p5
if (iVel <= 0) then
  iVel = 127
endif
aEnv linen 1, 0.28, p3, 0.7
aLeft oscili kVol * 0.22 * (iVel / 127) * aEnv, cpsmidinn(iMidi - kDetune)
aRight oscili kVol * 0.22 * (iVel / 127) * aEnv, cpsmidinn(iMidi + kDetune)
outs aLeft, aRight
gaCawT_Pad1_1 = gaCawT_Pad1_1 + (aLeft)
gaCawT_Pad1_2 = gaCawT_Pad1_2 + (aRight)

endin


instr Unison1
iRun = 3
indx = 0
if p1 == int(p1) then
while indx < 3 do
  schedule nstrnum("Unison1") + 0.001, 0, p3, p4, (0) + ((1) - (0)) * indx / max(iRun - 1, 1), (400) + indx * (100)
  indx += 1
od
turnoff
else
; Unison demo: the clip's x box = number of copies, all start together.
; The 




iAmp = 0.1
aEnv linen iAmp, 0.05, p3, 0.3
aOut oscili aEnv, p6
outs aOut * (1 - p5), aOut * p5
gaCawT_Unison1_1 = gaCawT_Unison1_1 + (aOut * (1 - p5))
gaCawT_Unison1_2 = gaCawT_Unison1_2 + (aOut * p5)
endif

endin

chn_k "Pd1_freq", 3
chnset 440, "Pd1_freq"

instr Pd1
aPdL, aPdR diskin2 "D:/myWork/codes/cpp/caw/17_score_4/build/Release/caw_pd/Pd1.wav", 1, p4
outs aPdL, aPdR
gaCawT_Pd1_1 = gaCawT_Pd1_1 + (aPdL)
gaCawT_Pd1_2 = gaCawT_Pd1_2 + (aPdR)

endin

chn_k "Voice1_input1", 3
chnset 60, "Voice1_input1"
chn_k "Voice1_input2", 3
chnset 1200, "Voice1_input2"

instr Voice1
; Voice1 receives DATA from Pure Data (pitch, input1) and SuperCollider (filter cutoff, input2)




kNote chnget "Voice1_input1"
kCut chnget "Voice1_input2"
aEnv  linen 1, 0.05, p3, 0.2
aSaw  vco2 0.25, cpsmidinn(kNote)     ; vco2 takes a k-rate / i-rate amplitude, so the envelope is applied after it
aSaw  = aSaw * aEnv
aOut  moogladder aSaw, limit(kCut, 100, 8000), 0.3
outs aOut, aOut
gaCawT_Voice1_1 = gaCawT_Voice1_1 + (aOut)
gaCawT_Voice1_2 = gaCawT_Voice1_2 + (aOut)

endin

chn_k "PdSeq_tempo", 3
chnset 250, "PdSeq_tempo"

instr PdSeq
aPdL, aPdR, aPdS1 diskin2 "D:/myWork/codes/cpp/caw/17_score_4/build/Release/caw_pd/PdSeq.wav", 1, p4
outs aPdL, aPdR
gaCawT_PdSeq_1 = gaCawT_PdSeq_1 + (aPdL)
gaCawT_PdSeq_2 = gaCawT_PdSeq_2 + (aPdR)
iPdScale = ampdbfs(0)
kPdS1 downsamp aPdS1
kPdS1 = kPdS1 / iPdScale
chnset kPdS1, "Voice1_input1"

endin

chn_k "ScPad_depth", 3
chnset 0.5, "ScPad_depth"

instr ScPad
aPdL, aPdR, aPdS1 diskin2 "D:/myWork/codes/cpp/caw/17_score_4/build/Release/caw_pd/ScPad.wav", 1, p4
outs aPdL, aPdR
gaCawT_ScPad_1 = gaCawT_ScPad_1 + (aPdL)
gaCawT_ScPad_2 = gaCawT_ScPad_2 + (aPdR)
iPdScale = ampdbfs(0)
kPdS1 downsamp aPdS1
kPdS1 = kPdS1 / iPdScale
chnset kPdS1, "Voice1_input2"

endin

instr CawAuto
Sch strcpy p6
kA line p4, p3, p5
chnset kA, Sch
endin

giCawOsc OSCinit 7770
instr CawOsc
Sname init ""
Sline init ""
kDel init 0
kDur init 0
kP4 init 0
kP5 init 0
kP6 init 0
nxtI:
kGotI OSClisten giCawOsc, "/caw/i", "sfffff", Sname, kDel, kDur, kP4, kP5, kP6
if kGotI == 0 kgoto nxtA
Sline sprintfk "i \"%s\" %f %f %f %f", Sname, kDel, kDur, kP4, kP5
scoreline Sline, 1
kgoto nxtI
nxtA:
kGotA OSClisten giCawOsc, "/caw/a", "sffff", Sname, kDel, kDur, kP4, kP5
if kGotA == 0 kgoto nxtO
Sline sprintfk "i \"CawAuto\" %f %f %f %f \"%s\"", kDel, kDur, kP4, kP5, Sname
scoreline Sline, 1
kgoto nxtA
nxtO:
kGotO OSClisten giCawOsc, "/caw/off", "s", Sname
if kGotO == 0 kgoto done
Sline sprintfk "i \"-%s\" 0 0", Sname
scoreline Sline, 1
turnoff2 Sname, 0, 0
kgoto nxtO
done:
kEnd = 0
endin

instr CawMon
aM1, aM2 monitor
aL = aM1
aR = aM2
kTrig metro 20
aAbsM1 = abs(aM1)
kPkM1 max_k aAbsM1, kTrig, 1
aAbsM2 = abs(aM2)
kPkM2 max_k aAbsM2, kTrig, 1
aLn2 = abs(gaCawT_Lead1_1 + gaCawT_Lead2_1 + gaCawT_Lead3_1)
kLn2 max_k aLn2, kTrig, 1
aLn3 = abs(gaCawT_Lead1_2 + gaCawT_Lead2_2 + gaCawT_Lead3_2)
kLn3 max_k aLn3, kTrig, 1
aLn4 = abs(gaCawT_Bass1_1 + gaCawT_Bass2_1)
kLn4 max_k aLn4, kTrig, 1
aLn5 = abs(gaCawT_Bass1_2 + gaCawT_Bass2_2)
kLn5 max_k aLn5, kTrig, 1
aLn6 = abs(gaCawT_Pad1_1)
kLn6 max_k aLn6, kTrig, 1
aLn7 = abs(gaCawT_Pad1_2)
kLn7 max_k aLn7, kTrig, 1
aLn8 = abs(gaCawT_Unison1_1)
kLn8 max_k aLn8, kTrig, 1
aLn9 = abs(gaCawT_Unison1_2)
kLn9 max_k aLn9, kTrig, 1
aLn10 = abs(gaCawT_Pd1_1)
kLn10 max_k aLn10, kTrig, 1
aLn11 = abs(gaCawT_Pd1_2)
kLn11 max_k aLn11, kTrig, 1
aLn12 = abs(gaCawT_Voice1_1)
kLn12 max_k aLn12, kTrig, 1
aLn13 = abs(gaCawT_Voice1_2)
kLn13 max_k aLn13, kTrig, 1
aLn14 = abs(gaCawT_PdSeq_1)
kLn14 max_k aLn14, kTrig, 1
aLn15 = abs(gaCawT_PdSeq_2)
kLn15 max_k aLn15, kTrig, 1
aLn16 = abs(gaCawT_ScPad_1)
kLn16 max_k aLn16, kTrig, 1
aLn17 = abs(gaCawT_ScPad_2)
kLn17 max_k aLn17, kTrig, 1
gaCawT_Bass1_1 = 0
gaCawT_Bass1_2 = 0
gaCawT_Bass2_1 = 0
gaCawT_Bass2_2 = 0
gaCawT_Lead1_1 = 0
gaCawT_Lead1_2 = 0
gaCawT_Lead2_1 = 0
gaCawT_Lead2_2 = 0
gaCawT_Lead3_1 = 0
gaCawT_Lead3_2 = 0
gaCawT_Pad1_1 = 0
gaCawT_Pad1_2 = 0
gaCawT_Pd1_1 = 0
gaCawT_Pd1_2 = 0
gaCawT_PdSeq_1 = 0
gaCawT_PdSeq_2 = 0
gaCawT_ScPad_1 = 0
gaCawT_ScPad_2 = 0
gaCawT_Unison1_1 = 0
gaCawT_Unison1_2 = 0
gaCawT_Voice1_1 = 0
gaCawT_Voice1_2 = 0
kScL[] init 64
kScR[] init 64
kW init 0
kSmpL downsamp aL
kSmpR downsamp aR
kScL[kW] = kSmpL
kScR[kW] = kSmpR
kW = kW + 1
if kW >= 64 then
kW = 0
endif
aMono = (aL + aR) * 0.5
aBd0 butterbp aMono, 25, 7.5
kBd0 rms aBd0
aBd1 butterbp aMono, 30.9109, 9.27328
kBd1 rms aBd1
aBd2 butterbp aMono, 38.2194, 11.4658
kBd2 rms aBd2
aBd3 butterbp aMono, 47.2559, 14.1768
kBd3 rms aBd3
aBd4 butterbp aMono, 58.4289, 17.5287
kBd4 rms aBd4
aBd5 butterbp aMono, 72.2437, 21.6731
kBd5 rms aBd5
aBd6 butterbp aMono, 89.3248, 26.7974
kBd6 rms aBd6
aBd7 butterbp aMono, 110.445, 33.1334
kBd7 rms aBd7
aBd8 butterbp aMono, 136.558, 40.9673
kBd8 rms aBd8
aBd9 butterbp aMono, 168.845, 50.6535
kBd9 rms aBd9
aBd10 butterbp aMono, 208.766, 62.6299
kBd10 rms aBd10
aBd11 butterbp aMono, 258.126, 77.4379
kBd11 rms aBd11
aBd12 butterbp aMono, 319.157, 95.7471
kBd12 rms aBd12
aBd13 butterbp aMono, 394.617, 118.385
kBd13 rms aBd13
aBd14 butterbp aMono, 487.92, 146.376
kBd14 rms aBd14
aBd15 butterbp aMono, 603.282, 180.985
kBd15 rms aBd15
aBd16 butterbp aMono, 745.92, 223.776
kBd16 rms aBd16
aBd17 butterbp aMono, 922.283, 276.685
kBd17 rms aBd17
aBd18 butterbp aMono, 1140.35, 342.104
kBd18 rms aBd18
aBd19 butterbp aMono, 1409.96, 422.989
kBd19 rms aBd19
aBd20 butterbp aMono, 1743.33, 523
kBd20 rms aBd20
aBd21 butterbp aMono, 2155.52, 646.656
kBd21 rms aBd21
aBd22 butterbp aMono, 2665.17, 799.55
kBd22 rms aBd22
aBd23 butterbp aMono, 3295.31, 988.593
kBd23 rms aBd23
aBd24 butterbp aMono, 4074.44, 1222.33
kBd24 rms aBd24
aBd25 butterbp aMono, 5037.79, 1511.34
kBd25 rms aBd25
aBd26 butterbp aMono, 6228.92, 1868.67
kBd26 rms aBd26
aBd27 butterbp aMono, 7701.66, 2310.5
kBd27 rms aBd27
aBd28 butterbp aMono, 9522.62, 2856.79
kBd28 rms aBd28
aBd29 butterbp aMono, 11774.1, 3532.24
kBd29 rms aBd29
aBd30 butterbp aMono, 14558, 4367.39
kBd30 rms aBd30
kBd31 = 0
kCnt init 0
if kTrig == 1 then
kCnt = kCnt + 1
endif
OSCsend kCnt, "127.0.0.1", 7771, "/caw/m", "iffffffffffffffff", 0, kPkM1, kPkM2, kLn2, kLn3, kLn4, kLn5, kLn6, kLn7, kLn8, kLn9, kLn10, kLn11, kLn12, kLn13, kLn14, kLn15
OSCsend kCnt, "127.0.0.1", 7771, "/caw/m", "iff", 16, kLn16, kLn17
OSCsend kCnt, "127.0.0.1", 7771, "/caw/s", "iffffffffffffffff", 0, kBd0, kBd1, kBd2, kBd3, kBd4, kBd5, kBd6, kBd7, kBd8, kBd9, kBd10, kBd11, kBd12, kBd13, kBd14, kBd15
OSCsend kCnt, "127.0.0.1", 7771, "/caw/s", "iffffffffffffffff", 16, kBd16, kBd17, kBd18, kBd19, kBd20, kBd21, kBd22, kBd23, kBd24, kBd25, kBd26, kBd27, kBd28, kBd29, kBd30, kBd31
kQ0 = kW + 0
if kQ0 >= 64 then
kQ0 = kQ0 - 64
endif
kPL0 = kScL[kQ0]
kPR0 = kScR[kQ0]
kQ1 = kW + 1
if kQ1 >= 64 then
kQ1 = kQ1 - 64
endif
kPL1 = kScL[kQ1]
kPR1 = kScR[kQ1]
kQ2 = kW + 2
if kQ2 >= 64 then
kQ2 = kQ2 - 64
endif
kPL2 = kScL[kQ2]
kPR2 = kScR[kQ2]
kQ3 = kW + 3
if kQ3 >= 64 then
kQ3 = kQ3 - 64
endif
kPL3 = kScL[kQ3]
kPR3 = kScR[kQ3]
kQ4 = kW + 4
if kQ4 >= 64 then
kQ4 = kQ4 - 64
endif
kPL4 = kScL[kQ4]
kPR4 = kScR[kQ4]
kQ5 = kW + 5
if kQ5 >= 64 then
kQ5 = kQ5 - 64
endif
kPL5 = kScL[kQ5]
kPR5 = kScR[kQ5]
kQ6 = kW + 6
if kQ6 >= 64 then
kQ6 = kQ6 - 64
endif
kPL6 = kScL[kQ6]
kPR6 = kScR[kQ6]
kQ7 = kW + 7
if kQ7 >= 64 then
kQ7 = kQ7 - 64
endif
kPL7 = kScL[kQ7]
kPR7 = kScR[kQ7]
kQ8 = kW + 8
if kQ8 >= 64 then
kQ8 = kQ8 - 64
endif
kPL8 = kScL[kQ8]
kPR8 = kScR[kQ8]
kQ9 = kW + 9
if kQ9 >= 64 then
kQ9 = kQ9 - 64
endif
kPL9 = kScL[kQ9]
kPR9 = kScR[kQ9]
kQ10 = kW + 10
if kQ10 >= 64 then
kQ10 = kQ10 - 64
endif
kPL10 = kScL[kQ10]
kPR10 = kScR[kQ10]
kQ11 = kW + 11
if kQ11 >= 64 then
kQ11 = kQ11 - 64
endif
kPL11 = kScL[kQ11]
kPR11 = kScR[kQ11]
kQ12 = kW + 12
if kQ12 >= 64 then
kQ12 = kQ12 - 64
endif
kPL12 = kScL[kQ12]
kPR12 = kScR[kQ12]
kQ13 = kW + 13
if kQ13 >= 64 then
kQ13 = kQ13 - 64
endif
kPL13 = kScL[kQ13]
kPR13 = kScR[kQ13]
kQ14 = kW + 14
if kQ14 >= 64 then
kQ14 = kQ14 - 64
endif
kPL14 = kScL[kQ14]
kPR14 = kScR[kQ14]
kQ15 = kW + 15
if kQ15 >= 64 then
kQ15 = kQ15 - 64
endif
kPL15 = kScL[kQ15]
kPR15 = kScR[kQ15]
kQ16 = kW + 16
if kQ16 >= 64 then
kQ16 = kQ16 - 64
endif
kPL16 = kScL[kQ16]
kPR16 = kScR[kQ16]
kQ17 = kW + 17
if kQ17 >= 64 then
kQ17 = kQ17 - 64
endif
kPL17 = kScL[kQ17]
kPR17 = kScR[kQ17]
kQ18 = kW + 18
if kQ18 >= 64 then
kQ18 = kQ18 - 64
endif
kPL18 = kScL[kQ18]
kPR18 = kScR[kQ18]
kQ19 = kW + 19
if kQ19 >= 64 then
kQ19 = kQ19 - 64
endif
kPL19 = kScL[kQ19]
kPR19 = kScR[kQ19]
kQ20 = kW + 20
if kQ20 >= 64 then
kQ20 = kQ20 - 64
endif
kPL20 = kScL[kQ20]
kPR20 = kScR[kQ20]
kQ21 = kW + 21
if kQ21 >= 64 then
kQ21 = kQ21 - 64
endif
kPL21 = kScL[kQ21]
kPR21 = kScR[kQ21]
kQ22 = kW + 22
if kQ22 >= 64 then
kQ22 = kQ22 - 64
endif
kPL22 = kScL[kQ22]
kPR22 = kScR[kQ22]
kQ23 = kW + 23
if kQ23 >= 64 then
kQ23 = kQ23 - 64
endif
kPL23 = kScL[kQ23]
kPR23 = kScR[kQ23]
kQ24 = kW + 24
if kQ24 >= 64 then
kQ24 = kQ24 - 64
endif
kPL24 = kScL[kQ24]
kPR24 = kScR[kQ24]
kQ25 = kW + 25
if kQ25 >= 64 then
kQ25 = kQ25 - 64
endif
kPL25 = kScL[kQ25]
kPR25 = kScR[kQ25]
kQ26 = kW + 26
if kQ26 >= 64 then
kQ26 = kQ26 - 64
endif
kPL26 = kScL[kQ26]
kPR26 = kScR[kQ26]
kQ27 = kW + 27
if kQ27 >= 64 then
kQ27 = kQ27 - 64
endif
kPL27 = kScL[kQ27]
kPR27 = kScR[kQ27]
kQ28 = kW + 28
if kQ28 >= 64 then
kQ28 = kQ28 - 64
endif
kPL28 = kScL[kQ28]
kPR28 = kScR[kQ28]
kQ29 = kW + 29
if kQ29 >= 64 then
kQ29 = kQ29 - 64
endif
kPL29 = kScL[kQ29]
kPR29 = kScR[kQ29]
kQ30 = kW + 30
if kQ30 >= 64 then
kQ30 = kQ30 - 64
endif
kPL30 = kScL[kQ30]
kPR30 = kScR[kQ30]
kQ31 = kW + 31
if kQ31 >= 64 then
kQ31 = kQ31 - 64
endif
kPL31 = kScL[kQ31]
kPR31 = kScR[kQ31]
kQ32 = kW + 32
if kQ32 >= 64 then
kQ32 = kQ32 - 64
endif
kPL32 = kScL[kQ32]
kPR32 = kScR[kQ32]
kQ33 = kW + 33
if kQ33 >= 64 then
kQ33 = kQ33 - 64
endif
kPL33 = kScL[kQ33]
kPR33 = kScR[kQ33]
kQ34 = kW + 34
if kQ34 >= 64 then
kQ34 = kQ34 - 64
endif
kPL34 = kScL[kQ34]
kPR34 = kScR[kQ34]
kQ35 = kW + 35
if kQ35 >= 64 then
kQ35 = kQ35 - 64
endif
kPL35 = kScL[kQ35]
kPR35 = kScR[kQ35]
kQ36 = kW + 36
if kQ36 >= 64 then
kQ36 = kQ36 - 64
endif
kPL36 = kScL[kQ36]
kPR36 = kScR[kQ36]
kQ37 = kW + 37
if kQ37 >= 64 then
kQ37 = kQ37 - 64
endif
kPL37 = kScL[kQ37]
kPR37 = kScR[kQ37]
kQ38 = kW + 38
if kQ38 >= 64 then
kQ38 = kQ38 - 64
endif
kPL38 = kScL[kQ38]
kPR38 = kScR[kQ38]
kQ39 = kW + 39
if kQ39 >= 64 then
kQ39 = kQ39 - 64
endif
kPL39 = kScL[kQ39]
kPR39 = kScR[kQ39]
kQ40 = kW + 40
if kQ40 >= 64 then
kQ40 = kQ40 - 64
endif
kPL40 = kScL[kQ40]
kPR40 = kScR[kQ40]
kQ41 = kW + 41
if kQ41 >= 64 then
kQ41 = kQ41 - 64
endif
kPL41 = kScL[kQ41]
kPR41 = kScR[kQ41]
kQ42 = kW + 42
if kQ42 >= 64 then
kQ42 = kQ42 - 64
endif
kPL42 = kScL[kQ42]
kPR42 = kScR[kQ42]
kQ43 = kW + 43
if kQ43 >= 64 then
kQ43 = kQ43 - 64
endif
kPL43 = kScL[kQ43]
kPR43 = kScR[kQ43]
kQ44 = kW + 44
if kQ44 >= 64 then
kQ44 = kQ44 - 64
endif
kPL44 = kScL[kQ44]
kPR44 = kScR[kQ44]
kQ45 = kW + 45
if kQ45 >= 64 then
kQ45 = kQ45 - 64
endif
kPL45 = kScL[kQ45]
kPR45 = kScR[kQ45]
kQ46 = kW + 46
if kQ46 >= 64 then
kQ46 = kQ46 - 64
endif
kPL46 = kScL[kQ46]
kPR46 = kScR[kQ46]
kQ47 = kW + 47
if kQ47 >= 64 then
kQ47 = kQ47 - 64
endif
kPL47 = kScL[kQ47]
kPR47 = kScR[kQ47]
kQ48 = kW + 48
if kQ48 >= 64 then
kQ48 = kQ48 - 64
endif
kPL48 = kScL[kQ48]
kPR48 = kScR[kQ48]
kQ49 = kW + 49
if kQ49 >= 64 then
kQ49 = kQ49 - 64
endif
kPL49 = kScL[kQ49]
kPR49 = kScR[kQ49]
kQ50 = kW + 50
if kQ50 >= 64 then
kQ50 = kQ50 - 64
endif
kPL50 = kScL[kQ50]
kPR50 = kScR[kQ50]
kQ51 = kW + 51
if kQ51 >= 64 then
kQ51 = kQ51 - 64
endif
kPL51 = kScL[kQ51]
kPR51 = kScR[kQ51]
kQ52 = kW + 52
if kQ52 >= 64 then
kQ52 = kQ52 - 64
endif
kPL52 = kScL[kQ52]
kPR52 = kScR[kQ52]
kQ53 = kW + 53
if kQ53 >= 64 then
kQ53 = kQ53 - 64
endif
kPL53 = kScL[kQ53]
kPR53 = kScR[kQ53]
kQ54 = kW + 54
if kQ54 >= 64 then
kQ54 = kQ54 - 64
endif
kPL54 = kScL[kQ54]
kPR54 = kScR[kQ54]
kQ55 = kW + 55
if kQ55 >= 64 then
kQ55 = kQ55 - 64
endif
kPL55 = kScL[kQ55]
kPR55 = kScR[kQ55]
kQ56 = kW + 56
if kQ56 >= 64 then
kQ56 = kQ56 - 64
endif
kPL56 = kScL[kQ56]
kPR56 = kScR[kQ56]
kQ57 = kW + 57
if kQ57 >= 64 then
kQ57 = kQ57 - 64
endif
kPL57 = kScL[kQ57]
kPR57 = kScR[kQ57]
kQ58 = kW + 58
if kQ58 >= 64 then
kQ58 = kQ58 - 64
endif
kPL58 = kScL[kQ58]
kPR58 = kScR[kQ58]
kQ59 = kW + 59
if kQ59 >= 64 then
kQ59 = kQ59 - 64
endif
kPL59 = kScL[kQ59]
kPR59 = kScR[kQ59]
kQ60 = kW + 60
if kQ60 >= 64 then
kQ60 = kQ60 - 64
endif
kPL60 = kScL[kQ60]
kPR60 = kScR[kQ60]
kQ61 = kW + 61
if kQ61 >= 64 then
kQ61 = kQ61 - 64
endif
kPL61 = kScL[kQ61]
kPR61 = kScR[kQ61]
kQ62 = kW + 62
if kQ62 >= 64 then
kQ62 = kQ62 - 64
endif
kPL62 = kScL[kQ62]
kPR62 = kScR[kQ62]
kQ63 = kW + 63
if kQ63 >= 64 then
kQ63 = kQ63 - 64
endif
kPL63 = kScL[kQ63]
kPR63 = kScR[kQ63]
OSCsend kCnt, "127.0.0.1", 7771, "/caw/l", "iffffffffffffffff", 0, kPL0, kPL1, kPL2, kPL3, kPL4, kPL5, kPL6, kPL7, kPL8, kPL9, kPL10, kPL11, kPL12, kPL13, kPL14, kPL15
OSCsend kCnt, "127.0.0.1", 7771, "/caw/l", "iffffffffffffffff", 16, kPL16, kPL17, kPL18, kPL19, kPL20, kPL21, kPL22, kPL23, kPL24, kPL25, kPL26, kPL27, kPL28, kPL29, kPL30, kPL31
OSCsend kCnt, "127.0.0.1", 7771, "/caw/l", "iffffffffffffffff", 32, kPL32, kPL33, kPL34, kPL35, kPL36, kPL37, kPL38, kPL39, kPL40, kPL41, kPL42, kPL43, kPL44, kPL45, kPL46, kPL47
OSCsend kCnt, "127.0.0.1", 7771, "/caw/l", "iffffffffffffffff", 48, kPL48, kPL49, kPL50, kPL51, kPL52, kPL53, kPL54, kPL55, kPL56, kPL57, kPL58, kPL59, kPL60, kPL61, kPL62, kPL63
OSCsend kCnt, "127.0.0.1", 7771, "/caw/r", "iffffffffffffffff", 0, kPR0, kPR1, kPR2, kPR3, kPR4, kPR5, kPR6, kPR7, kPR8, kPR9, kPR10, kPR11, kPR12, kPR13, kPR14, kPR15
OSCsend kCnt, "127.0.0.1", 7771, "/caw/r", "iffffffffffffffff", 16, kPR16, kPR17, kPR18, kPR19, kPR20, kPR21, kPR22, kPR23, kPR24, kPR25, kPR26, kPR27, kPR28, kPR29, kPR30, kPR31
OSCsend kCnt, "127.0.0.1", 7771, "/caw/r", "iffffffffffffffff", 32, kPR32, kPR33, kPR34, kPR35, kPR36, kPR37, kPR38, kPR39, kPR40, kPR41, kPR42, kPR43, kPR44, kPR45, kPR46, kPR47
OSCsend kCnt, "127.0.0.1", 7771, "/caw/r", "iffffffffffffffff", 48, kPR48, kPR49, kPR50, kPR51, kPR52, kPR53, kPR54, kPR55, kPR56, kPR57, kPR58, kPR59, kPR60, kPR61, kPR62, kPR63
endin

</CsInstruments>
<CsScore>
i "CawOsc" 0 86400
i "CawMon" 0 86400
f 0 86400
</CsScore>
</CsoundSynthesizer>

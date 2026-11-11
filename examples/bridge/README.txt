Bridge demo (opens with the default project): data and sound between Csound, Pure Data and SuperCollider

Lanes: "Voice (Csound)" = Voice1,  "Sequencer (Pd)" = PdSeq,  "Pad (SuperCollider)" = ScPad.   Pick the language of a clip in its editor window (top left).

SOUND   every clip ends in the master mix (Pd: [dac~], SuperCollider: first two values [left, right], Csound: outs).
DATA    send:  line in the <caw> block   ->   channel of a Csound clip
          Pd  : [s pitch]                    -> { send: "pitch", to: "Voice1", chn: "input1" }   (note numbers)
          SC  : 3rd value of the last line   -> { send: "lfo",   to: "Voice1", chn: "input2" }   (filter cutoff)
          Csound Voice1 reads them with   kNote caw "input1"   and   kCut caw "input2".
AUTOMATION   chn: lines make automation lanes: Pd [r tempo], SuperCollider \depth.kr(0.5), Csound  caw "name".

Pd and SuperCollider clips are rendered offline to a wav when you press Play / Run clip (set pd.exe and sclang.exe in Settings),
so they SEND data to Csound but cannot read live data back from it.
Ctrl+Q quits. Space plays / stops the timeline.

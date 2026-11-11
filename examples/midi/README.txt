MIDI for Pure Data and SuperCollider clips

1. Make a clip, set its language to "Pure Data" or "SuperCollider".
2. Add   midi: "on"   inside the <caw> block. The lane now shows the MIDI piano roll, exactly like a Csound clip with midi: "on".
3. Draw MIDI regions and notes. Play / Run clip / Render play them.

Pure Data  (PdSynth.pd; the file name is the clip name when you use  load: <folder>)
   [r midinote]   list: pitch velocity   (velocity 0 = note off, like [notein])  -> [unpack f f]
   [r midipitch]  pitch of the last note on / off
   [r midivel]    velocity of the last note on, 0 on note off
   [r midigate]   1 on note on, 0 on note off
   Poly patches: route [r midinote] into your own voice allocator ([poly] works).

SuperCollider  (ScSynth.txt)
   One synth per note with controls  \note (0..127)  \vel (1..127)  \gate (1 held, 0 released).
   End the envelope with doneAction: 2. A synth that is still running is freed 2 s after its note ends.

How it works: the clip is rendered offline once with all its notes inside (timeline 0 s = wav 0 s), the same way as without MIDI.
The wav is as long as the last note + 2 s of release. Changing a note, or moving a MIDI region, renders the clip again on the next Play / Run.
Automation lanes (chn:) and sends (send:) keep working; in SuperCollider an automation value reaches every sounding note.
Notes are sample accurate in Pd (logical time) and in SuperCollider (NRT score).

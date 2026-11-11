Pure Data clip that loads a file by instrument name

1. Make a clip named Lead1, set its language to "Pure Data".
2. Replace the whole text with this single line (a folder path):

   load: C:/Users/you/caw/examples/pd

   The clip loads  <folder>/Lead1.pd  (the file name = the instrument name).
   Rename the clip to Pad2 and it loads Pad2.pd from the same folder.

   Or point to one file:   load: C:/Users/you/caw/examples/pd/Lead1.pd

3. Press Run clip or Play. Pd renders the patch, edit the .pd file and press again: the change is picked up.
   Abstractions next to the .pd file are found too.

Automation into Pd, and values from Pd into a Csound instrument
Clip named PdAuto, language Pure Data, text:

   load: C:/Users/you/caw/examples/pd
   <caw>
   { chn: "cutoff", range: (100,1000,1), value: 440 }
   { send: "pitch", to: "Lead1", chn: "input1" }
   </caw>

 - chn: "cutoff"  makes an automation lane for the clip. The lane is played into the patch: [r cutoff] receives it (10 ms steps).
 - send: "pitch"  everything the patch sends with [s pitch] arrives in channel input1 of the Csound clip Lead1
   (Lead1 reads it with  kP caw "input1"  and declares it with { chn: "input1", range: (...) } in its own <caw>).
 Moving automation points makes the next Run clip / Play render the clip again. Sends are recorded as extra wav channels, 10 ms is not exact: they are baked, not live.
 You can also write the patch itself under the <caw> block instead of the load: line.


Copies and inputs p4, p5 ... (x box)  - examples/pd/PdUnison.pd
Same idea as a Csound clip with <caw> {p: ...} lines. The clip's x box = number of copies, all play together from the clip start.
Put [inlet] objects in the patch. The RIGHT-most inlet is p4, the one to its left is p5, and so on (Pd sends right to left,
so p4 arrives first and the left-most inlet, the "hot" one, comes last - build your sound on the left inlet).
Clip text (x = 3):

   load: C:/Users/you/caw/examples/pd/PdUnison.pd
   <caw>
   {p: 4, from: 300, +100}     copy 1 = 300 Hz, copy 2 = 400 Hz, copy 3 = 500 Hz   -> right inlet
   {p: 5, from: 0.05}          every copy 0.05                                       -> left inlet
   </caw>

Lines: {p: 5, from: 0.05}  same value for all | {p: 5, from: 0, to: 1}  spread first..last | {p: 4, from: 100, +100} (also - * /)
A p-field without a line gets 0. Inlets with no p-line are not touched when no line exists at all.
In a MIDI clip (midi: "on") every note starts x copies: p4 = pitch, p5 = velocity unless a p-line sets them.

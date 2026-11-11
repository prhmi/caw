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

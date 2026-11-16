# PD live coding, x-fields, pen and MIDI toggle (this update)

Tested only with the mock libpd/PortAudio tests and a compile check of app.cpp
on Linux. Not built or heard on Windows with the real libpd and audio device.

## Is PD live or an offline render?
Live. libpd runs in 64-sample ticks on a worker thread and the audio device
plays the result in real time. There is no offline PD render (export is still
missing). Before this update a patch was loaded ONCE when Run / Run PD started,
so edits to the .pd were never heard until a restart.

## Live coding
* `PdBackend::reloadPatch` loads the saved patch into the SAME running libpd
  instance: the new patch is opened first, then the old one is closed, DSP is
  never switched off. A failure keeps the old patch sounding (message in the log).
* Clip editor **Run** (single patch): while the clip is running, a pause in typing
  (0.7 s) writes the patch file and reloads it ("LIVE" indicator). **Save patch**
  does the same immediately.
* **Run PD** (PD-only engine) and **Run Csound + PD** (shared engine): re-saving the
  patch (from CAW or from Pure Data itself) is noticed within 0.25 s and the clip is
  reloaded live. A sounding clip is re-initialised ($p1-p3, gate, start, current run).
  Runs, x-fields and MIDI changes are applied live too. Only adding/removing/muting
  clips (a different set of clips) still needs a restart.
* Csound+PD audition: Save restarts the audition from the saved file (as before).

## Other changes
* Button "x1 runs / x3-fields" is now **x-fields** (the popup still has Runs).
* The "+" next to the pen is gone. Click Pen = use pen, right-click = Pen 1 / Pen 2 (keys 8 / 9).
* Pen drawing: the lane is kept sorted by time while drawing, new dots of the stroke
  have no dot symbol until release, and overlapping left-to-right or right-to-left
  overwrites cleanly.
* MIDI on/off in the clip editor (PD and Csound): ON creates the MIDI track (region),
  OFF asks for confirmation, then removes the MIDI region and its notes (Ctrl+Z undoes).
* The PD clip editor now edits the WHOLE raw .pd file (no readable object-only view, no fixed row labels). Save / live apply write that text as is.
* PD clip editor redesigned as a code editor: description / buttons / short path / the .pd text. No Save, Discard or Objects / wiring buttons:
  typing is saved to the .pd file after a short pause (like the Csound editor), loaded live into a running clip, and external changes
  (Pure Data) appear in the editor. "..." holds Use test tone, Open in Pure Data, Reload from disk.
* The clip description box has the ghost text "Describe this clip..." (PD and Csound clip editors).

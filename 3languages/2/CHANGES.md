# PD compiler diagnostic transport (v27)

* Fixed the shared-engine log transport splitting the Pd error and its object
  text into separate rows. The compiler's object text now travels on the same
  log row, so the editor matcher can locate it.
* Preload errors are published even before the timeline starts playing.
* Generic errors no longer paint every object red. If Pd provides no reliable
  object text, CAW leaves the line unknown instead of claiming every line.
* The regression test uses a simulated libpd print hook, the actual PdBackend
  and shared PdHostMixer, and the same newline and clip-regex console path.
  It checks the `oc~ 440` error resolves only to line 1 for preload and audition.
  It does not replace real libpd or Windows app testing.

# PD engine state and source-line diagnostics (earlier)

* The transport now counts PD inside the shared Csound+PD engine as running, not just
  the standalone PD engine or clip audition.
* PD now keeps the object text Pd's compiler prints immediately before its generic
  `couldn't create` error, then compares that text to the readable editor rows. A
  unique match points to that exact row. If Pd truly supplies no object text, the
  console falls back to clickable candidate rows (`line ~N?`) rather than guessing one.
* libpd's `verbose(N):` prefix is stripped from that object-text line before matching.

# Clip-linked SC/PD console diagnostics (this update)

* SuperCollider now includes clip ID markers around code evaluation, so compiler errors
  such as `Class not defined` are tied to the correct clip even when sclang does not
  throw through CAW's error handler.
* The console keeps one primary SC error row with the clip and timeline location; stack
  trace rows are details, not extra counted problems. Exact parser lines and unique
  best-guess runtime lines still open and mark the code line.
* PD captures libpd error prints per isolated patch, attaches failures to the owning
  clip, and gives clip-specific warnings when an active patch produces no audio for
  1.5 seconds. Intentional silence remains a warning, not a fatal error.

# SuperCollider clip error lines (this update)

* The console now attaches SuperCollider parser-reported source lines to the clip error.
* For runtime errors, CAW shows `line ~N` and highlights the line only when the missing
  selector or variable points to one unambiguous line in that clip. The `~` means the
  location is an inference: SuperCollider does not provide exact source lines for most
  runtime errors.
* Clicking the console line tag opens the clip editor at that line; the editor also marks
  the line in its error gutter.

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

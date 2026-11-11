# Changes

- MIDI regions: drag on empty space in a MIDI lane to box-select (Ctrl/Shift adds), like Csound clips.
- Monitor fix: the CawMon monitor instrument was defined but never started. The engine score now starts it, so master meter, scope, spectrum and track meters get data.
- View menu lists the shortcuts (Alt+H/J/M/N for automation and MIDI lanes, tools, M/S, etc.).
- MIDI lane is hidden unless a Csound clip on the lane has midi: "on" (a new lane with no clips has no MIDI lane).
- Settings: one typed box holds the Csound options line and the header values (sr, ksmps, nchnls...). The read-only preview is gone. Saved in project format 10.
- Tools: Select / Pen / Cut / Join in one list, one active at a time. Keys 1, 8, 3, 4. M and S mute / solo the selected track.
- Master meter has one bar per output channel (nchnls). Monitor panels resize by dragging the gap between them (double-click = reset), and reorder by dragging a panel title onto another panel.

## Round 2
- Monitor data now travels over UDP (OSCsend from CawMon to port 7771) instead of the console text, so buffering/message levels cannot block it. The app warns in the console if nothing arrives after 5 s.
- Play (timeline) now starts Csound automatically if it is not running.
- One box-select for Csound clips and MIDI regions: drag on empty space, or double-click and drag. A double-click without a drag still adds a clip.
- Csound clips and MIDI regions move together when both are selected.

## Round 3
- Selection box is drawn on top of all lanes (MIDI, automation, clips).
- Box-select also picks automation dots of visible lanes.

## Round 4
- Settings: Output and Input soundcard have a "None" entry. Output None writes `-odac -+rtaudio=dummy` (real-time clock, no soundcard opened).
- Del removes everything selected in one go: Csound clips AND MIDI regions AND automation dots, on every lane (a box over several automation lanes now keeps the dots of all lanes, not just the busiest one). Clicking a dot drops a stale clip selection first.
- The box-select ends at the lane area edge, so releasing the mouse over the left panel gives the same box. Keyboard shortcuts no longer depend on where the mouse is.
- Track meters: every output channel has its own red clip LED (click one to reset it).
- Automation: a click on an empty lane drops the dot on mouse-down (no wait for release). Dragging instead still box-selects (the dot is taken back).
- Play only plays the timeline. It never starts Csound; use Play Csound for that. Without a running engine the playhead moves silently.
- `dawrun` / self-scheduling, no tags. Write the loop straight in the clip code. `schedule "this", ...` (or `dawrun "this", ...`) starts a copy of the clip's own instrument; every p-field is plain Csound maths (p4 + indx * 2, p5 * 0.8 ...). The loop (the top-level while/if block that holds the "this" lines) runs once when the clip starts, then that instance switches itself off; each scheduled copy skips the loop and runs the code below it. Code above the loop runs for every copy. `dawrun "Other", ...` is a plain schedule of another instrument.

      indx = 0
      while indx < 5 do
        dawrun "this", indx * 0.5, 4, p4 + indx * 2, p5 * 0.8
        indx += 1
      od
      asig oscili 0.2, mtof(p4)
      outs asig, asig
- `dawrun <p4 maths>, <p5 maths>, ...` (one line, in the clip code): the clip's **x** field is the number of copies. They all start at the clip start and last the clip length. `indx` (0 .. x-1) can be used in the maths, e.g. `dawrun 60 + indx*4, 0.3 - indx*0.04` with x = 4 gives p4 = 60, 64, 68, 72 and p5 = 0.30, 0.26, 0.22, 0.18. The lines below are the sound of every copy. Change x or the code, then restart Csound. A clip with dawrun / schedule "this" is triggered once by the timeline (no slot splitting).
- The **x** box in the clip editor bottom row is now shown for MIDI clips too (with dawrun, every MIDI note starts x copies).

## Round 5
- `cawRun` (alias `dawrun`): one line replaces the whole loop. `cawRun <p4>, <p5>, <p6> ...` starts x copies (the clip's x box) together at the clip start, every argument is plain Csound maths. Inside the maths: `indx` = copy number 0 .. x-1, `iRun` = number of copies (also usable in the rest of the code), and `400 step 100` = 400 + indx * 100 (400, 500, 600 ...).
  Same as: `iSendp6 = 400; while indx < iRun do schedule "same instr", 0, p3, p4, p5, iSendp6; iSendp6 += 100; indx += 1; od`
- New demo lane "Unison" with the clip Unison1 (x = 3): `cawRun 0.3 / iRun, indx / max(iRun - 1, 1), 400 step 100` -> p4 volume, p5 pan, p6 frequency 400 / 500 / 600 Hz.
- Copies are now set in the `<caw>` block, no Csound maths and no iRun / indx / step in the code. The clip's x box is the number of copies; one line per parameter:
  `{p: 4, value: 0.1}` same for all, `{p: 4, total: 0.3}` shared between the copies, `{p: 5, from: 0, to: 1}` spread first to last, `{p: 6, start: 400, step: 100}` 400, 500, 600 ...
  A p-field without a line is passed on unchanged (MIDI clips: p4 pitch, p5 velocity). The Unison demo uses this. `cawRun` in the code still works for special cases.
- `<caw>` copies shorthand: `{p: 6, from: 400, +100}` (400, 500, 600), `-100` (400, 300, 200), `*2` (100, 200, 400), `/2` (800, 400, 200). `from` is the first copy's value (`start` works too; default 0, or 1 for * and /).

## MIDI editor playhead
- The MIDI editor shows the playhead; click or drag its ruler to move it.
- Keys in the MIDI editor: - / + move the playhead back / forward (0.25 s), Shift+- / Shift++ move fast (2 s).

## Copies syntax cleanup
- `<caw>` p-lines are now only: `{p: 4, value: 0.1}`, `{p: 5, from: 0, to: 1}`, `{p: 6, from: 100, +100}` (also -, *, /). x3 with `from: 100, +100` gives 100, 200, 300. The old `total`, `start` and `step` keys are removed. Unison demo updated.

## Code editor
- `<caw>` p-lines have no `value` key any more (write `p4 = ...` or `iAmp = ...` in the code). Left: `from, to` and `from, +/-/*/ step`.
- Colors: control words (instr, if, while ...), ~1000 Csound opcodes, p-fields (p4, p5 ...) and `<caw>` tags have their own colors. Opcodes you define yourself (`opcode MyUdo, ...`) are recognized as soon as you type them, and at startup the app also reads the full opcode list from `csound -z`.
- "Test" button next to the clip name: runs only that clip (no sound, 5 s max) and prints the Csound messages in the console; "line N" is translated to the line of the clip code. The `<caw>` block keeps its lines, so the numbers match the editor.

## Console: errors name the instrument
- Play, Play Csound, Render and Test: Csound's `line N` in a message becomes `instr Lead1, clip line K` (the line of that clip's own code). Lines made by the app (cawRun expansion, meter taps) say `generated line` or point to the line they belong to.

## Pure Data clips + Run clip
- "Test" is now "Run clip".
- Language box of a clip: Csound 7 or Pure Data (project format 11 saves it; each language keeps its own text when you switch). A Pure Data clip holds the .pd patch text, with its own colors. Write a normal patch ending in [dac~].
- Play / Play Csound / Render / Run clip first render the changed Pd clips offline (`pd -nogui -nosound -batch`, [dac~] redirected to a file, length = clip length) into `caw_pd/<clip>.wav`; the clip's Csound instrument plays that wav (`diskin2`), so it sits in the meters, mute/solo and the render. Pd messages show in the console as `[Pd Lead1] ...`.
- Settings: "Pure Data executable" (pd.exe path, or `pd` on PATH). Needs Pd 0.51+ (for -batch).

## Pd clip: load a file
- A Pure Data clip can hold one line `load: C:/patches` instead of the patch: it loads `<folder>/<instrument name>.pd`. `load: C:/patches/x.pd` loads one file. Changes in the file are noticed on the next Run clip / Play. Example in `examples/pd`.

## Pd clips: automation in, values out
- A Pd clip may have a `<caw>` block. `{ chn: "cutoff", range: (0,1000,1), value: 200 }` gives the clip an automation lane; its values reach the patch at `[r cutoff]` (rendered with the clip, 100 values per second).
- `{ send: "pitch", to: "Lead1", chn: "input1" }`: what the patch sends with `[s pitch]` is written as an extra wav channel and set in channel `input1` of the Csound instrument Lead1 while the Pd clip plays.
- Pd audio is now rendered as 32-bit float wav. Changed automation or sends re-render the clip. See examples/pd.

## Pd: patch inside the project
- Pd clip with a `load:` line: new "Import patch" button copies the .pd text (and keeps the `<caw>` block) into the clip, so the patch is visible in the editor and saved in the project file.

## Pd demo lane
- The default project has a "PureData" lane with the clip Pd1 (language Pure Data): the patch text is in the clip, and its `<caw>` block gives it a `freq` automation lane (`[r freq]`).

- Pure Data program: if Settings has just `pd`, the usual install folders (C:/Program Files/Pd/bin/pd.exe ...) are tried first. The error now says which path was tried.

## csbridge~ (Pd external)
- New folder pd_external/csbridge: [csbridge~] hosts Csound inside Pd. Pd signal inlets go to chnget "in1".., Csound chnset "out1".. goes to Pd signal outlets. Not yet used by the Caw offline Pd render. Demo: examples/pd/csbridge-help.pd + echo.csd.

## Round 6 fixes
- Pd render length is exactly the clip duration (was +0.1 s).
- Pd clips are one piece of wav: never split into x slots, and when the timeline starts or jumps inside a clip the wav starts at the right offset (p4 = skip seconds), so sound and baked automation stay in sync.
- Run clip: if Play Csound is running, it plays the WHOLE clip in that engine. Otherwise the old 5 s silent test.
- Space plays/stops the timeline also when a code editor window is focused, unless the text cursor is in the code (click the title or any field to leave the text). Ctrl+Space always works.

## Round 7
- Pd render no longer uses [writesf~] (it cannot keep up when Pd runs faster than real time: short / clicky wav). The sound is recorded into tables ([tabwrite~], exactly clip length x sample rate) and written with [soundfiler]. Old wav files are re-rendered once.

## Round 8
- Stop (Space / Stop) now really stops fixed-length notes such as Pd clips: the engine uses turnoff2 besides the old negative i-statement.

## Round 9
- SuperCollider clips (language list in the clip editor): rendered offline with sclang + Score.recordNRT, played like Pd clips. Code = body of a function, last value [left, right, send1 ...]. Automation lanes = \\name.kr(). Settings: SuperCollider executable. Project format 12.
- Editor window title is "Code editor - <clip>", the language is called "Csound" (was "Csound 7").
- Ctrl+Q / File > Exit quits.
- New default bridge demo (examples/bridge/README.txt): Pd and SuperCollider send data to a Csound instrument.

## Round 10
- Run clip becomes Stop while the clip plays in the engine, its silent test runs, or its Pd / SuperCollider render is pending. Stop turns the clip off (and cancels a pending render).

## Round 11
- Language dropdown is as wide as "SuperCollider".
- Bridge demo Voice1: vco2 gets a fixed amplitude (k/i-rate only), the envelope is multiplied after it.

## MIDI for Pure Data and SuperCollider
- `midi: "on"` in the `<caw>` block now works for Pure Data and SuperCollider clips too. The clip gets the MIDI lane / piano roll like a Csound clip.
- Pure Data: notes arrive as `[r midinote]` (pitch velocity, velocity 0 = note off), `[r midipitch]`, `[r midivel]`, `[r midigate]`.
- SuperCollider: one synth per note with the controls `\note` (0..127), `\vel` (1..127) and `\gate` (1 held, 0 released). Use `EnvGen.kr(Env.adsr, gate, doneAction: 2)`.
- The clip is rendered offline once with every note baked in (wav from timeline 0 s, last note + 2 s release), then played by its Csound wrapper instrument. Note or MIDI region edits trigger a new render on the next Play / Run clip.
- Run clip on such a clip plays all its notes. See examples/midi.

## Score (contemporary-music SVG score), round 2
- **Score lanes**: Score editor > "Score lanes". Define lanes (name, small line, staff / graphic / auto, clef) and put tracks or single clips into them. A graphic lane can hold many tracks (e.g. one "Electronics" line). "Create lanes from tracks" does it in one click.
- **Left panel**: new "Sc" button per track = score lane, hide, name, automation channels (hidden / detailed curve / overall up-down).
- **Clip editor / clip right-click**: "Score..." = lane of this clip, MIDI notes (every note / overall up-down contour / hidden), sound style (shape / line / grains / block), text in score.
- **Piece is for**: Electronics only (no 5-line staves, pitch is drawn as free pitch lines) or Instruments + electronics.
- **Detailed score is now a document**: A4 landscape pages: cover (lanes, sections, notes, key), system pages (as many systems per page as fit, stretched to fill the page; seconds per system and systems per page are settings), event-list pages. A .html file shows and prints all pages (print to PDF). Overview = one page.
- Format 14 (older files still open). Everything is in undo / redo.
- File > Load 3-min score example uses lanes, contour clips, grains and overall automation.

## Score round 3
- **Score preview (live)**: Score menu / Score editor button. Shows the score inside Caw and redraws within 0.2 s whenever you change anything (notes, clips, lanes, automation, texts). Overview / Detailed, colour / black, page buttons, zoom, "Export SVG".
- **File / Score > "Load 3-min example + preview"**: opens the example with editor + preview, edit parameters and watch the result.
- **Spectrum lane**: tick "Spectrum lane" in the score editor; after "Render audio" the spectrogram (40 Hz - 16 kHz, log) of the rendered WAV is drawn under the lanes. Per clip "frequency band (Hz)" (clip Score... menu) turns an electronics lane into a frequency axis and places each sound at its band.
- **Detailed score = full score**: cue numbers on every clip, times / length / copies / <caw> parameter values / frequency band under each clip, note count, pitch range, dynamics, automation start and end values, cue-list pages with all data.
- **Automation**: every automation lane now has an "Sc" button (orange = in the score): not in the score / detailed curve / overall up-down. Default is off.
- **Clef** only appears for instrument (staff) lanes; electronics lanes and electronics-only pieces have no clef or staff options.
- Format 15.

## Score round 2 (full score)
- Same name = same lane: tracks with the same name (or score name) are one lane of the score, and one row in a graphic lane. Switch off in Score editor > Lanes. The Lanes tab lists what the score will contain.
- Staff lanes have a "No clef" choice (five lines, no clef sign).
- Automation is now a lane like an instrument: it sits under the lane of its track with name, range, grid, filled curve, break points and peak values (full curve, or a compact contour). Every automation with points is in the score by default; the Sc button / track menu can switch it off.
- The Sc button on an automation lane no longer sits over the value box of the selected dots.
- The clip Score... menu knows where the notes come from. MIDI input clips: show every note / up-down / hidden / pitch range band / notes with names, and an "Edit MIDI notes" button. Clips whose notes are made in the code: write the notes you want to see ("C4 E4:2 - G3+D4"), choose how to show them, plus sound shape and frequency band. Every clip can have a dynamic marking (mf, pp<ff, f>p) drawn with hairpins.
- "Detailed score" is now "Full score": fixed A4 landscape pages, systems read from left to right and top to bottom, page after page. A system that is too tall is scaled to fit instead of growing the page. System barline and brackets at the left, abbreviated names after the first system, final barline, page numbers.
- Tempo and time signature (Score editor > Full score): barlines, bar numbers, tempo mark, every system ends on a barline.
- Full score export: one .html with all pages inside (print it to PDF, A4 landscape), plus _score_full_pNN.svg files.
- Top bar: Play / Play Csound / time stay on the left, the rest is in three tabs (Edit, Project, Score), and a status pill (Csound running / starting / stopped, rendering) is always visible on the right.
- Project format 16 (tempo, meter, merge by name, written notes, dynamics).

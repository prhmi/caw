# CAW

A timeline for Csound, Pure Data and SuperCollider clips, with MIDI, automation and a score export for contemporary music.

## Install
Installer coming later.

## Use
- File > New project starts an empty project, File > Open project loads a `.caw` file.
- **Csound / Pd / SC / Run all** (top bar): start only that language, or all three. The timeline (Play) triggers the languages that are running.
- In a clip editor, **Run** runs only that clip, in its own language.
- The master meter is the right panel (drag its left edge to resize). Right-click the spectrum for drawing detail, the signal display for the time window (x range).
- Pure Data clips: build the patch in Pd (Edit in Pd, then Reload). The code editor shows the objects as readable text where you can change arguments.
- Score: the name box of a track lane, a MIDI lane and an automation lane is in that lane's Sc button. The same name = one lane. Per track: **overlap** (default: the shapes are combined into one, same colour = one shape, different colours = both colours) or **separated** (on top of each other at 50%, not combined).
- Score, clip (Score... in the clip editor): **Overall** = the whole shape of the sound the clip makes (amplitude and where the spectrum sits, measured by **Render audio**, not the MIDI notes). **Detailed** = also everything the code sends to the channel `score`: Csound `chnset kVal, "score"` (a-rate signal or an S string work too), Pure Data `[s score]`, SuperCollider `{ send: "score" }` in the `<caw>` block. Looks: shape, line, grains, block, sliced wave (granular), FM graph, dots and lines (keys, leads). Every track is free to be instrument or electronics. Automation of the clip is set in the same Score... box.
- The console tags every line with its language and clip. **Filter** sets how much it tells (default: errors, warnings, what runs).
- **Errors in the console** (Csound, Pure Data, SuperCollider): every error gets a red tag with the clip, the line of its code and where the clip starts on the timeline, e.g. `line 5  @ 12.40 s (0:12.4)` (`+2` = the same instrument is used by more clips, the tooltip lists all their start times). The message also ends with `>> clip line N: the code of that line`. Click the tag: the clip editor opens with the cursor on that line, the line is marked red (hover it for the message) and the timeline scrolls to the clip. **Problems** (console header, or Filter) shows only the errors. Marks go away when you edit the code or run again.
  - Csound: the line of the clip code.
  - SuperCollider: compile errors (syntax, unknown variable or class) give the exact line (sclang `line N char M`). A bracket that is not closed is reported by sclang after the end of the code, so the tag says so and points at the last line. Run-time errors (`Message 'x' not understood`) have no line in sclang: the clip code is searched for the name `x`; if it appears on several lines the first is used and the others are listed (`or line 4, 9`) and marked "maybe".
  - Pure Data: Pd does not tell lines, so the object named in the message is looked up in the readable object list: the whole object text (`osc~x 440 ... couldn't create`), else objects with that name (`+: no method for 'foo'`), else objects that have it as an argument (`nosuch: no such array`). Several matches: the first is used, the others are listed. Pd 0.52+ prints a failed object on two lines (`osc~x 440`, then `... couldn't create`); they are joined. Warnings (`+~: extra arguments ignored`) get the line in their text.
  - The same error repeated many times during a render is shown once, with a count at the end.

## Build
Needs Visual Studio (C++), CMake and Git. Run `build.bat` to build and start.

Offline / download fails: download these two from GitHub as zip, unpack them into `third_party/` so that the files are at `third_party/imgui/imgui.cpp` (Dear ImGui v1.91.9, github.com/ocornut/imgui) and `third_party/ImGuiColorTextEdit/TextEditor.cpp` (github.com/BalazsJako/ImGuiColorTextEdit), then delete the `build` folder and run `build.bat` again. Or copy the `build/_deps` folder of an older working build into the new `build` folder.

`pd_external/csbridge` is an optional Pd external, built separately (see its README).

## Score items (marker and graph tracks)
- Marker types: Comment, Letter box, Cue (Q), Code. The Icon type is gone. Start and length are no longer typed: drag the item on the timeline, drag its right edge.
- **Size** (40 to 400 %) sets the size of the shape or box, **Font size** (6 to 40 pt, markers) sets the text, on the timeline and in the score. The score band grows with the biggest item.
- Graph shapes are graphic-score gestures that stretch to the length of the item: grain cloud, glissando up / down, swell, fade, swell and fade, sustained tone, noise band, pulses accelerating / slowing, tremolo wave, attack burst, tone cluster, glissando fan, free scribble, spiral, silence (fermata), rising points.
- Note heads, sharp and flat signs on the staff are drawn like the python score library (def.py: note(), sharp(), bflat()). A# is written as Bb.
- Project files are version 26. Older files open: icons become comments, old graph shapes become a grain cloud.
- Quarter-tone signs (from def.py) for written notes in the score: `Cq4` quarter sharp (Qsharp), `Eqb4` quarter flat (Qflat), `Bd3` koron. They change only the sign in the score, not the sound.
- Graph tracks: on the timeline a shape is at most as big as its row (100 %); the score shows the full size. Automation has no dots in the score. Pen 2 keeps every point of a stroke but only the start and the end of the stroke show a dot (saved in the project, so it stays when you switch tools). Pen 1 keeps its few dots.
- Graph items can also be note signs drawn like def.py: Note head, Sharp, Flat, Quarter sharp, Quarter flat, Koron. Place them on the graph track timeline like any shape.
- Cue (Q) styles: circle, square, diamond, underlined, brackets [ Q ].
- Graph items: a text box beside the shape (a few letters, with Font size), and a Ratio (30 to 300 %) that bends the edges of Swell, Fade and Swell and fade.
- Marker type Span bracket: a horizontal line with 90 degree ends (down with the comment above, or up with the comment below), the comment centred on it. Length = drag on the timeline.
- The text of a graph item starts right at the edge of its shape, so a note sign is followed directly by its letters (#A).

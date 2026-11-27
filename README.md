# CAW — Code Audio Workstation

A timeline DAW where every clip is **code**. Csound, Pure Data, SuperCollider, VST3 plugins and audio samples play side by side on one timeline, share automation lanes, and send audio and control values to each other. and generates musical score from the project

- **Engines:** Csound (libcsound64) · Pure Data (libpd) · SuperCollider (libscsynth)· host engine (two seperated engines; one for *.vst3 (effects and instrument) and another for sample tracks. + master host).
- **Live coding:** edit a clip while it plays, press **Run** (Alt+Enter), and the engine takes the new code at once (some functions are not live)
- **Virtual soundcard:** projects can be 1–64 channels; anything wider than your audio setting folds down (channel k → k mod N)
- **VST3 host:** VST3 instruments and effects, with their own editor windows on every system: Windows (HWND), macOS (Cocoa) and Linux (X11)
- **Platforms:** Windows (main, tested). macOS and Linux build with `build.sh` (see *Build*); the Linux build runs, macOS and the Linux/macOS editor windows are new and not tested yet

---

## Quick start

1. **Settings → Engines…:** tick the engines you want: **Csound**, **Pure Data**, **SuperCollider**, **CAW: VST**, **CAW: sample**. Only ticked engines start. With none ticked, CAW only edits and makes scores.
2. **Settings → Programs:** set the paths to Csound, Pure Data and SuperCollider if CAW didn't find them. VST3 folders are scanned automatically.
3. **Settings → Audio:** pick the output device, sample rate and channel count (2 = stereo).
4. Press **DSP** to start the ticked engines. The status at the top right shows **DSP: starting…** and then **DSP: on**.
5. Add a track (**+**), choose its kind, right‑click the lane → **Add clip**, and write its code.
6. Press **Play**. Edit a clip and press **Run** (Alt+Enter) to apply the change live.
7. **Automation:** a name used in code with `cawget` appears in the track's **+** menu as a lane. Draw it, or record it with **R**.
8. **File → Export → Audio export…:** render the project to WAV, FLAC or MP3, either **offline** or as a **real-time capture** (see [Working with the engines](#working-with-the-engines)).

---

## Working with the engines

### The DSP button

**DSP** starts and stops the sound engines: every engine ticked in **Settings → Engines**, together, mixed into one audio output. Press it again to stop them all.

- **DSP off:**
  - **Play** still moves the playhead and the timeline runs, but nothing sounds.
  - A clip's **Run** plays that clip once. If its engine isn't running, Run starts it first.
- **DSP on:**
  - **Play** sounds every engine.
  - **Run** only **applies** the edited code; it doesn't play the clip by itself. A clip that is playing restarts with the new code; otherwise the new code is used the next time the playhead reaches the clip.
- **A track whose engine isn't ticked** is skipped when you press Play, with a message such as *"Enable SuperCollider in Settings > Engines"*. The other engines still play.
- **Changing the ticked engines while DSP is on** stops everything. Press DSP again to run the new set.
- **Locked tracks (LKD)** play their rendered WAV and need no engine.

| Engine | What it plays | Runs |
|---|---|---|
| Csound | Csound tracks (one Csound with all clips) | `libcsound64` / `csound64.dll` |
| Pure Data | PD tracks (one Pd instance per clip) | libpd (`MULTI=true`) |
| SuperCollider | SC tracks: `sclang` sends to a `scsynth` running inside CAW's audio callback | CAW's `libscsynth` |
| CAW: VST | VSTi and VSTe tracks (CAW's own VST3 host) | inside CAW |
| CAW: sample | sample tracks (audio files, grains, CAW code) | inside CAW |

Every engine writes into the same **virtual soundcard**: the project's channels `out1…outN`. That's mixed once, put through the master volume and FX, and sent to the audio device. A device with fewer outputs gets the wider channels folded onto it: `out3 → out1`, `out4 → out2`, and so on.

### Offline render and real-time capture

**File → Export → Audio export…** has two **Render mode**s:

| | **Offline** (faster than real time) | **Real-time capture** |
|---|---|---|
| How | each track is rendered on its own by its engine's **offline** pipeline, then CAW mixes the parts into the file | the transport plays once from the start, and the final master mix (every engine live) is recorded |
| Speed | as fast as the CPU allows | as long as the song |
| DSP | not needed | needed; the export **starts the ticked engines by itself** and waits for them |
| Engines | Csound: offline Csound host · PD: C++ host · SuperCollider: **NRT** (`sclang` + `scsynth -N`, so `scsynth` must sit next to `sclang`) · VST and samples: offline rack | exactly what you hear live |
| Best for | final mixes, long pieces | checking that the file sounds exactly like playback, live input, things that only exist in real time |

Both modes follow the export settings:

| Setting | Meaning |
|---|---|
| **Format** | WAV 16/24/32‑bit, FLAC 16/24‑bit, MP3 with bitrate |
| **Source** | **as virtual** = the master mixdown · **as real routing** = the whole virtual soundcard at the project's channel count, every track at its routed outputs |
| **Convert** | fold to Mono / Stereo / Quadro (channel k → k mod M) |
| **Files** | one file, or one mono file per channel |
| **Channels** | channel count of the file |
| **Start / End / Tail** | the range, and extra seconds after the last clip |

### Locking a track (LKD)

**LKD** on a track renders it into a WAV, using the same render mode, and **locks** it. A locked track then plays that WAV instead of running its engine:
- It saves CPU.
- It keeps a sound fixed while you change other things.
- It lets a project play on a computer without that engine.

On a **VSTe** track, LKD renders the effect together with its sources. Press LKD again to unlock the track.

---

## Track kinds

| Track | Clip code | Output |
|---|---|---|
| **Csound** | an instrument body (`out`, `outch`) | `nchnls` channels |
| **Pure Data** | a `.pd` patch | `[dac~ 1..8]` |
| **SuperCollider** | SC code (`play`) | its bus channels |
| **VSTi** | CAW code (notes, parameters) | the plugin's outputs |
| **VSTe** | CAW code (parameters) | the effect's outputs |
| **Sample** | CAW code (rate, gain, grains) | the file's channels |

Each track has a **channel format** (VSTi/VSTe: Mono / Stereo / 4 / 6 / 8 ch). Track channels go to the master 1:1, and anything above the audio setting's channel count folds onto it: on a stereo setting, channels 1+3 go to the left output and 2+4 to the right.

---

## Csound clips

Write the instrument **body** (no `instr` line). `p3` is the clip length.

```csound
kNote cawget "input1"          ; automation lane "input1" (0..1)
aEnv  linen 1, 0.02, p3, 0.1
aOut  oscili 0.2 * aEnv, cpsmidinn(48 + kNote * 24)
outch 1, aOut, 2, aOut
```

| Command | Meaning |
|---|---|
| `kX cawget "name"` | read the automation lane `name` |
| `cawset kX, "name"` | write the lane `name` (recording) |
| `caws kX, "name"` | **send** a number to every engine |
| `kX cawr "name"` | **receive** a number sent with `caws` |
| `cawbsend "name", aL, aR` | send **audio** to a CAW bus |
| `aL, aR cawbrecv "name"` | receive audio from a CAW bus |
| `cawsa "VSTeTrack", aL, aR` | send audio into a VSTe track |
| `aL, aR cawra "Bus"` | read the audio a VST track sent to `Bus` |

---

## CAW code (VST and sample clips)

A small language that runs inside CAW. You need no Csound for it.

```
seed 0                          # new random numbers every run (seed 5 = same every run)
every 0.5 {                     # run the block every 0.5 s
  note 48 + int(rnd(12)), 100, 0.4    # pitch, velocity, length (s)
}
when P3 > 0.8 { cc 1, 127 }     # while true: MIDI CC 1 = 127
P1 = uni(lfo(0.2))              # drive picked plugin parameter 1 (0..1)
p2 = cawget("cutoff")           # bind parameter 2 to the automation lane "cutoff"
x += 1                          # your own variables (start at 0)
```

| Item | Meaning |
|---|---|
| `P1`…`P8` | the 8 plugin parameters picked in **Parameter mapping…** (0..1, read and write) |
| `every t { }` / `when c { }` / `once { }` | timed, conditional and one‑shot blocks |
| `note p, v, len` · `cc n, v` · `msg s, d1, d2` | MIDI notes, controllers, raw MIDI |
| `pitch vel dur time` | the starting MIDI note, clip length, seconds since the clip started |
| `lfo(f) tri(f) saw(f) sqr(f)` | oscillators (−1..1); `uni(x)` maps to 0..1 |
| `rnd(n)` `rnd(a,b)` | random numbers; `seed N` sets the sequence |
| `smooth(x,t) line(a,b) clip(x,lo,hi)` | smoothing, ramps over the clip, limits |
| `level(in)` | VSTe: the loudness of the incoming sound |
| `mtof(n) min max abs sin cos int sqrt` | maths |
| `csound: <line>` | any Csound line, passed through |

### Sample clips

The same language, plus a few sample-only settings:

```
rate = smooth(lfo(0.2) * 0.3 + 1)    # speed / length 0.25..4
gain = smooth(line(1, 0))            # fade out (0..2)
reverse = 1                          # play backwards
normalize = 1                        # scale the file to full level first
out1 = in1                           # routing: a mono file on both outputs
out2 = in1
every 0.1 { play: rnd(10), 0.2, 1, rnd(4) + 1 }   # granular: start (s), duration, speed (<0 = reverse), output channel
```

### VST plugins

Pick a plugin in the track's VST panel. **Open / Show** opens the plugin's own **editor window**, with its knobs and graphics. Closing it keeps the plugin, its sound and its settings.

- Knob moves in the editor reach the sound live, and are saved with the project.
- **Parameter mapping…** picks up to 8 parameters as `P1`…`P8`. They can then be driven from CAW code, automation lanes and MIDI CC.
- **Learn:** arm a slot, then turn a knob in the editor; that parameter becomes the slot.

| System | Editor window | Plugin folders scanned |
|---|---|---|
| Windows | native window (HWND) | `C:\Program Files\Common Files\VST3`, `%LOCALAPPDATA%\Programs\Common\VST3` |
| macOS | Cocoa window (NSView) | `/Library/Audio/Plug-Ins/VST3`, `~/Library/Audio/Plug-Ins/VST3` |
| Linux | X11 window (X11 or XWayland) | `~/.vst3`, `/usr/lib/vst3`, `/usr/local/lib/vst3` |

You can add more folders in **Settings → Programs → VST3**. A plugin has to be built for the system it runs on: a Windows `.vst3` doesn't load on Linux or macOS.

### VST output routing

**Routing…** in the VST panel maps plugin channels to track outputs. Press **Run** (or Ctrl+Enter) to apply it live:

```
out1 = in1
out2 = in2
out4 = in2      # out3 has no line: it stays silent
```

**Clear** removes the routing, so plugin channel k plays on track channel k again.

---

## Connecting engines

### Audio

| From \ To | How |
|---|---|
| any track → **VSTe** | in the VSTe panel, **AUDIO INPUT**: tick the source tracks (Csound, PD, SC, sample, another VST). Each input can be *insert* or *sidechain* |
| Csound → VSTe | `cawsa "VSTeName", aL, aR` |
| Csound ↔ PD ↔ SC | CAW audio buses (stereo, one block of delay between engines) |

| Engine | Send | Receive |
|---|---|---|
| Csound | `cawbsend "name", aL, aR` | `aL, aR cawbrecv "name"` |
| Pure Data | `[caws~ name]` | `[cawr~ name]` |
| SuperCollider | `~cawbsend.("name", sig)` | `~cawbrecv.("name")` |
| Sample track | writes its mix to `smp_<track name>` automatically | — |

### Control numbers (the CAW hub)

One shared number per name, seen by every engine:

| Engine | Send | Receive |
|---|---|---|
| Csound | `caws kVal, "name"` | `kVal cawr "name"` |
| Pure Data | `[s $0-caws-name]` | `[r $0-cawr-name]` |
| SuperCollider | `("CAWS name " ++ value).postln` | `~cawr[\name]` |

### Automation lanes

The same lane drives every kind of engine:

| Engine | Reads the lane `name` |
|---|---|
| Csound | `kX cawget "name"` |
| Pure Data | `[r $0-cawget-name]` |
| SuperCollider | `~cawget.("name")` |
| VST | `p2 = cawget("name")` or **Parameter mapping…** |

---

## Score generation

CAW draws a **contemporary-music score** of the project: a mix of staff notation and graphic notation. **No external library is used.** The whole renderer is CAW's own C++ code (`score_svg.cpp`, `marks_gfx.h`):

- It builds the score as a list of shapes (lines, rectangles, curves, text).
- The same list is written to **SVG** files and drawn in CAW's live **Score** preview window.
- Note heads, sharps, flats, quarter-tones and koron are drawn as vector curves, not with a music font.

**Two kinds of score:**

| Kind | Result |
|---|---|
| **Overview** | the whole piece on one page |
| **Detailed (full score)** | A4 landscape pages: a cover, then systems reading left to right and top to bottom, then cue-list pages. Each clip gets a cue number, time and parameters |

The full score is written as one SVG per page, plus an **HTML** file holding all pages. Open it and print (Ctrl+P, A4 landscape) to get a **PDF**.

**Where the content comes from:**

| Source | Shown as |
|---|---|
| tracks and clips | one lane per track (tracks with the same name share a lane), clip names, cue numbers, times |
| MIDI clips and written notes | notes on a staff with clefs; quarter-tones and koron |
| automation lanes | curves under their track |
| markers | comments, rehearsal letters, cues (Q), code shown in a monospace font |
| graph tracks | graphic shapes: grain cloud, glissando, swell… |
| **Render audio** (measured) | the real loudness shape and spectral centroid of every clip |
| the rendered WAV (optional) | a spectrogram strip |

**Written notes:** a clip can carry hand-written notes. Space separates notes, `:` gives a relative length, `-` is a rest, `+` makes a chord:
```
C4 E4:2 - G3+D4        # Cq4 quarter sharp, Eqb4 quarter flat, Bd3 koron
```

**Measured shapes:** without measurement, clip shapes are drawn from their envelopes. With **Render audio** and score analysis on, CAW adds a hidden Csound instrument (`CawScoreProbe`) to the render. Every 50 ms it writes the following for each clip:
- loudness, with Csound's `rms`;
- spectral centroid, with `pvsanal` + `pvscent`;
- anything the clip sends to the score.

The score then draws the real sound.

```csound
chnset kVal, "score"     ; plot a number or signal on the score under this clip
chnset Stext, "score"    ; write text on the score
```

Named streams can be set up under **Edit cawscore**. A signal stream is plotted as a min/max envelope plus its own spectrum frames, taken with `pvsanal` and grouped into logarithmic bands.

**Spectrogram:** CAW reads the rendered WAV itself (16/24/32-bit or float) and mixes it to mono. It then runs its own FFT: 4096 points, or 8192 at 96 kHz and above, with a Hann window and about 12 columns per second. The result is grouped into 40 logarithmic bands from 40 Hz to 16 kHz, with a 70 dB range.

**Options:** colour or black-and-white, overview or detailed, spectrum on/off, and lanes merged by track name.

---

## Build

**Build guides:**
- **[HOW to Build.md](HOW%20to%20Build.md)**: the short version, the commands for Windows, Linux and macOS.
- **[HOW to Build detailed.md](HOW%20to%20Build%20detailed.md)**: every step, what each script does, where files go, and the common errors with their fixes.

Short version:

| System | Build CAW | Engines (optional) |
|---|---|---|
| Windows | `build.bat` | `tools\windows\BUILD-CSOUND.bat`, `BUILD-LIBPD.bat`, `BUILD-LIBSCSYNTH.bat` |
| Linux / macOS | `./build.sh` | `tools/unix/build-engines.sh` |

**Packages with everything inside:** Windows `installer\` (Inno Setup `.exe`) · Linux `installer/linux/` (AppImage) · macOS `installer/macos/` (`.dmg`).

The VST3 SDK, Dear ImGui, PortAudio and GLFW are downloaded and built by CAW's own build, including the plugin editor windows (X11 on Linux, Cocoa on macOS). CAW runs without the engines; only their tracks are unavailable.

**Current limits on macOS / Linux:** a crashing plugin crashes CAW (on Windows CAW catches it). The macOS build, the Linux/macOS plugin editor windows and the Linux/macOS packages are new and not tested yet.

---

## License

Add your license here. CAW uses Dear ImGui (MIT), PortAudio (MIT), GLFW (zlib) and the Steinberg VST3 SDK (MIT / GPLv3). FFmpeg is bundled on Windows (GPL): see `ffmpeg-licenses/`.

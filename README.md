# CAW — Code Audio Workstation

A timeline DAW where every clip is **code**. Csound, Pure Data, SuperCollider, VST3 plugins and audio samples play side by side on one timeline, share automation lanes, and send audio and control values to each other.

- **Engines:** Csound · Pure Data (libpd) · SuperCollider · VST3 instruments (VSTi) and effects (VSTe) · sample tracks
- **Live coding:** edit a clip while it plays, press **Run** (Alt+Enter), and the engine takes the new code at once
- **Virtual soundcard:** projects can be 1–64 channels; anything wider than your audio setting folds down (channel k → k mod N)
- **Platforms:** Windows (main). macOS and Linux build with `build.sh` (see *Build*)

---

## Quick start

1. **Settings → Programs:** set the paths to Csound, and to Pure Data / SuperCollider if you use them. VST3 folders are scanned automatically.
2. **Settings → Audio:** pick the output device and the channel count (2 = stereo).
3. Add a track (**+**), choose its kind, right‑click the lane → **Add clip**, and write its code.
4. Press **Play**. Edit a clip and press **Run** to hear the change live.
5. **Automation:** a name used in code with `cawget` appears in the track's **+** menu as a lane. Draw it, or record it with **R**.
6. **File → Export:** render the project offline (WAV/MP3), as the stereo mixdown or every channel.

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

**Windows:** install Visual Studio with *Desktop development with C++* and Git, then run `build.bat`.

**macOS / Linux:** install CMake, Ninja and Git, plus on Linux the ALSA, JACK, GL and X11/Wayland development packages. Then run:

```bash
chmod +x build.sh
./build.sh            # ./build.sh nostart -DCAW_VST3=OFF  builds without VST
```

The first build downloads ImGui, PortAudio, GLFW and the VST3 SDK into `~/.caw_deps`. Later builds work offline.

**Current macOS / Linux limits:** plugin windows don't open (plugins still play), and a crashing plugin crashes CAW (on Windows CAW catches it).

---

## License

Add your license here. CAW uses Dear ImGui (MIT), PortAudio (MIT), GLFW (zlib) and the Steinberg VST3 SDK (MIT / GPLv3). FFmpeg is bundled on Windows (GPL): see `ffmpeg-licenses/`.

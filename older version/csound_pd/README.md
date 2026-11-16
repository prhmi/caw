# CAW 0.0.34 beta

## UI changes in this source package

- CAW starts maximized with a separate welcome screen using the original
  512-pixel logo as a texture; Help > About remains a different window.
- The master gain is a centered whole-dB drag control with a dB label
  (Ctrl-click to type). The meter panel can be narrowed further. Only the
  master meter has taller LEDs; track meters use the original size.
- Settings > Editor has separate program and code font sizes. These are
  stored per project. Graph and marker text uses higher-resolution font
  atlases when the system fonts are available.
- Score > Map score shows explicit VST outputs and literal cawsa/cawra calls
  in Csound clips on a read-only patch board. Dynamic routing, control flow
  and other kinds of code-defined sends cannot be reliably inferred.
- File > Export opens a side fly-out on click using a plus, not an arrow.
  Score opens a separate score export window.
- CAW window title bars have no collapse arrow. Double-click a title bar to
  collapse or expand the window.
- Windows plugin editor windows use the CAW application icon. Plugin editor
  sizing remains controlled by each plugin: this build does not force a
  700x400 limit or implement compact title-only plugin windows.

Projects saved here use format 51; back up existing projects before saving.
This build can read older formats, but older builds may not read format 51.
The program font setting is saved with the project rather than globally.

Build with `build.bat` on Windows or `./build.sh` on Linux/macOS. The first
build needs the dependencies described by CMake; the build scripts can fetch
them when internet access is available.

### Windows MP3 export without installing FFmpeg

The default Windows build downloads the pinned FFmpeg 8.1.2 essentials archive
once (about 105 MB), verifies its SHA-256 and copies `ffmpeg.exe` next to
`caw.exe`. Keep both files together when distributing CAW. CAW finds the
bundled encoder automatically even if launched from another working folder;
no end-user FFmpeg installation or PATH setup is needed. Csound is still
required for the audio render.

Optional: set `-DCAW_FFMPEG_EXECUTABLE=C:/path/ffmpeg.exe` to bundle a different
encoder, or `-DCAW_BUNDLE_FFMPEG=OFF` to disable bundling. A custom encoder
must support `libmp3lame`, and its licenses must be handled separately.

The downloaded Gyan FFmpeg build is GPL-licensed. Build documentation and
licenses found in the archive are copied to `ffmpeg-licenses` beside the
application. Before distributing, review the GPL and third-party obligations,
including provision of corresponding source and required notices. Copying
these files alone does not establish compliance. See
https://github.com/GyanD/codexffmpeg/releases/tag/8.1.2 and
https://ffmpeg.org/legal.html.

### Idle efficiency

The UI waits between frames. On Windows it targets 10 fps when the engine,
timeline, test and export are idle and there has been no recent input;
mouse or keyboard input wakes it promptly and temporarily raises the frame
rate. On Linux/macOS idle remains at 20 fps. During playback, timeline
movement or export it targets 60 fps. When minimized it continues servicing
the engine, transport, export, plugin learning and autosave without building
the arrangement or rendering the GPU frame. The application no longer forces Windows
process power throttling off, and its Csound opcode scan starts only when a
code editor or the engine needs it. Analyzer work only runs on new audio;
an inactive detailed FFT line no longer builds an empty curve. Autosave and
normal UI/event handling still run while the engine is off.

Temperature varies with the machine, drivers and workload. These changes
reduce avoidable work but do not guarantee a particular temperature.
See `PERFORMANCE.md` for the code audit, tests and remaining work.

## Exports and toolbar

The editing tools are directly in the toolbar. The Pen menu selects Pen 1 or
Pen 2. The Score button opens the score editor, including all score exports.
The old Edit and Project toolbar tabs are gone.

Use **File > Export > Audio export** to pick a destination, format, channels,
bit depth or MP3 bitrate, and tail. CAW's C++ host pulls Csound blocks, runs
its VST rack, applies the master gain, and writes audio without opening a
soundcard. FLAC and MP3 are encoded via a compatible libsndfile when present,
with ffmpeg as a fallback. The previous destination is replaced only after
the new output succeeds. MP3 is mono/stereo; FLAC supports up to 8 channels.
FLAC has a bit depth rather than a fixed bitrate because it is lossless.
On Windows CAW tries the bundled encoder, then any path saved by an older
project, then FFmpeg on PATH. On macOS/Linux CAW tries an existing saved path
or FFmpeg on PATH. The unnecessary FFmpeg path field is no longer shown.

Mono export averages source channels. Stereo export averages even channels
into left and odd channels into right; mono sources are copied to both.
For Quadro and custom output, CAW preserves channel order, drops extra
source channels and fills missing channels with silence. This is a simple,
documented mapping, not a surround-layout-aware downmixer.

The offline path uses the Csound API for Csound code and CAW's VST processing
for plugins. CAW captures exposed writable VST parameter values, including
unmapped parameters, before saving, starting the engine, or exporting.
It restores them by plugin parameter ID to the next engine or export instance.
Mapped automation and clip code can override the saved values. Plugin-private
state such as samples, preset data, and internal settings is not saved, so CAW
still warns before exporting a project with VSTs. Open a plugin editor at least
once to capture its settings before saving/exporting. Changes to the editor
after the engine starts require restarting the engine. Plugin load or routing failures stop the
export rather than silently omitting the plugin. SuperCollider and Pure Data
tracks are placeholders and make no sound; CAW logs a warning when they are
present. The score spectrum analyzer accepts WAV, not FLAC/MP3.

**File > Export > Score** and the controls in the Score editor offer dropdowns
for score type and color, followed by an Export button and a file picker.
Overview exports one SVG; full score exports one self-contained HTML file with
all pages. Print the HTML to create a PDF.

The **Master gain** control sits above the right-side master meter and accepts
whole dB values by dragging or Ctrl-clicking to type. 0 dB is unity. It affects
CAW host playback and offline export. It is not a mixer. The legacy external
Csound output path cannot apply it, so that path requires gain at 0 dB.
In the arrangement, Cut and Pen show scissors and pencil cursors.

**File > Export > Code clips** opens export settings. Choose separate raw
files, one labeled TXT, raw Csound clips in one ORC, Csound instruments in
one ORC, or a generated Csound project and score in one CSD. Raw exports can
include all clip sources or filter to Csound or VST CAW code. A fresh `CAW code
export` subfolder is created each time, so previous exports are not replaced.
Generated instruments can depend on CAW helpers, project UDOs and external
files. A CSD can reference external files; VST processing requires CAW's host
and does not run as a standalone Csound plugin. SuperCollider/Pure Data
backends are not implemented; their placeholder tracks are omitted from the
CSD and no native SC/PD project export is offered.

### Timeline loop

Right-click the ruler once at the loop start and again at the end. The range
is highlighted and Loop turns on. Use the toolbar Loop button to disable or
re-enable it, or right-click twice again to choose a new range. The range is
saved with the project. Play still requires Run Csound for sound.

The loop trims timeline events to the selected end and restarts transport at
the boundary. This first implementation uses UI-driven transport commands,
so it is **not sample-accurate or guaranteed gapless**; latency and very
short loops can cause a gap. It must be tested with real audio and VST plugins.
Loop playback does not change offline audio or score export.

## Autosave

Enable it under **Settings > General**, and set the interval (1 to 120
minutes, default 5). It writes a separate `.autosave.caw` recovery file next
to a saved project and does not overwrite the original or mark it saved.
Unsaved projects use a uniquely named file under `CAW Autosaves` in the user's
home directory. Open the recovery file through File > Open project.
Autosave preferences are saved per project. Save the project after changing
them to retain the setting.

## Clip Q

Select at least two code, VST, and/or MIDI clips with Ctrl/Shift-click or a
marquee, then press **Q** or choose **Q** in either clip's right-click menu.
Q aligns their start times to the earliest selected start. Durations and MIDI
notes relative to their clip stay unchanged; code/VST automation moves with
its clip. Undo restores the change. There is no timing grid.

In the single- and multi-clip MIDI editors, **Q** aligns selected note starts.
The multi-clip editor keeps notes inside their original clip start boundaries.

## MIDI clips

- The vertical pitch-scroll slider has the same travel length as the zoom
  slider. Both single and multi MIDI editors use it.
- Click **M** on a MIDI clip or in its editor to mute that clip, independently
  of other clips. During live playback, already sounding notes finish; future
  events are updated, matching the application's existing live-edit behavior.
- Edit **Name** to rename the MIDI clip without changing its instrument.
  Edit **Instrument** separately to route notes to an existing instrument.
- Right-click a Csound code clip and choose **Add MIDI clip** to create a
  MIDI clip with its name, start and duration. The MIDI lane is revealed.
- Enabling the MIDI button in the code editor creates a MIDI clip if one does
  not already exist for that clip. This also works for VST code clips. It does
  not create duplicate regions on repeated enables.

Project format 51 also saves the loop range. It retains the interface font,
FFmpeg path and exposed VST parameter snapshots introduced in format 50.
Older CAW builds may not open newly saved projects.
Keep a backup before saving an existing project in this version.

## Checks

From this folder, with a C++17 compiler:

```
g++ -std=c++17 -ffunction-sections -fdata-sections tests/q_midi_test.cpp src/csd_builder.cpp src/project_io.cpp -Wl,--gc-sections -o q_midi_test
./q_midi_test
g++ -std=c++17 -ffunction-sections -fdata-sections tests/export_test.cpp src/csd_builder.cpp src/project_io.cpp -Wl,--gc-sections -o export_test
./export_test
g++ -std=c++17 -pthread tests/process_test.cpp src/process_runner.cpp src/platform.cpp -o process_test
./process_test
g++ -std=c++17 tests/wav_test.cpp -o wav_test
./wav_test /tmp/caw-wav-test
g++ -std=c++17 tests/channels_test.cpp -o channels_test
./channels_test
g++ -std=c++17 -Itests/stubs tests/arrows_test.cpp -o arrows_test
./arrows_test
g++ -std=c++17 -pthread tests/codec_test.cpp -ldl -o codec_test
./codec_test /tmp/caw-codec-test
g++ -std=c++17 -ffunction-sections -fdata-sections tests/score_export_test.cpp src/score_svg.cpp src/csd_builder.cpp -Wl,--gc-sections -o score_export_test
./score_export_test /tmp/caw-score-test
mkdir -p /tmp/caw-fake
g++ -std=c++17 -shared -fPIC tests/fake_csound.cpp -o /tmp/caw-fake/libcsound64.so
g++ -std=c++17 -DCAW_VST3 -Itests/stubs -pthread tests/offline_test.cpp tests/fake_plugin.cpp src/csound_api_runner.cpp -ldl -o offline_test
./offline_test /tmp/caw-fake
g++ -std=c++17 -ffunction-sections -fdata-sections tests/legacy_midi_test.cpp src/csd_builder.cpp -Wl,--gc-sections -o legacy_midi_test
./legacy_midi_test
```

These command lines target Linux. The host test uses fake Csound and VST
implementations, not real Csound or a real plugin. The full GUI, native
dialogs, live output and real-plugin rendering still need end-to-end testing.

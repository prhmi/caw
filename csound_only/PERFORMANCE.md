# CAW performance audit and changes

The strongest recurring costs found in the source were repeated clip parsing,
full-project undo snapshots, and rebuilding the UI even when minimized. VST
processing also allocated four sample buffers for every plugin block. This
revision addresses those paths without changing audio block size, skipping
plugin processing, or putting blocking operations in the audio callback.

## Changes

- Windows: an engine-off, non-interacting window targets 10 fps; input wakes
  it promptly, while playback, transport, test and export stay at 60 fps.
  Linux/macOS keep the previous 20 fps idle target.
- A minimized window services the engine, transport, export completion,
  recording, plugin learn, undo history and autosave, but does not rebuild
  the entire invisible UI or submit a GPU frame.
- Undo snapshots run about once per second when idle, and retain their earlier
  0.25-second cadence during interaction. Undo and close still flush or check
  immediately. This is not yet true revision-based dirty tracking.
- Clip parsing caches the result of metadata scanning and VST translation,
  using the exact relevant source and settings as a key. The added cache has
  an approximate 16 MiB and 4,096-entry limit. Changed code, parameter ranges,
  MIDI, VST names, IDs, kind and picked names invalidate the entry.
- Arrangement drawing only parses a track for automation when at least one
  of its automation lanes is visible; combo item lists no longer allocate a
  temporary vector on every draw.
- CSD generation counts lines as it writes instead of copying and rescanning
  the growing output for each source map. Names are deduplicated with hash
  sets while preserving their original order; MIDI targets are indexed.
- VST input/output scratch buffers are preallocated at plugin preparation and
  reused for normal blocks. If an oversized block arrives, buffers grow rather
  than truncating it; that exceptional path can still allocate.

## What was tested

On one Linux sandbox, an optimized C++ microbenchmark of 10 tracks with 200
clips, parsed 50 times, took 20-24 ms after the change versus 431-447 ms
before, with the same checksum. This measures only that synthetic parsing
workload, not CAW's overall CPU, temperature, Windows performance or Reaper.

Generated CSD, source-line maps and timeline events matched the previous
implementation byte-for-byte in two synthetic projects, including VST,
automation, MIDI, engine and render modes. Twenty parser invalidation cases
also matched. The reusable VST buffer helper passed mono, stereo,
multichannel, silence, reuse and oversized-block tests under address and
undefined-behavior sanitizers. Project serialization and simulated VST
parameter-handoff tests passed.

The full GUI and real VST SDK could not be compiled or tested in that sandbox:
CMake and dependencies were unavailable, and it had no internet access.
Real Windows playback, plugins, minimize/restore, undo grouping, editor
interaction, responsiveness, CPU, GPU and temperatures therefore remain
unverified. Back up projects and test those before relying on this build.

## Remaining high-value work

1. Profile actual Windows idle and active projects. Compare the same sample
   rate, buffer size, plugins and visible windows. Record CAW and child-process
   CPU, GPU usage, UI frame time, audio underruns and temperature over time.
   Include external Csound and FFmpeg processes in the measurement.
2. Replace repeated full-project undo snapshots with explicit, comprehensive
   model revisions. Every mutation must participate, including plugin edits,
   autosave and undo/redo. A partial dirty flag risks losing changes.
3. Virtualize long console logs and clip off-screen automation geometry. Cache
   score-map and analyzer geometry only with correct invalidation. Avoid moving
   GUI/FFT work or heap allocations onto the audio callback.
4. Open code editors still copy their full text each frame. Use an editor
   change notification only after testing shared-code synchronization, text
   undo, external edits and the supported editor versions.
5. VST plugin-private state is still not saved: exposed parameter snapshots
   are not a substitute for complete presets and samples.
6. The `monitorFeed` project setting is not currently serialized, so it
   resets after reopening. Fix this separately with a versioned migration.

## Before adding SuperCollider and Pure Data

The existing SC/PD tracks are placeholders, not working backends. Do not run
extra engines at startup or send their code through the Csound parser. Define
separate backend lifecycle, event scheduling, routing, saved payload and
capability interfaces; launch an engine only when the project uses it.

`caw_bus.h` rings are single-producer/single-consumer and its audio buses are
not safe for concurrent writers. Give each producer its own bounded queue and
mix on one designated thread; handle queue overflow explicitly. Preserve
backend-specific project data with a versioned migration before implementing
the integrations. Validate synchronization, export, failure handling and
mixed-engine CPU/latency on real projects before promising performance.

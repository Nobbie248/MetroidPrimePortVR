# Chozo Ruins throughput investigation (2026-10-04)

PC reproduction: standing at the first doorway, looking into the open area.
The starting executable was `e8818f888256-dirty`, including the existing,
uncommitted renderer optimizations and detailed FIFO counters. Settings included
90 Hz OpenXR pacing, pipelined rendering, left-eye mirror, render scale 1 and
diagnostics enabled. The working tree was preserved.

## What the counters measure

The screenshots report 1023.1 FPS facing a wall and 208.1 FPS facing the room.
Those are reciprocals of about 0.98 ms and 4.81 ms of **producer loop elapsed
time**, excluding the OpenXR frame-request wait. They are not delivered frame
rates or GPU timings. Both screenshots show 89.9 presented FPS.

`PortDebug::RecordFrame` receives `workEndNs - loopStartNs` in
`src/MetroidPrime/main.cpp`. The update/world/HUD timers cover only selected game
functions. Aurora's `end_frame()` then drains the graphics FIFO; this wait falls
outside the displayed world/HUD timings. In pipelined mode, render-worker work
can also continue after the measured producer loop, so its reciprocal is not a
measurement of sustainable whole-renderer FPS.

## Observations from the live PC log

Typical steady open-area samples before the change:

| Work | Time per frame |
| --- | ---: |
| FIFO processing | 4.3-4.6 ms |
| Game waiting for FIFO | 3.5-3.7 ms |
| Render-worker encoding | 3.7-3.9 ms |
| Queue Submit call | 1.8-1.9 ms |
| Texture binding, within FIFO processing | 1.3-1.4 ms |

Do not add these as independent frame phases: FIFO work overlaps the game, and
the game wait represents time spent waiting for that same work. Queue Submit is
CPU elapsed time in the API call, not measured GPU execution time.

The view recorded 1,948 draws, including 1,868 world draws, plus about 16,544
primitives already merged into draws. Texture binding performed about 1,761
updates with 538-539 misses in the compact descriptor cache per frame. No texture
upload traffic appeared in these steady samples. OpenXR reported 90 new frames
per second with no repeated or skipped frames in this PC capture.

## Change and validation

The compact binding cache in `gx/command_processor.cpp` was being emptied by
`clear_draw_cache()` at every frame end. It now survives frame boundaries:

- Frame-local vertex/uniform state is still cleared.
- Entries are validated/touched in the GPU resource cache on first use in each
  frame; expired bindings are rebuilt.
- Keys use resolved texture views and sampler policy, without the global texture
  invalidation generation. Texture resolution still handles invalidations before
  looking up a binding. An unrelated EFB/palette invalidation therefore need not
  discard bindings whose actual descriptors have not changed.
- Multiview eye-copy epoch checks remain in place.
- FIFO initialization/shutdown reset the full cache, including device-dependent
  layouts and bindings.

The user's repeated doorway view looked correct. It contained about 1,952 draws
and slightly more geometry than the original capture, so this was a comparable
view, not an identical replay. Steady samples showed about 232 cache misses,
1.14-1.19 ms of texture binding, and 4.15-4.29 ms of FIFO processing. This is a
modest improvement and does **not** resolve the general room throughput gap.

The user then disabled `[xr-diag] logging` without moving the view and reported
throughput near **260 FPS**. This is not a cache-only speedup measurement: the
diagnostics themselves instrument tens of thousands of operations per frame.
Use logging for attribution, then disable it for normal throughput comparisons.

Validation: 211 FIFO tests passed, including six new cache lifetime/invalidation
tests; the existing timing and VR stereo tests passed; the Windows VR executable
linked and was tested in the scene. The Quest `aurora_gx` target compiled with
NDK 29. This turn did not produce/install a new Quest APK or verify Quest scene
performance.

Local evidence is in `build/chozo-investigation/`: `before.log`,
`after-cache.log`, `gx-fifo-tests.log`, and `quest-compile.log`. A Windows WPR CPU
trace was attempted, but the OS rejected it with `0xc5585011` (profiling policy
could not be enabled); no trace was recorded.

Further optimization should profile the remaining FIFO binding work and Dawn
encoding/submission, with diagnostics disabled for the final A/B measurement.
The current evidence identifies a CPU graphics-processing bottleneck; it does
not establish GPU execution time or prove that all graphics-thread elapsed time
is computation rather than synchronization.

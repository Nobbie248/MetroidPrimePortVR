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

## Quest: aligned vertices and material grouping (2026-10-05)

The Quest bottleneck is different from the PC producer-loop timing above.
The user measured these results at the Chozo entrance with a fixed headset pose:

| Configuration | Draws | GPU/frame | Presented FPS / 72 |
| --- | ---: | ---: | ---: |
| Original de-indexing (incorrect textures) | 2,455 | 26.9 ms | 35 |
| Aligned de-indexing | 2,317 | 21.8 ms | 43 |
| Aligned de-indexing and material grouping | 1,209 | 14.5 ms | 64 |

`229bc15f` aligns each non-byte-index attribute to four bytes and rounds the
de-indexed vertex stride to four bytes. The earlier packed records let float
loads straddle storage-buffer words; the Quest rendered those records wrongly.
The user isolated this independently of grouping. `9f732ff0` groups eligible
solid world surfaces by material and merges draws whose final state matches.
The grouping was first an opt-in experiment (`MP_SORT_OPAQUE=1`); it is now on
by default, and `MP_SORT_OPAQUE=0` (also a Quest launch extra) turns it off.
These fixes remain in place.

The saved `quest/.runs/chozo-align_sort/logcat.txt` contains 613 world, 570
head-locked and 26 sky draws. It shows two replayed EFB passes and one per-eye
EFB copy, approximately 9.0 ms FIFO processing and 14.5 ms app GPU time.
The compositor reports one layer and about 0.64 ms timewarp. A 72 Hz display
period is 13.89 ms; app GPU time alone therefore still exceeds that period.

A subsequent opt-in test sorted surfaces near-first within each material and
ordered material groups by their nearest surface. At the same 1,209 draws and
640 MHz GPU clock, 22 baseline samples averaged 15.05 ms / 60.09 FPS; 31
near-first samples averaged 14.91 ms / 60.23 FPS. Both runs had two compositor
layers and about 1.01 ms timewarp, so they must not be compared directly with
the earlier 64 FPS result. This small difference did not establish a useful
gain. The test code was removed and the aligned/grouped APK restored.

Disabling the existing minimap option reduced recorded draws from 1,209 to
716 (head-locked 570 to 77). The first run stopped replaying eye passes, so its
reported FPS/GPU time is invalid for performance attribution. Guardian then
blocked a repeat. With the user's authorization, setting
`debug.oculus.guardian_pause=1` (previously 0) and broadcasting
`com.oculus.vrpowermanager.prox_close` allowed clean, fixed-pose A/B/A runs:

| Configuration | Steady samples | GPU/frame | GPU clock | Presented FPS / 72 |
| --- | ---: | ---: | ---: | ---: |
| Minimap on | 40 | 14.34 ms | 640 MHz | 63.8 |
| Minimap hidden | 44 | 10.52 ms | 599 MHz | 72 |
| Minimap on again | 78 | 14.50 ms | 640 MHz | 63.5 |

All three used one compositor layer, approximately 0.64 ms timewarp, render
scale 0.85, and two replayed EFB passes. Automatic clock scaling lowered the
GPU clock with the minimap hidden, so this is not a fixed-clock measurement
of the minimap's isolated cost. Nevertheless, hiding it reaches native refresh
even at the lower clock; restoring it reproduces the deficit. The small
one-second FPS counter fluctuations include samples of 73 at a 72 Hz target.

The minimap's 493 recorded draws are the next optimization target. Its
translucent fills and outlines are interleaved in depth order, so reordering
all fills before all outlines is not equivalent. Batching must preserve that
order; an offscreen approach must also preserve alpha, the depth mask, and
stereo placement. Original settings and the minimap were restored after the
comparison; hiding it is a diagnostic, not an implemented rendering fix.
Afterward, `debug.oculus.guardian_pause` was restored to 0 and `prox_far`
cleared the proximity override; `GET_PROPERTY` confirmed
`disable_guardian=false` and `set_proximity_close=false`.

Local evidence for these follow-up tests is in `build/chozo-final/`:
`material-baseline.txt` (PID 30543, steady samples after 14:48:00), `front.txt`
(PID 30951, after 14:49:24), `front-experiment.patch`, and
`no-minimap-invalid.txt`. `aligned-grouping.apk` is the restored baseline.
The valid A/B/A evidence is `clean-baseline.txt` (PID 32518, after 14:56:25),
`clean-no-minimap.txt` (PID 317, after 14:57:40),
`clean-baseline-repeat.txt` (PID 1448, after 14:59:00), and
`clean-comparison.csv`.

## Ordered minimap batch (2026-10-05)

The dedicated HUD minimap path batches ordinary room surfaces in the existing
sorted order. Each surface still contributes its fill followed by its outlines,
including the two alpha-modulated passes for thick outlines. Colours and logical
line widths are vertex attributes, so changing a surface colour does not split
the draw. Area transforms are applied on the CPU; Aurora expands each outline
after the eye projection, using the same quad corners and viewport scaling as
its original GX line shader. Fill culling is retained in the fragment shader;
outline quads stay uncullable. The existing minimap depth mask and blend state
remain in use.

The vertex record is 36 bytes, with every float on a four-byte boundary. A new
ordered `GX_AURORA_MAP_BATCH` opcode selects the shader variant and is reset
after drawing. Vertex format 7 is saved/restored, and the normal map material
is restored before doors, icons, or pickup dots. Those objects retain their
original rendering path. Unsupported room primitives fall back after flushing
the batch. Interactive full-map rendering also retains its existing path.
The batch is enabled by default; `MP_MINIMAP_BATCH=0` disables it. Quest accepts
that flag as an activity launch extra for comparisons.

The release APK was built and installed, and slot 1 was loaded automatically
with `MP_SORT_OPAQUE=1`. The following comparisons kept a fixed pose within each
pair, render scale 0.85, and one compositor layer. Both runs used a 640 MHz GPU
clock and approximately 0.65 ms timewarp. With diagnostics enabled, both had
two replayed EFB passes and one EFB copy taken per eye:

| Configuration | Samples | GPU/frame | Presented FPS / 72 |
| --- | ---: | ---: | ---: |
| Batch off, diagnostics on | 25 | 14.34 ms | 63.8 |
| Batch on, diagnostics on | 21 | 11.73 ms | 70.9 |
| Batch off, diagnostics off | 55 | 14.33 ms | 63.9 |
| Batch on, diagnostics off | 36 | 11.69 ms | 72 |

The final pair reduces GPU time by 2.64 ms (18.4%) and reaches native refresh
with the minimap visible. The one-second FPS counter averages 72.47 in the last
run because some samples report 73 at a 72 Hz target. Head-locked draw counts
fell from 570 to 263: 307 fewer draws. Total draws fell from 1,214 to 904; world
draw counts differed slightly (618 versus 615), with sky draws unchanged at 26.
FIFO time fell from about 9.02 to 8.13 ms with diagnostics on. The batch increases
vertex uploads from 4,015 to 4,291 KB and reduces uniform uploads from 2,349 to
1,777 KB; the extra vertices are the explicitly expanded outline quads.

The user confirmed minimap colours/transparency, outlines, doors and icons in
both eyes and a successful transition into and out of the full map. No shader
validation failures were logged. Windows VR and Quest release builds passed;
224 FIFO tests, 14 renderer tests and the standalone minimap geometry checks
passed. Regression checks cover mode boundaries, aligned attribute offsets,
triangle winding, outline segment separation, inherited/quantized widths,
fill/outline ordering and all mono/stereo shader modes.

Evidence is in `build/chozo-final/`: `minimap-off.txt` (PID 6620,
15:56:57-15:57:21), `minimap-on.txt` (PID 6935, 15:57:41-15:58:01),
`minimap-off-normal.txt` (PID 7429, 15:59:55-16:00:49) and
`minimap-on-normal.txt` (PID 7839, 16:01:10-16:01:45). Each has a matching CSV;
`measure-minimap.ps1` reproduces the sample selection. The installed APK is
`minimap-batch.apk`. `MP_FRAME_STATS` was omitted from the final run. Settings
remain `vr_immersive_replay=1`; Guardian and proximity overrides were restored
and `GET_PROPERTY` confirmed both disabled.

## Conservative static surface culling (2026-10-05)

Static opaque and alpha world surfaces now get an individual bounding-box
visibility test after the existing model/PVS checks. Opaque surfaces are tested
before material grouping (also when grouping is disabled), and alpha surfaces
before insertion into the sorted buckets. The minimap room/door batches are
unchanged. This is frustum culling, not occlusion or back-face culling.

In immersive VR a surface is retained if its box intersects the original draw
frustum **or either eye's expanded frustum**. Each eye uses its located position,
orientation and asymmetric FOV, including the tracking base, world scale and
lean-back transform. Each frustum has a 7.5-degree angular margin and a 10 cm
translation margin; no extra far-plane cutoff is introduced. Unknown tracking,
invalid FOV or disabled VR frustum culling disables the additional rejection.
Keeping the original draw frustum also preserves offscreen capture views.

Missing or invalid surface bounds are kept: retail surfaces without an extra
bounds block return only their centre from `GetBounds()`, which cannot safely
stand in for an enclosing box. Reflection and PBR materials are kept to preserve
copy/probe side effects and potentially displaced vertices. Area/shadow draws,
dynamic actors and wireframe rendering keep their existing paths.

The feature is enabled by default; `MP_SURFACE_CULL=0` disables it. Quest accepts
that environment override as an activity launch extra, enabling same-APK A/B
comparisons. With `MP_FRAME_STATS` set, `[surface-cull]` logs per-frame considered,
rejected, missing/invalid-bound and special-material counts every 600 frames.
Omit `MP_FRAME_STATS` for final performance measurements.

Windows VR and Quest release builds passed. The standalone
`port_surface_culling_tests` covers boxes crossing a plane, a large wall whose
corners all fall outside the view, asymmetric FOV, geometry seen by only one
eye, camera rotation/translation, head-motion margins and invalid inputs. It
also checks 10,000 boxes containing points visible under an independent
projection calculation. Target GPU measurements and headset visual checks are
pending; evidence for this change is stored in `build/chozo-culling/`. The APK
was installed, but Quest's controller-required launch dialog prevented the
game from starting. No GPU gain is measured yet. Guardian and proximity
overrides were restored; `GET_PROPERTY` confirmed both disabled.

## Fixed foveated rendering (2026-10-06)

Wiicompiled VR's foveation ported (see `PORT_NOTES.md`, "VR: fixed foveated
rendering on the Quest"): Aurora builds an RG8 fragment density map per eye and
PrimedGun's patched Dawn attaches it to the eye passes through
`VK_EXT_fragment_density_map` on dynamic rendering. Setting `vr_foveation`
(`off`, `low`, `medium`, `high`; default `off`, see below); `MP_FOVEATION`
overrides it for one run and is accepted as a launch extra.

What to expect before measuring: at render scale 0.85 the plaza is per-draw
bound (scale 0.6 against 1.0 moved App GPU only 27.7 to 30.3 ms above), so a
density map saves little there. Wiicompiled measured no gain at 0.8 and 8 / 14 /
22 % at 1.3. The gain to look for is at a higher render scale, where the saved
pixels pay for sharper eyes at the same frame time.

Method: the interleaved A/B of the sections above (`quest_ab.sh` style, 11 s
windows, same GPU clock, `VrApi` `App=` of the game's PID, `LCnt=1` samples,
`MP_FRAME_STATS` off for the final numbers). Change the level between windows
from the F1 VR tab or `MP_FOVEATION=<0-3>` per launch (off to a level needs a
launch, since the device decides). Check the image too (`quest_shot.sh`): at
`high` the periphery shows 4x4 blocks and the centre stays sharp; at `low` the
HUD digits, the minimap and the visor frame stay readable. Meta's `Fov=` field
reports the runtime's own foveation and stays 0 here.

Measured 2026-10-06 at the plaza (headset on the desk, so the pose is fixed;
whole sessions, 30 s windows after a 10 s settle; `App=` and the GPU clock from
the game PID's `VrApi` lines, DVFS having moved between 545 and 640 MHz):

| Run | Level | Scale | App GPU ms | GPU MHz |
| --- | --- | ---: | ---: | ---: |
| off085a | off | 0.85 | 9.72 | 599 |
| low085a | low | 0.85 | 10.46 | 599 |
| low085b | low | 0.85 | 10.42 | 599 |
| off085b | off | 0.85 | 10.45 | 545 |
| off125 | off | 1.25 | 11.41 | 640 |
| low125 | low | 1.25 | 12.12 | 640 |
| med125 | medium | 1.25 | 12.21 | 640 |
| high125 | high | 1.25 | 11.89 | 640 |

A second series split the cost: `MP_FDM_DEVICE=1` gives the device the
extension and the pipeline flag with the level off, `MP_FOVEATION_LAYERS=1`
binds one shared map (each texel the finer of the two eyes') instead of the
two-layer per-eye map.

| Run | Configuration | App GPU ms | GPU MHz |
| --- | --- | ---: | ---: |
| off085c | off | 9.95 | 561 |
| off085d | off | 10.33 | 545 |
| flag085a | device flag only | 10.34 | 568 |
| flag085b | device flag only | 9.91 | 599 |
| low1l085a | low, one shared map | 10.19 | 599 |
| low1l085b | low, one shared map | 10.17 | 599 |
| low085c | low, per-eye 2-layer map | 10.39 | 599 |
| low085d | low, per-eye 2-layer map | 10.41 | 603 |

At 599 MHz: off 9.7, the pipeline flag alone 9.9, one shared map 10.2, the
per-eye map 10.4 ms. The density map costs about 0.7 ms (7 %) and saves nothing,
at 1.25 as at 0.85: the eye pass is bound by per-draw vertex fetch and binning,
not by fragment shading, and a map on a non-subsampled image still stores full
resolution tiles. The High screenshot (`build/foveation/quest_fov_high.jpg`)
shows the coarse shading really applied (4x4 blocks on the visor frame and the
walls, a sharp centre), so the mechanism works; the GPU has no pixel work to
save here. The default is therefore `off`. Every replayed eye pass of a frame is
foveated when it is on, splits included (passes are sealed for the render
worker while the frame is still being recorded, so Wiicompiled's "single pass
only" rule is not available at seal time); fill-heavy areas (water, fog, heat,
snow) and split-heavy frames (thermal visor, a held charge shot) remain to be
measured the same way before a level is worth recommending anywhere.

## Native vertex input experiment (2026-10-06)

The resident geometry cache avoids repeated CPU resolution and uploads, but
its vertex shaders still fetch byte-addressed storage data. The native
vertex input path (on by default; `MP_NATIVE_VERTICES=0` turns it off) uses
native vertex attributes for supported cached surfaces. The same resident buffer gains Vertex usage; offsets, strides,
absolute indices and batching remain unchanged. Numeric byte order is
converted once, on cache insertion. Unsupported layouts continue through
the resident storage path, and dynamic draws are unaffected.

`MP_FRAME_STATS=1` reports `native vertex input: N draws per frame` alongside
the existing draw and geometry-cache statistics. Both switches are accepted
as Quest activity string extras. A restart selects the input path.

Validation completed:

- Windows game and release Quest APK built; APK installed on the Quest 3.
- 235 FIFO tests, 19 renderer tests, 41 port tests passed, including cache
  conversion/reuse, unchanged merging, native/dynamic transitions, fallback,
  endian conversion, fixed-point formats and shader generation in all four
  mono/stereo modes.
- Desktop D3D12 real-disc Chozo boot with isolated settings: 37-38 native
  draws per steady frame, no shader validation errors. This is a functional
  GPU smoke test, not a substitute for Quest timing or headset visual checks.

Quest validation completed after the controller dialog was dismissed:

- Both settings produced 715 draws (600 world, 83 head-locked, 26 sky, 6
  skipped), the same two EFB passes and one eye replay, with foveation off.
- Native input covered 515 world draws per frame. Both had 1,744 geometry
  cache hits and zero steady-state misses; upload sizes were identical.
- No shader or pipeline validation errors. The user confirmed world textures,
  lighting, shadows and HUD looked correct in both eyes. Device screenshots
  (`native-input.png`, `storage-input.png`, `native-final.png`) agree; animated
  foliage and the gun are not at the same animation phase.

Timing method: same installed APK, slot 1, headset resting at a fixed pose,
scale 0.85 (1428x1496 per eye), 72 Hz, multiview, `MP_FOVEATION=0`. Launch
`org.primedgun.v2/.PrimedGunVrActivity` with `MP_BOOT_WORLD=83F6FF6F`,
`MP_LOAD_STATE=1`, and `MP_NATIVE_VERTICES=0` / `1`. The diagnostic extra
`MP_FRAME_STATS` was omitted entirely for timings. Each launch settled for
25 seconds before a 30-second window; only game-PID `VrApi` samples with
`LCnt=1`, scale 0.85 and target 72 Hz were accepted. Temporary
`debug.oculus.gpuLevel=5` and `debug.oculus.cpuLevel=3` overrides requested
consistent levels ([Meta system-property documentation](https://developers.meta.com/vr/documentation/native/android/ts-systemproperties/));
all accepted samples reported GPU 599 MHz. CPU and memory could still change,
so matching subsets were checked below. Guardian and proximity overrides
were used during profiling. All overrides were restored afterwards.

| Run, in order | Native input | Samples | App GPU ms, mean | Min-max ms | GPU MHz |
| --- | --- | ---: | ---: | ---: | ---: |
| storage-a | off | 31 | 9.593 | 9.51-9.79 | 599 |
| native-a | on | 31 | 5.188 | 4.88-5.73 | 599 |
| storage-b | off | 32 | 9.599 | 9.55-9.71 | 599 |
| native-b | on | 31 | 5.060 | 4.81-5.66 | 599 |

Combined: **9.596 -> 5.124 ms**, **4.472 ms / 46.6% less GPU time**.
Both paths sustained native 72 Hz; the benefit is GPU headroom. Matching CPU
1651 MHz and memory 2092 MHz samples gives 9.592 ms off (25 samples) against
5.142 ms on (39 samples), a 46.4% reduction. At CPU 1651 / memory 1708 MHz,
the corresponding means are 9.593 ms (31) and 5.034 ms (10). The gain persists
when those clocks match too.

Separate eight-second `ovrgpuprofiler --realtime` captures, taken outside the
timing windows, returned six samples each:

| Whole-device counter | Storage | Native |
| --- | ---: | ---: |
| Vertex fetch stall | 67.8% | 33.3% |
| Vertex instructions / second | 5.299 billion | 2.546 billion |
| Time shading vertices | 19.9% | 8.4% |
| Global memory load instructions | 10.065 million | 5.526 million |
| Global buffer data read bandwidth | 111.7 MB/s | 51.5 MB/s |

These counters include compositor work; they are attribution evidence, not
per-pass measurements. The instruction and buffer-read reductions support
removing vertex-side storage decoding as the source of the gain.

Evidence and capture/summary scripts are under `build/native-vertex/`
(ignored); `results.json` and `timing-samples.json` contain numeric summaries
independent of the local CSV number format. The running Quest test has native
input enabled. No Quest 2 frame rate is inferred from the Quest 3 result.

Follow-up (2026-10-06): native input is now on by default for play testing
(`MP_NATIVE_VERTICES=0` keeps the storage path). Review measurements with
`MP_FRAME_STATS=1` found the native path costing the FIFO thread +0.87 ms a
frame at the plaza (5.9 -> 7.0 ms; the game's wait on it +0.75 ms): every
cached display list, hits included, rebuilt a pipeline config, checked the
native layout and hashed its attributes. The decision now depends only on
the attribute inputs (vertex descriptor, the format's attribute formats,
array byte order, de-index and map-batch modes) and is memoized per
combination (`native_vertex_choice`, cleared by `reset_draw_cache` and the
geometry-cache tests). Each native draw also re-bound vertex buffer 0; the
pass now skips that bind when the buffer is already bound
(`bind_gx_native_vertices`). Alternating Quest runs (2 rounds, 40 s each):
storage 9.80 / 9.82 ms App GPU at 599 MHz, FIFO 5.94 / 5.86 ms; native
6.41 / 6.37 ms at 492 MHz, FIFO 6.35 / 6.39 ms. The FIFO overhead is down to
about +0.47 ms; what remains is likely the pipeline switching between native
cached draws and plain draws.

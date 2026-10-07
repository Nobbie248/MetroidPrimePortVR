## VR: the morph ball's HUD on the virtual screen (2026-10-07)

In the headset the morph ball's HUD sat at the edges of the view, half out
of sight, while the combat HUD worked. The combat HUD is drawn through a
perspective camera, so the head-locked route places it at its own angles. The
ball HUD's frame (`FRME_BallHud`) has an orthographic camera (PrimedGun's
classifier recognises it by its -3.2 left edge). An orthographic draw is
replayed unchanged in each eye, so it stretches over each eye's whole image.
Its energy bar and bomb gauges then land at the edges of the lenses, and in a
different spot in each eye because the eye frustums are asymmetric.

Aurora's reserved route `AURORA_STEREO_ROUTE_SCREEN_2D` now lays a draw's mono
picture on the virtual screen. The screen hangs `vr_screen_distance_meters`
ahead of the game camera and is `vr_screen_width_meters` wide, the same
settings as the menu and cinematic screen, with the picture's aspect. It is
Wiicompiled's "race 2D screen" (`HudScreen`): it stays in front of the
player, and the head can turn to look at its corners. Lean back and
recentring move it as they move the world.

- `stereo_replay::compose_screen_2d_projection` takes perspective draws as well
  as orthographic ones. It builds the screen point from the mono clip
  coordinates, (x * halfWidth, y * halfHeight, -distance * w, w), so the whole
  chain stays a single matrix. Depth uses the mono clip z times the eye depth
  of the screen's nearest corner. The draw's own NDC depth is therefore scaled
  by a factor of at most one. Every draw at a given point of the screen gets
  the same factor, so the layout keeps its depth order and its depth range,
  which keeps it in front of the world. A screen that faces the eye keeps the
  exact depth. A draw in a sub-viewport is remapped into the picture, and the
  remap is undone for the eye pass, which applies that viewport again.
- `aurora_set_stereo_screen_2d(width, distance, unitsPerMeter)` is read at
  frame begin. `PushVrSettingsToAurora` feeds it the screen settings and
  `vr_world_scale`. With no screen, the route draws like FULLSCREEN, as before.
- `CSamusHud::Draw` routes the ball frame and the base frame drawn with it
  (hint messages, counter) to the screen while `PortVrBallHudShown()` (the HUD
  state is `kHS_Ball`). `CInGameGuiManager::Draw` sends the minimap there too,
  so it keeps its place in the ball HUD's layout. The camera filters stay
  full-screen. The combat, scan, X-ray and thermal HUDs are unchanged.
- The stereo statistics line counts "screen 2D" draws.
- `port_vr_stereo_tests` checks the following: a screen facing the eye matches
  the head-locked plane; a perspective draw lands on the screen with its depth;
  on a turned screen, depth only shrinks, alike for every draw at a point and
  exact at the nearest corner; a sub-viewport draw keeps its place; and the
  eye uniform carries the projection. Picking the farthest corner instead of
  the nearest fails the depth check.

build/vr and build/nooxr pass 42/42 port tests, and a desktop boot to the
Landing Site runs clean. Confirmed in the headset (PSVR2, SteamVR): the ball
HUD shows on the screen, with 12 draws a frame on the route in morph ball and
none in first person.

The same headset session turned up two more things.

- **Region title cards.** On arrival in a region, a script billboard
  (`CScriptSpecialFunction` `kSF_Billboard`, which shows `TXTR_LavaBillboard`,
  `TXTR_ChozoRuinsBillboard` and the like from NoARAM.pak) sets
  `CStateManager`'s pending on-screen texture.
  `CInGameGuiManager::Draw` draws it with `CGraphics::Render2D`, which is
  orthographic, so it was stretched over each eye's image too. That draw now
  takes the screen route.
- **A message box looked like a freeze.** "Increased Pirate activity in
  Magmoor Caverns." waits for GameCube A. Because the player was still in
  first person and unmorphed, `vr_pad.cpp` kept the gameplay layout, where A
  is the weapon trigger and the controller's A does nothing. PrimedGun
  switched to its default controls through game flow hooks for the message,
  save, logbook, pause and map screens. Here
  `CInGameGuiManager::Update` tells the pad every tick whether one of its
  paused states (`kIGGS_MapScreen` to `kIGGS_PauseHUDMessage`) is the
  previous or the next state (`PortVr::VrNoteInGameMenu`), and the classic
  layout applies meanwhile. The diagnosis came from the thread stacks of the
  live process, read with dbghelp, and a capture of the mirror window. The game
  loop was running; its world pass was replaced by the pause blur and the box.

Confirmed in the headset: the Chozo Ruins title card shows on the screen (one
draw a frame on the route while it is up). The menu layout for message boxes
has not been tried on a message yet.

## Renderer: a full frame of vertices no longer aborts the game (2026-10-07)

A Quest play session crashed after a door load. The crash was a SIGABRT in
`ByteBuffer::append`, called from `push_verts` on the FIFO thread. A
virtual-screen transition had filled the frame's 5 MiB of vertex staging
(4.6 MiB used) before one large draw. That staging is mapped memory, which
cannot grow, so the append aborted.

- **Guard.** The FIFO processor now checks for room before it stages vertices
  or indices. A draw that does not fit is skipped and logged as "Frame staging
  full" (the first 20 such frames, then every 300th). New geometry-cache surfaces
  wait for a later frame when less than a quarter of the vertex staging would
  remain. `ByteBuffer::resize(0)` now keeps the capacity. Before, an empty
  append dropped it to 0, and the next append on mapped memory aborted.
- **Room.** A frame now holds at least 10 MiB of vertices
  (`MinVertexBufferSize`), so the transition fits and nothing is skipped. Mods
  with room geometry still scale from 5 MiB, so their buffers keep their
  sizes. `aurora_get_frame_buffer_scale` reports the scale it was given
  instead of inferring it from the vertex size. The cost is about 30 MiB of
  memory: 5 MiB in each of the five staging buffers and in the device buffer.

Validation: 236 FIFO tests pass, including a new one that fills a fixed
staging buffer. The 42 port tests pass. After the guard, a Quest walkthrough
through the loading zone that crashed did not crash. The log showed one draw
skipped during the transition frames; the 10 MiB floor is meant to remove
that, and the zone has not been walked through again since. Two same-sitting
5 MiB/10 MiB Quest runs at the Chozo plaza (scale 0.85, 72 Hz, 492 MHz) gave
App GPU 6.36/6.39 ms vs 6.39/6.40 ms. FIFO and encode times were equal, with
no staging warnings. App GPU varies 5.7-6.4 ms between sessions at the same
clocks, so compare builds in one sitting.

## VR: the VR menu's CONFIG and DEBUG tabs (2026-10-06)

Two tabs of the port's own join PrimedGun's six in the headset's VR menu:
CONFIG, next to LAYOUT, and DEBUG, last. Eight tabs do not fit PrimedGun's
150-pixel tabs, so each tab is as wide as its label plus 12 pixels a side. The
strip is centred in PrimedGun's.

The game's frame rate shows beside the title on every tab ("90 FPS"). For the
three seconds of SETTINGS SAVED, the notice takes its place. It
counts the game's own frames (`CGraphics::EndScene`) rather than using
`aurora_get_fps`, which counts the desktop window's presents. A Quest has no
window, so that read 0 there.

**CONFIG** holds the launcher's Port Config VR settings and the renderer
switches the F1 VR tab has. It spans two pages.

| Group | Rows |
|---|---|
| Both platforms | RENDER SCALE, EYE RESOLUTION (a readout), REFRESH RATE, WORLD SCALE, DRAW THE WORLD PER EYE, REMOVE CINEMATIC BARS, SKY AT INFINITY, SPACE WARP, SCAN WINDOW ZOOM, SCREEN DISTANCE, SCREEN WIDTH, LEAN BACK, PIPELINED RENDERING, INDEXED VERTICES ON CPU, LOOK TO SCAN, BEAM WHEEL HUD HIGHLIGHT, RESET CONFIG |
| Quest only | FOVEATION, PERFORMANCE LEVEL, PASSTHROUGH, MULTIVIEW EYES, DIRECT TO HEADSET |
| PC only | WINDOW SHOWS (the mirror) |

- EYE RESOLUTION shows the eye size the chosen scale gives (`OpenXRGetEyeResolution`).
- Choices get the value box's -/+, which stop at the ends; a click elsewhere on
  the row cycles on.
- Two rows are labelled "NEXT START": DIRECT TO HEADSET always, and FOVEATION
  when the session started without density maps.
- RESET CONFIG takes two clicks and leaves PrimedGun's settings alone.

**DEBUG** holds what of the F1 Debug tab works in the headset, plus live
readouts:
- FULL HEALTH, and GRANT EVERYTHING (two clicks). Both read "NO GAME" outside
  a game.
- INVULNERABLE, STREAMED AUDIO, MUSYX AUDIO, WRITE THE LOG TO A FILE, and XR
  DIAGNOSTICS LOG.
- Readouts: headset refresh, new frames to the headset, game frames per second,
  draws per frame. They are sampled twice a second, so the image is redrawn at
  most that often.
- The free camera and the modding tools need a pad and the desktop, and stay in
  F1.
- `PortDebug::CheatFullHealth` and `CheatGrantEverything` now hold the F1
  buttons' code, for both callers.

**Live from the menu.**
- Render scale changes reach the headset at once. `PushVrSettingsToAurora` now
  calls `OpenXRSetRenderScale`, and the backends rebuild the eyes at the new
  size.
- A changed performance level is asked for again on the pacing thread
  (`OpenXRReapplyPerformanceLevel`), not only at the next session.

**Tests.** `port_vr_menu_tests` adds the tab strip, both platforms' CONFIG rows
and pages, the steps, the choices, the readout and the reset. It also covers
DEBUG's cheats, two-press grant, switches and readouts.

## VR: PrimedGun's in-headset menu (2026-10-06)

PrimedGun's VR settings menu now opens in the headset, the way PrimedGun opened
it. The trigger is a click of the off hand's thumbstick (the left one unless
left-handed) or that hand's menu button. The other stick's click stays "SET
HEIGHT", as on the menu's own Layout page. The game keeps running, and the
controllers are withheld from it until everything is released after the menu
closes.

**The same image.** `platform/include/vr/vr_menu.h` is PrimedGun's
`PrimedGunOverlayCommon.h` raster: a 1024x512 canvas drawn on the CPU with its
5x7 bitmap font and solid rectangles that replace alpha. PrimedGun's colour
constants are kept unchanged. They read like amber hex (`0xE0FFB030`), but
PrimedGun uploaded the little-endian words into an RGBA8 swapchain, so the
headset showed `#30B0FF`. The port uploads the same bytes, so the menu is the
blue one players saw. The `?` of "ARE YOU SURE?" is still missing (the font has
no glyph for it) and the culling cone still reads "115.00".

**Placement and pointer.** The menu is a quad layer on the off hand's grip
(1.05 x 0.72 m, PrimedGun's offset), or 4 x 2 m latched 2.7 m ahead when
"DETACH VR MENU FROM HAND" is on. The cannon hand's laser (8 mm, warm yellow,
up to 8 m) and its 2 cm hit dot are two more quads. They are cut from a sprite
strip below the canvas in the same swapchain image (1024x560), so no backend
needed a new swapchain. PrimedGun laid the laser ribbon flat in the
controller's frame; here it turns about the ray to face the head, so it never
shows edge-on. The quads use straight alpha, as PrimedGun's did. The virtual
screen no longer leaves room for an in-eye panel, which nothing draws any more.

**Behaviour.** This is PrimedGun's `UpdateVrMenu`, `AdjustVrMenuSetting` and
`ActivateVrMenuSelection`:
- The row under the laser is selected.
- The trigger or A clicks. A numeric row's value box steps the value down on
  its left half and up on its right half.
- Resets, EXIT GAME and the save-state actions need a second click within six
  seconds.
- Every opening starts on LAYOUT.
- CALIBRATION and CONTROL turn pages with PREVIOUS and NEXT buttons under the
  rows, with the page number between them. This replaces PrimedGun's PAGE row
  at the top of the list (the user's request, after the first headset test).
- "LONGER HELD PRESS FOR VR MENU" (one second) and "MENU REQUIRES HAND NEAR
  HEAD" (the visor gesture's zone) work as in PrimedGun.

Changes apply live. SAVE SETTINGS writes `port_settings.ini` now; otherwise the
file is written whenever the port next saves. EXIT GAME saves and quits the way
the F1 overlay's Exit game does.

**Port mappings.**
- STATES drives `PortSaveState`. It shows 8 slots, PrimedGun's Dolphin had 10.
- LOAD NEWEST loads the slot saved last. SAVE OLDEST saves into the first empty
  slot, else the one saved longest ago.
- TEXTURES applies the launcher's cannon slots (`launcher/core/cannon_textures`,
  now linked into the game) and reloads the user texture pack.

Thirteen rows change settings the port saves but does not read yet. These are
the launcher's "not active yet" keys:
- Calibration tab: VISOR HELMET, HEIGHT PROMPT, FLOOR POSITION MARKER, HUD
  VERTICAL and HUD HORIZONTAL.
- Movement tab: LEFT STICK STRAFE, MOVEMENT DIRECTION, MOVEMENT DEADZONE,
  MOVEMENT SPEED, MOVEMENT ACCELERATION, AIR ACCELERATION and SNAP TURN ANGLE.

**Threads.**
- The XR pacing thread opens and closes the menu (`settings_panel::Controls`).
  It also places the menu and aims the laser (`OpenXRInput::PlaceMenu`), and
  publishes the hit point with a click counter, so a click survives a
  game-thread stall.
- The game thread runs the menu from `CGraphics::EndScene`
  (`PortVr::VrMenuUpdate`, `platform/vr/vr_menu.cpp`). It rasterises the image
  only when its text changes, then hands it over with
  `aurora_set_stereo_panel_image`.
- Aurora's frame worker uploads the image into the panel layer's image
  (`stereo_overlay::layer_source`), swizzled for a BGRA swapchain.

**Settings and tests.**
- The menu's own settings (`vr_vr_overlays_enabled`, `vr_vr_menu_floating`,
  `vr_vr_menu_hold_left_stick` and `vr_vr_menu_requires_head_zone`) lost their
  "not active yet" tag.
- They are also in the F1 VR tab, with "Show it in the headset now".
- `port_vr_menu_tests` covers the image's pixels and byte order, the hit boxes,
  the two-press actions, the panel, laser and dot poses, and the open/close
  rules.

**Headset result** (Quest 3, direct presentation, 32-minute session): the user
confirmed the look, the pointing, live changes and the game's input while the
menu is open. The log shows `OpenXR VR menu layer ready` and no menu errors.
The PC (D3D12) path is not tested in a headset yet.

## VR: the PrimedGun launcher on the Quest (2026-10-06)

The Quest APK now opens on PrimedGun's launcher, a 2D Horizon OS panel. The
game, `PrimedGunVrActivity`, is no longer in the library; the panel's Play
starts it. The panel is PrimedGun's Quest launcher (its Kotlin rows, layouts,
palette and strings) with the PC launcher's tabs and keys: Setup, Controller,
Calibration, Cannon Textures, Layout, Port Config and About. Settings the game
saves but does not read yet carry the same "not active yet" tag as on the PC.

**One core for both launchers.** `launcher/jni` builds `libprimedgun_launcher.so`
(`-DMP_BUILD_QUEST_LAUNCHER=ON`) from `launcher/core`. The panel therefore edits
`port_settings.ini` with the PC launcher's key table, `SettingsModel` and
line-preserving file editor. It applies cannon slots and reads `PrimedGun.ini`
with the same code. Only strings, numbers and arrays cross JNI. The library has
no SDL or Aurora.

**Game process.** The game keeps its own `:game` process.
- Play saves pending edits, re-applies a cannon slot whose files went missing,
  and starts the game.
- The tabs lock while the process lives, because the game rewrites the whole
  file as it exits. The file is read again once the process is gone.
- Stop sends a package-scoped broadcast that finishes the game, as Quit does.
  Pressing Stop again after 10 s ends the process.
- `last_error.txt` (why a start failed) is shown when the panel returns.

**Select Game** copies the picked image to `<user>/disc.iso` in a `dataSync`
foreground service. The game process cannot open the picker's document.
- The PC launcher's disc check runs on the image's first 0x8008 bytes before
  the copy. The extension is lower-cased, and a WBFS image is known by its
  magic. A WBFS image is checked by the header copy of disc slot 0, which sits
  at its second hard-disk sector.
- The copy goes to `disc.iso.part`, which must be as long as the provider says.
  It must also pass the game's own check (`QuestStorage.checkDisc`) before it
  replaces the disc in use.
- A copy's result is kept until the Setup tab has shown it.
- A `.part` left by a killed process is removed at the next start.

**Memory card transfer.** On a headset the player picks a card (`.raw`, `.gcp`,
`.gci`), a `PrimedGun.ini`, or the zip that PrimedGun's Export User Data writes.
- **From a zip:** the raw card comes first (`MemoryCardA.USA[.<blocks>].raw`), as
  the PC search ranks it. The GCI folder is used only when it holds Metroid
  Prime saves. Entry names without the UTF-8 flag are read as Latin-1.
- **The hand-over:** the panel cannot read a raw card, because `port_gci.cpp`
  uses Aurora's card code, which is built on SDL.
  - The panel copies a set into `primedgun/pending_import.tmp` and renames it
    to `pending_import`.
  - At boot, before the card mounts, `PortGci::ImportPending` claims the folder
    by renaming it to `pending_import.claimed`. The import reads only that
    claimed set. A set staged meanwhile waits for the next start.
  - The `.gci` files form one save set, in any case of extension; each raw image
    is one set too.
  - An imported claim is renamed to `.done` before it is deleted. A failed
    cleanup therefore never re-imports old saves over newer progress. A claim
    left by a crash is taken up again.
  - The import writes `primedgun/import_report.txt`, which the panel shows.
- **Staging** runs at process level, like the disc copy. Play is refused until
  it ends.
- **Old settings** become unsaved edits, as on the PC. They are saved at once
  if the panel is no longer in front.

**Port Config on the Quest** leaves out VR on/off, the mirror, fullscreen and
VSync. It adds the refresh rate, the performance level, passthrough,
foveation, and the renderer switches the PC reaches through F1 (multiview,
direct presentation, pipelined rendering, indexed vertices, `[xr-diag]`).

**Game fix found on the way.** `vr_passthrough` never reached the OpenXR session.
Its copy was taken when the session object was built, which can be before the
file's line is read, and nothing called `OpenXRSetPassthrough`.
`PushVrSettingsToAurora` now passes it on with the refresh rate and lean-back.

Validation:
- **Review.** Three rounds of review, each finding checked by an independent
  skeptic. They found 26 defects, all fixed here: a recursion crash in the
  Setup tab, the passthrough setting, the zip card priority, a disc copy that
  could replace a good disc, the hand-over's atomicity and others.
- **PC tests.** build/vr passes 41/41 port tests, with new cases for the
  hand-over (A and B together, `.GCI`, an unfinished claim) and for WBFS.
- **Core tests on the headset.** `port_launcher_tests`, cross-compiled with the
  NDK, passes on a Quest 3.
- **Quest 3, driven over adb.**
  - All seven tabs open.
  - The "not active yet" tags match the key table.
  - Save writes only the changed key into the VR block.
  - Cannon Slot 1 and Default apply.
  - Play then boots the game, which imports a staged card through the claim.
  - The panel brought forward is locked; Stop ends the game in about 1 s and the
    panel unlocks.
- **Not tested on the headset.** Select Game and Transfer open the system file
  picker, which adb cannot drive.

## Renderer: native vertex input experiment (2026-10-06)

Native vertex input makes supported resident world geometry use the GPU's
vertex-input stage instead of loading and decoding its attributes from a
storage buffer in the vertex shader. It is on by default for play testing;
`MP_NATIVE_VERTICES=0` (also a Quest launch extra) keeps the storage path.
Wider-area and Quest 2 validation are still pending.

The geometry cache converts numeric components to little-endian once on a
miss. Attribute offsets, aligned record stride, absolute indices, surface
batching and draw order stay the same. Native float/integer formats and RGBA8
colours preserve the existing fixed-point scaling. Matrix-index bytes are
extracted from aligned integer attributes. Unsupported layouts (including NBT
and packed colours) retain the resident storage path; dynamic geometry, lines
and the map batch retain their existing paths. Cache keys distinguish the two
encodings. Mono, per-eye and multiview pipelines all support native input.

Validation: Windows and Quest builds, 235 FIFO tests, 19 renderer tests and
41 port tests pass. A desktop D3D12 Chozo run exercised 37-38 native draws per
steady frame without shader validation errors. The installed Quest 3 build
passed a visual check in both eyes at the saved Chozo plaza. Same-APK
off/on/off/on runs at scale 0.85 and 599 MHz averaged **9.60 ms -> 5.12 ms App
GPU time (46.6% less)**, with the same 715 draws (600 world); 515 world draws
used native input. Vertex instructions fell about 52% and the reported vertex
fetch stall metric fell from 67.8% to 33.3%. All runs sustained 72 Hz.
Detailed logs, matched CPU/memory-clock subsets and screenshots are described
in `docs/CHOZO_PERFORMANCE.md`. Quest 2 and area transitions remain untested.
Temporary device overrides were restored.

## VR: fixed foveated rendering on the Quest (2026-10-06)

The Quest's GPU shades every pixel of each eye at full rate although the
headset's lenses blur the periphery. Wiicompiled VR shipped fixed foveated
rendering for the same renderer, and that design is ported here.

**Not `XR_FB_foveation`.** The runtime's density maps only shape render passes
that draw into its swapchain images, and the eyes are drawn into Aurora's own
targets and then blitted (direct presentation) or copied (AHardwareBuffer
bridge) into the swapchain. So the map has to go on Aurora's eye passes, which
stock Dawn cannot do.

- **Dawn patch** (`quest/dawn/aurora_fdm.{h,inc}`, applied by `apply.py`;
  `extern/aurora/include/aurora/dawn_fdm_abi.h` is the C ABI, version 2):
  `VK_EXT_fragment_density_map` on Dawn's dynamic rendering path. Aurora
  uploads an immutable RG8 map (raw `VkImage`, `FRAGMENT_DENSITY_MAP_OPTIMAL`,
  read by the driver on the CPU when the pass is recorded, so it is usable only
  once its upload has completed) and binds it to a texture view;
  `RecordBeginDynamicRenderPass` chains the bound map into every pass whose
  first colour attachment is that view. The extension is requested before the
  device is created and then flags every render pipeline
  (`VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT`), so
  the choice is made per launch and the first launch with it on recompiles the
  pipeline cache once. Version 2 adds layered maps: under multiview the eye
  pass renders both eyes into one two-layer array view, and Vulkan takes a
  density map with one layer per view (layer = view index).
- **Aurora**: `lib/webgpu/fdm.{hpp,cpp}` wraps the ABI (stubs on every other
  Dawn; the Quest build checks `AuroraDawnFdmVersion()` at runtime);
  `lib/gfx/foveation.hpp` builds the map (Wiicompiled's rings: Low full within
  30 degrees of the eye's forward direction and 2x2 beyond, Medium 25 / 40,
  High 18 / 34; densities 255, 127 and 63 so a half cannot round back to one
  pixel; each eye's map is centred on its own asymmetric frustum, towards the
  nose); `lib/gfx/stereo_foveation.{hpp,cpp}` owns the per-target state: a
  second, render-attachment-only view of the eye targets (an explicit
  descriptor, since Dawn hands back the same object for every default view),
  the bound map, and a key of size, level and the four tangents per eye in
  hundredths. A new map replaces the bound one only once uploaded, so a live
  level change never shows an unfoveated frame. `stereo_host::begin_frame`
  prepares it after the targets; `stereo_seal_pass` makes the eye pass render
  through the foveated view (the multiview array view, or one per eye) while
  the copies taken from an eye keep the eye's own view
  (`StereoEyePass::copySourceView`). Every replayed eye pass of an immersive
  frame is foveated, splits included: Wiicompiled's "single render pass only"
  rule cannot be applied here because passes are sealed and handed to the
  render worker while the frame is still being recorded, and on Adreno a
  world-sized eye pass is binned anyway, so a load under a density map reads
  the same full-resolution tiles as one without. The virtual screen (menus,
  cinematics), the blit, the mirror and the EFB copies are never foveated.
  The targets release the maps before they are recreated (a binding keeps its
  view, and so the old texture, alive).
- **Setting** `vr_foveation` (`off`, `low`, `medium`, `high`; default `off`, see
  the measurement below; the desktop has no path for it): F1 VR tab
  "Foveated rendering (Quest)" (never disabled: a session started with it off
  has no maps, yet the level chosen is the next start's), launcher Port Config,
  `MP_FOVEATION=<level>` for one run (also an `am start --es` extra). Off to a
  level takes a restart; between levels and back to off it is live.
  `aurora_get_stereo_foveation` / `aurora_stereo_foveation_available` report it.
- **Logs**: "Fragment density maps: enabled, AxB to CxD pixels per texel, using
  32" at device creation (or why they are off), "Eye foveation low: 45x47
  density map x2, 32 pixels per texel, for the 1428x1496 eyes" per rebuild, and
  the `stereo frame:` statistics line counts the foveated passes. The `Fov=`
  field of Meta's `VrApi` logcat line reports the runtime's own foveation and
  stays 0: it is not an indicator here.
- **Measured** (Quest 3, Chozo plaza save state, `docs/CHOZO_PERFORMANCE.md`
  "Fixed foveated rendering"): the maps cost GPU time and save none. At
  599 MHz, App GPU 9.7 ms off, 9.9 with the device extension and pipeline flag
  alone, 10.2 with one shared map, 10.4 with the per-eye two-layer map; at
  render scale 1.25, 11.4 off against 12.1 / 12.2 / 11.9 for low / medium /
  high. Wiicompiled measured 8 to 22 % savings at 1.3 on Mario Kart; Prime's
  eye pass is bound by per-draw vertex fetch and binning, not fragment
  shading, and a map on a non-subsampled image still stores full tiles. The
  High screenshot (`build/foveation/quest_fov_high.jpg`) shows the coarse
  shading really applied (4x4 blocks on the visor frame and the walls, sharp
  centre), so the mechanism works; there is no pixel work to save here. Hence
  the default `off`; the setting stays for fill-heavy areas (water, fog, heat,
  snow), to be measured the same way. Diagnostics for that: `MP_FDM_DEVICE=1`
  gives the device the extension with the level off, `MP_FOVEATION_LAYERS=1`
  binds one shared map (each texel the finer of the two eyes') instead of the
  per-eye layers; both are accepted as launch extras.
- Tests: `extern/aurora/tests/foveation_test.cpp` (`foveation_tests`, the map
  generator at the port's eye size), the launcher's default row. Verified on
  the headset: the patched Dawn rebuilt (`quest/Build-Quest.ps1`), the log
  lines above, screenshots at low / high / off, no Vulkan or WebGPU errors.

## VR: the beam wheel's hover lights the HUD's beam box (2026-10-04)

PrimedGun showed its beam wheel (hold the weapon hand's B, point the cannon up,
right, down or left) on a panel of its own: four PNG icons floating in front of
the controller with an orange frame around the hovered one. The port has had
the wheel itself since the pad synthesis was lifted (`platform/vr/vr_pad.cpp`
tracks the hovered beam in `VrPadState::weapon_selected`), but nothing showed
the hover: the player released B blind and watched the HUD flash afterwards.

With source access the HUD's own beam menu does the job. `CHudVisorBeamMenu`
already draws the four beam boxes (`model_beamloz*` / `model_beamicon*`) and
recolours them every tick from the GuiColors tweak: the active grey (0.66) for
the current beam's icon, the inactive grey (0.56) for the others, and a
translucent dark blue for the current beam's lozenge. Those colours modulate
the models, so the game's own select flash (`kAP_SelectFlash`, alternating the
two greys on the pending beam) is the brightest the boxes ever get in retail.

- `PortVr::VrBeamWheelHoverBeam()` (`platform/vr/vr_view.cpp`) turns the pad's
  hover into `CPlayerState::EBeamId` (the wheel counts PrimedGun's way, Power /
  Wave / Ice / Plasma; the game puts Ice before Wave), or -1 when the wheel is
  closed, nothing is hovered, the controllers are not in gameplay, or the
  setting is off.
- `CSamusHud::UpdateVisorAndBeamMenus` hands it to the beam menu
  (`PortVrSetHighlight`), and `CHudVisorBeamMenu::Update` sets that item's icon
  and lozenge to white after the phase's colours, so the hovered box reads as a
  clear step brighter than everything else, selected beam included. A beam the
  player does not own yet is left alone (its icon is invisible anyway). The
  highlight lasts exactly as long as the hover: every phase that follows
  rewrites the colours.
- Setting `vr_beam_wheel_hud_highlight` (default on): F1 VR tab, Controls
  (with a live "Beam wheel: open / hover: Ice" readout), launcher Controller
  tab, Reset Controller list, `tests/port_launcher.cpp` default check.

The first headset run showed the wheel itself inverted: aiming up lit (and
switched to) Ice, aiming right Plasma. PrimedGun's `PrimedGunRollFreeQuat`
levels the panel with right = forward x up = (-fz, 0, fx); the port's copy in
`vr_pad.cpp` had (fz, 0, -fx), the left vector. With up = right x forward that
basis is the level one rolled 180 degrees about the aim, so the frozen panel
was upside down: both of its axes, the hand-travel fallback's too, read
negated, and its centre sat 5.5 cm below the aim instead of above. The aim ray
itself was right, which is why nothing looked broken until the HUD showed the
hover.

- The wheel's maths moved to the header-only
  `platform/include/vr/vr_beam_wheel.h` (levelled frame, panel, ray or travel
  measure, four-way pick, C-stick direction, `EBeamId` order), with the sign
  fixed. `vr_pad.cpp` and `VrBeamWheelHoverBeam` use it.
- `tests/port_vr_beam_wheel.cpp` (`port_vr_beam_wheel_tests`) turns a
  simulated controller up, right, down and left from six facings, pitches and
  rolls and expects Power, Wave, Ice and Plasma, and checks the levelled frame,
  the travel fallback and the mappings. With the old sign it fails on its
  first check.

Confirmed in the headset: the wheel picks the box it points at and the HUD lights it. build/vr and build/nooxr
pass 39/39.

## The capture harness was reading a stranded save, and contradicted itself (2026-09-27)

Two harness defects found while closing the last prompt surface. Neither is a port
bug; both mean previous evidence was weaker than it looked.

- **The canonical save every capture copies from was a corrupt file.** Seven
  scripts - `audio-probe.sh`, `card-auto.sh`, `card-probe.sh`, `deathlink-e2e.sh`,
  the pause and map captures and the dump scripts - all seed
  `build/smoke-gcc/USA/Card A` from
  `/home/odran/.local/share/Metroid Prime/USA/Card A`. That file was
  `01-GM8E-MetroidPrime A.gci` with **0 non-zero bytes in the save region and an
  invalid CRC**, stored `00000000` against a computed `553E7B06` - which is the
  exact signature of the fast-boot stranding fixed in `f44737f2`. It was created
  before that fix and had sat in the real save directory ever since, so every
  capture since has been starting from a card the game would call corrupt. The
  good save from the "saves and reloads" proof (134 non-zero, valid `34adeb51`)
  was in `build/card-with-save-backup/`, which is **gitignored**, so the only
  good copy was local and the canonical path was left holding the bad one.
  Restored the good save to the canonical directory, and the smoke build's card
  now matches.
- **`build/continue-walk.sh` contradicted itself and the documentation.** Its
  header says it "walks the title screen looking for Continue", and it set
  `MP_FAST_BOOT=1` - which drives the front end straight past file select into a
  new game, as `docs/NATIVE_PORT.md` states. So the walk could never arrive where
  it was trying to go, and the frame-number presses were landing in the intro
  cinematics. It now leaves fast boot off, uses the state-driven
  `MP_SMOKE_CONTINUE` walker instead of fixed frames, and has a longer budget
  because the real front end has to load.
- **With that fixed, the front end is reachable and the prompt is correct.** The
  main menu lists the real save - `[Samus A] 00% | Space Pirate Frigate | 00:00
  Elapsed` - and its Select prompt is a clean `X` key cap, which is the keyboard
  binding for A, so the front end's A prompt (`0xbb21e875`) renders correctly.
  In `docs/images/prompt-front-end-menu.png` and
  `docs/images/prompt-front-end-select.png`. This also re-confirms the save
  loads, from a card that was verified good beforehand rather than assumed.
- **The front end's B prompt is still not photographed.** Nothing on the main
  menu draws it, and the screens that do were not reached this pass.
## Prompt audit: what is covered, and the one prompt still unplaced (2026-09-27)

Closing out the prompt work with a sweep of every surface I can reach, so the
coverage is a list of what was looked at rather than an impression.

- **Pause menu — photographed, correct.** `Q OPTIONS`, `X NEXT`, the game's
  sphere for `EXIT`, `Z BACK`, `E LOG BOOK`. The `EXIT` case is the bug that
  started all of this.
- **Map screen — photographed, correct.** `F Exit`, the arrow glyph on
  `Rotate`, `Q Zoom`, `E Move`. `Rotate` is the C-stick row that was simply
  missing, so the map had been showing no stick icon at all; it now does, at the
  right size, via the bindings path, with one log line instead of 4200.
- **Map legend — dumped and checked, nothing missed.** Of the map's textures,
  the six unclaimed ones in format 5 — the only size a prompt uses there — are
  the legend's three books, a "?" hint and an "E" elevator. All decoration.
- **Gameplay HUD — photographed, game art, correctly untouched.** The corner
  prompts are the game's own pale icons (a morph slider, a crosshair), and they
  are not in the replacement table, so the game drawing its own art there is
  right rather than a gap.
- **Front end A/B — still not reached by an automated capture.**
  `MP_SMOKE_FRONTEND` taps Start and walks past the publisher logo, the distress
  beacon narration and into gameplay, so it never sits on a front-end screen
  that shows the A/B prompts. Reaching them needs the main menu, which needs a
  save and the Continue walker. Not attempted this pass.
- **`0xe14dc493`, the yellow "C" badge — restored, still unplaced.** The dump
  proves it is a C-stick prompt; it is not the pause menu, not the map's
  `Rotate`, and not the gameplay HUD corner icons. Some screen I have not reached
  draws it. Having it in the table means that screen gets the bound input's icon
  rather than the game's badge, which is an improvement wherever it turns up, so
  leaving it is right even without knowing where it is.
- **The narration is not a bug, again.** The sweep photographed the opening
  "Unidentified distress beacon has been tracked to a derelict space vessel in
  orbit above Tallon IV", which is where the old "unidentified memory card"
  report came from. It is Metroid Prime's own opening narration.

## An implemented feature that was missing from the list (2026-09-27)

The objective names "checks persisted across save and reconnect" as an item to be
implemented or explicitly descoped. It was implemented and tested, and it was
not in the written list at all — so the list said nothing about it, and an
implemented feature that is not in the list cannot be audited from the list.
Now written down, with the mechanism and the end-to-end observation separated
rather than conflated: `port_apclient_tests` covers a recorded location not
being checked twice, `checkedLocations` accumulating, the state file round trip,
and a `RoomInfo` for a different seed discarding the recorded checks, item index
and progressive counts; the end-to-end evidence is a reconnect resuming at
`next_item_index 2` rather than re-granting.

## Audited two standing requirements rather than assuming them (2026-09-27)

- **"No disc image or copyrighted asset is ever packaged."** Checked, not
  assumed. The install tree has no `.iso`, `.gcm` or `.wbfs`; it ships the
  binary, the pipeline cache, 162 generated texture replacements, the desktop
  entry and the metainfo. The release APK likewise has nothing disc-shaped —
  a disc would be three orders of magnitude larger than its largest entry — and
  carries 162 texture assets, all derived from Kenney's CC0 Input Prompts pack
  with the attribution recorded in `tools/prompt_icons/README.md`.
  `assets/openssl-license.txt` is present, so the Apache-2.0 notice for the
  newly vendored OpenSSL travels with the APK.
- **The real Archipelago world is still unreachable.** Re-checked rather than
  carried forward on the assumption it had always been: `git ls-remote` on
  `UltiNaruto/MetroidAPPrime` and the GitHub API both return 404. So the
  mapping-verification gap stays a gap, and the local `Locations.py` fixture
  remains the stand-in. `Items.py` is not needed for `make_ap_config.py
  --strict` to be exercisable — the tool mirrors that file's item ids, and says
  so at line 65 — and `--self-test` passes.
## Review found a busy-loop I introduced, and a tool that already existed (2026-09-27)

A second opinion on the prompt work. It was right about all three things I
asked it to doubt, and one of them is a process failure worth more than the bug.

- **I introduced a per-frame retry loop.** The size guard called
  `reg.activeStem.clear()` when it refused art, and `Poll()` skips a key whose
  stem already matches - so clearing it made `Poll` re-run `Apply` on that key
  **every frame**: a `stat`, a DDS open and a stderr line, 60 times a second for
  the rest of the session. Two map-screen runs logged 4200 and 4199 of them.
  `activeStem` is now left recorded on both refusal paths, so there is one
  attempt and then silence. Verified: 4200 -> 1 and 4199 -> 1.
- **The comment I wrote was wrong.** It said a refusal "keeps the game's own
  art". It does not: `PortTextures` registers the static per-device set by
  filename, so if it has a file for that texture then *that* is what remains -
  art for the **default** bindings. So on the map a rebound Z would still have
  read "F". The photographs of the map bar were never evidence about the
  bindings path at all; they were the static set.
- **I had only written the sized bindings for pad stems, not key stems.** The
  key stems serve the same actions, so both 64x32 rows fell back to a 32x32
  file, were refused on size, and were left showing the static set. The key
  stems are now generated at every size in the table, like the pads.
- **I repeated the exact mistake I had diagnosed.** I removed the `0xe14dc493`
  row from the table but left the four `tex1_64x32_e14dc493b5513d14_5.dds`
  files in `textures/`, which still replace that map texture with the stick
  glyph. Deleting a table row changes nothing about the static set - that was
  the entire insight of the Exit-sphere bug, and I then failed to apply it.
  Four files removed.
- **THE PROCESS FAILURE, and it is the one that matters.** I added a
  `MP_LOG_TEX_HASH` hook to `extern/aurora/lib/gx/texture.cpp` to enumerate
  loaded textures. **Aurora already had exactly that**, gated on
  `MP_DUMP_TEXTURES=1`, wired in `platform/main.cpp:235` and documented at
  `docs/NATIVE_PORT.md:302` - in this repo, which I was already reading, with a
  worked description of the workflow I was about to reinvent: "dump the textures
  from a screen that shows the prompt, find the glyph by its size and contents".
  I should have read our own documentation before writing a hook into vendored
  code. The hook is reverted; `extern/` is untouched. The replacement is better
  than what I built in two specific ways: it does not dump textures that already
  have a replacement, so what it yields is exactly the unclaimed set, and it
  writes the images themselves, so a candidate can be **looked at** instead of
  being identified by which prompt turns lime.
- **What survives, and what finally works.** The C-stick row now points at
  `0x2d26352b420db007`, the map's Rotate prompt, and the stick glyph now actually
  appears there - photographed - via the bindings path, at the right size, with
  one log line instead of 4200. So the 64x32 route is genuinely exercised for
  the first time, rather than being insurance.
- **Still not verified:** the HUD hint memo prompt, the front end A/B prompts,
  and anything on a device. And I still do not know what the `0xe14dc493`
  texture is; leaving it replaced was the same class of mistake, so its files are
  gone and the game's own art shows there again.
## Checked the rest of the prompts; the C-stick one is still a gap (2026-09-27)

Having fixed the pause menu, the obvious question was whether the other prompt
screens were also wrong. They mostly were not, and the one that is has a
different cause than I first assumed.

- **Pause menu, after the fix: correct.** `Q OPTIONS`, `X NEXT`, the game's
  sphere for `EXIT`, `Z BACK`, `E LOG BOOK`. Photographed, not assumed.
- **Map screen: the three replaced prompts are correct.** `F Exit`, `Q Zoom`,
  `E Move` all draw proper key caps, which matches the bindings: L->Q, R->E and
  Z->F. `MP_SMOKE_MAP` reaches it; `build/map-capture.sh` is the recipe, made
  from `build/pause-capture.sh` with `MP_SMOKE_PAUSE` swapped for
  `MP_SMOKE_MAP`.
- **The map's `Rotate` prompt shows the game's own art, and that is NOT the size
  guard's doing.** I said it was: the only C-stick entry is 64x32 while every
  binding was 32x32, so I expected the new guard to be refusing it. It is not.
  The entry's hash simply does not match the texture the game draws there, so no
  replacement is attempted either way, before or after. **I was wrong, and the
  photograph corrected it.** The C-stick prompt has never appeared on that
  screen; it is a coverage gap, not a regression.
- **So the 64x32 handling is correct but currently unexercised.** The guard and
  the sized bindings it needs (`make_prompt_glyphs.py` now writes
  `<stem>_64x32.dds` alongside `<stem>.dds`, 52 of them, correctly 8320 bytes)
  are worth keeping, because the table does contain 64x32 slots and the old code
  would have served them 32x32 bytes. But I should be plain: **no reachable
  prompt currently exercises the path**, so it is insurance, not a verified fix.
  Presenting it as the second half of a two-bug story would overstate it.
- **What would actually settle the C-stick entry**, and why I stopped: it needs
  the texture hash the game really uses for the map's Rotate prompt, which means
  a texture dump this project does not have tooling for. The pause-menu sphere
  was the same class of error - a hash transcribed from a dump into the wrong
  row - so this one is plausibly a second transcription mistake. But "plausibly"
  is not "confirmed", and guessing a hash is how the first one happened.
- **Evidence.** `docs/images/prompt-screens-checked.png` has the pause bar and
  the map bar, both after the fix. 15/15 ctest.
## The wrong prompt art on the pause menu's Exit, and why the first two fixes failed (2026-09-26)

Reported as "the textures for button presses that are getting replaced in game are
incorrect". Real, reproducible, and shipped in the repository.

- **The symptom.** The pause menu's bottom bar draws `Q OPTIONS`, `X NEXT`,
  `EXIT`, `Z BACK`, `E LOG BOOK`. Four of the five are correct. **EXIT** drew a
  cluster of mismatched grey squares instead of the plain white sphere the game
  draws. Evidence in `docs/images/prompt-replacement-ab.png`: the same bar with
  replacements off, before, and after.
- **The cause.** `platform/port_prompts.cpp` listed a texture as a C-stick
  prompt that is actually the pause menu's **Exit sphere**:
  `{PAD_AXIS_CSTICK, 32, 32, 0x1ff9d2b310c0b706ull, "14"}`. Because the table
  said stick, `tools/make_prompt_glyphs.py` wrote the C-stick's four-square arrow
  glyph into it and shipped that to all four device folders, so the game was
  handed a stick icon for its Exit button.
- **The tell was in the table all along.** It was the **only** 32x32 entry with
  format `14`; the other eight are format `5`, and every 64x32 entry is format
  `14`. One row had its dimensions and format transposed relative to every other
  row, which is what a careless transcription looks like. Counting the format
  field per size would have flagged it without running anything.
- **Two fixes I tried first did not work, and both failures were informative.**
  1. *Deleting the table row* changed nothing. There are **two independent
     systems** registering the same texture names: `PortPrompts` serves the
     binding-aware art from `<textures>/bindings/`, and `PortTextures` registers
     static per-device files from `<textures>/<device>/tex1_<w>x<h>_<hash>_<fmt>.dds`
     at startup. The static one is keyed by **filename**, so it is untouched by
     any edit to the table. Deleting the row is necessary but not sufficient.
  2. *Hiding the whole `bindings/` directory* also changed nothing, for the same
     reason, and it was the clue: with `PortPrompts` entirely out of the picture
     the wrong art was still on screen, so the static file was the culprit.
  Removing that **one file** restored the sphere. The fix is the file, and the
  table row goes with it so the generator cannot recreate it.
- **A second, latent bug the hunt exposed.** Bindings are written
  `make_icon(icon)` with no dimensions, so every generated binding is **32x32** —
  but a single stem can serve textures of different sizes, and the remaining
  C-stick prompt is 64x32. Serving 32x32 bytes under a name that says 64x32 does
  not scale the image: the game reads 64x32 of pixels out of half the data and
  draws the right-hand half as noise. `Apply` now reads the DDS header and
  **refuses art of the wrong size**, keeping the game's own art instead, which is
  the correct outcome rather than a fallback to apologise for.
  **I have not seen this one on screen** — the 64x32 prompt is not on the pause
  menu — so it is fixed by inspection and by the size check, not by a capture.
- **How the capture was made, and one trap.** `MP_TEXTURES=<empty dir>` disables
  both replacement systems, which gives the "off" half of the A/B. The screenshots
  are PNGs with an **alpha channel of zero everywhere**, so cropping and viewing
  them shows black unless they are converted to RGB first; two crop attempts came
  back black and looked like a rendering fault before the alpha was noticed. Also
  worth recording: Gradle's `BUILD SUCCESSFUL` is not evidence, and a screenshot
  that renders is not evidence either.
## The touch overlay is testable now, and writing the test deleted the mapping (2026-09-26)

The virtual gamepad has been lifted out of `#if defined(__ANDROID__)` and given a
host test. That is the first time this code has ever been compiled outside an
Android build.

- **`platform/touch_pad.cpp` + `platform/include/touch_pad.h`** hold the
  descriptor, the axis conversion, attach and detach. `tests/touch_pad.cpp`
  attaches a **real** SDL virtual gamepad and asserts what the game would read
  back through `SDL_GetGamepadButton` / `SDL_GetGamepadAxis`, rather than what the
  code wrote. The host's `libSDL3.a` does contain the virtual joystick driver, so
  this runs in CI on Linux and Windows.
- **Why read-back and not write-only.** The overlay writes raw `SDL_Gamepad*`
  indices, and SDL turns those into logical controls using a mapping. If the
  mapping is wrong the write still *succeeds* and the game still reads
  *something* — the right stick moves when A is pressed — so nothing upstream can
  notice. Driving a real pad and reading it back is the only thing that tests it.
  The test presses each raw index in turn and asserts that exactly one logical
  button comes up, naming both on failure.
- **The first run failed, and the failure was the point.** 28 checks failed
  against the real code, and they were right to: the hand-written mapping string
  named `b0`-`b4` and `b9`-`b14`, while `SDL_GAMEPAD_BUTTON_COUNT` is **26**.
  Guide, both stick clicks and every paddle were unmapped, so a write to any of
  them went nowhere. The overlay does not send those today, so nothing was
  visibly broken — but the string was incomplete, and it was incomplete in a way
  that would have bitten the first person to add a stick-click to the overlay.
- **So the mapping string is gone, and that is the improvement.** The string was
  never doing the job its comment claimed. A virtual device gets a GUID with the
  virtual bus and a `'v'` signature, so the vendor and product ids never selected
  a built-in table by GUID match either; SDL routes an unmatched virtual device
  to the virtual driver's own generated mapping, which is exactly this enum order
  (`SDL_virtualjoystick.c:803, :933`). I checked that rather than assuming it:
  with the explicit string disabled, every button index still reached its own
  logical control. A hand-written table is one more thing that can disagree with
  SDL and it can only fall behind; the generated mapping is derived from the enum,
  which is SDL's public ABI. The test is what pins the behaviour now, so a change
  in SDL fails a build instead of quietly misplacing a button on a phone.
  A by-product: SDL's own database contains entries with `start:b6` and shoulders
  `b9`/`b10`, which independently corroborates that the layout was right all along.
- **My test had a bug too, and it was the kind that lies.** The cross-axis
  checks reported contamination that was not there, because each iteration left
  the previous axis at the *minimum* instead of at rest and then asserted the
  others read 0. A stick rests at 0 and a trigger rests at the minimum, so "all
  axes at rest" is not one value. Fixed with an explicit `RestAxes`, and the
  contamination went away — which is how I know the remaining failures were real.
- **The test is not vacuous, checked by breaking the code twice.** Reverting the
  conversion to truncation fails it on "half deflection must be a real half, not
  zero" — which is exactly the bug the conversion exists to prevent, since
  truncation made every partial deflection read as centred. Removing the
  detach-on-failed-open fails it on the device count. Both restored.
  **Stated honestly:** the open-failure path itself is not directly exercised,
  because forcing an allocation failure inside SDL is not something a test can
  ask for. What the test catches is the missing detach, by counting devices.
- **Verified on both builds, and the Android one the hard way.** Host: 15/15
  ctest. Android: Gradle reported `BUILD SUCCESSFUL`, which I now treat as no
  evidence at all, so I checked that both objects postdate the edits and that our
  mapping string is **absent** from the `.so` (0 occurrences) while SDL's own
  database entries remain, the JNI entry points are intact, and the log line
  survives. Absent-because-removed is a decisive marker; present-because-added
  would not have been, since the symbols are hidden by design.

## Virtual gamepad review: the threading worry was wrong, the comment was too (2026-09-26)

A second opinion on the Android touch overlay's virtual gamepad. It overturned my
main hypothesis and found one thing that does affect a player.

- **Thread safety: I was wrong, and the reviewer was right.** I expected the JNI
  writes from Java's UI thread to race SDL's reads on the game thread. They do
  not. `SDL_SetJoystickVirtualAxis` and `SDL_SetJoystickVirtualButton` both take
  `SDL_LockJoysticks()` around the inner call, and the inner functions assert the
  lock. The update path and the game's readers (`SDL_GetGamepadAxis`,
  `SDL_GetGamepadButton`) take the same lock. I checked this myself in
  `SDL_joystick.c:1529-1540,1569-1580` rather than taking it on trust. So no
  tearing, no half-written button, and no ARM-specific concern.
- **What *is* real: a short tap can be missed.** Not a race — the virtual
  joystick API is **state-sampling, not event-queueing**. Setting a button stores
  the latest value and marks it changed (`SDL_virtualjoystick.c:401`); the change
  is delivered at the next update, which sends the value *as it is then* (`:742`).
  A press and release that both land between two updates leave only the release,
  and the game never sees the press. So a quick tap on A or Start can do nothing,
  most visibly while a game frame is stalled. Sustained presses and ordinary
  releases are fine, which is exactly why it has never shown up as "the controls
  don't work". Fixing it means latching a press until the game has sampled it, and
  the latch must be released on an update the port does not control — a real
  design problem, so it is now documented at the code rather than half-solved.
- **The leak I suspected is real but minor, and is fixed.** If attach succeeds
  and `SDL_OpenJoystick` then fails, the id was discarded while
  `g_virtualPad` stayed null, so every later call attached another device. SDL
  has **no small fixed limit** on virtual joysticks — they are a linked list
  (`SDL_virtualjoystick.c:315`) — so this accumulates rather than hitting a
  ceiling. It needs an allocation failure to reach (`SDL_joystick.c:1360,1394`),
  which makes it a robustness fix rather than an ordinary-play bug. It now
  detaches before returning.
- **Lifecycle: not a bug, and SDL is why.** I expected a stale native pointer on
  activity recreation. It does not happen: `SDLActivity.java:426-439` checks a
  native run counter and calls `System.exit(0)` on a second creation in the same
  process, and `allow_recreate_activity` defaults to false
  (`SDL_android.c:784`). The manifest also handles configuration changes itself.
  Worth recording that the cached pointer would *not* survive a full `SDL_Quit()`
  (`main.cpp:368`), so anyone enabling same-process recreation later needs
  explicit close/detach/reset. Recorded, not fixed, because it is not reachable.
- **The mapping is correct — and the comment explaining it was factually wrong.**
  Every entry is right: `start` is b6 because **b5 is Guide** and b8 is
  right-stick click, and the triggers are correctly axes a4/a5, which is what
  Aurora's binding expects (`dolphin/pad/pad.cpp:243`). But the old comment
  claimed SDL's built-in Xbox mapping binds `rightx:a3, righty:a4, start:b8` and
  shoulders to b4/b5. That table is in SDL_gamepad_db.h's **macOS** section
  (`:483`); the Android `045e:02ea` entry (`:816`) already says `start:b6,
  rightx:a2, righty:a3`. Further, a virtual device gets a GUID with the virtual
  bus and a `'v'` signature, so the vendor/product ids do not select a table by
  GUID match at all, and an unmatched virtual device falls through to the
  virtual driver's own generated mapping — which is this same enum order
  (`:803, :933`). So the explicit mapping is belt and braces, not a correction.
  **The mapping was always right; the reason given for it was not.** That is the
  same failure mode as the fast-boot comment I had to fix earlier: a comment that
  confidently describes a mechanism that does not exist, which sends the next
  person hunting a fault that was never there.
- **The numeric conversion is correct.** Clamp, asymmetric scale, `std::lround`:
  `-1 -> -32768`, `0 -> 0`, `+1 -> +32767`, `±0.5 -> ±16384`, and exactly zero
  stays exactly zero. The Java side has a radial 0.12 dead zone, so there is no
  centre jitter, and a trigger release correctly sends `-1` rather than zero.
  A partially-sampled X/Y frame is possible — the two setters take the lock
  separately — but release sends both zeros on the UI thread and the final zero
  stays pending, so **an axis cannot stick**.
- **The `caDir`/AP work in `f28c580c` is unrelated and unaffected.** Noting it
  only because both touched `platform/` in the same session.
- **Not done, and the reviewer is right that it is the real remaining gap:**
  this code is inside `#if defined(__ANDROID__)`, so it is **never compiled on
  Linux or Windows and has no test at all**. The mapping, the conversion and the
  attach/detach logic are all platform-independent and could be extracted into a
  host test that drives a real SDL virtual pad and asserts what the game would
  read through `SDL_GetGamepadButton`/`SDL_GetGamepadAxis`. That would be the
  first time this code is ever compiled outside Android, and it is a genuine
  behaviour test rather than a comparison of constants. Deliberately not started
  here: it is a refactor, and doing it in the same change as a four-line
  robustness fix would bury both.
- **A build that proved nothing.** The first `assembleDebug` after the edit
  reported `BUILD SUCCESSFUL in 10s` having touched no C++ at all — the `.so` was
  older than the edit and the log never mentioned the file. A green build that
  built nothing is worth less than no build. Re-run with `--rerun-tasks`, and
  confirmed properly: the object file postdates the edit and carries
  `U SDL_DetachVirtualJoystick`, a reference that only exists because of the new
  code. Gradle suppresses ninja output unless it fails, so "successful" is not
  evidence that anything compiled.
- **The attach path was not exercised on the emulator.** `VirtualPad()` is lazy
  and nothing touched the overlay — on a fresh install the SAF picker is in
  front — so logcat had no `touchpad` lines at all. That costs little: the change
  only adds behaviour on the *failure* branch, and the success path is otherwise
  byte-identical. Stated rather than glossed.

## Found: no icon is shipped, at all (2026-09-26)

Chasing the Flatpak app id turned up something that was not on any list,
including the blocker list I wrote myself.

- **The desktop entry names an icon that does not exist.**
  `Icon=io.github.odrannnn.metroidprimeport`, and there is no icon file with
  that name in the tree, no `share/icons` install rule in `CMakeLists.txt`, and
  nothing in the Flatpak manifest that installs one. A desktop environment falls
  back to a generic icon, and Flathub requires a real one. It affects the
  AppImage, the Flatpak and a package-manager install equally — not just the one
  format I was looking at.
- **How it survived.** I had been reading `appstreamcli validate` as the gate on
  the metainfo, and it is: the file is valid. But the icon is not the
  metainfo's business, and the desktop file passes `desktop-file-validate`
  happily while pointing at nothing. The only way I found it was to install to a
  prefix and look at the tree rather than at the two validators — which is
  exactly the check I had used to prove the Flatpak install rules work in the
  first place, and then stopped doing.
- **It cannot be filled with Nintendo's artwork**, for the same reason the
  screenshots cannot: anything recognisably Nintendo's is not this package's to
  redistribute. So this one needs original artwork, which is a decision rather
  than a task, and I have not invented one.

## Flatpak app id: io.github.odrannnn.metroidprimeport (2026-09-26)

The app id is renamed, everywhere it appears.

- **The rename is complete and consistent**: the metainfo `<id>` and
  `<launchable>`, the desktop entry's `Icon=`, the manifest's `app-id:`, the two
  `install(FILES ...)` rules in `CMakeLists.txt`, `tools/make_flatpak.sh`'s
  manifest path, and the references in `PORT_NOTES.md`, `docs/RELEASING.md` and
  `docs/NATIVE_PORT.md`. The three files were `git mv`d, not copied.
- **The complaint is gone.** Before, `appstreamcli validate --pedantic` reported
  `cid-contains-uppercase-letter` for `org.metroidprime.MetroidPrimePort`. It
  now reports `Validation was successful.` with no pedantic warnings at all.
  Worth noting the flag matters: plain `appstreamcli validate` passed *before*
  the rename too, so the check I had been running was not the one that mattered.
  The docs said this was the outstanding complaint and I had believed the
  metainfo was clean because the laxer command said so.
- **Why `io.github.odrannnn`.** Flathub verifies an app id against a domain you
  control, and `org.metroidprime` was a placeholder nobody owns, so it could
  never have been submitted. `io.github.*` is a convention Flathub accepts and
  the account is the maintainer's own.
- **Verified by installing, not by validating.** `cmake --install` to a prefix:
  the binary runs (`a9c16516-dirty`), the desktop entry and metainfo land under
  `share/applications` and `share/metainfo` with the new id, the installed
  metainfo still validates pedantically clean from the install tree, and the
  desktop entry's id matches the filename it was installed as. That last check
  is what caught the missing icon.
- **The Android package name is unchanged** at `org.metroidprime.port`.
  It happens to share the old domain, but it is a different identifier in a
  different ecosystem, and changing an `applicationId` breaks installs for no
  reason. It was not in scope.

## The Flatpak manifest has never been buildable, and now I know why (2026-09-27)

`flatpak-builder` ran for the first time on this project. It got further than
anything before it and then failed in a way that is not a host problem.

- **Three host problems came first, all now fixed, none of them the port's
  fault.** `flatpak remote-add` needs the `/repo/` prefix on the Flathub URL
  (`https://dl.flathub.org/repo/flathub.flatpakrepo`; the bare path 404s), and
  fails outright if `DBUS_SESSION_BUS_ADDRESS` points at a dead bus. The build
  sandbox needs `kernel.apparmor_restrict_unprivileged_userns=0`, which Ubuntu
  26.04 sets to 1; that is what made `bwrap` fail, and `bwrap` works now.
  `rofiles-fuse` cannot mount because `fusermount3` is setuid and a setuid binary
  gains nothing inside a user namespace, so the script now retries with
  `--disable-rofiles-fuse`. And `user_allow_other` has to be set in
  `/etc/fuse.conf` - a manual mount as this user then succeeds while
  flatpak-builder's does not, which is what pinned it down.
- **And then the manifest itself, which is the real finding.** It failed during
  CMake configure, on Dawn, with `getaddrinfo(3) failed for github.com:443`.
  That is not a network-permission problem. **`flatpak-builder` has no network
  option at all** - I added `--share=network`, it does not exist, and I reverted
  that. A Flatpak build is meant to be offline: everything must be declared in
  `sources:`, because the sandbox shares no resolver. This host resolves through
  systemd-resolved's stub on `127.0.0.53`, which does not exist inside the
  sandbox's network namespace, so every `FetchContent` dies at DNS.
- **The manifest declares exactly one source: the repository.** It expects Dawn,
  SDL3 and nod to be downloaded at build time. That cannot work on any host, so
  this is not a defect of this machine - the manifest has simply never been
  buildable, which is consistent with the notes saying nothing here had ever run
  `flatpak-builder`. The install rules had been verified by installing to a
  prefix and running the tree, which proves the *output* is right and says
  nothing about whether the manifest can produce it.
- **The fix is smaller than it first looked.** There are 7 `FetchContent_Declare`
  sites in the build, but only four reach the network, and one of those is
  `cmake/AndroidOpenSSL.cmake`, which is inside `if(ANDROID)`. So the Flatpak
  needs **three** pinned sources: Dawn, SDL3 and nod. The rest come from the
  freedesktop SDK. Aurora's `AuroraDependencyVersions.cmake` already pins every
  version and ref, and the three provider files carry the URL templates, so the
  versions are known; what is not in the tree is any **hash** - there is no
  `URL_HASH` or `SHA256` anywhere in Aurora's dependency acquisition, which is
  fine for a developer build and not what a Flatpak manifest wants.
- **So the remaining decision is about pinning.** `flatpak-builder` accepts a
  `sources` entry with no `sha256` and only warns, which would get the build
  running today. Pinning them properly means downloading the three artefacts once
  to hash them - Dawn's prebuilt tarball is the large one - and recording the
  digests in the manifest. Pinning is the right answer for something that gets
  published, and it is what makes the build reproducible; not pinning is a
  faster route to a first successful build. I have not chosen between them,
  because it changes what the manifest guarantees.
## Android wss:// works, and it cost more than I said it would (2026-09-26)

The last AP gap that stopped a session is closed. A real `wss://` handshake from
the APK, on the emulator, proven in logcat rather than inferred from a build.

- **What was built.** `cmake/AndroidOpenSSL.cmake` builds OpenSSL 3.5.8 from a
  pinned, SHA-256-checked source for the NDK and links it statically. Perl and
  make are checked for at configure time with a message that says what to do,
  because OpenSSL's build is Perl and make and not CMake. The source is fetched
  through `FetchContent` so `FETCHCONTENT_SOURCE_DIR_OPENSSL` gives an offline
  build the same escape hatch every other dependency has; a local copy skips the
  hash check, so the version is read out of `VERSION.dat` instead. The licence
  travels in the APK through the existing `syncLicenseNotices` task, not a new
  mechanism.
- **On Android, OpenSSL is now required.** `find_package(OpenSSL)` stays
  optional on the desktop, but on Android a lookup that quietly finds nothing is
  precisely how an APK ends up refusing every `wss://` server — that is the
  whole reason this problem existed. `-DMP_ALLOW_NO_TLS=ON` is now the
  deliberate way to build without it.
- **The trust store, which was the part that would have cost a device.** As
  recorded earlier, `SSL_CTX_set_default_verify_paths` on Android points at a
  compiled-in `OPENSSLDIR` that does not exist, **returns success and loads
  nothing**, so the existing "could not load the system TLS trust store" check
  never fires. The port now enumerates the certificates itself and fails loudly
  on zero. The proof that this works is the *second* emulator result, not the
  first: with `tls_ca` the app connects (`connected as P`), and with `tls_ca`
  removed it reports
  `TLS certificate verification failed: unable to get local issuer certificate`.
  That is a **pass**. The test server's CA is not in Conscrypt's store, so a
  verification error naming the issuer is the correct outcome; the failure it
  rules out is `no TLS root certificates: loaded 0 from /apex/...`, which would
  mean the enumeration found nothing at all.
- **A better answer than the one I specified.** I asked for
  `-Wl,--exclude-libs,ALL` so OpenSSL's symbols would not be exported. The
  implementation narrowed it to
  `-Wl,--exclude-libs,libssl.a:libcrypto.a`, because `ALL` would also hide the
  JNI entry points in the static SDL archive, which Java looks up by name — so
  the version I asked for would have broken the launch. Worth remembering that
  the general form of a flag is not always the right form of it.
- **The APK cost is worse than estimated: +2.17 MB, +20.3%**, measured by
  building release both ways on the same tree (11,189,585 without, 13,464,781
  with). The first estimate was +0.8–1.2 MB. Static OpenSSL links more of
  itself into a binary this size than the estimate assumed. It is a real cost
  and it is a fair trade for `wss://` working, but it should be a number I
  measured rather than one I guessed.
- **Verified on the release `.so`**, since a stripped static link cannot be
  checked with `nm`: it contains `OpenSSL 3.5.8 25 Aug 2026`, does **not**
  contain `built without OpenSSL`, does contain the loud trust-store error, has
  **no** `NEEDED libssl.so` or `libcrypto.so`, and exports **no** OpenSSL
  symbols. That `NEEDED` check is the one that matters: a dynamic link builds
  cleanly and then fails on a device, because the system `libcrypto` is private
  BoringSSL.
- **The lane died on a session limit before reporting**, so I reviewed the diff,
  finished the link, ran the checks and did the emulator work myself. The code
  it left was good; the verification was simply not done.
- **Still not proven, and I am not claiming it:** a real phone. The emulator is
  ARM-translated onto x86_64 with a software rasteriser, so it says nothing
  about performance, a real GPU driver, or touch. What it *did* settle is
  everything about packaging, TLS and lifecycle — which is what it was worth
  using it for.

## Found: a location the table does not know was dropped in total silence (2026-09-26)

The last AP gap said the game side of the mapping is unverified and "needs a
played seed". **That is too pessimistic about the test and too optimistic about
the consequence.** Reading the path, the consequence is worse than an unverified
gap: it fails silently, in a way that looks like success.

- **The path.** `CScriptPickup` on collection
  (`src/MetroidPrime/ScriptObjects/CScriptPickup.cpp:179`) builds the key with
  `PortRandomizer::FormatLocationKey(world, area, entity)` — `%08X:%08X:%08X`,
  the same form the dump's `LOC` lines carry — and hands it to
  `PortAp::QueueCheck`, which calls `Session::MarkLocationChecked`. That returns
  **false for two entirely different reasons**: the key has no id in the table,
  or the key was already recorded. `QueueCheck` treated both as "nothing to do"
  and returned.
- **So what a key mismatch looks like.** The session connects, the handshake
  succeeds, items arrive, the HUD updates, the seed plays, and **the server
  records zero checks**. Nothing appears in any log. A player cannot tell this
  from a working session, and neither could I from the outside. For a multiworld
  that is the worst shape a failure can take, and it is the one failure the
  objective explicitly cares about — "plays a shuffled seed" is not true if no
  check is ever reported.
- **This is not hypothetical.** The upstream world's file has been unreachable
  throughout, and the fixture the 100/100 join is checked against is *generated
  from the same dump that produces the keys* (`tools/make_ap_fixture.py` reads
  `randomizer_locations.log` and writes both the table and `PICKUP_LOCATIONS`).
  So the check proves internal consistency and pins the join against regressions,
  but it **cannot** prove a real world uses the same key scheme. If it keys
  locations by a room name, a tuple or a different hash, every location the
  player collects fails to resolve. I have been reading 100/100 as more than it
  is.
- **Fixed: the two cases are now told apart.** `Session::KnowsLocation` answers
  whether the table has ever heard of a key, and `QueueCheck` uses it to report
  the unconfigured case once, naming the key and pointing at
  `docs/ARCHIPELAGO.md`. It deliberately does not log per pickup, because a
  legitimate session can touch a location that is not in the table and that
  should not become noise. The count is kept so it can be surfaced.
- **What is verified, and what is not.** `tests/port_apclient.cpp` covers
  `KnowsLocation` for an unknown key, a known key after it has been recorded,
  and an empty key — and I checked the new assertion is not vacuous by inverting
  it and watching the test fail. `tests/port_randomizer.cpp` already pins the key
  format. **The log line firing in a live session is not verified**: it needs a
  pickup actually collected against a config whose table lacks that key, and fast
  boot collects nothing, so triggering it would take a played seed — the thing I
  said was not needed. I am not going to manufacture a harness that reports a
  constant to cover that; the predicate is tested and the path compiles, and
  that is the honest state of it.
- **The gap is still a gap.** This makes the failure *visible*; it does not make
  the assumption *true*. Closing it needs a real Archipelago world file, and
  until one is reachable the correct claim is "the client tells you when its keys
  do not match", not "the mapping is verified".

## RETRACTED: the "badly corrupted frame" is the visor HUD working (2026-09-26)

I left an open item saying a captured frame was a corruption bug and might be the
same class of problem as the old "intermittent skinned geometry explosion".
**It is neither. It is the in-game visor, rendering correctly.** I chased it on
the strength of a text description and a filename, and I should have opened the
image before writing it down as a defect.

- **What the frame actually is.** `docs/images/front-end-file-select.png` is the
  first-person view *through Samus's visor*: the visor frame and its struts, the
  energy bar, the missile readout, the radar, her arm and cannon at bottom right,
  and "ENTERING" as the room-entry text. The magenta/orange cast is the visor
  tint and the horizontal banding is the visor's scanline effect. Every HUD
  element is legible and correctly placed. Nothing is broken.
- **The decisive argument, and it is worth keeping as a rule.** **Memory
  corruption does not produce clean, evenly-spaced horizontal scanlines.** Garbage
  from a bad upload or a mismatched array size shows up as blocky, irregular,
  structureless mush. Uniform periodic banding is a deliberate post effect, full
  stop. I had the evidence in front of me — the screenshot — and described it as
  "a magenta/orange wash with heavy scanlines" without asking why the corruption
  had a *period*.
- **The port adds no scanline effect, which settles where they come from.**
  `grep -ri 'scanline' src/ platform/` matches only `src/NESemu/` — the hidden
  NES emulator that ships on the Prime disc, not the render path. The banding is
  the game's. The visor is the game's too: `LoadVisorParameters` /
  `CVisorParameters` in `ScriptLoader.cpp:253`, carrying a mask and a
  `scanPassthrough` flag, and "ENTERING" is `kPBS_Entering`, a `CPlayerGun`
  beam state (`CPlayerGun.cpp:465`).
- **What the frame was doing there.** The capture was *meant* to be file select
  and caught the visor instead, which is what the surrounding note half-said at
  the time ("consistent with the attract movie or a file-select transition, but
  it is not the main menu"). So the real finding is a mistimed capture, not a
  rendering fault. Which is a smaller, duller, more believable thing.
- **What this does NOT close.** The older "intermittent skinned geometry
  explosion" item is a separate claim from a much earlier note, about array-based
  draws uploading zero bytes. That cause was found and fixed — the array section
  sizes are now threaded through to `GXSetArray` — and a draw-sync fence was
  added, with "confirm the fence removed it" still listed as a to-do. This
  screenshot is **not** evidence either way about that item; it is simply not
  evidence about it, and I should not have implied it was.

## The Android APK runs on the x86_64 emulator (2026-09-26)

I have been recording "no on-device testing for now" as if Android were entirely
unverifiable here. **That was too pessimistic, and I should have checked the SDK
on the machine before writing it off.** The arm64-only APK installs, launches and
survives on the x86_64 emulator.

- **The obstacle looked fatal.** `android/app/build.gradle:135` is
  `abiFilters "arm64-v8a"` and the only system image on this box is
  `android-34/google_apis/x86_64`. A lane claimed the emulator could run it; I did
  not believe that, and I was right to check — but the claim turned out to be
  **true**. Android 11+ x86_64 images carry ARM translation:
  `ro.dalvik.vm.native.bridge = libndk_translation.so`,
  `ro.enable.native.bridge.exec = 1`, and
  `ro.product.cpu.abilist = x86_64,arm64-v8a`. `adb install` of the existing
  debug APK returns **Success**. `/dev/kvm` is accessible, and the AVD boots in
  about 40 s.
- **It genuinely runs, and reaches its own UI.** Launched
  `org.metroidprime.port/.MetroidPrimeActivity` (note: a custom activity, *not*
  SDL's `SDLActivity`, which is what I guessed first and got
  `Error type 3` from). Logcat shows `SDL_main` running from
  `lib/arm64/libmetroid_prime_port.so`, the Dawn cache in use, the pipeline cache
  seeded with 444 rows, 11 texture replacements loaded, and
  `Displayed ... MetroidPrimeActivity for user 0: +853ms`. The process was still
  alive at t=100 s, and on a fresh install the top activity is
  `com.google.android.documentsui/.picker.PickActivity` — the app is correctly
  asking for the disc through SAF. That is the right behaviour, and it is *not*
  the "exits on a remembered `content://` URI" symptom from the earlier device
  run, which was a different situation: an install that already had a stale
  remembered URI.
- **What it does not prove, and the distinction matters.** Everything is
  ARM-translated onto an x86_64 CPU and rasterised by SwiftShader. So this
  settles packaging, native loading, WebGPU initialisation and the Java/SAF
  lifecycle. It settles **nothing** about frame rate, about how a real phone GPU
  driver behaves, or about whether touch input feels right. Performance and input
  still need a device, and I should not let this finding quietly turn into a
  claim that Android is "verified".
- **The trust store question is settled, and it was the one that mattered.** The
  plan for Android `wss://` needs to enumerate the CA certificates itself,
  because `SSL_CTX_set_default_verify_paths` on Android points at a compiled-in
  `OPENSSLDIR` that does not exist, returns success, and loads nothing — so the
  port's existing "could not load the system TLS trust store" check never fires
  and every public server then fails obscurely. From inside the app's own
  sandbox: `/apex/com.android.conscrypt/cacerts` lists **134** certificates and a
  real read returns the `-----BEGIN` PEM header. The files are
  `u:object_r:system_security_cacerts_file:s0` and the app runs in
  `untrusted_app`, so SELinux permits it. `/system/etc/security/cacerts` has the
  same 134 as a fallback, and the names are 8 hex digits plus `.0`
  (`01419da9.0`).
- **Consequence for the remaining work.** The last device-free engineering item
  is now buildable *and* verifiable, and the emulator is the vehicle:
  `docs/ANDROID_BUILD_PROBE.md` has the commands, what to conclude from them, and
  the one expected-benign `ndk_translation` log line. Recipe for reuse:
  `build/emu-probe.sh` (can it install?) and `build/emu-launch.sh`.

## ANSWERED: the corrupt save was MP_FAST_BOOT stranding a half-written file (2026-09-26)

The question left open by `51ab7244` — why the in-game save screen finds a corrupt
file on a card that should be empty — was mine, and the answer is the port's own
test flag. **It is a harness artefact, not a port defect.**

- **The mechanism.** On an empty card the front end's save screen makes the file in
  two async steps: `StartFileCreate()`, and when the create finishes
  `UpdateFileCreate()` calls `StartFileWrite()` (`CMemoryCardDriver.cpp:302`).
  `MP_FAST_BOOT=1` called `TransitionToFive()` the moment file select was current
  (`CFrontEndUI.cpp:1699`), which dropped the save screen after the create had
  produced a zero-filled file but before the write landed — a two-frame window.
  The stranded file reads back with a stored CRC of `0` against a computed
  `0x553E7B06`, and the **next** run reports `kUIT_SaveCorrupt`.
- **The differential** — card wiped before each run, nothing else changed:
  - fast boot, 1800 frames → `MetroidPrime A.gci`, save region **0 / 3004**,
    CRC **invalid** (`00000000` vs `553e7b06`)
  - front end with no fast boot → **no file at all**; it never gets past the
    title, so it never touches the card. A weak control, kept only for symmetry.
  - the player's path — press Start, then wait at file select → `MetroidPrime
    A.gci`, save region 0 / 3004, CRC **valid**. This is the real comparison, and
    the same create-then-write there leaves a good file.
  - fast boot **twice, no wipe between** → the same stranded file, and the second
    run logs `ui type 1 -> 11`. That is the sequence I actually hit.
- **A player cannot reach it.** File select only reads input
  (`CFrontEndUI.cpp:909`) and draws (`:936`) once `x10c_saveReady` is set, which
  is `GetUIType() == kUIT_SaveReady` (`:871`) and is reached only after the write
  completes, with `BusyWriting` shown in between. Fast boot was the only thing
  skipping that gate. The console had the same exposure to power loss mid-create,
  and no input can trigger it here — so this closes the item, though it does not
  make the two-frame window disappear, only unreachable.
- **Fixed rather than documented.** Fast boot now waits at file select for the
  same thing the player waits for: `SaveReady`, or a card dialog (where nothing is
  in flight, so it goes on without answering it), or no save UI at all. Confined to
  the `PortDebug::FastBoot()` guard — one hunk, and no other file under `src/`
  touched. After the fix, a wiped card and a fast-boot run leaves a blank file
  with a **valid** CRC, and a second fast-boot run reaches `1 -> 16` (SaveReady)
  where it used to reach `1 -> 11` (SaveCorrupt). The player's path is unchanged.
- **The 600-frame bound is in front-end frames, not seconds.** Card I/O is
  main-thread and advances one step per `Update`, so with `frame_limit=0` it passes
  far faster than 10 s. A measured create plus write is 10–12 frames, so the margin
  is ~60×. If the bound ever fires it prints to stderr, because a fallback that
  quietly reintroduces this bug must not be silent — it did not fire in any run.
- **`tools/card_inspect.py`** is now in the tree, because the evidence above rests
  on it and it was in gitignored `build/`. It reports the save region's non-zero
  bytes and checks the CRC the right way. Note `CCRC32::Calculate` is a **raw**
  CRC-32 with no final complement, so the check is
  `(zlib.crc32(payload[4:]) & 0xFFFFFFFF) ^ 0xFFFFFFFF` against the payload's
  first 4 bytes big-endian. Comparing against plain `zlib.crc32` makes a good file
  look corrupt; that mistake cost two commits and is now stated in the tool.
- **Loose end, harmless:** `build/card-fastboot-leftover/` holds a blank test file
  from a fast-boot run. Safe to delete.

## CORRECTED: the empty save is my automation, not the port (2026-09-26)

My last entry said the slot is never built and implied a port defect. **That
premise was wrong.** A focused read of the code says the in-game save never needs a
file-select list, and every reference checks out.

- **`StartGame` is not on the in-game path at all.** Its only callers are the
  title screen's file list (`CFrontEndUI.cpp:973`) and the new-game popup
  (`:1307`, `:1315`). The in-game save goes somewhere else entirely.
- **The in-game save confirms at `kUIT_SaveReady`.** `CSaveGameScreen.cpp:599`:
  when the context is not the front end and `userSel == 0`, it calls
  `BuildExistingFileSlot(gpGameState->GetFileIdx())` and then
  `StartFileCreateTransactional()`. `BuildExistingFileSlot`
  (`CMemoryCardDriver.cpp:782`) creates the slot from the live game state when it
  is empty.
- **So the blank file is created *by the screen itself*, with no user input.**
  `CSaveGameScreen.cpp:392`: when the driver reports `kS_FileBad` with
  `kE_FileMissing`, the screen calls `StartFileCreate()` directly. That path calls
  `InitializeFileInfo()` with no slots built - which is exactly the
  `slots 000, 0 of 3004 bytes non-zero` I kept seeing - and it is the 0->2 write.
- **Which means my driver was one press short.** It pressed A once, at the
  "corrupt" dialog, which is choice 0 - *delete the bad file*. That produced the
  self-service blank create, and the run ended before the screen reached
  `kUIT_SaveReady`, so the real confirm never happened. A second press, at type 16,
  is all it takes: my trace ends at `2->16` with no `advance: ui type 16` line.
- **The fix is in the tree already**, written by the lane before it timed out:
  `PortSmokeSaveScreenUI` watches the screen's own UI type and confirms at
  `kUIT_SaveCorrupt` and again at `kUIT_SaveReady`. My own run of it reached the
  delete and the self-service create, then was SIGKILLed at exit 137 because the
  investigation lanes were running the same binary and `pkill`ing it.
- **What I got wrong, precisely.** I assumed the missing piece was a file-select
  list, because that is how the *front end* loads a save, and I carried that
  assumption into the in-game path without checking. Two different screens, two
  different flows. The evidence was all there - the trace showed the flow
  *completing* - and I read "the slot is never built" instead of "the automation
  never chose a slot, because on this screen there is no slot to choose".

## Reading the save screen's UI trace: it never offers a slot (2026-09-26)

`CSaveGameScreen::EUIType` (include/MetroidPrime/CSaveGameScreen.hpp:23) makes the
instrumented trace readable: 0 Empty, 1 BusyReading, 2 BusyWriting, 11 SaveCorrupt,
16 SaveReady. The in-game run logged

    ui type 1 -> 11   BusyReading  -> SaveCorrupt
    advance: ui type 11, choice 0   the corrupt dialog answered with choice 0
    ui type 11 -> 0                 back to Empty
    ui type 0 -> 2                  Empty -> BusyWriting
    ui type 2 -> 16                 BusyWriting -> SaveReady

So the flow completes — read, report corrupt, delete, write, ready — and the file
that comes out has a 3004-byte save region of zeros. **The slot is never built.**

- **The likely shape of it.** `CMemoryCardDriver::BuildNewFileSlot` is what
  populates a slot, and its only caller is `CSaveGameScreen::StartGame`
  (`CSaveGameScreen.cpp:437`), reached from the file-select list once the player
  picks a slot. The trace never shows a slot list at all: the blind A press
  answers "delete corrupt file" (choice 0) and the screen then goes straight to
  BusyWriting. Answering the corrupt dialog is not the same as choosing a slot, so
  `StartGame` is never called and `InitializeFileInfo` later finds all three slots
  null.
- **This is very likely the real "fresh card" problem**, and I dismissed the
  original report too quickly two turns ago. The distress-beacon reading was right
  about the *message* and wrong to conclude there was nothing behind it. A card
  with no save gets a file created for it, the file reads back as all zeros, the
  CRC is 0 against a computed `0x553E7B06`, and the game says *corrupt*. On a
  genuinely fresh card the game should be offering to create a save, not
  apologising for a broken one.
- **What I am not claiming:** that `BuildNewFileSlot` is broken, or that the port
  cannot write a real save. The trace is consistent with the *automation* bypassing
  the slot-selection step rather than with a defect in the save writer. Separating
  those needs a run that actually selects slot 1, which is what the second lane is
  building - a walker that drives the front end from its own screen state instead of
  pressing A blindly.

## CORRECTED AGAIN: the save file is intact and the save is EMPTY (2026-09-26)

Two commits ago I wrote "saving is sound - written, read back, CRC-verified". The
file is sound. **The save is empty, and that is the real blocker.**

- **The payload, region by region**, from the file the port had just written:

  | region | bytes | non-zero |
  |---|---|---|
  | CRC | 4 | 4 |
  | comment | 64 | 47 |
  | banner/icon | 5120 | 4847 |
  | **save data** | **3004** | **0** |

  4 + 47 + 4847 = 4898 - exactly the "4898 non-zero bytes" I reported two commits
  ago. The number was right and I read it as "the save is substantial". It is
  entirely the banner and the icon. **The 3004 bytes of actual save data are
  zeros.**
- **So the CRC retraction was necessary but not sufficient.** A valid CRC proves
  the container is intact; it says nothing about the contents. I asked "is this
  file self-consistent?" when the question was "does it contain a save?" Two
  different questions, and only the second one matters here.
- **Confirmed independently** by instrumentation of
  `CMemoryCardDriver::InitializeFileInfo`, which reported
  `slots 000, 0 of 3004 bytes non-zero`, and `BuildCardBuffer`, which reported
  `save 3004 bytes (0 non-zero)`.
- **What that means.** A save "succeeds" - the file is created, the banner and icon
  are drawn, the comment is stamped, the CRC validates - and carries nothing. So
  the front end's file list has no real save to offer, Continue has nothing to
  load, and "saves and reloads" is broken on all three platforms. The slot is
  never built: `CMemoryCardDriver::BuildNewFileSlot` is what populates one, its
  only caller is `CSaveGameScreen::StartGame` (`CSaveGameScreen.cpp:437`), and at
  the moment of the write-back `InitializeFileInfo` found all three slots null.
- **On the delegation.** Handing this to a second lane was the right call. It
  pushed past the container and asked the serialiser what it had produced, which is
  the question that mattered, and it built a walker that presses from the front
  end's own screen state instead of guessed frame numbers - the correct shape of
  fix for a problem I had been losing runs to. Its first run timed out mid-rebuild;
  its work was in the tree and legible, which is how the diagnosis was recovered.

## A valid save survives a front-end run; Continue still not shown (2026-09-26)

With the CRC retraction settled, the experiment is finally clean: a save the game
itself wrote (4898 non-zero payload bytes, raw CRC matching the stored value) is
put on the card, and the front end is run with `MP_FAST_BOOT` **off** without
touching the card.

- **The save survives.** After 20000 frames the card still holds 8192 payload
  bytes, 4898 of them non-zero, raw CRC `C2C18A63` equal to the stored value. So
  an earlier front-end run leaving a zeroed file behind was a consequence of
  *those* runs being killed at the card dialog, not something the front end does
  to a good save unconditionally.
- **Saving is therefore sound**: written, read back, CRC-verified, and untouched by
  a front-end pass. The write and read paths agree.
- **What the front end did**: title, then a frame carrying the in-game visor HUD,
  then a badly corrupted frame, then back to `[ PRESS START ]`. The HUD frame is
  consistent with the attract movie or a file-select transition, but it is not the
  main menu, and no Continue was demonstrated in this run.
- **A new observation worth chasing**: frame 16000 is a magenta/orange wash with
  heavy scanlines over otherwise plausible geometry - not a transient fade. The
  notes already carry an open item about an "intermittent skinned geometry
  explosion"; this may be the same class of problem, and it appeared without fast
  boot, on a path that had not shown it before. Screenshot kept as
  `docs/images/front-end-file-select.png`.
- **Still open:** the main menu and its Continue. Everything that made the earlier
  attempts untrustworthy is now fixed - real GPU headroom, a real display or a
  fast Xvfb path, scripted input that is not discarded by the focus gate, and a
  card that holds a real save - so what remains is the walk itself.

## RETRACTED: there is no CRC bug, and I claimed there was (2026-09-26)

Two commits ago I reported that "a save the port writes fails the game's own CRC
check", and then localised it. **Both were wrong**, and the error was mine, in the
measurement rather than in the port.

- **What I did.** I read the saved `.gci` file and compared its stored CRC against
  `zlib.crc32` of the payload. It did not match in any byte order, so I concluded
  the write and the read covered different bytes.
- **Why that was wrong.** `CCRC32::Calculate`
  (`src/Kyoto/CCrc32.cpp:38-45`) is a **raw** CRC-32: it seeds `0xFFFFFFFF` and
  iterates the reflected table, but it never applies the final complement.

  ```cpp
  uint checksum = 0xFFFFFFFF;
  while (length-- > 0)
    checksum = (checksum >> 8) ^ gkCRC32Table[(checksum ^ *(buf++)) & 0xFF];
  return checksum;      // no ^ 0xFFFFFFFF
  ```

  `zlib.crc32()` *does* apply it, so the two differ by exactly `0xFFFFFFFF`.
- **The check that settles it**, on the file the port had just written:

  ```
  zlib crc32(payload[4:])      : 3D3E759C
  raw  = zlib ^ 0xFFFFFFFF      : C2C18A63   <- CCRC32::Calculate
  stored, big-endian read       : C2C18A63
  ```

  An exact match. The write and the read agree, and the earlier run's pair agreed
  the same way (`0xFD1235EB ^ 0xFFFFFFFF == 0x02EDCA14`, the value the instrumented
  build printed). **The save path is correct.**
- **So the "corrupt" dialog was right all along.** Every front-end run creates the
  save slot and then dies before deciding what to do with it, leaving 8192 zero
  bytes with a stored CRC of 0. Against a computed raw CRC of `0x553E7B06` that is
  a mismatch, and the game is correct to say so. My *first* diagnosis — a
  half-created file, not a save bug — was right, and I discarded it on the strength
  of a comparison against the wrong CRC function.
- **The lesson worth keeping:** I had a plausible mechanism, an instrumented run
  with real numbers, and a write-up, and it was still wrong, because the one
  assumption I never checked was that `CCRC32` is zlib's. I compared against a
  library function instead of the one the code actually calls. Reading the
  function would have taken a minute and saved two commits.
- **Consequence for the objective:** nothing here blocks saving. What is still
  unproven is the *front end's* Continue, and the obstacle is unchanged and
  mundane: front-end runs clobber the card, so the dialog always finds a bad file.

## The Flatpak has an AppStream description (2026-09-26)

- `packaging/io.github.odrannnn.metroidprimeport.metainfo.xml`, installed to
  `share/metainfo/` by the project's own install rules, so the AppImage and a
  package-manager install carry it as well as the Flatpak. It passes
  `appstreamcli validate --no-net`; the only remaining complaint is the component
  id's uppercase, which is the placeholder app id and has to change anyway.
- Two things the validator caught that would have shipped: `Mixed` is not an SPDX
  identifier, so `project_license` is now `LicenseRef-mixed` — which is also the
  only honest value, since the port's own code is MIT, the linked components
  carry their own terms, and the decompiled game code is not redistributable at
  all, and no single SPDX id can say that. And a component with no `url` is a
  warning, so the repository is now linked as the homepage.
- A false claim corrected: the notes here used to say "`MP_AUDIO_STATS=1` reports
  mixer rate/clipping". **No such flag existed** — grepping the tree for audio
  flags turns up only `MP_STREAM_LOG`, `MP_STREAM_TRACE`, `MP_STREAM_TRACE_LOG`
  and `MP_DISABLE_AI_AUDIO`. It was documented and never implemented, which is
  worse than a missing feature because it reads like a way to check the thing.
- **The two remaining Flathub blockers are not code.** The app id must change and
  be lowercased, and there can be no screenshots: any screenshot of the running
  game shows Nintendo's game, which this package may not redistribute. That one
  cannot be fixed by writing a file, and pretending otherwise would put
  copyrighted imagery in a release.


## Every "front end" capture this session was the game's intro, not the front end (2026-09-26)

`MP_FAST_BOOT=1` does not merely skip loading. In `CFrontEndUI::Update` it drives
the front end forward without waiting for input:

```
OpenCredits -> Title -> FileSelect -> TransitionToFive()   // straight into a new game
```

`TransitionToFive()` goes to `kP_ToPlayGame`, so **no run with fast boot set can
ever reach Continue**, and none of them shows the front end at all. Every capture
I took this session had `MP_FAST_BOOT=1` in it.

- What I described as the front end — the publisher screen, the Dolby logo, the
  "Unidentified distress beacon" text, the landing site — is the **game's**
  opening. The distress-beacon conclusion survives, and is if anything
  strengthened: it is game narration, and the differential showed the frames
  byte-identical with and without a save on the card.
- What does **not** survive: "the front end renders correctly, the only black
  frame is a fade". I never saw the front end. The old note that "GPU captures
  show the publisher screen, [ PRESS START ] title screen, and no-memory-card
  dialog rendering correctly" should be read as *the game's intro* until
  re-checked without fast boot.
- The card made no difference to the front-end runs for the same reason. A
  differential with a save and without one produced **byte-identical mean RGB at
  every captured frame**, which is what a bypassed menu looks like.
- The pause-screen and save-screen captures are unaffected: those are reached
  through `EnterPauseScreen()` and `EnterSaveGameScreen()`, not through the front
  end, and they render the real screens.
- **Blocked on the GPU right now.** With fast boot off, the front end needs more
  device memory than the fast-boot path and fails as
  `VK_ERROR_OUT_OF_DEVICE_MEMORY` on `GX Static Texture` with about 1.1 GB free —
  a stale LM Studio `llama-server` on the development machine is holding 15124 of
  16303 MiB. Software Vulkan (lavapipe) segfaults inside Dawn's surface setup, so
  that is not a way round it. Retrying once the board is free is all that is
  needed; nothing about this is a port defect.


## Saving works; loading through the title menu does not (yet) (2026-09-26)

The last unverified part of "saves and reloads" was the save path itself. It is
now exercised end to end, and one part of the load path is still open.

- **Where saving happens, and why that matters.** Not from the pause menu:
  `CPauseScreen::ESubScreen` is LogBook, Options, Inventory, ToGame, ToMap, with
  no save among them, which matches retail — Prime saves at a save station. A
  save station's trigger is a script special function calling
  `CStateManager::EnterSaveGameScreen()`, deferring `kSMT_SaveGame`, which
  `CMFGame::Think` turns into `CMFGame::SaveGame()` and a
  `CSaveGameScreen(kSC_InGame)`. So `MP_SMOKE_SAVE=<ticks>` calls
  `EnterSaveGameScreen()` and takes exactly that path: real screen, real file
  write, real card. Only the walk to the trigger is skipped.
- **A save is genuinely written.** With an empty card, the run creates the file
  and writes it: the payload stops being 8192 zero bytes and its comment field
  reads `Metroid Prime                   09.26.26  15:18`. On the next boot the
  card layer opens slot A and the game does **not** call the file corrupt — only
  slot B fails, which is correct, nothing has ever been written to it.
- **A mistake I made, and what it looked like.** My first run opened the save
  screen and never confirmed it, and the game then reported "The Metroid Prime
  save file on the Memory Card in Slot A is corrupt and must be deleted." That
  is not a port bug: the file was an 8192-byte buffer of zeroes with a stored CRC
  of 0, created by `CARDCreate` and never written. The game was right. Worth
  recording because the symptom — a fresh card reporting a corrupt save — looks
  exactly like the bug the objective asks about, and I nearly wrote it up as one.
- **Why the confirm could not be injected where I first put it.** The port hooks
  live in `CStateManager::Update`, which only runs from the `kSMT_InGame` branch
  of `CMFGame::Think`. Opening a menu takes the game out of that state, so a hook
  is never called again while a menu is up — the confirm could not happen from
  there at all. The press now comes from `PortSmokeFrame`, which runs from the
  main loop whatever state the game is in, and the request schedules it in the
  same invocation that makes it.
- **Tooling added**, all opt-in and off by default:
  - `MP_SMOKE_SCRIPT=<at>:<buttons>[:<hold>],...` — scripted button presses at
    chosen frames, because menus cannot be walked any other way. Names are
    `start a b x y up down left right z l r`, summed with `+`.
  - `MP_SMOKE_SAVE=<ticks>` — request the in-game save screen and confirm it.
  - `PortSmokeCurrentFrame` — the frame loop's counter, so a hook that is not
    handed a frame can ask what frame it is on.
- **Menu facts established by looking rather than guessing.** `EnterPauseScreen()`
  opens the **Inventory**, not a list. **B** leaves it; **Z** does nothing, even
  though the on-screen prompt says Z is BACK. The Inventory itself renders
  correctly — Samus, Arm Cannon, Morph Ball, Suits, Visors, Secondary Items,
  Power Beam, and the button prompts.
- Two bugs in my own script parser, both caught by checking the log rather than
  the exit code: `strtol` needs a mutable `char*` for its end pointer, and a loop
  guard on the step index meant the second and later steps were never reached at
  all. A third "failure" was my `grep | head` truncating the output while the
  steps had in fact all fired.
- **Still open: the title screen's Continue.** A save on the card is accepted by
  the card layer and by the in-game save screen, but the game resuming a loaded
  save from the front end is not demonstrated. A Start-tapping script runs past
  the title menu into the intro before a shot can catch it, so this needs a more
  deliberate walk than a repeated press, and has resisted a first attempt here.


## Frame pacing, measured — and the number that was not what it said (2026-09-26)

- Measured over 24 samples of 60 frames each, under Xvfb with the dummy audio
  driver: **16.667 ms/frame, 60.00 FPS presented**, p99 16.672 ms, jitter
  p99-p50 of **5.6 us**, and **0 of 24** intervals more than 1 ms late. Pacing is
  not a problem on the Linux path.
- Uncapped (`F10`): 137.0 FPS presented, p99 10.534 ms. Simulation stays at 60
  ticks/s, because it is fixed-step; uncapped presentation renders the same
  simulation more often rather than advancing it faster.
- **The reported rate was measuring the wrong thing.** `RecordFrame` was handed
  `SDL_GetTicksNS() - loopStartNs` *after* the frame-cap sleep, so the pacing wait
  was folded into the frame's own cost. The one number the port reported was
  therefore the presented rate whatever the machine's headroom was, and "is the
  CPU the problem" could never be answered from it. The frame's work is now
  measured before the sleep and both rates are reported: presented (per second of
  wall time) and throughput (with the wait excluded).
- The check that the split is real: uncapped, the two must agree, and they do —
  137.0 and 137.0. At the cap they differ by 21x, which is the number worth
  having when something stutters.
- My first measurement script had the arithmetic inverted — it computed frames per
  second and then printed the result as milliseconds per frame, so a 60 FPS run
  reported as 16.67 FPS. The raw number was right and the label was wrong, which
  is the worst combination; it is stated in the script so it cannot be
  reintroduced silently.

## The end-of-tour Vulkan failure is a busy GPU, not VRAM retention (2026-09-26)

- A full eight-world tour ended in
  `vkAllocateMemory failed with VK_ERROR_OUT_OF_DEVICE_MEMORY` while creating a
  `GX Static Texture`, which read like textures never being released across
  hundreds of world restarts. Measured instead:
  - Reproduced in a **one-world, one-area tour, 23 frames in**, immediately after
    `sweep: complete` and not before it — the opposite of accumulation.
  - The port's own process holds **118 MiB** of device memory while running, and a
    normal run reached 57+ frames with no failure at all under the same
    conditions.
  - `nvidia-smi` shows an LM Studio `llama-server` holding **15124 MiB of
    16303 MiB** — about 1.1 GB free for everything else.
- The one thing here worth keeping: the port aborts on the fatal rather than
  degrading, and the message does not say what was actually short, so a player
  whose GPU is busy sees "WebGPU error 3" and nothing about memory. Not fixed —
  the honest fix and the smaller-attachment fallback both belong in vendored
  Aurora.


## The "unidentified memory card" is a misreading (2026-09-26)

A fresh card was reported as *unidentified* in the Android front end, and
`docs/ANDROID_BUILD_PROBE.md` carried that as an open bug. It is not a bug.

- **What the text actually says.** Capturing the front-end sequence
  (`MP_SMOKE_FRONTEND` to walk it, `MP_SMOKE_SHOT` to shoot it) shows the screen
  reading **"Unidentified distress beacon has been transmitted"**. That is
  Metroid Prime's opening narration — the premise of the game is a distress
  beacon — and the sentence has nothing to do with the memory card. It is not in
  any source file in this tree, because it comes from the disc's string table.
  Screenshots kept at `build/distress-beacon-frame.png` and
  `build/front-end-landed.png`.
- **It does not depend on the card at all.** The capture was run twice, once with
  no card and once with a real save on it. Frames 1, 3 and 5 — including the
  distress-beacon frame — are **byte-identical** between the two. Only the last
  shot differs, which is the game loading further.
- **The card is genuinely read.** The card layer distinguishes the two states on
  its own: with a save present it opens slot A silently, and with an empty card
  it logs `Failed to open file: MetroidPrime A`. Same code path Android uses,
  because `__ANDROID__` is a Clang predefine for every NDK target — checked with
  `-dM`, and the string that only exists inside that branch is present in the
  arm64 `.so` from the built APK.
- **Three plausible "is there a save?" checks all failed**, each caught by the
  same negative control (delete the save, expect the answer to change):
  - `gpMemoryCard->GetMemoryWorlds()` — 8 worlds with or without a save. It is
    the set of worlds the *game* knows about, not the set it has saves for.
    `MP_SMOKE_WORLD=auto` picks its target from this list, so it looks like a
    card test and is not: it chose the same world `13D79165` from an empty card.
  - `CSaveWorldMemory::GetSaveWorldAssetId() != kInvalidAssetId` — non-invalid
    for all 8 worlds either way, so it does not mean "a save was loaded".
  - `CMemoryCardSys::GetNumFreeBytes` — 16728064 bytes and 126 files free either
    way, because GCI-folder mode does not account usage that way.
  The reliable discriminator is the one the front end uses, a null file slot in
  `CMemoryCardDriver::GetGameFileStateInfo`, and the driver only exists while the
  save screen is up, so it is not reachable from a per-frame hook.
- I wrote a `MP_SMOKE_CARD` hook around the first two, then **deleted it**: it
  printed the same thing in both states, which is worse than no hook because it
  looks like a check and is not one. The negative control is what caught it.
- **Still unverified:** loading a save *through the front-end menu*. That needs
  driven menu input, not a card query, and it is the remaining part of
  "saves and reloads".


## The memory card now says where it is (2026-09-26)

- A fresh card was reported as **unidentified** in the Android front end. **I did
  not find the cause.** What follows is what was ruled out, and what was done
  about it anyway.
- Ruled out on desktop: a fresh card works. With no card at all, a run reaches
  49+ frames, logs the two expected "Failed to open file: MetroidPrime A/B"
  lines for a save that does not exist yet, and creates
  `USA/Card A/01-GM8E-MetroidPrime A.gci`. The same run with a card already
  present behaves identically.
- Ruled out as the cause: the `__ANDROID__` branch. It is genuinely compiled -
  `__ANDROID__` is a Clang predefine for every NDK target, checked with `-dM`,
  and the string that only exists inside that branch is present in the arm64
  `.so` from the built APK.
- The silent failure I *did* find: a slot's card path is default-constructed,
  and `CARDInit` fills an empty one from aurora's user path - which is the
  `MP_USER_PATH` environment variable, **unset on Android** - or failing that
  from the process's working directory. On Android the working directory is not
  writable. A card under `/` is not a card: every write fails, the front end
  shows one it cannot identify, and nothing logs where it was meant to be. This
  is a real hole, but nothing observed suggests it is *this* report's cause.
- What changed: the resolved directory is logged on every platform
  (`memory card: storing under <path>`, which reaches logcat via `PortLog`), the
  Android branch falls back to `SDL_GetAndroidInternalStoragePath()` when
  `SDL_GetPrefPath` cannot answer, and if no directory can be resolved at all it
  says so and leaves the card uninitialised so a later call retries rather than
  leaving the card permanently unavailable.
- The next device run should start with that one log line. If it names a
  directory the app can write, the cause is elsewhere and this is a dead end;
  if it does not appear at all, the card is never reaching this code.
- I also corrected a comment I had written here first. I claimed the paths stay
  empty without `CARDSetBasePath`; `CARDInit` does fill them, just from somewhere
  worse.


## A licence, and the Flatpak could not have worked (2026-09-26)

Two release blockers, and the second turned out to be hiding a third.

### The licence

- `LICENSE` is an MIT grant over **this project's own work** - `platform/`, the
  build and packaging scripts, `tests/`, `tools/`, `docs/` - and `NOTICE` states
  what that deliberately leaves out: `src/` and `include/` are a recompilation of
  the retail game, no permission to redistribute them is asserted anywhere, and
  the grant stops at the repository's own code rather than sweeping those
  directories in.
- A blanket licence over the whole tree would have been a false claim, and a
  grant over nothing would have been no claim at all. The grant now also travels
  with every package - `port-license.txt` and `port-notice.txt` in the AppImage,
  the APK's `assets/` (verified: 8 notices) and the Windows `dist/licenses/` -
  because a grant nobody can read inside the package is not much of a grant.
- What the licence does **not** do is answer what may be done with a working copy
  of decompiled game code. That stays an open question for the copyright holder,
  and the docs now say so rather than implying the file settled it.

### The Flatpak was not merely unbuilt, it was broken

- The manifest declares `command: metroid_prime_port` and
  `buildsystem: cmake-ninja`, whose only install step is the build system's own.
  **The project had no `install()` rules at all.** So a Flatpak build would have
  succeeded and produced an application with no executable in it, and anything
  else installing this project would have got an empty tree. The docs described
  it as "never built here", which was true and understated.
- Fixed with install rules in `CMakeLists.txt` and a desktop entry in
  `packaging/`. The data files go to `bin/` beside the executable because that is
  where the port looks for them: `DefaultTexturesPath()` resolves
  `SDL_GetBasePath() + "textures"` and Aurora opens `initial_pipeline_cache.db`
  by name beside the binary. They are there by design, not by accident.
- **Verified by running the installed tree**, not the build tree, because that is
  the layout a Flatpak, a package manager and the AppImage all get: 44 frames,
  no texture errors, and `Seeded pipeline cache from
  '<prefix>/bin/initial_pipeline_cache.db' (444 rows merged)`. If the port could
  only find its data in a build tree, the install rules would be wrong and no
  packaging of it would work.
- The manifest's `post-install` collects the third-party notices, and the desktop
  entry is installed by CMake rather than patched with `desktop-file-edit` after
  the fact - which would have failed, since no desktop file existed to patch.
- Still unproven: nothing here has run `flatpak-builder`, there is no AppStream
  metainfo, and the app id is a placeholder.

### A regression caught while checking the docs

- Claiming "the opt-in produces an installable package" without building it hid a
  regression. With no key, the opt-in permitted the debug key but named no
  signing config at all, so AGP emitted `app-release-unsigned.apk` - which Android
  will not install. That is precisely the path a device test uses, so it was the
  worst possible place for it to be wrong. Setting *no* config is not a softer
  alternative to the debug key; it is a package that cannot be sideloaded.


## Android release signing has an identity of its own (2026-09-26)

- The `release` variant was signed with the shared debug key. It is now signed
  with a key of this project's own, created by `tools/make_android_keystore.sh`,
  and the build *refuses* the debug key unless it is asked for. The reason the
  refusal is worth having: Android treats a signature change as a different app,
  so a debug-signed release has to be uninstalled before a properly signed one
  will install. That is a much worse thing to learn after shipping than at build
  time.
- `tools/android_apk.sh` still opts in on the caller's behalf when no key is
  configured, because a build that is only going to be sideloaded should not
  need a key to exist first, but it announces it on stderr. `--strict-signing`
  turns that into a refusal. The rule is Gradle's, so calling Gradle directly
  with no key stops the build regardless of the script.
- Three things that were each a bug while getting there:
  - `signingConfigs` only exists inside the `android { }` block, so the values
    are resolved outside and the config is built inside. Calling
    `signingConfigs.create` at the top level fails to evaluate the project.
  - Naming the script-level variables `keyAlias` and `keyPassword` shadowed
    `SigningConfig`'s own accessors of those names, and Groovy resolved
    `keyAlias <value>` as calling the local String: `No signature of method:
    java.lang.String.call()`. They are now `configuredKeyAlias` and
    `configuredKeyPassword`.
  - `--strict-signing` has to be *consumed* by the script, not forwarded:
    Gradle rejects an option it does not know, so the first version failed with
    a usage dump instead of doing what it said. The POSIX way to drop one
    positional parameter without an array is to rotate `$@`.
- Also fixed while here: a half-configured key (three of the four values) was
  going to fall back to the debug key silently. It is now an error naming which
  values are missing, and a `storeFile` that is not there names the path.
- Verified by building and inspecting the package: signer
  `CN=Metroid Prime Port, OU=Port, O=Metroid Prime Port` (SHA-256 `d8814c79...`),
  not the debug key's `CN=Android Debug` (`edd22fdb...`); arm64 `.so` 29 MB; all
  six third-party notices in `assets/`; no `.iso`, `.pak` or `.strg`. A dry run
  of `assembleDebug` with no key at all still configures, so the rule is scoped
  to release and does not get in the way of a normal debug build.


## DeathLink, both directions (2026-09-26)

- A `Bounce` from another player now kills this one, and this client's deaths
  are announced. It needed no new mechanism: the world's own client reacts to a
  bounce by clearing the high bit of the player state's alive flag, which is
  `SetPlayerAlive(false)` here, and that already drives the whole death
  sequence in the game - `CPlayer::Think` keys the music, sound and morph-ball
  handling off it.
- Three details that were each a bug first:
  - A bounce was *owed* to the game rather than applied on the socket thread, so
    one arriving during a load or at the title screen is not lost. Taking the
    owed count clears it, so it cannot be applied twice.
  - A bounce whose `source` is this client's own slot is the echo of a death it
    already announced. The first version still counted those, so DeathLink would
    have killed the player a second time per death.
  - The pending announce was moved-from rather than cleared, and a
    moved-from-but-not-cleared string is still non-empty. The socket loop runs
    every iteration, so the same death was announced on every pass: four times
    in a twenty-second run, once after the fix.
- Opt-in through `"death_link": true`; `BuildBounce()` returns nothing when it
  is off, so a session that never asked for it emits nothing. Eleven new
  assertions in `port_apclient_tests`, and verified end to end against
  `tools/ap_fake_server.py --bounce`: a live run receives a bounce from another
  slot, dies, and announces exactly one death.
- Wire format fixed (2026-09-28). The first version spoke a made-up format that
  the fake server shared, so the end-to-end run proved nothing against a real
  server: it sent an untagged `Bounce` (routed to nobody) with an integer
  `source` and no `time`, never put `DeathLink` in the Connect tags (so the
  server never relayed deaths to it), and listened for `Bounce` although servers
  relay as `Bounced`. It now follows the AP DeathLink spec; the echo is spotted
  by `source` matching this client's slot name. `--bounce` takes a player name.


## Layer sweep: the stall is the game leaving kSMT_InGame, not the sweep (2026-09-26)

- The layer-cycling sweep works - Chozo Ruins has up to 7 layers per area, and
  each pass over an area with a different layer active finds pickups the
  one-layer dump did not (5 new keys in the first multi-layered area alone). The
  give-up diagnostic and the wait reasons are worth keeping for that reason.
- What it does not survive is a world area that leaves the game out of
  `kSMT_InGame`. `CStateManager::Update` - and therefore every port hook,
  including this sweep - only runs from the `kSMT_InGame` branch of
  `CMFGame::Think`. When the layer flip on one area pushes the game into another
  deferred state (a save, message or map screen), the tour stops receiving
  ticks entirely: frames keep going, nothing else does, and no guard in the
  sweep can notice because the sweep is never called. Instrumenting each of the
  three early returns showed none of them firing, which is what identified it -
  the absence of a message, not a message, was the evidence.
- So the fix is not in the sweep. It needs the area to be *unloaded* before its
  layer is changed - a real area teardown, not a bit flip - and the game to be
  back in `kSMT_InGame` while it happens. Until that exists, the reliable way to
  dump every layer is several seeded runs with the save in a different state,
  merged, which is the same reason `docs/RANDOMIZER.md` says to merge dumps.
- Left in place: the layer cycling, the wait reasons, and the 10s give-up. All
  are off by default or inert without `MP_RANDO_SWEEP_LAYERS`, and each of them
  turned "the tour is stuck" into a sentence naming what was outstanding.


## Layer-cycling sweep: it works, and it exposes why the dump was short (2026-09-26)

- `MP_RANDO_SWEEP_LAYERS=1` visits each area once per layer with only that
  layer active. Chozo Ruins reports up to **7 layers** per area, and the extra
  passes are what the old one-layer-per-area tour could not do: `CGameArea`
  builds the objects of the layers that are active *when the area is
  constructed*, so a pickup behind an inactive layer was never in the dump.
  After eight areas the new dump has 5 keys the old one did not, all in the
  first multi-layered area (`D5CDB809`: 0002035C, 0002035F, 00020361, 00020365,
  00020367).
- The tour then stalls, and the new diagnostics say why in one line:
  `[sweep] waiting on area 8: loading (target 165A4DE9, pass 9/64)`. The layer
  state is only a bit flip (`CScriptLayerManager::SetLayerActive`), so
  re-entering the *same* area re-enters an area that is already alive: its load
  was never scheduled again, and the loading chain head never advances. Cycling
  layers needs the area unloaded first, not re-entered, so the next pass has to
  travel away and come back rather than teleport onto the area it is changing.
  That is the outstanding work, and the diagnostic is what found it - a tour
  that never settles was previously invisible, just frames.
- 14/14 tests pass. The layer cycling is off by default and changes nothing for
  a normal sweep.


## The AP world was reachable all along; the join maps 95 of its 100 locations (2026-09-26)

- I had recorded the Archipelago world as unreachable and built a spec-shaped
  stand-in for it. That was wrong, and the reason is worth remembering: the
  repository is `UltiNaruto/MetroidAPrime` — **one "P" in Prime**. The name I
  had, `MetroidAPPrime`, is a different repository, and GitHub answers a request
  for a name that does not exist with the same 404 it uses for a repository you
  cannot see. Every probe I ran used the double-P spelling, so "the repo is
  private" was the only conclusion the evidence supported, and it was wrong.
  `git ls-remote` on the single-P name returns refs immediately.
- Against the real `src/Locations.py` and the full port dump, `make_ap_config.py
  --strict` maps **95 of 100 locations across 75 clean areas**, and the five it
  does not map are all dump coverage rather than pairing:
  - `Chozo Ruins: Main Plaza - Locked Door` and `Chozo Ruins: Ruined Shrine -
    Plated Beetle` have no counterpart in the dump at the area's delta (+1):
    those pickups live in layers a plain area tour does not build, which is the
    limit `docs/RANDOMIZER.md` already describes.
  - Three more are count mismatches — an area holding more AP locations than the
    dump has pickups (Chozo Ruins 0x0025, Phazon Mines 0x001A).
  - Five areas with a mixed delta stay flagged **review** (Chozo Ruins 0x001C,
    0x0031, Phendrana Drifts 0x0035 and the two above), which is why `--strict`
    refuses to write a config from this dump rather than pairing on rank.
- The real table confirms the id-delta design the tool was built around: the
  measured delta is a per-area constant (Tallon Overworld Life Grove pairs at
  **+1**, and 75 areas are clean), and a wrong pairing would report another
  player's check, so reporting these five is the correct behaviour rather than a
  shortfall.
- Closing the five needs a dump that visits those layers, not a better join.
  `--extra` can name them by hand for a session that does not include them.


## Item tracker, and the on-disc rewrite re-verified (2026-09-26)

- An item arriving while the player was not looking at the HUD used to be lost
  for good: the notification queue is capped at 32 and drained by whoever shows
  it. The session now keeps the receipts - display name, sender, progressive
  step - and the F1 overlay's Archipelago section lists them. No protocol change
  was needed; the data was already in `Session`, and the sender alias was already
  resolved for the notification text.
- One subtlety worth recording: an `ItemEntry`'s flat `display` field mirrors
  step 0, so reading it directly labelled every copy of a progressive item
  "Power Beam". The tracker resolves the step the grant actually used. Eight new
  assertions cover names, step numbering, an unknown item id, copies past the
  last step, and sender attribution.
- The 100-location randomizer path was re-verified end to end with the current
  binary: the full dump feeds `tools/rando_seed.py` (100 locations, 33 models),
  and a seeded run of the game rewrites pickups on disc, logging e.g.
  `PLACE 39F2DE28:B2701146:0000007E Missiles -> Wavebuster amount=1 capacity=1
  model=74A39FE6 acs=7C04E388` and an energy tank replacing a missile at
  `B9ABCD56:000801FB`. Six such rewrites across fourteen areas of the Tallon
  Overworld, each carrying a model and animation from the seed.
- Worth remembering when testing this by hand: `MP_RANDO_DUMP` takes precedence
  over `MP_RANDO_SEED`, and dump mode returns before any placement is applied.
  Running both makes the run look like it ignored the seed. A seed run wants
  `MP_RANDO_SWEEP=1` and no `MP_RANDO_DUMP`.


## A spec-shaped AP world table, so the location join is exercisable (2026-09-26)

- The join tool had nothing to run against: the upstream world's repository
  answers for metadata but 404s every content path, so `--strict` could only be
  driven by the tool's own built-in self-test fixture.
  `tools/make_ap_fixture.py` writes the two files `make_ap_config.py` reads - a
  per-world location table and a `PICKUP_LOCATIONS` list of level and entity ids
  - from a real port dump, and `tools/ap-world-fixture/Locations.py` is that
  generated file.
- What is real in it: every entity id, the area each lives in, the vanilla item,
  and the count of 100, which is retail Prime's number, all taken from the
  sweep. What is synthetic and documented as such: the AP location ids,
  assigned from 50310000 so none can be mistaken for a real one, and the names,
  which are derived from the dump. It stands in for the upstream table, it is
  not a copy of it.
- Verified end to end rather than merely generated: `--strict` over this table
  and the full dump reports **100 locations mapped, 0 unmapped, 82 areas clean,
  0 for review, 0 missing from the dump**; the config it writes carries all 100
  locations and 51 items with the four progressive beams; and the port connects
  with that generated config against `tools/ap_fake_server.py`, receives the
  three items it offers and records the progressive step (`next_item_index` 3,
  `progressive {"5031043":1}`).
- `tests/port_ap_fixture.py` is registered as a ctest target so this cannot rot:
  it checks the generator reproduces every location from the same dump, that
  `--strict` maps all of them, and that the written config carries every
  location, passes the server through and still emits four progressive beams.
  It builds its dump from the fixture, since a real sweep log is far too large
  to commit. 14/14 port tests pass on Linux and Windows.


## Android APK builds again, and the Archipelago client was missing from it (2026-09-26)

- `tools/android_apk.sh :app:assembleRelease` works from this tree with the
  cached SDK/NDK and produces an 11 MB RelWithDebInfo APK carrying
  `lib/arm64-v8a/libmetroid_prime_port.so`, with no disc image or game asset
  inside. All the toolchain is present: SDK at `~/android/sdk`, the pinned NDK
  at `~/android-ndk-cache/android-ndk-r29`, Gradle 8.7 in the user's wrapper
  cache, and a Rust toolchain under `build/android-rust`.
- It did not build at first: `platform/port_ws.cpp` used `IPPROTO_TCP` without
  including `<netinet/in.h>`, where Bionic declares it (glibc and Winsock both
  get it from `<netdb.h>`). So the entire WebSocket client — and therefore the
  entire Archipelago feature — was absent from every Android build until now.
- `wss://` on Android remains impossible as configured: the NDK ships no
  OpenSSL, so there is no `MP_HAVE_OPENSSL` and the client refuses a `wss://`
  server instead of downgrading it. A JNI `SSLSocket` backend or a vendored TLS
  library is the remaining work.
- The port's diagnostics were `fprintf(stderr, …)`, which Android discards, so a
  device run that exited during startup reported nothing. They now go to logcat
  under the `metroidprime` tag. That is what made the disc-path failure below
  readable rather than invisible; desktop output is unchanged.
- On-device findings from one run of the built APK, recorded for whoever picks
  this up: it installs and launches, Vulkan initialises on the device GPU,
  the surface and framebuffer come up (2351x1056), texture replacements load,
  and then it exits. The reason was a remembered `content://` disc URI that no
  longer opens — and because that is exactly what the port remembers, every
  later launch fails the same way. The fix is to ask again when a remembered
  `content://` URI fails to open; it is not applied because it cannot be
  verified without a device, and on-device testing is out of scope for now.


## wss:// proven by a handshake on Windows, not just a configure line (2026-09-26)

- The TLS end-to-end test skipped itself on Windows, so `wss://` there was a
  configure line saying OpenSSL 3.6.4 was found. It now runs there, ported
  rather than duplicated: a small platform shim covers the four POSIX-only
  spots (shell quoting for cmd.exe, a temp directory made from
  temp_directory_path, a Server object that is fork/exec on POSIX and
  CreateProcessA on Windows, and WSAStartup/closesocket for the free-port
  probe). FreeLoopbackPort deliberately never calls WSACleanup, matching
  `PortWs::EnsureWinsock` — a cleanup between the probe and the client connect
  would drop the reference count to zero.
- Three failures surfaced only by actually running it, all of them real:
  - `CreateProcessA` does not interpret `>`, so the server's log redirection
    was passed to python as arguments and it exited before binding. The log is
    now a CreateFileA handle passed through STARTF_USESTDHANDLES.
  - The "certificate for another host" case connected to `localhost`, which on
    Windows can resolve to `::1` where this IPv4 server is not listening, so it
    timed out instead of reaching the certificate check. It connects to
    127.0.0.2 with the server bound to all IPv4 interfaces: still loopback
    everywhere, still not in the certificate, and nothing depends on how a
    machine resolves a name.
  - The job then went green once and failed on the next run with "TLS handshake
    timed out" while all four rejection cases worked seconds later — the server
    was still starting. The test now polls a plain TCP connect for up to thirty
    seconds before the first handshake, and prints the server's log at the point
    of failure rather than only at the end.
- Evidence, in the CI log rather than inferred: Linux and Windows both complete
  a verified handshake and all four rejections, and both jobs now run
  `port_ws_tests` directly because ctest only prints a failing test's output.
  `wss://` on Android is untouched and still needs a TLS backend.


## Windows and Linux CI green over the randomizer and Archipelago work (2026-09-25)

- All 28 commits of randomizer and Archipelago work had never been through CI,
  because they sat unpushed. The first Windows run over them failed in five
  places, all of them things only clang-cl sees:
  - `platform/port_ws.cpp` includes winsock2.h, whose windows.h `min`/`max`
    macros break every `std::max` in the file. `NOMINMAX` now goes in first.
  - `tests/port_randomizer.cpp` and `tests/port_apclient.cpp` used `<unistd.h>`,
    `fork` and `waitpid`. The randomizer's four cases that need an unloaded
    process now re-run the test binary with the case name and directory as
    arguments (fork/execv, CreateProcessA), and both files' pid and environment
    helpers are spelled per platform.
  - The self-spawn built its Windows command line by wrapping narrow pointers in
    `std::wstring`, which has no such constructor; and `argv[0]` is whatever
    ctest used, which need not be a path the process can spawn itself by, so the
    executable's own path is read with `GetModuleFileNameA` on Windows.
  - The test then died with `0xc0000409` and no output. Phase markers named the
    phase, and the bytes named the cause: the test read the child's dump log
    with `std::ios::binary` while the port appends through a text-mode
    `ofstream`, so on Windows the file ended CRLF and no retry could match a
    line ending in a bare LF. That was a bug in the test, not the port; the
    markers, exit-code reporting and byte dump stay because they are what turned
    an unexplained fast-fail into that one line.
- Windows is now green: build, `ctest -L port` 13/13, the FIFO regressions, the
  packaged startup check and the artifact upload. `native-linux` is green on the
  same commit, and the configure step reports **OpenSSL 3.6.4 found: wss://
  enabled** on Windows, which is the first evidence that `wss://` is available
  there rather than merely possible. The TLS end-to-end test still skips itself
  on Windows, so that is a configure-level fact and not yet a handshake.


## Archipelago state belongs to a session, not to a file (2026-09-25)

- The recorded checks and item index were only invalidated by a slot change, so a
  state file left over from a different multiworld was silently replayed: the
  client claimed locations the save never collected and skipped the items the
  server still owed it. `archipelago_state.json` now records the seed name from
  the server's `RoomInfo`; a different seed discards the progress, logs why, and
  continues from empty. A file with no seed in it (written before this) adopts
  the server's and keeps its progress, so no one loses a session to the upgrade.
- Verified on disc against `tools/ap_fake_server.py`: a state file recorded
  against "MP Seed Alpha" connecting to the fake seed is discarded, the file is
  rewritten empty with the new seed, and the server sees no `LocationChecks` for
  the stale location; the same file with the server's own seed keeps its
  progress and re-sends the check on connect. Eight new assertions in
  `port_apclient_tests` cover both paths, the round trip and the legacy file.
- `MP_AP_RESET_STATE=1` discards the state before connecting. A new game or an
  older save on the same slot *and* seed is indistinguishable from continued
  progress, so this stays the manual way out; the memory card exposes no play
  time or save identity to detect it with.

## Impact Crater sweeps clean; the Wayland hang is not the port (2026-09-25)

- Impact Crater (`C13B09D1`) is no longer the sweep's stopping point. It reaches
  all 12 of its areas (`[sweep] complete: 1 worlds, 12 areas`) and dumps 85
  pickups, all `capacity=0` drops, so it contributes 0 item locations — which
  agrees with retail, where the crater holds no items. Every world is now
  covered, and one clean tour of all eight (`complete: 8 worlds, 276 areas`,
  ~12 minutes) yields the whole table: 2542 LOC lines, 1333 distinct pickup
  keys across 180 areas, and exactly **100 capacity-granting item locations** —
  50 missile expansions, 14 energy tanks, 5 power bomb expansions and the rest of
  retail's set, which is the count vanilla Prime has. `tools/rando_seed.py`
  turns that single dump into a seed with all 100 locations and 33 models.
  The earlier "4969 lines / 328 locations" figures came from merging ten partial
  runs and counted the same locations repeatedly; one tour needs no merging.
- A tour that long ends in `vkAllocateMemory failed with
  VK_ERROR_OUT_OF_DEVICE_MEMORY` while the frontend loads its first texture, and
  the process aborts. The dump is already complete when that happens, so it is
  cosmetic for the sweep, but a full eight-world tour is a ~37000-frame session
  and VRAM is not returned as fast as it is taken.
- The "unreproduced free" that ended earlier crater sweeps was never in the port.
  The run had stopped making progress because SDL3 was using its Wayland backend,
  where `SDL_ShowWindow` dispatches into libdecor's client-side decorations and
  never returns; the process sat at 100% with no frames and no log output past
  `Using surface format`. A backtrace of a hung instance pinned it, and
  `SDL_VIDEODRIVER=x11` runs normally (3481 frames in 60 s). Documented under
  "A Wayland session hangs at startup" in `docs/NATIVE_PORT.md`; no code change.
- Consequence for tooling: the sweep recipe needs `DISPLAY`, `XAUTHORITY` and
  `SDL_VIDEODRIVER=x11` on a GNOME Wayland session; unattended runs that set only
  the first two hang silently instead of failing.

## World restart crash, full location sweep, audio bug (2026-09-25)

- Restarting the current world used to crash after about ten restarts: the
  retiring mapper kept updating after its PAK's resource table was retired and
  locked a new STRG against it, dereferencing a null `CDvdFile`. `CStateManager`
  now stops the outgoing simulation as soon as a restart is requested and
  `CAutoMapper::Update` returns while `GetWantsToQuit()`, so no scripts or loads
  run against the retired world. Fifteen consecutive restarts are clean under
  ASan (`MP_SMOKE_WORLD_RESTARTS`, `MP_SMOKE_WORLD_TICKS`).
- `MP_RANDO_SWEEP=1` (+ `MP_RANDO_DUMP=1`) walks all eight worlds and their
  areas in one run (~12 minutes) and now produces a full dump in a single pass —
  see the section above for the counts and what they mean. `tools/make_ap_config.py
  --strict` maps a matching AP table with 100/100 entries and 0 areas for review,
  and the seed tool derives 33 item models from it.
- Two port-side fixes were needed in vendored code along the way:
  - `extern/aurora/lib/gx/shader.cpp`: a texcoord generator whose source
    register is still `GX_MAX_TEXGENSRC` ("no source", what the game leaves
    behind for a disabled texcoord) aborted the process. It now emits a constant
    texcoord, matching the existing skip in the shader dump.
  - `SStreamInfo::x0_fileName` and `CDSPStream::x10_fileName` were non-owning
    pointers into a `CDSPStreamManager` that is reassigned and freed while the
    stream is live, so `activate ok file=...` printed garbage and a long sweep
    died with `free(): invalid size`. Both are now owned `rstl::string`s.
    The sweeps that still ended in Impact Crater were not failing at all; see
    the section above.

## Archipelago progressive items (2026-09-25)

- The AP world's progressive beam items work: a config entry can carry
  `progressive`, a list of grants applied in order as copies arrive, and the
  count lives in `archipelago_state.json` so a reconnect resumes at the right
  step (verified on disc: two copies of id 5031043 recorded
  `"progressive":{"5031043":2}`). `tools/make_ap_config.py` emits the four beam
  sequences and maps the tracker-only ids 47-50 to the Charge Beam.
- The HUD notification names the resolved step ("Charge Beam"), not the item id,
  so a progressive grant still reads sensibly.
- Note for future edits to `tests/port_apclient.cpp`: JSON fixtures that contain
  `)"` (for example `"Charge Beam (Power)"`) must use a delimited raw string
  (`R"json(...)json"`), otherwise the fixture ends early and the file stops
  compiling.

## Archipelago over TLS (2026-09-25)

- `wss://` works through OpenSSL, enabled by CMake when it finds it (`OpenSSL
  found: wss:// enabled`) and optional otherwise: Android refuses `wss://` with
  a clear error instead of downgrading to plaintext. Verification is always on
  (system trust store or the config's `tls_ca`, TLS 1.2 floor, host-name check);
  there is deliberately no insecure switch.
- `port_ws_tests` runs a real TLS handshake against `tools/ap_fake_server.py
  --tls` with a generated CA and asserts four rejections (wrong CA, host-name
  mismatch, system store only, missing CA file); with the game, a `wss://`
  config connected and granted items, and a missing CA was refused.
- Worth knowing: OpenSSL writes with `write()`, which raises SIGPIPE when the
  server has gone, so the TLS calls block it on the calling thread and swallow
  one that arrived, rather than letting a dead server kill the process.

## Archipelago generator, status and notifications (2026-09-24)

- `tools/make_ap_config.py` joins the AP world's location table (parsed with
  `ast`) to a port location dump and writes `archipelago.json` plus, from the
  spoiler, a `randomizer_seed.json` whose remote-item locations become
  zero-amount placeholders (the real item arrives over the network). Locations
  are joined by the entity id's area index; the report prints the per-area id
  delta, `--strict` refuses mixed-delta areas, and unpaired areas are reported
  rather than guessed. Verified against the real AP data shape: Chozo Ruins
  Main Plaza flagged for review (`+1,+1,+1,+3`), Ruined Fountain clean at `+1`.
- Received items and server messages now show as HUD memos, one every two
  seconds, using the config's optional per-item `display` name; `PortAp` also
  exposes the seed name and a notification queue for the F1 overlay's Session
  tab (connection, seed, items, checks, last message).
- Observation: the port's screenshot capture does not include Aurora's ImGui
  layer — a front-end run with `MP_SHOW_DEBUG_UI=1` produced a screenshot with
  no overlay, as every checked-in screenshot also has none — so the overlay
  cannot be verified from a capture. The HUD (game-drawn) notifications are
  captured normally.

## Archipelago client (2026-09-24)

- The port can join an Archipelago multiworld natively: a background thread owns
  a minimal RFC 6455 WebSocket client (`platform/port_ws.*`) and the AP JSON
  protocol (`platform/port_ap_protocol.*`); the game reports collected pickups
  from `CScriptPickup::Touch` (`PortAp::QueueCheck`) and receives items in
  `PortAp::Poll`, called from `CStateManager::Update` next to the other port
  hooks. `CPlayerState::InitializePowerUp`/`IncrPickUp` are the grant calls, the
  same ones the retail pickup makes.
- Verified end to end against `tools/ap_fake_server.py` (a dependency-free
  WebSocket AP server): handshake and Connect, items granted into the player
  state (the HUD missile readout went from 15 to the granted 250),
  `LocationChecks` sent, and `archipelago_state.json` remembering the processed
  item index so a reconnect does not re-grant. See `docs/ARCHIPELAGO.md`.
- Only plain `ws://` (no TLS) and no per-message compression, which Archipelago
  marks deprecated; DeathLink, hints, chat, an in-game status line and a
  generator for the location/item id maps are still missing. `PortWs` was
  written because the server speaks WebSocket, not raw TCP, and no WebSocket
  dependency is vendored.
- Follow-up: `platform/port_json.*` is now a general JSON parser while
  `platform/port_randomizer.cpp` still carries its own seed parser. Folding the
  seed reader onto `PortJson` would leave one parser in the port.

## Randomizer: pickup models and drop filtering (2026-09-24)

- Rewritten pickups now draw the item they grant. `tools/rando_seed.py` derives
  a `models` map from the dump and the seed's `locations`; `LoadPickup` copies
  the entry into the pickup's static model and animation parameters exactly as
  the area data does. Verified on disc: an energy tank swapped into a missile
  location resolved to `model=86908399 acs=F37BCBC7`, both `ANCS` assets.
- Real item pickups are animated (`ANCS`) in the retail data; `model` and `acs`
  are both set and the engine prefers the animation, so the rewrite mirrors
  both fields instead of picking one.
- Areas also list their enemy drop templates as pickups (health and ammo
  refills with `capacity=0`). They are not item locations; the seed tool filters
  them by default (`--include-drops` keeps them). Confirmed against Chozo Ruins
  and Tallon Overworld dumps.
- Item models come from a dump, so a full dump gives full model coverage; an
  item the dump never saw keeps its retail model. See `docs/RANDOMIZER.md`.

## Item randomizer proof of concept (2026-09-24)

- Item pickups can be rewritten from a seed file at load time. The gameplay
  hooks are in `ScriptLoader::LoadPickup` (rewrite the item/capacity/amount
  before `CScriptPickup` is constructed) and `CScriptPickup::Touch` (record the
  check); the port layer is `platform/port_randomizer.cpp` with
  `MP_RANDO_DUMP` / `MP_RANDO_SEED`. Nothing changes when no seed is set.
- Verified on a real USA v1.00 disc: dump mode logged a live pickup
  (`LOC 39F2DE28:B2701146:0000007E Missiles amount=5 capacity=5`, Tallon
  Overworld landing site) and a seed moved an item to that location
  (`PLACE ... Missiles -> EnergyTanks amount=1 capacity=100`). See
  `docs/RANDOMIZER.md` for the format, the `tools/rando_seed.py` helper, and
  what is still missing (placement logic, pickup models, non-pickup item
  grants, check persistence, the Archipelago transport).
- Observation, not from this feature: restarting the world repeatedly (the F1
  overlay's world teleport path) crashed the ASan build after about ten
  restarts, in `CDvdFile::IsARAMFileLoaded` under `CAutoMapper::Update ->
  CStringTable::Lock`. Worth reproducing on a normal build before trusting the
  repeated world-teleport path.

## Remote-test regressions (2026-09-19)

- A CachyOS/Radeon 8060S crash in `CCubeMaterial::GetFlags` during door opening
  was reproduced locally by evicting/restoring area 0 geometry. Surface headers
  were converted from big endian again after an ARAM round trip. Conversion is
  now tracked for the lifetime of the area payload, independently of rebuilding
  its model instances. Three repeated eviction/restoration cycles pass under ASan.
- The old tick-remainder heuristic could produce only 45 simulation ticks/second
  at 60 rendered FPS with ±5 us jitter. A double-precision fixed-step accumulator
  now preserves fractional time and bounded scheduling debt. Runtime logging
  measures about 60 render FPS / 60 simulation TPS when capped, and 60 TPS when
  uncapped. F1 shows measured rates separately from the target setting.
- Free mouse movement now translates A/D instead of using console turning torque
  which mouse look immediately overwrote. Input is normalized, movement respects
  native physics, and mouse heading is applied before movement without consuming
  the same delta twice. The smoke driver verifies strafe before and after F1.
- MusyX ambient loops active in the first area included non-block-aligned loop
  starts (e.g. sample 832) and loop ends before the resource length. The software
  decoder now advances predictor history sample-by-sample, uses the actual loop
  end and loop context, and preserves streaming history across circular-buffer
  wraps. Signed PCM8 and wide intermediate Q15 products were corrected too.
  Golden-sample tests pass; confirmation of the audible static fix on the remote
  PulseAudio setup is still needed.
- Build revisions are generated at build time and exposed by `--version`, the
  launch log and F1. `MP_TRACE_TIMING=1` logs actual render and simulation rates.

Diagnostics and reproduction flags are documented in `docs/NATIVE_PORT.md`.
Validation: nine native checks pass in GCC and Clang/ASan builds. The combined
real-disc run passes three area reload cycles and the mouse scenario including
free strafe before/after F1, and exits cleanly. Steady capped timing reports
60 FPS / 60 TPS; uncapped presentation retains 60 TPS. Runtime leak detection
was disabled; allocation mismatch and invalid-access detection were enabled.

## Mouse aim / arm-cannon integration (2026-09-19)

- Mouse mode now uses immediate pitch/yaw rather than the retail 60°/s camera
  easing. Pitch is limited to ±1.52 radians, yaw is normalized, and independent
  X/Y inversion controls are available (normal, non-inverted Y by default).
- Playable first-person state owns mouse aim. Lock-on tracks the effective camera
  and discards hidden deltas; cinematics, scripted input locks, morph ball and
  menus relinquish ownership and rebase on return. Jump/fall camera pitching no
  longer replaces mouse pitch. The GC crosshair is independent of holding R.
- LMB drives fire/charge/release, RMB drives lock-on, and MMB drives missiles via
  the ordinary PAD input path. Existing mappings remain available. UI/focus
  changes cancel charging and require a neutral mouse-button state on recapture.
- Uncapped free mouse aim uses current rotation with interpolated translation.
  Held weapon geometry and muzzle effects use a simulation-time view, restoring
  the world view afterwards; the old assertion below about automatic viewmodel
  anchoring has been corrected.
- The real-disc mouse smoke scenario verifies firing, charged shots, missiles,
  lock-on release, jump/morph handoffs, crosshair state, and uncapped cannon/view
  alignment. It also exposed and fixed ImGui cloned draw-list allocator mismatch
  and a deferred MusyX stream stop that could read a freed host stream buffer.

See `docs/NATIVE_PORT.md` for controls, opt-outs, and the opt-in smoke driver.

Validation: GCC and Clang/ASan builds plus all six native regression executables
pass. The real-disc mouse run produced normal/charged/missile shots, observed live
projectiles, held lock-on for 60 ticks, checked 947 weapon views, and completed
jump, cinematic interruption, UI cancellation and morph/unmorph handoffs. A separate
1,800-frame mouse-disabled lifecycle run passed. Runtime ASan leak detection was
disabled; allocation mismatch and use-after-free checks remained enabled.

## Current native-port hardening (2026-09-19)

Current build/run instructions are in [docs/NATIVE_PORT.md](docs/NATIVE_PORT.md).
The dated sections below are a historical bring-up log, not a current blocker
list or reproducible build guide.

- Aurora/MusyX are now vendored source snapshots with exact provenance in
  `extern/README.md`. Clean clones do not depend on unpublished fork commits.
- The native game uses an OBJECT target and no duplicate-symbol suppression.
  Windows packaging includes runtime DLLs; Linux GCC/Clang and Windows clang-cl
  CI run asset-free regression tests.
- Native font resources are read from the validated user's DOL at startup.
  Generated asset arrays are no longer a native-build prerequisite.
- Host arrays and game-heap owners have explicit, different deleters. GUI,
  animation, collision, movie, resource and streaming owners have been corrected.
- MusyX addresses stay pointer-sized on Windows; group resources are pinned only
  while pushed, voices are retired under the IRQ mutex, mute is atomic, and
  shutdown joins the mixer before freeing its state. Missing devices run silently.
- AI audio supports enable-after-disabled startup, callback self-unregistration,
  and explicit teardown. Failed frame acquisition skips rendering and frame-based
  retirement; hidden windows keep a bounded event pump.
- DVD/ARAM state changes share a mutex, failed reads terminate with diagnostics,
  and resource EOF, AGSC section bounds, PATH counts/indices/ranges/cycles and DMA
  ranges are checked. Empty retail PATH resources are supported.
- Mouse capture ignores overlay/unfocused motion; the cursor-warp workaround is
  removed. Slow frames use bounded fixed-step catch-up. Native reset avoids the
  console reboot/cancel-all path.
- Save bit fields are MSB-first; CRC writes are big-endian, with legacy native CRC
  reads accepted. Save compatibility still needs a directed retail round trip.
- CARD initialization supplies the four-byte game ID; optional callbacks are
  null-safe, unmount commits successfully, and CARD readiness handles absent
  channels. A filesystem regression covers format/create/write/status/rename,
  physical remount/read/delete with the game's null-callback usage.

Validation: GCC 15 and Clang 19 builds; asset-free CTest regressions; a real-disc
2,400-frame AddressSanitizer lifecycle run through hide/restore, audio re-enable,
uncapped rendering, restart, and clean exit, with allocation mismatch checking
enabled. Leak detection was disabled for that runtime run. Windows runtime and
full-game traversal remain separate validation tasks.
All 205 GPU-free FIFO/GX tests also pass under ThreadSanitizer. Full-game TSan
reaches reports in uninstrumented GLib/libdbus/nod startup paths first, so it has
not validated the full game/audio/DVD thread interaction.
The CARD filesystem regression passes with null callbacks and a physical
remount. An isolated real-disc profile produced `MetroidPrime A.gci` and was
loaded by a subsequent process. A clean source clone builds without a disc or
generated game headers; five native regression executables pass.

Goal: turn the Metroid Prime recompilation experiment into an actual, maintainable
PC port by building the PrimeDecomp matching decompilation against the Aurora
compatibility layer (MIT), instead of a static-recomp module on a Dolphin-derived
runtime.

## Architecture

- Game + engine: PrimeDecomp/prime `src/` + `include/` (real C++, targets
  `GM8E01_00`, the same disc we own).
- Platform/GPU/input/disc/saves/UI: `extern/aurora` (MIT). Aurora provides a
  drop-in `dolphin/*` SDK API plus an SDL3 + WebGPU(Dawn) app layer and a
  performant GX implementation.
- Assets: read from the user's own disc (`orig/GM8E01_00/`); nothing is
  distributed.
- Integration reference: Dusklight (Twilight Princess port) — same CMake shape
  (`extern/aurora`, decomp sources listed in a `files.cmake`, `aurora::*` libs).

## Status (2026-09-18)

- Repo cloned, branch `port`; `extern/musyx` and `extern/aurora` submodules present.
- Matching decomp builds and reproduces the retail DOL byte-for-byte:
  `build/GM8E01_00/main.dol` sha1 `949c5ed7368aef547e0b0db1c3678f466e2afbff`.
  Report: SDK 100%, Core Engine (Kyoto) 90.99%, Game 87.50% matched.
  `objdiff.json` is available for reference diffing.
- Aurora `examples/simple` builds on this machine (prebuilt Dawn for
  linux-x86_64 is fetched automatically by CMake).

### Port scaffold build status (2026-09-18)

`CMakeLists.txt` + `files.cmake` (632 sources) + `platform/{compat.h,main.cpp}` build
`mp_game` against Aurora. First full compile went 1269 -> 138 errors after:
- `platform/compat.h` (force-included) restoring the SDK `AUTO`/`AUTO_REF`/
  `AUTO_CONST_REF` macros Aurora omits;
- excluding `src/NESemu/modwrapper.cpp` (raw PowerPC asm) and
  `src/MetroidPrime/TypesMatch.cpp` (decomp-only type scaffolding);
- `-Wno-narrowing`.

Remaining 138 errors are a bounded compatibility-shim queue:
- GX declarations Aurora implements but does not declare in headers:
  `GXSetTexCopyDst/Src`, `GXPixModeSync`, `GXCopyTex`, `GXSetTevColor`,
  `GXInitLightPos/Attn`, `GXSetCullMode`; plus a `GXSetArray` signature
  difference (Aurora adds a 5th arg).
- Missing SDK headers: `dolphin/arq.h`, `dolphin/thp/THPInfo.h`.
- `OSContext` member mismatches (`gpr`, `srr0`) in the game's own view.
- `COBBTree::CNode::operator new` placement-new mismatch.

### Build clears (2026-09-18)

`metroid_prime_port` now **compiles and links**: all 632 decomp sources build
against Aurora with zero errors and produce a 64.5 MB executable that
initializes Aurora and a Vulkan device.

What clearing the remaining errors required:
- `platform/compat.h` (force-included): libc, the GX/SI/PAD/OS/CARD umbrellas,
  `AUTO*`, `nofralloc`, `__abs`, and the `triggerL/R` -> `triggerLeft/Right`
  mapping for Aurora's `TARGET_PC` `PADStatus`.
- `platform/include/dolphin/{arq,gba,PPCArch,thp/*}.h`: SDK headers Aurora omits
  (arq.h re-exports Aurora's `ar.h`; the rest are the original SDK declarations).
- `platform/include/dolphin/gx/GXShims.h` + `platform/shims.cpp`: GX token,
  breakpoint, and write-gather-pipe entry points Aurora lacks, plus GBA/PPC
  shims.
- Decomp source fixes (port-only, documented in code comments): `RAssertDolphin`
  OSContext dump guarded for opaque PC `OSContext`; `rstl/string.hpp` declares
  the member specializations `rstl_strings.cpp` defines (clang requires
  declaration before instantiation); `GXSetArray` call updated to Aurora's
  5-arg form; small conversion casts; `CARDFormatAsync` declaration only (Aurora
  defines it); case-corrected `Kyoto/CCrc32.hpp` include.
- CMake: `LINK_GROUP:RESCAN` around `aurora::core`/`aurora::gx` to break their
  static-library cycle; excluded `src/NESemu` (raw PowerPC asm).

### Boot progress (2026-09-18)

`metroid_prime_port` now boots well into initialization under Aurora:
1. Aurora initializes (Vulkan device, 2240x1680 framebuffer, CARD, ARAM `0x1000000`).
2. Disc mounts via `aurora_dvd_open` (user's ISO).
3. Game entry runs: `CMain` ctor, `RsMain`, `CGameGlobalObjects` ctor, default font
   (zlib) load, `InitializeSubsystems` (AR/ARQ), `PostInitialize`.
4. GX commands reach Aurora's FIFO worker and a frame is presented.

Issues fixed along the way:
- zlib ABI: the bundled zlib 1.1.3 declares a non-standard 3-arg `inflateInit2_`;
  retargeted to the standard 4-arg form so it matches the linked zlib-ng.
- `TOneStatic<T>` 1-arg `operator new` had no definition.
- The guest stack "paint" in `InitializeSubsystems` wrote to address 0 (no emulated
  guest stack); skipped it on PC and gave the dummy `OSThread` a MEM1 stack range.
- Aurora aborted when the game's error handler called `PADRead` before `PADInit`.

Current blocker: the game's custom `CGameAllocator` (main heap) returns null for a
64 KB allocation in `CDvdFile::StartARAMFileLoad` (loading `aram:Tweaks.pak`),
which triggers the error handler (and then Aurora's `PADRead before PADInit`
fatal). Two separate issues:

1. **Heap sizing (fixed).** Aurora's internal framebuffer defaulted to the
   window-scaled 2240x1680, so the game's two `x2c_frameBufferSize` allocations
   took ~15 MB of the 24 MB MEM1, leaving only a ~10 MB game heap. Pinning
   `windowWidth/Height` to 640x480 raised the heap to ~20 MB. (Aurora still
   scales the internal fb to 1120x840 @1.75; a true 640x480 fb needs the DPI
   scale forced to 1.)
2. **Allocator free-list (open).** With ~20 MB free the 64 KB allocation still
   fails, so `CGameAllocator`'s free-block/split bookkeeping is not surviving on
   the 64-bit host (`SGameMemInfo` packs flags in low pointer bits and uses
   `sizeof(SGameMemInfo)`, which is ~4x larger than on GameCube). Needs a focused
   pass over `FindFreeBlock`/`FixupAllocPtrs`/`AddFreeEntryToFreeList`.

Temporary memory diagnostics are in `CGameAllocator::Initialize` and
`COsContext::OpenWindow` (stderr prints of heap/arena/framebuffer sizes).

### Bring-up fixes after the allocator (2026-09-18)

The game now runs through `PostInitialize`/`AddPaksAndFactories` pak loading with
no crashes. Fixes landed:
- **`CGameAllocator` overflow** (root cause of the OOM): `Alloc(0x20)`/`Alloc(0x1c)`
  for `CSmallAllocPool`/`CMediumAllocPool` were 32-bit object sizes; on x86-64 the
  objects are larger and overflowed the next free block's header. Now use
  `sizeof(...)`. Free list stays healthy.
- **ARQ recursion**: Aurora's `ARQPostRequest` invoked the ARAM completion callback
  synchronously, so `CDvdFile::PingARAMTransfer <-> HandleARAMInterrupt` recursed to
  stack overflow. Callbacks are now queued and drained by an iterative `ARQPoll()`
  at explicit pump/wait points. Running the poll inside `ARQPostRequest` was still
  too early: multi-chunk transfers observed stale length/interrupt state and left
  `aram:MiscData.pak` permanently loading.
- **GX breakpoint / VI retrace**: `CGraphics::EndScene` spins on
  `mNumBreakpointsWaiting`, which only a VI retrace decrements. `GXEnableBreakPt`
  now pulses the registered breakpoint + pre/post retrace callbacks (stored by the
  VI shims) so frames complete.
- **`delete` on CMemory memory**: `rstl::aligned_allocator::deallocate` used
  `delete[]` while `allocate` used `CMemory::Alloc`; on clang the game's
  `operator delete`->`CMemory::Free` is MWCC-only, so glibc freed a game pointer.
  Routed the free to `CMemory::Free`; `CDvdFileARAM` buffers now use `rs_new`.

The apparent frame-slot deadlock was normal frame pacing. The repeated
`CGraphics::EndScene` caller was the initial pak-loading loop; after fixing ARQ
completion ordering and synchronous ARAM waits, startup reaches the main loop
(`MP frame` verified through frame 361). Resource buffers passed to `delete`-based
owners now use matching host allocations, and palette frame state is initialized
so ARAM palette storage follows delayed `CMemory::Free` cleanup.

Further host bring-up fixes now sustain the main loop through at least frame 48,601:
- GameCube AGSC payloads are big-endian 32-bit MusyX structures, while the vendored
  host runtime expects native-endian structures (including 64-bit sample directory
  pointers). `CAudioGrpSetLoc` now converts the pool, project, and sample directory
  into host-native layout on little-endian hosts: big-endian `GROUP_DATA`,
  `POOL_DATA`, `MEM_DATA`/`FX_TAB`, ID lists, and `SDIR_DATA_INTER` are byte-swapped
  and expanded to native `SDIR_DATA`; raw curve bodies and PCM samples are left
  untouched. MusyX group push/pop is enabled again and `sndPushGroup` succeeds for
  the boot groups. A `s32`->`size_t` cast in `dataAddSampleReference` fixes truncation
  of 64-bit sample bases.
- CMDL header fields, section sizes, bounds, and per-surface metadata are byte-swapped
  before model setup. Surface parent/next links are expanded in place for 64-bit hosts
  while raw GX display lists remain untouched.
- Movie buffers owned by `single_ptr`/`auto_ptr` use matching host allocations and
  rounded DVD request sizes instead of placing `CMemory::Alloc` pointers behind
  host `delete` owners.
- Aurora keyboard input has initial GameCube mappings when no saved mapping exists:
  WASD and IJKL drive the sticks, X/Z/C/V map A/B/X/Y, Return maps Start, and the
  arrow keys map the D-pad. Existing user mappings and physical controllers win.
- PATH version-4 resources are read field-by-field into native vectors. Packed
  big-endian node, link, region, connectivity, and octree fields are converted and
  their 32-bit indices are rebased only after vector storage is stable.
- CMDL and MREA geometry now share native surface-header conversion, including
  material indices, display-list lengths, pointer-sized renderer links, normals,
  and optional bounds.
- MusyX now uses the upstream `origin/sdl3` PC backend (merged into the vendored
  submodule, with conflicts resolved in favor of the host `s64` typedef and the SDL
  mutex IRQ). It provides a software voice mixer covering ADPCM/PCM decode, pitch
  resampling, ADSR envelopes, and studio/AUX mixing, and feeds interleaved `s16`
  stereo to an SDL3 audio stream. PC sequence playback is enabled again and the
  audio thread drives `snd_handle_irq`.
- Big-endian `ARR` song payloads from `CSNG` resources are converted in place for
  little-endian hosts before sequencing: header offsets, the 64-entry track table,
  per-track `TENTRY` arrays, the `MTRACK` tempo list, the pattern table, pattern
  headers, and `NOTE_DATA` note streams are byte-swapped. Byte-oriented pitch-bend
  and modulation streams need no conversion.
- Native text rendering checks explicit string lengths before dereferencing the
  next character. Palette entries, MREA section buffers, and map buffers now return
  to the allocator that created them during runtime and delayed shutdown cleanup.
- ~~AGSC group buffers are retained for the session on PC.~~ **No longer true.**
  This was fixed in `f2888a72` and the note above it had gone stale. The history,
  because it explains why the code looks the way it does: `hwSaveSample` never
  copies samples into ARAM here, and MusyX **2.0.0**'s `sndPopGroup` could leave
  voices referencing sample data after a group was popped, so freeing the buffer
  left the audio thread reading unmapped memory (confirmed with AddressSanitizer).
  The deliberate leak (`RetainAudioGroupBuffer`) that papered over it is gone from
  the tree.
- `CCameraFilterPass::DrawRandomStatic` previously faked a random main-memory
  address as its texture source (the GameCube renderer ignored the pointer, the PC
  renderer hashes it). It now samples a real scratch buffer of noise, sized for the
  tiled IA4 extent.
- DVD ARAM streaming state is serialized with a recursive mutex: `OSDisableInterrupts`
  is a no-op on PC, and the DVD worker and main threads raced on the transfer
  counters until `mBufferLen` went negative and `ARQPostRequest` memcpy'd a huge
  length. Non-positive transfer lengths are also treated as complete.
- Vertex array byte sizes are threaded through to `GXSetArray`. Aurora uploads
  `size` bytes of each attribute array, and the port was passing 0, so every
  array-based draw (MREA world geometry, CMDL models, skinned models) uploaded
  zero bytes and rendered nothing while immediate-mode effects and the HUD still
  drew. Positions, normals, colors, and UVs now carry the section sizes from the
  MREA/CMDL loaders.
- The PC MusyX mixer render thread is paced to real time. It previously free-ran
  (measured ~29x real time) because it only throttled on the SDL queue depth,
  overrunning the stream and producing dropouts; it now sleeps until the next
  160-sample frame is due, which removed the buffer-boundary discontinuities.
- The AGSC sample directory's trailing ADPCM info blocks are now preserved and
  converted for little-endian hosts, and each entry's `extraData` offset is rebased
  onto the native `SDIR_DATA` array (whose entries are larger than the disc's
  32-bit form). Without this, in-level voices decoded with garbage coefficients and
  produced noise.
- The disc sample directory ends with a 4-byte `0xFFFFFFFF` terminator rather than
  a full entry, so the ADPCM info blocks start at `(count - 1) * entrySize + 4`
  and not `count * entrySize`. The old base was 28 bytes too high, which skipped
  the first block and left samples whose information begins there reading
  coefficients from the entry table; the charge-beam looping layer (id 209) was
  the audible case and buzzed continuously. `MP_VALIDATE_SAMPLES=1` scans every
  loaded sample directory and reports any ADPCM sample whose rebased `extraData`
  is missing or does not hold `numCoef == 8` (verified clean across the front end
  and first areas).
- In-game streamed music now plays. The PC MusyX ARAM layer was entirely stubbed
  (`aramAllocateStreamBuffer` returned 0, `aramGetStreamBufferAddress` returned
  NULL, `aramUploadData` did nothing), so streamed voices got a null sample address
  and the software mixer skipped them; the stream never advanced and
  `UpdateStream` was never called. Each ARAM stream buffer is now backed by host
  memory and `hwFlushStream` keeps the full 64-bit host pointer instead of
  truncating it to `u32` (which faulted on the first upload).
- Streamed audio (front-end music) now decodes correctly: `DecodeMonoAndMix` wrote
  the decoded samples *on top of* the buffer still being played, feeding the
  output back into itself until it saturated into full-scale noise. It writes the
  decoded samples now, `IsReady` waits for every chunk rather than only the last,
  and the non-ARAM `CDvdFile` read is blocking (`DVDReadPrio`) so playback cannot
  start on unfilled buffers.
- `CARAMToken::UpdateAllDMAs` pumps `ARQPoll` before refreshing status. Aurora
  defers ARQ completion callbacks until `ARQPoll` runs on the main thread, but
  the map/pause texture eviction and room-transition code spin on a token
  (`while (texture.IsARAMTransferInProgress()) UpdateAllDMAs();`) and so never
  reached the main-loop poll; the DMA never completed and the game froze with
  audio still playing (opening the map always hung).
- The GameCube audio-interface DMA path is implemented in `platform/ai_dma.cpp`:
  the registered DMA callback is driven from the main loop (`AIPortPoll`) at the
  buffer rate and the submitted buffer is fed to its own SDL stream. Streamed
  audio (front-end/in-game music via `CStaticAudioPlayer`, movie audio)
  previously had no output at all because the AI functions were no-ops. It runs on
  the main thread because the guest mixer is not thread-safe, and the port keeps
  the full 64-bit DMA pointer that the SDK's 32-bit `AIGetDMAStartAddr` truncates.
  `CMoviePlayer::StaticMyAudioCallback` reads the previous DMA buffer through the
  same 64-bit accessor; the truncated form resolved to unrelated memory whose
  bytes were then mixed as audio. `MP_DISABLE_AI_AUDIO=1` isolates this path.
- Streamed in-game music (`Audio/*.dsp` software streams) was silent on Android
  only. ARM compilers default plain `char` to unsigned, so the `-1` companion
  sentinels in `CDSPStreamManager`'s `char` fields read back as 255: the
  header-read completion then took the companion path with a 255 index, read
  `g_Streams[255]` out of bounds and discarded the stream, so no streamed voice
  was ever created and all streamed audio (cutscene and area music) went quiet
  while MusyX sound effects kept working. CMake now passes `-fsigned-char` to the
  game, port and MusyX targets (`mp_signed_char`), matching the x86/Windows
  behaviour the port is verified against, and `platform/compat.h` asserts the
  invariant so losing the flag fails the build instead of muting the music.
- Streamed music stopped refilling part-way through a track and looped the few
  seconds it already had. `CDSPStream::BufferStream` started the async disc read
  before publishing either `xec_readsPending` or the destination-half selector,
  and the completion runs on Aurora's DVD worker thread: a fast completion
  decremented the count before this call assigned it, wrapping the uchar to 255,
  after which every refill saw a read outstanding, `UpdateStream` returned 0
  forever and the mixer looped its buffer. The guest serialized this with
  `OSDisableInterrupts`, a no-op on PC, so both values are published before the
  read starts, and a completion for a read the stream no longer owns is dropped
  instead of counted down. Verified on device: the 99-second intro track now
  plays to its end (`end of stream`) instead of freezing at half the file. The
  tracing is in the `mpstream`/`mpstream-mx` logcat tags, with the per-chunk
  detail behind `MP_STREAM_TRACE=1`.
- Android touch sticks and triggers reached the game wrong in three ways. The
  overlay sends normalised -1..1 deflections but `SDL_SetJoystickVirtualAxis`
  takes a Sint16 joystick value, so partial deflection truncated to 0 and the
  player could neither move nor aim; the Y axis was negated (SDL's gamepad +Y is
  down, which Aurora inverts for the GameCube stick); and a released trigger sent
  0, which is the axis *centre* rather than a trigger's resting minimum, so the
  game stayed locked on and strafing after the player let go. Axes now map the
  full -32768..32767 range and triggers release at the minimum.
- Controls still held when the debug overlay opened were never released: the
  overlay stops claiming touches as soon as it is visible, so the matching
  releases never arrived and whatever was down stayed down for the session. Held
  controls are released on that transition.
- Skinned vertex generation advances its output cursor explicitly. On the console
  the write-gather pipe advances itself as data is written, so `BuildPoints`,
  `BuildNormals`, and `Calculate`'s padding pass all reused one `pipe` value; on PC
  that made every bone overwrite the same offset and left the rest of the
  workspace uninitialised (vertex explosions). The vertex/normal workspaces are
  also one contiguous allocation, matching the points-then-normals write order.
- Runtime-generated vertex arrays are marked host-native (`le=true`) instead of
  big-endian, matching the port's native skinning/workspace writes. `ClearArray`
  forces the backend to drop its cached copy, which the skinned path needs because
  the workspace pointer is reused every frame. Map-screen mappable-object and area
  arrays are also host-native and provide their real byte sizes.
- `GXSetDrawSync`/`GXReadDrawSync` are real FIFO-ordered tokens in Aurora rather
  than a shim that echoed the last token. The skinned-model circular workspace
  frees a buffer once its token is readable, so an echoed token let the game reuse
  a workspace before Aurora's FIFO thread had copied its vertices, corrupting
  intermittent draws (the reported geometry explosion). The token command is
  processed by the FIFO worker in the same order as the draw that references the
  data, so the fence now holds.
- `F12` asynchronously reads back the resolved EFB and saves a 640x480 BMP under
  `screenshots/`. This avoids compositor-dependent tools and provides captures for
  diagnosing rendering regressions.

Host shutdown now completes cleanly. Game-heap buffers owned by `CGBASupport`,
`CStaticAudioPlayer`, and `SMediumAllocPuddle` are released through `CMemory` instead
of host `delete`, and the PC build skips the guest-stack usage scan that it does not
initialize.

## Next steps

Automated PAD input advances through the front end, loads the first room,
constructs `CInGameGuiManager` and `CMFGame`, and runs beyond frame 48,000 with an
AddressSanitizer-clean run past frame 21,000 and a normal exit on forced SIGTERM.

Verified:
- World, actor, and skinned geometry render (array sizes and array endianness were
  the draw-stopping bugs); the front end, HUD, and combat visor draw correctly.
- Left-stick input moves the player; the walk stays grounded and is constrained by
  room collision, so input, physics, and collision are live.
- The player stays alive (`CPlayer::x9f4_deathTime` remains 0).
- Streamed (front-end) audio decodes to tonal PCM, and in-level MusyX frames are
  tonal rather than noise after the ADPCM info-block conversion.
- Both SDL audio paths maintain a bounded queue instead of depending on exact
  5 ms thread/main-loop scheduling. MusyX also reads raw PCM16 sample payloads as
  GameCube big-endian data instead of host-endian data.
- In-game `.dsp` stream headers are converted from GameCube endianness after the
  DVD read. Without this, the native sample-rate check rejected every stream;
  traced intro playback now allocates a stream at 32000 Hz.
- GPU captures from the current build show the publisher screen, `[ PRESS START ]`
  title screen, and no-memory-card dialog rendering correctly. The user's reported
  blank front end therefore needs a capture at the exact failing transition.

Remaining:
1. Directed input needed to reach and open a door; wandering for ~48k frames never
   triggered a second `CWorld::TravelToArea`, so room transitions are unverified.
2. Retest title music, spaceship music, and effects by ear after queue-depth and
   PCM16-endianness fixes; verify pitch/tempo and that spaceship music starts.
3. Confirm the draw-sync fence removed the intermittent skinned geometry
   explosion with an F12 capture at the failing frame if it still occurs.
4. ~~Replace the session-long AGSC buffer retention with a bounded lifetime.~~
   **Already done**, in `f2888a72`. The buffers are now held exactly as long as
   MusyX has their group pushed, and no longer. See the entry above. The item was
   on this list for weeks after it was finished.
5. Verify CARD saves and the remaining menu flows. The card itself is confirmed
   read; loading a save through the front-end menu is not, and needs driven menu
   input.
6. ~~The reported blank-front-end screen has not been reproduced.~~ **Seen, and
   it does not reproduce** — the title, publisher and Dolby screens all render
   correctly with `MP_FAST_BOOT` off. What replaced it is a sharper question: the
   front end does not get past the Dolby screen on Start or A, which may be the
   same report seen from the other side.

Wayland presentation keeps the game EFB locked to its configured 640x480 with
`VISetFrameBufferScale(1)`, while Aurora scales that image to the native high-DPI
swapchain. This prevents the title background from disappearing at fractional
display scales. Compositor vsync is disabled because it misses presentation
intervals around the game's own async-idle work; an absolute 60 Hz deadline is
used instead.

Debug shortcuts: `F10` toggles the 60 FPS deadline/unlimited mode; `F12` saves
the resolved framebuffer under `screenshots/`. Unlimited mode changes only the
presentation rate; simulation, input, SFX, and streamed audio remain on Prime's
fixed 60 Hz clock.

Debug env flags for fast iteration: `MP_FAST_BOOT=1` skips the pre-front-end and
drives the title through file select into a new game without input;
`MP_SKIP_CUTSCENES=1` skips cutscenes that set a cinematic skip object and
fast-forwards the ones that do not (the opening frigate sequence deliberately has
no skip object), so control is granted in roughly 15 seconds instead of minutes.

HD textures: `MP_TEXTURES=<dir>` loads replacements with Aurora's
`tex1_<w>x<h>[_m]_<texhash>[_<tluthash>]_<format>.dds|.png` convention (hash
fields may be `$` wildcards). The format may be Aurora's numeric GX format or
Dolphin's name (`CMPR`, `RGBA8`, `C8`, ...); Dolphin also hashes with
`XXH64(data, size, 0)` and uses the same `tex1_` layout, so Dolphin packs should
resolve once the hashed size and paletted tlut handling are confirmed against a
real pack.

Lock-on uses `CPlayer::WithinOrbitScreenBox`/`WithinOrbitScreenEllipse`, which
compare the target's live-viewport screen position against the player tweak's
fixed 640x480 coordinates. Those coordinates are now scaled by the viewport size;
otherwise the lock-on zone sits left of the reticle in widescreen and centred
targets are never acquired.

Aspect ratio is selectable via `MP_ASPECT=4:3|16:9|window` (or the debug
overlay's Render tab, applied live): 4:3 is the original 640x480, 16:9 is a fixed
854x480, and `window` tracks the window on `AURORA_WINDOW_RESIZED`. Each frame the
game recomputes the render-mode width (`CGraphics::PortResizeFrameBuffer`) and
refreshes `CCameraManager`'s cached aspect; Aurora derives the internal EFB from
the render mode, so the horizontal FOV, culling frustum, and present all widen
together. The port enables `AURORA_VIEWPORT_FIT`, so the EFB matches the selected
aspect and the present letterboxes rather than stretching when the window shape
differs. The HUD is anchored to the view edges and scales with it.

Mouse aim (`MP_MOUSE_AIM=1`, or the debug overlay's Input tab) integrates relative
motion into world yaw/pitch in playable first person. Its state handoff, camera
response, buttons and crosshair are described in the current mouse section above.
`MP_MOUSE_SENS` sets radians per pixel (default 0.0035).

The former Wayland cursor-warp workaround and large-delta rejection were removed
during native hardening. SDL relative capture now owns cursor locking; focus/UI
changes clear pending motion instead of guessing which deltas came from a warp.

`F1` toggles an in-game debug overlay (Aurora's ImGui) with sections for
Performance (frame limiter, FPS), Cutscenes (skip and speed), Render (vsync and
internal EFB scale), Audio (mute the streamed/AI path or MusyX independently),
and Session (restart to menu, screenshot). The same settings are read from
`MP_FAST_BOOT`, `MP_SKIP_CUTSCENES`, `MP_CUTSCENE_SPEED`, and
`MP_SHOW_DEBUG_UI` at startup, and the overlay writes them live. `MP_DISABLE_AI_AUDIO`
still exists for isolating streamed audio at startup.

Unlimited presentation interpolates the active world camera between simulation
transforms. Free mouse look keeps current rotation so the reticle and shot agree;
translation remains interpolated. Camera switches, large translations and
non-mouse camera cuts reset interpolation. A world-space gun transform does not
automatically stay camera-relative when only the camera is interpolated: the
held-weapon render pass now uses its matching simulation view explicitly. Actor
and weapon animations still update at the fixed simulation rate.

`assets/initial_pipeline_cache.db` contains machine-independent Aurora pipeline
descriptions collected from the title, menus, and intro gameplay. CMake copies
it beside the executable; Aurora merges it into each user's persistent cache
and compiles the entries on its background pipeline thread.

## Licensing

- Aurora and the checked-in MusyX snapshot: MIT (see their `LICENSE` files).
  Port-specific code: ours. Preserve individual source notices as well.
- The decompiled game/engine source and the game assets remain Nintendo's; this
  is the usual decomp-port situation. Ship no assets; require the user's disc.
- The earlier recomp path (GPL: DolRecomp / ModernGekko) is preserved separately
  and is not linked into the port.

## Preserved recomp baseline

- git tag `recomp-baseline-2026-09-18` in the MetroidPrimeRecomp repo.
- Backup bundle and build artifacts under `/home/odran/backups/` (`mpr-*`).

# Native Metroid Prime port

This branch compiles the decompiled game and Kyoto engine as native C++20.
Aurora provides the GameCube SDK compatibility layer, SDL3, and WebGPU/Dawn;
MusyX provides the software audio engine. It does not link DolRecomp or the
ModernGekko runtime from the separate recompilation experiment.

## Build

Requirements: CMake 3.25+, Ninja, a C++20 compiler/library, and a C compiler with
C23 support for MusyX. Tested locally with GCC 15 and Clang 19 on x86-64 Linux;
CI is configured for GCC 14, Clang 18, and Windows clang-cl. Windows requires LLVM and an
MSVC developer environment/Windows SDK. Older compiler versions are unverified.

Aurora and MusyX are vendored snapshots; a normal clone is sufficient. Aurora
fetches pinned transitive dependencies on the first configure. On Linux install
SDL build prerequisites (X11/Wayland, ALSA/PulseAudio, EGL/OpenGL, FreeType, PNG);
see `.github/workflows/native-linux.yml` for the Ubuntu package list and
`extern/aurora/docs/building.md` for dependency-provider options.

```sh
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DAURORA_ENABLE_TESTS=OFF -DMP_BUILD_TESTS=ON
cmake --build build/native -j 4
ctest --test-dir build/native -L port --output-on-failure
```

For Windows, run from an MSVC developer shell and add
`-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl` to configure.
Plain MSVC `cl.exe`, MinGW, macOS, and ARM builds are not currently validated.

If `ccache` is installed, configure compiles through it, and on Linux `mold`
links when it is installed. Turn them off with `-DMP_USE_CCACHE=OFF` /
`-DMP_USE_MOLD=OFF`. The cache is shared between build directories in the same
checkout, so a new `build/<name>` mostly fills from it instead of recompiling.
One configuration takes about 0.5 GB of cache (`ccache -M` sets the limit).

No disc or generated game-asset headers are needed to compile. The two embedded
default-font resources are read from the mounted retail DOL at runtime, using
the addresses in `config/GM8E01_00/symbols.txt`. Native builds do not depend on
`build/GM8E01_00/include` or distribute extracted font data.

### Linking and packaging

`mp_game` is an OBJECT library: every source in the game manifest is deliberately
linked once, so COFF archive extraction does not decide which game/shim-dependent
translation units are present. Duplicate-definition errors remain enabled;
neither whole-archive nor `/FORCE:MULTIPLE` is used. The Aurora core/GX static
library cycle uses `LINK_GROUP:RESCAN` on supported ELF linkers; COFF resolves
those archive dependencies normally.

Keep `initial_pipeline_cache.db` beside the executable. Windows also needs the
runtime DLLs copied there by CMake, including Dawn's dynamically loaded DXC
libraries when provided. Use the CI `dist` package rather than copying just the
EXE. The workflow runs regression tests and checks the packaged no-disc startup
path. Preserve the accompanying dependency licenses/notices.

## Run

```sh
./build/native/metroid_prime_port "/path/to/Metroid Prime (USA) (v1.00).iso"
```

Alternatively set `MP_DISC`, keep the image beside the executable (or in a
folder beside it; for an AppImage, beside the `.AppImage` file), which starts
the game with no prompt, or let the port ask for it. A plain `.iso`/`.gcm` there
is only taken when its header says GM8E01 v1.00, so another game's image next to
it is skipped; compressed formats are taken as found, after a matching plain
image. Otherwise, when no disc is found it opens the platform's file dialog and
remembers the answer as `disc_path` in the settings file. There is no prompt
when the port has no window to show one on, or with `MP_NO_DISC_DIALOG=1`
(for scripted runs that do have a window, as on a build runner). The disc must
identify as **GM8E01, disc 0, revision 0**; other revisions/regions are
rejected. Nod/Aurora supports additional image
containers, but the same retail content is required.

`metroid_prime_port --version` prints the source revision without initializing
graphics. The same revision appears in the launch log and F1 Performance tab.
Include it when reporting a copied build from another machine; a `-dirty` suffix
means the executable was built with uncommitted source changes.

A copied build is self-contained: everything the port writes goes in the folder
holding the executable (for an AppImage, the folder the `.AppImage` file is in).
That is `port_settings.ini`, `mods/`, `savestates/`, `importers/`,
`user_textures/`, the randomizer and Archipelago files, the controller bindings
and the shader caches (`dawn_cache.db`, `pipeline_cache.db`), next to the memory
card in `<region>/Card A`. The disc image is auto-detected there too (or one
level below), and texture replacements are loaded from `textures/` when present;
`MP_DISC` and `MP_TEXTURES` override those two. The launch log names the folder
(`port: user folder ...`).

The per-user folder (`~/.local/share/Metroid Prime`, `%APPDATA%\Metroid Prime`)
is used only when:

- the executable's folder cannot be written to (a Flatpak, a system package), or
- it already holds a `port_settings.ini` from an older build and the
  executable's folder has none. Move its contents next to the executable to make
  that install portable.

On Android the folder starts in the app's own storage, which other apps and file
managers cannot reach. F1 > Extras > Data folder moves it to
`/storage/emulated/0/MetroidPrime/` (the shared storage root): it asks for "All
files access" (Android 11+; storage permission on 9/10), copies saves, settings,
mods, save states, the texture pack and the copied disc there with a progress
bar (built-in textures and the pipeline cache stay behind), then restarts the
game. If the shared folder already holds data, it offers to use that data or to
copy over it. The choice is kept in `data_folder.txt` in app storage; "Move
back to app storage" reverses it, and "Delete the old copy" frees the space the
old folder still takes. If access is revoked later, the game starts from app
storage and says so, with buttons to grant access again or stay there.

`MP_USER_PATH` overrides the folder
and `MP_CACHE_PATH` the caches alone; use separate directories for automated
testing so runs do not share normal saves/settings (the memory card stays with
the executable either way). Screenshots are written to `screenshots/` in the
working directory.

### A Wayland session hangs at startup

On a GNOME Wayland session the port can pin a core at 100% and never reach its
first frame. The log stops after `Using surface format BGRA8Unorm`, and no
`MP frame` line ever appears. The cause is outside the port: SDL3 chooses its
Wayland backend when `WAYLAND_DISPLAY` is set, and `SDL_ShowWindow` dispatches
pending Wayland events, one of which is libdecor's client-side decoration
configure. libdecor then re-enters GTK layout from inside that dispatch and
never returns. A backtrace of the hung process shows the loop:

```
hypot → cairo_scaled_font_create → pango_context_get_metrics
      → gtk_widget_get_preferred_height → libdecor_frame_commit
      → decoration_frame_configure → SDL_waylandwindow.c → Wayland_ShowWindow
      → SDL_ShowWindow_REAL → aurora::window::show_window
```

Run with `SDL_VIDEODRIVER=x11` (and `DISPLAY` plus `XAUTHORITY` pointing at the
session's Xwayland) to use the X11 backend instead, which reaches the main loop
normally. The port does this for you: when `SDL_VIDEODRIVER` names no driver and
`DISPLAY` is set, it requests `x11` and says so in the log. Keying off `DISPLAY`
rather than `WAYLAND_DISPLAY` matters — SDL3 reaches for Wayland even with
`WAYLAND_DISPLAY` unset, falling back to the default socket in `XDG_RUNTIME_DIR`.
With no `DISPLAY` there is nothing to fall back to, so the port prints why it is
about to hang rather than leaving only the frozen frame to go on. Neither backend
is a workaround for missing functionality — the only difference is which window
system draws the window.

### Time to first frame

The port prints what startup cost, once a frame has been presented:

```
MP startup: first frame 913 ms after the main loop began, 2349 frames run
```

This is the time from entering the main loop to the first presented frame, which
is where shader compilation, pipeline creation and the first texture uploads
happen. It is **not** time from launching the process: the disc image is read,
mounted and identified before the loop is entered, and a cold shader cache or a
slow disc will dominate that part instead.

Measured on the development machine (RTX 5070 Ti, warm pipeline cache, cutscenes
skipped, `SDL_VIDEODRIVER=x11`): **913 ms, 1180 ms, 1526 ms** over three runs.
The spread is the first-frame work varying with what the driver had cached, so
treat it as roughly a second rather than a precise figure. The existing F1
Performance tab still reports the steady-state render and simulation rates once
running.

### Frame pacing

Measured on Linux with `MP_TRACE_TIMING=1`, sampling the frame log by arrival
time so the wall-clock interval is what is measured rather than the frame's own
cost (`build/frame-pace.sh`):

| | presented | throughput | mean interval | p99 | jitter p99-p50 | late by >1 ms |
|---|---|---|---|---|---|---|
| capped (default) | 60.00 FPS | 1282.8 FPS | 16.667 ms | 16.672 ms | 5.6 us | 0 of 24 |
| uncapped (`F10`) | 137.0 FPS | 137.0 FPS | 7.126 ms | 10.534 ms | 3277 us | 0 of 48 |

Two rates are reported because they answer different questions, and the gap
between them is the useful part:

- **presented** — frames that reached the screen per second of wall time. What a
  player perceives.
- **throughput** — what the machine could produce, with the pacing wait excluded.

At the default cap the two differ by 21x, so stutter here is never the CPU
running out of budget. Uncapped they converge, which is the check that the two
really are measuring different things: once nothing is waiting, they must agree.
Simulation stays at 60 ticks/s either way, because it is fixed-step — uncapped
presentation renders the same simulation more often rather than advancing it
faster.

Under **Xvfb with `SDL_AUDIO_DRIVER=dummy`**, so treat the throughput figure as a
best case. The presented rate and the jitter are the parts that transfer.

### Seeing the front end, and where it stops

With `MP_FAST_BOOT` **off** the real front end runs and waits for input. The
screens captured on the real session, on the AMD adapter, are in
`docs/images/`:

| Screen | File |
|---|---|
| Metroid Prime title | `front-end-title.png` |
| Nintendo publisher logo | `front-end-publisher.png` |
| Dolby Surround Pro Logic II | `front-end-dolby.png` |
| Title with `[ PRESS START ]` | `front-end-press-start.png` |

All three render correctly, so **the reported blank front end does not
reproduce**. Note that `MP_FAST_BOOT=1` never shows any of them: it drives
`Title -> FileSelect -> TransitionToFive()` into a new game, which is why
captures taken with it set are pictures of the *game's* opening, not the front
end.

**Input is discarded when the window is not focused**, which is worth knowing
before concluding that a screen is unresponsive. `CDolphinController::ReadDevices`
(`src/Kyoto/Input/CDolphinController.cpp:105`) zeroes the whole pad status when
`SDL_GetKeyboardFocus()` is null and then preserves the error code, so the
controller still reports *present* while every button is dropped, and the front
end receives input messages containing no buttons. A scripted press launched into
an unfocused window on a live desktop is therefore thrown away before anything
reads it. `MP_SMOKE_SCRIPT` and `MP_SMOKE_FRONTEND` now claim focus the way
`MP_SMOKE_MOUSE` already did; with that fixed the front end reaches the title and
its `[ PRESS START ]` prompt (`front-end-press-start.png`).

The front end then loops between the title and the attract movie. The main menu,
and the Continue option on it, has not been reached: at the few frames per second
this path presents, a single scripted press is a lottery — one during a fade-in
is ignored — and the loop's phase drifts with machine speed, so a press timed
from a fixed frame number does not land. Not a suspected defect, just not done.

Two things to know when reproducing this:

- **The card follows the executable, not `MP_USER_PATH`.** `CARDSetBasePath` is
  given `SDL_GetBasePath()`, so each build directory has its own card. A save
  written by one build is invisible to another.
- **A single press at a guessed frame is not enough.** The title takes ~2000
  frames to fade in on this path, and a press during the fade is silently
  ignored, which is indistinguishable from "the button does not work". Use
  `MP_SMOKE_FRONTEND=<frame>`, which taps repeatedly.

### Audio

A run reports which backend it got, and with what:

```
MP audio: driver pulseaudio
```

or `MP audio: no driver; SDL opened no audio device`. SDL3 picks the first
available backend, so on Linux this is normally `pipewire` or `pulseaudio` — SDL
loads both at runtime, which is why neither appears in `ldd` — and it is the only
place a missing `libpulse` becomes visible at all. Force one with
`SDL_AUDIO_DRIVER`; `SDL_AUDIO_DRIVER=dummy` is what the automated runs use so
they do not fight over a real device.

There is deliberately **no** mixer level or queue-depth report. The mix happens in
vendored MusyX, not in the port, so measuring it there means instrumenting a
vendored snapshot, and a report taken from the port's AI DMA path would describe a
path that is idle in a normal run.

### Choosing a GPU, and why the headless runs all use the NVIDIA one

The loader picks a GPU on its own, which is fine on a desktop and a problem on a
machine with more than one. To pin it, restrict the ICD — no code change is
involved:

```sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json   # AMD (RADV)
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json   # NVIDIA
```

Worth knowing on a multi-GPU machine: pinning a vendor is not the same as pinning
a device. `MESA_VK_DEVICE_SELECT` did not move the choice off the Radeon RX 7900
XTX onto the adjacent Raphael iGPU, so the selector matched a name the loader was
not using. It does not matter for VRAM purposes — either AMD adapter has its own
memory — but "use the integrated GPU" is not reliably expressible through the
environment.

**Headless runs on Xvfb only present with the NVIDIA driver.** Xvfb exposes no
DRI3 extension, and Mesa's Vulkan WSI requires it:

```
MESA: info: vulkan: No DRI3 support detected - required for presentation
```

With the AMD ICD the run selects an adapter, reports its limits, creates the
device, and then dies at surface creation with **0 frames** — so it never gets far
enough to need the textures it was out of memory for. The same Xvfb works with
the NVIDIA driver, which is why every automated run so far has used the discrete
NVIDIA card. Software Vulkan (lavapipe) fails the same way, more abruptly,
segfaulting inside Dawn's surface setup rather than logging.

So on a headless box the GPU is not the thing to change; the display server is.
Free VRAM on the card you are already presenting through, or run against a real X
session or Wayland, where DRI3 exists.

### Running out of device memory

A Vulkan allocation that does not fit is fatal:

```
[fatal] [aurora::gpu] WebGPU error 3: vkAllocateMemory failed with VK_ERROR_OUT_OF_DEVICE_MEMORY
 - While calling [Device].CreateTexture([TextureDescriptor ""GX Static Texture""]).
```

and the process stops. The message names an allocation and nothing else, so the
first reading of it is that the port is holding too much. **It usually is not.**
The port's own process uses about **118 MiB** of device memory while running, a
full eight-world tour does not accumulate allocations, and the failure reproduces
**23 frames into a one-area tour** — immediately after the tour completes and
never during it, which is the opposite of cumulative exhaustion. What actually
caused the failure observed here was an LM Studio model holding **15124 of
16303 MiB**, leaving about 1.1 GB for everything else.

So the first thing to check is what else is on the GPU:

```sh
nvidia-smi --query-compute-apps=pid,used_memory --format=csv
```

Not fixed: the port aborts on a fatal allocation with a message that does not say
what was short, which a player on a busy GPU would see as an unexplained crash.
The honest fix and the smaller-attachment fallback both belong in vendored Aurora,
so they are better decided there than worked around from the port.

### HD texture replacements

`MP_TEXTURES` (default `<executable dir>/textures`) points at a folder of
Aurora-format replacements (`tex1_<w>x<h>_<texhash>[_<tluthash>]_<fmt>.dds` or
`.png`). The folder may hold per-device subfolders — `xbox`, `playstation`,
`switch`, `gamecube`, `standard`, `keyboard` — selected from the input the
player last used (a pad's type; keyboard for a keyboard or mouse, or when no pad
is connected; the Android touch overlay's layout), so in-game button prompts
match what is in the player's hands. Only deliberate input switches it: a key
press, a mouse move or click, a pad button, or a stick or trigger pushed past
half way; the touch overlay's virtual pad does not count. The set is swapped
automatically, and `MP_TEXTURE_DEVICE` forces the name. A folder with no subfolders is used as a
single device-agnostic pack; a folder that has device subfolders but not the one
selected loads nothing rather than mixing packs. `MP_DUMP_TEXTURES=1` writes
every source texture to `<cachePath>/texture_dumps` as DDS, for authoring
replacements.

A user pack is layered over that built-in set, with the same folder rules. It
lives in `user_textures` in the user folder (next to the executable; for a read-only install `~/.local/share/Metroid Prime/`
on Linux, `~/.var/app/io.github.odrannnn.metroidprimeport/data/Metroid Prime/`
in the Flatpak, `%APPDATA%\Metroid Prime\` on Windows), or wherever
`MP_USER_TEXTURES` points, so updates never touch it; the overlay's Render page
has a Reload button. On Android, Render > Texture pack > Choose texture pack
folder opens the system folder picker and copies the folder's `.png`/`.dds`
files into the data folder (pick it again after changing it; Remove deletes the
copy). The copy lands in `user_textures.new` and is swapped in on the next
frame, or at the next start if the app closed first. Priorities: built-in 0,
user pack 1, binding icons 2, so a remapped action still shows its binding.

In-game button prompts are ordinary textures (`CFontImageDef` holds one texture
per glyph), so they can be swapped the same way. To re-author them: dump the
textures from a screen that shows the prompt, find the glyph by its size and
contents, then write a replacement with the same stem into the device folder.
`MP_DUMP_TEXTURES=1` is the supported way to do this and needs no code change:
textures that already have a replacement are not dumped, so the result is
exactly the unclaimed set, and the images can simply be looked at. To see every
texture on the disc instead, `tools/extract_textures.py <disc.iso> <outdir>
--png` writes them all under Aurora's names, with the developers' PAK names
(`LStickN`, `AButtonIn`, `DPadU`...) in `index.tsv` and the textures the game's
text draws inline (`&image=` tags, the HUD hints) in `strg_images.tsv`; that is
how the prompt table was completed. It needs libxxhash, numpy and PIL. The prompt
table in `platform/port_prompts.cpp` is the authority on which textures are
claimed, and every hash in it should be one that appears in a dump from the
screen that shows it - two rows were transcribed into the wrong action and that
is how the pause menu's Exit prompt ended up drawing the C-stick's glyph.
`tools/make_prompt_glyphs.py <dir>` builds a set for every prompt in the table
(xbox, playstation, switch, keyboard) into `<dir>/<device>/`, compositing the CC0 icons
vendored in `tools/prompt_icons/` (Kenney's Input Prompts pack); the keyboard
set labels the keys the port binds by default. The build copies `textures/` next
to the executable, so a built binary picks the replacements up without a manual
copy. Two details matter: the icon must keep the game's inset (the glyph is only
about 22px in the 32x32 buttons and 28px in the 64x32 stick and D-pad art, and a
full-bleed icon reads as a square), and DDS is used
rather than PNG because Aurora's DDS path preserves the texture's alpha.

The button prompts are binding-aware: `platform/port_prompts.cpp` registers, at
runtime, the icon for the input actually bound to each action, so rebinding a
key or mouse button changes the prompt. The per-input icons live in
`<textures>/bindings/` (also generated by `tools/make_prompt_glyphs.py`) and are
served through Aurora's virtual-replacement callback rather than shipped as a
fixed set. Each screen draws its own prompt art, so one action maps to several
game textures. Every button, stick and D-pad prompt texture on the disc is in
the table (A, B, X, Y, Start, L, R, Z; the stick, C-stick and D-pad whole and
per direction), which covers the front end, the pause and map screens and the
HUD hints (the visor hints' D-pad arrows, the beam hints' C-stick, the strafe
and R animations). The sticks follow their axis bindings: a pad shows the stick
that drives the axis (so Southpaw swaps them) or, for an axis driven by buttons,
those buttons; a keyboard shows the key for a direction frame and, for the
whole stick, the WASD, IJKL or arrow cluster when the four keys form one (the
up key otherwise). The stick's diagonal frames take the whole-stick icon. A
keyboard follows its key bindings (the main key, else the second
key); a pad follows its button mapping, so a remapped button or a preset shows
the button it now uses. Letters, digits, punctuation, the named keys, the keypad
(its digits share the main row's art) and all five mouse buttons have icons; a
binding without one leaves the static set in place. A GameCube pad (adapter or
NSO) on its default mapping keeps the game's own art, and only an action moved
to another button gets a GameCube icon, named after the button's default action.
The binding icons register above the static set, so reloading that set never
hides them. The chosen icon is logged when it changes ("prompt A keyboard_x").
`MP_SMOKE_BIND_A=<scancode>` (negative for mouse buttons, -3 is middle) rebinds
the A action once, for checking this without going through the Controls tab; it
needs a `-DMP_ENABLE_SMOKE_DRIVER=ON` build such as `build/smoke-gcc`, and
`MP_FAST_BOOT=1 MP_SKIP_CUTSCENES=1 MP_SMOKE_PAUSE=<ticks>` reaches the pause
screen quickly to see the result.

### Mods folder

`mods` in the user folder (or wherever `MP_MODS` points; created on first
start) replaces disc data without touching the disc image. Each folder in it is
a mod, applied in name order so a later name wins; folders starting with `.` are
skipped. Inside a mod:

- a file at a disc path (`Metroid1.pak`, `Audio/frigate.dsp`,
  `Video/attract0.thp`; case does not matter) replaces that file. Only
  existing disc files can be replaced; others are reported and ignored.
- a file named `<8 hex digits>.<type>` (`1A2B3C4D.TXTR`, `0552A456.STRG`),
  anywhere in the mod, replaces that resource, uncompressed, in every PAK that
  holds it. The PAK is served as a virtual file: its table is patched to point
  past the original data, where the loose file is appended (32-byte aligned). A
  loose resource also applies inside a PAK another mod replaced whole. An id
  that no PAK holds is added to `NoARAM.pak` (loaded from boot, so any model or
  frame can reference it): the table grows by one entry and the original data
  moves down to keep its 32-byte alignment. An id another PAK holds under a
  different type is reported and ignored. A resource can be any size (the port
  keeps PAK sizes whole; retail stops at 4 MB), though the game heap still
  bounds what fits: a 16 MB texture runs it out of memory. `MP_MEM1_MB=<n>`
  raises the emulated MEM1 arena (default 24 MB, capped at 1024) and the game
  heap with it, for mods heavier than the retail heap allows; the log says
  `port: MEM1 arena raised to <n> MB`.
- a file named `<8 hex digits>.dds`, anywhere in the mod, is a native texture:
  the full-size image of the TXTR with that id, as BC7, BC5 (two-channel, for
  PBR normal maps), BC3, BC1 or RGBA8, with a full mip chain. The TXTR still
  loads (the mod's own, which can be a few texels across, or the disc's) and
  supplies the wrap and filter state; the `.dds` is what is drawn. Aurora
  streams it on worker threads into GPU memory, so its size never counts
  against the game heap: a mod of 2048 px maps runs at the default arena. A
  later mod's TXTR without a `.dds` drops an earlier mod's `.dds` for that id.
  On a GPU without BC support (most phones) the TXTR is drawn instead. The log
  and the Mods panel count them (`mods: <n> native texture(s)`).
- a file named `<name>.sdfont`, anywhere in the mod, is a distance-field font
  (each texel holds how far it is from the glyph's edge), which stays sharp at
  any resolution. The port draws its glyphs in place of the disc's bitmap ones
  for the Deface fonts (HUD, menus, scans, logbook), fitted to the disc's
  capital H; the layout is still the disc's (advances, kerning, line breaks),
  so text sits where it did. From a character the file lacks, the rest of
  that run of text is drawn from the disc. The last mod with one wins,
  `MP_HD_FONT=0` turns it off, and the layout is in
  `platform/include/port_hd_font.h`.
- a file named `<FRME id, 8 hex digits>.hudbars`, anywhere in the mod, gives
  the energy-bar widgets of that HUD frame their own shape: per bar, named
  after its widget, a strip of stations (two points and their texture
  coordinates) from the empty end to the full end. The game still supplies the
  value, the colours and the texture; the port fills the strip by its measured
  length, so stations need not be evenly spaced, and blends it as the widget's
  draw flags say. The last mod with one for a frame wins, and the layout is in
  `platform/include/port_hud_bars.h`.
- a file named `<MREA id, 8 hex digits>.roomenv`, anywhere in the mod, is that
  area's lighting environment for PBR materials: reflection probes, each a box
  of the world with a prefiltered HDR cube map (BC6H) of what surrounds it. A
  PBR model reflects the cube of the smallest box it stands in (else the
  nearest), in place of the port's live probe, exposed so the cube's average
  is middle grey. A file may also hold baked ambient light: a grid of points
  through the room, each with the light arriving there and the direction most
  of it comes from, per colour. A PBR model standing in a grid takes the
  colour and direction of its ambient light from the points around its origin;
  the game's ambient colour only sets how bright it is. A file also carries
  the room's tonemap values and its auto exposure settings, and a
  probe's scale turns its cube (stored normalised) back into radiance. Those
  set one exposure for the frame, the way Remastered does: the room the
  camera is in is exposed by its own radiance, its exposure bias and the
  range its auto exposure is held to, cubes and baked ambient keep their
  level relative to that, and the result goes through that room's tone curve
  (the formulas are Remastered's own; the frame's average is stood in for by
  the room's middle probe). `MP_ROOM_ENV_EXPOSURE=0` turns that off: every
  cube is then exposed to middle grey and the game's ambient sets the level.
  Files load and unload with their areas, and a cube is decoded
  on first use. The layout is in `platform/include/port_room_env.h`.
- a file named `<MREA id, 8 hex digits>.roomgeo`, anywhere in the mod, is that
  area's static geometry: a list of models, each with the transform that
  places it in the area, drawn in place of the area's own surfaces (its
  actors, doors and pickups are drawn as before). Little-endian: the tag
  `MPRG`, a version (1), a count, then per instance a CMDL id and a 3x4
  matrix, 52 bytes. The models are ordinary CMDLs of the mod and are loaded
  with the area. The Thermal and X-Ray visors draw the retail area. A game
  started with such a mod installed takes a 256 MB arena and frame buffers 12
  times the usual size (see `MP_FRAME_BUFFERS`), since these rooms are many
  times retail's vertex count; a mod loaded into a running game without them
  is not drawn until the next start.
- a file named `<MREA id, 8 hex digits>.roomliquid`, anywhere in the mod, holds
  that area's liquid surfaces (water, poison, lava) as models: each is drawn in
  place of the fluid plane of the area's water object that stands where its
  transform says, at the height the game gives that plane, so a rising or
  draining liquid still moves. The object itself is untouched (fog, splashes,
  damage). Little-endian: the tag `MPRL`, a version (1), a count, then per
  surface a type (0 water, 1 poison, 2 lava), a CMDL id and a 3x4 matrix, 56
  bytes. Their materials carry a `PBR4` record of kind 5 (water, poison: a
  colour, an opacity and two moving normal maps) or 6 (a lava pool: a pattern
  carried along a flow map, coloured by a ramp).
  The Thermal and X-Ray visors show the surface too: the Thermal visor's passes
  shade it as they shade a fluid plane (the hot pass adds it), and the X-Ray
  visor draws it unchanged. `MP_ROOM_LIQUID=0` or the
  console's `roomliquid off` draws the retail planes instead.
- a PBR material (flag bit 14) may end in a 28-byte record: six big-endian floats
  (emissive multiplier rgb, backlight weight rgb) and the tag `PBRM`, inside the
  material's own span in the offset table. The multiplier scales the emissive map;
  the backlight adds a rim of the surface's colour on edges turned away from the
  viewer. A material without the record gets 1 and 0.
  Two longer forms follow the same six floats: `PBR2` (36 bytes) adds a height-blend
  threshold and a mode (1 unlit: the material's own colour and glow; 2 the base
  map's alpha scales the glow; 4 the vertex colour tints the surface; summed),
  and `PBR3` (56 bytes) adds a second layer's edge width and the scale and offset of both layers' heights. A `PBR3`
  material binds three more maps (base, metal/roughness, normal of the second
  layer) and blends the two by the vertex alpha and the base maps' alphas.
  A colour attribute tints a PBR material's albedo only with mode 4.
- text and image files (`.txt`, `.md`, `.json`, `.png`, ...) are ignored
  silently, so a mod can carry its readme.

Mods load at startup, after the disc is opened (the game caches PAK tables when
it boots), and everything reads through Aurora's DVD overlays, so the game code
is unchanged. The log lists each mod's counts and any problems ("mods: ..."),
and F1 > Extras > Mods shows the same, with a Load mods switch (`mods`) and a
checkbox per mod (`mods_disabled`, `/`-separated names); both take effect on
the next start. `tools/extract_textures.py`'s `disc_files`/`pak_resources` get
a resource's original bytes to edit. Checked with a `0552A456.STRG` (the file
select's "New Game") read from the disc and from a whole-file `MiscData.pak`
mod. On Android `mods/` is in the data folder; move that to shared storage
(F1 > Extras > Data folder) to add mods with a file manager.

#### Reloading without a restart

F1 > Extras > Mods > Reload mods (console: `mods reload`) reads the mods folder
again while the game runs: added, removed, edited, enabled and disabled mods
all take effect, PAK resources and native `.dds` textures alike. The game is
rebuilt where Samus stands, exactly as a save state load does (enemies, doors
and puzzles come back as after a memory card load), with every PAK reopened so
that all it draws is loaded from the new files. It runs at the next game tick,
so unpause first; pressed in the menus, it happens when a game starts. Change
the folder first, then reload: until then the game still reads the old files,
so don't delete a loaded mod and keep playing.

#### Importers

An importer is a program of your own that builds a mod from files you have (a
model pack, another release of the game); the port ships none. Put an
executable file (Windows: `.exe`, `.bat`, `.cmd`) in `importers` in the pref
folder, beside `mods`, and F1 > Extras > Mods gets a Run button for it, with an
optional argument field, the tail of its output while it runs, and Cancel.
`metroid_prime_port --import` lists them and `--import <name> [argument]` runs
one in the terminal without starting the game, returning its exit code.

The importer gets the argument as `argv[1]`, the mods folder to write into as
`MP_MODS_DIR`, and the importers folder as its working directory. Its stdout
and stderr are shown a line at a time (a carriage return rewrites the line, so
progress bars work); exit code 0 means the mod is in place, and Reload mods (or
a restart) loads it. Cancel sends a terminate request to the importer
alone, so a script should pass it on to its children. Desktop only.

#### Metroid Prime Remastered models

**Very experimental and currently unsupported.** The import, the room
environments and above all room geometry are work in progress: expect wrong
or missing models, lighting that is off, crashes and heavy memory use, and a
re-import after most updates. Bug reports about a game running with this mod
are not handled for now; remove `mods/remastered-models` to get the retail
game back.

The port can build a model mod from your own copy of Metroid Prime Remastered;
nothing of it ships. F1 > Extras > Mods > "Metroid Prime Remastered models"
takes the game's `.nsp` and your console's key file (`~/.switch/prod.keys` is
filled in when it exists). The panel remembers both files once they are picked
or used, as `remastered_nsp` and `remastered_keys` in the settings file (on
Android, the picked documents). Import converts in the background while the game
runs, on all but two cores. The result is staged in `mods/.remastered-models.importing`
and becomes `mods/remastered-models` when you press Load it now (a mod reload,
above) or at the next start, replacing an older one;
a cancelled or interrupted import leaves nothing behind. From a terminal,
`metroid_prime_port --import-remastered <image.nsp> [key file]` does the same
on every core without starting the game and installs at once (the disc comes
from `MP_DISC`, the remembered path, or a copy beside the executable).

The port reads the image itself (`platform/port_remastered_nsp.cpp`, `_pak`,
`_txtr`, `_cmdl`), and `_convert` writes a CMDL (plus CSKR for skinned models)
over each retail model listed in `_table`, with its PBR maps as native `.dds`
textures. Texture ids are a CRC of the Remastered texture's id and its
role, so a map shared by several models is written once.

After the models, `_room` writes a `.roomenv` (above) for every area into the
mod's `roomenv` folder: it matches each Remastered room to the retail area by
its doors, and takes the room's reflection probes, their HDR cubes and its
baked ambient grid. A grid of more than about a million points is stored at
half resolution, which keeps every file under 25 MB. A world that can't be
read is reported and skipped; the models are installed all the same.

With "Room geometry too" ticked in the panel (it is by default; there is no
such box on Android), or `MP_REMASTERED_GEOMETRY=all` (or a comma-separated
list of room names, or `none`) for the command line and the console, the
import also writes the rooms' static geometry: every model a room places,
converted as above with textures capped at 1024 px, and a `.roomgeo` per
area, in the mod's `roomgeo` folder. Experimental: the full set is about
8,500 models and brings the mod from 1 GB to 6.5 GB, and the game has to be
restarted to draw it.

The room geometry also brings the rooms' liquid
surfaces: each water, poison or lava volume's own surface model, converted
with that room's colour, opacity, wave directions and flow, and a
`.roomliquid` per area in the `roomgeo` folder.

The import also carries over Remastered's rewording of the English text (scan
entries, logbook, pickups: 120 strings in 100 tables on the US disc). Each
changed table is written as `<id>.STRG` in the mod's `text` folder: the
disc's own table with the reworded strings replaced. A string is replaced only
when its words differ, so layout-only differences stay as on the disc;
Remastered's hand-placed line breaks (fitted to its own boxes) are kept only
where the disc's string breaks a line too, the rest left to word wrap;
highlight colours are kept (`&push;&main-color=…;…&pop;`) and the layout tags
a disc string opens with are carried over. Strings that name a button of
Remastered's controls or use one of its icons keep the disc's text, as do
strings with characters outside ASCII. `MP_REMASTERED_TEXT=0` leaves the text
out.

It also writes Remastered's typeface as `font/deface.sdfont` (see above): the
FONT asset with the most characters, its first face.

The in-game HUD is carried over as well, into the mod's `hud` folder: the
combat, scan, thermal, X-ray, ball and base frames. The disc's frame stays the
skeleton, so every widget the game looks up by name is still there; a widget
Remastered has under the same name takes its placement, colour and model from
there, and the models and pictures Remastered added are placed under their
parents. Each frame is written as `<id>.FRME` with its models (`.CMDL`), its
pictures (a small `.TXTR` and, above 64 px, a `.dds`) and, for the energy,
missile and threat bars, a `.hudbars`. `MP_REMASTERED_HUD=0` leaves the HUD
out. The map screen comes too, with one difference: Remastered lays it out for
a wider view and shows fewer prompts, so its legend, area name and hint are
set from Remastered on the disc's plane, and the prompts it lacks keep the
disc's places. The map's icons come as well, into `map/`: the save, missile
and elevator stations replace the disc's textures, and the six arrows of a
door between floors, which the disc draws with one tinted texture, get
Remastered's own under ids the port looks for (`port_map_icons.h`). So do the
rooms: Remastered's map of a world (`CMAP`) names no room of the disc, so its
areas are paired with the disc's by place and size, and the few it reshaped
(14 over the seven worlds) are written as `<id>.MAPA` with the disc's doors
and markers; the rest keep the disc's map. Experimental: the pause and message
screens are still the disc's, as is the map's compass, which the disc does not
have.

The menu movies come along too, into the mod's `Video` folder under the disc's
names: the title, the file select and its transitions, and four of the attract
movies (the other six, and the credits and ending movies, stay the disc's).
Remastered's are H.264; the game plays THP, a JPEG per frame, so each is decoded
once and written again, by default as 1600x900 at 30 frames a second (the disc's
own rate; the game's decoder does not keep up with 60 at that size), about
550 MB for the sixteen. `MP_REMASTERED_MOVIES=1280x720@30` picks another size
and rate, `MP_REMASTERED_MOVIES=0` leaves them out. A movie of any size is
fitted to the view with its shape kept, so in 4:3 these have bars above and
below.

The decoding is done by **ffmpeg**, run as a separate program: `MP_FFMPEG` if
set, else an `ffmpeg` next to the game's executable, else the one on the path.
The Windows package has one next to the executable; on Linux install it from
your distribution.
Without one the import finishes without the movies and says so; install ffmpeg
and use "Import movies" in the same panel (or `--import-remastered-movies
<nsp> [keys]`), which adds only the movies to the mod already there.
Android cannot start a program like that, so there the system's own decoder
(MediaCodec) reads the movies and the port writes the JPEGs itself; nothing has
to be installed.

Measured on the development machine: 342 models and 275 room environments, 44
seconds on 16 threads, 2.3 GB of memory at the peak, 1.1 GB on disk (half of it
the room environments).

On Android the panel has two buttons that open the system's file picker, one
for the `.nsp` and one for the key file. Neither file is copied: the import
reads them where they are. It runs on two threads there to keep its memory
down. Not yet run on a device.

### Platforms

The port is built and tested on Linux and Windows. Its own platform code is
portable - SDL3 and `std::filesystem` throughout - and the CMake keeps the MSVC
linker paths from the template. The Windows build is exercised by
`.github/workflows/windows.yml` on `windows-latest`, which only runs when
started by hand for now (`gh workflow run windows.yml --ref port`): it configures with
clang-cl, builds, runs `ctest -L port`, runs the FIFO regressions, packages a
`dist/` directory with licences, checks that the packaged executable reaches
main and reports the missing disc image, and uploads the result as an artifact.
All of that is green.

Only the things that need a GPU, a window or a real controller are verified on
one platform alone. `wss://` is verified on both: each CI job runs
`port_ws_tests` directly as well as through ctest, so the log shows a completed
TLS handshake against a throwaway CA plus the four rejections (wrong CA,
certificate for another address, system trust store, missing CA file) rather
than only a configure line saying OpenSSL was found.

`platform/glibc_compat.c`, which lowers the glibc the Linux build needs, is
guarded to Linux and takes no part elsewhere. The AppImage and Flatpak packaging
are Linux-only.

Note that `README.md` is inherited from the upstream decompilation project and
describes building that, not this port.

### Distribution

`tools/make_appimage.sh [build-dir] [output-dir]` packages the executable and
its texture replacements as an AppImage, fetching appimagetool on first use.
The disc image is deliberately not included, so the port asks for it with the
platform's file dialog on first launch and remembers the answer as `disc_path`
in the settings file. A path given as an argument or in `MP_DISC` still wins,
then the saved path, then a copy beside the executable.

The AppImage bundles the executable (Aurora, WebGPU/Dawn and SDL3 are linked
statically), the texture replacements, the shared libraries a base desktop may
lack (freetype, libpng, zlib, bzip2 and brotli), and the third-party notices
for the vendored and fetched components in
`usr/share/licenses/metroid-prime-port/`, alongside a
`BUNDLED_LIBRARIES.txt` naming the host libraries it copied. It relies on the
system for glibc, libstdc++, a Vulkan driver, X11 or Wayland, and DBus for the
file dialog.

`docs/RELEASING.md` covers what a release has to carry, what has not been done
yet, and the licensing position.

glibc is deliberately not bundled, so the build is only as portable as the
machine it was built on. `platform/glibc_compat.c` lowers that floor: recent
glibc gives the float math functions and the C23 strtol/scanf family new symbol
versions, which would otherwise pin the binary to the build host's glibc
(2.43 here). Defining those names in terms of the long-standing
double-precision and pre-C23 functions brings the requirement down to
**glibc 2.39** (Ubuntu 24.04, the current LTS).

What is left above that is `pidfd_spawnp`/`pidfd_getpid`, used by nod - the
prebuilt Rust library behind Aurora's disc access - which would need either an
older nod build or a build on an older base to remove.

A Flatpak sidesteps the whole question: `tools/make_flatpak.sh` builds one from
`flatpak/io.github.odrannnn.metroidprimeport.yml`, and glibc then comes from the
runtime (24.08) rather than the host, so the floor above does not apply. The GPU
driver still comes from the host, the disc is not bundled, and the sandbox sees
the home directory read-only. It needs flatpak and flatpak-builder and compiles
the whole game, so it is not part of the normal build; change the app id in the
manifest before publishing. This path has not been built here - flatpak is not
installed on the machine it was written on.

The AppImage embeds the statically linked type-2 runtime, so libfuse2 is not
needed on the target system; check it with `--appimage-version`. Where FUSE
itself is unavailable, such as in a container, run it with
`APPIMAGE_EXTRACT_AND_RUN=1` (or `--appimage-extract-and-run`), which unpacks to
a temporary directory instead of mounting.

### Controls and settings

- Keyboard defaults: WASD / IJKL for sticks, X/Z/C/V for A/B/X/Y, Return for
  Start, arrows for D-pad, Q/E for L/R, and F for Z. Existing mappings take
  precedence. SDL controllers are supported.
- F1: debug overlay. F10: 60 FPS cap/unlimited presentation. F11: fullscreen.
  F12: screenshot.
- Fullscreen (F1 > Render, persisted as `fullscreen`): a borderless window over
  the whole screen on desktop, toggled with F11; on Android it hides the status
  and navigation bars (on by default there; a swipe from the edge shows them
  for a moment).
- Settings changed in the F1 overlay (aspect, vsync, render scale, frame limit,
  mouse aim/inversion/sensitivity, audio mutes) are saved to
  `port_settings.ini` in the user folder (next to the executable, see above) and restored on the next launch. The Session tab shows the
  path and has a **Save settings now** button. Environment variables still
  override the file for that run, and are written back into it if any setting is
  changed during that run.
- `MP_ASPECT=4:3|16:9|window`; the legacy `MP_WIDESCREEN` selects 16:9.
- `MP_TWIN_STICK=1` (Input tab, persisted as `twin_stick`): twin-stick aiming. The
  right stick feeds the first-person aim through the same path as the mouse (so
  the same sensitivity/invert apply, tuned by `stick_aim_rate`, default 900 px/s)
  and is consumed, so it no longer drives the game's free-look. Fire stays on
  whatever is bound to A; remap it in the Controls tab.
- Spring Ball (Input tab, persisted as `spring_ball`, on by default): C-stick
  up in morph ball jumps, as in Metroid Prime Trilogy and the randomprime discs
  the Archipelago world makes, once the Morph Ball Bombs are held. It is a bomb
  jump from the ball's position that keeps the horizontal speed, with the same
  conditions (on the ground, not in shrubbery, spider ball or an energy drain)
  and 40-frame cooldown as randomprime's patch. With twin stick on, the right
  stick's up still triggers it. A connected Archipelago seed decides it instead.
  **Spring Ball on gyro flick** (`spring_ball_flick`, off by default) also
  springs when the pad or phone is tilted up faster than `spring_ball_flick_rate`
  (default 6 rad/s), like Trilogy's nunchuk flick: once per swing, and a flick up
  to 0.2 s before landing still counts. It reads the gyro aim's source but works
  with gyro aim off. The phone's gyro is turned to the screen's orientation, so
  pitch and yaw stay right in landscape.
  A press of the beam shift springs too (beams don't change in morph ball), as
  X does in Remastered; jump (B) stays the Boost Ball's alone. A shift held from
  before the ball formed has to be let go first.
- Beam shift (Controls tab): while it is held, the D-pad picks beams the way the
  C-stick does, and visors stay on the plain D-pad, so both are reachable without
  a C-stick. It has two key slots (`shift_key`, `shift_key_alt`; default left
  shift, which already did this under twin stick), a pad slot (`shift_pad`, a
  button or trigger, default none, because Aurora maps LB on many pads to L), and
  it can go on a mouse button. Twin stick keeps left shift as its own modifier,
  and L and LB too while `shift_pad` is unbound.
- Alt controller buttons (Controls tab, "Alt button" column, persisted as
  `pad_alt`, 16 comma-separated native codes indexed by the PAD bit, -1 for
  none): a second controller button or trigger per GameCube button. Aurora maps
  one native button to each PAD button, so the port reads the alt one itself and
  ORs it in (`PortControls::HeldAltPadButtons`, from `CDolphinController`).
- Control presets (Controls tab). Keyboard: **Classic** (the first-run layout)
  and **Mouse & keyboard** (WASD, E fire, Space jump, left ctrl/C morph, F
  missile, Q lock on, left alt free look, Tab/M map, 1-4 beams and 5-8 visors
  through whichever C-stick direction or D-pad button the disc's tweak gives
  each, arrows on the D-pad too; turns mouse aim on and twin stick off, since
  twin stick takes the C-stick). Both also reset the mouse buttons and the beam
  shift keys. Controller: **GameCube** (Aurora's default), **Remastered**
  (Remastered's Dual Sticks: RT or right face button fire, LT lock on, bottom
  face button or LB jump, left
  morph, RB missile, Start map, Back pause, right stick click free look, top
  face button as the pad beam shift (which springs in morph ball, as any bound
  beam shift does), Scan and X-Ray swapped; twin stick on),
  **Modern** and **Southpaw**. Only Remastered sets `shift_pad`, `pad_alt` and
  `swap_scan_xray`; the others clear them. Remastered and Modern are off for a
  GameCube adapter.
- Swap the Scan and X-Ray visor buttons (Input tab, persisted as
  `swap_scan_xray`, off by default): each visor takes the other's D-pad
  direction, as in Remastered (D-pad right Scan, left X-Ray).
- Fast Morph (Input tab and pause Options > Controller, persisted as
  `fast_morph`, off by default): morph ball transitions in the style of Metroid
  Prime 4. Morphing takes 0.2 s instead of 1 s and unmorphing is instant; both
  keep the player's velocity instead of stopping them. Unmorphing on the ground
  holds the speed at walking speed; unmorphing in the air keeps the whole arc
  until landing. Samus's curl-up animation is not shown: the ball forms inside
  the transition flash while the camera eases out, and an unmorph cuts to first
  person behind the transition filter.
- Toggle Lock-On (Input tab and pause Options > Controller, persisted as
  `lock_on_toggle`, off by default): a press of L latches it held (lock-on,
  scan, strafe, grapple) and the next press lets go. The latch also lets go by
  itself when a lock the player had ends (target dead or out of range). Sticky
  Charge (`sticky_charge`, off by default, needs the Charge Beam): holding fire
  for 0.35 s or more keeps the charge held after letting go, and the next press
  fires it; shorter taps shoot as usual. Both are off in morph ball and while
  input is disabled. The console's `status` prints the game's L/A and the charge.
- The overlay's **Controls** tab rebinds pad 1: click Bind, then press the input.
  "Keyboard & mouse" assigns a key or mouse button to each pad button and stick
  axis; "Controller" assigns a physical controller button or axis. Bindings are
  saved by Aurora next to the other controller data, with buttons to clear the
  keyboard bindings and restore the controller defaults. The beam shift and the
  mouse buttons are rows here too (port settings, not Aurora's).
- `MP_HUD_WIDE=1` (Render tab, persisted as `hud_wide`): widescreen HUD. The
  aspect-matched in-game HUD frames keep each element's shape but move it away
  from the screen centre, so edge elements (scan panels, energy bar, map) reach
  the true wide corners instead of being pulled inward. Under a perspective
  camera the element is rotated rigidly about the eye rather than slid sideways,
  since sliding turns off-axis elements away from the viewer and shears them;
  the rotation leaves what is seen of the element unchanged and the angle is
  derived from the aspect ratio, so it holds at any aspect rather than only
  16:9. Menus, the credits and other non-aspect-matched frames are unaffected.
- Field of view (Render tab and pause Options > Display, persisted as `fov`,
  45-90, retail 55): the first-person camera's vertical FOV. The overlay also
  shows the horizontal FOV it gives at the current aspect. The arm cannon is
  drawn at the retail FOV whatever the setting (a view-model FOV), so it keeps
  its size and place. Morph ball, cutscene and other scripted cameras keep
  their own FOVs; cutscenes that end in Samus's eyes ease to the setting.
- Anti-aliasing and anisotropic filtering (Render tab, persisted as `msaa`,
  1 or 4, default 1, and `anisotropy`, 1-16, default 16, 4 on the Quest;
  pause Options > Display has an Anti-Aliasing on/off row): 4x MSAA on the
  scene framebuffer (WebGPU only guarantees 1x and 4x) and the anisotropy cap
  for mipmapped textures, which retail asks for as GX_ANISO_4. Both apply at
  the start of the next frame without a restart; an MSAA change rebuilds the
  framebuffers and pipelines, so it hitches once. Console: `msaa <1|4>`,
  `aniso <1..16>`.
- HUD scale (Render tab and pause Options > Display, persisted as `hud_scale`,
  50-100 percent, default 100): shrinks the combat HUD, radar, beam and visor
  menus and the minimap toward the screen centre, each frame as a whole so the
  pieces stay on the visor frame. The helmet is not scaled. The minimap eases
  back to full size as it opens into the map screen.
- Hide helmet and hide visor effects (Render tab and pause Options > Visor,
  persisted as `hide_helmet`, on by default, and `hide_visor_effects`, off by
  default): the first drops the helmet frame (the dome and the lights at the
  bottom); the second drops the faceplate decoration, Samus's face reflection
  and the on-visor billboard effects (rain, splashes, steam). Console:
  `hudscale <50..100>`, `helmet <0|1>`, `visorfx <0|1>` (0 hides).
- Speedrun timer and LiveSplit (F1 > Extras; the timer also in pause Options >
  Display as In-Game Timer). `speedrun_timer` draws the in-game time (the play
  time the save shows, which stops in cutscenes, menus and loads) in the bottom
  right while a game runs. `livesplit` connects to LiveSplit's TCP server
  (right-click LiveSplit > Control > Start TCP Server) at `livesplit_address`
  (default `127.0.0.1:16834`), retrying every 3 s. A new file sends `reset`,
  `starttimer` and `pausegametime`, so LiveSplit's Game Time then follows the
  in-game time (`setgametime` every 0.1 s of it); compare against Game Time. It
  splits when an upgrade or artifact is first gained (`livesplit_split_upgrades`,
  on by default; not expansions or energy tanks, and items found again after the
  frigate split again) and on the final blow (the EndGame special function).
  Loading a save does not start the timer, and nothing resets it but a new file.
  The logic is `PortLiveSplit::Tracker` (`platform/include/port_livesplit.h`,
  covered by `port_livesplit_tests`). Console: `timer <0|1>`, `igt <seconds>`,
  `livesplit <0|1> | addr <host:port> | send <command> | status`.
- Discord Rich Presence (F1 > Extras > Discord; desktop only, off by default).
  Shows the current room as the activity, with the world, the item percentage
  and Hard mode below it, and time elapsed since the game was loaded; the menus
  show "In the menus". It talks to the local Discord client's IPC socket
  (`$XDG_RUNTIME_DIR/discord-ipc-N`, also the Flatpak and Snap Discord paths;
  `\\?\pipe\discord-ipc-N` on Windows), retries every 5 s and sends at most one
  update per 4 s. There is no built-in application id: create an application at
  https://discord.com/developers/applications (its name is what Discord shows
  as "Playing ..."), optionally upload a Rich Presence art asset named `logo`,
  and paste its Application ID into the overlay (`discord_app_id`). The Flatpak
  build is granted both socket locations. Turning it off clears the activity.
  The wire format is in `platform/include/port_discord.h` (covered by
  `port_discord_tests`). Console: `discord <0|1> | id <application id> | status`.
- Map and progress tracker (F1 > Tracker). Reveal map (`reveal_map`, also
  pause Options > Visor, off by default) shows every world's map as if its map
  station had been used and lists every world on the star map. It only changes
  what the map screen asks, never the save; rooms a map station leaves hidden
  stay hidden. Pickup dots (`map_pickups`, also pause Options > Visor, off by
  default, always on in randomized games) draw a plain white dot on the map and
  minimap for every pickup not yet collected, in rooms the map shows. The dots
  never tell what the item is; an Archipelago game colours them by its logic
  (`map_logic_colors`, see `ARCHIPELAGO.md`). The positions come from
  `tools/gen_map_pickups.py` (`platform/port_map_pickups.inc`). The Tracker
  tab shows item percentage, energy tanks, missile
  capacity (in packs of 5, launcher included), power bombs, artifacts, missing
  upgrades, logbook scans per category (artifacts count at the game's 50%),
  rooms visited per world and the current world's unvisited rooms (their names
  load on first view). Console: `reveal <0|1>`, `pickups <0|1>`, `tracker`.
- Skippable cutscenes (F1 > Extras > Cutscenes, pause Options > Visor,
  `skippable_cutscenes`, off by default; always on in randomizer and
  Archipelago games) lets Start skip every cutscene, including the ones retail
  never lets you skip and ones not yet watched. It applies randomprime's
  "skippable" room-script patches (`tools/gen_skippable_cutscenes.py`
  generates them from two randomprime ISOs) plus its engine tweaks (rotations
  finish at once, a Reset ends a beetle's emergence, no skip without a
  cutscene camera). Rooms a mod replaced are left unpatched. Archipelago
  games also skip the Landing Site intro: Samus starts on top of her ship.
- F1 > Debug holds the audio switches, the MusyX voice list (collapsed) and the
  cheats: health, items, ammo, area and world teleport. The cheats stay hidden
  until Show cheats is ticked (`cheats`, off by default). Invulnerable
  (`invulnerable`, off by default) makes Samus take no damage; it stays on
  across runs until unticked, and `MP_GODMODE=<0|1>` overrides it for one run.
- F1 > Debug > Log, "Write the log to a file" (`log_file`, off by default):
  everything the game prints to stdout/stderr, including the
  line Aurora prints before it aborts, also goes to `metroid_prime_port.log` in
  the user folder; the previous run's is kept as `metroid_prime_port.old.log`.
  Ticking it starts the log at once; unticking stops it at the next start.
  On Linux a forked copy process tees a pipe to the terminal and the file, so
  nothing written before a crash is lost; on Windows the streams go to the file
  only. On Android the logcat lines (port, Aurora, SDL, and stdout/stderr,
  which are copied into logcat under the `stdout` tag while the log runs) are
  written to the file one line at a time; it lives in
  `Android/data/org.metroidprime.port/files/` (reachable over USB) unless the
  data folder was moved to shared storage, where it sits in that folder.
  `MP_LOG_FILE=<0|1>` overrides the setting for one run.
- Save states (F1 > States): eight slots in `savestates/` under the pref
  folder (`slot<N>.mpss`). F5 saves to the selected slot and F9 loads it
  (`savestate_hotkeys`, on by default). A state holds the whole game save
  (items, ammo, doors, pickups, map, scans, world layers) plus Samus's room,
  position, facing and morph ball state; loading reloads that world and puts
  her back. It does not keep enemies, projectiles, cutscene or boss-fight
  progress, or velocity: the room comes back as if just entered. Every load
  first writes slot 0, so "Undo last load" goes back to where you were.
  Saving is refused while Samus is dead. A state saved on an elevator pad
  rides the elevator after loading, like stepping onto it. Console:
  `state list | last | save [n] | load [n] | undo | slot <n>`.
- Memory card transfer (F1 > Extras > Memory card): moves saves between the
  port's card (a GCI folder, `USA/Card A` next to the executable, or the current
  Archipelago game's) and Dolphin's. Import takes a Dolphin `.gci`, a whole raw
  card image (`MemoryCardA.USA.raw`, every Metroid Prime file in it) or, from the
  console, a folder of `.gci` files; only GM8E/01 files are taken. The game
  alternates between `MetroidPrime A` and `B` and loads the newer, so an import
  replaces the whole set: the card's existing game files move to `_replaced/`
  (timestamped, never deleted). Imports are refused in game, since the next save
  would overwrite them; at the front end the file select re-reads the card once
  it is idle (`PortGci::RemountIfChanged` in `CSaveGameScreen::Update`). Export
  copies the game files to a folder, or into a raw image when the path ends in
  `.raw` (backed up to `.raw.bak` first; game files not being exported are
  removed from it). Desktop also has Import from / Export to Dolphin, which finds
  Dolphin's user folder (`XDG_DATA_HOME`/`~/.local/share/dolphin-emu`, the
  Flatpak's, `~/.dolphin-emu`; the registry's UserConfigPath or Documents on
  Windows) and uses its `GC/USA/Card A` folder and/or `GC/MemoryCardA.USA.raw`
  (import takes the more recently written one); close Dolphin before exporting.
  Android uses the system pickers: one save dialog per file on export, so keep
  Dolphin's names (`01-GM8E-MetroidPrime A.gci`). Logic in
  `platform/port_gci.cpp`, covered by `port_gci_tests`. Console:
  `gci list | import <path> | export <dir or .raw> | dolphin import|export`.
- Unlocks (F1 > Extras, persisted as `unlock_hard_mode`, `unlock_fusion_suit`,
  `unlock_galleries`, all off by default): offer what finishing the game
  unlocks without finishing it. Hard mode adds Normal/Hard to a new file; the
  Fusion Suit adds its Disabled/Enabled row under Metroid Fusion Connection
  Bonuses (retail needs a GBA link) and implies normal mode beaten; galleries
  opens all four image galleries. The save's own flags are never written, so
  switching an unlock off locks the extra again (switching the Fusion Suit off
  also takes the suit off unless the save really has it). Metroid (NES) stays
  locked, since its emulator is a PowerPC REL the port can't run.
- The in-game pause and map screens (`FRME_PauseScreen`, `FRME_PauseScreenInstructions`,
  `FRME_MapScreen`) are aspect-matched like the HUD, so they keep their
  proportions and spread across a wide viewport instead of stretching.
- The front end (`FRME_FrontEndPL` title/menu, `FRME_NewFileSelect`, the GBA
  screens) is aspect-matched too, so the title screen is pillarboxed rather than
  stretched.
- The mouse cursor is hidden while the game has focus and is shown only over the
  F1 overlay. The overlay is also openable and navigable with a controller: the
  Start+Back chord toggles it (Start is suppressed for the game while Back is
  held, so it does not also pause), the D-pad or left stick moves, A activates, B
  cancels and the shoulder buttons switch tabs (ImGui gamepad navigation).
- `MP_MOUSE_AIM=1`, `MP_MOUSE_SENS=0.0035`: relative mouse aim. Motion is ignored
  while the overlay is visible or relative capture/focus is absent. Mouse mode
  uses immediate yaw/pitch with an approximately ±87° pitch range. Up moves aim
  up by default; `MP_MOUSE_INVERT_X=1` and `MP_MOUSE_INVERT_Y=1` invert either axis.
  SDL and the compositor own pointer locking; capture is released outside
  playable first person (menus, cinematics, morph ball, and scripted input locks).
- In mouse mode the five mouse buttons act as pad buttons, set in the Controls
  tab's "Mouse buttons" list (`mouse_left`, `mouse_middle`, `mouse_right`,
  `mouse_x1`, `mouse_x2`: none, a pad button, a D-pad direction or the beam
  shift). By default **left-click fires / holds a charge / releases a charged
  shot** (A), **right-click holds lock-on** (L) and **middle-click fires
  missiles** (Y); the side buttons are unset. Either keyboard
  preset restores them too. They feed the normal PAD/gun input path, preserving charge
  timing and weapon cooldowns, and L/R also press the analog trigger fully.
  Keyboard/controller bindings still work alongside them.
  `MP_DISABLE_MOUSE_BUTTONS=1` opts out. Outside playable first person (morph
  ball, text boxes, menus) only the buttons set to A or B count, so the left
  button still lays bombs and advances text. Every mouse button only counts
  after it has been seen released.
- Outside lock-on, A/D (the left-stick lateral axis) strafe in mouse mode rather
  than applying the console's turning torque. Movement uses the current mouse
  heading and the game's acceleration, friction, surface restraints and collision
  handling. Diagonal input/speed is bounded. Lock-on keeps its native orbit/dash
  behavior; closing F1 does not require reacquiring a lock to strafe.
- Mouse mode requests the GC aiming crosshair without holding R or entering the
  console's movement-restricting free-look mode. `MP_DISABLE_MOUSE_CROSSHAIR=1`
  opts out. The Input tab exposes inversion, weapon-button and crosshair toggles.
- Crosshair size (Input tab and pause Options > Controller, persisted as
  `crosshair_size`, 25-100 percent, default 50): scales the free-aim crosshair
  under mouse aim and twin stick, where it is always shown and the retail size
  covers much of the view. Holding R without either keeps the retail size.
  Console: `crosshair <25..100>`.
  Lock-on owns the camera while held; releasing it resumes at the actual locked
  direction rather than at accumulated mouse angles. Jump/fall auto-pitch is
  bypassed during free mouse aim. Capture/UI transitions cancel held charges and
  require mouse-button release before another mouse shot can begin.
- `MP_DISABLE_AI_AUDIO=1`: start streamed AI audio muted. It can subsequently be
  enabled from the overlay. MusyX mute is independent.
- `MP_FAST_BOOT=1`, `MP_SKIP_CUTSCENES=1`, `MP_CUTSCENE_SPEED=8`,
  `MP_SHOW_DEBUG_UI=1`: development controls. Presence flags are enabled by
  being set; unset them to disable them. Cutscene speed is restricted to 1–32.
  Cutscene skipping is only available this way, for tests: it is not a player
  setting, since skipping every cinematic at once broke script state.
  `MP_DEBUG_TAB=<name>` (e.g. `Session`) opens the desktop overlay on that tab,
  enlarged, for captures. The console's `shot` leaves the overlay out; grab the
  X display instead (PIL `ImageGrab.grab(xdisplay=':99')` under Xvfb).
- `MP_BOOT_WORLD=<MLVL hex>[:<MREA hex>]`: tests only. Skips the splash screens and
  the front end and starts a new game (default options, no save card) in that world:
  in its default area, or in the given MREA. It applies once, so quitting the game
  returns to the normal front end. For example, `83F6FF6F` gives Chozo Ruins' default
  room (MREA 3E6B2BB7) in first person about 5 s after launch, and
  `39F2DE28:B2701146` gives the Landing Site. It needs no smoke build, and the
  console's `status` answers as soon as the room is up.
- `MP_TURBO[=<ticks>]`: lockstep for automated runs. Every frame runs exactly
  `<ticks>` fixed ticks (default 1, at most 16) with no frame limiter and no
  vsync, so a run goes as fast as the machine renders it; game time per tick
  stays exact. Under Xvfb presentation caps near 100 fps, so extra ticks per
  frame are what give the speedup: 3000 ticks took 50 s at real time, 30 s at
  `MP_TURBO=1`, 7 s at `4` and 3.3 s at `8`. Audio and streamed music do not
  keep up. Not saved to the settings file. On exit the port prints
  `MP run: <frames> frames in <s> s`.
- `MP_PRESENT_T=<0..1|cycle|tick>`: force the frame-interpolation factor, even
  under `MP_TURBO` or with the frame limiter on (testing only, not saved).
  `cycle` steps through 0, 0.25, 0.5 and 0.75 per drawn frame; `tick` draws the
  plain tick state, as the frame limiter does. The console's `present` sets it
  live; see `docs/FRAME_INTERPOLATION.md` (section 7) for the comparison
  recipe.
- `MP_CONSOLE=<port>` (any build, `1` = 4777, POSIX only): a debug
  command console on 127.0.0.1. Its `press`/`stick` input is read even when
  the window has no keyboard focus. `tools/mpcon.py` is the client: one-shot
  (`tools/mpcon.py 'warp chozo 492CBF4A' 'objs eyeball' shot`), a script
  (`-f file`) or an interactive prompt with no arguments. Commands: `status`
  (world, area, position, whether the camera is first person or a cinematic
  has it, the sky, and the probe mode, weight and PBR draw count),
  `worlds`, `areas`, `warp <world id or name prefix> [mrea]` (replies once the
  new world runs), `tp x y z`, `enter <area>` (makes an area of the current
  world the current one, as walking into it would; `tp` alone does not),
  `face <yaw>`, `look <id>`, `objs [filter]`,
  `obj <id>` (AI state, health, body state and animation, connections, whether
  it is frustum-culled),
  `send <id> <msg>`, `give <item> [n]`, `take <item> [n]`, `items`, `heal`, `god [on|off]`, `press <a+b> [frames]`
  (`sx:<n>`, `sy:<n>`, `cx:<n>`, `cy:<n>` tokens hold stick axes along with
  the buttons, e.g. `press x+sy:127 30`),
  `stick`/`cstick <x> <y> [frames]` (frames `0` on `press`/`stick`/`cstick`
  keeps holding until the next one), `gyro <pitch> [yaw] [frames]` (stand-in
  gyro rates in rad/s), `shot` (prints the bmp path), `present
  <0..1|cycle|tick|off>`, `hold <0|1>` (stop ticking), `step [ticks]` (run
  that many ticks while held), `interp [actor|pose|particle|all <0|1>]`,
  `aspect <4:3|16:9|window>`, `fov <45..90>`, `msaa <1|4>`, `aniso <1..16>`, `hudscale <50..100>`, `helmet <0|1>`, `visorfx <0|1>`, `crosshair <25..100>`, `reveal <0|1>`, `pickups <0|1>`, `tracker`, `state list | last | save [n] | load [n] | undo | slot <n>`, `viewmodel <cmdl> [dist] [yaw] [pitch] | off | status | light <0|1>` (draws any model, retail or a mod's, in front of the camera with the arm cannon hidden; dist 0 fits its bounds; `light 1` swaps the flat white ambient for a key light, which PBR mod materials need to shade), `probe [off|on|mirror|window]` (the PBR reflection probe, live: `mirror` and `window` show the probe itself on PBR materials, as a reflection and looked straight through; no argument prints the mode), `remastered [start <image.nsp> [key file] | cancel]` (the Remastered model import and its progress), `mods [reload]` (what is loaded; `reload` reads the mods folder again), `roomgeo [on|off|overlay | at <x> <y> <z> [margin] | hide <cmdl> | show [cmdl]]` (a mod's room geometry: in place of the retail area, off, or drawn over it; `at` lists the instances whose box holds a point and `hide` stops drawing a model, for finding which one a surface belongs to; no argument prints what is loaded and drawn), `roomgeo lights on|off` (light room geometry with the area's lights even where the room has baked light), `roomgeo pick` (the instances the middle of the view looks through, nearest first, with each model's materials), `roomgeo mats <cmdl>` (a loaded model's materials: flags, PBR or TEV, the PBR record), `roomgeo mat <cmdl> <material> <field> <value...> | mat clear` (changes a value of a material's PBR record as drawn, until cleared or the next start; fields `emissive`, `backlight`, `height`, `mode`, `kind`, `strength`, `p0`-`p3`, or an index 0 to 18; emissive multiplies the emissive map, so it shows only on a material that has one), `roomliquid [on|off]` (a mod's liquid surfaces in place of the retail fluid planes; no argument prints what is loaded and drawn), `roomenv [on|off|exposure on|off|volume on|off|ambient <scale>|show off|coords|light|info [<x> <y> <z>]]` (room environments: `volume` is the baked light per pixel, `ambient` scales the baked ambient, `show` draws the grid's coordinates or light in place of the surface, `info` prints exposure, tone curve, probe and baked ambient at the view or a point), `view [off|albedo|normal|rough|metal|ao|ambient|reflection|glow|exposure|kind]` (what PBR surfaces show: one input of the shading in place of the result), `stats` (the last frame's draws and buffers, the heap, room geometry and environments), `hdfont [on|off]`, `touchpad [attach|detach|stick <x> <y>]` (a virtual gamepad of the kind Android's touch overlay uses, to test controller hotplug against it on any platform), `freecam [on|off|freeze on|off|player on|off|speed <n>|pos <x> <y> <z>|look <yaw> <pitch>]` (see below), `timer <0|1>`, `igt <seconds>`, `livesplit <0|1> | addr <host:port> | send <command> | status`, `discord <0|1> | id <application id> | status`, `gci list | import <path> | export <dir or .raw> | dolphin import|export`, `ap [connect <server> <slot> [password] | disconnect | recent | resume <n> | say <text> | chat]`, `wait <frames>`, `quit`; `help` lists them. Ids are hex editor ids, `u<n>`
  unique ids or exact debug names. Every reply ends with `=> ok` or
  `=> err: <why>`, and the client exits 1 if any command failed. Game commands
  run inside the state manager tick, so they fail with "not ticking" on the
  title screen or while paused. Pair with `MP_TURBO` for speed.
- Free camera (F1 > Debug > Camera, or the console's `freecam`): the world is
  drawn from a viewpoint of its own while the player stays put, without the
  HUD and the arm cannon. It starts at the game camera; the left stick moves,
  the C stick (or a captured mouse) turns, R is four times as fast, L and Z
  (or the d-pad's down and up) go down and up. `freeze` stops the game's simulation while it flies,
  which also makes two screenshots of one view comparable. Samus's body is
  drawn where the player stands (`player off` or "Show Samus" hides it). The same page's
  Rendering section has the console's `view`, `probe`, `hdfont`, `roomgeo` and
  `roomenv` switches.
- `MP_PBR_PROBE=<off|on|mirror|window>` (or 0-3): the reflection probe PBR mod
  materials reflect, on by default. The console's `probe` changes it live.
- `MP_PBR_ANISO=<1-16>`: the most anisotropic filtering a PBR mod's native maps
  take, 2 by default whatever the Anisotropy setting says: higher levels turn a
  tiled floor into streaks towards the horizon.
- `MP_GODMODE=<0|1>`: the Invulnerable cheat for this run, whatever the setting
  says. The console's `god [on|off]` changes the setting itself.
- `MP_LOG_FILE=<0|1>`: the file log (`metroid_prime_port.log` in the user
  folder) for this run, whatever the `log_file` setting says.
- `MP_ROOM_GEO=<0|1|overlay>`: whether a mod's `.roomgeo` replaces an area's
  geometry (default 1; `overlay` draws both). Console `roomgeo [on|off|overlay]`,
  which also prints what is loaded and what the last frame streamed.
  `MP_FRAME_BUFFERS=<1..16>` scales the buffers a frame's vertices, arrays and
  uniforms are streamed through (1 = 5 + 8 + 24 MiB); it is 12 when a mod has
  room geometry. A frame that outgrows them aborts with a buffer overflow.
- `MP_ROOM_LIQUID=0`: ignore the mods' `.roomliquid` files and draw the
  retail fluid planes (console `roomliquid [on|off]`, which also counts what is
  loaded and drawn).
- `MP_ROOM_ENV=0`: ignore the mods' `.roomenv` files (console `roomenv
  [on|off]`, which also counts what is loaded). For tuning:
  `MP_ROOM_ENV_GAIN` (exposure, default 1), `MP_ROOM_ENV_LOD` (the mip a
  roughness of 1 reflects, default 5) and `MP_ROOM_ENV_AMBIENT` (scale of the
  baked ambient light, default 1; 0 keeps the game's ambient colour).
  `MP_ROOM_ENV_EXPOSURE=0` (console `roomenv exposure on|off`) exposes each
  cube on its own instead of the frame by the camera's room, and drops the
  room's tone curve. Files older than version 4 lack the exposure bias and
  the curve's contrast, so re-import to get the right levels.
  Room geometry is lit by the baked ambient grid per pixel, as a 3D texture,
  and takes no area lights (Remastered has no lightmaps; this grid is its room
  lighting). `MP_ROOM_ENV_VOLUME=0` goes back to the area's lights, as does
  `MP_ROOM_GEO_AREA_LIGHTS=1`. For tuning: `MP_ROOM_ENV_VOLUME_BIAS` (metres off
  the surface a sample is taken, default 0.25) and `MP_ROOM_ENV_VOLUME_SHOW`
  (1 draws the texture coordinates, 2 the light alone, 3 the shading normal).
- `tools/pbr_shots.py`: contact sheets of models under PBR, for comparing mod
  builds. One game per (variant, place), each booted straight into the room
  with `MP_BOOT_WORLD` on its own console port and an Xvfb display, about 10 s
  a game. `--variant name=<mod dir>|none` (columns), `--place
  chozo|tallon|frigate|phendrana|mines|magmoor|crater`, `<MLVL>[:<MREA>]` or
  `name=<file.mpss>` for an exact spot from a save state, `--model <cmdl>`,
  `--probe off,on`, `--yaw`. Writes `<out>/<place>.png` and a `.tsv` with the
  mean luminance of each cell, and exits 1 naming any game that never reached
  first-person gameplay. Needs a smoke build (`--build`, default
  `build/fm-smoke`) and `MP_ISO` or `--iso`. Do not use `MP_SMOKE_WORLD` for
  this: it starts a new game and only warps once the Frigate intro has played.
- `MP_TOUCH_UI=1`: force the page layout for the debug overlay even when
  Render > "Overlay as a floating window" is set. The page layout is the default
  everywhere (the only one on Android): a full-screen window inside the safe
  area, with a page list instead of tabs, larger hit targets, drag-to-scroll
  with fling for touches and a Close button.
- `MP_VALIDATE_SAMPLES=1`: log MusyX sample-directory validation.
- `MP_SIM_RATE=<hz>`: experimental simulation tick rate (30–480, default 60).
  60 is console-accurate; higher values step the game logic at the display rate
  instead of interpolating the camera. `MP_SIM_ADAPTIVE=1` instead takes one
  step per frame with `dt` = the measured frame time (clamped 30–480 Hz), so a
  variable frame rate is matched exactly. Both are also settable from the F1
  Performance tab and persisted. See `docs/HIGH_FPS_AUDIT.md` for what still
  assumes 60 Hz.

The simulation uses a fixed-step accumulator (60 Hz by default) independently of
the presentation cap. Ordinary slow frames catch up; pauses/debugger stalls are
capped to 250 ms of simulation work per iteration. Audio runs on
wall-clock/device consumption.
Fractional simulation time is retained through frame jitter. The capped scheduler
can borrow at most 0.25 ms near a tick boundary and carries that debt forward, so
it does not alternate zero/two ticks merely due to microsecond sleep jitter.
The Performance tab distinguishes the **60 FPS target** from measured render FPS
and simulation ticks/second. `MP_TRACE_TIMING=1` logs both rates once per second.
Hidden windows continue pumping events and main-thread audio without recording
rendered frames. Restart-to-menu rebuilds the game architecture instead of
attempting a console reboot.

In uncapped presentation, free mouse aim uses the current simulation orientation
so the visible reticle does not lag the shot direction; camera translation still
interpolates. The held cannon/arm and muzzle effects render against the matching
simulation camera, then restore the world view before world-space effects. This
keeps the viewmodel stable instead of mixing an interpolated view with a cached
60 Hz gun transform. Weapon animation and projectile simulation remain 60 Hz.

Per-frame look (`frame_interpolation`, F1 Performance "Per-frame look
(uncapped)", on by default) turns the presented view every rendered frame by the
look input the next tick will consume: pending mouse and gyro deltas, plus
twin-stick velocity times the time since the tick. The tick still applies the
whole amount, so aim and shots are unchanged; the free-aim crosshair is rotated
with the view so it stays centred. It only applies with the frame limiter off,
under free mouse look (mouse aim, gyro aim or twin stick). The game's own stick
look is not previewed.

Smooth actor motion (`actor_interpolation`, F1 Performance, off by default,
experimental) draws each moving actor, Samus and the morph ball included,
between its last two tick transforms when the frame limiter is off. Moves of
more than 4 units or 45° in a tick snap. The arm cannon's bob and sway blend
too. Queued particles, shadows and the HUD sway still step at 60 Hz. `docs/FRAME_INTERPOLATION.md` has the design and scopes the rest
(particles, projectiles).

Smooth animation (`pose_interpolation`, F1 Performance, off by default,
experimental) skins animated models with a per-bone blend of the poses built on
the last two ticks when the frame limiter is off. The animation tree is not
touched, so events, sounds and particles are unchanged; attachments on locators
and swarms stay on the tick pose. A bone that turns more than 45° or moves more
than 4 units in a tick snaps the whole pose.

Smooth particles (`particle_interpolation`, F1 Performance, off by default,
experimental) draws particle effects, projectile effects included, between
their last two 60 Hz positions when the frame limiter is off, using the game's
own sub-frame particle path. Beam trails (swooshes), electric effects and
the flamethrower still step at 60 Hz.

## Ownership and threading rules

- Use `rstl::auto_ptr<T[]>` / `single_ptr<T[]>` for host arrays and scalar owners
  for host objects. `rstl::game_memory<T>` selects `CMemory::Free` for game-heap
  buffers. `rs_new` is ordinary host `new` in the native build.
- `rstl::auto_ptr` still transfers ownership on copy and `release()` retains a
  non-owning view; do not treat it like `std::unique_ptr`.
- Reference-counted and resource owners capture a deleter where the type is
  complete; their release sites can safely live in forward-declaration-only
  translation units. This avoids inconsistent template destructor definitions.
- MusyX pins resources for its bounded pushed-group stack. Group pop holds the
  IRQ mutex while detaching software voices and unregistering data, so samples
  can be freed afterwards. Audio output is joined before DSP state is destroyed.
- DVD callbacks can run on the DVD worker. ARAM file queue/state/lifetime changes
  share one recursive mutex. ARQ completions are pumped on the main thread.
- FIFO draw-sync tokens protect CPU-side consumption of referenced arrays.
  Synthetic VI retraces are bookkeeping callbacks, not WebGPU completion fences.
- If `BeginScene()` returns false, do not draw or call `EndScene()`. Delayed
  render-resource retirement advances only after a successful frame.

Saves write the retail big-endian CRC word. Reading also accepts the early
native port's little-endian CRC representation for compatibility. Bit fields
are serialized MSB-first on both host byte orders.

## Validation

CTest's `port` label covers array/game-heap ownership, golden save bit fields,
truncated-stream errors, pathfinder bitset bounds, AI enable/callback teardown,
the MusyX pointer-sized DMA API, and DOL section mapping/bounds. It does not
require a disc or GPU.
`port_mouse_tests` checks direction/inversion, pitch limits, yaw normalization,
lock and non-first-person handoffs, movement bounds and held-button/capture edge
behavior. `port_timing_tests` covers fractional-time carry and capped jitter;
`port_audio_math_tests` covers ADPCM partial loops, PCM8 and wide Q15 mixing.
The CARD regression creates its own `card-test-data` directory under the build
tree, tests null callbacks and file operations, and closes/reopens the backing
store before comparing the saved bytes. It never uses the normal game profile.
Real-disc smoke runs also created a native `MetroidPrime A.gci` in an isolated
profile and reopened that profile in a subsequent process.

For a real-disc lifecycle run, configure with `-DMP_ENABLE_SMOKE_DRIVER=ON`:

```sh
MP_FAST_BOOT=1 MP_SKIP_CUTSCENES=1 MP_DISABLE_AI_AUDIO=1 \
MP_SMOKE_FRAMES=2400 MP_SMOKE_LIFECYCLE=1 SDL_AUDIO_DRIVER=dummy \
./build/native/metroid_prime_port "/path/to/Metroid Prime (USA) (v1.00).iso"
```

This opt-in driver hides/restores the window, mutes/unmutes both audio paths,
toggles presentation pacing, restarts to the menu, and exits normally. Omit
`MP_SMOKE_LIFECYCLE` for a bounded ordinary run. Production builds omit the
driver unless enabled at configure time.

`MP_SMOKE_MOUSE=1` enables an additional deterministic real-disc scenario in that
build. Run it with `MP_FAST_BOOT=1 MP_SKIP_CUTSCENES=1 MP_SMOKE_FRAMES=1800` and an
isolated `MP_USER_PATH`/`MP_CACHE_PATH`. It injects mouse input without grabbing
the real pointer, verifies immediate camera aim, live power/missile projectiles,
charged release, lock/release, jump and morph-ball handoffs, a left-click bomb in
morph ball, UI charge cancellation,
crosshair state, and cannon/view orientation while uncapped. Scripted cinematics
pause the test sequence; the frame limit is a minimum until the scenario finishes.
Success is reported as `[mouse-smoke] passed` followed by a clean exit.
The sequence also verifies free strafing on both sides of opening/closing F1.

`MP_SMOKE_AREA_RELOAD=1` exercises three real geometry eviction/ARAM restoration
cycles. It reproduced the material-flags crash seen when opening a door before
the one-time native surface-header conversion fix. It can be combined with the
mouse scenario and reports `[area-smoke] passed`.

`MP_SMOKE_WORLD=<hex MLVL id>`, or `MP_SMOKE_WORLD=auto` to pick the first world
other than the current one, jumps to another world through the same restart path
the in-game world teleporters use. It waits for gameplay, requests the jump, and
reports `[world-smoke] passed: world <id> area <n>` once a freshly constructed
world is running. The F1 debug overlay's Debug tab (with Show cheats ticked)
lists every world by its front-end name and jumps to it on click.

`MP_SMOKE_ELEVATOR=<ticks>` rides the elevator most recently loaded in the current world
once gameplay has run for `<ticks>` ticks. It sends the elevator's `Play` and
`SetToZero` messages, like the ride trigger does, and reports
`[elevator-smoke] passed: world <id> area <n>` once the destination world is
playable. Combine it with `MP_SMOKE_WORLD=83F6FF6F` (Chozo Ruins, whose spawn
area loads the Tallon elevator). It reproduced the elevator crash:
`CWorldTransManager::WaitForModelsAndTextures` bounced model buffers through ARAM,
which over-read them and freed host `new[]` memory into the game heap. The port
now skips that model pass.

`MP_SMOKE_VISOR=1` grants and switches to the thermal visor (`MP_SMOKE_VISOR=xray`:
the X-ray visor) after gameplay starts and reports `[visor-smoke] passed` once it has stayed up. It reproduces
the FIFO-worker crash where the game binds a texture whose source pointer is an
unmapped value, which the content hash then dereferences. The thermal cold blend
was passing a deliberately fake random address as its noise texture (the console
reads raw memory for noise); the port now fills a real scratch noise buffer.
Aurora also skips any texture whose source page is not mapped instead of hashing
it; set `MP_LOG_TEX_INVALID=1` to log each rejected texture's pointer, format,
size and object id.

`MP_SMOKE_WALK=<ticks>` holds the stick fully forward after gameplay starts and
reports `[walk-smoke] passed: ticks=... dist=... maxFlatSpeed=... speed=.../s`.
Run it with the same real duration at two simulation rates (for example 60 ticks
at 60 Hz and 120 ticks at 120 Hz) to confirm ground movement stays real-time.
With `MP_SMOKE_WORLD` set, the walk waits until the world jump has finished, so
it starts in the destination room. For example, this reaches the Parasite Queen
fight (Frigate Orpheon, Reactor Core) in about 15 s:
`MP_TURBO=8 MP_SMOKE_WORLD=158EFE17 MP_SMOKE_WORLD_AREA=87452DC1 MP_SMOKE_WALK=3000 MP_SMOKE_FRAMES=1500`
together with `MP_FAST_BOOT=1 MP_SKIP_CUTSCENES=1`.

`MP_SMOKE_STICK=1` holds the right stick and reports the aim yaw change, to
verify twin-stick aiming (run with `MP_TWIN_STICK=1`).

`MP_SMOKE_DASH=1` turns the player until an orbit target is offered, locks on with
L, holds the stick right and presses B, then reports
`[dash-smoke] passed/failed: lockedTicks=... dashTicks=...`. `MP_SMOKE_DASH=scan`
switches to the scan visor first and locks onto scan points, for rooms with no
targetable enemies; Reactor Core (the walk example above) passes that way. Run it
plain, with `MP_MOUSE_AIM=1` and with `MP_TWIN_STICK=1`: all three should dash.

`MP_SMOKE_PAUSE=<ticks>` enters the pause screen after gameplay starts;
`MP_SMOKE_MAP=<ticks>` presses Z that many ticks into gameplay to open the map
screen (Z opens the map from gameplay; in the pause screen it does nothing).
Combine with `MP_SMOKE_SHOT` to capture them.
`MP_SMOKE_FRONTEND=<frame>` taps Start every 300 frames from that frame, so the
front-end screens (title, save dialogs, main menu) are reached without a player;
Start alone leaves dialogs on screen rather than dismissing them.
`MP_SMOKE_CONTINUE=1` walks the title and file select to Continue on slot 1. It
does not combine with `MP_FAST_BOOT`: the walker only acts at file select, and
fast boot is what leaves file select, so with both set the run quietly becomes a
new game instead. This has always been so; set only `MP_SMOKE_CONTINUE` to test
the Continue path.

For audio reports, `MP_AUDIO_STATS=1` logs MusyX's generated samples/second, queued
audio, peak output and clipping. Nominal output is 32,000 stereo frames/second;
short windows vary with the device's buffering. Static ADPCM loops must wrap at
`loop + loopLength` and restart from the exact loop nibble/history. Streaming
ADPCM retains its predictor history across ring-buffer wraps. These rules are
now distinct; neither is tied to the renderer's frame count.

For stuck or unexpected sounds, `MP_LOG_VOICES=1` logs the active MusyX voices
(sample id, source pointer, length/loop, compression, pitch, volumes and the
listener heading) about three times a second, and `MP_MUTE_SMP=65535,93` silences
voices by sample id so a persistent one can be identified by ear. Streamed
voices report sample id 65535. `MP_LOG_3D=1` logs any 3D emitter whose Doppler
factor is not 1. The overlay's **Voices** tab lists the live voices (loudest
first) with a per-sample mute checkbox and an "Unmute all" button; the muted ids
are saved to `voices_muted` in the settings file.

A main-loop iteration that takes longer than 250 ms logs `MP stall: frame N took
… ms (events, input, tick, draw)`, at most one line every two seconds, and a run
of frames that were not presented logs `MP stall: N frames not presented over …
ms` when presenting resumes. On Android they go to logcat under the tag
`metroidprime` (`adb logcat -s metroidprime`). `MP_NO_STALL_LOG=1` turns them off.

For doors that stay shut, `MP_LOG_DOORS=1` prints `MP door <id> opened after N
ms` with the ticks spent on each condition that held it (`anim`, `thisArea`,
`sky`, `areaLoad`, `actors`, `otherDoor`, `occluding`, `aram`, `map`), `still
waiting` every 3 s while a wait lasts (followed by `now on otherDoor <id>` or
`now on occluding <area>` when the condition names an object, or `sky: …` with
the skybox state, the sky model's lock/build/queue state and the loader list size
while the world sky is pending, and, per unloaded sky texture, whether it is
locked, loading and in the loader's queue: 0 absent, 1 read pending, 2 read
done), `MP sky <id> requested` / `model built after N ms` / `textures loaded
after N ms` whenever the world sky has to be (re)loaded (the port keeps it
resident, so after world load this should not appear), and
`opened at once`, `ignored Open
(inactive)`, `refused Open (area missing)` or `wait cancelled by Close` for the
other outcomes (a trigger's Open usually ends a wait, since newer objects think
first). Console `obj u<n>` shows an actor's touch bounds. `MP area`
lines for each area's stream start, dependencies ready, load time and
cancellation, and `streaming held N ticks` when the world held streaming back.

For AddressSanitizer, use a separate Clang build with
`-DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer"` and the same
`CMAKE_CXX_FLAGS`. Keep allocation/deallocation mismatch checks enabled.
`ASAN_OPTIONS=detect_leaks=0` can isolate memory-safety/lifetime checks from
process-global caches; such a run does not establish leak freedom.

With `-DAURORA_ENABLE_TESTS=ON`, build and run `gx_fifo_tests` under
`build/native/extern/aurora/tests/`. This GPU-free suite includes concurrent
producer/worker append, buffer growth, and draw-sync ordering. All 205 FIFO/GX
tests passed in the local ThreadSanitizer build. Full-application TSan validation
is currently blocked by reports in uninstrumented system GLib/libdbus and the
prebuilt Rust nod preloader before game execution; no whole-game TSan-clean
claim is made. The standalone FIFO suite avoids those dependencies.

Manual validation still matters: door/room traversal, map/pause transitions,
retail/Dolphin save import/export, interrupted saves, controller hot-plugging,
audio pitch/tempo by ear, and Windows packaged gameplay on a clean machine.
The automated lifecycle driver does not claim full-game coverage.

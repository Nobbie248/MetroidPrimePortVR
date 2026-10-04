#ifndef METROID_PRIME_PORT_PORT_DEBUG_H
#define METROID_PRIME_PORT_PORT_DEBUG_H
#include <chrono>
#include <cstdint>
#include <string>

// Runtime debug settings shared between the platform layer and the game.
// Defaults come from environment variables so existing workflows keep working,
// and the in-game debug window can toggle them live.

class CStateManager;

namespace PortDebug {

// Live game state for the debug menu (set every simulation tick by
// CStateManager::Update). Null before gameplay starts.
void SetStateManager(CStateManager* mgr);
CStateManager* StateManager();
// Where the world is drawn from and which way it looks (the free camera's while it is
// on); false outside a world.
bool ViewRay(float origin[3], float forward[3]);
// What PBR surfaces show in place of their shaded result (GXSetPBRDebugView); 0 is off.
int PbrViewCount();
const char* PbrViewName(int view);
int PbrView();
void SetPbrView(int view);
// Requests an area change; consumed and executed by the game update so it does
// not run from the render/UI path.
void RequestTeleport(int areaId);
bool ConsumeTeleportRequest(int& areaId);
// Requests a jump to a different world (MLVL). areaAssetId is a MREA asset id,
// or 0 to land in the world's default area. The game update runs the same
// restart the in-game world teleporters use, so the world is fully reloaded.
void RequestWorldTeleport(uint32_t worldId, uint32_t areaAssetId);
bool ConsumeWorldTeleportRequest(uint32_t& worldId, uint32_t& areaAssetId);
// Opt-in pickup-dump tour (MP_RANDO_SWEEP=1, with MP_RANDO_DUMP=1). Advance
// once per gameplay simulation tick; returns true when a restart was queued.
// Duplicate sweep requests and requests conflicting with a teleport are ignored.
void RequestWorldSweep();
bool ConsumeWorldSweepRequest(CStateManager& mgr);

// Disc image chosen on a previous launch, from the settings file. SetDiscPath
// marks the settings dirty so the choice is written back out on exit.
void LoadDiscPath();
const char* DiscPath();
void SetDiscPath(const char* path);

// Fast iteration
bool FastBoot();
// MP_BOOT_WORLD=<MLVL hex>[:<MREA hex>]: no splash screens or front end; a new game
// starts straight in that world (its default area without an MREA). Tests only.
bool BootWorld(uint32_t& worldId, uint32_t& areaAssetId);
// MP_SKIP_CUTSCENES / MP_CUTSCENE_SPEED (tests only; not a saved setting).
bool SkipCutscenes();
float CutsceneSpeed();
// Simulation tick rate. 60 is console-accurate; higher values run the tick at
// the display rate instead of interpolating presentation. Experimental.
unsigned SimRate();
void SetSimRate(unsigned hz);
// The current simulation step in seconds (1 / SimRate()).
float SimPeriod();
// When enabled the simulation step follows the measured frame time (clamped to
// a sane range) instead of SimRate(), so a variable frame rate is matched
// tick-for-tick. Experimental.
bool SimAdaptive();
void SetSimAdaptive(bool enabled);
// Per-frame look (docs/FRAME_INTERPOLATION.md, phase 1): with the frame limiter
// off, frames between ticks show the look input the next tick will apply.
bool FrameInterpolation();
void SetFrameInterpolation(bool enabled);
// Actor transform smoothing (phase 2): with the frame limiter off, actors draw
// at a blend of their previous and current tick transforms. Off by default.
bool ActorInterpolation();
void SetActorInterpolation(bool enabled);
// Pose smoothing (phase 3): with the frame limiter off, skinned models draw a
// blend of their previous and current tick poses. Off by default.
bool PoseInterpolation();
void SetPoseInterpolation(bool enabled);
// Particle smoothing (phase 4): with the frame limiter off, particle systems
// draw between their previous and current tick frames. Off by default.
bool ParticleInterpolation();
void SetParticleInterpolation(bool enabled);
// Frame interpolation tests (phase 5). MP_PRESENT_T=<0..1> or "cycle" (console
// `present`) forces the presentation factor, even under MP_TURBO or with the
// frame limiter on; cycle steps through 0, 0.25, 0.5 and 0.75 per drawn frame;
// "tick" draws the plain tick state (-1) as the frame limiter does.
// Replaces t and returns true when forced. Not saved.
bool PresentOverride(float& t);
// value in [0,1] = fixed, kPresentCycle = cycle, kPresentTick = tick state,
// anything negative = off.
constexpr float kPresentCycle = 2.f;
constexpr float kPresentTick = 3.f;
void SetPresentOverride(float value);
float PresentOverrideValue();
// Console `hold`/`step`: while held, main loops run no ticks except the ones
// queued by StepTicks, so one tick state can be drawn at several factors.
bool TickHold();
void SetTickHold(bool held);
void StepTicks(unsigned count);
unsigned PendingHeldTicks();
unsigned TakeHeldTicks();
// MP_TURBO[=<ticks>]: lockstep for tests. Every loop runs exactly <ticks> fixed
// ticks (default 1, at most 16) and nothing waits for the wall clock, so a run
// goes as fast as the machine can render it; more ticks per frame skip
// presents, which is what limits a run under Xvfb. Game time stays exact per
// tick; audio and streams do not keep up. Not saved to the settings file.
bool Turbo();
unsigned TurboTicks();
// The dt of the simulation tick currently being processed, set by
// CGameArchitectureSupport::UpdateTicks. Game constants authored per 60 Hz tick
// (friction, damping) scale by this so they stay real-time at any rate.
float TickPeriod();
void SetTickPeriod(float dt);
// The same step expressed in 60 Hz frames (1.0 at 60 Hz). Per-tick counters and
// cadences add this instead of 1 so their real-time timing is unchanged.
float TickFrames();

// Presentation
bool FrameLimitEnabled();
void SetFrameLimitEnabled(bool enabled);
void RecordFrame(uint64_t durationNs, unsigned ticks, bool presented);
bool VsyncEnabled();
void SetVsyncEnabled(bool enabled);
// Setting `fullscreen`: a borderless fullscreen window on desktop (F11 toggles
// it), the status and navigation bars hidden on Android. Applied at window
// creation; Set changes the live window too.
bool Fullscreen();
void SetFullscreen(bool enabled);
// 0 = auto (native, driven by the display scale), otherwise a fixed multiplier.
float RenderScale();
void SetRenderScale(float scale);
// Rendering aspect ratio. kAspect_4_3 is the game's original 640x480.
// kAspect_16_9 widens to 16:9; kAspect_Window follows the window and updates
// live as it is resized.
enum EAspectMode {
  kAspect_4_3 = 0,
  kAspect_16_9,
  kAspect_Window,
};
EAspectMode AspectMode();
void SetAspectMode(EAspectMode mode);
// Current size of the game window in window coordinates; false before it exists.
bool WindowSize(int& width, int& height);
// Framebuffer width that is shown at 4:3 for the given height: the width itself
// in 4:3 mode (640x448 is displayed at 4:3), height * 4 / 3 otherwise, since the
// widened framebuffers have square pixels. Fits 4:3 art into a wider viewport.
inline int FourThreeWidth(int width, int height) {
  return AspectMode() == kAspect_4_3 ? width : height * 4 / 3;
}
// Widescreen HUD: keep each HUD element's shape but spread its position about
// the screen centre so edge elements reach the true wide corners. Only affects
// the aspect-matched in-game HUD frames.
bool HudWide();
void SetHudWide(bool enabled);
// HUD scale in percent (50-100). Compact HUD elements shrink toward the nearest
// screen edge or corner; screen-spanning decoration keeps its size. Only the
// combat/scan/ball HUD frames and the minimap, not the helmet or menus.
const int kHudScaleMin = 50;
const int kHudScaleMax = 100;
int HudScale();
void SetHudScale(int percent);
// Hide the helmet frame (the visor rim and its glow and lights).
bool HideHelmet();
void SetHideHelmet(bool enabled);
// Hide visor effects: steam, Samus's face reflection, and rain, water, goo and
// hit splashes on the visor.
bool HideVisorEffects();
void SetHideVisorEffects(bool enabled);
// Hide the HUD's minimap, hundreds of draws a frame in a busy area.
bool HideMinimap();
void SetHideMinimap(bool enabled);
// Where the game thread's frame goes (Performance tab): the game's update, the
// world's draw and the HUD's, timed in the game code and averaged per frame.
enum FramePhase { kPhaseUpdate = 0, kPhaseDrawWorld = 1, kPhaseDrawGui = 2, kPhaseCount = 3 };
void NoteFramePhase(FramePhase phase, uint64_t nanoseconds);
struct ScopedPhaseTimer {
  explicit ScopedPhaseTimer(FramePhase p) : phase(p), start(std::chrono::steady_clock::now()) {}
  ~ScopedPhaseTimer() {
    NoteFramePhase(phase, static_cast<uint64_t>((std::chrono::steady_clock::now() - start).count()));
  }
  ScopedPhaseTimer(const ScopedPhaseTimer&) = delete;
  ScopedPhaseTimer& operator=(const ScopedPhaseTimer&) = delete;
  FramePhase phase;
  std::chrono::steady_clock::time_point start;
};
// Every world's map shows as if its map station had been used; the save is
// not changed.
bool RevealMap();
void SetRevealMap(bool enabled);
// A white dot on the map for each item pickup not yet collected. Randomizer
// and Archipelago games force it on (PortMapPickups::Active).
bool MapPickups();
void SetMapPickups(bool enabled);
// In an Archipelago game, colour the dots by the seed's logic like a tracker
// (green in logic, yellow sequence break, blue visible only, red out of reach,
// grey checked). On by default; off gives the plain white dots.
bool MapLogicColors();
void SetMapLogicColors(bool enabled);
// The apworld's staggered_suit_damage, which the server doesn't send: 0 the
// game's rule, 1 progressive (the apworld's default, and the port's), 2
// additive. Only an Archipelago seed uses it.
int ApSuitDamage();
void SetApSuitDamage(int mode);
// randomprime's skippable cutscenes: every cinematic can be skipped with the
// usual button. Rooms load patched, so a change applies to rooms loaded
// afterwards. Randomizer and Archipelago games force it on
// (PortSkipCutscenes::Active).
bool SkippableCutscenes();
void SetSkippableCutscenes(bool enabled);
// First-person vertical field of view in degrees (retail 55). The arm cannon is
// drawn at the retail FOV whatever this is, like a view-model FOV.
const float kFovRetail = 55.f;
const float kFovMin = 45.f;
const float kFovMax = 90.f;
float FirstPersonFov();
void SetFirstPersonFov(float degrees);
// Multisample anti-aliasing (1 = off, or 4) and the max texture anisotropy
// (1-16, retail-style mipmapped textures ask for the max). Applied next frame.
int Msaa();
void SetMsaa(int samples);
int Anisotropy();
void SetAnisotropy(int level);
// Extras normally earned by finishing the game (or, for the Fusion Suit, by a
// GBA link to Metroid Fusion). They only change what the title screen offers;
// nothing is written into the save's persistent flags.
bool UnlockHardMode();
void SetUnlockHardMode(bool enabled);
bool UnlockFusionSuit();
void SetUnlockFusionSuit(bool enabled);
bool UnlockGalleries();
void SetUnlockGalleries(bool enabled);

// Mouse FPS mode owns aim only in playable first person; target locks retain
// their native camera and synchronize the mouse angles for a clean handoff.
// Motion/buttons are accepted only while SDL owns relative capture.
bool MouseAim();
void SetMouseAim(bool enabled);
// Twin-stick: the right stick aims the first-person camera directly (through the
// same aim state as the mouse) and is consumed, so it no longer drives the
// game's free-look. Works with or without mouse aim.
bool TwinStick();
void SetTwinStick(bool enabled);
// Right stick Y (-1..1) before twin-stick consumed it, for the Spring Ball;
// 0 when twin-stick is off (the game input still carries it then).
float TwinStickRightY();
void SetTwinStickRightY(float y);
// The bound beam shift is held in game this poll (no overlay, window focused),
// which springs the Spring Ball in morph ball, as X does in Remastered.
bool BeamShiftHeld();
void SetBeamShiftHeld(bool held);
// Spring Ball (C-stick up in morph ball, as in Metroid Prime Trilogy) once the
// Morph Ball Bombs are held. A connected Archipelago seed overrides it.
bool SpringBall();
void SetSpringBall(bool enabled);
// The Scan and X-Ray visors trade D-pad directions (Remastered's Dual Sticks
// layout), off by default.
bool SwapScanXray();
void SetSwapScanXray(bool enabled);
// The beam shift's bindings (PortControls::ShiftHeld): slots 0 and 1 are keys
// or mouse buttons (scancode or PAD_KEY_MOUSE_*), slot 2 a controller button
// (SDL gamepad button or PAD_NATIVE_BUTTON_TRIGGER_*); -1 for none.
int ShiftBinding(int slot);
void SetShiftBinding(int slot, int code);
// A second controller button for a GameCube button (Aurora maps one each),
// ORed in by CDolphinController: `bit` is the PAD_BUTTON_* / PAD_TRIGGER_* bit's
// position, the code as ShiftBinding's slot 2; -1 for none.
constexpr int kPadAltCount = 16;
int PadAltButton(int bit);
void SetPadAltButton(int bit, int code);
// What mouse button `button` (0 left, 1 middle, 2 right, 3 X1, 4 X2) does under
// mouse aim: a PortInputMap::EMouseAction.
int MouseAction(int button);
void SetMouseAction(int button, int action);
// Speedrun support: an on-screen in-game time (the play time the save shows)
// and the LiveSplit Server client (port_livesplit.h), which connects to
// "host:port" while enabled.
bool SpeedrunTimer();
void SetSpeedrunTimer(bool enabled);
bool LiveSplit();
void SetLiveSplit(bool enabled);
std::string LiveSplitAddress();
void SetLiveSplitAddress(const std::string& address);
// Discord Rich Presence (port_discord.h), connecting while enabled and an
// application id (digits only) is set.
bool DiscordPresence();
void SetDiscordPresence(bool enabled);
std::string DiscordAppId();
void SetDiscordAppId(const std::string& id);
// Mods folder (port_mods.h): all mods on or off, and the folder names turned
// off, '/'-separated. Both take effect on the next launch.
bool ModsEnabled();
void SetModsEnabled(bool enabled);
std::string ModsDisabled();
void SetModsDisabled(const std::string& list);
// Memory card transfer to and from Dolphin (port_gci.h), for the overlay and
// the console. Each returns a message for the user. Imports are refused in
// game; `path` may be a .gci, a raw card image, a folder of .gci files or an
// Android content:// URI, and an export to a path ending in .raw writes a card
// image instead of .gci files.
std::string CardList();
std::string CardImport(const std::string& path);
std::string CardExport(const std::string& dest);
std::string CardImportDolphin();
std::string CardExportDolphin();
// Cheat: the player takes no damage (F1 > Debug > cheats, MP_GODMODE, console `god`).
bool Invulnerable();
void SetInvulnerable(bool enabled);
// Write the log to <user folder>/metroid_prime_port.log (port_log_file.h). MP_LOG_FILE=1
// turns it on for one run without changing the setting.
bool LogFile();
void SetLogFile(bool enabled);
// Fast Morph, as in Metroid Prime 4: short morph/unmorph transitions that keep
// momentum (capped at walking speed when unmorphing on the ground).
bool FastMorph();
void SetFastMorph(bool enabled);
// Toggle Lock-On: L latches until pressed again (lock-on, scan, strafe,
// grapple), and a lock that ends lets go by itself. Sticky Charge: letting go
// of a long A hold keeps the beam charging until the next press fires it. Both
// apply only unmorphed, in gameplay (port_hold_toggle.h).
bool LockOnToggle();
void SetLockOnToggle(bool enabled);
bool StickyCharge();
void SetStickyCharge(bool enabled);
// Spring Ball on a gyro flick (pad or phone tilted up sharply, like Trilogy's
// nunchuk flick), on top of C-stick up. Rate is the pitch speed in rad/s a flick
// must pass. The gyro source is the aim's.
bool SpringBallFlick();
void SetSpringBallFlick(bool enabled);
float SpringBallFlickRate();
void SetSpringBallFlickRate(float radiansPerSecond);
// True for a short while after a flick, so one just before landing still counts.
bool SpringBallFlickPending();
void ClearSpringBallFlick();
// Debug console: stand-in gyro rates (rad/s) instead of the sensors.
void SetGyroOverride(bool active, float pitch, float yaw);
// Aim travel in pixels per second at full stick deflection (scaled by the mouse
// sensitivity, so both share the same feel).
float StickAimRate();
void SetStickAimRate(float pixelsPerSecond);
// Gyro aiming. Mode: 0 off, 1 aim while the hold input is down, 2 always aim.
// Source: 0 auto (a pad's gyro if it has one, else the phone's), 1 controller
// only, 2 the device's own gyro (Android phones). Rate is aim pixels per second
// per radian per second of rotation, so it reads like the stick aim speed.
int GyroMode();
void SetGyroMode(int mode);
int GyroSource();
void SetGyroSource(int source);
float GyroRate();
void SetGyroRate(float pixelsPerSecondPerRad);
// Reads the gyro, feeds the aim and spots Spring Ball flicks. Call once per
// tick, before the frame.
void PollGyro();
// Short description of what the gyro is doing, for the overlay.
const char* GyroStatus();
// Feeds a normalised right-stick vector (-1..1, x right, y up) for this tick.
void AddStickAim(float x, float y, float dt);void SetMouseCaptured(bool captured);
bool MouseCaptured();
bool MouseGameplayActive();
void SetMouseGameplayActive(bool active);
bool MouseInvertX();
bool MouseInvertY();
bool MouseButtons();
bool MouseCrosshair();
// Crosshair size in percent while mouse aim or twin stick is on (the retail
// free-aim crosshair is sized for a held R, and it stays up the whole time
// under those modes). Retail R free-aim keeps 100.
const int kCrosshairSizeMin = 25;
const int kCrosshairSizeMax = 100;
const int kCrosshairSizeDefault = 50;
int CrosshairSize();
void SetCrosshairSize(int percent);
unsigned MouseWeaponButtons(unsigned held);
// Outside mouse gameplay (morph ball, text boxes, menus) the buttons bound to A
// or B still press them; returns the held buttons (SDL masks) that may count.
unsigned MouseMenuButtons(unsigned held, bool focused);
// Real-mouse button state (SDL_BUTTON_*MASK), fed from the event loop. Touch-
// and pen-synthesised mouse buttons are left out; see PortMouse::HeldButtons.
void NoteMouseButton(bool synthetic, unsigned mask, bool down);
void ClearMouseButtons();
unsigned MouseHeldButtons();
// Called during simulation, using the effective unbobbed camera direction.
bool UpdateMouseAim(bool active, bool locked, float x, float y, float z);
void SynchronizeMouseAim(float x, float y, float z);
void ResetMouseAim();
float MouseSensitivity();
void SetMouseSensitivity(float radiansPerPixel);
// Called from the input event loop as relative motion arrives.
void AddMouseDelta(float dx, float dy);
// Called once per simulated frame to latch the deltas for that frame.
void BeginFrameMouse();
void GetFrameMouseDelta(float& dx, float& dy);
// The yaw/pitch change (radians) the next tick's look input will apply, as seen
// a fraction of a tick after the last one. False when there is none to show.
bool PresentedAimDelta(float fraction, float& dyaw, float& dpitch);
// Mouse-look camera angles (radians): world yaw and pitch. The game owns the
// update and applies them to the first-person camera.
float AimYaw();
float AimPitch();
bool AimInitialized();

// Audio paths
bool AiAudioEnabled();
void SetAiAudioEnabled(bool enabled);
bool MusyxAudioEnabled();
void SetMusyxAudioEnabled(bool enabled);

// Session
void RequestReset();
bool ConsumeResetRequest();

bool Visible();
// Writes the settings file now instead of waiting for the next overlay frame,
// so a disc chosen during startup is remembered even if no frame is drawn yet.
void SaveSettingsNow();
// Marks the settings file dirty from outside the overlay (the VR settings
// store), so it is written on exit or by SaveSettingsNow like any other change.
void MarkVrSettingsDirty();
// Thread-safe snapshot of the overlay's visibility, for the Android touch
// controls. Unlike Visible() it performs no lazy initialization, so it is safe
// to call from the UI thread.
bool OverlayVisible();
// Thread-safe snapshot of the twin-stick setting, for the Android touch overlay
// to choose a controller layout. Like OverlayVisible(), performs no lazy
// initialization, so it is safe to call from the UI thread.
bool TwinStickFlag();
void Toggle();
// Asks for the overlay to be toggled on the next frame. Safe to call from any
// thread, unlike Toggle(), which touches ImGui state.
void RequestToggle();
// Feeds the pad into ImGui's gamepad navigation, applies any requested toggle,
// and handles F1. Call once per frame before the frame is built.
void UpdateControllerNav();

// Builds the debug windows for the current ImGui frame. Call once per presented
// frame, after Aurora begins the frame and before it ends it.
void DrawUI();

} // namespace PortDebug

#endif // METROID_PRIME_PORT_PORT_DEBUG_H

// In-game debug overlay. The port draws it with Aurora's ImGui backend, which
// is already initialized and rendered every presented frame, so this only has
// to build the windows between aurora_begin_frame and aurora_end_frame.

#include "port_debug.h"
#include "port_freecam.h"
#include "port_hd_font.h"
#include "port_room_env.h"
#include "port_room_geo.h"
#include "port_log.h"
#include "port_log_file.h"
#include "port_paths.h"
#include "port_apclient.h"
#include "port_controls.h"
#include "port_data_folder.h"
#include "port_gci.h"
#include "port_mods.h"
#include "port_importers.h"
#include "port_remastered_import.h"
#include "port_discord.h"
#include "port_livesplit.h"
#include "port_map_pickups.h"
#include "port_prompts.h"
#include "port_tracker.h"
#include "port_savestate.h"
#include "port_skip_cutscenes.h"
#include "port_mouse.h"
#include "port_input_map.h"
#include "port_textures.h"
#include "port_build_info.h"
#include "vr/vr_debug_tab.h"
#include "vr/vr_settings.h"
#if defined(__ANDROID__)
#include "touch_pad.h"
#endif

#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "Kyoto/Graphics/CCubeMaterial.hpp"
#include "MetroidPrime/CHealthInfo.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CMapWorld.hpp"
#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/CScriptLayerManager.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "Kyoto/CResFactory.hpp"

#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <dolphin/gx/GXExtra.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <musyx/port_voices.h>

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_sensor.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>

#if defined(__ANDROID__)
#include <jni.h>
#include <android/log.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_system.h>
#include <cctype>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace aurora {
void request_screenshot() noexcept;
}

// Implemented by the AI and MusyX audio backends.
extern "C" void AIPortSetOutputEnabled(int enabled);
extern "C" int AIPortOutputEnabled(void);
extern "C" void salSetMuted(int muted);

namespace {
bool sInitialized = false;
bool sFastBoot = false;
bool sSkipCutscenes = false;
float sCutsceneSpeed = 8.f;
unsigned sSimRate = 60;
bool sSimAdaptive = false;
float sTickPeriod = 1.f / 60.f;
bool sFrameLimitEnabled = true;
bool sTurbo = false;
unsigned sTurboTicks = 1;
bool sTraceTiming = false;
uint64_t sTimingNs = 0;
// Wall-clock for the same span as sTimingNs, so presented frames can be divided
// by elapsed time rather than by the frames' own cost. Zero on the first call,
// which is why the delta is skipped until a previous reading exists.
uint64_t sTimingWallNs = 0;
uint64_t sTimingWallLastNs = 0;
unsigned sTimingFrames = 0, sTimingTicks = 0;
double sActualFps = 0.0, sActualTps = 0.0;
double sThroughputFps = 0.0;
bool sVsyncEnabled = false;
// Android hides the status and navigation bars by default; a desktop starts windowed.
#if defined(__ANDROID__)
bool sFullscreen = true;
#else
bool sFullscreen = false;
#endif
bool sOverlayWindowed = false; // desktop: the old floating tabbed window
float sRenderScale = 1.f;
PortDebug::EAspectMode sAspectMode = PortDebug::kAspect_4_3;
bool sHudWide = false;
int sHudScale = PortDebug::kHudScaleMax;
bool sHideHelmet = false;
bool sHideVisorEffects = false;
bool sRevealMap = false;
bool sMapPickups = false;
bool sMapLogicColors = true;
int sApSuitDamage = 1;
bool sCheats = false;
bool sSkippableCutscenes = false;
bool sSaveStateHotkeys = true;
bool sMouseAim = false;
bool sTwinStick = false;
float sTwinStickRightY = 0.f;
bool sBeamShiftHeld = false;
bool sSpringBall = false;
bool sSwapScanXray = false;
bool sFastMorph = false;
bool sInvulnerable = false;
// MP_GODMODE, for this run only: -1 unset, else 0 or 1. Never saved, and changing the
// setting ends it.
int sInvulnerableRun = -1;
bool sLogFile = false;
bool sLockOnToggle = false;
bool sStickyCharge = false;
bool sSpringFlick = false;
float sSpringFlickRate = 6.f;
float sStickAimRate = 900.f;
float sFirstPersonFov = PortDebug::kFovRetail;
int sMsaa = 1;
int sAnisotropy = 16;
bool sUnlockHardMode = false;
bool sUnlockFusionSuit = false;
bool sUnlockGalleries = false;
bool sSpeedrunTimer = false;
bool sLiveSplit = false;
std::string sLiveSplitAddress = "127.0.0.1:16834";
bool sLiveSplitSplitUpgrades = true;
bool sDiscord = false;
std::string sDiscordAppId;
// Mods folder (port_mods.h): read at startup only.
bool sModsEnabled = true;
std::string sModsDisabled;
// Gyro aiming: off / hold / always, auto / controller / phone, and how fast a
// rotation turns into aim travel.
int sGyroMode = 0;
int sGyroSource = 0;
float sGyroRate = 600.f;
bool sMouseCaptured = false;
bool sMouseGameplayActive = false;
bool sMouseInvertX = false;
bool sMouseInvertY = false;
bool sMouseButtons = true;
// What each mouse button does under mouse aim (PortInputMap::EMouseAction).
int sMouseActions[PortInputMap::kMouseButtonCount] = {
    PortInputMap::DefaultMouseAction(0), PortInputMap::DefaultMouseAction(1),
    PortInputMap::DefaultMouseAction(2), PortInputMap::DefaultMouseAction(3),
    PortInputMap::DefaultMouseAction(4)};
// The beam shift: two keys or mouse buttons (scancode or PAD_KEY_MOUSE_*) and
// a controller button (an SDL gamepad button or PAD_NATIVE_BUTTON_TRIGGER_*),
// -1 for none.
int sShiftBindings[3] = {SDL_SCANCODE_LSHIFT, -1, -1};
// A second controller button per PAD button, indexed by the PAD bit's position;
// the same codes as the shift's pad slot, -1 for none.
int sPadAltButtons[PortDebug::kPadAltCount] = {-1, -1, -1, -1, -1, -1, -1, -1,
                                               -1, -1, -1, -1, -1, -1, -1, -1};
bool sMouseCrosshair = true;
int sCrosshairSize = PortDebug::kCrosshairSizeDefault;
PortMouse::AimState sMouseAimState;
PortMouse::ButtonGate sMouseButtonGate;
PortMouse::ButtonGate sMouseMenuGate;
PortMouse::HeldButtons sMouseHeldButtons;
float sMouseSensitivity = 0.0035f;
float sMousePendingX = 0.f;
float sMousePendingY = 0.f;
float sMouseFrameX = 0.f;
float sMouseFrameY = 0.f;
// Gyro travel since the last tick. Kept apart from the mouse's pending delta,
// which is dropped whenever the mouse is not captured (twin stick, phones).
float sGyroPendingX = 0.f;
float sGyroPendingY = 0.f;
// The twin stick's aim speed at the last tick, in pixels per second, so frames
// between ticks can show the travel the next tick will add.
float sStickAimVelX = 0.f;
float sStickAimVelY = 0.f;
// Frame interpolation (docs/FRAME_INTERPOLATION.md): uncapped frames show look
// input before the tick that applies it.
bool sFrameInterpolation = true;
bool sActorInterpolation = false;
bool sPoseInterpolation = false;
bool sParticleInterpolation = false;
float sPresentOverride = -1.f;
unsigned sPresentCycleFrame = 0;
bool sTickHold = false;
unsigned sHeldTicks = 0;
// The last tick applied look input. A paused game or a cinematic skips the
// player update, and would then drop what the frames between ticks showed.
bool sAimAppliedLastTick = false;
bool sAiAudioEnabled = true;
bool sMusyxAudioEnabled = true;
bool sResetRequested = false;
std::atomic< bool > sToggleRequested{false};
// F5 = 1 (save), F9 = 2 (load), from the event watch; handled on the game thread.
std::atomic< int > sSaveStateHotkey{0};
// F11 asks for a fullscreen toggle; DrawUI applies it on the main thread.
std::atomic< bool > sFullscreenHotkey{false};
// The window's own fullscreen state as SDL last reported it (-1 = no report
// yet), so leaving fullscreen through the window manager updates the setting.
std::atomic< int > sWindowFullscreen{-1};
// Mirrors sVisible for readers on other threads, so they never touch the lazy
// initialization or the ImGui state owned by the game thread.
std::atomic< bool > sOverlayVisible{false};
// Same idea for the twin-stick setting, which the Android touch overlay uses to
// pick a controller layout.
std::atomic< bool > sTwinStickFlag{false};
// Set when a real pad, keyboard or mouse is used; the Android touch overlay takes
// it to get out of the way.
std::atomic< bool > sPhysicalInput{false};
// Gyro state: the phone's sensor is looked up once, so the sensor list is not
// walked on every tick.
bool sPhoneGyroSearched = false;
SDL_Sensor* sPhoneGyro = nullptr;
const char* sGyroStatus = "off";
// Flick state: seconds left on the last flick, and whether the pitch has dropped
// back since, so a long flick counts once.
float sSpringFlickLatch = 0.f;
bool sSpringFlickArmed = true;
bool sGyroOverride = false;
float sGyroOverridePitch = 0.f;
float sGyroOverrideYaw = 0.f;
bool sVisible = false;
bool sSettingsDirty = false;
bool sAudioSettingsApplied = false;
bool sPresentationSettingsApplied = false;
CStateManager* sStateManager = nullptr;
int sPendingTeleport = -1;
bool sHasWorldTeleport = false;
std::string sDiscPath;
// The Remastered import's image and key file as last used: paths, or on Android
// the picked content:// addresses (the picker keeps their read grant).
std::string sRemasteredImagePath;
std::string sRemasteredKeysPath;
uint32_t sWorldTeleportWorld = 0;
uint32_t sWorldTeleportArea = 0;
bool sWorldSweepRequested = false;
struct WorldSweep {
  std::vector< uint32_t > worlds;
  std::vector< uint32_t > areas;
  size_t world = 0;
  size_t area = 0;
  unsigned settledTicks = 0;
  unsigned stalledTicks = 0;
  unsigned completedAreas = 0;
  bool active = false;
  bool waiting = false;
  // An area can hold several layers, and only the active ones are built, so a
  // pickup behind a layer the save has not unlocked is not in the dump at all.
  // The tour revisits each area once per layer with a different one active,
  // which is what makes the dump cover the whole area rather than the state the
  // save happens to be in.
  int layer = 0;
  int layerCount = 1;
  unsigned layerPasses = 0;
  // Set between the hop away from an area and the hop back to it: the area has
  // to be gone before it is rebuilt with the next layer.
  bool revisiting = false;
} sWorldSweep;

std::string SettingsFilePath() {
  const std::string& dir = PortPaths::UserFolder();
  return (dir.empty() ? std::string("./") : dir) + "port_settings.ini";
}

bool ParseBool(const std::string& value) {
  return value == "1" || value == "true" || value == "on" || value == "yes";
}

// The mouse button a settings key such as "mouse_left" names, or -1.
int MouseButtonSetting(const std::string& key) {
  for (int i = 0; i < PortInputMap::kMouseButtonCount; ++i) {
    if (key == PortInputMap::MouseButtonKey(i)) {
      return i;
    }
  }
  return -1;
}

std::string Trim(const std::string& text) {
  const size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return std::string();
  }
  const size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

void MarkDirty() { sSettingsDirty = true; }

void ApplyLiveSplit() {
  PortLiveSplit::Configure(sLiveSplit, sLiveSplitAddress, sLiveSplitSplitUpgrades);
}

// Discord application ids are decimal snowflakes; a pasted id may carry spaces.
std::string DigitsOnly(const std::string& text) {
  std::string out;
  for (const char c : text) {
    if (c >= '0' && c <= '9') {
      out += c;
    }
  }
  return out;
}

void ApplyDiscord() { PortDiscord::Configure(sDiscord, sDiscordAppId); }

void ApplySetting(const std::string& key, const std::string& value) {
  // VR (PrimedGun) settings live in their own table (vr/vr_settings.cpp).
  if (PortVr::ApplyVrSetting(key, value)) {
    return;
  }
  if (key == "frame_limit") {
    sFrameLimitEnabled = ParseBool(value);
  } else if (key == "vsync") {
    sVsyncEnabled = ParseBool(value);
  } else if (key == "fullscreen") {
    sFullscreen = ParseBool(value);
  } else if (key == "overlay_windowed") {
    sOverlayWindowed = ParseBool(value);
  } else if (key == "disc_path") {
    sDiscPath = value;
  } else if (key == "remastered_nsp") {
    sRemasteredImagePath = value;
  } else if (key == "remastered_keys") {
    sRemasteredKeysPath = value;
  } else if (key == "render_scale") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f >= 0.f && f <= 4.f) {
      sRenderScale = f;
    }
  } else if (key == "aspect") {
    if (value == "16:9") {
      sAspectMode = PortDebug::kAspect_16_9;
    } else if (value == "window") {
      sAspectMode = PortDebug::kAspect_Window;
    } else if (value == "4:3") {
      sAspectMode = PortDebug::kAspect_4_3;
    }
  } else if (key == "hud_wide") {
    sHudWide = ParseBool(value);
  } else if (key == "hud_scale") {
    const int s = std::atoi(value.c_str());
    if (s >= PortDebug::kHudScaleMin && s <= PortDebug::kHudScaleMax) {
      sHudScale = s;
    }
  } else if (key == "hide_helmet") {
    sHideHelmet = ParseBool(value);
  } else if (key == "hide_visor_effects") {
    sHideVisorEffects = ParseBool(value);
  } else if (key == "reveal_map") {
    sRevealMap = ParseBool(value);
  } else if (key == "map_pickups") {
    sMapPickups = ParseBool(value);
  } else if (key == "map_logic_colors") {
    sMapLogicColors = ParseBool(value);
  } else if (key == "ap_suit_damage") {
    const int mode = std::atoi(value.c_str());
    sApSuitDamage = mode >= 0 && mode <= 2 ? mode : 1;
  } else if (key == "cheats") {
    sCheats = ParseBool(value);
  } else if (key == "skippable_cutscenes") {
    sSkippableCutscenes = ParseBool(value);
  } else if (key == "savestate_hotkeys") {
    sSaveStateHotkeys = ParseBool(value);
  } else if (key == "unlock_hard_mode") {
    sUnlockHardMode = ParseBool(value);
  } else if (key == "unlock_fusion_suit") {
    sUnlockFusionSuit = ParseBool(value);
  } else if (key == "unlock_galleries") {
    sUnlockGalleries = ParseBool(value);
  } else if (key == "msaa") {
    sMsaa = std::atoi(value.c_str()) >= 4 ? 4 : 1;
  } else if (key == "anisotropy") {
    const int a = std::atoi(value.c_str());
    if (a >= 1 && a <= 16) {
      sAnisotropy = a;
    }
  } else if (key == "fov") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f >= PortDebug::kFovMin && f <= PortDebug::kFovMax) {
      sFirstPersonFov = f;
    }
  } else if (key == "mouse_aim") {
    sMouseAim = ParseBool(value);
  } else if (key == "twin_stick") {
    sTwinStick = ParseBool(value);
  } else if (key == "stick_aim_rate") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f >= 50.f && f <= 4000.f) {
      sStickAimRate = f;
    }
  } else if (key == "gyro_mode") {
    const long v = std::strtol(value.c_str(), nullptr, 10);
    if (v >= 0 && v <= 2) {
      sGyroMode = static_cast< int >(v);
    }
  } else if (key == "gyro_source") {
    const long v = std::strtol(value.c_str(), nullptr, 10);
    if (v >= 0 && v <= 2) {
      sGyroSource = static_cast< int >(v);
    }
  } else if (key == "gyro_rate") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f >= 20.f && f <= 5000.f) {
      sGyroRate = f;
    }
  } else if (key == "mouse_invert_x") {
    sMouseInvertX = ParseBool(value);
  } else if (key == "mouse_invert_y") {
    sMouseInvertY = ParseBool(value);
  } else if (key == "mouse_buttons") {
    sMouseButtons = ParseBool(value);
  } else if (key == "mouse_crosshair") {
    sMouseCrosshair = ParseBool(value);
  } else if (key == "crosshair_size") {
    const int s = std::atoi(value.c_str());
    if (s >= PortDebug::kCrosshairSizeMin && s <= PortDebug::kCrosshairSizeMax) {
      sCrosshairSize = s;
    }
  } else if (key == "mouse_sensitivity") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f > 0.f && f <= 1.f) {
      sMouseSensitivity = f;
    }
  } else if (key == "spring_ball") {
    sSpringBall = ParseBool(value);
  } else if (key == "swap_scan_xray") {
    sSwapScanXray = ParseBool(value);
  } else if (key == "shift_key" || key == "shift_key_alt" || key == "shift_pad") {
    const int slot = key == "shift_key" ? 0 : key == "shift_key_alt" ? 1 : 2;
    // A number, or the value is ignored (atoi would read junk as scancode 0).
    char* end = nullptr;
    const long code = std::strtol(value.c_str(), &end, 10);
    if (end != value.c_str() && *end == '\0') {
      sShiftBindings[slot] = static_cast< int >(code);
    }
  } else if (key == "pad_alt") {
    // kPadAltCount comma-separated codes; a short or malformed list keeps the
    // rest as they are.
    const char* cursor = value.c_str();
    for (int i = 0; i < PortDebug::kPadAltCount && *cursor != '\0'; ++i) {
      char* end = nullptr;
      const long code = std::strtol(cursor, &end, 10);
      if (end == cursor) {
        break;
      }
      sPadAltButtons[i] = static_cast< int >(code);
      cursor = *end == ',' ? end + 1 : end;
    }
  } else if (MouseButtonSetting(key) >= 0) {
    const int action = PortInputMap::MouseActionFromName(value.c_str());
    if (action >= 0) {
      sMouseActions[MouseButtonSetting(key)] = action;
    }
  } else if (key == "speedrun_timer") {
    sSpeedrunTimer = ParseBool(value);
  } else if (key == "livesplit") {
    sLiveSplit = ParseBool(value);
  } else if (key == "livesplit_address") {
    if (!value.empty()) {
      sLiveSplitAddress = value;
    }
  } else if (key == "livesplit_split_upgrades") {
    sLiveSplitSplitUpgrades = ParseBool(value);
  } else if (key == "discord") {
    sDiscord = ParseBool(value);
  } else if (key == "discord_app_id") {
    sDiscordAppId = DigitsOnly(value);
  } else if (key == "mods") {
    sModsEnabled = ParseBool(value);
  } else if (key == "mods_disabled") {
    sModsDisabled = value;
  } else if (key == "fast_morph") {
    sFastMorph = ParseBool(value);
  } else if (key == "invulnerable") {
    sInvulnerable = ParseBool(value);
  } else if (key == "log_file") {
    sLogFile = ParseBool(value);
  } else if (key == "lock_on_toggle") {
    sLockOnToggle = ParseBool(value);
  } else if (key == "sticky_charge") {
    sStickyCharge = ParseBool(value);
  } else if (key == "spring_ball_flick") {
    sSpringFlick = ParseBool(value);
  } else if (key == "spring_ball_flick_rate") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f >= 2.f && f <= 20.f) {
      sSpringFlickRate = f;
    }
  } else if (key == "sim_rate") {
    const long rate = std::strtol(value.c_str(), nullptr, 10);
    if (rate >= 30 && rate <= 480) {
      sSimRate = static_cast< unsigned >(rate);
    }
  } else if (key == "sim_adaptive") {
    sSimAdaptive = ParseBool(value);
  } else if (key == "frame_interpolation") {
    sFrameInterpolation = ParseBool(value);
  } else if (key == "actor_interpolation") {
    sActorInterpolation = ParseBool(value);
  } else if (key == "pose_interpolation") {
    sPoseInterpolation = ParseBool(value);
  } else if (key == "particle_interpolation") {
    sParticleInterpolation = ParseBool(value);
  } else if (key == "ai_audio") {
    sAiAudioEnabled = ParseBool(value);
  } else if (key == "musyx_audio") {
    sMusyxAudioEnabled = ParseBool(value);
  } else if (key == "voices_muted") {
    MusyxPortClearSampleMutes();
    const char* cursor = value.c_str();
    while (*cursor != '\0') {
      char* end = nullptr;
      const unsigned long id = std::strtoul(cursor, &end, 10);
      if (end == cursor) {
        break;
      }
      MusyxPortSetSampleMuted(static_cast< unsigned >(id), 1);
      cursor = end;
      while (*cursor == ',' || *cursor == ' ') {
        ++cursor;
      }
    }
  }
}

void LoadSettings() {
  const std::string path = SettingsFilePath();
  std::ifstream file(path);
  if (!file.is_open()) {
    return;
  }
  std::fprintf(stderr, "metroid_prime_port: loaded settings from %s\n", path.c_str());
  std::string line;
  while (std::getline(file, line)) {
    const size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line.erase(comment);
    }
    const size_t separator = line.find('=');
    if (separator == std::string::npos) {
      continue;
    }
    const std::string key = Trim(line.substr(0, separator));
    const std::string value = Trim(line.substr(separator + 1));
    if (!key.empty()) {
      ApplySetting(key, value);
    }
  }
}

void SaveSettings() {
  if (!sInitialized || !sSettingsDirty) {
    return;
  }
  const std::string path = SettingsFilePath();
  std::ofstream file(path, std::ios::trunc);
  if (!file.is_open()) {
    std::fprintf(stderr, "metroid_prime_port: could not write settings to %s\n", path.c_str());
    return;
  }
  const char* aspect = sAspectMode == PortDebug::kAspect_16_9  ? "16:9"
                       : sAspectMode == PortDebug::kAspect_Window ? "window"
                                                                  : "4:3";
  file << "# Metroid Prime native port settings. Written by the F1 debug overlay.\n";
  file << "# Environment variables (MP_*) override these for a single run.\n";
  file << "aspect=" << aspect << '\n';
  file << "hud_wide=" << (sHudWide ? 1 : 0) << '\n';
  file << "hud_scale=" << sHudScale << '\n';
  file << "hide_helmet=" << (sHideHelmet ? 1 : 0) << '\n';
  file << "hide_visor_effects=" << (sHideVisorEffects ? 1 : 0) << '\n';
  file << "reveal_map=" << (sRevealMap ? 1 : 0) << '\n';
  file << "map_pickups=" << (sMapPickups ? 1 : 0) << '\n';
  file << "map_logic_colors=" << (sMapLogicColors ? 1 : 0) << '\n';
  file << "ap_suit_damage=" << sApSuitDamage << '\n';
  file << "cheats=" << (sCheats ? 1 : 0) << '\n';
  file << "skippable_cutscenes=" << (sSkippableCutscenes ? 1 : 0) << '\n';
  file << "savestate_hotkeys=" << (sSaveStateHotkeys ? 1 : 0) << '\n';
  file << "fov=" << sFirstPersonFov << '\n';
  file << "msaa=" << sMsaa << '\n';
  file << "anisotropy=" << sAnisotropy << '\n';
  file << "unlock_hard_mode=" << (sUnlockHardMode ? 1 : 0) << '\n';
  file << "unlock_fusion_suit=" << (sUnlockFusionSuit ? 1 : 0) << '\n';
  file << "unlock_galleries=" << (sUnlockGalleries ? 1 : 0) << '\n';
  file << "speedrun_timer=" << (sSpeedrunTimer ? 1 : 0) << '\n';
  file << "livesplit=" << (sLiveSplit ? 1 : 0) << '\n';
  file << "livesplit_address=" << sLiveSplitAddress << '\n';
  file << "livesplit_split_upgrades=" << (sLiveSplitSplitUpgrades ? 1 : 0) << '\n';
  file << "discord=" << (sDiscord ? 1 : 0) << '\n';
  file << "discord_app_id=" << sDiscordAppId << '\n';
  file << "mods=" << (sModsEnabled ? 1 : 0) << '\n';
  file << "mods_disabled=" << sModsDisabled << '\n';
  file << "vsync=" << (sVsyncEnabled ? 1 : 0) << '\n';
  file << "fullscreen=" << (sFullscreen ? 1 : 0) << '\n';
  file << "overlay_windowed=" << (sOverlayWindowed ? 1 : 0) << '\n';
  file << "render_scale=" << sRenderScale << '\n';
  file << "frame_limit=" << (sFrameLimitEnabled ? 1 : 0) << '\n';
  file << "sim_rate=" << sSimRate << '\n';
  file << "sim_adaptive=" << (sSimAdaptive ? 1 : 0) << '\n';
  file << "frame_interpolation=" << (sFrameInterpolation ? 1 : 0) << '\n';
  file << "actor_interpolation=" << (sActorInterpolation ? 1 : 0) << '\n';
  file << "pose_interpolation=" << (sPoseInterpolation ? 1 : 0) << '\n';
  file << "particle_interpolation=" << (sParticleInterpolation ? 1 : 0) << '\n';
  file << "mouse_aim=" << (sMouseAim ? 1 : 0) << '\n';
  file << "twin_stick=" << (sTwinStick ? 1 : 0) << '\n';
  file << "spring_ball=" << (sSpringBall ? 1 : 0) << '\n';
  file << "swap_scan_xray=" << (sSwapScanXray ? 1 : 0) << '\n';
  file << "shift_key=" << sShiftBindings[0] << '\n';
  file << "shift_key_alt=" << sShiftBindings[1] << '\n';
  file << "shift_pad=" << sShiftBindings[2] << '\n';
  file << "pad_alt=";
  for (int i = 0; i < PortDebug::kPadAltCount; ++i) {
    file << (i != 0 ? "," : "") << sPadAltButtons[i];
  }
  file << '\n';
  file << "fast_morph=" << (sFastMorph ? 1 : 0) << '\n';
  file << "invulnerable=" << (sInvulnerable ? 1 : 0) << '\n';
  file << "log_file=" << (sLogFile ? 1 : 0) << '\n';
  file << "lock_on_toggle=" << (sLockOnToggle ? 1 : 0) << '\n';
  file << "sticky_charge=" << (sStickyCharge ? 1 : 0) << '\n';
  file << "spring_ball_flick=" << (sSpringFlick ? 1 : 0) << '\n';
  file << "spring_ball_flick_rate=" << sSpringFlickRate << '\n';
  file << "stick_aim_rate=" << sStickAimRate << '\n';
  file << "gyro_mode=" << sGyroMode << '\n';
  file << "gyro_source=" << sGyroSource << '\n';
  file << "gyro_rate=" << sGyroRate << '\n';
  file << "mouse_invert_x=" << (sMouseInvertX ? 1 : 0) << '\n';
  file << "mouse_invert_y=" << (sMouseInvertY ? 1 : 0) << '\n';
  file << "mouse_buttons=" << (sMouseButtons ? 1 : 0) << '\n';
  for (int i = 0; i < PortInputMap::kMouseButtonCount; ++i) {
    file << PortInputMap::MouseButtonKey(i) << '=' << PortInputMap::MouseActionInfo(sMouseActions[i]).name
         << '\n';
  }
  file << "mouse_crosshair=" << (sMouseCrosshair ? 1 : 0) << '\n';
  file << "crosshair_size=" << sCrosshairSize << '\n';
  file << "mouse_sensitivity=" << sMouseSensitivity << '\n';
  if (!sDiscPath.empty()) {
    file << "disc_path=" << sDiscPath << '\n';
  }
  if (!sRemasteredImagePath.empty()) {
    file << "remastered_nsp=" << sRemasteredImagePath << '\n';
  }
  if (!sRemasteredKeysPath.empty()) {
    file << "remastered_keys=" << sRemasteredKeysPath << '\n';
  }
  file << "ai_audio=" << (sAiAudioEnabled ? 1 : 0) << '\n';
  file << "musyx_audio=" << (sMusyxAudioEnabled ? 1 : 0) << '\n';
  unsigned muted[64];
  const int mutedCount = MusyxPortGetMutedSamples(muted, 64);
  if (mutedCount > 0) {
    file << "voices_muted=";
    for (int i = 0; i < mutedCount; ++i) {
      file << (i == 0 ? "" : ",") << muted[i];
    }
    file << '\n';
  }
  PortVr::WriteVrSettings(file);
  file.flush();
  std::fprintf(stderr, "metroid_prime_port: saved settings to %s\n", path.c_str());
  sSettingsDirty = false;
}

// Whether the player just used a real pad, keyboard or mouse. The touch overlay
// is itself a virtual pad, and touches also arrive as a mouse, so neither counts.
// A stick or trigger has to move well past rest, so drift does not count either.
bool IsPhysicalInput(const SDL_Event& event) {
  switch (event.type) {
  case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    return !SDL_IsJoystickVirtual(event.gbutton.which);
  case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    return std::abs(static_cast< int >(event.gaxis.value)) > 16000 &&
           !SDL_IsJoystickVirtual(event.gaxis.which);
  case SDL_EVENT_KEY_DOWN:
    // The soft keyboard types into the overlay's text fields, and Back and the
    // media keys come from the phone itself.
    if (event.key.repeat || sOverlayVisible.load(std::memory_order_acquire)) {
      return false;
    }
    switch (event.key.scancode) {
    case SDL_SCANCODE_AC_BACK:
    case SDL_SCANCODE_VOLUMEUP:
    case SDL_SCANCODE_VOLUMEDOWN:
    case SDL_SCANCODE_MUTE:
    case SDL_SCANCODE_MEDIA_PLAY:
    case SDL_SCANCODE_MEDIA_PAUSE:
    case SDL_SCANCODE_MEDIA_PLAY_PAUSE:
    case SDL_SCANCODE_MEDIA_NEXT_TRACK:
    case SDL_SCANCODE_MEDIA_PREVIOUS_TRACK:
    case SDL_SCANCODE_MEDIA_STOP:
    case SDL_SCANCODE_POWER:
      return false;
    default:
      return true;
    }
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
    return event.button.which != SDL_TOUCH_MOUSEID && event.button.which != SDL_PEN_MOUSEID;
  default:
    return false;
  }
}

bool SDLCALL debug_event_watch(void*, SDL_Event* event) {
  // F1 toggles the overlay. Watch the event rather than polling the key state:
  // a short tap can begin and end between two frames, so polling misses it.
  if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat &&
      event->key.scancode == SDL_SCANCODE_F1) {
    PortDebug::RequestToggle();
  }
  if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat &&
      (event->key.scancode == SDL_SCANCODE_F5 || event->key.scancode == SDL_SCANCODE_F9)) {
    sSaveStateHotkey.store(event->key.scancode == SDL_SCANCODE_F5 ? 1 : 2,
                           std::memory_order_release);
  }
  if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat &&
      event->key.scancode == SDL_SCANCODE_F11) {
    sFullscreenHotkey.store(true, std::memory_order_release);
  }
  if (event->type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
      event->type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) {
    sWindowFullscreen.store(event->type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ? 1 : 0,
                            std::memory_order_release);
  }
  if (IsPhysicalInput(*event)) {
    sPhysicalInput.store(true, std::memory_order_release);
  }
  return true;
}

void EnsureInitialized() {
  if (sInitialized) {
    return;
  }
  sInitialized = true;
  LoadSettings();

  // Environment variables are explicit per-run overrides and win over the file.
  if (std::getenv("MP_TRACE_TIMING") != nullptr) {
    sTraceTiming = true;
  }
  if (std::getenv("MP_FAST_BOOT") != nullptr) {
    sFastBoot = true;
  }
  if (const char* turbo = std::getenv("MP_TURBO")) {
    sTurbo = true;
    const long ticks = std::strtol(turbo, nullptr, 10);
    if (ticks >= 1 && ticks <= 16) {
      sTurboTicks = static_cast< unsigned >(ticks);
    }
  }
  if (const char* present = std::getenv("MP_PRESENT_T")) {
    if (std::strcmp(present, "cycle") == 0) {
      sPresentOverride = PortDebug::kPresentCycle;
    } else if (std::strcmp(present, "tick") == 0) {
      sPresentOverride = PortDebug::kPresentTick;
    } else {
      const float t = static_cast< float >(std::atof(present));
      sPresentOverride = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
    }
  }
  // Cutscene skipping is a test aid only: skipping on the first frame of each
  // cinematic left script state unbalanced (stuck visor filters, missing
  // music), so it is no longer a player setting.
  if (std::getenv("MP_SKIP_CUTSCENES") != nullptr) {
    sSkipCutscenes = true;
  }
  if (const char* god = std::getenv("MP_GODMODE")) {
    sInvulnerableRun = ParseBool(god) ? 1 : 0;
  }
  if (std::getenv("MP_SHOW_DEBUG_UI") != nullptr) {
    sVisible = true;
  }
  if (const char* aspect = std::getenv("MP_ASPECT")) {
    if (std::strcmp(aspect, "16:9") == 0) {
      sAspectMode = PortDebug::kAspect_16_9;
    } else if (std::strcmp(aspect, "window") == 0) {
      sAspectMode = PortDebug::kAspect_Window;
    } else if (std::strcmp(aspect, "4:3") == 0) {
      sAspectMode = PortDebug::kAspect_4_3;
    }
  } else if (std::getenv("MP_WIDESCREEN") != nullptr) {
    sAspectMode = PortDebug::kAspect_16_9;
  }
  if (std::getenv("MP_HUD_WIDE") != nullptr) {
    sHudWide = true;
  }
  if (std::getenv("MP_MOUSE_AIM") != nullptr) {
    sMouseAim = true;
  }
  if (std::getenv("MP_TWIN_STICK") != nullptr) {
    sTwinStick = true;
  }
  if (std::getenv("MP_MOUSE_INVERT_X") != nullptr) {
    sMouseInvertX = true;
  }
  if (std::getenv("MP_MOUSE_INVERT_Y") != nullptr) {
    sMouseInvertY = true;
  }
  if (std::getenv("MP_DISABLE_MOUSE_BUTTONS") != nullptr) {
    sMouseButtons = false;
  }
  if (std::getenv("MP_DISABLE_MOUSE_CROSSHAIR") != nullptr) {
    sMouseCrosshair = false;
  }
  if (const char* sens = std::getenv("MP_MOUSE_SENS")) {
    const float value = static_cast< float >(std::atof(sens));
    if (std::isfinite(value) && value > 0.f) {
      sMouseSensitivity = value;
    }
  }
  if (std::getenv("MP_DISABLE_AI_AUDIO") != nullptr) {
    sAiAudioEnabled = false;
  }
  if (const char* speed = std::getenv("MP_CUTSCENE_SPEED")) {
    const float value = static_cast< float >(std::atof(speed));
    if (std::isfinite(value) && value >= 1.f && value <= 32.f) {
      sCutsceneSpeed = value;
    }
  }
  if (const char* rate = std::getenv("MP_SIM_RATE")) {
    const long value = std::strtol(rate, nullptr, 10);
    if (value >= 30 && value <= 480) {
      sSimRate = static_cast< unsigned >(value);
    }
  }
  if (std::getenv("MP_SIM_ADAPTIVE") != nullptr) {
    sSimAdaptive = true;
  }

  std::atexit(SaveSettings);
  ApplyLiveSplit();
  ApplyDiscord();
}
} // namespace

namespace PortDebug {

bool FastBoot() {
  EnsureInitialized();
  return sFastBoot;
}

bool BootWorld(uint32_t& worldId, uint32_t& areaAssetId) {
  static uint32_t sWorld = 0, sArea = 0;
  static const bool sSet = [] {
    const char* value = std::getenv("MP_BOOT_WORLD");
    if (value == nullptr) {
      return false;
    }
    char* end = nullptr;
    sWorld = static_cast< uint32_t >(std::strtoul(value, &end, 16));
    if (end != nullptr && *end == ':') {
      sArea = static_cast< uint32_t >(std::strtoul(end + 1, nullptr, 16));
    }
    return sWorld != 0;
  }();
  worldId = sWorld;
  areaAssetId = sArea;
  return sSet;
}

bool SkipCutscenes() {
  EnsureInitialized();
  return sSkipCutscenes;
}

float CutsceneSpeed() {
  EnsureInitialized();
  return sCutsceneSpeed;
}

unsigned SimRate() {
  EnsureInitialized();
  return sSimRate;
}

void SetSimRate(unsigned hz) {
  EnsureInitialized();
  if (hz < 30u || hz > 480u) {
    return;
  }
  sSimRate = hz;
  MarkDirty();
}

float SimPeriod() { return 1.f / static_cast< float >(SimRate()); }

bool SimAdaptive() {
  EnsureInitialized();
  return sSimAdaptive;
}

bool Turbo() {
  EnsureInitialized();
  return sTurbo;
}

unsigned TurboTicks() {
  EnsureInitialized();
  return sTurbo ? sTurboTicks : 1;
}

void SetSimAdaptive(bool enabled) {
  EnsureInitialized();
  sSimAdaptive = enabled;
  MarkDirty();
}

float TickPeriod() {
  EnsureInitialized();
  return sTickPeriod;
}

void SetTickPeriod(float dt) {
  if (std::isfinite(dt) && dt > 0.f) {
    sTickPeriod = dt;
  }
}

float TickFrames() { return sTickPeriod * 60.f; }

bool FrameLimitEnabled() {
  EnsureInitialized();
  return sFrameLimitEnabled;
}

void RecordFrame(uint64_t durationNs, unsigned ticks, bool presented) {
  sTimingNs += durationNs;
  sTimingTicks += ticks;
  if (presented) ++sTimingFrames;
  // Wall-clock time for the same span, kept separately from the frame's own
  // duration. Dividing presented frames by the frame's CPU time reports
  // throughput, which is what the process can *produce*; dividing by wall time
  // reports what reaches the screen. The two agree while the 60 Hz cap is
  // waiting more than the frame costs, and part company exactly when a frame
  // overruns its budget - which is the case someone opens the Performance tab
  // to diagnose. One of the two numbers without the other is misleading there.
  {
    const uint64_t nowNs = SDL_GetTicksNS();
    if (sTimingWallLastNs != 0) {
      sTimingWallNs += nowNs - sTimingWallLastNs;
    }
    sTimingWallLastNs = nowNs;
  }
  if (sTimingNs >= 1000000000ull) {
    const double seconds = static_cast<double>(sTimingNs) / 1000000000.0;
    const double wallSeconds = static_cast<double>(sTimingWallNs) / 1000000000.0;
    sActualFps = sTimingFrames / (wallSeconds > 0.0 ? wallSeconds : seconds);
    sThroughputFps = sTimingFrames / seconds;
    sActualTps = sTimingTicks / seconds;
    if (sTraceTiming) {
      std::fprintf(stderr,
                   "[timing] presented=%.1f FPS throughput=%.1f FPS simulation=%.1f ticks/s cap=%s\n",
                   sActualFps, sThroughputFps, sActualTps, sFrameLimitEnabled ? "60" : "off");
    }
    sTimingNs = 0;
    sTimingWallNs = 0;
    sTimingFrames = sTimingTicks = 0;
  }
}

void SetFrameLimitEnabled(bool enabled) {
  EnsureInitialized();
  if (sFrameLimitEnabled != enabled) {
    sFrameLimitEnabled = enabled;
    MarkDirty();
  }
}

bool VsyncEnabled() {
  EnsureInitialized();
  return sVsyncEnabled;
}

void SetVsyncEnabled(bool enabled) {
  EnsureInitialized();
  // Always re-apply: the stored value can match while the surface still has the
  // previous present mode (e.g. the persisted value applied before the first
  // frame), which made the first toggle a no-op.
  sVsyncEnabled = enabled;
  aurora_enable_vsync(enabled);
}

bool Fullscreen() {
  EnsureInitialized();
  return sFullscreen;
}

void SetFullscreen(bool enabled) {
  EnsureInitialized();
  if (sFullscreen != enabled) {
    sFullscreen = enabled;
    MarkDirty();
  }
  PortLog::Write("metroid_prime_port: fullscreen %s\n", enabled ? "on" : "off");
  VISetWindowFullscreen(enabled);
}

float RenderScale() {
  EnsureInitialized();
  return sRenderScale;
}

void SetRenderScale(float scale) {
  EnsureInitialized();
  if (sRenderScale == scale) {
    return;
  }
  sRenderScale = scale;
  VISetFrameBufferScale(scale);
}

EAspectMode AspectMode() {
  EnsureInitialized();
  return sAspectMode;
}

void SetAspectMode(EAspectMode mode) {
  EnsureInitialized();
  sAspectMode = mode;
  MarkDirty();
}

bool HudWide() {
  EnsureInitialized();
  return sHudWide;
}

void SetHudWide(bool enabled) {
  EnsureInitialized();
  sHudWide = enabled;
  MarkDirty();
}

int HudScale() {
  EnsureInitialized();
  return sHudScale;
}

void SetHudScale(int percent) {
  EnsureInitialized();
  sHudScale = std::clamp(percent, kHudScaleMin, kHudScaleMax);
  MarkDirty();
}

bool HideHelmet() {
  EnsureInitialized();
  return sHideHelmet;
}

void SetHideHelmet(bool enabled) {
  EnsureInitialized();
  sHideHelmet = enabled;
  MarkDirty();
}

bool HideVisorEffects() {
  EnsureInitialized();
  return sHideVisorEffects;
}

void SetHideVisorEffects(bool enabled) {
  EnsureInitialized();
  sHideVisorEffects = enabled;
  MarkDirty();
}

bool RevealMap() {
  EnsureInitialized();
  return sRevealMap;
}

void SetRevealMap(bool enabled) {
  EnsureInitialized();
  sRevealMap = enabled;
  MarkDirty();
}

bool MapPickups() {
  EnsureInitialized();
  return sMapPickups;
}

void SetMapPickups(bool enabled) {
  EnsureInitialized();
  sMapPickups = enabled;
  MarkDirty();
}

bool MapLogicColors() {
  EnsureInitialized();
  return sMapLogicColors;
}

void SetMapLogicColors(bool enabled) {
  EnsureInitialized();
  sMapLogicColors = enabled;
  MarkDirty();
}

int ApSuitDamage() {
  EnsureInitialized();
  return sApSuitDamage;
}

void SetApSuitDamage(int mode) {
  EnsureInitialized();
  sApSuitDamage = mode >= 0 && mode <= 2 ? mode : 1;
  MarkDirty();
}

bool SkippableCutscenes() {
  EnsureInitialized();
  return sSkippableCutscenes;
}

void SetSkippableCutscenes(bool enabled) {
  EnsureInitialized();
  sSkippableCutscenes = enabled;
  MarkDirty();
}

float FirstPersonFov() {
  EnsureInitialized();
  return sFirstPersonFov;
}

void SetFirstPersonFov(float degrees) {
  EnsureInitialized();
  if (std::isfinite(degrees)) {
    sFirstPersonFov = std::clamp(degrees, kFovMin, kFovMax);
    MarkDirty();
  }
}

int Msaa() {
  EnsureInitialized();
  return sMsaa;
}

void SetMsaa(int samples) {
  EnsureInitialized();
  samples = samples >= 4 ? 4 : 1;
  if (sMsaa != samples) {
    sMsaa = samples;
    aurora_set_graphics_quality(static_cast< uint32_t >(sMsaa), static_cast< uint16_t >(sAnisotropy));
    MarkDirty();
  }
}

int Anisotropy() {
  EnsureInitialized();
  return sAnisotropy;
}

void SetAnisotropy(int level) {
  EnsureInitialized();
  level = std::clamp(level, 1, 16);
  if (sAnisotropy != level) {
    sAnisotropy = level;
    aurora_set_graphics_quality(static_cast< uint32_t >(sMsaa), static_cast< uint16_t >(sAnisotropy));
    MarkDirty();
  }
}

bool UnlockHardMode() {
  EnsureInitialized();
  return sUnlockHardMode;
}

void SetUnlockHardMode(bool enabled) {
  EnsureInitialized();
  sUnlockHardMode = enabled;
  MarkDirty();
}

bool UnlockFusionSuit() {
  EnsureInitialized();
  return sUnlockFusionSuit;
}

void SetUnlockFusionSuit(bool enabled) {
  EnsureInitialized();
  sUnlockFusionSuit = enabled;
  MarkDirty();
}

bool UnlockGalleries() {
  EnsureInitialized();
  return sUnlockGalleries;
}

void SetUnlockGalleries(bool enabled) {
  EnsureInitialized();
  sUnlockGalleries = enabled;
  MarkDirty();
}

bool MouseAim() {
  EnsureInitialized();
  return sMouseAim;
}

void SetMouseAim(bool enabled) {
  EnsureInitialized();
  sMouseAim = enabled;
  ResetMouseAim();
}

bool TwinStick() {
  EnsureInitialized();
  return sTwinStick;
}

void SetTwinStick(bool enabled) {
  EnsureInitialized();
  sTwinStick = enabled;
  MarkDirty();
}

float TwinStickRightY() { return sTwinStickRightY; }

void SetTwinStickRightY(float y) { sTwinStickRightY = y; }

bool BeamShiftHeld() { return sBeamShiftHeld; }

void SetBeamShiftHeld(bool held) { sBeamShiftHeld = held; }

bool SpringBall() {
  EnsureInitialized();
  return sSpringBall;
}

void SetSpringBall(bool enabled) {
  EnsureInitialized();
  sSpringBall = enabled;
  MarkDirty();
}

bool SwapScanXray() {
  EnsureInitialized();
  return sSwapScanXray;
}

void SetSwapScanXray(bool enabled) {
  EnsureInitialized();
  sSwapScanXray = enabled;
  MarkDirty();
}

int ShiftBinding(int slot) {
  EnsureInitialized();
  return slot >= 0 && slot < 3 ? sShiftBindings[slot] : -1;
}

void SetShiftBinding(int slot, int code) {
  EnsureInitialized();
  if (slot >= 0 && slot < 3) {
    sShiftBindings[slot] = code;
    MarkDirty();
  }
}

int PadAltButton(int bit) {
  EnsureInitialized();
  return bit >= 0 && bit < kPadAltCount ? sPadAltButtons[bit] : -1;
}

void SetPadAltButton(int bit, int code) {
  EnsureInitialized();
  if (bit >= 0 && bit < kPadAltCount && sPadAltButtons[bit] != code) {
    sPadAltButtons[bit] = code;
    MarkDirty();
  }
}

int MouseAction(int button) {
  EnsureInitialized();
  return button >= 0 && button < PortInputMap::kMouseButtonCount ? sMouseActions[button]
                                                                  : PortInputMap::kMA_None;
}

void SetMouseAction(int button, int action) {
  EnsureInitialized();
  if (button >= 0 && button < PortInputMap::kMouseButtonCount && action >= 0 &&
      action < PortInputMap::kMA_Count) {
    sMouseActions[button] = action;
    // A button held as it changes must not start the new action mid-press.
    sMouseButtonGate.Reset();
    MarkDirty();
  }
}

bool SpeedrunTimer() {
  EnsureInitialized();
  return sSpeedrunTimer;
}

void SetSpeedrunTimer(bool enabled) {
  EnsureInitialized();
  sSpeedrunTimer = enabled;
  MarkDirty();
}

bool LiveSplit() {
  EnsureInitialized();
  return sLiveSplit;
}

void SetLiveSplit(bool enabled) {
  EnsureInitialized();
  sLiveSplit = enabled;
  ApplyLiveSplit();
  MarkDirty();
}

bool DiscordPresence() {
  EnsureInitialized();
  return sDiscord;
}

void SetDiscordPresence(bool enabled) {
  EnsureInitialized();
  sDiscord = enabled;
  ApplyDiscord();
  MarkDirty();
}

std::string DiscordAppId() {
  EnsureInitialized();
  return sDiscordAppId;
}

void SetDiscordAppId(const std::string& id) {
  EnsureInitialized();
  sDiscordAppId = DigitsOnly(id);
  ApplyDiscord();
  MarkDirty();
}

std::string LiveSplitAddress() {
  EnsureInitialized();
  return sLiveSplitAddress;
}

void SetLiveSplitAddress(const std::string& address) {
  EnsureInitialized();
  if (address.empty()) {
    return;
  }
  sLiveSplitAddress = address;
  ApplyLiveSplit();
  MarkDirty();
}

bool ModsEnabled() {
  EnsureInitialized();
  return sModsEnabled;
}

void SetModsEnabled(bool enabled) {
  EnsureInitialized();
  sModsEnabled = enabled;
  MarkDirty();
}

std::string ModsDisabled() {
  EnsureInitialized();
  return sModsDisabled;
}

void SetModsDisabled(const std::string& list) {
  EnsureInitialized();
  sModsDisabled = list;
  MarkDirty();
}

bool Invulnerable() {
  EnsureInitialized();
  return sInvulnerableRun >= 0 ? sInvulnerableRun != 0 : sInvulnerable;
}

void SetInvulnerable(bool enabled) {
  EnsureInitialized();
  sInvulnerable = enabled;
  sInvulnerableRun = -1;
  MarkDirty();
}

bool LogFile() {
  EnsureInitialized();
  return sLogFile;
}

void SetLogFile(bool enabled) {
  EnsureInitialized();
  sLogFile = enabled;
  MarkDirty();
}

bool FastMorph() {
  EnsureInitialized();
  return sFastMorph;
}

void SetFastMorph(bool enabled) {
  EnsureInitialized();
  sFastMorph = enabled;
  MarkDirty();
}

bool LockOnToggle() {
  EnsureInitialized();
  return sLockOnToggle;
}

void SetLockOnToggle(bool enabled) {
  EnsureInitialized();
  sLockOnToggle = enabled;
  MarkDirty();
}

bool StickyCharge() {
  EnsureInitialized();
  return sStickyCharge;
}

void SetStickyCharge(bool enabled) {
  EnsureInitialized();
  sStickyCharge = enabled;
  MarkDirty();
}

bool SpringBallFlick() {
  EnsureInitialized();
  return sSpringFlick;
}

void SetSpringBallFlick(bool enabled) {
  EnsureInitialized();
  sSpringFlick = enabled;
  MarkDirty();
}

float SpringBallFlickRate() {
  EnsureInitialized();
  return sSpringFlickRate;
}

void SetSpringBallFlickRate(float radiansPerSecond) {
  EnsureInitialized();
  if (std::isfinite(radiansPerSecond) && radiansPerSecond >= 2.f && radiansPerSecond <= 20.f) {
    sSpringFlickRate = radiansPerSecond;
    MarkDirty();
  }
}

bool SpringBallFlickPending() { return sSpringFlickLatch > 0.f; }

void ClearSpringBallFlick() { sSpringFlickLatch = 0.f; }

void SetGyroOverride(bool active, float pitch, float yaw) {
  sGyroOverride = active;
  sGyroOverridePitch = pitch;
  sGyroOverrideYaw = yaw;
}

float StickAimRate() {
  EnsureInitialized();
  return sStickAimRate;
}

void SetStickAimRate(float pixelsPerSecond) {
  EnsureInitialized();
  if (std::isfinite(pixelsPerSecond) && pixelsPerSecond >= 50.f && pixelsPerSecond <= 4000.f) {
    sStickAimRate = pixelsPerSecond;
    MarkDirty();
  }
}

void AddStickAim(float x, float y, float dt) {
  EnsureInitialized();
  if (!sTwinStick || Visible() || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(dt) ||
      dt <= 0.f) {
    return;
  }
  // Stick aim stands in for free look, so it follows the game's Reverse Y Axis
  // option, as free look does.
  if (gpGameState != nullptr && gpGameState->GameOptions().GetInvertYAxis()) {
    y = -y;
  }
  // x right / y up; the aim state expects SDL-style right/down positive.
  sStickAimVelX = x * sStickAimRate;
  sStickAimVelY = -y * sStickAimRate;
  sMouseFrameX += sStickAimVelX * dt;
  sMouseFrameY += sStickAimVelY * dt;
}

int GyroMode() {
  EnsureInitialized();
  return sGyroMode;
}

void SetGyroMode(int mode) {
  EnsureInitialized();
  if (mode < 0 || mode > 2) {
    return;
  }
  sGyroMode = mode;
  MarkDirty();
}

int GyroSource() {
  EnsureInitialized();
  return sGyroSource;
}

void SetGyroSource(int source) {
  EnsureInitialized();
  if (source < 0 || source > 2) {
    return;
  }
  sGyroSource = source;
  MarkDirty();
}

float GyroRate() {
  EnsureInitialized();
  return sGyroRate;
}

void SetGyroRate(float pixelsPerSecondPerRad) {
  EnsureInitialized();
  if (std::isfinite(pixelsPerSecondPerRad) && pixelsPerSecondPerRad >= 20.f &&
      pixelsPerSecondPerRad <= 5000.f) {
    sGyroRate = pixelsPerSecondPerRad;
    MarkDirty();
  }
}

const char* GyroStatus() { return sGyroStatus; }

namespace {
// Quarter turns from portrait, as SDL numbers Android rotations (landscape is
// the phone's right side up).
int OrientationQuarters(SDL_DisplayOrientation orientation) {
  switch (orientation) {
  case SDL_ORIENTATION_LANDSCAPE:
    return 1;
  case SDL_ORIENTATION_PORTRAIT_FLIPPED:
    return 2;
  case SDL_ORIENTATION_LANDSCAPE_FLIPPED:
    return 3;
  default:
    return 0;
  }
}

// Pitch (x, positive tilts the far edge up) and yaw (positive turns right)
// rates in rad/s from the chosen source; false when there is none.
bool ReadGyroRates(float& pitch, float& yaw) {
  if (sGyroOverride) {
    pitch = sGyroOverridePitch;
    yaw = sGyroOverrideYaw;
    sGyroStatus = "console";
    return true;
  }
  const bool wantController = sGyroSource == 0 || sGyroSource == 1;
  const bool wantPhone = sGyroSource == 0 || sGyroSource == 2;
  bool haveRates = false;

  if (wantController) {
    if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0)) {
      if (SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) {
        // Asked per pad, so a reconnected or swapped pad gets its gyro on too.
        if (!SDL_GamepadSensorEnabled(pad, SDL_SENSOR_GYRO)) {
          SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
        }
        float data[3];
        if (SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, data, 3)) {
          // Radians per second; x is pitch, y is yaw. Sensor rates are
          // counter-clockwise positive, so a positive y turns left.
          yaw = -data[1];
          pitch = data[0];
          haveRates = true;
          sGyroStatus = "controller";
        }
      }
    }
  }

  if (!haveRates && wantPhone) {
    if (sPhoneGyro == nullptr && !sPhoneGyroSearched) {
      sPhoneGyroSearched = true;
      int count = 0;
      if (SDL_SensorID* ids = SDL_GetSensors(&count)) {
        for (int i = 0; i < count; ++i) {
          if (SDL_GetSensorTypeForID(ids[i]) == SDL_SENSOR_GYRO) {
            sPhoneGyro = SDL_OpenSensor(ids[i]);
            break;
          }
        }
        SDL_free(ids);
      }
    }
    if (sPhoneGyro != nullptr) {
      float data[3];
      if (SDL_GetSensorData(sPhoneGyro, data, 3)) {
        // The phone reports its own axes (x right, y up in its natural
        // orientation), so turn them to the screen's, as SDL does for its
        // accelerometer: landscape would otherwise swap pitch and yaw. The
        // rate about the screen's up axis is negated, as for a pad.
        const SDL_DisplayID display = SDL_GetPrimaryDisplay();
        const int quarters = (OrientationQuarters(SDL_GetCurrentDisplayOrientation(display)) -
                              OrientationQuarters(SDL_GetNaturalDisplayOrientation(display)) + 4) %
                             4;
        switch (quarters) {
        case 1:
          pitch = -data[1];
          yaw = -data[0];
          break;
        case 2:
          pitch = -data[0];
          yaw = data[1];
          break;
        case 3:
          pitch = data[1];
          yaw = data[0];
          break;
        default:
          pitch = data[0];
          yaw = -data[1];
          break;
        }
        haveRates = true;
        sGyroStatus = "phone";
      }
    }
  }
  return haveRates;
}
} // namespace

void PollGyro() {
  EnsureInitialized();
  // Called once per presented frame, which is not once per tick when the
  // frame limiter is off, so the rates integrate over the real frame time.
  static uint64_t sLastNs = 0;
  const uint64_t nowNs = SDL_GetTicksNS();
  const float dt = sLastNs == 0 ? 0.f : std::min(float(double(nowNs - sLastNs) * 1e-9), 0.1f);
  sLastNs = nowNs;
  if (sSpringFlickLatch > 0.f) {
    sSpringFlickLatch -= dt;
  }
  // Gyro feeds the same aim state the mouse and twin stick use, so aiming only
  // has an effect where that is driving the camera. Flicks need no aim.
  const bool aim = sGyroMode != 0 && (sMouseAim || sTwinStick);
  if (!aim && !sSpringFlick) {
    sGyroStatus = sGyroMode == 0 ? "off" : "needs mouse aim or twin stick";
    return;
  }

  float yaw = 0.f;
  float pitch = 0.f;
  if (!ReadGyroRates(pitch, yaw)) {
    sGyroStatus = "no gyro found";
    sSpringFlickArmed = true;
    return;
  }

  if (sSpringFlick) {
    // One flick per upward swing: it re-arms once the pitch speed has dropped
    // to half the threshold. The latch outlives the tick so a flick just before
    // the ball lands still springs.
    if (sSpringFlickArmed && pitch > sSpringFlickRate) {
      sSpringFlickLatch = 0.2f;
      sSpringFlickArmed = false;
    } else if (pitch < sSpringFlickRate * 0.5f) {
      sSpringFlickArmed = true;
    }
  }
  if (!aim) {
    if (sGyroMode != 0) {
      sGyroStatus = "flicks only (aim needs mouse aim or twin stick)";
    }
    return;
  }

  bool active = sGyroMode == 2;
  if (!active) {
    // Hold to aim: right stick click on a pad, left ctrl on a keyboard.
    if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0)) {
      active = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    }
    if (!active) {
      const bool* keys = SDL_GetKeyboardState(nullptr);
      active = keys != nullptr && keys[SDL_SCANCODE_LCTRL] != 0;
    }
  }
  if (!active) {
    sGyroStatus = "held off";
    return;
  }

  if (!std::isfinite(dt) || dt <= 0.f) {
    return;
  }
  // x right / y up, the same shape AddStickAim takes; the aim state expects
  // right/down positive.
  sGyroPendingX += yaw * sGyroRate * dt;
  sGyroPendingY -= pitch * sGyroRate * dt;
}

void ResetMouseAim() {
  sMouseAimState.Reset();
  sMouseGameplayActive = false;
  sMouseButtonGate.Reset();
  sMousePendingX = sMousePendingY = sMouseFrameX = sMouseFrameY = 0.f;
  sGyroPendingX = sGyroPendingY = sStickAimVelX = sStickAimVelY = 0.f;
}

void SetMouseCaptured(bool captured) {
  sMouseCaptured = captured;
  if (!captured) {
    sMouseButtonGate.Reset();
    sMousePendingX = sMousePendingY = sMouseFrameX = sMouseFrameY = 0.f;
  }
}

bool MouseCaptured() { return sMouseCaptured; }
bool MouseGameplayActive() { return sMouseGameplayActive; }
void SetMouseGameplayActive(bool active) {
  sMouseGameplayActive = active;
  if (!active) ResetMouseAim();
}
bool MouseInvertX() { EnsureInitialized(); return sMouseInvertX; }
bool MouseInvertY() { EnsureInitialized(); return sMouseInvertY; }
bool MouseButtons() { EnsureInitialized(); return sMouseButtons; }
bool MouseCrosshair() { EnsureInitialized(); return sMouseCrosshair; }
int CrosshairSize() { EnsureInitialized(); return sCrosshairSize; }
void SetCrosshairSize(int percent) {
  EnsureInitialized();
  sCrosshairSize = std::clamp(percent, kCrosshairSizeMin, kCrosshairSizeMax);
  MarkDirty();
}
unsigned MouseWeaponButtons(unsigned held) {
  return sMouseButtonGate.Poll(MouseAim() && MouseButtons() && MouseGameplayActive() &&
                               MouseCaptured() && !Visible(), held);
}
unsigned MouseMenuButtons(unsigned held, bool focused) {
  return sMouseMenuGate.Poll(MouseAim() && MouseButtons() && !MouseGameplayActive() &&
                                 !Visible() && focused,
                             held);
}
void NoteMouseButton(bool synthetic, unsigned mask, bool down) {
  sMouseHeldButtons.Note(synthetic, mask, down);
}
void ClearMouseButtons() { sMouseHeldButtons.Clear(); }
unsigned MouseHeldButtons() { return sMouseHeldButtons.Held(); }

bool UpdateMouseAim(bool active, bool locked, float x, float y, float z) {
  SetMouseGameplayActive(active);
  const bool applied = sMouseAimState.Update(active, locked, x, y, z, sMouseFrameX, sMouseFrameY,
                                            MouseSensitivity(), MouseInvertX(), MouseInvertY());
  if (applied) sMouseFrameX = sMouseFrameY = 0.f;
  sAimAppliedLastTick = applied;
  return applied;
}
void SynchronizeMouseAim(float x, float y, float z) { sMouseAimState.Synchronize(x, y, z); }

float MouseSensitivity() {
  EnsureInitialized();
  return sMouseSensitivity;
}

void SetMouseSensitivity(float radiansPerPixel) {
  EnsureInitialized();
  if (std::isfinite(radiansPerPixel) && radiansPerPixel > 0.f) {
    sMouseSensitivity = radiansPerPixel;
  }
}

void AddMouseDelta(float dx, float dy) {
  if (!sMouseCaptured || !MouseAim() || Visible() || !std::isfinite(dx) || !std::isfinite(dy)) {
    return;
  }
  sMousePendingX += dx;
  sMousePendingY += dy;
}

void BeginFrameMouse() {
  sMouseFrameX = sMousePendingX + sGyroPendingX;
  sMouseFrameY = sMousePendingY + sGyroPendingY;
  sMousePendingX = sMousePendingY = 0.f;
  sGyroPendingX = sGyroPendingY = 0.f;
  // AddStickAim sets it again during this tick's input update.
  sStickAimVelX = sStickAimVelY = 0.f;
  sAimAppliedLastTick = false;
}

bool PresentedAimDelta(float fraction, float& dyaw, float& dpitch) {
  dyaw = dpitch = 0.f;
  if (!sFrameInterpolation || !sMouseGameplayActive || !sAimAppliedLastTick ||
      !std::isfinite(fraction) || fraction < 0.f) {
    return false;
  }
  // What the next tick will consume: the mouse and gyro travel so far, plus the
  // stick's travel over the part of the tick already shown.
  const float ahead = std::min(fraction, 1.f) * TickPeriod();
  const float dx = sMousePendingX + sGyroPendingX + sStickAimVelX * ahead;
  const float dy = sMousePendingY + sGyroPendingY + sStickAimVelY * ahead;
  float yaw = 0.f;
  float pitch = 0.f;
  if (!sMouseAimState.Preview(dx, dy, MouseSensitivity(), MouseInvertX(), MouseInvertY(), yaw,
                              pitch)) {
    return false;
  }
  dyaw = static_cast<float>(std::remainder(double(yaw) - sMouseAimState.yaw, 2.0 * PortMouse::kPi));
  dpitch = pitch - sMouseAimState.pitch;
  return dyaw != 0.f || dpitch != 0.f;
}

bool FrameInterpolation() {
  EnsureInitialized();
  return sFrameInterpolation;
}

void SetFrameInterpolation(bool enabled) {
  EnsureInitialized();
  sFrameInterpolation = enabled;
  MarkDirty();
}

bool ActorInterpolation() {
  EnsureInitialized();
  return sActorInterpolation;
}

void SetActorInterpolation(bool enabled) {
  EnsureInitialized();
  sActorInterpolation = enabled;
  MarkDirty();
}

bool PoseInterpolation() {
  EnsureInitialized();
  return sPoseInterpolation;
}

void SetPoseInterpolation(bool enabled) {
  EnsureInitialized();
  sPoseInterpolation = enabled;
  MarkDirty();
}

bool ParticleInterpolation() {
  EnsureInitialized();
  return sParticleInterpolation;
}

void SetParticleInterpolation(bool enabled) {
  EnsureInitialized();
  sParticleInterpolation = enabled;
  MarkDirty();
}

bool PresentOverride(float& t) {
  EnsureInitialized();
  if (sPresentOverride < 0.f) {
    return false;
  }
  if (sPresentOverride == kPresentTick) {
    t = -1.f;
  } else if (sPresentOverride > 1.f) {
    t = 0.25f * static_cast< float >(sPresentCycleFrame++ & 3);
  } else {
    t = sPresentOverride;
  }
  return true;
}

void SetPresentOverride(float value) {
  EnsureInitialized();
  sPresentOverride = value < 0.f             ? -1.f
                     : value == kPresentTick ? kPresentTick
                     : value > 1.f           ? kPresentCycle
                                             : value;
  sPresentCycleFrame = 0;
}

float PresentOverrideValue() {
  EnsureInitialized();
  return sPresentOverride;
}

bool TickHold() { return sTickHold; }

void SetTickHold(bool held) {
  sTickHold = held;
  sHeldTicks = 0;
}

void StepTicks(unsigned count) { sHeldTicks += count; }

unsigned PendingHeldTicks() { return sHeldTicks; }

unsigned TakeHeldTicks() {
  // One tick per loop, like MP_TURBO=1, so each step is one drawn frame.
  if (sHeldTicks == 0) {
    return 0;
  }
  --sHeldTicks;
  return 1;
}

void GetFrameMouseDelta(float& dx, float& dy) {
  dx = sMouseFrameX;
  dy = sMouseFrameY;
}

float AimYaw() { return sMouseAimState.yaw; }
float AimPitch() { return sMouseAimState.pitch; }
bool AimInitialized() { return sMouseAimState.initialized; }

bool AiAudioEnabled() {
  EnsureInitialized();
  return AIPortOutputEnabled() != 0;
}

void SetAiAudioEnabled(bool enabled) {
  EnsureInitialized();
  if (sAiAudioEnabled == enabled && AiAudioEnabled() == enabled) {
    return;
  }
  sAiAudioEnabled = enabled;
  AIPortSetOutputEnabled(enabled ? 1 : 0);
}

bool MusyxAudioEnabled() {
  EnsureInitialized();
  return sMusyxAudioEnabled;
}

void SetMusyxAudioEnabled(bool enabled) {
  EnsureInitialized();
  if (sMusyxAudioEnabled == enabled) {
    return;
  }
  sMusyxAudioEnabled = enabled;
  salSetMuted(enabled ? 0 : 1);
}

void RequestReset() { sResetRequested = true; }

bool ConsumeResetRequest() {
  const bool requested = sResetRequested;
  sResetRequested = false;
  return requested;
}

void SetStateManager(CStateManager* mgr) {
  if (mgr != sStateManager) {
    // The tracker's room names come from the old world's PAKs.
    PortTracker::Reset();
  }
  sStateManager = mgr;
}
CStateManager* StateManager() { return sStateManager; }
bool ViewRay(float origin[3], float forward[3]) {
  if (sStateManager == nullptr || sStateManager->GetCameraManager() == nullptr) {
    return false;
  }
  const CTransform4f view =
      PortFreeCam::View(sStateManager->GetCameraManager()->GetCurrentCameraTransform(*sStateManager));
  const CVector3f at = view.GetTranslation();
  const CVector3f to = view.GetForward();
  origin[0] = at.GetX();
  origin[1] = at.GetY();
  origin[2] = at.GetZ();
  forward[0] = to.GetX();
  forward[1] = to.GetY();
  forward[2] = to.GetZ();
  return true;
}

namespace {
const char* const kPbrViews[] = {"off",     "albedo",     "normal", "rough",    "metal", "ao",
                                 "ambient", "reflection", "glow",   "exposure", "kind"};
int sPbrView = 0;
} // namespace
int PbrViewCount() { return int(sizeof(kPbrViews) / sizeof(kPbrViews[0])); }
const char* PbrViewName(int view) { return view >= 0 && view < PbrViewCount() ? kPbrViews[view] : "?"; }
int PbrView() { return sPbrView; }
void SetPbrView(int view) {
  sPbrView = view >= 0 && view < PbrViewCount() ? view : 0;
  GXSetPBRDebugView(u32(sPbrView));
}
void RequestTeleport(int areaId) { sPendingTeleport = areaId; }
bool ConsumeTeleportRequest(int& areaId) {
  if (sPendingTeleport < 0) {
    return false;
  }
  areaId = sPendingTeleport;
  sPendingTeleport = -1;
  return true;
}
void RequestWorldTeleport(uint32_t worldId, uint32_t areaAssetId) {
  sWorldTeleportWorld = worldId;
  sWorldTeleportArea = areaAssetId;
  sHasWorldTeleport = true;
}
bool ConsumeWorldTeleportRequest(uint32_t& worldId, uint32_t& areaAssetId) {
  if (!sHasWorldTeleport) {
    return false;
  }
  worldId = sWorldTeleportWorld;
  areaAssetId = sWorldTeleportArea;
  sHasWorldTeleport = false;
  return true;
}

void RequestWorldSweep() {
  if (sWorldSweepRequested || sWorldSweep.active || sHasWorldTeleport || sPendingTeleport >= 0)
    return;
  sWorldSweepRequested = true;
}

// Activates one layer of the current area and deactivates the rest, so the
// area's objects are built for that layer when it is next reconstructed.
// Layer 0 is the one a fresh save has, which is why a plain tour never sees
// anything behind another layer.
void SetSweepLayer(CStateManager& mgr, CWorld* world, TAreaId area, int layer) {
  rstl::rc_ptr< CScriptLayerManager >& layers = mgr.WorldLayerState();
  if (layers.IsNull())
    return;
  const int count = layers->GetAreaLayerCount(area);
  for (int i = 0; i < count; ++i)
    layers->SetLayerActive(area, TLayerId(i), i == layer);
  (void)world;
}

bool SweepLayersEnabled() {
  const char* value = std::getenv("MP_RANDO_SWEEP_LAYERS");
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

bool ConsumeWorldSweepRequest(CStateManager& mgr) {
  // This entry point is called only by gameplay, never the frontend or UI.
  static const bool envChecked = [] {
    const char* value = std::getenv("MP_RANDO_SWEEP");
    if (value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0)
      RequestWorldSweep();
    return true;
  }();
  (void)envChecked;
  WorldSweep& sweep = sWorldSweep;
  if ((!sWorldSweepRequested && !sweep.active) || mgr.GetWantsToQuit()) {
    return false;
  }
  if (sHasWorldTeleport || sPendingTeleport >= 0 || sResetRequested) {
    // A manual debug request wins; do not resume the tour behind the user's back.
    if (sweep.active) std::fputs("[sweep] cancelled: another debug teleport/reset\n", stderr);
    sweep = {};
    sWorldSweepRequested = false;
    return false;
  }
  CWorld* world = mgr.World();
  if (mgr.GetGameState() != CStateManager::kGS_Running || world == nullptr ||
      gpGameState == nullptr || gpMemoryCard == nullptr) return false;

  if (sWorldSweepRequested) {
    const auto& worlds = gpMemoryCard->GetMemoryWorlds();
    if (worlds.empty()) return false;
    sweep = {};
    // MP_RANDO_SWEEP_WORLDS=<hex id>[,<hex id>...] limits the tour to those
    // worlds, e.g. to redo one world without walking the seven before it.
    std::vector< uint32_t > only;
    if (const char* list = std::getenv("MP_RANDO_SWEEP_WORLDS")) {
      for (const char* p = list; *p != '\0';) {
        char* end = nullptr;
        const unsigned long id = std::strtoul(p, &end, 16);
        if (end == p) {
          ++p;
          continue;
        }
        only.push_back(static_cast< uint32_t >(id));
        p = end;
      }
    }
    const auto wanted = [&only](uint32_t id) {
      if (only.empty()) return true;
      for (uint32_t w : only)
        if (w == id) return true;
      return false;
    };
    // Visit the live world first so its MLVL can supply the first area list;
    // later worlds are entered at area zero, which also visits their first area.
    if (wanted(world->IGetWorldAssetId())) sweep.worlds.push_back(world->IGetWorldAssetId());
    for (const auto& entry : worlds) {
      if (entry.first != world->IGetWorldAssetId() && wanted(entry.first))
        sweep.worlds.push_back(entry.first);
    }
    sWorldSweepRequested = false;
    if (sweep.worlds.empty()) {
      std::fputs("[sweep] stopped: MP_RANDO_SWEEP_WORLDS matches no world\n", stderr);
      sweep = {};
      return false;
    }
    sweep.active = true;
    std::fprintf(stderr, "[sweep] begin: %zu worlds (use MP_RANDO_DUMP=1 for pickups)\n",
                 sweep.worlds.size());
    if (world->IGetWorldAssetId() != sweep.worlds[0]) {
      // The live world was filtered out: enter the first wanted world the same
      // way a finished world hands over to the next one.
      sweep.waiting = true;
      RequestWorldTeleport(sweep.worlds[0], 0u);
      return true;
    }
  }
  if (world->IGetWorldAssetId() != sweep.worlds[sweep.world]) {
    std::fputs("[sweep] cancelled: gameplay changed worlds\n", stderr);
    sweep = {};
    return false;
  }
  if (sweep.areas.empty()) {
    for (int i = 0; i < world->IGetAreaCount(); ++i)
      sweep.areas.push_back(world->IGetAreaAlways(TAreaId(i))->IGetAreaAssetId());
    if (sweep.areas.empty()) {
      std::fputs("[sweep] stopped: world has no areas\n", stderr);
      sweep = {};
      return false;
    }
    std::fprintf(stderr, "[sweep] world %zu/%zu: %08X, %zu areas\n", sweep.world + 1,
                 sweep.worlds.size(), sweep.worlds[sweep.world], sweep.areas.size());
    if (SweepLayersEnabled()) {
      // How many layers the widest area has, so every area gets a pass per
      // layer even where its own count is lower. An area with fewer layers
      // simply rebuilds the same thing, which costs a restart and nothing else.
      int widest = 1;
      for (int i = 0; i < world->IGetAreaCount(); ++i) {
        const int count = mgr.WorldLayerState()
                              ? mgr.WorldLayerState()->GetAreaLayerCount(TAreaId(i))
                              : 1;
        if (count > widest)
          widest = count;
      }
      sweep.layerCount = widest;
      std::fprintf(stderr, "[sweep] layers: up to %d per area\n", widest);
    }
  }

  // Count consecutive quiet simulation ticks, not rendered frames or a wall
  // clock timeout. Current-area construction alone does not imply that adjacent
  // areas, map tiles and factory requests have finished streaming.
  const TAreaId current = world->GetCurrentAreaId();
  const bool areaExists = world->DoesAreaExist(current);
  const bool areaValid = areaExists && world->GetArea(current)->IsValidated();
  const bool loadingIdle = world->GetChainHead(CWorld::kC_Loading) == CWorld::GetAliveAreasEnd();
  const bool freeingIdle = world->GetChainHead(CWorld::kC_ToDeallocate) == CWorld::GetAliveAreasEnd();
  const bool mapIdle = !world->GetMapWorld()->IsMapAreasStreaming();
  const bool loadsIdle = !gpResourceFactory->HasPendingLoads();
  const bool ready = areaExists && areaValid && loadingIdle && freeingIdle && mapIdle && loadsIdle;
  if (!ready) {
    // A tour that never settles is otherwise invisible: no output, no
    // progress, just frames. Say what is still outstanding, and how long it has
    // been that way, so the stall names itself instead of looking like work.
    if (sweep.settledTicks == 0) {
      std::fprintf(stderr,
                   "[sweep] waiting on area %d: %s%s%s%s%s (target %08X, pass %zu/%zu)\n",
                   static_cast<int>(current.Value()), areaExists ? "" : "no-area ",
                   areaValid ? "" : "unvalidated ", loadingIdle ? "" : "loading ",
                   freeingIdle ? "" : "freeing ", mapIdle ? "" : "map-streaming ",
                   sweep.areas.empty() ? 0u : sweep.areas[sweep.area], sweep.area + 1,
                   sweep.areas.size());
    }
    // 10 seconds of simulation. A pass that has not settled by then is not
    // going to: the loading chain stays put while the area is alive and
    // nothing else moves it. The window is short because the wait is measured
    // in ticks and a throttled run reaches a tick slowly.
    if (++sweep.stalledTicks == 600) {
      // One area that will not settle must not end a whole tour: the rest of
      // the world is still worth dumping, and this area's objects were built on
      // the way in even if the settle check never agreed. Report it, skip the
      // pass, and carry on from the next one.
      std::fprintf(stderr, "[sweep] giving up on %08X after 10s: %s%s%s%s%s\n",
                   sweep.areas.empty() ? 0u : sweep.areas[sweep.area],
                   areaExists ? "" : "no-area ", areaValid ? "" : "unvalidated ",
                   loadingIdle ? "" : "loading ", freeingIdle ? "" : "freeing ",
                   mapIdle ? "" : "map-streaming ");
      sweep.stalledTicks = 0;
      sweep.settledTicks = 0;
      sweep.revisiting = false;
      if (sweep.waiting) {
        ++sweep.completedAreas;
        ++sweep.area;
        sweep.layer = 0;
        if (sweep.area >= sweep.areas.size()) {
          sweep.areas.clear();
          sweep.area = 0;
          ++sweep.world;
        }
        sweep.waiting = false;
      }
      if (sweep.area >= sweep.areas.size() || sweep.world >= sweep.worlds.size()) {
        std::fprintf(stderr, "[sweep] complete: %zu worlds, %u areas\n", sweep.worlds.size(),
                     sweep.completedAreas);
        sweep = {};
        return false;
      }
      return true;
    }
    sweep.settledTicks = 0;
    return false;
  }
  sweep.stalledTicks = 0;
  if (++sweep.settledTicks < 30) return false;
  sweep.settledTicks = 0;
  if (sweep.waiting) {
    const uint32_t settled = world->IGetAreaAlways(current)->IGetAreaAssetId();
    if (sweep.revisiting) {
      // This is the hop away from the area being re-layered: its objects have
      // been dumped, so all that matters is that it is gone by the time we go
      // back. Wait here, then return to it with the new layer active.
      sweep.revisiting = false;
      std::fprintf(stderr, "[sweep] left %08X for layer %d; returning\n", settled,
                   sweep.layer + 1);
      RequestWorldTeleport(sweep.worlds[sweep.world], sweep.areas[sweep.area]);
      return true;
    }
    if (settled != sweep.areas[sweep.area]) {
      // The destination's own scripts moved the player on (Impact Crater's
      // spawn points do). Its objects, and so its dump, were already built.
      std::fprintf(stderr, "[sweep] note: %08X moved the player to %08X; continuing\n",
                   sweep.areas[sweep.area], settled);
    }
    // One area, one layer: the count reports passes, not distinct areas, so a
    // multi-layered area is visibly a few.
    ++sweep.completedAreas;
    std::fprintf(stderr, "[sweep] area %zu/%zu: %08X layer %d/%d (total %u)\n", sweep.area + 1,
                 sweep.areas.size(), sweep.areas[sweep.area], sweep.layer + 1, sweep.layerCount,
                 sweep.completedAreas);
    if (++sweep.layer < sweep.layerCount) {
      // Another layer of the area just built. The layer state has to be set
      // before the area is reconstructed, because CGameArea builds the objects
      // of the layers that are active at construction time - which is why a
      // pickup behind an inactive layer is not in the dump at all.
      ++sweep.layerPasses;
      SetSweepLayer(mgr, world, current, sweep.layer);
      std::fprintf(stderr, "[sweep] re-entering %08X with layer %d active\n",
                   sweep.areas[sweep.area], sweep.layer);
      // The area has to be unloaded before it is rebuilt: SetLayerActive is a
      // bit flip, and travelling straight back onto a live area re-enters it
      // without ever scheduling its load again, so the pass never settles. Two
      // hops: out to the world's first area, which unloads this one, and the
      // return below lands on it with the new layer active.
      sweep.revisiting = true;
      RequestWorldTeleport(sweep.worlds[sweep.world], 0u);
      return true;
    }
    sweep.layer = 0;
    if (++sweep.area == sweep.areas.size()) {
      if (++sweep.world == sweep.worlds.size()) {
        std::fprintf(stderr, "[sweep] complete: %zu worlds, %u areas\n", sweep.worlds.size(),
                     sweep.completedAreas);
        sweep = {};
        return false;
      }
      sweep.areas.clear();
      sweep.area = 0;
    }
  }
  // QuitGame is consumed in this same tick. The next call with !GetWantsToQuit
  // belongs to the new manager even if the allocator reused its old address.
  sweep.waiting = true;
  RequestWorldTeleport(sweep.worlds[sweep.world],
                       sweep.areas.empty() ? 0u : sweep.areas[sweep.area]);
  return true;
}

bool Visible() {
  EnsureInitialized();
  return sVisible;
}

bool OverlayVisible() { return sOverlayVisible.load(std::memory_order_acquire); }

bool TwinStickFlag() { return sTwinStickFlag.load(std::memory_order_acquire); }

void SaveSettingsNow() {
  EnsureInitialized();
  SaveSettings();
}

// A VR setting changed (vr/vr_settings.cpp): write the file on the next
// occasion the overlay would, as for any of its own settings.
void MarkVrSettingsDirty() { sSettingsDirty = true; }

// The overlay is a full-screen panel with a page list instead of tabs, which
// fits a touchscreen and reads better on the desktop too. The desktop can go
// back to the old floating tabbed window (Render > Overlay as a floating
// window); MP_TOUCH_UI forces the page layout regardless.
bool PageLayout() {
#if defined(__ANDROID__)
  return true;
#else
  static const bool sForced = std::getenv("MP_TOUCH_UI") != nullptr;
  return sForced || !sOverlayWindowed;
#endif
}

SDL_Window* MainWindow() {
  static SDL_Window* sWindow = nullptr;
  if (sWindow == nullptr) {
    int windowCount = 0;
    if (SDL_Window** windows = SDL_GetWindows(&windowCount)) {
      if (windowCount > 0) {
        sWindow = windows[0];
      }
      SDL_free(windows);
    }
  }
  return sWindow;
}

bool WindowSize(int& width, int& height) {
  SDL_Window* window = MainWindow();
  return window != nullptr && SDL_GetWindowSize(window, &width, &height) && width > 0 &&
         height > 0;
}

// Sizes in unscaled pixels; UpdateUiScale multiplies them by the display scale
// like the defaults. A fingertip covers far more than a cursor does, so frames,
// grabs and scrollbars grow, and TouchExtraPadding widens every hit box a
// little beyond what is drawn.
void ApplyTouchStyle(ImGuiStyle& style) {
  style.WindowPadding = ImVec2(10.f, 10.f);
  style.FramePadding = ImVec2(10.f, 7.f);
  style.ItemSpacing = ImVec2(10.f, 8.f);
  style.ItemInnerSpacing = ImVec2(8.f, 6.f);
  style.CellPadding = ImVec2(6.f, 4.f);
  style.TouchExtraPadding = ImVec2(3.f, 3.f);
  style.IndentSpacing = 22.f;
  style.ScrollbarSize = 18.f;
  style.GrabMinSize = 18.f;
  style.FrameRounding = 5.f;
  style.GrabRounding = 5.f;
  style.ScrollbarRounding = 9.f;
  style.TabRounding = 5.f;
}

// Phones report a density of roughly 3, which leaves ImGui's default 13px font
// unreadably small, so scale the overlay to match the display. The scale is not
// known on the first frame, so keep watching for it instead of latching once.
void UpdateUiScale() {
  if (ImGui::GetCurrentContext() == nullptr) {
    return;
  }
  static bool sInitialized = false;
  static float sAppliedScale = 1.f;
  if (!sInitialized) {
    sInitialized = true;
    // The overlay has to size itself to the scaled font, so do not restore a
    // window size remembered from a previous, smaller run.
    ImGui::GetIO().IniFilename = nullptr;
    // Before any scaling, so the scaling applies to these sizes too. Both
    // layouts use it, so switching layout needs no restyle.
    ApplyTouchStyle(ImGui::GetStyle());
  }
  SDL_Window* window = MainWindow();
  if (window == nullptr) {
    return;
  }
  const float displayScale = SDL_GetWindowDisplayScale(window);
  const float uiScale = std::clamp(displayScale, 1.f, 4.f);
  if (uiScale == sAppliedScale) {
    return;
  }
  const float ratio = uiScale / sAppliedScale;
  sAppliedScale = uiScale;
  ImGui::GetStyle().ScaleAllSizes(ratio);
  ImGui::GetIO().FontGlobalScale *= ratio;
}

void RequestToggle() { sToggleRequested.store(true, std::memory_order_release); }

void UpdateControllerNav() {
  EnsureInitialized();
  // Registered here rather than in EnsureInitialized so the event system is only
  // touched from the game thread; the Java visibility query can reach that
  // initialization from the UI thread.
  static bool sEventWatchRegistered = false;
  if (!sEventWatchRegistered) {
    sEventWatchRegistered = true;
    SDL_AddEventWatch(debug_event_watch, nullptr);
  }
  UpdateUiScale();
  if (sToggleRequested.exchange(false, std::memory_order_acq_rel)) {
    Toggle();
  }
  if (const int hotkey = sSaveStateHotkey.exchange(0, std::memory_order_acq_rel);
      hotkey != 0 && sSaveStateHotkeys) {
    if (hotkey == 1) {
      PortSaveState::RequestSave(PortSaveState::SelectedSlot());
    } else {
      PortSaveState::RequestLoad(PortSaveState::SelectedSlot());
    }
  }
  sOverlayVisible.store(sVisible, std::memory_order_release);
  sTwinStickFlag.store(sTwinStick, std::memory_order_release);

  ImGuiIO& io = ImGui::GetIO();
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  // ImGui applies one change per key and frame and holds everything queued
  // behind a second one. Whatever queues changes faster than that stalls the
  // overlay's input until it stops, and then the backlog replays. Nothing
  // should; if something does, take the queue in one go and say so.
  {
    const int queued = ImGui::GetCurrentContext()->InputEventsQueue.Size;
    io.ConfigInputTrickleEventQueue = queued <= 64;
    static uint64_t sLastLogNs = 0;
    const uint64_t now = SDL_GetTicksNS();
    if (queued > 64 && (sLastLogNs == 0 || now - sLastLogNs > 2000000000ull)) {
      sLastLogNs = now;
      PortLog::Write("metroid_prime_port: %d overlay input events were queued; flushed\n", queued);
    }
  }
  // The SDL3 backend calls SDL_ShowCursor on every NewFrame, and the main loop
  // hides the cursor again during play, so it flickered wherever relative
  // mouse mode wasn't hiding it (Android, menus, cutscenes). Let the backend
  // own the cursor only while the overlay is open.
  if (sVisible) {
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
  } else {
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  }

  // This is the only writer of the gamepad keys (Aurora turns the backend's
  // own pad polling off): a second one that disagrees about a key, as the
  // backend did with a stick held as a d-pad or with another pad than the
  // player's, queues two changes a frame where ImGui applies one.
  SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0);
  // While the Controls tab captures a pad input, the pad binds instead of
  // navigating (releasing everything here also ends any nav press in progress).
  // A pad that went away releases everything the same way.
  const bool capturing = pad == nullptr || PortControls::Capturing();
  const auto held = [pad, capturing](SDL_GamepadButton button) {
    return !capturing && SDL_GetGamepadButton(pad, button);
  };
  const auto axis = [pad, capturing](SDL_GamepadAxis a) {
    return capturing ? Sint16{0} : SDL_GetGamepadAxis(pad, a);
  };
  constexpr Sint16 kStickThreshold = 16000;
  io.AddKeyEvent(ImGuiKey_GamepadDpadUp,
                 held(SDL_GAMEPAD_BUTTON_DPAD_UP) || axis(SDL_GAMEPAD_AXIS_LEFTY) < -kStickThreshold);
  io.AddKeyEvent(ImGuiKey_GamepadDpadDown,
                 held(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || axis(SDL_GAMEPAD_AXIS_LEFTY) > kStickThreshold);
  io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,
                 held(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || axis(SDL_GAMEPAD_AXIS_LEFTX) < -kStickThreshold);
  io.AddKeyEvent(ImGuiKey_GamepadDpadRight,
                 held(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || axis(SDL_GAMEPAD_AXIS_LEFTX) > kStickThreshold);
  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, held(SDL_GAMEPAD_BUTTON_SOUTH));
  io.AddKeyEvent(ImGuiKey_GamepadFaceRight, held(SDL_GAMEPAD_BUTTON_EAST));
  io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, held(SDL_GAMEPAD_BUTTON_WEST));
  io.AddKeyEvent(ImGuiKey_GamepadFaceUp, held(SDL_GAMEPAD_BUTTON_NORTH));
  io.AddKeyEvent(ImGuiKey_GamepadL1, held(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
  io.AddKeyEvent(ImGuiKey_GamepadR1, held(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));

  static bool sChordHeld = false;
  const bool chord = held(SDL_GAMEPAD_BUTTON_START) && held(SDL_GAMEPAD_BUTTON_BACK);
  if (chord && !sChordHeld) {
    Toggle();
  }
  sChordHeld = chord;
}

void Toggle() {
  EnsureInitialized();
  sVisible = !sVisible;
  SetMouseCaptured(false);
}

void DrawPerformanceTab() {
  ImGui::Text("Build: %s", MP_BUILD_REVISION);
  bool frameLimit = sFrameLimitEnabled;
  if (ImGui::Checkbox("60 FPS cap (target)", &frameLimit)) {
    SetFrameLimitEnabled(frameLimit);
    MarkDirty();
  }
  // Both, because they answer different questions. Throughput is what the
  // machine produces once the pacing wait is excluded; presented is what
  // reaches the screen. A player seeing stutter wants the second one, and the
  // gap between them is the headroom.
  ImGui::Text("Presented: %.1f FPS", sActualFps);
  ImGui::Text("Throughput: %.1f FPS (headroom at the current frame cost)", sThroughputFps);
  if (sSimAdaptive) {
    ImGui::Text("Measured simulation: %.1f ticks/s (adaptive)", sActualTps);
  } else {
    ImGui::Text("Measured simulation: %.1f ticks/s (target %u)", sActualTps, sSimRate);
  }
  ImGui::Text("Frame time: %.2f ms", static_cast< double >(ImGui::GetIO().DeltaTime) * 1000.0);

  bool interpolate = sFrameInterpolation;
  if (ImGui::Checkbox("Per-frame look (uncapped)", &interpolate)) {
    PortDebug::SetFrameInterpolation(interpolate);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("With the FPS cap off, mouse, gyro and twin-stick look turn the view "
                      "on every frame instead of every 60 Hz tick. Aim and shots are unchanged.");
  }
  bool smoothActors = sActorInterpolation;
  if (ImGui::Checkbox("Smooth actor motion (uncapped, experimental)", &smoothActors)) {
    PortDebug::SetActorInterpolation(smoothActors);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("With the FPS cap off, moving objects, enemies and Samus are drawn between "
                      "their last two 60 Hz positions.");
  }
  bool smoothPoses = sPoseInterpolation;
  if (ImGui::Checkbox("Smooth animation (uncapped, experimental)", &smoothPoses)) {
    PortDebug::SetPoseInterpolation(smoothPoses);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("With the FPS cap off, animated models (Samus, enemies, the arm cannon) are "
                      "posed between their last two 60 Hz poses.");
  }
  bool smoothParticles = sParticleInterpolation;
  if (ImGui::Checkbox("Smooth particles (uncapped, experimental)", &smoothParticles)) {
    PortDebug::SetParticleInterpolation(smoothParticles);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("With the FPS cap off, particle effects and projectile trails are drawn "
                      "between their last two 60 Hz positions.");
  }

  ImGui::Separator();
  ImGui::TextUnformatted("Experimental: simulation rate");
  bool adaptive = sSimAdaptive;
  if (ImGui::Checkbox("Adaptive (follow frame rate)", &adaptive)) {
    PortDebug::SetSimAdaptive(adaptive);
  }
  ImGui::BeginDisabled(adaptive);
  int simRate = static_cast< int >(sSimRate);
  if (ImGui::SliderInt("Sim Hz", &simRate, 30, 480)) {
    PortDebug::SetSimRate(static_cast< unsigned >(simRate));
  }
  ImGui::EndDisabled();
  if (adaptive) {
    ImGui::TextWrapped(
        "One step per frame with dt = the measured frame time (clamped 30-480 Hz), "
        "so a variable frame rate is matched exactly. Leave the FPS cap off.");
  } else if (simRate != 60) {
    ImGui::TextWrapped(
        "60 Hz is console-accurate. Higher values step the game logic at the display "
        "rate instead of interpolating the camera; leave the FPS cap off for it to "
        "matter.");
  }
}

// Memory card transfer (port_gci.h). The work runs on the main thread; the
// file dialogs answer on another one, so their picks wait in sCardPicks.
namespace {
enum CardPick { kCardPick_Import, kCardPick_ExportFolder, kCardPick_ExportFile };
std::mutex sCardPickMutex;
std::vector<std::pair<CardPick, std::string>> sCardPicks;
std::atomic<bool> sCardDialogOpen{false};
std::string sCardStatus;
// Android has no folder dialog: each file gets its own save dialog, in turn.
std::vector<std::filesystem::path> sCardExportQueue;

// The last path segment, for messages; content:// URIs keep theirs escaped.
std::string CardDisplayName(const std::string& path) {
  std::string name = path.substr(path.find_last_of("/\\") + 1);
  const size_t escaped = name.rfind("%2F");
  return escaped == std::string::npos ? name : name.substr(escaped + 3);
}

std::string CardImportTarget(std::filesystem::path& folder) {
  if (sStateManager != nullptr) {
    return "Return to the title screen to import: saving the game in progress would "
           "overwrite the imported save.";
  }
  folder = PortGci::MountedCardFolder();
  if (folder.empty()) {
    return "The memory card is not a GCI folder, so there is nowhere to import to.";
  }
  return {};
}

std::string CardExportSource(std::filesystem::path& folder) {
  folder = PortGci::MountedCardFolder();
  if (folder.empty()) {
    return "The memory card is not a GCI folder.";
  }
  if (PortGci::GameFiles(folder).empty()) {
    return "There are no saves on the memory card yet.";
  }
  return {};
}

std::string FinishCardImport(const PortGci::Report& report) {
  if (report.copied > 0) {
    // The save screen remounts the card and reads it again once it is idle.
    PortGci::MarkCardChanged();
  }
  return report.Summary("Imported");
}
} // namespace

std::string CardList() {
  const std::filesystem::path folder = PortGci::MountedCardFolder();
  if (folder.empty()) {
    return "The memory card is not a GCI folder.";
  }
  std::string text = "card: " + PortGci::PathString(folder);
  for (const std::filesystem::path& file : PortGci::GameFiles(folder)) {
    std::error_code ec;
    text += "\n  " + PortGci::PathString(file.filename()) + " (" +
            std::to_string(std::filesystem::file_size(file, ec)) + " bytes)";
  }
  const PortGci::DolphinCard dolphin = PortGci::FindDolphinCard();
  if (!dolphin.gciFolder.empty()) {
    text += "\ndolphin folder: " + PortGci::PathString(dolphin.gciFolder);
  }
  if (!dolphin.rawImage.empty()) {
    text += "\ndolphin raw: " + PortGci::PathString(dolphin.rawImage);
  }
  return text;
}

std::string CardImport(const std::string& path) {
  std::filesystem::path folder;
  const std::string refusal = CardImportTarget(folder);
  if (!refusal.empty()) {
    return refusal;
  }
  std::error_code ec;
  if (std::filesystem::is_directory(PortGci::PathFromString(path), ec)) {
    return FinishCardImport(PortGci::ImportFolder(PortGci::PathFromString(path), folder));
  }
  // SDL reads content:// URIs from the Android picker as well as paths.
  size_t size = 0;
  void* data = SDL_LoadFile(path.c_str(), &size);
  if (data == nullptr) {
    return "Could not read " + CardDisplayName(path) + ": " + SDL_GetError();
  }
  const uint8_t* bytes = static_cast< const uint8_t* >(data);
  const std::vector<uint8_t> contents(bytes, bytes + size);
  SDL_free(data);
  return FinishCardImport(
      PortGci::ImportBytes(contents, CardDisplayName(path), folder, folder.parent_path()));
}

std::string CardExport(const std::string& dest) {
  std::filesystem::path folder;
  const std::string refusal = CardExportSource(folder);
  if (!refusal.empty()) {
    return refusal;
  }
  const std::filesystem::path target = PortGci::PathFromString(dest);
  if (target.extension() == ".raw") {
    return PortGci::ExportRaw(folder, target).Summary("Exported");
  }
  return PortGci::ExportFolder(folder, target).Summary("Exported");
}

std::string CardImportDolphin() {
  std::filesystem::path folder;
  const std::string refusal = CardImportTarget(folder);
  if (!refusal.empty()) {
    return refusal;
  }
  const PortGci::DolphinCard dolphin = PortGci::FindDolphinCard();
  if (!dolphin.Found()) {
    return "No Dolphin memory card found (GC/USA/Card A or GC/MemoryCardA.USA.raw in "
           "Dolphin's user folder).";
  }
  // Dolphin uses one or the other, per its settings: try the one written last.
  std::error_code ec;
  std::filesystem::file_time_type folderTime = std::filesystem::file_time_type::min();
  for (const std::filesystem::path& file : PortGci::GameFiles(dolphin.gciFolder)) {
    folderTime = std::max(folderTime, std::filesystem::last_write_time(file, ec));
  }
  const auto rawTime = dolphin.rawImage.empty() ? std::filesystem::file_time_type::min()
                                                : std::filesystem::last_write_time(dolphin.rawImage, ec);
  std::vector<std::filesystem::path> sources;
  if (!dolphin.gciFolder.empty() && folderTime != std::filesystem::file_time_type::min()) {
    sources.push_back(dolphin.gciFolder);
  }
  if (!dolphin.rawImage.empty()) {
    sources.insert(rawTime > folderTime ? sources.begin() : sources.end(), dolphin.rawImage);
  }
  for (const std::filesystem::path& source : sources) {
    const PortGci::Report report = source == dolphin.gciFolder
                                       ? PortGci::ImportFolder(source, folder)
                                       : PortGci::ImportFile(source, folder, folder.parent_path());
    if (report.copied > 0 || !report.errors.empty()) {
      return "From " + PortGci::PathString(source) + ": " + FinishCardImport(report);
    }
  }
  return "Dolphin's memory card holds no Metroid Prime saves.";
}

std::string CardExportDolphin() {
  std::filesystem::path folder;
  const std::string refusal = CardExportSource(folder);
  if (!refusal.empty()) {
    return refusal;
  }
  const PortGci::DolphinCard dolphin = PortGci::FindDolphinCard();
  if (!dolphin.Found()) {
    return "No Dolphin memory card found (GC/USA/Card A or GC/MemoryCardA.USA.raw in "
           "Dolphin's user folder); start a GameCube game in Dolphin once to create it.";
  }
  std::string text;
  if (!dolphin.gciFolder.empty()) {
    text = "To " + PortGci::PathString(dolphin.gciFolder) + ": " +
           PortGci::ExportFolder(folder, dolphin.gciFolder).Summary("Exported");
  }
  if (!dolphin.rawImage.empty()) {
    text += std::string(text.empty() ? "" : "\n") + "To " +
            PortGci::PathString(dolphin.rawImage) + ": " +
            PortGci::ExportRaw(folder, dolphin.rawImage).Summary("Exported");
  }
  return text;
}

namespace {
void OpenCardDialog(CardPick pick) {
  int windowCount = 0;
  SDL_Window** windows = SDL_GetWindows(&windowCount);
  SDL_Window* window = windows != nullptr && windowCount > 0 ? windows[0] : nullptr;
  SDL_free(windows);
  const SDL_DialogFileCallback done = [](void* userdata, const char* const* files, int) {
    std::lock_guard lock(sCardPickMutex);
    const CardPick kind = static_cast< CardPick >(reinterpret_cast< intptr_t >(userdata));
    // An empty path means cancelled (or a failed dialog, with the error set).
    sCardPicks.emplace_back(kind, files != nullptr && files[0] != nullptr ? files[0] : "");
    if (files == nullptr) {
      sCardPicks.back().second = std::string("\x01") + SDL_GetError();
    }
    sCardDialogOpen = false;
  };
  void* userdata = reinterpret_cast< void* >(static_cast< intptr_t >(pick));
  sCardDialogOpen = true;
  switch (pick) {
  case kCardPick_Import: {
#if defined(__ANDROID__)
    // Android turns filters into MIME types, and .gci has none.
    SDL_ShowOpenFileDialog(done, userdata, window, nullptr, 0, nullptr, false);
#else
    static const SDL_DialogFileFilter filters[] = {
        {"GameCube saves (.gci, card images)", "gci;raw;mcp;sav"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(done, userdata, window, filters, 2, nullptr, false);
#endif
    break;
  }
  case kCardPick_ExportFolder:
    SDL_ShowOpenFolderDialog(done, userdata, window, nullptr, false);
    break;
  case kCardPick_ExportFile: {
    static std::string location;
    location = sCardExportQueue.empty()
                   ? std::string()
                   : PortGci::PathString(sCardExportQueue.front().filename());
    SDL_ShowSaveFileDialog(done, userdata, window, nullptr, 0,
                           location.empty() ? nullptr : location.c_str());
    break;
  }
  }
}

std::string CardSaveTo(const std::filesystem::path& source, const std::string& target) {
  std::ifstream in(source, std::ios::binary);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  SDL_IOStream* out = in ? SDL_IOFromFile(target.c_str(), "wb") : nullptr;
  bool ok = out != nullptr && SDL_WriteIO(out, bytes.data(), bytes.size()) == bytes.size();
  if (out != nullptr) {
    ok = SDL_CloseIO(out) && ok;
  }
  return ok ? "Saved " + PortGci::PathString(source.filename()) + " as " + CardDisplayName(target) + "."
            : "Could not save " + PortGci::PathString(source.filename()) + ": " + SDL_GetError();
}

// Runs every frame, overlay open or not, so a dialog's answer is acted on.
void ProcessCardPicks() {
  std::vector<std::pair<CardPick, std::string>> picks;
  {
    std::lock_guard lock(sCardPickMutex);
    picks.swap(sCardPicks);
  }
  for (const auto& [kind, path] : picks) {
    if (path.empty() || path[0] == '\x01') {
      if (path.size() > 1) {
        sCardStatus = "The file dialog failed: " + path.substr(1);
      }
      sCardExportQueue.clear();
      continue;
    }
    switch (kind) {
    case kCardPick_Import:
      sCardStatus = CardImport(path);
      break;
    case kCardPick_ExportFolder:
      sCardStatus = CardExport(path);
      break;
    case kCardPick_ExportFile:
      if (!sCardExportQueue.empty()) {
        sCardStatus = CardSaveTo(sCardExportQueue.front(), path);
        sCardExportQueue.erase(sCardExportQueue.begin());
        if (!sCardExportQueue.empty()) {
          OpenCardDialog(kCardPick_ExportFile);
        }
      }
      break;
    }
  }
}

void DrawMemoryCard() {
  ImGui::SeparatorText("Memory card");
  const std::filesystem::path folder = PortGci::MountedCardFolder();
  if (folder.empty()) {
    ImGui::TextDisabled("The card is not a GCI folder; nothing to import to or export.");
    return;
  }
  const size_t saves = PortGci::GameFiles(folder).size();
  ImGui::TextWrapped("Card: %s (%zu save file%s)", PortGci::PathString(folder).c_str(), saves,
                     saves == 1 ? "" : "s");
  const bool inGame = sStateManager != nullptr;
  const bool busy = sCardDialogOpen;
  ImGui::BeginDisabled(inGame || busy);
  if (ImGui::Button("Import file...")) {
    OpenCardDialog(kCardPick_Import);
  }
#if !defined(__ANDROID__)
  ImGui::SameLine();
  if (ImGui::Button("Import from Dolphin")) {
    sCardStatus = CardImportDolphin();
  }
#endif
  ImGui::EndDisabled();
  ImGui::BeginDisabled(busy || saves == 0);
#if defined(__ANDROID__)
  if (ImGui::Button("Export...")) {
    sCardExportQueue = PortGci::GameFiles(folder);
    OpenCardDialog(kCardPick_ExportFile);
  }
#else
  if (ImGui::Button("Export to folder...")) {
    OpenCardDialog(kCardPick_ExportFolder);
  }
  ImGui::SameLine();
  if (ImGui::Button("Export to Dolphin")) {
    sCardStatus = CardExportDolphin();
  }
#endif
  ImGui::EndDisabled();
#if !defined(__ANDROID__)
  ImGui::SameLine();
  if (ImGui::Button("Open card folder")) {
    // "Card A" has a space, which a URL can't carry as is.
    const std::string path = PortGci::PathString(folder);
    std::string url = path.front() == '/' ? "file://" : "file:///"; // C:\ on Windows
    for (const char c : path) {
      url += c == ' ' ? std::string("%20") : std::string(1, c == '\\' ? '/' : c);
    }
    SDL_OpenURL(url.c_str());
  }
#endif
  if (inGame) {
    ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "Return to the title screen to import.");
  }
  if (!sCardStatus.empty()) {
    ImGui::TextWrapped("%s", sCardStatus.c_str());
  }
  ImGui::TextWrapped(
#if defined(__ANDROID__)
      "Imports Dolphin .gci saves or a whole card image (.raw). Export saves each file "
      "in turn; keep Dolphin's names (01-GM8E-MetroidPrime A.gci) for its GCI folder. "
#else
      "Imports Dolphin .gci saves or a whole card image (.raw). Dolphin's card is looked "
      "for in its user folder (GC/USA/Card A, GC/MemoryCardA.USA.raw); close Dolphin "
      "before exporting to it, and a raw card is backed up to .raw.bak first. "
#endif
      "An import replaces the card's saves; the old ones move to _replaced in the card "
      "folder.");
}

// The Remastered import (port_remastered_import.h): the user's own image and
// key file, converted here into the remastered-models mod.
std::mutex sRemasteredPickMutex;
std::vector<std::pair<int, std::string>> sRemasteredPicks;

#if defined(__ANDROID__)
// Not SDL_ShowOpenFileDialog: Android often kills the game behind the picker
// for its memory (seen on a tablet: the pick came back to a new process), and
// SDL's callback dies with it. MetroidPrimeActivity.pickRemasteredFile writes
// the picked address to this file, which the panel reads, in this process or
// the next one.
std::string RemasteredPickFile(int which) {
  const char* root = SDL_GetAndroidInternalStoragePath();
  return std::string(root != nullptr ? root : ".") + "/remastered_pick_" + std::to_string(which) + ".txt";
}

void TakeRemasteredPickFiles() {
  for (int which = 0; which < 2; ++which) {
    std::ifstream in(RemasteredPickFile(which));
    std::string uri;
    if (std::getline(in, uri) && !uri.empty()) {
      sRemasteredPicks.emplace_back(which, uri);
    }
  }
}

void OpenRemasteredDialog(int which) {
  JNIEnv* env = static_cast< JNIEnv* >(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast< jobject >(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  jmethodID method = env->GetMethodID(cls, "pickRemasteredFile", "(I)V");
  if (method != nullptr) {
    env->CallVoidMethod(activity, method, jint(which));
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
}
#else
void OpenRemasteredDialog(int which) {
  int windowCount = 0;
  SDL_Window** windows = SDL_GetWindows(&windowCount);
  SDL_Window* window = windows != nullptr && windowCount > 0 ? windows[0] : nullptr;
  SDL_free(windows);
  const SDL_DialogFileCallback done = [](void* userdata, const char* const* files, int) {
    if (files != nullptr && files[0] != nullptr) {
      std::lock_guard lock(sRemasteredPickMutex);
      sRemasteredPicks.emplace_back(int(reinterpret_cast< intptr_t >(userdata)), files[0]);
    }
  };
  static const SDL_DialogFileFilter imageFilters[] = {{"Switch images (.nsp)", "nsp"}, {"All files", "*"}};
  static const SDL_DialogFileFilter keyFilters[] = {{"Key files (.keys)", "keys"}, {"All files", "*"}};
  SDL_ShowOpenFileDialog(done, reinterpret_cast< void* >(static_cast< intptr_t >(which)), window,
                         which == 0 ? imageFilters : keyFilters, 2, nullptr, false);
}
#endif

#if defined(__ANDROID__)
// Android's picker gives a content:// address, which only the system can open.
// The image is several GB, so it is not copied as the disc is: the file is
// opened here and the import reads it through the descriptor ("fd:<n>", see
// SourceFile). One descriptor per field, closed when another file is picked.
std::string OpenRemasteredPick(int which, const std::string& uri) {
  static int sHeld[2] = {-1, -1};
  if (sHeld[which] >= 0) {
    close(sHeld[which]);
    sHeld[which] = -1;
  }
  SDL_IOStream* io = SDL_IOFromFile(uri.c_str(), "rb");
  if (io == nullptr) {
    PortLog::Write("metroid_prime_port: could not open the picked file: %s: %s\n", uri.c_str(), SDL_GetError());
    return {};
  }
  const int fd = int(SDL_GetNumberProperty(SDL_GetIOProperties(io), SDL_PROP_IOSTREAM_FILE_DESCRIPTOR_NUMBER, -1));
  sHeld[which] = fd >= 0 ? dup(fd) : -1;
  SDL_CloseIO(io);
  return sHeld[which] >= 0 ? "fd:" + std::to_string(sHeld[which]) : std::string();
}

// "content://.../document/primary%3ADownload%2Fgame.nsp" -> "game.nsp".
std::string RemasteredPickName(const std::string& uri) {
  std::string text;
  for (size_t i = 0; i < uri.size(); ++i) {
    if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit(static_cast< unsigned char >(uri[i + 1])) &&
        std::isxdigit(static_cast< unsigned char >(uri[i + 2]))) {
      text += char(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      text += uri[i];
    }
  }
  const size_t cut = text.find_last_of("/:");
  return cut == std::string::npos ? text : text.substr(cut + 1);
}
#endif

// What the last "Reload mods" came to: it waits for a game to start when none
// is loaded, and is refused where the game can't be saved.
void DrawModReloadMessage() {
  const std::string message = PortSaveState::LastMessage();
  if (message.find("mods") != std::string::npos || message.find("Mods") != std::string::npos) {
    ImGui::TextWrapped("%s", message.c_str());
  }
}

void DrawRemasteredImport() {
  static char sImage[1024] = "";
  static char sKeys[1024] = "";
#if defined(__ANDROID__)
  static std::string sPickNames[2];
#endif
  static bool sFilled = false;
  const auto remember = [](int which, const std::string& path) {
    std::string& saved = which == 0 ? sRemasteredImagePath : sRemasteredKeysPath;
    if (!path.empty() && saved != path) {
      saved = path;
      MarkDirty();
    }
  };
  if (!sFilled) {
    sFilled = true;
    std::snprintf(sKeys, sizeof(sKeys), "%s", PortRemastered::DefaultKeysPath().c_str());
#if defined(__ANDROID__)
    // The files picked last time open again as if picked now.
    std::lock_guard lock(sRemasteredPickMutex);
    if (!sRemasteredImagePath.empty()) {
      sRemasteredPicks.emplace_back(0, sRemasteredImagePath);
    }
    if (!sRemasteredKeysPath.empty()) {
      sRemasteredPicks.emplace_back(1, sRemasteredKeysPath);
    }
    // A pick the previous process never saw: after the remembered ones, so it wins.
    TakeRemasteredPickFiles();
#else
    if (!sRemasteredImagePath.empty()) {
      std::snprintf(sImage, sizeof(sImage), "%s", sRemasteredImagePath.c_str());
    }
    if (!sRemasteredKeysPath.empty()) {
      std::snprintf(sKeys, sizeof(sKeys), "%s", sRemasteredKeysPath.c_str());
    }
#endif
  }
  {
    std::lock_guard lock(sRemasteredPickMutex);
#if defined(__ANDROID__)
    for (const auto& [which, path] : sRemasteredPicks) {
      const std::string opened = OpenRemasteredPick(which, path);
      std::snprintf(which == 0 ? sImage : sKeys, sizeof(sImage), "%s", opened.c_str());
      sPickNames[which] = opened.empty() ? "could not be opened" : RemasteredPickName(path);
      if (!opened.empty()) {
        remember(which, path);
      }
      std::remove(RemasteredPickFile(which).c_str());
    }
#else
    for (const auto& [which, path] : sRemasteredPicks) {
      std::snprintf(which == 0 ? sImage : sKeys, sizeof(sImage), "%s", path.c_str());
      remember(which, path);
    }
#endif
    sRemasteredPicks.clear();
  }
  if (!ImGui::CollapsingHeader("Metroid Prime Remastered models")) {
    return;
  }
  const PortRemastered::ImportState state = PortRemastered::ImportStatus();
  ImGui::TextWrapped("Very experimental and currently unsupported: expect wrong or missing models, crashes and "
                     "heavy memory use. Remove mods/remastered-models to get the retail game back.");
  ImGui::TextWrapped("Converts the models of your own copy of Metroid Prime Remastered into a mod. It needs the "
                     "game's .nsp and your console's key file (prod.keys), and takes a few minutes.");
  ImGui::BeginDisabled(state.running);
#if defined(__ANDROID__)
  // No path to type here: the files are picked, and shown by name.
  if (ImGui::Button("Pick the .nsp...##remastered-image")) {
    OpenRemasteredDialog(0);
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(sPickNames[0].empty() ? "Metroid Prime Remastered .nsp" : sPickNames[0].c_str());
  if (ImGui::Button("Pick the keys...##remastered-keys")) {
    OpenRemasteredDialog(1);
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(sPickNames[1].empty() ? "prod.keys" : sPickNames[1].c_str());
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("Android may close the game while you pick a file. The pick is kept: open this page again.");
  ImGui::PopStyleColor();
#else
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
  ImGui::InputTextWithHint("##remastered-image", "Metroid Prime Remastered .nsp", sImage, sizeof(sImage));
  ImGui::SameLine();
  if (ImGui::Button("Browse...##remastered-image")) {
    OpenRemasteredDialog(0);
  }
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
  ImGui::InputTextWithHint("##remastered-keys", "prod.keys", sKeys, sizeof(sKeys));
  ImGui::SameLine();
  if (ImGui::Button("Browse...##remastered-keys")) {
    OpenRemasteredDialog(1);
  }
#endif
#if defined(__ANDROID__)
  // Off on a phone: the rooms have never run on one, and need storage and
  // memory many phones lack (a 256 MB game arena and 12x frame buffers).
  static bool sGeometry = false;
#else
  static bool sGeometry = true;
#endif
  ImGui::Checkbox("Room geometry too##remastered", &sGeometry);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Also converts the rooms themselves, not only the models in them. About 6.5 GB in place of "
                      "1 GB, and twice as long. Restart the game afterwards.");
  }
#if defined(__ANDROID__)
  // A tap shows no tooltip, so the warning is spelled out.
  if (sGeometry) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.75f, 0.3f, 1.f));
    ImGui::TextWrapped("Untested on phones: needs about 6.5 GB free and lots of RAM, and the game may run slowly "
                       "or be closed by Android. Remove mods/remastered-models to go back.");
    ImGui::PopStyleColor();
  }
#endif
  PortRemastered::SetImportGeometry(sGeometry);
  ImGui::EndDisabled();
  if (state.running) {
    ImGui::ProgressBar(state.total > 0 ? float(state.done) / float(state.total) : 0.f, ImVec2(-1.f, 0.f),
                       state.message.c_str());
    if (ImGui::Button("Cancel##remastered")) {
      PortRemastered::CancelImport();
    }
  } else {
    ImGui::BeginDisabled(sImage[0] == '\0' || sKeys[0] == '\0');
#if !defined(__ANDROID__)
    // A typed path is kept once it is used (Android keeps what was picked).
    const auto rememberTyped = [&] {
      remember(0, sImage);
      remember(1, sKeys);
    };
#else
    const auto rememberTyped = [] {};
#endif
    if (ImGui::Button("Import##remastered")) {
      rememberTyped();
#if defined(__ANDROID__)
      // Each worker holds a world's models while it converts them; a phone has
      // the memory for two of those, not for one per core.
      PortRemastered::StartImport(sImage, sKeys, 2);
#else
      PortRemastered::StartImport(sImage, sKeys);
#endif
    }
    ImGui::SameLine();
    if (ImGui::Button("Import movies##remastered")) {
      rememberTyped();
      PortRemastered::StartMovieImport(sImage, sKeys);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
#if defined(__ANDROID__)
      ImGui::SetTooltip("Only the menu movies, added to the mod already imported.");
#else
      ImGui::SetTooltip("Only the menu movies, added to the mod already imported. Needs ffmpeg.");
#endif
    }
    ImGui::EndDisabled();
    if (state.finished && state.ok) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 1.f, 0.5f, 1.f));
      ImGui::TextWrapped("%s", state.message.c_str());
      ImGui::PopStyleColor();
      if (ImGui::Button("Load it now##remastered")) {
        PortSaveState::RequestModReload();
      }
      ImGui::SameLine();
      ImGui::TextDisabled("or restart the game");
      DrawModReloadMessage();
    } else if (state.finished && state.cancelled) {
      ImGui::TextDisabled("The import was cancelled.");
    } else if (state.finished) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.5f, 0.3f, 1.f));
      ImGui::TextWrapped("The import failed: %s", state.message.c_str());
      ImGui::PopStyleColor();
    }
  }
  if (!state.lines.empty() && ImGui::TreeNode("remastered-log", "%d notes", int(state.lines.size()))) {
    for (const std::string& line : state.lines) {
      ImGui::TextWrapped("%s", line.c_str());
    }
    ImGui::TreePop();
  }
}

#if !defined(__ANDROID__)
// Importers (port_importers.h): the user's own programs that build a mod.
// Nothing is drawn until the importers folder holds one.
void DrawImporters() {
  // The folder is read when the panel opens and after a run, not every frame.
  static std::vector<std::string> sNames;
  static int sListedFrame = -2;
  static char sArgument[512] = "";
  const int frame = ImGui::GetFrameCount();
  const PortImporters::State& state = PortImporters::Poll();
  if (sListedFrame != frame - 1 || ImGui::IsWindowAppearing()) {
    sNames = PortImporters::List();
  }
  sListedFrame = frame;
  if (sNames.empty() && !state.running && !state.finished) {
    return;
  }
  ImGui::SeparatorText("Importers");
  ImGui::BeginDisabled(state.running);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
  ImGui::InputTextWithHint("Argument", "optional, e.g. the file to import", sArgument, sizeof(sArgument));
  for (const std::string& name : sNames) {
    if (ImGui::Button(("Run " + name).c_str())) {
      PortImporters::Start(name, sArgument);
    }
  }
  ImGui::EndDisabled();
  if (state.running) {
    ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "%s is running...", state.name.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      PortImporters::Cancel();
    }
  } else if (state.finished && state.exitCode == 0) {
    ImGui::TextColored(ImVec4(0.5f, 1.f, 0.5f, 1.f), "%s finished. Reload the mods to load it.",
                       state.name.c_str());
  } else if (state.finished && state.cancelled) {
    ImGui::TextDisabled("%s was cancelled.", state.name.c_str());
  } else if (state.finished) {
    ImGui::TextColored(ImVec4(1.f, 0.5f, 0.3f, 1.f), "%s failed (exit code %d).", state.name.c_str(), state.exitCode);
  }
  if (!state.lines.empty()) {
    // The tail of the output; a failure shows more of it.
    const size_t shown = std::min<size_t>(state.lines.size(), state.finished && state.exitCode != 0 && !state.cancelled ? 12 : 3);
    for (size_t i = state.lines.size() - shown; i < state.lines.size(); ++i) {
      ImGui::TextWrapped("%s", state.lines[i].c_str());
    }
  }
  ImGui::TextWrapped("An importer is a program in the importers folder beside the mods folder; it builds a "
                     "mod from files of your own.");
}
#endif

void DrawMods() {
  ImGui::SeparatorText("Mods");
  const PortMods::Status& status = PortMods::CurrentStatus();
  bool enabled = sModsEnabled;
  if (ImGui::Checkbox("Load mods", &enabled)) {
    SetModsEnabled(enabled);
  }
  // What the settings would load next time, against what this launch loaded.
  std::vector<std::string> disabled = PortMods::SplitDisabled(sModsDisabled);
  bool changed = sModsEnabled != status.active;
  if (status.mods.empty()) {
    ImGui::TextDisabled("No mods in the folder.");
  }
  for (const PortMods::ModInfo& mod : status.mods) {
    const auto found = std::find(disabled.begin(), disabled.end(), mod.name);
    bool on = found == disabled.end();
    ImGui::BeginDisabled(!sModsEnabled);
    if (ImGui::Checkbox(mod.name.c_str(), &on)) {
      if (on) {
        disabled.erase(std::remove(disabled.begin(), disabled.end(), mod.name), disabled.end());
      } else {
        disabled.push_back(mod.name);
      }
      SetModsDisabled(PortMods::JoinDisabled(disabled));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (mod.enabled) {
      if (mod.textures > 0) {
        ImGui::TextDisabled("%d file(s), %d resource(s), %d native texture(s)", mod.files, mod.resources, mod.textures);
      } else {
        ImGui::TextDisabled("%d file(s), %d resource(s)", mod.files, mod.resources);
      }
    } else {
      ImGui::TextDisabled("not loaded");
    }
    changed = changed || (sModsEnabled && on) != mod.enabled;
  }
  if (changed) {
    ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "Reload the mods to apply.");
  }
  if (ImGui::Button("Reload mods")) {
    PortSaveState::RequestModReload();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Reads the mods folder again and reloads the room, as a save state does.");
  }
  DrawModReloadMessage();
  for (const std::string& message : status.messages) {
    ImGui::TextColored(ImVec4(1.f, 0.5f, 0.3f, 1.f), "%s", message.c_str());
  }
  if (PortMods::NativeTextureCount() > 0) {
    ImGui::TextDisabled("Native textures: %zu, %zu in use", PortMods::NativeTextureCount(), PortMods::NativeTexturesBound());
  }
  ImGui::TextWrapped("Folder: %s", status.folder.c_str());
#if !defined(__ANDROID__)
  if (!status.folder.empty() && ImGui::Button("Open mods folder")) {
    std::string url = status.folder.front() == '/' ? "file://" : "file:///";
    for (const char c : status.folder) {
      url += c == ' ' ? std::string("%20") : std::string(1, c == '\\' ? '/' : c);
    }
    SDL_OpenURL(url.c_str());
  }
#endif
  ImGui::TextWrapped(
      "Each folder in the mods folder is a mod; later names win. A file at a disc path "
      "(Metroid1.pak, Audio/..., Video/...) replaces that file, and a resource named "
      "by id and type (1A2B3C4D.TXTR) replaces it in every PAK. Mods load at startup.");
  DrawRemasteredImport();
#if !defined(__ANDROID__)
  DrawImporters();
#endif
}
} // namespace

void DrawExtrasTab() {
  ImGui::SeparatorText("Cutscenes");
  bool skippable = sSkippableCutscenes;
  if (ImGui::Checkbox("Skippable cutscenes", &skippable)) {
    SetSkippableCutscenes(skippable);
  }
  if (PortSkipCutscenes::Forced()) {
    ImGui::SameLine();
    ImGui::TextDisabled("(on in randomized games)");
  }
  ImGui::TextWrapped(
      "Every cutscene can be skipped with the usual button, including the ones the "
      "game never lets you skip (randomprime's room patches). Applies to rooms "
      "loaded after the change.");

  ImGui::SeparatorText("Unlocks");
  bool hardMode = sUnlockHardMode;
  if (ImGui::Checkbox("Hard mode", &hardMode)) {
    SetUnlockHardMode(hardMode);
  }
  bool fusionSuit = sUnlockFusionSuit;
  if (ImGui::Checkbox("Fusion Suit", &fusionSuit)) {
    SetUnlockFusionSuit(fusionSuit);
    // The suit choice itself is saved; without the unlock (or a real link)
    // there is no menu left to switch it back off, so drop it here.
    if (!fusionSuit && gpGameState != nullptr &&
        !(gpGameState->SystemState().GetFusionLinked() &&
          gpGameState->SystemState().GetNormalModeBeat())) {
      gpGameState->SystemState().SetHasFusion(false);
      gpGameState->PlayerState()->SetIsFusionEnabled(false);
    }
  }
  bool galleries = sUnlockGalleries;
  if (ImGui::Checkbox("Image galleries", &galleries)) {
    SetUnlockGalleries(galleries);
  }
  ImGui::TextWrapped(
      "Offers what finishing the game normally unlocks: hard mode when starting a "
      "file, the Fusion Suit under Fusion Bonus (retail needs a GBA link to "
      "Metroid Fusion), and all four image galleries. Nothing is written into the "
      "save, so turning an option off locks it again. Metroid (NES) stays locked: "
      "its emulator can't run in the port.");

  ImGui::SeparatorText("Speedrun");
  bool timer = sSpeedrunTimer;
  if (ImGui::Checkbox("On-screen in-game time", &timer)) {
    SetSpeedrunTimer(timer);
  }
  bool liveSplit = sLiveSplit;
  if (ImGui::Checkbox("LiveSplit", &liveSplit)) {
    SetLiveSplit(liveSplit);
  }
  ImGui::SameLine();
  switch (PortLiveSplit::Status()) {
  case PortLiveSplit::kStatus_Off:
    ImGui::TextDisabled("off");
    break;
  case PortLiveSplit::kStatus_Connecting:
    ImGui::TextUnformatted("connecting...");
    break;
  case PortLiveSplit::kStatus_Connected:
    ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "connected");
    break;
  case PortLiveSplit::kStatus_Failed:
    ImGui::TextColored(ImVec4(1.f, 0.5f, 0.3f, 1.f), "%s", PortLiveSplit::LastError().c_str());
    break;
  }
  // Applied when the field loses focus; until then the text is left alone.
  static char address[128];
  static bool editingAddress = false;
  if (!editingAddress) {
    std::snprintf(address, sizeof(address), "%s", sLiveSplitAddress.c_str());
  }
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.f);
  ImGui::InputText("Server (host:port)", address, sizeof(address));
  editingAddress = ImGui::IsItemActive();
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    SetLiveSplitAddress(address);
  }
  bool splitUpgrades = sLiveSplitSplitUpgrades;
  if (ImGui::Checkbox("Split on upgrades", &splitUpgrades)) {
    sLiveSplitSplitUpgrades = splitUpgrades;
    ApplyLiveSplit();
    MarkDirty();
  }
  ImGui::TextWrapped(
      "The in-game time is the play time the save file shows: it stops in cutscenes, "
      "menus and loads. LiveSplit: right-click it, Control > Start TCP Server (port "
      "16834), and compare against Game Time. A new file resets and starts the timer; "
      "the game time follows the in-game time; it splits on each new upgrade or "
      "artifact (not expansions or energy tanks) when enabled, and on the final blow.");

  if (PortDiscord::Supported()) {
    ImGui::SeparatorText("Discord");
    bool discord = sDiscord;
    if (ImGui::Checkbox("Rich Presence", &discord)) {
      SetDiscordPresence(discord);
    }
    ImGui::SameLine();
    switch (PortDiscord::Status()) {
    case PortDiscord::kStatus_Off:
      ImGui::TextDisabled("off");
      break;
    case PortDiscord::kStatus_Connecting:
      ImGui::TextUnformatted("connecting...");
      break;
    case PortDiscord::kStatus_Connected:
      ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "connected");
      break;
    case PortDiscord::kStatus_Failed:
      ImGui::TextColored(ImVec4(1.f, 0.5f, 0.3f, 1.f), "%s", PortDiscord::LastError().c_str());
      break;
    }
    static char appId[64];
    static bool editingAppId = false;
    if (!editingAppId) {
      std::snprintf(appId, sizeof(appId), "%s", sDiscordAppId.c_str());
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.f);
    ImGui::InputText("Application id", appId, sizeof(appId));
    editingAppId = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      SetDiscordAppId(appId);
    }
    if (sDiscord) {
      ImGui::TextDisabled("Showing: %s", PortDiscord::CurrentText().c_str());
    }
    ImGui::TextWrapped(
        "Shows the world, room and item percentage on your Discord profile while the "
        "Discord app runs. It needs a Discord application: create one at "
        "discord.com/developers/applications (its name is what Discord shows as the "
        "game), add an art asset named \"logo\" under Rich Presence, and paste its "
        "Application ID here.");
  }

#if defined(__ANDROID__)
  PortDataFolder::DrawPanel();
#endif
  DrawMemoryCard();
  DrawMods();
}

#if defined(__ANDROID__)
// The folder picker's progress, set from the Java copy thread.
std::mutex sTexturePackStatusMutex;
std::string sTexturePackStatus;

std::string TexturePackStatus() {
  std::lock_guard lock(sTexturePackStatusMutex);
  return sTexturePackStatus;
}

void SetTexturePackStatus(std::string status) {
  std::lock_guard lock(sTexturePackStatusMutex);
  sTexturePackStatus = std::move(status);
}

// MetroidPrimeActivity.pickTexturePack opens the system folder picker and
// copies the chosen folder in the background, reporting back through the
// nativeTexturePack* functions below.
void PickTexturePack() {
  JNIEnv* env = static_cast< JNIEnv* >(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast< jobject >(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    SetTexturePackStatus("Could not open the folder picker.");
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  jmethodID method = env->GetMethodID(cls, "pickTexturePack", "(Ljava/lang/String;)V");
  if (method != nullptr) {
    // The pack is copied into the data folder, wherever that is.
    jstring folder = env->NewStringUTF(PortPaths::UserFolder().c_str());
    env->CallVoidMethod(activity, method, folder);
    env->DeleteLocalRef(folder);
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    SetTexturePackStatus("Could not open the folder picker.");
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
}
#endif

void DrawTexturePack();

void DrawRenderTab() {
#if !defined(__ANDROID__)
  if (ImGui::Checkbox("Overlay as a floating window", &sOverlayWindowed)) {
    MarkDirty();
  }
#endif
  bool fullscreen = sFullscreen;
#if defined(__ANDROID__)
  if (ImGui::Checkbox("Fullscreen (hide the status and navigation bars)", &fullscreen)) {
#else
  if (ImGui::Checkbox("Fullscreen (F11)", &fullscreen)) {
#endif
    SetFullscreen(fullscreen);
  }
  bool vsync = sVsyncEnabled;
  if (ImGui::Checkbox("Vsync", &vsync)) {
    SetVsyncEnabled(vsync);
    MarkDirty();
  }

  int aspect = static_cast< int >(sAspectMode);
  if (ImGui::Combo("Aspect ratio", &aspect, "4:3\0" "16:9\0" "Follow window\0")) {
    SetAspectMode(static_cast< EAspectMode >(aspect));
    MarkDirty();
  }

  bool hudWide = sHudWide;
  if (ImGui::Checkbox("Widescreen HUD (spread to edges)", &hudWide)) {
    SetHudWide(hudWide);
    MarkDirty();
  }
  ImGui::TextWrapped(
      "Keeps each HUD element's shape but spreads its position so edge elements "
      "reach the wide corners. Only affects the in-game HUD, not menus.");

  int hudScale = sHudScale;
  if (ImGui::SliderInt("HUD scale", &hudScale, kHudScaleMin, kHudScaleMax, "%d%%")) {
    SetHudScale(hudScale);
  }
  bool hideHelmet = sHideHelmet;
  if (ImGui::Checkbox("Hide helmet", &hideHelmet)) {
    SetHideHelmet(hideHelmet);
  }
  ImGui::SameLine();
  bool hideVisorFx = sHideVisorEffects;
  if (ImGui::Checkbox("Hide visor effects", &hideVisorFx)) {
    SetHideVisorEffects(hideVisorFx);
  }
  ImGui::TextWrapped(
      "Visor effects: steam, Samus's reflection, and rain, water and goo on the visor.");

  float fov = sFirstPersonFov;
  if (ImGui::SliderFloat("Field of view", &fov, kFovMin, kFovMax, "%.0f deg")) {
    SetFirstPersonFov(std::round(fov));
  }
  ImGui::SameLine();
  if (ImGui::Button("Retail##fov")) {
    SetFirstPersonFov(kFovRetail);
  }
  {
    // The horizontal FOV this gives at the current aspect, which is the number
    // most PC games show.
    const float aspect = CCameraManager::GetDefaultAspectRatio();
    const float hfov = 2.f * std::atan(std::tan(sFirstPersonFov * 0.5f * 0.017453292f) * aspect) /
                       0.017453292f;
    ImGui::TextWrapped(
        "First-person vertical FOV (retail 55); about %.0f deg horizontal at this aspect. "
        "The arm cannon stays at the retail FOV. Morph ball and cutscene cameras are unchanged.",
        hfov);
  }

  int msaa = sMsaa >= 4 ? 1 : 0;
  if (ImGui::Combo("Anti-aliasing", &msaa, "Off\0" "4x MSAA\0")) {
    SetMsaa(msaa == 1 ? 4 : 1);
  }
  {
    int aniso = 0;
    while ((2 << aniso) <= sAnisotropy && aniso < 4) {
      ++aniso;
    }
    if (ImGui::Combo("Anisotropic filtering", &aniso, "1x\0" "2x\0" "4x\0" "8x\0" "16x\0")) {
      SetAnisotropy(1 << aniso);
    }
  }
  ImGui::TextWrapped(
      "MSAA smooths polygon edges at about 4x the framebuffer memory; anisotropic "
      "filtering keeps textures sharp at grazing angles (default 16x).");

  bool autoScale = sRenderScale <= 0.f;
  if (ImGui::Checkbox("Auto render scale (native)", &autoScale)) {
    SetRenderScale(autoScale ? 0.f : 1.f);
    MarkDirty();
  }
  if (!autoScale) {
    float scale = sRenderScale;
    if (ImGui::SliderFloat("EFB scale", &scale, 1.f, 2.f, "%.2fx")) {
      SetRenderScale(scale);
      MarkDirty();
    }
    ImGui::TextUnformatted("Scales the internal EFB; higher values use more GPU memory.");
  }

  ImGui::Text("HD texture set: %s", PortTextures::DeviceName());
  ImGui::TextWrapped(
      "Follows the input last used (xbox, playstation, switch, gamecube, "
      "standard, keyboard); set MP_TEXTURE_DEVICE to override.");
  DrawTexturePack();
}

// The user's texture pack, layered over the built-in set (see port_textures.h).
// On Android the folder is picked with the system folder picker and copied into
// the data folder; on the desktop the player fills the folder themselves.
void DrawTexturePack() {
  ImGui::SeparatorText("Texture pack");
  const char* root = PortTextures::UserRoot();
  if (root[0] == '\0') {
    ImGui::TextUnformatted("No user texture folder.");
    return;
  }
  const size_t count = PortTextures::UserPackCount();
  if (count > 0) {
    ImGui::Text("%zu replacements loaded, over the built-in set.", count);
  } else {
    ImGui::TextUnformatted("No texture pack loaded.");
  }
#if defined(__ANDROID__)
  const std::string status = TexturePackStatus();
  if (!status.empty()) {
    ImGui::TextWrapped("%s", status.c_str());
  }
  if (ImGui::Button("Choose texture pack folder...")) {
    PickTexturePack();
  }
  ImGui::SameLine();
  if (ImGui::Button("Remove texture pack")) {
    PortTextures::RequestUserPackRemoval();
    SetTexturePackStatus("Texture pack removed.");
  }
  ImGui::TextWrapped(
      "The folder is copied into the data folder, so it keeps working if the original is "
      "moved. Pick it again after changing it.");
#else
  ImGui::TextWrapped("Folder: %s", root);
  if (ImGui::Button("Reload texture pack")) {
    PortTextures::RequestUserPackReload();
  }
#endif
}

void DrawInputTab() {
  bool mouseAim = sMouseAim;
  if (ImGui::Checkbox("Mouse aim", &mouseAim)) {
    SetMouseAim(mouseAim);
    MarkDirty();
  }
  bool twinStick = sTwinStick;
  if (ImGui::Checkbox("Twin stick (right stick aims)", &twinStick)) {
    SetTwinStick(twinStick);
    MarkDirty();
  }
  ImGui::BeginDisabled(!sTwinStick);
  float stickRate = sStickAimRate;
  if (ImGui::SliderFloat("Stick aim speed", &stickRate, 100.f, 3000.f, "%.0f px/s",
                         ImGuiSliderFlags_Logarithmic)) {
    SetStickAimRate(stickRate);
  }
  // The game's own option, saved with its settings; free look uses it too.
  ImGui::BeginDisabled(gpGameState == nullptr);
  bool invertY = gpGameState != nullptr && gpGameState->GameOptions().GetInvertYAxis();
  if (ImGui::Checkbox("Invert stick aim Y (the game's Reverse Y Axis)", &invertY)) {
    gpGameState->GameOptions().SetInvertYAxis(invertY);
  }
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  ImGui::TextWrapped(
      "Twin stick uses the right stick as a direct camera aim (the same path as "
      "the mouse) and consumes it, so it no longer free-looks. Fire stays on "
      "whatever is bound to A; remap it in the Controls tab.");
  ImGui::SeparatorText("Hold or toggle");
  bool lockOnToggle = sLockOnToggle;
  if (ImGui::Checkbox("Toggle Lock-On", &lockOnToggle)) {
    SetLockOnToggle(lockOnToggle);
  }
  ImGui::TextWrapped(
      "Press L once to lock on, scan, strafe or grapple, and again to let go. "
      "The lock also lets go by itself when its target is gone.");
  bool stickyCharge = sStickyCharge;
  if (ImGui::Checkbox("Sticky Charge", &stickyCharge)) {
    SetStickyCharge(stickyCharge);
  }
  ImGui::TextWrapped(
      "Taps fire as usual. Hold fire for a moment and let go, and the beam keeps "
      "charging; press fire again to shoot. Needs the Charge Beam.");
  ImGui::SeparatorText("Morph ball");
  bool fastMorph = sFastMorph;
  if (ImGui::Checkbox("Fast Morph", &fastMorph)) {
    SetFastMorph(fastMorph);
  }
  ImGui::TextWrapped(
      "Morphing and unmorphing take a fraction of a second and keep your "
      "momentum, as in Metroid Prime 4. Unmorphing on the ground caps speed at "
      "walking speed; in the air the whole jump arc carries over.");
  const int springRule = PortAp::SpringBallRule();
  ImGui::BeginDisabled(springRule >= 0);
  bool springBall = sSpringBall;
  if (ImGui::Checkbox("Spring Ball (C-stick up)", &springBall)) {
    SetSpringBall(springBall);
  }
  ImGui::EndDisabled();
  bool swapScanXray = sSwapScanXray;
  if (ImGui::Checkbox("Swap the Scan and X-Ray visor buttons", &swapScanXray)) {
    SetSwapScanXray(swapScanXray);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
    ImGui::SetTooltip("Each takes the other's D-pad direction, as in Metroid Prime\n"
                      "Remastered's Dual Sticks layout. The Remastered controller preset\n"
                      "turns it on and the other presets off.");
  }
  if (springRule >= 0) {
    ImGui::TextWrapped("Set by the connected Archipelago seed: %s.",
                       springRule == 0   ? "off"
                       : springRule == 1 ? "with the Morph Ball Bombs"
                                         : "unlocked");
  } else {
    ImGui::TextWrapped(
        "A small jump in morph ball, as in Metroid Prime Trilogy, once the Morph "
        "Ball Bombs are held. Twin stick still passes the right stick up to it, "
        "and the beam shift (X in the Remastered preset) springs too.");
  }
  bool springFlick = sSpringFlick;
  if (ImGui::Checkbox("Spring Ball on gyro flick", &springFlick)) {
    SetSpringBallFlick(springFlick);
  }
  ImGui::BeginDisabled(!sSpringFlick);
  float flickRate = sSpringFlickRate;
  if (ImGui::SliderFloat("Flick strength", &flickRate, 2.f, 20.f, "%.1f rad/s")) {
    SetSpringBallFlickRate(flickRate);
  }
  ImGui::EndDisabled();
  ImGui::TextWrapped(
      "Tilt the pad or phone up sharply to spring, like Trilogy's nunchuk flick. "
      "Uses the gyro source below; gyro aim can stay off. Raise the strength if "
      "it springs by accident.");
  ImGui::SeparatorText("Gyro aim");
  const char* gyroModes[] = {"Off", "Hold to aim", "Always aim"};
  int gyroMode = sGyroMode;
  if (ImGui::Combo("Mode", &gyroMode, gyroModes, 3)) {
    SetGyroMode(gyroMode);
  }
  ImGui::BeginDisabled(sGyroMode == 0 && !sSpringFlick);
  const char* gyroSources[] = {"Auto", "Controller", "Phone"};
  int gyroSource = sGyroSource;
  if (ImGui::Combo("Source", &gyroSource, gyroSources, 3)) {
    SetGyroSource(gyroSource);
  }
  ImGui::Text("Gyro: %s", GyroStatus());
  ImGui::EndDisabled();
  ImGui::BeginDisabled(sGyroMode == 0);
  float gyroRate = sGyroRate;
  // The ## suffix keeps its ImGui id apart from the mouse Sensitivity slider.
  if (ImGui::SliderFloat("Sensitivity##gyro", &gyroRate, 50.f, 3000.f, "%.0f px/s per rad/s",
                         ImGuiSliderFlags_Logarithmic)) {
    SetGyroRate(gyroRate);
  }
  ImGui::TextWrapped(
      "Tilt the pad or the phone to aim. Hold to aim uses right stick click or "
      "left ctrl. Needs mouse aim or twin stick, since the gyro feeds that same "
      "aim.");
  ImGui::EndDisabled();

  if (ImGui::Checkbox("Invert mouse X", &sMouseInvertX)) {
    MarkDirty();
  }
  if (ImGui::Checkbox("Invert mouse Y", &sMouseInvertY)) {
    MarkDirty();
  }
  if (ImGui::Checkbox("Mouse weapon buttons", &sMouseButtons)) {
    sMouseButtonGate.Reset();
    MarkDirty();
  }
  if (ImGui::Checkbox("Mouse-aim crosshair", &sMouseCrosshair)) {
    MarkDirty();
  }
  int crosshairSize = sCrosshairSize;
  if (ImGui::SliderInt("Crosshair size", &crosshairSize, kCrosshairSizeMin, kCrosshairSizeMax,
                       "%d%%")) {
    SetCrosshairSize(crosshairSize);
  }
  ImGui::TextUnformatted("Crosshair size applies under mouse aim and twin stick.");
  ImGui::TextUnformatted("Mouse buttons are set in Controls > Keyboard & mouse.");
  ImGui::TextUnformatted("Existing keyboard/controller weapon bindings also work.");
  if (ImGui::SliderFloat("Sensitivity", &sMouseSensitivity, 0.0005f, 0.02f, "%.4f rad/px",
                         ImGuiSliderFlags_Logarithmic)) {
    MarkDirty();
  }
}

void DrawAudio() {
  bool ai = AiAudioEnabled();
  if (ImGui::Checkbox("Streamed audio (music/movies)", &ai)) {
    SetAiAudioEnabled(ai);
    MarkDirty();
  }
  bool musyx = sMusyxAudioEnabled;
  if (ImGui::Checkbox("MusyX audio (effects/streams)", &musyx)) {
    SetMusyxAudioEnabled(musyx);
    MarkDirty();
  }
}

void DrawVoices() {
  PortMusyxVoice voices[64];
  const int count = MusyxPortCopyVoices(voices, 64);
  if (count == 0) {
    ImGui::TextUnformatted("No active MusyX voices.");
    return;
  }

  struct Agg {
    PortMusyxVoice voice;
    int instances;
  };
  std::vector< Agg > aggs;
  for (int i = 0; i < count; ++i) {
    bool found = false;
    for (Agg& agg : aggs) {
      if (agg.voice.smpId == voices[i].smpId) {
        ++agg.instances;
        if (voices[i].rms > agg.voice.rms) {
          agg.voice = voices[i];
        }
        found = true;
        break;
      }
    }
    if (!found) {
      aggs.push_back(Agg{voices[i], 1});
    }
  }
  std::sort(aggs.begin(), aggs.end(),
            [](const Agg& a, const Agg& b) { return a.voice.rms > b.voice.rms; });

  ImGui::TextUnformatted("Active samples, loudest first. Mute one to isolate it.");
  for (const Agg& agg : aggs) {
    ImGui::PushID(static_cast< int >(agg.voice.smpId));
    bool muted = MusyxPortIsSampleMuted(agg.voice.smpId) != 0;
    if (ImGui::Checkbox("##mute", &muted)) {
      MusyxPortSetSampleMuted(agg.voice.smpId, muted ? 1 : 0);
      MarkDirty();
    }
    ImGui::SameLine();
    ImGui::Text("smp %u  %s  len %u  pitch %u  rms %d  vol %u/%u  x%d", agg.voice.smpId,
                agg.voice.looped ? "loop" : "one-shot", agg.voice.length, agg.voice.pitch,
                agg.voice.rms, agg.voice.volL, agg.voice.volR, agg.instances);
    ImGui::PopID();
  }
  if (ImGui::Button("Unmute all")) {
    MusyxPortClearSampleMutes();
    MarkDirty();
  }
}

// Archipelago's Connect screen: the room's address, the slot name and the
// room password, saved to archipelago.json. The built-in Metroid Prime tables
// mean nothing else is needed, so this works where editing a file does not.
void DrawArchipelagoConnect() {
  static bool sLoaded = false;
  static char sServer[256];
  static char sSlot[64];
  static char sPassword[128];
  static std::string sResult;
  if (!sLoaded) {
    sLoaded = true;
    const PortAp::ConnectionDetails saved = PortAp::SavedConnection();
    SDL_strlcpy(sServer, saved.server.c_str(), sizeof(sServer));
    SDL_strlcpy(sSlot, saved.slot.c_str(), sizeof(sSlot));
    SDL_strlcpy(sPassword, saved.password.c_str(), sizeof(sPassword));
  }

  ImGui::SeparatorText("Archipelago");
  // The server doesn't send this option, so it is set here to match the seed.
  int suitDamage = sApSuitDamage;
  if (ImGui::Combo("Staggered suit damage", &suitDamage, "Default\0Progressive\0Additive\0")) {
    SetApSuitDamage(suitDamage);
  }
  ImGui::SetItemTooltip("Set it to your YAML's staggered_suit_damage (the apworld's default is\n"
                        "Progressive: damage reduction by how many suits you have).");
  ImGui::InputTextWithHint("Server", "archipelago.gg:38281", sServer, sizeof(sServer));
  ImGui::InputTextWithHint("Slot name", "your player name in the seed", sSlot, sizeof(sSlot));
  ImGui::InputTextWithHint("Password", "only if the room has one", sPassword, sizeof(sPassword),
                           ImGuiInputTextFlags_Password);
  if (ImGui::Button(PortAp::Enabled() ? "Reconnect" : "Connect")) {
    PortAp::ConnectionDetails details;
    details.server = sServer;
    details.slot = sSlot;
    details.password = sPassword;
    std::string error;
    sResult = PortAp::Connect(details, error) ? std::string() : error;
    // The saved form (trimmed, "/connect " dropped) goes back into the fields.
    if (sResult.empty())
      sLoaded = false;
  }
  if (PortAp::Enabled()) {
    ImGui::SameLine();
    if (ImGui::Button("Disconnect")) {
      std::string error;
      sResult = PortAp::Disconnect(error) ? std::string() : error;
    }
  }
  if (!sResult.empty())
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", sResult.c_str());
  ImGui::TextDisabled("Start a new game after connecting to a new seed.");

  // Games played before, each with its own save card. The list is re-read now
  // and then: a game is recorded when its server answers, after Connect.
  static std::vector<PortAp::ConnectionDetails> sRecent;
  static uint64_t sRecentReadAt = 0;
  if (!ImGui::CollapsingHeader("Recent Archipelago games", ImGuiTreeNodeFlags_DefaultOpen)) {
    sRecentReadAt = 0;
    return;
  }
  const uint64_t now = SDL_GetTicks();
  if (sRecentReadAt == 0 || now - sRecentReadAt > 2000) {
    sRecent = PortAp::RecentGames();
    sRecentReadAt = now == 0 ? 1 : now;
  }
  if (sRecent.empty()) {
    ImGui::TextDisabled("None yet. A game is listed once its server has answered.");
    return;
  }
  // The save card only changes on the title screen, so a game in progress
  // would keep saving to the card it was loaded from.
  const bool inGame = StateManager() != nullptr;
  if (inGame)
    ImGui::TextDisabled("Quit to the title screen to resume another game.");
  const PortAp::ConnectionDetails current = PortAp::SavedConnection();
  for (size_t i = 0; i < sRecent.size(); ++i) {
    const PortAp::ConnectionDetails& game = sRecent[i];
    const bool isCurrent = PortAp::Enabled() && current.server == game.server &&
                           current.slot == game.slot && current.seed == game.seed;
    ImGui::PushID(static_cast<int>(i));
    ImGui::BeginDisabled(inGame || isCurrent);
    if (ImGui::Button(isCurrent ? "Current" : "Resume")) {
      std::string error;
      sResult = PortAp::Connect(game, error) ? std::string() : error;
      if (sResult.empty()) {
        sLoaded = false;
        sRecentReadAt = 0;
      }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    char played[32] = "";
    const time_t when = static_cast<time_t>(game.lastPlayed);
    if (const std::tm* local = game.lastPlayed > 0 ? std::localtime(&when) : nullptr)
      std::strftime(played, sizeof(played), "%Y-%m-%d %H:%M", local);
    ImGui::Text("%s @ %s", game.slot.c_str(), game.server.c_str());
    ImGui::Indent();
    ImGui::TextDisabled("%s%s%s", game.seed.c_str(), played[0] != '\0' ? "  -  " : "", played);
    ImGui::Unindent();
    ImGui::PopID();
  }
}

ImVec4 ChatLineColor(const std::string& type) {
  if (type == "Chat")
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
  if (type == "ServerChat" || type == "CommandResult" || type == "AdminCommandResult")
    return ImVec4(0.55f, 0.85f, 1.0f, 1.0f);
  if (type == "Hint")
    return ImVec4(1.0f, 0.85f, 0.4f, 1.0f);
  if (type == "ItemSend" || type == "ItemCheat")
    return ImVec4(0.6f, 0.95f, 0.6f, 1.0f);
  if (type == "Goal" || type == "Release" || type == "Collect" || type == "Countdown")
    return ImVec4(1.0f, 0.6f, 1.0f, 1.0f);
  if (type == "port")
    return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
  return ImVec4(0.8f, 0.8f, 0.8f, 1.0f); // Join, Part, TagsChanged, Tutorial, ...
}

// The multiworld's chat: every PrintJSON message the server sent, and a box that
// sends Say, so server commands such as !hint work from inside the game.
void DrawChatTab() {
  static char sInput[512] = {};
  static std::string sError;
  static uint64_t sSeenSerial = ~uint64_t{0};

  if (!PortAp::Enabled()) {
    ImGui::TextWrapped("Connect to an Archipelago room in the Session tab to chat.");
    return;
  }
  uint64_t serial = 0;
  const std::vector< PortAp::ChatLine > log = PortAp::ChatLog(&serial);
  // The log takes the tab down to the input row, the error and the help line.
  const ImGuiStyle& style = ImGui::GetStyle();
  const float below = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * 3.0f;
  // The window may reach past a small display, so stop at whichever ends first.
  const float room = std::min(ImGui::GetContentRegionAvail().y,
                              ImGui::GetIO().DisplaySize.y - style.WindowPadding.y -
                                  ImGui::GetCursorScreenPos().y);
  const float logHeight = std::max(ImGui::GetTextLineHeightWithSpacing() * 6.0f, room - below);
  if (ImGui::BeginChild("apChat", ImVec2(0.0f, logHeight), ImGuiChildFlags_Borders)) {
    // Follow new lines only while the log is scrolled to the bottom, so reading
    // back is not interrupted.
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    if (log.empty())
      ImGui::TextDisabled("No messages yet.");
    ImGui::PushTextWrapPos(0.0f);
    for (const PortAp::ChatLine& line : log) {
      ImGui::PushStyleColor(ImGuiCol_Text, ChatLineColor(line.type));
      ImGui::TextUnformatted(line.text.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    if (serial != sSeenSerial && (atBottom || sSeenSerial == ~uint64_t{0}))
      ImGui::SetScrollHereY(1.0f);
    sSeenSerial = serial;
  }
  ImGui::EndChild();

  const bool connected = PortAp::Connected();
  ImGui::BeginDisabled(!connected);
  const float sendWidth = ImGui::CalcTextSize("Send").x + style.FramePadding.x * 2.0f;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - sendWidth - style.ItemSpacing.x);
  bool send = ImGui::InputTextWithHint("##apSay", connected ? "Message or !command" : "Not connected",
                                       sInput, sizeof(sInput), ImGuiInputTextFlags_EnterReturnsTrue);
  if (send)
    ImGui::SetKeyboardFocusHere(-1); // Enter keeps the box focused for the next line
  ImGui::SameLine();
  send = ImGui::Button("Send") || send;
  ImGui::EndDisabled();
  if (send && sInput[0] != '\0') {
    if (PortAp::SendChat(sInput, sError))
      sInput[0] = '\0';
  }
  if (!sError.empty())
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", sError.c_str());
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("Commands: !hint <item>, !hint_location <location>, !remaining, !release, "
                     "!collect, !help");
  ImGui::PopStyleColor();
}

void DrawSessionTab() {
  if (ImGui::Button("Restart to menu")) {
    RequestReset();
  }
  ImGui::SameLine();
  if (ImGui::Button("Screenshot (F12)")) {
    aurora::request_screenshot();
  }
  ImGui::SameLine();
  if (ImGui::Button("Exit game")) {
    ImGui::OpenPopup("Exit game?");
  }
  if (ImGui::BeginPopupModal("Exit game?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Progress since the last save station is lost.");
    if (ImGui::Button("Exit")) {
      // The same path as closing the window: the main loop sees AURORA_EXIT
      // and shuts down cleanly.
      SaveSettings();
      SDL_Event quit{};
      quit.type = SDL_EVENT_QUIT;
      SDL_PushEvent(&quit);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::Separator();
  ImGui::TextUnformatted("Settings are saved automatically when changed.");
  const std::string path = SettingsFilePath();
  ImGui::TextWrapped("File: %s", path.c_str());
  if (ImGui::Button("Save settings now")) {
    sSettingsDirty = true;
    SaveSettings();
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(sSettingsDirty ? "Unsaved changes" : "Saved");

  DrawArchipelagoConnect();
  if (PortAp::Enabled()) {
    ImGui::TextWrapped("Status: %s", PortAp::StatusText());
    const char* seedName = PortAp::SeedName();
    if (seedName != nullptr && seedName[0] != '\0')
      ImGui::TextWrapped("Seed: %s", seedName);
    ImGui::Text("Items received: %d", PortAp::ItemCount());
    ImGui::Text("Location checks sent: %d", PortAp::CheckCount());
    ImGui::Text("Connection: %s", PortAp::Connected() ? "connected" : "not connected");
    const char* lastMessage = PortAp::LastMessage();
    if (lastMessage != nullptr && lastMessage[0] != '\0')
      ImGui::TextWrapped("Last message: %s", lastMessage);

    // Item tracker: the session's receipts, so an item that arrived while the
    // player was not looking at the HUD is still readable here.
    const std::vector< PortAp::TrackedItem > tracked = PortAp::TrackedItems();
    ImGui::SeparatorText("Received items");
    if (tracked.empty()) {
      ImGui::TextDisabled("Nothing yet.");
    } else {
      // Half the space the section has left, so the table does not push the
      // settings below it off the tab.
      if (ImGui::BeginTable("apTracked", 3,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_BordersInnerH,
                            ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.5f))) {
        ImGui::TableSetupColumn("Item");
        ImGui::TableSetupColumn("From");
        ImGui::TableSetupColumn("Step");
        ImGui::TableHeadersRow();
        for (const PortAp::TrackedItem& item : tracked) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item.name.c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item.from.empty() ? "(your item)" : item.from.c_str());
          ImGui::TableNextColumn();
          if (item.total > 1) {
            ImGui::Text("%d of %d", item.step, item.total);
          } else {
            ImGui::TextDisabled("-");
          }
        }
        ImGui::EndTable();
      }
    }
  }
}

void GrantItem(CPlayerState& ps, CPlayerState::EItemType type, int amount, int capacity) {
  ps.SetPowerUp(type, capacity);
  ps.SetPickup(type, amount);
}

void DrawCheats() {
  CStateManager* mgr = sStateManager;
  if (mgr == nullptr) {
    ImGui::TextUnformatted("Waiting for gameplay...");
    return;
  }
  CPlayerState* ps = mgr->PlayerState();
  if (ps == nullptr) {
    ImGui::TextUnformatted("No player state.");
    return;
  }

  ImGui::Text("Health: %.0f / %.0f", ps->HealthInfo()->GetHP(), ps->CalculateHealth());
  if (ImGui::Button("Full health")) {
    ps->HealthInfo()->SetHP(ps->CalculateHealth());
  }
  ImGui::SameLine();
  if (ImGui::Button("Grant everything")) {
    for (int i = CPlayerState::kIT_PowerBeam; i < CPlayerState::kIT_Max; ++i) {
      GrantItem(*ps, static_cast< CPlayerState::EItemType >(i), 1, 1);
    }
    GrantItem(*ps, CPlayerState::kIT_Missiles, 250, 250);
    GrantItem(*ps, CPlayerState::kIT_PowerBombs, 8, 8);
    GrantItem(*ps, CPlayerState::kIT_EnergyTanks, 14, 14);
    ps->HealthInfo()->SetHP(ps->CalculateHealth());
  }
  bool invulnerable = Invulnerable();
  if (ImGui::Checkbox("Invulnerable", &invulnerable)) {
    SetInvulnerable(invulnerable);
  }
  ImGui::TextWrapped("Samus takes no damage from anything. Stays on until unticked.");

  ImGui::Separator();
  ImGui::TextUnformatted("Abilities");
  struct SItemToggle {
    const char* name;
    CPlayerState::EItemType type;
  };
  static const SItemToggle kItems[] = {
      {"Power Beam", CPlayerState::kIT_PowerBeam},
      {"Ice Beam", CPlayerState::kIT_IceBeam},
      {"Wave Beam", CPlayerState::kIT_WaveBeam},
      {"Plasma Beam", CPlayerState::kIT_PlasmaBeam},
      {"Charge Beam", CPlayerState::kIT_ChargeBeam},
      {"Super Missile", CPlayerState::kIT_SuperMissile},
      {"Ice Spreader", CPlayerState::kIT_IceSpreader},
      {"Wavebuster", CPlayerState::kIT_Wavebuster},
      {"Flamethrower", CPlayerState::kIT_Flamethrower},
      {"Combat Visor", CPlayerState::kIT_CombatVisor},
      {"Scan Visor", CPlayerState::kIT_ScanVisor},
      {"Thermal Visor", CPlayerState::kIT_ThermalVisor},
      {"X-Ray Visor", CPlayerState::kIT_XRayVisor},
      {"Morph Ball", CPlayerState::kIT_MorphBall},
      {"Morph Ball Bombs", CPlayerState::kIT_MorphBallBombs},
      {"Boost Ball", CPlayerState::kIT_BoostBall},
      {"Spider Ball", CPlayerState::kIT_SpiderBall},
      {"Space Jump Boots", CPlayerState::kIT_SpaceJumpBoots},
      {"Grapple Beam", CPlayerState::kIT_GrappleBeam},
      {"Gravity Suit", CPlayerState::kIT_GravitySuit},
      {"Varia Suit", CPlayerState::kIT_VariaSuit},
      {"Phazon Suit", CPlayerState::kIT_PhazonSuit},
  };
  for (const SItemToggle& item : kItems) {
    bool owned = ps->HasPowerUp(item.type);
    if (ImGui::Checkbox(item.name, &owned)) {
      if (owned) {
        GrantItem(*ps, item.type, 1, 1);
      } else {
        ps->SetPowerUp(item.type, 0);
        ps->SetPickup(item.type, 0);
      }
    }
  }

  int missiles = ps->GetItemAmount(CPlayerState::kIT_Missiles);
  if (ImGui::SliderInt("Missiles", &missiles, 0, 250)) {
    GrantItem(*ps, CPlayerState::kIT_Missiles, missiles, 250);
  }
  int powerBombs = ps->GetItemAmount(CPlayerState::kIT_PowerBombs);
  if (ImGui::SliderInt("Power Bombs", &powerBombs, 0, 8)) {
    GrantItem(*ps, CPlayerState::kIT_PowerBombs, powerBombs, 8);
  }
  int tanks = ps->GetItemAmount(CPlayerState::kIT_EnergyTanks);
  if (ImGui::SliderInt("Energy Tanks", &tanks, 0, 14)) {
    GrantItem(*ps, CPlayerState::kIT_EnergyTanks, tanks, 14);
    ps->HealthInfo()->SetHP(ps->CalculateHealth());
  }

  ImGui::Separator();
  ImGui::TextUnformatted("Teleport");
  CWorld* world = mgr->World();
  if (world == nullptr) {
    ImGui::TextUnformatted("No world.");
    return;
  }
  if (mgr->GetGameState() != CStateManager::kGS_Running) {
    ImGui::TextUnformatted("(waiting for gameplay)");
  }
  const int areaCount = world->IGetAreaCount();
  const int current = world->GetCurrentAreaId().Value();
  ImGui::Text("Current area: %d of %d", current, areaCount);
  for (int i = 0; i < areaCount; ++i) {
    ImGui::PushID(i);
    if (ImGui::Button(i == current ? "Reload" : "Go")) {
      PortDebug::RequestTeleport(i);
    }
    ImGui::SameLine();
    ImGui::Text("Area %d", i);
    ImGui::PopID();
  }

  ImGui::Separator();
  ImGui::TextUnformatted("Worlds");
  if (gpMemoryCard == nullptr) {
    ImGui::TextUnformatted("(memory card not ready)");
    return;
  }
  static bool sWorldListBuilt = false;
  static std::vector< std::pair< uint32_t, std::string > > sWorldList;
  if (!sWorldListBuilt && !gpMemoryCard->GetMemoryWorlds().empty()) {
    sWorldListBuilt = true;
    const rstl::vector< CMemoryCard::MemoryWorld >& worlds = gpMemoryCard->GetMemoryWorlds();
    for (int i = 0; i < worlds.size(); ++i) {
      const uint32_t id = static_cast< uint32_t >(worlds[i].first);
      std::string name;
      const wchar_t* wide = worlds[i].second.GetFrontEndName();
      if (wide != nullptr) {
        for (const wchar_t* p = wide; *p != 0; ++p) {
          name.push_back(static_cast< char >(*p));
        }
      }
      if (name.empty()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "MLVL %08X", static_cast< unsigned >(id));
        name = buf;
      }
      sWorldList.emplace_back(id, name);
    }
  }
  if (!sWorldListBuilt) {
    ImGui::TextUnformatted("(loading worlds...)");
    return;
  }
  for (const std::pair< uint32_t, std::string >& entry : sWorldList) {
    ImGui::PushID(static_cast< int >(entry.first));
    const bool isCurrent =
        gpGameState != nullptr && gpGameState->CurrentWorldAssetId() == entry.first;
    if (ImGui::Button(isCurrent ? "Here" : "Go")) {
      PortDebug::RequestWorldTeleport(entry.first, 0u);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(entry.second.c_str());
    ImGui::PopID();
  }
}

// The free camera: the view leaves the player, who stands still meanwhile.
void DrawFreeCam() {
  CStateManager* mgr = StateManager();
  bool on = PortFreeCam::Active();
  ImGui::BeginDisabled(mgr == nullptr && !on);
  if (ImGui::Checkbox("Free camera", &on)) {
    PortFreeCam::SetActive(on, mgr);
  }
  ImGui::EndDisabled();
  if (!PortFreeCam::Active()) {
    return;
  }
  ImGui::SameLine();
  bool frozen = PortFreeCam::Frozen();
  if (ImGui::Checkbox("Freeze the game", &frozen)) {
    PortFreeCam::SetFrozen(frozen);
  }
  ImGui::SameLine();
  bool showPlayer = PortFreeCam::ShowPlayer();
  if (ImGui::Checkbox("Show Samus", &showPlayer)) {
    PortFreeCam::SetShowPlayer(showPlayer);
  }
  float speed = PortFreeCam::Speed();
  if (ImGui::SliderFloat("Speed", &speed, 1.f, 100.f, "%.0f m/s", ImGuiSliderFlags_Logarithmic)) {
    PortFreeCam::SetSpeed(speed);
  }
  PortFreeCam::Pose pose = PortFreeCam::GetPose();
  float pos[3] = {pose.x, pose.y, pose.z};
  float look[2] = {pose.yaw, pose.pitch};
  bool moved = ImGui::InputFloat3("Position", pos, "%.2f");
  moved |= ImGui::InputFloat2("Yaw, pitch", look, "%.1f");
  if (moved) {
    pose.x = pos[0];
    pose.y = pos[1];
    pose.z = pos[2];
    pose.yaw = look[0];
    pose.pitch = look[1];
    PortFreeCam::SetPose(pose);
  }
  ImGui::TextWrapped("Close this menu to fly: stick moves, C stick or mouse looks, Z / D-pad up "
                     "rises, L / D-pad down sinks, R goes four times as fast.");
}

// The switches and readouts for working on what mods draw: room geometry, room
// environments, PBR.
void DrawRendering() {
  int view = PortDebug::PbrView();
  if (ImGui::BeginCombo("PBR surfaces show", view == 0 ? "the shaded result" : PortDebug::PbrViewName(view))) {
    for (int i = 0; i < PortDebug::PbrViewCount(); ++i) {
      if (ImGui::Selectable(i == 0 ? "the shaded result" : PortDebug::PbrViewName(i), i == view)) {
        PortDebug::SetPbrView(i);
      }
    }
    ImGui::EndCombo();
  }
  static const char* const kProbes[] = {"off", "on", "mirror", "window"};
  int probe = std::clamp(CCubeMaterial::sPortPBRProbeMode, 0, 3);
  if (ImGui::Combo("Reflection probe", &probe, kProbes, 4)) {
    CCubeMaterial::sPortPBRProbeMode = probe;
  }
  bool font = PortHdFont::Enabled();
  if (ImGui::Checkbox("HD font", &font)) {
    PortHdFont::SetEnabled(font);
  }

  static const char* const kModes[] = {"off", "in place of the room", "on top of the room"};
  int mode = int(PortRoomGeo::GetMode());
  if (ImGui::Combo("Room geometry", &mode, kModes, 3)) {
    PortRoomGeo::SetMode(PortRoomGeo::Mode(mode));
  }
  bool areaLights = PortRoomGeo::AreaLights();
  if (ImGui::Checkbox("Room geometry takes the area's lights", &areaLights)) {
    PortRoomGeo::SetAreaLights(areaLights);
  }
  bool env = PortRoomEnv::Enabled();
  if (ImGui::Checkbox("Room environments", &env)) {
    PortRoomEnv::SetEnabled(env);
  }
  ImGui::BeginDisabled(!env);
  bool exposed = PortRoomEnv::RoomExposed();
  if (ImGui::Checkbox("Exposure by room", &exposed)) {
    PortRoomEnv::SetRoomExposed(exposed);
  }
  ImGui::SameLine();
  bool volumes = PortRoomEnv::VolumesEnabled();
  if (ImGui::Checkbox("Baked light per pixel", &volumes)) {
    PortRoomEnv::SetVolumesEnabled(volumes);
  }
  float ambient = PortRoomEnv::AmbientScale();
  if (ImGui::SliderFloat("Baked ambient scale", &ambient, 0.f, 4.f, "%.2f")) {
    PortRoomEnv::SetAmbientScale(ambient);
  }
  static const char* const kVolumeViews[] = {"the shaded surface", "volume coordinates", "the baked light"};
  int volumeView = std::clamp(PortRoomEnv::VolumeView(), 0, 2);
  if (ImGui::Combo("Baked surfaces show", &volumeView, kVolumeViews, 3)) {
    PortRoomEnv::SetVolumeView(volumeView);
  }
  ImGui::EndDisabled();

  // What the middle of the screen looks at.
  static std::string picked;
  static uint32_t pickedModel = 0;
  static std::string materials;
  float origin[3];
  float forward[3];
  const bool inWorld = PortDebug::ViewRay(origin, forward);
  ImGui::BeginDisabled(!inWorld);
  if (ImGui::Button("Pick the model ahead")) {
    picked.clear();
    pickedModel = PortRoomGeo::Pick(CVector3f(origin[0], origin[1], origin[2]),
                                    CVector3f(forward[0], forward[1], forward[2]), picked);
    materials = pickedModel != 0 ? PortRoomGeo::Materials(pickedModel) : std::string();
    if (picked.empty()) {
      picked = "No room geometry ahead.";
    }
  }
  ImGui::EndDisabled();
  if (pickedModel != 0) {
    ImGui::SameLine();
    if (ImGui::Button("Hide it")) {
      PortRoomGeo::SetHidden(pickedModel, true);
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Show all")) {
    PortRoomGeo::SetHidden(0, false);
  }
  if (!picked.empty()) {
    ImGui::TextUnformatted(picked.c_str());
  }
  if (!materials.empty() && ImGui::TreeNode("Materials of the first")) {
    ImGui::TextUnformatted(materials.c_str());
    ImGui::TreePop();
  }

  if (ImGui::TreeNode("Frame")) {
    if (const AuroraStats* stats = aurora_get_stats()) {
      ImGui::Text("%.0f fps, %u draws (%u merged), %u PBR", aurora_get_fps(), stats->drawCallCount,
                  stats->mergedDrawCallCount, CCubeMaterial::sPortPBRDraws);
      ImGui::Text("vertices %.1f MiB, indices %.1f, arrays %.1f, uniforms %.1f, texture uploads %.1f",
                  stats->lastVertSize / 1048576.f, stats->lastIndexSize / 1048576.f,
                  stats->lastStorageSize / 1048576.f, stats->lastUniformSize / 1048576.f,
                  stats->lastTextureUploadSize / 1048576.f);
      ImGui::Text("pipelines %u made, %u waiting", stats->createdPipelines, stats->queuedPipelines);
    }
    AuroraTextureStats textures{};
    aurora_get_texture_stats(&textures);
    ImGui::Text("textures %u, %.0f MiB; render targets %u, %.0f MiB", textures.count[0],
                textures.bytes[0] / 1048576.f, textures.count[1], textures.bytes[1] / 1048576.f);
    int areas = 0;
    int instances = 0;
    int models = 0;
    int loaded = 0;
    int drawn = 0;
    PortRoomGeo::Stats(areas, instances, models, loaded, drawn);
    ImGui::Text("room geometry: %d area(s), %d of %d model(s) loaded, %d of %d instance(s) drawn", areas, loaded,
                models, drawn, instances);
    ImGui::TreePop();
  }
  if (inWorld && ImGui::TreeNode("Room environment here")) {
    ImGui::TextUnformatted(PortRoomEnv::Info(origin).c_str());
    ImGui::TreePop();
  }
}

void DrawDebugTab() {
  ImGui::SeparatorText("Camera");
  DrawFreeCam();

  ImGui::SeparatorText("Rendering");
  DrawRendering();

  ImGui::SeparatorText("Audio");
  DrawAudio();
  if (ImGui::CollapsingHeader("Voices")) {
    DrawVoices();
  }

  ImGui::SeparatorText("Log");
  bool logFile = sLogFile || PortLogFile::Active();
  if (ImGui::Checkbox("Write the log to a file", &logFile)) {
    SetLogFile(logFile);
    if (logFile) {
      PortLogFile::Start();
    }
  }
  const std::string logPath = PortLogFile::Path();
  if (PortLogFile::Active()) {
    ImGui::TextWrapped("Writing to %s (last run's: metroid_prime_port.old.log).", logPath.c_str());
    if (!sLogFile) {
      ImGui::TextDisabled("Stops at the next start.");
    }
  } else {
    ImGui::TextWrapped("Everything the game logs, including the reason for a crash, goes to %s.",
                       logPath.empty() ? "(no user folder)" : logPath.c_str());
  }
  if (!logPath.empty() && ImGui::Button("Copy log path")) {
    ImGui::SetClipboardText(logPath.c_str());
  }

  ImGui::SeparatorText("Cheats");
  bool cheats = sCheats;
  if (ImGui::Checkbox("Show cheats (items, health, teleport)", &cheats)) {
    sCheats = cheats;
    MarkDirty();
  }
  if (sCheats) {
    DrawCheats();
  }
}

void DrawTrackerCount(const char* label, const PortTracker::Count& count) {
  const bool done = count.total > 0 && count.have >= count.total;
  if (done) {
    ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "%s %d/%d", label, count.have, count.total);
  } else {
    ImGui::Text("%s %d/%d", label, count.have, count.total);
  }
}

// The Archipelago checks within reach, coloured as on the map.
void DrawTrackerLogic() {
  static PortAp::LogicState state;
  if (!PortAp::Logic(state)) {
    return;
  }
  ImGui::SeparatorText("Archipelago checks");
  bool colors = sMapLogicColors;
  if (ImGui::Checkbox("Colour the map's dots by logic", &colors)) {
    SetMapLogicColors(colors);
  }
  static const ImVec4 kColors[] = {
      ImVec4(0.95f, 0.30f, 0.30f, 1.f), // out of logic
      ImVec4(0.35f, 0.60f, 1.00f, 1.f), // inspect
      ImVec4(1.00f, 0.85f, 0.25f, 1.f), // sequence break
      ImVec4(0.35f, 0.90f, 0.40f, 1.f), // in logic
  };
  static const ImVec4 kGrey(0.55f, 0.55f, 0.55f, 1.f);
  size_t count = 0;
  const PortApLogic::Check* checks = PortApLogic::Checks(count);
  int totals[4] = {};
  int checked = 0;
  for (size_t i = 0; i < count; ++i) {
    if (state.checked[i]) {
      ++checked;
    } else {
      ++totals[static_cast< int >(state.levels[i])];
    }
  }
  ImGui::TextColored(kColors[3], "%d in logic", totals[3]);
  ImGui::SameLine();
  ImGui::TextColored(kColors[2], "%d sequence break", totals[2]);
  ImGui::SameLine();
  ImGui::TextColored(kColors[1], "%d visible only", totals[1]);
  ImGui::SameLine();
  ImGui::TextColored(kColors[0], "%d out of reach", totals[0]);
  ImGui::SameLine();
  ImGui::TextColored(kGrey, "%d checked", checked);
  ImGui::TextWrapped(
      "Worked out from the items received and the seed's logic options, with the rules of "
      "the Metroid Prime Archipelago tracker pack. Green is in logic; yellow can be reached "
      "with a trick the seed doesn't count on; blue can be seen but not collected.");

  // One header per area, holding what can be reached there, best first.
  const char* area = nullptr;
  bool open = false;
  for (size_t i = 0; i < count; ++i) {
    if (area == nullptr || std::strcmp(area, checks[i].area) != 0) {
      area = checks[i].area;
      int inLogic = 0;
      int other = 0;
      for (size_t j = 0; j < count; ++j) {
        if (std::strcmp(checks[j].area, area) != 0 || state.checked[j]) {
          continue;
        }
        if (state.levels[j] == PortApLogic::Level::Normal) {
          ++inLogic;
        } else if (state.levels[j] != PortApLogic::Level::None) {
          ++other;
        }
      }
      char header[160];
      std::snprintf(header, sizeof(header), "%s (%d in logic, %d other)###aplogic%s", area, inLogic,
                    other, area);
      ImGui::SetNextItemOpen(inLogic > 0, ImGuiCond_Once);
      open = ImGui::CollapsingHeader(header);
      if (open) {
        for (int level = 3; level >= 1; --level) {
          for (size_t j = 0; j < count; ++j) {
            if (std::strcmp(checks[j].area, area) != 0 || state.checked[j] ||
                static_cast< int >(state.levels[j]) != level) {
              continue;
            }
            if (checks[j].section[0] != 0) {
              ImGui::TextColored(kColors[level], "  %s - %s", checks[j].room, checks[j].section);
            } else {
              ImGui::TextColored(kColors[level], "  %s", checks[j].room);
            }
          }
        }
        if (inLogic + other == 0) {
          ImGui::TextDisabled("  nothing within reach");
        }
      }
    }
  }
}

void DrawTrackerTab() {
  bool reveal = sRevealMap;
  if (ImGui::Checkbox("Reveal map", &reveal)) {
    SetRevealMap(reveal);
  }
  ImGui::TextWrapped(
      "Shows every world's map as if its map station had been used, and lists every "
      "world on the star map. Rooms a map station leaves hidden stay hidden, and rooms "
      "you haven't entered keep the unexplored colour. The save is not changed.");
  bool pickups = sMapPickups;
  if (ImGui::Checkbox("Pickup dots on the map", &pickups)) {
    SetMapPickups(pickups);
  }
  if (PortMapPickups::Forced()) {
    ImGui::SameLine();
    ImGui::TextDisabled("(on in randomized games)");
  }
  ImGui::TextWrapped(
      "A white dot marks each item pickup in the rooms the map shows, until you collect "
      "it. Every item gets the same dot, so it doesn't give away what a pickup holds. "
      "An Archipelago game colours them by what its logic lets you reach.");

  DrawTrackerLogic();

  CStateManager* mgr = sStateManager;
  if (mgr == nullptr || mgr->GetPlayerState() == nullptr) {
    ImGui::TextDisabled("Progress shows once a game is running.");
    return;
  }
  const PortTracker::Summary summary = PortTracker::Collect(*mgr);

  ImGui::SeparatorText("Items");
  ImGui::Text("Item collection %d%%", summary.itemPercent);
  DrawTrackerCount("Energy Tanks", summary.energyTanks);
  ImGui::SameLine(ImGui::GetFontSize() * 12.f);
  DrawTrackerCount("Missile expansions", summary.missileExpansions);
  DrawTrackerCount("Power Bombs", summary.powerBombExpansions);
  ImGui::SameLine(ImGui::GetFontSize() * 12.f);
  DrawTrackerCount("Artifacts", summary.artifacts);
  const PortTracker::Count upgrades = {
      static_cast< int >(summary.upgradesHeld.size()),
      static_cast< int >(summary.upgradesHeld.size() + summary.upgradesMissing.size())};
  DrawTrackerCount("Upgrades", upgrades);
  if (!summary.upgradesMissing.empty()) {
    std::string missing;
    for (const std::string& name : summary.upgradesMissing) {
      missing += (missing.empty() ? "" : ", ") + name;
    }
    ImGui::TextWrapped("Missing: %s", missing.c_str());
  }

  ImGui::SeparatorText("Scans");
  for (int i = 0; i < PortTracker::kScan_Count; ++i) {
    DrawTrackerCount(PortTracker::ScanGroupName(i), summary.scans[i]);
    if (i % 2 == 0) {
      ImGui::SameLine(ImGui::GetFontSize() * 12.f);
    }
  }
  DrawTrackerCount("All scans", summary.scanTotal);

  ImGui::SeparatorText("Rooms visited");
  for (const PortTracker::World& world : summary.worlds) {
    const PortTracker::Count rooms = {world.visited, world.total};
    DrawTrackerCount(world.name.c_str(), rooms);
    if (world.mapStation || world.current) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s%s%s", world.mapStation ? "map station" : "",
                          world.mapStation && world.current ? ", " : "",
                          world.current ? "you are here" : "");
    }
  }

  int unvisited = 0;
  for (const PortTracker::Room& room : summary.rooms) {
    unvisited += room.visited ? 0 : 1;
  }
  char header[160];
  std::snprintf(header, sizeof(header), "Rooms not yet visited in %s (%d)###trackerrooms",
                summary.currentWorld.c_str(), unvisited);
  if (ImGui::CollapsingHeader(header)) {
    if (summary.roomNamesLoading > 0) {
      ImGui::TextDisabled("Loading room names...");
    }
    for (const PortTracker::Room& room : summary.rooms) {
      if (!room.visited) {
        ImGui::BulletText("%s", room.name.c_str());
      }
    }
  }
}

void DrawSaveStatesTab() {
  ImGui::TextWrapped(
      "Save anywhere and load back to the same spot. A state holds what a memory card save "
      "holds (items, health, ammo, map, scans, doors and puzzles already solved, in-game time) "
      "plus where Samus stands and whether she is in morph ball. Loading rebuilds the room as "
      "a memory card load does, so enemies and moving parts start over. While the game is "
      "paused, a save or load waits until you unpause.");
  bool hotkeys = sSaveStateHotkeys;
  if (ImGui::Checkbox("F5 saves, F9 loads the selected slot", &hotkeys)) {
    sSaveStateHotkeys = hotkeys;
    MarkDirty();
  }
  const bool running = sStateManager != nullptr;
  if (!running) {
    ImGui::TextDisabled("Saving and loading need a running game.");
  }

  const int selected = PortSaveState::SelectedSlot();
  if (ImGui::BeginTable("##savestates", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Where");
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (int slot = 1; slot <= PortSaveState::kSlotCount; ++slot) {
      const PortSaveState::Info info = PortSaveState::SlotInfo(slot);
      ImGui::PushID(slot);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      char label[16];
      std::snprintf(label, sizeof(label), "%d", slot);
      if (ImGui::RadioButton(label, selected == slot)) {
        PortSaveState::SetSelectedSlot(slot);
      }
      ImGui::TableNextColumn();
      if (info.exists) {
        ImGui::Text("%s - %s%s", info.world.c_str(), info.room.c_str(),
                    info.morphed ? " (ball)" : "");
      } else {
        ImGui::TextDisabled("empty");
      }
      ImGui::TableNextColumn();
      if (info.exists) {
        const int total = static_cast< int >(info.playTime);
        ImGui::Text("%d:%02d:%02d", total / 3600, total / 60 % 60, total % 60);
      }
      ImGui::TableNextColumn();
      ImGui::BeginDisabled(!running);
      if (ImGui::SmallButton("Save")) {
        PortSaveState::SetSelectedSlot(slot);
        PortSaveState::RequestSave(slot);
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(!info.exists);
      if (ImGui::SmallButton("Load")) {
        PortSaveState::SetSelectedSlot(slot);
        PortSaveState::RequestLoad(slot);
      }
      ImGui::EndDisabled();
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  const PortSaveState::Info undo = PortSaveState::SlotInfo(PortSaveState::kUndoSlot);
  ImGui::BeginDisabled(!running || !undo.exists);
  if (ImGui::Button("Undo last load")) {
    PortSaveState::RequestLoad(PortSaveState::kUndoSlot);
  }
  ImGui::EndDisabled();
  if (undo.exists) {
    ImGui::SameLine();
    ImGui::TextDisabled("back to %s - %s", undo.world.c_str(), undo.room.c_str());
  }
  const std::string message = PortSaveState::LastMessage();
  if (!message.empty()) {
    ImGui::TextWrapped("%s", message.c_str());
  }
  ImGui::TextDisabled("Folder: %s", PortSaveState::Folder().c_str());
}

struct DebugPage {
  const char* name;
  void (*draw)();
};

const DebugPage kDebugPages[] = {
    {"Input", DrawInputTab},     {"Controls", PortControls::DrawTab},
    {"Render", DrawRenderTab},   {"Performance", DrawPerformanceTab},
    {"Extras", DrawExtrasTab},   {"Tracker", DrawTrackerTab},
    {"States", DrawSaveStatesTab}, {"Session", DrawSessionTab},
    {"Chat", DrawChatTab},       {"Debug", DrawDebugTab},
    {"VR", PortVr::DrawVrDebugTab},
};

// The innermost window under the finger that can actually scroll vertically,
// climbing out of child windows (a table, the page list) that cannot.
ImGuiWindow* ScrollableWindowAt(ImGuiWindow* window) {
  for (; window != nullptr; window = window->ParentWindow) {
    if (window->ScrollMax.y > 0.f && (window->Flags & ImGuiWindowFlags_NoScrollWithMouse) == 0) {
      return window;
    }
    if ((window->Flags & ImGuiWindowFlags_ChildWindow) == 0) {
      break;
    }
  }
  return nullptr;
}

bool IsResizeGrip(ImGuiWindow* window, ImGuiID id) {
  for (int n = 0; n < 4; ++n) {
    if (id == ImGui::GetWindowResizeCornerID(window, n) ||
        id == ImGui::GetWindowResizeBorderID(window, static_cast< ImGuiDir >(n))) {
      return true;
    }
  }
  return false;
}

// ImGui has no touch scrolling: a finger dragged down a page presses whatever
// it landed on and scrolls nothing. A mostly vertical drag is taken away from
// the widget it began on (so a button under it does not fire on release) and
// scrolls the window instead, and the finger's speed carries on as a fling
// after it lifts. A mostly horizontal drag is left alone, so sliders still
// work, and so are the scrollbar, a window being moved or resized, and drags
// that start outside the content area. Runs after NewFrame, before any window.
struct TouchScroll {
  ImGuiWindow* window = nullptr;
  bool decided = false;
  bool dragging = false;
  float velocity = 0.f; // pixels per second, positive scrolls down
};
TouchScroll sTouchScroll;

void UpdateTouchScroll() {
  ImGuiContext& g = *ImGui::GetCurrentContext();
  const ImGuiIO& io = g.IO;
  TouchScroll& scroll = sTouchScroll;
  const bool touch = io.MouseSource == ImGuiMouseSource_TouchScreen;

  if (io.MouseClicked[0]) {
    scroll = TouchScroll{};
    if (touch) {
      ImGuiWindow* window = ScrollableWindowAt(g.HoveredWindow);
      if (window != nullptr && window->InnerClipRect.Contains(io.MouseClickedPos[0])) {
        scroll.window = window;
      }
    }
  }
  if (scroll.window == nullptr) {
    return;
  }

  if (io.MouseDown[0]) {
    if (!scroll.decided) {
      const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f);
      // A share of the font size rather than io.MouseDragThreshold's fixed
      // pixels, which on a dense phone screen is a jitter, not a drag.
      const float threshold = g.FontSize * 0.6f;
      if (delta.x * delta.x + delta.y * delta.y < threshold * threshold) {
        return;
      }
      scroll.decided = true;
      ImGuiWindow* activeWindow = g.ActiveIdWindow;
      const bool ownDrag =
          g.MovingWindow != nullptr ||
          (g.ActiveId != 0 && activeWindow != nullptr &&
           (g.ActiveId == ImGui::GetWindowScrollbarID(activeWindow, ImGuiAxis_X) ||
            g.ActiveId == ImGui::GetWindowScrollbarID(activeWindow, ImGuiAxis_Y) ||
            IsResizeGrip(activeWindow, g.ActiveId)));
      if (std::fabs(delta.y) <= std::fabs(delta.x) || ownDrag) {
        scroll.window = nullptr;
        return;
      }
      scroll.dragging = true;
    }
    if (!scroll.dragging) {
      return;
    }
    if (g.ActiveId != 0) {
      ImGui::ClearActiveID();
    }
    ImGui::SetScrollY(scroll.window, scroll.window->Scroll.y - io.MouseDelta.y);
    if (io.DeltaTime > 0.f) {
      // Smoothed, so the last jittery frame before the finger lifts does not
      // decide the whole fling.
      const float instant = -io.MouseDelta.y / io.DeltaTime;
      scroll.velocity += (instant - scroll.velocity) * 0.4f;
    }
    return;
  }

  // Released: keep scrolling at the finger's speed, easing off.
  if (!scroll.dragging) {
    scroll.window = nullptr;
    return;
  }
  const float y = scroll.window->Scroll.y;
  const bool atEdge = (scroll.velocity < 0.f && y <= 0.f) ||
                      (scroll.velocity > 0.f && y >= scroll.window->ScrollMax.y);
  if (atEdge || std::fabs(scroll.velocity) < g.FontSize) {
    scroll = TouchScroll{};
    return;
  }
  ImGui::SetScrollY(scroll.window, y + scroll.velocity * io.DeltaTime);
  scroll.velocity *= std::exp(-4.f * io.DeltaTime);
}

// A finger that lifts leaves ImGui's cursor where it was, so whatever was last
// tapped stays drawn as hovered. Move the cursor off-screen once the release
// has been seen; queued now, it lands on the next frame.
void ClearTouchHover() {
  ImGuiIO& io = ImGui::GetIO();
  if (io.MouseSource == ImGuiMouseSource_TouchScreen && io.MouseReleased[0] &&
      !ImGui::IsAnyMouseDown()) {
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
  }
}

// Full screen inside the display's safe area (clear of the notch and the
// gesture bars), no title bar to drag, a Close button a thumb can hit, and a
// page list down the side in place of a tab strip too narrow to tap.
bool DrawPageWindow() {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 pos = viewport->WorkPos;
  ImVec2 size = viewport->WorkSize;
  SDL_Window* window = MainWindow();
  SDL_Rect safe;
  int windowWidth = 0, windowHeight = 0;
  if (window != nullptr && SDL_GetWindowSafeArea(window, &safe) &&
      SDL_GetWindowSize(window, &windowWidth, &windowHeight) && windowWidth > 0 &&
      windowHeight > 0 && safe.w > 0 && safe.h > 0) {
    // ImGui's display size need not be in window coordinates.
    const float sx = size.x / static_cast< float >(windowWidth);
    const float sy = size.y / static_cast< float >(windowHeight);
    pos = ImVec2(pos.x + safe.x * sx, pos.y + safe.y * sy);
    size = ImVec2(safe.w * sx, safe.h * sy);
  }
  ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoScrollbar |
                                      ImGuiWindowFlags_NoScrollWithMouse;
  bool open = true;
  if (ImGui::Begin("Metroid Prime Port##touch", nullptr, kFlags)) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const char* const kClose = "Close";
    const float closeWidth = ImGui::CalcTextSize(kClose).x + style.FramePadding.x * 4.f;
    const char* const kTitle = "Metroid Prime Port";
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(kTitle);
#if !defined(__ANDROID__)
    // Only when it fits beside the title and the Close button.
    const char* const kKeys = "F1: hide   F10: frame limit   F12: screenshot";
    if (ImGui::GetContentRegionAvail().x > ImGui::CalcTextSize(kTitle).x +
                                               ImGui::CalcTextSize(kKeys).x + closeWidth +
                                               style.ItemSpacing.x * 5.f) {
      ImGui::SameLine(0.f, style.ItemSpacing.x * 3.f);
      ImGui::TextDisabled("%s", kKeys);
    }
#endif
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - closeWidth);
    if (ImGui::Button(kClose, ImVec2(closeWidth, 0.f))) {
      open = false;
    }
    ImGui::Separator();

    static int sPage = 0;
    // MP_DEBUG_TAB=<name> opens on that page, for captures of the overlay.
    static const char* sStartPage = std::getenv("MP_DEBUG_TAB");
    if (sStartPage != nullptr) {
      for (int i = 0; i < static_cast< int >(ARRAY_SIZE(kDebugPages)); ++i) {
        if (SDL_strcasecmp(sStartPage, kDebugPages[i].name) == 0) {
          sPage = i;
        }
      }
      sStartPage = nullptr;
    }
    float listWidth = 0.f;
    for (const DebugPage& page : kDebugPages) {
      listWidth = std::max(listWidth, ImGui::CalcTextSize(page.name).x);
    }
    listWidth += style.FramePadding.x * 2.f + style.WindowPadding.x * 2.f;
    const float rowHeight = ImGui::GetFrameHeight() * 1.2f;
    if (ImGui::BeginChild("##pages", ImVec2(listWidth, 0.f), ImGuiChildFlags_Borders)) {
      for (int i = 0; i < static_cast< int >(ARRAY_SIZE(kDebugPages)); ++i) {
        if (ImGui::Selectable(kDebugPages[i].name, sPage == i, ImGuiSelectableFlags_None,
                              ImVec2(0.f, rowHeight))) {
          sPage = i;
        }
      }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    // Keyed by page, so each page keeps its own scroll position.
    ImGui::PushID(sPage);
    if (ImGui::BeginChild("##page", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders)) {
      kDebugPages[sPage].draw();
    }
    ImGui::EndChild();
    ImGui::PopID();
  }
  ImGui::End();
  return open;
}

bool DrawDesktopWindow() {
  ImGui::SetNextWindowPos(ImVec2(8.f, 8.f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(440.f, 200.f), ImGuiCond_FirstUseEver);
  if (std::getenv("MP_DEBUG_TAB") != nullptr) // a capture wants to see the tab
    ImGui::SetNextWindowSize(ImVec2(520.f, 620.f), ImGuiCond_Once);
  bool open = true;
  if (ImGui::Begin("Metroid Prime Port", &open, ImGuiWindowFlags_MenuBar)) {
    if (ImGui::BeginMenuBar()) {
      ImGui::TextUnformatted("F1: hide   F10: frame limit   F12: screenshot");
      ImGui::EndMenuBar();
    }

    if (ImGui::BeginTabBar("##debug_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
      // MP_DEBUG_TAB=<name> opens on that tab, for captures of the overlay.
      static const char* sStartTab = std::getenv("MP_DEBUG_TAB");
      for (const DebugPage& page : kDebugPages) {
        const bool start = sStartTab != nullptr && SDL_strcasecmp(sStartTab, page.name) == 0;
        if (ImGui::BeginTabItem(page.name, nullptr, start ? ImGuiTabItemFlags_SetSelected : 0)) {
          if (start)
            sStartTab = nullptr;
          page.draw();
          ImGui::EndTabItem();
        }
      }
      ImGui::EndTabBar();
    }
  }
  ImGui::End();
  return open;
}

// The in-game time in the bottom-right corner while a game runs.
void DrawSpeedrunTimer() {
  if (!sSpeedrunTimer || sStateManager == nullptr || gpGameState == nullptr) {
    return;
  }
  const long long cs =
      static_cast< long long >(std::floor(gpGameState->GetTotalPlayTime() * 100.0));
  char text[32];
  if (cs >= 360000) {
    std::snprintf(text, sizeof(text), "%lld:%02lld:%02lld.%02lld", cs / 360000, cs / 6000 % 60,
                  cs / 100 % 60, cs % 100);
  } else {
    std::snprintf(text, sizeof(text), "%lld:%02lld.%02lld", cs / 6000, cs / 100 % 60, cs % 100);
  }
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  ImFont* font = ImGui::GetFont();
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float size = std::max(ImGui::GetFontSize() * 1.5f, display.y * 0.035f);
  const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.f, text);
  const float margin = size * 0.6f;
  const ImVec2 pos(display.x - extent.x - margin, display.y - extent.y - margin);
  const float shadow = std::max(1.f, size / 12.f);
  draw->AddText(font, size, ImVec2(pos.x + shadow, pos.y + shadow), IM_COL32(0, 0, 0, 200), text);
  draw->AddText(font, size, pos, IM_COL32(255, 255, 255, 230), text);
}

void DrawUI() {
  EnsureInitialized();
  if (!sAudioSettingsApplied) {
    // Apply persisted audio mutes once the backends are alive.
    sAudioSettingsApplied = true;
    SetAiAudioEnabled(sAiAudioEnabled);
    SetMusyxAudioEnabled(sMusyxAudioEnabled);
  }
  if (!sPresentationSettingsApplied) {
    // Apply persisted vsync once the swapchain surface exists (first drawn
    // frame), so the present mode is chosen from real surface capabilities.
    sPresentationSettingsApplied = true;
    aurora_enable_vsync(sVsyncEnabled && !sTurbo);
  }
  if (sFullscreenHotkey.exchange(false, std::memory_order_acq_rel)) {
    SetFullscreen(!VIGetWindowFullscreen());
  }
#if !defined(__ANDROID__)
  else if (const int window = sWindowFullscreen.exchange(-1, std::memory_order_acq_rel);
           window >= 0 && (window != 0) != sFullscreen) {
    sFullscreen = window != 0;
    MarkDirty();
  }
#endif
  DrawSpeedrunTimer();
  ProcessCardPicks();
#if !defined(__ANDROID__)
  // Every frame, not only with the panel open: an importer stops when its
  // output pipe fills.
  PortImporters::Poll();
#endif
  if (!sVisible) {
    sTouchScroll = TouchScroll{};
    return;
  }

  UpdateTouchScroll();
  const bool open = PageLayout() ? DrawPageWindow() : DrawDesktopWindow();
  ClearTouchHover();

  if (!open) {
    sVisible = false;
  }

  if (sSettingsDirty) {
    SaveSettings();
  }
}

void LoadDiscPath() {
  const std::string path = SettingsFilePath();
  std::ifstream file(path);
  if (!file.is_open()) {
    return;
  }
  std::string line;
  while (std::getline(file, line)) {
    const size_t separator = line.find('=');
    if (separator == std::string::npos || Trim(line.substr(0, separator)) != "disc_path") {
      continue;
    }
    sDiscPath = Trim(line.substr(separator + 1));
    std::fprintf(stderr, "metroid_prime_port: saved disc image %s\n", sDiscPath.c_str());
    return;
  }
}

const char* DiscPath() {
  return sDiscPath.empty() ? nullptr : sDiscPath.c_str();
}

void SetDiscPath(const char* path) {
  sDiscPath = path != nullptr ? path : "";
  sSettingsDirty = true;
}

} // namespace PortDebug

#if defined(__ANDROID__)
// The touch overlay covers the display and consumes every touch before SDL
// sees it. While the debug overlay is open the game is paused and those touches
// belong to ImGui, so the Java side asks this and stops claiming them.
#if defined(__ANDROID__)
// The pad itself - descriptor, mapping, axis conversion, attach and detach -
// lives in platform/touch_pad.cpp so that it can be built and tested on the
// host. It used to be here, inside this #if, which meant it was never compiled
// anywhere except an Android build and no test could ever have caught a wrong
// button mapping. See tests/touch_pad.cpp.
namespace {
PortTouchPad::Pad g_touchPad;

PortTouchPad::Pad& TouchPad() {
  if (!g_touchPad.ok()) {
    g_touchPad = PortTouchPad::Attach();
    __android_log_print(ANDROID_LOG_INFO, "touchpad", "attached id=%d gamepad=%d open=%d",
                        g_touchPad.id, SDL_IsGamepad(g_touchPad.id) ? 1 : 0,
                        g_touchPad.ok() ? 1 : 0);
    if (!g_touchPad.ok())
      __android_log_print(ANDROID_LOG_ERROR, "touchpad", "attach failed: %s", SDL_GetError());
  }
  return g_touchPad;
}
} // namespace

// KNOWN LIMITATION: a short tap can be missed.
//
// The virtual joystick API is state-sampling, not event-queueing. Setting a
// button stores the latest value and marks it changed (SDL_virtualjoystick.c:401);
// the change is only delivered at the next update, which sends whatever the
// value is *then* (:742). So a press and release that both land between two
// updates leave only the release, and the game never sees the press. A quick tap
// on A or Start can therefore do nothing, most visibly while a game frame is
// stalled. Triggers behave the same way.
//
// This is not a data race - the setters hold SDL's joystick mutex, so nothing
// tears - and it is not specific to this port; it is how SDL's virtual joystick
// works. Sustained presses and ordinary releases are unaffected, which is why it
// has not shown up as "controls don't work".
//
// Fixing it properly means latching a press until the game has sampled it, and
// the latch has to be released on an update the port does not control. That is a
// real design problem, not a two-line patch, so it is recorded rather than
// half-solved. A missed tap is recoverable by tapping again; a control that fires
// when it should not is not.

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeVirtualButton(JNIEnv*, jclass, jint button,
                                                                 jboolean down) {
  if (SDL_Joystick* pad = TouchPad().handle) {
    SDL_SetJoystickVirtualButton(pad, static_cast< int >(button), down == JNI_TRUE);
  }
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeVirtualAxis(JNIEnv*, jclass, jint axis,
                                                               jfloat value) {
  if (SDL_Joystick* pad = TouchPad().handle) {
    SDL_SetJoystickVirtualAxis(pad, static_cast< int >(axis), PortTouchPad::AxisValue(value));
  }
}
#endif

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeDebugOverlayVisible(JNIEnv*, jclass) {
  return PortDebug::OverlayVisible() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeSetTouchDevice(JNIEnv*, jclass, jboolean xbox) {
  PortPrompts::NoteTouchInput(xbox == JNI_TRUE);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTwinStick(JNIEnv*, jclass) {
  return PortDebug::TwinStickFlag() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeToggleDebugOverlay(JNIEnv*, jclass) {
  PortDebug::RequestToggle();
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_MetroidPrimeActivity_nativeTexturePackStatus(JNIEnv* env, jclass,
                                                                       jstring status) {
  const char* chars = env->GetStringUTFChars(status, nullptr);
  if (chars != nullptr) {
    PortDebug::SetTexturePackStatus(chars);
    env->ReleaseStringUTFChars(status, chars);
  }
}

// The copy landed in <user root>.new; the next frame swaps it in.
extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_MetroidPrimeActivity_nativeTexturePackReady(JNIEnv*, jclass) {
  PortTextures::RequestUserPackReload();
}

// A file picked for the Remastered import, while this process lived. The
// address is also in RemasteredPickFile, for when it did not.
extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_MetroidPrimeActivity_nativeRemasteredPicked(JNIEnv* env, jclass, jint which,
                                                                      jstring uri) {
  const char* chars = env->GetStringUTFChars(uri, nullptr);
  if (chars != nullptr) {
    std::lock_guard lock(PortDebug::sRemasteredPickMutex);
    PortDebug::sRemasteredPicks.emplace_back(int(which), chars);
    env->ReleaseStringUTFChars(uri, chars);
  }
}

// Whether a real pad, keyboard or mouse was used since the last call.
extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTakePhysicalInput(JNIEnv*, jclass) {
  return sPhysicalInput.exchange(false, std::memory_order_acq_rel) ? JNI_TRUE : JNI_FALSE;
}
#endif

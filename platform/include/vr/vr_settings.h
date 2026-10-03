// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every user-facing VR setting, in one struct: the headset renderer's (from
// Wiicompiled VR's [vr] table) and PrimedGun's RuntimeSettings (NativeRuntime.h),
// with the same defaults PrimedGun shipped. Persisted in the port's
// port_settings.ini under vr_* keys (see vr_settings.cpp), read by the
// pacing thread, the input layer and the game hooks through a copy, and
// written by the F1 overlay, the headset panel and PrimedGun's own
// one-click calibrations.

#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

namespace PortVr {

enum class MirrorView : uint8_t { Normal, Both, Left, Right, None };
enum class ControllerModeSetting : uint8_t { PrimedGun, Gamepad, None };
enum class RumbleHand : uint8_t { Both, Left, Right };

struct PortVrSettings {
    // --- headset renderer (Wiicompiled [vr]) ---
    bool enabled = true;
    bool required = false;             // a failed headset start stops the game instead of falling back
    MirrorView mirror_view = MirrorView::Normal;
    ControllerModeSetting controller_mode = ControllerModeSetting::PrimedGun;
    uint32_t frame_interpolation_fps = 0; // 0 off, 1 auto, else 72/90/120 (Quest; later)
    float render_scale = 1.0f;         // of the runtime's recommended eye size
    float lean_back_degrees = 0.0f;
    bool passthrough = true;           // Quest only
    bool diagnostics_logging = false;  // [xr-diag]
    bool immersive_replay = true;      // draw the world per eye; off shows the mono image on the virtual screen
    std::string performance_level = "boost"; // XR_EXT_performance_settings
    // Where a virtual screen (menus, cinematics, pause/map when detached) hangs.
    float screen_distance_meters = 1.5f;  // PrimedGun ScreenDistance
    float screen_width_meters = 2.0f;     // PrimedGun ScreenSize: 1.5 m tall at 4:3
    bool cinematic_screen_enabled = true; // cutscenes on the virtual screen
    bool game_menu_screen_enabled = true; // pause / map / logbook / save detached onto it

    // --- PrimedGun RuntimeSettings (NativeRuntime.h:14-87) ---
    bool builtin_patches_enabled = true;
    bool patch_no_idle_sway = true;
    bool patch_disable_arm_cannon_idle_fidget = true;
    bool patch_cannon_rotation = true;
    bool patch_gun_ray_target = true;
    bool patch_reticle = true;
    bool use_right_hand = true;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float offset_z = 0.0f;
    float model_offset_x = 0.0f;
    float model_offset_y = 0.0f;
    float model_offset_z = 0.0f;
    float rot_offset_x = 0.0f;
    float rot_offset_y = 0.0f;
    float rot_offset_z = 0.0f;
    float world_scale = 1.50f;         // game units per metre (PrimedGun GFX_VR_UNITS_PER_METER)
    bool rumble_enabled = true;
    float rumble_intensity = 1.0f;
    RumbleHand rumble_hand = RumbleHand::Right;
    bool grip_inputs_enabled = true;
    bool grip_inputs_use_trackpad = false;
    float trackpad_press_threshold = 0.5f;
    float index_grip_press_threshold = 0.5f;
    bool combat_jump_use_primary_button = false;
    bool gun_targeting_enabled = true;
    float gun_targeting_distance = 60.0f;
    float gun_targeting_radius = 4.0f;
    bool visor_helmet_enabled = false;
    bool vr_overlays_enabled = true;
    bool height_prompt_enabled = true;
    bool vr_menu_hold_left_stick = false;
    bool vr_menu_requires_head_zone = false;
    bool vr_menu_floating = false;
    bool frustum_culling_enabled = true;
    float frustum_culling_degrees = 115.0f;
    float metroid_hud_distance = 0.75f;
    float metroid_hud_size = 0.75f;
    float metroid_hud_offset_up = 0.0f;
    float metroid_hud_offset_down = 0.0f;
    float metroid_hud_offset_left = 0.0f;
    float metroid_hud_offset_right = 0.0f;
    bool position_marker_enabled = true;
    bool xr_dpad_enabled = true;
    float xr_dpad_head_radius = 0.28f;
    float xr_dpad_head_y_below = 0.02f;
    float xr_dpad_deadzone = 0.45f;
    bool directional_movement_enabled = true;
    bool directional_movement_use_right_stick = false;
    bool directional_movement_use_hmd_direction = false;
    float directional_movement_deadzone = 0.25f;
    float directional_movement_speed = 14.0f;
    float directional_movement_accel = 45.0f;
    float directional_movement_air_accel = 8.0f;
    float look_yaw_sensitivity = 1.0f;
    bool snap_turn_enabled = false;
    int snap_turn_degrees = 45;
    int vr_state_slot = 1;
    int cannon_texture_slot = 0;
};

inline constexpr float kVrRenderScaleMin = 0.25f;
inline constexpr float kVrRenderScaleMax = 2.0f;
inline constexpr float kVrLeanBackDegreesLimit = 45.0f;

// Thread-safe copy of the current settings, and replacement of them. A
// replacement marks the port's settings file dirty (PortDebug::SaveSettingsNow
// writes it; the overlay writes it on exit as it does for every other setting).
PortVrSettings GetVrSettings() noexcept;
void SetVrSettings(const PortVrSettings& settings) noexcept;
// Hands aurora the settings it applies itself (the replay switch, the HUD's
// head-locked size and distance). Called after every change and at startup.
void PushVrSettingsToAurora() noexcept;
void ResetVrCalibrationOffsets() noexcept;
void ApplyVrSamusArmPreset() noexcept;

// The settings file: one vr_<name>=<value> line per field. ApplyVrSetting
// returns false for a key that is not a VR setting, so the overlay's parser can
// fall through to its own table; WriteVrSettings appends every VR line.
bool ApplyVrSetting(const std::string& key, const std::string& value) noexcept;
void WriteVrSettings(std::ostream& out);

// Environment overrides for one run, read once at startup: MP_VR=0|1,
// MP_VR_MIRROR=normal|both|left|right|none, MP_VR_LOG=1.
void ApplyVrEnvironmentOverrides() noexcept;

} // namespace PortVr

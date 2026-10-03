// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/vr_pad.h"

#include "vr/openxr_controller_snapshot.h"
#include "vr/openxr_integration.h"
#include "vr/openxr_screen_math.h"
#include "vr/vr_settings.h"

#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"

#include <dolphin/pad.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

namespace PortVr {
namespace {

using screen_math::Quat;
using screen_math::Vec3;

constexpr int8_t kStickFull = 100;   // PADClamp trims this to the octagon
constexpr uint8_t kTriggerFull = 255;
constexpr float kJumpStickThreshold = 0.55f;
constexpr uint32_t kGripGraceSamples = 30; // after gameplay starts, like PrimedGun
constexpr uint32_t kBeamPulseSamples = 8;

// The beam wheel's panel: 0.26 m ahead and 0.055 m above the aim pose at the
// moment the modifier was pressed, 0.42 m square; coordinates normalised by
// 0.21 m with a 0.25 deadzone, or the hand's travel / 0.075 m as a fallback.
constexpr float kPanelForward = 0.26f;
constexpr float kPanelUp = 0.055f;
constexpr float kPanelHalfExtent = 0.21f;
constexpr float kPanelHitLimit = 1.8f;
constexpr float kPanelDeadzone = 0.25f;
constexpr float kTravelScale = 0.075f;

enum class Beam : int { None = -1, Power = 0, Wave = 1, Ice = 2, Plasma = 3 };

struct WheelState {
    bool open = false;
    Quat base_orientation{0.0f, 0.0f, 0.0f, 1.0f};
    Vec3 base_position{};
    Vec3 panel_center{};
    bool first_sample = true;
    float zero_x = 0.0f;
    float zero_y = 0.0f;
    Beam selected = Beam::None;
    Beam pulse = Beam::None;
    uint32_t pulse_samples = 0;
};

std::mutex g_mutex;
VrPadState g_state;
WheelState g_wheel;
bool g_was_active = false;
bool g_recenter_was_pressed = false; // the right stick click's last state
bool g_was_gameplay = false;
uint32_t g_gameplay_samples = 0;

Quat Normalize(Quat q) noexcept {
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(length > 1.0e-6f)) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    return {q[0] / length, q[1] / length, q[2] / length, q[3] / length};
}

Quat Multiply(const Quat& a, const Quat& b) noexcept {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

// The orientation with its roll removed: forward kept, up re-levelled.
Quat RollFree(const Quat& q) noexcept {
    const Vec3 forward = screen_math::Rotate(q, {0.0f, 0.0f, -1.0f});
    Vec3 right{forward[2], 0.0f, -forward[0]}; // forward x world up
    const float length = std::sqrt(right[0] * right[0] + right[2] * right[2]);
    if (!(length > 1.0e-4f)) {
        return q;
    }
    right = {right[0] / length, 0.0f, right[2] / length};
    const Vec3 up{right[1] * forward[2] - right[2] * forward[1], right[2] * forward[0] - right[0] * forward[2],
                  right[0] * forward[1] - right[1] * forward[0]};
    // Rotation matrix columns (right, up, -forward) -> quaternion.
    const float m00 = right[0], m01 = up[0], m02 = -forward[0];
    const float m10 = right[1], m11 = up[1], m12 = -forward[1];
    const float m20 = right[2], m21 = up[2], m22 = -forward[2];
    const float trace = m00 + m11 + m22;
    Quat out{};
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        out = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        out = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        out = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        out = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return Normalize(out);
}

int8_t ToStick(float value) noexcept {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int8_t>(std::lround(clamped * kStickFull));
}

bool Pressed(float value) noexcept { return value > 0.5f; }

// A grip "press" per PrimedGun: the squeeze past half travel, or on a Valve
// Index the grip force / trackpad force against the configured thresholds.
bool GripPressed(const OpenXRControllerState& hand, const std::string& profile, const PortVrSettings& settings) {
    const bool index = profile.find("valve/index_controller") != std::string::npos;
    if (index) {
        if (settings.grip_inputs_use_trackpad) {
            return hand.trackpad_force > settings.trackpad_press_threshold || hand.trackpad_click;
        }
        return hand.squeeze_force > settings.index_grip_press_threshold || hand.squeeze_click;
    }
    return hand.squeeze_click || hand.squeeze_value > 0.5f;
}

void SetBeamCStick(Beam beam, PADStatus& pad) noexcept {
    switch (beam) {
    case Beam::Power:
        pad.substickY = kStickFull;
        break;
    case Beam::Wave:
        pad.substickX = kStickFull;
        break;
    case Beam::Ice:
        pad.substickY = static_cast<int8_t>(-kStickFull);
        break;
    case Beam::Plasma:
        pad.substickX = static_cast<int8_t>(-kStickFull);
        break;
    case Beam::None:
        break;
    }
}

// The beam wheel, run every sample so a release is seen. Returns true while
// the modifier is held (the wheel owns the weapon hand's B).
void UpdateWeaponWheel(const OpenXRControllerState& weapon, bool modifier, bool gameplay, PADStatus& pad) {
    WheelState& wheel = g_wheel;
    if (wheel.pulse_samples > 0) {
        SetBeamCStick(wheel.pulse, pad);
        if (--wheel.pulse_samples == 0) {
            wheel.pulse = Beam::None;
        }
    }
    if (!modifier || !gameplay) {
        if (wheel.open && wheel.selected != Beam::None) {
            wheel.pulse = wheel.selected;
            wheel.pulse_samples = kBeamPulseSamples;
            SetBeamCStick(wheel.pulse, pad);
        }
        wheel.open = false;
        wheel.selected = Beam::None;
        return;
    }
    const OpenXRPoseState& pose = weapon.aim_pose.valid ? weapon.aim_pose : weapon.grip_pose;
    if (!pose.valid) {
        return;
    }
    if (!wheel.open) {
        wheel.open = true;
        wheel.base_orientation = RollFree(Normalize(pose.orientation));
        wheel.base_position = pose.position;
        const Vec3 offset = screen_math::Rotate(wheel.base_orientation, {0.0f, kPanelUp, -kPanelForward});
        wheel.panel_center = {pose.position[0] + offset[0], pose.position[1] + offset[1],
                              pose.position[2] + offset[2]};
        wheel.first_sample = true;
        wheel.selected = Beam::None;
    }
    // The roll-free aim ray against the frozen panel's plane.
    const Quat current = RollFree(Normalize(pose.orientation));
    const Vec3 direction = screen_math::Rotate(current, {0.0f, 0.0f, -1.0f});
    const Vec3 panel_right = screen_math::Rotate(wheel.base_orientation, {1.0f, 0.0f, 0.0f});
    const Vec3 panel_up = screen_math::Rotate(wheel.base_orientation, {0.0f, 1.0f, 0.0f});
    const Vec3 panel_forward = screen_math::Rotate(wheel.base_orientation, {0.0f, 0.0f, -1.0f});
    const Vec3 to_panel{wheel.panel_center[0] - pose.position[0], wheel.panel_center[1] - pose.position[1],
                        wheel.panel_center[2] - pose.position[2]};
    const float denominator = screen_math::Dot(direction, panel_forward);
    float x = 0.0f;
    float y = 0.0f;
    bool hit = false;
    if (std::fabs(denominator) > 0.025f) {
        const float t = screen_math::Dot(to_panel, panel_forward) / denominator;
        if (t > 0.02f && t < 2.0f) {
            const Vec3 point{pose.position[0] + direction[0] * t - wheel.panel_center[0],
                             pose.position[1] + direction[1] * t - wheel.panel_center[1],
                             pose.position[2] + direction[2] * t - wheel.panel_center[2]};
            x = screen_math::Dot(point, panel_right) / kPanelHalfExtent;
            y = screen_math::Dot(point, panel_up) / kPanelHalfExtent;
            hit = std::fabs(x) <= kPanelHitLimit && std::fabs(y) <= kPanelHitLimit;
        }
    }
    if (!hit) {
        // The hand's travel in the base frame.
        const Vec3 travel = screen_math::Rotate(screen_math::Conjugate(wheel.base_orientation),
                                                {pose.position[0] - wheel.base_position[0],
                                                 pose.position[1] - wheel.base_position[1],
                                                 pose.position[2] - wheel.base_position[2]});
        x = travel[0] / kTravelScale;
        y = travel[1] / kTravelScale;
    }
    if (wheel.first_sample) {
        wheel.first_sample = false;
        wheel.zero_x = x;
        wheel.zero_y = y;
    }
    x = std::clamp(x - wheel.zero_x, -1.0f, 1.0f);
    y = std::clamp(y - wheel.zero_y, -1.0f, 1.0f);
    if (std::fabs(x) < kPanelDeadzone && std::fabs(y) < kPanelDeadzone) {
        wheel.selected = Beam::None;
    } else if (std::fabs(x) >= std::fabs(y)) {
        wheel.selected = x < 0.0f ? Beam::Plasma : Beam::Wave;
    } else {
        wheel.selected = y < 0.0f ? Beam::Ice : Beam::Power;
    }
}

// Classic: menus, the map, cutscenes, the morph ball. The plain GameCube
// layout on the two controllers.
void ApplyClassic(const OpenXRControllerState& left, const OpenXRControllerState& right, uint32_t weapon_hand,
                  const PortVrSettings& settings, const std::array<std::string, 2>& profiles, PADStatus& pad) {
    const OpenXRControllerState& weapon = weapon_hand == 1 ? right : left;
    const OpenXRControllerState& off = weapon_hand == 1 ? left : right;
    pad.stickX = ToStick(left.thumbstick_x);
    pad.stickY = ToStick(left.thumbstick_y);
    pad.substickX = ToStick(right.thumbstick_x);
    pad.substickY = ToStick(right.thumbstick_y);
    if (left.primary) pad.button |= PAD_BUTTON_X;
    if (left.secondary) pad.button |= PAD_BUTTON_START;
    if (right.primary) pad.button |= PAD_BUTTON_A;
    if (right.secondary) pad.button |= PAD_BUTTON_B;
    if (Pressed(off.trigger_value) || off.trigger_click) {
        pad.button |= PAD_TRIGGER_L;
        pad.triggerLeft = kTriggerFull;
    }
    if (Pressed(weapon.trigger_value) || weapon.trigger_click) {
        pad.button |= PAD_TRIGGER_R;
        pad.triggerRight = kTriggerFull;
    }
    if (settings.grip_inputs_enabled) {
        if (GripPressed(off, profiles[weapon_hand == 1 ? 0 : 1], settings)) pad.button |= PAD_TRIGGER_Z;
        if (GripPressed(weapon, profiles[weapon_hand], settings)) pad.button |= PAD_BUTTON_Y;
    }
}

// Gameplay and orbit lock: PrimedGun's first-person layout.
void ApplyGameplay(const OpenXRControllerState& left, const OpenXRControllerState& right, uint32_t weapon_hand,
                   bool orbit, bool grips_ready, const PortVrSettings& settings,
                   const std::array<std::string, 2>& profiles, PADStatus& pad) {
    const OpenXRControllerState& weapon = weapon_hand == 1 ? right : left;
    const OpenXRControllerState& off = weapon_hand == 1 ? left : right;
    const OpenXRControllerState& move = settings.directional_movement_use_right_stick ? right : left;
    const OpenXRControllerState& look = settings.directional_movement_use_right_stick ? left : right;
    const bool modifier = weapon.secondary; // the beam wheel
    const bool a_jump = settings.combat_jump_use_primary_button;

    // Forward / back on the move stick. Sideways: the orbit strafe in a lock,
    // otherwise the look stick's turn (directional movement, which takes the
    // move stick's X off the pad, is a later phase; until then both turn).
    pad.stickY = ToStick(move.thumbstick_y);
    if (orbit) {
        pad.stickX = ToStick(move.thumbstick_x);
    } else {
        float turn = move.thumbstick_x;
        if (!settings.snap_turn_enabled && !modifier) {
            const float sensitivity = std::min(settings.look_yaw_sensitivity, 1.0f);
            turn += look.thumbstick_x * sensitivity;
        }
        pad.stickX = ToStick(turn);
    }
    // Jump: the look stick pushed up, or A when the option says so.
    const bool jump = a_jump ? weapon.primary : (look.thumbstick_y > kJumpStickThreshold && !modifier);
    if (jump) {
        pad.button |= PAD_BUTTON_B;
        pad.analogB = kTriggerFull;
    }
    // Fire on the weapon trigger.
    if (Pressed(weapon.trigger_value) || weapon.trigger_click) {
        pad.button |= PAD_BUTTON_A;
        pad.analogA = kTriggerFull;
    }
    // Lock / scan on the off trigger.
    if (Pressed(off.trigger_value) || off.trigger_click) {
        pad.button |= PAD_TRIGGER_L;
        pad.triggerLeft = kTriggerFull;
    }
    // Morph ball on the off hand's X; with A-jump the off X is the jump instead.
    if (!a_jump && off.primary) pad.button |= PAD_BUTTON_X;
    if (a_jump && off.primary) {
        pad.button |= PAD_BUTTON_B;
        pad.analogB = kTriggerFull;
    }
    // Start on the off hand's Y, unless that hand is busy with the menu button
    // or the weapon modifier is held.
    if (off.secondary && !off.menu && !modifier) pad.button |= PAD_BUTTON_START;
    // Grips: map on the off hand, missile on the weapon hand, after the grace.
    if (settings.grip_inputs_enabled && grips_ready) {
        if (GripPressed(off, profiles[weapon_hand == 1 ? 0 : 1], settings)) pad.button |= PAD_TRIGGER_Z;
        if (GripPressed(weapon, profiles[weapon_hand], settings)) pad.button |= PAD_BUTTON_Y;
    }
}

} // namespace

void VrPadUpdate(const CStateManager* mgr) noexcept {
    const PortVrSettings settings = GetVrSettings();
    const bool active = settings.enabled && settings.controller_mode == ControllerModeSetting::PrimedGun &&
                        OpenXRIsRunning();
    OpenXRInputSnapshot snapshot;
    if (active) {
        snapshot = OpenXRGetInputSnapshot();
    }
    const bool controllers = active && snapshot.runtime_active &&
                             (snapshot.controllers[0].connected || snapshot.controllers[1].connected);
    if (!controllers) {
        if (g_was_active) {
            PADClearVirtualStatus(0);
            g_was_active = false;
        }
        std::lock_guard lock(g_mutex);
        g_state = {};
        g_wheel = {};
        g_gameplay_samples = 0;
        return;
    }
    g_was_active = true;
    // PrimedGun's one-click height set: the right stick click recenters the
    // immersive origin on where the head is now (position only, no yaw).
    {
        const bool pressed = snapshot.controllers[1].connected && snapshot.controllers[1].thumbstick_click;
        if (pressed && !g_recenter_was_pressed) {
            OpenXRRequestRecenter();
        }
        g_recenter_was_pressed = pressed;
    }

    // The mapping, from the game's own state.
    bool gameplay = false;
    bool orbit = false;
    if (mgr != nullptr && mgr->GetPlayer() != nullptr) {
        const CPlayer* player = mgr->GetPlayer();
        const bool first_person = player->GetCameraState() == CPlayer::kCS_FirstPerson;
        const bool unmorphed = player->GetMorphballTransitionState() == CPlayer::kMS_Unmorphed;
        const bool cinematic = mgr->GetCameraManager() != nullptr && mgr->GetCameraManager()->IsInCinematicCamera();
        gameplay = first_person && unmorphed && !cinematic && !mgr->GetInMapScreen();
        if (gameplay) {
            const CPlayer::EPlayerOrbitState state = player->GetOrbitState();
            orbit = state == CPlayer::kOS_OrbitObject || state == CPlayer::kOS_ForcedOrbitObject ||
                    state == CPlayer::kOS_OrbitPoint || state == CPlayer::kOS_OrbitCarcass;
        }
    }
    if (gameplay && !g_was_gameplay) {
        g_gameplay_samples = 0;
    }
    g_was_gameplay = gameplay;
    if (gameplay && g_gameplay_samples < kGripGraceSamples) {
        ++g_gameplay_samples;
    }
    const bool grips_ready = g_gameplay_samples >= kGripGraceSamples;

    const uint32_t weapon_hand = settings.use_right_hand ? 1u : 0u;
    const OpenXRControllerState& left = snapshot.controllers[0];
    const OpenXRControllerState& right = snapshot.controllers[1];
    const OpenXRControllerState& weapon = snapshot.controllers[weapon_hand];

    PADStatus pad{};
    pad.err = PAD_ERR_NONE;
    if (gameplay) {
        ApplyGameplay(left, right, weapon_hand, orbit, grips_ready, settings, snapshot.interaction_profiles, pad);
    } else {
        ApplyClassic(left, right, weapon_hand, settings, snapshot.interaction_profiles, pad);
    }
    // The beam wheel owns the weapon hand's B while it is held; the orbit lock
    // keeps its L (the game does the locking) and the pad's C-stick is free.
    UpdateWeaponWheel(weapon, gameplay && weapon.secondary, gameplay, pad);
    PADSetVirtualStatus(0, &pad);

    std::lock_guard lock(g_mutex);
    g_state.active = true;
    g_state.gameplay = gameplay;
    g_state.orbit_lock = orbit;
    g_state.weapon_panel = g_wheel.open;
    g_state.weapon_selected = static_cast<int>(g_wheel.selected);
    g_state.weapon_hand = weapon_hand;
}

VrPadState GetVrPadState() noexcept {
    std::lock_guard lock(g_mutex);
    return g_state;
}

} // namespace PortVr

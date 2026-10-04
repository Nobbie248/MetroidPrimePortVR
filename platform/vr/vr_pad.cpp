// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/vr_pad.h"

#include "vr/openxr_controller_snapshot.h"
#include "vr/openxr_integration.h"
#include "vr/openxr_screen_math.h"
#include "vr/vr_beam_wheel.h"
#include "vr/vr_settings.h"
#include "vr/vr_visor_dpad.h"

#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/ScriptObjects/CScriptGrapplePoint.hpp"
#include "MetroidPrime/TCastTo.hpp"

#include <dolphin/pad.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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

// The beam wheel's panel and pick (vr/vr_beam_wheel.h).
using BeamWheel::Beam;

struct WheelState {
    bool open = false;
    BeamWheel::Panel panel;
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
VisorDpad::Tracker g_visor;
bool g_was_active = false;
bool g_recenter_was_pressed = false; // the right stick click's last state
bool g_was_gameplay = false;
uint32_t g_gameplay_samples = 0;
std::atomic<bool> g_owns_motor{false}; // port 0's rumble goes to the headset controllers

// PADControlMotor's hook. While the headset controllers own port 0, its motor
// drives their haptics, on or off as PrimedGun's GCPad::SetOutput fed them,
// instead of the physical pad assigned to that port.
BOOL ClaimPortZeroMotor(u32 chan, u32 cmd) {
    if (chan != 0 || !g_owns_motor.load(std::memory_order_relaxed)) {
        return FALSE;
    }
    OpenXRSetRumble(cmd == PAD_MOTOR_RUMBLE ? 1.0f : 0.0f);
    return TRUE;
}

Quat Multiply(const Quat& a, const Quat& b) noexcept {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

int8_t ToStick(float value) noexcept {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int8_t>(std::lround(clamped * kStickFull));
}

bool Pressed(float value) noexcept { return value > 0.5f; }

double NowSeconds() noexcept {
    using namespace std::chrono;
    return duration< double >(steady_clock::now().time_since_epoch()).count();
}

// The guard at the top of CPlayer::UpdateVisorState: a visor press counts only
// unmorphed, out of a grapple and a grapple point lock, between transitions and
// outside a scan. Anything else the game drops.
bool VisorPressAccepted(const CStateManager& mgr, const CPlayer& player) {
    if (player.GetOrbitState() == CPlayer::kOS_Grapple ||
        TCastToConstPtr< CScriptGrapplePoint >(mgr.GetObjectById(player.GetOrbitTargetId()))) {
        return false;
    }
    return player.GetMorphballTransitionState() == CPlayer::kMS_Unmorphed &&
           !mgr.GetPlayerState()->GetIsVisorTransitioning() &&
           player.GetPlayerScanState() == CPlayer::kSS_NotScanning;
}

// The game's control map gives the visors to the D-pad (up combat, right
// X-ray, down thermal, left scan on the retail disc).
uint16_t DpadButton(VisorDpad::Dir dir) noexcept {
    switch (dir) {
    case VisorDpad::Dir::Up:
        return PAD_BUTTON_UP;
    case VisorDpad::Dir::Right:
        return PAD_BUTTON_RIGHT;
    case VisorDpad::Dir::Down:
        return PAD_BUTTON_DOWN;
    case VisorDpad::Dir::Left:
        return PAD_BUTTON_LEFT;
    default:
        return 0;
    }
}

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
    const std::array<int, 2> direction = BeamWheel::CStickDirection(beam);
    if (direction[0] != 0) {
        pad.substickX = static_cast<int8_t>(direction[0] * kStickFull);
    }
    if (direction[1] != 0) {
        pad.substickY = static_cast<int8_t>(direction[1] * kStickFull);
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
        wheel.panel = BeamWheel::OpenPanel(pose.position, pose.orientation);
        wheel.first_sample = true;
        wheel.selected = Beam::None;
    }
    // The roll-free aim ray against the frozen panel, read from where the
    // first sample landed: up Power, right Wave, down Ice, left Plasma.
    const std::array<float, 2> aim = BeamWheel::Measure(wheel.panel, pose.position, pose.orientation);
    if (wheel.first_sample) {
        wheel.first_sample = false;
        wheel.zero_x = aim[0];
        wheel.zero_y = aim[1];
    }
    wheel.selected = BeamWheel::Pick(aim[0] - wheel.zero_x, aim[1] - wheel.zero_y);
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
            // The motor goes back to port 0's pad, and the headset stops.
            g_owns_motor.store(false, std::memory_order_relaxed);
            OpenXRSetRumble(0.0f);
            g_was_active = false;
        }
        std::lock_guard lock(g_mutex);
        g_state = {};
        g_wheel = {};
        g_visor.Reset();
        g_gameplay_samples = 0;
        return;
    }
    if (!g_was_active) {
        // The pad that had port 0 may be mid-pulse, and its stop would now
        // reach the headset instead: stop it before the controllers take over.
        PADControlMotor(0, PAD_MOTOR_STOP_HARD);
        OpenXRSetRumble(0.0f);
        PADSetMotorCallback(ClaimPortZeroMotor);
        g_owns_motor.store(true, std::memory_order_relaxed);
    }
    g_was_active = true;

    // The mapping, from the game's own state.
    bool gameplay = false;
    bool orbit = false;
    bool visor_press_accepted = false;
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
            visor_press_accepted = VisorPressAccepted(*mgr, *player);
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
    const uint32_t visor_hand = weapon_hand == 1 ? 0u : 1u; // PrimedGun's D-pad hand: the off hand
    OpenXRControllerState left = snapshot.controllers[0];
    OpenXRControllerState right = snapshot.controllers[1];
    const OpenXRControllerState& weapon = snapshot.controllers[weapon_hand];

    // The visor gesture, in gameplay only (PrimedGun: first person, unmorphed).
    VisorDpad::Output visor;
    {
        const OpenXRControllerState& hand = snapshot.controllers[visor_hand];
        VisorDpad::Input in;
        in.armed = settings.xr_dpad_enabled && gameplay && hand.connected && hand.aim_pose.valid &&
                   snapshot.head_pose.valid;
        in.near_head = in.armed && VisorDpad::HandNearHead(hand.aim_pose.position, snapshot.head_pose.position,
                                                           settings.xr_dpad_head_radius,
                                                           settings.xr_dpad_head_y_below);
        in.stick_x = hand.thumbstick_x;
        in.stick_y = hand.thumbstick_y;
        in.deadzone = VisorDpad::EffectiveDeadzone(settings.xr_dpad_deadzone);
        in.accepting = visor_press_accepted;
        in.now = NowSeconds();
        visor = g_visor.Update(in);
    }
    // While the hand is at the head its stick is the D-pad's alone: no walking,
    // strafing, turning or jumping from it.
    if (visor.zone) {
        OpenXRControllerState& hand = visor_hand == 0 ? left : right;
        hand.thumbstick_x = 0.0f;
        hand.thumbstick_y = 0.0f;
    }

    // PrimedGun's one-click height set: the right stick click recenters the
    // immersive origin on where the head is now (position only, no yaw).
    // PrimedGun ignores it while the visor gesture is up.
    {
        const bool pressed = right.connected && right.thumbstick_click && !visor.zone;
        if (pressed && !g_recenter_was_pressed) {
            OpenXRRequestRecenter();
        }
        g_recenter_was_pressed = pressed;
    }

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
    pad.button |= DpadButton(visor.held);
    PADSetVirtualStatus(0, &pad);

    std::lock_guard lock(g_mutex);
    g_state.active = true;
    g_state.gameplay = gameplay;
    g_state.orbit_lock = orbit;
    g_state.weapon_panel = g_wheel.open;
    g_state.weapon_selected = static_cast<int>(g_wheel.selected);
    g_state.weapon_hand = weapon_hand;
    g_state.visor_zone = visor.zone;
    g_state.visor_direction = static_cast<int>(visor.direction);
}

VrPadState GetVrPadState() noexcept {
    std::lock_guard lock(g_mutex);
    return g_state;
}

} // namespace PortVr

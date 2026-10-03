// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pacing thread's view of the headset and controllers, published once per
// XR frame for the game thread: PrimedGun's Common::VR::OpenXRInputSnapshot
// (Common/VR/OpenXRInputState.h) rebuilt on top of Wiicompiled's action set.
// Poses are in the application reference space (metres, +Y up, -Z forward),
// relative to nothing: the game-side cannon and camera code subtracts the
// latched tracking base and converts to Prime's Z-up units itself.

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace PortVr {

struct OpenXRPoseState {
    bool valid = false;
    std::array<float, 3> position{};
    std::array<float, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w
};

struct OpenXRControllerState {
    bool connected = false;
    // Buttons, by position on the controller, independent of the profile.
    bool primary = false;   // A (right) / X (left)
    bool secondary = false; // B (right) / Y (left)
    bool menu = false;
    bool thumbstick_click = false;
    bool thumbstick_touch = false;
    bool trackpad_click = false;    // Index / Vive trackpads
    float trackpad_force = 0.0f;
    float trigger_value = 0.0f;
    bool trigger_click = false;
    float squeeze_value = 0.0f;     // grip
    bool squeeze_click = false;
    float squeeze_force = 0.0f;     // Index grip force
    float thumbstick_x = 0.0f;
    float thumbstick_y = 0.0f;      // +up
    OpenXRPoseState aim_pose;
    OpenXRPoseState grip_pose;
    bool velocity_valid = false;
    std::array<float, 3> linear_velocity{};
    std::array<float, 3> angular_velocity{};
};

struct OpenXRInputSnapshot {
    bool runtime_active = false;    // a session is running and focused
    uint64_t frame_serial = 0;
    int64_t sample_time_xr = 0;     // XrTime the poses were located at
    OpenXRPoseState head_pose;      // centre eye, same space as the controllers
    std::array<OpenXRControllerState, 2> controllers{}; // 0 left, 1 right
    std::array<std::string, 2> interaction_profiles{};
};

// Pacing thread writes, game thread reads; both take one short lock.
void OpenXRPublishInputSnapshot(const OpenXRInputSnapshot& snapshot) noexcept;
OpenXRInputSnapshot OpenXRGetInputSnapshot() noexcept;

// Game thread -> pacing thread: the game's rumble, from port 0's motor while
// the VR pad owns that port (vr_pad.cpp, on or off) or from the virtual
// gamepad's rumble in Gamepad mode. The pacing thread plays it on the hand(s)
// the settings name, scaled by the rumble intensity. amplitude 0 stops it.
void OpenXRSetRumble(float amplitude) noexcept;
float OpenXRTakeRumble() noexcept;

} // namespace PortVr

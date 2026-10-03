// SPDX-License-Identifier: GPL-3.0-or-later
//
// Backend-neutral controller sample and screen-hit vocabulary shared by the
// OpenXR input layer, the headset settings panel and the pad synthesis.
// Lifted from Wiicompiled VR's openxr_wii_remote.h; Metroid Prime has no Wii
// Remote, so only the generic pieces live here.

#pragma once

#include <cstdint>

namespace PortVr {

// What the headset's controllers are to the game.
enum class OpenXRControllerMode : uint8_t {
    // PrimedGun's modern scheme: the pacing thread's snapshot is turned into a
    // GameCube pad by vr_pad.cpp (gestures, grips, gun-hand triggers).
    PrimedGun,
    // One ordinary gamepad: A/B/X/Y, triggers, grips and sticks map 1:1 and
    // every binding in the port's controller tab applies.
    Gamepad,
    // Nothing to the game. The controllers still open the settings panel and
    // point at it.
    None,
};

struct ScreenHit {
    bool valid = false;
    float u = 0.0f; // -1..1 across the picture, +right; beyond +-1 off the edge
    float v = 0.0f; // -1..1, +up
    float distance_meters = 0.0f;
};

struct HandInputs {
    bool primary = false;   // A / X
    bool secondary = false; // B / Y
    bool menu = false;
    bool thumbstick_click = false;
    float trigger = 0.0f;
    float squeeze = 0.0f;
    float stick_x = 0.0f;
    float stick_y = 0.0f; // +up
};

// Analogue inputs count as pressed past half travel.
inline constexpr float kPressThreshold = 0.5f;

} // namespace PortVr

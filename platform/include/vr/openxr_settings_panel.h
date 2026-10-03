// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vr/openxr_hand_inputs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace PortVr {

// The settings panel shown inside the headset: the F10 settings, drawn by the
// settings overlay with an ImGui context of its own and laid over the eyes by
// Aurora (aurora_imgui_set_stereo_overlay). It sits on the virtual screen, the
// menu quad or the race's 2D screen, and is operated with the tracked
// controllers, which are withheld from the game while it is open.
//
// The XR pacing thread owns the controllers: it opens and closes the panel from
// the controllers' panel button, aims the pointer and publishes it here. The
// game thread draws the panel, may close it (its Close button) or open it (the
// F10 bar), and reads the pointer once per presented frame. Nothing in this
// header depends on OpenXR, so the settings overlay compiles the same in builds
// without it.

// The panel's canvas in ImGui pixels, drawn at twice the desktop menu's scale.
inline constexpr float kSettingsPanelWidthPixels = 1440.0f;
inline constexpr float kSettingsPanelHeightPixels = 1080.0f;
inline constexpr float kSettingsPanelUiScale = 2.0f;
// Its width as a fraction of the virtual screen's.
inline constexpr float kSettingsPanelWidthFraction = 0.75f;

struct OpenXRSettingsPanelPointer {
    bool valid = false;
    float x = 0.0f; // canvas pixels, +x right
    float y = 0.0f; // canvas pixels, +y down
    bool select = false;
    float wheel = 0.0f; // ImGui wheel steps since the last read, +up
};

// Either thread.
void OpenXRSetSettingsPanelOpen(bool open) noexcept;
bool OpenXRSettingsPanelOpen() noexcept;
// XR thread: this frame's pointer, and wheel steps to add to what is pending.
void OpenXRPublishSettingsPanelPointer(bool valid, float x, float y, bool select, float wheel) noexcept;
// Game thread: the latest pointer, taking the wheel steps accumulated since the last call.
OpenXRSettingsPanelPointer OpenXRTakeSettingsPanelPointer() noexcept;

namespace settings_panel {

using PortVr::HandInputs;

// Thumbstick deflection below this does not scroll.
inline constexpr float kScrollDeadzone = 0.25f;
// Wheel steps per second at full deflection. ImGui scrolls about five lines a step.
inline constexpr float kScrollStepsPerSecond = 8.0f;

// The panel's half extents, in metres, on a virtual screen `screen_width` metres across.
inline std::array<float, 2> HalfExtents(float screen_width) noexcept {
    const float half_width = 0.5f * screen_width * kSettingsPanelWidthFraction;
    return {half_width, half_width * kSettingsPanelHeightPixels / kSettingsPanelWidthPixels};
}

// A hit on the panel (-1..1 across it, +v up) in canvas pixels.
inline std::array<float, 2> CanvasPoint(const ScreenHit& hit) noexcept {
    return {0.5f * (hit.u + 1.0f) * kSettingsPanelWidthPixels, 0.5f * (1.0f - hit.v) * kSettingsPanelHeightPixels};
}

// What the controllers do this frame.
struct Frame {
    bool open = false;     // the panel is showing
    bool withheld = false; // the game must not see the controllers
    uint32_t pointing_hand = 1;
    bool select = false;
    float wheel = 0.0f;
};

// Whether the button that opens and closes the panel is held. As a Wii Remote
// that is left Y, which has no Wii button, and with the controllers nothing to
// the game it is left Y too; as a gamepad left Y is GameCube Y, so both
// thumbsticks clicked together stand in for it.
inline bool ToggleHeld(const std::array<HandInputs, 2>& hands, OpenXRControllerMode mode) noexcept {
    switch (mode) {
    case OpenXRControllerMode::PrimedGun:
        // PrimedGun: the off hand's thumbstick click or menu button (left Y is
        // Start in its layout).
        return hands[0].thumbstick_click || hands[0].menu;
    case OpenXRControllerMode::Gamepad:
        // Left Y is GameCube Y here, so both thumbsticks clicked together.
        return hands[0].thumbstick_click && hands[1].thumbstick_click;
    case OpenXRControllerMode::None:
        break;
    }
    return hands[0].secondary;
}

// The controller side of the panel, one Update per XR frame:
//
// - The panel button (ToggleHeld) opens or closes it. Nothing else opens it
//   from the controllers: every other button already means something to the
//   game.
// - While it is open, the left menu button also closes it, both triggers and
//   the A / X buttons select, the thumbsticks scroll, and the hand whose trigger
//   was pulled last is the one pointing.
// - The game gets the controllers back only once everything is released, so the
//   press that closed the panel never lands in the game as well.
class Controls {
public:
    // `open` is the shared flag: the panel button flips it, and the game thread
    // may have changed it since the last frame. `dt_seconds` is the time since
    // the previous frame; `mode` is how the game sees the controllers.
    Frame Update(const std::array<HandInputs, 2>& hands, bool& open, float dt_seconds,
                 OpenXRControllerMode mode) noexcept {
        const bool toggle = ToggleHeld(hands, mode);
        if (toggle && !m_toggle_held) {
            open = !open;
        }
        m_toggle_held = toggle;

        if (open && hands[0].menu && !m_menu_held && m_was_open) {
            open = false;
        }
        m_menu_held = hands[0].menu;

        for (uint32_t hand = 0; hand < 2; ++hand) {
            const bool pulled = hands[hand].trigger > kPressThreshold;
            if (pulled && !m_trigger_held[hand]) {
                m_pointing_hand = hand;
            }
            m_trigger_held[hand] = pulled;
        }

        if (open != m_was_open) {
            // Whatever is held across the change belongs to that change.
            m_release_pending = true;
        }
        if (m_release_pending && !AnyHeld(hands)) {
            m_release_pending = false;
        }
        m_was_open = open;

        Frame frame{};
        frame.open = open;
        frame.withheld = open || m_release_pending;
        frame.pointing_hand = m_pointing_hand;
        if (!open) {
            return frame;
        }
        // Opening held nothing that selects, but a trigger still down from the
        // game must not click the first thing under the pointer.
        frame.select = !m_release_pending && (m_trigger_held[0] || m_trigger_held[1] || hands[0].primary ||
                                              hands[1].primary);
        const float stick = std::fabs(hands[1].stick_y) >= std::fabs(hands[0].stick_y) ? hands[1].stick_y
                                                                                       : hands[0].stick_y;
        if (std::fabs(stick) > kScrollDeadzone) {
            const float magnitude = (std::fabs(stick) - kScrollDeadzone) / (1.0f - kScrollDeadzone);
            frame.wheel = std::copysign(magnitude, stick) * kScrollStepsPerSecond * std::clamp(dt_seconds, 0.0f, 0.1f);
        }
        return frame;
    }

    void Reset() noexcept { *this = Controls{}; }

    static bool AnyHeld(const std::array<HandInputs, 2>& hands) noexcept {
        for (const HandInputs& hand : hands) {
            if (hand.primary || hand.secondary || hand.menu || hand.thumbstick_click ||
                hand.trigger > kPressThreshold || hand.squeeze > kPressThreshold) {
                return true;
            }
        }
        return false;
    }

private:
    bool m_toggle_held = false;
    bool m_menu_held = false;
    std::array<bool, 2> m_trigger_held{};
    bool m_was_open = false;
    bool m_release_pending = false;
    uint32_t m_pointing_hand = 1;
};

} // namespace settings_panel

} // namespace PortVr

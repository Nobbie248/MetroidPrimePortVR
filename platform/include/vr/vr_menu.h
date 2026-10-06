// SPDX-License-Identifier: GPL-3.0-or-later
//
// PrimedGun's in-headset settings menu, header-only so the tests can check it
// without a headset or the game. platform/vr/vr_menu.cpp drives it on the game
// thread; the XR pacing thread places it and aims the pointer at it
// (openxr_input.cpp).
//
// The image is PrimedGun's own (Source/Core/VideoCommon/VR/
// PrimedGunOverlayCommon.h, BuildMenuPixels): a 1024x512 canvas rasterised on
// the CPU with a 5x7 bitmap font and solid rectangles that replace what is
// under them, alpha included. The colours are PrimedGun's constants unchanged.
// A uint32 written on a little-endian machine is the bytes R, G, B, A of an
// RGBA8 image, which is how PrimedGun uploaded it, so 0xE0FFB030 shows as
// #30B0FF at 224/255. Below the canvas the image carries the pointer's sprites
// (the laser and its hit dot), so the one panel swapchain feeds the menu's
// three quad layers.
//
// The behaviour is PrimedGun's UpdateVrMenu, AdjustVrMenuSetting and
// ActivateVrMenuSelection (Source/Core/Core/PrimedGun/NativeRuntime.cpp):
// - The row under the laser is the selected one.
// - A click on a numeric row's value box steps it down (left half) or up
//   (right half). A click anywhere else on a row toggles, cycles or runs it.
// - Resets, EXIT GAME and the save-state actions take a second click within
//   six seconds. PrimedGun counted that in 60 Hz frames; here it is seconds.
//
// One departure from PrimedGun: the two-page tabs (CALIBRATION, CONTROL) turn
// pages with PREVIOUS and NEXT buttons under the rows, where PrimedGun had a
// PAGE row at the top of the list.

#pragma once

#include "vr/openxr_screen_math.h"
#include "vr/vr_settings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace PortVr::VrMenu {

// --- The image ---

inline constexpr int kWidth = 1024;
inline constexpr int kMenuHeight = 512;
// The sprite strip sits eight rows below the canvas, so the menu layer's
// filtering never samples it at its bottom edge.
inline constexpr int kImageHeight = 560;

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// PrimedGun's laser texture was a 32x30 image whose first column is filled,
// shown 1 texel wide. Here it is a filled 4x32 block whose middle 2x30 is
// shown, so the layer's filtering finds the laser's colour on every side.
inline constexpr Rect kLaserBlock{0, 520, 4, 32};
inline constexpr Rect kLaserRect{1, 521, 2, 30};
// The hit dot: PrimedGun's circle in a 24x24 square.
inline constexpr Rect kDotRect{12, 524, 24, 24};
inline constexpr uint32_t kPointerColor = 0xE080D8FFu;

using Pixels = std::vector<uint32_t>;

inline std::array<uint8_t, 7> Glyph(char ch) {
    switch (ch) {
    case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    case 'C': return {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F};
    case 'D': return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    case 'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    case 'G': return {0x0F, 0x10, 0x10, 0x13, 0x11, 0x11, 0x0F};
    case 'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'I': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
    case 'J': return {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E};
    case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    case 'Q': return {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
    case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'V': return {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04};
    case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11};
    case 'X': return {0x11, 0x0A, 0x04, 0x04, 0x04, 0x0A, 0x11};
    case 'Y': return {0x11, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04};
    case 'Z': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
    case '0': return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    case '1': return {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    case '2': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    case '3': return {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
    case '4': return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    case '5': return {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
    case '6': return {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    case '7': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    case '8': return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    case '9': return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
    case '.': return {0, 0, 0, 0, 0, 0x0C, 0x0C};
    case '-': return {0, 0, 0, 0x1F, 0, 0, 0};
    case '+': return {0, 0x04, 0x04, 0x1F, 0x04, 0x04, 0};
    case '/': return {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10};
    default: return {};
    }
}

// Writes `color` over the rectangle, clipped to width x height; `width` is
// also the image's row length.
inline void FillRect(Pixels& pixels, int width, int height, int x, int y, int w, int h, uint32_t color) {
    const int x0 = std::clamp(x, 0, width);
    const int y0 = std::clamp(y, 0, height);
    const int x1 = std::clamp(x + w, 0, width);
    const int y1 = std::clamp(y + h, 0, height);
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            pixels[static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px)] = color;
        }
    }
}

// Every character advances six cells, the ones the font lacks (space, '?')
// drawing nothing, as in PrimedGun.
inline int TextWidth(const char* text, int scale) {
    const int chars = static_cast<int>(std::strlen(text));
    return chars > 0 ? ((chars * 6) - 1) * scale : 0;
}

inline void DrawString(Pixels& pixels, int width, int height, const char* text, int x, int y, int scale,
                       uint32_t color) {
    int cursor = x;
    for (const char* p = text; *p; ++p) {
        const auto rows = Glyph(*p);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((rows[static_cast<size_t>(row)] & (1u << (4 - col))) != 0) {
                    FillRect(pixels, width, height, cursor + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
        cursor += 6 * scale;
    }
}

// PrimedGun's FloatText: any precision but 1 and 3 prints two decimals, so
// the culling cone reads "115.00".
inline std::string FloatText(float value, int precision) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), precision == 3 ? "%.3f" : precision == 1 ? "%.1f" : "%.2f",
                  static_cast<double>(value));
    return buffer;
}

// --- The menu's state ---

enum Tab : uint32_t {
    kLayoutTab = 0,
    kCalibrationTab = 1,
    kControlTab = 2,
    kMovementTab = 3,
    kTexturesTab = 4,
    kStatesTab = 5,
};
inline constexpr uint32_t kTabCount = 6;
inline constexpr uint32_t kPageCount = 2;
inline constexpr uint32_t kCalibrationFirstPageItems = 12;
inline constexpr uint32_t kCalibrationTotalItems = 24;
inline constexpr uint32_t kControlFirstPageItems = 8;
inline constexpr uint32_t kControlTotalItems = 16;
// The two-page tabs' PREVIOUS (left) and NEXT (right) buttons, under the
// longest list and clear of the bottom bar, the page number between them.
inline constexpr int kPageButtonY = 458;
inline constexpr int kPageButtonWidth = 220;
inline constexpr int kPageButtonHeight = 28;
inline constexpr int kPreviousButtonX = 52;
inline constexpr int kNextButtonX = 52 + 920 - kPageButtonWidth; // right-aligned with the rows
inline constexpr uint32_t kMovementItems = 11;
inline constexpr uint32_t kCannonSlotCount = 6; // DEFAULT, SLOT 1-4, CUSTOM
inline constexpr uint32_t kStateActionRows = 4;
// PortSaveState's slots 1..8 (PrimedGun's Dolphin had ten).
inline constexpr uint32_t kStateSlotCount = 8;

// The two-press actions. The resets and EXIT GAME share one confirmation, the
// save states have their own.
enum ResetAction : uint32_t {
    kNoReset = 0,
    kResetAll = 1,
    kResetTargeting = 2,
    kResetCalibration = 3,
    kResetController = 4,
    kResetMovement = 5,
    kExitGame = 6,
};
enum StateAction : uint32_t {
    kNoStateAction = 0,
    kLoadState = 1,
    kSaveState = 2,
    kLoadNewest = 3,
    kSaveOldest = 4,
};

inline constexpr double kConfirmSeconds = 6.0;     // PrimedGun: 360 frames
inline constexpr double kSavedNoticeSeconds = 3.0; // 180 frames
inline constexpr double kLongPressSeconds = 1.0;   // 60 frames

struct State {
    uint32_t tab = kLayoutTab;
    uint32_t selected = 0;
    uint32_t calibration_page = 0;
    uint32_t control_page = 0;
    uint32_t reset_confirm = kNoReset;
    double reset_confirm_until = 0.0;
    uint32_t state_confirm = kNoStateAction;
    double state_confirm_until = 0.0;
    double saved_notice_until = 0.0;
};

// What the menu does outside its own state and the settings. The game thread
// implements it (vr_menu.cpp); the tests record the calls.
class Actions {
public:
    virtual ~Actions() = default;
    // Writes the settings file now.
    virtual void SaveSettings() = 0;
    // Saves the settings and leaves the game the way closing its window does.
    virtual void ExitGame() = 0;
    // The save-state slot the actions use, 1..kStateSlotCount.
    virtual int StateSlot() const = 0;
    virtual void SelectStateSlot(int slot) = 0;
    virtual void LoadState(int slot) = 0;
    virtual void SaveState(int slot) = 0;
    virtual void LoadNewestState() = 0;
    virtual void SaveOldestState() = 0;
    // Puts the cannon texture slot (0 DEFAULT .. 5 CUSTOM) in place; false if
    // it could not.
    virtual bool ApplyCannonSlot(int slot) = 0;
};

// --- Rows ---

// A row's place in its tab's whole list, both pages together (PrimedGun's
// numbering, which its settings and resets are keyed on).
inline int CalibrationActualIndex(const State& s, uint32_t local) {
    return static_cast<int>(s.calibration_page == 0 ? local : kCalibrationFirstPageItems + local);
}

inline int ControlActualIndex(const State& s, uint32_t local) {
    return static_cast<int>(s.control_page == 0 ? local : kControlFirstPageItems + local);
}

inline bool HasPages(uint32_t tab) { return tab == kCalibrationTab || tab == kControlTab; }

// The page a two-page tab shows (0 for the others).
inline uint32_t Page(const State& s) {
    return s.tab == kCalibrationTab ? s.calibration_page : s.tab == kControlTab ? s.control_page : 0;
}

inline uint32_t ItemCount(const State& s) {
    switch (s.tab) {
    case kCalibrationTab:
        return s.calibration_page == 0 ? kCalibrationFirstPageItems
                                       : kCalibrationTotalItems - kCalibrationFirstPageItems;
    case kControlTab:
        return s.control_page == 0 ? kControlFirstPageItems : kControlTotalItems - kControlFirstPageItems;
    case kMovementTab:
        return kMovementItems;
    case kTexturesTab:
        return kCannonSlotCount;
    case kStatesTab:
        return kStateActionRows + kStateSlotCount;
    default:
        return 0;
    }
}

// The top of row `index`'s text; the save-state slots sit 18 pixels lower.
inline int RowTextY(uint32_t tab, uint32_t index) {
    int y = 146 + static_cast<int>(index) * 22;
    if (tab == kStatesTab && index >= kStateActionRows) {
        y += 18;
    }
    return y;
}

// The row a texture y falls on: the nearest text top within 13 pixels, as in
// PrimedGun (which measures from the text's top, not the row's middle).
inline int RowFromTextureY(uint32_t tab, float texture_y, uint32_t item_count) {
    int best_index = -1;
    float best_distance = 13.0f;
    for (uint32_t i = 0; i < item_count; ++i) {
        const float distance = std::fabs(texture_y - static_cast<float>(RowTextY(tab, i)));
        if (distance <= best_distance) {
            best_distance = distance;
            best_index = static_cast<int>(i);
        }
    }
    return best_index;
}

inline bool RowIsNumeric(const State& s, uint32_t index) {
    switch (s.tab) {
    case kCalibrationTab: {
        const int actual = CalibrationActualIndex(s, index);
        return (actual >= 1 && actual <= 6) || actual == 10 || (actual >= 12 && actual <= 17);
    }
    case kControlTab: {
        const int actual = ControlActualIndex(s, index);
        return actual == 3 || actual == 9 || (actual >= 11 && actual <= 14);
    }
    case kMovementTab:
        return (index >= 3 && index <= 7) || index == 9;
    default:
        return false;
    }
}

inline const char* RumbleHandText(RumbleHand hand) {
    switch (hand) {
    case RumbleHand::Left:
        return "LEFT";
    case RumbleHand::Right:
        return "RIGHT";
    case RumbleHand::Both:
        break;
    }
    return "BOTH";
}

inline const char* OnOff(bool value) { return value ? "ON" : "OFF"; }

struct Row {
    const char* label = "";
    std::string value;
};

inline std::string ConfirmText(const State& s, uint32_t action) {
    return s.reset_confirm == action ? "ARE YOU SURE?" : "PRESS";
}

inline std::vector<Row> BuildRows(const State& s, const PortVrSettings& v, int state_slot) {
    std::vector<Row> rows;
    switch (s.tab) {
    case kCalibrationTab:
        if (s.calibration_page == 0) {
            rows.push_back({"CUTSCENE CINEMA SCREEN", OnOff(v.cinematic_screen_enabled)});
            rows.push_back({"HUD DISTANCE", FloatText(v.metroid_hud_distance, 2)});
            rows.push_back({"HUD SIZE", FloatText(v.metroid_hud_size, 2)});
            rows.push_back({"HUD VERTICAL", FloatText(v.metroid_hud_offset_up - v.metroid_hud_offset_down, 2)});
            rows.push_back(
                {"HUD HORIZONTAL", FloatText(v.metroid_hud_offset_right - v.metroid_hud_offset_left, 2)});
            rows.push_back({"TARGET DISTANCE", FloatText(v.gun_targeting_distance, 1)});
            rows.push_back({"TARGET RADIUS", FloatText(v.gun_targeting_radius, 1)});
            rows.push_back({"VISOR HELMET", OnOff(v.visor_helmet_enabled)});
            rows.push_back({"HEIGHT PROMPT", OnOff(v.height_prompt_enabled)});
            rows.push_back({"FRUSTUM CULLING", OnOff(v.frustum_culling_enabled)});
            rows.push_back({"CULLING CONE", FloatText(v.frustum_culling_degrees, 0)});
            rows.push_back({"RESET TARGETING", ConfirmText(s, kResetTargeting)});
        } else {
            rows.push_back({"POSITION LEFT/RIGHT", FloatText(v.model_offset_x, 3)});
            rows.push_back({"POSITION FORWARD/BACK", FloatText(v.model_offset_y, 3)});
            rows.push_back({"POSITION UP/DOWN", FloatText(v.model_offset_z, 3)});
            rows.push_back({"ROTATION PITCH", FloatText(v.rot_offset_x, 1)});
            rows.push_back({"ROTATION YAW", FloatText(v.rot_offset_y, 1)});
            rows.push_back({"ROTATION ROLL", FloatText(v.rot_offset_z, 1)});
            rows.push_back({"FLOOR POSITION MARKER", OnOff(v.position_marker_enabled)});
            rows.push_back({"RESET CALIBRATION", ConfirmText(s, kResetCalibration)});
            rows.push_back({"DEFAULT ARM PRESET", "APPLY"});
            rows.push_back({"SAMUS ARM PRESET", "APPLY"});
            rows.push_back({"DETACH VR MENU FROM HAND", OnOff(v.vr_menu_floating)});
            rows.push_back({"DETACH GAME MENU AND MAP", OnOff(v.game_menu_screen_enabled)});
        }
        break;
    case kControlTab:
        if (s.control_page == 0) {
            rows.push_back({"RIGHT HAND", OnOff(v.use_right_hand)});
            rows.push_back({"RUMBLE", OnOff(v.rumble_enabled)});
            rows.push_back({"RUMBLE TARGET", RumbleHandText(v.rumble_hand)});
            rows.push_back({"RUMBLE INTENSITY", FloatText(v.rumble_intensity, 2)});
            rows.push_back({"GRIP INPUT", OnOff(v.grip_inputs_enabled)});
            rows.push_back({"A BUTTON JUMP", OnOff(v.combat_jump_use_primary_button)});
            rows.push_back({"LONGER HELD PRESS FOR VR MENU", OnOff(v.vr_menu_hold_left_stick)});
            rows.push_back({"MENU REQUIRES HAND NEAR HEAD", OnOff(v.vr_menu_requires_head_zone)});
        } else {
            rows.push_back({"INDEX GRIP SOURCE", v.grip_inputs_use_trackpad ? "TRACKPAD" : "GRIP"});
            rows.push_back({"INDEX TOUCHPAD PRESSURE", FloatText(v.trackpad_press_threshold, 2)});
            rows.push_back({"VISOR GESTURE", OnOff(v.xr_dpad_enabled)});
            rows.push_back({"INDEX GRIP PRESSURE", FloatText(v.index_grip_press_threshold, 2)});
            rows.push_back({"HEAD RADIUS", FloatText(v.xr_dpad_head_radius, 2)});
            rows.push_back({"HEAD BELOW", FloatText(v.xr_dpad_head_y_below, 2)});
            rows.push_back({"STICK DEADZONE", FloatText(v.xr_dpad_deadzone, 2)});
            rows.push_back({"RESET CONTROLLER", ConfirmText(s, kResetController)});
        }
        break;
    case kMovementTab:
        rows.push_back({"LEFT STICK STRAFE", OnOff(v.directional_movement_enabled)});
        rows.push_back({"MOVEMENT STICK", v.directional_movement_use_right_stick ? "RIGHT" : "LEFT"});
        rows.push_back({"MOVEMENT DIRECTION", v.directional_movement_use_hmd_direction ? "HEADSET" : "CONTROLLER"});
        rows.push_back({"MOVEMENT DEADZONE", FloatText(v.directional_movement_deadzone, 2)});
        rows.push_back({"MOVEMENT SPEED", FloatText(v.directional_movement_speed, 1)});
        rows.push_back({"MOVEMENT ACCELERATION", FloatText(v.directional_movement_accel, 1)});
        rows.push_back({"AIR ACCELERATION", FloatText(v.directional_movement_air_accel, 1)});
        rows.push_back({"LOOK YAW SENSITIVITY", FloatText(v.look_yaw_sensitivity, 2)});
        rows.push_back({"SNAP TURN", OnOff(v.snap_turn_enabled)});
        rows.push_back({"SNAP TURN ANGLE", std::to_string(v.snap_turn_degrees)});
        rows.push_back({"RESET MOVEMENT", ConfirmText(s, kResetMovement)});
        break;
    case kTexturesTab: {
        static constexpr const char* kLabels[kCannonSlotCount] = {"DEFAULT", "SLOT 1", "SLOT 2",
                                                                  "SLOT 3",  "SLOT 4", "CUSTOM"};
        for (uint32_t slot = 0; slot < kCannonSlotCount; ++slot) {
            rows.push_back({kLabels[slot], v.cannon_texture_slot == static_cast<int>(slot) ? "SELECTED" : "SELECT"});
        }
        break;
    }
    case kStatesTab: {
        const std::string selected_slot = "SLOT " + std::to_string(state_slot);
        rows.push_back({"LOAD STATE", s.state_confirm == kLoadState ? "ARE YOU SURE?" : selected_slot});
        rows.push_back({"SAVE STATE", s.state_confirm == kSaveState ? "ARE YOU SURE?" : selected_slot});
        rows.push_back({"LOAD NEWEST", s.state_confirm == kLoadNewest ? "ARE YOU SURE?" : "PRESS"});
        rows.push_back({"SAVE OLDEST", s.state_confirm == kSaveOldest ? "ARE YOU SURE?" : "PRESS"});
        static constexpr const char* kSlots[kStateSlotCount] = {"SLOT 1", "SLOT 2", "SLOT 3", "SLOT 4",
                                                                "SLOT 5", "SLOT 6", "SLOT 7", "SLOT 8"};
        for (uint32_t slot = 1; slot <= kStateSlotCount; ++slot) {
            rows.push_back({kSlots[slot - 1], state_slot == static_cast<int>(slot) ? "SELECTED" : "SELECT"});
        }
        break;
    }
    default:
        break;
    }
    return rows;
}

// --- Drawing ---

inline void DrawLayoutTextPage(Pixels& pixels) {
    constexpr int width = kWidth;
    constexpr int height = kMenuHeight;
    constexpr uint32_t title_color = 0xFFFFE6B8u;
    constexpr uint32_t body_color = 0xFFD8C0A0u;
    constexpr uint32_t accent_color = 0xFF40F0E0u;
    constexpr uint32_t row_color = 0x50201810u;
    constexpr uint32_t rule_color = 0x80FFB030u;

    constexpr const char* title = "CONTROLLER LAYOUT";
    DrawString(pixels, width, height, title, (width - TextWidth(title, 3)) / 2, 124, 3, title_color);

    constexpr const char* visor_help = "PLACE OFFHAND NEAR YOUR HEAD AND USE THE CONTROL STICK TO CHANGE VISORS";
    DrawString(pixels, width, height, visor_help, (width - TextWidth(visor_help, 2)) / 2, 174, 2, accent_color);

    constexpr int left_x = 72;
    constexpr int right_x = 570;
    constexpr int heading_y = 220;
    DrawString(pixels, width, height, "LEFT HAND", left_x, heading_y, 2, title_color);
    DrawString(pixels, width, height, "RIGHT HAND", right_x, heading_y, 2, title_color);
    FillRect(pixels, width, height, left_x, heading_y + 24, 360, 3, rule_color);
    FillRect(pixels, width, height, right_x, heading_y + 24, 360, 3, rule_color);

    constexpr std::array<const char*, 6> left_rows = {
        "LEFT STICK - MOVE",          "Y BUTTON - START/PAUSE", "X BUTTON - MORPH BALL",
        "LEFT STICK CLICK - VR SETTINGS", "LEFT GRIP - MAP",    "L TRIGGER - LOCK ON"};
    constexpr std::array<const char*, 6> right_rows = {
        "RIGHT STICK - LOOK/JUMP",        "B BUTTON - BEAM SELECT", "A BUTTON - SELECT",
        "RIGHT STICK CLICK - SET HEIGHT", "RIGHT GRIP - MISSILES", "RIGHT TRIGGER - SHOOT"};
    for (int i = 0; i < 6; ++i) {
        const int y = 262 + i * 34;
        FillRect(pixels, width, height, left_x - 14, y - 8, 398, 26, row_color);
        FillRect(pixels, width, height, right_x - 14, y - 8, 398, 26, row_color);
        FillRect(pixels, width, height, left_x - 14, y - 8, 5, 26, rule_color);
        FillRect(pixels, width, height, right_x - 14, y - 8, 5, 26, rule_color);
        DrawString(pixels, width, height, left_rows[static_cast<size_t>(i)], left_x, y, 2, body_color);
        DrawString(pixels, width, height, right_rows[static_cast<size_t>(i)], right_x, y, 2, body_color);
    }
}

// The pointer's sprites below the canvas; the rest of the strip stays clear.
inline void DrawPointerSprites(Pixels& pixels) {
    FillRect(pixels, kWidth, kImageHeight, kLaserBlock.x, kLaserBlock.y, kLaserBlock.w, kLaserBlock.h, kPointerColor);
    for (int y = 0; y < kDotRect.h; ++y) {
        for (int x = 0; x < kDotRect.w; ++x) {
            // PrimedGun's circle: centre (15.5, 14.5) in a square placed at (4, 3).
            const float dx = static_cast<float>(x) - 11.5f;
            const float dy = static_cast<float>(y) - 11.5f;
            if (dx * dx + dy * dy <= 144.0f) {
                pixels[static_cast<size_t>(kDotRect.y + y) * kWidth + static_cast<size_t>(kDotRect.x + x)] =
                    kPointerColor;
            }
        }
    }
}

// The whole kWidth x kImageHeight image. `saved_notice` shows SETTINGS SAVED.
inline Pixels BuildPixels(const State& s, const PortVrSettings& v, int state_slot, bool saved_notice) {
    constexpr int width = kWidth;
    constexpr int height = kMenuHeight;
    Pixels pixels(static_cast<size_t>(kWidth) * kImageHeight, 0u);
    FillRect(pixels, width, height, 0, 0, width, height, 0xD0100804u);
    FillRect(pixels, width, height, 0, 0, width, 10, 0xE0FFB030u);
    FillRect(pixels, width, height, 0, height - 10, width, 10, 0xE0FFB030u);
    DrawString(pixels, width, height, "PRIMEDGUN SETTINGS", 48, 28, 4, 0xFFFFD8A0u);

    constexpr const char* tabs[kTabCount] = {"LAYOUT", "CALIBRATION", "CONTROL", "MOVEMENT", "TEXTURES", "STATES"};
    constexpr int tab_width = 150;
    constexpr int tab_step = 166;
    for (int i = 0; i < static_cast<int>(kTabCount); ++i) {
        const int x = 22 + i * tab_step;
        const bool active = i == static_cast<int>(s.tab);
        FillRect(pixels, width, height, x, 64, tab_width, 38, active ? 0xD04A2C12u : 0x7030180Cu);
        FillRect(pixels, width, height, x, 98, tab_width, 4, active ? 0xFFFFB030u : 0x604A2C12u);
        DrawString(pixels, width, height, tabs[i], x + (tab_width - TextWidth(tabs[i], 2)) / 2, 75, 2,
                   active ? 0xFFFFE6B8u : 0xFFD8C0A0u);
    }

    const auto draw_button = [&](const char* label, int x, int y, int w) {
        FillRect(pixels, width, height, x, y, w, 28, 0x9030180Cu);
        FillRect(pixels, width, height, x, y, 5, 28, 0x80FFB030u);
        DrawString(pixels, width, height, label, x + (w - TextWidth(label, 2)) / 2, y + 7, 2, 0xFFFFE6B8u);
    };

    // EXIT GAME sits on the title row, so it is there on every tab, the Layout
    // tab the menu opens on included.
    constexpr int exit_button_x = 752;
    draw_button(s.reset_confirm == kExitGame ? "ARE YOU SURE?" : "EXIT GAME", exit_button_x, 26, 220);
    if (saved_notice) {
        DrawString(pixels, width, height, "SETTINGS SAVED", exit_button_x - 16 - TextWidth("SETTINGS SAVED", 2), 34,
                   2, 0xFFFFE6B8u);
    }

    if (s.tab == kLayoutTab) {
        DrawLayoutTextPage(pixels);
    } else {
        draw_button("SAVE SETTINGS", 52, 108, 220);
        draw_button(s.reset_confirm == kResetAll ? "ARE YOU SURE?" : "RESET ALL", 300, 108, 220);

        const auto rows = BuildRows(s, v, state_slot);
        constexpr int row_x = 52;
        constexpr int row_w = width - 104;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const int y = RowTextY(s.tab, static_cast<uint32_t>(i));
            const bool selected = i == static_cast<int>(s.selected);
            FillRect(pixels, width, height, row_x, y - 8, row_w, 24, selected ? 0xE04A2C12u : 0x70201810u);
            FillRect(pixels, width, height, row_x, y - 8, 6, 24, selected ? 0xFFFFB030u : 0x80FFB030u);
            DrawString(pixels, width, height, rows[static_cast<size_t>(i)].label, row_x + 16, y, 2,
                       selected ? 0xFFFFE6B8u : 0xFFD8C0A0u);
            constexpr int value_x = row_x + row_w - 190;
            FillRect(pixels, width, height, value_x, y - 4, 170, 20, selected ? 0xD030180Cu : 0x9030180Cu);
            if (RowIsNumeric(s, static_cast<uint32_t>(i))) {
                DrawString(pixels, width, height, "-", value_x + 8, y, 2, 0xFFFFB030u);
                DrawString(pixels, width, height, "+", value_x + 148, y, 2, 0xFFFFB030u);
            }
            const char* value = rows[static_cast<size_t>(i)].value.c_str();
            DrawString(pixels, width, height, value, value_x + (170 - TextWidth(value, 2)) / 2, y, 2, 0xFFFFF0C8u);
        }
        if (HasPages(s.tab)) {
            // Only the button that leads somewhere; the page number between them.
            const uint32_t page = Page(s);
            if (page > 0) {
                draw_button("PREVIOUS", kPreviousButtonX, kPageButtonY, kPageButtonWidth);
            }
            if (page + 1 < kPageCount) {
                draw_button("NEXT", kNextButtonX, kPageButtonY, kPageButtonWidth);
            }
            const std::string number = "PAGE " + std::to_string(page + 1) + "/" + std::to_string(kPageCount);
            DrawString(pixels, width, height, number.c_str(), (width - TextWidth(number.c_str(), 2)) / 2,
                       kPageButtonY + 7, 2, 0xFFD8C0A0u);
        }
    }
    DrawPointerSprites(pixels);
    return pixels;
}

// Everything the image shows, as text: a change means the image must be
// rasterised again.
inline std::string ViewKey(const State& s, const PortVrSettings& v, int state_slot, bool saved_notice) {
    std::string key;
    key.reserve(512);
    key += static_cast<char>('0' + s.tab);
    key += static_cast<char>('0' + Page(s));
    key += static_cast<char>('0' + s.reset_confirm);
    key += saved_notice ? '1' : '0';
    if (s.tab != kLayoutTab) {
        key += '|';
        key += std::to_string(s.selected);
        for (const Row& row : BuildRows(s, v, state_slot)) {
            key += '|';
            key += row.label;
            key += '=';
            key += row.value;
        }
    }
    return key;
}

// --- Behaviour ---

inline void ClampSelection(State& s) {
    const uint32_t count = ItemCount(s);
    if (count == 0) {
        s.selected = 0;
    } else if (s.selected >= count) {
        s.selected = count - 1;
    }
}

inline void SetCalibrationPage(State& s, uint32_t page) {
    s.calibration_page = page % kPageCount;
    ClampSelection(s);
}

inline void SetControlPage(State& s, uint32_t page) {
    s.control_page = page % kPageCount;
    ClampSelection(s);
}

inline void ClearStateConfirmation(State& s) {
    s.state_confirm = kNoStateAction;
    s.state_confirm_until = 0.0;
}

inline void ClearResetConfirmation(State& s) {
    s.reset_confirm = kNoReset;
    s.reset_confirm_until = 0.0;
}

inline void ClearConfirmations(State& s) {
    ClearStateConfirmation(s);
    ClearResetConfirmation(s);
}

// Lets go of confirmations that waited too long for their second click.
inline void Refresh(State& s, double now) {
    if (s.state_confirm != kNoStateAction && now >= s.state_confirm_until) {
        ClearStateConfirmation(s);
    }
    if (s.reset_confirm != kNoReset && now >= s.reset_confirm_until) {
        ClearResetConfirmation(s);
    }
}

// True on the second click of `action` within kConfirmSeconds; the first one
// arms it ("ARE YOU SURE?").
inline bool ConfirmReset(State& s, uint32_t action, double now) {
    if (action == kNoReset) {
        return true;
    }
    Refresh(s, now);
    ClearStateConfirmation(s);
    if (s.reset_confirm == action) {
        ClearResetConfirmation(s);
        return true;
    }
    s.reset_confirm = action;
    s.reset_confirm_until = now + kConfirmSeconds;
    return false;
}

inline uint32_t ResetActionForSelection(const State& s) {
    if (s.tab == kCalibrationTab && CalibrationActualIndex(s, s.selected) == 11) {
        return kResetTargeting;
    }
    if (s.tab == kCalibrationTab && CalibrationActualIndex(s, s.selected) == 19) {
        return kResetCalibration;
    }
    if (s.tab == kControlTab && ControlActualIndex(s, s.selected) == 15) {
        return kResetController;
    }
    if (s.tab == kMovementTab && s.selected == 10) {
        return kResetMovement;
    }
    return kNoReset;
}

inline int SnapTurnStep(int current, int direction) {
    constexpr std::array<int, 4> kChoices = {30, 45, 60, 90};
    int closest_index = 1;
    int closest_delta = std::numeric_limits<int>::max();
    for (int i = 0; i < static_cast<int>(kChoices.size()); ++i) {
        const int delta = std::abs(current - kChoices[static_cast<size_t>(i)]);
        if (delta < closest_delta) {
            closest_delta = delta;
            closest_index = i;
        }
    }
    return kChoices[static_cast<size_t>(std::clamp(closest_index + direction, 0, static_cast<int>(kChoices.size()) - 1))];
}

inline void ResetController(PortVrSettings& v) {
    v.use_right_hand = true;
    v.rumble_enabled = true;
    v.rumble_intensity = 1.0f;
    v.rumble_hand = RumbleHand::Right;
    v.grip_inputs_enabled = true;
    v.grip_inputs_use_trackpad = false;
    v.trackpad_press_threshold = 0.5f;
    v.index_grip_press_threshold = 0.5f;
    v.combat_jump_use_primary_button = false;
    v.vr_menu_hold_left_stick = false;
    v.vr_menu_requires_head_zone = false;
    v.vr_menu_floating = false;
    v.game_menu_screen_enabled = true;
    v.xr_dpad_enabled = true;
    v.xr_dpad_head_radius = 0.28f;
    v.xr_dpad_head_y_below = 0.02f;
    v.xr_dpad_deadzone = 0.45f;
}

inline void ResetMovement(PortVrSettings& v) {
    v.directional_movement_enabled = true;
    v.directional_movement_use_right_stick = false;
    v.directional_movement_use_hmd_direction = false;
    v.directional_movement_deadzone = 0.25f;
    v.directional_movement_speed = 14.0f;
    v.directional_movement_accel = 45.0f;
    v.directional_movement_air_accel = 8.0f;
    v.look_yaw_sensitivity = 1.0f;
    v.snap_turn_enabled = false;
    v.snap_turn_degrees = 45;
}

// PrimedGun's RESET ALL: its RuntimeSettings back to their defaults, the mod
// switch and the built-in patches kept. The headset renderer's own settings
// were never in that struct and are not touched.
inline void ResetAll(PortVrSettings& v) {
    const PortVrSettings d{};
    v.use_right_hand = d.use_right_hand;
    v.offset_x = d.offset_x;
    v.offset_y = d.offset_y;
    v.offset_z = d.offset_z;
    v.model_offset_x = d.model_offset_x;
    v.model_offset_y = d.model_offset_y;
    v.model_offset_z = d.model_offset_z;
    v.rot_offset_x = d.rot_offset_x;
    v.rot_offset_y = d.rot_offset_y;
    v.rot_offset_z = d.rot_offset_z;
    v.world_scale = d.world_scale;
    v.rumble_enabled = d.rumble_enabled;
    v.rumble_intensity = d.rumble_intensity;
    v.rumble_hand = d.rumble_hand;
    v.grip_inputs_enabled = d.grip_inputs_enabled;
    v.grip_inputs_use_trackpad = d.grip_inputs_use_trackpad;
    v.trackpad_press_threshold = d.trackpad_press_threshold;
    v.index_grip_press_threshold = d.index_grip_press_threshold;
    v.combat_jump_use_primary_button = d.combat_jump_use_primary_button;
    v.gun_targeting_enabled = d.gun_targeting_enabled;
    v.gun_targeting_distance = d.gun_targeting_distance;
    v.gun_targeting_radius = d.gun_targeting_radius;
    v.visor_helmet_enabled = d.visor_helmet_enabled;
    v.vr_overlays_enabled = d.vr_overlays_enabled;
    v.height_prompt_enabled = d.height_prompt_enabled;
    v.vr_menu_hold_left_stick = d.vr_menu_hold_left_stick;
    v.vr_menu_requires_head_zone = d.vr_menu_requires_head_zone;
    v.vr_menu_floating = d.vr_menu_floating;
    v.cinematic_screen_enabled = d.cinematic_screen_enabled;
    v.game_menu_screen_enabled = d.game_menu_screen_enabled;
    v.frustum_culling_enabled = d.frustum_culling_enabled;
    v.frustum_culling_degrees = d.frustum_culling_degrees;
    v.metroid_hud_distance = d.metroid_hud_distance;
    v.metroid_hud_size = d.metroid_hud_size;
    v.metroid_hud_offset_up = d.metroid_hud_offset_up;
    v.metroid_hud_offset_down = d.metroid_hud_offset_down;
    v.metroid_hud_offset_left = d.metroid_hud_offset_left;
    v.metroid_hud_offset_right = d.metroid_hud_offset_right;
    v.position_marker_enabled = d.position_marker_enabled;
    v.xr_dpad_enabled = d.xr_dpad_enabled;
    v.xr_dpad_head_radius = d.xr_dpad_head_radius;
    v.xr_dpad_head_y_below = d.xr_dpad_head_y_below;
    v.xr_dpad_deadzone = d.xr_dpad_deadzone;
    ResetMovement(v);
    v.vr_state_slot = d.vr_state_slot;
    v.cannon_texture_slot = d.cannon_texture_slot;
}

// The value box's -/+ halves. The bounds are PrimedGun's, narrowed to what the
// settings file keeps (vr_settings.cpp) where that is narrower.
inline void Adjust(State& s, PortVrSettings& v, int direction) {
    const float sign = direction < 0 ? -1.0f : 1.0f;
    const auto step = [sign](float& value, float amount, float low, float high) {
        value = std::clamp(value + sign * amount, low, high);
    };
    switch (s.tab) {
    case kCalibrationTab:
        switch (CalibrationActualIndex(s, s.selected)) {
        case 1:
            step(v.metroid_hud_distance, 0.05f, 0.1f, 3.0f);
            break;
        case 2:
            step(v.metroid_hud_size, 0.05f, 0.1f, 3.0f);
            break;
        case 3: {
            float value = v.metroid_hud_offset_up - v.metroid_hud_offset_down;
            step(value, 0.01f, -1.0f, 1.0f);
            v.metroid_hud_offset_up = std::max(value, 0.0f);
            v.metroid_hud_offset_down = std::max(-value, 0.0f);
            break;
        }
        case 4: {
            float value = v.metroid_hud_offset_right - v.metroid_hud_offset_left;
            step(value, 0.01f, -1.0f, 1.0f);
            v.metroid_hud_offset_right = std::max(value, 0.0f);
            v.metroid_hud_offset_left = std::max(-value, 0.0f);
            break;
        }
        case 5:
            step(v.gun_targeting_distance, 1.0f, 1.0f, 200.0f);
            break;
        case 6:
            step(v.gun_targeting_radius, 0.1f, 0.1f, 25.0f);
            break;
        case 10:
            step(v.frustum_culling_degrees, 1.0f, 70.0f, 175.0f);
            break;
        case 12:
            step(v.model_offset_x, 0.01f, -2.0f, 2.0f);
            break;
        case 13:
            step(v.model_offset_y, 0.01f, -2.0f, 2.0f);
            break;
        case 14:
            step(v.model_offset_z, 0.01f, -2.0f, 2.0f);
            break;
        case 15:
            step(v.rot_offset_x, 1.0f, -180.0f, 180.0f);
            break;
        case 16:
            step(v.rot_offset_y, 1.0f, -180.0f, 180.0f);
            break;
        case 17:
            step(v.rot_offset_z, 1.0f, -180.0f, 180.0f);
            break;
        default:
            break;
        }
        break;
    case kControlTab:
        switch (ControlActualIndex(s, s.selected)) {
        case 3:
            step(v.rumble_intensity, 0.05f, 0.0f, 1.0f);
            break;
        case 9:
            step(v.trackpad_press_threshold, 0.01f, 0.05f, 1.0f);
            break;
        case 11:
            step(v.index_grip_press_threshold, 0.01f, 0.05f, 1.0f);
            break;
        case 12:
            step(v.xr_dpad_head_radius, 0.01f, 0.05f, 0.60f);
            break;
        case 13:
            step(v.xr_dpad_head_y_below, 0.01f, 0.0f, 0.60f);
            break;
        case 14:
            step(v.xr_dpad_deadzone, 0.05f, 0.05f, 0.95f);
            break;
        default:
            break;
        }
        break;
    case kMovementTab:
        switch (s.selected) {
        case 3:
            step(v.directional_movement_deadzone, 0.05f, 0.0f, 0.95f);
            break;
        case 4:
            step(v.directional_movement_speed, 0.5f, 1.0f, 60.0f);
            break;
        case 5:
            step(v.directional_movement_accel, 1.0f, 1.0f, 120.0f);
            break;
        case 6:
            step(v.directional_movement_air_accel, 1.0f, 1.0f, 60.0f);
            break;
        case 7:
            step(v.look_yaw_sensitivity, 0.05f, 0.20f, 3.00f);
            break;
        case 9:
            v.snap_turn_degrees = SnapTurnStep(v.snap_turn_degrees, direction < 0 ? -1 : 1);
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
}

// A click on a row anywhere but a numeric value box.
inline void Activate(State& s, PortVrSettings& v, double now, Actions& actions) {
    const uint32_t reset_action = ResetActionForSelection(s);
    if (reset_action == kNoReset) {
        ClearResetConfirmation(s);
    }

    switch (s.tab) {
    case kCalibrationTab: {
        switch (CalibrationActualIndex(s, s.selected)) {
        case 0:
            v.cinematic_screen_enabled = !v.cinematic_screen_enabled;
            break;
        case 7:
            v.visor_helmet_enabled = !v.visor_helmet_enabled;
            break;
        case 8:
            v.height_prompt_enabled = !v.height_prompt_enabled;
            break;
        case 9:
            v.frustum_culling_enabled = !v.frustum_culling_enabled;
            break;
        case 11:
            if (ConfirmReset(s, reset_action, now)) {
                v.gun_targeting_enabled = true;
                v.gun_targeting_distance = 60.0f;
                v.gun_targeting_radius = 4.0f;
                v.visor_helmet_enabled = false;
            }
            break;
        case 18:
            v.position_marker_enabled = !v.position_marker_enabled;
            break;
        case 19:
            if (ConfirmReset(s, reset_action, now)) {
                v.model_offset_x = v.model_offset_y = v.model_offset_z = 0.0f;
                v.rot_offset_x = v.rot_offset_y = v.rot_offset_z = 0.0f;
            }
            break;
        case 20: // DEFAULT ARM PRESET
            v.offset_x = v.offset_y = v.offset_z = 0.0f;
            v.model_offset_x = v.model_offset_y = v.model_offset_z = 0.0f;
            v.rot_offset_x = v.rot_offset_y = v.rot_offset_z = 0.0f;
            break;
        case 21: // SAMUS ARM PRESET
            v.offset_x = v.offset_y = v.offset_z = 0.0f;
            v.model_offset_x = 0.0f;
            v.model_offset_y = -0.300f;
            v.model_offset_z = 0.0f;
            v.rot_offset_x = 0.0f;
            v.rot_offset_y = 20.0f;
            v.rot_offset_z = -90.0f;
            break;
        case 22:
            v.vr_menu_floating = !v.vr_menu_floating;
            break;
        case 23:
            v.game_menu_screen_enabled = !v.game_menu_screen_enabled;
            break;
        default:
            break;
        }
        return;
    }
    case kControlTab: {
        switch (ControlActualIndex(s, s.selected)) {
        case 0:
            v.use_right_hand = !v.use_right_hand;
            break;
        case 1:
            v.rumble_enabled = !v.rumble_enabled;
            break;
        case 2: // BOTH -> LEFT -> RIGHT, PrimedGun's rumble_hand_mode 0, 1, 2
            v.rumble_hand = v.rumble_hand == RumbleHand::Both   ? RumbleHand::Left
                            : v.rumble_hand == RumbleHand::Left ? RumbleHand::Right
                                                                : RumbleHand::Both;
            break;
        case 4:
            v.grip_inputs_enabled = !v.grip_inputs_enabled;
            break;
        case 5:
            v.combat_jump_use_primary_button = !v.combat_jump_use_primary_button;
            break;
        case 6:
            v.vr_menu_hold_left_stick = !v.vr_menu_hold_left_stick;
            break;
        case 7:
            v.vr_menu_requires_head_zone = !v.vr_menu_requires_head_zone;
            break;
        case 8:
            v.grip_inputs_use_trackpad = !v.grip_inputs_use_trackpad;
            break;
        case 10:
            v.xr_dpad_enabled = !v.xr_dpad_enabled;
            break;
        case 15:
            if (ConfirmReset(s, reset_action, now)) {
                ResetController(v);
            }
            break;
        default:
            break;
        }
        return;
    }
    case kMovementTab:
        switch (s.selected) {
        case 0:
            v.directional_movement_enabled = !v.directional_movement_enabled;
            break;
        case 1:
            v.directional_movement_use_right_stick = !v.directional_movement_use_right_stick;
            break;
        case 2:
            v.directional_movement_use_hmd_direction = !v.directional_movement_use_hmd_direction;
            break;
        case 8:
            v.snap_turn_enabled = !v.snap_turn_enabled;
            break;
        case 10:
            if (ConfirmReset(s, reset_action, now)) {
                ResetMovement(v);
            }
            break;
        default:
            break;
        }
        return;
    case kTexturesTab:
        if (s.selected < kCannonSlotCount && actions.ApplyCannonSlot(static_cast<int>(s.selected))) {
            v.cannon_texture_slot = static_cast<int>(s.selected);
        }
        return;
    case kStatesTab: {
        if (s.selected >= kStateActionRows) {
            const int slot = static_cast<int>(s.selected - kStateActionRows + 1);
            v.vr_state_slot = slot;
            actions.SelectStateSlot(slot);
            ClearStateConfirmation(s);
            return;
        }
        const uint32_t requested = s.selected + 1;
        Refresh(s, now);
        if (s.state_confirm != requested) {
            s.state_confirm = requested;
            s.state_confirm_until = now + kConfirmSeconds;
            return;
        }
        ClearStateConfirmation(s);
        const int slot = actions.StateSlot();
        switch (requested) {
        case kLoadState:
            actions.LoadState(slot);
            break;
        case kSaveState:
            actions.SaveState(slot);
            break;
        case kLoadNewest:
            actions.LoadNewestState();
            break;
        case kSaveOldest:
            actions.SaveOldestState();
            break;
        default:
            break;
        }
        return;
    }
    default:
        return;
    }
}

// Every opening starts on the LAYOUT tab; the two-page tabs keep their page.
inline void Open(State& s) {
    s.tab = kLayoutTab;
    s.selected = 0;
}

inline void Close(State& s) { ClearConfirmations(s); }

// The laser over the canvas (texture pixels, y down) selects the row under it.
inline void Hover(State& s, float texture_y) {
    const uint32_t count = ItemCount(s);
    if (count == 0) {
        return;
    }
    const int hovered = RowFromTextureY(s.tab, std::clamp(texture_y, 0.0f, static_cast<float>(kMenuHeight)), count);
    if (hovered >= 0) {
        s.selected = static_cast<uint32_t>(hovered);
    }
}

// A click (the pointer hand's trigger or A) at a canvas point.
inline void Click(State& s, PortVrSettings& v, float x, float y, double now, Actions& actions) {
    x = std::clamp(x, 0.0f, static_cast<float>(kWidth));
    y = std::clamp(y, 0.0f, static_cast<float>(kMenuHeight));
    if (y >= 26.0f && y <= 54.0f) {
        if (x >= 752.0f && x <= 972.0f && ConfirmReset(s, kExitGame, now)) {
            ClearConfirmations(s);
            actions.ExitGame();
        }
        return;
    }
    if (y >= 64.0f && y <= 102.0f) {
        constexpr float tab_start_x = 22.0f;
        constexpr float tab_step = 166.0f;
        constexpr float tab_width = 150.0f;
        const int tab = static_cast<int>((x - tab_start_x) / tab_step);
        const float tab_local_x = x - (tab_start_x + static_cast<float>(tab) * tab_step);
        if (x >= tab_start_x && tab >= 0 && tab < static_cast<int>(kTabCount) && tab_local_x <= tab_width &&
            s.tab != static_cast<uint32_t>(tab)) {
            ClearConfirmations(s);
            s.tab = static_cast<uint32_t>(tab);
            s.selected = 0;
        }
        return;
    }
    if (HasPages(s.tab) && y >= static_cast<float>(kPageButtonY) &&
        y <= static_cast<float>(kPageButtonY + kPageButtonHeight)) {
        const auto on = [x](int button_x) {
            return x >= static_cast<float>(button_x) && x <= static_cast<float>(button_x + kPageButtonWidth);
        };
        const uint32_t page = Page(s);
        uint32_t turned = page;
        if (on(kPreviousButtonX) && page > 0) {
            turned = page - 1;
        } else if (on(kNextButtonX) && page + 1 < kPageCount) {
            turned = page + 1;
        }
        if (turned != page) {
            ClearConfirmations(s);
            if (s.tab == kCalibrationTab) {
                SetCalibrationPage(s, turned);
            } else {
                SetControlPage(s, turned);
            }
        }
        return;
    }
    if (s.tab != kLayoutTab && y >= 108.0f && y <= 136.0f) {
        if (x >= 52.0f && x <= 272.0f) {
            ClearConfirmations(s);
            actions.SaveSettings();
            s.saved_notice_until = now + kSavedNoticeSeconds;
        } else if (x >= 300.0f && x <= 520.0f && ConfirmReset(s, kResetAll, now)) {
            ResetAll(v);
        }
        return;
    }
    const int row = RowFromTextureY(s.tab, y, ItemCount(s));
    if (row < 0) {
        return;
    }
    s.selected = static_cast<uint32_t>(row);
    constexpr float value_box_x = 52.0f + 920.0f - 190.0f;
    constexpr float value_width = 170.0f;
    if (x >= value_box_x && x <= value_box_x + value_width && RowIsNumeric(s, s.selected)) {
        Adjust(s, v, x >= value_box_x + value_width * 0.5f ? 1 : -1);
    } else {
        Activate(s, v, now, actions);
    }
}

// --- Where it hangs (OpenXR application space: metres, +Y up, -Z forward) ---

using screen_math::Pose;
using screen_math::Quat;
using screen_math::Vec3;

// On the off hand (PrimedGun's default): 1.05 x 0.72 m, so the 2:1 canvas is
// drawn about 37% taller than wide, as PrimedGun showed it.
inline constexpr float kWristWidth = 1.05f;
inline constexpr float kWristHeight = 0.72f;
// "DETACH VR MENU FROM HAND": 4 x 2 m, 2.7 m ahead of where the head looked
// when it opened.
inline constexpr float kFloatingWidth = 4.00f;
inline constexpr float kFloatingHeight = 2.00f;
inline constexpr float kFloatingDistance = 2.70f;
// The laser reaches the panel when it hits it between these, else this far.
inline constexpr float kPointerMinDistance = 0.02f;
inline constexpr float kPointerMaxDistance = 8.0f;
inline constexpr float kLaserWidth = 0.008f;
inline constexpr float kDotSize = 0.02f;
inline constexpr float kDotLift = 0.002f;

inline Quat Multiply(const Quat& a, const Quat& b) noexcept {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

inline Vec3 Add(const Vec3& a, const Vec3& b) noexcept { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 Scale(const Vec3& a, float s) noexcept { return {a[0] * s, a[1] * s, a[2] * s}; }

// PrimedGun's VrMenuPanelPoseFromController: the grip turned -90 degrees about
// its X axis (the panel faces the grip's +Y, its top edge towards the grip's
// -Z), moved 0.10 up and 0.18 forward in the panel's own frame.
inline Pose PanelPoseFromGrip(const Pose& grip) noexcept {
    constexpr float h = 0.70710678f;
    Pose panel;
    panel.orientation = Multiply(grip.orientation, {-h, 0.0f, 0.0f, h});
    panel.position = Add(grip.position, screen_math::Rotate(panel.orientation, {0.0f, 0.10f, -0.18f}));
    return panel;
}

// PrimedGun's VrMenuFloatingPoseFromHead: upright, facing the head's
// horizontal forward, kFloatingDistance ahead at head height. False when the
// head looks straight up or down.
inline bool FloatingPoseFromHead(const Pose& head, Pose& panel) noexcept {
    const Vec3 forward = screen_math::Rotate(head.orientation, {0.0f, 0.0f, -1.0f});
    const float horizontal = std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
    if (horizontal < 0.0001f) {
        return false;
    }
    const float half_yaw = 0.5f * std::atan2(-forward[0] / horizontal, -forward[2] / horizontal);
    panel.orientation = {0.0f, std::sin(half_yaw), 0.0f, std::cos(half_yaw)};
    panel.position = Add(head.position, screen_math::Rotate(panel.orientation, {0.0f, 0.0f, -kFloatingDistance}));
    return true;
}

struct PointerHit {
    bool valid = false;
    float x = 0.0f; // 0..1 across the panel, left to right
    float y = 0.0f; // 0..1 down it
    float distance = 0.0f;
};

// PrimedGun's VrMenuPointerHit: the aim ray against the panel's plane, from
// either side, between kPointerMinDistance and kPointerMaxDistance.
inline PointerHit HitPanel(const Pose& panel, float width, float height, const Pose& aim) noexcept {
    PointerHit hit;
    const Vec3 right = screen_math::Rotate(panel.orientation, {1.0f, 0.0f, 0.0f});
    const Vec3 up = screen_math::Rotate(panel.orientation, {0.0f, 1.0f, 0.0f});
    const Vec3 normal = screen_math::Rotate(panel.orientation, {0.0f, 0.0f, 1.0f});
    const Vec3 direction = screen_math::Rotate(aim.orientation, {0.0f, 0.0f, -1.0f});
    const float denominator = screen_math::Dot(direction, normal);
    if (std::fabs(denominator) < 0.001f || !(width > 0.0f) || !(height > 0.0f)) {
        return hit;
    }
    const Vec3 to_panel{panel.position[0] - aim.position[0], panel.position[1] - aim.position[1],
                        panel.position[2] - aim.position[2]};
    const float t = screen_math::Dot(to_panel, normal) / denominator;
    if (t < kPointerMinDistance || t > kPointerMaxDistance) {
        return hit;
    }
    const Vec3 point = Add(aim.position, Scale(direction, t));
    const Vec3 local{point[0] - panel.position[0], point[1] - panel.position[1], point[2] - panel.position[2]};
    hit.x = 0.5f + screen_math::Dot(local, right) / width;
    hit.y = 0.5f - screen_math::Dot(local, up) / height;
    hit.distance = t;
    hit.valid = hit.x >= 0.0f && hit.x <= 1.0f && hit.y >= 0.0f && hit.y <= 1.0f;
    return hit;
}

// The rotation whose columns are the orthonormal right, up and normal axes.
inline Quat QuatFromAxes(const Vec3& x, const Vec3& y, const Vec3& z) noexcept {
    const float trace = x[0] + y[1] + z[2];
    Quat q;
    if (trace > 0.0f) {
        const float s = 2.0f * std::sqrt(trace + 1.0f);
        q = {(y[2] - z[1]) / s, (z[0] - x[2]) / s, (x[1] - y[0]) / s, 0.25f * s};
    } else if (x[0] > y[1] && x[0] > z[2]) {
        const float s = 2.0f * std::sqrt(1.0f + x[0] - y[1] - z[2]);
        q = {0.25f * s, (y[0] + x[1]) / s, (z[0] + x[2]) / s, (y[2] - z[1]) / s};
    } else if (y[1] > z[2]) {
        const float s = 2.0f * std::sqrt(1.0f + y[1] - x[0] - z[2]);
        q = {(y[0] + x[1]) / s, 0.25f * s, (z[1] + y[2]) / s, (z[0] - x[2]) / s};
    } else {
        const float s = 2.0f * std::sqrt(1.0f + z[2] - x[0] - y[1]);
        q = {(z[0] + x[2]) / s, (z[1] + y[2]) / s, 0.25f * s, (x[1] - y[0]) / s};
    }
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    return {q[0] / length, q[1] / length, q[2] / length, q[3] / length};
}

// The laser: a kLaserWidth ribbon from the aim pose, `length` along the ray.
// PrimedGun laid it flat in the controller's frame; here it turns about the
// ray to face `eye`, so it never shows edge-on.
inline Pose LaserPose(const Pose& aim, float length, const Vec3& eye) noexcept {
    const Vec3 forward = screen_math::Rotate(aim.orientation, {0.0f, 0.0f, -1.0f});
    Pose laser;
    laser.position = Add(aim.position, Scale(forward, 0.5f * length));
    const Vec3 to_eye{eye[0] - laser.position[0], eye[1] - laser.position[1], eye[2] - laser.position[2]};
    Vec3 normal = Add(to_eye, Scale(forward, -screen_math::Dot(to_eye, forward)));
    const float normal_length = std::sqrt(screen_math::Dot(normal, normal));
    if (normal_length < 1.0e-4f) {
        // Looking straight down the ray: PrimedGun's orientation.
        constexpr float h = 0.70710678f;
        laser.orientation = Multiply(aim.orientation, {-h, 0.0f, 0.0f, h});
        return laser;
    }
    normal = Scale(normal, 1.0f / normal_length);
    const Vec3 right{forward[1] * normal[2] - forward[2] * normal[1], forward[2] * normal[0] - forward[0] * normal[2],
                     forward[0] * normal[1] - forward[1] * normal[0]};
    laser.orientation = QuatFromAxes(right, forward, normal);
    return laser;
}

// The hit dot: on the panel where the ray meets it, 2 mm in front of it.
inline Pose DotPose(const Pose& panel, const Pose& aim, float distance) noexcept {
    const Vec3 forward = screen_math::Rotate(aim.orientation, {0.0f, 0.0f, -1.0f});
    Vec3 normal = screen_math::Rotate(panel.orientation, {0.0f, 0.0f, 1.0f});
    if (screen_math::Dot(normal, forward) > 0.0f) {
        normal = Scale(normal, -1.0f); // the side the ray came from
    }
    Pose dot;
    dot.orientation = panel.orientation;
    dot.position = Add(Add(aim.position, Scale(forward, distance)), Scale(normal, kDotLift));
    return dot;
}

} // namespace PortVr::VrMenu

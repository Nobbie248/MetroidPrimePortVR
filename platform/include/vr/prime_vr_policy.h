// SPDX-License-Identifier: GPL-3.0-or-later
//
// Game-facing presentation policy: how Metroid Prime's content is shown in
// the headset this frame. Derived from Wiicompiled VR's mkw_vr_policy, but
// where Mario Kart had to infer its state from instrumented guest functions,
// the port's game code tells the policy directly, so there are no binding
// masks and no scene/camera coherence check.
//
// Fails safe: anything the game thread has not explicitly reported as in-world
// gameplay is shown on the virtual screen (front end, pause, map, logbook,
// save, cinematics when the cinema screen is on).

#pragma once

#include <cstdint>

namespace PortVr {

enum class VRPresentationMode : uint8_t {
    Desktop,
    VirtualScreen,
    Immersive,
};

// What the game thread reports once per presented frame, from source.
enum class VRGameMode : uint8_t {
    Unknown,     // nothing reported yet: virtual screen
    FrontEnd,    // CFrontEndUI, splash, credits, NES
    InGame,      // CStateManager drawing the world, first person or morph ball
    Paused,      // CInGameGuiManager pause / map / logbook / save / message screens
    Cinematic,   // a cinematic camera is active
    Transition,  // world / area transition screens
};

struct PrimeVRPolicyConfig {
    bool enabled = false;
    // False keeps everything on the virtual screen (PrimedGun's "flat screen").
    bool immersive = true;
    bool cinematic_screen = true;   // cinematics on the virtual screen rather than immersive
    bool game_menu_screen = true;   // pause/map/logbook/save on the virtual screen
    float world_units_per_meter = 1.5f;
    float screen_distance_meters = 1.5f;
    float screen_width_meters = 2.0f;
};

struct PrimeVRPolicySnapshot {
    VRPresentationMode presentation = VRPresentationMode::Desktop;
    PrimeVRPolicyConfig config{};
    VRGameMode game_mode = VRGameMode::Unknown;
    uint64_t game_frame = 0;
    bool session_active = false;
    // Changes whenever the stable presentation-safety state changes (config,
    // session, game mode class); never per frame.
    uint64_t safety_generation = 1;
    // (safety_generation << 2) | presentation. Sealed with each GX frame; an
    // immersive packet is accepted only on an exact match.
    uint64_t content_tag = 0;

    float EffectiveUnitsPerMeter() const noexcept { return config.world_units_per_meter; }
};

// All thread-safe. Publish* run on the game thread, SetSessionActive on the
// pacing thread, GetSnapshot anywhere.
void PrimeVRPolicyReset() noexcept;
void PrimeVRPolicyConfigure(const PrimeVRPolicyConfig& config) noexcept;
void PrimeVRPolicySetSessionActive(bool active) noexcept;
void PrimeVRPolicyPublishGameMode(VRGameMode mode, uint64_t game_frame) noexcept;
PrimeVRPolicySnapshot PrimeVRPolicyGetSnapshot() noexcept;
// The tag to seal the frame being drawn with (aurora_end_frame_tagged).
uint64_t PrimeVRPolicyContentTag() noexcept;

} // namespace PortVr

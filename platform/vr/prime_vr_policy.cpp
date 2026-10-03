// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/prime_vr_policy.h"

#include <mutex>

namespace PortVr {
namespace {

// Frames without a world draw (a load hitch, a door's area load) report
// FrontEnd; they must not bounce the headset onto the virtual screen and back.
// Half a second at 90 Hz covers a door load; a real exit to the menu or a
// world transition lasts longer and switches after this.
constexpr int kModeDebounceFrames = 45;

struct State {
    PrimeVRPolicyConfig config{};
    bool session_active = false;
    VRGameMode game_mode = VRGameMode::Unknown;
    VRGameMode pending_mode = VRGameMode::Unknown;
    int pending_frames = 0;
    uint64_t game_frame = 0;
    uint64_t safety_generation = 1;
    VRPresentationMode stable_presentation = VRPresentationMode::Desktop;
};

std::mutex& Mutex() {
    static std::mutex mutex;
    return mutex;
}

State& Stored() {
    static State state;
    return state;
}

VRPresentationMode SelectPresentation(const State& state) noexcept {
    if (!state.config.enabled || !state.session_active) {
        return VRPresentationMode::Desktop;
    }
    if (!state.config.immersive) {
        return VRPresentationMode::VirtualScreen;
    }
    switch (state.game_mode) {
    case VRGameMode::InGame:
        return VRPresentationMode::Immersive;
    case VRGameMode::Paused:
        return state.config.game_menu_screen ? VRPresentationMode::VirtualScreen : VRPresentationMode::Immersive;
    case VRGameMode::Cinematic:
        return state.config.cinematic_screen ? VRPresentationMode::VirtualScreen : VRPresentationMode::Immersive;
    case VRGameMode::Unknown:
    case VRGameMode::FrontEnd:
    case VRGameMode::Transition:
        break;
    }
    return VRPresentationMode::VirtualScreen;
}

uint64_t ContentTagOf(uint64_t generation, VRPresentationMode presentation) noexcept {
    return (generation << 2) | static_cast<uint64_t>(presentation);
}

// Mutex held. The safety generation moves only when what is safe to present
// changes, never on a frame publish that keeps the same presentation.
void Reselect(State& state) noexcept {
    const VRPresentationMode presentation = SelectPresentation(state);
    if (presentation != state.stable_presentation) {
        state.stable_presentation = presentation;
        ++state.safety_generation;
    }
}

PrimeVRPolicySnapshot Snapshot(const State& state) noexcept {
    PrimeVRPolicySnapshot snapshot{};
    snapshot.presentation = state.stable_presentation;
    snapshot.config = state.config;
    snapshot.game_mode = state.game_mode;
    snapshot.game_frame = state.game_frame;
    snapshot.session_active = state.session_active;
    snapshot.safety_generation = state.safety_generation;
    snapshot.content_tag = ContentTagOf(state.safety_generation, state.stable_presentation);
    return snapshot;
}

} // namespace

void PrimeVRPolicyReset() noexcept {
    std::lock_guard lock(Mutex());
    Stored() = State{};
}

void PrimeVRPolicyConfigure(const PrimeVRPolicyConfig& config) noexcept {
    std::lock_guard lock(Mutex());
    State& state = Stored();
    const bool safety_changed = state.config.enabled != config.enabled ||
                                state.config.immersive != config.immersive ||
                                state.config.cinematic_screen != config.cinematic_screen ||
                                state.config.game_menu_screen != config.game_menu_screen;
    state.config = config;
    if (safety_changed) {
        // A configuration change is a new safety state even when it lands on
        // the same presentation: a packet built against the old config is stale.
        ++state.safety_generation;
        state.stable_presentation = SelectPresentation(state);
    } else {
        Reselect(state);
    }
}

void PrimeVRPolicySetSessionActive(bool active) noexcept {
    std::lock_guard lock(Mutex());
    State& state = Stored();
    if (state.session_active == active) {
        return;
    }
    state.session_active = active;
    Reselect(state);
}

void PrimeVRPolicyPublishGameMode(VRGameMode mode, uint64_t game_frame) noexcept {
    std::lock_guard lock(Mutex());
    State& state = Stored();
    state.game_frame = game_frame;
    // Leaving the world for the front end or a transition only counts once it
    // persists; the pause screen, a cinematic and any return to the world
    // switch at once, as does anything while not in the world.
    const bool immediate = mode == VRGameMode::InGame || mode == VRGameMode::Paused ||
                           mode == VRGameMode::Cinematic || state.game_mode != VRGameMode::InGame;
    if (immediate) {
        state.game_mode = mode;
        state.pending_mode = mode;
        state.pending_frames = 0;
    } else if (mode == state.pending_mode) {
        if (++state.pending_frames >= kModeDebounceFrames) {
            state.game_mode = mode;
        }
    } else {
        state.pending_mode = mode;
        state.pending_frames = 1;
    }
    Reselect(state);
}

PrimeVRPolicySnapshot PrimeVRPolicyGetSnapshot() noexcept {
    std::lock_guard lock(Mutex());
    return Snapshot(Stored());
}

uint64_t PrimeVRPolicyContentTag() noexcept {
    std::lock_guard lock(Mutex());
    const State& state = Stored();
    return ContentTagOf(state.safety_generation, state.stable_presentation);
}

} // namespace PortVr

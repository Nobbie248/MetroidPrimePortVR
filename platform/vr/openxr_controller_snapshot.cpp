// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/openxr_controller_snapshot.h"

#include <atomic>
#include <mutex>

namespace PortVr {
namespace {

std::mutex& Mutex() {
    static std::mutex mutex;
    return mutex;
}

OpenXRInputSnapshot& Stored() {
    static OpenXRInputSnapshot snapshot;
    return snapshot;
}

std::array<std::atomic<float>, kRumbleChannelCount>& Rumble() {
    static std::array<std::atomic<float>, kRumbleChannelCount> amplitudes{};
    return amplitudes;
}

} // namespace

void OpenXRPublishInputSnapshot(const OpenXRInputSnapshot& snapshot) noexcept {
    std::lock_guard lock(Mutex());
    Stored() = snapshot;
}

OpenXRInputSnapshot OpenXRGetInputSnapshot() noexcept {
    std::lock_guard lock(Mutex());
    return Stored();
}

void OpenXRSetRumble(float amplitude, RumbleChannel channel) noexcept {
    Rumble()[static_cast<size_t>(channel)].store(amplitude < 0.0f ? 0.0f : amplitude > 1.0f ? 1.0f : amplitude,
                                                 std::memory_order_relaxed);
}

void OpenXRClearRumble() noexcept {
    for (std::atomic<float>& amplitude : Rumble()) {
        amplitude.store(0.0f, std::memory_order_relaxed);
    }
}

// Not cleared on read: a held rumble keeps pulsing until the game sets 0.
RumbleLevels OpenXRTakeRumble() noexcept {
    RumbleLevels levels{};
    for (size_t i = 0; i < kRumbleChannelCount; ++i) {
        levels[i] = Rumble()[i].load(std::memory_order_relaxed);
    }
    return levels;
}

} // namespace PortVr

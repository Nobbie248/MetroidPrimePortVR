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

std::atomic<float>& Rumble() {
    static std::atomic<float> amplitude{0.0f};
    return amplitude;
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

void OpenXRSetRumble(float amplitude) noexcept {
    Rumble().store(amplitude < 0.0f ? 0.0f : amplitude > 1.0f ? 1.0f : amplitude, std::memory_order_relaxed);
}

// Not cleared on read: a held rumble keeps pulsing until the game sets 0.
float OpenXRTakeRumble() noexcept {
    return Rumble().load(std::memory_order_relaxed);
}

} // namespace PortVr

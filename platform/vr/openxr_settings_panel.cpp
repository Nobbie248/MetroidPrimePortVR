// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/openxr_settings_panel.h"

#include <atomic>
#include <mutex>

namespace PortVr {
// Named rather than anonymous: runtime sources are unity-built in groups.
namespace settings_panel_bridge {

std::atomic_bool g_open{false};

struct PublishedPointer {
    std::mutex mutex;
    OpenXRSettingsPanelPointer pointer{};
};

PublishedPointer& Published() {
    static PublishedPointer published;
    return published;
}

} // namespace settings_panel_bridge

void OpenXRSetSettingsPanelOpen(bool open) noexcept {
    settings_panel_bridge::g_open.store(open, std::memory_order_release);
}

bool OpenXRSettingsPanelOpen() noexcept {
    return settings_panel_bridge::g_open.load(std::memory_order_acquire);
}

void OpenXRPublishSettingsPanelPointer(bool valid, float x, float y, bool clicked) noexcept {
    auto& published = settings_panel_bridge::Published();
    std::lock_guard lock(published.mutex);
    OpenXRSettingsPanelPointer& pointer = published.pointer;
    pointer.valid = valid;
    pointer.x = x;
    pointer.y = y;
    if (clicked) {
        ++pointer.clicks;
        pointer.click_valid = valid;
        pointer.click_x = x;
        pointer.click_y = y;
    }
}

OpenXRSettingsPanelPointer OpenXRTakeSettingsPanelPointer() noexcept {
    auto& published = settings_panel_bridge::Published();
    std::lock_guard lock(published.mutex);
    OpenXRSettingsPanelPointer pointer = published.pointer;
    published.pointer.clicks = 0;
    published.pointer.click_valid = false;
    return pointer;
}

} // namespace PortVr

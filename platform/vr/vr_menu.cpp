// SPDX-License-Identifier: GPL-3.0-or-later
//
// PrimedGun's VR menu on the game thread (vr/vr_menu.h): reads the laser the
// XR pacing thread aimed, runs the menu, applies what it changed and hands
// Aurora the menu's image whenever what it shows changes.

#include "vr/vr_menu.h"

#include "vr/vr_log.h"
#include "vr/vr_settings.h"
#include "vr/vr_view.h"

#if defined(MP_ENABLE_OPENXR)
#include "cannon_textures.h"
#include "port_debug.h"
#include "port_paths.h"
#include "port_savestate.h"
#include "port_textures.h"
#include "vr/openxr_integration.h"
#include "vr/openxr_settings_panel.h"

#include <aurora/aurora.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>

#include <chrono>
#include <cstdint>
#include <string>
#endif

namespace PortVr {

#if defined(MP_ENABLE_OPENXR)
namespace {

double NowSeconds() noexcept {
    using namespace std::chrono;
    return duration< double >(steady_clock::now().time_since_epoch()).count();
}

// The menu's actions on the port: the settings file, the port's save states
// (PortSaveState, the overlay's States page) and the launcher's cannon slots.
class GameActions final : public VrMenu::Actions {
public:
    // Both wait for the settings the click changed to be stored (Flush).
    void SaveSettings() override { m_save = true; }
    void ExitGame() override {
        m_save = true;
        m_exit = true;
    }

    int StateSlot() const override {
        const int slot = PortSaveState::SelectedSlot();
        return slot >= 1 && slot <= static_cast<int>(VrMenu::kStateSlotCount) ? slot : 1;
    }
    void SelectStateSlot(int slot) override { PortSaveState::SetSelectedSlot(slot); }
    void LoadState(int slot) override { Report(PortSaveState::RequestLoad(slot)); }
    void SaveState(int slot) override { Report(PortSaveState::RequestSave(slot)); }

    // The slot saved last.
    void LoadNewestState() override {
        int newest = 0;
        int64_t newest_at = 0;
        for (int slot = 1; slot <= static_cast<int>(VrMenu::kStateSlotCount); ++slot) {
            const PortSaveState::Info info = PortSaveState::SlotInfo(slot);
            if (info.exists && (newest == 0 || info.savedAt > newest_at)) {
                newest = slot;
                newest_at = info.savedAt;
            }
        }
        if (newest == 0) {
            PORTVR_LOG() << "VR menu: LOAD NEWEST found no saved state" << std::endl;
            return;
        }
        PortSaveState::SetSelectedSlot(newest);
        Report(PortSaveState::RequestLoad(newest));
    }

    // The first empty slot, else the one saved longest ago.
    void SaveOldestState() override {
        int oldest = 0;
        int64_t oldest_at = 0;
        for (int slot = 1; slot <= static_cast<int>(VrMenu::kStateSlotCount); ++slot) {
            const PortSaveState::Info info = PortSaveState::SlotInfo(slot);
            if (!info.exists) {
                oldest = slot;
                break;
            }
            if (oldest == 0 || info.savedAt < oldest_at) {
                oldest = slot;
                oldest_at = info.savedAt;
            }
        }
        PortSaveState::SetSelectedSlot(oldest);
        Report(PortSaveState::RequestSave(oldest));
    }

    // The launcher's Cannon Textures tab, from the headset: the slot's files
    // into the managed folder of the user texture pack, which is then reloaded.
    bool ApplyCannonSlot(int slot) override {
        namespace Cannon = PrimedGunLauncher::Cannon;
        const std::string user_textures = PortTextures::UserRoot();
        if (user_textures.empty() || PortPaths::UserFolder().empty()) {
            PORTVR_LOG() << "VR menu: no user texture pack folder for the cannon textures" << std::endl;
            return false;
        }
        Cannon::Folders folders;
        folders.library = PortPaths::detail::FromUtf8(PortPaths::UserFolder()) / "primedgun" / "cannon_textures";
        if (const char* base = SDL_GetBasePath(); base != nullptr) {
            folders.shippedLibrary = PortPaths::detail::FromUtf8(base) / "primedgun" / "cannon_textures";
        }
        folders.userTextures = PortPaths::detail::FromUtf8(user_textures);
        std::string error;
        if (!Cannon::ApplySlot(folders, slot, error)) {
            PORTVR_LOG() << "VR menu: cannon texture slot " << slot << " not applied: " << error << std::endl;
            return false;
        }
        PortTextures::RequestUserPackReload();
        PORTVR_LOG() << "VR menu: cannon texture slot " << Cannon::SlotName(slot) << " applied" << std::endl;
        return true;
    }

    // After the click's settings are stored: the requested save, then the exit
    // the way closing the window exits (the F1 overlay's Exit game).
    void Flush() {
        if (m_save) {
            m_save = false;
            PortDebug::SaveSettingsNow();
        }
        if (m_exit) {
            m_exit = false;
            OpenXRSetSettingsPanelOpen(false);
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
    }

private:
    static void Report(bool accepted) {
        if (!accepted) {
            PORTVR_LOG() << "VR menu: " << PortSaveState::LastMessage() << std::endl;
        }
    }

    bool m_save = false;
    bool m_exit = false;
};

struct Menu {
    VrMenu::State state;
    GameActions actions;
    bool was_open = false;
    // What the image Aurora has shows (VrMenu::ViewKey); empty when it has none.
    std::string shown;
};

Menu& TheMenu() {
    static Menu menu;
    return menu;
}

} // namespace
#endif

void VrMenuUpdate() noexcept {
#if defined(MP_ENABLE_OPENXR)
    Menu& menu = TheMenu();
    if (!OpenXRIsRunning() || !OpenXRSettingsPanelOpen()) {
        if (menu.was_open) {
            menu.was_open = false;
            VrMenu::Close(menu.state);
            menu.shown.clear();
            aurora_set_stereo_panel_image(nullptr, 0, 0);
        }
        return;
    }
    const double now = NowSeconds();
    if (!menu.was_open) {
        menu.was_open = true;
        VrMenu::Open(menu.state);
    }

    PortVrSettings settings = GetVrSettings();
    const OpenXRSettingsPanelPointer pointer = OpenXRTakeSettingsPanelPointer();
    VrMenu::Refresh(menu.state, now);
    if (pointer.valid) {
        VrMenu::Hover(menu.state, pointer.y);
    }
    if (pointer.clicks > 0 && pointer.click_valid) {
        VrMenu::Click(menu.state, settings, pointer.click_x, pointer.click_y, now, menu.actions);
        // Live, as in PrimedGun; the file is written by SAVE SETTINGS, EXIT
        // GAME, or whenever the port next saves its settings.
        SetVrSettings(settings);
        menu.actions.Flush();
    }

    const int slot = menu.actions.StateSlot();
    const bool notice = now < menu.state.saved_notice_until;
    std::string key = VrMenu::ViewKey(menu.state, settings, slot, notice);
    if (key != menu.shown) {
        menu.shown = std::move(key);
        const VrMenu::Pixels pixels = VrMenu::BuildPixels(menu.state, settings, slot, notice);
        aurora_set_stereo_panel_image(pixels.data(), VrMenu::kWidth, VrMenu::kImageHeight);
    }
#endif
}

} // namespace PortVr

// SPDX-License-Identifier: GPL-3.0-or-later
//
// PrimedGun's modern control scheme: the headset controllers' snapshot
// (openxr_controller_snapshot.h) turned into Metroid Prime's GameCube pad on
// port 0, once per simulation tick, as PrimedGun's GCPadEmu did inside Dolphin
// (ApplyPrimedGunModernControls). Which mapping applies comes from the game's
// own state rather than memory probes: classic (menus, map, cutscenes, morph
// ball), gameplay (first person, unmorphed), or orbit lock.
//
// The beam wheel: holding the weapon hand's B freezes a panel 0.26 m ahead of
// the aim pose; the aim ray's hit on it (or the hand's travel, as a fallback)
// picks Power / Wave / Ice / Plasma by its dominant axis, and letting go pulses
// the C-stick that way for eight samples.
//
// The visor gesture (vr_visor_dpad.h): with the off-hand controller held next
// to the headset, its stick is the D-pad, which picks the visors.
//
// The snap turn (vr_snap_turn.h): with the setting on, a flick of the look
// stick turns Samus by the snap turn angle instead of turning her smoothly.

#pragma once

#include <cstdint>

class CStateManager;

namespace PortVr {

// Game thread, right before the game reads its controllers. `mgr` may be null
// (no state manager: the front end). Does nothing, and leaves port 0 to the
// real pads, unless a headset session is running in PrimedGun controller mode.
void VrPadUpdate(const CStateManager* mgr) noexcept;

// Game thread, every tick from CInGameGuiManager::Update: one of its paused
// screens (map, pause, logbook, save, HUD message) is open, opening or
// closing. The classic mapping applies meanwhile, as PrimedGun's game flow
// hooks made it, so the controllers' A confirms and B backs out.
void VrNoteInGameMenu(bool open) noexcept;

// Game thread, from CPlayer::PortVrSnapTurn: the snap turn a look stick flick
// asked for since the last call (vr_snap_turn.h), in degrees, positive to the
// right, or 0. Taking it clears it; a request left over when gameplay ends
// (menu, map, cinematic, morph ball) is dropped.
float VrTakeSnapTurn() noexcept;

// What the pad synthesis last decided, for the game hooks and the overlays.
struct VrPadState {
    bool active = false;        // the VR controllers own port 0
    bool gameplay = false;      // first person, unmorphed, no menu: PrimedGun gameplay mapping
    bool orbit_lock = false;    // gameplay with the lock button held on an orbit target
    bool weapon_panel = false;  // the beam wheel is open
    int weapon_selected = -1;   // 0 Power, 1 Wave, 2 Ice, 3 Plasma, -1 none
    uint32_t weapon_hand = 1;   // 0 left, 1 right
    bool visor_zone = false;    // the off hand is at the head: its stick is the D-pad
    int visor_direction = -1;   // VisorDpad::Dir: 0 up (combat), 1 right (X-ray), 2 down (thermal), 3 left (scan)
};
VrPadState GetVrPadState() noexcept;

} // namespace PortVr

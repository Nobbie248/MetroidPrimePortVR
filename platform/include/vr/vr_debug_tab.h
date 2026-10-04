// SPDX-License-Identifier: GPL-3.0-or-later
//
// The "VR" page of the port's F1 overlay: the headset's status and the
// PortVrSettings a player tunes while testing (replay, world scale, culling,
// the head-locked HUD, the cannon calibration, camera and control options).
// Every change goes through SetVrSettings, so it reaches aurora and the
// settings file like any other overlay setting.

#pragma once

#include <cstdint>

namespace PortVr {

void DrawVrDebugTab();

// What the HUD costs per frame, in GX draw commands, by section: the visor
// effects, the HUD frame, the minimap and the helmet (CInGameGuiManager::Draw).
void PortVrNoteHudDraws(uint32_t visor, uint32_t hud, uint32_t map, uint32_t helmet);

} // namespace PortVr

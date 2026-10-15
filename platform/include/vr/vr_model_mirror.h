// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "vr/vr_view.h"
#include "dolphin/gx/GXAurora.h"

namespace PortVr {
class ModelMirrorScope {
    u8 previous;
public:
    explicit ModelMirrorScope(bool mirrored = VrMirrorWeaponModels())
        : previous(AuroraSetCullMirrored(mirrored ? 1 : 0)) {}
    ~ModelMirrorScope() { AuroraSetCullMirrored(previous); }
    ModelMirrorScope(const ModelMirrorScope&) = delete;
    ModelMirrorScope& operator=(const ModelMirrorScope&) = delete;
};
}

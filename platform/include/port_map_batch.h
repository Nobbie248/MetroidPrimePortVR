#pragma once

#include "port_map_batch_geometry.h"

class CTransform4f;
namespace PortMapBatch {
bool Enabled();
// The map screen's rooms too (MP_MAP_SCREEN_BATCH=0 keeps their draw each), as long as
// the minimap's batch is on.
bool EnabledInMapScreen();
// Caller restores CMapAreaSurface's material after flushing.
void Draw(const Geometry& geometry, const CTransform4f& model);
} // namespace PortMapBatch

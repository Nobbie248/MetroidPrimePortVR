#pragma once

#include "port_map_batch_geometry.h"

class CTransform4f;
namespace PortMapBatch {
bool Enabled();
// Caller restores CMapAreaSurface's material after flushing.
void Draw(const Geometry& geometry, const CTransform4f& model);
} // namespace PortMapBatch

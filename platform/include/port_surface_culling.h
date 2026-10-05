#pragma once

#include "port_surface_culling_math.h"

class CFrustumPlanes;
class CCubeSurface;

namespace PortSurfaceCulling {
// A single pose/settings snapshot for one world pass. Offscreen area/shadow
// draws bypass this; the draw's own frustum is always part of the visible union.
class Context {
  const CFrustumPlanes& drawFrustum_;
  StereoVolume stereo_;
  bool enabled_ = false;
  bool immersive_ = false;

public:
  Context(const CFrustumPlanes& drawFrustum, bool allow);
  bool Visible(const CCubeSurface& surface, unsigned materialFlags) const;
};
} // namespace PortSurfaceCulling

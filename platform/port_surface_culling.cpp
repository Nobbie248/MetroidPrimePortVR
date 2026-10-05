#include "port_surface_culling.h"

#include "Kyoto/Graphics/CCubeMaterial.hpp"
#include "Kyoto/Graphics/CCubeSurface.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Math/CFrustumPlanes.hpp"
#include "port_log.h"
#include "vr/vr_view.h"

#include <cstdlib>

namespace PortSurfaceCulling {
namespace {
bool Enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("MP_SURFACE_CULL");
    return value == nullptr || value[0] != '0';
  }();
  return enabled;
}

void Record(bool enabled, bool rejected, bool missingBounds, bool specialMaterial) {
  static const bool diagnostics = std::getenv("MP_FRAME_STATS") != nullptr;
  if (!diagnostics) { return; }
  static int startFrame = CGraphics::GetFrameCounter();
  static unsigned tested = 0, culled = 0, missing = 0, special = 0;
  const int frame = CGraphics::GetFrameCounter();
  if (frame - startFrame >= 600) {
    const float frames = static_cast<float>(frame - startFrame);
    PortLog::Write("[surface-cull] enabled=%d per frame: considered %.1f, rejected %.1f, "
                   "missing/invalid bounds %.1f, special material %.1f\n",
                   enabled, tested / frames, culled / frames, missing / frames, special / frames);
    startFrame = frame;
    tested = culled = missing = special = 0;
  }
  ++tested;
  culled += rejected;
  missing += missingBounds;
  special += specialMaterial;
}
} // namespace

Context::Context(const CFrustumPlanes& drawFrustum, bool allow) : drawFrustum_(drawFrustum) {
  enabled_ = allow && Enabled();
  if (enabled_ && PortVr::VrImmersive()) {
    immersive_ = true;
    // Unknown tracking, disabled VR culling or an unsupported FOV keeps all
    // surfaces. Never fall back to the narrower desktop view in that case.
    enabled_ = PortVr::VrSurfaceCullingVolume(CGraphics::GetViewMatrix(), stereo_);
  }
}

bool Context::Visible(const CCubeSurface& surface, unsigned materialFlags) const {
  if (!enabled_) { Record(false, false, false, false); return true; }
  // Preserve reflection/copy side effects and mod shaders that may displace
  // vertices beyond the authored bounds.
  constexpr unsigned specialFlags = kStateFlag_Reflection | kStateFlag_ReflectionSurfaceEye |
                                    kStateFlag_ReflectionIndirectTexture | kStateFlag_PortPBR;
  if (materialFlags & specialFlags) { Record(true, false, false, true); return true; }
  if (!surface.HasBounds()) { Record(true, false, true, false); return true; }
  const CAABox box = surface.GetBounds();
  const auto& min = box.GetMinPoint();
  const auto& max = box.GetMaxPoint();
  const Bounds bounds{{min.GetX(), min.GetY(), min.GetZ()}, {max.GetX(), max.GetY(), max.GetZ()}};
  if (!ValidBounds(bounds)) { Record(true, false, true, false); return true; }
  const CVector3f epsilon(0.01f, 0.01f, 0.01f);
  const bool visible = drawFrustum_.BoxInFrustumPlanes(CAABox(min - epsilon, max + epsilon)) ||
                       (immersive_ && stereo_.Visible(bounds));
  Record(true, !visible, false, false);
  return visible;
}
} // namespace PortSurfaceCulling

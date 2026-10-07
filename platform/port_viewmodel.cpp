#include "port_viewmodel.h"

#include "Kyoto/CResFactory.hpp"
#include "Kyoto/Graphics/CLight.hpp"
#include "Kyoto/Graphics/CModelFlags.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "MetroidPrime/CActorLights.hpp"
#include "MetroidPrime/CModelData.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"

#include <cmath>
#include <cstdio>
#include <memory>

namespace PortViewModel {
namespace {

struct SView {
  uint32_t id = 0;
  float dist = 0.f;
  float yaw = 0.f;
  float pitch = 0.f;
  bool loaded = false;
  bool light = false;
  CVector3f centre = CVector3f(0.f, 0.f, 0.f);
  float fitDist = 1.f;
  float radius = 1.f;
  float fov = 55.f;
  std::unique_ptr< CModelData > model;
};

// Never destroyed: a static SView's model would be freed during static teardown, after the
// resource pool and PortMods' texture table it unbinds from are already gone.
SView& sView = *new SView;

} // namespace

bool Show(uint32_t id, float dist, float yaw, float pitch, std::string& err) {
  if (gpResourceFactory == nullptr ||
      gpResourceFactory->GetResourceTypeById(static_cast< CAssetId >(id)) != 'CMDL') {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%08X is not a CMDL", id);
    err = buf;
    return false;
  }
  if (sView.model == nullptr || sView.id != id) {
    sView.model.reset();
    sView.model.reset(new CModelData(CStaticRes(static_cast< CAssetId >(id), CVector3f(1.f, 1.f, 1.f))));
    sView.loaded = false;
  }
  sView.id = id;
  sView.dist = dist;
  sView.yaw = yaw;
  sView.pitch = pitch;
  return true;
}

void Hide() {
  sView.model.reset();
  sView.id = 0;
  sView.loaded = false;
}

bool Active() { return sView.model != nullptr; }

std::string Status() {
  if (sView.model == nullptr) {
    return "viewmodel off";
  }
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "viewmodel %08X %s dist %.3f fov %.1f yaw %.1f pitch %.1f centre %.3f %.3f %.3f",
                sView.id, sView.loaded ? "loaded" : "loading",
                sView.dist > 0.f ? sView.dist : sView.fitDist, sView.fov, sView.yaw, sView.pitch,
                sView.centre.GetX(), sView.centre.GetY(), sView.centre.GetZ());
  return buf;
}

void Draw(const CStateManager& mgr) {
  if (sView.model == nullptr) {
    return;
  }
  if (!sView.loaded) {
    if (!sView.model->IsLoaded(0)) {
      sView.model->Touch(CModelData::kWM_Normal, 0);
      return;
    }
    const CAABox box = sView.model->GetBounds();
    const CVector3f lo = box.GetMinPoint();
    const CVector3f hi = box.GetMaxPoint();
    sView.centre = (lo + hi) * 0.5f;
    const CVector3f ext = hi - lo;
    sView.radius = 0.5f * std::sqrt(ext.MagSquared());
    sView.loaded = true;
  }
  // Fit the bounding sphere into whatever vertical FOV the camera currently has, not a hardcoded
  // 55: CStateManager sets the projection from cam.GetFov() every frame, so a cinematic or a
  // visor transition silently changes the on-screen size of every model between shots. Recomputing
  // from the live FOV keeps a model's framing the same in every cell of a capture sheet.
  const CGameCamera& camera = mgr.GetCameraManager()->GetCurrentCamera(mgr);
  sView.fov = camera.GetRenderFov();
  const float fovDeg = sView.fov > 5.f && sView.fov < 150.f ? sView.fov : 55.f;
  sView.fitDist = sView.radius / std::sin(0.5f * fovDeg * (M_PIF / 180.f)) * 1.1f + 0.05f;
  const float dist = sView.dist > 0.f ? sView.dist : sView.fitDist;
  const CTransform4f cam = mgr.GetCameraManager()->GetCurrentCameraTransform(mgr);
  const CTransform4f xf = cam * CTransform4f::Translate(0.f, dist, 0.f) *
                          CTransform4f::RotateX(CRelAngle::FromDegrees(sView.pitch)) *
                          CTransform4f::RotateZ(CRelAngle::FromDegrees(sView.yaw)) *
                          CTransform4f::Translate(-sView.centre.GetX(), -sView.centre.GetY(), -sView.centre.GetZ());
  if (!sView.light) {
    sView.model->Render(CModelData::kWM_Normal, xf, nullptr, CModelFlags::Normal());
    return;
  }
  // A key light from over the camera's left shoulder plus a dim ambient, so lit
  // material paths (per-vertex lighting, the PBR shading) can be compared.
  CActorLights lights(0, CVector3f(0.f, 0.f, 0.f), 1, 0);
  rstl::vector< CLight > key;
  // CLight::BuildDirectional takes the direction of travel, not the direction toward the light:
  // DolphinCGraphics places a directional light at -dir (DolphinCGraphics.cpp, the directional
  // case), so this points from over the camera's shoulder into the scene. Negating it puts the key
  // light behind the model and leaves the camera-facing side lit by ambient only.
  CVector3f dir = cam.GetForward() + cam.GetRight() * 0.5f - cam.GetUp() * 0.7f;
  dir.Normalize();
  key.push_back(CLight::BuildDirectional(dir, CColor(0.9f, 0.9f, 0.9f)));
  // Ambient was 0.3, which is 0.07 once the PBR shader takes it to linear space, so a PBR
  // surface in a dim room rendered as its emissive map alone and a correctly converted model
  // read as a dark smudge next to retail's unlit one. 0.55 (0.27 linear) lights the albedo
  // without washing out the emissive, which is what the comparison needs.
  lights.BuildFakeLightList(key, CColor(0.55f, 0.55f, 0.55f));
  sView.model->Render(CModelData::kWM_Normal, xf, &lights, CModelFlags::Normal());
}

void SetLight(bool on) { sView.light = on; }

} // namespace PortViewModel

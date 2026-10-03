// Liquid surfaces at run time: which areas have a file, their models, and the draw. See
// port_room_liquid.h.
#include "port_room_liquid.h"

#include "port_gci.h"
#include "port_log.h"
#include "port_mods.h"
#include "port_room_env.h"

#include "Kyoto/CResFactory.hpp"
#include "Kyoto/Graphics/CCubeMaterial.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Graphics/CModelFlags.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "MetroidPrime/CActorLights.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CModelData.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetaRender/CCubeRenderer.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>

namespace PortRoomLiquid {
namespace {

constexpr uint32_t kMagic = 0x4C52504D; // 'MPRL'
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderBytes = 12;
constexpr size_t kSurfaceBytes = 8 + 12 * 4;
// How far a surface may be from the water object it is drawn for.
constexpr float kReach = 2.f;

struct Placed {
  uint32_t type = 0;
  uint32_t id = 0;
  std::unique_ptr< CModelData > data;
  CTransform4f xf = CTransform4f::Identity(); // model -> world
  std::unique_ptr< CActorLights > lights;
  bool taken = false; // a water object draws it
};

struct Area {
  bool read = false;
  bool placed = false; // the surfaces have their world transforms
  std::vector< Placed > items;
  std::unordered_map< uint32_t, int > owners; // water object -> its surface, -1 for none
};

// Never destroyed, as port_room_geo.cpp's: the models' tokens must not outlive the pool.
std::unordered_map< uint32_t, Area >& Areas() {
  static auto* const areas = new std::unordered_map< uint32_t, Area >();
  return *areas;
}

int sEnabled = -1;
int sDrawn = 0;
int sDrawnLast = 0;

int HexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

uint32_t ReadU32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void Load(uint32_t mrea, Area& area) {
  area.read = true;
  const std::string path = PortMods::RoomLiquidPath(mrea);
  if (path.empty() || gpResourceFactory == nullptr) {
    return;
  }
  std::ifstream in(PortGci::PathFromString(path), std::ios::binary);
  const std::vector< uint8_t > data((std::istreambuf_iterator< char >(in)),
                                    std::istreambuf_iterator< char >());
  std::vector< Surface > surfaces;
  std::string error;
  if (!in || !Parse(data, surfaces, error)) {
    PortLog::Write("room liquid: %s: %s\n", path.c_str(), error.empty() ? "cannot read" : error.c_str());
    return;
  }
  size_t missing = 0;
  for (const Surface& surface : surfaces) {
    if (gpResourceFactory->GetResourceTypeById(static_cast< CAssetId >(surface.model)) != 'CMDL') {
      ++missing;
      continue;
    }
    Placed& item = area.items.emplace_back();
    item.type = surface.type;
    item.id = surface.model;
    item.data.reset(
        new CModelData(CStaticRes(static_cast< CAssetId >(surface.model), CVector3f(1.f, 1.f, 1.f))));
    const float* const m = surface.transform;
    item.xf = CTransform4f(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
  }
  PortLog::Write("room liquid: %08X: %zu surface(s), %zu without a model\n", mrea, area.items.size(), missing);
}

} // namespace

bool ParseFileName(const std::string& fileName, uint32_t& id) {
  static const char kSuffix[] = ".roomliquid";
  const size_t suffix = sizeof(kSuffix) - 1;
  if (fileName.size() != 8 + suffix) {
    return false;
  }
  for (size_t i = 0; i < suffix; ++i) {
    const char c = fileName[8 + i];
    if ((c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c) != kSuffix[i]) {
      return false;
    }
  }
  uint32_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    const int digit = HexDigit(fileName[i]);
    if (digit < 0) {
      return false;
    }
    value = value << 4 | uint32_t(digit);
  }
  id = value;
  return true;
}

bool Parse(const std::vector< uint8_t >& data, std::vector< Surface >& out, std::string& error) {
  out.clear();
  if (data.size() < kHeaderBytes || ReadU32(data.data()) != kMagic) {
    error = "not a room liquid file";
    return false;
  }
  if (ReadU32(data.data() + 4) != kVersion) {
    error = "unknown version";
    return false;
  }
  const size_t count = ReadU32(data.data() + 8);
  if (count > (data.size() - kHeaderBytes) / kSurfaceBytes) {
    error = "truncated";
    return false;
  }
  out.resize(count);
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* p = data.data() + kHeaderBytes + i * kSurfaceBytes;
    out[i].type = ReadU32(p);
    out[i].model = ReadU32(p + 4);
    for (int k = 0; k < 12; ++k) {
      const uint32_t bits = ReadU32(p + 8 + 4 * k);
      std::memcpy(&out[i].transform[k], &bits, 4);
      if (!std::isfinite(out[i].transform[k])) {
        error = "a transform is not finite";
        out.clear();
        return false;
      }
    }
  }
  return true;
}

void SetLoadedAreas(const uint32_t* mreas, size_t count) {
  auto& areas = Areas();
  for (auto it = areas.begin(); it != areas.end();) {
    bool here = false;
    for (size_t i = 0; i < count; ++i) {
      here |= mreas[i] == it->first;
    }
    it = here ? std::next(it) : areas.erase(it);
  }
  sDrawnLast = sDrawn;
  sDrawn = 0;
}

bool Draw(const CStateManager& mgr, const CGameArea& gameArea, uint32_t uid, const CVector3f& position,
          int fluidType, float surfaceZ) {
  if (!Enabled()) {
    return false;
  }
  const uint32_t mrea = gameArea.GetAreaAssetId();
  Area& area = Areas()[mrea];
  if (!area.read) {
    Load(mrea, area);
  }
  if (area.items.empty()) {
    return false;
  }
  if (!area.placed) {
    for (Placed& item : area.items) {
      item.xf = gameArea.GetTM() * item.xf;
    }
    area.placed = true;
  }
  auto owner = area.owners.find(uid);
  if (owner == area.owners.end()) {
    // The nearest surface of what the object holds; thick lava is lava here.
    const uint32_t type = fluidType == 1 ? 1 : fluidType == 2 || fluidType == 5 ? 2 : fluidType == 0 ? 0 : ~0u;
    int best = -1;
    float bestDistance = 1e30f;
    for (size_t i = 0; i < area.items.size(); ++i) {
      const Placed& item = area.items[i];
      const float distance = (CVector3f(item.xf.Get03(), item.xf.Get13(), item.xf.Get23()) - position).Magnitude();
      if (item.type == type && !item.taken && distance < bestDistance) {
        best = int(i);
        bestDistance = distance;
      }
    }
    PortLog::Write("room liquid: %08X: object %u (fluid %d) at (%.1f, %.1f, %.1f), surface at %.1f: %s, %.2f away\n",
                   mrea, uid, fluidType, position.GetX(), position.GetY(), position.GetZ(), surfaceZ,
                   best < 0 ? "no surface" : bestDistance > kReach ? "nearest surface too far" : "surface found",
                   best < 0 ? 0.f : bestDistance);
    if (bestDistance > kReach) {
      best = -1;
    }
    if (best >= 0) {
      area.items[size_t(best)].taken = true;
    }
    owner = area.owners.emplace(uid, best).first;
  }
  if (owner->second < 0) {
    return false;
  }
  Placed& item = area.items[size_t(owner->second)];
  if (!item.data->IsLoaded(0)) {
    item.data->Touch(CModelData::kWM_Normal, 0);
    return false;
  }
  // The sheet lies where the game has the surface, which a script may raise or drain.
  CTransform4f xf(item.xf);
  xf.AddTranslationZ(surfaceZ - xf.Get23());
  const CAABox bounds = item.data->GetBounds().GetTransformedAABox(xf);
  const bool baked = PortRoomEnv::HasVolume(mrea);
  if (item.lights == nullptr) {
    // As room geometry: the baked ambient already holds the area's lights.
    item.lights.reset(new CActorLights(8, CVector3f(0.f, 0.f, 0.f), 4, baked ? 0 : 4));
    if (baked) {
      item.lights->SetAmbientColor(CColor::White());
    }
  }
  if (baked) {
    const CVector3f centre = bounds.GetCenterPoint();
    const float at[3] = {centre.GetX(), centre.GetY(), centre.GetZ()};
    PortRoomEnv::SetVolumeHint(mrea, at);
  } else {
    item.lights->BuildAreaLightList(mgr, gameArea, bounds);
  }
  item.lights->BuildDynamicLightList(mgr, bounds);
  // The object is drawn in the sorted pass, so both halves go here: lava is opaque, water
  // blended. The thermal visor's passes take the fluid plane's whole shader too (the hot
  // one adds it), so the surface keeps its own; the X-Ray visor draws it as it is.
  const EThermalDrawFlag thermal = mgr.GetThermalDrawFlag();
  CCubeMaterial::sPortPBRThermal = thermal == kTD_Hot    ? CCubeMaterial::kPT_Additive
                                   : thermal == kTD_Cold ? CCubeMaterial::kPT_Cold
                                                         : CCubeMaterial::kPT_None;
  const CModel& model = **item.data->PickStaticModel(CModelData::kWM_Normal);
  gpRender->SetModelMatrix(xf);
  item.lights->ActivateLights();
  model.DrawUnsortedParts(CModelFlags::Normal());
  model.DrawSortedParts(CModelFlags::Normal());
  if (CCubeMaterial::sPortPBRThermal == CCubeMaterial::kPT_Additive) {
    // The blend was set past the material cache.
    CCubeMaterial::ResetCachedMaterials();
  }
  CCubeMaterial::sPortPBRThermal = CCubeMaterial::kPT_None;
  gpRender->SetAmbientColor(CColor::White());
  CGraphics::DisableAllLights();
  if (baked) {
    PortRoomEnv::ClearVolumeHint();
  }
  ++sDrawn;
  return true;
}

void Reset() { Areas().clear(); }

void SetEnabled(bool enabled) {
  sEnabled = enabled ? 1 : 0;
  // The objects are matched again, and the models let go when it is off.
  Areas().clear();
}

bool Enabled() {
  if (sEnabled < 0) {
    const char* const env = std::getenv("MP_ROOM_LIQUID");
    sEnabled = env != nullptr && env[0] == '0' ? 0 : 1;
  }
  return sEnabled != 0;
}

void Stats(int& areas, int& surfaces, int& drawn) {
  areas = surfaces = 0;
  for (const auto& [mrea, area] : Areas()) {
    if (!area.items.empty()) {
      ++areas;
      surfaces += int(area.items.size());
    }
  }
  drawn = sDrawnLast;
}

} // namespace PortRoomLiquid

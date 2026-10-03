// Room environments at run time: which areas have one, their cubes on the GPU, and the
// cube and ambient for a model. See port_room_env.h.
#include "port_room_env.h"

#include "port_gci.h"
#include "port_log.h"
#include "port_mods.h"
#include "port_remastered_txtr.h"

#include <dolphin/gx/GXExtra.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace PortRoomEnv {
namespace {

struct GpuCube {
  uint32_t id = 0;      // 0: not made yet (or it failed)
  bool failed = false;
  float average = 0.f;  // luminance over every direction, as stored
  float peak = 0.f;     // the largest channel of the colour over every direction
  uint32_t mipCount = 0;
};

struct GpuVolume {
  uint32_t id = 0; // 0: not made yet
};

struct Area {
  File file;
  std::vector<GpuCube> cubes;
  std::vector<GpuVolume> volumes; // one a grid
  float exposure = 0.f; // what takes the room's radiance to the display's range; 0: unknown
  float tone[3][4] = {}; // its tone curve
};

// Areas in memory; one without a file has an empty File.
std::unordered_map<uint32_t, Area> sAreas;
uint32_t sNextCube = 1;
uint32_t sNextVolume = 1;
int sVolumes = -1;
float sAmbientScale = -1.f;
float sVolumeView = -1.f;
bool sHint = false;
uint32_t sHintArea = 0;
float sHintCentre[3];
int sEnabled = -1;
int sExposure = -1;
uint32_t sViewArea = 0;
// Model draws come in runs at one position.
bool sLastValid = false;
float sLastPos[3];
bool sLastFound = false;
Selection sLast;

float HalfToFloat(uint16_t h) {
  const int exponent = (h >> 10) & 0x1F;
  const int mantissa = h & 0x3FF;
  float value;
  if (exponent == 0) {
    value = std::ldexp(float(mantissa), -24);
  } else if (exponent == 31) {
    value = mantissa == 0 ? 65504.f : 0.f; // infinity is clamped, a NaN is dropped
  } else {
    value = std::ldexp(float(mantissa | 0x400), exponent - 25);
  }
  return (h & 0x8000) != 0 ? -value : value;
}

float EnvFloat(const char* name, float fallback) {
  const char* const text = std::getenv(name);
  return text != nullptr && text[0] != '\0' ? float(std::atof(text)) : fallback;
}

void Free(Area& area) {
  for (GpuCube& cube : area.cubes) {
    if (cube.id != 0) {
      GXDestroyPBRCube(cube.id);
    }
  }
  area.cubes.clear();
  for (GpuVolume& volume : area.volumes) {
    if (volume.id != 0) {
      GXDestroyPBRVolume(volume.id);
    }
  }
  area.volumes.clear();
}

uint16_t FloatToHalf(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint16_t sign = uint16_t((bits >> 16) & 0x8000);
  const int exponent = int((bits >> 23) & 0xFF) - 127 + 15;
  const uint32_t mantissa = bits & 0x7FFFFF;
  if (exponent <= 0) {
    return exponent < -10 ? sign : uint16_t(sign | ((mantissa | 0x800000) >> (14 - exponent)));
  }
  if (exponent >= 31) {
    return uint16_t(sign | 0x7BFF); // the largest half, for anything past it
  }
  return uint16_t(sign | (exponent << 10) | (mantissa >> 13));
}


// A grid as the textures of GXCreatePBRVolume. The points inside walls are empty, and a
// surface sits between those and the lit ones, so the texture filter would darken every
// wall; the empty points take the light of their lit neighbours first, layer by layer.
void UploadVolume(const File& file, const Grid& grid, GpuVolume& gpu) {
  constexpr size_t kPoint = 24;
  constexpr int kLayers = 16;
  const size_t sx = grid.size[0], sy = grid.size[1], sz = grid.size[2];
  const size_t count = sx * sy * sz;
  std::vector<uint8_t> points(file.data.begin() + grid.offset, file.data.begin() + grid.offset + count * kPoint);
  const auto lit = [&points](size_t index) {
    const uint8_t* p = points.data() + index * kPoint;
    // Means are not negative, so any set bit but the sign is light.
    return ((p[0] | p[2] | p[4]) != 0) || (((p[1] | p[3] | p[5]) & 0x7F) != 0);
  };
  std::vector<uint8_t> state(count); // 1: lit, 2: filled in this layer
  for (size_t i = 0; i < count; ++i) {
    state[i] = lit(i) ? 1 : 0;
  }
  const size_t step[3] = {1, sx, sx * sy};
  const size_t size[3] = {sx, sy, sz};
  for (int layer = 0; layer < kLayers; ++layer) {
    size_t filled = 0;
    size_t index = 0;
    for (size_t z = 0; z < sz; ++z) {
      for (size_t y = 0; y < sy; ++y) {
        for (size_t x = 0; x < sx; ++x, ++index) {
          if (state[index] != 0) {
            continue;
          }
          const size_t at[3] = {x, y, z};
          float halves[6] = {};
          float bytes[12] = {};
          int total = 0;
          for (int axis = 0; axis < 3; ++axis) {
            for (int side = 0; side < 2; ++side) {
              if (side == 0 ? at[axis] == 0 : at[axis] + 1 == size[axis]) {
                continue;
              }
              const size_t other = side == 0 ? index - step[axis] : index + step[axis];
              if (state[other] != 1) {
                continue;
              }
              const uint8_t* p = points.data() + other * kPoint;
              for (int i = 0; i < 6; ++i) {
                halves[i] += HalfToFloat(uint16_t(p[i * 2] | (p[i * 2 + 1] << 8)));
              }
              for (int i = 0; i < 12; ++i) {
                bytes[i] += float(p[12 + i]);
              }
              ++total;
            }
          }
          if (total == 0) {
            continue;
          }
          uint8_t* p = points.data() + index * kPoint;
          for (int i = 0; i < 6; ++i) {
            const uint16_t half = FloatToHalf(halves[i] / float(total));
            p[i * 2] = uint8_t(half);
            p[i * 2 + 1] = uint8_t(half >> 8);
          }
          for (int i = 0; i < 12; ++i) {
            p[12 + i] = uint8_t(bytes[i] / float(total) + 0.5f);
          }
          state[index] = 2;
          ++filled;
        }
      }
    }
    if (filled == 0) {
      break;
    }
    for (uint8_t& value : state) {
      value = value != 0 ? 1 : 0;
    }
  }
  std::vector<uint8_t> texels(count * 28);
  uint8_t* mean = texels.data();
  uint8_t* lobe = mean + count * 8;
  uint8_t* direction = lobe + count * 8;
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* p = points.data() + i * kPoint;
    std::memcpy(mean + i * 8, p, 6);
    std::memcpy(lobe + i * 8, p + 6, 6);
    mean[i * 8 + 7] = lobe[i * 8 + 7] = 0x3C; // alpha 1.0
    for (int channel = 0; channel < 3; ++channel) {
      uint8_t* out = direction + (count * channel + i) * 4;
      std::memcpy(out, p + 15 + channel * 3, 3);
      out[3] = p[12 + channel];
    }
  }
  gpu.id = sNextVolume++;
  if (sNextVolume == 0) {
    sNextVolume = 1;
  }
  GXCreatePBRVolume(gpu.id, grid.size[0], grid.size[1], grid.size[2], texels.data(), uint32_t(texels.size()));
}

// How far outside a grid a point is, in metres; 0 inside.
float GridDistance(const Grid& grid, const float pos[3]) {
  const float* m = grid.worldToGrid;
  const float scale = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
  float sum = 0.f;
  for (int row = 0; row < 3; ++row) {
    const float* r = m + row * 4;
    const float at = r[0] * pos[0] + r[1] * pos[1] + r[2] * pos[2] + r[3];
    const float out = std::max(std::max(-0.5f - at, at - (float(grid.size[row]) - 0.5f)), 0.f);
    sum += out * out;
  }
  return scale > 1e-12f ? std::sqrt(sum) / scale : 3.4e38f;
}

// A cube's average luminance, from its last mip (a texel or four a face), and the largest
// channel of its average colour.
float CubeAverage(const File& file, const Cube& cube, float& peak) {
  const uint32_t edge = std::max(cube.size >> (cube.mipCount - 1), 1u);
  const size_t blockBytes = size_t((edge + 3) / 4) * ((edge + 3) / 4) * 16;
  const uint8_t* blocks = file.data.data() + cube.offset + cube.length - blockBytes * 6;
  std::vector<uint16_t> texels(size_t(edge) * edge * 4);
  double sum[3] = {};
  for (uint32_t face = 0; face < 6; ++face) {
    PortRemastered::DecodeBc6hFace(blocks + blockBytes * face, edge, cube.isSigned, texels.data());
    for (size_t i = 0; i < size_t(edge) * edge; ++i) {
      for (int c = 0; c < 3; ++c) {
        sum[c] += HalfToFloat(texels[i * 4 + c]);
      }
    }
  }
  const double count = 6.0 * edge * edge;
  peak = float(std::max(std::max(sum[0], sum[1]), sum[2]) / count);
  return float((0.2126 * sum[0] + 0.7152 * sum[1] + 0.0722 * sum[2]) / count);
}

// What Remastered multiplies the room's radiance by before its tone curve: 2^(3 - EV).
// Without auto exposure EV is the Tonemap's own. With it (CPostFXManager::
// UpdateTonemapping), EV = log2(L / grey) + 3 + bias, held to the hint's range, where L is
// the largest channel of the last frame's average colour and grey is sRGB 128. The port
// has no HDR frame to average, so L is the middle one of the room's probes instead; over
// the 275 rooms that lands inside the hint's range for most.
float RoomExposure(const Area& area) {
  constexpr float kGrey = 0.2158605f; // sRGB 128, linear
  const File& file = area.file;
  float ev = file.tonemap[0];
  if (file.exposure[0] != 0.f || file.exposure[1] != 0.f) {
    std::vector<float> levels;
    for (const Probe& probe : file.probes) {
      const float level = area.cubes[probe.cube].peak * probe.scale;
      if (level > 0.f && std::isfinite(level)) {
        levels.push_back(level);
      }
    }
    float level = 0.f;
    if (!levels.empty()) {
      std::nth_element(levels.begin(), levels.begin() + levels.size() / 2, levels.end());
      level = levels[levels.size() / 2];
    } else {
      // The grid's points are the same radiance.
      for (const Grid& grid : file.grids) {
        level = std::max(level, grid.average);
      }
    }
    if (!(level > 0.f)) {
      return 0.f;
    }
    ev = std::log2(level / kGrey) + 3.f + file.exposureBias;
    ev = std::min(std::max(ev, file.exposure[0]), file.exposure[1]);
  }
  if (!std::isfinite(ev)) {
    return 0.f;
  }
  return std::exp2(3.f - ev);
}

void Load(uint32_t mrea, Area& area) {
  const std::string path = PortMods::RoomEnvPath(mrea);
  if (path.empty()) {
    return;
  }
  std::ifstream in(PortGci::PathFromString(path), std::ios::binary);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string error;
  if (!in || !Parse(std::move(data), area.file, error)) {
    PortLog::Write("room env: %s: %s\n", path.c_str(), error.empty() ? "cannot read" : error.c_str());
    area.file = {};
    return;
  }
  area.cubes.resize(area.file.cubes.size());
  for (size_t i = 0; i < area.cubes.size(); ++i) {
    area.cubes[i].average = CubeAverage(area.file, area.file.cubes[i], area.cubes[i].peak);
    // Black: nothing to reflect, and no exposure to set by it.
    area.cubes[i].failed = !(area.cubes[i].average > 1e-6f);
  }
  area.volumes.resize(area.file.grids.size());
  area.exposure = RoomExposure(area);
  const float* const t = area.file.tonemap;
  if (area.exposure > 0.f && t[1] > 0.f && t[1] < 1.f) {
    BuildTone(t[1], area.file.contrast, t[2], t[3], area.tone);
  }
  PortLog::Write("room env: %08X exposure %g (EV %g, hint %g..%g bias %g), tone mid %g contrast %g toe %g shoulder %g\n",
                 mrea, area.exposure, area.exposure > 0.f ? 3.f - std::log2(area.exposure) : 0.f,
                 area.file.exposure[0], area.file.exposure[1], area.file.exposureBias, t[1], area.file.contrast, t[2],
                 t[3]);
}

// The frame's exposure, for radiance from `area`: that of the room the camera is in, as
// one exposure covers the whole picture, or the area's own when that room has none.
float FrameExposure(const Area& area) {
  const auto view = sAreas.find(sViewArea);
  return view != sAreas.end() && view->second.exposure > 0.f ? view->second.exposure : area.exposure;
}

// Decodes a cube and hands it to the GPU.
void Upload(const File& file, const Cube& cube, GpuCube& gpu) {
  // RGBA16Float, every mip of face 0, then face 1 (GXCreatePBRCube); the file has every
  // face of mip 0, then mip 1.
  std::vector<size_t> mipOffset(cube.mipCount);
  size_t perFace = 0;
  for (uint32_t mip = 0; mip < cube.mipCount; ++mip) {
    const size_t edge = std::max(cube.size >> mip, 1u);
    mipOffset[mip] = perFace;
    perFace += edge * edge * 8;
  }
  std::vector<uint16_t> texels(perFace * 6 / 2);
  const uint8_t* blocks = file.data.data() + cube.offset;
  for (uint32_t mip = 0; mip < cube.mipCount; ++mip) {
    const uint32_t edge = std::max(cube.size >> mip, 1u);
    const size_t blockBytes = size_t((edge + 3) / 4) * ((edge + 3) / 4) * 16;
    for (uint32_t face = 0; face < 6; ++face) {
      uint16_t* out = texels.data() + (perFace * face + mipOffset[mip]) / 2;
      PortRemastered::DecodeBc6hFace(blocks, edge, cube.isSigned, out);
      blocks += blockBytes;
    }
  }
  gpu.id = sNextCube++;
  if (sNextCube == 0) {
    sNextCube = 1;
  }
  gpu.mipCount = cube.mipCount;
  GXCreatePBRCube(gpu.id, cube.size, cube.mipCount, texels.data(), uint32_t(texels.size() * 2));
}

} // namespace

void BuildTone(float mid, float contrast, float toe, float shoulder, float rows[3][4]) {
  const float a25 = std::atan2(0.25f, mid);
  const float a75 = std::atan2(0.75f, mid);
  const float slope = std::tan(a25 + contrast * (a75 - a25));
  const float lineEnd = mid + 0.75f * shoulder / slope;
  // The toe: a cubic through the origin that meets the line at `mid`, in value and slope.
  const float half = 0.5f * (0.75f / mid - slope);
  const float c = (half - slope >= 0.f ? slope : half) * (1.f - toe);
  rows[0][0] = (slope + c) / (mid * mid) - 0.5f / (mid * mid * mid);
  rows[0][1] = 0.75f / (mid * mid) - (slope + 2.f * c) / mid;
  rows[0][2] = c;
  rows[0][3] = 0.f;
  rows[1][0] = slope;
  rows[1][1] = 0.25f - slope * mid;
  rows[1][2] = mid;
  rows[1][3] = lineEnd;
  // The shoulder: from the line's end towards 1.
  const float top = 0.25f + 0.75f * shoulder;
  const float k = 1.f - top > 0.f ? slope / (1.f - top) : 0.f;
  rows[2][0] = 1.f - top;
  rows[2][1] = k;
  rows[2][2] = -lineEnd * k;
  rows[2][3] = top;
}

bool Tone(float rows[3][4]) {
  if (!Enabled() || !RoomExposed()) {
    return false;
  }
  const auto view = sAreas.find(sViewArea);
  if (view == sAreas.end() || !(view->second.tone[1][0] > 0.f)) {
    return false;
  }
  std::memcpy(rows, view->second.tone, sizeof(view->second.tone));
  return true;
}

void SetViewArea(uint32_t mrea) {
  if (sViewArea != mrea) {
    sViewArea = mrea;
    sLastValid = false;
  }
}

bool VolumesEnabled() {
  if (sVolumes < 0) {
    const char* const env = std::getenv("MP_ROOM_ENV_VOLUME");
    sVolumes = env != nullptr && env[0] == '0' ? 0 : 1;
  }
  return sVolumes != 0;
}

void SetVolumesEnabled(bool on) {
  sVolumes = on ? 1 : 0;
  sLastValid = false;
}

float AmbientScale() {
  if (sAmbientScale < 0.f) {
    sAmbientScale = std::max(EnvFloat("MP_ROOM_ENV_AMBIENT", 1.f), 0.f);
  }
  return sAmbientScale;
}

void SetAmbientScale(float scale) {
  sAmbientScale = std::max(scale, 0.f);
  sLastValid = false;
}

int VolumeView() {
  if (sVolumeView < 0.f) {
    sVolumeView = std::max(EnvFloat("MP_ROOM_ENV_VOLUME_SHOW", 0.f), 0.f);
  }
  return static_cast<int>(sVolumeView);
}

void SetVolumeView(int view) {
  sVolumeView = static_cast<float>(std::max(view, 0));
  sLastValid = false;
}

bool Enabled() {
  if (sEnabled < 0) {
    const char* const env = std::getenv("MP_ROOM_ENV");
    sEnabled = env != nullptr && env[0] == '0' ? 0 : 1;
  }
  return sEnabled != 0;
}

bool RoomExposed() {
  if (sExposure < 0) {
    const char* const env = std::getenv("MP_ROOM_ENV_EXPOSURE");
    sExposure = env != nullptr && env[0] == '0' ? 0 : 1;
  }
  return sExposure != 0;
}

void SetRoomExposed(bool on) {
  sExposure = on ? 1 : 0;
  sLastValid = false;
}

void SetEnabled(bool enabled) {
  sEnabled = enabled ? 1 : 0;
  sLastValid = false;
}

void SetVolumeHint(uint32_t mrea, const float centre[3]) {
  sHint = true;
  sHintArea = mrea;
  std::memcpy(sHintCentre, centre, sizeof(sHintCentre));
  sLastValid = false;
}

void ClearVolumeHint() {
  sHint = false;
  sLastValid = false;
}

bool HasVolume(uint32_t mrea) {
  if (!Enabled() || !VolumesEnabled()) {
    return false;
  }
  const auto found = sAreas.find(mrea);
  if (found == sAreas.end()) {
    return false;
  }
  for (const Grid& grid : found->second.file.grids) {
    if (grid.average > 0.f) {
      return true;
    }
  }
  return false;
}

void Reset() {
  for (auto& [mrea, area] : sAreas) {
    Free(area);
  }
  sAreas.clear();
  sLastValid = false;
}

void SetLoadedAreas(const uint32_t* mreas, size_t count) {
  bool changed = false;
  for (auto it = sAreas.begin(); it != sAreas.end();) {
    if (std::find(mreas, mreas + count, it->first) == mreas + count) {
      Free(it->second);
      it = sAreas.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (sAreas.find(mreas[i]) == sAreas.end()) {
      Load(mreas[i], sAreas[mreas[i]]);
      changed = true;
    }
  }
  if (changed) {
    sLastValid = false;
  }
}

bool Select(const float origin[3], Selection& out) {
  if (sLastValid && std::memcmp(origin, sLastPos, sizeof(sLastPos)) == 0) {
    out = sLast;
    return sLastFound;
  }
  std::memcpy(sLastPos, origin, sizeof(sLastPos));
  const float* const pos = sHint ? sHintCentre : origin;
  sLastValid = true;
  sLastFound = false;
  if (!Enabled()) {
    return false;
  }
  static const float gain = EnvFloat("MP_ROOM_ENV_GAIN", 1.f);
  static const float lod = EnvFloat("MP_ROOM_ENV_LOD", 5.f);
  const float ambient = AmbientScale();
  const float grey = 0.18f * gain;
  sLast = {};
  Area* bestArea = nullptr;
  Pick best;
  for (auto& [mrea, area] : sAreas) {
    const Pick pick = PickProbe(area.file, pos);
    if (pick.Better(best)) {
      best = pick;
      bestArea = &area;
    }
  }
  if (bestArea != nullptr) {
    const Probe& probe = bestArea->file.probes[best.probe];
    GpuCube& gpu = bestArea->cubes[probe.cube];
    if (gpu.id == 0 && !gpu.failed) {
      Upload(bestArea->file, bestArea->file.cubes[probe.cube], gpu);
    }
    if (gpu.id != 0) {
      // The cube is exposed so that its average direction is middle grey, which is what
      // Remastered's auto exposure aims for (its Tonemap's key is 0.18 too); the lamps in
      // it then come out many times brighter than white, as they should.
      // With the room's exposure the cube keeps its level instead: a probe in a dark
      // corner reflects a dark corner.
      const float exposure = RoomExposed() ? FrameExposure(*bestArea) : 0.f;
      const bool room = exposure > 0.f;
      sLast.cube = gpu.id;
      sLast.params[0] = room ? exposure * probe.scale * gain : grey / gpu.average;
      sLast.params[1] = std::min(lod, float(gpu.mipCount - 1));
      sLast.params[2] = float(gpu.mipCount > 2 ? gpu.mipCount - 2 : 0);
      sLast.params[3] = ambient > 0.f ? 1.f / (room ? gpu.average * sLast.params[0] : grey) : 0.f;
      std::memcpy(sLast.worldToCube, probe.worldToCube, sizeof(sLast.worldToCube));
    }
  }
  if (sHint && VolumesEnabled()) {
    static const float volumeBias = EnvFloat("MP_ROOM_ENV_VOLUME_BIAS", 0.25f);
    const auto found = sAreas.find(sHintArea);
    if (found != sAreas.end()) {
      Area& area = found->second;
      int pick = -1;
      float nearest = 0.f;
      for (size_t i = 0; i < area.file.grids.size(); ++i) {
        const float distance = GridDistance(area.file.grids[i], pos);
        if (area.file.grids[i].average > 0.f && (pick < 0 || distance < nearest)) {
          pick = int(i);
          nearest = distance;
        }
      }
      if (pick >= 0) {
        const Grid& grid = area.file.grids[pick];
        GpuVolume& gpu = area.volumes[pick];
        const bool fresh = gpu.id == 0;
        if (fresh) {
          UploadVolume(area.file, grid, gpu);
        }
        const float* m = grid.worldToGrid;
        const float scale = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        sLast.volume = gpu.id;
        for (int row = 0; row < 3; ++row) {
          // Point i is the middle of texel i.
          const float size = float(grid.size[row]);
          for (int col = 0; col < 4; ++col) {
            sLast.worldToVolume[row * 4 + col] = (m[row * 4 + col] + (col == 3 ? 0.5f : 0.f)) / size;
          }
          for (int col = 0; col < 3; ++col) {
            sLast.worldToAxes[row * 3 + col] = m[row * 4 + col] / scale;
          }
        }
        // The baked light is the level, at the frame's exposure; without one, the grid's
        // average comes out at the key. The grid holds irradiance, and a diffuse surface
        // sends 1/pi of that back.
        const float exposure = RoomExposed() ? FrameExposure(area) : 0.f;
        sLast.volumeLevel = (exposure > 0.f ? exposure * gain : 0.18f / grid.average) / 3.14159265f;
        sLast.volumeBias = volumeBias;
        sLast.volumeDiagnostic = static_cast<float>(VolumeView());
        if (fresh) {
          PortLog::Write("room env: %08X volume %u %ux%ux%u exposure %g average %g level %g\n", sHintArea, gpu.id,
                         grid.size[0], grid.size[1], grid.size[2], exposure, grid.average, sLast.volumeLevel);
        }
      }
    }
  }
  if (ambient > 0.f) {
    // A model's origin is often on the floor, where the grid has no point for it, so the
    // spot a metre up counts too.
    const float above[3] = {pos[0], pos[1], pos[2] + 1.f};
    Ambient sample;
    float average = 0.f;
    float roomExposure = 0.f;
    for (const float* spot : {pos, above}) {
      for (auto& [mrea, area] : sAreas) {
        for (const Grid& grid : area.file.grids) {
          if (!sLast.hasAmbient && grid.average > 0.f && SampleGrid(area.file, grid, spot, sample)) {
            sLast.hasAmbient = true;
            average = grid.average;
            roomExposure = FrameExposure(area);
          }
        }
      }
    }
    if (sLast.hasAmbient) {
      // The grid gives the light's colour and direction; how bright it is stays the game's
      // ambient, which the shader multiplies in. The baked levels are HDR that Remastered
      // exposes by what is on screen (one room spans 0.0001 to 100), and the world around
      // the model is still lit the retail way. Of the level only this is kept: a spot
      // darker or brighter than its room is, within a factor of two.
      const float luminance = 0.2126f * sample.mean[0] + 0.7152f * sample.mean[1] + 0.0722f * sample.mean[2];
      const float level = std::min(std::max(std::sqrt(luminance / average), 0.5f), 2.f);
      float exposure = luminance > 0.f ? level / luminance * ambient : 0.f;
      if (RoomExposed() && roomExposure > 0.f) {
        // Or the baked level itself, at the room's exposure: the game's ambient is left out.
        exposure = roomExposure * ambient * gain;
        sLast.ambientAbsolute = true;
      }
      for (int i = 0; i < 3; ++i) {
        sLast.ambient[0][i] = (sample.mean[i] - sample.lobe[i]) * exposure;
        sLast.ambient[1][i] = 2.f * sample.lobe[i] * (1.f + sample.sharpness[i]) * exposure;
        sLast.ambient[2][i] = 1.f + 2.f * sample.sharpness[i];
        std::memcpy(sLast.ambient[3 + i], sample.direction[i], sizeof(sample.direction[i]));
      }
    }
  }
  sLastFound = sLast.cube != 0 || sLast.hasAmbient || sLast.volume != 0;
  out = sLast;
  return sLastFound;
}

void Stats(int& areas, int& probes, int& cubes, int& grids) {
  areas = probes = cubes = grids = 0;
  for (const auto& [mrea, area] : sAreas) {
    if (!area.file.probes.empty() || !area.file.grids.empty()) {
      ++areas;
      probes += int(area.file.probes.size());
      grids += int(area.file.grids.size());
    }
    for (const GpuCube& cube : area.cubes) {
      cubes += cube.id != 0 ? 1 : 0;
    }
  }
}

std::string Info(const float pos[3]) {
  std::string out;
  char line[320];
  for (const auto& [mrea, area] : sAreas) {
    const File& file = area.file;
    if (file.probes.empty() && file.grids.empty()) {
      continue;
    }
    const float* const t = file.tonemap;
    std::snprintf(line, sizeof(line),
                  "%08X%s: exposure %g (EV %g, hint %g..%g, bias %g), tone EV %g mid %g contrast %g toe %g "
                  "shoulder %g, %zu probe(s), %zu cube(s), %zu grid(s)\n",
                  mrea, mrea == sViewArea ? " (camera)" : "", area.exposure,
                  area.exposure > 0.f ? 3.f - std::log2(area.exposure) : 0.f, file.exposure[0], file.exposure[1],
                  file.exposureBias, t[0], t[1], file.contrast, t[2], t[3], file.probes.size(), file.cubes.size(),
                  file.grids.size());
    out += line;
    const Pick pick = PickProbe(file, pos);
    if (pick.probe >= 0) {
      const Probe& probe = file.probes[pick.probe];
      const GpuCube& cube = area.cubes[probe.cube];
      std::snprintf(line, sizeof(line),
                    "  probe %d (%s, %s %g): cube %u, scale %g, blend %g, average %g, peak %g%s\n", pick.probe,
                    pick.inside ? "inside" : "outside", pick.inside ? "volume" : "distance", pick.score, probe.cube,
                    probe.scale, probe.blend, cube.average * probe.scale, cube.peak * probe.scale,
                    cube.failed ? ", black" : "");
      out += line;
    }
    for (size_t i = 0; i < file.grids.size(); ++i) {
      const Grid& grid = file.grids[i];
      const float distance = GridDistance(grid, pos);
      Ambient ambient;
      const bool lit = distance == 0.f && SampleGrid(file, grid, pos, ambient);
      int used = std::snprintf(line, sizeof(line), "  grid %zu: %u x %u x %u, average %g, ", i, grid.size[0],
                               grid.size[1], grid.size[2], grid.average);
      if (lit) {
        std::snprintf(line + used, sizeof(line) - used, "here mean %g %g %g, lobe %g %g %g\n", ambient.mean[0],
                      ambient.mean[1], ambient.mean[2], ambient.lobe[0], ambient.lobe[1], ambient.lobe[2]);
      } else if (distance == 0.f) {
        std::snprintf(line + used, sizeof(line) - used, "no lit point here\n");
      } else {
        std::snprintf(line + used, sizeof(line) - used, "%.1f m away\n", distance);
      }
      out += line;
    }
  }
  return out;
}

} // namespace PortRoomEnv

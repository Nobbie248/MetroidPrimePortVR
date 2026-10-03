// The .roomenv file: parsing, and picking the probe for a point. See port_room_env.h.
#include "port_room_env.h"

#include <cmath>
#include <cstring>

namespace PortRoomEnv {
namespace {

constexpr uint32_t kMagic = 0x5645504D; // 'MPEV'
constexpr uint32_t kVersion = 4;
constexpr size_t kHeaderSize = 32;
constexpr size_t kProbeSize = 100;
constexpr size_t kCubeHeaderSize = 16;
constexpr uint32_t kMaxProbes = 4096;
constexpr uint32_t kMaxCubeSize = 1024;
constexpr size_t kGridHeaderSize = 60;
constexpr size_t kPointSize = 24;
constexpr uint32_t kMaxGrids = 64;
constexpr uint32_t kMaxGridSize = 1024;

uint32_t Get32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

float GetFloat(const uint8_t* p) {
  const uint32_t bits = Get32(p);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

float GetHalf(const uint8_t* p) {
  const uint32_t h = uint32_t(p[0]) | (uint32_t(p[1]) << 8);
  const int exponent = int((h >> 10) & 0x1F);
  const int mantissa = int(h & 0x3FF);
  // An infinity is clamped; a NaN or a negative is no light.
  if ((h & 0x8000) != 0 || (exponent == 31 && mantissa != 0)) {
    return 0.f;
  }
  if (exponent == 31) {
    return 65504.f;
  }
  return exponent == 0 ? std::ldexp(float(mantissa), -24) : std::ldexp(float(mantissa | 0x400), exponent - 25);
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  c = char(c | 0x20);
  return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

} // namespace

bool ParseFileName(const std::string& fileName, uint32_t& id) {
  static const char kSuffix[] = ".roomenv";
  if (fileName.size() != 8 + sizeof(kSuffix) - 1) {
    return false;
  }
  for (size_t i = 0; i + 1 < sizeof(kSuffix); ++i) {
    const char c = fileName[8 + i];
    if ((c >= 'A' && c <= 'Z' ? char(c | 0x20) : c) != kSuffix[i]) {
      return false;
    }
  }
  id = 0;
  for (size_t i = 0; i < 8; ++i) {
    const int digit = HexDigit(fileName[i]);
    if (digit < 0) {
      return false;
    }
    id = (id << 4) | uint32_t(digit);
  }
  return true;
}

size_t CubeBytes(uint32_t size, uint32_t mipCount) {
  size_t bytes = 0;
  for (uint32_t mip = 0; mip < mipCount; ++mip) {
    const size_t edge = size >> mip > 0 ? size >> mip : 1;
    const size_t blocks = (edge + 3) / 4;
    bytes += blocks * blocks * 16 * 6;
  }
  return bytes;
}

bool Parse(std::vector<uint8_t>&& data, File& out, std::string& error) {
  out = {};
  if (data.size() < kHeaderSize || Get32(data.data()) != kMagic) {
    error = "not a room environment";
    return false;
  }
  const uint32_t version = Get32(data.data() + 4);
  if (version == 0 || version > kVersion) {
    error = "unknown version " + std::to_string(Get32(data.data() + 4));
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    out.tonemap[i] = GetFloat(data.data() + 8 + i * 4);
  }
  const uint32_t probes = Get32(data.data() + 24);
  const uint32_t cubes = Get32(data.data() + 28);
  if (probes > kMaxProbes || cubes > kMaxProbes || data.size() - kHeaderSize < size_t(probes) * kProbeSize) {
    error = "cut short";
    return false;
  }
  size_t at = kHeaderSize;
  out.probes.resize(probes);
  for (Probe& probe : out.probes) {
    const uint8_t* p = data.data() + at;
    for (int i = 0; i < 12; ++i) {
      probe.worldToBox[i] = GetFloat(p + i * 4);
    }
    for (int i = 0; i < 9; ++i) {
      probe.worldToCube[i] = GetFloat(p + 48 + i * 4);
    }
    probe.layer = int32_t(Get32(p + 84));
    probe.cube = Get32(p + 88);
    probe.scale = GetFloat(p + 92);
    probe.blend = GetFloat(p + 96);
    if (probe.cube >= cubes) {
      error = "a probe names a cube the file does not have";
      return false;
    }
    for (int i = 0; i < 21; ++i) {
      if (!std::isfinite(i < 12 ? probe.worldToBox[i] : probe.worldToCube[i - 12])) {
        error = "a probe is not finite";
        return false;
      }
    }
    at += kProbeSize;
  }
  out.cubes.resize(cubes);
  for (Cube& cube : out.cubes) {
    if (data.size() - at < kCubeHeaderSize) {
      error = "cut short";
      return false;
    }
    const uint8_t* p = data.data() + at;
    cube.size = Get32(p);
    cube.mipCount = Get32(p + 4);
    cube.isSigned = Get32(p + 8) != 0;
    cube.length = Get32(p + 12);
    cube.offset = at + kCubeHeaderSize;
    if (cube.size == 0 || cube.size > kMaxCubeSize || (cube.size & (cube.size - 1)) != 0 || cube.mipCount == 0 ||
        cube.mipCount > 11 || (cube.size >> (cube.mipCount - 1)) == 0) {
      error = "bad cube size";
      return false;
    }
    if (cube.length != CubeBytes(cube.size, cube.mipCount) || data.size() - cube.offset < cube.length) {
      error = "cut short";
      return false;
    }
    at = cube.offset + cube.length;
  }
  if (version >= 2) {
    if (data.size() - at < 4) {
      error = "cut short";
      return false;
    }
    const uint32_t grids = Get32(data.data() + at);
    at += 4;
    if (grids > kMaxGrids) {
      error = "too many grids";
      return false;
    }
    out.grids.resize(grids);
    for (Grid& grid : out.grids) {
      if (data.size() - at < kGridHeaderSize) {
        error = "cut short";
        return false;
      }
      const uint8_t* p = data.data() + at;
      for (int i = 0; i < 12; ++i) {
        grid.worldToGrid[i] = GetFloat(p + i * 4);
        if (!std::isfinite(grid.worldToGrid[i])) {
          error = "a grid is not finite";
          return false;
        }
      }
      size_t points = 1;
      for (int i = 0; i < 3; ++i) {
        grid.size[i] = Get32(p + 48 + i * 4);
        if (grid.size[i] == 0 || grid.size[i] > kMaxGridSize) {
          error = "bad grid size";
          return false;
        }
        points *= grid.size[i];
      }
      grid.offset = at + kGridHeaderSize;
      if ((data.size() - grid.offset) / kPointSize < points) {
        error = "cut short";
        return false;
      }
      double sum = 0.0;
      size_t filled = 0;
      // A sample of the points is enough for an average, and a big room has millions.
      const size_t step = points / 32768 + 1;
      for (size_t i = 0; i < points; i += step) {
        const uint8_t* point = data.data() + grid.offset + i * kPointSize;
        const double luminance = 0.2126 * GetHalf(point) + 0.7152 * GetHalf(point + 2) + 0.0722 * GetHalf(point + 4);
        if (luminance > 0.0) {
          sum += std::log(luminance);
          ++filled;
        }
      }
      // The geometric mean: a room's points span five decades, and the arithmetic mean is
      // only its few brightest.
      grid.average = filled != 0 ? float(std::exp(sum / double(filled))) : 0.f;
      at = grid.offset + points * kPointSize;
    }
  }
  if (version >= 3) {
    if (data.size() - at < 8) {
      error = "cut short";
      return false;
    }
    for (int i = 0; i < 2; ++i) {
      out.exposure[i] = GetFloat(data.data() + at + i * 4);
    }
    if (!std::isfinite(out.exposure[0]) || !std::isfinite(out.exposure[1]) || out.exposure[1] < out.exposure[0]) {
      out.exposure[0] = out.exposure[1] = 0.f;
    }
    at += 8;
  }
  if (version >= 4) {
    if (data.size() - at < 8) {
      error = "cut short";
      return false;
    }
    out.exposureBias = GetFloat(data.data() + at);
    out.contrast = GetFloat(data.data() + at + 4);
    if (!std::isfinite(out.exposureBias)) {
      out.exposureBias = 0.f;
    }
    if (!(out.contrast >= 0.f && out.contrast <= 1.f)) {
      out.contrast = 0.f;
    }
  }
  out.data = std::move(data);
  return true;
}

Pick PickProbe(const File& file, const float pos[3]) {
  Pick best;
  for (size_t i = 0; i < file.probes.size(); ++i) {
    const float* m = file.probes[i].worldToBox;
    Pick pick;
    pick.probe = int(i);
    pick.inside = true;
    float volume = 8.f;
    float distance2 = 0.f;
    for (int row = 0; row < 3; ++row) {
      const float* r = m + row * 4;
      const float u = r[0] * pos[0] + r[1] * pos[1] + r[2] * pos[2] + r[3];
      // The row's length is 1 / half extent.
      const float scale = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
      const float half = scale > 1e-12f ? 1.f / scale : 0.f;
      volume *= half;
      const float over = std::fabs(u) - 1.f;
      if (!(over <= 0.f)) {
        pick.inside = false;
        distance2 += over * half * over * half;
      }
    }
    pick.score = pick.inside ? volume : std::sqrt(distance2);
    if (pick.Better(best)) {
      best = pick;
    }
  }
  return best;
}

bool SampleGrid(const File& file, const Grid& grid, const float pos[3], Ambient& out) {
  const float* m = grid.worldToGrid;
  float at[3];
  int cell[3];
  for (int row = 0; row < 3; ++row) {
    const float* r = m + row * 4;
    at[row] = r[0] * pos[0] + r[1] * pos[1] + r[2] * pos[2] + r[3];
    // A point's light reaches half a cell past the edge, no further.
    if (!(at[row] >= -0.5f && at[row] <= float(grid.size[row]) - 0.5f)) {
      return false;
    }
    cell[row] = int(std::floor(at[row]));
    at[row] -= float(cell[row]);
  }
  // mean, lobe, sharpness, then the three directions along the grid's axes
  float sum[18] = {};
  float total = 0.f;
  for (int corner = 0; corner < 8; ++corner) {
    float weight = 1.f;
    size_t index = 0;
    bool outside = false;
    for (int axis = 2; axis >= 0; --axis) {
      const int high = (corner >> axis) & 1;
      const int i = cell[axis] + high;
      weight *= high != 0 ? at[axis] : 1.f - at[axis];
      outside = outside || i < 0 || i >= int(grid.size[axis]);
      index = index * grid.size[axis] + size_t(outside ? 0 : i);
    }
    if (outside || weight <= 0.f) {
      continue;
    }
    const uint8_t* p = file.data.data() + grid.offset + index * kPointSize;
    const float mean[3] = {GetHalf(p), GetHalf(p + 2), GetHalf(p + 4)};
    if (mean[0] + mean[1] + mean[2] <= 0.f) {
      continue;
    }
    for (int i = 0; i < 3; ++i) {
      sum[i] += weight * mean[i];
      sum[3 + i] += weight * GetHalf(p + 6 + i * 2);
      sum[6 + i] += weight * float(p[12 + i]) / 255.f;
    }
    for (int i = 0; i < 9; ++i) {
      sum[9 + i] += weight * (float(p[15 + i]) / 127.5f - 1.f);
    }
    total += weight;
  }
  if (total < 0.02f) {
    return false;
  }
  for (float& value : sum) {
    value /= total;
  }
  // The grid's axes are the world's turned and scaled alike, so a direction goes back
  // through the transpose.
  const float scale = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
  if (!(scale > 1e-12f)) {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    out.mean[i] = sum[i];
    out.lobe[i] = sum[3 + i];
    out.sharpness[i] = sum[6 + i];
    const float* d = sum + 9 + i * 3;
    for (int axis = 0; axis < 3; ++axis) {
      out.direction[i][axis] = (m[axis] * d[0] + m[4 + axis] * d[1] + m[8 + axis] * d[2]) / scale;
    }
  }
  return true;
}

} // namespace PortRoomEnv

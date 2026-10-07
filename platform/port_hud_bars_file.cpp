// The .hudbars file: parsing, writing, and walking a bar's strip. See port_hud_bars.h.
#include "port_hud_bars.h"
#include "port_strings.h"
#include "port_bytes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace PortHudBars {
namespace {

constexpr uint32_t kMagic = 0x52414248; // 'HBAR'
constexpr uint32_t kVersion = 1;
constexpr size_t kStationSize = 40;
constexpr uint32_t kMaxBars = 64;
constexpr uint32_t kMaxName = 256;
constexpr uint32_t kMaxStations = 65536;

using port::AppendLE32;
using port::AppendLEFloat;
using port::ReadLE32;
using port::ReadLEFloat;

using port::HexDigit;

// The first station past `t`; stations at `t` itself are not past it.
size_t After(const Bar& bar, float t) {
  return size_t(std::upper_bound(bar.at.begin(), bar.at.end(), t) - bar.at.begin());
}

} // namespace

bool ParseFileName(const std::string& fileName, uint32_t& id) {
  return port::ParseHexFileName(fileName, ".hudbars", id);
}

bool ParseFile(const uint8_t* data, size_t size, Bars& out) {
  out.clear();
  if (size < 12 || ReadLE32(data) != kMagic || ReadLE32(data + 4) != kVersion) {
    return false;
  }
  const uint32_t count = ReadLE32(data + 8);
  if (count > kMaxBars) {
    return false;
  }
  size_t at = 12;
  Bars bars;
  for (uint32_t i = 0; i < count; ++i) {
    if (size - at < 4) {
      return false;
    }
    const uint32_t nameLength = ReadLE32(data + at);
    at += 4;
    if (nameLength > kMaxName || size - at < size_t(nameLength) + 4) {
      return false;
    }
    Bar bar;
    bar.name.assign(reinterpret_cast<const char*>(data + at), nameLength);
    at += nameLength;
    const uint32_t stations = ReadLE32(data + at);
    at += 4;
    if (stations < 2 || stations > kMaxStations || (size - at) / kStationSize < stations) {
      return false;
    }
    bar.stations.resize(stations);
    for (Station& station : bar.stations) {
      float values[10];
      for (int k = 0; k < 10; ++k) {
        values[k] = ReadLEFloat(data + at + size_t(k) * 4);
        if (!std::isfinite(values[k])) {
          return false;
        }
      }
      std::memcpy(station.a, values, sizeof(station.a));
      std::memcpy(station.b, values + 3, sizeof(station.b));
      std::memcpy(station.uvA, values + 6, sizeof(station.uvA));
      std::memcpy(station.uvB, values + 8, sizeof(station.uvB));
      at += kStationSize;
    }
    if (!Measure(bar)) {
      return false;
    }
    bars.push_back(std::move(bar));
  }
  out = std::move(bars);
  return true;
}

void WriteFile(const Bars& bars, std::vector<uint8_t>& out) {
  out.clear();
  AppendLE32(out, kMagic);
  AppendLE32(out, kVersion);
  AppendLE32(out, uint32_t(bars.size()));
  for (const Bar& bar : bars) {
    AppendLE32(out, uint32_t(bar.name.size()));
    out.insert(out.end(), bar.name.begin(), bar.name.end());
    AppendLE32(out, uint32_t(bar.stations.size()));
    for (const Station& station : bar.stations) {
      for (float value : station.a) {
        AppendLEFloat(out, value);
      }
      for (float value : station.b) {
        AppendLEFloat(out, value);
      }
      for (float value : station.uvA) {
        AppendLEFloat(out, value);
      }
      for (float value : station.uvB) {
        AppendLEFloat(out, value);
      }
    }
  }
}

bool Measure(Bar& bar) {
  bar.at.assign(bar.stations.size(), 0.f);
  float length = 0.f;
  for (size_t i = 1; i < bar.stations.size(); ++i) {
    const Station& from = bar.stations[i - 1];
    const Station& to = bar.stations[i];
    float square = 0.f;
    for (int k = 0; k < 3; ++k) {
      const float step = ((to.a[k] + to.b[k]) - (from.a[k] + from.b[k])) * 0.5f;
      square += step * step;
    }
    length += std::sqrt(square);
    bar.at[i] = length;
  }
  if (bar.stations.size() < 2 || !(length > 0.f) || !std::isfinite(length)) {
    return false;
  }
  for (float& at : bar.at) {
    at /= length;
  }
  bar.at.back() = 1.f;
  return true;
}

Station Sample(const Bar& bar, float t) {
  t = std::clamp(t, 0.f, 1.f);
  const size_t index = std::min(std::max(After(bar, t), size_t(1)), bar.stations.size() - 1) - 1;
  const float span = bar.at[index + 1] - bar.at[index];
  const float f = span > 0.f ? std::clamp((t - bar.at[index]) / span, 0.f, 1.f) : 0.f;
  const Station& from = bar.stations[index];
  const Station& to = bar.stations[index + 1];
  Station out;
  for (int i = 0; i < 3; ++i) {
    out.a[i] = from.a[i] + (to.a[i] - from.a[i]) * f;
    out.b[i] = from.b[i] + (to.b[i] - from.b[i]) * f;
  }
  for (int i = 0; i < 2; ++i) {
    out.uvA[i] = from.uvA[i] + (to.uvA[i] - from.uvA[i]) * f;
    out.uvB[i] = from.uvB[i] + (to.uvB[i] - from.uvB[i]) * f;
  }
  return out;
}

void Inside(const Bar& bar, float from, float to, size_t& first, size_t& last) {
  first = After(bar, std::clamp(from, 0.f, 1.f));
  last = size_t(std::lower_bound(bar.at.begin(), bar.at.end(), std::clamp(to, 0.f, 1.f)) - bar.at.begin());
}

} // namespace PortHudBars

// SPDX-License-Identifier: GPL-3.0-or-later

#include "dds_preview.h"

#include <array>
#include <cstring>

namespace PrimedGunLauncher {
namespace {

uint16_t ReadU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t ReadU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

struct Rgba {
  uint8_t r = 0, g = 0, b = 0, a = 0;
};

Rgba Expand565(uint16_t color) {
  const int r5 = (color >> 11) & 0x1f;
  const int g6 = (color >> 5) & 0x3f;
  const int b5 = color & 0x1f;
  return {static_cast<uint8_t>((r5 << 3) | (r5 >> 2)), static_cast<uint8_t>((g6 << 2) | (g6 >> 4)),
          static_cast<uint8_t>((b5 << 3) | (b5 >> 2)), 255};
}

Rgba Mix(Rgba a, Rgba b, int aw, int bw, int div) {
  return {static_cast<uint8_t>((a.r * aw + b.r * bw) / div),
          static_cast<uint8_t>((a.g * aw + b.g * bw) / div),
          static_cast<uint8_t>((a.b * aw + b.b * bw) / div), 255};
}

} // namespace

RgbaImage DecodeDxt1Dds(const uint8_t* data, size_t size) {
  constexpr size_t kHeaderSize = 128; // "DDS " + DDS_HEADER
  constexpr uint32_t kDxt1 = 0x31545844; // 'DXT1'
  if (size < kHeaderSize || std::memcmp(data, "DDS ", 4) != 0) {
    return {};
  }
  const uint32_t height = ReadU32(data + 12);
  const uint32_t width = ReadU32(data + 16);
  const uint32_t fourcc = ReadU32(data + 84);
  if (width == 0 || height == 0 || width > 8192 || height > 8192 || fourcc != kDxt1) {
    return {};
  }

  RgbaImage image;
  image.width = static_cast<int>(width);
  image.height = static_cast<int>(height);
  image.pixels.assign(static_cast<size_t>(width) * height * 4, 0);

  const uint32_t blocksX = (width + 3) / 4;
  const uint32_t blocksY = (height + 3) / 4;
  size_t offset = kHeaderSize;
  for (uint32_t blockY = 0; blockY < blocksY; ++blockY) {
    for (uint32_t blockX = 0; blockX < blocksX; ++blockX) {
      if (offset + 8 > size) {
        return image;
      }
      const uint16_t c0 = ReadU16(data + offset);
      const uint16_t c1 = ReadU16(data + offset + 2);
      const uint32_t indices = ReadU32(data + offset + 4);
      offset += 8;

      std::array<Rgba, 4> colors{};
      colors[0] = Expand565(c0);
      colors[1] = Expand565(c1);
      if (c0 > c1) {
        colors[2] = Mix(colors[0], colors[1], 2, 1, 3);
        colors[3] = Mix(colors[0], colors[1], 1, 2, 3);
      } else {
        colors[2] = Mix(colors[0], colors[1], 1, 1, 2);
        colors[3] = {0, 0, 0, 0};
      }

      for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
          const uint32_t px = blockX * 4 + x;
          const uint32_t py = blockY * 4 + y;
          if (px >= width || py >= height) {
            continue;
          }
          const Rgba& c = colors[(indices >> (2 * (y * 4 + x))) & 0x3];
          uint8_t* out = &image.pixels[(static_cast<size_t>(py) * width + px) * 4];
          out[0] = c.r;
          out[1] = c.g;
          out[2] = c.b;
          out[3] = c.a;
        }
      }
    }
  }
  return image;
}

} // namespace PrimedGunLauncher

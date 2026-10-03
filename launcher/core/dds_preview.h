// SPDX-License-Identifier: GPL-3.0-or-later
//
// A DXT1 (BC1) .dds decoded to RGBA8 for the Cannon Textures previews; Qt has
// no DDS reader of its own. From PrimedGun's PrimedGunDecodeDxt1Preview
// (DolphinQt/MainWindow.cpp).

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace PrimedGunLauncher {

struct RgbaImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> pixels; // width * height * 4, row-major RGBA
  bool Empty() const { return width == 0 || height == 0; }
};

// The base level of a DXT1 .dds file's bytes; empty for anything else. A
// truncated file decodes as far as its blocks go.
RgbaImage DecodeDxt1Dds(const uint8_t* data, size_t size);

} // namespace PrimedGunLauncher

#pragma once

// The Extras gallery's pictures (see docs/NATIVE_PORT.md): Remastered's concept art, kept in the mod as
// gallery/NNN.jpg. The import makes the JPEGs with the port's own encoder (port_remastered_jpeg.h); the viewer reads
// them back with the THP decoder. No SDL and no ImGui in here, so the desktop builds and tests it.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace PortGallery {

// The largest a stored picture gets; a bigger one is scaled down to fit, keeping its aspect.
inline constexpr int kMaxWidth = 1920;
inline constexpr int kMaxHeight = 1080;

// `rgba` (width * height * 4 bytes, alpha ignored) to a JPEG: fitted within kMaxWidth x kMaxHeight, never enlarged,
// even in both directions, quality 90, BT.601 over the full range. False when the size is not positive.
bool EncodeGalleryJpeg(const uint8_t* rgba, int width, int height, std::vector<uint8_t>& jpeg);

// A baseline 4:2:0 JPEG to RGBA8 with alpha 255. False when it is none, or truncated before its scan data.
bool DecodeGalleryJpeg(const uint8_t* data, size_t size, int& width, int& height, std::vector<uint8_t>& rgba);

} // namespace PortGallery

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Whether a disc image is one the game will boot: Metroid Prime NTSC-U
// revision 0 (GM8E01, disc 0, v1.00; platform/main.cpp checks the same after
// mounting it), in a format the port opens (main.cpp IsDiscImage).

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace PrimedGunLauncher {

enum class DiscCheck {
  Ok,
  WrongGame,
  WrongRevision,
  UnsupportedFormat, // an extension the port does not open (.gcz, ...)
  Unverified,        // a format whose header is not read here (.wbfs); checked at boot
  Unreadable,
};

struct DiscInfo {
  DiscCheck check = DiscCheck::Unreadable;
  std::string gameId; // six characters when the header was found
  int revision = -1;
};

// The port's own extension list: .iso .gcm .rvz .wbfs .ciso .nkit.
bool IsSupportedDiscExtension(const std::filesystem::path& path);

DiscInfo ProbeDisc(const std::filesystem::path& path);
// The same on the first bytes of a file (at least 0x8008 for a CISO image).
DiscInfo ProbeDiscBytes(std::string_view extension, const uint8_t* data, size_t size);

} // namespace PrimedGunLauncher

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Whether a disc image is one the game will boot: Metroid Prime USA 1.00/1.01/1.02
// (GM8E01, revisions 0-2) or PAL (GM8P01, revision 0), disc 0, as
// platform/disc.cpp PortDisc::IsAccepted takes them (main.cpp checks the
// mounted disc again; USA 1.01 and 1.02 boot only under MP_DISC_ANY_VERSION,
// which is left to the game), in a format the port opens (main.cpp IsDiscImage).

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
  // A PAL disc (GM8P01), whatever the check says: its game keeps its saves on
  // Dolphin's EUR card instead of the USA one (PortGci::CardRegion).
  bool Pal() const { return gameId == "GM8P01"; }
};

// The port's own extension list: .iso .gcm .rvz .wbfs .ciso .nkit.
bool IsSupportedDiscExtension(const std::filesystem::path& path);

DiscInfo ProbeDisc(const std::filesystem::path& path);
// The same on the first bytes of a file (at least 0x8008 for a CISO image).
DiscInfo ProbeDiscBytes(std::string_view extension, const uint8_t* data, size_t size);

} // namespace PrimedGunLauncher

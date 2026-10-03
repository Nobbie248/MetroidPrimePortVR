// SPDX-License-Identifier: GPL-3.0-or-later

#include "disc_probe.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <vector>

namespace PrimedGunLauncher {
namespace {

// WIA and RVZ: a 0x48-byte file header, then the disc struct whose first 16
// bytes (type, compression, level, chunk size) precede a copy of the disc's
// own 0x80-byte header.
constexpr size_t kWiaDiscHeaderOffset = 0x48 + 0x10;
// CISO: a 0x8000-byte header (magic, block size, block map), then the blocks
// in order; the disc header is at the start of block 0 when the map has it.
constexpr size_t kCisoDataOffset = 0x8000;
constexpr size_t kProbeSize = kCisoDataOffset + 8;

std::string LowerExtension(const std::filesystem::path& path) {
  const std::u8string u8 = path.extension().u8string();
  std::string ext(u8.begin(), u8.end());
  for (char& c : ext) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return ext;
}

DiscInfo FromHeader(const uint8_t* header) {
  DiscInfo info;
  info.gameId.assign(reinterpret_cast<const char*>(header), 6);
  info.revision = header[7];
  if (info.gameId != "GM8E01" || header[6] != 0) {
    info.check = DiscCheck::WrongGame;
  } else if (header[7] != 0) {
    info.check = DiscCheck::WrongRevision;
  } else {
    info.check = DiscCheck::Ok;
  }
  return info;
}

} // namespace

bool IsSupportedDiscExtension(const std::filesystem::path& path) {
  static const char* const kExtensions[] = {".iso", ".gcm", ".rvz", ".wbfs", ".ciso", ".nkit"};
  const std::string ext = LowerExtension(path);
  for (const char* candidate : kExtensions) {
    if (ext == candidate) {
      return true;
    }
  }
  return false;
}

DiscInfo ProbeDiscBytes(std::string_view extension, const uint8_t* data, size_t size) {
  DiscInfo unreadable;
  if (size >= 4 && (std::memcmp(data, "RVZ\x01", 4) == 0 || std::memcmp(data, "WIA\x01", 4) == 0)) {
    return size >= kWiaDiscHeaderOffset + 8 ? FromHeader(data + kWiaDiscHeaderOffset) : unreadable;
  }
  if (size >= 4 && std::memcmp(data, "CISO", 4) == 0) {
    // data[8] is block 0's entry in the map.
    if (size < kProbeSize || data[8] == 0) {
      return unreadable;
    }
    return FromHeader(data + kCisoDataOffset);
  }
  if (extension == ".wbfs") {
    DiscInfo info;
    info.check = DiscCheck::Unverified;
    return info;
  }
  return size >= 8 ? FromHeader(data) : unreadable;
}

DiscInfo ProbeDisc(const std::filesystem::path& path) {
  if (!IsSupportedDiscExtension(path)) {
    DiscInfo info;
    info.check = DiscCheck::UnsupportedFormat;
    return info;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  std::vector<uint8_t> bytes(kProbeSize);
  in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  const size_t got = static_cast<size_t>(in.gcount());
  return ProbeDiscBytes(LowerExtension(path), bytes.data(), got);
}

} // namespace PrimedGunLauncher

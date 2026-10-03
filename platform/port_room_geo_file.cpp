// The .roomgeo file. See port_room_geo.h.
#include "port_room_geo.h"

#include <cmath>
#include <cstring>

namespace PortRoomGeo {
namespace {

constexpr uint32_t kMagic = 0x4752504D; // 'MPRG'
constexpr uint32_t kVersion = 2;
constexpr size_t kHeaderBytes = 12;
constexpr size_t kInstanceBytes = 4 + 12 * 4; // version 1; version 2 adds 4 + links
constexpr size_t kLinkBytes = 8;

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

void PutU32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

} // namespace

bool ParseFileName(const std::string& fileName, uint32_t& id) {
  static const char kSuffix[] = ".roomgeo";
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

bool Parse(const std::vector<uint8_t>& data, std::vector<Instance>& out, std::string& error) {
  out.clear();
  if (data.size() < kHeaderBytes || ReadU32(data.data()) != kMagic) {
    error = "not a room geometry file";
    return false;
  }
  const uint32_t version = ReadU32(data.data() + 4);
  if (version != 1 && version != kVersion) {
    error = "unknown version";
    return false;
  }
  const uint32_t count = ReadU32(data.data() + 8);
  const size_t instanceBytes = version == 1 ? kInstanceBytes : kInstanceBytes + 4;
  if (count > (data.size() - kHeaderBytes) / instanceBytes) {
    error = "truncated";
    return false;
  }
  out.clear();
  out.resize(count);
  size_t at = kHeaderBytes;
  for (uint32_t i = 0; i < count; ++i) {
    if (data.size() - at < instanceBytes) {
      error = "truncated";
      out.clear();
      return false;
    }
    const uint8_t* const p = data.data() + at;
    Instance& instance = out[i];
    instance.model = ReadU32(p);
    for (int j = 0; j < 12; ++j) {
      const uint32_t bits = ReadU32(p + 4 + j * 4);
      std::memcpy(&instance.transform[j], &bits, 4);
      if (!std::isfinite(instance.transform[j])) {
        error = "bad transform";
        out.clear();
        return false;
      }
    }
    at += instanceBytes;
    if (version == 1) {
      continue;
    }
    instance.layer = p[kInstanceBytes];
    instance.active = p[kInstanceBytes + 1] != 0;
    const size_t links = size_t(p[kInstanceBytes + 2]) | size_t(p[kInstanceBytes + 3]) << 8;
    if ((data.size() - at) / kLinkBytes < links) {
      error = "truncated";
      out.clear();
      return false;
    }
    instance.links.resize(links);
    for (Link& link : instance.links) {
      link.sender = ReadU32(data.data() + at);
      link.state = data[at + 4];
      link.action = data[at + 5];
      at += kLinkBytes;
    }
  }
  return true;
}

std::vector<uint8_t> Write(const std::vector<Instance>& instances) {
  std::vector<uint8_t> out;
  out.reserve(kHeaderBytes + instances.size() * kInstanceBytes);
  PutU32(out, kMagic);
  PutU32(out, kVersion);
  PutU32(out, uint32_t(instances.size()));
  for (const Instance& instance : instances) {
    PutU32(out, instance.model);
    for (int j = 0; j < 12; ++j) {
      uint32_t bits;
      std::memcpy(&bits, &instance.transform[j], 4);
      PutU32(out, bits);
    }
    const size_t links = instance.links.size() < 0xffff ? instance.links.size() : 0xffff;
    out.push_back(instance.layer);
    out.push_back(instance.active ? 1 : 0);
    out.push_back(uint8_t(links));
    out.push_back(uint8_t(links >> 8));
    for (size_t j = 0; j < links; ++j) {
      PutU32(out, instance.links[j].sender);
      out.push_back(instance.links[j].state);
      out.push_back(instance.links[j].action);
      out.push_back(0);
      out.push_back(0);
    }
  }
  return out;
}

} // namespace PortRoomGeo

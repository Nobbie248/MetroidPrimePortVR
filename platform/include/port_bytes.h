#pragma once

// Little- and big-endian loads and stores for the port's file readers and
// writers. Each helper here used to be a file-local copy in a platform/*.cpp
// file; gathering them keeps the copies from drifting apart. They are exact
// on any host: the bytes are assembled with shifts, and floats go through
// memcpy rather than a reinterpret cast.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace port {

// The bytes at `p`, least significant first.
inline uint16_t ReadLE16(const uint8_t* p) {
  return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8);
}

inline uint32_t ReadLE32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline uint64_t ReadLE64(const uint8_t* p) {
  return uint64_t(ReadLE32(p)) | (uint64_t(ReadLE32(p + 4)) << 32);
}

// A little-endian IEEE-754 binary32.
inline float ReadLEFloat(const uint8_t* p) {
  const uint32_t bits = ReadLE32(p);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// The bytes at `p`, most significant first. FourCCs are read this way so
// they stay printable and comparable as one number.
inline uint32_t ReadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// A big-endian IEEE-754 binary32.
inline float ReadBEFloat(const uint8_t* p) {
  const uint32_t bits = ReadBE32(p);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// Big-endian words at an offset inside a byte vector, the layout of the
// disc's PAK-like formats.
inline uint32_t GetBE32(const std::vector<uint8_t>& data, size_t at) {
  return (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) | (uint32_t(data[at + 2]) << 8) |
         uint32_t(data[at + 3]);
}

inline float GetBEFloat(const std::vector<uint8_t>& data, size_t at) {
  const uint32_t bits = GetBE32(data, at);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline void SetBE32(std::vector<uint8_t>& data, size_t at, uint32_t value) {
  data[at] = uint8_t(value >> 24);
  data[at + 1] = uint8_t(value >> 16);
  data[at + 2] = uint8_t(value >> 8);
  data[at + 3] = uint8_t(value);
}

inline void SetBEFloat(std::vector<uint8_t>& data, size_t at, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  SetBE32(data, at, bits);
}

// The appends take any byte container with push_back: std::vector<uint8_t>
// for blobs, std::string for the savestate writer.
template <typename Bytes>
void AppendLE16(Bytes& out, uint16_t value) {
  out.push_back(uint8_t(value));
  out.push_back(uint8_t(value >> 8));
}

template <typename Bytes>
void AppendLE32(Bytes& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

template <typename Bytes>
void AppendLE64(Bytes& out, uint64_t value) {
  AppendLE32(out, uint32_t(value));
  AppendLE32(out, uint32_t(value >> 32));
}

template <typename Bytes>
void AppendBE16(Bytes& out, uint16_t value) {
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value));
}

template <typename Bytes>
void AppendBE32(Bytes& out, uint32_t value) {
  out.push_back(uint8_t(value >> 24));
  out.push_back(uint8_t(value >> 16));
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value));
}

template <typename Bytes>
void AppendLEFloat(Bytes& out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  AppendLE32(out, bits);
}

// A double is narrowed to binary32 first, matching what the formats store.
template <typename Bytes>
void AppendLEFloat(Bytes& out, double value) {
  AppendLEFloat(out, float(value));
}

template <typename Bytes>
void AppendBEFloat(Bytes& out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  AppendBE32(out, bits);
}

template <typename Bytes>
void AppendBEFloat(Bytes& out, double value) {
  AppendBEFloat(out, float(value));
}

}  // namespace port

#pragma once

// Readers for the two small formats around a Switch executable: the PFS0 that
// holds an ExeFS, and the NSO (LZ4-compressed segments) that is `main` in it.
// Pure parsing, no crypto, so it is testable without any user data.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace PortRemastered {

// LZ4 block format. `outSize` is exact: a stream that decodes to anything else,
// reads past `size`, or points before the start of the output is an error.
bool Lz4Decode(const uint8_t* src, size_t size, size_t outSize, std::vector<uint8_t>& out, std::string& error);

struct Pfs0Entry {
  std::string name;
  uint64_t offset = 0;  // from the start of the PFS0
  uint64_t size = 0;
};
// Size of the header, entries and strings, from the first 16 bytes. 0 if implausible.
size_t Pfs0HeaderSize(const uint8_t* header16);
// `data` holds the whole header (Pfs0HeaderSize bytes). Bounds-checked.
bool Pfs0Parse(const uint8_t* data, size_t size, std::vector<Pfs0Entry>& out, std::string& error);

// Copies [memOffset, memOffset + size) of the NSO's laid-out image into `out`,
// decoding only the segment holding it. The range must lie inside one segment.
bool NsoReadImage(const std::vector<uint8_t>& nso, uint32_t memOffset, size_t size, std::vector<uint8_t>& out,
                  std::string& error);

} // namespace PortRemastered

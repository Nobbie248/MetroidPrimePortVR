#include "port_remastered_movie.h"

#include <cstdlib>
#include <cstring>
#include <ostream>

namespace PortRemastered {
namespace {

constexpr size_t kFormSize = 32;
constexpr uint32_t kThpHeaderSize = 0x30;
// The component table: a count, sixteen types, then the video's width and height.
constexpr uint32_t kThpDataOffset = kThpHeaderSize + 4 + 16 + 8;
constexpr uint32_t kThpFrameHeader = 12;

void PutBE32(uint8_t* out, uint32_t value) {
  out[0] = uint8_t(value >> 24);
  out[1] = uint8_t(value >> 16);
  out[2] = uint8_t(value >> 8);
  out[3] = uint8_t(value);
}

} // namespace

const std::vector<Movie>& Movies() {
  // Matched by eye, frame for frame, against the disc's. Remastered has one
  // take of each transition and four attract movies; the disc's other six
  // attract movies, and the ones Remastered has no counterpart for, stay.
  static const std::vector<Movie> movies = {
      {"45d4dd7e-45af-40f6-b6ed-8b76c33c90fb", {"00_first_start"}},
      {"6278ecfd-6c9c-406d-b845-fce4e92de8fb", {"01_startloop"}},
      {"96261c32-2c15-40dc-ad41-e136217bee56",
       {"02_start_fileselect_A", "02_start_fileselect_B", "02_start_fileselect_C"}},
      {"67c1c7c1-b269-44f4-892f-8840eeb98dc3", {"03_fileselectloop"}},
      {"d6816cf4-fd6f-4127-9e00-ede78af3f464",
       {"04_fileselect_playgame_A", "04_fileselect_playgame_B", "04_fileselect_playgame_C"}},
      {"c9175984-e39a-4e2c-a39f-08d7caff041a", {"06_fileselect_GBA"}},
      {"1a3c8b77-4bb0-4f39-8ae9-23e8846bdca1", {"07_GBAloop"}},
      {"a7aa0bf1-be04-4338-9f44-a405a1296e45", {"08_GBA_fileselect"}},
      {"23c1a512-f5c7-45f1-8ff7-6aa977a41cb1", {"attract0"}},
      {"80d5212a-e9ee-4ce1-b101-089b8dba87fb", {"attract1"}},
      {"94ed99a0-b0d2-4d1e-be9e-964e83e21c9c", {"attract2"}},
      {"dfd58e49-6dcd-4ed0-a4c0-7bc3fee46251", {"attract3"}},
  };
  return movies;
}

bool ParseMovieFormat(const std::string& text, MovieFormat& format) {
  MovieFormat parsed = format;
  const size_t at = text.find('@');
  const std::string size = text.substr(0, at);
  if (!size.empty()) {
    char* end = nullptr;
    const long width = std::strtol(size.c_str(), &end, 10);
    if (end == size.c_str() || (*end != 'x' && *end != 'X')) {
      return false;
    }
    const char* second = end + 1;
    const long height = std::strtol(second, &end, 10);
    // The decoder works in 16x16 blocks of 4:2:0; a ragged edge is padded by
    // the encoder, but an odd size has no chroma layout.
    if (end == second || *end != '\0' || width < 16 || height < 16 || width > 4096 || height > 4096 ||
        width % 2 != 0 || height % 2 != 0) {
      return false;
    }
    parsed.width = int(width);
    parsed.height = int(height);
  }
  if (at != std::string::npos) {
    const std::string rate = text.substr(at + 1);
    char* end = nullptr;
    const long fps = std::strtol(rate.c_str(), &end, 10);
    if (end == rate.c_str() || *end != '\0' || fps < 1 || fps > 60) {
      return false;
    }
    parsed.fps = int(fps);
  } else if (size.empty()) {
    return false;
  }
  format = parsed;
  return true;
}

bool MovieStream(const uint8_t* data, size_t size, size_t& offset, size_t& length) {
  if (size < kFormSize + 8 || std::memcmp(data, "RFRM", 4) != 0 || std::memcmp(data + 20, "FMV0", 4) != 0) {
    return false;
  }
  uint64_t formSize = 0;
  for (int i = 7; i >= 0; --i) {
    formSize = formSize << 8 | data[4 + i];
  }
  if (formSize < 8 || formSize > size - kFormSize) {
    return false;
  }
  offset = kFormSize;
  length = size_t(formSize);
  return true;
}

bool JpegSplitter::Feed(const uint8_t* data, size_t size, const Sink& sink) {
  m_data.insert(m_data.end(), data, data + size);
  for (;;) {
    if (m_scanStart == 0) {
      // Header segments: a marker and a length each, up to the start of scan.
      if (m_pos == 0) {
        if (m_data.size() < 2) {
          return true;
        }
        if (m_data[0] != 0xFF || m_data[1] != 0xD8) {
          return false;
        }
        m_pos = 2;
      }
      if (m_pos + 4 > m_data.size()) {
        return true;
      }
      if (m_data[m_pos] != 0xFF) {
        return false;
      }
      const uint8_t marker = m_data[m_pos + 1];
      if (marker == 0xFF) { // fill
        ++m_pos;
        continue;
      }
      const size_t length = size_t(m_data[m_pos + 2]) << 8 | m_data[m_pos + 3];
      if (length < 2) {
        return false;
      }
      m_pos += 2 + length;
      if (marker == 0xDA) {
        if (m_pos > m_data.size()) {
          // Wait for the whole scan header, so the scan starts inside the buffer.
          m_pos -= 2 + length;
          return true;
        }
        m_scanStart = m_pos;
      }
      continue;
    }
    // The scan: runs to the first marker that is neither stuffing nor a restart.
    const uint8_t* found = m_pos < m_data.size()
                               ? static_cast<const uint8_t*>(std::memchr(m_data.data() + m_pos, 0xFF, m_data.size() - m_pos))
                               : nullptr;
    if (found == nullptr) {
      m_pos = m_data.size();
      return true;
    }
    m_pos = size_t(found - m_data.data());
    if (m_pos + 1 >= m_data.size()) {
      return true;
    }
    const uint8_t marker = m_data[m_pos + 1];
    if (marker == 0x00 || (marker >= 0xD0 && marker <= 0xD7)) {
      m_pos += 2;
      continue;
    }
    if (marker == 0xFF) {
      ++m_pos;
      continue;
    }
    if (marker != 0xD9) {
      return false; // a second scan: not a baseline picture
    }
    std::vector<uint8_t> frame(m_data.begin(), m_data.begin() + m_scanStart);
    frame.reserve(m_pos + 2);
    for (size_t i = m_scanStart; i < m_pos; ++i) {
      frame.push_back(m_data[i]);
      if (m_data[i] == 0xFF && m_data[i + 1] == 0x00) {
        ++i;
      }
    }
    frame.push_back(0xFF);
    frame.push_back(0xD9);
    m_data.erase(m_data.begin(), m_data.begin() + m_pos + 2);
    m_pos = 0;
    m_scanStart = 0;
    if (!sink(frame)) {
      return false;
    }
  }
}

ThpWriter::ThpWriter(std::ostream& out, int width, int height, float fps)
: m_out(out), m_width(width), m_height(height), m_fps(fps) {
  // Room for the header, written by Finish().
  const std::vector<char> blank(kThpDataOffset, 0);
  m_out.write(blank.data(), std::streamsize(blank.size()));
}

void ThpWriter::Add(const std::vector<uint8_t>& jpeg) {
  // A frame is its header and picture, padded to a multiple of 32 bytes.
  const uint32_t size = (kThpFrameHeader + uint32_t(jpeg.size()) + 31) & ~31u;
  if (m_frames == 0) {
    m_firstSize = size;
  } else {
    Flush(size);
  }
  m_heldPrev = m_lastSize;
  m_held.assign(size, 0);
  PutBE32(m_held.data() + 8, uint32_t(jpeg.size()));
  std::memcpy(m_held.data() + kThpFrameHeader, jpeg.data(), jpeg.size());
  m_lastSize = size;
  m_maxSize = size > m_maxSize ? size : m_maxSize;
  m_total += size;
  ++m_frames;
}

void ThpWriter::Flush(uint32_t nextSize) {
  PutBE32(m_held.data(), nextSize);
  PutBE32(m_held.data() + 4, m_heldPrev);
  m_out.write(reinterpret_cast<const char*>(m_held.data()), std::streamsize(m_held.size()));
}

bool ThpWriter::Finish() {
  if (m_frames == 0) {
    return false;
  }
  // The sizes wrap around: a looping movie reads the first frame after the last.
  Flush(m_firstSize);
  uint8_t word[4];
  PutBE32(word, m_lastSize);
  m_out.seekp(kThpDataOffset + 4);
  m_out.write(reinterpret_cast<const char*>(word), 4);

  uint8_t header[kThpDataOffset] = {};
  std::memcpy(header, "THP\0", 4);
  PutBE32(header + 0x04, 0x00010000); // version
  PutBE32(header + 0x08, m_maxSize);
  PutBE32(header + 0x0C, 0); // no audio
  uint32_t fpsBits;
  std::memcpy(&fpsBits, &m_fps, 4);
  PutBE32(header + 0x10, fpsBits);
  PutBE32(header + 0x14, uint32_t(m_frames));
  PutBE32(header + 0x18, m_firstSize);
  PutBE32(header + 0x1C, m_total);
  PutBE32(header + 0x20, kThpHeaderSize); // the component table
  PutBE32(header + 0x24, 0);              // no frame offset table
  PutBE32(header + 0x28, kThpDataOffset);
  PutBE32(header + 0x2C, kThpDataOffset + m_total - m_lastSize);
  PutBE32(header + 0x30, 1); // one component: video
  std::memset(header + 0x35, 0xFF, 15);
  PutBE32(header + 0x44, uint32_t(m_width));
  PutBE32(header + 0x48, uint32_t(m_height));
  m_out.seekp(0);
  m_out.write(reinterpret_cast<const char*>(header), sizeof(header));
  m_out.flush();
  return bool(m_out);
}

} // namespace PortRemastered

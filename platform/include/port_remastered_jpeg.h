#pragma once

// The movie import without ffmpeg (see port_remastered_movie.h): where the
// decoder is the system's own and hands over raw pictures, as Android's does,
// these turn them into the JPEGs a THP holds. No SDL and nothing of Android in
// here, so the desktop builds and tests it.

#include <cstdint>
#include <vector>

#include "port_remastered_movie.h"

namespace PortRemastered {

// A decoded picture: 8 bits, 4:2:0, the chroma either as two planes
// (`chromaStep` 1) or interleaved in one (`chromaStep` 2, `cb` and `cr` a byte
// apart).
struct Picture {
  const uint8_t* y = nullptr;
  const uint8_t* cb = nullptr;
  const uint8_t* cr = nullptr;
  int stride = 0;
  int chromaStride = 0;
  int chromaStep = 1;
  int width = 0;
  int height = 0;
  // How the stream says it is coded. Remastered's movies are BT.709 over the
  // limited range.
  bool bt709 = true;
  bool fullRange = false;
};

// Which pictures of a stream to keep so that it plays at another rate: the
// first one at or after each tick of the new rate.
class FramePacer {
public:
  explicit FramePacer(int fps) : m_fps(fps) {}

  // How many times the picture shown at `timeUs` (microseconds from the first
  // one) is written: 0 to drop it, more than 1 when the stream is the slower.
  int Copies(int64_t timeUs);

private:
  int m_fps;
  int64_t m_next = 0;
};

// Pictures in, JPEGs out: BT.601 over the full range, which is what the game
// converts with, at the size the format asks for, baseline with the standard
// Huffman tables.
class MovieEncoder {
public:
  explicit MovieEncoder(const MovieFormat& format, int quality = 85);

  void Encode(const Picture& picture, std::vector<uint8_t>& jpeg);

private:
  struct Plane {
    std::vector<uint8_t> data;
    int width = 0;
    int height = 0;
  };

  void Convert(const Picture& picture);
  static void Resize(const Plane& from, Plane& to, int width, int height, std::vector<uint8_t>& scratch);

  int m_width;
  int m_height;
  float m_divisors[2][64];
  uint8_t m_quant[2][64];
  Plane m_source[3];
  Plane m_sized[3];
  std::vector<uint8_t> m_scratch;
  std::vector<int> m_chroma[2];
};

} // namespace PortRemastered

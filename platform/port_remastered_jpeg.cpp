// Raw pictures to the JPEGs of a THP (see port_remastered_jpeg.h).

#include "port_remastered_jpeg.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace PortRemastered {
namespace {

// The example tables of the JPEG standard (K.1 to K.6), in the order a file
// stores them.
const uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                             41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                             30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
const uint8_t kBaseQuant[2][64] = {
    {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,  14, 13, 16, 24, 40,  57,
     69, 56, 14, 17, 22,  29,  51,  87,  80, 62, 18, 22, 37,  56,  68,  109, 103, 77, 24, 35, 55,  64,
     81, 104, 113, 92, 49, 64,  78,  87,  103, 121, 120, 101, 72,  92,  95,  98,  112, 100, 103, 99},
    {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
     99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
     99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99}};
const uint8_t kDcCounts[2][16] = {{0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0},
                                  {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0}};
const uint8_t kDcSymbols[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const uint8_t kAcCounts[2][16] = {{0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d},
                                  {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77}};
const uint8_t kAcSymbols[2][162] = {
    {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
     0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
     0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
     0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
     0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
     0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
     0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
     0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
     0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa},
    {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
     0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
     0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
     0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
     0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
     0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
     0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
     0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
     0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa}};

struct Code {
  uint16_t bits = 0;
  uint8_t length = 0;
};

struct Codes {
  Code dc[2][12];
  Code ac[2][256];
};

void Assign(const uint8_t* counts, const uint8_t* symbols, Code* codes) {
  uint16_t code = 0;
  for (int length = 1; length <= 16; ++length) {
    for (int i = 0; i < counts[length - 1]; ++i) {
      codes[*symbols++] = {code++, uint8_t(length)};
    }
    code <<= 1;
  }
}

const Codes& Tables() {
  static const Codes sCodes = [] {
    Codes codes;
    for (int i = 0; i < 2; ++i) {
      Assign(kDcCounts[i], kDcSymbols, codes.dc[i]);
      Assign(kAcCounts[i], kAcSymbols[i], codes.ac[i]);
    }
    return codes;
  }();
  return sCodes;
}

class BitWriter {
public:
  explicit BitWriter(std::vector<uint8_t>& out) : m_out(out) {}

  void Put(uint32_t bits, int count) {
    m_bits = m_bits << count | (bits & ((1u << count) - 1));
    m_count += count;
    while (m_count >= 8) {
      m_count -= 8;
      const uint8_t byte = uint8_t(m_bits >> m_count);
      m_out.push_back(byte);
      if (byte == 0xFF) {
        m_out.push_back(0);
      }
    }
  }

  // Ones up to the next byte.
  void Flush() {
    if (m_count > 0) {
      Put(0x7F, 8 - m_count);
    }
  }

private:
  std::vector<uint8_t>& m_out;
  uint64_t m_bits = 0;
  int m_count = 0;
};

// The floating point DCT of the Independent JPEG Group's library (Arai, Agui
// and Nakajima's): the result is scaled, and the divisors take that back out.
void Transform(float* data) {
  float* p = data;
  for (int pass = 0; pass < 2; ++pass) {
    const int step = pass == 0 ? 1 : 8;
    for (int i = 0; i < 8; ++i) {
      p = data + (pass == 0 ? i * 8 : i);
      const float t0 = p[0] + p[7 * step], t7 = p[0] - p[7 * step];
      const float t1 = p[step] + p[6 * step], t6 = p[step] - p[6 * step];
      const float t2 = p[2 * step] + p[5 * step], t5 = p[2 * step] - p[5 * step];
      const float t3 = p[3 * step] + p[4 * step], t4 = p[3 * step] - p[4 * step];
      float t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
      p[0] = t10 + t11;
      p[4 * step] = t10 - t11;
      const float z1 = (t12 + t13) * 0.707106781f;
      p[2 * step] = t13 + z1;
      p[6 * step] = t13 - z1;
      t10 = t4 + t5;
      t11 = t5 + t6;
      t12 = t6 + t7;
      const float z5 = (t10 - t12) * 0.382683433f;
      const float z2 = 0.541196100f * t10 + z5;
      const float z4 = 1.306562965f * t12 + z5;
      const float z3 = t11 * 0.707106781f;
      const float z11 = t7 + z3, z13 = t7 - z3;
      p[5 * step] = z13 + z2;
      p[3 * step] = z13 - z2;
      p[step] = z11 + z4;
      p[7 * step] = z11 - z4;
    }
  }
}

void Segment(std::vector<uint8_t>& out, uint8_t marker, const std::vector<uint8_t>& body) {
  out.push_back(0xFF);
  out.push_back(marker);
  out.push_back(uint8_t((body.size() + 2) >> 8));
  out.push_back(uint8_t(body.size() + 2));
  out.insert(out.end(), body.begin(), body.end());
}

uint8_t Clamp(int value) { return uint8_t(value < 0 ? 0 : value > 255 ? 255 : value); }

// One block of a plane as the transform takes it. Past the plane's edge the
// last row and column repeat.
void Load(const uint8_t* plane, int width, int height, int x0, int y0, float* block) {
  if (x0 + 8 <= width && y0 + 8 <= height) {
    for (int y = 0; y < 8; ++y) {
      const uint8_t* row = plane + size_t(y0 + y) * width + x0;
      for (int x = 0; x < 8; ++x) {
        block[y * 8 + x] = float(row[x]) - 128.f;
      }
    }
    return;
  }
  for (int y = 0; y < 8; ++y) {
    const uint8_t* row = plane + size_t(std::min(y0 + y, height - 1)) * width;
    for (int x = 0; x < 8; ++x) {
      block[y * 8 + x] = float(row[std::min(x0 + x, width - 1)]) - 128.f;
    }
  }
}

// The bits of a coefficient after its size: the value, or one less than it when
// negative.
int Size(int value, uint32_t& bits) {
  const int magnitude = value < 0 ? -value : value;
  int size = 0;
  while (magnitude >> size != 0) {
    ++size;
  }
  bits = uint32_t(value < 0 ? value - 1 : value);
  return size;
}

void Block(BitWriter& writer, const uint8_t* plane, int width, int height, int x0, int y0, const float* divisors,
           const Code* dc, const Code* ac, int& predicted) {
  float block[64];
  Load(plane, width, height, x0, y0, block);
  Transform(block);
  int coefficients[64];
  int last = 0;
  for (int i = 0; i < 64; ++i) {
    const int value = int(std::lrintf(block[kZigzag[i]] * divisors[kZigzag[i]]));
    coefficients[i] = value;
    if (value != 0) {
      last = i;
    }
  }
  uint32_t bits = 0;
  int size = Size(coefficients[0] - predicted, bits);
  predicted = coefficients[0];
  writer.Put(dc[size].bits, dc[size].length);
  if (size != 0) {
    writer.Put(bits, size);
  }
  int run = 0;
  for (int i = 1; i <= last; ++i) {
    if (coefficients[i] == 0) {
      ++run;
      continue;
    }
    for (; run >= 16; run -= 16) {
      writer.Put(ac[0xF0].bits, ac[0xF0].length);
    }
    size = Size(coefficients[i], bits);
    writer.Put(ac[run << 4 | size].bits, ac[run << 4 | size].length);
    writer.Put(bits, size);
    run = 0;
  }
  if (last != 63) {
    writer.Put(ac[0].bits, ac[0].length);
  }
}

// For each output sample, where its share of the input starts and the weights
// of the samples from there on, out of 4096: the input it covers when shrinking,
// the two it lies between when growing.
struct Taps {
  std::vector<int> start;
  std::vector<int> count;
  std::vector<int> weights;
};

Taps MakeTaps(int from, int to) {
  Taps taps;
  const double scale = double(from) / double(to);
  for (int i = 0; i < to; ++i) {
    std::vector<std::pair<int, double>> parts;
    if (scale >= 1.0) {
      const double begin = i * scale;
      const double end = std::min(double(from), (i + 1) * scale);
      for (int s = int(begin); s < from && double(s) < end; ++s) {
        parts.emplace_back(s, std::min(end, double(s + 1)) - std::max(begin, double(s)));
      }
    } else {
      const double centre = std::max(0.0, (i + 0.5) * scale - 0.5);
      const int first = std::min(int(centre), from - 1);
      const double fraction = centre - first;
      parts.emplace_back(first, 1.0 - fraction);
      if (first + 1 < from && fraction > 0.0) {
        parts.emplace_back(first + 1, fraction);
      }
    }
    double total = 0.0;
    for (const auto& part : parts) {
      total += part.second;
    }
    taps.start.push_back(parts.front().first);
    taps.count.push_back(int(parts.size()));
    int sum = 0;
    const size_t at = taps.weights.size();
    size_t largest = at;
    for (const auto& part : parts) {
      taps.weights.push_back(int(std::lround(part.second / total * 4096.0)));
      sum += taps.weights.back();
      if (taps.weights.back() > taps.weights[largest]) {
        largest = taps.weights.size() - 1;
      }
    }
    // Rounding must not change the brightness.
    taps.weights[largest] += 4096 - sum;
  }
  return taps;
}

} // namespace

int FramePacer::Copies(int64_t timeUs) {
  // A millisecond of give: the stream's times are rounded.
  int copies = 0;
  while (m_next * 1000000 <= (timeUs + 1000) * m_fps) {
    ++copies;
    ++m_next;
  }
  return copies;
}

MovieEncoder::MovieEncoder(const MovieFormat& format, int quality) : m_width(format.width), m_height(format.height) {
  static const double kScale[8] = {1.0,         1.387039845, 1.306562965, 1.175875602,
                                   1.0,         0.785694958, 0.541196100, 0.275899379};
  const int percent = quality < 50 ? 5000 / std::max(quality, 1) : 200 - 2 * std::min(quality, 100);
  for (int table = 0; table < 2; ++table) {
    for (int i = 0; i < 64; ++i) {
      const int value = std::clamp((kBaseQuant[table][i] * percent + 50) / 100, 1, 255);
      m_quant[table][i] = uint8_t(value);
      m_divisors[table][i] = float(1.0 / (value * kScale[i / 8] * kScale[i % 8] * 8.0));
    }
  }
}

void MovieEncoder::Convert(const Picture& picture) {
  // The stream's values to red, green and blue, and those to BT.601 over the
  // full range, as one matrix in 16.16 fixed point.
  const double kr = picture.bt709 ? 0.2126 : 0.299;
  const double kb = picture.bt709 ? 0.0722 : 0.114;
  const double kg = 1.0 - kr - kb;
  const double lumaScale = picture.fullRange ? 1.0 : 255.0 / 219.0;
  const double chromaScale = picture.fullRange ? 1.0 : 255.0 / 224.0;
  const int lumaBase = picture.fullRange ? 0 : 16;
  // Per unit of the stream's blue and red difference: what each colour gains.
  const double redFromCr = 2.0 * (1.0 - kr);
  const double blueFromCb = 2.0 * (1.0 - kb);
  const double greenFromCb = -kb * blueFromCb / kg;
  const double greenFromCr = -kr * redFromCr / kg;
  const double lumaFromCb = 0.587 * greenFromCb + 0.114 * blueFromCb;
  const double lumaFromCr = 0.299 * redFromCr + 0.587 * greenFromCr;
  const auto fixed = [](double value) { return int(std::lround(value * 65536.0)); };
  const int yy = fixed(lumaScale);
  const int yCb = fixed(lumaFromCb * chromaScale);
  const int yCr = fixed(lumaFromCr * chromaScale);
  const int cbCb = fixed((blueFromCb - lumaFromCb) / 1.772 * chromaScale);
  const int cbCr = fixed(-lumaFromCr / 1.772 * chromaScale);
  const int crCb = fixed(-lumaFromCb / 1.402 * chromaScale);
  const int crCr = fixed((redFromCr - lumaFromCr) / 1.402 * chromaScale);

  const int width = picture.width;
  const int height = picture.height;
  const int chromaWidth = (width + 1) / 2;
  const int chromaHeight = (height + 1) / 2;
  m_source[0].width = width;
  m_source[0].height = height;
  m_source[0].data.resize(size_t(width) * height);
  for (int i = 1; i < 3; ++i) {
    m_source[i].width = chromaWidth;
    m_source[i].height = chromaHeight;
    m_source[i].data.resize(size_t(chromaWidth) * chromaHeight);
  }
  // The luma takes a share of the chroma, which has a sample for every four of
  // it: each luma sample uses the chroma at its own place, between the samples
  // (they sit on the even columns, and halfway between two rows), in sixteenths.
  m_chroma[0].resize(size_t(chromaWidth) + 1);
  m_chroma[1].resize(size_t(chromaWidth) + 1);
  for (int y = 0; y < height; ++y) {
    const int here = y / 2;
    const int away = std::clamp(y % 2 == 0 ? here - 1 : here + 1, 0, chromaHeight - 1);
    for (int i = 0; i < 2; ++i) {
      const uint8_t* plane = i == 0 ? picture.cb : picture.cr;
      const uint8_t* a = plane + size_t(here) * picture.chromaStride;
      const uint8_t* b = plane + size_t(away) * picture.chromaStride;
      int* row = m_chroma[i].data();
      for (int x = 0; x < chromaWidth; ++x) {
        row[x] = 3 * a[x * picture.chromaStep] + b[x * picture.chromaStep] - 4 * 128;
      }
      row[chromaWidth] = row[chromaWidth - 1];
    }
    const uint8_t* luma = picture.y + size_t(y) * picture.stride;
    const int* cb = m_chroma[0].data();
    const int* cr = m_chroma[1].data();
    uint8_t* out = m_source[0].data.data() + size_t(y) * width;
    for (int x = 0; x < width; ++x) {
      const int at = x / 2;
      const int b = x % 2 == 0 ? 2 * cb[at] : cb[at] + cb[at + 1];
      const int r = x % 2 == 0 ? 2 * cr[at] : cr[at] + cr[at + 1];
      // 16.16 with eighths of a chroma step in b and r.
      out[x] = Clamp(int((int64_t(yy) * (luma[x] - lumaBase) * 8 + int64_t(yCb) * b + int64_t(yCr) * r + (1 << 18)) >> 19));
    }
  }
  for (int y = 0; y < chromaHeight; ++y) {
    const uint8_t* cb = picture.cb + size_t(y) * picture.chromaStride;
    const uint8_t* cr = picture.cr + size_t(y) * picture.chromaStride;
    uint8_t* outCb = m_source[1].data.data() + size_t(y) * chromaWidth;
    uint8_t* outCr = m_source[2].data.data() + size_t(y) * chromaWidth;
    for (int x = 0; x < chromaWidth; ++x) {
      const int b = cb[x * picture.chromaStep] - 128;
      const int r = cr[x * picture.chromaStep] - 128;
      outCb[x] = Clamp(128 + ((cbCb * b + cbCr * r + 32768) >> 16));
      outCr[x] = Clamp(128 + ((crCb * b + crCr * r + 32768) >> 16));
    }
  }
}

void MovieEncoder::Resize(const Plane& from, Plane& to, int width, int height, std::vector<uint8_t>& scratch) {
  to.width = width;
  to.height = height;
  to.data.resize(size_t(width) * height);
  const Taps across = MakeTaps(from.width, width);
  const Taps down = MakeTaps(from.height, height);
  scratch.resize(size_t(width) * from.height);
  for (int y = 0; y < from.height; ++y) {
    const uint8_t* row = from.data.data() + size_t(y) * from.width;
    uint8_t* out = scratch.data() + size_t(y) * width;
    const int* weight = across.weights.data();
    for (int x = 0; x < width; ++x) {
      int sum = 2048;
      for (int i = 0; i < across.count[x]; ++i) {
        sum += row[across.start[x] + i] * *weight++;
      }
      out[x] = Clamp(sum >> 12);
    }
  }
  const int* weight = down.weights.data();
  std::vector<int> sums(width);
  for (int y = 0; y < height; ++y) {
    std::fill(sums.begin(), sums.end(), 2048);
    for (int i = 0; i < down.count[y]; ++i) {
      const uint8_t* row = scratch.data() + size_t(down.start[y] + i) * width;
      const int w = *weight++;
      for (int x = 0; x < width; ++x) {
        sums[x] += row[x] * w;
      }
    }
    uint8_t* out = to.data.data() + size_t(y) * width;
    for (int x = 0; x < width; ++x) {
      out[x] = Clamp(sums[x] >> 12);
    }
  }
}

void MovieEncoder::Encode(const Picture& picture, std::vector<uint8_t>& jpeg) {
  Convert(picture);
  const Plane* planes = m_source;
  if (picture.width != m_width || picture.height != m_height) {
    Resize(m_source[0], m_sized[0], m_width, m_height, m_scratch);
    Resize(m_source[1], m_sized[1], (m_width + 1) / 2, (m_height + 1) / 2, m_scratch);
    Resize(m_source[2], m_sized[2], (m_width + 1) / 2, (m_height + 1) / 2, m_scratch);
    planes = m_sized;
  }

  jpeg.clear();
  jpeg.push_back(0xFF);
  jpeg.push_back(0xD8);
  std::vector<uint8_t> body;
  for (int table = 0; table < 2; ++table) {
    body.push_back(uint8_t(table));
    for (int i = 0; i < 64; ++i) {
      body.push_back(m_quant[table][kZigzag[i]]);
    }
  }
  Segment(jpeg, 0xDB, body);
  body = {8,    uint8_t(m_height >> 8), uint8_t(m_height), uint8_t(m_width >> 8), uint8_t(m_width), 3, 1, 0x22, 0, 2,
          0x11, 1,                      3,                 0x11,                  1};
  Segment(jpeg, 0xC0, body);
  body.clear();
  for (int table = 0; table < 2; ++table) {
    body.push_back(uint8_t(table));
    body.insert(body.end(), kDcCounts[table], kDcCounts[table] + 16);
    body.insert(body.end(), kDcSymbols, kDcSymbols + 12);
    body.push_back(uint8_t(0x10 | table));
    body.insert(body.end(), kAcCounts[table], kAcCounts[table] + 16);
    body.insert(body.end(), kAcSymbols[table], kAcSymbols[table] + 162);
  }
  Segment(jpeg, 0xC4, body);
  body = {3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0};
  Segment(jpeg, 0xDA, body);

  const Codes& codes = Tables();
  BitWriter writer(jpeg);
  int predicted[3] = {0, 0, 0};
  for (int y = 0; y < m_height; y += 16) {
    for (int x = 0; x < m_width; x += 16) {
      for (int i = 0; i < 4; ++i) {
        Block(writer, planes[0].data.data(), planes[0].width, planes[0].height, x + (i & 1) * 8, y + (i >> 1) * 8,
              m_divisors[0], codes.dc[0], codes.ac[0], predicted[0]);
      }
      for (int i = 1; i < 3; ++i) {
        Block(writer, planes[i].data.data(), planes[i].width, planes[i].height, x / 2, y / 2, m_divisors[1],
              codes.dc[1], codes.ac[1], predicted[i]);
      }
    }
  }
  writer.Flush();
  jpeg.push_back(0xFF);
  jpeg.push_back(0xD9);
}

} // namespace PortRemastered

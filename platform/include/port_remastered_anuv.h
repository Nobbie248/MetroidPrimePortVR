#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace PortRemastered {

// Remastered animates a model's UVs with a small program in the ANUV sub-chunk of its
// HEAD (layout and evaluator: build/mpr/anuv/FORMAT.md and anuv.py). Run from the
// seconds clock, every number it feeds a texture transform is either a constant or a
// looping curve, so the program is flattened here to those curves and the game
// evaluates the curves directly; nothing of the program itself ships.

// One argument of TextureTransform2x4: a constant, or a looping curve of
// `samples.size() - 1` steps read `samplesPerSecond` per second.
struct AnuvArg {
  enum class Kind : uint32_t { Const = 0, Linear = 1, Nearest = 2 };
  Kind kind = Kind::Const;
  float value = 0.0f;
  float samplesPerSecond = 0.0f;
  float period = 0.0f;
  std::vector<float> samples;
};

// A texture transform as its five arguments: U and V translation, U and V scale and
// the rotation in radians. The default is the identity.
struct AnuvTransform {
  AnuvArg arg[5];
  AnuvTransform();
  bool Identity() const;
};

enum class AnuvSkip {
  None,
  NotScalarArg,     // a transform argument that isn't a plain value or curve
  NotTransform,     // a transform id of a kind other than the three known
  ContextNotRoot,   // a curve whose clock isn't driven by the root time
  ContextConflict,  // one clock reached with two different rates
  ShortCurve,       // fewer samples than the period needs
  DoubleWrite,      // two curves feed the same word
  BadIndex,         // an id or word past the end of its table
};

// One CVector4i entry: three transforms, one per texture coordinate index.
struct AnuvEntry {
  AnuvSkip skip = AnuvSkip::None;
  AnuvTransform xf[3];
};

struct Anuv {
  std::vector<AnuvEntry> entries;
  std::vector<uint8_t> matmap;  // per mesh index: the entry it uses, 255 = none
};

// `data` is the bytes of the ANUV sub-chunk after its tag, up to the end of the HEAD
// chunk. False, with the reason in `error`, when it doesn't parse.
bool ParseAnuv(const uint8_t* data, size_t size, Anuv& out, std::string& error);

// The 2x4 matrix (row-major, U row then V row) of a transform at `seconds` on the
// root clock: what the game computes from the serialised words.
void EvalAnuvTransform(const AnuvTransform& xf, double seconds, float out[8]);

// The transform as the words HandleAnimatedUV's port-only type reads (host order;
// the converter writes them big-endian).
inline constexpr uint32_t kAnuvAnimType = 0x50;
std::vector<uint32_t> AnuvAnimWords(const AnuvTransform& xf);

const char* AnuvSkipName(AnuvSkip skip);

}  // namespace PortRemastered

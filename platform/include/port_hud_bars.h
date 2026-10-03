#pragma once

// A mod's own shape for a HUD frame's bars (energy, threat, missiles, boss).
//
// The game draws a bar as a strip between pairs of points it computes from a
// curve built into the code, one curve per visor, with the bar's texture
// stretched along it. A frame that lays its bars out differently needs other
// curves, so a mod can supply them as data: <FRME id>.hudbars lists, for each
// bar widget it names, the strip's stations in order from empty to full. The
// fill, drain and colour rules stay the game's.
//
// The file, little endian:
//   char[4] "HBAR", u32 version (1), u32 barCount
//   barCount x {
//     u32 nameLength, nameLength bytes   the widget's name in the frame
//     u32 stationCount                   at least 2
//     stationCount x { f32 a[3], b[3], uvA[2], uvB[2] }
//   }
// A station is the strip's two edges at one place along the bar, in the
// widget's own space, each with its texture coordinate.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace PortHudBars {

struct Station {
  float a[3] = {};
  float b[3] = {};
  float uvA[2] = {};
  float uvB[2] = {};
};

struct Bar {
  std::string name;
  std::vector<Station> stations;
  // How far along the strip's middle each station is, 0 at the first and 1 at the last (Measure).
  std::vector<float> at;
};

using Bars = std::vector<Bar>;

// "1A2B3C4D.hudbars" (any case) -> the frame's id.
bool ParseFileName(const std::string& fileName, uint32_t& id);
// False for a file that is cut short, or has a bar with fewer than two stations or no length.
bool ParseFile(const uint8_t* data, size_t size, Bars& out);
void WriteFile(const Bars& bars, std::vector<uint8_t>& out);

// Fills `at`; false when the strip has no length. ParseFile does it for the bars it reads.
bool Measure(Bar& bar);
// The bar fills by length, so a strip with long end pieces and a finely cut middle fills evenly.
// The strip at `t` (0 empty end, 1 full end), between the stations either side.
Station Sample(const Bar& bar, float t);
// The stations strictly between `from` and `to` are [first, last); none when first >= last.
void Inside(const Bar& bar, float from, float to, size_t& first, size_t& last);

// The bars a mod supplies for a frame, read once; null when there are none.
std::shared_ptr<const Bars> ForFrame(uint32_t frame);
// The mods changed: read the files again on the next ForFrame.
void Reset();

} // namespace PortHudBars

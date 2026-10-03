#pragma once

#include <cstddef>
#include <cstdint>

// Pickup dots on the map, like randomprime's: a plain white dot where each of
// the 100 item pickups sits, until it is collected. Deliberately one colour
// for every item, so the map doesn't spoil a randomized game; the colours an
// Archipelago game gives them say where to go, not what is there. CMapWorld
// draws them (grep PortMapPickups).
namespace PortMapPickups {

struct Dot {
  uint32_t world; // MLVL
  uint32_t area;  // MREA
  uint32_t relay; // memory relay the pickup activates when collected
  float pos[3];   // world space
};

// The setting, or forced on in randomizer and Archipelago games.
bool Active();
// True when a randomized game forces it on regardless of the setting.
bool Forced();

// Every pickup, from tools/gen_map_pickups.py.
const Dot* Dots(size_t& count);

// In an Archipelago game the dots take a tracker's colours instead (the
// logic_colors setting, on by default), and collected ones stay as grey dots.
enum EColor {
  kC_White,  // no logic to go by
  kC_Green,  // in logic
  kC_Yellow, // reachable out of logic (a sequence break)
  kC_Blue,   // can be seen, not collected
  kC_Red,    // out of reach
  kC_Grey,   // checked
};
// One EColor per dot, in Dots order. False when the dots are plain white:
// call once a frame, not per dot.
bool Colors(const unsigned char*& colors);

} // namespace PortMapPickups

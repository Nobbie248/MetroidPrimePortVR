#include "port_map_pickups.h"

#include <cmath>

namespace PortMapPickups {
namespace {

const Dot kDots[] = {
#include "port_map_pickups.inc"
};

} // namespace

const Dot* Dots(size_t& count) {
  count = sizeof(kDots) / sizeof(kDots[0]);
  return kDots;
}

int DotIndex(uint32_t world, uint32_t area, const float pos[3]) {
  int best = -1;
  float bestDist = kMatchRadius * kMatchRadius;
  for (size_t i = 0; i < sizeof(kDots) / sizeof(kDots[0]); ++i) {
    const Dot& dot = kDots[i];
    if (dot.world != world || dot.area != area) {
      continue;
    }
    const float dx = dot.pos[0] - pos[0];
    const float dy = dot.pos[1] - pos[1];
    const float dz = dot.pos[2] - pos[2];
    const float dist = dx * dx + dy * dy + dz * dz;
    if (dist <= bestDist) {
      bestDist = dist;
      best = static_cast< int >(i);
    }
  }
  return best;
}

EKind KindOfItem(int item, int capacity) {
  switch (item) {
  case 4:  // Missiles: the disc's launcher is one too
  case 24: // EnergyTanks
    return kK_Minor;
  case 7: // PowerBombs: the main one carries 4
    return capacity < 4 ? kK_Minor : kK_Major;
  default:
    return kK_Major;
  }
}

bool IsSeen(const uint32_t* words, size_t dot) {
  return dot < kSeenWords * 32 && (words[dot / 32] >> (dot % 32) & 1u) != 0;
}

void SetSeen(uint32_t* words, size_t dot) {
  if (dot < kSeenWords * 32) {
    words[dot / 32] |= 1u << (dot % 32);
  }
}

} // namespace PortMapPickups

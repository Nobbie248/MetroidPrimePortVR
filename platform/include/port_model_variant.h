#pragma once

// A second look of one retail model or character. Retail draws every suit's
// ball from one model and picks a material set; a Remastered import replaces
// that model with one set of textures, so it writes the other suits' balls as
// separate resources under the id this gives, and the game loads that id when
// a mod has it. Shared by the importer and the game, so both agree on the id.

#include <cstdint>

namespace PortModelVariant {

// `key` is the suit (CPlayerState::EPlayerSuit). The id of a variant that is
// also a retail resource's is never written, so it cannot shadow one.
inline uint32_t Id(uint32_t retail, int key) {
  uint32_t h = 2166136261u;
  const auto mix = [&h](uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      h = (h ^ ((v >> (8 * i)) & 0xFF)) * 16777619u;
    }
  };
  mix(0x56415249u); // "VARI"
  mix(retail);
  mix(uint32_t(key));
  return h;
}

} // namespace PortModelVariant

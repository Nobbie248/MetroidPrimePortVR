#pragma once

#include <cstdint>

// Remastered's per-visor mesh filter. A CMDL mesh carries a flag word (unkC | unkE << 16);
// InitMeshFilter (0xf9e764) excludes the meshes whose bit for the active visor is set (table
// 0x1d2d7f4: combat and scan bit 16, thermal bit 17, X-ray bit 18), and the shadow passes
// (0x3bbbd0, 0x3b7314, 0x3b820c) also exclude bit 5. The converter keeps those four bits per
// surface as a tagged word at surface header offset 0x5c ('VM' in the top half, a bit per
// filter in the bottom); 0 means a surface with none of them.
namespace PortVisorMesh {

constexpr uint32_t kTag = 0x564d0000;
constexpr uint32_t kShadow = 1;  // flag bit 5: not cast into shadows
constexpr uint32_t kVisor16 = 2; // flag bit 16: hidden in the combat and scan visors
constexpr uint32_t kVisor17 = 4; // flag bit 17: hidden in the thermal visor
constexpr uint32_t kVisor18 = 8; // flag bit 18: hidden in the X-ray visor

// The surface word for a mesh flag word (0 when no filter bit is set).
inline uint32_t FromFlags(uint32_t flags) {
  const uint32_t bits = ((flags >> 5) & 1 ? kShadow : 0) | ((flags >> 16) & 1 ? kVisor16 : 0) |
                        ((flags >> 17) & 1 ? kVisor17 : 0) | ((flags >> 18) & 1 ? kVisor18 : 0);
  return bits != 0 ? kTag | bits : 0;
}

// The filter bits of a surface word (0 for an untagged word).
inline uint32_t Bits(uint32_t word) { return (word & 0xffff0000u) == kTag ? (word & 0xf) : 0; }

// The exclusion bit of a visor in CPlayerState::EPlayerVisor order (combat, X-ray, scan, thermal).
inline uint32_t VisorBit(int visor) {
  return visor == 1 ? kVisor18 : visor == 3 ? kVisor17 : kVisor16;
}

// A surface is skipped when its bit for the active visor is set, and, with the shadow caster
// mode 2 (casts only), when it has the shadow bit.
inline bool SkipsVisor(uint32_t word, int visor) { return (Bits(word) & VisorBit(visor)) != 0; }
inline bool SkipsShadow(uint32_t word) { return (Bits(word) & kShadow) != 0; }

} // namespace PortVisorMesh

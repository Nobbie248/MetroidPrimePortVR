#pragma once

// A Remastered material's AUVI parameter, and what it does to the texcoords of
// the material's maps.
//
// A map's texcoord names one of the vertex shader's three UV output channels, and
// AUVI says which of the model's texcoord sets each channel is fed from: a map
// whose texcoord is c is drawn with the model's texcoord set AUVI[c].
//
// Bit 6 of the feature word (MFAV) says the material has more than one animated
// UV channel, which is what an AUVI goes with: the flag admits the parameter, it
// does not promise one. A material with no AUVI keeps the texcoord its own map
// was authored on; that is the port's fallback, not a rule read out of
// Remastered.
//
// Only the first three values are read, one per output channel. A coord past them
// has no channel to go through and is left as it is; a negative value is a
// selector the port does not recognise, so the map keeps the texcoord it had.

#include <cstddef>
#include <cstdint>

#include "port_remastered_cmdl.h"

namespace PortRemastered {

// Bit 6 of a material's feature word (MFAV).
inline constexpr uint32_t kAuviFlag = 0x40;

// The usage tag an AUVI parameter carries, packed as the file stores it.
inline constexpr uint32_t kAuviUsage = 0x41555649;  // 'AUVI'

// What to remap a material's map texcoords with, or null when there is none: no
// flag, or no AUVI parameter that is an Int4. The last Int4 AUVI is the one that
// counts, as for the parameters naming a material's maps, and it is applied once:
// two of them are a later saying again, not a chain to walk the coords through.
inline const int32_t* Auvi(const ModelMaterial& mat) {
  if ((mat.unk1 & kAuviFlag) == 0) {
    return nullptr;
  }
  const int32_t* found = nullptr;
  for (const ModelMaterialData& d : mat.data) {
    if (d.usage == kAuviUsage && d.kind == ModelMaterialData::Kind::Int4) {
      found = d.int4;
    }
  }
  return found;
}

// What one map's texcoord becomes, given the AUVI: the set the channel named by
// `coord` is fed from, or `coord` itself where there is no such channel or the
// selector is not one the port recognises.
inline uint32_t AuviCoord(uint32_t coord, const int32_t* auvi) {
  constexpr uint32_t kOutputs = 3;  // the vertex shader's three UV output channels
  if (auvi == nullptr || coord >= kOutputs) {
    return coord;
  }
  const int32_t mapped = auvi[coord];
  return mapped < 0 ? coord : uint32_t(mapped);
}

// The texcoord every one of `coords` becomes, in place. A material has at most
// seven: base, MR, normal and emissive, then the same three of a second layer.
inline void ApplyAuvi(const ModelMaterial& mat, uint32_t* coords, size_t count) {
  const int32_t* auvi = Auvi(mat);
  if (auvi == nullptr) {
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    coords[i] = AuviCoord(coords[i], auvi);
  }
}

} // namespace PortRemastered

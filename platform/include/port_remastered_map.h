#pragma once

// Remastered's map screen art, as resources the original game loads.
//
// The frame around the map is one of the HUD frames (port_remastered_hud.h).
// This is what is drawn inside it:
//
//   icons   the save, missile and elevator markers replace the disc's pictures
//           under the disc's ids; the six lift arrows, which Remastered has as
//           one coloured picture each where the disc tints two white ones, go
//           under the ids of port_map_icons.h.
//   rooms   a world's CMAP holds every room's map shape. Most are the disc's own
//           MAPA; the few Remastered reshaped are written as MAPA under the
//           disc's ids, with the disc's doors and markers.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "port_remastered_image.h"

namespace PortRemastered {

// A Remastered picture by its asset name, and the TXTR id it is written under.
struct MapIcon {
  const char* name;
  uint32_t id;
};
const std::vector<MapIcon>& MapIcons();

// The TXTR for an icon: the disc's size, with mips, since the game draws them
// both larger and far smaller than they are. Empty when `image` is not one.
std::vector<uint8_t> EncodeMapIcon(const Image& image);

// Remastered's map of a world by its asset name, and the retail MLVL it is.
struct MapWorld {
  const char* name;  // "CMAP_IceLevel"
  uint32_t mlvl;
};
const std::vector<MapWorld>& MapWorlds();

struct MapIO {
  // A retail resource by FourCC type ('MLVL', 'MAPW', 'MAPA') and id, from the
  // unmodded disc, decompressed.
  std::function<bool(uint32_t type, uint32_t id, std::vector<uint8_t>& out)> retail;
  // Stores one output file, "<MAPA id as 8 upper case hex>.MAPA".
  std::function<bool(const std::string& name, const std::vector<uint8_t>& data)> write;
  std::function<void(const std::string& line)> log;  // optional
};

// Writes a MAPA for each room of the world whose shape in `cmap` is not the
// disc's. A CMAP names no retail room, so its areas are paired with the disc's
// by where they lie and how large they are; a room that cannot be paired or
// has more corners than a MAPA can index is logged and keeps the disc's map.
// `written` counts files stored; false (with `error`) when the world as a whole
// cannot be read.
bool WriteWorldMapAreas(uint32_t mlvl, const uint8_t* cmap, size_t size, const MapIO& io, int& written,
                        std::string& error);

}  // namespace PortRemastered

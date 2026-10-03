#pragma once

// The map screen's lift arrows as pictures of a mod's own, and its compass.
//
// The disc has two arrow pictures, white, and the game tints them yellow, green
// or red by the object's type. A mod with one picture per type, already
// coloured, puts them under these ids, which no disc resource has; the game
// then draws that picture untinted (CMappableObject::Draw). The Remastered
// import writes them (port_remastered_map.h).

namespace PortMapIcons {

constexpr int kFirstArrowType = 27;  // kMOT_DownArrowYellow
constexpr int kLastArrowType = 32;   // kMOT_UpArrowRed
constexpr unsigned int kArrowBase = 0x4D415030;  // "MAP0"

// The TXTR id for an arrow type (kMOT_DownArrowYellow .. kMOT_UpArrowRed).
constexpr unsigned int ArrowId(int type) { return kArrowBase + static_cast< unsigned int >(type - kFirstArrowType); }

// The map screen's compass, which only Remastered has: a disc tilted with the
// map and a needle turned with it too, pointing the map's north. CMDLs in the
// disc's axes; the game draws them when both are there (CAutoMapper::Draw).
constexpr unsigned int kCompassShell = 0x4D415040;   // "MAP@"
constexpr unsigned int kCompassNeedle = 0x4D415041;  // "MAPA"

}  // namespace PortMapIcons

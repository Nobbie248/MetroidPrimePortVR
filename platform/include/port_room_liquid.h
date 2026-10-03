#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class CGameArea;
class CStateManager;
class CVector3f;

// A room's liquid surfaces (water, poison, lava) as models. A mod supplies them per area
// as <MREA id>.roomliquid, with the models as ordinary CMDL files whose materials are the
// port's liquid ones (PBR kinds 5 and 6); the port draws one in place of the fluid plane
// of the area's water object that stands where it does. The object itself is untouched:
// its fog, splashes, ripples' sounds and damage are the game's.
//
// The file is little endian:
//   'MPRL', u32 version (1), u32 surfaces
//   surface: u32 type (0 water, 1 poison, 2 lava), u32 CMDL id,
//            f32 transform[12] (rows of model -> area)
// The transform's translation is where the water object is; the surface is drawn at the
// height the game gives its own.
namespace PortRoomLiquid {

struct Surface {
  uint32_t type;
  uint32_t model;
  float transform[12];
};

// "1A2B3C4D.roomliquid" (any case) -> 0x1A2B3C4D.
bool ParseFileName(const std::string& fileName, uint32_t& id);
bool Parse(const std::vector<uint8_t>& data, std::vector<Surface>& out, std::string& error);

// The areas in memory now; frees the surfaces of areas that left.
void SetLoadedAreas(const uint32_t* mreas, size_t count);
// Draws the surface of the area's water object `uid`, which is at `position` and holds
// `fluidType` (CFluidPlane::EFluidType). False when there is none or its model has not
// loaded yet, and the object draws its own fluid plane.
bool Draw(const CStateManager& mgr, const CGameArea& area, uint32_t uid, const CVector3f& position, int fluidType,
          float surfaceZ);
// Lets go of every model (the mods folder is about to change).
void Reset();

// MP_ROOM_LIQUID=0|1, the console's `roomliquid`.
void SetEnabled(bool enabled);
bool Enabled();
// Areas with a file, their surfaces, and those drawn in the last frame.
void Stats(int& areas, int& surfaces, int& drawn);

} // namespace PortRoomLiquid

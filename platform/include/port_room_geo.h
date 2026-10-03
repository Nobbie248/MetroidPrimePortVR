#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class CFrustumPlanes;
class CGameArea;
class CStateManager;
class CVector3f;

// A room's static geometry as a list of models and where each stands. A mod supplies one
// per area as <MREA id>.roomgeo, with the models as ordinary CMDL files; the port draws
// them in place of the area's own world geometry.
//
// The file is little endian:
//   'MPRG', u32 version (1 or 2), u32 instances
//   instance: u32 CMDL id, f32 transform[12] (rows of model -> area)
//     version 2 adds: u8 layer, u8 active, u16 links, then per link
//     u32 sender, u8 state, u8 action, u16 0
// An instance whose CMDL does not exist is skipped.
//
// Version 2 is for scenery Remastered added as actors, which its scripts show and hide:
// such an instance is drawn only while the area's script layer `layer` is on (kEveryLayer:
// always), starts shown or hidden by `active`, and changes when the retail object with
// editor id `sender` sends `state` (an EScriptObjectState).
namespace PortRoomGeo {

enum : uint8_t { kEveryLayer = 0xff };
enum LinkAction : uint8_t { kShow = 1, kHide = 2, kToggle = 3 };

struct Link {
  uint32_t sender; // retail editor id, layer bits included
  uint8_t state;
  uint8_t action; // LinkAction
};

struct Instance {
  uint32_t model = 0;
  float transform[12] = {};
  uint8_t layer = kEveryLayer;
  bool active = true;
  std::vector<Link> links;
};

// --- The file (port_room_geo_file.cpp; no game or GX dependencies) -------------

// "1A2B3C4D.roomgeo" (any case) -> 0x1A2B3C4D.
bool ParseFileName(const std::string& fileName, uint32_t& id);
bool Parse(const std::vector<uint8_t>& data, std::vector<Instance>& out, std::string& error);
// The file for these instances (version 2).
std::vector<uint8_t> Write(const std::vector<Instance>& instances);

// --- The game side (port_room_geo.cpp) -----------------------------------------

// The areas in memory now. Loads the files of new ones and frees those of areas that left.
void SetLoadedAreas(const uint32_t* mreas, size_t count);
// Draws the area's room geometry. True when it stands in for the area's own, which is
// when the area has a file, the mode says so and every model of it has loaded.
bool Draw(const CStateManager& mgr, const CGameArea& area, const CFrustumPlanes& frustum);
// Queues the blended surfaces of what Draw drew for the area this frame in the renderer's
// sorted pass; the renderer hands each back to DrawSorted through the state manager's
// drawable callback, as type kDrawableType.
enum { kDrawableType = 3 };
void AddSorted(const CGameArea& area);
void DrawSorted(const void* drawable);
// Set around the renderer's sorted pass for an area Draw stood in for, so that pass
// draws the actors and leaves the area's own surfaces out.
extern bool sReplacingArea;
// Lets go of every model (the mods folder is about to change).
void Reset();
// A script object sent a state (CEntity::SendScriptMsgs): shows or hides the instances
// linked to it. Loads the area's file if the area is newer than the last SetLoadedAreas,
// since objects send states while their area is still being set up.
void OnScriptState(CStateManager& mgr, uint32_t editorId, int state);
// A new game, a death or a save state builds a new CStateManager: every instance goes back
// to how the file starts it, since the scripts will not resend what already happened.
void ResetScriptState();

// Whether the frame's buffers were sized for room geometry at startup (main.cpp). Until
// they are, nothing is drawn: a room of it overflows the default ones.
void SetBuffersReady(bool ready);

enum class Mode {
  Off,
  Replace, // in place of the area's world geometry
  Overlay, // on top of it
};
// MP_ROOM_GEO=0|1|overlay, the console's `roomgeo`.
void SetMode(Mode mode);
Mode GetMode();
// Whether room geometry takes the area's lights where the room has baked light too
// (MP_ROOM_GEO_AREA_LIGHTS, the console's `roomgeo lights`).
void SetAreaLights(bool on);
bool AreaLights();
// Areas with a file, their instances, the distinct models and how many have loaded, and
// the instances drawn in the last frame.
void Stats(int& areas, int& instances, int& models, int& loaded, int& drawn);


// For finding which model a surface belongs to (the console's `roomgeo at|hide|show`).
// One line per drawn instance whose box holds the point, give or take the margin.
std::string At(const CVector3f& point, float margin);
// Stops or resumes drawing a model, or every model for id 0; how many it matched.
int SetHidden(uint32_t id, bool hidden);
// What a ray meets (the console's `roomgeo pick`): one line per instance whose box it
// passes through, nearest first, then the boxes the origin is already inside, smallest
// first. Boxes, not triangles, so the surface looked at is among the first few. Returns
// the first one's model, 0 for none.
uint32_t Pick(const CVector3f& origin, const CVector3f& direction, std::string& out);
// One line per material of a loaded model: flags, whether it is drawn through PBR, and
// its record (see CCubeModel::PortSetPBRMaterial). Empty when no loaded model has the id.
std::string Materials(uint32_t id);
// Draws a model's material with one value of its record replaced (index 0 to 18), until
// cleared; kept across room loads. False when no loaded model has that material.
bool SetMaterialValue(uint32_t id, int material, int field, float value);
int ClearMaterialValues();

} // namespace PortRoomGeo

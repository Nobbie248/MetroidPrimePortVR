#pragma once

// Remastered's HUD frames, as frames the original game loads.
//
// A Remastered frame (GUIF) lists the same widgets as the disc's FRME, by the
// same names, laid out for a 16:9 screen and drawn with meshes of one model the
// frame names. The game's code finds its widgets by name and by worker id, so
// the disc's frame stays the skeleton: every widget of it is kept, and the ones
// Remastered also has take its transform, colour, draw mode and mesh. Widgets
// only Remastered has (trim, housings) are added under their parents; models
// only the disc has are emptied, their art having no place in the new layout.
//
// One frame becomes:
//   <FRME id>.FRME      the frame, in the disc's format, under the disc's id
//   <id>.CMDL           one model per mesh widget, unlit, one texture a surface
//   <id>.dds + .TXTR    each picture once: the native texture and its stub
//   <FRME id>.hudbars   the bars' strips (port_hud_bars.h)
//
// As with the model converter, bytes come and go through ConvertIO, and the
// output is the player's own, made from their copies of both games.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "port_remastered_cmdl.h"
#include "port_remastered_convert.h"

namespace PortRemastered {

// A Remastered frame by its asset name, and the disc frame it stands in for.
struct HudFrame {
  const char* name;
  uint32_t retail;
};
const std::vector<HudFrame>& HudFrames();

// The model a frame draws with, in a pak's byte order. False when `guif` is
// not a frame.
bool HudFrameModel(const uint8_t* guif, size_t size, ModelUuid& model);

struct HudCounts {
  int widgets = 0;
  int models = 0;
  int textures = 0;
  int bars = 0;
};

// Converts frames one after another, sharing the pictures they have in common.
class HudConverter {
public:
  explicit HudConverter(ConvertIO io);

  // `retailFrame` is the disc frame's id; it is read through ConvertIO::retail.
  // `counts` is added to.
  bool Convert(uint32_t retailFrame, const uint8_t* guif, size_t size, const Model& model, HudCounts& counts,
               std::string& error);

  // A whole model (its finest level of detail), drawn the way the frames' are,
  // under the given id: for models the game places itself, like the map's compass.
  bool ConvertModel(const Model& model, uint32_t id, HudCounts& counts, std::string& error);

private:
  uint32_t NewId(uint32_t& next);
  bool LoadMaterial(std::string& error);
  // The port id of a Remastered picture (pak byte order), written once; nothing
  // for one that would not open.
  std::optional<uint32_t> Texture(const ModelUuid& id, HudCounts& counts, const std::string& owner);

  ConvertIO m_io;
  std::map<ModelUuid, uint32_t> m_textures;  // by pak-order id; 0 for one that would not open
  std::vector<uint8_t> m_material;           // the disc material the models are drawn with
  uint32_t m_nextModel;
  uint32_t m_nextTexture;
};

}  // namespace PortRemastered

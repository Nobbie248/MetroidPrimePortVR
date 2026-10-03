#pragma once

// Converts one Metroid Prime Remastered model into the files a port mod holds:
// a GameCube CMDL (and CSKR, for a skinned retail model) plus its textures.
//
// The Remastered model replaces a retail one, so the retail model decides
// everything the game's own code depends on: its material sets, vertex
// descriptors, skin bones and resource id. The Remastered side supplies the
// geometry and the maps. A material becomes one of two things: a PBR material
// (base, metal/roughness, normal and emissive maps, flagged for the port's PBR
// shading, with the port's material record appended) or, where that cannot be
// done (a blended effect, no base map), the retail material with its texture
// slots refilled.
//
// Nothing here knows where bytes come from or go to. Retail resources, decoded
// Remastered textures and the output files all pass through ConvertIO, so the
// same code serves the in-game importer and the test tool.
//
// The output is derived from the player's own copy of both games and is for
// their use only; nothing produced here may ship.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "port_remastered_cmdl.h"
#include "port_remastered_image.h"

namespace PortRemastered {

struct ConvertOptions {
  uint32_t retail = 0;  // CMDL id the model replaces
  // The model replaces nothing (a piece of room geometry): `retail` is only the
  // id it is written under, and every surface gets one opaque lit PBR material.
  // Surfaces with no base map are dropped, having no retail material to keep.
  bool standalone = false;
  int nativeMax = 0;  // largest edge of a native .dds, 0 for the converter's own
  // gc = orient * remastered + offset. The default is Remastered's y-up frame
  // onto the GameCube's z-up one.
  double orient[3][3] = {{-1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 1.0, 0.0}};
  double offset[3] = {0.0, 0.0, 0.0};
  // Material name substrings to drop: the low-detail copies a model carries.
  std::vector<std::string> skip = {"simple"};
  int material = -1;      // force every surface onto this retail material
  bool pbr = true;        // false keeps every material on the retail TEV path
  int maxTexture = 2048;  // largest edge of a TEV path texture
  // Remaps U of one role's coordinates (TEV path): from [u0, u1] onto [lo, hi].
  bool squeeze = false;
  std::string squeezeRole;
  double squeezeFrom[2] = {0.0, 1.0};
  double squeezeTo[2] = {0.0, 1.0};
  // Every CSKR id a character binds this model to; empty for a static model.
  // The weights are written under each one whose skeleton covers the first's.
  std::vector<uint32_t> skins;
  // Ids the output is written under instead, for a second look of one retail
  // model (another suit's ball, another beam's gun): the CMDL's (0 for
  // `retail`), and one per id in `skins`, in that order (empty for the same).
  // `retail` and `skins` are still read for the materials and the skeleton.
  uint32_t outputModel = 0;
  std::vector<uint32_t> outputSkins;
  // The model is a liquid's surface (water, poison): the room, not the model, says how it
  // looks, so every material is the port's liquid one (standalone only). The normal map's
  // id is in a pak's byte order; the tint's alpha is the opacity seen straight on; the
  // flow is the texcoords a second each of the two wave layers moves by.
  bool water = false;
  bool waterHasNormal = false;
  ModelUuid waterNormal{};
  double waterTint[4] = {0.1, 0.3, 0.35, 0.7};
  double waterScale[2] = {0.08, 0.08};  // texcoords per unit of the model, along x and z
  double waterFlow[4] = {0.02, 0.01, -0.012, 0.018};
  double waterNormalStrength = 0.5;
  double waterFresnel = 0.02;
  // A texture's id is hashed from a tag naming its source. These wrap the
  // Remastered texture's uuid in that tag; the test tool sets them to
  // reproduce the reference converter's ids, the game leaves them empty.
  std::string texturePrefix;
  std::string textureSuffix;
};

struct ConvertIO {
  // A retail resource by type FourCC ('CMDL', 'CSKR', 'TXTR') and id, from the
  // unmodded disc. False when there is none.
  std::function<bool(uint32_t type, uint32_t id, std::vector<uint8_t>& out)> retail;
  // Whether any retail resource has this id, so a new texture never takes one.
  std::function<bool(uint32_t id)> retailId;
  // The top mip of a Remastered texture as RGBA8. The id is in a pak's byte
  // order (what IdToString prints), not the order a model stores it in.
  std::function<bool(const ModelUuid& id, Image& out, std::string& error)> texture;
  // Optional, for converters that run side by side into one folder: asked once
  // for each PBR map a converter is about to write, by the id it will have;
  // false when another converter has taken it, and this one then only names it.
  std::function<bool(uint32_t id)> claim;
  // Stores one output file: "<ID>.CMDL", "<ID>.CSKR", "<ID>.TXTR", "<ID>.dds".
  std::function<bool(const std::string& name, const std::vector<uint8_t>& data)> write;
  std::function<void(const std::string& line)> log;  // optional
};

// Holds what is shared between the models of one import: the textures already
// written, so a map used by several models is converted once.
class Converter {
public:
  explicit Converter(ConvertIO io);
  ~Converter();
  Converter(const Converter&) = delete;
  Converter& operator=(const Converter&) = delete;

  bool Convert(const Model& model, const ConvertOptions& options, std::string& error);

  // How many output materials took the PBR path and how many kept the TEV one.
  int PbrMaterials() const;
  int TevMaterials() const;

private:
  struct State;
  State* m_state;
};

} // namespace PortRemastered

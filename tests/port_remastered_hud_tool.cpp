// Runs the HUD frames through PortRemastered::HudConverter, for comparing
// against the prototype's output. Everything it reads is the developer's own
// extracted data, so it is built by hand and is not a ctest.
//
//   port_remastered_hud_tool <disc dir> <remastered dir> <out dir>
//
// <disc dir> holds the disc's <ID>.FRME and <ID>.CMDL, <remastered dir> the
// extracted FRME_<Name>.GUIF, <uuid>.CMDL and <uuid>.TXTR.

#include <cstdio>
#include <fstream>
#include <utility>

#include "port_map_icons.h"
#include "port_remastered_hud.h"
#include "port_remastered_pak.h"
#include "port_remastered_txtr.h"

using namespace PortRemastered;

static bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <disc dir> <remastered dir> <out dir>\n", argv[0]);
    return 2;
  }
  const std::string disc = argv[1], remDir = argv[2], outDir = argv[3];
  ConvertIO io;
  io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
    char name[32];
    std::snprintf(name, sizeof(name), "/%08X.%c%c%c%c", id, char(type >> 24), char(type >> 16), char(type >> 8),
                  char(type));
    return ReadFile(disc + name, out);
  };
  io.retailId = [](uint32_t) { return false; };
  io.texture = [&](const ModelUuid& id, Image& out, std::string& error) {
    std::vector<uint8_t> raw;
    TxtrImage image;
    if (!ReadFile(remDir + "/" + IdToString(id) + ".TXTR", raw)) {
      error = "no such file";
      return false;
    }
    if (!DecodeTxtr(raw.data(), raw.size(), image, error)) {
      return false;
    }
    out.width = int(image.width);
    out.height = int(image.height);
    out.rgba = std::move(image.rgba);
    return true;
  };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    std::ofstream f(outDir + "/" + name, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return bool(f);
  };
  io.log = [](const std::string& line) { std::printf("  %s\n", line.c_str()); };
  HudConverter converter(io);
  int failed = 0;
  for (const HudFrame& frame : HudFrames()) {
    std::vector<uint8_t> guif, raw;
    ModelUuid id{};
    Model model;
    HudCounts counts;
    std::string error;
    if (!ReadFile(remDir + "/" + frame.name + ".GUIF", guif) || !HudFrameModel(guif.data(), guif.size(), id)) {
      error = "no frame";
    } else if (!ReadFile(remDir + "/" + IdToString(id) + ".CMDL", raw)) {
      error = "no model " + IdToString(id);
    } else if (ParseModel(raw.data(), raw.size(), model, error) &&
               converter.Convert(frame.retail, guif.data(), guif.size(), model, counts, error)) {
      std::printf("%s: %d widgets, %d models, %d textures, %d bars\n", frame.name, counts.widgets, counts.models,
                  counts.textures, counts.bars);
      continue;
    }
    std::printf("%s: %s\n", frame.name, error.c_str());
    ++failed;
  }
  // The map screen's compass, which the import finds by name.
  for (const auto& [name, id] : {std::pair<const char*, uint32_t>{"CMDL_MapCompassShell", PortMapIcons::kCompassShell},
                                 std::pair<const char*, uint32_t>{"CMDL_MapCompass", PortMapIcons::kCompassNeedle}}) {
    std::vector<uint8_t> raw;
    Model model;
    HudCounts counts;
    std::string error;
    if (!ReadFile(remDir + "/" + name + ".CMDL", raw)) {
      error = "no model";
    } else if (ParseModel(raw.data(), raw.size(), model, error) &&
               converter.ConvertModel(model, id, counts, error)) {
      std::printf("%s: %d textures\n", name, counts.textures);
      continue;
    }
    std::printf("%s: %s\n", name, error.c_str());
    ++failed;
  }
  return failed == 0 ? 0 : 1;
}

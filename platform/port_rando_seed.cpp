#include "port_strings.h"
#include "port_rando_gen.h"

#include "port_json.h"
#include "port_paths.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

// Settings text and seed files for the built-in randomizer. The generator
// itself is in port_rando_gen.cpp; a seed is saved so that it can be played
// again, and its name is all the player needs to keep.
namespace PortRandoGen {
namespace {

constexpr int kSeedFileVersion = 1;

// PortJson only parses, and slot_data is kept as JSON text, so a loaded value
// is written back out here. Whole numbers stay integers (item and location ids).
void Write(const PortJson::Value& value, std::string& out) {
  switch (value.GetType()) {
  case PortJson::Value::Type::Null: out += "null"; break;
  case PortJson::Value::Type::Bool: out += value.AsBool() ? "true" : "false"; break;
  case PortJson::Value::Type::Number: {
    const double number = value.AsNumber();
    char text[40];
    if (std::fabs(number) < 9e15 && number == std::floor(number))
      std::snprintf(text, sizeof(text), "%lld", static_cast< long long >(number));
    else
      std::snprintf(text, sizeof(text), "%.17g", number);
    out += text;
    break;
  }
  case PortJson::Value::Type::String: out += port::JsonQuote(value.AsString()); break;
  case PortJson::Value::Type::Array: {
    out.push_back('[');
    bool first = true;
    for (const PortJson::Value& element : value.AsArray()) {
      if (!first)
        out.push_back(',');
      first = false;
      Write(element, out);
    }
    out.push_back(']');
    break;
  }
  case PortJson::Value::Type::Object: {
    out.push_back('{');
    bool first = true;
    for (const auto& [key, member] : value.AsObject()) {
      if (!first)
        out.push_back(',');
      first = false;
      out += port::JsonQuote(key);
      out.push_back(':');
      Write(member, out);
    }
    out.push_back('}');
    break;
  }
  }
}

// A number or a bool (the apworld writes toggles as 0/1), clamped.
int IntMember(const PortJson::Value& root, const char* name, int fallback, int low, int high) {
  const PortJson::Value* member = root.Find(name);
  if (member == nullptr)
    return fallback;
  int64_t value = fallback;
  if (member->IsBool())
    value = member->AsBool() ? 1 : 0;
  else if (member->IsNumber())
    value = member->AsInt(fallback);
  return static_cast< int >(std::clamp< int64_t >(value, low, high));
}

bool BoolMember(const PortJson::Value& root, const char* name, bool fallback) {
  return IntMember(root, name, fallback ? 1 : 0, 0, 1) != 0;
}

void NamesMember(const PortJson::Value& root, const char* name, std::vector< std::string >& out) {
  out.clear();
  const PortJson::Value* member = root.Find(name);
  if (member == nullptr || !member->IsArray())
    return;
  for (const PortJson::Value& entry : member->AsArray()) {
    if (entry.IsString())
      out.push_back(entry.AsString());
  }
}

void NamesText(const std::vector< std::string >& names, std::string& out) {
  out.push_back('[');
  for (size_t i = 0; i < names.size(); ++i) {
    if (i != 0)
      out.push_back(',');
    out += port::JsonQuote(names[i]);
  }
  out.push_back(']');
}

// The name is typed by the player and ends up in a file name.
std::string SafeName(const std::string& name) {
  std::string safe;
  for (const char c : name) {
    const bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       c == '-' || c == '_' || c == '.';
    safe.push_back(plain ? c : '_');
  }
  // No hidden files, and "." / ".." must not name a directory.
  const size_t start = safe.find_first_not_of('.');
  safe = start == std::string::npos ? std::string() : safe.substr(start);
  if (safe.size() > 64)
    safe.resize(64);
  // <name>.state.json is the solo server's state of seed <name>.
  if (port::EndsWith(safe, ".state"))
    safe.back() = '_';
  return safe.empty() ? "seed" : safe;
}

bool ReadFile(const std::filesystem::path& path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;
  out.assign(std::istreambuf_iterator< char >(file), std::istreambuf_iterator< char >());
  return !file.bad();
}

} // namespace

bool Settings::operator==(const Settings& other) const {
  return requiredArtifacts == other.requiredArtifacts && finalBosses == other.finalBosses &&
         artifactHints == other.artifactHints && missileLauncher == other.missileLauncher &&
         mainPowerBomb == other.mainPowerBomb && shuffleScanVisor == other.shuffleScanVisor &&
         preScanElevators == other.preScanElevators &&
         elevatorRandomization == other.elevatorRandomization &&
         doorColorRandomization == other.doorColorRandomization &&
         progressiveBeams == other.progressiveBeams &&
         nonVariaHeatDamage == other.nonVariaHeatDamage &&
         staggeredSuitDamage == other.staggeredSuitDamage && combatLogic == other.combatLogic &&
         trickDifficulty == other.trickDifficulty && trickAllow == other.trickAllow &&
         trickDeny == other.trickDeny && flaahgraPowerBombs == other.flaahgraPowerBombs &&
         backwardsLowerMines == other.backwardsLowerMines && removeXray == other.removeXray &&
         removeThermal == other.removeThermal && removeHiveMecha == other.removeHiveMecha &&
         springBall == other.springBall && startingRoom == other.startingRoom &&
         randomizeStartingBeam == other.randomizeStartingBeam &&
         blastShieldRandomization == other.blastShieldRandomization &&
         blastShieldFrequency == other.blastShieldFrequency &&
         blastShieldAvailableTypes == other.blastShieldAvailableTypes &&
         lockedDoorCount == other.lockedDoorCount &&
         includePowerBeamDoors == other.includePowerBeamDoors &&
         includeMorphBallBombDoors == other.includeMorphBallBombDoors;
}

std::string SettingsText(const Settings& s) {
  const auto flag = [](bool value) { return value ? "1" : "0"; };
  std::string out = "{";
  out += "\"required_artifacts\":" + std::to_string(s.requiredArtifacts);
  out += ",\"final_bosses\":" + std::to_string(s.finalBosses);
  out += std::string(",\"artifact_hints\":") + flag(s.artifactHints);
  out += std::string(",\"missile_launcher\":") + flag(s.missileLauncher);
  out += std::string(",\"main_power_bomb\":") + flag(s.mainPowerBomb);
  out += std::string(",\"shuffle_scan_visor\":") + flag(s.shuffleScanVisor);
  out += std::string(",\"pre_scan_elevators\":") + flag(s.preScanElevators);
  out += std::string(",\"elevator_randomization\":") + flag(s.elevatorRandomization);
  out += ",\"door_color_randomization\":" + std::to_string(s.doorColorRandomization);
  out += std::string(",\"progressive_beam_upgrades\":") + flag(s.progressiveBeams);
  out += std::string(",\"non_varia_heat_damage\":") + flag(s.nonVariaHeatDamage);
  out += ",\"staggered_suit_damage\":" + std::to_string(s.staggeredSuitDamage);
  out += ",\"combat_logic_difficulty\":" + std::to_string(s.combatLogic);
  out += ",\"trick_difficulty\":" + std::to_string(s.trickDifficulty);
  out += ",\"trick_allow_list\":";
  NamesText(s.trickAllow, out);
  out += ",\"trick_deny_list\":";
  NamesText(s.trickDeny, out);
  out += std::string(",\"flaahgra_power_bombs\":") + flag(s.flaahgraPowerBombs);
  out += std::string(",\"backwards_lower_mines\":") + flag(s.backwardsLowerMines);
  out += ",\"remove_xray_requirements\":" + std::to_string(s.removeXray);
  out += ",\"remove_thermal_requirements\":" + std::to_string(s.removeThermal);
  out += std::string(",\"remove_hive_mecha\":") + flag(s.removeHiveMecha);
  out += std::string(",\"spring_ball\":") + flag(s.springBall);
  out += ",\"starting_room\":" + std::to_string(s.startingRoom);
  out += std::string(",\"randomize_starting_beam\":") + flag(s.randomizeStartingBeam);
  out += ",\"blast_shield_randomization\":" + std::to_string(s.blastShieldRandomization);
  out += ",\"blast_shield_frequency\":" + std::to_string(s.blastShieldFrequency);
  out += ",\"blast_shield_available_types\":" + std::to_string(s.blastShieldAvailableTypes);
  out += ",\"locked_door_count\":" + std::to_string(s.lockedDoorCount);
  out += std::string(",\"include_power_beam_doors\":") + flag(s.includePowerBeamDoors);
  out += std::string(",\"include_morph_ball_bomb_doors\":") + flag(s.includeMorphBallBombDoors);
  out.push_back('}');
  return out;
}

namespace {

void ReadSettings(const PortJson::Value& root, Settings& out) {
  Settings s;
  s.requiredArtifacts = IntMember(root, "required_artifacts", s.requiredArtifacts, 1, 12);
  s.finalBosses = IntMember(root, "final_bosses", s.finalBosses, 0, 3);
  s.artifactHints = BoolMember(root, "artifact_hints", s.artifactHints);
  s.missileLauncher = BoolMember(root, "missile_launcher", s.missileLauncher);
  s.mainPowerBomb = BoolMember(root, "main_power_bomb", s.mainPowerBomb);
  s.shuffleScanVisor = BoolMember(root, "shuffle_scan_visor", s.shuffleScanVisor);
  s.preScanElevators = BoolMember(root, "pre_scan_elevators", s.preScanElevators);
  s.elevatorRandomization = BoolMember(root, "elevator_randomization", s.elevatorRandomization);
  s.doorColorRandomization =
      IntMember(root, "door_color_randomization", s.doorColorRandomization, 0, 2);
  s.progressiveBeams = BoolMember(root, "progressive_beam_upgrades", s.progressiveBeams);
  s.nonVariaHeatDamage = BoolMember(root, "non_varia_heat_damage", s.nonVariaHeatDamage);
  s.staggeredSuitDamage = IntMember(root, "staggered_suit_damage", s.staggeredSuitDamage, 0, 2);
  s.combatLogic = IntMember(root, "combat_logic_difficulty", s.combatLogic, -1, 1);
  s.trickDifficulty = IntMember(root, "trick_difficulty", s.trickDifficulty, -1, 2);
  NamesMember(root, "trick_allow_list", s.trickAllow);
  NamesMember(root, "trick_deny_list", s.trickDeny);
  s.flaahgraPowerBombs = BoolMember(root, "flaahgra_power_bombs", s.flaahgraPowerBombs);
  s.backwardsLowerMines = BoolMember(root, "backwards_lower_mines", s.backwardsLowerMines);
  s.removeXray = IntMember(root, "remove_xray_requirements", s.removeXray, 0, 2);
  s.removeThermal = IntMember(root, "remove_thermal_requirements", s.removeThermal, 0, 2);
  s.removeHiveMecha = BoolMember(root, "remove_hive_mecha", s.removeHiveMecha);
  s.springBall = BoolMember(root, "spring_ball", s.springBall);
  s.startingRoom = IntMember(root, "starting_room", s.startingRoom, 0, 2);
  s.randomizeStartingBeam = BoolMember(root, "randomize_starting_beam", s.randomizeStartingBeam);
  s.blastShieldRandomization =
      IntMember(root, "blast_shield_randomization", s.blastShieldRandomization, 0, 2);
  s.blastShieldFrequency = IntMember(root, "blast_shield_frequency", s.blastShieldFrequency, 1, 6);
  s.blastShieldAvailableTypes =
      IntMember(root, "blast_shield_available_types", s.blastShieldAvailableTypes, 0, 1);
  s.lockedDoorCount = IntMember(root, "locked_door_count", s.lockedDoorCount, 0, 2);
  s.includePowerBeamDoors = BoolMember(root, "include_power_beam_doors", s.includePowerBeamDoors);
  s.includeMorphBallBombDoors =
      BoolMember(root, "include_morph_ball_bomb_doors", s.includeMorphBallBombDoors);
  out = std::move(s);
}

} // namespace

bool ParseSettings(const std::string& text, Settings& out) {
  PortJson::Value root;
  size_t offset = 0;
  const char* reason = nullptr;
  if (!PortJson::Parse(text, root, offset, &reason) || !root.IsObject())
    return false;
  ReadSettings(root, out);
  return true;
}

std::string SeedDirectory() {
  const std::string& user = PortPaths::UserFolder();
  return (std::filesystem::path(user.empty() ? std::string(".") : user) / "randomizer_seeds").string();
}

std::string SeedPath(const std::string& name) {
  return (std::filesystem::path(SeedDirectory()) / (SafeName(name) + ".json")).string();
}

bool Save(Seed& seed, std::string& error) {
  try {
    if (seed.name.empty()) {
      error = "the seed has no name";
      return false;
    }
    // Another seed (other options, or a name that comes out as the same file
    // name) may own this file already, and with it checked locations and a save
    // card that aren't this seed's, so this one is saved as <name>-2, -3, ...
    // The base is cut so the suffix survives SafeName's length limit.
    const std::string base = seed.name.substr(0, 56);
    for (int n = 2; std::filesystem::exists(SeedPath(seed.name)); ++n) {
      if (n > 999) {
        error = "too many seeds named " + base;
        return false;
      }
      Seed existing;
      std::string ignored;
      if (Load(SeedPath(seed.name), existing, ignored) && existing.settings == seed.settings &&
          existing.startItems == seed.startItems && existing.placements == seed.placements)
        break;
      seed.name = base + "-" + std::to_string(n);
    }
    std::string slotData = seed.slotData.empty() ? std::string("{}") : seed.slotData;
    // Checked here so a seed file is never written that Load would refuse.
    PortJson::Value slot;
    size_t offset = 0;
    const char* reason = nullptr;
    if (!PortJson::Parse(slotData, slot, offset, &reason) || !slot.IsObject()) {
      error = "the seed's slot data is not a JSON object";
      return false;
    }
    std::string out = "{\"version\":" + std::to_string(kSeedFileVersion);
    out += ",\"name\":" + port::JsonQuote(seed.name);
    out += ",\"settings\":" + SettingsText(seed.settings);
    out += ",\"slot_data\":";
    Write(slot, out); // normalised, so the file has no stray whitespace
    out += ",\"start_items\":[";
    for (size_t i = 0; i < seed.startItems.size(); ++i) {
      if (i != 0)
        out.push_back(',');
      out += std::to_string(seed.startItems[i]);
    }
    out += "],\"placements\":{";
    bool first = true;
    for (const auto& [location, item] : seed.placements) {
      if (!first)
        out.push_back(',');
      first = false;
      out += "\"" + std::to_string(location) + "\":" + std::to_string(item);
    }
    out += "},\"spoiler\":" + port::JsonQuote(seed.spoiler) + "}\n";

    const std::filesystem::path path = SeedPath(seed.name);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // A temporary beside it, renamed over it: a crash mid-write leaves the old
    // seed, and a seed that is being played is never half a file.
    const std::filesystem::path temp = path.string() + ".tmp";
    {
      std::ofstream file(temp, std::ios::binary | std::ios::trunc);
      file.write(out.data(), static_cast< std::streamsize >(out.size()));
      file.flush();
      if (!file) {
        error = "could not write " + temp.string();
        std::filesystem::remove(temp, ec);
        return false;
      }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
      error = "could not write " + path.string() + ": " + ec.message();
      std::filesystem::remove(temp, ec);
      return false;
    }
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

bool Load(const std::string& path, Seed& out, std::string& error) {
  try {
    std::string text;
    if (!ReadFile(path, text)) {
      error = "could not read " + path;
      return false;
    }
    PortJson::Value root;
    size_t offset = 0;
    const char* reason = nullptr;
    if (!PortJson::Parse(text, root, offset, &reason) || !root.IsObject()) {
      error = path + ": not a seed file" +
              (reason != nullptr ? std::string(" (") + reason + ")" : std::string());
      return false;
    }
    const PortJson::Value* version = root.Find("version");
    if (version == nullptr || version->AsInt(0) != kSeedFileVersion) {
      error = path + ": unsupported seed file version";
      return false;
    }
    const PortJson::Value* slot = root.Find("slot_data");
    const PortJson::Value* placements = root.Find("placements");
    if (slot == nullptr || !slot->IsObject() || placements == nullptr || !placements->IsObject()) {
      error = path + ": the seed file is missing its slot data or placements";
      return false;
    }
    Seed seed;
    seed.name = root.StringOr("name");
    if (seed.name.empty())
      seed.name = std::filesystem::path(path).stem().string();
    if (const PortJson::Value* settings = root.Find("settings"); settings != nullptr && settings->IsObject())
      ReadSettings(*settings, seed.settings);
    Write(*slot, seed.slotData);
    if (const PortJson::Value* items = root.Find("start_items"); items != nullptr && items->IsArray()) {
      for (const PortJson::Value& item : items->AsArray()) {
        if (item.IsNumber())
          seed.startItems.push_back(item.AsInt());
      }
    }
    for (const auto& [key, item] : placements->AsObject()) {
      char* end = nullptr;
      const long long location = std::strtoll(key.c_str(), &end, 10);
      if (key.empty() || end == nullptr || *end != '\0' || !item.IsNumber()) {
        error = path + ": bad placement \"" + key + "\"";
        return false;
      }
      seed.placements[location] = item.AsInt();
    }
    seed.spoiler = root.StringOr("spoiler");
    out = std::move(seed);
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

} // namespace PortRandoGen

// SPDX-License-Identifier: GPL-3.0-or-later

#include "primedgun_import.h"

#include "launcher_keys.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace PrimedGunLauncher {
namespace {

std::string_view Trim(std::string_view text) {
  const size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string Utf8(const fs::path& path) {
  const std::u8string u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
}

std::string FileName(const fs::path& path) { return Lower(Utf8(path.filename())); }

// PrimedGun.ini's settings whose port key is not "vr_" + the same name.
struct Rename {
  std::string_view from;
  std::string_view to;
};
constexpr Rename kRenames[] = {
    {"primedgun_grip_inputs_enabled", "vr_grip_inputs_enabled"},
    {"primedgun_grip_inputs_use_trackpad", "vr_grip_inputs_use_trackpad"},
    {"primedgun_trackpad_press_threshold", "vr_trackpad_press_threshold"},
    {"primedgun_index_grip_press_threshold", "vr_index_grip_press_threshold"},
    {"vr_menu_hold_left_stick", "vr_vr_menu_hold_left_stick"},
    {"vr_menu_requires_head_zone", "vr_vr_menu_requires_head_zone"},
    {"vr_menu_floating", "vr_vr_menu_floating"},
    {"vr_overlays_enabled", "vr_vr_overlays_enabled"},
};

// Dolphin's IniFile writes True/False, QSettings true/false.
std::string ImportedBool(std::string_view value) {
  const std::string lower = Lower(value);
  return FormatBool(lower == "true" || lower == "1" || lower == "yes" || lower == "on");
}

// PrimedGun's search: a memory card file worth considering, and how likely it
// is to be the one the old install played on.
bool LooksLikeMemoryCard(const fs::path& file) {
  const std::string name = FileName(file);
  if (name.find(".backup-") != std::string::npos) {
    return false;
  }
  const std::string ext = Lower(Utf8(file.extension()));
  return ext == ".raw" || ext == ".gcp";
}

int MemoryCardScore(const fs::path& file) {
  const std::string name = FileName(file);
  int score = 0;
  if (name == "online.usa.raw") {
    score += 100;
  }
  if (name == "memorycarda.usa.raw") {
    score += 90;
  }
  if (name.find(".usa.") != std::string::npos) {
    score += 50;
  }
  if (name.find("memorycarda") != std::string::npos || name.find("online") != std::string::npos) {
    score += 25;
  }
  if (Lower(Utf8(file.extension())) == ".raw") {
    score += 10;
  }
  return score;
}

// A Dolphin GCI folder holding Metroid Prime saves ranks below a raw image,
// which is what PrimedGun installs used.
constexpr int kGciFolderScore = 40;

bool HasGameGci(const fs::path& folder) {
  std::error_code ec;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = FileName(it->path());
    if (name.find("gm8e") != std::string::npos && name.size() > 4 &&
        name.compare(name.size() - 4, 4, ".gci") == 0) {
      return true;
    }
  }
  return false;
}

OldCard BestCardInDir(const fs::path& dir) {
  OldCard best;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    return best;
  }
  std::vector<std::pair<fs::file_time_type, fs::path>> files;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code fileEc;
    if (it->is_regular_file(fileEc) && LooksLikeMemoryCard(it->path())) {
      files.emplace_back(it->last_write_time(fileEc), it->path());
    }
  }
  // Newest first, so the most recently played card wins a tie.
  std::sort(files.begin(), files.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  for (const auto& [time, file] : files) {
    const int score = MemoryCardScore(file);
    if (score > best.score) {
      best = {file, false, score};
    }
  }
  // Dolphin's own card folders sit in User/GC; the port keeps its card in
  // <exe>/USA/Card A, which must not be mistaken for an old one.
  if (FileName(dir) == "gc") {
    const fs::path folder = dir / "USA" / "Card A";
    if (kGciFolderScore > best.score && HasGameGci(folder)) {
      best = {folder, true, kGciFolderScore};
    }
  }
  return best;
}

void AddUnique(std::vector<fs::path>& dirs, const fs::path& dir) {
  const fs::path clean = dir.lexically_normal();
  for (const fs::path& existing : dirs) {
    if (Lower(Utf8(existing)) == Lower(Utf8(clean))) {
      return;
    }
  }
  dirs.push_back(clean);
}

void AddSearchBase(std::vector<fs::path>& dirs, const fs::path& base) {
  AddUnique(dirs, base);
  AddUnique(dirs, base / "GC");
  AddUnique(dirs, base / "User" / "GC");
  AddUnique(dirs, base / "x64" / "User" / "GC");
  AddUnique(dirs, base / "Binary" / "User" / "GC");
  AddUnique(dirs, base / "Binary" / "x64" / "User" / "GC");
  AddUnique(dirs, base / "build" / "bin" / "User" / "GC");
  AddUnique(dirs, base / "build" / "bin" / "x64" / "User" / "GC");
  AddUnique(dirs, base / "core" / "User" / "GC");
}

std::vector<fs::path> ChildFolders(const fs::path& dir) {
  std::vector<std::pair<fs::file_time_type, fs::path>> children;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code childEc;
    if (it->is_directory(childEc)) {
      children.emplace_back(it->last_write_time(childEc), it->path());
    }
  }
  std::sort(children.begin(), children.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<fs::path> out;
  for (auto& [time, path] : children) {
    out.push_back(std::move(path));
  }
  return out;
}

bool SamePath(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  if (fs::equivalent(a, b, ec)) {
    return true;
  }
  return Lower(Utf8(a.lexically_normal())) == Lower(Utf8(b.lexically_normal()));
}

OldCard BestCardIn(const std::vector<fs::path>& dirs) {
  OldCard best;
  for (const fs::path& dir : dirs) {
    const OldCard candidate = BestCardInDir(dir);
    if (candidate.Found() && candidate.score > best.score) {
      best = candidate;
    }
  }
  return best;
}

} // namespace

IniSections ParseIni(std::string_view text) {
  IniSections sections;
  std::map<std::string, std::string>* current = &sections[""];
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    const std::string_view line = Trim(text.substr(start, end - start));
    start = end + 1;
    if (line.empty() || line[0] == ';' || line[0] == '#') {
      continue;
    }
    if (line.front() == '[' && line.back() == ']') {
      current = &sections[Lower(Trim(line.substr(1, line.size() - 2)))];
      continue;
    }
    const size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
      continue;
    }
    const std::string_view key = Trim(line.substr(0, equals));
    std::string_view value = Trim(line.substr(equals + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    (*current)[std::string(key)] = std::string(value);
  }
  return sections;
}

IniSections ReadIni(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return ParseIni(text);
}

std::vector<std::pair<std::string, std::string>>
MapRuntimeSettings(const std::map<std::string, std::string>& section) {
  std::vector<std::pair<std::string, std::string>> values;
  for (const auto& [name, value] : section) {
    if (name == "rumble_hand_mode") {
      const std::string mode = std::string(Trim(value));
      values.emplace_back("vr_rumble_hand", mode == "0" ? "both" : mode == "1" ? "left" : "right");
      continue;
    }
    std::string key = "vr_" + name;
    for (const Rename& rename : kRenames) {
      if (rename.from == name) {
        key = std::string(rename.to);
        break;
      }
    }
    // Only the keys the launcher owns: `enabled`, visor_helmet_skip_hidden_draw
    // and vr_state_slot have nothing to go to.
    const KeyInfo* info = name == "enabled" ? nullptr : FindKey(key);
    if (info == nullptr) {
      continue;
    }
    values.emplace_back(key, info->kind == KeyKind::Bool ? ImportedBool(value)
                                                         : Canonical(*info, Trim(value)));
  }
  return values;
}

OldSettings ReadOldSettings(const fs::path& userFolder) {
  OldSettings result;
  const fs::path config = userFolder / "Config";
  std::error_code ec;

  const fs::path runtimeIni = config / "PrimedGun.ini";
  const fs::path qtIni = config / "Qt.ini";
  const IniSections qt = fs::is_regular_file(qtIni, ec) ? ReadIni(qtIni) : IniSections{};

  if (fs::is_regular_file(runtimeIni, ec)) {
    const IniSections runtime = ReadIni(runtimeIni);
    for (const char* name : {"runtime", "primedgun", "primegun"}) {
      const auto section = runtime.find(name);
      if (section != runtime.end()) {
        result.values = MapRuntimeSettings(section->second);
        result.source = runtimeIni;
        break;
      }
    }
  }
  if (result.source.empty()) {
    // Builds before PrimedGun.ini: the older group first so the newer wins.
    std::map<std::string, std::string> merged;
    for (const char* name : {"primegun", "primedgun"}) {
      const auto section = qt.find(name);
      if (section != qt.end()) {
        for (const auto& [key, value] : section->second) {
          merged[key] = value;
        }
      }
    }
    if (!merged.empty()) {
      result.values = MapRuntimeSettings(merged);
      result.source = qtIni;
    }
  }

  if (const auto group = qt.find("primedgun"); group != qt.end()) {
    if (const auto slot = group->second.find("cannon_texture_slot"); slot != group->second.end()) {
      const std::string value = Canonical(*FindKey("vr_cannon_texture_slot"), Trim(slot->second));
      std::erase_if(result.values, [](const auto& kv) { return kv.first == "vr_cannon_texture_slot"; });
      result.values.emplace_back("vr_cannon_texture_slot", value);
      if (result.source.empty()) {
        result.source = qtIni;
      }
    }
  }
  if (const auto window = qt.find("mainwindow"); window != qt.end()) {
    if (const auto game = window->second.find("selected_metroid_prime_path");
        game != window->second.end()) {
      result.gamePath = game->second;
    }
  }
  return result;
}

fs::path OldCard::UserFolder() const {
  const fs::path gc = isFolder ? path.parent_path().parent_path() : path.parent_path();
  return gc.parent_path();
}

OldCard FindNearbyOldCard(const fs::path& launcherFolder) {
  const fs::path appDir = launcherFolder.lexically_normal();
  const fs::path installRoot = appDir.parent_path();

  // First pass: folders beside the launcher's own.
  std::vector<fs::path> dirs;
  for (const fs::path& child : ChildFolders(installRoot)) {
    if (!SamePath(child, appDir)) {
      AddSearchBase(dirs, child);
    }
  }
  if (OldCard card = BestCardIn(dirs); card.Found()) {
    return card;
  }

  // Fallback pass: folders beside the launcher's parent.
  const fs::path installParent = installRoot.parent_path();
  dirs.clear();
  AddUnique(dirs, installParent / "GC");
  AddUnique(dirs, installParent / "User" / "GC");
  AddUnique(dirs, installParent / "core" / "User" / "GC");
  for (const fs::path& sibling : ChildFolders(installParent)) {
    if (!SamePath(sibling, installRoot)) {
      AddSearchBase(dirs, sibling);
    }
  }
  return BestCardIn(dirs);
}

OldCard FindOldCardUnder(const fs::path& base) {
  std::vector<fs::path> dirs;
  AddSearchBase(dirs, base);
  return BestCardIn(dirs);
}

} // namespace PrimedGunLauncher

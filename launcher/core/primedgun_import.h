// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bringing a PrimedGun (Dolphin) install's save and settings over: finding its
// memory card the way PrimedGun's own "Transfer Old Memory Card" did
// (DolphinQt/MainWindow.cpp), and turning its RuntimeSettings (PrimedGun.ini
// [Runtime], or Qt.ini [primedgun] / [primegun] from older builds) into the
// port's vr_* keys. The card itself is copied by PortGci (platform/port_gci.h).

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PrimedGunLauncher {

// Section name (lower case) -> key -> value, for Dolphin's IniFile and QSettings
// files alike; surrounding quotes are removed from values.
using IniSections = std::map<std::string, std::map<std::string, std::string>>;
IniSections ParseIni(std::string_view text);
IniSections ReadIni(const std::filesystem::path& path);

// One PrimedGun RuntimeSettings section as port keys and values. `enabled`,
// the internal patch flags and the settings the port has no key for are left
// out.
std::vector<std::pair<std::string, std::string>>
MapRuntimeSettings(const std::map<std::string, std::string>& section);

struct OldSettings {
  std::filesystem::path source; // the file the values came from; empty when none was found
  std::vector<std::pair<std::string, std::string>> values;
  std::string gamePath; // the disc the old launcher had selected, if any
};
// Reads `<user>/Config/PrimedGun.ini`, falling back to `<user>/Config/Qt.ini`.
// The Qt launcher kept the applied cannon slot in Qt.ini, so that one wins.
OldSettings ReadOldSettings(const std::filesystem::path& userFolder);

// A Dolphin card found near an old install: a raw image (MemoryCardA.USA.raw,
// .gcp) or a GCI folder (<GC>/USA/Card A).
struct OldCard {
  std::filesystem::path path;
  bool isFolder = false;
  int score = -1;
  bool Found() const { return !path.empty(); }
  // The Dolphin user folder holding it (the parent of GC).
  std::filesystem::path UserFolder() const;
};

// PrimedGun's two passes around the launcher's folder: the folders beside it,
// then the folders beside its parent, each with the usual portable layouts.
OldCard FindNearbyOldCard(const std::filesystem::path& launcherFolder);
// The same layouts under a folder the player picked (or that folder itself).
OldCard FindOldCardUnder(const std::filesystem::path& base);

} // namespace PrimedGunLauncher

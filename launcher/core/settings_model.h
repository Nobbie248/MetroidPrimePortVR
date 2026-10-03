// SPDX-License-Identifier: GPL-3.0-or-later
//
// The launcher's working copy of the keys in launcher_keys: what the file had
// when it was last read, and what the tabs changed since. Saving writes only
// the changed keys, so a value the game wrote in the meantime survives.

#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PrimedGunLauncher {

class PortSettingsFile;

class SettingsModel {
public:
  SettingsModel();

  // Takes every key's value from the file, or its default; drops all changes.
  void Load(const PortSettingsFile& file);

  const std::string& Value(std::string_view key) const;
  bool Bool(std::string_view key) const;
  float Float(std::string_view key) const;
  int Int(std::string_view key) const;

  // Unknown keys are ignored; values are stored as the game would write them.
  void Set(std::string_view key, std::string_view value);
  void SetBool(std::string_view key, bool value);
  void SetFloat(std::string_view key, float value);
  void SetInt(std::string_view key, int value);
  void ResetToDefault(std::string_view key);
  // Every key marked resetAll in launcher_keys.
  void ResetAll();

  bool Dirty() const;
  std::vector<std::pair<std::string, std::string>> Changes() const;
  // Writes the changes into `file` (read again just before saving); once the
  // file is on disk, MarkSaved makes the current values the loaded ones.
  void ApplyChanges(PortSettingsFile& file) const;
  void MarkSaved();
  // After one key was written on its own.
  void MarkSaved(std::string_view key);
  void DiscardChanges();

private:
  std::map<std::string, std::string, std::less<>> m_loaded;
  std::map<std::string, std::string, std::less<>> m_current;
};

} // namespace PrimedGunLauncher

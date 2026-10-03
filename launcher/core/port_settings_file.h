// SPDX-License-Identifier: GPL-3.0-or-later
//
// The port's port_settings.ini as text, edited line by line. The game reads it
// with its own rules (platform/debug_ui.cpp LoadSettings: '#' starts a comment
// anywhere, keys and values are trimmed, a later line overrides an earlier
// one) and rewrites the whole file when it saves, so the launcher changes only
// the lines of the keys it owns and leaves every other byte alone.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PrimedGunLauncher {

class PortSettingsFile {
public:
  // The comment line WriteVrSettings (platform/vr/vr_settings.cpp) puts before
  // the vr_* keys; new vr_* keys are added to that block.
  static constexpr std::string_view kVrBlockHeader = "# VR (PrimedGun)";

  // A missing file reads as empty. False only when the file exists and cannot
  // be read.
  bool Load(const std::filesystem::path& path);
  void Parse(std::string_view text);

  // The value the game would read for `key`, or nothing when no line sets it.
  std::optional<std::string> Get(std::string_view key) const;
  // Rewrites the line the game reads `key` from, or adds one: a vr_* key at the
  // end of the VR block (created when missing), any other key before it.
  void Set(const std::string& key, const std::string& value);

  std::string Text() const;
  // Writes Text() to `<path>.tmp` and renames it over `path`.
  bool Save(const std::filesystem::path& path, std::string& error) const;

private:
  std::vector<std::string> m_lines;
  bool m_crlf =
#if defined(_WIN32)
      true; // the game writes the file in text mode
#else
      false;
#endif
};

} // namespace PrimedGunLauncher

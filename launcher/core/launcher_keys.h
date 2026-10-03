// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every port_settings.ini key the launcher edits, with the value the game uses
// when the key is absent (the PortVrSettings defaults in
// platform/include/vr/vr_settings.h, or the overlay's own for the display
// keys) and the range the launcher's slider offers. `active` is false for the
// PrimedGun settings the port saves but nothing reads yet; the launcher still
// shows them, tagged, so they are ready when the feature lands.

#pragma once

#include <span>
#include <string>
#include <string_view>

namespace PrimedGunLauncher {

enum class KeyKind { Bool, Float, Int, Choice };

struct KeyInfo {
  std::string_view key;
  KeyKind kind;
  std::string_view defaultValue; // as the game writes it
  float uiMin = 0.0f;            // Float / Int: the slider's range and step
  float uiMax = 0.0f;
  float uiStep = 0.0f;
  bool active = true;    // read by the game today
  bool resetAll = true;  // restored by the footer's Reset All
  std::string_view choices = {}; // Choice: the accepted names, comma separated
};

std::span<const KeyInfo> LauncherKeys();
const KeyInfo* FindKey(std::string_view key);

// The game's spellings: booleans 1/0, floats as an ostream prints a float.
bool ParseBool(std::string_view value);
std::string FormatBool(bool value);
std::string FormatFloat(float value);
std::string FormatInt(int value);
// `value` as the game would write it back after reading it, or the key's
// default when the game would ignore it.
std::string Canonical(const KeyInfo& info, std::string_view value);

} // namespace PrimedGunLauncher

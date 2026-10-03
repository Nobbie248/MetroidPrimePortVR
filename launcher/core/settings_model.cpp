// SPDX-License-Identifier: GPL-3.0-or-later

#include "settings_model.h"

#include "launcher_keys.h"
#include "port_settings_file.h"

#include <cstdlib>

namespace PrimedGunLauncher {

SettingsModel::SettingsModel() {
  for (const KeyInfo& info : LauncherKeys()) {
    m_loaded.emplace(std::string(info.key), std::string(info.defaultValue));
  }
  m_current = m_loaded;
}

void SettingsModel::Load(const PortSettingsFile& file) {
  for (const KeyInfo& info : LauncherKeys()) {
    const auto stored = file.Get(info.key);
    m_loaded[std::string(info.key)] =
        stored.has_value() ? Canonical(info, *stored) : std::string(info.defaultValue);
  }
  m_current = m_loaded;
}

const std::string& SettingsModel::Value(std::string_view key) const {
  static const std::string kEmpty;
  const auto it = m_current.find(key);
  return it != m_current.end() ? it->second : kEmpty;
}

bool SettingsModel::Bool(std::string_view key) const { return ParseBool(Value(key)); }

float SettingsModel::Float(std::string_view key) const {
  return static_cast<float>(std::strtod(Value(key).c_str(), nullptr));
}

int SettingsModel::Int(std::string_view key) const {
  return static_cast<int>(std::strtol(Value(key).c_str(), nullptr, 10));
}

void SettingsModel::Set(std::string_view key, std::string_view value) {
  const KeyInfo* info = FindKey(key);
  if (info == nullptr) {
    return;
  }
  m_current[std::string(key)] = Canonical(*info, value);
}

void SettingsModel::SetBool(std::string_view key, bool value) { Set(key, FormatBool(value)); }

void SettingsModel::SetFloat(std::string_view key, float value) { Set(key, FormatFloat(value)); }

void SettingsModel::SetInt(std::string_view key, int value) { Set(key, FormatInt(value)); }

void SettingsModel::ResetToDefault(std::string_view key) {
  if (const KeyInfo* info = FindKey(key)) {
    m_current[std::string(key)] = std::string(info->defaultValue);
  }
}

void SettingsModel::ResetAll() {
  for (const KeyInfo& info : LauncherKeys()) {
    if (info.resetAll) {
      m_current[std::string(info.key)] = std::string(info.defaultValue);
    }
  }
}

bool SettingsModel::Dirty() const { return m_current != m_loaded; }

std::vector<std::pair<std::string, std::string>> SettingsModel::Changes() const {
  std::vector<std::pair<std::string, std::string>> changes;
  for (const auto& [key, value] : m_current) {
    const auto loaded = m_loaded.find(key);
    if (loaded == m_loaded.end() || loaded->second != value) {
      changes.emplace_back(key, value);
    }
  }
  return changes;
}

void SettingsModel::ApplyChanges(PortSettingsFile& file) const {
  for (const auto& [key, value] : Changes()) {
    file.Set(key, value);
  }
}

void SettingsModel::MarkSaved() { m_loaded = m_current; }

void SettingsModel::MarkSaved(std::string_view key) {
  const auto it = m_current.find(key);
  if (it != m_current.end()) {
    m_loaded[it->first] = it->second;
  }
}

void SettingsModel::DiscardChanges() { m_current = m_loaded; }

} // namespace PrimedGunLauncher

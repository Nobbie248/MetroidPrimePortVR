// SPDX-License-Identifier: GPL-3.0-or-later

#include "port_settings_file.h"

#include <fstream>
#include <iterator>
#include <system_error>

namespace PrimedGunLauncher {
namespace {

std::string_view Trim(std::string_view text) {
  const size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    return {};
  }
  const size_t last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

// The parts of a line as the game sees them; `key` is empty for a line the
// game ignores (blank, comment only, no '=').
struct LineParts {
  std::string_view key;
  std::string_view value;
  std::string_view comment; // from '#' to the end, kept when the line is rewritten
};

LineParts Split(std::string_view line) {
  LineParts parts;
  std::string_view content = line;
  const size_t hash = line.find('#');
  if (hash != std::string_view::npos) {
    content = line.substr(0, hash);
    parts.comment = line.substr(hash);
  }
  const size_t equals = content.find('=');
  if (equals == std::string_view::npos) {
    return parts;
  }
  parts.key = Trim(content.substr(0, equals));
  parts.value = Trim(content.substr(equals + 1));
  return parts;
}

bool IsVrKey(std::string_view key) { return key.substr(0, 3) == "vr_"; }

} // namespace

bool PortSettingsFile::Load(const std::filesystem::path& path) {
  m_lines.clear();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return true;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) {
    return false;
  }
  Parse(text);
  return true;
}

void PortSettingsFile::Parse(std::string_view text) {
  m_lines.clear();
  if (text.find('\n') != std::string_view::npos) {
    m_crlf = text.find("\r\n") != std::string_view::npos;
  }
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string_view line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    m_lines.emplace_back(line);
    start = end + 1;
  }
}

std::optional<std::string> PortSettingsFile::Get(std::string_view key) const {
  std::optional<std::string> value;
  for (const std::string& line : m_lines) {
    const LineParts parts = Split(line);
    if (!parts.key.empty() && parts.key == key) {
      value = std::string(parts.value);
    }
  }
  return value;
}

void PortSettingsFile::Set(const std::string& key, const std::string& value) {
  for (size_t i = m_lines.size(); i-- > 0;) {
    const LineParts parts = Split(m_lines[i]);
    if (!parts.key.empty() && parts.key == key) {
      std::string line = key + '=' + value;
      if (!parts.comment.empty()) {
        line += ' ';
        line += parts.comment;
      }
      m_lines[i] = std::move(line);
      return;
    }
  }

  size_t header = m_lines.size();
  for (size_t i = 0; i < m_lines.size(); ++i) {
    if (Trim(m_lines[i]) == kVrBlockHeader) {
      header = i;
      break;
    }
  }
  const std::string line = key + '=' + value;
  if (!IsVrKey(key)) {
    m_lines.insert(m_lines.begin() + static_cast<std::ptrdiff_t>(header), line);
    return;
  }
  if (header == m_lines.size()) {
    m_lines.emplace_back(kVrBlockHeader);
    m_lines.push_back(line);
    return;
  }
  size_t insertAt = header + 1;
  for (size_t i = header + 1; i < m_lines.size(); ++i) {
    if (IsVrKey(Split(m_lines[i]).key)) {
      insertAt = i + 1;
    }
  }
  m_lines.insert(m_lines.begin() + static_cast<std::ptrdiff_t>(insertAt), line);
}

std::string PortSettingsFile::Text() const {
  const char* eol = m_crlf ? "\r\n" : "\n";
  std::string text;
  for (const std::string& line : m_lines) {
    text += line;
    text += eol;
  }
  return text;
}

bool PortSettingsFile::Save(const std::filesystem::path& path, std::string& error) const {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  std::filesystem::path temp = path;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "cannot write the settings file";
      return false;
    }
    const std::string text = Text();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) {
      error = "cannot write the settings file";
      return false;
    }
  }
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    error = "cannot replace the settings file";
    return false;
  }
  return true;
}

} // namespace PrimedGunLauncher

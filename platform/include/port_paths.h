#pragma once

// Where the port keeps what it writes: settings, mods, save states, importers,
// the shader caches. One answer for every caller, ending in a separator.
//
// The folder is the one holding the executable, so a copied build carries its
// data with it. In order:
//   1. MP_USER_PATH, when set.
//   2. Android: the folder in shared storage the player moved the data to
//      (F1 > Extras > Data folder, port_data_folder.h) when it can be written
//      to, else the app's private storage (the executable is inside the APK).
//   3. The executable's folder - for an AppImage, the folder the .AppImage file
//      is in - when it can be written to. One exception: an install from
//      before this, whose settings are still in the per-user folder and which
//      has none next to the executable, stays on the per-user folder, so an
//      update does not appear to lose the settings, mods and save states.
//      Moving that folder's contents next to the executable switches it over.
//   4. The per-user folder (SDL_GetPrefPath), for a read-only install such as
//      a Flatpak or a system package.
//
// Header-only so the tests that build a single platform source need no more.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

namespace PortPaths {

namespace detail {

inline std::filesystem::path FromUtf8(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline std::string ToUtf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

inline std::string WithSeparator(std::string dir) {
  if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') {
    dir += '/';
  }
  return dir;
}

inline std::string PrefFolder() {
  std::string dir;
  if (char* pref = SDL_GetPrefPath(nullptr, "Metroid Prime")) {
    dir = pref;
    SDL_free(pref);
  }
  return dir;
}

inline bool Writable(const std::string& dir) {
  const std::filesystem::path probe = FromUtf8(dir) / ".port_write_test";
  {
    std::ofstream file(probe, std::ios::binary);
    if (!file) {
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::remove(probe, ec);
  return true;
}

#if defined(__ANDROID__)
// The app's private storage (getFilesDir). The built-in textures and the
// initial pipeline cache are always here, whatever folder the data is in.
inline std::string PrivateFolder() {
  return WithSeparator(PrefFolder());
}

// Names the folder the data was moved to (port_data_folder.h); absent while
// the data is in private storage.
inline std::string MarkerPath() {
  const std::string priv = PrivateFolder();
  return priv.empty() ? std::string() : priv + "data_folder.txt";
}

inline std::string ReadMarker() {
  const std::string marker = MarkerPath();
  if (marker.empty()) {
    return {};
  }
  std::ifstream file(FromUtf8(marker), std::ios::binary);
  std::string dir;
  std::getline(file, dir);
  while (!dir.empty() && (dir.back() == '\r' || dir.back() == '\n' || dir.back() == ' ')) {
    dir.pop_back();
  }
  // Only an absolute path: anything else would land in the working directory.
  return dir.empty() || dir[0] != '/' ? std::string() : WithSeparator(dir);
}
#endif

#if !defined(__ANDROID__)
// The per-user folder SDL_GetPrefPath would answer with, worked out by hand
// because asking SDL creates it, and a portable copy should leave no trace.
inline std::string LegacyFolder() {
#if defined(_WIN32)
  // SDL's environment is UTF-8 on Windows, unlike std::getenv.
  const char* root = SDL_getenv("APPDATA");
  return root != nullptr && root[0] != '\0' ? std::string(root) + "\\Metroid Prime\\" : std::string();
#elif defined(__APPLE__)
  const char* home = std::getenv("HOME");
  return home != nullptr && home[0] != '\0'
             ? std::string(home) + "/Library/Application Support/Metroid Prime/"
             : std::string();
#else
  if (const char* data = std::getenv("XDG_DATA_HOME"); data != nullptr && data[0] == '/') {
    return WithSeparator(data) + "Metroid Prime/";
  }
  const char* home = std::getenv("HOME");
  return home != nullptr && home[0] != '\0' ? std::string(home) + "/.local/share/Metroid Prime/"
                                            : std::string();
#endif
}

inline std::string ExecutableFolder() {
  // Inside an AppImage the executable sits in a read-only mount that is gone
  // when the game exits; the file the user copies around is $APPIMAGE.
  if (const char* image = std::getenv("APPIMAGE"); image != nullptr && image[0] != '\0') {
    const std::filesystem::path parent = FromUtf8(image).parent_path();
    if (!parent.empty()) {
      return WithSeparator(ToUtf8(parent));
    }
  }
  const char* base = SDL_GetBasePath();
  return base != nullptr ? WithSeparator(base) : std::string();
}
#endif

inline std::string Resolve() {
  if (const char* env = std::getenv("MP_USER_PATH"); env != nullptr && env[0] != '\0') {
    return WithSeparator(env);
  }
#if defined(__ANDROID__)
  // A folder in shared storage the player moved the data to, as long as it
  // can still be written: without the storage permission (revoked, or a
  // reinstall) the private folder is used, and UnavailableFolder says so.
  if (const std::string chosen = ReadMarker(); !chosen.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(FromUtf8(chosen), ec);
    if (Writable(chosen)) {
      return chosen;
    }
  }
  return PrivateFolder();
#else
  const std::string exe = ExecutableFolder();
  if (!exe.empty() && Writable(exe)) {
    std::error_code ec;
    const std::string legacy = LegacyFolder();
    const bool hereHasSettings = std::filesystem::exists(FromUtf8(exe + "port_settings.ini"), ec);
    const bool legacyHasSettings =
        !legacy.empty() && std::filesystem::exists(FromUtf8(legacy + "port_settings.ini"), ec);
    if (hereHasSettings || !legacyHasSettings) {
      return exe;
    }
    return legacy;
  }
  return WithSeparator(PrefFolder());
#endif
}

} // namespace detail

// Empty only when no folder could be found at all.
inline const std::string& UserFolder() {
  static const std::string sFolder = detail::Resolve();
  return sFolder;
}

// The memory card's folder. It has always been the executable's own, also for
// an install whose other data is still in the per-user folder and whatever
// MP_USER_PATH says; it follows UserFolder only where the executable's folder
// is read-only (an AppImage, a Flatpak).
inline std::string CardFolder() {
#if !defined(__ANDROID__)
  if (const char* base = SDL_GetBasePath(); base != nullptr && detail::Writable(base)) {
    return detail::WithSeparator(base);
  }
#endif
  return UserFolder();
}

// True when the data is kept next to the executable or, on Android, outside
// the app's private storage.
inline bool IsPortable() {
  const std::string& folder = UserFolder();
#if defined(__ANDROID__)
  return !folder.empty() && folder != detail::PrivateFolder();
#else
  return !folder.empty() && folder == detail::ExecutableFolder();
#endif
}

#if defined(__ANDROID__)
// The data folder the player chose but this run could not use (no storage
// permission), or empty.
inline std::string UnavailableFolder() {
  const std::string chosen = detail::ReadMarker();
  if (chosen.empty() || chosen == UserFolder() || std::getenv("MP_USER_PATH") != nullptr) {
    return {};
  }
  return chosen;
}
#endif

} // namespace PortPaths

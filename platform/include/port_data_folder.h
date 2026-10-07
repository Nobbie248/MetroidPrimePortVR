#pragma once

// Android's data folder: moving the settings, memory card, mods, save states and
// texture pack out of the app's private storage into a folder the player can
// reach with a file manager or a USB cable, and back (F1 > System > Data
// folder). The copy engine is platform-neutral so it can be tested on the
// desktop; the panel is Android only.
//
// The choice is a marker file in private storage (port_paths.h), read once at
// startup, so a move ends in a restart.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace PortDataFolder {

// The folder in shared storage the data moves to.
std::string SharedFolder();

// Whether a path relative to a data folder is left out of a move: what the app
// re-creates on every launch (the built-in textures, the initial pipeline
// cache), the marker itself, and half-written copies.
bool IsSkipped(const std::string& relative);

struct Totals {
  uint64_t bytes = 0;
  uint64_t files = 0;
};

// What a move of `from` would copy.
Totals Measure(const std::string& from);

// Whether `dir` holds anything a move would copy.
bool HasData(const std::string& dir);

// False while `dir` holds a copy that was cut short (cancelled, storage full,
// the app killed): some files are there, others are not.
bool IsComplete(const std::string& dir);

struct Progress {
  std::atomic< uint64_t > bytesDone{0};
  std::atomic< uint64_t > filesDone{0};
  std::atomic< bool > cancel{false};
};

// Copies every file of `from` that IsSkipped keeps into `to`, replacing what is
// there. Each file is written under a temporary name and renamed when complete,
// so a copy cut short leaves no truncated file, and `to` reads as incomplete
// until the last file is in. `changedOnly` skips files whose copy has the same
// size and is not older (a second pass for what changed since the first).
// False with `error` set on failure or cancel.
bool CopyTree(const std::string& from, const std::string& to, Progress& progress, std::string& error,
              bool changedOnly = false);

// Deletes what a move would copy from `dir`, except the files `keep` (given
// the relative path) holds on to; the rest stays.
void DeleteData(const std::string& dir, const std::function< bool(const std::string&) >& keep = {});

#if defined(__ANDROID__)
void DrawPanel();
#endif

} // namespace PortDataFolder

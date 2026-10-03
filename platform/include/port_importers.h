#pragma once

// Mod importers: programs the user puts in <user folder>/importers that build a mod
// from files of their own (another release of the game, a model pack), so the
// port can start one and show how it is getting on instead of the user running
// it by hand. The port ships none and knows nothing about what one reads.
//
// An importer is any executable file in that folder (on Windows: .exe, .bat,
// .cmd). It is started with the optional argument the user typed as argv[1]
// and with MP_MODS_DIR set to the mods folder it should write its mod into.
// Its stdout and stderr are read a line at a time; the last line is its
// status. Exit code 0 means the mod is in place, and since mods are read at
// startup the game has to be restarted to load it.
//
// Desktop only: Android has no way to run a user's program.

#include <cstddef>
#include <string>
#include <vector>

namespace PortImporters {

struct State {
  bool running = false;
  bool finished = false; // a run ended; exitCode and lines are its result
  int exitCode = 0;
  bool cancelled = false; // Cancel ended it
  std::string name;
  std::vector<std::string> lines; // the newest kMaxLines of output
};

inline constexpr size_t kMaxLines = 200;

// The importers folder, created if missing. Empty if there is no user folder.
std::string Folder();
// The importers in it, sorted by name.
std::vector<std::string> List();

// Starts `name` (from List) with `argument` (may be empty). False, with a
// line in the state saying why, if it could not be started or one is running.
bool Start(const std::string& name, const std::string& argument);
// Reads what the importer has printed since the last call; call once a frame.
const State& Poll();
// Kills the running importer.
void Cancel();

// `--import [name [argument]]`: lists the importers, or runs one to the end
// with its output on stdout. Returns the process exit code.
int RunFromCommandLine(int argc, char** argv);

// Appends `text` to `lines` split at line ends, the unfinished tail kept in
// `partial`; a carriage return restarts the line, as a progress bar does.
void SplitLines(const char* text, size_t size, std::string& partial, std::vector<std::string>& lines);

} // namespace PortImporters

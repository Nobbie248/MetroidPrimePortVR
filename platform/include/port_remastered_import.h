#pragma once

// Builds the "remastered-models" mod inside the port, from the user's own
// copy of Metroid Prime Remastered (.nsp) and their own Switch key file.
//
// Nothing is extracted to disk on the way: the paks are read straight out of
// the encrypted image (port_remastered_nsp.h), the retail models they replace
// come off the unmodded disc that is already open, and each model in
// port_remastered_table.h goes through port_remastered_convert.h. The output
// is the user's, for their use only; the port ships none of it.
//
// The mod is written to a hidden staging folder in the mods folder and only
// takes the place of <mods>/remastered-models at the next start, before mods
// are scanned: the running game may be reading the files of the one it loaded.

#include <string>
#include <vector>

namespace PortRemastered {

struct ImportState {
  bool running = false;
  bool finished = false;   // a run ended; ok, message and lines are its result
  bool ok = false;         // at least one model converted and the mod is staged
  bool cancelled = false;
  int done = 0;            // models tried so far
  int total = 0;
  int failed = 0;
  std::string message;     // what it is doing, or how it ended
  std::vector<std::string> lines;  // the newest kImportMaxLines of its log
};

inline constexpr size_t kImportMaxLines = 200;

// The mod folder an import ends up as.
inline constexpr const char* kImportModName = "remastered-models";

// ~/.switch/prod.keys if it is there, else empty.
std::string DefaultKeysPath();

// Starts an import on worker threads; `threads` 0 leaves a couple of cores to
// the game. The disc must be open. False (with the reason in the state's
// message) when one is already running or there is no mods folder.
bool StartImport(const std::string& nspPath, const std::string& keysPath, int threads = 0);
// Only the menu movies (port_remastered_movie.h), into the mod an earlier
// import made: for a player who had no ffmpeg then. Same state and cancelling;
// false when one is running or there is no such mod.
bool StartMovieImport(const std::string& nspPath, const std::string& keysPath);
// Whether the next import also converts the rooms themselves (five times the
// size and twice the time). MP_REMASTERED_GEOMETRY, when set, decides instead.
void SetImportGeometry(bool on);
ImportState ImportStatus();
// Asks the running import to stop; it ends at the next model.
void CancelImport();
// Cancels and waits. Call before the disc is closed.
void StopImport();

// Moves a finished import into place. Call before PortMods::Initialize().
// True if a mod was installed.
bool ApplyPendingImport();

// `--import-remastered <nsp> [keys]`: imports with every core, progress on
// stdout, and installs the mod. The disc must be open. Returns the exit code.
// `--import-remastered-movies` is StartMovieImport() the same way.
int RunImportFromCommandLine(const std::string& nspPath, const std::string& keysPath, bool moviesOnly = false);

}  // namespace PortRemastered

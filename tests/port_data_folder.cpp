// The data folder move's copy engine (port_data_folder.h): what it leaves out,
// that it replaces what is there, and that a cancelled copy leaves nothing
// half-written.

#include "port_data_folder.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

int sFailures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator< char >(file), {});
}

} // namespace

int main(int argc, char** argv) {
  const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "port-data-folder-test";
  fs::remove_all(root);
  const fs::path from = root / "private";
  const fs::path to = root / "shared";

  Check(PortDataFolder::IsSkipped("textures"), "built-in textures skipped");
  Check(PortDataFolder::IsSkipped("textures/xbox/a.png"), "inside the built-in textures skipped");
  Check(PortDataFolder::IsSkipped("initial_pipeline_cache.db"), "initial pipeline cache skipped");
  Check(PortDataFolder::IsSkipped("data_folder.txt"), "marker skipped");
  Check(PortDataFolder::IsSkipped("user_textures.partial"), "texture pack staging skipped");
  Check(PortDataFolder::IsSkipped("disc.iso.part"), "partial disc copy skipped");
  Check(PortDataFolder::IsSkipped("mods/a.CMDL.mpcopy"), "half-copied file skipped");
  Check(!PortDataFolder::IsSkipped("user_textures/textures/a.png"), "a pack's own textures folder kept");
  Check(!PortDataFolder::IsSkipped("mods/textures"), "a nested textures folder kept");
  Check(!PortDataFolder::IsSkipped("port_settings.ini"), "settings kept");

  Check(!PortDataFolder::HasData((from).string()), "a missing folder holds no data");
  Write(from / "textures" / "xbox" / "a.png", "built-in");
  Write(from / "initial_pipeline_cache.db", "cache");
  Check(!PortDataFolder::HasData(from.string()), "only skipped files is no data");

  Write(from / "port_settings.ini", "settings");
  Write(from / "MemoryCardA.USA.raw", std::string(3 << 20, 'c'));
  Write(from / "mods" / "remastered-models" / "1234.CMDL", "model");
  Write(from / "user_textures" / "textures" / "b.png", "pack");
  Write(to / "port_settings.ini", "old settings");

  const PortDataFolder::Totals totals = PortDataFolder::Measure(from.string());
  Check(totals.files == 4, "four files to move");
  Check(totals.bytes == 8 + (3 << 20) + 5 + 4, "their size");
  Check(PortDataFolder::HasData(to.string()), "a folder with settings holds data");

  PortDataFolder::Progress progress;
  std::string error;
  Check(PortDataFolder::CopyTree(from.string(), to.string(), progress, error), "copy succeeds");
  Check(progress.filesDone == 4 && progress.bytesDone == totals.bytes, "progress counts everything");
  Check(Read(to / "port_settings.ini") == "settings", "existing file replaced");
  Check(Read(to / "mods" / "remastered-models" / "1234.CMDL") == "model", "nested file copied");
  Check(fs::file_size(to / "MemoryCardA.USA.raw") == (3u << 20), "large file copied whole");
  Check(!fs::exists(to / "textures"), "built-in textures not copied");
  Check(!fs::exists(to / "initial_pipeline_cache.db"), "pipeline cache not copied");

  const fs::path cancelled = root / "cancelled";
  PortDataFolder::Progress stop;
  stop.cancel = true;
  Check(!PortDataFolder::CopyTree(from.string(), cancelled.string(), stop, error), "cancelled copy fails");
  bool leftovers = false;
  if (fs::exists(cancelled)) {
    for (const auto& entry : fs::recursive_directory_iterator(cancelled)) {
      leftovers |= entry.is_regular_file() && entry.path().filename() != ".mpcopy_incomplete";
    }
  }
  Check(!leftovers, "cancelled copy leaves no files");
  Check(!PortDataFolder::IsComplete(cancelled.string()), "cancelled copy marked incomplete");
  Check(PortDataFolder::IsComplete(to.string()), "finished copy complete");
  Check(!PortDataFolder::HasData(cancelled.string()), "the incomplete flag is not data");

  // The pass before the switch copies only what changed since.
  Write(from / "port_settings.ini", "newer settings");
  PortDataFolder::Progress sync;
  Check(PortDataFolder::CopyTree(from.string(), to.string(), sync, error, true), "sync succeeds");
  Check(Read(to / "port_settings.ini") == "newer settings", "changed file re-copied");
  Check(sync.filesDone == 4 && sync.bytesDone == 14, "unchanged files skipped");
  Check(PortDataFolder::IsComplete(to.string()), "synced copy complete");

  Check(PortDataFolder::IsSkipped("pipeline_cache.db"), "shader cache skipped");
  Check(PortDataFolder::IsSkipped("pipeline_cache.db-wal"), "shader cache journal skipped");
  Check(PortDataFolder::IsSkipped("dawn_cache.db"), "Dawn cache skipped");
  Check(PortDataFolder::IsSkipped("texture_dumps/a.png"), "texture dumps skipped");

  Write(from / "disc.iso", "disc");
  PortDataFolder::DeleteData(from.string(), [](const std::string& rel) { return rel == "disc.iso"; });
  Check(Read(from / "disc.iso") == "disc", "kept file survives the delete");
  PortDataFolder::DeleteData(from.string());
  Check(!PortDataFolder::HasData(from.string()), "old copy deleted");
  Check(fs::exists(from / "textures" / "xbox" / "a.png"), "built-in textures survive the delete");
  Check(fs::exists(from / "initial_pipeline_cache.db"), "pipeline cache survives the delete");
  Check(!fs::exists(from / "mods"), "emptied folders removed");

  fs::remove_all(root);
  if (sFailures == 0) {
    std::printf("port_data_folder_tests: all passed\n");
  }
  return sFailures == 0 ? 0 : 1;
}

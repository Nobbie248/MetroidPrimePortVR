#include "port_remastered_import.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <aurora/dvd.h>

#include "port_map_icons.h"
#include "port_model_variant.h"
#include "port_mods.h"
#include "port_remastered_cmdl.h"
#include "port_remastered_convert.h"
#include "port_remastered_font.h"
#include "port_remastered_hud.h"
#include "port_remastered_map.h"
#include "port_remastered_movie.h"
#include "port_remastered_nsp.h"
#include "port_remastered_pak.h"
#include "port_remastered_room.h"
#include "port_remastered_table.h"
#include "port_remastered_text.h"
#include "port_remastered_txtr.h"
#include "port_ws.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace PortRemastered {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kSMDL = 0x534D444C;
constexpr uint32_t kWMDL = 0x574D444C;  // a liquid's surface
constexpr uint32_t kCSKR = 0x43534B52;
constexpr uint32_t kANCS = 0x414E4353;
constexpr uint32_t kTXTR = 0x54585452;
constexpr uint32_t kMLVL = 0x4D4C564C;
constexpr uint32_t kMREA = 0x4D524541;
constexpr uint32_t kSTRG = 0x53545247;
constexpr uint32_t kMSBT = 0x4D534254;
constexpr uint32_t kFONT = 0x464F4E54;
constexpr uint32_t kGUIF = 0x47554946;
constexpr uint32_t kCMAP = 0x434D4150;
constexpr uint32_t kMAPA = 0x4D415041;
constexpr uint32_t kMAPW = 0x4D415057;
constexpr uint32_t kFRME = 0x46524D45;
constexpr uint32_t kFMV0 = 0x464D5630;

constexpr const char* kStagingName = ".remastered-models.importing";
// Written last, so a staging folder without it is an import that was cut short.
constexpr const char* kMarkerName = "import-complete";
constexpr const char* kRoomFolder = "roomenv";
constexpr const char* kGeometryFolder = "roomgeo";
constexpr const char* kTextFolder = "text";
constexpr const char* kFontFolder = "font";
constexpr const char* kFontName = "deface.sdfont";
constexpr const char* kHudFolder = "hud";
constexpr const char* kMapFolder = "map";
// The disc's own folder: a mod's file there is opened in place of the disc's.
constexpr const char* kMovieFolder = "Video";
// Largest edge of a room geometry texture: there are thousands of them.
constexpr int kGeometryTexture = 1024;
// Texcoords a second a water surface's wave layers move by.
constexpr double kLiquidDrift = 0.02;

// The rooms whose geometry is imported, from MP_REMASTERED_GEOMETRY: "all", or
// room names (any part of one) separated by commas, or "none". Without it,
// what SetImportGeometry() last said: none, the port's drawing of room
// geometry being unfinished.
std::atomic<bool> sGeometry{false};

bool WantsGeometry(const std::string& room) {
  const char* env = std::getenv("MP_REMASTERED_GEOMETRY");
  if (env == nullptr || env[0] == '\0') {
    return sGeometry.load();
  }
  const std::string list = env;
  if (list == "none") {
    return false;
  }
  if (list == "all") {
    return true;
  }
  for (size_t at = 0; at <= list.size();) {
    const size_t comma = std::min(list.find(',', at), list.size());
    if (comma > at && room.find(list.substr(at, comma - at)) != std::string::npos) {
      return true;
    }
    at = comma + 1;
  }
  return false;
}

// The import runs beside the game on nearly every core: its threads only take
// the time the game leaves, or the game stutters for as long as it runs.
void YieldToGame() {
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#elif defined(__linux__)
  setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), 19);
#endif
}

std::mutex sStateMutex;
ImportState sState;
std::thread sThread;
std::atomic<bool> sCancel{false};

fs::path PathFromString(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

void SetMessage(const std::string& message) {
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState.message = message;
}

void AddLine(const std::string& line) {
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState.lines.push_back(line);
  if (sState.lines.size() > kImportMaxLines) {
    sState.lines.erase(sState.lines.begin(), sState.lines.end() - kImportMaxLines);
  }
}

void Finish(bool ok, const std::string& message) {
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState.running = false;
  sState.finished = true;
  sState.ok = ok;
  sState.cancelled = !ok && sCancel.load();
  sState.message = message;
}

// MP_REMASTERED_TEXT=0 leaves the disc's wording alone.
bool WantsText() {
  const char* env = std::getenv("MP_REMASTERED_TEXT");
  return env == nullptr || std::strcmp(env, "0") != 0;
}

// MP_REMASTERED_HUD=0 leaves the disc's HUD alone.
bool WantsHud() {
  const char* env = std::getenv("MP_REMASTERED_HUD");
  return env == nullptr || std::strcmp(env, "0") != 0;
}

// MP_REMASTERED_MOVIES=0 leaves the disc's movies alone; a size and rate
// ("1280x720@30") is what they are written as.
bool WantsMovies(MovieFormat& format) {
  const char* env = std::getenv("MP_REMASTERED_MOVIES");
  if (env == nullptr || env[0] == '\0' || std::strcmp(env, "1") == 0) {
    return true;
  }
  if (std::strcmp(env, "0") == 0) {
    return false;
  }
  if (!ParseMovieFormat(env, format)) {
    AddLine(std::string("MP_REMASTERED_MOVIES: \"") + env + "\" is not a size and rate like 1280x720@30");
  }
  return true;
}

// --- The retail disc ----------------------------------------------------------

// The CMDL, CSKR, TXTR, MLVL, MREA, STRG, FRME, MAPA and MAPW resources of the unmodded disc, and every id on it.
class Retail {
public:
  ~Retail() {
    for (auto& [entry, handle] : m_handles) {
      aurora_dvd_base_close(handle);
    }
  }

  bool Index(std::string& error) {
    const int32_t baseCount = aurora_dvd_base_entry_count();
    for (const auto& [entry, path] : PortMods::DiscPaks()) {
      if (entry < 0 || entry >= baseCount) {
        continue;  // a PAK only a mod brings
      }
      void* handle = aurora_dvd_base_open(entry);
      if (handle == nullptr) {
        continue;
      }
      m_handles[entry] = handle;
      std::vector<uint8_t> header;
      PortMods::PakTable table;
      size_t needed = 0x10000;
      bool parsed = false;
      while (needed <= (64u << 20)) {
        header.resize(needed);
        const size_t got = ReadAt(handle, 0, header.data(), header.size());
        if (PortMods::ParsePakTable(header.data(), got, table, needed)) {
          parsed = true;
          break;
        }
        if (got < header.size() || needed <= header.size()) {
          break;
        }
      }
      if (!parsed) {
        continue;
      }
      for (const PortMods::PakResource& res : table.resources) {
        m_ids.insert(res.id);
        if (res.type == kCMDL || res.type == kCSKR || res.type == kANCS || res.type == kTXTR || res.type == kMLVL ||
            res.type == kMREA || res.type == kSTRG || res.type == kFRME || res.type == kMAPA || res.type == kMAPW) {
          m_resources.emplace(Key(res.type, res.id), Where{entry, res.offset, res.size, res.compressed != 0});
        }
      }
    }
    if (m_resources.empty()) {
      error = "no models found on the disc";
      return false;
    }
    return true;
  }

  bool HasId(uint32_t id) const { return m_ids.count(id) != 0; }

  bool Read(uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
    const auto found = m_resources.find(Key(type, id));
    if (found == m_resources.end()) {
      return false;
    }
    const Where& where = found->second;
    std::vector<uint8_t> raw(where.size);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (ReadAt(m_handles[where.entry], where.offset, raw.data(), raw.size()) != raw.size()) {
        return false;
      }
    }
    if (!where.compressed) {
      out = std::move(raw);
      return true;
    }
    // A big-endian length, then a zlib stream: two header bytes, the DEFLATE
    // data, and a checksum the decoder never reaches.
    if (raw.size() < 6) {
      return false;
    }
    const size_t length = size_t(raw[0]) << 24 | size_t(raw[1]) << 16 | size_t(raw[2]) << 8 | size_t(raw[3]);
    PortWs::Inflater inflater;
    inflater.SetKeepWindow(false);
    std::string inflated;
    if (!inflater.InflateMessage(std::string(raw.begin() + 6, raw.end()), inflated, length) ||
        inflated.size() != length) {
      return false;
    }
    out.assign(inflated.begin(), inflated.end());
    return true;
  }

private:
  struct Where {
    int32_t entry;
    uint32_t offset;
    uint32_t size;
    bool compressed;
  };

  static uint64_t Key(uint32_t type, uint32_t id) { return uint64_t(type) << 32 | id; }

  static size_t ReadAt(void* handle, uint64_t offset, uint8_t* out, size_t size) {
    if (aurora_dvd_base_seek(handle, int64_t(offset), 0) != int64_t(offset)) {
      return 0;
    }
    size_t done = 0;
    while (done < size) {
      const int64_t got = aurora_dvd_base_read(handle, out + done, size - done);
      if (got <= 0) {
        break;
      }
      done += size_t(got);
    }
    return done;
  }

  std::mutex m_mutex;
  std::map<int32_t, void*> m_handles;
  std::unordered_map<uint64_t, Where> m_resources;
  std::unordered_set<uint32_t> m_ids;
};

// --- The Remastered image ------------------------------------------------------

// Every model and texture in the image's paks, by id.
class Remastered {
public:
  bool Open(const std::string& nspPath, const std::string& keysPath, std::string& error) {
    if (!m_nsp.Open(nspPath, keysPath, error)) {
      return false;
    }
    std::vector<const RomfsFile*> files;
    for (const RomfsFile& file : m_nsp.Files()) {
      if (file.path.size() > 4 && file.path.compare(file.path.size() - 4, 4, ".pak") == 0) {
        files.push_back(&file);
      }
    }
    for (size_t i = 0; i < files.size(); ++i) {
      if (sCancel) {
        error = "cancelled";
        return false;
      }
      SetMessage("Reading the paks (" + std::to_string(i + 1) + "/" + std::to_string(files.size()) + ")");
      const RomfsFile* file = files[i];
      auto pak = std::make_unique<Pak>();
      std::string pakError;
      const ReadFn read = [this, file](uint64_t offset, void* out, size_t size) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string ignored;
        return m_nsp.Read(*file, offset, out, size, ignored);
      };
      if (!pak->Open(read, file->size, pakError)) {
        AddLine(file->path + ": " + pakError);
        continue;
      }
      const std::vector<PakAsset>& assets = pak->Assets();
      for (size_t a = 0; a < assets.size(); ++a) {
        const uint32_t type = assets[a].type;
        if (type == kCMDL || type == kSMDL || type == kWMDL) {
          m_models.emplace(assets[a].id, Where{m_paks.size(), a});
          for (const std::string& name : assets[a].names) {
            m_modelNames.emplace(FrameKey(name), Where{m_paks.size(), a});
          }
        } else if (type == kTXTR) {
          m_textures.emplace(assets[a].id, Where{m_paks.size(), a});
          for (const std::string& name : assets[a].names) {
            m_textureNames.emplace(FrameKey(name), Where{m_paks.size(), a});
          }
        } else if (type == kMSBT) {
          m_texts.emplace(assets[a].id, Where{m_paks.size(), a});
        } else if (type == kFONT) {
          m_fonts.emplace(assets[a].id, Where{m_paks.size(), a});
        } else if (type == kFMV0) {
          m_movies.emplace(IdToString(assets[a].id), Where{m_paks.size(), a});
        } else if (type == kGUIF) {
          for (const std::string& name : assets[a].names) {
            m_frames.emplace(FrameKey(name), Where{m_paks.size(), a});
          }
        } else if (type == kCMAP) {
          for (const std::string& name : assets[a].names) {
            m_maps.emplace(FrameKey(name), Where{m_paks.size(), a});
          }
        }
      }
      m_paks.push_back(std::move(pak));
      m_paths.push_back(file->path);
    }
    if (m_models.empty() && m_movies.empty()) {
      error = "no models in this image; is it Metroid Prime Remastered?";
      return false;
    }
    return true;
  }

  bool ReadModel(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    return Read(m_models, id, out, error);
  }
  bool ReadTexture(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    return Read(m_textures, id, out, error);
  }

  // The FONT assets, in id order. There are several, with different sets of characters.
  std::vector<ModelUuid> Fonts() const {
    std::vector<ModelUuid> ids;
    for (const auto& [id, where] : m_fonts) {
      ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  }
  bool ReadFont(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    return Read(m_fonts, id, out, error);
  }

  // A GUI frame by its asset name ("FRME_CombatHud"), whatever folder and case the pak has it under.
  bool ReadFrame(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = m_frames.find(FrameKey(name));
    if (found == m_frames.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  // A texture by its asset name ("TXTR_IconS"), as ReadFrame finds a frame.
  bool ReadTextureNamed(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = m_textureNames.find(FrameKey(name));
    if (found == m_textureNames.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  // A model by its asset name ("CMDL_MapCompass"), as ReadFrame finds a frame.
  bool ReadModelNamed(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = m_modelNames.find(FrameKey(name));
    if (found == m_modelNames.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  // A world's map by its asset name ("CMAP_IceLevel"), as ReadFrame finds a frame.
  bool ReadMap(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = m_maps.find(FrameKey(name));
    if (found == m_maps.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  // A movie by its id as IdToString prints it.
  bool ReadMovie(const std::string& id, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = m_movies.find(id);
    if (found == m_movies.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  // Every text asset, each once however many paks carry it.
  std::vector<ModelUuid> Texts() const {
    std::vector<ModelUuid> ids;
    for (const auto& [id, where] : m_texts) {
      ids.push_back(id);
    }
    return ids;
  }
  bool ReadText(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    return Read(m_texts, id, out, error);
  }

  // The paks of one world directory ("Intro_Master") as the room writer takes
  // them, and every pak of the image for the assets rooms share.
  void World(const std::string& dir, RoomPak& master, std::vector<RoomPak>& rooms) const {
    const std::string folder = "/!" + dir + "/";
    for (size_t i = 0; i < m_paks.size(); ++i) {
      const std::string& path = m_paths[i];
      const size_t at = path.find(folder);
      if (at == std::string::npos) {
        continue;
      }
      const std::string name = path.substr(at + folder.size(), path.size() - at - folder.size() - 4);
      if (name == "!" + dir) {
        master = RoomPak{dir, m_paks[i].get()};
      } else if (name.find('/') == std::string::npos) {
        rooms.push_back(RoomPak{name, m_paks[i].get()});
      }
    }
  }
  std::vector<RoomPak> AllPaks() const {
    std::vector<RoomPak> all;
    for (size_t i = 0; i < m_paks.size(); ++i) {
      all.push_back(RoomPak{m_paths[i], m_paks[i].get()});
    }
    return all;
  }

private:
  struct Where {
    size_t pak;
    size_t asset;
  };
  using Index = std::unordered_map<std::array<uint8_t, 16>, Where, PakIdHash>;

  static std::string FrameKey(const std::string& name) {
    const size_t slash = name.find_last_of("/\\");
    std::string key = name.substr(slash == std::string::npos ? 0 : slash + 1);
    key = key.substr(0, key.find('.'));
    for (char& c : key) {
      c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return key;
  }

  bool Read(const Index& index, const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = index.find(id);
    if (found == index.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  Nsp m_nsp;
  // Nsp::Read is for one thread at a time.
  mutable std::mutex m_mutex;
  std::vector<std::unique_ptr<Pak>> m_paks;
  std::vector<std::string> m_paths;  // of m_paks, in the image
  Index m_models;
  Index m_textures;
  Index m_texts;
  Index m_fonts;
  std::unordered_map<std::string, Where> m_frames;  // GUIF, by FrameKey
  std::unordered_map<std::string, Where> m_textureNames;  // the named TXTR, by FrameKey
  std::unordered_map<std::string, Where> m_modelNames;    // the named CMDL, by FrameKey
  std::unordered_map<std::string, Where> m_maps;  // CMAP, by FrameKey
  std::unordered_map<std::string, Where> m_movies;  // FMV0, by IdToString
};

// --- The import ------------------------------------------------------------------

fs::path StagingFolder() {
  const std::string mods = PortMods::Folder();
  return mods.empty() ? fs::path() : PathFromString(mods) / kStagingName;
}

ConvertOptions OptionsFor(const TableEntry& entry) {
  ConvertOptions options;
  options.retail = entry.retail;
  for (int i = 0; i < 9; ++i) {
    options.orient[i / 3][i % 3] = entry.orient[i];
  }
  for (int i = 0; i < 3; ++i) {
    options.offset[i] = entry.offset[i];
  }
  const uint32_t* skins = TableSkins(entry);
  options.skins.assign(skins, skins + entry.skinCount);
  options.pbr = entry.pbr;
  if (const TableOptions* extra = TableExtra(entry)) {
    options.material = extra->material;
    options.maxTexture = extra->maxTexture;
    if (extra->squeezeRole != nullptr) {
      options.squeeze = true;
      options.squeezeRole = extra->squeezeRole;
      options.squeezeFrom[0] = extra->squeeze[0];
      options.squeezeFrom[1] = extra->squeeze[1];
      options.squeezeTo[0] = extra->squeeze[2];
      options.squeezeTo[1] = extra->squeeze[3];
    }
  }
  return options;
}

// Remastered's menu movies, written into `folder` under the disc's names
// (port_remastered_movie.h). Returns how many of the disc's movies were replaced.
int ImportMovies(const Remastered& remastered, const fs::path& folder, const MovieFormat& format, bool& noFfmpeg) {
  SetMessage("Looking for ffmpeg");
  const std::string ffmpeg = FindFfmpeg();
  noFfmpeg = ffmpeg.empty();
  if (noFfmpeg) {
    AddLine("movies skipped: ffmpeg not found. Install ffmpeg (or put it next to the game), then use \"Import "
            "movies\".");
    return 0;
  }
  std::error_code ec;
  fs::create_directories(folder, ec);
  const auto text = [](const fs::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
  };
  // ffmpeg reads the MP4 from a file: it has to seek in it.
  const fs::path source = folder / "import.tmp.mp4";
  const std::vector<Movie>& movies = Movies();
  int written = 0;
  for (size_t i = 0; i < movies.size() && !sCancel; ++i) {
    const Movie& movie = movies[i];
    SetMessage("Converting the movies (" + std::to_string(i + 1) + "/" + std::to_string(movies.size()) + ")");
    std::vector<uint8_t> raw;
    std::string error;
    size_t offset = 0;
    size_t length = 0;
    int frames = 0;
    const fs::path first = folder / (std::string(movie.names[0]) + ".thp");
    const fs::path tmp = folder / "import.tmp.thp";
    bool ok = remastered.ReadMovie(movie.id, raw, error) &&
              (MovieStream(raw.data(), raw.size(), offset, length) || (error = "not a movie", false));
    if (ok) {
      std::ofstream file(source, std::ios::binary | std::ios::trunc);
      file.write(reinterpret_cast<const char*>(raw.data() + offset), std::streamsize(length));
      file.close();
      ok = bool(file) || (error = "cannot write to the mod folder", false);
    }
    raw = {};
    ok = ok && ConvertMovie(ffmpeg, text(source), text(tmp), format, [] { return sCancel.load(); }, frames, error);
    if (ok) {
      // Written beside it and renamed, so a movie cut short never has the name.
      fs::rename(tmp, first, ec);
      ok = !ec || (error = "cannot replace the movie: " + ec.message(), false);
    }
    if (!ok) {
      fs::remove(tmp, ec);
      if (!sCancel) {
        AddLine(std::string(movie.names[0]) + ".thp: " + error);
      }
      continue;
    }
    ++written;
    // The disc's other takes of a transition are the same file again.
    for (size_t n = 1; n < movie.names.size(); ++n) {
      const fs::path other = folder / (std::string(movie.names[n]) + ".thp");
      fs::remove(other, ec);
      fs::create_hard_link(first, other, ec);
      if (ec) {
        fs::copy_file(first, other, fs::copy_options::overwrite_existing, ec);
      }
      if (ec) {
        AddLine(std::string(movie.names[n]) + ".thp: " + ec.message());
      } else {
        ++written;
      }
    }
  }
  fs::remove(source, ec);
  if (written == 0) {
    fs::remove(folder, ec); // only if nothing is in it
  }
  return written;
}

// Only the movies, into the mod an earlier import made: for a player who had
// no ffmpeg at the time.
void RunMovies(std::string nspPath, std::string keysPath, fs::path mod) {
  YieldToGame();
  SetMessage("Opening the image");
  std::string error;
  Remastered remastered;
  if (!remastered.Open(nspPath, keysPath, error)) {
    Finish(false, sCancel ? std::string("Cancelled.") : error);
    return;
  }
  MovieFormat format;
  WantsMovies(format);
  bool noFfmpeg = false;
  const int movies = ImportMovies(remastered, mod / kMovieFolder, format, noFfmpeg);
  if (sCancel) {
    Finish(false, "Cancelled.");
  } else if (noFfmpeg) {
    Finish(false, "ffmpeg not found. Install it, or put it next to the game.");
  } else if (movies == 0) {
    Finish(false, "No movies converted.");
  } else {
    Finish(true, std::to_string(movies) + " movies converted.");
  }
}

void Run(std::string nspPath, std::string keysPath, int threads, fs::path staging) {
  YieldToGame();
  std::error_code ec;
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  if (ec) {
    Finish(false, "Cannot create the mod folder: " + ec.message());
    return;
  }
  auto fail = [&](const std::string& message) {
    fs::remove_all(staging, ec);
    Finish(false, sCancel ? std::string("Cancelled.") : message);
  };

  SetMessage("Opening the image");
  std::string error;
  Remastered remastered;
  if (!remastered.Open(nspPath, keysPath, error)) {
    fail(error);
    return;
  }
  SetMessage("Reading the disc");
  Retail retail;
  if (!retail.Index(error)) {
    fail(error);
    return;
  }

  size_t count = 0;
  const TableEntry* table = Table(count);
  {
    std::lock_guard<std::mutex> lock(sStateMutex);
    sState.total = int(count);
  }
  std::atomic<size_t> next{0};
  std::atomic<int> converted{0};
  // Ids the import has given out: no two resources of it share one, whatever their types.
  std::mutex takenMutex;
  std::unordered_set<uint32_t> taken;
  auto makeIO = [&](int worker, const fs::path& folder) {
    ConvertIO io;
    io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) { return retail.Read(type, id, out); };
    io.retailId = [&](uint32_t id) { return retail.HasId(id); };
    io.texture = [&](const ModelUuid& id, Image& out, std::string& textureError) {
      std::vector<uint8_t> raw;
      TxtrImage image;
      if (!remastered.ReadTexture(id, raw, textureError) ||
          !DecodeTxtr(raw.data(), raw.size(), image, textureError)) {
        return false;
      }
      out.width = int(image.width);
      out.height = int(image.height);
      out.rgba = std::move(image.rgba);
      return true;
    };
    // Workers can meet the same texture at once; each writes its own temporary
    // file and the rename decides, the content being the same either way.
    io.write = [&, worker, folder](const std::string& name, const std::vector<uint8_t>& data) {
      {
        std::lock_guard<std::mutex> lock(takenMutex);
        taken.insert(uint32_t(std::strtoul(name.substr(0, 8).c_str(), nullptr, 16)));
      }
      const fs::path path = folder / PathFromString(name);
      const fs::path tmp = folder / PathFromString(name + ".tmp" + std::to_string(worker));
      {
        std::ofstream file(tmp, std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!file) {
          return false;
        }
      }
      std::error_code renameError;
      fs::rename(tmp, path, renameError);
      return !renameError;
    };
    return io;
  };
  // Second looks of a retail model (TableEntry::ancs, key) are written under
  // ids of their own, given out here before anything is written so that they
  // come out the same in every import.
  struct Look {
    bool ok = true;
    std::string error;
    uint32_t model = 0;  // the CMDL's id, 0 for the retail one
    std::vector<uint32_t> skins;  // the CSKRs' ids, empty for the retail ones
    uint32_t ancs = 0;            // the ANCS copy's id, 0 for none
    std::vector<uint8_t> ancsData;
  };
  std::vector<Look> looks(count);
  {
    auto hex = [](uint32_t id) {
      char name[16];
      std::snprintf(name, sizeof(name), "%08X", id);
      return std::string(name);
    };
    auto variant = [&](Look& look, uint32_t id, int key) {
      const uint32_t out = PortModelVariant::Id(id, key);
      if (retail.HasId(out) || taken.count(out) != 0) {
        look.ok = false;
        look.error = "the id for look " + std::to_string(key) + " of " + hex(id) + " is taken";
      }
      taken.insert(out);
      return out;
    };
    for (size_t i = 0; i < count; ++i) {
      const TableEntry& entry = table[i];
      Look& look = looks[i];
      if (entry.ancs == 0 && entry.key >= 0) {
        look.model = variant(look, entry.retail, entry.key);
      } else if (entry.ancs != 0) {
        look.ancs = entry.key >= 0 ? variant(look, entry.ancs, entry.key) : entry.ancs;
        if (entry.skinCount != 1) {
          look.ok = false;
          look.error = "a character's look needs exactly one skin";
        }
      }
    }
    // The new models and skins of the looks: hashed, then moved past every id
    // the disc or this import has.
    auto fresh = [&](uint32_t seed) {
      uint32_t id = 0x811C9DC5u;  // FNV-1a
      for (int i = 0; i < 4; ++i) {
        id = (id ^ ((seed >> (i * 8)) & 0xFFu)) * 0x01000193u;
      }
      while (id == 0 || id == 0xFFFFFFFFu || retail.HasId(id) || taken.count(id) != 0) {
        ++id;
      }
      taken.insert(id);
      return id;
    };
    for (size_t i = 0; i < count; ++i) {
      const TableEntry& entry = table[i];
      Look& look = looks[i];
      if (entry.ancs != 0 || entry.key < 0 || !look.ok) {
        continue;
      }
      // A static look of a skinned model (the low-poly and glass balls) is
      // drawn without its skin, but the converter still writes one; it must
      // not land on the retail model's.
      const uint32_t* skins = TableSkins(entry);
      for (int s = 0; s < entry.skinCount; ++s) {
        look.skins.push_back(fresh(look.model ^ skins[s] * 0x9E3779B1u));
      }
    }
    for (size_t i = 0; i < count; ++i) {
      const TableEntry& entry = table[i];
      Look& look = looks[i];
      if (entry.ancs == 0 || !look.ok) {
        continue;
      }
      // The copy binds the new pair where the retail one binds the old: one
      // character's model and skin ids, big-endian and side by side.
      const uint32_t skin = TableSkins(entry)[0];
      if (!retail.Read(kANCS, entry.ancs, look.ancsData)) {
        look.ok = false;
        look.error = "character " + hex(entry.ancs) + " is not on the disc";
        continue;
      }
      const uint8_t pair[8] = {uint8_t(entry.retail >> 24), uint8_t(entry.retail >> 16), uint8_t(entry.retail >> 8),
                               uint8_t(entry.retail),       uint8_t(skin >> 24),         uint8_t(skin >> 16),
                               uint8_t(skin >> 8),          uint8_t(skin)};
      std::vector<uint8_t>& data = look.ancsData;
      size_t at = data.size();
      int found = 0;
      for (auto it = std::search(data.begin(), data.end(), pair, pair + 8); it != data.end();
           it = std::search(it + 1, data.end(), pair, pair + 8)) {
        at = size_t(it - data.begin());
        ++found;
      }
      if (found != 1) {
        look.ok = false;
        look.error = "character " + hex(entry.ancs) + " binds " + hex(entry.retail) + " " + std::to_string(found) +
                     " times";
        continue;
      }
      const uint32_t seed = entry.ancs * 0x9E3779B1u ^ entry.retail ^ uint32_t(entry.key + 1) * 0x85EBCA6Bu;
      look.model = fresh(seed);
      look.skins = {fresh(seed ^ 0x534B494Eu)};
      for (int b = 0; b < 4; ++b) {
        data[at + b] = uint8_t(look.model >> (24 - b * 8));
        data[at + 4 + b] = uint8_t(look.skins[0] >> (24 - b * 8));
      }
    }
  }
  auto work = [&](int worker) {
    YieldToGame();
    ConvertIO io = makeIO(worker, staging);
    const auto write = io.write;
    Converter converter(std::move(io));
    for (size_t i = next++; i < count && !sCancel; i = next++) {
      const TableEntry& entry = table[i];
      const Look& look = looks[i];
      ModelUuid id;
      std::memcpy(id.data(), entry.rem, 16);
      std::string modelError = look.error;
      std::vector<uint8_t> raw;
      Model model;
      ConvertOptions options = OptionsFor(entry);
      options.outputModel = look.model;
      options.outputSkins = look.skins;
      bool ok = look.ok && remastered.ReadModel(id, raw, modelError) &&
                ParseModel(raw.data(), raw.size(), model, modelError) &&
                converter.Convert(model, options, modelError);
      // The character copy last, so that it never names a model that failed.
      if (ok && look.ancs != 0) {
        char name[16];
        std::snprintf(name, sizeof(name), "%08X", look.ancs);
        ok = write(std::string(name) + ".ANCS", look.ancsData);
        if (!ok) {
          modelError = "could not write " + std::string(name) + ".ANCS";
        }
      }
      if (ok) {
        ++converted;
      } else {
        char name[16];
        std::snprintf(name, sizeof(name), "%08X", entry.retail);
        AddLine(std::string(name) + ": " + modelError);
      }
      std::lock_guard<std::mutex> lock(sStateMutex);
      ++sState.done;
      if (!ok) {
        ++sState.failed;
      }
      sState.message = "Converting models (" + std::to_string(sState.done) + "/" + std::to_string(sState.total) + ")";
    }
  };
  SetMessage("Converting models");
  std::vector<std::thread> workers;
  for (int i = 1; i < threads; ++i) {
    workers.emplace_back(work, i);
  }
  work(0);
  for (std::thread& worker : workers) {
    worker.join();
  }

  if (sCancel) {
    fail("Cancelled.");
    return;
  }
  if (converted == 0) {
    fail("No model could be converted.");
    return;
  }

  // The rooms' reflection cubes and baked ambient light, a file per area. A
  // world that cannot be read costs its rooms their environment, not the import.
  SetMessage("Writing the room environments");
  const fs::path roomFolder = staging / kRoomFolder;
  fs::create_directories(roomFolder, ec);
  fs::create_directories(staging / kGeometryFolder, ec);
  const std::vector<RoomPak> allPaks = remastered.AllPaks();
  const std::vector<RoomWorld>& worlds = RoomWorlds();
  std::atomic<size_t> nextWorld{0};
  std::atomic<int> roomFiles{0};
  // A room's geometry names its models; they are given ids here and converted
  // afterwards, on every thread. A model that then fails leaves its id naming
  // nothing, which the port skips.
  const fs::path geometryFolder = staging / kGeometryFolder;
  struct GeometryModel {
    ModelUuid uuid;
    uint32_t id;
    int liquid = -1;  // index into `liquids` when it is a liquid's surface
  };
  std::vector<RoomLiquid> liquids;
  std::vector<GeometryModel> geometry;
  std::unordered_map<ModelUuid, uint32_t, PakIdHash> geometryIds;
  auto geometryId = [&](const ModelUuid& uuid, uint32_t& id) {
    std::lock_guard<std::mutex> lock(takenMutex);
    const auto known = geometryIds.find(uuid);
    if (known != geometryIds.end()) {
      id = known->second;
      return true;
    }
    id = 0x811C9DC5u;  // FNV-1a
    for (const uint8_t byte : uuid) {
      id = (id ^ byte) * 0x01000193u;
    }
    while (id == 0 || id == 0xFFFFFFFFu || retail.HasId(id) || taken.count(id) != 0) {
      ++id;
    }
    taken.insert(id);
    geometryIds.emplace(uuid, id);
    geometry.push_back({uuid, id});
    return true;
  };
  // A liquid's surface is converted with what its room says of it, so it is a model of its
  // own even where two rooms share the sheet.
  auto liquidId = [&](const RoomLiquid& liquid, uint32_t& id) {
    std::lock_guard<std::mutex> lock(takenMutex);
    id = 0x811C9DC5u ^ uint32_t(liquids.size() + 1) * 0x9E3779B1u;
    for (const uint8_t byte : liquid.model) {
      id = (id ^ byte) * 0x01000193u;
    }
    while (id == 0 || id == 0xFFFFFFFFu || retail.HasId(id) || taken.count(id) != 0) {
      ++id;
    }
    taken.insert(id);
    geometry.push_back({liquid.model, id, int(liquids.size())});
    liquids.push_back(liquid);
    return true;
  };
  auto roomWork = [&] {
    YieldToGame();
    for (size_t i = nextWorld++; i < worlds.size() && !sCancel; i = nextWorld++) {
      RoomPak master;
      std::vector<RoomPak> rooms;
      remastered.World(worlds[i].dir, master, rooms);
      RoomIO io;
      io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) { return retail.Read(type, id, out); };
      io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
        const bool isGeometry = (name.size() > 8 && name.compare(name.size() - 8, 8, ".roomgeo") == 0) ||
                                (name.size() > 11 && name.compare(name.size() - 11, 11, ".roomliquid") == 0);
        std::ofstream file((isGeometry ? geometryFolder : roomFolder) / PathFromString(name), std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        return bool(file);
      };
      io.model = geometryId;
      io.liquid = liquidId;
      io.wantsGeometry = WantsGeometry;
      io.cancelled = [] { return sCancel.load(); };
      int written = 0;
      std::string worldError;
      if (!WriteWorldRoomEnvs(worlds[i].mlvl, master, rooms, allPaks, io, written, worldError) && !sCancel) {
        AddLine(std::string(worlds[i].dir) + ": " + worldError);
      }
      roomFiles += written;
    }
  };
  workers.clear();
  for (int i = 1; i < threads; ++i) {
    workers.emplace_back(roomWork);
  }
  roomWork();
  for (std::thread& worker : workers) {
    worker.join();
  }
  if (sCancel) {
    fail("Cancelled.");
    return;
  }

  std::atomic<int> geometryDone{0};
  if (!geometry.empty()) {
    std::unordered_set<uint32_t> modelIds;
    for (const GeometryModel& g : geometry) {
      modelIds.insert(g.id);
    }
    std::atomic<size_t> nextModel{0};
    std::atomic<int> seen{0};
    std::mutex claimMutex;
    std::unordered_set<uint32_t> claimed;
    auto geometryWork = [&](int worker) {
      YieldToGame();
      ConvertIO io = makeIO(worker, geometryFolder);
      // A texture never takes a geometry model's id either.
      io.retailId = [&](uint32_t id) { return retail.HasId(id) || modelIds.count(id) != 0; };
      io.claim = [&](uint32_t id) {
        std::lock_guard<std::mutex> lock(claimMutex);
        return claimed.insert(id).second;
      };
      Converter converter(std::move(io));
      for (size_t i = nextModel++; i < geometry.size() && !sCancel; i = nextModel++) {
        ConvertOptions options;
        options.retail = geometry[i].id;
        options.standalone = true;
        // The list drops a character's simplified meshes by name; a room has none, and its
        // stone is named "simple".
        options.skip.clear();
        options.nativeMax = kGeometryTexture;
        if (geometry[i].liquid >= 0 && liquids[size_t(geometry[i].liquid)].type != RoomLiquid::kLava) {
          const RoomLiquid& liquid = liquids[size_t(geometry[i].liquid)];
          options.water = true;
          options.waterHasNormal = liquid.hasNormal;
          options.waterNormal = liquid.normal;
          for (int k = 0; k < 4; ++k) {
            options.waterTint[k] = liquid.tint[k];
          }
          // Each wave layer drifts the way it faces; Remastered's speeds are not read.
          for (int k = 0; k < 2; ++k) {
            const double angle = double(liquid.waveAngle[k]) * (3.14159265358979323846 / 180.0);
            options.waterScale[k] = liquid.normalScale[k];
            options.waterFlow[k * 2] = kLiquidDrift * std::cos(angle);
            options.waterFlow[k * 2 + 1] = kLiquidDrift * std::sin(angle);
          }
        }
        std::string modelError;
        std::vector<uint8_t> raw;
        Model model;
        if (remastered.ReadModel(geometry[i].uuid, raw, modelError) &&
            ParseModel(raw.data(), raw.size(), model, modelError) && converter.Convert(model, options, modelError)) {
          ++geometryDone;
        } else {
          char name[16];
          std::snprintf(name, sizeof(name), "%08X", geometry[i].id);
          AddLine(std::string("room model ") + name + ": " + modelError);
        }
        SetMessage("Converting room models (" + std::to_string(++seen) + "/" + std::to_string(geometry.size()) + ")");
      }
    };
    workers.clear();
    for (int i = 1; i < threads; ++i) {
      workers.emplace_back(geometryWork, i);
    }
    geometryWork(0);
    for (std::thread& worker : workers) {
      worker.join();
    }
    if (sCancel) {
      fail("Cancelled.");
      return;
    }
  }
  if (geometry.empty()) {
    fs::remove(geometryFolder, ec);
  }

  // The strings Remastered reworded, as the disc's tables with those strings changed.
  int textTables = 0;
  int textStrings = 0;
  if (WantsText()) {
    SetMessage("Writing the text");
    std::map<uint32_t, std::map<uint32_t, std::u16string>> tables;
    for (const ModelUuid& id : remastered.Texts()) {
      std::vector<uint8_t> raw;
      std::vector<TextEntry> entries;
      std::string textError;
      if (!remastered.ReadText(id, raw, textError) ||
          !ParseMsbt(raw.data(), raw.size(), "USEN", entries, textError)) {
        AddLine("text " + IdToString(id) + ": " + textError);
        continue;
      }
      for (TextEntry& entry : entries) {
        uint32_t strg = 0;
        uint32_t index = 0;
        if (SplitTextLabel(entry.label, strg, index)) {
          tables[strg][index] = std::move(entry.text);
        }
      }
    }
    const fs::path textFolder = staging / kTextFolder;
    fs::create_directories(textFolder, ec);
    for (const auto& [strg, strings] : tables) {
      std::vector<uint8_t> original;
      std::vector<uint8_t> merged;
      int changed = 0;
      if (!retail.Read(kSTRG, strg, original) ||
          !MergeStringTable(original.data(), original.size(), strings, merged, changed)) {
        continue;  // not on this disc, or worded as it was
      }
      char name[16];
      std::snprintf(name, sizeof(name), "%08X.STRG", strg);
      std::ofstream file(textFolder / name, std::ios::binary);
      file.write(reinterpret_cast<const char*>(merged.data()), std::streamsize(merged.size()));
      if (!file) {
        AddLine(std::string(name) + ": cannot write");
        continue;
      }
      ++textTables;
      textStrings += changed;
    }
    if (textTables == 0) {
      fs::remove(textFolder, ec);
    }
  }
  // Remastered's typeface, which the port draws the disc's text with.
  bool fontWritten = false;
  {
    std::vector<uint8_t> raw;
    std::vector<uint8_t> out;
    ModelUuid atlas{};
    PortHdFont::Font font;
    TxtrImage image;
    std::string fontError = "not in the image";
    // The one with the most characters, so that every language's text is covered.
    for (const ModelUuid& id : remastered.Fonts()) {
      ModelUuid candidateAtlas{};
      PortHdFont::Font candidate;
      std::string candidateError;
      if (!remastered.ReadFont(id, raw, candidateError) ||
          !ParseFont(raw.data(), raw.size(), candidateAtlas, candidate, candidateError)) {
        fontError = candidateError;
      } else if (candidate.glyphs.size() > font.glyphs.size()) {
        font = std::move(candidate);
        atlas = candidateAtlas;
      }
    }
    if (font.glyphs.empty() || !remastered.ReadTexture(atlas, raw, fontError) ||
        !DecodeTxtr(raw.data(), raw.size(), image, fontError)) {
      AddLine("font: " + fontError);
    } else if (!SetFontAtlas(font, image.width, image.height, image.rgba.data(), image.rgba.size()) ||
               !PortHdFont::WriteFont(font, out)) {
      AddLine("font: its texture is not usable");
    } else {
      const fs::path fontFolder = staging / kFontFolder;
      fs::create_directories(fontFolder, ec);
      std::ofstream file(fontFolder / kFontName, std::ios::binary);
      file.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
      fontWritten = bool(file);
      if (!fontWritten) {
        AddLine("font: cannot write");
      }
    }
  }
  // Remastered's HUD: the disc's frames laid out and drawn as its own.
  int hudFrames = 0;
  if (WantsHud() && !sCancel) {
    SetMessage("Converting the HUD");
    const fs::path hudFolder = staging / kHudFolder;
    fs::create_directories(hudFolder, ec);
    HudConverter converter(makeIO(0, hudFolder));
    HudCounts counts;
    for (const HudFrame& frame : HudFrames()) {
      std::vector<uint8_t> raw;
      std::vector<uint8_t> rawModel;
      ModelUuid modelId{};
      Model model;
      std::string hudError;
      if (!remastered.ReadFrame(frame.name, raw, hudError) ||
          !(HudFrameModel(raw.data(), raw.size(), modelId) || (hudError = "not a frame", false)) ||
          !remastered.ReadModel(modelId, rawModel, hudError) ||
          !ParseModel(rawModel.data(), rawModel.size(), model, hudError) ||
          !converter.Convert(frame.retail, raw.data(), raw.size(), model, counts, hudError)) {
        AddLine(std::string(frame.name) + ": " + hudError);
        continue;
      }
      ++hudFrames;
    }
    // The map screen's compass, which the game draws itself (port_map_icons.h).
    int compassModels = 0;
    for (const auto& [name, id] : {std::pair<const char*, uint32_t>{"CMDL_MapCompassShell", PortMapIcons::kCompassShell},
                                   std::pair<const char*, uint32_t>{"CMDL_MapCompass", PortMapIcons::kCompassNeedle}}) {
      std::vector<uint8_t> raw;
      Model model;
      std::string compassError;
      if (!remastered.ReadModelNamed(name, raw, compassError) ||
          !ParseModel(raw.data(), raw.size(), model, compassError) ||
          !converter.ConvertModel(model, id, counts, compassError)) {
        AddLine(std::string(name) + ": " + compassError);
        continue;
      }
      ++compassModels;
    }
    if (hudFrames == 0 && compassModels == 0) {
      fs::remove_all(hudFolder, ec);
    }
  }
  // Remastered's map icons, where the game looks for the disc's, and the rooms
  // whose map it reshaped.
  if (WantsHud() && !sCancel) {
    const fs::path mapFolder = staging / kMapFolder;
    fs::create_directories(mapFolder, ec);
    ConvertIO io = makeIO(0, mapFolder);
    int icons = 0;
    for (const MapIcon& icon : MapIcons()) {
      std::vector<uint8_t> raw;
      TxtrImage decoded;
      std::string iconError;
      if (!remastered.ReadTextureNamed(icon.name, raw, iconError) ||
          !DecodeTxtr(raw.data(), raw.size(), decoded, iconError)) {
        AddLine(std::string(icon.name) + ": " + iconError);
        continue;
      }
      Image image;
      image.width = int(decoded.width);
      image.height = int(decoded.height);
      image.rgba = std::move(decoded.rgba);
      char name[32];
      std::snprintf(name, sizeof(name), "%08X.TXTR", icon.id);
      const std::vector<uint8_t> txtr = EncodeMapIcon(image);
      if (txtr.empty() || !io.write(name, txtr)) {
        AddLine(std::string(icon.name) + ": cannot write");
        continue;
      }
      ++icons;
    }
    MapIO mapIO;
    mapIO.retail = io.retail;
    mapIO.write = io.write;
    mapIO.log = [](const std::string& line) { AddLine(line); };
    for (const MapWorld& world : MapWorlds()) {
      if (sCancel) {
        break;
      }
      std::vector<uint8_t> raw;
      std::string mapError;
      if (!remastered.ReadMap(world.name, raw, mapError) ||
          !WriteWorldMapAreas(world.mlvl, raw.data(), raw.size(), mapIO, icons, mapError)) {
        AddLine(std::string(world.name) + ": " + mapError);
      }
    }
    if (icons == 0) {
      fs::remove_all(mapFolder, ec);
    }
  }
  // Remastered's menu movies.
  int movies = 0;
  bool noFfmpeg = false;
  if (MovieFormat format; WantsMovies(format) && !sCancel) {
    movies = ImportMovies(remastered, staging / kMovieFolder, format, noFfmpeg);
  }
  if (sCancel) {
    fail("Cancelled.");
    return;
  }
  {
    std::ofstream marker(staging / kMarkerName);
    if (!marker) {
      fail("Cannot write to the mod folder.");
      return;
    }
  }
  const int failed = int(count) - converted.load();
  std::string message = std::to_string(converted.load()) + " models converted";
  if (failed != 0) {
    message += ", " + std::to_string(failed) + " failed";
  }
  message += ", " + std::to_string(roomFiles.load()) + " room environments";
  if (!geometry.empty()) {
    message += ", " + std::to_string(geometryDone.load()) + " of " + std::to_string(geometry.size()) + " room models";
  }
  if (textTables != 0) {
    message += ", " + std::to_string(textStrings) + " strings in " + std::to_string(textTables) + " text tables";
  }
  if (fontWritten) {
    message += ", the font";
  }
  if (hudFrames != 0) {
    message += ", " + std::to_string(hudFrames) + " HUD frames";
  }
  if (movies != 0) {
    message += ", " + std::to_string(movies) + " movies";
  }
  Finish(true, message + (noFfmpeg ? ". Movies skipped: ffmpeg not found." : "."));
}

}  // namespace

std::string DefaultKeysPath() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') {
    home = std::getenv("USERPROFILE");
  }
  if (home == nullptr || home[0] == '\0') {
    return {};
  }
  const fs::path path = PathFromString(home) / ".switch" / "prod.keys";
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return {};
  }
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

bool StartMovieImport(const std::string& nspPath, const std::string& keysPath) {
  {
    std::lock_guard<std::mutex> lock(sStateMutex);
    if (sState.running) {
      return false;
    }
  }
  if (sThread.joinable()) {
    sThread.join();
  }
  // A finished import that has not been moved into place yet is the newer mod.
  const fs::path staging = StagingFolder();
  std::error_code ec;
  fs::path mod;
  if (!staging.empty()) {
    mod = fs::exists(staging / kMarkerName, ec) ? staging : staging.parent_path() / kImportModName;
  }
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState = {};
  if (mod.empty() || !fs::is_directory(mod, ec)) {
    sState.finished = true;
    sState.message = "Import the models first: the movies go into that mod.";
    return false;
  }
  sCancel = false;
  sState.running = true;
  sState.message = "Starting";
  sThread = std::thread(RunMovies, nspPath, keysPath, mod);
  return true;
}

bool StartImport(const std::string& nspPath, const std::string& keysPath, int threads) {
  {
    std::lock_guard<std::mutex> lock(sStateMutex);
    if (sState.running) {
      return false;
    }
  }
  if (sThread.joinable()) {
    sThread.join();
  }
  const fs::path staging = StagingFolder();
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState = {};
  if (staging.empty()) {
    sState.finished = true;
    sState.message = "There is no mods folder to write to.";
    return false;
  }
  if (threads <= 0) {
    threads = std::max(1, int(std::thread::hardware_concurrency()) - 2);
  }
  sCancel = false;
  sState.running = true;
  sState.message = "Starting";
  sThread = std::thread(Run, nspPath, keysPath, threads, staging);
  return true;
}

void SetImportGeometry(bool on) { sGeometry = on; }

ImportState ImportStatus() {
  std::lock_guard<std::mutex> lock(sStateMutex);
  return sState;
}

void CancelImport() { sCancel = true; }

void StopImport() {
  sCancel = true;
  if (sThread.joinable()) {
    sThread.join();
  }
}

bool ApplyPendingImport() {
  const fs::path staging = StagingFolder();
  std::error_code ec;
  // A running import is still writing there.
  if (staging.empty() || ImportStatus().running || !fs::is_directory(staging, ec)) {
    return false;
  }
  if (!fs::exists(staging / kMarkerName, ec)) {
    fs::remove_all(staging, ec);
    return false;
  }
  const fs::path target = staging.parent_path() / kImportModName;
  fs::remove_all(target, ec);
  fs::rename(staging, target, ec);
  if (ec) {
    std::fprintf(stderr, "metroid_prime_port: could not move the imported mod to %s: %s\n",
                 target.string().c_str(), ec.message().c_str());
    return false;
  }
  fs::remove(target / kMarkerName, ec);
  return true;
}

int RunImportFromCommandLine(const std::string& nspPath, const std::string& keysPath, bool moviesOnly) {
  std::string keys = keysPath.empty() ? DefaultKeysPath() : keysPath;
  if (keys.empty()) {
    std::fprintf(stderr, "no key file given and no ~/.switch/prod.keys\n");
    return 2;
  }
  if (moviesOnly ? !StartMovieImport(nspPath, keys)
                 : !StartImport(nspPath, keys, int(std::max(1u, std::thread::hardware_concurrency())))) {
    std::fprintf(stderr, "%s\n", ImportStatus().message.c_str());
    return 1;
  }
  std::string shown;
  for (;;) {
    const ImportState state = ImportStatus();
    if (state.message != shown) {
      shown = state.message;
      std::printf("%s\n", shown.c_str());
      std::fflush(stdout);
    }
    if (!state.running) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  StopImport();
  const ImportState state = ImportStatus();
  for (const std::string& line : state.lines) {
    std::printf("  %s\n", line.c_str());
  }
  if (!state.ok) {
    return 1;
  }
  if (moviesOnly) {
    return 0;
  }
  if (!ApplyPendingImport()) {
    std::fprintf(stderr, "could not move the mod into %s\n", PortMods::Folder().c_str());
    return 1;
  }
  std::printf("installed as %s/%s\n", PortMods::Folder().c_str(), kImportModName);
  return 0;
}

}  // namespace PortRemastered

#include "port_apclient.h"
#include "port_log.h"
#include "port_paths.h"

#include "port_ap_metroidprime.h"
#include "port_ap_protocol.h"
#include "port_custom_res.h"
#include "port_randomizer.h"
#include "port_skip_cutscenes.h"
#include "port_ws.h"

#include "MetroidPrime/CHealthInfo.hpp"
#include "MetroidPrime/CScriptLayerManager.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Player/CWorldState.hpp"
#include "MetroidPrime/HUD/CSamusHud.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"

#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <functional>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace PortAp {
namespace {

using Protocol::Config;
using Protocol::ItemGrant;
using Protocol::Session;

bool EnvEnabled(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

std::string UserDirectory() {
  const std::string& dir = PortPaths::UserFolder();
  return dir.empty() ? std::string(".") : dir;
}

std::string ConfigPath() {
  if (const char* env = std::getenv("MP_AP_CONFIG")) {
    if (env[0] != '\0')
      return env;
  }
  return (std::filesystem::path(UserDirectory()) / "archipelago.json").string();
}

// archipelago.json and the per-game directories live here.
std::filesystem::path ConfigDirectory() {
  std::filesystem::path parent = std::filesystem::path(ConfigPath()).parent_path();
  return parent.empty() ? std::filesystem::path(".") : parent;
}

// One slot's game in one seed: its save card, state file and game.json.
std::filesystem::path GameDirectory(const std::string& slot, const std::string& seed) {
  return ConfigDirectory() / "archipelago_games" / Protocol::GameDirectoryName(slot, seed);
}

std::string GameStatePath(const std::string& slot, const std::string& seed) {
  return (GameDirectory(slot, seed) / "archipelago_state.json").string();
}

// archipelago.json is written from the overlay (Connect, Disconnect) and by the
// socket thread (the seed), so the read-modify-writes take turns.
std::mutex& ConnectionFileMutex() {
  static std::mutex mutex;
  return mutex;
}

std::string ErrorText(const char* text) {
  return text != nullptr && text[0] != '\0' ? text : "connection failed";
}

rstl::wstring ToHudWide(const std::string& text) {
  std::wstring wide;
  for (size_t i = 0; i < text.size();) {
    const uint8_t first = static_cast<uint8_t>(text[i]);
    uint32_t codepoint = 0xfffd;
    size_t length = 1;
    if (first < 0x80) {
      codepoint = first;
    } else {
      size_t expected = 0;
      uint32_t minimum = 0;
      if (first >= 0xc2 && first <= 0xdf) {
        expected = 2;
        minimum = 0x80;
        codepoint = first & 0x1f;
      } else if (first >= 0xe0 && first <= 0xef) {
        expected = 3;
        minimum = 0x800;
        codepoint = first & 0x0f;
      } else if (first >= 0xf0 && first <= 0xf4) {
        expected = 4;
        minimum = 0x10000;
        codepoint = first & 0x07;
      }
      bool valid = expected != 0 && i + expected <= text.size();
      for (size_t offset = 1; valid && offset < expected; ++offset) {
        const uint8_t continuation = static_cast<uint8_t>(text[i + offset]);
        if ((continuation & 0xc0) != 0x80) {
          valid = false;
          break;
        }
        codepoint = (codepoint << 6) | (continuation & 0x3f);
      }
      valid = valid && codepoint >= minimum && codepoint <= 0x10ffff &&
              !(codepoint >= 0xd800 && codepoint <= 0xdfff);
      if (valid) {
        length = expected;
      } else {
        codepoint = 0xfffd;
      }
    }
    i += length;
    if constexpr (sizeof(wchar_t) >= 4) {
      wide.push_back(static_cast<wchar_t>(codepoint));
    } else if (codepoint <= 0xffff) {
      wide.push_back(static_cast<wchar_t>(codepoint));
    } else {
      codepoint -= 0x10000;
      wide.push_back(static_cast<wchar_t>(0xd800 + (codepoint >> 10)));
      wide.push_back(static_cast<wchar_t>(0xdc00 + (codepoint & 0x3ff)));
    }
  }
  return rstl::wstring(wide.c_str());
}

struct Runtime {
  // Called once at exit. A worker blocked in a DNS lookup or a connect cannot
  // be cut short, so it gets a moment to notice `stop` and is otherwise left
  // behind (the Runtime is never freed) rather than holding up the exit.
  void Shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      exiting = true; // a restart still in flight must not start a new worker
    }
    stop.store(true, std::memory_order_release);
    wake.notify_all();
    if (worker.joinable()) {
      bool done;
      {
        std::unique_lock<std::mutex> lock(mutex);
        done = wake.wait_for(lock, std::chrono::seconds(2), [this] { return workerDone; });
      }
      if (done)
        worker.join();
      else
        worker.detach();
    }
    FlushState(); // whatever the worker had not written yet
  }

  void LogStateLocked(const std::string& message) {
    if (lastLogged != message) {
      PortLog::Write( "archipelago: %s\n", message.c_str());
      lastLogged = message;
    }
  }

  void SetError(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex);
    connected = false;
    stateLabel = "error";
    lastError = error;
    LogStateLocked(error);
  }

  bool WaitBackoff(int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::unique_lock<std::mutex> lock(mutex);
    while (!stop.load(std::memory_order_acquire)) {
      if (!wake.wait_until(lock, deadline, [this] {
            return stop.load(std::memory_order_acquire) || stateDirty;
          }))
        break; // the backoff ran out
      if (stateDirty) {
        lock.unlock();
        FlushState(); // checks collected while offline are still recorded
        lock.lock();
      }
    }
    return !stop.load(std::memory_order_acquire);
  }

  // The state file is written by the socket thread (or at shutdown, once it
  // has stopped), never by the game thread: a pickup must not wait on disk.
  void MarkStateDirtyLocked() {
    stateDirty = true;
    wake.notify_all();
  }

  void AppendChatLocked(std::string type, std::string text) {
    constexpr size_t kChatLines = 500;
    if (chat.size() >= kChatLines)
      chat.pop_front();
    chat.push_back(ChatLine{std::move(type), std::move(text)});
    ++chatSerial;
  }

  // Writes the state file if it has changed. Only one thread calls this at a
  // time, so the snapshot taken under the lock is written in order.
  void FlushState() {
    std::lock_guard<std::mutex> flushing(flushMutex); // a restart and the exit may overlap
    Protocol::State snapshot;
    std::string path;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!stateDirty || session == nullptr)
        return;
      stateDirty = false;
      // No seed yet, so no game to write it for. The save records the checks,
      // and the state is written once the server names the seed.
      if (statePath.empty())
        return;
      snapshot = session->GetState();
      path = statePath;
    }
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    const bool written = Protocol::SaveStateFile(path, snapshot);
    if (!written && !stateWriteFailed) // said once, not on every pickup
      PortLog::Write("archipelago: could not write %s\n", path.c_str());
    stateWriteFailed = !written;
  }

  std::mutex mutex;
  std::condition_variable wake;
  std::atomic<bool> stop{false};
  std::thread worker;
  bool workerDone = false; // under mutex; set as the worker returns
  bool attempted = false;
  bool exiting = false; // under mutex; Shutdown() has begun
  std::mutex flushMutex;
  // Held for the whole of a restart (Connect/Disconnect), so two in a row run
  // one after the other.
  std::mutex restartMutex;
  // The last logic evaluation and what it was made from; it only changes with
  // an item or the seed's options, and the map asks every frame.
  bool logicValid = false;
  PortApLogic::Options logicOptions;
  bool logicHasWorld = false;
  PortApWorld::Layout logicWorld;
  PortApLogic::Items logicItems;
  std::vector<PortApLogic::Level> logicLevels;
  bool enabled = false;
  bool connected = false;
  std::string stateLabel = "off";
  std::string lastError;
  std::string lastLogged;
  std::string lastMessage;
  // The seed's own state file, in its game directory; empty until the seed is
  // known (remembered in archipelago.json, or named by the server).
  std::string statePath;
  // MP_AP_RESET_STATE=1 at launch, waiting for the seed's state to be loaded.
  bool resetPending = false;
  Config config;
  std::unique_ptr<Session> session;
  std::deque<ItemGrant> grants;
  std::deque<int64_t> queuedChecks;
  // DeathLink: a death noticed on the game thread, waiting for the socket
  // thread to announce it. Non-empty means one is owed.
  std::string pendingBounce;
  // A loaded game rewound the session, so the socket thread owes the server a
  // Sync to get the full inventory replayed.
  bool syncWanted = false;
  // The session state has changed since the state file was last written.
  bool stateDirty = false;
  bool stateWriteFailed = false; // only touched by the thread that flushes
  std::deque<std::string> notifications;
  // The overlay's chat log, kept across reconnects, and Say texts waiting for
  // the socket thread.
  std::deque<ChatLine> chat;
  uint64_t chatSerial = 0;
  std::deque<std::string> pendingSay;
  int itemCount = 0;
  int checkCount = 0;
  // Locations collected in game that the table has no id for. Counted and
  // reported once, because dropping them is otherwise invisible.
  int unmappedCount = 0;
  // DeathLink: whether this client's death has already been announced, so a
  // death is sent once rather than on every tick the flag stays clear for.
  bool deathAnnounced = false;
  // The game reached the end-of-game world: the goal is owed to the server
  // (`goalWanted`) until the socket thread has sent it once (`goalSent`).
  bool goalWanted = false;
  bool goalSent = false;
  // The checks the running game records have been handed to this session.
  bool recordSynced = false;
};

Runtime& GetRuntime() {
  // Leaked on purpose, so a worker Shutdown() leaves behind never touches
  // freed memory; the stopper runs Shutdown() with the other static
  // destructors.
  static Runtime* runtime = new Runtime;
  static struct Stopper {
    Runtime* runtime;
    ~Stopper() { runtime->Shutdown(); }
  } stopper{runtime};
  return *runtime;
}

// Names the session a save's received items came from: FNV-1a over the seed
// and slot, never 0, which marks a game no session has given items to.
uint32_t SessionIdentity(const std::string& seed, const std::string& slot) {
  uint32_t hash = 2166136261u;
  const auto mix = [&hash](const std::string& text) {
    for (const char c : text) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 16777619u;
    }
  };
  mix(seed);
  mix(std::string(1, '\0'));
  mix(slot);
  return hash != 0 ? hash : 1;
}

// A game some session has given items to. Its pickups at the built-in
// locations stay the multiworld's while the client is off.
bool IsApGame(const CGameState::ApProgress& progress) {
  return progress.recorded && progress.identity != 0;
}

// A built-in location's bit in the save's checked record, or -1.
int CheckedBit(const MetroidPrime::Location* location) {
  size_t count = 0;
  const MetroidPrime::Location* first = MetroidPrime::Locations(count);
  if (location == nullptr)
    return -1;
  const ptrdiff_t index = location - first;
  return index >= 0 && static_cast<size_t>(index) < count && index < 128 ? static_cast<int>(index)
                                                                          : -1;
}

void ClearChecked(CGameState::ApProgress& progress) {
  for (uint& word : progress.checked)
    word = 0;
  for (uint& word : progress.shields)
    word = 0;
}

// Hands the session the checks the game records but the session has not:
// ones made while the client was off, which go out on the next connection.
void QueueRecordedChecksLocked(Runtime& runtime, const CGameState::ApProgress& progress) {
  size_t count = 0;
  const MetroidPrime::Location* locations = MetroidPrime::Locations(count);
  int queued = 0;
  for (size_t i = 0; i < count && i < 128; ++i) {
    if (((progress.checked[i / 32] >> (i % 32)) & 1) == 0)
      continue;
    char key[32];
    PortRandomizer::FormatLocationKey(locations[i].world, locations[i].area, locations[i].pickup,
                                      key, sizeof(key));
    int64_t id = 0;
    if (runtime.session->MarkLocationChecked(key, id)) {
      runtime.queuedChecks.push_back(id);
      ++queued;
    }
  }
  if (queued == 0)
    return;
  runtime.MarkStateDirtyLocked();
  PortLog::Write("archipelago: the loaded game holds %d checks the server has not had; "
                 "sending them\n",
                 queued);
}

// Lines the session up with the loaded game. The state file's item index says
// what this client has received, but not what the game holds: quitting without
// saving, or loading an older save, drops items the index has moved past. So
// the save records how many it holds, and on load the session rewinds to that
// many and has the server replay the rest. The other way round, the save
// records the checks it made, some of which the session may not have seen.
void ReconcileLocked(Runtime& runtime, CGameState::ApProgress& progress) {
  const Protocol::State& state = runtime.session->GetState();
  if (state.seed.empty())
    return; // no session yet to compare with; nothing can have been granted
  const uint32_t identity = SessionIdentity(state.seed, runtime.config.slot);
  if (progress.reconciled) {
    // The server changed seeds under a running game. The session has already
    // dropped the old seed's progress, and none of the new one's is held yet.
    if (progress.identity != identity) {
      progress.identity = identity;
      progress.appliedIndex = 0;
      ClearChecked(progress);
    } else if (!runtime.recordSynced) {
      QueueRecordedChecksLocked(runtime, progress); // reconnected mid-game
    }
    runtime.recordSynced = true;
    return;
  }
  progress.reconciled = true;
  runtime.recordSynced = true;
  // Checks made in another seed or slot are not this one's. A new game's
  // (identity 0) were made under this session before its seed was known.
  if (progress.identity != 0 && progress.identity != identity)
    ClearChecked(progress);
  else
    QueueRecordedChecksLocked(runtime, progress);
  // Where the game's items end on the session's side: grants still queued
  // for the game are about to be applied, so they count as not held yet.
  const int64_t firstPending =
      runtime.grants.empty() ? state.nextItemIndex : runtime.grants.front().index;
  if (!progress.recorded) {
    // A save from before the record existed: take it to hold what the state
    // file says was received, which is what the client assumed until now.
    progress.recorded = true;
    progress.identity = identity;
    progress.appliedIndex = static_cast<uint>(
        std::clamp<int64_t>(firstPending, 0, std::numeric_limits<uint32_t>::max()));
    return;
  }
  // A new game, or a save from another seed or slot, holds none of these items.
  const int64_t held = progress.identity == identity ? progress.appliedIndex : 0;
  progress.identity = identity;
  progress.appliedIndex = static_cast<uint>(held);
  if (held == firstPending)
    return;
  PortLog::Write("archipelago: the loaded game holds %lld of %lld received items; "
                 "asking the server for the rest\n",
                 static_cast<long long>(held), static_cast<long long>(state.nextItemIndex));
  runtime.session->RewindTo(held);
  runtime.MarkStateDirtyLocked();
  runtime.grants.clear();
  runtime.itemCount = static_cast<int>(std::min<int64_t>(held, std::numeric_limits<int>::max()));
  runtime.syncWanted = true;
}

// With the built-in tables the game is a plain disc, so what the AP ISO patches
// in is done here each tick instead: unlimited ammo, the Artifact Temple totems
// following the artifacts held (the retail pickup scripts light the totem of the
// artifact that used to be at a location, not the one received), and the goal.
void ApplyBuiltinWorld(Runtime& runtime, CStateManager& mgr, CPlayerState& player) {
  namespace Prime = MetroidPrime;
  bool unlimitedMissiles = false;
  bool unlimitedPowerBombs = false;
  PortApWorld::Layout layout;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    const bool builtin = runtime.config.builtin && runtime.session != nullptr;
    if (builtin && runtime.session->GetState().hasWorld)
      layout = runtime.session->GetState().world;
    // The world's item limits (its Config.py; not in slot data) hold in a
    // seed's game, the disc's anywhere else.
    const bool seedGame = builtin && runtime.session->GetState().hasWorld;
    CPlayerState::PortSetAmmoLimits(seedGame ? 999 : 250, seedGame ? 99 : 8);
    // The seed's tank capacity; a game outside Archipelago has the disc's.
    const float capacity = static_cast<float>(layout.etankCapacity);
    if (CPlayerState::GetEnergyTankCapacity() != capacity) {
      // Full health stays full under the new capacity.
      const bool full = player.GetHealthInfo().GetHP() >= player.CalculateHealth();
      CPlayerState::PortSetEnergyTankCapacity(capacity);
      if (full)
        player.HealthInfo()->SetHP(player.CalculateHealth());
    }
    if (!builtin)
      return;
    unlimitedMissiles =
        runtime.session->ReceivedCount(Prime::kItemBase + Prime::kUnlimitedMissiles) > 0;
    unlimitedPowerBombs =
        runtime.session->ReceivedCount(Prime::kItemBase + Prime::kUnlimitedPowerBombs) > 0;
    if (!runtime.goalWanted && gpGameState->CurrentWorldAssetId() == Prime::kEndOfGameWorld) {
      runtime.goalWanted = true;
      runtime.wake.notify_all();
    }
  }
  const auto topOff = [&player](CPlayerState::EItemType type) {
    const int missing = player.GetItemCapacity(type) - player.GetItemAmount(type);
    if (missing > 0)
      player.IncrPickUp(type, missing);
  };
  if (unlimitedMissiles)
    topOff(CPlayerState::kIT_Missiles);
  if (unlimitedPowerBombs)
    topOff(CPlayerState::kIT_PowerBombs);

  // Tallon's layer state is shared with the running world while in Tallon, so
  // writing it through the game state covers both cases.
  CScriptLayerManager* layers =
      gpGameState->StateForWorld(Prime::kTallonWorld).GetLayerState().GetPtr();
  TAreaId temple = Prime::kArtifactTempleIndex;
  const CWorld* world = mgr.GetWorld();
  if (world != nullptr && world->IGetWorldAssetId() == Prime::kTallonWorld)
    temple = world->IGetAreaId(Prime::kArtifactTempleArea);
  // The layers the seed keeps on or off.
  for (const PortApWorld::LayerChange& change : PortApWorld::Layers(layout)) {
    CScriptLayerManager* state = gpGameState->StateForWorld(change.mlvl).GetLayerState().GetPtr();
    TAreaId area(change.area);
    if (world != nullptr && world->IGetWorldAssetId() == change.mlvl)
      area = world->IGetAreaId(change.mrea);
    if (state == nullptr || area.Value() < 0 ||
        static_cast<size_t>(area.Value()) >= state->GetAreaLayers().size())
      continue;
    if (change.whileLayer >= 0 && !state->IsLayerActive(area, TLayerId(change.whileLayer)))
      continue;
    if (state->IsLayerActive(area, TLayerId(change.layer)) != change.active)
      state->SetLayerActive(area, TLayerId(change.layer), change.active);
  }
  if (layers == nullptr || temple.Value() < 0 ||
      static_cast<size_t>(temple.Value()) >= layers->GetAreaLayers().size())
    return;
  for (int id = CPlayerState::kIT_Truth; id <= CPlayerState::kIT_Newborn; ++id) {
    const bool held = player.GetItemAmount(static_cast<CPlayerState::EItemType>(id)) > 0;
    // Truth's totem is the first thing in the room, so it has its own layer.
    const TLayerId layer(id == CPlayerState::kIT_Truth ? 23 : id - 28);
    if (layers->IsLayerActive(temple, layer) != held)
      layers->SetLayerActive(temple, layer, held);
  }
}

void CountChecks(Runtime& runtime, size_t count) {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  const size_t room = static_cast<size_t>(std::max(0, std::numeric_limits<int>::max() -
                                                       runtime.checkCount));
  runtime.checkCount += static_cast<int>(std::min(count, room));
}

// Archipelago servers take each message as a JSON array of commands; a bare
// object makes MultiServer iterate its keys and drop the connection.
bool SendPacket(PortWs::Client& client, const std::string& packet, std::string& error) {
  if (client.SendText(!packet.empty() && packet.front() == '[' ? packet : "[" + packet + "]"))
    return true;
  error = ErrorText(client.Error());
  return false;
}

// The URLs to try for the configured server. A bare "host:port", as the
// Archipelago site shows it, is tried as ws:// and then wss://, the way the
// official client does, and gets Archipelago's default port when it has none.
std::vector<std::string> ServerUrls(const std::string& server) {
  if (server.find("://") != std::string::npos)
    return {server};
  std::string address = server;
  const size_t close = address.rfind(']');
  const size_t colon = address.rfind(':');
  if (colon == std::string::npos || (close != std::string::npos && colon < close))
    address += ":38281";
  return {"ws://" + address, "wss://" + address};
}

// Records a game in its directory's game.json (for the recent-games list) and
// remembers its seed in archipelago.json, so the next launch picks its save card
// before the server answers.
void RecordGame(const Config& config, const std::string& seed) {
  const std::filesystem::path dir = GameDirectory(config.slot, seed);
  std::error_code ignored;
  std::filesystem::create_directories(dir, ignored);
  Protocol::Connection game;
  game.server = config.server;
  game.slot = config.slot;
  game.password = config.password;
  game.seed = seed;
  game.lastPlayed = static_cast<int64_t>(std::time(nullptr));
  std::string error;
  if (!Protocol::SaveConnectionFile((dir / "game.json").string(), game, error))
    PortLog::Write("archipelago: %s\n", error.c_str());
  std::lock_guard<std::mutex> lock(ConnectionFileMutex());
  Protocol::Connection saved = Protocol::LoadConnectionFile(ConfigPath());
  if (saved.server != config.server || saved.slot != config.slot || saved.seed == seed)
    return; // changed by a Connect since this session started, or already known
  saved.seed = seed;
  if (!Protocol::SaveConnectionFile(ConfigPath(), saved, error))
    PortLog::Write("archipelago: %s\n", error.c_str());
}

// The server named its seed. Each game keeps its progress in its own
// directory, so a seed other than the one loaded swaps that game's state in.
// Runs on the socket thread before the session sees the RoomInfo.
void EnterSeed(Runtime& runtime, const std::string& seed) {
  if (seed.empty())
    return;
  Config config;
  std::string oldPath;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (runtime.session == nullptr)
      return;
    config = runtime.config;
    oldPath = runtime.statePath;
  }
  RecordGame(config, seed);
  const std::string path = GameStatePath(config.slot, seed);
  if (path == oldPath)
    return;
  runtime.FlushState(); // the old game's progress, to its own file
  Protocol::State state = Protocol::LoadStateFile(path);
  if (state.slot != config.slot || (!state.seed.empty() && state.seed != seed))
    state = Protocol::State();
  state.slot = config.slot;
  state.seed = seed;

  std::lock_guard<std::mutex> lock(runtime.mutex);
  if (runtime.session == nullptr)
    return;
  const Protocol::State& current = runtime.session->GetState();
  if (current.seed.empty()) {
    // Checks made before the seed was known are this game's.
    for (const int64_t id : current.checkedLocations) {
      if (std::find(state.checkedLocations.begin(), state.checkedLocations.end(), id) ==
          state.checkedLocations.end())
        state.checkedLocations.push_back(id);
    }
  } else {
    // Another game's: its items and unsent checks do not carry over.
    PortLog::Write("archipelago: the server has seed \"%s\", not \"%s\"; switching games\n",
                   seed.c_str(), current.seed.c_str());
    runtime.grants.clear();
    runtime.queuedChecks.clear();
  }
  if (runtime.resetPending) {
    PortLog::Write("archipelago: MP_AP_RESET_STATE=1 discarded saved progress for %s\n",
                   config.slot.c_str());
    state.nextItemIndex = 0;
    state.checkedLocations.clear();
    state.progressive.clear();
    runtime.resetPending = false;
  }
  runtime.session->SetState(state);
  runtime.statePath = path;
  runtime.recordSynced = false;
  runtime.MarkStateDirtyLocked();
  PortLog::Write("archipelago: game directory %s\n",
                 std::filesystem::path(path).parent_path().string().c_str());
}

void WorkerLoop(Runtime& runtime) {
  Config config;
  std::string statePath;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    config = runtime.config;
    statePath = runtime.statePath;
  }

  int backoff = 5;
  while (!runtime.stop.load(std::memory_order_acquire)) {
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      runtime.connected = false;
      runtime.stateLabel = "connecting";
      runtime.LogStateLocked("connecting to " + config.server);
    }

    std::string host;
    std::string path;
    uint16_t port = 0;
    bool secure = false;
    std::string connectionError;
    PortWs::Client client;
    client.SetCancelFlag(&runtime.stop); // quitting must not wait out a timeout
    PortWs::TlsOptions tls;
    tls.caFile = config.tlsCa;
    bool transportReady = false;
    for (const std::string& url : ServerUrls(config.server)) {
      if (!PortWs::ParseUrl(url, host, port, path, secure)) {
        connectionError = "invalid server URL: " + config.server;
      } else if (!client.Connect(host, port, path, 10000, secure, tls)) {
        connectionError = ErrorText(client.Error());
      } else {
        transportReady = true;
        client.SetTimeoutMs(10000);
        PortLog::Write("archipelago: WebSocket open (%s)\n",
                       client.Compressed() ? "permessage-deflate" : "uncompressed");
        break;
      }
      if (runtime.stop.load(std::memory_order_acquire))
        break;
    }

    bool connectionFailed = !transportReady;
    bool apConnected = false;
    if (!transportReady) {
      if (!runtime.stop.load(std::memory_order_acquire)) // not a failure when cut short
        runtime.SetError(connectionError);
    } else {
      while (!runtime.stop.load(std::memory_order_acquire) && client.IsOpen()) {
        std::string message;
        const bool received = client.ReceiveText(message, 1000);
        if (!received && !client.IsOpen()) {
          connectionError = ErrorText(client.Error());
          connectionFailed = true;
          break;
        }
        if (!received && client.IsOpen() &&
            std::string(client.Error()) != "receive timed out") {
          connectionError = ErrorText(client.Error());
          connectionFailed = true;
          break;
        }

        if (received) {
          PortJson::Value root;
          size_t errorOffset = 0;
          const char* parseReason = nullptr;
          if (!PortJson::Parse(message, root, errorOffset, &parseReason)) {
            connectionError = "invalid server JSON at byte " + std::to_string(errorOffset) +
                              (parseReason != nullptr ? std::string(": ") + parseReason : "");
            connectionFailed = true;
            break;
          }

          std::vector<PortJson::Value> commands;
          if (root.IsArray())
            commands = root.AsArray();
          else
            commands.push_back(std::move(root));

          for (const PortJson::Value& command : commands) {
            const PortJson::Value* cmdValue = command.Find("cmd");
            const std::string cmd = cmdValue != nullptr && cmdValue->IsString()
                                        ? cmdValue->AsString()
                                        : std::string();
            std::vector<std::string> outgoing;
            std::vector<ItemGrant> newGrants;
            std::string connectPacket;
            std::vector<int64_t> initialChecks;
            std::vector<int64_t> sendAllChecks;
            bool connectedNow = false;
            bool refused = false;
            if (cmd == "RoomInfo") {
              const PortJson::Value* seedName = command.Find("seed_name");
              if (seedName != nullptr && seedName->IsString())
                EnterSeed(runtime, seedName->AsString());
            }
            {
              std::lock_guard<std::mutex> lock(runtime.mutex);
              if (runtime.session == nullptr)
                continue;
              const int64_t oldIndex = runtime.session->GetState().nextItemIndex;
              const std::vector<int64_t> oldChecks = runtime.session->GetState().checkedLocations;
              const std::map<int64_t, int64_t> oldProgressive =
                  runtime.session->GetState().progressive;
              const std::string oldSeed = runtime.session->GetState().seed;
              const bool oldHasLogic = runtime.session->GetState().hasLogic;
              const PortApLogic::Options oldLogic = runtime.session->GetState().logic;
              const bool oldHasWorld = runtime.session->GetState().hasWorld;
              const PortApWorld::Layout oldWorld = runtime.session->GetState().world;
              const std::string oldError = runtime.session->LastError();
              const std::string oldResetReason = runtime.session->ResetReason();
              runtime.session->HandlePacket(command, outgoing, newGrants);
              if (runtime.session->ResetReason() != oldResetReason) {
                // The recorded checks belonged to another session, so the state
                // file has just been rewritten empty. Say why, because the
                // symptom otherwise is a multiworld that sends nothing.
                PortLog::Write(
                             "archipelago: saved progress was %s; starting %s fresh (set "
                             "MP_AP_RESET_STATE=1 to discard it deliberately)\n",
                             runtime.session->ResetReason().c_str(), config.slot.c_str());
              }
              const Protocol::State& state = runtime.session->GetState();
              if (state.nextItemIndex != oldIndex || state.checkedLocations != oldChecks ||
                  state.progressive != oldProgressive || state.seed != oldSeed ||
                  state.hasLogic != oldHasLogic || state.logic != oldLogic ||
                  state.hasWorld != oldHasWorld || state.world != oldWorld)
                runtime.MarkStateDirtyLocked();
              runtime.grants.insert(runtime.grants.end(), newGrants.begin(), newGrants.end());
              runtime.lastMessage = runtime.session->LastMessage();
              for (Protocol::ChatLine line; runtime.session->TakeChatLine(line);)
                runtime.AppendChatLocked(std::move(line.type), std::move(line.text));
              const std::string& packetError = runtime.session->LastError();
              if (cmd != "ConnectionRefused" && !packetError.empty() && packetError != oldError &&
                  packetError != runtime.lastError) {
                runtime.lastError = packetError;
                runtime.LogStateLocked(packetError);
              }
              if (cmd == "Connected")
                runtime.syncWanted = false; // the handshake replays everything anyway
              if (cmd == "RoomInfo") {
                connectPacket = runtime.session->BuildConnect();
              } else if (cmd == "Connected" && runtime.session->HandshakeComplete()) {
                connectedNow = true;
                runtime.connected = true;
                runtime.stateLabel = "connected";
                runtime.lastError.clear();
                runtime.LogStateLocked("connected as " + config.slot);
                runtime.AppendChatLocked("port", "Connected to " + config.server + " as " + config.slot);
                initialChecks = state.checkedLocations;
                for (auto queued = runtime.queuedChecks.begin(); queued != runtime.queuedChecks.end();) {
                  if (std::find(initialChecks.begin(), initialChecks.end(), *queued) !=
                      initialChecks.end())
                    queued = runtime.queuedChecks.erase(queued);
                  else
                    ++queued;
                }
                if (EnvEnabled("MP_AP_SEND_ALL"))
                  sendAllChecks = runtime.session->AllLocationIds();
                for (const std::string& warning : runtime.session->GetSlotData().warnings)
                  PortLog::Write("archipelago: seed option %s; the game will not match the "
                                 "seed's logic\n",
                                 warning.c_str());
              } else if (cmd == "ConnectionRefused") {
                refused = true;
                runtime.connected = false;
                runtime.stateLabel = "error";
                runtime.lastError = runtime.session->LastError();
                if (runtime.lastError.empty())
                  runtime.lastError = "connection refused";
                connectionError = runtime.lastError;
                runtime.LogStateLocked(runtime.lastError);
              } else if (runtime.session->HandshakeComplete() && !apConnected) {
                apConnected = true;
              }
            }

            if (!connectPacket.empty() && !SendPacket(client, connectPacket, connectionError)) {
              connectionFailed = true;
              break;
            }
            if (connectedNow) {
              apConnected = true;
              backoff = 5;
              if (!initialChecks.empty() &&
                  !SendPacket(client, Session::BuildLocationChecks(initialChecks), connectionError)) {
                connectionFailed = true;
                break;
              }
              if (!initialChecks.empty())
                CountChecks(runtime, initialChecks.size());
              if (!sendAllChecks.empty() &&
                  !SendPacket(client, Session::BuildLocationChecks(sendAllChecks), connectionError)) {
                connectionFailed = true;
                break;
              }
              if (!sendAllChecks.empty())
                CountChecks(runtime, sendAllChecks.size());
            }
            for (const std::string& packet : outgoing) {
              if (!SendPacket(client, packet, connectionError)) {
                connectionFailed = true;
                break;
              }
            }
            if (connectionFailed)
              break;
            if (refused) {
              connectionFailed = true;
              break;
            }
          }
          if (connectionFailed)
            break;
        }

        std::vector<int64_t> pendingChecks;
        std::string pendingBounce;
        bool syncWanted = false;
        bool goalWanted = false;
        std::vector<std::string> pendingSay;
        if (apConnected) {
          std::lock_guard<std::mutex> lock(runtime.mutex);
          goalWanted = runtime.goalWanted && !runtime.goalSent;
          runtime.goalSent = runtime.goalSent || goalWanted;
          while (!runtime.queuedChecks.empty()) {
            pendingChecks.push_back(runtime.queuedChecks.front());
            runtime.queuedChecks.pop_front();
          }
          syncWanted = runtime.syncWanted;
          runtime.syncWanted = false;
          pendingSay.assign(runtime.pendingSay.begin(), runtime.pendingSay.end());
          runtime.pendingSay.clear();
          // DeathLink: a death the game thread noticed, announced once. The
          // pending flag is *cleared* rather than moved-from, because this block
          // runs on every loop iteration and a moved-from-but-not-cleared
          // string is still non-empty, which announced the same death again on
          // every pass round the socket loop.
          if (!runtime.pendingBounce.empty()) {
            runtime.pendingBounce.clear();
            if (runtime.session != nullptr)
              pendingBounce = runtime.session->BuildBounce();
            if (pendingBounce.empty())
              runtime.deathAnnounced = false; // not in DeathLink; do not latch
          }
        }
        if (syncWanted && !SendPacket(client, Session::BuildSync(), connectionError)) {
          connectionFailed = true;
          break;
        }
        if (!pendingChecks.empty()) {
          if (!SendPacket(client, Session::BuildLocationChecks(pendingChecks), connectionError)) {
            connectionFailed = true;
            break;
          }
          CountChecks(runtime, pendingChecks.size());
        }
        if (!pendingBounce.empty()) {
          if (!SendPacket(client, pendingBounce, connectionError)) {
            connectionFailed = true;
            break;
          }
          PortLog::Write("archipelago: announced a death to the multiworld\n");
        }
        if (goalWanted) {
          if (!SendPacket(client, Session::BuildGoal(), connectionError)) {
            std::lock_guard<std::mutex> lock(runtime.mutex);
            runtime.goalSent = false; // owed again on the next connection
            connectionFailed = true;
            break;
          }
          PortLog::Write("archipelago: goal complete\n");
        }
        for (const std::string& text : pendingSay) {
          if (!SendPacket(client, Session::BuildSay(text), connectionError)) {
            connectionFailed = true;
            break;
          }
        }
        if (connectionFailed)
          break;
        runtime.FlushState();

        if (!received && !client.IsOpen()) {
          connectionError = ErrorText(client.Error());
          connectionFailed = true;
          break;
        }
      }
    }

    client.Close();
    runtime.FlushState();
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (runtime.connected && !runtime.stop.load(std::memory_order_acquire))
        runtime.AppendChatLocked("port", "Disconnected: " +
                                             (connectionError.empty() ? std::string("connection closed")
                                                                      : connectionError));
      runtime.connected = false;
    }
    if (runtime.stop.load(std::memory_order_acquire))
      break;
    if (connectionError.empty())
      connectionError = "connection closed";
    if (connectionFailed || !apConnected)
      runtime.SetError(connectionError);
    if (!runtime.WaitBackoff(backoff))
      break;
    backoff = std::min(backoff * 2, 60);
  }
}

// An exception escaping a std::thread terminates the game, and a broken or
// hostile server can cause one (bad_alloc on a huge message, a packet the
// session does not expect). It costs the connection instead, which retries.
void Worker(Runtime& runtime) {
  while (!runtime.stop.load(std::memory_order_acquire)) {
    try {
      WorkerLoop(runtime);
      break;
    } catch (const std::exception& error) {
      PortLog::Write("archipelago: client error: %s\n", error.what());
      runtime.SetError(std::string("internal error: ") + error.what());
    } catch (...) {
      PortLog::Write("archipelago: client error: unknown exception\n");
      runtime.SetError("internal error");
    }
    if (!runtime.WaitBackoff(30))
      break;
  }
  std::lock_guard<std::mutex> lock(runtime.mutex);
  runtime.workerDone = true;
  runtime.wake.notify_all();
}

// Before games had directories, one archipelago_state.json sat beside the
// configuration. It moves into its game's directory, if it names one.
void MigrateLegacyState(const std::filesystem::path& parent) {
  const std::filesystem::path legacy = parent / "archipelago_state.json";
  std::error_code error;
  if (!std::filesystem::exists(legacy, error))
    return;
  const Protocol::State state = Protocol::LoadStateFile(legacy.string());
  if (state.slot.empty() || state.seed.empty())
    return;
  const std::filesystem::path target = GameStatePath(state.slot, state.seed);
  if (std::filesystem::exists(target, error))
    return;
  std::filesystem::create_directories(target.parent_path(), error);
  std::filesystem::rename(legacy, target, error);
  if (error)
    PortLog::Write("archipelago: could not move %s to %s: %s\n", legacy.string().c_str(),
                   target.string().c_str(), error.message().c_str());
  else
    PortLog::Write("archipelago: moved %s to %s\n", legacy.string().c_str(),
                   target.string().c_str());
}

// Loads the configuration and starts the worker. `mutex` is held and no worker
// is running. `firstStart` is the launch, as opposed to a Connect from the
// overlay, which must not run the launch-only MP_AP_RESET_STATE again.
void StartLocked(Runtime& runtime, bool firstStart) {
  const std::string configPath = ConfigPath();
  runtime.config = Protocol::LoadConfigFile(configPath);
  if (!runtime.config.valid) {
    runtime.stateLabel = "off";
    runtime.lastError = runtime.config.error;
    PortLog::Write( "archipelago: %s\n", runtime.config.error.c_str());
    return;
  }

  std::filesystem::path parent = std::filesystem::path(configPath).parent_path();
  if (parent.empty())
    parent = ".";
  // tls_ca is written relative to the config file, not the working directory.
  if (!runtime.config.tlsCa.empty() && std::filesystem::path(runtime.config.tlsCa).is_relative())
    runtime.config.tlsCa = (parent / runtime.config.tlsCa).string();
  MigrateLegacyState(parent);
  // The seed this slot had last time picks the game (and its save card) until
  // the server says otherwise.
  const std::string seed = Protocol::LoadConnectionFile(configPath).seed;
  Protocol::State state;
  runtime.statePath.clear();
  if (!seed.empty()) {
    runtime.statePath = GameStatePath(runtime.config.slot, seed);
    state = Protocol::LoadStateFile(runtime.statePath);
    if (state.slot != runtime.config.slot || (!state.seed.empty() && state.seed != seed))
      state = Protocol::State();
    state.seed = seed;
  }
  // A rewind the client cannot see - a new game or an older save on the same
  // slot and seed - leaves the recorded checks looking valid, so this is the
  // way out: it drops them before the first connect (or once the seed is known).
  runtime.resetPending = false;
  if (firstStart && EnvEnabled("MP_AP_RESET_STATE")) {
    if (runtime.statePath.empty()) {
      runtime.resetPending = true;
    } else if (state.nextItemIndex != 0 || !state.checkedLocations.empty() ||
               !state.progressive.empty()) {
      PortLog::Write("archipelago: MP_AP_RESET_STATE=1 discarded saved progress for %s\n",
                     runtime.config.slot.c_str());
      state = Protocol::State();
      state.seed = seed;
    }
  }
  state.slot = runtime.config.slot;
  runtime.session = std::make_unique<Session>(runtime.config, state);
  runtime.enabled = true;
  runtime.stateLabel = "connecting";
  runtime.stop.store(false, std::memory_order_release);
  runtime.workerDone = false;
  runtime.worker = std::thread(Worker, std::ref(runtime));
}

void EnsureLoadedImpl(Runtime& runtime) {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  if (runtime.attempted)
    return;
  runtime.attempted = true;
  if (EnvEnabled("MP_AP_DISABLE")) {
    runtime.stateLabel = "off";
    return;
  }
  StartLocked(runtime, true);
}

// Runs on its own thread, because stopping the old worker can wait on a DNS
// lookup that cannot be cut short and the overlay must not freeze meanwhile.
// Stops the session, writes what it had, and starts again from the file.
void Restart(Runtime& runtime) {
  std::lock_guard<std::mutex> restarting(runtime.restartMutex);
  std::thread old;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (runtime.exiting)
      return;
    old = std::move(runtime.worker);
  }
  runtime.stop.store(true, std::memory_order_release);
  runtime.wake.notify_all();
  if (old.joinable())
    old.join();
  runtime.FlushState(); // the old session's progress, before the file is reread

  std::lock_guard<std::mutex> lock(runtime.mutex);
  if (runtime.exiting)
    return;
  const std::string oldSlot = runtime.config.slot;
  const std::string oldServer = runtime.config.server;
  runtime.session.reset();
  runtime.enabled = false;
  runtime.connected = false;
  runtime.stateLabel = "off";
  runtime.lastError.clear();
  runtime.lastMessage.clear();
  runtime.lastLogged.clear();
  // Unsent checks are in the state file and go out with the next handshake.
  runtime.queuedChecks.clear();
  runtime.notifications.clear();
  runtime.pendingSay.clear();
  runtime.pendingBounce.clear();
  runtime.syncWanted = false;
  runtime.stateDirty = false;
  runtime.itemCount = 0;
  runtime.checkCount = 0;
  runtime.unmappedCount = 0;
  runtime.deathAnnounced = false;
  runtime.goalWanted = false;
  runtime.goalSent = false;
  runtime.recordSynced = false;
  StartLocked(runtime, false);
  // Items received but not yet handed to the game belong to that slot; the
  // state file already counts them as processed, so a new session on the same
  // slot would not send them again.
  if (runtime.config.slot != oldSlot || runtime.config.server != oldServer || !runtime.enabled)
    runtime.grants.clear();
}

void StartRestart(Runtime& runtime) {
  std::thread(
      [&runtime] {
        try {
          Restart(runtime);
        } catch (const std::exception& error) {
          PortLog::Write("archipelago: restart failed: %s\n", error.what());
        } catch (...) {
          PortLog::Write("archipelago: restart failed\n");
        }
      })
      .detach();
}

} // namespace

void EnsureLoaded() {
  try {
    EnsureLoadedImpl(GetRuntime());
  } catch (const std::exception& error) {
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.attempted)
      runtime.attempted = true;
    runtime.enabled = false;
    runtime.stateLabel = "off";
    PortLog::Write( "archipelago: initialization failed: %s\n", error.what());
  } catch (...) {
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    runtime.attempted = true;
    runtime.enabled = false;
    runtime.stateLabel = "off";
    PortLog::Write( "archipelago: initialization failed\n");
  }
}

ConnectionDetails SavedConnection() {
  try {
    const Protocol::Connection saved = Protocol::LoadConnectionFile(ConfigPath());
    ConnectionDetails details;
    details.server = saved.server;
    details.slot = saved.slot;
    details.password = saved.password;
    details.enabled = saved.enabled;
    details.seed = saved.seed;
    return details;
  } catch (...) {
    return ConnectionDetails();
  }
}

std::vector<ConnectionDetails> RecentGames() {
  std::vector<ConnectionDetails> games;
  try {
    std::error_code error;
    for (std::filesystem::directory_iterator it(ConfigDirectory() / "archipelago_games", error), end;
         !error && it != end; it.increment(error)) {
      const std::filesystem::path file = it->path() / "game.json";
      if (!std::filesystem::is_regular_file(file, error))
        continue;
      const Protocol::Connection game = Protocol::LoadConnectionFile(file.string());
      if (game.server.empty() || game.slot.empty() || game.seed.empty())
        continue;
      ConnectionDetails details;
      details.server = game.server;
      details.slot = game.slot;
      details.password = game.password;
      details.seed = game.seed;
      details.lastPlayed = game.lastPlayed;
      games.push_back(std::move(details));
    }
  } catch (...) {
  }
  std::sort(games.begin(), games.end(), [](const ConnectionDetails& a, const ConnectionDetails& b) {
    return a.lastPlayed > b.lastPlayed;
  });
  return games;
}

bool Connect(const ConnectionDetails& details, std::string& error) {
  try {
    Protocol::Connection connection;
    connection.server = details.server;
    connection.slot = details.slot;
    connection.password = details.password;
    connection.enabled = true;
    const auto trim = [](std::string text) {
      const size_t first = text.find_first_not_of(" \t\r\n");
      if (first == std::string::npos)
        return std::string();
      return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    };
    connection.server = trim(connection.server);
    connection.slot = trim(connection.slot);
    // The room page shows "/connect host:port"; pasting that whole is fine.
    if (connection.server.rfind("/connect ", 0) == 0)
      connection.server = trim(connection.server.substr(9));
    if (connection.server.empty() || connection.slot.empty()) {
      error = "a server and a slot name are needed";
      return false;
    }
    std::string host;
    std::string path;
    uint16_t port = 0;
    bool secure = false;
    const bool bare = connection.server.find("://") == std::string::npos;
    if ((bare && connection.server.find('/') != std::string::npos) ||
        !PortWs::ParseUrl(ServerUrls(connection.server).front(), host, port, path, secure)) {
      error = "not a server address: " + connection.server;
      return false;
    }
    if (EnvEnabled("MP_AP_DISABLE")) {
      error = "MP_AP_DISABLE is set";
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(ConnectionFileMutex());
      // The same slot on the same server is most likely the same game, so its
      // seed (and save card) stays until the server says otherwise.
      // A game resumed from the recent list brings its own seed.
      const Protocol::Connection saved = Protocol::LoadConnectionFile(ConfigPath());
      if (!details.seed.empty())
        connection.seed = details.seed;
      else if (saved.server == connection.server && saved.slot == connection.slot)
        connection.seed = saved.seed;
      if (!Protocol::SaveConnectionFile(ConfigPath(), connection, error))
        return false;
    }
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      runtime.stateLabel = "connecting";
    }
    StartRestart(runtime);
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  } catch (...) {
    error = "could not connect";
    return false;
  }
}

bool Disconnect(std::string& error) {
  try {
    {
      std::lock_guard<std::mutex> lock(ConnectionFileMutex());
      Protocol::Connection connection = Protocol::LoadConnectionFile(ConfigPath());
      connection.enabled = false;
      if (!Protocol::SaveConnectionFile(ConfigPath(), connection, error))
        return false;
    }
    EnsureLoaded();
    StartRestart(GetRuntime());
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  } catch (...) {
    error = "could not disconnect";
    return false;
  }
}

std::string SaveCardDirectory() {
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.enabled || runtime.statePath.empty())
      return std::string();
    return std::filesystem::path(runtime.statePath).parent_path().string();
  } catch (...) {
    return std::string();
  }
}

std::string ConfigFilePath() {
  try {
    return ConfigPath();
  } catch (...) {
    return std::string();
  }
}

bool Enabled() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  return runtime.enabled;
}

bool Connected() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  return runtime.connected;
}

const char* StatusText() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  thread_local std::string text;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.enabled) {
      text = runtime.stateLabel == "connecting" ? "ap: connecting" : "ap: off";
    } else if (runtime.connected) {
      text = "ap: connected, " + std::to_string(runtime.itemCount) + " items, " +
             std::to_string(runtime.checkCount) + " checks";
    } else if (runtime.stateLabel == "error" && !runtime.lastError.empty()) {
      text = "ap: " + runtime.lastError;
    } else {
      text = "ap: connecting";
    }
  }
  constexpr size_t kOverlayLimit = 48;
  if (text.size() > kOverlayLimit)
    text.resize(kOverlayLimit);
  return text.c_str();
}

int ItemCount() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  return runtime.itemCount;
}

std::vector< TrackedItem > TrackedItems() {
  EnsureLoaded();
  std::vector< TrackedItem > result;
  Runtime& runtime = GetRuntime();
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (runtime.session == nullptr)
      return result;
    const std::vector< Protocol::TrackedItem >& tracked = runtime.session->Tracked();
    result.reserve(tracked.size());
    for (const Protocol::TrackedItem& item : tracked) {
      TrackedItem entry;
      entry.name = item.name;
      entry.from = item.from;
      entry.step = static_cast< int >(item.step);
      entry.total = static_cast< int >(item.total);
      result.push_back(std::move(entry));
    }
  }
  return result;
}

std::vector< ChatLine > ChatLog(uint64_t* serial) {
  std::vector< ChatLine > result;
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    result.assign(runtime.chat.begin(), runtime.chat.end());
    if (serial != nullptr)
      *serial = runtime.chatSerial;
  } catch (...) {
    result.clear();
  }
  return result;
}

bool SendChat(const std::string& text, std::string& error) {
  try {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      error = "nothing to send";
      return false;
    }
    const size_t last = text.find_last_not_of(" \t\r\n");
    constexpr size_t kSayLimit = 1000;
    std::string trimmed = text.substr(first, std::min(last - first + 1, kSayLimit));
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.connected) {
      error = "not connected";
      return false;
    }
    // The socket thread sends these after its receive wait (up to a second).
    constexpr size_t kPendingSay = 64;
    if (runtime.pendingSay.size() >= kPendingSay) {
      error = "still sending the last messages";
      return false;
    }
    runtime.pendingSay.push_back(std::move(trimmed));
    runtime.wake.notify_all();
    error.clear();
    return true;
  } catch (...) {
    error = "could not queue the message";
    return false;
  }
}

int CheckCount() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  return runtime.checkCount;
}

const char* LastMessage() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  thread_local std::string message;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    message = runtime.lastMessage;
  }
  return message.c_str();
}

bool TakeNotification(std::string& text) {
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.notifications.empty()) {
      text = std::move(runtime.notifications.front());
      runtime.notifications.pop_front();
      return true;
    }
    return runtime.session != nullptr && runtime.session->TakeNotification(text);
  } catch (...) {
    text.clear();
    return false;
  }
}

const char* SeedName() {
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    thread_local std::string seed;
    std::lock_guard<std::mutex> lock(runtime.mutex);
    seed = runtime.session != nullptr ? runtime.session->SeedName() : std::string();
    return seed.c_str();
  } catch (...) {
    return "";
  }
}

void QueueCheck(const char* locationKey) {
  if (locationKey == nullptr || locationKey[0] == '\0')
    return;
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.enabled || runtime.session == nullptr)
      return;
    int64_t id = 0;
    if (!runtime.session->MarkLocationChecked(locationKey, id)) {
      // Two things come back false here and only one of them is a problem. A key
      // the table knows but has already recorded is the ordinary case. A key the
      // table has never heard of means this location will never be reported, and
      // that fails silently: the session plays fine, items arrive, and the server
      // just never records a check. For a multiworld that is the worst shape a
      // failure can take, so name it once instead of dropping it quietly. The
      // usual cause is a location table whose keys are not the world:area:entity
      // form the game produces - see the note in docs/ARCHIPELAGO.md.
      if (!runtime.session->KnowsLocation(locationKey)) {
        if (runtime.unmappedCount == 0) {
          PortLog::Write(
              "archipelago: location '%s' has no id in the location table, so it was not "
              "reported. The session still works and the server will simply never record "
              "this check; if none ever arrives, the world's location keys are probably "
              "not world:area:entity (see docs/ARCHIPELAGO.md).\n",
              locationKey);
        }
        ++runtime.unmappedCount;
      }
      return;
    }
    runtime.MarkStateDirtyLocked();
    runtime.queuedChecks.push_back(id);
  } catch (...) {
    // This is called from game pickup handling; AP must never disrupt gameplay.
  }
}

namespace {

bool BuiltinEnabled() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  return runtime.enabled && runtime.session != nullptr && runtime.session->GetConfig().builtin;
}

// The built-in locations belong to the multiworld: a session on the built-in
// tables is running, or, with the client off, the running game is an AP game.
bool BuiltinRules() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (runtime.enabled && runtime.session != nullptr)
      return runtime.session->GetConfig().builtin;
  }
  return gpGameState != nullptr && IsApGame(gpGameState->PortApProgress());
}

// The layout of the seed in play, once its slot_data has been seen (in this
// session or an earlier one).
bool SeedLayout(PortApWorld::Layout& out) {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  if (!runtime.enabled || runtime.session == nullptr || !runtime.session->GetConfig().builtin ||
      !runtime.session->GetState().hasWorld)
    return false;
  out = runtime.session->GetState().world;
  return true;
}

} // namespace

bool OwnsPickup(uint32_t world, uint32_t area, uint32_t entity) {
  try {
    return MetroidPrime::FindPickup(world, area, entity) != nullptr && BuiltinRules();
  } catch (...) {
    return false;
  }
}

bool OwnsMemo(uint32_t world, uint32_t area, uint32_t entity) {
  try {
    return MetroidPrime::FindMemo(world, area, entity) != nullptr && BuiltinRules();
  } catch (...) {
    return false;
  }
}

void RecordPickup(uint32_t world, uint32_t area, uint32_t entity) {
  try {
    const int bit = CheckedBit(MetroidPrime::FindPickup(world, area, entity));
    if (bit < 0 || gpGameState == nullptr)
      return;
    gpGameState->PortApProgress().checked[bit / 32] |= 1u << (bit % 32);
  } catch (...) {
  }
}

void AnnouncePickup(uint32_t world, uint32_t area, uint32_t entity) {
  try {
    const MetroidPrime::Location* location = MetroidPrime::FindPickup(world, area, entity);
    if (location == nullptr)
      return;
    std::string text;
    bool offline = true;
    {
      Runtime& runtime = GetRuntime();
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (runtime.enabled && runtime.session != nullptr) {
        text = runtime.session->AnnounceLocation(location->id);
        offline = false;
      }
    }
    if (offline)
      text = std::string("Checked ") + location->name + ", sent when connected";
    else if (text.empty())
      text = std::string("Checked ") + location->name;
    PortLog::Write("archipelago: %s\n", text.c_str());
    CSamusHud::DisplayHudMemo(ToHudWide(text), CHUDMemoParms(5.f, true, false, false));
  } catch (...) {
  }
}

bool PickupModel(uint32_t world, uint32_t area, uint32_t entity, PortRandomizer::PickupModel& out) {
  try {
    const MetroidPrime::Location* location = MetroidPrime::FindPickup(world, area, entity);
    if (location == nullptr || !BuiltinRules())
      return false;
    int64_t item = 0;
    bool sameGame = false;
    int64_t flags = 0;
    {
      Runtime& runtime = GetRuntime();
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (!runtime.enabled || runtime.session == nullptr ||
          !runtime.session->ScoutedAt(location->id, item, sameGame, &flags))
        return false;
    }
    const int key = sameGame ? MetroidPrime::PickupModelKey(item)
                             : MetroidPrime::OtherGameModelKey(flags);
    return PortSkipCutscenes::PickupModel(key, out);
  } catch (...) {
    return false;
  }
}

bool PickupScanText(uint32_t world, uint32_t area, uint32_t entity, std::string& out) {
  try {
    const MetroidPrime::Location* location = MetroidPrime::FindPickup(world, area, entity);
    if (location == nullptr || !BuiltinRules())
      return false;
    {
      Runtime& runtime = GetRuntime();
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (runtime.enabled && runtime.session != nullptr)
        out = runtime.session->ScanText(location->id);
    }
    // Empty when not scouted yet (or playing offline): the pickup is still a
    // check, not the retail item.
    return true;
  } catch (...) {
    return false;
  }
}

bool ArtifactHint(int itemType, std::string& out) {
  try {
    if (itemType < MetroidPrime::kArtifactTruth || itemType > MetroidPrime::kArtifactNewborn ||
        !BuiltinRules())
      return false;
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.enabled || runtime.session == nullptr || !runtime.session->HandshakeComplete())
      return false;
    out = runtime.session->ArtifactHint(MetroidPrime::kItemBase + itemType);
    return !out.empty();
  } catch (...) {
    return false;
  }
}

bool NewGameStart(uint32_t& world, uint32_t& area) {
  try {
    if (!BuiltinEnabled())
      return false;
    world = MetroidPrime::kTallonWorld;
    area = 0;
    PortApWorld::Layout layout;
    if (SeedLayout(layout)) {
      PortApWorld::Place place;
      if (PortApWorld::StartRoom(layout, place)) {
        world = place.mlvl;
        area = place.mrea;
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool WarpToStart(uint32_t& world, uint32_t& area) {
  try {
    PortApWorld::Layout layout;
    if (!SeedLayout(layout))
      return false;
    // A start room the tables don't have is the Landing Site, as for a new game.
    PortApWorld::Place place;
    place.mlvl = MetroidPrime::kTallonWorld;
    PortApWorld::StartRoom(layout, place);
    world = place.mlvl;
    area = place.mrea;
    return true;
  } catch (...) {
    return false;
  }
}

bool SeedStrings(uint32_t strg, std::vector< std::string >& out) {
  try {
    PortApWorld::Layout layout;
    return SeedLayout(layout) && PortApWorld::Strings(layout, strg, out);
  } catch (...) {
    return false;
  }
}

bool SuitDamageReduction(int mode, bool varia, bool gravity, bool phazon, float& out) {
  try {
    PortApWorld::Layout layout;
    if (!SeedLayout(layout))
      return false;
    const float reduction = PortApWorld::SuitDamageReduction(mode, varia, gravity, phazon);
    if (reduction < 0.f)
      return false;
    out = reduction;
    return true;
  } catch (...) {
    return false;
  }
}

bool SeedResultsLine(std::string& out) {
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.enabled || runtime.session == nullptr || !runtime.session->GetConfig().builtin ||
        !runtime.session->GetState().hasWorld || runtime.session->GetState().seed.empty())
      return false;
    out = "Archipelago | " + runtime.session->GetState().seed + " | " +
          runtime.session->GetConfig().slot;
    return true;
  } catch (...) {
    return false;
  }
}

bool SeedGivesStartItems() {
  try {
    PortApWorld::Layout layout;
    return SeedLayout(layout);
  } catch (...) {
    return false;
  }
}

bool TeleporterDestination(uint32_t world, uint32_t editorId, uint32_t& destWorld,
                           uint32_t& destArea) {
  try {
    PortApWorld::Layout layout;
    if (!SeedLayout(layout))
      return false;
    PortApWorld::Place retail, place;
    retail.mlvl = destWorld;
    retail.mrea = destArea;
    if (!PortApWorld::TeleporterDestination(layout, world, editorId, retail, place))
      return false;
    destWorld = place.mlvl;
    destArea = place.mrea;
    return true;
  } catch (...) {
    return false;
  }
}

bool TempleOps(std::vector< uint8_t >& ops) {
  try {
    PortApWorld::Layout layout;
    if (!SeedLayout(layout))
      return false;
    ops = PortApWorld::TempleOps(layout);
    return !ops.empty();
  } catch (...) {
    return false;
  }
}

// The blast shields placed in each loaded room, watched by Poll. Rooms are
// patched on the loading thread.
std::mutex& PlacedShieldsMutex() {
  static std::mutex mutex;
  return mutex;
}
std::map< uint32_t, std::vector< PortApWorld::PlacedShield > >& PlacedShields() {
  static std::map< uint32_t, std::vector< PortApWorld::PlacedShield > > shields;
  return shields;
}

// The shields this game has broken, null outside a game.
const uint32_t* BrokenShields(uint32_t (&copy)[4]) {
  if (gpGameState == nullptr)
    return nullptr;
  for (int i = 0; i < 4; ++i)
    copy[i] = gpGameState->PortApProgress().shields[i];
  return copy;
}

// A shield whose trigger went inactive was broken (or its door was opened
// from behind): remember it, so the room comes back without it.
void WatchShields(CStateManager& mgr, CGameState::ApProgress& progress) {
  std::lock_guard< std::mutex > lock(PlacedShieldsMutex());
  std::vector< int > broken;
  for (auto& room : PlacedShields()) {
    auto& shields = room.second;
    for (size_t i = 0; i < shields.size();) {
      const TUniqueId uid = mgr.GetIdForScript(TEditorId(shields[i].trigger));
      const CEntity* entity = uid == kInvalidUniqueId ? nullptr : mgr.GetObjectById(uid);
      // The id is one DoorOps made up, so whatever answers to it is the trigger.
      if (entity == nullptr || entity->GetActive()) {
        ++i;
        continue;
      }
      const int bit = shields[i].bit;
      if (bit >= 0 && bit < PortApWorld::kShieldBits)
        progress.shields[bit / 32] |= 1u << (bit % 32);
      shields.erase(shields.begin() + i);
      broken.push_back(bit);
    }
  }
  // The same doorway's shield in the room behind it goes too, quietly: its
  // relay does the unlocking, with the sound, music and shake switched off.
  for (auto& room : PlacedShields()) {
    auto& shields = room.second;
    for (size_t i = 0; i < shields.size();) {
      if (std::find(broken.begin(), broken.end(), shields[i].bit) == broken.end()) {
        ++i;
        continue;
      }
      const uint32_t trigger = shields[i].trigger;
      auto send = [&](uint32_t id, EScriptObjectMessage msg) {
        const TUniqueId uid = mgr.GetIdForScript(TEditorId(id));
        if (uid != kInvalidUniqueId)
          mgr.SendScriptMsgAlways(uid, kInvalidUniqueId, msg);
      };
      for (uint32_t id = trigger + 3; id <= trigger + 5; ++id)
        send(id, kSM_Deactivate);
      send(trigger + 2, kSM_SetToZero);
      shields.erase(shields.begin() + i);
    }
  }
}

bool RoomOps(uint32_t mrea, const uint8_t* scly, size_t size, std::vector< uint8_t >& ops) {
  try {
    PortApWorld::Layout layout;
    if (!SeedLayout(layout))
      return false;
    std::vector< PortSkipCutscenes::ScriptObject > objects;
    if (!PortSkipCutscenes::ScanObjects(scly, size, objects))
      return false;
    ops = PortApWorld::RoomOps(layout, mrea, objects);
    return !ops.empty();
  } catch (...) {
    return false;
  }
}

bool DoorOps(uint32_t mrea, const uint8_t* scly, size_t size, std::vector< uint8_t >& ops) {
  try {
    {
      std::lock_guard< std::mutex > lock(PlacedShieldsMutex());
      PlacedShields().erase(mrea);
    }
    PortApWorld::Layout layout;
    if (!SeedLayout(layout))
      return false;
    uint32_t broken[4];
    const std::vector< PortApWorld::DoorChange > doors =
        PortApWorld::Doors(layout, mrea, BrokenShields(broken));
    std::vector< PortSkipCutscenes::ScriptObject > objects;
    if (doors.empty() || !PortSkipCutscenes::ScanObjects(scly, size, objects))
      return false;
    std::vector< PortApWorld::PlacedShield > placed;
    ops = PortApWorld::DoorOps(
        doors, objects,
        [](const std::string& text) {
          // One scan per text: the key only has to tell the door types apart.
          return PortCustomRes::TextScan(0xD0020000ull << 32 | std::hash< std::string >()(text),
                                         text);
        },
        &placed);
    if (!placed.empty()) {
      std::lock_guard< std::mutex > lock(PlacedShieldsMutex());
      PlacedShields()[mrea] = std::move(placed);
    }
    return !ops.empty();
  } catch (...) {
    return false;
  }
}

bool MapDoors(uint32_t mapa, std::vector< std::pair< uint32_t, int > >& doors) {
  try {
    doors.clear();
    PortApWorld::Layout layout;
    if (!SeedLayout(layout))
      return false;
    uint32_t broken[4];
    for (const PortApWorld::MapDoor& door :
         PortApWorld::MapDoors(layout, mapa, BrokenShields(broken)))
      doors.emplace_back(door.doorId, door.type);
    return !doors.empty();
  } catch (...) {
    return false;
  }
}

int RequiredArtifacts() {
  try {
    PortApWorld::Layout layout;
    return SeedLayout(layout) ? layout.requiredArtifacts : 12;
  } catch (...) {
    return 12;
  }
}

bool VariaOnlyHeatProtection() {
  try {
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    return runtime.enabled && runtime.session != nullptr &&
           runtime.session->GetSlotData().variaOnlyHeat;
  } catch (...) {
    return false;
  }
}

int PreScanElevators() {
  try {
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.enabled)
      return 0;
    if (runtime.session == nullptr || !runtime.session->GetSlotData().received)
      return -1;
    return runtime.session->GetSlotData().preScanElevators ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

bool RandomizedGame() {
  try {
    return BuiltinRules();
  } catch (...) {
    return false;
  }
}

int SpringBallRule() {
  namespace Prime = MetroidPrime;
  try {
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.enabled || !runtime.config.builtin || runtime.session == nullptr ||
        !runtime.session->GetSlotData().received)
      return -1;
    switch (runtime.session->GetSlotData().springBall) {
    case 1:
      return 1;
    case 2:
      return runtime.session->ReceivedCount(Prime::kItemBase + Prime::kSpringBall) > 0 ? 2 : 0;
    case 3:
      return runtime.session->ReceivedCount(Prime::kItemBase + Prime::kProgressiveBomb) > 0 ? 2
                                                                                            : 0;
    default:
      return 0;
    }
  } catch (...) {
    return -1;
  }
}

bool Logic(LogicState& out) {
  try {
    Runtime& runtime = GetRuntime();
    size_t count = 0;
    const MetroidPrime::Location* locations = MetroidPrime::Locations(count);
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (!runtime.enabled || !runtime.config.builtin || runtime.session == nullptr)
        return false;
      const Protocol::State& state = runtime.session->GetState();
      if (!state.hasLogic)
        return false;
      if (!runtime.logicValid || runtime.logicOptions != state.logic ||
          runtime.logicHasWorld != state.hasWorld || runtime.logicWorld != state.world ||
          runtime.logicItems != state.progressive) {
        runtime.logicOptions = state.logic;
        runtime.logicHasWorld = state.hasWorld;
        runtime.logicWorld = state.world;
        runtime.logicItems = state.progressive;
        PortApLogic::Options options = state.logic;
        if (state.hasWorld)
          PortApWorld::FillLogic(state.world, options);
        runtime.logicLevels = PortApLogic::Evaluate(options, state.progressive);
        runtime.logicValid = true;
      }
      out.levels = runtime.logicLevels;
      out.checked.assign(count, false);
      for (size_t i = 0; i < count; ++i) {
        out.checked[i] = std::find(state.checkedLocations.begin(), state.checkedLocations.end(),
                                   locations[i].id) != state.checkedLocations.end();
      }
    }
    // Checks made while disconnected are only in the save so far.
    if (gpGameState != nullptr && gpGameState->PortApProgress().reconciled) {
      const CGameState::ApProgress& progress = gpGameState->PortApProgress();
      for (size_t i = 0; i < count && i < 128; ++i) {
        if (((progress.checked[i / 32] >> (i % 32)) & 1) != 0)
          out.checked[i] = true;
      }
    }
    return out.levels.size() == count;
  } catch (...) {
    return false;
  }
}

std::string LogicText() {
  try {
    LogicState state;
    if (!Logic(state))
      return "logic: no Archipelago seed options yet\n";
    static const char* const kNames[] = {"out of logic", "inspect", "sequence break", "in logic"};
    size_t count = 0;
    const PortApLogic::Check* checks = PortApLogic::Checks(count);
    int totals[4] = {};
    int checked = 0;
    std::string lines;
    for (size_t i = 0; i < count; ++i) {
      if (state.checked[i]) {
        ++checked;
        continue;
      }
      ++totals[static_cast<int>(state.levels[i])];
      if (state.levels[i] == PortApLogic::Level::None)
        continue;
      lines += std::string("check ") + kNames[static_cast<int>(state.levels[i])] + ": " +
               checks[i].area + " / " + checks[i].room +
               (checks[i].section[0] != 0 ? std::string(" / ") + checks[i].section : std::string()) +
               "\n";
    }
    return "logic: " + std::to_string(totals[3]) + " in logic, " + std::to_string(totals[2]) +
           " sequence break, " + std::to_string(totals[1]) + " inspect, " +
           std::to_string(totals[0]) + " out of logic, " + std::to_string(checked) + " checked\n" +
           lines;
  } catch (...) {
    return std::string();
  }
}

void OnInventoryReset() {
  try {
    if (gpGameState == nullptr)
      return;
    CGameState::ApProgress& progress = gpGameState->PortApProgress();
    if (!Enabled() && !IsApGame(progress))
      return;
    progress.appliedIndex = 0;
    progress.reconciled = false; // reconcile again, now holding nothing
  } catch (...) {
  }
}

void Poll(CStateManager& mgr) {
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    CPlayerState* player = mgr.PlayerState();
    if (player == nullptr || gpGameState == nullptr)
      return;
    CGameState::ApProgress& progress = gpGameState->PortApProgress();

    static unsigned int notificationTicks = 0;
    std::deque<ItemGrant> grants;
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (!runtime.enabled || runtime.session == nullptr)
        return;
      ReconcileLocked(runtime, progress);
      grants.swap(runtime.grants);
      const size_t room = static_cast<size_t>(std::max(0, std::numeric_limits<int>::max() -
                                                           runtime.itemCount));
      runtime.itemCount += static_cast<int>(std::min(grants.size(), room));
    }
    for (const ItemGrant& grant : grants) {
      if (grant.index >= 0 && grant.index < std::numeric_limits<uint32_t>::max())
        progress.appliedIndex =
            std::max(progress.appliedIndex, static_cast<uint>(grant.index + 1));
      if (grant.itemType < 0)
        continue;
      const auto type = static_cast<CPlayerState::EItemType>(grant.itemType);
      player->InitializePowerUp(type, grant.capacity);
      player->IncrPickUp(type, grant.amount);
      if (type == CPlayerState::kIT_EnergyTanks)
        player->HealthInfo()->SetHP(player->CalculateHealth());
    }
    ApplyBuiltinWorld(runtime, mgr, *player);
    WatchShields(mgr, progress);

    // DeathLink, inbound. A bounce the server sent is applied by clearing the
    // alive flag, which is what the world's own client does and what drives
    // the whole death sequence here. Bounces that arrive while the game is not
    // running are left pending by the session and picked up on a later tick,
    // so one sent during a load is not lost.
    int deathsOwed = 0;
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (runtime.enabled && runtime.session != nullptr) {
        deathsOwed = runtime.session->TakeDeathPending();
        // Outbound: a death is announced once. The flag stays cleared for
        // several seconds of the death animation, so this is latched rather
        // than sent on every tick, and cleared when the player is alive again.
        const bool alive = player->IsAlive();
        if (alive) {
          runtime.deathAnnounced = false;
        } else if (!runtime.deathAnnounced && runtime.connected) {
          // Only while connected: a death during an outage would otherwise
          // be announced, stale, on the next connect.
          runtime.deathAnnounced = true;
          runtime.pendingBounce = "death";
        }
        // A death that came in over DeathLink is not announced back: every
        // other client would echo it too, and the deaths would go round the
        // multiworld forever.
        if (deathsOwed > 0)
          runtime.deathAnnounced = true;
      }
    }
    for (int i = 0; i < deathsOwed; ++i)
      player->SetPlayerAlive(false);

    if (mgr.GetGameState() != CStateManager::kGS_Running)
      return;
    if (++notificationTicks < 120)
      return;
    notificationTicks = 0;

    std::string notification;
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (!runtime.enabled || runtime.session == nullptr)
        return;
      std::string next;
      while (runtime.session->TakeNotification(next)) {
        runtime.notifications.push_back(std::move(next));
        next.clear();
      }
      while (runtime.notifications.size() > 8)
        runtime.notifications.pop_front();
      if (!runtime.notifications.empty()) {
        notification = std::move(runtime.notifications.front());
        runtime.notifications.pop_front();
      }
    }
    if (!notification.empty()) {
      CSamusHud::DisplayHudMemo(ToHudWide(notification), CHUDMemoParms(5.f, true, false, false));
    }
  } catch (...) {
    // Avoid leaking exceptions into the simulation loop.
  }
}

} // namespace PortAp

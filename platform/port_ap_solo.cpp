#include "port_strings.h"
#include "port_ap_solo.h"

#include "port_ap_metroidprime.h"
#include "port_json.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace PortApSolo {
namespace {

constexpr int64_t kSlot = 1;
constexpr int64_t kGoalComplete = 30; // ClientStatus.CLIENT_GOAL
constexpr int64_t kStartLocation = -2; // what Archipelago gives a starting item

std::string IdList(const std::vector< int64_t >& ids) {
  std::string out = "[";
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i != 0)
      out.push_back(',');
    out += std::to_string(ids[i]);
  }
  out.push_back(']');
  return out;
}

// One line of server text, the way MultiServer words its PrintJSON.
std::string PrintJson(const char* type, const std::string& text, const std::string& extra = "") {
  return std::string("{\"cmd\":\"PrintJSON\",\"type\":\"") + type + "\"," + extra +
         "\"data\":[{\"type\":\"text\",\"text\":" + port::JsonQuote(text) + "}]}";
}

std::vector< int64_t > Ids(const PortJson::Value* list) {
  std::vector< int64_t > ids;
  if (list == nullptr || !list->IsArray())
    return ids;
  for (const PortJson::Value& id : list->AsArray()) {
    if (id.IsNumber())
      ids.push_back(id.AsInt());
  }
  return ids;
}

bool WriteAtomic(const std::string& path, const std::string& text) {
  std::error_code ec;
  const std::filesystem::path target(path);
  std::filesystem::create_directories(target.parent_path(), ec);
  const std::string temp = path + ".tmp";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast< std::streamsize >(text.size()));
    file.flush();
    if (!file) {
      std::filesystem::remove(temp, ec);
      return false;
    }
  }
  std::filesystem::rename(temp, target, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    return false;
  }
  return true;
}

} // namespace

bool ParseServer(const std::string& server, std::string& seedName) {
  const std::string prefix = kServerPrefix;
  if (server.compare(0, prefix.size(), prefix) != 0 || server.size() == prefix.size())
    return false;
  seedName = server.substr(prefix.size());
  return true;
}

std::string StatePath(const std::string& seedName) {
  // SeedPath adds ".json" after making the name safe for a file name.
  std::string path = PortRandoGen::SeedPath(seedName);
  path.resize(path.size() - 5);
  return path + ".state.json";
}

bool Server::Open(const std::string& seedName, std::string& error) {
  PortRandoGen::Seed seed;
  if (!PortRandoGen::Load(PortRandoGen::SeedPath(seedName), seed, error))
    return false;
  return OpenSeed(seed, StatePath(seedName), error);
}

bool Server::OpenSeed(const PortRandoGen::Seed& seed, const std::string& statePath, std::string& error) {
  PortJson::Value slotData;
  size_t offset = 0;
  const char* reason = nullptr;
  if (!PortJson::Parse(seed.slotData, slotData, offset, &reason) || !slotData.IsObject()) {
    error = "seed " + seed.name + ": its slot data is not a JSON object";
    return false;
  }
  std::lock_guard< std::mutex > lock(mMutex);
  mSeed = seed;
  mStatePath = statePath;
  mChecked.clear();
  mReceived.clear();
  mOutgoing.clear();
  mError.clear();
  for (const int64_t item : mSeed.startItems)
    mReceived.push_back({item, kStartLocation});
  // The checks made before, in the order they were made. A location the seed
  // does not have (the seed was regenerated under the same name) is dropped.
  std::string text;
  if (!mStatePath.empty()) {
    std::ifstream file(mStatePath, std::ios::binary);
    text.assign(std::istreambuf_iterator< char >(file), std::istreambuf_iterator< char >());
  }
  PortJson::Value state;
  if (!text.empty() && PortJson::Parse(text, state, offset, &reason) && state.IsObject()) {
    for (const int64_t location : Ids(state.Find("checked")))
      Check(location);
  }
  mOpen = true;
  // The first thing a server says; the client's Connect answers it.
  Queue("{\"cmd\":\"RoomInfo\",\"version\":{\"major\":0,\"minor\":6,\"build\":0,\"class\":\"Version\"},"
        "\"generator_version\":{\"major\":0,\"minor\":6,\"build\":0,\"class\":\"Version\"},"
        "\"tags\":[],\"password\":false,\"permissions\":{\"release\":0,\"collect\":0,\"remaining\":0},"
        "\"hint_cost\":0,\"location_check_points\":1,\"games\":[\"Metroid Prime\"],"
        "\"datapackage_checksums\":{},\"seed_name\":" +
        port::JsonQuote("solo-" + mSeed.name) + ",\"time\":0}");
  return true;
}

bool Server::IsOpen() const {
  std::lock_guard< std::mutex > lock(mMutex);
  return mOpen;
}

void Server::Close() {
  {
    std::lock_guard< std::mutex > lock(mMutex);
    mOpen = false;
  }
  mWake.notify_all();
}

bool Server::ReceiveText(std::string& message, int timeoutMs) {
  std::unique_lock< std::mutex > lock(mMutex);
  mWake.wait_for(lock, std::chrono::milliseconds(std::max(0, timeoutMs)),
                 [this] { return !mOutgoing.empty() || !mOpen; });
  if (!mOutgoing.empty()) {
    message = std::move(mOutgoing.front());
    mOutgoing.pop_front();
    return true;
  }
  mError = mOpen ? "receive timed out" : "connection closed";
  return false;
}

std::vector< int64_t > Server::CheckedLocations() const {
  std::lock_guard< std::mutex > lock(mMutex);
  return mChecked;
}

bool Server::SendText(const std::string& message) {
  PortJson::Value root;
  size_t offset = 0;
  const char* reason = nullptr;
  std::unique_lock< std::mutex > lock(mMutex);
  if (!mOpen) {
    mError = "connection closed";
    return false;
  }
  if (!PortJson::Parse(message, root, offset, &reason)) {
    Queue("{\"cmd\":\"InvalidPacket\",\"type\":\"cmd\",\"original_cmd\":null,\"text\":\"not JSON\"}");
  } else {
    std::vector< PortJson::Value > commands;
    if (root.IsArray())
      commands = root.AsArray();
    else
      commands.push_back(std::move(root));
    for (const PortJson::Value& command : commands)
      Handle(command);
  }
  lock.unlock();
  mWake.notify_all();
  return true;
}

void Server::Queue(const std::string& packet) {
  mOutgoing.push_back("[" + packet + "]");
}

// Archipelago flags: 1 progression, 2 useful, 4 trap. Expansions and tanks are
// the only filler a seed holds; everything else is something a seed's logic
// can need.
int64_t Server::Flags(int64_t item) const {
  namespace Mp = PortAp::MetroidPrime;
  const int64_t offset = item - Mp::kItemBase;
  if (offset == Mp::kMissileExpansion || offset == Mp::kPowerBombExpansion || offset == Mp::kEnergyTank)
    return 0;
  return 1;
}

std::string Server::ItemJson(const Received& received) const {
  return "{\"item\":" + std::to_string(received.item) + ",\"location\":" +
         std::to_string(received.location) + ",\"player\":" + std::to_string(kSlot) +
         ",\"flags\":" + std::to_string(Flags(received.item)) + "}";
}

std::string Server::ReceivedItemsJson(size_t from) const {
  std::string out = "{\"cmd\":\"ReceivedItems\",\"index\":" + std::to_string(from) + ",\"items\":[";
  for (size_t i = from; i < mReceived.size(); ++i) {
    if (i != from)
      out.push_back(',');
    out += ItemJson(mReceived[i]);
  }
  out += "]}";
  return out;
}

bool Server::Check(int64_t location) {
  const auto placed = mSeed.placements.find(location);
  if (placed == mSeed.placements.end() ||
      std::find(mChecked.begin(), mChecked.end(), location) != mChecked.end())
    return false;
  mChecked.push_back(location);
  mReceived.push_back({placed->second, location});
  return true;
}

void Server::SaveState() {
  if (mStatePath.empty())
    return;
  // Items are numbered in check order, so a lost state file renumbers them
  // on the next launch: say so rather than lose it quietly.
  if (!WriteAtomic(mStatePath, "{\"checked\":" + IdList(mChecked) + "}\n"))
    Queue(PrintJson("Tutorial", "Could not save the seed's progress to " + mStatePath));
}

void Server::Handle(const PortJson::Value& packet) {
  const std::string command = packet.StringOr("cmd");
  if (command == "Connect") {
    if (packet.StringOr("name") != kSlotName) {
      Queue("{\"cmd\":\"ConnectionRefused\",\"errors\":[\"InvalidSlot\"]}");
      return;
    }
    if (packet.StringOr("game") != "Metroid Prime") {
      Queue("{\"cmd\":\"ConnectionRefused\",\"errors\":[\"InvalidGame\"]}");
      return;
    }
    std::vector< int64_t > missing;
    for (const auto& [location, item] : mSeed.placements) {
      if (std::find(mChecked.begin(), mChecked.end(), location) == mChecked.end())
        missing.push_back(location);
    }
    // No slot_info: it only exists to have the client fetch other games' names.
    Queue("{\"cmd\":\"Connected\",\"team\":0,\"slot\":" + std::to_string(kSlot) +
          ",\"players\":[{\"team\":0,\"slot\":" + std::to_string(kSlot) + ",\"alias\":" + port::JsonQuote(kSlotName) +
          ",\"name\":" + port::JsonQuote(kSlotName) + "}],\"missing_locations\":" + IdList(missing) +
          ",\"checked_locations\":" + IdList(mChecked) + ",\"slot_data\":" + mSeed.slotData + "}");
    Queue(ReceivedItemsJson(0));
  } else if (command == "LocationChecks") {
    const size_t before = mReceived.size();
    for (const int64_t location : Ids(packet.Find("locations")))
      Check(location);
    // The client repeats its checks on every connection; those add nothing.
    if (mReceived.size() != before) {
      SaveState();
      Queue(ReceivedItemsJson(before));
    }
  } else if (command == "Sync") {
    Queue(ReceivedItemsJson(0));
  } else if (command == "LocationScouts") {
    std::string out = "{\"cmd\":\"LocationInfo\",\"locations\":[";
    bool first = true;
    for (const int64_t location : Ids(packet.Find("locations"))) {
      const auto placed = mSeed.placements.find(location);
      if (placed == mSeed.placements.end())
        continue;
      if (!first)
        out.push_back(',');
      first = false;
      out += ItemJson({placed->second, location});
    }
    out += "]}";
    Queue(out);
  } else if (command == "StatusUpdate") {
    const PortJson::Value* status = packet.Find("status");
    if (status != nullptr && status->AsInt() == kGoalComplete) {
      Queue(PrintJson("Goal", std::string(kSlotName) + " has completed their goal.",
                      "\"team\":0,\"slot\":" + std::to_string(kSlot) + ","));
    }
  } else if (command == "Say") {
    const std::string text = packet.StringOr("text");
    if (!text.empty() && text[0] == '!') {
      Queue(PrintJson("CommandResult", "Commands are not available in a solo game."));
    } else {
      Queue(PrintJson("Chat", std::string(kSlotName) + ": " + text,
                      "\"team\":0,\"slot\":" + std::to_string(kSlot) + ",\"message\":" + port::JsonQuote(text) + ","));
    }
  } else if (command == "Get" || command == "SetNotify") {
    // Nothing is stored in a solo game; a key asked for is just empty.
    std::string out = "{\"cmd\":\"Retrieved\",\"keys\":{";
    const PortJson::Value* keys = packet.Find("keys");
    bool first = true;
    if (keys != nullptr && keys->IsArray()) {
      for (const PortJson::Value& key : keys->AsArray()) {
        if (!key.IsString())
          continue;
        if (!first)
          out.push_back(',');
        first = false;
        out += port::JsonQuote(key.AsString()) + ":null";
      }
    }
    out += "}}";
    Queue(out);
  } else if (command == "GetDataPackage") {
    // The client has the built-in names for Metroid Prime already.
    Queue("{\"cmd\":\"DataPackage\",\"data\":{\"games\":{}}}");
  }
  // Bounce, ConnectUpdate and the rest have nobody to be relayed to.
}

} // namespace PortApSolo

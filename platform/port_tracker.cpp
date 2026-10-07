#include "port_tracker.h"

#include "port_discord.h"

#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/TToken.hpp"
#include "Kyoto/Text/CStringTable.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CMapWorldInfo.hpp"
#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/CWorldSaveGameInfo.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/Player/CWorldState.hpp"
#include "rstl/optional_object.hpp"

#include <cstdio>
#include <memory>
#include <unordered_map>

namespace PortTracker {
namespace {

typedef rstl::optional_object< TCachedToken< CStringTable > > NameToken;

// The current world's room name tables, one per area.
struct SRoomNames {
  CAssetId worldId;
  std::vector< NameToken > tokens;
  std::vector< std::string > names;
  SRoomNames() : worldId(kInvalidAssetId) {}
};
std::unique_ptr< SRoomNames > sRooms;

struct SUpgrade {
  const char* name;
  CPlayerState::EItemType type;
};
// In pause-screen inventory order; the power beam, combat visor and power
// suit are always held.
const SUpgrade kUpgrades[] = {
    {"Charge Beam", CPlayerState::kIT_ChargeBeam},
    {"Ice Beam", CPlayerState::kIT_IceBeam},
    {"Wave Beam", CPlayerState::kIT_WaveBeam},
    {"Plasma Beam", CPlayerState::kIT_PlasmaBeam},
    {"Super Missile", CPlayerState::kIT_SuperMissile},
    {"Wavebuster", CPlayerState::kIT_Wavebuster},
    {"Ice Spreader", CPlayerState::kIT_IceSpreader},
    {"Flamethrower", CPlayerState::kIT_Flamethrower},
    {"Grapple Beam", CPlayerState::kIT_GrappleBeam},
    {"Scan Visor", CPlayerState::kIT_ScanVisor},
    {"Thermal Visor", CPlayerState::kIT_ThermalVisor},
    {"X-Ray Visor", CPlayerState::kIT_XRayVisor},
    {"Morph Ball", CPlayerState::kIT_MorphBall},
    {"Morph Ball Bomb", CPlayerState::kIT_MorphBallBombs},
    {"Boost Ball", CPlayerState::kIT_BoostBall},
    {"Spider Ball", CPlayerState::kIT_SpiderBall},
    {"Space Jump Boots", CPlayerState::kIT_SpaceJumpBoots},
    {"Varia Suit", CPlayerState::kIT_VariaSuit},
    {"Gravity Suit", CPlayerState::kIT_GravitySuit},
    {"Phazon Suit", CPlayerState::kIT_PhazonSuit},
};

std::string WideName(const wchar_t* wide) {
  return PortDiscord::GameTextToUtf8(wide);
}

// Loads (or keeps loading) the name of every room in `world`; returns how many
// are still loading.
int UpdateRoomNames(const CWorld& world) {
  if (!sRooms) {
    sRooms.reset(new SRoomNames);
  }
  SRoomNames& rooms = *sRooms;
  const int count = world.IGetAreaCount();
  if (rooms.worldId != world.IGetWorldAssetId() || static_cast< int >(rooms.tokens.size()) != count) {
    rooms.worldId = world.IGetWorldAssetId();
    rooms.tokens.clear();
    rooms.names.clear();
    rooms.tokens.resize(count);
    rooms.names.resize(count);
    for (int i = 0; i < count; ++i) {
      const IGameArea* area = world.IGetAreaAlways(TAreaId(i));
      const CAssetId id = area != nullptr ? area->IGetStringTableAssetId() : kInvalidAssetId;
      if (id != kInvalidAssetId) {
        rooms.tokens[i] = TCachedToken< CStringTable >(gpSimplePool->GetObj(SObjectTag('STRG', id)));
        rooms.tokens[i]->Lock();
      }
    }
  }
  int loading = 0;
  for (int i = 0; i < count; ++i) {
    NameToken& token = rooms.tokens[i];
    if (!token || !rooms.names[i].empty()) {
      continue;
    }
    if (!token->TryCache()) {
      ++loading;
      continue;
    }
    if (token->GetObject()->GetStringCount() > 0) {
      rooms.names[i] = PortDiscord::GameTextToUtf8(token->GetObject()->GetString(0));
    }
    // Loaded (named or not): the table is not needed any more.
    token = NameToken();
  }
  return loading;
}

} // namespace

const std::vector< std::string >& RoomNames(const CWorld& world) {
  UpdateRoomNames(world);
  return sRooms->names;
}

const char* ScanGroupName(int group) {
  static const char* const names[kScan_Count] = {"Pirate Data", "Chozo Lore", "Creatures",
                                                 "Research", "Artifacts"};
  return group >= 0 && group < kScan_Count ? names[group] : "";
}

Summary Collect(const CStateManager& mgr) {
  Summary out;
  const CPlayerState& ps = *mgr.GetPlayerState();
  const CWorld* world = mgr.GetWorld();
  const CAssetId currentWorld =
      world != nullptr ? world->IGetWorldAssetId() : static_cast< CAssetId >(kInvalidAssetId);

  if (gpMemoryCard != nullptr && gpGameState != nullptr) {
    const rstl::vector< CMemoryCard::MemoryWorld >& worlds = gpMemoryCard->GetMemoryWorlds();
    for (int i = 0; i < worlds.size(); ++i) {
      World entry;
      entry.id = static_cast< unsigned >(worlds[i].first);
      entry.name = WideName(worlds[i].second.GetFrontEndName());
      // The end cinema is a nameless one-room world; it isn't somewhere to
      // explore.
      if (entry.name.empty() && worlds[i].second.GetAreaCount() <= 1) {
        continue;
      }
      if (entry.name.empty()) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "MLVL %08X", entry.id);
        entry.name = buf;
      }
      entry.total = worlds[i].second.GetAreaCount();
      // Retail's universe map makes the same call, which adds the world's
      // (empty) state if it has none yet.
      const rstl::rc_ptr< CMapWorldInfo > info =
          gpGameState->StateForWorld(worlds[i].first).GetMapWorldInfo();
      for (int a = 0; a < entry.total; ++a) {
        if (info->IsAreaVisited(TAreaId(a))) {
          ++entry.visited;
        }
      }
      entry.mapStation = info->GetMapStationUsed();
      entry.current = worlds[i].first == currentWorld;
      if (entry.current) {
        out.currentWorld = entry.name;
      }
      out.worlds.push_back(entry);
    }

    // Scans complete as the logbook counts them (artifact totems at half).
    std::unordered_map< uint32_t, float > times;
    const rstl::vector< rstl::pair< CAssetId, float > >& scanTimes = ps.GetScanTimes();
    for (int i = 0; i < scanTimes.size(); ++i) {
      times[static_cast< uint32_t >(scanTimes[i].first)] = scanTimes[i].second;
    }
    const rstl::vector< CMemoryCard::ScanState >& scans = gpMemoryCard->GetScanStates();
    for (int i = 0; i < scans.size(); ++i) {
      const int group = static_cast< int >(scans[i].second) - CWorldSaveGameInfo::kSC_Data;
      if (group < 0 || group >= kScan_Count) {
        continue;
      }
      const std::unordered_map< uint32_t, float >::const_iterator it =
          times.find(static_cast< uint32_t >(scans[i].first));
      const float time = it != times.end() ? it->second : 0.f;
      const bool done = time >= (group == kScan_Artifact ? 0.5f : 1.f);
      ++out.scans[group].total;
      ++out.scanTotal.total;
      if (done) {
        ++out.scans[group].have;
        ++out.scanTotal.have;
      }
    }
  }

  for (const SUpgrade& upgrade : kUpgrades) {
    (ps.HasPowerUp(upgrade.type) ? out.upgradesHeld : out.upgradesMissing).push_back(upgrade.name);
  }
  out.energyTanks.have = ps.GetItemCapacity(CPlayerState::kIT_EnergyTanks);
  out.energyTanks.total = 14;
  out.missileExpansions.have = ps.GetItemCapacity(CPlayerState::kIT_Missiles) / 5;
  out.missileExpansions.total = 50;
  const int powerBombs = ps.GetItemCapacity(CPlayerState::kIT_PowerBombs);
  out.powerBombExpansions.have = powerBombs >= 4 ? 1 + (powerBombs - 4) : 0;
  out.powerBombExpansions.total = 5;
  for (int i = CPlayerState::kIT_Truth; i <= CPlayerState::kIT_Newborn; ++i) {
    if (ps.HasPowerUp(static_cast< CPlayerState::EItemType >(i))) {
      ++out.artifacts.have;
    }
  }
  out.artifacts.total = 12;
  out.itemPercent = ps.CalculateItemCollectionPercentage();

  if (world != nullptr && gpGameState != nullptr) {
    out.roomNamesLoading = UpdateRoomNames(*world);
    const rstl::rc_ptr< CMapWorldInfo > info =
        gpGameState->StateForWorld(currentWorld).GetMapWorldInfo();
    const int count = world->IGetAreaCount();
    for (int i = 0; i < count; ++i) {
      Room room;
      room.index = i;
      room.name = sRooms->names[i];
      if (room.name.empty()) {
        room.name = "Room " + std::to_string(i);
      }
      room.visited = info->IsAreaVisited(TAreaId(i));
      out.rooms.push_back(room);
    }
  }
  return out;
}

void Reset() { sRooms.reset(); }

std::string Text(const Summary& summary) {
  std::string out;
  char line[256];
  std::snprintf(line, sizeof(line), "items %d%%\n", summary.itemPercent);
  out += line;
  for (const World& world : summary.worlds) {
    std::snprintf(line, sizeof(line), "world %08X %s: rooms %d/%d%s%s\n", world.id,
                  world.name.c_str(), world.visited, world.total,
                  world.mapStation ? ", map station" : "", world.current ? " (here)" : "");
    out += line;
  }
  for (int i = 0; i < kScan_Count; ++i) {
    std::snprintf(line, sizeof(line), "scans %s: %d/%d\n", ScanGroupName(i),
                  summary.scans[i].have, summary.scans[i].total);
    out += line;
  }
  std::snprintf(line, sizeof(line), "scans total: %d/%d\n", summary.scanTotal.have,
                summary.scanTotal.total);
  out += line;
  std::snprintf(line, sizeof(line),
                "energy tanks %d/%d, missiles %d/%d, power bombs %d/%d, artifacts %d/%d\n",
                summary.energyTanks.have, summary.energyTanks.total, summary.missileExpansions.have,
                summary.missileExpansions.total, summary.powerBombExpansions.have,
                summary.powerBombExpansions.total, summary.artifacts.have, summary.artifacts.total);
  out += line;
  std::string missing;
  for (const std::string& name : summary.upgradesMissing) {
    missing += (missing.empty() ? "" : ", ") + name;
  }
  out += "upgrades " + std::to_string(summary.upgradesHeld.size()) + "/" +
         std::to_string(summary.upgradesHeld.size() + summary.upgradesMissing.size()) +
         (missing.empty() ? std::string() : ", missing: " + missing) + "\n";
  int unvisited = 0;
  std::string rooms;
  for (const Room& room : summary.rooms) {
    if (!room.visited) {
      ++unvisited;
      rooms += "  " + std::to_string(room.index) + " " + room.name + "\n";
    }
  }
  std::snprintf(line, sizeof(line), "unvisited in %s: %d%s\n", summary.currentWorld.c_str(),
                unvisited,
                summary.roomNamesLoading > 0 ? " (some names still loading)" : "");
  out += line;
  out += rooms;
  if (!out.empty() && out.back() == '\n') {
    out.pop_back();
  }
  return out;
}

} // namespace PortTracker

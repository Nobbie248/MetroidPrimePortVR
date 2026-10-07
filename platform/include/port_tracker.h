#pragma once

#include <string>
#include <vector>

class CStateManager;
class CWorld;

// Progress tracker for the F1 overlay and the debug console: rooms visited per
// world, scans per logbook category, upgrades held and missing, expansions,
// and the current world's rooms by name. Read-only: nothing here touches the
// save.
namespace PortTracker {

struct World {
  std::string name;
  unsigned id = 0;
  int visited = 0;
  int total = 0;
  bool mapStation = false;
  bool current = false;
};

struct Count {
  int have = 0;
  int total = 0;
};

struct Room {
  std::string name; // "Room 12" until its name table loads
  int index = 0;
  bool visited = false;
};

// The logbook's categories, in its order.
enum EScanGroup { kScan_Data, kScan_Lore, kScan_Creature, kScan_Research, kScan_Artifact, kScan_Count };
const char* ScanGroupName(int group);

struct Summary {
  std::vector< World > worlds;
  Count scans[kScan_Count];
  Count scanTotal;
  std::vector< std::string > upgradesHeld;
  std::vector< std::string > upgradesMissing;
  Count energyTanks;
  Count missileExpansions; // the launcher counts as one
  Count powerBombExpansions; // the main Power Bomb counts as one
  Count artifacts;
  int itemPercent = 0;
  std::string currentWorld;
  std::vector< Room > rooms; // the current world's
  int roomNamesLoading = 0;
};

// Builds the summary from the live game; call on the main thread with a
// running state manager. Room names load in the background over a few calls.
Summary Collect(const CStateManager& mgr);
// The names of `world`'s rooms by area index, empty for any still loading.
const std::vector< std::string >& RoomNames(const CWorld& world);
// Drops the room name tables (called when the state manager goes away, since a
// world change retires the PAKs they come from).
void Reset();
// Plain-text form for the console.
std::string Text(const Summary& summary);

} // namespace PortTracker

#pragma once

#include <cstdint>
#include <string>
#include <vector>

class CStateManager;
class CInputStream;

// Save states: save anywhere, load back to the same spot. A state is the
// retail save data (the same CGameState stream a memory card save holds:
// items, health, ammo, map, scans, script layers and relays, in-game time)
// plus where Samus stood, how she faced and whether she was in morph ball.
// Loading rebuilds the world from that save, as the debug world warp does,
// then puts Samus back. Enemies, doors and puzzles come back the way a
// memory card load leaves them, not mid-fight.
namespace PortSaveState {

constexpr int kSlotCount = 8;  // slots 1..8
constexpr int kUndoSlot = 0;   // written before every load

struct Info {
  bool exists = false;
  std::string world;       // front-end name
  std::string room;        // room name, or "Room N"
  double playTime = 0.0;   // in-game seconds
  int64_t savedAt = 0;     // Unix seconds
  bool morphed = false;
};

// The fixed header ahead of the game-state blob. Pure, for the unit test.
struct Header {
  uint32_t worldId = 0;
  int32_t areaId = 0;
  float position[3] = {0.f, 0.f, 0.f};
  float forward[3] = {0.f, 1.f, 0.f};
  bool morphed = false;
  double playTime = 0.0;
  int64_t savedAt = 0;
  std::string world;
  std::string room;
};
std::string Encode(const Header& header, const std::vector< uint8_t >& blob);
bool Decode(const std::string& data, Header& header, std::vector< uint8_t >& blob);

// Port glue (port_savestate.cpp); main thread only.

// The folder states live in (created on first use).
std::string Folder();
Info SlotInfo(int slot);
// The slot F5/F9 use.
int SelectedSlot();
void SetSelectedSlot(int slot);

// Save or load from the overlay, a hotkey or the console. They run at the
// next game tick; the pause and map screens hold them until you unpause.
// A request with no game running is refused at once.
bool RequestSave(int slot); // false (and LastMessage says why) if refused now
bool RequestLoad(int slot);
// Reads the mods folder again (and installs a finished Remastered import):
// the game is rebuilt where Samus stands, as by a save and a load, with every
// PAK reopened. In the front end CFrontEndUI does it at its next tick; with
// no game running otherwise, the next one to start does it.
bool RequestModReload();
// How many times the mod files were read again, to tell when a request ran.
int ModReloads();
// What the last request did, for the overlay and the console.
std::string LastMessage();

// CStateManager::Update: does a queued save, places Samus after a load, and
// returns true when a load has started (the caller must return at once: the
// world is going away).
bool Tick(CStateManager& mgr);
// CMainFlow, before the next game loads: swaps in the loaded game state.
void InstallPending();
// CMainFlow: whether the front end (title screen, file select) is the flow.
void SetInFrontEnd(bool inFrontEnd);
// CFrontEndUI, each tick: true (once) when a reload was asked for in the
// front end. It closes its movies, calls ReloadModsNow and opens them again,
// since rebuilding the disc-file overlays breaks the reads of open files.
bool TakeFrontEndReload();
void ReloadModsNow();

} // namespace PortSaveState

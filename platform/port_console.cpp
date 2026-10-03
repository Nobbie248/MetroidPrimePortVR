// Debug command console: with
// MP_CONSOLE=<port> (1 = 4777) the game listens on 127.0.0.1 and runs one
// command per line, e.g. `warp 83F6FF6F 492CBF4A`, `objs EyeBall`, `shot`.
// Every reply ends with a line `=> ok` or `=> err: <why>`. tools/mpcon.py is
// the client. Commands that touch the game run inside the state manager's
// tick, where every object pointer is live; the rest run once per frame.
#include "port_apclient.h"
#include "port_debug.h"
#include "port_discord.h"
#include "port_freecam.h"
#include "port_hd_font.h"
#include "port_livesplit.h"
#include "port_remastered_import.h"
#include "port_room_env.h"
#include "port_room_geo.h"
#include "port_room_liquid.h"
#include "port_console.h"
#include "port_mods.h"
#include "port_savestate.h"
#include "port_tracker.h"
#include "port_viewmodel.h"
#include "touch_pad.h"
#include "Kyoto/CResFactory.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/Graphics/CCubeMaterial.hpp"
#include "Kyoto/TToken.hpp"
#include "Kyoto/Alloc/CMemorySys.hpp"
#include "Kyoto/Alloc/IAllocator.hpp"
#include "Kyoto/Text/CStringTable.hpp"
#include "MetroidPrime/CActor.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/CObjectList.hpp"
#include "MetroidPrime/CPhysicsActor.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/BodyState/CBodyController.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#include "MetroidPrime/Enemies/CPatterned.hpp"
#include "MetroidPrime/HUD/CSamusHud.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerGun.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include <aurora/gfx.h>
#include <dolphin/pad.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>
#if defined(__GNUC__)
#include <cxxabi.h>
#endif
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace aurora {
void request_screenshot() noexcept;
}

namespace {

// Frames a game command waits for a state manager tick before giving up.
constexpr unsigned kTickTimeout = 180;
constexpr unsigned kWarpTimeout = 6000;

struct Incoming {
  std::string line;
  unsigned generation;
};

std::mutex sQueueMutex;
std::deque< Incoming > sQueue;
std::mutex sClientMutex;
int sClient = -1;
unsigned sGeneration = 0;
bool sStarted = false;
bool sEnabled = false;

void SendRaw(unsigned generation, const std::string& text) {
#ifndef _WIN32
  std::lock_guard< std::mutex > lock(sClientMutex);
  if (sClient < 0 || generation != sGeneration) {
    return;
  }
  size_t done = 0;
  while (done < text.size()) {
    const ssize_t n = send(sClient, text.data() + done, text.size() - done, MSG_NOSIGNAL);
    if (n <= 0) {
      return;
    }
    done += static_cast< size_t >(n);
  }
#else
  (void)generation;
  (void)text;
#endif
}

#ifndef _WIN32
void ListenThread(int listener) {
  std::string buffer;
  for (;;) {
    const int client = accept(listener, nullptr, nullptr);
    if (client < 0) {
      continue;
    }
    unsigned generation;
    {
      std::lock_guard< std::mutex > lock(sClientMutex);
      if (sClient >= 0) {
        close(sClient);
      }
      sClient = client;
      generation = ++sGeneration;
    }
    std::fprintf(stderr, "[console] client connected\n");
    buffer.clear();
    char chunk[512];
    for (;;) {
      const ssize_t n = recv(client, chunk, sizeof(chunk), 0);
      if (n <= 0) {
        break;
      }
      buffer.append(chunk, static_cast< size_t >(n));
      size_t eol;
      while ((eol = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, eol);
        buffer.erase(0, eol + 1);
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        std::lock_guard< std::mutex > lock(sQueueMutex);
        sQueue.push_back({line, generation});
      }
    }
    std::lock_guard< std::mutex > lock(sClientMutex);
    if (sClient == client) {
      close(sClient);
      sClient = -1;
    }
    std::fprintf(stderr, "[console] client disconnected\n");
  }
}
#endif

void Start() {
  sStarted = true;
  const char* value = std::getenv("MP_CONSOLE");
  if (value == nullptr || value[0] == '\0' || std::strcmp(value, "0") == 0) {
    return;
  }
#ifdef _WIN32
  std::fputs("[console] MP_CONSOLE is not supported on Windows\n", stderr);
#else
  int port = std::atoi(value);
  if (port <= 1) {
    port = 4777;
  }
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  const int yes = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast< uint16_t >(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (listener < 0 || bind(listener, reinterpret_cast< sockaddr* >(&addr), sizeof(addr)) != 0 ||
      listen(listener, 1) != 0) {
    std::fprintf(stderr, "[console] cannot listen on 127.0.0.1:%d: %s\n", port, std::strerror(errno));
    if (listener >= 0) {
      close(listener);
    }
    return;
  }
  std::fprintf(stderr, "[console] listening on 127.0.0.1:%d\n", port);
  sEnabled = true;
  std::thread(ListenThread, listener).detach();
#endif
}

// ---------------------------------------------------------------------------
// Command state

enum class Where { Frame, Tick };

struct Command {
  std::vector< std::string > args;
  unsigned generation = 0;
  unsigned startFrame = 0;
  std::string out;
  // Continuations: a command that spans frames sets these.
  int phase = 0;
  unsigned untilFrame = 0;
  unsigned ticks = 0;
  uint32_t warpWorld = 0;
  uint32_t warpArea = 0;
  PADStatus pad{};
  std::string shotDir;
  size_t shotCount = 0;
};

bool sHasCommand = false;
Command sCmd;
unsigned sFrame = 0;
bool sQuit = false;

void Out(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  sCmd.out += buf;
  sCmd.out += '\n';
}

void Finish(const char* error = nullptr) {
  std::string text = sCmd.out;
  if (error == nullptr) {
    text += "=> ok\n";
  } else {
    text += "=> err: ";
    text += error;
    text += '\n';
  }
  SendRaw(sCmd.generation, text);
  sHasCommand = false;
}

std::string Lower(std::string s) {
  for (char& c : s) {
    c = static_cast< char >(std::tolower(static_cast< unsigned char >(c)));
  }
  return s;
}

bool ParseHex(const std::string& s, uint32_t& value) {
  if (s.empty()) {
    return false;
  }
  char* end = nullptr;
  const unsigned long v = std::strtoul(s.c_str(), &end, 16);
  if (*end != '\0') {
    return false;
  }
  value = static_cast< uint32_t >(v);
  return true;
}

bool ParseFloat(const std::string& s, float& value) {
  char* end = nullptr;
  value = std::strtof(s.c_str(), &end);
  return !s.empty() && *end == '\0';
}

bool ParseUnsigned(const std::string& s, unsigned& value) {
  char* end = nullptr;
  const unsigned long v = std::strtoul(s.c_str(), &end, 10);
  if (s.empty() || *end != '\0') {
    return false;
  }
  value = static_cast< unsigned >(v);
  return true;
}

std::string ClassName(const CEntity& ent) {
  const char* raw = typeid(ent).name();
#if defined(__GNUC__)
  int status = 0;
  char* demangled = abi::__cxa_demangle(raw, nullptr, nullptr, &status);
  if (status == 0 && demangled != nullptr) {
    std::string name = demangled;
    std::free(demangled);
    return name;
  }
#endif
  return raw;
}

const char* const kMessageNames[] = {
    "UNKM0", "Activate", "Arrived", "Close", "Deactivate", "Decrement", "Follow",
    "Increment", "Next", "Open", "Reset", "ResetAndStart", "SetToMax", "SetToZero",
    "Start", "Stop", "StopAndReset", "ToggleActive", "UNKM18", "Action", "Play",
    "Alert", "InternalMessage00", "OnFloor", "InternalMessage02", "InternalMessage03",
    "Falling", "OnIceSurface", "OnMudSlowSurface", "OnNormalSurface", "Touched",
    "AddPlatformRider", "LandOnNotFloor", "Registered", "Deleted", "InitializedInArea",
    "WorldInitialized", "AddSplashInhabitant", "UpdateSplashInhabitant",
    "RemoveSplashInhabitant", "Jumped", "Damage", "InvulnDamage", "ProjectileCollide",
    "InSnakeWeed", "AddPhazonPoolInhabitant", "UpdatePhazonPoolInhabitant",
    "RemovePhazonPoolInhabitant", "SuspendedMove",
};
constexpr int kMessageCount = sizeof(kMessageNames) / sizeof(kMessageNames[0]);

const char* const kStateNames[] = {
    "Active", "Arrived", "Closed", "Entered", "Exited", "Inactive", "Inside",
    "MaxReached", "Open", "Zero", "Attack", "CloseIn", "Retreat", "Patrol", "Dead",
    "CameraPath", "CameraTarget", "DeactivateState", "Play", "MassiveDeath",
    "DeathRattle", "AboutToMassivelyDie", "Damage", "InvulnDamage", "MassiveFrozenDeath",
    "Modify", "ScanStart", "ScanProcessing", "ScanDone", "UnFrozen", "Default",
    "ReflectedDamage", "InheritBounds",
};
constexpr int kStateCount = sizeof(kStateNames) / sizeof(kStateNames[0]);

const char* const kItemNames[] = {
    "PowerBeam", "IceBeam", "WaveBeam", "PlasmaBeam", "Missiles", "ScanVisor",
    "MorphBallBombs", "PowerBombs", "Flamethrower", "ThermalVisor", "ChargeBeam",
    "SuperMissile", "GrappleBeam", "XRayVisor", "IceSpreader", "SpaceJumpBoots",
    "MorphBall", "CombatVisor", "BoostBall", "SpiderBall", "PowerSuit", "GravitySuit",
    "VariaSuit", "PhazonSuit", "EnergyTanks", "UnknownItem1", "HealthRefill",
    "UnknownItem2", "Wavebuster", "Truth", "Strength", "Elder", "Wild", "Lifegiver",
    "Warrior", "Chozo", "Nature", "Sun", "World", "Spirit", "Newborn",
};
constexpr int kItemCount = sizeof(kItemNames) / sizeof(kItemNames[0]);

int LookupName(const std::string& arg, const char* const* names, int count) {
  unsigned number;
  if (ParseUnsigned(arg, number)) {
    return number < static_cast< unsigned >(count) ? static_cast< int >(number) : -1;
  }
  const std::string want = Lower(arg);
  for (int i = 0; i < count; ++i) {
    if (Lower(names[i]) == want) {
      return i;
    }
  }
  return -1;
}

const char* NameOr(const char* const* names, int count, int index) {
  return index >= 0 && index < count ? names[index] : "?";
}

std::string WorldName(int index) {
  std::string name;
  const wchar_t* wide = gpMemoryCard->GetMemoryWorlds()[index].second.GetFrontEndName();
  for (const wchar_t* p = wide; p != nullptr && *p != 0; ++p) {
    name.push_back(static_cast< char >(*p));
  }
  return name;
}

// An object by editor id (hex, as in the level data), by unique id (`u` +
// decimal index), or by exact debug name.
CEntity* FindObject(CStateManager& mgr, const std::string& arg) {
  const CObjectList& list = mgr.GetObjectListById(kOL_All);
  const bool byUid = arg.size() > 1 && (arg[0] == 'u' || arg[0] == 'U');
  unsigned uid = 0;
  if (byUid && !ParseUnsigned(arg.substr(1), uid)) {
    return nullptr;
  }
  uint32_t eid = 0;
  const bool byEid = !byUid && ParseHex(arg, eid);
  for (int i = list.GetFirstObjectIndex(); i != -1; i = list.GetNextObjectIndex(i)) {
    CEntity* ent = const_cast< CEntity* >(list[i]);
    if (ent == nullptr) {
      continue;
    }
    if (byUid ? ent->GetUniqueId().Value() == uid
              : byEid ? ent->GetEditorId().Value() == (eid & 0x3FFFFFF)
                      : std::strcmp(ent->GetDebugName().data(), arg.c_str()) == 0) {
      return ent;
    }
  }
  return nullptr;
}

std::string Describe(CStateManager& mgr, CEntity& ent) {
  char buf[512];
  std::snprintf(buf, sizeof(buf), "u%-4u %08X %-9s %-26s %-28s", ent.GetUniqueId().Value(),
                ent.GetEditorId().Value(), ent.GetActive() ? "active" : "inactive",
                ClassName(ent).c_str(), ent.GetDebugName().data());
  std::string line = buf;
  if (const CActor* actor = TCastToConstPtr< CActor >(&ent)) {
    const CVector3f pos = actor->GetTranslation();
    const float dist = (pos - mgr.GetPlayer()->GetTranslation()).Magnitude();
    std::snprintf(buf, sizeof(buf), " pos=(%.1f, %.1f, %.1f) dist=%.1f", pos.GetX(), pos.GetY(),
                  pos.GetZ(), dist);
    line += buf;
  }
  return line;
}

// ---------------------------------------------------------------------------
// Commands

void CmdHelp() {
  Out("status                     world, area, player position, health, visor, beam");
  Out("worlds                     world ids and names");
  Out("areas                      areas of the current world (index, MREA)");
  Out("warp <world> [mrea]        load a world (hex MLVL id or name prefix), optionally an area");
  Out("enter <area>               make a loaded area current, as crossing its dock does");
  Out("tp <x> <y> <z>             move the player");
  Out("face <yaw deg> | look <id> turn the player (yaw 0 = +y, 90 = -x)");
  Out("objs [filter]              objects whose class or name contains filter");
  Out("obj <id>                   one object: state, health, animation, connections");
  Out("send <id> <msg>            deliver a script message (name or number; relays fire on SetToZero)");
  Out("give <item> [n]            add an item (name or number, see `items`)");
  Out("take <item> [n]            remove item capacity (drops a suit)");
  Out("items                      the player's inventory");
  Out("heal                       refill health");
  Out("god [on|off]               the player takes no damage (no argument: show)");
  Out("memo <text>                show text as a HUD message");
  Out("strg <id> [index]          a string table as the game loads it (mods included)");
  Out("press <a+b+...> [frames]   hold pad buttons (a b x y z l r start up down left right;");
  Out("                           sx:<n> sy:<n> cx:<n> cy:<n> also hold a stick axis;");
  Out("                           frames 0 = keep holding until the next press/stick)");
  Out("stick <x> <y> [frames]     hold the main stick (-127..127); cstick for the C stick");
  Out("gyro <pitch> [yaw] [frames] fake gyro rates in rad/s (pitch > 0 tilts up; a flick is ~6)");
  Out("shot                       take a screenshot and print its path");
  Out("wait <frames>              let frames pass");
  Out("probe [off|on|mirror|window]   the PBR reflection probe, or what PBR surfaces show of it");
  Out("roomgeo [on|off|overlay]  the room geometry mods supply, in place of the area's own or on top of it");
  Out("roomgeo at <x> <y> <z> [margin] | hide <cmdl> | show [cmdl]   its instances at a point; stop drawing one");
  Out("roomliquid [on|off]       the water, poison and lava surfaces mods supply, in place of the game's");
  Out("roomgeo lights on|off     light it with the area's lights even where the room has baked light");
  Out("roomgeo pick              the instances the middle of the view looks through, nearest first, and the");
  Out("                           first one's materials");
  Out("roomgeo mats <cmdl>       a loaded model's materials: flags, PBR or TEV, the PBR record");
  Out("roomgeo mat <cmdl> <material> <field> <value...> | mat clear");
  Out("                           draw a material with a record value replaced: emissive, backlight, height,");
  Out("                           mode, kind, strength, p0..p3, or the value's index (0 to 18)");
  Out("roomenv info [<x> <y> <z>] exposure, tone curve, probe and baked ambient at the view or a point");
  Out("view [off|albedo|normal|rough|metal|ao|ambient|reflection|glow|exposure|kind]");
  Out("                           what PBR surfaces show in place of their shaded result");
  Out("stats                      the last frame's draws and buffers, the heap, room geometry and environments");
  Out("roomenv [on|off|exposure on|off]  the room environments mods supply; exposure: by room, not by cube");
  Out("roomenv volume on|off | ambient <scale> | show off|coords|light");
  Out("                           the baked light per pixel; the baked ambient's weight (0: the game's);");
  Out("                           draw the volume's coordinates or its light alone on room geometry");
  Out("hdfont [on|off]            the distance-field font mods supply, in place of the disc's glyphs");
  Out("touchpad [attach|detach|stick <x> <y>]  a virtual gamepad like Android's touch overlay");
  Out("freecam [on|off|freeze on|off|player on|off|speed <n>|pos <x> <y> <z>|look <yaw> <pitch>]");
  Out("                           fly the view away from the player (HUD hidden; freeze holds the game still)");
  Out("aspect <4:3|16:9|window>   switch the rendering aspect, as the Options row does");
  Out("fov <45..90>               first-person vertical FOV, as the Options row does");
  Out("msaa <1|4>, aniso <1..16>  anti-aliasing and anisotropic filtering, applied next frame");
  Out("hudscale <50..100>         HUD scale in percent, as the Options row does");
  Out("crosshair <25..100>        mouse/twin-stick crosshair size in percent");
  Out("helmet <0|1>, visorfx <0|1> show (1) or hide (0) the helmet and visor effects");
  Out("interp [actor|pose|particle|all <0|1>]   frame interpolation settings (F1 Performance)");
  Out("present <0..1|cycle|tick|off> force the presentation factor (also MP_PRESENT_T);");
  Out("                           tick draws the plain tick state");
  Out("hold <0|1>, step [ticks]   stop the simulation; step runs ticks one per frame");
  Out("reveal <0|1>               reveal every world's map, as the Options row does");
  Out("pickups <0|1>              white dots on the map for uncollected pickups");
  Out("tracker                    items, scans and rooms visited (the F1 Tracker tab)");
  Out("viewmodel <cmdl> [dist] [yaw] [pitch] | off | status | light <0|1>   draw a model in front of the camera");
  Out("state list | last | save [n] | load [n] | undo | slot <n>   save states (F1 States tab)");
  Out("timer <0|1>                on-screen in-game time; igt <seconds> sets the play time");
  Out("livesplit <0|1> | addr <host:port> | send <command> | status   LiveSplit Server client");
  Out("discord <0|1> | id <application id> | status   Discord Rich Presence");
  Out("gci list | import <path> | export <dir or .raw> | dolphin import|export   memory card transfer");
  Out("ap [connect <server> <slot> [password] | disconnect | recent | resume <n> | say <text> | chat]   Archipelago, as the F1 Session tab does");
  Out("quit                       exit the game");
  Out("ids: hex editor id (002900A1), u<index> unique id, or an exact debug name");
}

void CmdWorlds() {
  const auto& worlds = gpMemoryCard->GetMemoryWorlds();
  const uint32_t current = gpGameState != nullptr ? gpGameState->CurrentWorldAssetId() : 0;
  for (int i = 0; i < worlds.size(); ++i) {
    const uint32_t id = static_cast< uint32_t >(worlds[i].first);
    Out("%08X %s%s", id, WorldName(i).c_str(), id == current ? "  <- current" : "");
  }
}

const char* ProbeModeName() {
  static const char* const names[] = {"off", "on", "mirror", "window"};
  const int mode = CCubeMaterial::sPortPBRProbeMode;
  return mode >= 0 && mode < 4 ? names[mode] : "unset";
}

void CmdStatus(CStateManager& mgr) {
  const CWorld* world = mgr.GetWorld();
  const CPlayer& player = *mgr.GetPlayer();
  CPlayerState& ps = *mgr.GetPlayerState();
  Out("frame %u, game state %d", sFrame, static_cast< int >(mgr.GetGameState()));
  if (world != nullptr) {
    const TAreaId area = world->GetCurrentAreaId();
    Out("world %08X area %d (MREA %08X)", static_cast< uint32_t >(world->IGetWorldAssetId()),
        area.Value(),
        static_cast< uint32_t >(world->GetAreaAlways(area).GetAreaAssetId()));
  }
  const CVector3f pos = player.GetTranslation();
  const CVector3f vel = player.GetVelocityWR();
  const CVector3f fwd = player.GetTransform().GetForward();
  const float yaw = std::atan2(-fwd.GetX(), fwd.GetY()) * 57.29578f;
  Out("player u%u pos=(%.2f, %.2f, %.2f) vel=(%.2f, %.2f, %.2f) yaw=%.1f",
      player.GetUniqueId().Value(), pos.GetX(), pos.GetY(), pos.GetZ(), vel.GetX(), vel.GetY(),
      vel.GetZ(), yaw);
  static const char* const morph[] = {"unmorphed", "morphed", "morphing", "unmorphing"};
  static const char* const visors[] = {"combat", "xray", "scan", "thermal"};
  static const char* const beams[] = {"power", "ice", "wave", "plasma", "phazon"};
  const int m = static_cast< int >(player.GetMorphballTransitionState());
  const int v = static_cast< int >(ps.GetCurrentVisor());
  const int b = static_cast< int >(ps.GetCurrentBeam());
  Out("hp %.0f/%.0f, %s, visor %s, beam %s, missiles %d/%d", ps.GetHealthInfo().GetHP(),
      ps.CalculateHealth(), m >= 0 && m < 4 ? morph[m] : "?", v >= 0 && v < 4 ? visors[v] : "?",
      b >= 0 && b < 5 ? beams[b] : "?", ps.GetItemAmount(CPlayerState::kIT_Missiles),
      ps.GetItemCapacity(CPlayerState::kIT_Missiles));
  const CGameCamera& cam = mgr.GetCameraManager()->GetCurrentCamera(mgr);
  Out("camera u%u fov %.1f aspect %.3f (viewport %.3f)", cam.GetUniqueId().Value(), cam.GetFov(),
      cam.GetAspectRatio(), CCameraManager::GetDefaultAspectRatio());
  const CFinalInput& in = mgr.GetFinalInput();
  Out("orbit state %d, game input L=%d A=%d, charging %d (%.2f)",
      static_cast< int >(player.GetOrbitState()), in.DL() ? 1 : 0, in.DA() ? 1 : 0,
      player.GetPlayerGun()->IsCharging() ? 1 : 0, player.GetPlayerGun()->GetChargePercentage());
  if (const CEntity* target = mgr.GetObjectById(player.GetOrbitTargetId())) {
    Out("orbit target u%u %08X %s", target->GetUniqueId().Value(), target->GetEditorId().Value(),
        target->GetDebugName().data());
  }
  Out("first person %d, cinematic %d", mgr.GetCameraManager()->IsInFPCamera() ? 1 : 0,
      mgr.GetCameraManager()->IsInCinematicCamera() ? 1 : 0);
  if (world != nullptr) {
    char sky[256];
    world->PortDescribeSky(sky, sizeof(sky));
    Out("sky %s", sky);
  }
  Out("probe %s weight %.0f, pbr draws %u", ProbeModeName(), CCubeMaterial::sPortPBRProbeWeight,
      CCubeMaterial::sPortPBRDraws);
}

void CmdAreas(CStateManager& mgr) {
  const CWorld* world = mgr.GetWorld();
  if (world == nullptr) {
    return Finish("no world");
  }
  const int current = world->GetCurrentAreaId().Value();
  for (int i = 0; i < world->GetNumAreas(); ++i) {
    const CGameArea& area = world->GetAreaAlways(TAreaId(i));
    Out("%3d %08X%s%s", i, static_cast< uint32_t >(area.GetAreaAssetId()),
        area.IsPostConstructed() ? " loaded" : "", i == current ? "  <- current" : "");
  }
  Finish();
}

// What CScriptDock does when the player crosses it: the next tick's
// TravelToArea unloads the areas that are no longer adjacent.
void CmdEnter(CStateManager& mgr) {
  const CWorld* world = mgr.GetWorld();
  int idx = -1;
  if (sCmd.args.size() < 2 || std::sscanf(sCmd.args[1].c_str(), "%d", &idx) != 1) {
    return Finish("usage: enter <area index>");
  }
  if (world == nullptr || idx < 0 || idx >= world->GetNumAreas()) {
    return Finish("no such area");
  }
  if (!world->GetAreaAlways(TAreaId(idx)).IsPostConstructed()) {
    return Finish("area not loaded");
  }
  mgr.SetCurrentAreaId(TAreaId(idx));
  Finish();
}

void CmdObjs(CStateManager& mgr) {
  const std::string filter = sCmd.args.size() > 1 ? Lower(sCmd.args[1]) : std::string();
  const CObjectList& list = mgr.GetObjectListById(kOL_All);
  int count = 0;
  for (int i = list.GetFirstObjectIndex(); i != -1; i = list.GetNextObjectIndex(i)) {
    CEntity* ent = const_cast< CEntity* >(list[i]);
    if (ent == nullptr) {
      continue;
    }
    if (!filter.empty() && Lower(ClassName(*ent)).find(filter) == std::string::npos &&
        Lower(ent->GetDebugName().data()).find(filter) == std::string::npos) {
      continue;
    }
    Out("%s", Describe(mgr, *ent).c_str());
    ++count;
  }
  Out("%d objects", count);
  Finish();
}

void CmdObj(CStateManager& mgr) {
  if (sCmd.args.size() < 2) {
    return Finish("usage: obj <id>");
  }
  CEntity* ent = FindObject(mgr, sCmd.args[1]);
  if (ent == nullptr) {
    return Finish("no such object");
  }
  Out("%s", Describe(mgr, *ent).c_str());
  Out("area %d", ent->GetAreaId().Value());
  if (CActor* actor = TCastToPtr< CActor >(ent)) {
    const CVector3f fwd = actor->GetTransform().GetForward();
    Out("forward (%.3f, %.3f, %.3f)", fwd.GetX(), fwd.GetY(), fwd.GetZ());
    if (actor->HasModelData()) {
      Out("%s", actor->GetPreRenderClipped() ? "outside the view frustum (not animated or drawn)"
                                             : "inside the view frustum");
    }
    if (const CHealthInfo* health = actor->GetHealthInfo(mgr)) {
      Out("hp %.2f", health->GetHP());
    }
    const rstl::optional_object< CAABox > touch = actor->GetTouchBounds();
    if (touch.valid()) {
      const CVector3f lo = touch->GetMinPoint();
      const CVector3f hi = touch->GetMaxPoint();
      Out("touch bounds (%.1f, %.1f, %.1f) .. (%.1f, %.1f, %.1f)", lo.GetX(), lo.GetY(), lo.GetZ(),
          hi.GetX(), hi.GetY(), hi.GetZ());
    }
  }
  if (const CPhysicsActor* physics = TCastToConstPtr< CPhysicsActor >(ent)) {
    const CVector3f vel = physics->GetVelocityWR();
    Out("velocity (%.2f, %.2f, %.2f)", vel.GetX(), vel.GetY(), vel.GetZ());
  }
  if (CPatterned* patterned = TCastToPtr< CPatterned >(ent)) {
    const CAiState* state = patterned->GetStateMachineState().GetActorState();
    Out("ai state %s for %.2fs", state != nullptr ? state->GetName() : "(none)",
        patterned->GetStateMachineTime());
    if (const CBodyController* body = patterned->GetBodyCtrl()) {
      Out("body state %d, anim %d", static_cast< int >(body->GetCurrentStateId()),
          body->GetCurrentAnimId());
    }
  }
  const rstl::vector< SConnection >& conns = ent->GetConnectionList();
  for (int i = 0; i < conns.size(); ++i) {
    const SConnection& c = conns[i];
    const CEntity* target = mgr.GetObjectById(mgr.GetIdForScript(c.x8_objId));
    Out("on %s send %s to %08X %s", NameOr(kStateNames, kStateCount, c.x0_state),
        NameOr(kMessageNames, kMessageCount, c.x4_msg), c.x8_objId.Value(),
        target != nullptr ? target->GetDebugName().data() : "(not loaded)");
  }
  Finish();
}

void CmdSend(CStateManager& mgr) {
  if (sCmd.args.size() < 3) {
    return Finish("usage: send <id> <msg>");
  }
  CEntity* ent = FindObject(mgr, sCmd.args[1]);
  if (ent == nullptr) {
    return Finish("no such object");
  }
  const int msg = LookupName(sCmd.args[2], kMessageNames, kMessageCount);
  if (msg < 0) {
    return Finish("unknown message");
  }
  mgr.SendScriptMsgAlways(ent->GetUniqueId(), kInvalidUniqueId,
                          static_cast< EScriptObjectMessage >(msg));
  Out("sent %s to u%u %08X", kMessageNames[msg], ent->GetUniqueId().Value(),
      ent->GetEditorId().Value());
  Finish();
}

// give adds capacity and fills it; take removes capacity (and any amount above
// it), which also drops a suit.
void CmdGive(CStateManager& mgr, bool take) {
  if (sCmd.args.size() < 2) {
    return Finish(take ? "usage: take <item> [n]" : "usage: give <item> [n]");
  }
  const int item = LookupName(sCmd.args[1], kItemNames, kItemCount);
  unsigned amount = 1;
  if (item < 0 || (sCmd.args.size() > 2 && !ParseUnsigned(sCmd.args[2], amount))) {
    return Finish("unknown item or amount");
  }
  CPlayerState& ps = *mgr.PlayerState();
  const CPlayerState::EItemType type = static_cast< CPlayerState::EItemType >(item);
  if (take) {
    ps.InitializePowerUp(type, -static_cast< int >(amount));
  } else {
    ps.InitializePowerUp(type, static_cast< int >(amount));
    ps.IncrPickUp(type, static_cast< int >(amount));
  }
  if (type == CPlayerState::kIT_EnergyTanks) {
    ps.HealthInfo()->SetHP(ps.CalculateHealth());
  }
  Out("%s: %d/%d", kItemNames[item], ps.GetItemAmount(type), ps.GetItemCapacity(type));
  Finish();
}

void CmdItems(CStateManager& mgr) {
  CPlayerState& ps = *mgr.PlayerState();
  for (int i = 0; i < kItemCount; ++i) {
    const CPlayerState::EItemType type = static_cast< CPlayerState::EItemType >(i);
    if (ps.GetItemCapacity(type) > 0) {
      Out("%2d %-16s %d/%d", i, kItemNames[i], ps.GetItemAmount(type), ps.GetItemCapacity(type));
    }
  }
  Finish();
}

void TeleportPlayer(CStateManager& mgr, const CVector3f& pos, const CVector3f& look) {
  CPlayer& player = *mgr.Player();
  CVector3f dir = look;
  dir.SetZ(0.f);
  if (!dir.CanBeNormalized()) {
    dir = player.GetTransform().GetForward();
    dir.SetZ(0.f);
  }
  player.Teleport(CTransform4f::LookAt(pos, pos + dir, CVector3f::Up()), mgr, true);
  player.SetVelocityWR(CVector3f(0.f, 0.f, 0.f));
}

void CmdTp(CStateManager& mgr) {
  float x, y, z;
  if (sCmd.args.size() < 4 || !ParseFloat(sCmd.args[1], x) || !ParseFloat(sCmd.args[2], y) ||
      !ParseFloat(sCmd.args[3], z)) {
    return Finish("usage: tp <x> <y> <z>");
  }
  TeleportPlayer(mgr, CVector3f(x, y, z), mgr.GetPlayer()->GetTransform().GetForward());
  Finish();
}

void CmdFace(CStateManager& mgr) {
  float yaw;
  if (sCmd.args.size() < 2 || !ParseFloat(sCmd.args[1], yaw)) {
    return Finish("usage: face <yaw degrees>");
  }
  const float rad = yaw / 57.29578f;
  TeleportPlayer(mgr, mgr.GetPlayer()->GetTranslation(),
                 CVector3f(-std::sin(rad), std::cos(rad), 0.f));
  Finish();
}

void CmdLook(CStateManager& mgr) {
  CEntity* ent = sCmd.args.size() > 1 ? FindObject(mgr, sCmd.args[1]) : nullptr;
  const CActor* actor = TCastToConstPtr< CActor >(ent);
  if (actor == nullptr) {
    return Finish("no such actor");
  }
  const CVector3f from = mgr.GetPlayer()->GetTranslation();
  TeleportPlayer(mgr, from, actor->GetTranslation() - from);
  Finish();
}

void CmdHeal(CStateManager& mgr) {
  CPlayerState& ps = *mgr.PlayerState();
  ps.HealthInfo()->SetHP(ps.CalculateHealth());
  Finish();
}

void CmdGod() {
  if (sCmd.args.size() > 1) {
    const std::string arg = Lower(sCmd.args[1]);
    if (arg != "on" && arg != "off") {
      return Finish("usage: god [on|off]");
    }
    PortDebug::SetInvulnerable(arg == "on");
  }
  Out(PortDebug::Invulnerable() ? "god on" : "god off");
  Finish();
}

void CmdMemo() {
  std::wstring wide;
  for (size_t i = 1; i < sCmd.args.size(); ++i) {
    if (i > 1) {
      wide += L' ';
    }
    for (const char c : sCmd.args[i]) {
      wide += static_cast< wchar_t >(static_cast< unsigned char >(c));
    }
  }
  if (wide.empty()) {
    return Finish("usage: memo <text>");
  }
  CSamusHud::DisplayHudMemo(rstl::wstring(wide.c_str()), CHUDMemoParms(5.f, true, false, false));
  Finish();
}

// Loads the table through the game's own factory, so a mod's replacement is what is printed.
// A blocking load, released before returning: a token kept across frames
// outlives the pool when the game quits mid-command.
void CmdStrg() {
  uint32_t id = 0;
  if (sCmd.args.size() < 2 || !ParseHex(sCmd.args[1], id)) {
    return Finish("usage: strg <id> [index]");
  }
  if (!gpResourceFactory->CanBuild(SObjectTag('STRG', id))) {
    return Finish("no such string table (only the current world's and the common ones load)");
  }
  const TLockedToken< CStringTable > token(gpSimplePool->GetObj(SObjectTag('STRG', id)));
  const CStringTable& table = **token;
  int first = 0;
  int last = table.GetStringCount();
  if (sCmd.args.size() > 2) {
    if (std::sscanf(sCmd.args[2].c_str(), "%d", &first) != 1 || first < 0 || first >= last) {
      return Finish("no such string");
    }
    last = first + 1;
  }
  for (int i = first; i < last; ++i) {
    sCmd.out += std::to_string(i) + ": " + PortDiscord::GameTextToUtf8(table.GetString(i)) + "\n";
  }
  Finish();
}

bool ResolveWorld(const std::string& arg, uint32_t& id) {
  const auto& worlds = gpMemoryCard->GetMemoryWorlds();
  const std::string want = Lower(arg);
  for (int i = 0; i < worlds.size(); ++i) {
    if (Lower(WorldName(i)).rfind(want, 0) == 0) {
      id = static_cast< uint32_t >(worlds[i].first);
      return true;
    }
  }
  return ParseHex(arg, id);
}

void CmdWarp(CStateManager& mgr) {
  if (sCmd.phase == 0) {
    if (sCmd.args.size() < 2 || !ResolveWorld(sCmd.args[1], sCmd.warpWorld) ||
        (sCmd.args.size() > 2 && !ParseHex(sCmd.args[2], sCmd.warpArea))) {
      return Finish("usage: warp <world id or name> [mrea]");
    }
    PortDebug::RequestWorldTeleport(sCmd.warpWorld, sCmd.warpArea);
    sCmd.phase = 1;
    sCmd.untilFrame = sFrame + kWarpTimeout;
    return;
  }
  // The request is consumed later in the tick that made it and quits that
  // state manager, so any later tick from a manager that is not quitting
  // belongs to the new world; it is done once that one has run a while.
  const CWorld* world = mgr.GetWorld();
  if (sCmd.phase == 1 && world != nullptr && !mgr.GetWantsToQuit() &&
      static_cast< uint32_t >(world->IGetWorldAssetId()) == sCmd.warpWorld) {
    sCmd.phase = 2;
  }
  if (sCmd.phase == 2 && mgr.GetGameState() == CStateManager::kGS_Running && ++sCmd.ticks >= 30) {
    Out("in world %08X area %d (MREA %08X)", sCmd.warpWorld, world->GetCurrentAreaId().Value(),
        static_cast< uint32_t >(world->GetAreaAlways(world->GetCurrentAreaId()).GetAreaAssetId()));
    Finish();
  }
}

// A line at a time: Out() holds 1 KiB.
void OutLines(const std::string& text) {
  for (size_t from = 0; from < text.size();) {
    size_t to = text.find('\n', from);
    if (to == std::string::npos) {
      to = text.size();
    }
    Out("%s", text.substr(from, to - from).c_str());
    from = to + 1;
  }
}

// A model id: hex, and not 0, which several commands take to mean every model.
bool ParseModel(const std::string& s, uint32_t& id) {
  char* end = nullptr;
  const unsigned long long value = std::strtoull(s.c_str(), &end, 16);
  if (end == s.c_str() || *end != '\0' || value == 0 || value > 0xFFFFFFFFull) {
    return false;
  }
  id = uint32_t(value);
  return true;
}

// The value of a PBR record a name stands for, and how many in a row; see
// CCubeModel::PortSetPBRMaterial.
bool ParseMaterialField(const std::string& s, int& field, int& count) {
  static const struct {
    const char* name;
    int field;
    int count;
  } kFields[] = {{"emissive", 0, 3}, {"backlight", 3, 3}, {"height", 6, 1},  {"mode", 7, 1}, {"kind", 13, 1},
                 {"strength", 14, 1}, {"p0", 15, 1},      {"p1", 16, 1},     {"p2", 17, 1},  {"p3", 18, 1}};
  for (const auto& entry : kFields) {
    if (s == entry.name) {
      field = entry.field;
      count = entry.count;
      return true;
    }
  }
  unsigned index = 0;
  if (ParseUnsigned(s, index) && index < 19) {
    field = int(index);
    count = 1;
    return true;
  }
  return false;
}

// stats: what the last frame cost, and the game's heap.
void CmdStats() {
  if (const AuroraStats* stats = aurora_get_stats()) {
    Out("frame: %.0f fps, %u draws (%u merged), %u PBR, pipelines %u made %u waiting", aurora_get_fps(),
        stats->drawCallCount, stats->mergedDrawCallCount, CCubeMaterial::sPortPBRDraws, stats->createdPipelines,
        stats->queuedPipelines);
    Out("buffers: %.1f MiB vertices, %.1f indices, %.1f arrays, %.1f uniforms, %.1f texture uploads",
        stats->lastVertSize / 1048576.f, stats->lastIndexSize / 1048576.f, stats->lastStorageSize / 1048576.f,
        stats->lastUniformSize / 1048576.f, stats->lastTextureUploadSize / 1048576.f);
  }
  {
    AuroraTextureStats textures{};
    aurora_get_texture_stats(&textures);
    Out("textures: %u, %.1f MiB; render targets %u, %.1f MiB", textures.count[0], textures.bytes[0] / 1048576.f,
        textures.count[1], textures.bytes[1] / 1048576.f);
  }
  {
    const IAllocator::SMetrics m = CMemorySys::GetGameAllocator().GetMetrics();
    Out("heap: %.1f of %.1f MiB in use (%u allocations), %.1f free, peak %.1f", m.x10_ / 1048576.f,
        m.x0_heapSize / 1048576.f, m.x8_, m.x14_heapSize2 / 1048576.f, m.x1c_ / 1048576.f);
  }
  int areas = 0;
  int instances = 0;
  int models = 0;
  int loaded = 0;
  int drawn = 0;
  PortRoomGeo::Stats(areas, instances, models, loaded, drawn);
  Out("room geometry: %d area(s), %d of %d model(s) loaded, %d of %d instance(s) drawn", areas, loaded, models,
      drawn, instances);
  int probes = 0;
  int cubes = 0;
  int grids = 0;
  PortRoomEnv::Stats(areas, probes, cubes, grids);
  Out("room environments: %d area(s), %d probe(s), %d cube(s) loaded, %d grid(s)", areas, probes, cubes, grids);
  Finish();
}

// view [off|albedo|...]: what PBR surfaces show in place of their shaded result.
void CmdView() {
  if (sCmd.args.size() > 1) {
    const std::string arg = Lower(sCmd.args[1]);
    int view = -1;
    for (int i = 0; i < PortDebug::PbrViewCount(); ++i) {
      if (arg == PortDebug::PbrViewName(i)) {
        view = i;
      }
    }
    if (view < 0) {
      return Finish("usage: view [off|albedo|normal|rough|metal|ao|ambient|reflection|glow|exposure|kind]");
    }
    PortDebug::SetPbrView(view);
  }
  Out("view %s", PortDebug::PbrViewName(PortDebug::PbrView()));
  Finish();
}

// touchpad [attach|detach|stick <x> <y>]: the touch overlay's kind of virtual gamepad, for
// testing how controllers share the ports with it on any platform.
void CmdTouchPad() {
  static PortTouchPad::Pad pad;
  float x = 0.f;
  float y = 0.f;
  const std::string arg = sCmd.args.size() > 1 ? Lower(sCmd.args[1]) : std::string();
  if (arg == "attach") {
    if (!pad.ok()) {
      pad = PortTouchPad::Attach();
    }
  } else if (arg == "detach") {
    PortTouchPad::Detach(pad);
  } else if (arg == "stick" && sCmd.args.size() > 3 && ParseFloat(sCmd.args[2], x) && ParseFloat(sCmd.args[3], y) &&
             pad.ok()) {
    SDL_SetJoystickVirtualAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFTX, PortTouchPad::AxisValue(x));
    SDL_SetJoystickVirtualAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFTY, PortTouchPad::AxisValue(y));
  } else if (!arg.empty()) {
    return Finish("usage: touchpad [attach|detach|stick <x> <y>] (stick needs an attached pad)");
  }
  if (pad.ok()) {
    Out("touchpad attached (joystick %u, port %d)", static_cast< unsigned >(pad.id),
        SDL_GetJoystickPlayerIndex(pad.handle));
  } else {
    Out("touchpad detached");
  }
  Finish();
}

// freecam [on|off|freeze on|off|player on|off|speed <n>|pos <x> <y> <z>|look <yaw> <pitch>]
void CmdFreeCam() {
  static const char* const usage =
      "usage: freecam [on|off|freeze on|off|player on|off|speed <n>|pos <x> <y> <z>|look <yaw> <pitch>]";
  const CStateManager* mgr = PortDebug::StateManager();
  if (sCmd.args.size() > 1) {
    const std::string arg = Lower(sCmd.args[1]);
    const std::string value = sCmd.args.size() > 2 ? Lower(sCmd.args[2]) : std::string();
    PortFreeCam::Pose pose = PortFreeCam::GetPose();
    float number = 0.f;
    if (arg == "on" || arg == "off") {
      if (arg == "on" && mgr == nullptr) {
        return Finish("not in a world");
      }
      PortFreeCam::SetActive(arg == "on", mgr);
    } else if (arg == "freeze" && (value == "on" || value == "off")) {
      PortFreeCam::SetFrozen(value == "on");
    } else if (arg == "player" && (value == "on" || value == "off")) {
      PortFreeCam::SetShowPlayer(value == "on");
    } else if (arg == "speed" && sCmd.args.size() > 2 && ParseFloat(sCmd.args[2], number)) {
      PortFreeCam::SetSpeed(number);
    } else if (arg == "pos" && sCmd.args.size() > 4 && ParseFloat(sCmd.args[2], pose.x) &&
               ParseFloat(sCmd.args[3], pose.y) && ParseFloat(sCmd.args[4], pose.z)) {
      PortFreeCam::SetPose(pose);
    } else if (arg == "look" && sCmd.args.size() > 3 && ParseFloat(sCmd.args[2], pose.yaw) &&
               ParseFloat(sCmd.args[3], pose.pitch)) {
      PortFreeCam::SetPose(pose);
    } else {
      return Finish(usage);
    }
  }
  const PortFreeCam::Pose pose = PortFreeCam::GetPose();
  Out("freecam %s%s%s speed %.1f pos %.2f %.2f %.2f yaw %.1f pitch %.1f",
      PortFreeCam::Active() ? "on" : "off", PortFreeCam::Frozen() ? " (frozen)" : "",
      PortFreeCam::ShowPlayer() ? " (player shown)" : "",
      PortFreeCam::Speed(), pose.x, pose.y, pose.z, pose.yaw, pose.pitch);
  Finish();
}

bool IsTickCommand(const std::string& name) {
  static const char* const names[] = {"status", "areas", "objs", "obj", "send", "give",
                                      "take", "items", "heal", "god", "memo", "strg", "tp", "face", "look", "warp",
                                      "tracker", "enter"};
  for (const char* n : names) {
    if (name == n) {
      return true;
    }
  }
  return false;
}

void RunTick(CStateManager& mgr) {
  const std::string& name = sCmd.args[0];
  if (name == "status") {
    CmdStatus(mgr);
    Finish();
  } else if (name == "areas") {
    CmdAreas(mgr);
  } else if (name == "enter") {
    CmdEnter(mgr);
  } else if (name == "objs") {
    CmdObjs(mgr);
  } else if (name == "obj") {
    CmdObj(mgr);
  } else if (name == "send") {
    CmdSend(mgr);
  } else if (name == "give" || name == "take") {
    CmdGive(mgr, name == "take");
  } else if (name == "items") {
    CmdItems(mgr);
  } else if (name == "heal") {
    CmdHeal(mgr);
  } else if (name == "god") {
    CmdGod();
  } else if (name == "memo") {
    CmdMemo();
  } else if (name == "strg") {
    CmdStrg();
  } else if (name == "tp") {
    CmdTp(mgr);
  } else if (name == "face") {
    CmdFace(mgr);
  } else if (name == "look") {
    CmdLook(mgr);
  } else if (name == "warp") {
    CmdWarp(mgr);
  } else if (name == "tracker") {
    sCmd.out += PortTracker::Text(PortTracker::Collect(mgr)) + "\n" + PortAp::LogicText();
    Finish();
  }
}

bool ParseButtons(const std::string& spec, PADStatus& pad) {
  u16& buttons = pad.button;
  size_t start = 0;
  while (start <= spec.size()) {
    size_t end = spec.find_first_of("+,", start);
    if (end == std::string::npos) {
      end = spec.size();
    }
    const std::string b = Lower(spec.substr(start, end - start));
    // sx:<n>, sy:<n>, cx:<n>, cy:<n> hold a stick axis along with the buttons.
    if (b.size() > 3 && b[2] == ':' && (b[0] == 's' || b[0] == 'c') && (b[1] == 'x' || b[1] == 'y')) {
      float v;
      if (!ParseFloat(b.substr(3), v)) {
        return false;
      }
      const s8 axis = static_cast< s8 >(std::clamp(v, -127.f, 127.f));
      (b[0] == 's' ? (b[1] == 'x' ? pad.stickX : pad.stickY)
                   : (b[1] == 'x' ? pad.substickX : pad.substickY)) = axis;
      start = end + 1;
      continue;
    }
    if (b == "a") buttons |= PAD_BUTTON_A;
    else if (b == "b") buttons |= PAD_BUTTON_B;
    else if (b == "x") buttons |= PAD_BUTTON_X;
    else if (b == "y") buttons |= PAD_BUTTON_Y;
    else if (b == "z") buttons |= PAD_TRIGGER_Z;
    else if (b == "l") { buttons |= PAD_TRIGGER_L; pad.triggerLeft = 255; }
    else if (b == "r") { buttons |= PAD_TRIGGER_R; pad.triggerRight = 255; }
    else if (b == "start") buttons |= PAD_BUTTON_START;
    else if (b == "up") buttons |= PAD_BUTTON_UP;
    else if (b == "down") buttons |= PAD_BUTTON_DOWN;
    else if (b == "left") buttons |= PAD_BUTTON_LEFT;
    else if (b == "right") buttons |= PAD_BUTTON_RIGHT;
    else return false;
    start = end + 1;
  }
  return true;
}

// Frame-level commands, and unknown ones.
void RunFrame() {
  const std::string& name = sCmd.args[0];
  if (name == "help") {
    CmdHelp();
    Finish();
  } else if (name == "worlds") {
    if (gpMemoryCard == nullptr || gpMemoryCard->GetMemoryWorlds().empty()) {
      return Finish("world list not loaded yet");
    }
    CmdWorlds();
    Finish();
  } else if (name == "quit") {
    Finish();
    sQuit = true;
  } else if (name == "aspect") {
    const std::string mode = sCmd.args.size() > 1 ? Lower(sCmd.args[1]) : "";
    if (mode == "4:3") {
      PortDebug::SetAspectMode(PortDebug::kAspect_4_3);
    } else if (mode == "16:9") {
      PortDebug::SetAspectMode(PortDebug::kAspect_16_9);
    } else if (mode == "window") {
      PortDebug::SetAspectMode(PortDebug::kAspect_Window);
    } else {
      return Finish("usage: aspect <4:3|16:9|window>");
    }
    Finish();
  } else if (name == "fov") {
    const float fov =
        sCmd.args.size() > 1 ? static_cast< float >(std::atof(sCmd.args[1].c_str())) : 0.f;
    if (!(fov >= PortDebug::kFovMin && fov <= PortDebug::kFovMax)) {
      return Finish("usage: fov <45..90>   first-person vertical FOV (retail 55)");
    }
    PortDebug::SetFirstPersonFov(fov);
    Finish();
  } else if (name == "msaa" || name == "aniso") {
    const int value = sCmd.args.size() > 1 ? std::atoi(sCmd.args[1].c_str()) : 0;
    if (value < 1 || value > 16) {
      return Finish("usage: msaa <1|4> | aniso <1..16>");
    }
    if (name == "msaa") {
      PortDebug::SetMsaa(value);
    } else {
      PortDebug::SetAnisotropy(value);
    }
    Finish();
  } else if (name == "hudscale") {
    const int value = sCmd.args.size() > 1 ? std::atoi(sCmd.args[1].c_str()) : 0;
    if (value < PortDebug::kHudScaleMin || value > PortDebug::kHudScaleMax) {
      return Finish("usage: hudscale <50..100>");
    }
    PortDebug::SetHudScale(value);
    Finish();
  } else if (name == "crosshair") {
    const int value = sCmd.args.size() > 1 ? std::atoi(sCmd.args[1].c_str()) : 0;
    if (value < PortDebug::kCrosshairSizeMin || value > PortDebug::kCrosshairSizeMax) {
      return Finish("usage: crosshair <25..100>");
    }
    PortDebug::SetCrosshairSize(value);
    Finish();
  } else if (name == "helmet" || name == "visorfx") {
    const std::string value = sCmd.args.size() > 1 ? sCmd.args[1] : "";
    if (value != "0" && value != "1") {
      return Finish("usage: helmet <0|1> | visorfx <0|1>");
    }
    if (name == "helmet") {
      PortDebug::SetHideHelmet(value == "0");
    } else {
      PortDebug::SetHideVisorEffects(value == "0");
    }
    Finish();
  } else if (name == "reveal") {
    const std::string value = sCmd.args.size() > 1 ? sCmd.args[1] : "";
    if (value != "0" && value != "1") {
      return Finish("usage: reveal <0|1>   reveal every world's map");
    }
    PortDebug::SetRevealMap(value == "1");
    Finish();
  } else if (name == "pickups") {
    const std::string value = sCmd.args.size() > 1 ? sCmd.args[1] : "";
    if (value != "0" && value != "1") {
      return Finish("usage: pickups <0|1>   pickup dots on the map (on in randomized games)");
    }
    PortDebug::SetMapPickups(value == "1");
    Finish();
  } else if (name == "mods") {
    // mods [reload]: what is loaded; reload reads the folder again.
    if (sCmd.args.size() > 1 && Lower(sCmd.args[1]) == "reload") {
      PortSaveState::RequestModReload();
      Out("reload queued");
      return Finish();
    }
    const PortMods::Status& status = PortMods::CurrentStatus();
    Out("%d mod(s), %d disc file(s), %zu native texture(s), %zu bound", int(status.mods.size()), status.overlays,
        PortMods::NativeTextureCount(), PortMods::NativeTexturesBound());
    for (const PortMods::ModInfo& mod : status.mods) {
      Out("%s %s files=%d resources=%d textures=%d", mod.enabled ? "+" : "-", mod.name.c_str(), mod.files,
          mod.resources, mod.textures);
    }
    Out("%s", PortSaveState::LastMessage().c_str());
    Finish();
  } else if (name == "remastered") {
    // remastered [start <image.nsp> [key file] | cancel]: the import of
    // port_remastered_import.h, and how far it is.
    const std::string verb = sCmd.args.size() > 1 ? Lower(sCmd.args[1]) : "";
    if (verb == "start" && sCmd.args.size() > 2) {
      const std::string keys = sCmd.args.size() > 3 ? sCmd.args[3] : PortRemastered::DefaultKeysPath();
      if (!PortRemastered::StartImport(sCmd.args[2], keys)) {
        return Finish("an import is already running, or there is no mods folder");
      }
    } else if (verb == "cancel") {
      PortRemastered::CancelImport();
    } else if (!verb.empty()) {
      return Finish("usage: remastered [start <image.nsp> [key file] | cancel]");
    }
    const PortRemastered::ImportState state = PortRemastered::ImportStatus();
    Out("%s %d/%d failed %d: %s",
        state.running ? "running" : !state.finished ? "idle" : state.ok ? "done" : state.cancelled ? "cancelled" : "failed",
        state.done, state.total, state.failed, state.message.c_str());
    Finish();
  } else if (name == "probe") {
    static const char* const names[] = {"off", "on", "mirror", "window"};
    if (sCmd.args.size() > 1) {
      const std::string arg = Lower(sCmd.args[1]);
      int mode = -1;
      for (int i = 0; i < 4; ++i) {
        if (arg == names[i]) {
          mode = i;
        }
      }
      if (mode < 0) {
        return Finish("usage: probe [off|on|mirror|window]");
      }
      CCubeMaterial::sPortPBRProbeMode = mode;
    }
    Out("probe %s weight %.0f", ProbeModeName(), CCubeMaterial::sPortPBRProbeWeight);
    Finish();
  } else if (name == "freecam") {
    CmdFreeCam();
  } else if (name == "view") {
    CmdView();
  } else if (name == "stats") {
    CmdStats();
  } else if (name == "touchpad") {
    CmdTouchPad();
  } else if (name == "hdfont") {
    if (sCmd.args.size() > 1) {
      const std::string arg = Lower(sCmd.args[1]);
      if (arg != "on" && arg != "off") {
        return Finish("usage: hdfont [on|off]");
      }
      PortHdFont::SetEnabled(arg == "on");
    }
    Out("hdfont %s", PortHdFont::Enabled() ? "on" : "off");
    Finish();
  } else if (name == "roomenv") {
    float number = 0.f;
    if (sCmd.args.size() > 1) {
      const std::string arg = Lower(sCmd.args[1]);
      if (arg == "info") {
        // At a point, or where the view is.
        float at[3];
        float forward[3];
        if (sCmd.args.size() > 4) {
          if (!ParseFloat(sCmd.args[2], at[0]) || !ParseFloat(sCmd.args[3], at[1]) ||
              !ParseFloat(sCmd.args[4], at[2])) {
            return Finish("usage: roomenv info [<x> <y> <z>]");
          }
        } else if (!PortDebug::ViewRay(at, forward)) {
          return Finish("not in a world");
        }
        const std::string info = PortRoomEnv::Info(at);
        Out("at %.2f %.2f %.2f", at[0], at[1], at[2]);
        OutLines(info.empty() ? std::string("no room environment loaded") : info);
        return Finish();
      } else if (arg == "exposure" && sCmd.args.size() > 2 &&
                 (Lower(sCmd.args[2]) == "on" || Lower(sCmd.args[2]) == "off")) {
        PortRoomEnv::SetRoomExposed(Lower(sCmd.args[2]) == "on");
      } else if (arg == "on" || arg == "off") {
        PortRoomEnv::SetEnabled(arg == "on");
      } else if (arg == "volume" && sCmd.args.size() > 2 &&
                 (Lower(sCmd.args[2]) == "on" || Lower(sCmd.args[2]) == "off")) {
        PortRoomEnv::SetVolumesEnabled(Lower(sCmd.args[2]) == "on");
      } else if (arg == "ambient" && sCmd.args.size() > 2 && ParseFloat(sCmd.args[2], number)) {
        PortRoomEnv::SetAmbientScale(number);
      } else if (arg == "show" && sCmd.args.size() > 2 &&
                 (Lower(sCmd.args[2]) == "off" || Lower(sCmd.args[2]) == "coords" ||
                  Lower(sCmd.args[2]) == "light")) {
        PortRoomEnv::SetVolumeView(Lower(sCmd.args[2]) == "off" ? 0 : Lower(sCmd.args[2]) == "coords" ? 1 : 2);
      } else {
        return Finish("usage: roomenv [on|off|info [<x> <y> <z>]|exposure on|off|volume on|off|ambient <scale>|"
                      "show off|coords|light]");
      }
    }
    int areas = 0;
    int probes = 0;
    int cubes = 0;
    int grids = 0;
    PortRoomEnv::Stats(areas, probes, cubes, grids);
    static const char* const kViews[] = {"off", "coords", "light"};
    Out("roomenv %s: %d area(s), %d probe(s), %d cube(s) loaded, %d ambient grid(s), exposure by %s, "
        "volume %s, ambient %g, show %s",
        PortRoomEnv::Enabled() ? "on" : "off", areas, probes, cubes, grids,
        PortRoomEnv::RoomExposed() ? "room" : "cube", PortRoomEnv::VolumesEnabled() ? "on" : "off",
        PortRoomEnv::AmbientScale(), kViews[std::clamp(PortRoomEnv::VolumeView(), 0, 2)]);
    Finish();
  } else if (name == "roomgeo") {
    if (sCmd.args.size() > 1) {
      const std::string arg = Lower(sCmd.args[1]);
      if (arg == "on" || arg == "off" || arg == "overlay") {
        PortRoomGeo::SetMode(arg == "on" ? PortRoomGeo::Mode::Replace
                             : arg == "off" ? PortRoomGeo::Mode::Off
                                            : PortRoomGeo::Mode::Overlay);
      } else if (arg == "lights" && sCmd.args.size() > 2 &&
                 (Lower(sCmd.args[2]) == "on" || Lower(sCmd.args[2]) == "off")) {
        PortRoomGeo::SetAreaLights(Lower(sCmd.args[2]) == "on");
      } else if (arg == "at" && sCmd.args.size() > 4) {
        const CVector3f point(float(std::atof(sCmd.args[2].c_str())), float(std::atof(sCmd.args[3].c_str())),
                              float(std::atof(sCmd.args[4].c_str())));
        const float margin = sCmd.args.size() > 5 ? float(std::atof(sCmd.args[5].c_str())) : 0.f;
        const std::string list = PortRoomGeo::At(point, margin);
        OutLines(list.empty() ? std::string("no instance there") : list);
        return Finish();
      } else if (arg == "pick") {
        float origin[3];
        float forward[3];
        if (!PortDebug::ViewRay(origin, forward)) {
          return Finish("not in a world");
        }
        std::string list;
        const uint32_t first = PortRoomGeo::Pick(CVector3f(origin[0], origin[1], origin[2]),
                                                 CVector3f(forward[0], forward[1], forward[2]), list);
        OutLines(list.empty() ? std::string("no instance ahead") : list);
        if (first != 0) {
          Out("materials of %08X:", first);
          OutLines(PortRoomGeo::Materials(first));
        }
        return Finish();
      } else if (arg == "mats" && sCmd.args.size() > 2) {
        uint32_t id = 0;
        if (!ParseModel(sCmd.args[2], id)) {
          return Finish("roomgeo: not a model id");
        }
        const std::string list = PortRoomGeo::Materials(id);
        OutLines(list.empty() ? std::string("no loaded model has that id") : list);
        return Finish();
      } else if (arg == "mat" && sCmd.args.size() == 3 && Lower(sCmd.args[2]) == "clear") {
        Out("%d value(s) cleared", PortRoomGeo::ClearMaterialValues());
        return Finish();
      } else if (arg == "mat" && sCmd.args.size() > 5) {
        uint32_t id = 0;
        unsigned material = 0;
        int field = 0;
        int count = 0;
        if (!ParseModel(sCmd.args[2], id) || !ParseUnsigned(sCmd.args[3], material) ||
            !ParseMaterialField(Lower(sCmd.args[4]), field, count)) {
          return Finish("usage: roomgeo mat <cmdl> <material> <field> <value...> | mat clear; fields: emissive "
                        "backlight height mode kind strength p0 p1 p2 p3, or 0 to 18");
        }
        // One value for all of a colour, or one each.
        float values[3] = {};
        const int given = int(sCmd.args.size()) - 5;
        if (given != 1 && given != count) {
          return Finish("roomgeo: that field takes one value, or one per component");
        }
        for (int i = 0; i < given; ++i) {
          if (!ParseFloat(sCmd.args[5 + i], values[i])) {
            return Finish("roomgeo: not a number");
          }
        }
        for (int i = 0; i < count; ++i) {
          if (!PortRoomGeo::SetMaterialValue(id, int(material), field + i, values[given == 1 ? 0 : i])) {
            return Finish("roomgeo: no loaded model has that material");
          }
        }
        OutLines(PortRoomGeo::Materials(id));
        Out("(drawn until `roomgeo mat clear` or the next start)");
        return Finish();
      } else if ((arg == "hide" || arg == "show") && (sCmd.args.size() > 2 || arg == "show")) {
        uint32_t id = 0;
        // 0 means every model, so a mistyped id must not parse as one.
        if (sCmd.args.size() > 2 && !ParseModel(sCmd.args[2], id)) {
          return Finish("roomgeo: not a model id");
        }
        Out("%d model(s)", PortRoomGeo::SetHidden(id, arg == "hide"));
      } else {
        return Finish("usage: roomgeo [on|off|overlay | lights on|off | at <x> <y> <z> [margin] | pick | "
                      "hide <cmdl> | show [cmdl] | mats <cmdl> | mat <cmdl> <material> <field> <value...> | "
                      "mat clear]");
      }
    }
    int areas = 0;
    int instances = 0;
    int models = 0;
    int loaded = 0;
    int drawn = 0;
    PortRoomGeo::Stats(areas, instances, models, loaded, drawn);
    const PortRoomGeo::Mode mode = PortRoomGeo::GetMode();
    Out("roomgeo %s: %d area(s), %d instance(s), %d of %d model(s) loaded, %d drawn",
        mode == PortRoomGeo::Mode::Off ? "off" : mode == PortRoomGeo::Mode::Replace ? "on" : "overlay", areas,
        instances, loaded, models, drawn);
    if (const AuroraStats* stats = aurora_get_stats()) {
      Out("frame: %u draws, %.1f MiB vertices, %.1f MiB indices, %.1f MiB arrays, %.1f MiB uniforms, %.0f fps",
          stats->drawCallCount, stats->lastVertSize / 1048576.f, stats->lastIndexSize / 1048576.f,
          stats->lastStorageSize / 1048576.f, stats->lastUniformSize / 1048576.f, aurora_get_fps());
    }
    Finish();
  } else if (name == "roomliquid") {
    if (sCmd.args.size() > 1) {
      const std::string arg = Lower(sCmd.args[1]);
      if (arg != "on" && arg != "off") {
        return Finish("usage: roomliquid [on|off]");
      }
      PortRoomLiquid::SetEnabled(arg == "on");
    }
    int areas = 0;
    int surfaces = 0;
    int drawn = 0;
    PortRoomLiquid::Stats(areas, surfaces, drawn);
    Out("roomliquid %s: %d area(s), %d surface(s), %d drawn", PortRoomLiquid::Enabled() ? "on" : "off", areas,
        surfaces, drawn);
    Finish();
  } else if (name == "viewmodel") {
    const std::string arg = sCmd.args.size() > 1 ? Lower(sCmd.args[1]) : "status";
    if (arg == "off") {
      PortViewModel::Hide();
    } else if (arg == "light") {
      PortViewModel::SetLight(sCmd.args.size() > 2 && sCmd.args[2] == "1");
    } else if (arg != "status") {
      const auto num = [](size_t i) {
        return sCmd.args.size() > i ? static_cast< float >(std::atof(sCmd.args[i].c_str())) : 0.f;
      };
      std::string err;
      if (!PortViewModel::Show(static_cast< uint32_t >(std::strtoul(arg.c_str(), nullptr, 16)),
                               num(2), num(3), num(4), err)) {
        return Finish(err.c_str());
      }
    }
    Out("%s", PortViewModel::Status().c_str());
    Finish();
  } else if (name == "state") {
    const std::string action = sCmd.args.size() > 1 ? Lower(sCmd.args[1]) : "list";
    const int slot = sCmd.args.size() > 2 ? std::atoi(sCmd.args[2].c_str())
                                          : PortSaveState::SelectedSlot();
    if (action == "save" || action == "load" || action == "undo") {
      const bool queued = action == "save" ? PortSaveState::RequestSave(slot)
                          : action == "load"
                              ? PortSaveState::RequestLoad(slot)
                              : PortSaveState::RequestLoad(PortSaveState::kUndoSlot);
      if (!queued) {
        return Finish(PortSaveState::LastMessage().c_str());
      }
      Out("queued; see 'state last'");
      return Finish();
    } else if (action == "slot") {
      PortSaveState::SetSelectedSlot(slot);
      Out("selected slot %d", PortSaveState::SelectedSlot());
      return Finish();
    } else if (action == "last") {
      Out("%s", PortSaveState::LastMessage().c_str());
      return Finish();
    } else if (action == "list") {
      for (int i = PortSaveState::kUndoSlot; i <= PortSaveState::kSlotCount; ++i) {
        const PortSaveState::Info info = PortSaveState::SlotInfo(i);
        if (info.exists) {
          Out("%s%d %s - %s igt=%.1f%s", i == PortSaveState::SelectedSlot() ? "*" : " ", i,
              info.world.c_str(), info.room.c_str(), info.playTime, info.morphed ? " ball" : "");
        }
      }
      Out("folder %s", PortSaveState::Folder().c_str());
      return Finish();
    }
    return Finish("usage: state list|last|save [n]|load [n]|undo|slot <n>");
  } else if (name == "timer") {
    const std::string value = sCmd.args.size() > 1 ? sCmd.args[1] : "";
    if (value != "0" && value != "1") {
      return Finish("usage: timer <0|1>");
    }
    PortDebug::SetSpeedrunTimer(value == "1");
    Finish();
  } else if (name == "igt") {
    const double value = sCmd.args.size() > 1 ? std::atof(sCmd.args[1].c_str()) : -1.0;
    if (!(value >= 0.0) || gpGameState == nullptr) {
      return Finish("usage: igt <seconds>");
    }
    gpGameState->SetTotalPlayTime(value);
    Finish();
  } else if (name == "livesplit") {
    const std::string action = sCmd.args.size() > 1 ? Lower(sCmd.args[1]) : "status";
    if (action == "0" || action == "1") {
      PortDebug::SetLiveSplit(action == "1");
    } else if (action == "addr" && sCmd.args.size() == 3) {
      PortDebug::SetLiveSplitAddress(sCmd.args[2]);
    } else if (action == "send" && sCmd.args.size() > 2) {
      std::string line = sCmd.args[2];
      for (size_t i = 3; i < sCmd.args.size(); ++i) {
        line += " " + sCmd.args[i];
      }
      PortLiveSplit::SendRaw(line);
    } else if (action != "status") {
      return Finish("usage: livesplit <0|1> | addr <host:port> | send <command> | status");
    }
    static const char* const kStatusNames[] = {"off", "connecting", "connected", "failed"};
    Out("livesplit %s %s %s", PortDebug::LiveSplit() ? "on" : "off",
        PortDebug::LiveSplitAddress().c_str(), kStatusNames[PortLiveSplit::Status()]);
    const std::string error = PortLiveSplit::LastError();
    if (!error.empty()) {
      Out("last error: %s", error.c_str());
    }
    Finish();
  } else if (name == "discord") {
    const std::string action = sCmd.args.size() > 1 ? Lower(sCmd.args[1]) : "status";
    if (action == "0" || action == "1") {
      PortDebug::SetDiscordPresence(action == "1");
    } else if (action == "id" && sCmd.args.size() == 3) {
      PortDebug::SetDiscordAppId(sCmd.args[2]);
    } else if (action != "status") {
      return Finish("usage: discord <0|1> | id <application id> | status");
    }
    static const char* const kStatusNames[] = {"off", "connecting", "connected", "failed"};
    Out("discord %s id=%s %s", PortDebug::DiscordPresence() ? "on" : "off",
        PortDebug::DiscordAppId().c_str(), kStatusNames[PortDiscord::Status()]);
    Out("showing: %s", PortDiscord::CurrentText().c_str());
    const std::string error = PortDiscord::LastError();
    if (!error.empty()) {
      Out("last error: %s", error.c_str());
    }
    Finish();
  } else if (name == "gci") {
    const std::string action = sCmd.args.size() > 1 ? Lower(sCmd.args[1]) : "list";
    // The rest of the line is one path, spaces and all.
    std::string path;
    for (size_t i = 2; i < sCmd.args.size(); ++i) {
      path += (i > 2 ? " " : "") + sCmd.args[i];
    }
    const std::string sub = sCmd.args.size() > 2 ? Lower(sCmd.args[2]) : "";
    std::string text;
    if (action == "list") {
      text = PortDebug::CardList();
    } else if (action == "import" && !path.empty()) {
      text = PortDebug::CardImport(path);
    } else if (action == "export" && !path.empty()) {
      text = PortDebug::CardExport(path);
    } else if (action == "dolphin" && sCmd.args.size() == 3 && (sub == "import" || sub == "export")) {
      text = sub == "import" ? PortDebug::CardImportDolphin() : PortDebug::CardExportDolphin();
    } else {
      return Finish("usage: gci list | import <path> | export <dir or .raw> | dolphin import|export");
    }
    size_t start = 0;
    while (start <= text.size()) {
      const size_t end = std::min(text.find('\n', start), text.size());
      Out("%s", text.substr(start, end - start).c_str());
      start = end + 1;
    }
    Finish();
  } else if (name == "ap") {
    const std::string action = sCmd.args.size() > 1 ? Lower(sCmd.args[1]) : "";
    std::string error;
    if (action == "connect" && (sCmd.args.size() == 4 || sCmd.args.size() == 5)) {
      PortAp::ConnectionDetails details;
      details.server = sCmd.args[2];
      details.slot = sCmd.args[3];
      details.password = sCmd.args.size() == 5 ? sCmd.args[4] : "";
      if (!PortAp::Connect(details, error))
        return Finish(error.c_str());
    } else if (action == "disconnect") {
      if (!PortAp::Disconnect(error))
        return Finish(error.c_str());
    } else if (action == "recent" && sCmd.args.size() == 2) {
      const std::vector<PortAp::ConnectionDetails> games = PortAp::RecentGames();
      for (size_t i = 0; i < games.size(); ++i)
        Out("%zu: %s @ %s, seed %s, played %lld", i, games[i].slot.c_str(), games[i].server.c_str(),
            games[i].seed.c_str(), static_cast<long long>(games[i].lastPlayed));
    } else if (action == "resume" && sCmd.args.size() == 3) {
      unsigned index = 0;
      const std::vector<PortAp::ConnectionDetails> games = PortAp::RecentGames();
      if (!ParseUnsigned(sCmd.args[2], index) || index >= games.size())
        return Finish("no such recent game (see ap recent)");
      if (!PortAp::Connect(games[index], error))
        return Finish(error.c_str());
    } else if (action == "say" && sCmd.args.size() >= 3) {
      std::string text = sCmd.args[2];
      for (size_t i = 3; i < sCmd.args.size(); ++i)
        text += " " + sCmd.args[i];
      if (!PortAp::SendChat(text, error))
        return Finish(error.c_str());
    } else if (action == "chat" && sCmd.args.size() == 2) {
      for (const PortAp::ChatLine& line : PortAp::ChatLog())
        Out("[%s] %s", line.type.c_str(), line.text.c_str());
    } else if (!action.empty()) {
      return Finish("usage: ap [connect <server> <slot> [password] | disconnect | recent | resume <n> | "
                    "say <text> | chat]");
    }
    Out("%s (%s)", PortAp::StatusText(), PortAp::ConfigFilePath().c_str());
    Finish();
  } else if (name == "wait") {
    unsigned frames = 0;
    if (sCmd.phase == 0) {
      if (sCmd.args.size() < 2 || !ParseUnsigned(sCmd.args[1], frames)) {
        return Finish("usage: wait <frames>");
      }
      sCmd.phase = 1;
      sCmd.untilFrame = sFrame + frames;
    }
    if (sFrame >= sCmd.untilFrame) {
      Finish();
    }
  } else if (name == "press" || name == "stick" || name == "cstick") {
    if (sCmd.phase == 0) {
      unsigned frames = 6;
      PADStatus& pad = sCmd.pad;
      pad = PADStatus{};
      pad.err = PAD_ERR_NONE;
      size_t framesArg;
      if (name == "press") {
        if (sCmd.args.size() < 2 || !ParseButtons(sCmd.args[1], pad)) {
          return Finish("usage: press <a+b+...> [frames]");
        }
        framesArg = 2;
      } else {
        float x, y;
        if (sCmd.args.size() < 3 || !ParseFloat(sCmd.args[1], x) || !ParseFloat(sCmd.args[2], y)) {
          return Finish("usage: stick <x> <y> [frames]");
        }
        const s8 sx = static_cast< s8 >(std::clamp(x, -127.f, 127.f));
        const s8 sy = static_cast< s8 >(std::clamp(y, -127.f, 127.f));
        (name == "stick" ? pad.stickX : pad.substickX) = sx;
        (name == "stick" ? pad.stickY : pad.substickY) = sy;
        framesArg = 3;
      }
      if (sCmd.args.size() > framesArg && !ParseUnsigned(sCmd.args[framesArg], frames)) {
        return Finish("bad frame count");
      }
      if (frames == 0) {
        // Stays held until the next press/stick (used with hold/step).
        PADSetVirtualStatus(0, &sCmd.pad);
        return Finish();
      }
      sCmd.phase = 1;
      sCmd.untilFrame = sFrame + std::max(frames, 1u);
    }
    if (sFrame < sCmd.untilFrame) {
      PADSetVirtualStatus(0, &sCmd.pad);
    } else {
      // The virtual status is sticky; release so the next press is an edge.
      PADStatus released{};
      released.err = PAD_ERR_NONE;
      PADSetVirtualStatus(0, &released);
      Finish();
    }
  } else if (name == "gyro") {
    if (sCmd.phase == 0) {
      float pitch = 0.f;
      float yaw = 0.f;
      unsigned frames = 6;
      if (sCmd.args.size() < 2 || !ParseFloat(sCmd.args[1], pitch) ||
          (sCmd.args.size() > 2 && !ParseFloat(sCmd.args[2], yaw)) ||
          (sCmd.args.size() > 3 && !ParseUnsigned(sCmd.args[3], frames))) {
        return Finish("usage: gyro <pitch> [yaw] [frames]");
      }
      PortDebug::SetGyroOverride(true, pitch, yaw);
      sCmd.phase = 1;
      sCmd.untilFrame = sFrame + std::max(frames, 1u);
    }
    if (sFrame >= sCmd.untilFrame) {
      PortDebug::SetGyroOverride(false, 0.f, 0.f);
      Finish();
    }
  } else if (name == "present") {
    const std::string arg = sCmd.args.size() > 1 ? Lower(sCmd.args[1]) : "";
    float t = 0.f;
    if (arg == "off") {
      PortDebug::SetPresentOverride(-1.f);
    } else if (arg == "cycle") {
      PortDebug::SetPresentOverride(PortDebug::kPresentCycle);
    } else if (arg == "tick") {
      PortDebug::SetPresentOverride(PortDebug::kPresentTick);
    } else if (!arg.empty() && ParseFloat(arg, t) && t >= 0.f && t <= 1.f) {
      PortDebug::SetPresentOverride(t);
    } else if (arg.empty()) {
      const float value = PortDebug::PresentOverrideValue();
      if (value < 0.f) {
        Out("present off");
      } else if (value == PortDebug::kPresentTick) {
        Out("present tick");
      } else if (value > 1.f) {
        Out("present cycle");
      } else {
        Out("present %.3f", value);
      }
    } else {
      return Finish("usage: present <0..1|cycle|tick|off>");
    }
    Finish();
  } else if (name == "hold") {
    const std::string arg = sCmd.args.size() > 1 ? sCmd.args[1] : "";
    if (arg != "0" && arg != "1") {
      return Finish("usage: hold <0|1>");
    }
    PortDebug::SetTickHold(arg == "1");
    Finish();
  } else if (name == "step") {
    if (sCmd.phase == 0) {
      unsigned count = 1;
      if (!PortDebug::TickHold()) {
        return Finish("not held (hold 1 first)");
      }
      if (sCmd.args.size() > 1 && !ParseUnsigned(sCmd.args[1], count)) {
        return Finish("usage: step [ticks]");
      }
      PortDebug::StepTicks(count);
      sCmd.phase = 1;
      sCmd.untilFrame = sFrame + count + 120;
      return;
    }
    if (PortDebug::PendingHeldTicks() == 0) {
      Finish();
    } else if (sFrame >= sCmd.untilFrame) {
      Finish("ticks still pending");
    }
  } else if (name == "interp") {
    const std::string which = sCmd.args.size() > 1 ? Lower(sCmd.args[1]) : "";
    const std::string arg = sCmd.args.size() > 2 ? sCmd.args[2] : "";
    if (which.empty()) {
      Out("actor %d pose %d particle %d frame_limit %d", PortDebug::ActorInterpolation(),
          PortDebug::PoseInterpolation(), PortDebug::ParticleInterpolation(),
          PortDebug::FrameLimitEnabled());
      return Finish();
    }
    if ((arg != "0" && arg != "1") ||
        (which != "actor" && which != "pose" && which != "particle" && which != "all")) {
      return Finish("usage: interp [actor|pose|particle|all <0|1>]");
    }
    const bool on = arg == "1";
    if (which == "actor" || which == "all") {
      PortDebug::SetActorInterpolation(on);
    }
    if (which == "pose" || which == "all") {
      PortDebug::SetPoseInterpolation(on);
    }
    if (which == "particle" || which == "all") {
      PortDebug::SetParticleInterpolation(on);
    }
    Finish();
  } else if (name == "shot") {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::current_path(ec) / "screenshots";
    const auto count = [&dir] {
      std::error_code e;
      size_t n = 0;
      for (fs::directory_iterator it(dir, e), end; !e && it != end; it.increment(e)) {
        ++n;
      }
      return n;
    };
    if (sCmd.phase == 0) {
      sCmd.shotCount = count();
      aurora::request_screenshot();
      sCmd.phase = 1;
      sCmd.untilFrame = sFrame + 120;
      return;
    }
    if (count() > sCmd.shotCount) {
      fs::path newest;
      fs::file_time_type newestTime{};
      for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::file_time_type t = it->last_write_time(ec);
        if (newest.empty() || t > newestTime) {
          newest = it->path();
          newestTime = t;
        }
      }
      Out("%s", newest.string().c_str());
      Finish();
    } else if (sFrame >= sCmd.untilFrame) {
      Finish("no screenshot appeared");
    }
  } else {
    Finish("unknown command (try help)");
  }
}

void Tokenize(const std::string& line, std::vector< std::string >& args) {
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && std::isspace(static_cast< unsigned char >(line[i]))) {
      ++i;
    }
    if (i >= line.size()) {
      break;
    }
    size_t j = i;
    while (j < line.size() && !std::isspace(static_cast< unsigned char >(line[j]))) {
      ++j;
    }
    args.push_back(line.substr(i, j - i));
    i = j;
  }
}

} // namespace

bool PortConsoleEnabled() {
  if (!sStarted) {
    Start();
  }
  return sEnabled;
}

bool PortConsoleFrame(unsigned frame) {
  if (!sStarted) {
    Start();
  }
  if (!sEnabled) {
    return false;
  }
  sFrame = frame;
  while (!sHasCommand) {
    Incoming next;
    {
      std::lock_guard< std::mutex > lock(sQueueMutex);
      if (sQueue.empty()) {
        break;
      }
      next = sQueue.front();
      sQueue.pop_front();
    }
    sCmd = Command{};
    Tokenize(next.line, sCmd.args);
    if (sCmd.args.empty()) {
      continue;
    }
    sCmd.args[0] = Lower(sCmd.args[0]);
    sCmd.generation = next.generation;
    sCmd.startFrame = frame;
    sHasCommand = true;
  }
  if (sHasCommand) {
    if (!IsTickCommand(sCmd.args[0])) {
      RunFrame();
    } else if (sCmd.phase == 0 &&
               frame - sCmd.startFrame > (sCmd.args[0] == "warp" ? kWarpTimeout : kTickTimeout)) {
      // A warp sent while the game boots waits for it to reach a world.
      Finish("the game is not ticking (not in a world, or paused)");
    } else if (sCmd.phase != 0 && frame > sCmd.untilFrame) {
      Finish("timed out");
    }
  }
  return sQuit;
}

void PortConsoleTick(CStateManager& mgr) {
  if (!sEnabled || !sHasCommand || !IsTickCommand(sCmd.args[0])) {
    return;
  }
  RunTick(mgr);
}

#include "port_ap_logic.h"

#include "port_ap_metroidprime.h"

#include <algorithm>

namespace PortApLogic {
namespace {

// Item and option codes the pack's rules name.
enum class Code : uint16_t {
  None,
  PowerBeam,
  IceBeam,
  WaveBeam,
  PlasmaBeam,
  MissileExpansion,
  MissileLauncher,
  ScanVisor,
  ThermalVisor,
  XRayVisor,
  MorphBall,
  Bombs,
  SpringBall,
  BoostBall,
  SpiderBall,
  PowerBomb,
  PowerBombExpansion,
  SpaceJump,
  GrappleBeam,
  VariaSuit,
  GravitySuit,
  PhazonSuit,
  EnergyTank,
  FlaahgraPowerBombs,
  RemoveHiveMecha,
  BackwardsLowerMines,
  TricksEasy,
  TricksMedium,
  TricksHard,
};

// The pack's Lua helpers (scripts/logic/logic.lua), under their own names.
enum class Func : uint16_t {
  can_access_elevators,
  can_ball_jump,
  can_bomb,
  can_boost,
  can_charge_beam,
  can_charge_combo,
  _can_climb_observatory_via_puzzle,
  can_climb_sun_tower,
  can_climb_tower_of_light,
  can_combat_beam_pirates,
  can_combat_flaahgra,
  can_combat_ghosts,
  can_combat_labs,
  can_combat_mines,
  can_combat_omega_pirate,
  can_combat_prime,
  can_combat_ridley,
  can_combat_thardus,
  can_crashed_frigate,
  can_crashed_frigate_backwards,
  can_crashed_frigate_front,
  can_defeat_sheegoth,
  can_grapple,
  can_heat,
  can_ice_beam,
  can_infinite_speed,
  can_melt_ice,
  can_missile,
  can_morph_ball,
  can_move_underwater,
  can_phazon,
  can_plasma_beam,
  can_power_beam,
  can_power_bomb,
  _can_reach_top_of_ruined_courtyard,
  can_scan,
  can_space_jump,
  can_spider,
  can_super_missile,
  can_thermal,
  can_wave_beam,
  can_xray,
  has_energy_tanks,
  has_power_bomb_count,
};

enum class Kind : uint8_t {
  Func,  // id = Func, arg = its number argument, code = its beam argument
  Trick, // id = trick, arg = the difficulty that puts it in logic
  Node,  // id = another node, whose level is taken
  Code,  // id = Code, arg = the count needed
  Never, // "[]": counts nothing
  Start,    // id = a starting room in kStartRooms: the seed starts there
  Elevator, // id = this node, arg = the node its elevator comes from on the disc: the level
            // of whichever elevator leads here in the seed
  Door,     // id = a door in kDoors: it can be opened
  Empty, // "{}": only opens the inspect braces
};

enum : uint8_t {
  kOptional = 1, // [token]: failing it is a sequence break, not a dead end
  kBrace = 2,    // {token: the rest of the rule only inspects
  kLevel = 4,    // ^$func: the function returns a level
};

struct Token {
  Kind kind;
  uint8_t flags;
  uint16_t id;
  int16_t arg;
  Code code;
};

struct Rule {
  uint16_t first;
  uint16_t count;
};

struct Node {
  const char* area;
  const char* room;
  const char* section; // null for a room
  uint16_t firstRule;
  uint16_t ruleCount;
  bool always;
};

struct Trick {
  const char* name;
  int difficulty;
};

struct Door {
  const char* area;
  const char* source;      // null: any door of the area with this lock
  const char* destination;
  const char* lock;        // null for a plain door
  bool missile;
};

struct LocationNode {
  int64_t id;
  uint16_t node;
};

#include "port_ap_logic_data.inc"

constexpr size_t kNodeCount = sizeof(kNodes) / sizeof(kNodes[0]);
constexpr size_t kTrickCount = sizeof(kTricks) / sizeof(kTricks[0]);
constexpr size_t kLocationCount = sizeof(kLocationNodes) / sizeof(kLocationNodes[0]);
constexpr size_t kTokenCount = sizeof(kTokens) / sizeof(kTokens[0]);

std::string Squeezed(const std::string& name) {
  std::string out;
  for (const char c : name) {
    if (c != ' ')
      out += c;
  }
  return out;
}

enum TrickSetting : int8_t { kDeny = -1, kGlobal = 0, kAllow = 1 };

class Evaluator {
public:
  Evaluator(const Options& options, const Items& items) : mOptions(options), mItems(items) {
    for (size_t i = 0; i < kTrickCount; ++i) {
      const auto listed = [i](const std::vector< std::string >& names) {
        return std::find(names.begin(), names.end(), kTricks[i].name) != names.end();
      };
      // The tracker pack applies the allow list after the deny list.
      mTricks[i] = listed(options.trickAllow) ? kAllow : listed(options.trickDeny) ? kDeny : kGlobal;
    }
    mStart = Squeezed(options.startRoom.empty() ? "Landing Site" : options.startRoom);

    // Where every elevator leads: the disc's pairs, then the seed's.
    std::map< uint16_t, uint16_t > leads;
    for (size_t i = 0; i < kTokenCount; ++i) {
      if (kTokens[i].kind == Kind::Elevator)
        leads[static_cast< uint16_t >(kTokens[i].arg)] = kTokens[i].id;
    }
    const auto elevator = [&leads](const std::string& area, const std::string& name) {
      // A name two areas share comes as "Area: name".
      const size_t colon = name.find(": ");
      const std::string in = colon != std::string::npos ? name.substr(0, colon) : area;
      const std::string room = colon != std::string::npos ? name.substr(colon + 2) : name;
      for (const auto& entry : leads) {
        const Node& node = kNodes[entry.first];
        if (room == node.room && (in.empty() || in == node.area))
          return static_cast< int >(entry.first);
      }
      return -1;
    };
    for (const auto& area : options.elevators) {
      for (const auto& entry : area.second) {
        const int from = elevator(area.first, entry.first);
        const int to = elevator(std::string(), entry.second);
        if (from >= 0 && to >= 0)
          leads[static_cast< uint16_t >(from)] = static_cast< uint16_t >(to);
      }
    }
    for (const auto& entry : leads)
      mArrivals[entry.second].push_back(entry.first);
  }

  std::vector< Level > Run() {
    // PopTracker's cacheAccessibility: nodes refer to each other, so resolve
    // every one that is not yet in logic until a pass changes nothing.
    std::vector< Level > levels(kNodeCount, Level::None);
    // A starting room the pack has no rule for is still where the game starts.
    const auto known = [this](const char* name) { return mStart == name; };
    if (std::none_of(std::begin(kStartRooms), std::end(kStartRooms), known)) {
      for (size_t i = 0; i < kNodeCount; ++i) {
        if (kNodes[i].section == nullptr && Squeezed(kNodes[i].room) == mStart)
          levels[i] = Level::Normal;
      }
    }
    for (size_t pass = 0; pass < kNodeCount; ++pass) {
      bool changed = false;
      for (size_t i = 0; i < kNodeCount; ++i) {
        if (levels[i] == Level::Normal)
          continue;
        const Level level = Resolve(kNodes[i], levels);
        if (level != levels[i]) {
          levels[i] = level;
          changed = true;
        }
      }
      if (!changed)
        break;
    }
    return levels;
  }

private:
  int64_t Item(int offset) const {
    const auto found = mItems.find(PortAp::MetroidPrime::kItemBase + offset);
    return found != mItems.end() ? found->second : 0;
  }

  // The stage of one of the four progressive beams: 1 the beam, 2 its charge, 3 its combo.
  int64_t Beam(Code beam) const {
    switch (beam) {
    case Code::PowerBeam: return Item(49);
    case Code::IceBeam: return Item(51);
    case Code::WaveBeam: return Item(52);
    case Code::PlasmaBeam: return Item(53);
    default: return 0;
    }
  }

  // What Tracker:ProviderCountForCode gives for the pack's item with this code.
  int64_t Count(Code code) const {
    switch (code) {
    case Code::None: return 0;
    case Code::PowerBeam: return Item(0) + Beam(code);
    case Code::IceBeam: return Item(1) + Beam(code);
    case Code::WaveBeam: return Item(2) + Beam(code);
    case Code::PlasmaBeam: return Item(3) + Beam(code);
    case Code::MissileExpansion: return 5 * Item(4); // counted in missiles
    case Code::MissileLauncher: return Item(43);
    case Code::ScanVisor: return Item(5);
    case Code::ThermalVisor: return Item(9);
    case Code::XRayVisor: return Item(13);
    case Code::MorphBall: return Item(16);
    // Bombs are a two-stage item: Spring Ball, then the bombs, which keep it.
    case Code::Bombs: return Item(6) > 0 || Item(54) >= 2 ? 1 : 0;
    case Code::SpringBall: return Item(6) + Item(45) + Item(54);
    case Code::BoostBall: return Item(18);
    case Code::SpiderBall: return Item(19);
    case Code::PowerBomb: return Item(44);
    case Code::PowerBombExpansion: return Item(7);
    case Code::SpaceJump: return Item(15);
    case Code::GrappleBeam: return Item(12);
    case Code::VariaSuit: return Item(22);
    case Code::GravitySuit: return Item(21);
    case Code::PhazonSuit: return Item(23);
    case Code::EnergyTank: return Item(24);
    case Code::FlaahgraPowerBombs: return mOptions.flaahgraPowerBombs ? 1 : 0;
    case Code::RemoveHiveMecha: return mOptions.removeHiveMecha ? 1 : 0;
    case Code::BackwardsLowerMines: return mOptions.backwardsLowerMines ? 1 : 0;
    // The stages of the pack's trick option do not inherit: each code is its own stage only.
    case Code::TricksEasy: return mOptions.trickDifficulty == 0 ? 1 : 0;
    case Code::TricksMedium: return mOptions.trickDifficulty == 1 ? 1 : 0;
    case Code::TricksHard: return mOptions.trickDifficulty == 2 ? 1 : 0;
    }
    return 0;
  }

  bool Has(Code code) const { return Count(code) > 0; }

  // The Charge Beam item; the port also takes the four per-beam ones as it.
  bool ChargeItem() const { return Item(10) + Item(55) + Item(56) + Item(57) + Item(58) > 0; }

  bool Charge(Code beam) const {
    if (beam != Code::None)
      return mOptions.progressiveBeams ? Beam(beam) >= 2 : ChargeItem() && Has(beam);
    if (mOptions.progressiveBeams)
      return Item(49) >= 2 || Item(51) >= 2 || Item(52) >= 2 || Item(53) >= 2;
    return ChargeItem();
  }

  bool Missile(int expansions = 1) const {
    const int64_t count = 5 * std::max(expansions, 1);
    if (mOptions.mainMissile)
      return Has(Code::MissileLauncher) && Count(Code::MissileExpansion) >= count - 5;
    return Count(Code::MissileExpansion) >= count;
  }

  bool Bomb() const { return Has(Code::MorphBall) && Has(Code::Bombs); }
  bool Boost() const { return Has(Code::MorphBall) && Has(Code::BoostBall); }
  bool Spider() const { return Has(Code::MorphBall) && Has(Code::SpiderBall); }
  bool PowerBomb() const {
    return Has(Code::MorphBall) && Has(mOptions.mainPowerBomb ? Code::PowerBomb : Code::PowerBombExpansion);
  }

  bool SuperMissile() const {
    const bool beam = mOptions.progressiveBeams ? Item(49) >= 3 : ChargeItem() && Item(11) > 0;
    return Has(Code::PowerBeam) && Missile() && beam;
  }

  bool ChargeCombo(Code beam) const {
    if (!Missile(2) || !Charge(beam))
      return false;
    const bool combo = mOptions.progressiveBeams ? Beam(beam) >= 3 : false;
    switch (beam) {
    case Code::WaveBeam: return Missile(3) && (combo || Item(28) > 0);
    case Code::IceBeam: return combo || Item(14) > 0;
    case Code::PlasmaBeam: return Missile(3) && (combo || Item(8) > 0);
    default: return false;
    }
  }

  // requirement: 0 every use of the visor, 1 a few, 2 the Omega Pirate only.
  Level Visor(Code visor, int removed, int requirement) const {
    if (Has(visor))
      return Level::Normal;
    if (requirement == 2)
      return Level::None;
    return removed > requirement ? Level::Normal : Level::SequenceBreak;
  }
  Level XRay(int requirement = 0) const { return Visor(Code::XRayVisor, mOptions.removeXray, requirement); }
  Level Thermal(int requirement = 0) const {
    return Visor(Code::ThermalVisor, mOptions.removeThermal, requirement);
  }

  bool Combat(int normalTanks, int minimalTanks, bool charge = true) const {
    if (mOptions.combatLogic < 0)
      return true;
    const int tanks = mOptions.combatLogic == 1 ? minimalTanks : normalTanks;
    return Count(Code::EnergyTank) >= tanks && (!charge || Charge(Code::None));
  }

  Level FrigateFront() const {
    if (!(Has(Code::MorphBall) && Has(Code::WaveBeam) && (Has(Code::GravitySuit) || Has(Code::SpaceJump))))
      return Level::None;
    return Thermal();
  }

  static Level Truth(bool value) { return value ? Level::Normal : Level::None; }

  // A helper's result. The ones that return a level are called with ^ in the
  // rules; the rest return a boolean, given here as Normal or None.
  Level Call(const Token& token) const {
    const int arg = token.arg;
    switch (static_cast< Func >(token.id)) {
    case Func::can_access_elevators: return Truth(mOptions.preScanElevators || Has(Code::ScanVisor));
    case Func::can_ball_jump: return Truth(Has(Code::MorphBall) && Has(Code::SpringBall));
    case Func::can_bomb: return Truth(Bomb());
    case Func::can_boost: return Truth(Boost());
    case Func::can_charge_beam: return Truth(Charge(token.code));
    case Func::can_charge_combo: return Truth(ChargeCombo(token.code));
    case Func::_can_climb_observatory_via_puzzle:
      return Truth(Boost() && Bomb() && Has(Code::SpaceJump) && Has(Code::ScanVisor));
    case Func::can_climb_sun_tower: return Truth(Has(Code::ScanVisor) && Spider() && SuperMissile() && Bomb());
    case Func::can_climb_tower_of_light:
      return Truth(Missile() && Count(Code::MissileExpansion) >= 36 && Has(Code::SpaceJump));
    case Func::can_combat_beam_pirates: return Truth(mOptions.combatLogic == 1 || Has(token.code));
    case Func::can_combat_flaahgra: return Truth(Combat(2, 1, false));
    case Func::can_combat_ghosts:
      if (mOptions.combatLogic < 0)
        return Level::Normal;
      if (mOptions.combatLogic == 1)
        return Truth(Has(Code::PowerBeam));
      return Truth(Charge(Code::PowerBeam) && Has(Code::PowerBeam) && XRay(1) == Level::Normal);
    case Func::can_combat_labs: return Truth(Combat(1, 0, false));
    case Func::can_combat_mines: return Truth(Combat(5, 3));
    case Func::can_combat_omega_pirate: {
      const Level level = XRay(2);
      if (level == Level::None)
        return Level::None;
      return Combat(6, 3) ? level : Level::SequenceBreak;
    }
    case Func::can_combat_prime: return Truth(Combat(8, 5));
    case Func::can_combat_ridley: return Truth(Combat(8, 8));
    case Func::can_combat_thardus:
      if (mOptions.combatLogic < 0)
        return Level::Normal;
      if (mOptions.combatLogic == 1)
        return Truth(Has(Code::PlasmaBeam) || Has(Code::PowerBeam) || Has(Code::WaveBeam));
      return Truth(Count(Code::EnergyTank) >= 3 && Charge(Code::None) &&
                   (Has(Code::PlasmaBeam) || Has(Code::PowerBeam)));
    case Func::can_crashed_frigate:
      if (!(Bomb() && Has(Code::SpaceJump) && Has(Code::GravitySuit)))
        return Level::None;
      return FrigateFront();
    case Func::can_crashed_frigate_backwards:
      return Truth(Has(Code::SpaceJump) && Has(Code::GravitySuit) && Bomb());
    case Func::can_crashed_frigate_front: return FrigateFront();
    case Func::can_defeat_sheegoth: return Truth(Bomb() || Missile() || PowerBomb() || Has(Code::PowerBeam));
    case Func::can_grapple: return Truth(Has(Code::GrappleBeam));
    case Func::can_heat:
      return Truth(Has(Code::VariaSuit) ||
                   (!mOptions.variaOnlyHeat && (Has(Code::GravitySuit) || Has(Code::PhazonSuit))));
    case Func::can_ice_beam: return Truth(Has(Code::IceBeam));
    case Func::can_infinite_speed: return Truth(Boost() && Bomb());
    case Func::can_melt_ice: return Truth(Has(Code::PlasmaBeam));
    case Func::can_missile: return Truth(Missile(arg));
    case Func::can_morph_ball: return Truth(Has(Code::MorphBall));
    case Func::can_move_underwater: return Truth(Has(Code::GravitySuit));
    case Func::can_phazon: return Truth(Has(Code::PhazonSuit));
    case Func::can_plasma_beam: return Truth(Has(Code::PlasmaBeam));
    case Func::can_power_beam: return Truth(Has(Code::PowerBeam));
    case Func::can_power_bomb: return Truth(PowerBomb());
    case Func::_can_reach_top_of_ruined_courtyard:
      return Truth(((Boost() && Bomb() && Has(Code::ScanVisor)) || Spider()) && Has(Code::SpaceJump));
    case Func::can_scan: return Truth(Has(Code::ScanVisor));
    case Func::can_space_jump: return Truth(Has(Code::SpaceJump));
    case Func::can_spider: return Truth(Spider());
    case Func::can_super_missile: return Truth(SuperMissile());
    case Func::can_thermal: return Thermal(arg);
    case Func::can_wave_beam: return Truth(Has(Code::WaveBeam));
    case Func::can_xray: return XRay(arg);
    case Func::has_energy_tanks: return Truth(Count(Code::EnergyTank) >= arg);
    case Func::has_power_bomb_count:
      return Truth(Count(Code::PowerBombExpansion) + (mOptions.mainPowerBomb ? 4 : 0) >= arg);
    }
    return Level::None;
  }

  // A door lock under randomprime's name; one it can't be opened with is none of these.
  bool Lock(const std::string& lock) const {
    if (lock.empty() || lock == "Blue" || lock == "None")
      return true;
    if (lock == "Wave Beam")
      return Has(Code::WaveBeam);
    if (lock == "Ice Beam")
      return Has(Code::IceBeam);
    if (lock == "Plasma Beam")
      return Has(Code::PlasmaBeam);
    if (lock == "Power Beam Only")
      return Has(Code::PowerBeam);
    if (lock == "Missile")
      return Missile();
    if (lock == "Bomb")
      return Bomb();
    return false;
  }

  bool Shield(const std::string& shield) const {
    if (shield.empty() || shield == "None")
      return true;
    if (shield == "Missile")
      return Missile();
    if (shield == "Bomb")
      return Bomb();
    if (shield == "Power Bomb")
      return PowerBomb();
    if (shield == "Charge Beam")
      return Charge(Code::None);
    if (shield == "Super Missile")
      return SuperMissile();
    if (shield == "Wavebuster")
      return ChargeCombo(Code::WaveBeam);
    if (shield == "Ice Spreader")
      return ChargeCombo(Code::IceBeam);
    if (shield == "Flamethrower")
      return ChargeCombo(Code::PlasmaBeam);
    return false;
  }

  bool Opens(const Door& door) const {
    if (door.source != nullptr) {
      const auto seen =
          mOptions.doors.find(std::string(door.area) + '|' + door.source + '|' + door.destination);
      if (seen != mOptions.doors.end())
        return Shield(seen->second.shield) && Lock(seen->second.lock);
    }
    if (door.missile && !Missile())
      return false;
    if (door.lock == nullptr)
      return true;
    const auto area = mOptions.doorColors.find(door.area);
    if (area != mOptions.doorColors.end()) {
      const auto lock = area->second.find(door.lock);
      if (lock != area->second.end())
        return Lock(lock->second);
    }
    return Lock(door.lock);
  }

  Level Arrival(uint16_t node, const std::vector< Level >& levels) const {
    Level best = Level::None;
    const auto arrivals = mArrivals.find(node);
    if (arrivals != mArrivals.end()) {
      for (const uint16_t from : arrivals->second)
        best = std::max(best, levels[from]);
    }
    return best;
  }

  // Whether a token that counts something counts enough.
  bool Passes(const Token& token) const {
    switch (token.kind) {
    case Kind::Func: return Call(token) != Level::None;
    case Kind::Trick: {
      const TrickSetting setting = mTricks[token.id];
      if (setting != kGlobal)
        return setting == kAllow;
      return mOptions.trickDifficulty + 1 >= token.arg;
    }
    case Kind::Code: return Count(static_cast< Code >(token.id)) >= token.arg;
    case Kind::Start: return mStart == kStartRooms[token.id];
    case Kind::Door: return Opens(kDoors[token.id]);
    default: return false;
    }
  }

  // PopTracker's Tracker::resolveRules, branch for branch.
  Level Resolve(const Node& node, const std::vector< Level >& levels) const {
    if (node.always)
      return Level::Normal;
    bool glitched = false;
    bool inspectable = false;
    for (size_t r = 0; r < node.ruleCount; ++r) {
      const Rule& rule = kRules[node.firstRule + r];
      Level reachable = Level::Normal;
      bool inspectOnly = false;
      for (size_t t = 0; t < rule.count && reachable != Level::None; ++t) {
        const Token& token = kTokens[rule.first + t];
        if (token.flags & kBrace)
          inspectOnly = true;
        const bool optional = (token.flags & kOptional) != 0;
        if (token.kind == Kind::Empty)
          continue;
        const bool elevator = token.kind == Kind::Elevator;
        if (token.kind == Kind::Node || elevator || (token.flags & kLevel)) {
          Level sub = elevator ? Arrival(token.id, levels)
                               : token.kind == Kind::Node ? levels[token.id] : Call(token);
          if (!inspectOnly && sub == Level::Inspect) {
            // A node that can only be inspected does not lead anywhere, though
            // PopTracker leaves the rule standing; a function that inspects
            // turns the whole rule into an inspection.
            if (token.kind == Kind::Node || elevator)
              sub = Level::None;
            else
              inspectOnly = true;
          } else if (optional && sub == Level::None) {
            sub = Level::SequenceBreak;
          } else if (sub == Level::None) {
            reachable = Level::None;
          }
          if (sub == Level::SequenceBreak && reachable != Level::None)
            reachable = Level::SequenceBreak;
        } else if (!Passes(token)) {
          reachable = optional ? Level::SequenceBreak : Level::None;
        }
      }
      if (reachable == Level::Normal && !inspectOnly)
        return Level::Normal;
      if (reachable != Level::None && inspectOnly)
        inspectable = true;
      if (reachable == Level::SequenceBreak)
        glitched = true;
    }
    if (glitched && inspectable)
      return Level::Inspect;
    return glitched ? Level::SequenceBreak : inspectable ? Level::Inspect : Level::None;
  }

  const Options& mOptions;
  const Items& mItems;
  TrickSetting mTricks[kTrickCount];
  std::string mStart;
  // Elevator node -> the elevator nodes that lead to it.
  std::map< uint16_t, std::vector< uint16_t > > mArrivals;
};

} // namespace

bool Options::operator==(const Options& other) const {
  return trickDifficulty == other.trickDifficulty && combatLogic == other.combatLogic &&
         removeXray == other.removeXray && removeThermal == other.removeThermal &&
         flaahgraPowerBombs == other.flaahgraPowerBombs && progressiveBeams == other.progressiveBeams &&
         mainMissile == other.mainMissile && mainPowerBomb == other.mainPowerBomb &&
         variaOnlyHeat == other.variaOnlyHeat && preScanElevators == other.preScanElevators &&
         trickAllow == other.trickAllow && trickDeny == other.trickDeny && startRoom == other.startRoom &&
         removeHiveMecha == other.removeHiveMecha && backwardsLowerMines == other.backwardsLowerMines &&
         elevators == other.elevators && doorColors == other.doorColors && doors == other.doors;
}

const Check* Checks(size_t& count) {
  static const std::vector< Check > checks = [] {
    std::vector< Check > out;
    for (const LocationNode& location : kLocationNodes) {
      const Node& node = kNodes[location.node];
      out.push_back({location.id, node.area, node.room, node.section != nullptr ? node.section : ""});
    }
    return out;
  }();
  count = checks.size();
  return checks.data();
}

std::vector< Level > Evaluate(const Options& options, const Items& items) {
  const std::vector< Level > nodes = Evaluator(options, items).Run();
  std::vector< Level > out(kLocationCount);
  for (size_t i = 0; i < kLocationCount; ++i)
    out[i] = nodes[kLocationNodes[i].node];
  return out;
}

const char* TrickName(size_t index) {
  return index < kTrickCount ? kTricks[index].name : nullptr;
}

} // namespace PortApLogic

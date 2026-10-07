#pragma once

// The collision box a solid actor's own model describes, read from the original
// disc instead of from the model that is drawn.
//
// A CScriptActor authored with no collision extent (or a negative one) takes
// its box from its model's bounding box (ScriptLoader.cpp, LoadActor), so a mod
// that replaces the model replaces the collider with it: a lava crust whose
// replacement is drawn a few centimetres thick becomes a step Samus cannot walk
// onto, and the frozen-lava floor in Magmoor Workstation is one. The disc's own
// CMDL carries the box the actor was authored against, so reading that back
// gives the collider the level expects whatever the mod draws. Rendering is
// untouched: only the collision box changes.
//
// Nothing here reads a mod's overlay. Every byte comes from aurora_dvd_base_*,
// which serves the unmodded file behind the DVD overlays, and the PAK index is
// built from base entries only, so neither a replaced whole PAK nor a loose
// <id>.CMDL in a mods folder can move a collider. The index is built on the
// first actor that asks and every box is cached, so this costs one pass over the
// PAK tables per run and one resource read per model id.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace PortActorCollisionBounds {

// How the original disc's files are reached. The port fills in aurora's base
// DVD calls; a test fills in a synthetic disc.
struct SourceIo {
  // Every PAK on the disc, overlays included, as (entry number, path).
  std::vector< std::pair< int32_t, std::string > > (*discPaks)() = nullptr;
  // How many entries the base disc itself has. An entry at or past this one is
  // a mod's file, so its bytes are not the disc's and are not read.
  int32_t (*entryCount)() = nullptr;
  void* (*open)(int32_t entry) = nullptr;
  int64_t (*readAt)(void* handle, uint64_t offset, uint8_t* buffer, size_t length) = nullptr;
  void (*close)(void* handle) = nullptr;
};

// The models of one disc, indexed by id.
class Reader {
 public:
  explicit Reader(SourceIo io);
  ~Reader();
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  // Indexes the disc's PAK tables. False when no model was found at all (no
  // disc open, or tables that do not parse): the reader then stays empty and
  // every Bounds answers false, leaving callers on their own boxes.
  bool Build();
  // True once Build has run and found the disc's models.
  bool Indexed();
  size_t ModelCount();

  // The box of the disc's model with this id, as six floats: min x, y, z then
  // max x, y, z. False when the id is not on the disc, or when the model's
  // header does not parse; either way the answer is cached, so a missing or
  // malformed model is never looked for twice.
  bool Bounds(uint32_t id, float out[6]);

 private:
  struct Where {
    int32_t entry = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
    bool compressed = false;
  };

  bool ModelBounds(const Where& where, float out[6]);

  SourceIo m_io;
  std::mutex m_mutex;
  std::map< int32_t, void* > m_handles;
  std::unordered_map< uint32_t, Where > m_models;
  std::unordered_map< uint32_t, std::pair< bool, std::array< float, 6 > > > m_cache;
  bool m_indexed = false;
  bool m_found = false;
};

// The six floats a CMDL header carries at 0x0c: min x, y, z then max x, y, z,
// big-endian as the file stores them. False when the data is not a CMDL header,
// is cut short before the box, or holds a box that is not finite or whose min
// is above its max. A box flat in one axis (a plane, min == max) is a real one
// and parses.
bool ParseCmdlBounds(const uint8_t* data, size_t size, float out[6]);

// The disc's box for a model id, read once per run and cached. False when the
// disc has no such model, or its header does not parse; the caller then keeps
// whatever box it had.
bool ReadOriginalModelBounds(uint32_t id, float out[6]);

// Drops the index, the open files and the cache. The base disc never changes
// while the game runs, so this belongs to the disc's lifecycle (a new disc),
// not to a mods reload; it is also what lets a test start over.
void Reset();

} // namespace PortActorCollisionBounds
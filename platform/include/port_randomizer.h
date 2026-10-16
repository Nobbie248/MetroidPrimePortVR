#ifndef METROID_PRIME_PORT_PORT_RANDOMIZER_H
#define METROID_PRIME_PORT_PORT_RANDOMIZER_H
#include <cstdint>

namespace PortRandomizer {

// The model a pickup is drawn with. Pickups are built from the area file's
// static model, or from an animated model when `acs` is non-zero, so both are
// carried here. Mirrors CAnimationParameters plus the static model asset id.
struct PickupModel {
  uint32_t model = 0;
  uint32_t acs = 0;
  uint32_t character = 0;
  uint32_t animation = 0;
};

// Loads the configured seed once. Call on the game thread; never throws.
void EnsureLoaded();
// True when a seed contains at least one placement.
bool Enabled();
// True when MP_RANDO_DUMP is set to a non-empty value other than "0".
bool DumpEnabled();
// Name from the loaded seed, or an empty string when none is available.
const char* SeedName();
// Number of pickup checks recorded during this process.
int CheckCount();
// Short status line suitable for the F1 overlay.
const char* StatusText();
// Logs a location in dump mode, or applies its placement when enabled. `model`
// is only read in dump mode, where it is recorded so a seed's item models can
// be derived from the dump.
bool ApplyPickup(uint32_t worldAssetId, uint32_t areaAssetId, uint32_t entityId,
                 int& itemType, int& capacity, int& amount,
                 const PickupModel& model = PickupModel());
// The item and capacity the seed puts at a location, for the map's pickup
// kinds. Leaves them alone and returns false when the randomizer is off or the
// location keeps its item; a placement without a capacity keeps `capacity`.
bool Placed(uint32_t worldAssetId, uint32_t areaAssetId, uint32_t entityId, int& itemType,
            int& capacity);
// Looks up the model the seed associates with an item type. False when the seed
// carries no model for it, in which case the original model is kept.
bool ModelForItem(int itemType, PickupModel& out);
// Where the seed put an item (the first location holding it, by key order).
// False when the randomizer is off or no location holds it.
bool FindItem(int itemType, uint32_t& worldAssetId, uint32_t& areaAssetId, uint32_t& entityId);
// Records a collected pickup check when randomizer or dump mode is active.
void RecordCheck(uint32_t worldAssetId, uint32_t areaAssetId, uint32_t entityId, int itemType);
// Returns the retail item name, or "Unknown" for an out-of-range item type.
const char* ItemName(int itemType);
// Returns an item type for a case-insensitive exact name, or -1 when unknown.
int ItemFromName(const char* name);
// Formats a location as three uppercase, zero-padded eight-digit hexadecimal IDs.
void FormatLocationKey(uint32_t worldAssetId, uint32_t areaAssetId, uint32_t entityId,
                       char* out, int outSize);

} // namespace PortRandomizer

#endif

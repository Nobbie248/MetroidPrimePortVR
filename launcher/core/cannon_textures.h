// SPDX-License-Identifier: GPL-3.0-or-later
//
// PrimedGun's arm-cannon texture slots on the port. The three textures keep
// Dolphin's names, which Aurora reads as they are (the base level's XXH64 is
// the same in both). A slot is applied by copying its files into a managed
// folder of the user texture pack whose name sorts first, because Aurora takes
// the first file it meets for each texture (case-insensitive path order), so
// the slot wins over any other pack file for the same texture, as PrimedGun's
// 000_PrimedGunCannon pack did in Dolphin.

#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace PrimedGunLauncher::Cannon {

constexpr std::array<std::string_view, 3> kTextureNames = {
    "tex1_128x128_m_3c6ded49d64d30f2_14",
    "tex1_128x128_m_bec6d78ea7dd739e_14",
    "tex1_64x64_m_c7625e7ecd9cd5c2_14",
};
constexpr std::array<std::string_view, 3> kTextureLabels = {
    "Cannon texture A",
    "Cannon texture B",
    "Shine mask",
};
constexpr int kShineIndex = 2;
constexpr int kSlotCount = 6; // Default, Slot 1-4, Custom
constexpr int kCustomSlot = 5;
constexpr std::string_view kPackFolder = "000_primedgun_cannon";
// platform/port_textures.cpp kDeviceDirs.
constexpr std::array<std::string_view, 6> kDeviceFolders = {"xbox",     "playstation", "switch",
                                                            "gamecube", "standard",    "keyboard"};

std::string SlotName(int slot);

struct Folders {
  std::filesystem::path library;        // <user>/primedgun/cannon_textures
  std::filesystem::path shippedLibrary; // <exe>/primedgun/cannon_textures
  std::filesystem::path userTextures;   // <user>/user_textures, or MP_USER_TEXTURES
};

// Copies the shipped files the user library lacks; never overwrites.
void SeedLibrary(const Folders& folders);

std::filesystem::path SlotFolder(const Folders& folders, int slot);
// `<slot>/<texture>.dds` or `.png`, whichever the slot holds; empty when it
// has neither.
std::filesystem::path ResolveSource(const Folders& folders, int slot, int index);
std::filesystem::path DefaultPreview(const Folders& folders, int index);
std::filesystem::path RemoveShinePreset(const Folders& folders);
std::filesystem::path RestoreShinePreset(const Folders& folders, int slot);

// Where the managed folder goes: the pack root, or every device folder when the
// pack is split by device (only the active device's folder loads then).
std::vector<std::filesystem::path> PackFolders(const Folders& folders);

// Removes every managed folder; slot 0 (Default) is just this.
bool ClearPack(const Folders& folders, std::string& error);
// Clears the pack and copies the slot's three files into it.
bool ApplySlot(const Folders& folders, int slot, std::string& error);
// Copies `source` into the slot as texture `index`, replacing what it had.
// Returns the file written.
std::filesystem::path ImportIntoSlot(const Folders& folders, int slot, int index,
                                     const std::filesystem::path& source, std::string& error);
// Keeps the slot's shine mask in presets/restore_shine/slot_N (once), then
// puts the remove-shine preset in its place. Returns the file written.
std::filesystem::path RemoveShine(const Folders& folders, int slot, std::string& error);
// Puts the kept shine mask back. Returns the file written.
std::filesystem::path RestoreShine(const Folders& folders, int slot, std::string& error);

} // namespace PrimedGunLauncher::Cannon

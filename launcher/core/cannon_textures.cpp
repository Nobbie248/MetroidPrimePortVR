// SPDX-License-Identifier: GPL-3.0-or-later

#include "cannon_textures.h"

#include <cctype>
#include <system_error>

namespace fs = std::filesystem;

namespace PrimedGunLauncher::Cannon {
namespace {

std::string Utf8(const fs::path& path) {
  const std::u8string u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
}

fs::path FromUtf8(std::string_view text) {
  return fs::path(std::u8string(text.begin(), text.end()));
}

std::string LowerExtension(const fs::path& path) {
  std::string ext = Utf8(path.extension());
  for (char& c : ext) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return ext;
}

fs::path TextureFile(int index, std::string_view extension) {
  return FromUtf8(std::string(kTextureNames[index]) + std::string(extension));
}

// The user library's copy when it has one, else the shipped one, else the
// user library's path (where a new file would go).
fs::path LibraryFile(const Folders& folders, const fs::path& relative) {
  std::error_code ec;
  const fs::path user = folders.library / relative;
  if (fs::is_regular_file(user, ec)) {
    return user;
  }
  const fs::path shipped = folders.shippedLibrary / relative;
  if (!folders.shippedLibrary.empty() && fs::is_regular_file(shipped, ec)) {
    return shipped;
  }
  return user;
}

fs::path SlotRelative(int slot) { return FromUtf8("slot_" + std::to_string(slot)); }

void RemoveSlotTexture(const Folders& folders, int slot, int index) {
  std::error_code ec;
  fs::remove(SlotFolder(folders, slot) / TextureFile(index, ".png"), ec);
  fs::remove(SlotFolder(folders, slot) / TextureFile(index, ".dds"), ec);
}

bool CopyReplacing(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  return fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec) && !ec;
}

bool SamePath(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  return fs::equivalent(a, b, ec);
}

} // namespace

std::string SlotName(int slot) {
  if (slot == 0) {
    return "Default";
  }
  if (slot == kCustomSlot) {
    return "Custom";
  }
  return "Slot " + std::to_string(slot);
}

void SeedLibrary(const Folders& folders) {
  std::error_code ec;
  if (folders.shippedLibrary.empty() || !fs::is_directory(folders.shippedLibrary, ec) ||
      SamePath(folders.shippedLibrary, folders.library)) {
    return;
  }
  for (fs::recursive_directory_iterator it(folders.shippedLibrary, ec), end; !ec && it != end;
       it.increment(ec)) {
    std::error_code fileEc;
    if (!it->is_regular_file(fileEc)) {
      continue;
    }
    const fs::path relative = fs::relative(it->path(), folders.shippedLibrary, fileEc);
    const fs::path target = folders.library / relative;
    if (!fileEc && !fs::exists(target, fileEc)) {
      fs::create_directories(target.parent_path(), fileEc);
      fs::copy_file(it->path(), target, fileEc);
    }
  }
}

fs::path SlotFolder(const Folders& folders, int slot) {
  return folders.library / SlotRelative(slot);
}

fs::path ResolveSource(const Folders& folders, int slot, int index) {
  std::error_code ec;
  for (const char* extension : {".dds", ".png"}) {
    const fs::path file = LibraryFile(folders, SlotRelative(slot) / TextureFile(index, extension));
    if (fs::is_regular_file(file, ec)) {
      return file;
    }
  }
  return {};
}

fs::path DefaultPreview(const Folders& folders, int index) {
  std::error_code ec;
  for (const char* extension : {".dds", ".png"}) {
    const fs::path file = LibraryFile(folders, FromUtf8("default") / TextureFile(index, extension));
    if (fs::is_regular_file(file, ec)) {
      return file;
    }
  }
  return {};
}

fs::path RemoveShinePreset(const Folders& folders) {
  return LibraryFile(folders, FromUtf8("presets/remove_shine") / TextureFile(kShineIndex, ".dds"));
}

fs::path RestoreShinePreset(const Folders& folders, int slot) {
  return LibraryFile(folders, FromUtf8("presets/restore_shine") / SlotRelative(slot) /
                                  TextureFile(kShineIndex, ".dds"));
}

std::vector<fs::path> PackFolders(const Folders& folders) {
  std::error_code ec;
  bool split = false;
  for (std::string_view device : kDeviceFolders) {
    if (fs::is_directory(folders.userTextures / FromUtf8(device), ec)) {
      split = true;
      break;
    }
  }
  if (!split) {
    return {folders.userTextures};
  }
  std::vector<fs::path> out;
  for (std::string_view device : kDeviceFolders) {
    out.push_back(folders.userTextures / FromUtf8(device));
  }
  return out;
}

bool ClearPack(const Folders& folders, std::string& error) {
  std::error_code ec;
  const fs::path pack = FromUtf8(kPackFolder);
  fs::remove_all(folders.userTextures / pack, ec);
  if (ec) {
    error = "Could not remove " + Utf8(folders.userTextures / pack);
    return false;
  }
  for (std::string_view device : kDeviceFolders) {
    const fs::path deviceFolder = folders.userTextures / FromUtf8(device);
    fs::remove_all(deviceFolder / pack, ec);
    if (ec) {
      error = "Could not remove " + Utf8(deviceFolder / pack);
      return false;
    }
    // A device folder made only to hold the slot would otherwise keep the
    // pack split by device after the slot is gone.
    if (fs::is_directory(deviceFolder, ec) && fs::is_empty(deviceFolder, ec)) {
      fs::remove(deviceFolder, ec);
    }
  }
  return true;
}

bool ApplySlot(const Folders& folders, int slot, std::string& error) {
  if (!ClearPack(folders, error)) {
    return false;
  }
  if (slot <= 0) {
    return true;
  }
  const std::vector<fs::path> targets = PackFolders(folders);
  for (int index = 0; index < static_cast<int>(kTextureNames.size()); ++index) {
    const fs::path source = ResolveSource(folders, slot, index);
    if (source.empty()) {
      continue;
    }
    const std::string extension = LowerExtension(source);
    if (extension != ".png" && extension != ".dds") {
      error = "Cannon textures must be PNG or DDS files.";
      return false;
    }
    for (const fs::path& target : targets) {
      const fs::path destination = target / FromUtf8(kPackFolder) / TextureFile(index, extension);
      if (!CopyReplacing(source, destination)) {
        error = "Could not copy cannon texture to:\n" + Utf8(destination);
        return false;
      }
    }
  }
  return true;
}

fs::path ImportIntoSlot(const Folders& folders, int slot, int index, const fs::path& source,
                        std::string& error) {
  const std::string extension = LowerExtension(source);
  if (extension != ".png" && extension != ".dds") {
    error = "Cannon textures must be PNG or DDS files.";
    return {};
  }
  const fs::path destination = SlotFolder(folders, slot) / TextureFile(index, extension);
  if (SamePath(source, destination)) {
    return destination;
  }
  RemoveSlotTexture(folders, slot, index);
  if (!CopyReplacing(source, destination)) {
    error = "Could not copy the selected texture into the slot.";
    return {};
  }
  return destination;
}

fs::path RemoveShine(const Folders& folders, int slot, std::string& error) {
  std::error_code ec;
  const fs::path preset = RemoveShinePreset(folders);
  if (!fs::is_regular_file(preset, ec)) {
    error = "Could not find the remove-shine DDS at:\n" + Utf8(preset);
    return {};
  }

  // Keep the slot's own shine mask once, so Restore Shine can put it back.
  const fs::path backup = RestoreShinePreset(folders, slot);
  if (!fs::is_regular_file(backup, ec)) {
    const fs::path current = ResolveSource(folders, slot, kShineIndex);
    if (!current.empty() && !SamePath(current, preset)) {
      const fs::path keep = folders.library / FromUtf8("presets/restore_shine") /
                            SlotRelative(slot) / TextureFile(kShineIndex, ".dds");
      if (!CopyReplacing(current, keep)) {
        error = "Could not save the current slot shine texture into:\n" + Utf8(keep);
        return {};
      }
    }
  }

  const fs::path destination = SlotFolder(folders, slot) / TextureFile(kShineIndex, ".dds");
  RemoveSlotTexture(folders, slot, kShineIndex);
  if (!CopyReplacing(preset, destination)) {
    error = "Could not copy the remove-shine texture into the slot.";
    return {};
  }
  return destination;
}

fs::path RestoreShine(const Folders& folders, int slot, std::string& error) {
  std::error_code ec;
  const fs::path source = RestoreShinePreset(folders, slot);
  if (!fs::is_regular_file(source, ec)) {
    error = "Could not find the saved shine texture for this slot.";
    return {};
  }
  const fs::path destination =
      SlotFolder(folders, slot) / TextureFile(kShineIndex, LowerExtension(source));
  RemoveSlotTexture(folders, slot, kShineIndex);
  if (!CopyReplacing(source, destination)) {
    error = "Could not copy the default shine texture into the slot.";
    return {};
  }
  return destination;
}

} // namespace PrimedGunLauncher::Cannon

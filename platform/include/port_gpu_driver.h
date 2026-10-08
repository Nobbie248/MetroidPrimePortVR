#ifndef METROID_PRIME_PORT_PORT_GPU_DRIVER_H
#define METROID_PRIME_PORT_PORT_GPU_DRIVER_H
#include <string>
#include <vector>

// Custom Vulkan drivers on Android (Mesa Turnip for Adreno), loaded through
// libadrenotools. Drivers are the zips the Android emulators use (AdrenoToolsDrivers
// releases): a meta.json naming the driver library, plus that library. Installed ones
// live in <internal storage>/gpu_drivers/<id>/, since dlopen refuses shared storage.
// The setting `gpu_driver=<id>` picks one for the next start; empty = the system's.
// Everywhere but the arm64 phone build (MP_CUSTOM_GPU_DRIVERS, CMakeLists.txt) this
// is all stubs. PrimedGun: the Quest build (MP_ENABLE_OPENXR) has none.
namespace PortGpuDriver {

struct Driver {
  std::string id;          // its folder name, what the setting stores
  std::string name;        // meta.json name, e.g. "Turnip"
  std::string description;
  std::string version;     // meta.json driverVersion/packageVersion
  std::string library;     // meta.json libraryName
};

// Whether custom drivers can be loaded on this platform.
bool Supported();
std::vector<Driver> List();
// Installs a driver zip (a path or Android content:// address). Returns its id,
// or "" with `error` set.
std::string Install(const std::string& zipPath, std::string& error);
// Removes an installed driver (not the one this run started with).
bool Remove(const std::string& id);

// Before aurora_initialize: loads driver `id` and returns the directory Dawn
// should search first for libvulkan.so, or "" (system driver) when `id` is empty
// or the driver failed to load (logged; LoadError() says why).
std::string Prepare(const std::string& id);
// The driver this run started with ("" = system) and why loading one failed.
const std::string& Active();
const std::string& LoadError();
// For a driver this run doesn't use after all: main() didn't try it (it crashed
// starting last time), or Vulkan failed with it. Clears Active().
void SetLoadError(std::string why);

} // namespace PortGpuDriver

#endif // METROID_PRIME_PORT_PORT_GPU_DRIVER_H

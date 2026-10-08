// libmport_vkshim.so: what Dawn loads as "libvulkan.so" when a custom GPU driver
// (Turnip, through libadrenotools) is selected. Dawn has no way to take a
// vkGetInstanceProcAddr, only a directory to search for libvulkan.so, so the port
// copies this library there under that name and points it at the driver's loader
// (port_gpu_driver.cpp). Dawn resolves every other Vulkan entry point through it.
#include <stddef.h>

typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(void* instance, const char* name);

static PFN_vkGetInstanceProcAddr sGetInstanceProcAddr;

__attribute__((visibility("default"))) void mport_vkshim_set(PFN_vkGetInstanceProcAddr gipa) {
  sGetInstanceProcAddr = gipa;
}

__attribute__((visibility("default"))) PFN_vkVoidFunction vkGetInstanceProcAddr(void* instance, const char* name) {
  return sGetInstanceProcAddr != NULL ? sGetInstanceProcAddr(instance, name) : NULL;
}

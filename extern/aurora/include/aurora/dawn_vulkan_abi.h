// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
// The C entry points PrimedGun's patched Dawn adds for direct presentation (quest/dawn,
// aurora_vulkan_interop.inc): no Dawn C++ type crosses them. Vulkan handles are borrowed.
#define AURORA_DAWN_VULKAN_ABI 1
#ifdef __cplusplus
extern "C" {
#endif
// Dawn creates its VkInstance and VkDevice, and picks its VkPhysicalDevice, through these
// while they are configured (the OpenXR runtime's xrCreateVulkan*KHR). Results are VkResults.
typedef struct {
  void* userdata;
  int32_t (*createInstance)(void*, void* getProc, const void* info, const void* allocator, void** instance);
  int32_t (*createDevice)(void*, void* getProc, void* physical, const void* info, const void* allocator, void** device);
  int32_t (*getPhysicalDevice)(void*, void* instance, void** physical);
} AuroraDawnVulkanHooks;
typedef struct {
  void* instance;
  void* physicalDevice;
  void* device;
  uint32_t queueFamily;
  uint32_t queueIndex;
} AuroraDawnVulkanHandles;
typedef uint32_t (*AuroraDawnVulkanVersionFn)(void);
typedef int (*AuroraDawnVulkanConfigureFn)(const AuroraDawnVulkanHooks*);
typedef int (*AuroraDawnVulkanHandlesFn)(void* device, AuroraDawnVulkanHandles*);
typedef void* (*AuroraDawnVulkanWrapFn)(void* device, const void* textureDescriptor, uint64_t image);
typedef int (*AuroraDawnVulkanReleaseFn)(void* device, void* const* textures, uint32_t count);
typedef void* (*AuroraDawnVulkanLockFn)(void* device);
typedef void (*AuroraDawnVulkanUnlockFn)(void* guard);
typedef int (*AuroraDawnVulkanDrainFn)(void* device);
#ifdef __cplusplus
}
#endif

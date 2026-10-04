// SPDX-License-Identifier: GPL-3.0-or-later
// PrimedGun's Dawn patch (apply.py): compiled only inside the patched Dawn source build.
#pragma once
#include "dawn/native/DawnNative.h"
#include "aurora_dawn_vulkan_abi.h"
#include "src/dawn/common/vulkan_platform.h"
namespace dawn::native::vulkan {
// Dawn's vkCreateInstance, vkCreateDevice and vkEnumeratePhysicalDevices: the Vulkan loader's,
// or Aurora's hooks (AuroraDawnVulkanConfigure) while it has installed them.
PFN_vkCreateInstance AuroraCreateInstance(PFN_vkGetInstanceProcAddr proc);
PFN_vkCreateDevice AuroraCreateDevice(PFN_vkGetInstanceProcAddr proc, VkInstance instance);
PFN_vkEnumeratePhysicalDevices AuroraEnumeratePhysicalDevices(PFN_vkGetInstanceProcAddr proc, VkInstance instance);
}  // namespace dawn::native::vulkan

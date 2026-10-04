// SPDX-License-Identifier: GPL-3.0-or-later
// PrimedGun's Dawn patch (apply.py), compiled at the end of VulkanBackend.cpp against the pinned
// Dawn: Aurora's direct presentation entry points (aurora/dawn_vulkan_abi.h), after
// Wiicompiled_VR's Windows Vulkan binding.
//  - While Aurora has installed its hooks, Dawn creates its VkInstance and VkDevice and picks its
//    VkPhysicalDevice through them: the OpenXR runtime's XR_KHR_vulkan_enable2 calls, so the
//    device carries the runtime's extensions and can be bound to the session.
//  - AuroraDawnVulkanWrap makes a Dawn texture of a runtime-owned swapchain VkImage, which Aurora
//    then copies an eye into on Dawn's queue; AuroraDawnVulkanRelease returns such images to the
//    colour-attachment layout OpenXR expects at xrReleaseSwapchainImage.
#include "aurora_vulkan_hooks.h"
#include "src/dawn/native/ChainUtils.h"
#include "src/dawn/native/vulkan/PhysicalDeviceVk.h"
#include "src/dawn/native/vulkan/QueueVk.h"
#include <mutex>
namespace dawn::native::vulkan {
namespace {
AuroraDawnVulkanHooks auroraHooks{};
std::recursive_mutex auroraHooksMutex;
PFN_vkGetInstanceProcAddr auroraGetProc = nullptr;
VkInstance auroraInstance = VK_NULL_HANDLE;
::VkResult VKAPI_CALL AuroraCreateInstanceImpl(const VkInstanceCreateInfo* info,
                                               const VkAllocationCallbacks* allocator,
                                               VkInstance* instance) {
    std::lock_guard lock(auroraHooksMutex);
    if (auroraHooks.createInstance) {
        return static_cast<::VkResult>(auroraHooks.createInstance(
            auroraHooks.userdata, reinterpret_cast<void*>(auroraGetProc), info, allocator,
            reinterpret_cast<void**>(instance)));
    }
    return reinterpret_cast<PFN_vkCreateInstance>(auroraGetProc(nullptr, "vkCreateInstance"))(
        info, allocator, instance);
}
::VkResult VKAPI_CALL AuroraCreateDeviceImpl(VkPhysicalDevice physical,
                                             const VkDeviceCreateInfo* info,
                                             const VkAllocationCallbacks* allocator,
                                             VkDevice* device) {
    std::lock_guard lock(auroraHooksMutex);
    if (auroraHooks.createDevice) {
        return static_cast<::VkResult>(auroraHooks.createDevice(
            auroraHooks.userdata, reinterpret_cast<void*>(auroraGetProc), physical, info,
            allocator, reinterpret_cast<void**>(device)));
    }
    return reinterpret_cast<PFN_vkCreateDevice>(auroraGetProc(auroraInstance, "vkCreateDevice"))(
        physical, info, allocator, device);
}
::VkResult VKAPI_CALL AuroraEnumerateImpl(VkInstance instance,
                                          uint32_t* count,
                                          VkPhysicalDevice* devices) {
    std::lock_guard lock(auroraHooksMutex);
    if (!auroraHooks.getPhysicalDevice) {
        return reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
            auroraGetProc(instance, "vkEnumeratePhysicalDevices"))(instance, count, devices);
    }
    // The runtime's GPU only.
    void* physical = nullptr;
    const auto result = static_cast<::VkResult>(
        auroraHooks.getPhysicalDevice(auroraHooks.userdata, instance, &physical));
    if (result != VK_SUCCESS) {
        return result;
    }
    if (!devices) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count == 0) {
        return VK_INCOMPLETE;
    }
    devices[0] = static_cast<VkPhysicalDevice>(physical);
    *count = 1;
    return VK_SUCCESS;
}
struct AuroraDeviceGuard {
    decltype(std::declval<Device*>()->GetGuard()) guard;
    explicit AuroraDeviceGuard(Device* device) : guard(device->GetGuard()) {}
};
}  // namespace
PFN_vkCreateInstance AuroraCreateInstance(PFN_vkGetInstanceProcAddr proc) {
    std::lock_guard lock(auroraHooksMutex);
    if (!auroraHooks.createInstance) {
        return reinterpret_cast<PFN_vkCreateInstance>(proc(nullptr, "vkCreateInstance"));
    }
    auroraGetProc = proc;
    return AuroraCreateInstanceImpl;
}
PFN_vkCreateDevice AuroraCreateDevice(PFN_vkGetInstanceProcAddr proc, VkInstance instance) {
    std::lock_guard lock(auroraHooksMutex);
    if (!auroraHooks.createDevice) {
        return reinterpret_cast<PFN_vkCreateDevice>(proc(instance, "vkCreateDevice"));
    }
    auroraGetProc = proc;
    auroraInstance = instance;
    return AuroraCreateDeviceImpl;
}
PFN_vkEnumeratePhysicalDevices AuroraEnumeratePhysicalDevices(PFN_vkGetInstanceProcAddr proc,
                                                              VkInstance instance) {
    std::lock_guard lock(auroraHooksMutex);
    if (!auroraHooks.getPhysicalDevice) {
        return reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
            proc(instance, "vkEnumeratePhysicalDevices"));
    }
    auroraGetProc = proc;
    auroraInstance = instance;
    return AuroraEnumerateImpl;
}
extern "C" DAWN_NATIVE_EXPORT uint32_t AuroraDawnVulkanVersion() {
    return AURORA_DAWN_VULKAN_ABI;
}
extern "C" DAWN_NATIVE_EXPORT int AuroraDawnVulkanConfigure(const AuroraDawnVulkanHooks* hooks) {
    std::lock_guard lock(auroraHooksMutex);
    // Called before adapter discovery, or cleared after Aurora shutdown. The hooks' userdata
    // must outlive every device creation they serve.
    auroraHooks = hooks ? *hooks : AuroraDawnVulkanHooks{};
    return 1;
}
extern "C" DAWN_NATIVE_EXPORT int AuroraDawnVulkanGetHandles(void* handle,
                                                             AuroraDawnVulkanHandles* out) {
    auto* device = ToBackend(FromAPI(static_cast<WGPUDevice>(handle)));
    auto guard = device->GetGuard();
    // The OpenXR runtime uses the queue from its own threads; Aurora serializes those calls
    // with Dawn's through the device lock (AuroraDawnVulkanLock).
    if (!device->HasFeature(Feature::ImplicitDeviceSynchronization)) {
        return 0;
    }
    *out = {device->GetVkInstance(), ToBackend(device->GetPhysicalDevice())->GetVkPhysicalDevice(),
            device->GetVkDevice(), device->GetGraphicsQueueFamily(), 0};
    return 1;
}
extern "C" DAWN_NATIVE_EXPORT void* AuroraDawnVulkanWrap(void* handle,
                                                         const void* desc,
                                                         uint64_t image) {
    auto* device = ToBackend(FromAPI(static_cast<WGPUDevice>(handle)));
    auto guard = device->GetGuard();
    // As DeviceBase::CreateTexture: the trivial frontend defaults (dimension, mip and sample
    // counts), then validation, before the backend reads the descriptor.
    TextureDescriptor raw =
        reinterpret_cast<const TextureDescriptor*>(desc)->WithTrivialFrontendDefaults();
    UnpackedPtr<TextureDescriptor> descriptor;
    if (device->ConsumedError(ValidateAndUnpack(&raw), &descriptor)) {
        return nullptr;
    }
    if (device->ConsumedError(
            ValidateTextureDescriptor(device, descriptor, AllowMultiPlanarTextureFormat::No))) {
        return nullptr;
    }
    auto texture = SwapChainTexture::Create(
        device, descriptor, VkImage::CreateFromHandle(reinterpret_cast<::VkImage>(image)));
    // OpenXR hands an acquired image over in the colour-attachment layout.
    texture->SetIsSubresourceContentInitialized(true, texture->GetAllSubresources());
    texture->UpdateUsage(wgpu::TextureUsage::RenderAttachment, wgpu::ShaderStage::None,
                         texture->GetAllSubresources());
    return ToAPI(ReturnToAPI(std::move(texture)));
}
extern "C" DAWN_NATIVE_EXPORT int AuroraDawnVulkanRelease(void* handle,
                                                          void* const* textures,
                                                          uint32_t count) {
    auto* device = ToBackend(FromAPI(static_cast<WGPUDevice>(handle)));
    auto guard = device->GetGuard();
    auto* queue = ToBackend(device->GetQueue());
    auto* context = queue->GetPendingRecordingContext();
    for (uint32_t i = 0; i < count; ++i) {
        auto* texture = ToBackend(FromAPI(static_cast<WGPUTexture>(textures[i])));
        texture->TransitionUsageNow(context, wgpu::TextureUsage::RenderAttachment,
                                    wgpu::ShaderStage::None, texture->GetAllSubresources());
    }
    return !device->ConsumedError(queue->SubmitPendingCommands());
}
extern "C" DAWN_NATIVE_EXPORT void* AuroraDawnVulkanLock(void* handle) {
    auto* device = ToBackend(FromAPI(static_cast<WGPUDevice>(handle)));
    return new AuroraDeviceGuard(device);
}
extern "C" DAWN_NATIVE_EXPORT void AuroraDawnVulkanUnlock(void* guard) {
    delete static_cast<AuroraDeviceGuard*>(guard);
}
extern "C" DAWN_NATIVE_EXPORT int AuroraDawnVulkanDrain(void* handle) {
    auto* device = ToBackend(FromAPI(static_cast<WGPUDevice>(handle)));
    auto guard = device->GetGuard();
    if (device->ConsumedError(ToBackend(device->GetQueue())->SubmitPendingCommands())) {
        return 0;
    }
    return device->fn.QueueWaitIdle(ToBackend(device->GetQueue())->GetVkQueue()) == VK_SUCCESS;
}
}  // namespace dawn::native::vulkan

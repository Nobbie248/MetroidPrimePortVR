#ifndef AURORA_VULKAN_INTEROP_H
#define AURORA_VULKAN_INTEROP_H

#ifdef __cplusplus
#include <cstdint>
extern "C" {
#else
#include "stdbool.h"
#include "stdint.h"
#endif

/**
 * Android/Vulkan counterpart of aurora/d3d12_interop.h.
 *
 * Dawn's Vulkan device cannot be bound to an OpenXR session (the pinned Dawn
 * package exposes no VkDevice/VkQueue and would not enable the runtime's
 * required extensions anyway), so the OpenXR side owns a second VkDevice
 * created through XR_KHR_vulkan_enable2. The two devices meet on
 * AHardwareBuffer-backed images: Aurora imports each buffer as Dawn shared
 * texture memory and copies an eye into it inside the frame worker's command
 * buffer; the OpenXR side imports the same buffer on its own device and copies
 * it into the acquired XrSwapchain image. Ordering across the two devices uses
 * Android sync file descriptors (Dawn's SharedFenceSyncFD).
 *
 * Every handle in this API is a plain C value so the runtime never includes
 * Dawn's C++ headers.
 */

enum { AURORA_VULKAN_STEREO_MAX_TARGETS = 2 };
// The eyes, then the settings panel's quad-layer image when one was given.
enum { AURORA_VULKAN_STEREO_MAX_RELEASES = AURORA_VULKAN_STEREO_MAX_TARGETS + 1 };

/**
 * Borrowed facts about Aurora's Dawn Vulkan device. colorVkFormat is the
 * VkFormat enum value matching Aurora's single-sample eye output.
 */
typedef struct {
  int64_t colorVkFormat;
  bool sharedTextureMemoryAHardwareBuffer;
  bool sharedFenceSyncFd;
} AuroraVulkanNativeHandles;

/**
 * One AHardwareBuffer the next Aurora stereo sink must copy an eye into. The
 * buffer is imported into Dawn on first use and the import is cached for as
 * long as the bridge lives, so callers should recycle a small ring of buffers
 * rather than allocating per frame.
 *
 * acquireFenceFd is a sync file descriptor Dawn waits on before writing (the
 * OpenXR side's previous copy out of this buffer), or -1 when the buffer has no
 * pending reader. Ownership of the descriptor transfers to Aurora on a
 * successful aurora_vulkan_set_stereo_targets call; on failure the caller
 * still owns it.
 *
 * acquireImageLayout is the VkImageLayout the buffer's image currently holds
 * (VK_IMAGE_LAYOUT_UNDEFINED when the contents may be discarded). It must
 * equal the layout the OpenXR side's release barrier left the image in.
 */
typedef struct {
  struct AHardwareBuffer* buffer;
  uint32_t width;
  uint32_t height;
  int64_t vkFormat;
  int acquireFenceFd;
  int32_t acquireImageLayout;
} AuroraVulkanStereoTarget;

/**
 * Per-target result handed to the submitted callback. releaseFenceFd is a
 * sync file descriptor that signals once Aurora's copy into the buffer has
 * completed on Dawn's queue (-1 if Dawn reported no fence); ownership passes to
 * the callee. releasedImageLayout is the VkImageLayout Dawn's release barrier
 * left the image in; the OpenXR side's acquire barrier must start from it.
 */
typedef struct {
  int releaseFenceFd;
  int32_t releasedImageLayout;
} AuroraVulkanStereoRelease;

/**
 * Fired when Aurora either finishes or abandons the stereo sink. `success`
 * guarantees that the copies were submitted and that every release entry is
 * valid. Otherwise `gpuWorkQueued` says whether the copies may have reached
 * Dawn's queue before the failure (the shared buffers may then be written with
 * no fence to wait on) or nothing was recorded at all. The callback runs on
 * Aurora's frame worker while its queue-submit mutex is held: it may record and
 * submit work on the OpenXR side's own Vulkan queue, but must not wait for the
 * GPU or re-enter Aurora.
 */
typedef void (*AuroraVulkanStereoSubmittedCallback)(uint64_t frameToken, bool success, bool gpuWorkQueued,
                                                    const AuroraVulkanStereoRelease* releases,
                                                    uint32_t releaseCount, void* userdata);

/** Returns false unless the active Aurora backend is Dawn Vulkan. */
bool aurora_vulkan_get_native_handles(AuroraVulkanNativeHandles* handles);

/**
 * Installs the internal AHardwareBuffer stereo sink. Call while Aurora's frame
 * worker is idle, after aurora_initialize(). Fails when the Dawn device was not
 * created with the AHardwareBuffer shared-memory and sync-fd fence features.
 */
bool aurora_vulkan_enable_stereo_bridge(AuroraVulkanStereoSubmittedCallback submitted,
                                        void* userdata);

/**
 * Publishes the buffer(s) for frameToken. Immersive projection frames supply
 * two targets; virtual-screen quad frames supply one. Exactly one frame may be
 * pending at a time.
 */
bool aurora_vulkan_set_stereo_targets(uint64_t frameToken,
                                      const AuroraVulkanStereoTarget* targets,
                                      uint32_t targetCount);

/**
 * The same, plus the headset settings panel's quad-layer buffer when `panel` is
 * not null (aurora_set_stereo_panel_layer): Aurora copies the panel into it, or
 * a transparent image while the panel is not showing, with the eyes. Its
 * release entry follows the eyes' in the submitted callback, whose
 * releaseCount then counts it too.
 */
bool aurora_vulkan_set_stereo_targets_with_panel(uint64_t frameToken,
                                                 const AuroraVulkanStereoTarget* targets,
                                                 uint32_t targetCount,
                                                 const AuroraVulkanStereoTarget* panel);

/**
 * Withdraws frameToken only while its targets have not been encoded. Semantics
 * match aurora_d3d12_cancel_stereo_targets: false means the worker already
 * owns encoded work and the submitted callback remains the completion
 * authority. A successful cancellation closes the acquire descriptors it was
 * given and fires no callback.
 */
bool aurora_vulkan_cancel_stereo_targets(uint64_t frameToken);

/**
 * Releases Dawn's imports of buffers the caller is about to free (the eyes'
 * ring, when the render resolution changes), which would otherwise keep every
 * old buffer alive for the bridge's lifetime. Dawn's own reference to each
 * buffer lasts until its queue is done with it, so the caller need not wait for
 * Dawn. Call with no frame token pending; false if one is, releasing nothing.
 */
bool aurora_vulkan_forget_stereo_buffers(struct AHardwareBuffer* const* buffers, uint32_t count);

/**
 * Removes the sink and releases the cached Dawn imports. The worker must be
 * idle. Returns false only when Dawn could not be drained, in which case the
 * bridge is retained for the process lifetime.
 */
bool aurora_vulkan_disable_stereo_bridge();

#ifdef __cplusplus
}
#endif

#endif

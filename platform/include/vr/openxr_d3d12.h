// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MP_ENABLE_OPENXR) && defined(_WIN32)

#include "vr/openxr_backend.h"
#include "vr/openxr_runtime.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace PortVr {

// The frame vocabulary is shared with the Vulkan backend (vr/openxr_backend.h).
// These aliases keep the D3D12 spelling that the replay tests and the original
// integration code were written against.
using OpenXRD3D12FrameMode = OpenXRFrameMode;
using OpenXRD3D12BeginStatus = OpenXRBeginStatus;
using OpenXRD3D12SubmissionStatus = OpenXRSubmissionStatus;
using OpenXRD3D12Presentation = OpenXRPresentation;
using OpenXRD3D12Frame = OpenXRBackendFrame;

struct OpenXRD3D12GraphicsRequirements {
    uint32_t adapter_luid_low = 0;
    int32_t adapter_luid_high = 0;
    uint32_t minimum_feature_level = 0;
};

// Same-device Dawn/OpenXR D3D12 backend.
//
// Startup is deliberately split in two. QueryGraphicsRequirements() runs
// after OpenXRRuntime::Initialize() but before aurora_initialize(), allowing
// its LUID to be placed in AuroraConfig. BindAurora() runs afterwards and
// rejects any device/queue that does not match the queried runtime adapter.
//
// All methods from BeginFrame() through FinishFrame(), plus PollEvents on the
// associated OpenXRRuntime, belong to one XR pacing thread. Aurora's frame
// worker never calls OpenXR: its post-submit callback only publishes a token
// that WaitForSubmission() consumes. This is the synchronization boundary
// required by the asynchronous sealed-frame renderer.
class OpenXRD3D12Backend final {
public:
    explicit OpenXRD3D12Backend(OpenXRLogCallback logger = {});
    ~OpenXRD3D12Backend();

    OpenXRD3D12Backend(const OpenXRD3D12Backend&) = delete;
    OpenXRD3D12Backend& operator=(const OpenXRD3D12Backend&) = delete;
    OpenXRD3D12Backend(OpenXRD3D12Backend&&) = delete;
    OpenXRD3D12Backend& operator=(OpenXRD3D12Backend&&) = delete;

    bool QueryGraphicsRequirements(OpenXRRuntime& runtime);
    bool BindAurora(OpenXRRuntime& runtime);

    // The eyes' resolution, as a scale of the size the runtime recommends (the
    // session starts at OpenXRConfig::resolution_scale). A new one applies from
    // the next BeginFrame or PreparePacket: each swapchain pair is rebuilt at it
    // the next time it is the pair Aurora writes, so the pair on display is
    // never touched. A size the runtime cannot allocate keeps the one before,
    // until the scale changes again.
    void SetRenderScale(float scale);

    OpenXRD3D12BeginStatus BeginFrame(const OpenXRD3D12Presentation& presentation,
                                      OpenXRD3D12Frame& frame);

    // timeout_ms == UINT32_MAX waits until Aurora publishes this token or
    // Shutdown() interrupts the wait. A timeout does not release XR images;
    // the caller may keep pacing with RepeatFrame while Aurora still owns them.
    OpenXRD3D12SubmissionStatus WaitForSubmission(const OpenXRD3D12Frame& frame,
                                                  uint32_t timeout_ms = UINT32_MAX);

    // Withdraws this token only if Aurora has not encoded it. On success no GPU
    // command can reference the acquired images, and FinishFrame(frame, false)
    // is required to release them and close the compositor frame.
    bool TryCancelPendingFrame(OpenXRD3D12Frame& frame);

    // Ends the current compositor cycle with the last completed layer and starts
    // another, without releasing or changing Aurora's pending images/render token.
    // The original render poses remain attached to the pending and retained images.
    bool RepeatFrame(const OpenXRD3D12Frame& frame);

    // Releases acquired images and calls xrEndFrame. submit_layer must only be
    // true after WaitForSubmission returned Success. Immersive frames submit
    // XrCompositionLayerProjection; virtual-screen frames submit an
    // XrCompositionLayerQuad using the single mono target, placed as the
    // presentation's quad_anchored/quad_pose describe. If no new usable layer is
    // available, resubmits the retained layer using the current display time.
    bool FinishFrame(OpenXRD3D12Frame& frame, bool submit_layer);

    // Render-first path when interpolation is off. Aurora renders into acquired
    // non-retained XR images while no compositor frame is open. BeginFrameForPacket
    // accepts only a completed packet; CopyRenderedEyes verifies the already queued
    // bridge copy. FinishFrame releases the images and submits their original poses.
    OpenXRBeginStatus PreparePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet);
    bool TryCancelPendingPacket(OpenXRBackendFrame& packet);
    OpenXRBeginStatus BeginFrameForPacket(const OpenXRBackendFrame& packet, OpenXRBackendFrame& frame);
    OpenXRSubmissionStatus CopyRenderedEyes(const OpenXRBackendFrame& frame);
    OpenXRBeginStatus KeepAliveCycle();

    // Call on the XR owner thread after Aurora's worker is idle and before
    // aurora_shutdown(). Safe to repeat. False means a submitted D3D12 command
    // could not be fenced; the caller must retain this backend and its runtime
    // for the process lifetime instead of destroying possibly live resources.
    bool Shutdown();

    bool IsBound() const;
    // False once the settings panel's own layer could not be set up; the panel
    // is then drawn into the eyes again.
    bool PanelLayerAvailable() const;
    const OpenXRD3D12GraphicsRequirements& GraphicsRequirements() const;
    int64_t SwapchainFormat() const;
    const std::string& LastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace PortVr

#endif // defined(MP_ENABLE_OPENXR) && defined(_WIN32)

// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MP_ENABLE_OPENXR)

#include "vr/openxr_runtime.h"

#include <string_view>

namespace PortVr {

// The headset's camera view of the room (XR_FB_passthrough) around the virtual
// screen, as PPSSPP VR and DolphinXR show it: one reconstruction layer,
// submitted before every other layer so they all cover it, with the frame's
// blend mode left OPAQUE. Its objects are created the first time the view is
// wanted and paused whenever it is not, so a race never keeps the cameras
// running. On a Quest the app must also declare com.oculus.feature.PASSTHROUGH
// in its manifest, or the runtime accepts every call and composites nothing.
//
// Not synchronized: every call belongs to the thread that ends the session's
// frames, while the session exists. The handles are the session's children:
// Destroy() before xrDestroySession, which otherwise frees them itself.
class OpenXRPassthrough final {
public:
    explicit OpenXRPassthrough(OpenXRLogCallback logger = {});

    // Starts (creating it on first use) or pauses the view. A runtime without
    // XR_FB_passthrough, or one that refuses it, is logged once and leaves the
    // view off until Destroy().
    void SetRunning(OpenXRRuntime& runtime, bool running);

    // The layer to submit first while the view runs, otherwise null.
    const XrCompositionLayerBaseHeader* Layer() const noexcept;

    void Destroy() noexcept;

private:
    bool Create(OpenXRRuntime& runtime);
    // gives_up: the failure turns the view off until Destroy().
    void LogResult(const OpenXRRuntime& runtime, std::string_view operation, XrResult result, bool gives_up) const;
    void Log(OpenXRLogLevel level, std::string_view message) const noexcept;

    OpenXRLogCallback m_logger;
    PFN_xrCreatePassthroughFB m_create_passthrough = nullptr;
    PFN_xrDestroyPassthroughFB m_destroy_passthrough = nullptr;
    PFN_xrPassthroughStartFB m_start_passthrough = nullptr;
    PFN_xrPassthroughPauseFB m_pause_passthrough = nullptr;
    PFN_xrCreatePassthroughLayerFB m_create_layer = nullptr;
    PFN_xrDestroyPassthroughLayerFB m_destroy_layer = nullptr;
    PFN_xrPassthroughLayerResumeFB m_resume_layer = nullptr;
    PFN_xrPassthroughLayerPauseFB m_pause_layer = nullptr;
    XrPassthroughFB m_passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB m_layer = XR_NULL_HANDLE;
    XrCompositionLayerPassthroughFB m_composition{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    bool m_running = false;
    bool m_failed = false;
};

} // namespace PortVr

#endif // defined(MP_ENABLE_OPENXR)

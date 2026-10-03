// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(MP_ENABLE_OPENXR)

#include "vr/openxr_passthrough.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

namespace PortVr {

OpenXRPassthrough::OpenXRPassthrough(OpenXRLogCallback logger) : m_logger(std::move(logger)) {}

void OpenXRPassthrough::SetRunning(OpenXRRuntime& runtime, bool running) {
    if (running == m_running || m_failed || !runtime.HasSession()) {
        return;
    }
    if (m_passthrough == XR_NULL_HANDLE) {
        // Only a start reaches here: the view and its layer are created running.
        m_failed = !Create(runtime);
        m_running = !m_failed;
        return;
    }
    if (running) {
        XrResult result = m_start_passthrough(m_passthrough);
        runtime.ObserveResult(result);
        if (XR_SUCCEEDED(result)) {
            result = m_resume_layer(m_layer);
            runtime.ObserveResult(result);
        }
        if (XR_FAILED(result)) {
            LogResult(runtime, "resuming the passthrough view", result, true);
            m_failed = true;
            return;
        }
        m_running = true;
        Log(OpenXRLogLevel::Info, "OpenXR passthrough resumed");
        return;
    }
    // The layer is no longer submitted from here on, whatever the runtime answers.
    m_running = false;
    const XrResult layer_result = m_pause_layer(m_layer);
    runtime.ObserveResult(layer_result);
    const XrResult result = m_pause_passthrough(m_passthrough);
    runtime.ObserveResult(result);
    if (XR_FAILED(layer_result) || XR_FAILED(result)) {
        LogResult(runtime, "pausing the passthrough view", XR_FAILED(layer_result) ? layer_result : result,
                  false);
        return;
    }
    Log(OpenXRLogLevel::Info, "OpenXR passthrough paused");
}

const XrCompositionLayerBaseHeader* OpenXRPassthrough::Layer() const noexcept {
    return m_running ? reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_composition) : nullptr;
}

void OpenXRPassthrough::Destroy() noexcept {
    if (m_layer != XR_NULL_HANDLE) {
        m_destroy_layer(m_layer);
    }
    if (m_passthrough != XR_NULL_HANDLE) {
        m_destroy_passthrough(m_passthrough);
    }
    m_layer = XR_NULL_HANDLE;
    m_passthrough = XR_NULL_HANDLE;
    m_running = false;
    m_failed = false;
}

bool OpenXRPassthrough::Create(OpenXRRuntime& runtime) {
    const auto& extensions = runtime.EnabledExtensions();
    if (std::find(extensions.begin(), extensions.end(), XR_FB_PASSTHROUGH_EXTENSION_NAME) == extensions.end()) {
        Log(OpenXRLogLevel::Warning,
            "OpenXR passthrough unavailable: the runtime does not offer " XR_FB_PASSTHROUGH_EXTENSION_NAME);
        return false;
    }
    if (!runtime.LoadFunction("xrCreatePassthroughFB", &m_create_passthrough) ||
        !runtime.LoadFunction("xrDestroyPassthroughFB", &m_destroy_passthrough) ||
        !runtime.LoadFunction("xrPassthroughStartFB", &m_start_passthrough) ||
        !runtime.LoadFunction("xrPassthroughPauseFB", &m_pause_passthrough) ||
        !runtime.LoadFunction("xrCreatePassthroughLayerFB", &m_create_layer) ||
        !runtime.LoadFunction("xrDestroyPassthroughLayerFB", &m_destroy_layer) ||
        !runtime.LoadFunction("xrPassthroughLayerResumeFB", &m_resume_layer) ||
        !runtime.LoadFunction("xrPassthroughLayerPauseFB", &m_pause_layer)) {
        Log(OpenXRLogLevel::Warning, "OpenXR passthrough unavailable: " + runtime.LastError().message);
        return false;
    }

    XrPassthroughCreateInfoFB create_info{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    create_info.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    XrResult result = m_create_passthrough(runtime.Session(), &create_info, &m_passthrough);
    runtime.ObserveResult(result);
    if (XR_FAILED(result)) {
        m_passthrough = XR_NULL_HANDLE;
        LogResult(runtime, "xrCreatePassthroughFB", result, true);
        return false;
    }

    XrPassthroughLayerCreateInfoFB layer_info{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    layer_info.passthrough = m_passthrough;
    layer_info.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    layer_info.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    result = m_create_layer(runtime.Session(), &layer_info, &m_layer);
    runtime.ObserveResult(result);
    if (XR_FAILED(result)) {
        m_layer = XR_NULL_HANDLE;
        m_destroy_passthrough(m_passthrough);
        m_passthrough = XR_NULL_HANDLE;
        LogResult(runtime, "xrCreatePassthroughLayerFB", result, true);
        return false;
    }

    // A reconstruction layer has no space of its own; the layers after it cover it.
    m_composition = {XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    m_composition.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    m_composition.space = XR_NULL_HANDLE;
    m_composition.layerHandle = m_layer;
    Log(OpenXRLogLevel::Info, "OpenXR passthrough started");
    return true;
}

void OpenXRPassthrough::LogResult(const OpenXRRuntime& runtime, std::string_view operation, XrResult result,
                                  bool gives_up) const {
    char name[XR_MAX_RESULT_STRING_SIZE]{};
    std::ostringstream message;
    message << "OpenXR passthrough: " << operation << " failed: ";
    if (XR_SUCCEEDED(xrResultToString(runtime.Instance(), result, name))) {
        message << name;
    } else {
        message << static_cast<int32_t>(result);
    }
    if (gives_up) {
        message << "; the view stays off for this session";
    }
    Log(OpenXRLogLevel::Warning, message.str());
}

void OpenXRPassthrough::Log(OpenXRLogLevel level, std::string_view message) const noexcept {
    if (!m_logger) {
        return;
    }
    try {
        m_logger(level, message);
    } catch (...) {
        // A diagnostic callback must never break frame submission.
    }
}

} // namespace PortVr

#endif // defined(MP_ENABLE_OPENXR)

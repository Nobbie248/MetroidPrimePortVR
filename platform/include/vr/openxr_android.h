// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MP_ENABLE_OPENXR) && defined(__ANDROID__)

#include "vr/openxr_runtime.h"

#include <string>

namespace PortVr {

// Android-specific OpenXR bootstrap.
//
// The Khronos loader on Android must be told which JavaVM and Context it lives
// in before any other OpenXR entry point is called (xrInitializeLoaderKHR), and
// xrCreateInstance must chain an XrInstanceCreateInfoAndroidKHR naming the
// activity. Both come from SDL's Android glue (the SDLActivity that hosts the
// runtime), so no extra JNI surface is needed. DolphinXR found that the loader
// context must be the *activity*, not the application context: with a plain
// application context the Quest runtime never leaves XR_SESSION_STATE_IDLE.
//
// Call order: OpenXRAndroidInitializeLoader() once, before OpenXRRuntime::Initialize;
// then pass OpenXRAndroidInstanceCreateNext() as OpenXRConfig::instance_create_next.
bool OpenXRAndroidInitializeLoader(OpenXRLogCallback logger, std::string* error);
const void* OpenXRAndroidInstanceCreateNext();

// Optional XR_KHR_android_thread_settings hint for the calling thread. Failure
// is not an error: some Quest runtime builds advertise the extension but reject
// particular thread types, so the caller only logs the outcome.
enum class OpenXRAndroidThreadType {
    ApplicationMain,
    ApplicationWorker,
    RendererMain,
    RendererWorker,
};
bool OpenXRAndroidRegisterThread(OpenXRRuntime& runtime, OpenXRAndroidThreadType type);
// The same hint for another thread, named by its Linux thread id (gettid).
bool OpenXRAndroidRegisterThreadId(OpenXRRuntime& runtime, OpenXRAndroidThreadType type, uint32_t thread_id);

// Asks the hosting activity to finish (PrimedGunVrActivity.requestQuit in quest/),
// which ends the game and its process: a standalone headset has no desktop to fall
// back to. error is empty for a plain exit (Quit in the system menu); otherwise the
// launcher shows it. Callable from any thread; a no-op for an activity without the
// method (upstream's phone app).
void OpenXRAndroidRequestQuit(const std::string& error);

} // namespace PortVr

#endif // defined(MP_ENABLE_OPENXR) && defined(__ANDROID__)

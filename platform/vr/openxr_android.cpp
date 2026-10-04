// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(MP_ENABLE_OPENXR) && defined(__ANDROID__)

#include "vr/openxr_android.h"

#include <SDL3/SDL_system.h>
#include <jni.h>
#include <sys/syscall.h>
#include <unistd.h>

#define XR_USE_PLATFORM_ANDROID
#include <openxr/openxr_platform.h>

#include <mutex>
#include <sstream>

namespace PortVr {
namespace {

std::mutex g_mutex;
JavaVM* g_vm = nullptr;
jobject g_activity = nullptr; // global reference, never released: it lives as long as the process
bool g_loader_initialized = false;
XrInstanceCreateInfoAndroidKHR g_instance_create{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};

void Emit(const OpenXRLogCallback& logger, OpenXRLogLevel level, const std::string& message) noexcept {
    if (!logger) {
        return;
    }
    try {
        logger(level, message);
    } catch (...) {
    }
}

bool ResolveAndroidObjectsLocked(std::string* error) {
    if (g_vm != nullptr && g_activity != nullptr) {
        return true;
    }
    // SDL attaches the calling thread to the VM on demand and hands back the
    // activity as a local reference the caller owns.
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (env == nullptr || activity == nullptr) {
        if (error != nullptr) {
            *error = "SDL has not published the Android activity yet";
        }
        return false;
    }
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK || vm == nullptr) {
        env->DeleteLocalRef(activity);
        if (error != nullptr) {
            *error = "JNIEnv::GetJavaVM failed";
        }
        return false;
    }
    g_vm = vm;
    g_activity = env->NewGlobalRef(activity);
    env->DeleteLocalRef(activity);
    if (g_activity == nullptr) {
        if (error != nullptr) {
            *error = "could not pin the Android activity";
        }
        return false;
    }
    return true;
}

} // namespace

bool OpenXRAndroidInitializeLoader(OpenXRLogCallback logger, std::string* error) {
    std::lock_guard lock(g_mutex);
    if (g_loader_initialized) {
        return true;
    }
    if (!ResolveAndroidObjectsLocked(error)) {
        return false;
    }

    PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
    XrResult result = xrGetInstanceProcAddr(
        XR_NULL_HANDLE, "xrInitializeLoaderKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&initialize_loader));
    if (XR_FAILED(result) || initialize_loader == nullptr) {
        if (error != nullptr) {
            std::ostringstream message;
            message << "xrInitializeLoaderKHR is unavailable (" << static_cast<int>(result) << ')';
            *error = message.str();
        }
        return false;
    }

    // The activity is deliberately used as the loader context (an Activity is
    // a Context). Meta's runtime keys activity readiness off it; with a plain
    // application context the session parks in IDLE forever (DolphinXR finding).
    XrLoaderInitInfoAndroidKHR loader_init{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loader_init.applicationVM = g_vm;
    loader_init.applicationContext = g_activity;
    result = initialize_loader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loader_init));
    if (XR_FAILED(result)) {
        if (error != nullptr) {
            std::ostringstream message;
            message << "xrInitializeLoaderKHR failed (" << static_cast<int>(result) << ')';
            *error = message.str();
        }
        return false;
    }

    g_instance_create.next = nullptr;
    g_instance_create.applicationVM = g_vm;
    g_instance_create.applicationActivity = g_activity;
    g_loader_initialized = true;
    Emit(logger, OpenXRLogLevel::Info, "OpenXR Android loader initialized against the SDL activity");
    return true;
}

const void* OpenXRAndroidInstanceCreateNext() {
    std::lock_guard lock(g_mutex);
    return g_loader_initialized ? &g_instance_create : nullptr;
}

bool OpenXRAndroidRegisterThread(OpenXRRuntime& runtime, OpenXRAndroidThreadType type) {
    return OpenXRAndroidRegisterThreadId(runtime, type, static_cast<uint32_t>(syscall(SYS_gettid)));
}

bool OpenXRAndroidRegisterThreadId(OpenXRRuntime& runtime, OpenXRAndroidThreadType type, uint32_t thread_id) {
    if (!runtime.HasSession()) {
        return false;
    }
    const auto& extensions = runtime.EnabledExtensions();
    bool enabled = false;
    for (const auto& extension : extensions) {
        if (extension == XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME) {
            enabled = true;
            break;
        }
    }
    if (!enabled) {
        return false;
    }
    PFN_xrSetAndroidApplicationThreadKHR set_thread = nullptr;
    if (!runtime.LoadFunction("xrSetAndroidApplicationThreadKHR", &set_thread) || set_thread == nullptr) {
        return false;
    }
    XrAndroidThreadTypeKHR xr_type = XR_ANDROID_THREAD_TYPE_APPLICATION_WORKER_KHR;
    switch (type) {
    case OpenXRAndroidThreadType::ApplicationMain:
        xr_type = XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR;
        break;
    case OpenXRAndroidThreadType::ApplicationWorker:
        xr_type = XR_ANDROID_THREAD_TYPE_APPLICATION_WORKER_KHR;
        break;
    case OpenXRAndroidThreadType::RendererMain:
        xr_type = XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR;
        break;
    case OpenXRAndroidThreadType::RendererWorker:
        xr_type = XR_ANDROID_THREAD_TYPE_RENDERER_WORKER_KHR;
        break;
    }
    XrResult result = set_thread(runtime.Session(), xr_type, thread_id);
    if (XR_FAILED(result) && type == OpenXRAndroidThreadType::RendererWorker) {
        // Some Quest runtime builds advertise the extension but reject the
        // renderer-worker type; the renderer-main hint still raises priority.
        result = set_thread(runtime.Session(), XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, thread_id);
    }
    runtime.ObserveResult(result);
    return XR_SUCCEEDED(result);
}

void OpenXRAndroidRequestQuit(const std::string& error) {
    // SDL attaches the calling thread (the XR pacing thread, say) to the VM.
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (env == nullptr || activity == nullptr) {
        return;
    }
    jclass activity_class = env->GetObjectClass(activity);
    const jmethodID request_quit = env->GetMethodID(activity_class, "requestQuit", "(Ljava/lang/String;)V");
    if (request_quit != nullptr) {
        jstring text = env->NewStringUTF(error.c_str());
        env->CallVoidMethod(activity, request_quit, text);
        env->DeleteLocalRef(text);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    env->DeleteLocalRef(activity_class);
    env->DeleteLocalRef(activity);
}

} // namespace PortVr

#endif // defined(MP_ENABLE_OPENXR) && defined(__ANDROID__)

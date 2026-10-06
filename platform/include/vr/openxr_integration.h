// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <aurora/aurora.h>

#include <array>
#include <cstdint>
#include <string>

namespace PortVr {

enum class OpenXRStartupResult {
    Disabled,
    Prepared,
    Unavailable,
};

// Performs the OpenXR instance/system and graphics-requirements work that must
// happen before Aurora selects an adapter. On success this may force the
// backend and populate AuroraConfig's XR interop fields.
OpenXRStartupResult OpenXRPrepareAurora(AuroraConfig& config);

// Completes the graphics binding and starts the asynchronous XR pacing thread.
// Call after aurora_initialize(), while Aurora's frame worker is idle.
bool OpenXRStartAfterAurora(AuroraBackend active_backend);

// Stops publishing stereo work, drains the pacing thread, and destroys the XR
// session before aurora_shutdown(). Safe to call after partial initialization.
void OpenXRShutdownBeforeAurora() noexcept;

// Services an XR-owned teardown request at the producer's safe frame boundary:
// after aurora_begin_frame() has granted the worker's prepare phase and before
// aurora_end_frame_tagged() seals the current frame. No-op unless the pacing
// thread has fallen back to desktop rendering.
void OpenXRServiceProducerFrameBoundary() noexcept;

bool OpenXRIsRunning() noexcept;

// Writes the controllers the pacing thread last published to the virtual
// gamepad (Gamepad controller mode). Call it from the game thread wherever the
// game is about to read controllers: the pacing thread deliberately leaves SDL
// alone. Cheap and safe to call when VR is off.
void OpenXRApplyControllerState() noexcept;
std::string OpenXRLastError();

// True where the headset is the only display there is (the Quest build): there VR
// failing to start ends the game instead of falling back to a desktop window.
bool OpenXRHeadsetIsOnlyDisplay() noexcept;

// On the Quest, ends the app (the launcher then shows error; empty for a plain
// exit). A no-op everywhere else.
void OpenXRRequestAppQuit(const std::string& error);

// Recenters on where the player is now: it moves the immersive view's origin
// and re-places the anchored virtual screen upright in front of them. Position
// only: the forward direction and the horizon come from the OpenXR reference
// space and are never relatched from the headset. Callable from any thread;
// serviced once per frame on the XR pacing thread.
void OpenXRRequestRecenter() noexcept;

// Called on the pacing thread whenever a recenter is serviced (an explicit
// request or the runtime's own reference-space change), so the game layer can
// relatch whatever it anchors on the head (the cannon base, the height).
void OpenXRSetRecenterCallback(void (*callback)()) noexcept;

// Sets a fixed pitch of the game camera for a player sitting reclined, in
// degrees, clamped to kVrLeanBackDegreesLimit. Applies to the immersive view
// only; callable from any thread and read once per published frame.
void OpenXRSetLeanBackDegrees(float degrees) noexcept;

// Shows the room through the headset's cameras around the virtual screen,
// never during immersive play. Only the standalone (Quest) backend offers it.
void OpenXRSetPassthrough(bool enabled) noexcept;

// The headset's eye resolution, as a scale of the size the OpenXR runtime
// recommends (clamped to kVrRenderScaleMin..Max). Callable from any thread.
void OpenXRSetRenderScale(float scale) noexcept;

// The headset's display refresh rate in Hz (XR_FB_display_refresh_rate): the
// nearest rate the runtime offers is requested at each session start and again
// when this changes; 0 leaves the runtime's own. Callable from any thread.
void OpenXRSetDisplayRefreshRate(float hz) noexcept;

// Asks the runtime for the vr_performance_level setting again now; otherwise it is
// asked for at each session start (XR_EXT_performance_settings). Callable from
// any thread.
void OpenXRReapplyPerformanceLevel() noexcept;

// The left eye's image size: width x height is what the headset is shown now,
// scaled_width x scaled_height what `scale` gives on this headset. All zero
// while no OpenXR session runs.
struct OpenXREyeResolution {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t scaled_width = 0;
    uint32_t scaled_height = 0;
};
OpenXREyeResolution OpenXRGetEyeResolution(float scale) noexcept;

// Live scene interpolation at the headset's own display deadlines (retained
// replay; a later Quest optimisation). 0 = Off, 1 = Auto, else 72/90/120.
void OpenXRSetFrameInterpolationFps(uint32_t target) noexcept;
bool OpenXRFrameInterpolationAvailable() noexcept;

struct OpenXRFrameTiming {
    float headset_hz = 0;
    float rendered_fps = 0; // Newly rendered pairs; excludes retained-layer repeats.
};
OpenXRFrameTiming OpenXRGetFrameTiming() noexcept;

// One XR frame's worth of poses, published by the pacing thread when it hands
// Aurora a stereo packet. The game thread draws once per request: it waits for
// the next one in place of its frame cap, poses the camera from head_* (the
// located centre pose in the application space, metres, +Y up, -Z forward;
// subtract base_position for the latched immersive origin), seals the frame
// with content_tag, and Aurora replays it per eye with the matching packet.
struct OpenXRFrameRequest {
    uint64_t serial = 0;
    int64_t predicted_display_time = 0; // XrTime
    uint64_t display_time_nanos = 0;    // steady_clock, 0 when unconvertible
    bool immersive = false;
    uint64_t content_tag = 0;
    float units_per_meter = 1.5f;
    bool head_valid = false;
    std::array<float, 3> head_position{};
    std::array<float, 4> head_orientation{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<std::array<float, 3>, 2> eye_position{};
    std::array<std::array<float, 4>, 2> eye_orientation{};
    std::array<std::array<float, 4>, 2> eye_fov{}; // left, right, up, down angles (radians)
    std::array<float, 3> base_position{};
    bool base_valid = false;
};

// Blocks until the pacing thread publishes a request newer than request.serial,
// copies it into `request` and returns true; false on timeout or when VR is not
// running (the caller then paces itself as on the desktop).
bool OpenXRWaitForFrameRequest(OpenXRFrameRequest& request, uint32_t timeout_ms) noexcept;
// How long the game loop should wait for the next request: while the session runs
// the pacing thread publishes one per headset frame and gives a packet 50 ms, so
// the loop waits as long and never draws a frame no packet asked for; otherwise
// (session idle, headset off) about a 60 Hz frame, to keep the game turning.
uint32_t OpenXRFrameRequestTimeoutMs() noexcept;
// The newest request without waiting; false before any was published.
bool OpenXRLatestFrameRequest(OpenXRFrameRequest& request) noexcept;

} // namespace PortVr

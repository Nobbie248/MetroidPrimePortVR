#pragma once

#include <cstdint>

namespace PortVr {

// When an OpenXR backend may rebuild the eye swapchain pair it writes next for a
// new render resolution (ResizeWritablePair in openxr_d3d12.cpp,
// openxr_vulkan.cpp, openxr_vulkan_direct.cpp).
//
// Two pairs take turns: one on display, one written. A new size reaches the
// written pair first; its old pair is retired, kept alive for
// kOpenXRRetiredSwapchainCycles while the compositor may still read it, and the
// other pair follows when its turn comes. Until both have the size the stereo
// packets alternate between the two sizes, and Aurora makes its eye targets
// again at every switch.
//
// A pair is only rebuilt with no packet in flight, so pipelined pacing
// (openxr_integration.cpp) stays paused while EyesResizing: it used to drain
// one cycle per new scale, enough for one pair, and the other kept its size
// until the pipeline next drained (2026-10-12, PSVR2 on SteamVR: the eyes made
// again every frame for minutes). Runtimes also cap how many swapchains a
// session holds (SteamVR answers XR_ERROR_LIMIT_REACHED during a render scale
// slider's drag), and a refused pair sends the backend back to that pair's
// size, which the other pair may have to be rebuilt to again. So a pair is
// rebuilt only once no retired pair is alive (the session then holds the two
// pairs in use plus the one being made), and after kMaxRefusals refusals of
// one request nothing more is rebuilt until another is made.
struct OpenXREyeResizeGate {
    // One refused rebuild, then one to bring the other pair back to the size
    // the first one kept.
    static constexpr uint32_t kMaxRefusals = 2;

    uint32_t refusals = 0;

    // A new render resolution was asked for.
    void Requested() noexcept { refusals = 0; }

    // Whether pairs not at the size aimed for still have a rebuild to come.
    bool Pending() const noexcept { return refusals < kMaxRefusals; }

    // Whether the written pair, not at the size aimed for, may be rebuilt now.
    bool MayRebuild(bool retired_pairs_alive) const noexcept { return !retired_pairs_alive && Pending(); }

    // The runtime refused a rebuild; the backend aims back at that pair's size.
    void Refused() noexcept { ++refusals; }
};

} // namespace PortVr

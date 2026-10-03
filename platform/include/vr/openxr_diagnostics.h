// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Opt-in OpenXR pacing and presentation diagnostics (F10 > Diagnostics).
//
// Off by default. Every hook below is an inline test of one atomic that returns
// at once while logging is off, so carrying them costs the XR pacing thread
// nothing measurable. When on, one-second windows are accumulated and written to
// console.log as a "[xr-diag]" summary line, plus rate-limited event lines for
// late, empty or discarded frames. OPENXR.md documents every field.
//
// All hooks except SetEnabled/Enabled and NotePacketConsumed run on the XR
// pacing thread, the collector's only writer. Nothing here includes OpenXR, so
// the settings overlay and the headless tests can use it in any build.
namespace PortVr::diagnostics {

// Pacing-thread wall time, including scheduling delays. Stages may nest.
enum class Stage : uint8_t {
    PollEvents, BeginCall, LocateViews, InputSync, SyncActions, Publish,
    Withdraw, SubmissionWait, Cancel, SetTargets, Count
};

enum class EmptyFrameReason : uint8_t {
    NoRetainedLayer,
    ShouldRenderOff,
};

enum class DiscardReason : uint8_t {
    SessionRestarted,
    ReferenceSpaceChanged,
};

// Why a layer Aurora finished rendering was not submitted.
enum class RejectReason : uint8_t {
    ReleaseFailed,
    ShouldRenderOff,
    ViewsInvalid,
    PositionInvalid,
};

// The backends' submit condition, checked in the same order: release, then
// should_render, then views; a layer passing all three failed on head position.
inline RejectReason ClassifyRejectedLayer(bool release_ok, bool should_render, bool views_valid) noexcept {
    if (!release_ok) {
        return RejectReason::ReleaseFailed;
    }
    if (!should_render) {
        return RejectReason::ShouldRenderOff;
    }
    return views_valid ? RejectReason::PositionInvalid : RejectReason::ViewsInvalid;
}

struct ViewGeometry {
    // Per eye: left, right, up, down half-angles, in degrees.
    std::array<std::array<float, 4>, 2> fov_degrees{};
    // Angle between the two eyes' forward axes; non-zero on canted displays.
    float cant_degrees = 0.0f;
    // Negative when the runtime reported no valid eye positions.
    float ipd_millimeters = -1.0f;
    std::array<uint32_t, 2> width{};
    std::array<uint32_t, 2> height{};
};

// Accumulates one window of frame measurements and formats it. Times are
// steady-clock nanoseconds passed in by the caller, so tests can drive it with
// a synthetic clock. Not synchronized: one thread owns an instance.
class FrameDiagnostics {
public:
    using Sink = std::function<void(std::string_view line)>;

    static constexpr int64_t kWindowNs = 1'000'000'000;
    static constexpr uint32_t kMaxEventsPerWindow = 8;

    explicit FrameDiagnostics(Sink sink = {});

    void SetSink(Sink sink);
    // Starts a fresh window and forgets per-session state (display-time grid,
    // tracking, logged geometry), and asks for the session description again.
    void Reset(int64_t now_ns);
    // True once after each Reset: the owner should describe the session.
    bool ConsumeSessionInfoRequest();

    // Compositor cycle: xrWaitFrame returned. deadline_ns is the predicted
    // display time on the steady clock, or 0 when it cannot be converted.
    void OnWaitFrame(int64_t now_ns, int64_t wait_ns, int64_t display_time,
                     int64_t display_period, int64_t deadline_ns);
    void OnBeginFrame(int64_t now_ns);
    // xrEndFrame was entered at submit_ns and took call_ns.
    void OnEndFrame(int64_t submit_ns, int64_t call_ns);

    void OnFrameBegun(bool immersive, bool should_render, bool views_valid,
                      bool orientation_valid, bool position_valid);
    void OnViewGeometry(const ViewGeometry& geometry);
    void OnSwapchainAcquire(int64_t ns);
    void OnSwapchainRelease(int64_t ns);

    void OnLayer(bool fresh);
    void OnEmptyFrame(EmptyFrameReason reason);
    void OnRetainedLayerDiscarded(DiscardReason reason);
    void OnLayerRejected(RejectReason reason);

    void OnInterpolationSkip();
    void OnPacketPublished(int64_t now_ns);
    // consumed_ns is when Aurora took the packet, or 0 if it never did.
    void OnPacketCanceled(int64_t now_ns, int64_t consumed_ns);
    void OnStage(Stage stage, int64_t ns, int64_t now_ns);
    void OnKeepaliveRepeat();
    void OnSubmission(int64_t now_ns, int64_t consumed_ns, bool success);

    void OnReferenceSpaceChange(std::string_view space, bool pose_valid, bool effective_known,
                                double effective_in_ms);

    // Unconditional line (session description); not rate-limited.
    void Info(std::string_view text);

    // Emits the summary when the window has lasted kWindowNs. Called from
    // OnEndFrame, so any compositor cycle, repeated or not, flushes it.
    void Tick(int64_t now_ns);

private:
    struct Samples {
        std::vector<float> values;
        void Add(double ms);
        void Clear() { values.clear(); }
    };

    void Event(std::string_view text);
    void ClearWindow(int64_t now_ns);
    void EmitSummary(int64_t now_ns);

    Sink sink_;
    int64_t window_start_ns_ = 0;
    bool session_info_requested_ = true;

    int64_t last_display_time_ = 0;
    int64_t display_period_ = 0;
    int64_t deadline_ns_ = 0;
    int64_t begin_ns_ = 0;
    int64_t last_submit_ns_ = 0;
    int64_t published_ns_ = 0;

    bool tracking_known_ = false;
    bool orientation_valid_ = false;
    bool position_valid_ = false;
    bool geometry_logged_ = false;
    ViewGeometry geometry_{};

    uint32_t cycles_ = 0;
    uint32_t skipped_slots_ = 0;
    uint32_t late_ = 0;
    uint32_t layers_new_ = 0;
    uint32_t layers_repeat_ = 0;
    uint32_t empty_ = 0;
    uint32_t discarded_ = 0;
    uint32_t layer_rejected_ = 0;
    uint32_t keepalive_ = 0;
    uint32_t packet_unused_ = 0;
    uint32_t packet_rejected_ = 0;
    uint32_t submit_failed_ = 0;
    uint32_t interp_skip_ = 0;
    uint32_t immersive_ = 0;
    uint32_t screen_ = 0;
    uint32_t not_rendered_ = 0;
    uint32_t no_orientation_ = 0;
    uint32_t no_position_ = 0;
    uint32_t events_ = 0;
    uint32_t suppressed_ = 0;

    struct StageSamples {
        Samples samples;
        int64_t worst_ns = -1;
        int64_t worst_at_ns = 0;
        uint64_t cycle = 0;
    };
    std::array<StageSamples, static_cast<size_t>(Stage::Count)> stages_{};
    int64_t session_start_ns_ = 0;
    uint64_t cycle_sequence_ = 0;
    Samples cancel_age_ms_;
    Samples cancel_pickup_ms_;
    Samples cancel_after_pickup_ms_;
    Samples wait_frame_ms_;
    Samples open_ms_;
    Samples margin_ms_;
    Samples end_gap_ms_;
    Samples end_call_ms_;
    Samples acquire_ms_;
    Samples release_ms_;
    Samples game_wait_ms_;
    Samples encode_ms_;
};

namespace detail {
inline std::atomic_bool g_enabled{false};
inline std::atomic_int64_t g_packet_consumed_ns{0};

int64_t NowNs() noexcept;
void OnStage(Stage stage, int64_t ns);
void OnWaitFrame(int64_t wait_ns, int64_t display_time, int64_t display_period);
void OnBeginFrame();
void OnEndFrame(int64_t submit_ns, int64_t call_ns);
void OnFrameBegun(bool immersive, bool should_render, bool views_valid, bool orientation_valid,
                  bool position_valid);
void OnViewGeometry(const ViewGeometry& geometry);
void OnSwapchainAcquire(int64_t ns);
void OnSwapchainRelease(int64_t ns);
void OnLayer(bool fresh);
void OnEmptyFrame(EmptyFrameReason reason);
void OnRetainedLayerDiscarded(DiscardReason reason);
void OnLayerRejected(RejectReason reason);
void OnInterpolationSkip();
void OnPacketPublished();
void OnPacketCanceled();
void OnKeepaliveRepeat();
void OnSubmission(bool success);
void OnReferenceSpaceChange(std::string_view space, bool pose_valid, int64_t change_time);
void OnSessionStarted();
bool ConsumeSessionInfoRequest();
void Info(std::string_view text);
} // namespace detail

inline bool Enabled() noexcept { return detail::g_enabled.load(std::memory_order_relaxed); }

// Live switch. Turning logging on starts a new window and re-describes the
// session on the next frame.
void SetEnabled(bool enabled) noexcept;

// Where lines go; installed by the OpenXR integration. Set it only while the
// pacing thread is not running.
void SetLogSink(FrameDiagnostics::Sink sink);

// Converts an XrTime to steady-clock nanoseconds, or returns 0. Set it only
// while the pacing thread is not running, and clear it before its runtime dies.
void SetDisplayTimeConverter(std::function<int64_t(int64_t xr_time)> converter);

// Measures a span only when logging was on at its start.
class Stopwatch {
public:
    Stopwatch() noexcept : start_ns_(Enabled() ? detail::NowNs() : 0) {}
    int64_t StartNs() const noexcept { return start_ns_; }
    // -1 when the stopwatch did not start.
    int64_t ElapsedNs() const noexcept { return start_ns_ == 0 ? -1 : detail::NowNs() - start_ns_; }

private:
    int64_t start_ns_;
};

// Only use on the XR pacing thread. A diagnostic failure must not interrupt
// frame ownership or change the instrumented call's return value.
class ScopedStage {
public:
    explicit ScopedStage(Stage stage) noexcept : stage_(stage) {}
    ~ScopedStage() noexcept {
        const int64_t ns = timer_.ElapsedNs();
        if (ns >= 0 && Enabled()) {
            try { detail::OnStage(stage_, ns); } catch (...) {}
        }
    }
    ScopedStage(const ScopedStage&) = delete;
    ScopedStage& operator=(const ScopedStage&) = delete;
private:
    Stage stage_;
    Stopwatch timer_;
};

template <typename Call>
decltype(auto) Measure(Stage stage, Call&& call) {
    const ScopedStage timer(stage);
    return call();
}

inline void OnWaitFrame(const Stopwatch& wait, int64_t display_time, int64_t display_period) {
    if (const int64_t ns = wait.ElapsedNs(); ns >= 0 && Enabled()) detail::OnWaitFrame(ns, display_time, display_period);
}
inline void OnBeginFrame() {
    if (Enabled()) detail::OnBeginFrame();
}
inline void OnEndFrame(const Stopwatch& call) {
    if (const int64_t ns = call.ElapsedNs(); ns >= 0 && Enabled()) detail::OnEndFrame(call.StartNs(), ns);
}
inline void OnFrameBegun(bool immersive, bool should_render, bool views_valid, bool orientation_valid,
                         bool position_valid) {
    if (Enabled()) detail::OnFrameBegun(immersive, should_render, views_valid, orientation_valid, position_valid);
}
inline void OnSwapchainAcquire(const Stopwatch& acquire) {
    if (const int64_t ns = acquire.ElapsedNs(); ns >= 0 && Enabled()) detail::OnSwapchainAcquire(ns);
}
inline void OnSwapchainRelease(const Stopwatch& release) {
    if (const int64_t ns = release.ElapsedNs(); ns >= 0 && Enabled()) detail::OnSwapchainRelease(ns);
}
inline void OnLayer(bool fresh) {
    if (Enabled()) detail::OnLayer(fresh);
}
inline void OnEmptyFrame(EmptyFrameReason reason) {
    if (Enabled()) detail::OnEmptyFrame(reason);
}
inline void OnRetainedLayerDiscarded(DiscardReason reason) {
    if (Enabled()) detail::OnRetainedLayerDiscarded(reason);
}
inline void OnLayerRejected(RejectReason reason) {
    if (Enabled()) detail::OnLayerRejected(reason);
}
inline void OnInterpolationSkip() {
    if (Enabled()) detail::OnInterpolationSkip();
}
inline void OnPacketPublished() {
    if (Enabled()) detail::OnPacketPublished();
}
// Aurora's frame worker took the published packet. Any thread.
inline void NotePacketConsumed() {
    if (Enabled()) detail::g_packet_consumed_ns.store(detail::NowNs(), std::memory_order_relaxed);
}
inline void OnPacketCanceled() {
    if (Enabled()) detail::OnPacketCanceled();
}
inline void OnKeepaliveRepeat() {
    if (Enabled()) detail::OnKeepaliveRepeat();
}
inline void OnSubmission(bool success) {
    if (Enabled()) detail::OnSubmission(success);
}
inline void OnReferenceSpaceChange(std::string_view space, bool pose_valid, int64_t change_time) {
    if (Enabled()) detail::OnReferenceSpaceChange(space, pose_valid, change_time);
}
inline void OnSessionStarted() {
    if (Enabled()) detail::OnSessionStarted();
}
inline void OnViewGeometry(const ViewGeometry& geometry) {
    if (Enabled()) detail::OnViewGeometry(geometry);
}
// True once after logging starts or a session begins: describe the session.
inline bool ConsumeSessionInfoRequest() {
    return Enabled() && detail::ConsumeSessionInfoRequest();
}
inline void Info(std::string_view text) {
    if (Enabled()) detail::Info(text);
}

} // namespace PortVr::diagnostics

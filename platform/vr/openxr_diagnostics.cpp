// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/openxr_diagnostics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <mutex>
#include <utility>

namespace PortVr::diagnostics {
namespace {

constexpr std::array<const char*, static_cast<size_t>(Stage::Count)> kStageNames{
    "poll-events", "begin-call", "locate-views", "input-sync", "sync-actions",
    "publish", "withdraw", "submission-wait", "cancel", "set-targets"};

constexpr double kNanosecondsPerMillisecond = 1'000'000.0;

double Milliseconds(int64_t nanoseconds) noexcept {
    return static_cast<double>(nanoseconds) / kNanosecondsPerMillisecond;
}

// Appends printf-formatted text. Each call is one short field, so a fixed
// buffer is enough; the summary is assembled from many of them.
void AppendFormat(std::string& output, const char* format, ...) {
    char buffer[256];
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (length > 0) {
        output.append(buffer, std::min(static_cast<size_t>(length), sizeof(buffer) - 1));
    }
}

// "median/worst" in milliseconds, where worst is the maximum, or the minimum
// for a quantity (the display-time margin) where smaller is worse.
void AppendStat(std::string& output, const char* name, std::vector<float>& values, bool worst_is_min) {
    if (values.empty()) {
        AppendFormat(output, " %s=-", name);
        return;
    }
    std::sort(values.begin(), values.end());
    const float median = values[values.size() / 2];
    const float worst = worst_is_min ? values.front() : values.back();
    AppendFormat(output, " %s=%.1f/%.1f", name, median, worst);
}

const char* EmptyFrameText(EmptyFrameReason reason) noexcept {
    switch (reason) {
    case EmptyFrameReason::NoRetainedLayer:
        return "no completed layer is retained, so the headset shows black";
    case EmptyFrameReason::ShouldRenderOff:
        return "the runtime set shouldRender to false";
    }
    return "unknown reason";
}

const char* DiscardText(DiscardReason reason) noexcept {
    switch (reason) {
    case DiscardReason::SessionRestarted:
        return "the OpenXR session restarted";
    case DiscardReason::ReferenceSpaceChanged:
        return "the reference space changed";
    }
    return "unknown reason";
}

const char* RejectText(RejectReason reason) noexcept {
    switch (reason) {
    case RejectReason::ReleaseFailed:
        return "releasing its swapchain images failed";
    case RejectReason::ShouldRenderOff:
        return "the runtime set shouldRender to false";
    case RejectReason::ViewsInvalid:
        return "its views were not valid";
    case RejectReason::PositionInvalid:
        return "the head position was not valid";
    }
    return "unknown reason";
}

bool GeometryChanged(const ViewGeometry& before, const ViewGeometry& after) noexcept {
    constexpr float kAngleTolerance = 0.5f;
    constexpr float kIpdTolerance = 0.5f;
    for (size_t eye = 0; eye < 2; ++eye) {
        for (size_t side = 0; side < 4; ++side) {
            if (std::fabs(before.fov_degrees[eye][side] - after.fov_degrees[eye][side]) > kAngleTolerance) {
                return true;
            }
        }
        if (before.width[eye] != after.width[eye] || before.height[eye] != after.height[eye]) {
            return true;
        }
    }
    if (std::fabs(before.cant_degrees - after.cant_degrees) > kAngleTolerance) {
        return true;
    }
    if ((before.ipd_millimeters < 0.0f) != (after.ipd_millimeters < 0.0f)) {
        return true;
    }
    return after.ipd_millimeters >= 0.0f &&
           std::fabs(before.ipd_millimeters - after.ipd_millimeters) > kIpdTolerance;
}

} // namespace

FrameDiagnostics::FrameDiagnostics(Sink sink) : sink_(std::move(sink)) {}

void FrameDiagnostics::SetSink(Sink sink) {
    sink_ = std::move(sink);
}

void FrameDiagnostics::Samples::Add(double ms) {
    // A window at a 144 Hz display holds a few hundred samples at most; the
    // cap only guards a runaway caller.
    if (values.size() < 4096) {
        values.push_back(static_cast<float>(ms));
    }
}

void FrameDiagnostics::Reset(int64_t now_ns) {
    Sink sink = std::move(sink_);
    *this = FrameDiagnostics(std::move(sink));
    window_start_ns_ = now_ns;
    session_start_ns_ = now_ns;
    session_info_requested_ = true;
}

bool FrameDiagnostics::ConsumeSessionInfoRequest() {
    return std::exchange(session_info_requested_, false);
}

void FrameDiagnostics::OnWaitFrame(int64_t now_ns, int64_t wait_ns, int64_t display_time,
                                   int64_t display_period, int64_t deadline_ns) {
    if (window_start_ns_ == 0) {
        window_start_ns_ = now_ns;
    }
    ++cycles_;
    ++cycle_sequence_;
    wait_frame_ms_.Add(Milliseconds(wait_ns));
    if (display_period > 0) {
        display_period_ = display_period;
        if (last_display_time_ != 0 && display_time > last_display_time_) {
            const int64_t advance = display_time - last_display_time_;
            const int64_t slots = (advance + display_period / 2) / display_period;
            if (slots > 1) {
                skipped_slots_ += static_cast<uint32_t>(slots - 1);
                std::string text;
                AppendFormat(text,
                             "compositor skipped %lld display slot(s): the predicted display time advanced "
                             "%.1f ms at %.1f Hz",
                             static_cast<long long>(slots - 1), Milliseconds(advance),
                             1.0e9 / static_cast<double>(display_period));
                Event(text);
            }
        }
    }
    last_display_time_ = display_time;
    deadline_ns_ = deadline_ns;
}

void FrameDiagnostics::OnBeginFrame(int64_t now_ns) {
    begin_ns_ = now_ns;
}

void FrameDiagnostics::OnEndFrame(int64_t submit_ns, int64_t call_ns) {
    end_call_ms_.Add(Milliseconds(call_ns));
    const double open_ms = begin_ns_ != 0 ? Milliseconds(submit_ns - begin_ns_) : -1.0;
    if (open_ms >= 0.0) {
        open_ms_.Add(open_ms);
    }
    if (deadline_ns_ != 0) {
        const double margin_ms = Milliseconds(deadline_ns_ - submit_ns);
        margin_ms_.Add(margin_ms);
        if (margin_ms < 0.0) {
            ++late_;
            std::string text;
            AppendFormat(text,
                         "late frame: xrEndFrame came %.1f ms after the predicted display time "
                         "(frame open %.1f ms)",
                         -margin_ms, open_ms);
            Event(text);
        }
    }
    if (last_submit_ns_ != 0) {
        const double gap_ms = Milliseconds(submit_ns - last_submit_ns_);
        end_gap_ms_.Add(gap_ms);
        const double stall_ms = std::max(25.0, 2.5 * Milliseconds(display_period_));
        if (gap_ms > stall_ms) {
            std::string text;
            AppendFormat(text, "stall: %.1f ms since the previous xrEndFrame", gap_ms);
            Event(text);
        }
    }
    last_submit_ns_ = submit_ns;
    begin_ns_ = 0;
    deadline_ns_ = 0;
    Tick(submit_ns + call_ns);
}

void FrameDiagnostics::OnFrameBegun(bool immersive, bool should_render, bool views_valid,
                                    bool orientation_valid, bool position_valid) {
    ++(immersive ? immersive_ : screen_);
    if (!should_render || !views_valid) {
        ++not_rendered_;
    }
    if (!should_render) {
        // Views are only located when the runtime wants rendering.
        return;
    }
    no_orientation_ += orientation_valid ? 0 : 1;
    no_position_ += position_valid ? 0 : 1;
    if (tracking_known_ && orientation_valid != orientation_valid_) {
        Event(orientation_valid ? "tracking: head orientation regained" : "tracking: head orientation lost");
    }
    if (tracking_known_ && position_valid != position_valid_) {
        Event(position_valid ? "tracking: head position regained" : "tracking: head position lost");
    }
    tracking_known_ = true;
    orientation_valid_ = orientation_valid;
    position_valid_ = position_valid;
}

void FrameDiagnostics::OnViewGeometry(const ViewGeometry& geometry) {
    if (geometry_logged_ && !GeometryChanged(geometry_, geometry)) {
        return;
    }
    geometry_logged_ = true;
    geometry_ = geometry;
    std::string text = "view geometry: swapchains";
    AppendFormat(text, " %ux%u / %ux%u", geometry.width[0], geometry.height[0], geometry.width[1],
                 geometry.height[1]);
    for (size_t eye = 0; eye < 2; ++eye) {
        const auto& fov = geometry.fov_degrees[eye];
        AppendFormat(text, " | %s eye fov left %+.1f right %+.1f up %+.1f down %+.1f deg",
                     eye == 0 ? "left" : "right", fov[0], fov[1], fov[2], fov[3]);
    }
    AppendFormat(text, " | eye cant %.1f deg (%s)", geometry.cant_degrees,
                 geometry.cant_degrees > 1.0f ? "canted displays" : "parallel");
    if (geometry.ipd_millimeters >= 0.0f) {
        AppendFormat(text, " | ipd %.1f mm", geometry.ipd_millimeters);
    } else {
        text += " | ipd unknown";
    }
    Info(text);
}

void FrameDiagnostics::OnSwapchainAcquire(int64_t ns) {
    acquire_ms_.Add(Milliseconds(ns));
}

void FrameDiagnostics::OnSwapchainRelease(int64_t ns) {
    release_ms_.Add(Milliseconds(ns));
}

void FrameDiagnostics::OnLayer(bool fresh) {
    ++(fresh ? layers_new_ : layers_repeat_);
}

void FrameDiagnostics::OnEmptyFrame(EmptyFrameReason reason) {
    ++empty_;
    Event(std::string("empty frame submitted (no layers): ") +
          EmptyFrameText(reason));
}

void FrameDiagnostics::OnRetainedLayerDiscarded(DiscardReason reason) {
    ++discarded_;
    Event(std::string("retained layer discarded: ") + DiscardText(reason));
}

void FrameDiagnostics::OnLayerRejected(RejectReason reason) {
    ++layer_rejected_;
    Event(std::string("rendered layer not submitted: ") + RejectText(reason));
}

void FrameDiagnostics::OnInterpolationSkip() {
    ++interp_skip_;
}

void FrameDiagnostics::OnPacketPublished(int64_t now_ns) {
    published_ns_ = now_ns;
}

void FrameDiagnostics::OnStage(Stage stage, int64_t ns, int64_t now_ns) {
    if (ns < 0 || static_cast<size_t>(stage) >= stages_.size()) return;
    auto& sample = stages_[static_cast<size_t>(stage)];
    sample.samples.Add(Milliseconds(ns));
    if (ns > sample.worst_ns) {
        sample.worst_ns = ns;
        sample.worst_at_ns = now_ns;
        sample.cycle = cycle_sequence_;
    }
}

void FrameDiagnostics::OnPacketCanceled(int64_t now_ns, int64_t consumed_ns) {
    if (published_ns_ != 0 && now_ns >= published_ns_) {
        cancel_age_ms_.Add(Milliseconds(now_ns - published_ns_));
        if (consumed_ns >= published_ns_ && consumed_ns <= now_ns) {
            cancel_pickup_ms_.Add(Milliseconds(consumed_ns - published_ns_));
            cancel_after_pickup_ms_.Add(Milliseconds(now_ns - consumed_ns));
        }
    }
    if (consumed_ns == 0 || consumed_ns < published_ns_) {
        ++packet_unused_;
        Event("stereo packet withdrawn: no game frame picked it up before cancellation");
    } else {
        ++packet_rejected_;
        Event("stereo packet canceled: Aurora picked it up but the bridge had not encoded it before cancellation");
    }
    published_ns_ = 0;
}

void FrameDiagnostics::OnKeepaliveRepeat() {
    ++keepalive_;
}

void FrameDiagnostics::OnSubmission(int64_t now_ns, int64_t consumed_ns, bool success) {
    if (!success) {
        ++submit_failed_;
        Event("stereo submission failed");
    }
    if (published_ns_ != 0 && consumed_ns >= published_ns_) {
        game_wait_ms_.Add(Milliseconds(consumed_ns - published_ns_));
        encode_ms_.Add(Milliseconds(now_ns - consumed_ns));
    }
    published_ns_ = 0;
}

void FrameDiagnostics::OnReferenceSpaceChange(std::string_view space, bool pose_valid, bool effective_known,
                                              double effective_in_ms) {
    std::string text = "reference space change pending: ";
    text.append(space);
    text += pose_valid ? ", previous pose valid" : ", previous pose not valid";
    if (effective_known) {
        AppendFormat(text, ", takes effect in %.1f ms", effective_in_ms);
    }
    Info(text);
}

void FrameDiagnostics::Info(std::string_view text) {
    if (!sink_) {
        return;
    }
    std::string line = "[xr-diag] ";
    line.append(text);
    sink_(line);
}

void FrameDiagnostics::Event(std::string_view text) {
    if (events_ >= kMaxEventsPerWindow) {
        ++suppressed_;
        return;
    }
    ++events_;
    Info(text);
}

void FrameDiagnostics::Tick(int64_t now_ns) {
    if (window_start_ns_ == 0) {
        window_start_ns_ = now_ns;
        return;
    }
    if (now_ns - window_start_ns_ >= kWindowNs) {
        EmitSummary(now_ns);
        ClearWindow(now_ns);
    }
}

void FrameDiagnostics::ClearWindow(int64_t now_ns) {
    window_start_ns_ = now_ns;
    cycles_ = skipped_slots_ = late_ = 0;
    layers_new_ = layers_repeat_ = empty_ = discarded_ = layer_rejected_ = 0;
    keepalive_ = packet_unused_ = packet_rejected_ = submit_failed_ = interp_skip_ = 0;
    immersive_ = screen_ = not_rendered_ = no_orientation_ = no_position_ = 0;
    events_ = suppressed_ = 0;
    for (auto& stage : stages_) {
        stage.samples.Clear();
        stage.worst_ns = -1;
    }
    for (Samples* samples : {&wait_frame_ms_, &open_ms_, &margin_ms_, &end_gap_ms_, &end_call_ms_,
                             &acquire_ms_, &release_ms_, &game_wait_ms_, &encode_ms_,
                             &cancel_age_ms_, &cancel_pickup_ms_, &cancel_after_pickup_ms_}) {
        samples->Clear();
    }
}

void FrameDiagnostics::EmitSummary(int64_t now_ns) {
    std::string text;
    AppendFormat(text, "%.2fs", Milliseconds(now_ns - window_start_ns_) / 1000.0);
    if (display_period_ > 0) {
        AppendFormat(text, " predicted-rate=%.1fHz", 1.0e9 / static_cast<double>(display_period_));
    }
    AppendFormat(text, " cycles=%u skipped-slots=%u late=%u", cycles_, skipped_slots_, late_);
    AppendFormat(text, " | layers new=%u repeat=%u empty=%u discarded=%u layer-rejected=%u", layers_new_,
                 layers_repeat_, empty_, discarded_, layer_rejected_);
    text += " | ms";
    AppendStat(text, "wait-frame", wait_frame_ms_.values, false);
    AppendStat(text, "open", open_ms_.values, false);
    AppendStat(text, "end-margin", margin_ms_.values, true);
    AppendStat(text, "end-gap", end_gap_ms_.values, false);
    AppendStat(text, "end-call", end_call_ms_.values, false);
    AppendStat(text, "pickup", game_wait_ms_.values, false);
    AppendStat(text, "render", encode_ms_.values, false);
    AppendStat(text, "acquire", acquire_ms_.values, false);
    AppendStat(text, "release", release_ms_.values, false);
    AppendFormat(text, " | keepalive=%u packet-unused=%u packet-rejected=%u submit-failed=%u interp-skip=%u",
                 keepalive_, packet_unused_, packet_rejected_, submit_failed_, interp_skip_);
    AppendFormat(text, " | frames immersive=%u screen=%u not-rendered=%u no-orientation=%u no-position=%u",
                 immersive_, screen_, not_rendered_, no_orientation_, no_position_);
    AppendFormat(text, " | suppressed=%u cycle=%llu t=%.3fs", suppressed_,
                 static_cast<unsigned long long>(cycle_sequence_),
                 static_cast<double>(now_ns - session_start_ns_) / 1.0e9);
    Info(text);
    // Skipped-slot event spam must not suppress evidence of a blocking call.
    std::string stages = "stages ms";
    for (size_t i = 0; i < stages_.size(); ++i) {
        auto& sample = stages_[i];
        AppendStat(stages, kStageNames[i], sample.samples.values, false);
        if (sample.worst_ns >= 0) {
            AppendFormat(stages, "@cycle=%llu,t=%.3fs",
                         static_cast<unsigned long long>(sample.cycle),
                         static_cast<double>(sample.worst_at_ns - session_start_ns_) / 1.0e9);
        }
    }
    AppendStat(stages, "cancel-age", cancel_age_ms_.values, false);
    AppendStat(stages, "cancel-pickup", cancel_pickup_ms_.values, false);
    AppendStat(stages, "cancel-after-pickup", cancel_after_pickup_ms_.values, false);
    if (!cancel_age_ms_.values.empty() || std::any_of(stages_.begin(), stages_.end(),
            [](const auto& stage) { return stage.worst_ns >= 0; })) Info(stages);
}

namespace {

struct GlobalDiagnostics {
    std::mutex sink_mutex;
    FrameDiagnostics::Sink sink;
    std::function<int64_t(int64_t)> converter;
    FrameDiagnostics collector{[](std::string_view line) {
        auto& state = Global();
        std::lock_guard lock(state.sink_mutex);
        if (state.sink) {
            state.sink(line);
        }
    }};

    static GlobalDiagnostics& Global() {
        static GlobalDiagnostics state;
        return state;
    }
};

std::atomic_bool g_reset_pending{true};

void WriteLine(std::string_view line) {
    auto& state = GlobalDiagnostics::Global();
    std::lock_guard lock(state.sink_mutex);
    if (state.sink) {
        state.sink(line);
    }
}

// The pacing thread's view of the collector. A switch-on made elsewhere is
// applied here, on the owning thread, before the next measurement.
FrameDiagnostics& Collector() {
    auto& collector = GlobalDiagnostics::Global().collector;
    if (g_reset_pending.exchange(false, std::memory_order_acq_rel)) {
        collector.Reset(detail::NowNs());
    }
    return collector;
}

int64_t ConvertDisplayTime(int64_t xr_time) {
    const auto& converter = GlobalDiagnostics::Global().converter;
    return converter ? converter(xr_time) : 0;
}

} // namespace

void SetEnabled(bool enabled) noexcept {
    const bool was_enabled = detail::g_enabled.exchange(enabled, std::memory_order_acq_rel);
    if (enabled == was_enabled) {
        return;
    }
    if (enabled) {
        g_reset_pending.store(true, std::memory_order_release);
    }
    try {
        WriteLine(enabled ? "[xr-diag] logging enabled" : "[xr-diag] logging disabled");
    } catch (...) {
        // A diagnostic line must never take the settings path down.
    }
}

void SetLogSink(FrameDiagnostics::Sink sink) {
    auto& state = GlobalDiagnostics::Global();
    std::lock_guard lock(state.sink_mutex);
    state.sink = std::move(sink);
}

void SetDisplayTimeConverter(std::function<int64_t(int64_t xr_time)> converter) {
    GlobalDiagnostics::Global().converter = std::move(converter);
}

namespace detail {

int64_t NowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void OnWaitFrame(int64_t wait_ns, int64_t display_time, int64_t display_period) {
    auto& collector = Collector();
    collector.OnWaitFrame(NowNs(), wait_ns, display_time, display_period, ConvertDisplayTime(display_time));
}

void OnStage(Stage stage, int64_t ns) {
    Collector().OnStage(stage, ns, NowNs());
}

void OnBeginFrame() {
    Collector().OnBeginFrame(NowNs());
}

void OnEndFrame(int64_t submit_ns, int64_t call_ns) {
    Collector().OnEndFrame(submit_ns, call_ns);
}

void OnFrameBegun(bool immersive, bool should_render, bool views_valid, bool orientation_valid,
                  bool position_valid) {
    Collector().OnFrameBegun(immersive, should_render, views_valid, orientation_valid, position_valid);
}

void OnViewGeometry(const ViewGeometry& geometry) {
    Collector().OnViewGeometry(geometry);
}

void OnSwapchainAcquire(int64_t ns) {
    Collector().OnSwapchainAcquire(ns);
}

void OnSwapchainRelease(int64_t ns) {
    Collector().OnSwapchainRelease(ns);
}

void OnLayer(bool fresh) {
    Collector().OnLayer(fresh);
}

void OnEmptyFrame(EmptyFrameReason reason) {
    Collector().OnEmptyFrame(reason);
}

void OnRetainedLayerDiscarded(DiscardReason reason) {
    Collector().OnRetainedLayerDiscarded(reason);
}

void OnLayerRejected(RejectReason reason) {
    Collector().OnLayerRejected(reason);
}

void OnInterpolationSkip() {
    Collector().OnInterpolationSkip();
}

void OnPacketPublished() {
    g_packet_consumed_ns.store(0, std::memory_order_relaxed);
    Collector().OnPacketPublished(NowNs());
}

void OnPacketCanceled() {
    Collector().OnPacketCanceled(NowNs(), g_packet_consumed_ns.load(std::memory_order_relaxed));
}

void OnKeepaliveRepeat() {
    Collector().OnKeepaliveRepeat();
}

void OnSubmission(bool success) {
    Collector().OnSubmission(NowNs(), g_packet_consumed_ns.load(std::memory_order_relaxed), success);
}

void OnReferenceSpaceChange(std::string_view space, bool pose_valid, int64_t change_time) {
    // The runtime reports 0 when the change applies immediately; the converter
    // returns 0 when it has no clock to convert with.
    const int64_t effective_ns = change_time != 0 ? ConvertDisplayTime(change_time) : 0;
    const double effective_in_ms = effective_ns != 0 ? Milliseconds(effective_ns - NowNs()) : 0.0;
    Collector().OnReferenceSpaceChange(space, pose_valid, effective_ns != 0, effective_in_ms);
}

void OnSessionStarted() {
    g_reset_pending.store(true, std::memory_order_release);
}

bool ConsumeSessionInfoRequest() {
    return Collector().ConsumeSessionInfoRequest();
}

void Info(std::string_view text) {
    Collector().Info(text);
}

} // namespace detail
} // namespace PortVr::diagnostics

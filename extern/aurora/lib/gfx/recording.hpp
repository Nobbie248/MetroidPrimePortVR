#pragma once

#include "stereo_frame.hpp"
#include "stereo_replay.hpp"
#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace aurora::gfx::clear {
struct PipelineConfig;
} // namespace aurora::gfx::clear

namespace aurora::gfx::detail {

struct FramePacket;

struct RecordedFrame {
  FramePacket* packet;
  size_t frameSlot;
};

void begin_recording(FramePacket& packet, size_t frameSlot);
RecordedFrame end_recording();
void shutdown_recording();
void increment_merged_draw_count(uint32_t count = 1) noexcept;

namespace testing {
void suppress_render_worker(bool suppress) noexcept;
void seed_offscreen_cache(uint32_t width, uint32_t height, wgpu::TextureFormat colorFormat,
                          wgpu::TextureFormat depthFormat);
}

} // namespace aurora::gfx::detail

namespace aurora::gfx {
struct ColorPassDescriptor {
  const char* label = nullptr;
  wgpu::TextureView colorView;
  wgpu::TextureView resolveView;
  wgpu::TextureFormat colorFormat = wgpu::TextureFormat::Undefined;
  wgpu::TextureView depthStencilView;
  wgpu::TextureFormat depthStencilFormat = wgpu::TextureFormat::Undefined;
  wgpu::Extent3D targetSize;
  uint32_t sampleCount = 1;
  wgpu::LoadOp colorLoadOp = wgpu::LoadOp::Clear;
  wgpu::StoreOp colorStoreOp = wgpu::StoreOp::Store;
  wgpu::Color clearColor{0.f, 0.f, 0.f, 0.f};
  bool hasDepth = false;
  wgpu::LoadOp depthLoadOp = wgpu::LoadOp::Undefined;
  wgpu::StoreOp depthStoreOp = wgpu::StoreOp::Undefined;
  float depthClearValue = 0.f;
  bool hasStencil = false;
  wgpu::LoadOp stencilLoadOp = wgpu::LoadOp::Undefined;
  wgpu::StoreOp stencilStoreOp = wgpu::StoreOp::Undefined;
  uint32_t stencilClearValue = 0;
};

void finish();
// For the next finish(): nothing will read the final pass's mono image, so an
// immersive frame renders that pass into the eyes only (StereoPassReplay::skipMono).
void set_final_pass_mono_unneeded(bool unneeded) noexcept;
// Logs what an immersive frame asks of the eyes every 600 frames (passes, copies,
// draws); the host's VR diagnostics switch (aurora_set_stereo_motion_logging).
void set_stereo_diagnostics(bool enabled) noexcept;
void begin_color_pass(const ColorPassDescriptor& desc);
void end_color_pass();
void queue_texture_copy(wgpu::TexelCopyTextureInfo src, wgpu::TexelCopyTextureInfo dst, wgpu::Extent3D size);
void begin_offscreen(uint32_t width, uint32_t height);
void end_offscreen();
uint32_t get_sample_count() noexcept;
RenderTargetLayout get_render_target_layout() noexcept;
void clear_caches() noexcept;

namespace tex_palette_conv {
struct ConvRequest;
}
void queue_palette_conv(tex_palette_conv::ConvRequest req);

// What the push functions return when the frame's mapped buffer has no room left for the data
// (a mapped buffer cannot grow). The caller drops whatever needed it.
inline constexpr Range OverflowRange{UINT32_MAX, 0};
constexpr bool overflowed(const Range& range) noexcept { return range.offset == OverflowRange.offset; }

Range push_verts(const uint8_t* data, size_t length, size_t alignment);
template <typename T>
Range push_verts(ArrayRef<T> data, size_t alignment) {
  return push_verts(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T), alignment);
}
Range push_indices(const uint8_t* data, size_t length, size_t alignment);
// The frame's vertex and index staging, for gx's strip merging
// (gx/command_processor.cpp) to append to directly; null outside a frame.
ByteBuffer* staging_verts() noexcept;
ByteBuffer* staging_indices() noexcept;
template <typename T>
Range push_indices(ArrayRef<T> data, size_t alignment) {
  return push_indices(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T), alignment);
}
Range push_uniform(const uint8_t* data, size_t length);
template <typename T>
Range push_uniform(const T& data) {
  return push_uniform(reinterpret_cast<const uint8_t*>(&data), sizeof(T));
}
Range push_storage(const uint8_t* data, size_t length);
template <typename T>
Range push_storage(ArrayRef<T> data) {
  return push_storage(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T));
}
template <typename T>
Range push_storage(const T& data) {
  return push_storage(reinterpret_cast<const uint8_t*>(&data), sizeof(T));
}
Range push_texture_data(const uint8_t* data, uint32_t bytesPerRow, uint32_t rowsPerImage);

template <typename DrawData>
void push_draw_command(DrawData data);
// push_encoder_task's recording, for the FIFO processor, which is where the commands before
// it have already been recorded (push_encoder_task drains the FIFO to get there). The task
// type must be registered and the payload checked by the caller.
bool record_encoder_task(uint64_t type, const void* payload, size_t payloadSize); // type: an EncoderTaskId
// record_encoder_task, with the custom draw drawType (registered, no payload) first in the pass
// that follows, which clears its colour instead of loading it: the draw must cover every pixel.
// depth says what that pass does with the sealed pass's depth.
enum class DepthAfter : uint8_t {
  Load,
  Clear, // and drops the sealed pass's depth store
  // Clear if the frame ends in that pass and no draw there may test against the old depth (one
  // with depth compare whose viewport depth range reaches the nearest depth the EFB was given),
  // else Load. The sealed pass and the task wait for the render worker until that is known.
  IfUnread,
};
bool record_encoder_task_overwriting(uint64_t type, const void* payload, size_t payloadSize, uint64_t drawType,
                                     DepthAfter depth = DepthAfter::Load);
template <typename DrawData>
DrawData* get_last_draw_command();
template <typename PipelineConfig>
PipelineRef pipeline_ref(const PipelineConfig& config);

void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat = GX_TF_RGBA8,
                       int probeFace = -1, bool resolveMips = false);
uint32_t align_uniform(uint32_t value);
Vec2<uint32_t> get_render_target_size() noexcept;
void set_viewport(const Viewport& viewport) noexcept;
void set_scissor(const ClipRect& scissor) noexcept;
void push_debug_group(std::string label);
void insert_debug_marker(std::string label);

// --- stereo replay (stereo_frame.hpp) ---
struct StereoUniformLayout;
// The frame's stereo state: set right after begin_frame on the game thread,
// read back before end_frame for the frame worker's hand-off.
void set_frame_stereo(const StereoFrameState& state) noexcept;
StereoFrameState recorded_stereo_state() noexcept;
// The draw route the GX FIFO last set (AuroraStereoDrawRoute), applied to the
// uniforms staged from now on. Resets to WORLD at frame begin.
void set_stereo_draw_route(uint8_t route) noexcept;
// Whether the frame being recorded replays its eyes through multiview
// (stereo_multiview.hpp): its draws then need their multiview pipelines.
bool recording_multiview() noexcept;
// A clear draw's pipeline for a multiview eye pass, or zero outside a multiview frame.
PipelineRef clear_multiview_pipeline_ref(const clear::PipelineConfig& config) noexcept;
uint8_t stereo_draw_route() noexcept;
// The plane AURORA_STEREO_ROUTE_HEAD_LOCKED_2D lays orthographic draws on and
// takes EFB copies through (stereo_replay.hpp HeadLockedPlane), as the GX FIFO
// last set it; kept until set again.
void set_stereo_head_locked_plane(float tanHalfWidth, float tanHalfHeight, float distance) noexcept;
stereo_replay::HeadLockedPlane stereo_head_locked_plane() noexcept;
// A texture matrix the GX FIFO marked as a screen projection
// (AuroraSetStereoScreenTexMtx): every eye copy of a perspective draw's
// uniform gets texture matrix `texSlot` derived again from that eye's
// projection and position matrix `pnSlot` (stereo_uniform.hpp
// compose_stereo_screen_tex_mtx). `texSlot` is 0xFF when there is none.
// Resets at frame begin.
struct StereoScreenTexMtx {
  uint8_t texSlot = 0xFF;
  uint8_t pnSlot = 0;
};
void set_stereo_screen_tex_mtx(uint8_t texSlot, uint8_t pnSlot) noexcept;
StereoScreenTexMtx stereo_screen_tex_mtx() noexcept;
// Stages the eye copies of the GX uniform just pushed at `monoRange` (whose
// CPU-side bytes are `mono`) and returns the offsets the draw binds per eye:
// the mono offset for a draw that is identical in both eyes, UINT32_MAX for
// one that is left out of them. In a multiview frame (stereo_multiview.hpp) a
// draw keeps its mono uniform and gets the eyes' clip matrices pushed right
// after it, or for a per-eye screen texture matrix its two eye copies, back to
// back; stereo_multiview_mode then names the shader variant (gx::MultiviewMode)
// the draw needs, MultiviewNone when it stays out of the eye passes.
std::array<uint32_t, 2> stage_stereo_uniforms(const uint8_t* mono, Range monoRange,
                                              const StereoUniformLayout& layout) noexcept;
uint8_t stereo_multiview_mode() noexcept;
} // namespace aurora::gfx

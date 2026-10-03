#pragma once

#include "stereo_frame.hpp"
#include "stereo_replay.hpp"
#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace aurora::gfx::detail {

struct FramePacket;

struct RecordedFrame {
  FramePacket* packet;
  size_t frameSlot;
};

void begin_recording(FramePacket& packet, size_t frameSlot);
RecordedFrame end_recording();
void shutdown_recording();
void increment_merged_draw_count() noexcept;

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

Range push_verts(const uint8_t* data, size_t length, size_t alignment);
template <typename T>
Range push_verts(ArrayRef<T> data, size_t alignment) {
  return push_verts(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T), alignment);
}
Range push_indices(const uint8_t* data, size_t length, size_t alignment);
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
template <typename DrawData>
DrawData* get_last_draw_command();
template <typename PipelineConfig>
PipelineRef pipeline_ref(const PipelineConfig& config);

void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat = GX_TF_RGBA8,
                       int probeFace = -1);
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
uint8_t stereo_draw_route() noexcept;
// The plane AURORA_STEREO_ROUTE_HEAD_LOCKED_2D lays orthographic draws on and
// takes EFB copies through (stereo_replay.hpp HeadLockedPlane), as the GX FIFO
// last set it; kept until set again.
void set_stereo_head_locked_plane(float tanHalfWidth, float tanHalfHeight, float distance) noexcept;
stereo_replay::HeadLockedPlane stereo_head_locked_plane() noexcept;
// Stages the eye copies of the GX uniform just pushed at `monoRange` (whose
// CPU-side bytes are `mono`) and returns the offsets the draw binds per eye:
// the mono offset for a draw that is identical in both eyes, UINT32_MAX for
// one that is left out of them.
std::array<uint32_t, 2> stage_stereo_uniforms(const uint8_t* mono, Range monoRange,
                                              const StereoUniformLayout& layout) noexcept;
} // namespace aurora::gfx

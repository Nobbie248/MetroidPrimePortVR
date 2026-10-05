// Stub implementations for renderer symbols that the GX/FIFO/command_processor code
// references but that live in the full renderer (gfx, gx.cpp, model/shader.cpp, etc.).
// These allow the test binary to link without pulling in WebGPU runtime.

#include "gx/gx.hpp"
#include "gfx/clear.hpp"
#include "gfx/resources.hpp"
#include "gfx/depth_peek.hpp"
#include "gfx/recording.hpp"
#include "gfx/perf_counters.hpp"
#include "gfx/stereo_shadow.hpp"
#include "gfx/tex_copy_conv.hpp"
#include "gfx/tex_palette_conv.hpp"
#include "gfx/texture.hpp"
#include "gfx/texture_replacement.hpp"
#include "gx/pipeline.hpp"
#include "gx/shader_info.hpp"
#include "internal.hpp"
#include "webgpu/gpu.hpp"

#include <atomic>
#include <cstdio>
#include <fmt/format.h>

// --- aurora::g_config ---
namespace aurora {
AuroraConfig g_config{};
} // namespace aurora

// --- aurora::log_internal ---
namespace aurora {
void log_internal(AuroraLogLevel level, const char* module, const char* message, unsigned int len) noexcept {
  fprintf(stderr, "[%d] %s: %.*s\n", static_cast<int>(level), module, len, message);
}
} // namespace aurora

// --- fmt::formatter<AuroraLogLevel> ---
auto fmt::formatter<AuroraLogLevel>::format(AuroraLogLevel level, format_context& ctx) const
    -> format_context::iterator {
  return fmt::format_to(ctx.out(), "{}", static_cast<int>(level));
}

// --- GPU resources (default-constructed, not used in tests) ---
namespace aurora::gfx::detail {
Resources& resources() noexcept {
  static Resources resources;
  return resources;
}

void increment_merged_draw_count(uint32_t) noexcept {}
} // namespace aurora::gfx::detail

namespace aurora::webgpu {
GraphicsConfig g_graphicsConfig{};
} // namespace aurora::webgpu

// --- GXState ---
namespace aurora::gx {
GXState g_gxState{};
static std::atomic<u16> sTestDrawSyncToken{0};
void set_draw_sync_token(u16 token) noexcept { sTestDrawSyncToken.store(token, std::memory_order_release); }
u16 draw_sync_token() noexcept { return sTestDrawSyncToken.load(std::memory_order_acquire); }
void set_viewport_policy(AuroraViewportPolicy policy) noexcept {}
} // namespace aurora::gx

namespace aurora::vi {
Vec2<uint32_t> configured_fb_size() noexcept { return {640, 480}; }
void configure(const GXRenderModeObj*) noexcept {}
} // namespace aurora::vi

// --- get_texture ---
namespace aurora::gx {
namespace testing {
bool sampledTexture = false;
uint64_t bindGeneration = 1;
uint32_t bindBuilds = 0;
uint32_t multiviewBuilds = 0;
} // namespace testing
const gfx::TextureBind& get_texture(GXTexMapID id) noexcept { return g_gxState.textures[id]; }
namespace texture {
void invalidate_bindings() noexcept {}
uint64_t current_bind_generation() noexcept { return testing::bindGeneration; }
} // namespace texture
void evict_texture_object(u32 texObjId) noexcept {
  for (auto& obj : g_gxState.loadedTextures) {
    if (obj.texObjId == texObjId) {
      obj.set_no_cache(true);
    }
  }
}
void evict_tlut_object(u32 tlutObjId) noexcept {
  for (auto& obj : g_gxState.loadedTluts) {
    if (obj.tlutObjId == tlutObjId) {
      obj.set_no_cache(true);
    }
  }
}
void evict_copy_texture(const void* dest) noexcept {
  g_gxState.copyTextures.erase(dest);
  for (auto it = g_gxState.copyTextureCache.begin(); it != g_gxState.copyTextureCache.end();) {
    if (it->first.dest == dest) {
      g_gxState.copyTextureCache.erase(it++);
    } else {
      ++it;
    }
  }
}
void trim_copy_sizes(const void* dest, size_t keep) noexcept {}
void shutdown() noexcept {}
Vec2<uint32_t> logical_fb_size() noexcept { return {640, 480}; }
gfx::Viewport map_logical_viewport(const gfx::Viewport& logicalViewport) noexcept { return logicalViewport; }
gfx::ClipRect map_logical_scissor(const gfx::ClipRect& logicalScissor) noexcept { return logicalScissor; }
void set_logical_viewport(const gfx::Viewport& viewport) noexcept {
  g_gxState.logicalViewport = viewport;
  set_render_viewport(map_logical_viewport(viewport));
}
void set_render_viewport(const gfx::Viewport& viewport) noexcept { g_gxState.renderViewport = viewport; }
void set_logical_scissor(const gfx::ClipRect& scissor) noexcept {
  g_gxState.logicalScissor = scissor;
  set_render_scissor(map_logical_scissor(scissor));
}
void set_render_scissor(const gfx::ClipRect& scissor) noexcept { g_gxState.renderScissor = scissor; }
} // namespace aurora::gx

// --- Shader/pipeline stubs ---
namespace aurora::gx {
void populate_pipeline_config(PipelineConfig& config, GXPrimitive primitive, GXVtxFmt fmt) noexcept {
  config = {};
  config.cullMode = g_gxState.cullMode;
  config.shaderConfig.currentPnMtx = g_gxState.currentPnMtx;
  config.shaderConfig.lineMode = primitive == GX_LINES ? 1 : primitive == GX_LINESTRIP ? 2 : primitive == GX_POINTS ? 3 : 0;
}
GXBindGroups build_bind_groups(const ShaderInfo& info) noexcept {
  return {.textureBindGroup = ++testing::bindBuilds};
}
gfx::BindGroupRef build_multiview_bind_group(const ShaderInfo&) noexcept { return ++testing::multiviewBuilds; }
ShaderInfo build_shader_info(const ShaderConfig& config) noexcept {
  ShaderInfo info{};
  info.sampledTextures[0] = testing::sampledTexture;
  return info;
}
gfx::Range build_uniform(const ShaderInfo& info) noexcept { return {.size = 1}; }
gfx::Range build_uniform(const ShaderInfo& info, std::array<uint32_t, 2>& stereoUniformOffsets,
                         ByteBuffer* snapshot) noexcept {
  stereoUniformOffsets = {UINT32_MAX, UINT32_MAX};
  if (snapshot != nullptr) {
    snapshot->clear();
    snapshot->append(g_gxState.lineWidth);
  }
  return {.size = 1};
}
bool uniform_matches(const ShaderInfo&, const ByteBuffer& snapshot) noexcept {
  return snapshot.size() == sizeof(g_gxState.lineWidth) &&
         std::memcmp(snapshot.data(), &g_gxState.lineWidth, snapshot.size()) == 0;
}
void resolve_sampled_textures(const ShaderInfo& info) noexcept {}
std::array<gfx::BindGroupRef, 2> build_stereo_bind_groups(const ShaderInfo& info) noexcept { return {}; }
} // namespace aurora::gx

// --- Stereo replay stubs (the FIFO never runs immersive here) ---
namespace aurora::gfx {
namespace testing {
uint32_t frame = 0;
bool bindGroupsAlive = true;
bool multiview = false;
uint64_t stereoEpoch = 1;
} // namespace testing
uint32_t current_frame() noexcept { return testing::frame; }
bool touch_bind_group(BindGroupRef) { return testing::bindGroupsAlive; }
bool recording_multiview() noexcept { return testing::multiview; }
uint8_t stereo_multiview_mode() noexcept { return 0; }
uint32_t align_uniform(uint32_t value) { return (value + 255u) & ~255u; }
PipelineRef clear_multiview_pipeline_ref(const clear::PipelineConfig&) noexcept { return 0; }
void set_stereo_draw_route(uint8_t route) noexcept {}
uint8_t stereo_draw_route() noexcept { return 0; }
void set_stereo_head_locked_plane(float tanHalfWidth, float tanHalfHeight, float distance) noexcept {}
stereo_replay::HeadLockedPlane stereo_head_locked_plane() noexcept { return {}; }
void set_stereo_screen_tex_mtx(uint8_t texSlot, uint8_t pnSlot) noexcept {}
StereoScreenTexMtx stereo_screen_tex_mtx() noexcept { return {}; }
namespace stereo_shadow {
bool active() noexcept { return testing::multiview; }
uint64_t epoch() noexcept { return testing::stereoEpoch; }
} // namespace stereo_shadow
} // namespace aurora::gfx

// --- Buffer push stubs ---
namespace aurora::gfx {
namespace testing {
std::vector<uint8_t> pushedVerts; // the last push_verts' bytes
bool mergeDraws = false;
ByteBuffer stagedVerts;
ByteBuffer stagedIndices;
} // namespace testing
ByteBuffer* staging_verts() noexcept { return testing::mergeDraws ? &testing::stagedVerts : nullptr; }
ByteBuffer* staging_indices() noexcept { return testing::mergeDraws ? &testing::stagedIndices : nullptr; }
static Range stage_test_bytes(ByteBuffer& buffer, const uint8_t* data, size_t length, size_t alignment) {
  const auto offset = (buffer.size() + alignment - 1) / alignment * alignment;
  (void)buffer.append_uninitialized(offset - buffer.size());
  auto* dst = buffer.append_uninitialized(length);
  if (length != 0) {
    std::memcpy(dst, data, length);
  }
  return {static_cast<uint32_t>(offset), static_cast<uint32_t>(length)};
}
Range push_verts(const uint8_t* data, size_t length, size_t alignment) {
  testing::pushedVerts.assign(data, data + length);
  if (testing::mergeDraws) {
    return stage_test_bytes(testing::stagedVerts, data, length, alignment);
  }
  return {};
}
Range push_indices(const uint8_t* data, size_t length, size_t alignment) {
  return testing::mergeDraws ? stage_test_bytes(testing::stagedIndices, data, length, alignment) : Range{};
}
Range push_uniform(const uint8_t* data, size_t length) { return {}; }
Range push_storage(const uint8_t* data, size_t length) { return {}; }

Vec2<uint32_t> get_render_target_size() noexcept { return {640, 480}; }
void set_viewport(const Viewport& viewport) noexcept {}
void set_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) noexcept {}
uint32_t get_sample_count() noexcept { return 1; }
RenderTargetLayout get_render_target_layout() noexcept {
  return {
      .colorAttachmentCount = 1,
      .colorAttachments = {{{ColorAttachmentSemantic::SceneColor, wgpu::TextureFormat::RGBA8Unorm}}},
      .depthStencilFormat = wgpu::TextureFormat::Depth24Plus,
      .sampleCount = 1,
  };
}
} // namespace aurora::gfx

// --- Pipeline/draw command stubs ---
namespace aurora::gfx {
namespace clear {
PipelineConfig make_pipeline_config(const RenderTargetLayout& layout, bool clearColor, bool clearAlpha,
                                    bool clearDepth) noexcept {
  return {
      .targetLayoutKey = layout.key,
      .depthStencilFormat = layout.depthStencilFormat,
      .colorAttachmentCount = layout.colorAttachmentCount,
      .msaaSamples = layout.sampleCount,
      .clearColor = clearColor,
      .clearAlpha = clearAlpha,
      .clearDepth = clearDepth,
  };
}
} // namespace clear

template <>
PipelineRef pipeline_ref<clear::PipelineConfig>(const clear::PipelineConfig& config) {
  return 0;
}
template <>
void push_draw_command<clear::DrawData>(clear::DrawData data) {
  // No-op
}
template <>
PipelineRef pipeline_ref<gx::PipelineConfig>(const gx::PipelineConfig& config) {
  return 0;
}
gx::DrawData g_testLastDraw{};
uint32_t g_testDrawCount = 0;
std::atomic<uint32_t> g_testProcessedDrawCount{0};

template <>
void push_draw_command<gx::DrawData>(gx::DrawData data) {
  g_testLastDraw = data;
  ++g_testDrawCount;
  g_testProcessedDrawCount.fetch_add(1, std::memory_order_release);
}
template <>
gx::DrawData* get_last_draw_command() {
  return testing::mergeDraws && g_testDrawCount != 0 ? &g_testLastDraw : nullptr;
}
} // namespace aurora::gfx

// --- TextureBind::get_descriptor ---
namespace aurora::gfx {
void TextureRef::count_live(int) noexcept {}
wgpu::SamplerDescriptor TextureBind::get_descriptor() const noexcept { return wgpu::SamplerDescriptor{}; }
} // namespace aurora::gfx

// --- Texture creation/write/replacement stubs ---
namespace aurora::gfx {
TextureHandle new_static_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                    ArrayRef<uint8_t> data, bool tlut, const char* label) noexcept {
  return {};
}
TextureHandle new_dynamic_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                     const char* label) noexcept {
  return {};
}
TextureHandle new_render_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept {
  return {};
}
TextureHandle new_conv_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept { return {}; }
void write_texture(TextureRef& ref, ArrayRef<uint8_t> data) noexcept {}
void queue_texture_upload(TextureUpload upload) {}
void queue_texture_upload_data(const uint8_t* data, size_t length, uint32_t bytesPerRow, uint32_t rowsPerImage,
                               wgpu::TexelCopyTextureInfo tex, wgpu::Extent3D size) {}
void queue_palette_conv(tex_palette_conv::ConvRequest req) {}
namespace testing {
std::atomic<uint32_t> beginOffscreenCount{0};
std::atomic<uint32_t> endOffscreenCount{0};
std::atomic<uint32_t> resolvePassCount{0};
std::atomic<uint32_t> offscreenWidth{0};
std::atomic<uint32_t> offscreenHeight{0};
} // namespace testing

void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat, int probeFace) {
  testing::resolvePassCount.fetch_add(1, std::memory_order_release);
}
namespace probe {
TextureHandle face(uint32_t face) { return {}; }
void create_cube(uint32_t id, uint32_t size, uint32_t mipCount, const uint8_t* texels, size_t length) {}
void destroy_cube(uint32_t id) {}
bool has_cube(uint32_t id) { return false; }
void create_volume(uint32_t id, uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ, const uint8_t* texels, size_t length) {}
void destroy_volume(uint32_t id) {}
bool has_volume(uint32_t id) { return false; }
} // namespace probe
void begin_offscreen(uint32_t width, uint32_t height) {
  testing::offscreenWidth.store(width, std::memory_order_relaxed);
  testing::offscreenHeight.store(height, std::memory_order_relaxed);
  testing::beginOffscreenCount.fetch_add(1, std::memory_order_release);
}
void end_offscreen() { testing::endOffscreenCount.fetch_add(1, std::memory_order_release); }
bool is_offscreen() noexcept { return false; }
} // namespace aurora::gfx

namespace aurora::gfx::depth_peek {
namespace {
bool s_snapshotRequested = false;
uint32_t s_width = 0;
uint32_t s_height = 0;
std::vector<uint32_t> s_data;
} // namespace

void initialize() {}
void shutdown() {}
void request_snapshot() noexcept { s_snapshotRequested = true; }
void poll() noexcept {}
void encode_frame_snapshot(const wgpu::CommandEncoder& cmd, const wgpu::TextureView& depthView,
                           wgpu::Extent3D sourceSize, uint32_t msaaSamples) noexcept {}
void after_submit() noexcept {}

bool read_latest(uint16_t x, uint16_t y, uint32_t& z) noexcept {
  if (x >= s_width || y >= s_height || s_data.empty()) {
    return false;
  }
  z = s_data[static_cast<size_t>(y) * s_width + x] & 0x00ffffffu;
  return true;
}

namespace testing {
void reset() noexcept {
  s_snapshotRequested = false;
  s_width = 0;
  s_height = 0;
  s_data.clear();
}

bool snapshot_requested() noexcept { return s_snapshotRequested; }

void set_latest(uint32_t width, uint32_t height, const std::vector<uint32_t>& data) {
  s_width = width;
  s_height = height;
  s_data = data;
}
} // namespace testing
} // namespace aurora::gfx::depth_peek

namespace aurora::gfx::tex_copy_conv {
bool needs_conversion(GXTexFmt fmt) { return false; }
} // namespace aurora::gfx::tex_copy_conv

namespace aurora::gfx::tex_palette_conv {
void queue(ConvRequest req) {}
} // namespace aurora::gfx::tex_palette_conv

namespace aurora::gfx::texture_replacement {
u32 compute_texture_upload_size(const GXTexObj_& obj) noexcept { return 0; }
bool has_replacement(const GXTexObj_&) noexcept { return false; }
bool has_replacement(const GXTexObj_&, const GXTlutObj_&) noexcept { return false; }
} // namespace aurora::gfx::texture_replacement

// --- Window stub ---
#include "../lib/window.hpp"
namespace aurora::window {
AuroraWindowSize get_window_size() { return {640, 480, 640, 480, 640, 480, 1.0f}; }
void set_frame_buffer_aspect_fit(bool) {}
} // namespace aurora::window

// --- WebGPU C API stubs (prevent linker errors from wgpu:: destructors) ---
extern "C" {
void wgpuDeviceRelease(WGPUDevice) {}
void wgpuQueueRelease(WGPUQueue) {}
void wgpuSurfaceRelease(WGPUSurface) {}
void wgpuBufferRelease(WGPUBuffer) {}
void wgpuTextureRelease(WGPUTexture) {}
void wgpuTextureViewRelease(WGPUTextureView) {}
void wgpuSamplerRelease(WGPUSampler) {}
void wgpuShaderModuleRelease(WGPUShaderModule) {}
void wgpuRenderPipelineRelease(WGPURenderPipeline) {}
void wgpuBindGroupRelease(WGPUBindGroup) {}
void wgpuBindGroupLayoutRelease(WGPUBindGroupLayout) {}
void wgpuPipelineLayoutRelease(WGPUPipelineLayout) {}
void wgpuInstanceRelease(WGPUInstance) {}
void wgpuDeviceAddRef(WGPUDevice) {}
void wgpuQueueAddRef(WGPUQueue) {}
void wgpuSurfaceAddRef(WGPUSurface) {}
void wgpuBufferAddRef(WGPUBuffer) {}
void wgpuTextureAddRef(WGPUTexture) {}
void wgpuTextureViewAddRef(WGPUTextureView) {}
void wgpuInstanceAddRef(WGPUInstance) {}
}

void aurora::gfx::push_debug_group(std::string) {}
void push_debug_group(const char*) {}
void pop_debug_group() {}
void aurora::gfx::insert_debug_marker(std::string) {}

// Disabled diagnostics for the asset-free FIFO tests.
namespace aurora::gfx::perf {
bool enabled() noexcept { return false; }
std::atomic<uint64_t> g_fifoNs{0};
std::atomic<uint64_t> g_fifoBytes{0};
std::atomic<uint64_t> g_encodeNs{0};
std::atomic<uint64_t> g_submitNs{0};
std::atomic<uint64_t> g_fifoXfTicks{0};
std::atomic<uint64_t> g_fifoBpTicks{0};
std::atomic<uint64_t> g_fifoCpTicks{0};
std::atomic<uint64_t> g_fifoAuroraTicks{0};
std::atomic<uint64_t> g_fifoVertsTicks{0};
std::atomic<uint64_t> g_fifoPipelineTicks{0};
std::atomic<uint64_t> g_fifoBindsTicks{0};
std::atomic<uint64_t> g_fifoUniformTicks{0};
std::atomic<uint64_t> g_fifoPushTicks{0};
std::atomic<uint32_t> g_fifoXfLoads{0};
std::atomic<uint32_t> g_fifoBpLoads{0};
std::atomic<uint32_t> g_fifoDrawsMerged{0};
std::atomic<uint32_t> g_fifoDrawsPushed{0};
std::atomic<uint32_t> g_fifoPipelineBuilds{0};
std::atomic<uint32_t> g_fifoBindGroupBuilds{0};
std::atomic<uint32_t> g_fifoBindGroupMisses{0};
std::atomic<uint64_t> g_fifoResolveTicks{0};
std::atomic<uint32_t> g_fifoUniformBuilds{0};
std::atomic<uint32_t> g_mergeBreaks[BreakCount]{};
std::atomic<uint64_t> g_fifoDeindexTicks{0};
std::atomic<uint32_t> g_fifoDeindexedVerts{0};
std::atomic<uint64_t> g_drainWaitNs{0};
std::atomic<uint32_t> g_drainCalls{0};
}

#include "recording.hpp"

#include <cstdlib>
#include <atomic>

#include "encoding.hpp"
#include "frame.hpp"
#include "geometry_buffer.hpp"
#include "resource_cache.hpp"

#include "clear.hpp"
#include "draw_payload.hpp"
#include "perf_counters.hpp"
#include "pipeline_cache.hpp"
#include "stereo_eyes.hpp"
#include "stereo_shadow.hpp"
#include "stereo_uniform.hpp"
#include "render_worker.hpp"
#include "tex_copy_conv.hpp"
#include "tex_palette_conv.hpp"
#include "texture.hpp"
#include "../gx/fifo.hpp"
#include "../gx/gx.hpp"
#include "../gx/pipeline.hpp"
#ifdef AURORA_ENABLE_RMLUI
#include "../rmlui/pipeline.hpp"
#endif
#include "../window.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <magic_enum.hpp>
#include <tracy/Tracy.hpp>

namespace aurora::gfx {
using namespace detail;

// stereo_uniform.hpp mirrors the GX uniform's fixed counts so the tests need no GX headers.
static_assert(kStereoPositionMatrices == gx::MaxPnMtx);
static_assert(kStereoTextureMatrices == gx::MaxTexMtx);
static_assert(kStereoLights == GX::MaxLights);
static_assert(kStereoLightBytes == sizeof(gx::Light));

namespace {
constexpr Module Log{"aurora::gfx"};

struct FrameRecorder {
  FramePacket* packet = nullptr;
  size_t frameSlot = 0;
  uint32_t currentRenderPass = UINT32_MAX;
  uint32_t drawCallCount = 0;
  // Draws recorded under each stereo route this frame (AURORA_STEREO_ROUTE_*), for the
  // diagnostics line (log_stereo_frame_stats).
  std::array<uint32_t, 8> routeDrawCounts{};
  uint32_t mergedDrawCallCount = 0;
  bool inOffscreen = false;
  std::optional<RenderPass> suspendedEfbPass;
  Viewport suspendedEfbViewport;
  ClipRect suspendedEfbScissor;
  webgpu::TextureWithSampler offscreenColor;
  webgpu::TextureWithSampler offscreenDepth;
  Viewport cachedViewport;
  ClipRect cachedScissor;
  bool suppressRenderWorker = false;
  uint8_t stereoRoute = AURORA_STEREO_ROUTE_WORLD;
  bool finalPassMonoUnneeded = false;
  stereo_replay::HeadLockedPlane headLockedPlane;
  StereoScreenTexMtx stereoScreenTexMtx;
  uint8_t multiviewMode = 0; // gx::MultiviewMode of the last stage_stereo_uniforms
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> debugGroupStack;
#endif

  [[nodiscard]] bool active() const noexcept { return packet != nullptr; }

  FramePacket& frame() const {
    CHECK(packet != nullptr, "No active recording session");
    return *packet;
  }

  RenderPassList& passes() const { return frame().renderPasses; }
};

FrameRecorder g_recorder;
// MP_FRAME_STATS keeps the frame statistics on whatever the VR settings say,
// for scripted runs without a headset.
bool frame_stats_forced() noexcept { return std::getenv("MP_FRAME_STATS") != nullptr; }
std::atomic_bool g_stereoDiagnostics{frame_stats_forced()};
// What composing the eye uniform copies costs on the FIFO thread, between two
// stereo statistics lines (log_stereo_frame_stats), while diagnostics are on.
std::atomic<uint64_t> g_eyeUniformNs{0};
std::atomic<uint32_t> g_eyeUniformDraws{0};
} // namespace

namespace perf {
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
std::atomic<uint32_t> g_fifoCachedDlCalls{0};
std::atomic<uint64_t> g_fifoCachedDlBytes{0};
std::atomic<uint64_t> g_fifoCachedDlVertBytes{0};
std::atomic<uint64_t> g_fifoCachedDlIndexBytes{0};
std::atomic<uint32_t> g_fifoCachedDlVerts{0};
std::atomic<uint32_t> g_fifoGeometryHits{0};
std::atomic<uint32_t> g_fifoGeometryMisses{0};
std::atomic<uint32_t> g_geometryEntries{0};
std::atomic<uint64_t> g_geometryResidentBytes{0};
std::atomic<uint64_t> g_drainWaitNs{0};
std::atomic<uint32_t> g_drainCalls{0};
bool enabled() noexcept { return g_stereoDiagnostics.load(std::memory_order_relaxed); }
} // namespace perf

namespace {

void log_stereo_frame_stats(const FramePacket& frame) {
  // Every 600th frame, immersive or not (a desktop run measures the same FIFO)
  static uint32_t sFrames = 0;
  if ((sFrames++ % 600) != 0) {
    return;
  }
  uint32_t efbPasses = 0;
  uint32_t eyePasses = 0;
  uint32_t monoSkipped = 0;
  uint32_t foveated = 0;
  uint32_t copies = 0;
  uint32_t eyeCopies = 0;
  uint32_t discarded = 0;
  for (const auto& pass : frame.renderPasses) {
    efbPasses += pass.efb ? 1 : 0;
    eyePasses += pass.stereo.enabled ? 1 : 0;
    monoSkipped += pass.stereo.enabled && pass.stereo.skipMono ? 1 : 0;
    foveated += pass.stereo.enabled && pass.stereo.foveated ? 1 : 0;
    copies += pass.resolveTarget ? 1 : 0;
    eyeCopies += pass.stereo.copyTargets[0] ? 1 : 0;
    discarded += pass.discardable ? 1 : 0;
  }
  const auto& routes = g_recorder.routeDrawCounts;
  const double eyeUniformMs =
      static_cast<double>(g_eyeUniformNs.exchange(0, std::memory_order_relaxed)) / 600.0 / 1.0e6;
  const uint32_t eyeUniformDraws = g_eyeUniformDraws.exchange(0, std::memory_order_relaxed) / 600;
  const auto perFrameMs = [](std::atomic<uint64_t>& ns) {
    return static_cast<double>(ns.exchange(0, std::memory_order_relaxed)) / 600.0 / 1.0e6;
  };
  const double fifoMs = perFrameMs(perf::g_fifoNs);
  const uint64_t fifoKb = perf::g_fifoBytes.exchange(0, std::memory_order_relaxed) / 600 / 1024;
  const double drainWaitMs = perFrameMs(perf::g_drainWaitNs);
  const uint32_t drainCalls = perf::g_drainCalls.exchange(0, std::memory_order_relaxed) / 600;
  const double encodeMs = perFrameMs(perf::g_encodeNs);
  const double submitMs = perFrameMs(perf::g_submitNs);
  const auto& uploads = resources().stats;
  Log.info("stereo frame: {} passes ({} EFB, {} replayed per eye, {} foveated, {} eyes only, {} discarded), "
           "{} EFB copies "
           "({} taken per eye), {} draws (world {}, head-locked {}, head-locked 2D {}, fullscreen {}, sky {}, "
           "skipped {}), eye uniforms {:.2f} ms/frame ({} draws); per frame: fifo {:.2f} ms ({} KB), game waits "
           "for fifo {:.2f} ms ({} drains), encode {:.2f} ms, submit {:.2f} ms; uploads verts {} KB, indices {} KB, "
           "uniforms {} KB, storage {} KB, textures {} KB",
           frame.renderPasses.size(), efbPasses, eyePasses, foveated, monoSkipped, discarded, copies, eyeCopies,
           g_recorder.drawCallCount, routes[AURORA_STEREO_ROUTE_WORLD], routes[AURORA_STEREO_ROUTE_HEAD_LOCKED],
           routes[AURORA_STEREO_ROUTE_HEAD_LOCKED_2D], routes[AURORA_STEREO_ROUTE_FULLSCREEN],
           routes[AURORA_STEREO_ROUTE_SKY], routes[AURORA_STEREO_ROUTE_SKIP], eyeUniformMs, eyeUniformDraws, fifoMs,
           fifoKb, drainWaitMs, drainCalls, encodeMs, submitMs, uploads.lastVertSize / 1024,
           uploads.lastIndexSize / 1024, uploads.lastUniformSize / 1024, uploads.lastStorageSize / 1024,
           uploads.lastTextureUploadSize / 1024);
  // The FIFO processor's buckets, in ticks calibrated over the window since the
  // last line (the first line, with no window yet, shows zeros).
  static uint64_t sLastTick = perf::tick();
  static auto sLastSteady = std::chrono::steady_clock::now();
  const uint64_t nowTick = perf::tick();
  const auto nowSteady = std::chrono::steady_clock::now();
  const double windowNs =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(nowSteady - sLastSteady).count());
  const double ticksPerNs = windowNs > 0.0 ? static_cast<double>(nowTick - sLastTick) / windowNs : 0.0;
  sLastTick = nowTick;
  sLastSteady = nowSteady;
  const auto bucketMs = [ticksPerNs](std::atomic<uint64_t>& ticks) {
    const auto value = static_cast<double>(ticks.exchange(0, std::memory_order_relaxed));
    return ticksPerNs > 0.0 ? value / ticksPerNs / 600.0 / 1.0e6 : 0.0;
  };
  const auto perFrame = [](std::atomic<uint32_t>& calls) { return calls.exchange(0, std::memory_order_relaxed) / 600; };
  const double xfMs = bucketMs(perf::g_fifoXfTicks);
  const uint32_t xfLoads = perFrame(perf::g_fifoXfLoads);
  const double bpMs = bucketMs(perf::g_fifoBpTicks);
  const uint32_t bpLoads = perFrame(perf::g_fifoBpLoads);
  const double cpMs = bucketMs(perf::g_fifoCpTicks);
  const double auroraMs = bucketMs(perf::g_fifoAuroraTicks);
  const double vertsMs = bucketMs(perf::g_fifoVertsTicks);
  const uint32_t merged = perFrame(perf::g_fifoDrawsMerged);
  const uint32_t pushed = perFrame(perf::g_fifoDrawsPushed);
  const double pipelineMs = bucketMs(perf::g_fifoPipelineTicks);
  const uint32_t pipelineBuilds = perFrame(perf::g_fifoPipelineBuilds);
  const double bindsMs = bucketMs(perf::g_fifoBindsTicks);
  const uint32_t bindGroupBuilds = perFrame(perf::g_fifoBindGroupBuilds);
  const uint32_t bindGroupMisses = perFrame(perf::g_fifoBindGroupMisses);
  const double resolveMs = bucketMs(perf::g_fifoResolveTicks);
  const double uniformMs = bucketMs(perf::g_fifoUniformTicks);
  const uint32_t uniformBuilds = perFrame(perf::g_fifoUniformBuilds);
  const double pushMs = bucketMs(perf::g_fifoPushTicks);
  const double deindexMs = bucketMs(perf::g_fifoDeindexTicks);
  const uint32_t deindexedVerts = perFrame(perf::g_fifoDeindexedVerts);
  const auto perFrameKb = [](std::atomic<uint64_t>& bytes) {
    return bytes.exchange(0, std::memory_order_relaxed) / 600 / 1024;
  };
  const uint32_t cachedDlCalls = perFrame(perf::g_fifoCachedDlCalls);
  const uint64_t cachedDlKb = perFrameKb(perf::g_fifoCachedDlBytes);
  const uint64_t cachedDlVertKb = perFrameKb(perf::g_fifoCachedDlVertBytes);
  const uint64_t cachedDlIndexKb = perFrameKb(perf::g_fifoCachedDlIndexBytes);
  const uint32_t cachedDlVerts = perFrame(perf::g_fifoCachedDlVerts);
  const uint32_t geometryHits = perFrame(perf::g_fifoGeometryHits);
  const uint32_t geometryMisses = perFrame(perf::g_fifoGeometryMisses);
  const uint32_t geometryEntries = perf::g_geometryEntries.load(std::memory_order_relaxed);
  const uint64_t geometryMb = perf::g_geometryResidentBytes.load(std::memory_order_relaxed) >> 20;
  std::array<uint32_t, perf::BreakCount> breaks{};
  for (size_t i = 0; i < breaks.size(); ++i) {
    breaks[i] = perFrame(perf::g_mergeBreaks[i]);
  }
  Log.info("fifo processor per frame: xf {:.2f} ms ({} loads), bp {:.2f} ms ({} loads), cp {:.2f} ms, aurora {:.2f} "
           "ms; draws {} merged + {} pushed: verts {:.2f} ms, pipeline {:.2f} ms ({} builds), bind groups {:.2f} ms "
           "({} builds, {} cache misses, resolve {:.2f} ms), uniform {:.2f} ms ({} builds), push {:.2f} ms; "
           "de-indexing {:.2f} ms ({} vertices); in-place dl {} calls ({} KB): {} KB of records ({} de-indexed "
           "vertices), {} KB of indices; geometry cache {} hits, {} misses ({} entries, {} MiB); tick {:.3f} GHz",
           xfMs, xfLoads, bpMs, bpLoads, cpMs, auroraMs, merged, pushed, vertsMs, pipelineMs, pipelineBuilds, bindsMs,
           bindGroupBuilds, bindGroupMisses, resolveMs, uniformMs, uniformBuilds, pushMs, deindexMs, deindexedVerts,
           cachedDlCalls, cachedDlKb, cachedDlVertKb, cachedDlVerts, cachedDlIndexKb, geometryHits, geometryMisses,
           geometryEntries, geometryMb, ticksPerNs);
  Log.info("new draws per frame, by what kept them from joining the last one: pipeline state {}, textures {}, "
           "uniform data {}, immediates {}, vertex format or primitive kind {}, no draw to join {}",
           breaks[perf::BreakPipeline], breaks[perf::BreakTextures], breaks[perf::BreakUniform],
           breaks[perf::BreakImmediates], breaks[perf::BreakFormat], breaks[perf::BreakNoDraw]);
}

std::string pass_label(std::string_view kind) {
#ifdef AURORA_GFX_DEBUG_GROUPS
  if (!g_recorder.debugGroupStack.empty()) {
    return fmt::format("{} ({})", kind, g_recorder.debugGroupStack.back());
  }
#endif
  return std::string{kind};
}

void set_efb_targets(RenderPass& pass) {
  const auto layout = scene_render_target_layout();
  pass.colorAttachmentCount = layout.colorAttachmentCount;
  auto& sceneColor = pass.colorAttachments[SceneColorAttachmentIndex];
  sceneColor.semantic = ColorAttachmentSemantic::SceneColor;
  sceneColor.format = layout.colorAttachments[SceneColorAttachmentIndex].format;
  sceneColor.size = webgpu::g_frameBuffer.size;
  sceneColor.view = webgpu::g_frameBuffer.view;
  sceneColor.resolveView = layout.sampleCount > 1 ? webgpu::g_frameBufferResolved.view : nullptr;
  for (uint32_t i = SceneColorAttachmentIndex + 1; i < layout.colorAttachmentCount; ++i) {
    auto& color = pass.colorAttachments[i];
    color.semantic = layout.colorAttachments[i].semantic;
    color.format = layout.colorAttachments[i].format;
    AURORA_ASSERT(false, "Scene render-target attachment {} has no backing texture", i);
  }
  pass.depthStencilView = webgpu::g_depthBuffer.view;
  pass.depthStencilFormat = layout.depthStencilFormat;
  pass.copySourceTexture =
      webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_frameBufferResolved.texture : webgpu::g_frameBuffer.texture;
  pass.copySourceView =
      webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_frameBufferResolved.view : webgpu::g_frameBuffer.view;
  pass.copySourceDepthView = webgpu::g_depthBuffer.view;
  pass.msaaSamples = layout.sampleCount;
  pass.hasDepth = true;
  pass.hasStencil = false;
  pass.efb = true;
}

void set_single_color_target(RenderPass& pass, wgpu::TextureFormat format, const wgpu::Extent3D size,
                             wgpu::TextureView view, wgpu::TextureView resolveView = {}) {
  auto& color = pass.colorAttachments[SceneColorAttachmentIndex];
  color.semantic = ColorAttachmentSemantic::SceneColor;
  color.format = format;
  color.size = size;
  color.view = std::move(view);
  color.resolveView = std::move(resolveView);
  pass.colorAttachmentCount = 1;
}

struct OffscreenCacheKey {
  uint32_t width;
  uint32_t height;

  bool operator==(const OffscreenCacheKey& rhs) const { return width == rhs.width && height == rhs.height; }
  template <typename H>
  friend H AbslHashValue(H h, const OffscreenCacheKey& key) {
    return H::combine(std::move(h), key.width, key.height);
  }
};
struct OffscreenCacheEntry {
  webgpu::TextureWithSampler color;
  webgpu::TextureWithSampler depth;
};
absl::flat_hash_map<OffscreenCacheKey, OffscreenCacheEntry> g_offscreenCache;

// Pooled destinations for the public resolve_pass API. Entries are recycled
// per frame slot: a slot is only re-acquired after the render worker has
// submitted its previous frame, and queue serialization orders the new frame's
// copies after the old frame's reads.
struct PassSnapshotEntry {
  webgpu::TextureWithSampler color;
  webgpu::TextureWithSampler depth; // R32Float raw depth
};
struct PassSnapshotPool {
  std::vector<PassSnapshotEntry> entries;
  size_t used = 0;
};
std::array<PassSnapshotPool, FrameSlotCount> g_passSnapshotPools;

PassSnapshotEntry& acquire_pass_snapshot(uint32_t width, uint32_t height, bool wantColor, bool wantDepth) {
  auto& pool = g_passSnapshotPools[g_recorder.frameSlot];
  if (pool.used == pool.entries.size()) {
    pool.entries.emplace_back();
  }
  auto& entry = pool.entries[pool.used++];
  const wgpu::Extent3D size{width, height, 1};
  if (wantColor && (!entry.color.texture || entry.color.size.width != width || entry.color.size.height != height ||
                    entry.color.format != webgpu::g_graphicsConfig.surfaceConfiguration.format)) {
    const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
    const wgpu::TextureDescriptor desc{
        .label = "Pass Snapshot Color",
        .usage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::TextureBinding,
        .dimension = wgpu::TextureDimension::e2D,
        .size = size,
        .format = format,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    auto texture = webgpu::g_device.CreateTexture(&desc);
    auto view = texture.CreateView();
    entry.color = webgpu::TextureWithSampler{
        .texture = std::move(texture),
        .view = std::move(view),
        .size = size,
        .format = format,
    };
  }
  if (wantDepth && (!entry.depth.texture || entry.depth.size.width != width || entry.depth.size.height != height)) {
    const wgpu::TextureDescriptor desc{
        .label = "Pass Snapshot Depth",
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
        .dimension = wgpu::TextureDimension::e2D,
        .size = size,
        .format = wgpu::TextureFormat::R32Float,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    auto texture = webgpu::g_device.CreateTexture(&desc);
    auto view = texture.CreateView();
    entry.depth = webgpu::TextureWithSampler{
        .texture = std::move(texture),
        .view = std::move(view),
        .size = size,
        .format = wgpu::TextureFormat::R32Float,
    };
  }
  return entry;
}

FramePacket& current_frame_packet() { return g_recorder.frame(); }

RenderPassList& current_render_passes() { return g_recorder.passes(); }

StagingHighWater current_high_water(const FramePacket& frame) noexcept {
  return {
      .verts = static_cast<uint32_t>(frame.verts.size()),
      .uniforms = static_cast<uint32_t>(frame.uniforms.size()),
      .indices = static_cast<uint32_t>(frame.indices.size()),
      .storage = static_cast<uint32_t>(frame.storage.size()),
      .textureUpload = static_cast<uint32_t>(frame.textureUpload.size()),
      .textureUploadCount = frame.textureUploads.size(),
      .geometryUploadCount = frame.geometryUploads.size(),
  };
}

FrameOp capture_frame_op(FramePacket& frame, FrameOpType type, uint32_t index) {
  FrameOp op{
      .type = type,
      .index = index,
      .renderPass =
          type == FrameOpType::RenderPass && index < frame.renderPasses.size() ? &frame.renderPasses[index] : nullptr,
      .textureCopy = type == FrameOpType::TextureCopy && index < frame.textureCopies.size()
                         ? &frame.textureCopies[index]
                         : nullptr,
      .encoderTask =
          type == FrameOpType::EncoderTask && index < frame.encoderTasks.size() ? &frame.encoderTasks[index] : nullptr,
      .highWater = current_high_water(frame),
  };
  op.textureUploads.reserve(op.highWater.textureUploadCount);
  for (size_t i = 0; i < op.highWater.textureUploadCount; ++i) {
    op.textureUploads.push_back(&frame.textureUploads[i]);
  }
  op.geometryUploads.reserve(op.highWater.geometryUploadCount);
  for (size_t i = 0; i < op.highWater.geometryUploadCount; ++i) {
    op.geometryUploads.push_back(&frame.geometryUploads[i]);
  }
  return op;
}

void seal_pass(FramePacket& frame, uint32_t passIndex) {
  if (passIndex >= frame.renderPasses.size()) {
    return;
  }
  auto& pass = frame.renderPasses[passIndex];
  if (pass.sealed) {
    return;
  }
  pass.sealed = true;
}

Range push(ByteBuffer& target, const uint8_t* data, size_t length, size_t alignment) {
  if (alignment != 0) {
    const size_t begin = target.size();
    const size_t alignedBegin = AURORA_ALIGN(begin, alignment);
    if (alignedBegin > begin) {
      target.append_zeroes(alignedBegin - begin);
    }
  }
  const auto begin = target.size();
  if (length > 0) {
    target.append(data, length);
  }
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(length)};
}

Range map(ByteBuffer& target, size_t length, size_t alignment) {
  if (alignment != 0) {
    const size_t begin = target.size();
    const size_t alignedBegin = AURORA_ALIGN(begin, alignment);
    if (alignedBegin > begin) {
      target.append_zeroes(alignedBegin - begin);
    }
  }
  auto begin = target.size();
  if (length > 0) {
    target.append_zeroes(length);
  }
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(length)};
}

// For our public API, warn instead of fatal-ing when called outside an active recording frame.
bool check_recording(const char* name) {
  if (!g_recorder.active())
    UNLIKELY {
      Log.warn("{}: called outside an active frame", name);
      return false;
    }
  return true;
}

void push_command(CommandType type, const Command::Data& data) {
  if (g_recorder.currentRenderPass == UINT32_MAX)
    UNLIKELY {
      Log.warn("Dropping command {}", magic_enum::enum_name(type));
      return;
    }
  auto& renderPass = current_render_passes()[g_recorder.currentRenderPass];
  AURORA_ASSERT(!renderPass.sealed, "Attempted to append command {} to sealed render pass {}",
                magic_enum::enum_name(type), g_recorder.currentRenderPass);
  if (type == CommandType::Draw || type == CommandType::CustomDraw) {
    renderPass.hasDraws = true;
  }
  renderPass.commands.push_back({
      .type = type,
#ifdef AURORA_GFX_DEBUG_GROUPS
      .debugGroupStack = g_recorder.debugGroupStack,
#endif
      .data = data,
  });
}

// Inline payload helpers: draw_payload.hpp (shared with the stereo eye encoder).

void push_draw_command(DrawCommand data) {
  ++g_recorder.routeDrawCounts[g_recorder.stereoRoute & 7];
  push_command(CommandType::Draw, Command::Data{.draw = data});
  ++g_recorder.drawCallCount;
}

OffscreenCacheEntry get_offscreen_textures(uint32_t width, uint32_t height) {
  OffscreenCacheKey key{width, height};
  if (const auto it = g_offscreenCache.find(key); it != g_offscreenCache.end()) {
    return it->second;
  }
  const auto colorFormat = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  const wgpu::Extent3D size{width, height, 1};
  const wgpu::TextureDescriptor colorDesc{
      .label = "Offscreen Color",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc |
               wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = colorFormat,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto colorTexture = webgpu::g_device.CreateTexture(&colorDesc);
  auto colorView = colorTexture.CreateView();
  webgpu::TextureWithSampler color{
      .texture = std::move(colorTexture),
      .view = std::move(colorView),
      .size = size,
      .format = colorFormat,
  };
  const auto depthFormat = webgpu::g_graphicsConfig.depthFormat;
  const wgpu::TextureDescriptor depthDesc{
      .label = "Offscreen Depth",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = depthFormat,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto depthTexture = webgpu::g_device.CreateTexture(&depthDesc);
  auto depthView = depthTexture.CreateView();
  webgpu::TextureWithSampler depth{
      .texture = std::move(depthTexture),
      .view = std::move(depthView),
      .size = size,
      .format = depthFormat,
  };
  OffscreenCacheEntry entry{
      .color = std::move(color),
      .depth = std::move(depth),
  };
  auto [insertIt, _] = g_offscreenCache.emplace(key, std::move(entry));
  return insertIt->second;
}

void enqueue_pass(FramePacket& frame, uint32_t passIndex);

void resume_efb_pass_loading(const RenderPass& prevPass) {
  RenderPass newPass{
      .label = pass_label("EFB"),
      .colorAttachments = prevPass.colorAttachments,
      .colorAttachmentCount = prevPass.colorAttachmentCount,
      .depthStencilView = prevPass.depthStencilView,
      .depthStencilFormat = prevPass.depthStencilFormat,
      .copySourceTexture = prevPass.copySourceTexture,
      .copySourceView = prevPass.copySourceView,
      .copySourceDepthView = prevPass.copySourceDepthView,
      .msaaSamples = prevPass.msaaSamples,
      .clearDepth = false,
      .hasDepth = prevPass.hasDepth,
      .hasStencil = prevPass.hasStencil,
      // Still the EFB: the stereo replay carries on into the eyes after a copy.
      .efb = prevPass.efb,
  };
  for (uint32_t i = 0; i < newPass.colorAttachmentCount; ++i) {
    newPass.colorAttachments[i].loadOp = wgpu::LoadOp::Undefined;
    newPass.colorAttachments[i].clear = false;
  }
  newPass.commands.reserve(2048);
  current_render_passes().emplace_back(std::move(newPass));
  ++g_recorder.currentRenderPass;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_recorder.cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_recorder.cachedScissor});
}

void suspend_efb() {
  AURORA_ASSERT(g_recorder.active() && g_recorder.currentRenderPass != UINT32_MAX,
                "suspend_efb called outside of an active recording frame");
  AURORA_ASSERT(!g_recorder.inOffscreen, "suspend_efb called while offscreen rendering is active");
  AURORA_ASSERT(!g_recorder.suspendedEfbPass, "suspend_efb called with an EFB pass already suspended");

  auto& currentPass = current_render_passes()[g_recorder.currentRenderPass];
  if (!currentPass.has_consumer()) {
    g_recorder.suspendedEfbPass = std::move(currentPass);
    current_render_passes().pop_back();
    --g_recorder.currentRenderPass;
  } else {
    enqueue_pass(current_frame_packet(), g_recorder.currentRenderPass);
  }
  g_recorder.suspendedEfbViewport = g_recorder.cachedViewport;
  g_recorder.suspendedEfbScissor = g_recorder.cachedScissor;
}

void finish_current_offscreen() {
  AURORA_ASSERT(g_recorder.active() && g_recorder.currentRenderPass != UINT32_MAX,
                "finish_current_offscreen called outside of an active recording frame");
  AURORA_ASSERT(g_recorder.inOffscreen, "finish_current_offscreen called without an active offscreen pass");

  auto& offscreenPass = current_render_passes()[g_recorder.currentRenderPass];
  offscreenPass.discardable = !offscreenPass.has_consumer();
  enqueue_pass(current_frame_packet(), g_recorder.currentRenderPass);
  g_recorder.offscreenColor = {};
  g_recorder.offscreenDepth = {};
}

void start_offscreen(uint32_t width, uint32_t height) {
  AURORA_ASSERT(width != 0 && height != 0, "start_offscreen requires nonzero dimensions ({}x{})", width, height);
  AURORA_ASSERT(g_recorder.active(), "start_offscreen called outside of an active recording frame");

  auto offscreenEntry = get_offscreen_textures(width, height);
  g_recorder.offscreenColor = std::move(offscreenEntry.color);
  g_recorder.offscreenDepth = std::move(offscreenEntry.depth);

  RenderPass newPass{
      .label = pass_label("Offscreen"),
      .depthStencilView = g_recorder.offscreenDepth.view,
      .depthStencilFormat = g_recorder.offscreenDepth.format,
      .copySourceTexture = g_recorder.offscreenColor.texture,
      .copySourceView = g_recorder.offscreenColor.view,
      .copySourceDepthView = g_recorder.offscreenDepth.view,
      .msaaSamples = 1,
      .clearDepthValue = gx::UseReversedZ ? 0.f : 1.f,
      .clearDepth = true,
      .hasDepth = true,
      .hasStencil = false,
  };
  set_single_color_target(newPass, g_recorder.offscreenColor.format, {width, height}, g_recorder.offscreenColor.view);
  current_render_passes().emplace_back(std::move(newPass));
  ++g_recorder.currentRenderPass;
  g_recorder.inOffscreen = true;

  g_recorder.cachedViewport = {0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f};
  g_recorder.cachedScissor = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_recorder.cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_recorder.cachedScissor});
}

void restore_efb() {
  AURORA_ASSERT(g_recorder.active(), "restore_efb called outside of an active recording frame");
  AURORA_ASSERT(g_recorder.inOffscreen, "restore_efb called without a suspended EFB pass");

  g_recorder.inOffscreen = false;
  if (g_recorder.suspendedEfbPass) {
    current_render_passes().emplace_back(std::move(*g_recorder.suspendedEfbPass));
    g_recorder.suspendedEfbPass.reset();
  } else {
    auto& pass = current_render_passes().emplace_back();
    pass.label = pass_label("EFB");
    set_efb_targets(pass);
    for (uint32_t i = 0; i < pass.colorAttachmentCount; ++i) {
      pass.colorAttachments[i].clear = false;
      pass.colorAttachments[i].loadOp = wgpu::LoadOp::Undefined;
    }
    pass.clearDepth = false;
  }
  ++g_recorder.currentRenderPass;
  set_efb_targets(current_render_passes()[g_recorder.currentRenderPass]);

  g_recorder.cachedViewport = g_recorder.suspendedEfbViewport;
  g_recorder.cachedScissor = g_recorder.suspendedEfbScissor;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_recorder.cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_recorder.cachedScissor});
}

void enqueue_op(FramePacket& frame, uint32_t opIndex) {
  if (opIndex >= frame.ops.size() || g_recorder.suppressRenderWorker) {
    return;
  }
  auto op = frame.ops[opIndex];
  render_worker::enqueue_encode_pass(frame.frameId, opIndex, [packet = &frame, op = std::move(op)] {
    if (op.renderPass == nullptr && op.textureCopy == nullptr && op.encoderTask == nullptr) {
      return;
    }
    const perf::Timer timer{perf::g_encodeNs};
    encode_op(packet->encoder, *packet, op);
  });
}

// Gives an EFB pass of an immersive frame its eye passes: encoding.cpp
// re-encodes the pass into each eye target right after the mono pass.
void stereo_seal_pass(FramePacket& frame, uint32_t passIndex) {
  if (passIndex >= frame.renderPasses.size()) {
    return;
  }
  auto& pass = frame.renderPasses[passIndex];
  auto& state = frame.stereo;
  if (!state.immersive || !pass.efb || pass.sealed || pass.stereo.enabled || pass.stereo.monoOnly) {
    return;
  }
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    const auto& target = stereo_eye_target(eye);
    if (!target.valid()) {
      return;
    }
    // A foveated frame's eye pass renders through the foveated view of the
    // same image (stereo_foveation.hpp); the copies keep the eye's own view.
    const bool foveatedEye = !state.multiview && state.foveated && state.foveatedEyeColorViews[eye];
    pass.stereo.eyes[eye] = StereoEyePass{
        .colorView = foveatedEye ? state.foveatedEyeColorViews[eye] : target.color.view,
        .resolveView = target.sampleCount > 1 ? target.resolved.view : wgpu::TextureView{},
        .depthView = target.depth.view,
        .copySourceView = target.color.view,
        .size = {target.width, target.height, 1},
    };
  }
  if (state.multiview) {
    // Both eyes in one pass over the targets' two layers (stereo_multiview.hpp).
    const auto& multiview = stereo_multiview_target();
    if (!multiview.colorView || !multiview.depthView) {
      return;
    }
    const bool foveated = state.foveated && state.foveatedMultiviewColorView;
    pass.stereo.multiview = true;
    pass.stereo.multiviewColorView = foveated ? state.foveatedMultiviewColorView : multiview.colorView;
    pass.stereo.multiviewDepthView = multiview.depthView;
    pass.stereo.foveated = foveated;
  } else {
    pass.stereo.foveated = state.foveated && state.foveatedEyeColorViews[0] && state.foveatedEyeColorViews[1];
  }
  pass.stereo.enabled = true;
  state.replayed = true;
}

void enqueue_pass(FramePacket& frame, uint32_t passIndex) {
  stereo_seal_pass(frame, passIndex);
  seal_pass(frame, passIndex);
  const auto opIndex = static_cast<uint32_t>(frame.ops.size());
  frame.ops.emplace_back(capture_frame_op(frame, FrameOpType::RenderPass, passIndex));
  enqueue_op(frame, opIndex);
}
} // namespace

namespace detail {

void begin_recording(FramePacket& packet, size_t frameSlot) {
  CHECK(!g_recorder.active(), "A recording session is already active");
  g_recorder.packet = &packet;
  g_recorder.frameSlot = frameSlot;
  g_passSnapshotPools[frameSlot].used = 0;
  g_recorder.drawCallCount = 0;
  g_recorder.routeDrawCounts = {};
  g_recorder.mergedDrawCallCount = 0;
  g_recorder.suspendedEfbPass.reset();
  g_recorder.stereoRoute = AURORA_STEREO_ROUTE_WORLD;
  g_recorder.stereoScreenTexMtx = {};

  current_render_passes().emplace_back();
  auto& pass = current_render_passes()[0];
  pass.label = pass_label("EFB");
  set_efb_targets(pass);
  pass.colorAttachments[SceneColorAttachmentIndex].clearValue = gx::g_gxState.clearColor;
  pass.clearDepthValue = gx::clear_depth_value();
  g_recorder.currentRenderPass = 0;
  g_recorder.cachedViewport = gx::map_logical_viewport(gx::g_gxState.logicalViewport);
  g_recorder.cachedScissor = gx::map_logical_scissor(gx::g_gxState.logicalScissor);
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_recorder.cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_recorder.cachedScissor});
}

RecordedFrame end_recording() {
  CHECK(g_recorder.active(), "No active recording session");
  AURORA_ASSERT(!g_recorder.inOffscreen, "end_frame called while offscreen rendering is active");
  AURORA_ASSERT(g_recorder.currentRenderPass == UINT32_MAX,
                "end_frame called before finish finalized the current render pass");
  auto& frame = g_recorder.frame();
  frame.stats.drawCallCount = g_recorder.drawCallCount;
  frame.stats.mergedDrawCallCount = g_recorder.mergedDrawCallCount;
  frame.stats.lastVertSize = frame.verts.size();
  frame.stats.lastUniformSize = frame.uniforms.size();
  frame.stats.lastIndexSize = frame.indices.size();
  frame.stats.lastStorageSize = frame.storage.size();
  frame.stats.lastTextureUploadSize = frame.textureUpload.size();

  for (auto& array : gx::g_gxState.arrays) {
    array.cachedRange = {};
  }
#if defined(AURORA_GFX_DEBUG_GROUPS)
  if (!g_recorder.debugGroupStack.empty()) {
    for (auto& item : std::ranges::reverse_view(g_recorder.debugGroupStack)) {
      Log.warn("Debug group was not popped at end of frame: {}", item);
    }
    g_recorder.debugGroupStack.clear();
  }
#endif
  const RecordedFrame recorded{.packet = g_recorder.packet, .frameSlot = g_recorder.frameSlot};
  g_recorder.packet = nullptr;
  g_recorder.frameSlot = 0;
  return recorded;
}

void shutdown_recording() {
  for (auto& pool : g_passSnapshotPools) {
    pool = {};
  }
  g_recorder.currentRenderPass = UINT32_MAX;
  g_offscreenCache.clear();
  g_recorder.offscreenColor = {};
  g_recorder.offscreenDepth = {};
  g_recorder.suspendedEfbPass.reset();
  g_recorder.inOffscreen = false;
  g_recorder.packet = nullptr;
  g_recorder.frameSlot = 0;
  g_recorder.suppressRenderWorker = false;
}

namespace testing {

void suppress_render_worker(bool suppress) noexcept { g_recorder.suppressRenderWorker = suppress; }

void seed_offscreen_cache(uint32_t width, uint32_t height, wgpu::TextureFormat colorFormat,
                          wgpu::TextureFormat depthFormat) {
  const wgpu::Extent3D size{width, height, 1};
  g_offscreenCache.insert_or_assign(OffscreenCacheKey{width, height},
                                    OffscreenCacheEntry{
                                        .color = {.size = size, .format = colorFormat},
                                        .depth = {.size = size, .format = depthFormat},
                                    });
}

} // namespace testing

void increment_merged_draw_count(uint32_t count) noexcept {
  if (g_recorder.active()) {
    g_recorder.mergedDrawCallCount += count;
  }
}

} // namespace detail

void queue_texture_upload(TextureUpload upload) {
  if (g_recorder.currentRenderPass != UINT32_MAX) {
    AURORA_ASSERT(!current_render_passes()[g_recorder.currentRenderPass].sealed,
                  "Attempted to append texture upload to sealed render pass {}", g_recorder.currentRenderPass);
  }
  current_frame_packet().textureUploads.emplace_back(std::move(upload));
}

void queue_texture_upload_data(const uint8_t* data, uint32_t bytesPerRow, uint32_t rowsPerImage,
                               wgpu::TexelCopyTextureInfo tex, wgpu::Extent3D size) {
  const auto copyBytesPerRow = AURORA_ALIGN(bytesPerRow, 256);
  auto& frame = current_frame_packet();
  if (frame.textureUpload.size() + copyBytesPerRow * rowsPerImage <= TextureUploadSize) {
    const auto range = push_texture_data(data, bytesPerRow, rowsPerImage);
    const wgpu::TexelCopyBufferLayout layout{
        .offset = range.offset,
        .bytesPerRow = bytesPerRow,
        .rowsPerImage = rowsPerImage,
    };
    queue_texture_upload(TextureUpload{layout, std::move(tex), size});
    return;
  }

  const uint64_t uploadSize = copyBytesPerRow * rowsPerImage;
  const wgpu::BufferDescriptor descriptor{
      .label = "Overflow Texture Upload Buffer",
      .usage = wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc,
      .size = uploadSize,
      .mappedAtCreation = true,
  };
  auto buffer = webgpu::g_device.CreateBuffer(&descriptor);
  auto* dst = static_cast<uint8_t*>(buffer.GetMappedRange(0, uploadSize));
  for (uint32_t row = 0; row < rowsPerImage; ++row) {
    memcpy(dst, data, bytesPerRow);
    data += bytesPerRow;
    dst += copyBytesPerRow;
  }
  buffer.Unmap();

  const wgpu::TexelCopyBufferLayout layout{
      .offset = 0,
      .bytesPerRow = bytesPerRow,
      .rowsPerImage = rowsPerImage,
  };
  queue_texture_upload(TextureUpload{layout, std::move(tex), size, std::move(buffer)});
}

void queue_texture_copy(wgpu::TexelCopyTextureInfo src, wgpu::TexelCopyTextureInfo dst, wgpu::Extent3D size) {
  ZoneScoped;
  auto& frame = current_frame_packet();
  if (g_recorder.currentRenderPass != UINT32_MAX) {
    enqueue_pass(frame, g_recorder.currentRenderPass);
    g_recorder.currentRenderPass = UINT32_MAX;
  }

  const auto copyIndex = static_cast<uint32_t>(frame.textureCopies.size());
  frame.textureCopies.emplace_back(TextureCopy{
      .src = std::move(src),
      .dst = std::move(dst),
      .size = size,
  });
  const auto opIndex = static_cast<uint32_t>(frame.ops.size());
  frame.ops.emplace_back(capture_frame_op(frame, FrameOpType::TextureCopy, copyIndex));
  enqueue_op(frame, opIndex);
}

void begin_color_pass(const ColorPassDescriptor& desc) {
  ZoneScoped;
  auto& frame = current_frame_packet();
  if (g_recorder.currentRenderPass != UINT32_MAX) {
    enqueue_pass(frame, g_recorder.currentRenderPass);
  }

  RenderPass pass{
      .label = desc.label != nullptr ? desc.label : "",
      .depthStencilView = desc.depthStencilView,
      .depthStencilFormat = desc.depthStencilFormat,
      .msaaSamples = desc.sampleCount,
      .clearDepthValue = desc.depthClearValue,
      .depthLoadOp = desc.depthLoadOp,
      .depthStoreOp = desc.depthStoreOp,
      .stencilLoadOp = desc.stencilLoadOp,
      .stencilStoreOp = desc.stencilStoreOp,
      .stencilClearValue = desc.stencilClearValue,
      .clearDepth = desc.depthLoadOp == wgpu::LoadOp::Clear,
      .hasDepth = desc.hasDepth,
      .hasStencil = desc.hasStencil,
  };
  set_single_color_target(pass, desc.colorFormat, desc.targetSize, desc.colorView, desc.resolveView);
  auto& color = pass.colorAttachments[SceneColorAttachmentIndex];
  color.clearValue = {
      static_cast<float>(desc.clearColor.r),
      static_cast<float>(desc.clearColor.g),
      static_cast<float>(desc.clearColor.b),
      static_cast<float>(desc.clearColor.a),
  };
  color.loadOp = desc.colorLoadOp;
  color.storeOp = desc.colorStoreOp;
  color.clear = desc.colorLoadOp == wgpu::LoadOp::Clear;
  pass.commands.reserve(128);
  frame.renderPasses.emplace_back(std::move(pass));
  g_recorder.currentRenderPass = static_cast<uint32_t>(frame.renderPasses.size() - 1);

  g_recorder.cachedViewport = {
      0.f, 0.f, static_cast<float>(desc.targetSize.width), static_cast<float>(desc.targetSize.height), 0.f, 1.f};
  g_recorder.cachedScissor = {0, 0, static_cast<int32_t>(desc.targetSize.width),
                              static_cast<int32_t>(desc.targetSize.height)};
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_recorder.cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_recorder.cachedScissor});
}

void end_color_pass() {
  ZoneScoped;
  if (g_recorder.currentRenderPass == UINT32_MAX) {
    return;
  }
  enqueue_pass(current_frame_packet(), g_recorder.currentRenderPass);
  g_recorder.currentRenderPass = UINT32_MAX;
}

template <>
gx::DrawData* get_last_draw_command() {
  if (g_recorder.currentRenderPass >= current_render_passes().size()) {
    return nullptr;
  }
  auto& last = current_render_passes()[g_recorder.currentRenderPass].commands.back();
  if (last.type != CommandType::Draw || last.data.draw.encoder != encode_draw<gx::render, gx::DrawData>) {
    return nullptr;
  }
  return &inline_payload<gx::DrawData>(last.data.draw.payload.data());
}

Vec2<uint32_t> get_render_target_size() noexcept {
  // The GX FIFO worker can process a viewport command after the frame's
  // recording session has ended (e.g. during a world transition that waits on
  // the FIFO). frame() would then dereference a null packet; fall back to the
  // window size instead.
  if (g_recorder.active() && g_recorder.currentRenderPass < current_render_passes().size()) {
    const auto& size =
        current_render_passes()[g_recorder.currentRenderPass].colorAttachments[SceneColorAttachmentIndex].size;
    return {size.width, size.height};
  }
  const auto windowSize = window::get_window_size();
  return {windowSize.fb_width, windowSize.fb_height};
}

void set_viewport(const Viewport& cmd) noexcept {
  if (cmd != g_recorder.cachedViewport) {
    push_command(CommandType::SetViewport, Command::Data{.setViewport = cmd});
    g_recorder.cachedViewport = cmd;
  }
}

void set_scissor(const ClipRect& cmd) noexcept {
  if (cmd != g_recorder.cachedScissor) {
    push_command(CommandType::SetScissor, Command::Data{.setScissor = cmd});
    g_recorder.cachedScissor = cmd;
  }
}

// --- stereo replay ---

void set_frame_stereo(const StereoFrameState& state) noexcept {
  stereo_shadow::begin_frame(state.immersive, state.multiview);
  if (!g_recorder.active()) {
    return;
  }
  g_recorder.frame().stereo = state;
}

StereoFrameState recorded_stereo_state() noexcept {
  if (!g_recorder.active()) {
    return {};
  }
  return g_recorder.frame().stereo;
}

void set_stereo_draw_route(uint8_t route) noexcept { g_recorder.stereoRoute = route; }

bool recording_multiview() noexcept { return g_recorder.active() && g_recorder.frame().stereo.multiview; }

void set_final_pass_mono_unneeded(bool unneeded) noexcept { g_recorder.finalPassMonoUnneeded = unneeded; }

void set_stereo_diagnostics(bool enabled) noexcept {
  g_stereoDiagnostics.store(enabled || frame_stats_forced(), std::memory_order_relaxed);
}

uint8_t stereo_draw_route() noexcept { return g_recorder.stereoRoute; }

void set_stereo_head_locked_plane(float tanHalfWidth, float tanHalfHeight, float distance) noexcept {
  g_recorder.headLockedPlane = {tanHalfWidth, tanHalfHeight, distance};
}

stereo_replay::HeadLockedPlane stereo_head_locked_plane() noexcept { return g_recorder.headLockedPlane; }

void set_stereo_screen_tex_mtx(uint8_t texSlot, uint8_t pnSlot) noexcept {
  g_recorder.stereoScreenTexMtx = {texSlot, pnSlot};
}

StereoScreenTexMtx stereo_screen_tex_mtx() noexcept { return g_recorder.stereoScreenTexMtx; }

std::array<uint32_t, 2> stage_stereo_uniforms(const uint8_t* mono, Range monoRange,
                                              const StereoUniformLayout& layout) noexcept {
  constexpr std::array<uint32_t, 2> none{UINT32_MAX, UINT32_MAX};
  g_recorder.multiviewMode = gx::MultiviewNone;
  if (!g_recorder.active()) {
    return none;
  }
  auto& frame = g_recorder.frame();
  const auto& state = frame.stereo;
  if (!state.immersive || g_recorder.inOffscreen || g_recorder.currentRenderPass == UINT32_MAX) {
    return none;
  }
  const uint8_t route = g_recorder.stereoRoute;
  if (route == AURORA_STEREO_ROUTE_SKIP) {
    return none;
  }
  if (!layout.valid() || monoRange.size != layout.size) {
    return none;
  }
  const size_t alignment = resources().limits.minUniformBufferOffsetAlignment;
  const auto exhausted = [&] {
    if (!frame.stereo.uniformsExhausted) {
      Log.warn("Stereo replay ran out of uniform space at {} bytes; the rest of frame {} is mono only",
               frame.uniforms.size(), frame.frameId);
      frame.stereo.uniformsExhausted = true;
    }
    return none;
  };
  const auto& pass = current_render_passes()[g_recorder.currentRenderPass];
  const auto& monoSize = pass.colorAttachments[SceneColorAttachmentIndex].size;
  Mat4x4<float> monoProjection;
  std::memcpy(&monoProjection, mono + layout.projectionOffset, sizeof(monoProjection));
  // gx/shader.cpp MultiviewClip (and EyeClipImmediate for a frame replayed per eye):
  // the draw keeps its mono uniform, followed by each eye's clip matrix and the eyes'
  // render size (the eye target's, as compose_stereo_uniform makes it, or for 2D
  // content the mono one).
  const auto stage_eye_clips = [&](const std::array<Mat4x4<float>, AURORA_STEREO_EYE_COUNT>& clips,
                                   bool eyeRenderSize) {
    struct EyeClipBlock {
      std::array<Mat4x4<float>, AURORA_STEREO_EYE_COUNT> clip;
      std::array<float, 4> render;
    };
    static_assert(sizeof(EyeClipBlock) == 144);
    float renderSize[2];
    std::memcpy(renderSize, mono, sizeof(renderSize));
    if (eyeRenderSize) {
      const auto& params = state.eyes[0]; // both eyes share a size under multiview
      renderSize[0] *=
          monoSize.width != 0 ? static_cast<float>(params.width) / static_cast<float>(monoSize.width) : 1.0f;
      renderSize[1] *=
          monoSize.height != 0 ? static_cast<float>(params.height) / static_cast<float>(monoSize.height) : 1.0f;
    }
    const EyeClipBlock block{
        .clip = clips,
        .render = {renderSize[0], renderSize[1], 0.0f, 0.0f},
    };
    if (frame.uniforms.size() + sizeof(block) + alignment + 2 * gx::MaxUniformSize > frame.uniforms.capacity()) {
      return exhausted();
    }
    const auto range = push(frame.uniforms, reinterpret_cast<const uint8_t*>(&block), sizeof(block), alignment);
    // The shader finds the block at the mono uniform's aligned end.
    if (range.offset != monoRange.offset + AURORA_ALIGN(monoRange.size, alignment)) {
      static bool sReported = false;
      if (!sReported) {
        sReported = true;
        Log.error("Multiview eye clips staged at {}, not after the uniform at {} ({} bytes)", range.offset,
                  monoRange.offset, monoRange.size);
      }
      return none;
    }
    g_recorder.multiviewMode = gx::MultiviewClip;
    return std::array<uint32_t, 2>{monoRange.offset, monoRange.offset};
  };
  // 2D content and full-screen effects: the same uniform in both eyes.
  const std::array<uint32_t, 2> same{monoRange.offset, monoRange.offset};
  if (route == AURORA_STEREO_ROUTE_FULLSCREEN || route == AURORA_STEREO_ROUTE_SCREEN_2D) {
    return state.multiview ? stage_eye_clips({monoProjection, monoProjection}, false) : same;
  }
  // An orthographic draw is 2D content, identical in both eyes, unless its
  // route lays it on the head-locked plane (stereo_replay.hpp HeadLockedPlane).
  const bool perspective = stereo_uniform_is_perspective(mono, layout);
  const bool onPlane =
      !perspective && route == AURORA_STEREO_ROUTE_HEAD_LOCKED_2D && g_recorder.headLockedPlane.valid();
  if (!perspective && !onPlane) {
    return state.multiview ? stage_eye_clips({monoProjection, monoProjection}, false) : same;
  }
  const stereo_replay::HudScreen plane =
      onPlane ? stereo_replay::head_locked_plane_screen(g_recorder.headLockedPlane, state.headLockedScaleXY,
                                                        state.headLockedScaleZ)
              : stereo_replay::HudScreen{};
  const bool headLocked = route == AURORA_STEREO_ROUTE_HEAD_LOCKED || route == AURORA_STEREO_ROUTE_HEAD_LOCKED_2D;
  // A screen-projecting texture matrix (AuroraSetStereoScreenTexMtx) differs per eye:
  // under multiview such a draw takes the full eye copies (MultiviewFull).
  const auto screenTexMtx = g_recorder.stereoScreenTexMtx;
  const bool perEyeTexMtx = !onPlane && screenTexMtx.texSlot != 0xFF;
  if (!perEyeTexMtx) {
    std::array<Mat4x4<float>, AURORA_STEREO_EYE_COUNT> clips;
    for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
      const auto& params = state.eyes[eye];
      if (onPlane) {
        clips[eye] = stereo_replay::compose_head_locked_2d_projection(
            params.projection, params.headLockedViewFromCenter, plane, monoProjection);
      } else {
        const Mat3x4<float>& viewFromCenter = headLocked                            ? params.headLockedViewFromCenter
                                              : route == AURORA_STEREO_ROUTE_SKY ? params.skyViewFromCenter
                                                                                 : params.viewFromCenter;
        clips[eye] = compose_eye_clip(stereo_replay::compose_projection(params.projection, monoProjection),
                                      viewFromCenter, headLocked ? state.headLockedScaleXY : 1.0f,
                                      headLocked ? state.headLockedScaleZ : 1.0f);
      }
    }
    return stage_eye_clips(clips, true);
  }
  const size_t needed =
      2 * (AURORA_ALIGN(static_cast<size_t>(layout.size), alignment) + alignment) + 2 * gx::MaxUniformSize;
  if (frame.uniforms.size() + needed > frame.uniforms.capacity()) {
    return exhausted();
  }
  // Diagnostics: what the eye copies cost (log_stereo_frame_stats).
  struct EyeUniformTimer {
    const bool on = g_stereoDiagnostics.load(std::memory_order_relaxed);
    const std::chrono::steady_clock::time_point start =
        on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ~EyeUniformTimer() {
      if (on) {
        g_eyeUniformNs.fetch_add(
            static_cast<uint64_t>((std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
        g_eyeUniformDraws.fetch_add(1, std::memory_order_relaxed);
      }
    }
  } eyeUniformTimer;
  // A screen-projecting texture matrix is derived from position matrix pnSlot, so
  // that slot must be composed whichever one the vertices use.
  StereoUniformLayout eyeLayout = layout;
  if (screenTexMtx.texSlot != 0xFF) {
    eyeLayout.fixedPositionSlot = -1;
  }
  thread_local std::vector<uint8_t> scratch;
  std::array<uint32_t, 2> offsets{};
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    scratch.assign(mono, mono + layout.size);
    const auto& params = state.eyes[eye];
    const float renderScaleX =
        monoSize.width != 0 ? static_cast<float>(params.width) / static_cast<float>(monoSize.width) : 1.0f;
    const float renderScaleY =
        monoSize.height != 0 ? static_cast<float>(params.height) / static_cast<float>(monoSize.height) : 1.0f;
    if (onPlane) {
      compose_stereo_2d_uniform(scratch.data(), layout,
                                StereoEye2DCompose{
                                    .projection = &params.projection,
                                    .headLockedViewFromCenter = &params.headLockedViewFromCenter,
                                    .plane = plane,
                                    .renderScaleX = renderScaleX,
                                    .renderScaleY = renderScaleY,
                                });
    } else {
      // A sky draw (AURORA_STEREO_ROUTE_SKY) takes the eye's rotation only.
      const Mat3x4<float>* viewFromCenter = headLocked                            ? &params.headLockedViewFromCenter
                                            : route == AURORA_STEREO_ROUTE_SKY ? &params.skyViewFromCenter
                                                                               : &params.viewFromCenter;
      compose_stereo_uniform(
          scratch.data(), eyeLayout,
          StereoEyeCompose{
              .projection = &params.projection,
              .viewFromCenter = viewFromCenter,
              .positionScaleXY = headLocked ? state.headLockedScaleXY : 1.0f,
              .positionScaleZ = headLocked ? state.headLockedScaleZ : 1.0f,
              .renderScaleX = renderScaleX,
              .renderScaleY = renderScaleY,
          });
      // A screen-projecting texture matrix (AuroraSetStereoScreenTexMtx) is
      // derived again from the eye's composed projection and position matrix.
      if (screenTexMtx.texSlot != 0xFF) {
        compose_stereo_screen_tex_mtx(scratch.data(), eyeLayout, screenTexMtx.texSlot, screenTexMtx.pnSlot);
      }
    }
    offsets[eye] = push(frame.uniforms, scratch.data(), layout.size, alignment).offset;
  }
  if (state.multiview) {
    g_recorder.multiviewMode = gx::MultiviewFull;
  }
  return offsets;
}

uint8_t stereo_multiview_mode() noexcept { return g_recorder.multiviewMode; }

template <>
void push_draw_command(clear::DrawData data) {
  push_draw_command(make_draw_command<clear::render>(data));
}

template <>
PipelineRef pipeline_ref(const clear::PipelineConfig& config) {
  return find_pipeline(ShaderType::Clear, config, [=] { return create_pipeline(config); });
}

PipelineRef clear_multiview_pipeline_ref(const clear::PipelineConfig& config) noexcept {
  if (!recording_multiview()) {
    return {};
  }
  clear::PipelineConfig multiviewConfig = config;
  multiviewConfig.multiview = true;
  return pipeline_ref(multiviewConfig);
}

void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat,
                       int probeFace) {
  // Resolve current render pass
  auto& prevPass = current_render_passes()[g_recorder.currentRenderPass];
  prevPass.resolveTarget = std::move(texture);
  prevPass.resolveRect = rect;
  prevPass.resolveFormat = resolveFormat;
  // Push UV transform uniform for tex_copy_conv (crop region in UV space)
  const auto srcW = static_cast<float>(prevPass.colorAttachments[SceneColorAttachmentIndex].size.width);
  const auto srcH = static_cast<float>(prevPass.colorAttachments[SceneColorAttachmentIndex].size.height);
  const std::array uvTransform{
      static_cast<float>(rect.x) / srcW,
      static_cast<float>(rect.y) / srcH,
      static_cast<float>(rect.width) / srcW,
      static_cast<float>(rect.height) / srcH,
  };
  prevPass.resolveUniformRange = push_uniform(uvTransform);
  if (probeFace >= 0) {
    prevPass.probeFace = probeFace;
    prevPass.probeUniformRange = push_uniform(std::array{0.f, 0.f, 1.f, 1.f});
  }
  // Stereo replay: a copy from an EFB pass that replays per eye is also taken
  // from each eye, into the copy's stand-ins (stereo_shadow.hpp), so effects
  // that sample it (the thermal and X-ray visors) see each eye's own view. Set
  // before the pass is enqueued: the frame worker may encode it at once.
  // A copy made under AURORA_STEREO_ROUTE_SKIP is not a view (a shadow rendered
  // from a light): it is taken from the mono EFB only and the eyes sample it.
  // When it also clears the whole EFB, the next pass's eye passes clear the
  // eyes too, so nothing could see this pass in them: it is not replayed.
  const bool monoCopy = g_recorder.stereoRoute == AURORA_STEREO_ROUTE_SKIP && probeFace < 0;
  if (monoCopy && clearColor && clearAlpha && (clearDepth || !prevPass.hasDepth) && !prevPass.hasStencil) {
    prevPass.stereo.monoOnly = true;
  }
  stereo_seal_pass(current_frame_packet(), g_recorder.currentRenderPass);
  if (probeFace < 0) {
    if (prevPass.stereo.enabled && !monoCopy && !gx::is_depth_format(resolveFormat)) {
      // Each eye's copy at that eye's resolution (stereo_shadow.hpp).
      const auto& efbSize = prevPass.colorAttachments[SceneColorAttachmentIndex].size;
      std::array<stereo_shadow::EyeSize, AURORA_STEREO_EYE_COUNT> eyeTargets{};
      for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
        eyeTargets[eye] = {prevPass.stereo.eyes[eye].size.width, prevPass.stereo.eyes[eye].size.height};
      }
      prevPass.stereo.copyTargets = stereo_shadow::copy_targets(
          prevPass.resolveTarget, {efbSize.width, efbSize.height}, eyeTargets);
      // A copy made under AURORA_STEREO_ROUTE_HEAD_LOCKED_2D is taken from
      // each eye through the head-locked plane, so it holds what that eye sees
      // behind the copied rectangle: the scan visor's window magnifies what
      // is straight ahead of the head (stereo_replay.hpp HeadLockedPlane).
      if (g_recorder.stereoRoute == AURORA_STEREO_ROUTE_HEAD_LOCKED_2D && g_recorder.headLockedPlane.valid()) {
        const auto& stereo = current_frame_packet().stereo;
        const auto plane = stereo_replay::head_locked_plane_screen(g_recorder.headLockedPlane,
                                                                   stereo.headLockedScaleXY, stereo.headLockedScaleZ);
        const float ndcLeft = 2.0f * static_cast<float>(rect.x) / srcW - 1.0f;
        const float ndcRight = 2.0f * static_cast<float>(rect.x + rect.width) / srcW - 1.0f;
        const float ndcTop = 1.0f - 2.0f * static_cast<float>(rect.y) / srcH;
        const float ndcBottom = 1.0f - 2.0f * static_cast<float>(rect.y + rect.height) / srcH;
        for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
          const auto& params = stereo.eyes[eye];
          const auto uv = stereo_replay::head_locked_plane_uv_rect(params.projection, params.headLockedViewFromCenter,
                                                                   plane, ndcLeft, ndcTop, ndcRight, ndcBottom);
          if (uv.valid()) {
            prevPass.stereo.copyUniformRanges[eye] = push_uniform(std::array{uv.u, uv.v, uv.width, uv.height});
          }
        }
      }
    } else {
      stereo_shadow::invalidate(prevPass.resolveTarget.get());
    }
  }
  enqueue_pass(current_frame_packet(), g_recorder.currentRenderPass);

  // Populate new render pass from previous
  const auto msaaSamples = prevPass.msaaSamples;
  RenderPass newPass{
      .label = pass_label(g_recorder.inOffscreen ? "Offscreen" : "EFB"),
      .colorAttachments = prevPass.colorAttachments,
      .colorAttachmentCount = prevPass.colorAttachmentCount,
      .depthStencilView = prevPass.depthStencilView,
      .depthStencilFormat = prevPass.depthStencilFormat,
      .copySourceTexture = prevPass.copySourceTexture,
      .copySourceView = prevPass.copySourceView,
      .copySourceDepthView = prevPass.copySourceDepthView,
      .msaaSamples = msaaSamples,
      .clearDepthValue = clearDepthValue,
      .clearDepth = clearDepth,
      .hasDepth = prevPass.hasDepth,
      .hasStencil = prevPass.hasStencil,
      // Still the EFB after a copy: the stereo replay carries on into the eyes.
      .efb = prevPass.efb,
  };
  const bool fullColorClear = clearColor && clearAlpha;
  for (uint32_t i = 0; i < newPass.colorAttachmentCount; ++i) {
    auto& color = newPass.colorAttachments[i];
    color.loadOp = wgpu::LoadOp::Undefined;
    color.clear = false;
  }
  if (fullColorClear) {
    auto& sceneColor = newPass.colorAttachments[SceneColorAttachmentIndex];
    sceneColor.clear = true;
    sceneColor.clearValue = clearColorValue;
  }
  newPass.commands.reserve(2048);
  current_render_passes().emplace_back(std::move(newPass));
  ++g_recorder.currentRenderPass;

  if (!fullColorClear && (clearColor || clearAlpha)) {
    // If we're only clearing color _or_ alpha, perform a clear draw
    const auto targetLayout = current_render_passes()[g_recorder.currentRenderPass].target_layout();
    const auto clearConfig = clear::make_pipeline_config(targetLayout, clearColor, clearAlpha, false);
    push_draw_command(clear::DrawData{
        .pipeline = pipeline_ref(clearConfig),
        .color =
            wgpu::Color{
                .r = clearColorValue.x(),
                .g = clearColorValue.y(),
                .b = clearColorValue.z(),
                .a = clearColorValue.w(),
            },
        .multiviewPipeline = clear_multiview_pipeline_ref(clearConfig),
    });
  }
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_recorder.cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_recorder.cachedScissor});
}

void queue_palette_conv(tex_palette_conv::ConvRequest req) {
  auto& renderPass = current_render_passes()[g_recorder.currentRenderPass];
  AURORA_ASSERT(!renderPass.sealed, "Attempted to append palette conversion to sealed render pass {}",
                g_recorder.currentRenderPass);
  // Stereo replay: a palette read of a copy with eye stand-ins runs per eye too
  // (DolphinXR's layered palette conversion), so the visor's coloured image
  // stays per eye.
  std::array<tex_palette_conv::ConvRequest, 2> eyeConvs;
  if (!g_recorder.inOffscreen && stereo_shadow::palette_conv(req, eyeConvs)) {
    for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
      renderPass.stereo.paletteConvs[eye].push_back(std::move(eyeConvs[eye]));
    }
  }
  renderPass.paletteConvs.push_back(std::move(req));
}

bool is_offscreen() noexcept { return g_recorder.inOffscreen; }

uint32_t get_sample_count() noexcept {
  CHECK(g_recorder.currentRenderPass != UINT32_MAX, "get_sample_count called outside of a frame");
  return current_render_passes()[g_recorder.currentRenderPass].msaaSamples;
}

RenderTargetLayout get_render_target_layout() noexcept {
  CHECK(g_recorder.currentRenderPass != UINT32_MAX, "get_render_target_layout called outside of a frame");
  return current_render_passes()[g_recorder.currentRenderPass].target_layout();
}

void clear_caches() noexcept {
  g_offscreenCache.clear();
  clear_bind_group_cache();
}

bool push_custom_draw(DrawTypeId type, const void* payload, size_t payloadSize) {
  if (type == InvalidDrawType) {
    Log.warn("push_custom_draw: invalid draw type");
    return false;
  }
  if (payloadSize > InlineDrawPayloadSize) {
    Log.warn("push_custom_draw: payload size {} exceeds inline payload size {}", payloadSize, InlineDrawPayloadSize);
    return false;
  }
  if (payloadSize > 0 && payload == nullptr) {
    Log.warn("push_custom_draw: non-zero payload size with null payload");
    return false;
  }
  if (!find_runtime_draw_type(type)) {
    Log.warn("push_custom_draw: unregistered draw type {:#x}", type);
    return false;
  }

  gx::fifo::drain();

  if (!g_recorder.active() || g_recorder.currentRenderPass == UINT32_MAX) {
    Log.warn("push_custom_draw: called outside an active render pass");
    return false;
  }

  CustomDrawCommand draw{};
  draw.type = type;
  draw.payloadSize = static_cast<uint32_t>(payloadSize);
  if (payloadSize > 0) {
    std::memcpy(draw.payload.data(), payload, payloadSize);
  }
  push_command(CommandType::CustomDraw, Command::Data{.customDraw = draw});
  ++g_recorder.drawCallCount;
  return true;
}

void begin_offscreen(uint32_t width, uint32_t height) {
  ZoneScoped;
  AURORA_ASSERT(width != 0 && height != 0, "begin_offscreen requires nonzero dimensions ({}x{})", width, height);
  AURORA_ASSERT(g_recorder.active() && g_recorder.currentRenderPass != UINT32_MAX,
                "begin_offscreen called outside of an active recording frame");

  if (g_recorder.inOffscreen) {
    finish_current_offscreen();
  } else {
    suspend_efb();
  }
  start_offscreen(width, height);
}

void end_offscreen() {
  ZoneScoped;
  finish_current_offscreen();
  restore_efb();
}

bool create_pass(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) {
    Log.warn("create_pass: invalid size {}x{}", width, height);
    return false;
  }

  gx::fifo::drain();

  if (!g_recorder.active() || g_recorder.currentRenderPass == UINT32_MAX) {
    Log.warn("create_pass: called outside an active render pass");
    return false;
  }
  if (g_recorder.inOffscreen) {
    Log.warn("create_pass: an offscreen pass is already active (nesting is unsupported)");
    return false;
  }

  begin_offscreen(width, height);
  return true;
}

bool resolve_pass(const ResolveDesc& desc, ResolvedTargets& out) {
  out = {};
  gx::fifo::drain();

  if (!g_recorder.active() || g_recorder.currentRenderPass == UINT32_MAX) {
    Log.warn("resolve_pass: called outside an active render pass");
    return false;
  }

  bool wantDepth = desc.depth;
  if (wantDepth && !tex_copy_conv::snapshot_depth_supported()) {
    Log.warn("resolve_pass: depth snapshots are unsupported on this device");
    wantDepth = false;
  }

  auto& prevPass = current_render_passes()[g_recorder.currentRenderPass];
  const uint32_t width = prevPass.colorAttachments[SceneColorAttachmentIndex].size.width;
  const uint32_t height = prevPass.colorAttachments[SceneColorAttachmentIndex].size.height;
  // Requesting no snapshots is a plain pass break (or offscreen close, discarding its output).
  if (desc.color || wantDepth) {
    auto& entry = acquire_pass_snapshot(width, height, desc.color, wantDepth);
    if (desc.color) {
      prevPass.snapshotColorDst = entry.color.texture;
      out.color = entry.color.view;
      out.colorFormat = entry.color.format;
    }
    if (wantDepth) {
      prevPass.snapshotDepthDst = entry.depth.view;
      out.depth = entry.depth.view;
    }
  }
  out.width = width;
  out.height = height;

  if (g_recorder.inOffscreen) {
    // Seal the offscreen pass and resume the EFB.
    end_offscreen();
    return true;
  }

  // Seal the current EFB pass and continue on a new one that loads the existing contents.
  // EFB writes persist into the loading continuation, so content keeps the pass alive even
  // without a consumer.
  prevPass.discardable = !prevPass.has_consumer() && !prevPass.has_content();
  enqueue_pass(current_frame_packet(), g_recorder.currentRenderPass);
  resume_efb_pass_loading(prevPass);
  return true;
}

bool push_encoder_task(EncoderTaskId type, const void* payload, size_t payloadSize) {
  if (type == InvalidEncoderTask) {
    Log.warn("push_encoder_task: invalid encoder task type");
    return false;
  }
  if (payloadSize > InlineDrawPayloadSize) {
    Log.warn("push_encoder_task: payload size {} exceeds inline payload size {}", payloadSize, InlineDrawPayloadSize);
    return false;
  }
  if (payloadSize > 0 && payload == nullptr) {
    Log.warn("push_encoder_task: non-zero payload size with null payload");
    return false;
  }
  if (!find_runtime_encoder_task_type(type)) {
    Log.warn("push_encoder_task: unregistered encoder task type {:#x}", type);
    return false;
  }

  gx::fifo::drain();

  if (!g_recorder.active() || g_recorder.currentRenderPass == UINT32_MAX) {
    Log.warn("push_encoder_task: called outside an active render pass");
    return false;
  }
  if (g_recorder.inOffscreen) {
    Log.warn("push_encoder_task: unsupported while an offscreen pass is active");
    return false;
  }

  // Seal the current EFB pass, record the task between it and a continuation
  // pass that loads the existing contents. EFB writes persist into the continuation, so
  // content keeps the sealed pass alive even without a consumer.
  auto& frame = current_frame_packet();
  auto& prevPass = current_render_passes()[g_recorder.currentRenderPass];
  prevPass.discardable = !prevPass.has_consumer() && !prevPass.has_content();
  enqueue_pass(frame, g_recorder.currentRenderPass);

  const auto taskIndex = static_cast<uint32_t>(frame.encoderTasks.size());
  auto& task = frame.encoderTasks.emplace_back(EncoderTask{.type = type});
  task.payloadSize = static_cast<uint32_t>(payloadSize);
  if (payloadSize > 0) {
    std::memcpy(task.payload.data(), payload, payloadSize);
  }
  const auto opIndex = static_cast<uint32_t>(frame.ops.size());
  frame.ops.emplace_back(capture_frame_op(frame, FrameOpType::EncoderTask, taskIndex));
  enqueue_op(frame, opIndex);

  resume_efb_pass_loading(prevPass);
  return true;
}

template <>
void push_draw_command(gx::DrawData data) {
  push_draw_command(make_draw_command<gx::render>(data));
}

#ifdef AURORA_ENABLE_RMLUI
template <>
void push_draw_command(rmlui::DrawData data) {
  push_draw_command(make_draw_command<rmlui::render>(data));
}
#endif

template <>
PipelineRef pipeline_ref(const gx::PipelineConfig& config) {
  return find_pipeline(ShaderType::GX, config, [=] { return create_pipeline(config); });
}

#ifdef AURORA_ENABLE_RMLUI
template <>
PipelineRef pipeline_ref(const rmlui::PipelineConfig& config) {
  return find_pipeline(ShaderType::Rml, config, [=] { return rmlui::create_pipeline(config); });
}
#endif

void finish() {
  ZoneScoped;
  // One frame's worth: set again before every finish() (aurora.cpp end_frame).
  const bool finalPassMonoUnneeded = std::exchange(g_recorder.finalPassMonoUnneeded, false);
  if (!g_recorder.active()) {
    return;
  }
  AURORA_ASSERT(!g_recorder.inOffscreen, "finish called while offscreen rendering is active");
  if (g_recorder.currentRenderPass != UINT32_MAX) {
    auto& frame = current_frame_packet();
    // A multiview draw's binding spans an eye pair (stereo_multiview.hpp).
    // The eye clip block's bind group spans two uniforms past any offset (frame.cpp).
    frame.uniforms.append_zeroes(2 * gx::MaxUniformSize);
    auto& pass = frame.renderPasses[g_recorder.currentRenderPass];
    pass.captureDepthSnapshot = true;
    // Honoured only if sealing gives the pass its eye passes (encoding.cpp).
    pass.stereo.skipMono = finalPassMonoUnneeded;
    enqueue_pass(frame, g_recorder.currentRenderPass);
    if (g_stereoDiagnostics.load(std::memory_order_relaxed)) {
      log_stereo_frame_stats(frame);
    }
    g_recorder.currentRenderPass = UINT32_MAX;
  }
}

Range push_verts(const uint8_t* data, size_t length, size_t alignment) {
  ZoneScoped;
  if (!check_recording("push_verts")) {
    return {};
  }
  return push(current_frame_packet().verts, data, length, alignment);
}

Range push_indices(const uint8_t* data, size_t length, size_t alignment) {
  ZoneScoped;
  if (!check_recording("push_indices")) {
    return {};
  }
  return push(current_frame_packet().indices, data, length, alignment);
}

ByteBuffer* staging_verts() noexcept { return g_recorder.active() ? &current_frame_packet().verts : nullptr; }

void queue_geometry_upload(uint32_t offset, const uint8_t* data, uint32_t size) {
  if (!check_recording("queue_geometry_upload") || size == 0) {
    return;
  }
  // Through this frame's vertex staging: the frame worker copies it on to the
  // geometry buffer before the frame's next op (encoding.cpp).
  auto& frame = current_frame_packet();
  const Range range = push(frame.verts, data, size, 4);
  frame.geometryUploads.push_back(GeometryUpload{.src = range.offset, .dst = offset, .size = size});
}

ByteBuffer* staging_indices() noexcept { return g_recorder.active() ? &current_frame_packet().indices : nullptr; }

Range push_uniform(const uint8_t* data, size_t length) {
  ZoneScoped;
  if (!check_recording("push_uniform")) {
    return {};
  }
  return push(current_frame_packet().uniforms, data, length, resources().limits.minUniformBufferOffsetAlignment);
}

Range push_storage(const uint8_t* data, size_t length) {
  ZoneScoped;
  if (!check_recording("push_storage")) {
    return {};
  }
  return push(current_frame_packet().storage, data, length, resources().limits.minStorageBufferOffsetAlignment);
}

Range push_texture_data(const uint8_t* data, u32 bytesPerRow, u32 rowsPerImage) {
  // For CopyBufferToTexture, we need an alignment of 256 per row (see Dawn kTextureBytesPerRowAlignment)
  const auto copyBytesPerRow = AURORA_ALIGN(bytesPerRow, 256);
  const auto range = map(current_frame_packet().textureUpload, copyBytesPerRow * rowsPerImage, 0);
  u8* dst = current_frame_packet().textureUpload.data() + range.offset;
  for (u32 i = 0; i < rowsPerImage; ++i) {
    memcpy(dst, data, bytesPerRow);
    data += bytesPerRow;
    dst += copyBytesPerRow;
  }
  return range;
}

uint32_t align_uniform(uint32_t value) {
  return AURORA_ALIGN(value, detail::resources().limits.minUniformBufferOffsetAlignment);
}

void insert_debug_marker(std::string label) {
#if defined(AURORA_GFX_DEBUG_GROUPS)
  auto& markers = current_frame_packet().debugMarkers;
  const auto idx = markers.size();
  markers.emplace_back(std::move(label));
  push_command(CommandType::DebugMarker, {.debugMarkerIndex = idx});
#endif
}

void push_debug_group(std::string label) {
#if defined(AURORA_GFX_DEBUG_GROUPS)
  g_recorder.debugGroupStack.push_back(std::move(label));
#endif
}
} // namespace aurora::gfx

void push_debug_group(const char* label) {
#ifdef AURORA_GFX_DEBUG_GROUPS
  aurora::gfx::g_recorder.debugGroupStack.emplace_back(label);
#endif
}
void pop_debug_group() {
#ifdef AURORA_GFX_DEBUG_GROUPS
  if (aurora::gfx::g_recorder.debugGroupStack.empty()) {
    aurora::gfx::Log.error("Debug group stack underflowed!");
    return;
  }

  aurora::gfx::g_recorder.debugGroupStack.pop_back();
#endif
}

#include "shadow.hpp"

#include "../gx/pipeline.hpp"
#include "../logging.hpp"
#include "../webgpu/gpu.hpp"
#include "../webgpu/gpu_prof.hpp"
#include "geometry_buffer.hpp"
#include "pipeline_cache.hpp"
#include "recording.hpp"
#include "resources.hpp"

#include <aurora/gfx.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

// The sun's shadow map. set_frame fits an orthographic box around the camera, its texels snapped
// to the light's axes so the map does not swim as the camera moves. The casters (the frame's
// opaque world draws, re-drawn depth only with a vs_shadow pipeline) are drawn into it once the
// frame's scene is, and the next frame's receivers read it through the matrix of the frame it was
// drawn in, so a receiver lags the camera by one frame but never reads a map half drawn.
namespace aurora::gfx::shadow {
namespace {
Module Log("aurora::gfx::shadow");
using webgpu::g_device;

struct Matrix {
  float m[4][4];
};

struct State {
  EncoderTaskId task = InvalidEncoderTask;
  uint32_t size = 0;
  wgpu::Texture map;
  wgpu::TextureView mapView;
  wgpu::Sampler sampler;
  // The light clip space (world -> map) of the frame being drawn, and of the map drawn last.
  Matrix pendingClip{};
  bool pendingActive = false;
  Matrix mapClip{};
  bool mapValid = false;
};

State g_state;
std::vector<gx::DrawData> g_casters;
std::array<std::vector<gx::DrawData>, 8> g_recorded;
uint32_t g_nextSlot = 0;
// Read by the console from the main thread.
std::atomic<uint32_t> g_lastCasterCount{0};

uint32_t map_size() {
  static const uint32_t size = [] {
    const char* env = std::getenv("MP_SHADOW_SIZE");
    const long value = env != nullptr ? std::strtol(env, nullptr, 10) : 0;
    return value >= 256 && value <= 8192 ? static_cast<uint32_t>(value) : 2048u;
  }();
  return size;
}

void ensure_map() {
  if (g_state.map) {
    return;
  }
  g_state.size = map_size();
  const wgpu::TextureDescriptor descriptor{
      .label = "Shadow Map",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
      .dimension = wgpu::TextureDimension::e2D,
      .size = {g_state.size, g_state.size, 1},
      .format = MapFormat,
  };
  g_state.map = g_device.CreateTexture(&descriptor);
  g_state.mapView = g_state.map.CreateView();
}

Matrix multiply(const Matrix& a, const Matrix& b) {
  Matrix out{};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      float sum = 0.f;
      for (int k = 0; k < 4; ++k) {
        sum += a.m[r][k] * b.m[k][c];
      }
      out.m[r][c] = sum;
    }
  }
  return out;
}

void normalize(float v[3]) {
  const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (len > 0.f) {
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
  }
}

void cross(const float a[3], const float b[3], float out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

// The map's box: its axes (right, up, and d the way the light travels) and its centre on them.
struct Box {
  float right[3];
  float up[3];
  float d[3];
  float cx, cy, cz;
};

} // namespace

// The box is centred half its radius ahead of the camera (GX looks down -z): the eye is -R^T t,
// and the view's z row points back at it.
void box_center(const float worldToView[3][4], float radius, float center[3]) {
  for (int i = 0; i < 3; ++i) {
    const float eye = -(worldToView[0][i] * worldToView[0][3] + worldToView[1][i] * worldToView[1][3] +
                        worldToView[2][i] * worldToView[2][3]);
    center[i] = eye - worldToView[2][i] * radius * 0.5f;
  }
}

namespace {

bool light_box(const float worldToView[3][4], const float sunDir[3], float radius, Box& box) {
  float* d = box.d;
  d[0] = sunDir[0], d[1] = sunDir[1], d[2] = sunDir[2];
  normalize(d);
  if (!(radius > 0.f) || (d[0] == 0.f && d[1] == 0.f && d[2] == 0.f)) {
    return false;
  }
  float center[3];
  box_center(worldToView, radius, center);
  const float upRef[3] = {std::fabs(d[2]) < 0.9f ? 0.f : 1.f, 0.f, std::fabs(d[2]) < 0.9f ? 1.f : 0.f};
  cross(upRef, d, box.right);
  normalize(box.right);
  cross(d, box.right, box.up);
  box.cx = box.right[0] * center[0] + box.right[1] * center[1] + box.right[2] * center[2];
  box.cy = box.up[0] * center[0] + box.up[1] * center[1] + box.up[2] * center[2];
  box.cz = d[0] * center[0] + d[1] * center[1] + d[2] * center[2];
  return true;
}

// Casters up to four radii toward the sun still shadow the box.
constexpr float BackRadii = 4.f;

void encode(const EncoderTaskContext&, const wgpu::CommandEncoder& cmd, const void* payload, size_t payloadSize,
            void*) {
  uint32_t slot = 0;
  if (payloadSize != sizeof(slot) || !g_state.mapView) {
    return;
  }
  std::memcpy(&slot, payload, sizeof(slot));
  const auto& casters = g_recorded[slot % g_recorded.size()];
  const wgpu::RenderPassDepthStencilAttachment depth{
      .view = g_state.mapView,
      .depthLoadOp = wgpu::LoadOp::Clear,
      .depthStoreOp = wgpu::StoreOp::Store,
      .depthClearValue = 1.f,
  };
  const wgpu::RenderPassDescriptor passDescriptor{
      .label = "Shadow Map",
      .depthStencilAttachment = &depth,
      .timestampWrites = webgpu::gpu_prof::pass_writes("Shadow map"),
  };
  const auto pass = cmd.BeginRenderPass(&passDescriptor);
  const auto& resources = detail::resources();
  // PrimedGun: a caster from the geometry cache (DrawData::cachedGeometry) reads that cache's buffer
  // as group 0, with 32-bit indices absolute into it, and with native vertex input
  // (DrawData::nativeVertices) as vertex buffer 0 as well, bound as gx::render binds them. Group 0
  // and the vertex buffer are set again only when they change: every caster shares the pipeline layout.
  int geometry = -1; // group 0: the frame's vertex buffer (0), the geometry cache's (1)
  bool nativeBound = false;
  for (const auto& draw : casters) {
    wgpu::RenderPipeline pipeline;
    if (!get_pipeline(draw.pipeline, pipeline)) {
      continue;
    }
    pass.SetPipeline(pipeline);
    pass.SetImmediates(0, &draw.immediateData, sizeof(draw.immediateData));
    pass.SetBindGroup(1, resources.uniformBindGroup, 1, &draw.uniformRange.offset);
    if (const int wanted = draw.cachedGeometry ? 1 : 0; wanted != geometry) {
      pass.SetBindGroup(0, draw.cachedGeometry ? detail::geometry_bind_group() : resources.staticBindGroup);
      geometry = wanted;
    }
    if (draw.nativeVertices && !nativeBound) {
      pass.SetVertexBuffer(0, detail::geometry_buffer());
      nativeBound = true;
    }
    pass.SetIndexBuffer(resources.indexBuffer,
                        draw.cachedGeometry ? wgpu::IndexFormat::Uint32 : wgpu::IndexFormat::Uint16,
                        draw.idxRange.offset, draw.idxRange.size);
    if (draw.indexCount == 0) {
      pass.Draw(draw.vtxCount, draw.instanceCount);
    } else {
      pass.DrawIndexed(draw.indexCount, draw.instanceCount);
    }
  }
  pass.End();
}
} // namespace

bool ensure_task() {
  if (g_state.task == InvalidEncoderTask) {
    g_state.task = register_encoder_task_type(EncoderTaskDescriptor{.label = "Shadow Map", .callback = encode});
    if (g_state.task == InvalidEncoderTask) {
      Log.warn("could not register the shadow map task");
      return false;
    }
  }
  return true;
}

bool set_frame(const float worldToView[3][4], const float sunDir[3], float radius, const float color[3],
               Uniform& out) {
  // A frame's casters are the draws since its set_frame; a frame that drew no map drops them.
  g_casters.clear();
  Box box;
  if (!light_box(worldToView, sunDir, radius, box)) {
    g_state.pendingActive = false;
    g_state.mapValid = false;
    return false;
  }
  ensure_map();
  const float* d = box.d;

  // view -> world: the rotation's transpose, and -R^T t.
  Matrix viewToWorld{};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      viewToWorld.m[r][c] = worldToView[c][r];
    }
    viewToWorld.m[r][3] =
        -(worldToView[0][r] * worldToView[0][3] + worldToView[1][r] * worldToView[1][3] +
          worldToView[2][r] * worldToView[2][3]);
  }
  viewToWorld.m[3][3] = 1.f;

  const float* right = box.right;
  const float* up = box.up;
  const float texel = 2.f * radius / static_cast<float>(g_state.size);
  const float cx = std::floor(box.cx / texel) * texel;
  const float cy = std::floor(box.cy / texel) * texel;
  const float cz = box.cz;
  const float back = BackRadii * radius;
  const float depthScale = 1.f / (back + radius);
  Matrix lightClip{};
  for (int i = 0; i < 3; ++i) {
    lightClip.m[0][i] = right[i] / radius;
    lightClip.m[1][i] = up[i] / radius;
    lightClip.m[2][i] = d[i] * depthScale;
  }
  lightClip.m[0][3] = -cx / radius;
  lightClip.m[1][3] = -cy / radius;
  lightClip.m[2][3] = (back - cz) * depthScale;
  lightClip.m[3][3] = 1.f;

  const Matrix caster = multiply(lightClip, viewToWorld);
  Matrix receiver{};
  if (g_state.mapValid) {
    receiver = multiply(g_state.mapClip, viewToWorld);
  } else {
    // No map yet: every receiver lands outside it and is lit.
    receiver.m[0][3] = 10.f;
    receiver.m[3][3] = 1.f;
  }
  g_state.pendingClip = lightClip;
  g_state.pendingActive = true;

  std::memcpy(out.caster, caster.m, sizeof(out.caster));
  std::memcpy(out.receiver, receiver.m, sizeof(out.receiver));
  // The direction to the sun in view space.
  for (int r = 0; r < 3; ++r) {
    out.dir[r] = -(worldToView[r][0] * d[0] + worldToView[r][1] * d[1] + worldToView[r][2] * d[2]);
  }
  out.dir[3] = 1.5f * texel;
  out.color[0] = std::max(color[0], 0.f);
  out.color[1] = std::max(color[1], 0.f);
  out.color[2] = std::max(color[2], 0.f);
  out.color[3] = 1.f / static_cast<float>(g_state.size);
  return true;
}

bool box_casts(const float worldToView[3][4], const float sunDir[3], float radius, const float min[3],
               const float max[3]) {
  Box box;
  if (!light_box(worldToView, sunDir, radius, box)) {
    return false;
  }
  // The world box against the map's on each of the map's axes, with a texel to spare for the snap.
  const auto overlaps = [&](const float axis[3], float lo, float hi) {
    float mid = 0.f;
    float half = 0.f;
    for (int i = 0; i < 3; ++i) {
      mid += axis[i] * (min[i] + max[i]) * 0.5f;
      half += std::fabs(axis[i]) * (max[i] - min[i]) * 0.5f;
    }
    return mid + half >= lo && mid - half <= hi;
  };
  // Any distance toward the sun casts: vs_shadow pancakes it onto the map's near plane.
  const float margin = radius * (1.f + 2.f / static_cast<float>(map_size()));
  return overlaps(box.right, box.cx - margin, box.cx + margin) && overlaps(box.up, box.cy - margin, box.cy + margin) &&
         overlaps(box.d, -std::numeric_limits<float>::infinity(), box.cz + radius);
}

void add_caster(const gx::DrawData& draw) {
  if (g_state.pendingActive) {
    g_casters.push_back(draw);
  }
}

uint32_t last_caster_count() { return g_lastCasterCount.load(std::memory_order_relaxed); }

bool record() {
  if (g_state.task == InvalidEncoderTask || !g_state.pendingActive || !g_state.mapView) {
    g_casters.clear();
    return false;
  }
  const uint32_t slot = g_nextSlot % g_recorded.size();
  g_recorded[slot].swap(g_casters);
  g_casters.clear();
  g_state.pendingActive = false;
  // Outside a pass the task isn't recorded: the old map stays, so keep its clip with it.
  if (!record_encoder_task(g_state.task, &slot, sizeof(slot))) {
    g_recorded[slot].clear();
    return false;
  }
  ++g_nextSlot;
  g_lastCasterCount.store(static_cast<uint32_t>(g_recorded[slot].size()), std::memory_order_relaxed);
  g_state.mapClip = g_state.pendingClip;
  g_state.mapValid = true;
  return true;
}

const wgpu::TextureView& map_view() {
  ensure_map();
  return g_state.mapView;
}

const wgpu::Sampler& sampler() {
  if (!g_state.sampler) {
    const wgpu::SamplerDescriptor descriptor{
        .label = "Shadow Map Sampler",
        .addressModeU = wgpu::AddressMode::ClampToEdge,
        .addressModeV = wgpu::AddressMode::ClampToEdge,
        .addressModeW = wgpu::AddressMode::ClampToEdge,
        .magFilter = wgpu::FilterMode::Linear,
        .minFilter = wgpu::FilterMode::Linear,
        .compare = wgpu::CompareFunction::Less,
    };
    g_state.sampler = g_device.CreateSampler(&descriptor);
  }
  return g_state.sampler;
}

void shutdown() {
  const auto task = g_state.task;
  g_state = {};
  g_casters = {};
  g_recorded = {};
  if (task != InvalidEncoderTask) {
    unregister_encoder_task_type(task);
  }
}
} // namespace aurora::gfx::shadow

#include "command_processor.hpp"
#include "../gfx/perf_counters.hpp"
#include "../gfx/frame.hpp"
#include "../gfx/hash.hpp"
#include "../gfx/resource_cache.hpp"

#include <absl/container/flat_hash_map.h>

#include <atomic>

#include <cstring>

#include "../gfx/depth_peek.hpp"
#include "../gfx/probe.hpp"
#include "../gfx/recording.hpp"
#include "../gfx/stereo_shadow.hpp"
#include "../internal.hpp"
#include "dolphin/gd/GDGeometry.h"
#include "dolphin/gx/GXAurora.h"
#include "gx.hpp"
#include "pipeline.hpp"
#include "regs.hpp"
#include "shader_info.hpp"
#include "texture.hpp"

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <memory>
#include <vector>

namespace aurora::gx::fifo {
namespace {
constexpr Module Log{"aurora::gx::fifo"};

// The triangle-list indices a primitive takes: its vertices as GX_TRIANGLES
// for a strip, fan or quads, and those indices written out.
u32 list_index_count(GXPrimitive prim, u16 vtxCount) noexcept {
  switch (prim) {
  case GX_QUADS:
    return (vtxCount / 4u) * 6u;
  case GX_TRIANGLES:
    return vtxCount;
  case GX_TRIANGLEFAN:
  case GX_TRIANGLESTRIP:
    return vtxCount < 3 ? vtxCount : (vtxCount - 3u) * 3u + 3u;
  case GX_LINES:
  case GX_LINESTRIP:
  case GX_POINTS:
    return 6;
  default:
    UNLIKELY FATAL("unsupported primitive type {}", static_cast<u32>(prim));
  }
  return 0;
}

void write_list_indices(u16* out, GXPrimitive prim, u16 vtxStart, u16 vtxCount) noexcept {
  switch (prim) {
  case GX_QUADS:
    for (u32 v = 0; v + 4 <= vtxCount; v += 4) {
      const u16 idx0 = static_cast<u16>(vtxStart + v);
      *out++ = idx0;
      *out++ = static_cast<u16>(idx0 + 1);
      *out++ = static_cast<u16>(idx0 + 2);
      *out++ = static_cast<u16>(idx0 + 2);
      *out++ = static_cast<u16>(idx0 + 3);
      *out++ = idx0;
    }
    break;
  case GX_TRIANGLES:
    for (u32 v = 0; v < vtxCount; ++v) {
      *out++ = static_cast<u16>(vtxStart + v);
    }
    break;
  case GX_TRIANGLEFAN:
    for (u32 v = 0; v < vtxCount; ++v) {
      const u16 idx = static_cast<u16>(vtxStart + v);
      if (v < 3) {
        *out++ = idx;
        continue;
      }
      *out++ = vtxStart;
      *out++ = static_cast<u16>(idx - 1);
      *out++ = idx;
    }
    break;
  case GX_TRIANGLESTRIP:
    for (u32 v = 0; v < vtxCount; ++v) {
      const u16 idx = static_cast<u16>(vtxStart + v);
      if (v < 3) {
        *out++ = idx;
        continue;
      }
      if ((v & 1) == 0) {
        *out++ = static_cast<u16>(idx - 2);
        *out++ = static_cast<u16>(idx - 1);
      } else {
        *out++ = static_cast<u16>(idx - 1);
        *out++ = static_cast<u16>(idx - 2);
      }
      *out++ = idx;
    }
    break;
  case GX_LINES:
  case GX_LINESTRIP:
  case GX_POINTS:
    out[0] = 0;
    out[1] = 1;
    out[2] = 3;
    out[3] = 3;
    out[4] = 2;
    out[5] = 0;
    break;
  default:
    break;
  }
}

u32 prepare_idx_buffer(ByteBuffer& buf, GXPrimitive prim, u16 vtxStart, u16 vtxCount) noexcept {
  const u32 count = list_index_count(prim, vtxCount);
  write_list_indices(reinterpret_cast<u16*>(buf.append_uninitialized(count * sizeof(u16))), prim, vtxStart, vtxCount);
  return count;
}

// GX FIFO opcodes - use CP_ prefix to avoid clashing with GXCommandList.h macros
constexpr u8 CP_CMD_NOP = GX_NOP;
constexpr u8 CP_CMD_LOAD_CP_REG = GX_LOAD_CP_REG;
constexpr u8 CP_CMD_LOAD_XF_REG = GX_LOAD_XF_REG;
constexpr u8 CP_CMD_LOAD_INDX_A = GX_LOAD_INDX_A;
constexpr u8 CP_CMD_LOAD_INDX_B = GX_LOAD_INDX_B;
constexpr u8 CP_CMD_LOAD_INDX_C = GX_LOAD_INDX_C;
constexpr u8 CP_CMD_LOAD_INDX_D = GX_LOAD_INDX_D;
constexpr u8 CP_CMD_CALL_DL = GX_CMD_CALL_DL;
constexpr u8 CP_CMD_INVAL_VTX = GX_CMD_INVL_VC;
constexpr u8 CP_CMD_LOAD_BP_REG = GX_LOAD_BP_REG & GX_OPCODE_MASK;

// Primitive type mask
constexpr u8 CP_OPCODE_MASK = GX_OPCODE_MASK;
constexpr u8 CP_VAT_MASK = GX_VAT_MASK;

struct FogRangeLutKey {
  std::array<u16, 10> rangeK;
  f32 rangeCenter;
  f32 renderWidth;
  u32 targetWidth;

  bool operator==(const FogRangeLutKey&) const = default;
};

struct FogRangeLutEntry {
  FogRangeLutKey key;
  std::vector<f32> factors;
};

constexpr size_t MaxFogRangeLuts = 32;
std::vector<FogRangeLutEntry> sFogRangeLuts;

struct DrawCache {
  PipelineConfig config{};
  ShaderInfo shaderInfo{};
  gfx::PipelineRef pipelineRef{};
  GXBindGroups bindGroups{};
  uint64_t bindGeneration = 0;
  GXVtxFmt fmt = GX_MAX_VTXFMT;
  u8 lineMode = 0;
  bool hasPipeline = false;
  gfx::Range uniformRange{};
  ByteBuffer uniformBytes;
  u8 uniformRoute = 0;
  gfx::stereo_replay::HeadLockedPlane uniformPlane{};
  gfx::StereoScreenTexMtx uniformScreenTexMtx{};
  std::array<uint32_t, 2> stereoUniformOffsets{UINT32_MAX, UINT32_MAX};
  std::array<gfx::BindGroupRef, 2> stereoBindGroups{};
  uint64_t stereoEpoch = 0;
  // Multiview stereo replay (gfx/stereo_multiview.hpp): the pipelines drawing both
  // views, one per MultiviewMode, made for the current config when a multiview
  // frame first needs one (bit `mode` of the mask); the mode the staged uniform
  // needs (gfx::stereo_multiview_mode); the stereo bind groups are then
  // [multiview bind group, 0].
  std::array<gfx::PipelineRef, 4> multiviewPipelineRefs{};
  u8 multiviewPipelineMask = 0;
  u8 multiviewMode = MultiviewNone;
  bool stereoBindGroupsMultiview = false;
  gfx::Range fogRange{};
  FogRangeLutKey fogRangeKey{};
  bool hasFogRange = false;
  GXVtxFmt lastDrawFmt = GX_MAX_VTXFMT;
};
DrawCache sDrawCache;

// Indexed vertex attributes resolved on the CPU (GXState::deindexVertices): the
// plan for the current vertex format and arrays, mirroring populate_pipeline_config's
// resolved layout, and the FIFO records rewritten with the array elements inline.
std::atomic<bool> sDeindexRequested{false};
struct DeindexAttr {
  const u8* data = nullptr; // the array; null when none is set
  u32 size = 0;
  u32 stride = 0;
  u32 base = 0;      // GX_AURORA_LOAD_ARRAY_BASE_INDEX
  u16 srcOffset = 0; // of the index, or the direct data, in the FIFO record
  u16 dstOffset = 0; // in the resolved record
  u8 bytes = 0;      // per index: the element, or one slice of it (NBT3)
  u8 indexSize = 0;  // 0: direct data, copied as it is
  u8 indices = 1;    // 3 for GX_NRM_NBT3, one per slice
};
struct DeindexPlan {
  GXVtxFmt fmt = GX_MAX_VTXFMT;
  u32 srcStride = 0;
  u32 dstStride = 0;
  bool active = false; // any indexed attribute
  u32 count = 0;
  std::array<DeindexAttr, GX_VA_TEX7 + 1> attrs{};
};
DeindexPlan sDeindexPlan;
u32 sDeindexOutOfRange = 0;

// Rebuilt after any state change (the dirty bits clear with the first draw, so
// the strips that follow reuse it).
const DeindexPlan& deindex_plan(GXVtxFmt fmt) noexcept {
  auto& plan = sDeindexPlan;
  if (plan.fmt == fmt && g_gxState.dirty == 0) {
    return plan;
  }
  plan.fmt = fmt;
  plan.count = 0;
  plan.active = false;
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  // gx.hpp deindexed_attr_align: the layout populate_pipeline_config describes
  const bool aligned = deindexed_layout();
  u32 src = 0;
  u32 dst = 0;
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    const auto type = g_gxState.vtxDesc[i];
    if (type == GX_NONE) {
      continue;
    }
    if (aligned) {
      dst = AURORA_ALIGN(dst, deindexed_attr_align(attr));
    }
    const auto& attrFmt = vtxFmt.attrs[i];
    const u32 compSize = comp_type_size(attr, attrFmt.type);
    const u32 cnt = comp_cnt_count(attr, attrFmt.cnt);
    const bool nbt3 = attr == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3;
    auto& a = plan.attrs[plan.count++];
    a = {};
    a.srcOffset = static_cast<u16>(src);
    a.dstOffset = static_cast<u16>(dst);
    if (type == GX_DIRECT) {
      a.bytes = static_cast<u8>(compSize * cnt);
      src += a.bytes;
      dst += a.bytes;
      continue;
    }
    const auto& array = g_gxState.arrays[i];
    a.data = static_cast<const u8*>(array.data);
    a.size = array.size;
    a.stride = array.stride;
    a.base = array.baseIndex;
    a.indexSize = type == GX_INDEX8 ? 1 : 2;
    a.indices = nbt3 ? 3 : 1;
    a.bytes = static_cast<u8>(compSize * cnt / a.indices);
    plan.active = true;
    src += a.indexSize * a.indices;
    dst += compSize * cnt;
  }
  plan.srcStride = src;
  plan.dstStride = aligned ? AURORA_ALIGN(dst, 4u) : dst;
  return plan;
}

// One attribute of `count` records: `Bytes` per element (0: a.bytes at run time),
// `IndexSize` bytes per big-endian index (0: direct data, copied).
template <u32 Bytes, u32 IndexSize>
void deindex_attr(const DeindexAttr& a, const u8* src, u32 srcStride, u32 count, u8* dst, u32 dstStride) noexcept {
  const u32 bytes = Bytes != 0 ? Bytes : a.bytes;
  src += a.srcOffset;
  dst += a.dstOffset;
  for (u32 v = 0; v < count; ++v, src += srcStride, dst += dstStride) {
    if constexpr (IndexSize == 0) {
      std::memcpy(dst, src, bytes);
    } else {
      const u32 index = IndexSize == 1 ? src[0] : (static_cast<u32>(src[0]) << 8) | src[1];
      const size_t offset = static_cast<size_t>(a.base + index) * a.stride;
      if (offset + bytes <= a.size) [[likely]] {
        std::memcpy(dst, a.data + offset, bytes);
      } else {
        // The GPU path would read whatever follows the array
        std::memset(dst, 0, bytes);
        ++sDeindexOutOfRange;
      }
    }
  }
}

template <u32 IndexSize>
void deindex_attr_sized(const DeindexAttr& a, const u8* src, u32 srcStride, u32 count, u8* dst,
                        u32 dstStride) noexcept {
  switch (a.bytes) {
  case 1:
    return deindex_attr<1, IndexSize>(a, src, srcStride, count, dst, dstStride);
  case 2:
    return deindex_attr<2, IndexSize>(a, src, srcStride, count, dst, dstStride);
  case 4:
    return deindex_attr<4, IndexSize>(a, src, srcStride, count, dst, dstStride);
  case 6:
    return deindex_attr<6, IndexSize>(a, src, srcStride, count, dst, dstStride);
  case 8:
    return deindex_attr<8, IndexSize>(a, src, srcStride, count, dst, dstStride);
  case 12:
    return deindex_attr<12, IndexSize>(a, src, srcStride, count, dst, dstStride);
  default:
    return deindex_attr<0, IndexSize>(a, src, srcStride, count, dst, dstStride);
  }
}

// `count` FIFO records at `src` resolved into records at `dst`, attribute by
// attribute. An index past its array reads as zeros.
void deindex_records(const DeindexPlan& plan, const u8* src, u32 count, u8* dst) noexcept {
  for (u32 k = 0; k < plan.count; ++k) {
    const auto& a = plan.attrs[k];
    if (a.indices != 1) {
      // GX_NRM_NBT3: three indices, one per 3-vector slice
      for (u32 s = 0; s < a.indices; ++s) {
        DeindexAttr slice = a;
        slice.srcOffset = static_cast<u16>(a.srcOffset + s * a.indexSize);
        slice.dstOffset = static_cast<u16>(a.dstOffset + s * a.bytes);
        slice.base = a.base;
        // The slice's element starts s slices into the array element
        slice.data = a.data != nullptr ? a.data + s * a.bytes : nullptr;
        slice.size = a.size > s * a.bytes ? a.size - s * a.bytes : 0;
        slice.indices = 1;
        if (a.indexSize == 1) {
          deindex_attr_sized<1>(slice, src, plan.srcStride, count, dst, plan.dstStride);
        } else {
          deindex_attr_sized<2>(slice, src, plan.srcStride, count, dst, plan.dstStride);
        }
      }
      continue;
    }
    switch (a.indexSize) {
    case 0:
      deindex_attr_sized<0>(a, src, plan.srcStride, count, dst, plan.dstStride);
      break;
    case 1:
      deindex_attr_sized<1>(a, src, plan.srcStride, count, dst, plan.dstStride);
      break;
    default:
      deindex_attr_sized<2>(a, src, plan.srcStride, count, dst, plan.dstStride);
      break;
    }
  }
}

// The records of a draw staged: resolved when the plan asks, else as they are.
gfx::Range push_vertex_records(const DeindexPlan* plan, std::span<const u8> data, u32 vtxCount,
                               size_t alignment) noexcept {
  if (plan == nullptr || !plan->active) {
    return gfx::push_verts(data.data(), data.size(), alignment);
  }
  static ByteBuffer resolved;
  resolved.clear();
  const size_t bytes = static_cast<size_t>(vtxCount) * plan->dstStride;
  {
    const gfx::perf::Bucket bucket{gfx::perf::g_fifoDeindexTicks, gfx::perf::enabled()};
    deindex_records(*plan, data.data(), vtxCount, resolved.append_uninitialized(bytes));
    gfx::perf::g_fifoDeindexedVerts.fetch_add(vtxCount, std::memory_order_relaxed);
  }
  return gfx::push_verts(resolved.data(), resolved.size(), alignment);
}

// The texture bind groups by what build_texture_bind_group reads. Prime binds a
// few hundred texture sets a frame, each for a run of draws, so the descriptor
// hashing and sampler lookups behind build_bind_groups only run for a set the
// cache has not seen; direct-mapped by the key's hash, the whole key compared.
struct BindGroupKey {
  struct Slot {
    const void* view = nullptr; // the texture's sample view
    u32 mode0 = 0;              // wrap, filters, LOD bias, anisotropy
    u32 mode1 = 0;              // LOD clamps
    u32 flags = 0;              // replacement, arbitrary mips
    u32 reserved = 0;
  };
  std::array<Slot, MaxTextures> slots{};
  u32 sampled = 0;
  u32 pbr = 0;
  // The PBR probes by id, under the generation of their creations and
  // destructions (an id's views are fixed in between).
  u32 pbrCube = 0;
  u32 pbrVolume = 0;
  u32 probeGeneration = 0;
  u32 reserved = 0;
};
static_assert(std::has_unique_object_representations_v<BindGroupKey>);
u32 sProbeGeneration = 0;

struct BindGroupCacheEntry {
  BindGroupKey key{};
  GXBindGroups mono{};
  gfx::BindGroupRef multiview{};
  uint64_t multiviewEpoch = 0;
  uint32_t touchedFrame = UINT32_MAX;
  bool monoValid = false;
  bool multiviewValid = false;
};
// By the key's hash; the entries nothing used for a while are dropped now and then.
absl::flat_hash_map<uint64_t, BindGroupCacheEntry> sBindGroupCache;
constexpr uint32_t BindGroupCacheRetainFrames = 64;
uint32_t sBindGroupCachePrunedFrame = 0;

BindGroupKey bind_group_key(const ShaderInfo& info) noexcept {
  BindGroupKey key;
  std::memset(&key, 0, sizeof(key));
  for (u32 i = 0; i < MaxTextures; ++i) {
    const auto& tex = g_gxState.textures[i];
    if (!tex || !(info.sampledTextures[i] || info.sampledIndTextures[i])) {
      continue;
    }
    key.sampled |= 1u << i;
    auto& slot = key.slots[i];
    slot.view = tex.ref->sampleTextureView.Get();
    slot.mode0 = tex.texObj.mode0;
    slot.mode1 = tex.texObj.mode1;
    slot.flags = (tex.ref->isReplacement ? 1u : 0u) | (tex.ref->hasArbitraryMips ? 2u : 0u);
  }
  // Resolution has already applied texture invalidations. Key the resulting
  // views, not that global generation: EFB copies can invalidate bindings
  // every frame without changing any of the world's material descriptors.
  key.pbr = g_gxState.pbr ? 1u : 0u;
  key.pbrCube = g_gxState.pbrCube;
  key.pbrVolume = g_gxState.pbrVolume;
  key.probeGeneration = sProbeGeneration;
  return key;
}

// The entry for the current textures, or null when the draw samples none. Once
// a frame it keeps the entry's bind groups from expiring in the gfx cache; one
// that has expired is built again.
BindGroupCacheEntry* current_bind_group_entry(const ShaderInfo& info) noexcept {
  if (!info.sampledTextures.any() && !info.sampledIndTextures.any()) {
    return nullptr;
  }
  const BindGroupKey key = bind_group_key(info);
  const uint64_t hash = XXH3_64bits(&key, sizeof(key));
  const uint32_t frame = gfx::current_frame();
  if (frame != UINT32_MAX && frame - sBindGroupCachePrunedFrame > BindGroupCacheRetainFrames) {
    sBindGroupCachePrunedFrame = frame;
    absl::erase_if(sBindGroupCache, [frame](const auto& item) {
      return frame - item.second.touchedFrame > BindGroupCacheRetainFrames;
    });
  }
  auto [it, inserted] = sBindGroupCache.try_emplace(hash);
  auto& entry = it->second;
  if (inserted) {
    entry.key = key;
  } else if (std::memcmp(&entry.key, &key, sizeof(key)) != 0) {
    // A hash collision: the newcomer takes the entry
    entry = BindGroupCacheEntry{};
    entry.key = key;
  }
  if (entry.touchedFrame != frame) {
    entry.touchedFrame = frame;
    if (entry.monoValid && entry.mono.textureBindGroup != 0 && !gfx::touch_bind_group(entry.mono.textureBindGroup)) {
      entry.monoValid = false;
    }
    if (entry.multiviewValid && entry.multiview != 0 && !gfx::touch_bind_group(entry.multiview)) {
      entry.multiviewValid = false;
    }
  }
  return &entry;
}

GXBindGroups cached_bind_groups(const ShaderInfo& info) noexcept {
  auto* entry = current_bind_group_entry(info);
  if (entry == nullptr) {
    return {};
  }
  if (!entry->monoValid || entry->mono.textureBindGroup == 0) {
    gfx::perf::count(gfx::perf::g_fifoBindGroupMisses, gfx::perf::enabled());
    entry->mono = build_bind_groups(info);
    entry->monoValid = true;
  }
  return entry->mono;
}

gfx::BindGroupRef cached_multiview_bind_group(const ShaderInfo& info, uint64_t stereoEpoch) noexcept {
  auto* entry = current_bind_group_entry(info);
  if (entry == nullptr) {
    return {};
  }
  if (!entry->multiviewValid || entry->multiview == 0 || entry->multiviewEpoch != stereoEpoch) {
    entry->multiview = build_multiview_bind_group(info);
    entry->multiviewEpoch = stereoEpoch;
    entry->multiviewValid = true;
  }
  return entry->multiview;
}

FogRangeLutKey fog_range_lut_key() noexcept {
  const auto& state = g_gxState.fog;
  const f32 logicalWidth = std::max(g_gxState.logicalViewport.width, 1.f);
  const f32 renderWidth = std::max(g_gxState.renderViewport.width, 1.f);
  return {
      .rangeK = state.rangeK,
      .rangeCenter = ((static_cast<f32>(state.rangeCenter) - g_gxState.logicalViewport.left) / logicalWidth) * 2.f -
                     1.f + (g_gxState.renderViewport.left / renderWidth) * 2.f,
      .renderWidth = renderWidth,
      .targetWidth = gfx::get_render_target_size().x,
  };
}

std::vector<f32> build_fog_range_lut(const FogRangeLutKey& key) {
  std::array<f32, 10> rangeK;
  for (u32 i = 0; i < rangeK.size(); ++i) {
    const u32 source = (i & ~1u) | (1u - (i & 1u));
    rangeK[i] = static_cast<f32>(key.rangeK[source]) / 64.f;
  }

  std::vector<f32> lut(key.targetWidth);
  for (u32 x = 0; x < key.targetWidth; ++x) {
    const f32 screenX = ((static_cast<f32>(x) + 0.5f) / key.renderWidth) * 2.f - 1.f;
    const f32 offset = screenX - key.rangeCenter;
    const f32 rangeIndex = std::clamp(9.f - std::abs(offset) * 9.f, 0.f, 9.f);
    const u32 lower = static_cast<u32>(rangeIndex);
    const u32 upper = std::min(lower + 1, 9u);
    const f32 fraction = rangeIndex - static_cast<f32>(lower);
    const f32 k = std::max(rangeK[lower] * (1.f - fraction) + rangeK[upper] * fraction, 0.000001f);
    lut[x] = std::sqrt(offset * offset + k * k) / k;
  }
  return lut;
}

const std::vector<f32>& resolve_fog_range_lut(const FogRangeLutKey& key) {
  for (const auto& entry : sFogRangeLuts) {
    if (entry.key == key) {
      return entry.factors;
    }
  }
  if (sFogRangeLuts.size() == MaxFogRangeLuts) {
    sFogRangeLuts.erase(sFogRangeLuts.begin());
  }
  sFogRangeLuts.emplace_back(FogRangeLutEntry{key, build_fog_range_lut(key)});
  return sFogRangeLuts.back().factors;
}

gfx::Range push_fog_range_lut(const FogRangeLutKey& key) {
  const auto& lut = resolve_fog_range_lut(key);
  return gfx::push_storage(reinterpret_cast<const u8*>(lut.data()), lut.size() * sizeof(f32));
}

u8 line_mode_for_prim(GXPrimitive prim) noexcept {
  switch (prim) {
  case GX_LINES:
    return 1;
  case GX_LINESTRIP:
    return 2;
  case GX_POINTS:
    return 3;
  default:
    return 0;
  }
}
} // namespace

static void handle_draw(u8 cmd, ByteReader& reader) noexcept;
static void handle_aurora(ByteReader& reader) noexcept;

ProcessResult process(const u8* data, u32 size) noexcept {
  ZoneScoped;
  ByteReader reader{{data, size}};
  const bool perfOn = gfx::perf::enabled();

  while (!reader.empty()) {
    const u8 cmd = reader.read<u8>();
    u8 opcode = cmd & CP_OPCODE_MASK;

    switch (opcode) {
    case CP_CMD_NOP:
      continue;

    case CP_CMD_LOAD_BP_REG: {
      const u32 value = reader.read<u32>();
      {
        const gfx::perf::Bucket bucket{gfx::perf::g_fifoBpTicks, perfOn, &gfx::perf::g_fifoBpLoads};
        handle_bp(value);
      }
      if (reg_get(value, 8, 24) == GX_BP_REG_DRAWDONE) {
        return {static_cast<u32>(reader.offset()), true};
      }
      break;
    }

    case CP_CMD_LOAD_CP_REG: {
      const u8 addr = reader.read<u8>();
      const gfx::perf::Bucket bucket{gfx::perf::g_fifoCpTicks, perfOn};
      handle_cp(addr, reader.read<u32>());
      break;
    }

    case CP_CMD_LOAD_XF_REG: {
      const u32 header = reader.read<u32>();
      const u32 count = ((header >> 16) & 0xFFFF) + 1;
      const u16 addr = header & 0xFFFF;
      const gfx::perf::Bucket bucket{gfx::perf::g_fifoXfTicks, perfOn, &gfx::perf::g_fifoXfLoads};
      handle_xf(addr, reader.take(count * sizeof(u32)));
      break;
    }

    case CP_CMD_LOAD_INDX_A:
    case CP_CMD_LOAD_INDX_B:
    case CP_CMD_LOAD_INDX_C:
    case CP_CMD_LOAD_INDX_D: {
      ZoneScopedN("LOAD_INDX");
      const gfx::perf::Bucket bucket{gfx::perf::g_fifoXfTicks, perfOn, &gfx::perf::g_fifoXfLoads};
      const u32 arrayType = GX_POS_MTX_ARRAY + (opcode - CP_CMD_LOAD_INDX_A) / 0x08;
      const u16 srcArrayIdx = reader.read<u16>();
      const u16 addrLen = reader.read<u16>();

      const u16 len = (addrLen >> 12) + 1;
      const u16 dstAddr = addrLen & 0x0FFF;
      auto const& array = g_gxState.arrays[arrayType];
      const u32 srcOffset = static_cast<u32>(srcArrayIdx) * array.stride;
      const u32 srcSize = static_cast<u32>(len) * sizeof(u32);
      AURORA_ASSERT(array.data != nullptr, "indexed XF load from unmapped array {}", arrayType);
      AURORA_ASSERT(srcOffset <= array.size && srcSize <= array.size - srcOffset,
                    "indexed XF load outside array {}: offset={}, size={}, array size={}", arrayType, srcOffset,
                    srcSize, array.size);
      auto const* srcData = static_cast<const u8*>(array.data) + srcOffset;
      if (!copy_xf_data(dstAddr, srcData, len, array.le ? std::endian::little : std::endian::big)) {
#ifndef NDEBUG
        Log.debug("Unimplemented indexed XF load (opcode 0x{:02X}, dstAddr=%04x)", opcode, dstAddr);
#endif
      }
      break;
    }

    case CP_CMD_CALL_DL: {
      // Call display list: 8 bytes (address + size)
      Log.warn("Ignoring nested GX_CMD_CALL_DL");
      reader.skip(8);
      break;
    }

    case CP_CMD_INVAL_VTX: {
      for (auto& array : g_gxState.arrays) {
        array.cachedRange = {};
      }
      g_gxState.dirty |= DirtyImmediates;
      break;
    }

    case GX_AURORA: {
      const gfx::perf::Bucket bucket{gfx::perf::g_fifoAuroraTicks, perfOn};
      handle_aurora(reader);
      break;
    }

    // Draw commands: 0x80-0xBF
    case GX_DRAW_QUADS:
    case GX_DRAW_TRIANGLES:
    case GX_DRAW_TRIANGLE_STRIP:
    case GX_DRAW_TRIANGLE_FAN:
    case GX_DRAW_LINES:
    case GX_DRAW_LINE_STRIP:
    case GX_DRAW_POINTS: {
      handle_draw(cmd, reader);
      break;
    }

    default:
      // Check if it's a draw command (0x80-0xBF range)
      if (cmd >= 0x80) {
        handle_draw(cmd, reader);
      } else {
        // Hex dump surrounding bytes for debugging
        {
          const size_t pos = reader.offset();
          size_t dumpStart = (pos > 17) ? pos - 17 : 0;
          size_t dumpEnd = (pos + 16 < size) ? pos + 16 : size;
          std::string hex;
          for (size_t i = dumpStart; i < dumpEnd; i++) {
            if (i == pos - 1)
              hex += fmt::format("[{:02x}]", data[i]);
            else
              hex += fmt::format(" {:02x}", data[i]);
          }
          Log.error("  hex dump (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
        }
        FATAL("command_processor: unknown opcode 0x{:02X} at pos {}", cmd, reader.offset() - 1);
      }
      break;
    }
  }
  return {size, false};
}

[[noreturn]] static void handle_draw_overrun(size_t totalVtxBytes, const ByteReader& reader) noexcept {
  // Hex dump around the draw command for debugging
  const size_t pos = reader.offset();
  const size_t size = reader.size();
  const u8* data = reader.data();
  size_t cmdPos = pos - 2 - 1; // opcode byte position (before vtxCount and pos++)
  size_t dumpStart = (cmdPos > 16) ? cmdPos - 16 : 0;
  size_t dumpEnd = (cmdPos + 32 < size) ? cmdPos + 32 : size;
  std::string hex;
  for (size_t i = dumpStart; i < dumpEnd; i++) {
    if (i == cmdPos)
      hex += fmt::format("[{:02x}]", data[i]);
    else
      hex += fmt::format(" {:02x}", data[i]);
  }
  Log.error("  hex dump around draw cmd (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
  FATAL("draw vertex data overrun: need {} bytes at pos {}, have {}", totalVtxBytes, pos, reader.remaining());
}

static u32 calc_vtx_size(GXVtxFmt fmt) noexcept {
  u32 vtxSize = 0;
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto& attrFmt = vtxFmt.attrs[i];
    switch (g_gxState.vtxDesc[i]) {
    case GX_NONE:
      break;
    case GX_DIRECT: {
      const auto attr = static_cast<GXAttr>(i);
      vtxSize += comp_type_size(attr, attrFmt.type) * comp_cnt_count(attr, attrFmt.cnt);
      break;
    }
    case GX_INDEX8:
      vtxSize += i == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3 ? 3 : 1;
      break;
    case GX_INDEX16:
      vtxSize += i == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3 ? 6 : 2;
      break;
    }
  }
  g_gxState.lastVtxFmt = fmt;
  g_gxState.lastVtxSize = vtxSize;
  return vtxSize;
}

// The multiview shader variant the uniform just staged needs
// (gfx::stage_stereo_uniforms), or MultiviewNone, which leaves the draw out of
// the eye passes, when the staged data does not sit where that variant reads it:
// the clip matrices or the second eye copy at the shader's uniform size
// (shader.cpp to_multiview_source).
static u8 staged_multiview_mode(const DrawCache& cache) noexcept {
  const u8 mode = gfx::stereo_multiview_mode();
  const auto& offsets = cache.stereoUniformOffsets;
  u32 stride = cache.shaderInfo.uniformSize;
  if (mode == MultiviewClip) {
    stride = gfx::align_uniform(cache.uniformRange.size);
  } else if (mode == MultiviewFull && offsets[0] != offsets[1]) {
    stride = offsets[1] - offsets[0];
  }
  if (stride != cache.shaderInfo.uniformSize) {
    static bool sReported = false;
    if (!sReported) {
      sReported = true;
      Log.error("Multiview uniform data lies {} bytes on, the shader expects {}", stride,
                cache.shaderInfo.uniformSize);
    }
    return MultiviewNone;
  }
  return mode;
}

static void push_gx_draw(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange, gfx::Range idxRange,
                         u32 numIndices) noexcept {
  auto& state = g_gxState;
  auto& cache = sDrawCache;
  const bool perfOn = gfx::perf::enabled();
  const auto t0 = gfx::perf::stamp(perfOn);

  DrawImmediateData immediates{.vtxStart = vertRange.offset, .currentPnMtx = state.currentPnMtx};
  // The arrays, unless the vertices carry their elements already (deindexVertices)
  for (int i = GX_VA_POS; i <= GX_VA_TEX7 && !state.deindexVertices; ++i) {
    if (state.vtxDesc[i] != GX_INDEX8 && state.vtxDesc[i] != GX_INDEX16) {
      continue;
    }
    auto& array = state.arrays[i];
    if (array.cachedRange.size == 0) {
      array.cachedRange = gfx::push_storage(static_cast<const uint8_t*>(array.data), array.size);
    }
    immediates.arrayStart[i - GX_VA_POS] = array.cachedRange.offset + array.baseIndex * array.stride;
  }

  const u8 lineMode = line_mode_for_prim(prim);
  const bool pipelineValid = cache.hasPipeline && (state.dirty & DirtyPipeline) == 0 && cache.fmt == fmt &&
                             cache.lineMode == lineMode && cache.config.msaaSamples == gfx::get_sample_count();
  if (!pipelineValid) {
    const bool hadPipeline = cache.hasPipeline;
    const auto prevSampledTextures = cache.shaderInfo.sampledTextures;
    const auto prevSampledIndTextures = cache.shaderInfo.sampledIndTextures;
    populate_pipeline_config(cache.config, prim, fmt);
    cache.shaderInfo = build_shader_info(cache.config.shaderConfig);
    cache.pipelineRef = gfx::pipeline_ref(cache.config);
    cache.multiviewPipelineMask = 0;
    cache.fmt = fmt;
    cache.lineMode = lineMode;
    cache.hasPipeline = true;
    state.dirty = (state.dirty & ~DirtyPipeline) | DirtyUniform;
    gfx::perf::count(gfx::perf::g_fifoPipelineBuilds, perfOn);
    if (!hadPipeline || prevSampledTextures != cache.shaderInfo.sampledTextures ||
        prevSampledIndTextures != cache.shaderInfo.sampledIndTextures) {
      cache.bindGeneration = 0;
    }
  }

  const auto tA = gfx::perf::stamp(perfOn);
  const bool bindGroupsValid =
      (state.dirty & DirtyTextures) == 0 && cache.bindGeneration == texture::current_bind_generation();
  if (!bindGroupsValid) {
    gfx::perf::count(gfx::perf::g_fifoBindGroupBuilds, perfOn);
    const auto prevBindGroup = cache.bindGroups.textureBindGroup;
    {
      const gfx::perf::Bucket bucket{gfx::perf::g_fifoResolveTicks, perfOn};
      resolve_sampled_textures(cache.shaderInfo);
    }
    cache.bindGroups = cached_bind_groups(cache.shaderInfo);
    cache.bindGeneration = texture::current_bind_generation();
    state.dirty &= ~DirtyTextures;
    // For texture_size_bias uniform
    if (cache.bindGroups.textureBindGroup != prevBindGroup) {
      state.dirty |= DirtyUniform;
    }
  }
  // Stereo replay: eye bind groups for draws that sample a per-eye EFB copy, or
  // under multiview every draw's bind group of 2D array views.
  const bool multiview = gfx::recording_multiview();
  if (gfx::stereo_shadow::active()) {
    const uint64_t stereoEpoch = gfx::stereo_shadow::epoch();
    if (!bindGroupsValid || cache.stereoEpoch != stereoEpoch || cache.stereoBindGroupsMultiview != multiview) {
      cache.stereoBindGroups =
          multiview ? std::array<gfx::BindGroupRef, 2>{cached_multiview_bind_group(cache.shaderInfo, stereoEpoch), {}}
                    : build_stereo_bind_groups(cache.shaderInfo);
      cache.stereoEpoch = stereoEpoch;
      cache.stereoBindGroupsMultiview = multiview;
    }
  } else {
    cache.stereoBindGroups = {};
  }

  const auto t1 = gfx::perf::stamp(perfOn);
  const bool uniformValid = (state.dirty & DirtyUniform) == 0 && cache.uniformRange.size != 0;
  if (!uniformValid) {
    gfx::perf::count(gfx::perf::g_fifoUniformBuilds, perfOn);
    cache.uniformRange = build_uniform(cache.shaderInfo, cache.stereoUniformOffsets, &cache.uniformBytes);
    cache.uniformRoute = gfx::stereo_draw_route();
    cache.uniformPlane = gfx::stereo_head_locked_plane();
    cache.uniformScreenTexMtx = gfx::stereo_screen_tex_mtx();
    cache.multiviewMode = staged_multiview_mode(cache);
    state.dirty &= ~DirtyUniform;
  }
  const auto t2 = gfx::perf::stamp(perfOn);
  // The eye passes' pipeline: under multiview the staged mode's; per eye, EyeClipImmediate
  // for a draw staged with its eye clips (a MultiviewFull one keeps its per-eye uniform
  // copies and the plain pipeline).
  gfx::PipelineRef multiviewPipeline{};
  const u8 eyeMode = multiview                                 ? cache.multiviewMode
                     : cache.multiviewMode == MultiviewClip ? static_cast<u8>(EyeClipImmediate)
                                                            : static_cast<u8>(MultiviewNone);
  if (eyeMode != MultiviewNone) {
    const u8 modeBit = 1u << eyeMode;
    if ((cache.multiviewPipelineMask & modeBit) == 0) {
      PipelineConfig eyeConfig = cache.config;
      eyeConfig.shaderConfig.multiview = eyeMode;
      cache.multiviewPipelineRefs[eyeMode] = gfx::pipeline_ref(eyeConfig);
      cache.multiviewPipelineMask |= modeBit;
    }
    multiviewPipeline = cache.multiviewPipelineRefs[eyeMode];
  }
  if (cache.config.shaderConfig.fogRangeEnabled) {
    const auto key = fog_range_lut_key();
    if (!cache.hasFogRange || cache.fogRangeKey != key) {
      cache.fogRange = push_fog_range_lut(key);
      cache.fogRangeKey = key;
      cache.hasFogRange = true;
    }
  }
  immediates.fogRangeBase = cache.fogRange.offset / sizeof(u32);
  // MultiviewFull: an eye pair (element view_index of it) or one uniform for both eyes.
  immediates.eyeMask = cache.stereoUniformOffsets[0] != cache.stereoUniformOffsets[1] ? 1 : 0;

  state.dirty &= ~DirtyImmediates;

  uint32_t instanceCount = 1;
  if (prim == GX_LINES) {
    instanceCount = vtxCount / 2;
  } else if (prim == GX_LINESTRIP) {
    instanceCount = vtxCount - 1;
  } else if (prim == GX_POINTS) {
    instanceCount = vtxCount;
  }
  cache.lastDrawFmt = fmt;
  gfx::push_draw_command(DrawData{
      .pipeline = cache.pipelineRef,
      .vertRange = vertRange,
      .idxRange = idxRange,
      .uniformRange = cache.uniformRange,
      .immediateData = immediates,
      .vtxCount = vtxCount,
      .indexCount = numIndices,
      .instanceCount = instanceCount,
      .bindGroups = cache.bindGroups,
      .dstAlpha = state.dstAlpha,
      .stereoUniformOffset = cache.stereoUniformOffsets,
      .stereoTextureBindGroup = cache.stereoBindGroups,
      .multiviewPipeline = multiviewPipeline,
  });
  if (perfOn) {
    const auto t3 = gfx::perf::tick();
    gfx::perf::add(gfx::perf::g_fifoPipelineTicks, t0, tA);
    gfx::perf::add(gfx::perf::g_fifoBindsTicks, tA, t1);
    gfx::perf::add(gfx::perf::g_fifoUniformTicks, t1, t2);
    gfx::perf::add(gfx::perf::g_fifoPushTicks, t2, t3);
    gfx::perf::g_fifoDrawsPushed.fetch_add(1, std::memory_order_relaxed);
  }
}

static void handle_draw_unmerged(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange) noexcept {
  ZoneScoped;
  u32 numIndices = 0;
  gfx::Range idxRange;

  if (prim != GX_TRIANGLES) {
    ZoneScopedN("build idx buffer");
    const gfx::perf::Bucket bucket{gfx::perf::g_fifoVertsTicks, gfx::perf::enabled()};
    static ByteBuffer idxBuf;
    numIndices = prepare_idx_buffer(idxBuf, prim, 0, vtxCount);
    idxRange = gfx::push_indices(idxBuf.data(), idxBuf.size(), 4);
    idxBuf.clear();
  }

  push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, numIndices);
}

// Index staging for a merged draw: `count` more list indices, a new range
// 4-byte aligned, its staged offset in `offset`.
static u16* reserve_staged_indices(ByteBuffer& indices, u32 count, bool newRange, u32& offset) noexcept {
  if (newRange) {
    const size_t aligned = AURORA_ALIGN(indices.size(), 4);
    if (aligned > indices.size()) {
      indices.append_zeroes(aligned - indices.size());
    }
  }
  offset = static_cast<u32>(indices.size());
  return reinterpret_cast<u16*>(indices.append_uninitialized(static_cast<size_t>(count) * sizeof(u16)));
}

// Counts why a draw could not join the last one (perf_counters.hpp MergeBreak).
static void count_merge_breaks(bool formatOrModeDiffers, bool noLastDraw) noexcept {
  if (!gfx::perf::enabled()) {
    return;
  }
  const u32 dirty = g_gxState.dirty;
  gfx::perf::count_merge_break(gfx::perf::BreakPipeline, (dirty & DirtyPipeline) != 0);
  gfx::perf::count_merge_break(gfx::perf::BreakTextures, (dirty & DirtyTextures) != 0);
  gfx::perf::count_merge_break(gfx::perf::BreakUniform, (dirty & DirtyUniform) != 0);
  gfx::perf::count_merge_break(gfx::perf::BreakImmediates, (dirty & DirtyImmediates) != 0);
  gfx::perf::count_merge_break(gfx::perf::BreakFormat, formatOrModeDiffers);
  gfx::perf::count_merge_break(gfx::perf::BreakNoDraw, dirty == 0 && !formatOrModeDiffers && noLastDraw);
}

// GX register writes can change unused state or temporarily change it and put
// it back. Only the final shader, sampled textures and uniform bytes need to
// agree to concatenate vertices. Keep submission order and all pass boundaries.
static void reconcile_merge_state(GXPrimitive prim, GXVtxFmt fmt) noexcept {
  auto& state = g_gxState;
  auto& cache = sDrawCache;
  if (!cache.hasPipeline || cache.uniformRange.size == 0 || prim == GX_LINESTRIP ||
      gfx::get_last_draw_command<DrawData>() == nullptr) {
    return;
  }
  const auto plane = gfx::stereo_head_locked_plane();
  const auto screen = gfx::stereo_screen_tex_mtx();
  if (cache.uniformRoute != gfx::stereo_draw_route() ||
      cache.uniformPlane.tanHalfWidth != plane.tanHalfWidth ||
      cache.uniformPlane.tanHalfHeight != plane.tanHalfHeight || cache.uniformPlane.distance != plane.distance ||
      cache.uniformScreenTexMtx.texSlot != screen.texSlot || cache.uniformScreenTexMtx.pnSlot != screen.pnSlot) {
    state.dirty |= DirtyUniform;
    return;
  }
  if (gfx::stereo_shadow::active() && cache.stereoEpoch != gfx::stereo_shadow::epoch()) {
    state.dirty |= DirtyTextures;
    return;
  }
  if ((state.dirty & DirtyPipeline) != 0 || cache.fmt != fmt || cache.lineMode != line_mode_for_prim(prim)) {
    PipelineConfig candidate{};
    populate_pipeline_config(candidate, prim, fmt);
    if (std::memcmp(&candidate, &cache.config, sizeof(candidate)) != 0) {
      return;
    }
    state.dirty &= ~DirtyPipeline;
    cache.fmt = fmt;
    cache.lastDrawFmt = fmt;
  }
  if ((state.dirty & DirtyTextures) != 0 || cache.bindGeneration != texture::current_bind_generation()) {
    state.dirty |= DirtyTextures;
    resolve_sampled_textures(cache.shaderInfo);
    const auto groups = cached_bind_groups(cache.shaderInfo);
    if (groups.textureBindGroup != cache.bindGroups.textureBindGroup) {
      return;
    }
    cache.bindGeneration = texture::current_bind_generation();
    state.dirty &= ~DirtyTextures;
    // Even an unchanged texture binding may have new logical dimensions or
    // LOD bias. These live in the uniform, not in the bind-group descriptor.
    state.dirty |= DirtyUniform;
  }
  if ((state.dirty & DirtyUniform) != 0 && uniform_matches(cache.shaderInfo, cache.uniformBytes)) {
    state.dirty &= ~DirtyUniform;
  }
}

// `cmd` is the FIFO draw command, whose following commands of the same kind
// merge along; 0 for a draw delivered otherwise (GX_AURORA_DRAW_SIZED).
static void draw_prim(u8 cmd, GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, ByteReader& reader) noexcept {
  ZoneScoped;
  u32 vtxSize;
  if (g_gxState.lastVtxFmt == fmt)
    LIKELY { vtxSize = g_gxState.lastVtxSize; }
  else
    UNLIKELY { vtxSize = calc_vtx_size(fmt); }

  u32 totalVtxBytes = vtxCount * vtxSize;
  if (totalVtxBytes > reader.remaining())
    UNLIKELY { handle_draw_overrun(totalVtxBytes, reader); }

  // The GPU expands each strip segment to an independent quad already. Give it
  // independent endpoint pairs so adjacent strips can share one instanced draw
  // without creating a segment between strips. Expand the FIFO records before
  // de-indexing, preserving all per-vertex attributes and their byte order.
  if (prim == GX_LINESTRIP) {
    const auto source = reader.take(totalVtxBytes);
    static ByteBuffer pairs;
    for (u32 first = 0; first + 1 < vtxCount;) {
      const u32 segments = std::min<u32>(vtxCount - first - 1, UINT16_MAX / 2);
      pairs.clear();
      auto* dst = pairs.append_uninitialized(segments * 2 * vtxSize);
      for (u32 segment = 0; segment < segments; ++segment) {
        std::memcpy(dst + segment * 2 * vtxSize, source.data() + (first + segment) * vtxSize, 2 * vtxSize);
      }
      ByteReader pairReader{pairs.data(), pairs.size()};
      draw_prim(0, GX_LINES, fmt, static_cast<u16>(segments * 2), pairReader);
      first += segments;
    }
    return;
  }

  // Resolve the plan before clearing array dirtiness: after de-indexing, new
  // array addresses/base indices change the copied records, not the GPU state.
  const DeindexPlan* const plan = g_gxState.deindexVertices ? &deindex_plan(fmt) : nullptr;
  if (g_gxState.dirty != 0 || fmt != sDrawCache.lastDrawFmt) {
    reconcile_merge_state(prim, fmt);
  }
  const u32 drawDirty = g_gxState.dirty & ~(g_gxState.deindexVertices ? DirtyImmediates : 0u);
  const u8 lineMode = line_mode_for_prim(prim);
  const bool formatDiffers = fmt != sDrawCache.lastDrawFmt || sDrawCache.lineMode != lineMode;
  // Independent lines and points are instances of the same quad. Line strips
  // cannot concatenate: doing so would connect the end of one to the next.
  const bool cleanState = drawDirty == 0 && !formatDiffers && prim != GX_LINESTRIP;
  auto* lastDraw = cleanState ? gfx::get_last_draw_command<DrawData>() : nullptr;
  ByteBuffer* const verts = lastDraw != nullptr ? gfx::staging_verts() : nullptr;
  ByteBuffer* const indices = lastDraw != nullptr ? gfx::staging_indices() : nullptr;
  const bool canMerge = lastDraw != nullptr && (lineMode != 0 || lastDraw->instanceCount == 1) &&
                        lastDraw->immediateData.currentPnMtx == g_gxState.currentPnMtx && verts != nullptr &&
                        indices != nullptr && lastDraw->vtxCount + vtxCount <= UINT16_MAX &&
                        (prim != GX_LINES || (lastDraw->vtxCount % 2 == 0 && vtxCount % 2 == 0)) &&
                        (prim != GX_TRIANGLES || ((lastDraw->indexCount != 0 || lastDraw->vtxCount % 3 == 0) &&
                                                  vtxCount % 3 == 0));
  const bool perfOn = gfx::perf::enabled();
  const auto t0 = gfx::perf::stamp(perfOn);
  const bool deindex = plan != nullptr && plan->active;

  if (!canMerge) {
    count_merge_breaks(formatDiffers || prim == GX_LINESTRIP, lastDraw == nullptr);
    const auto vertexData = reader.take(totalVtxBytes);
    const gfx::Range vertRange = push_vertex_records(plan, vertexData, vtxCount, 4);
    if (perfOn) {
      gfx::perf::add(gfx::perf::g_fifoVertsTicks, t0, gfx::perf::tick());
    }
    handle_draw_unmerged(prim, fmt, vtxCount, vertRange);
    return;
  }

  // Merge into the previous draw. Prime's world surfaces are thousands of short
  // strips a frame, so this appends to the staging buffers directly, and takes
  // the strips that follow under the same command in the same loop.
  g_gxState.dirty &= ~DirtyImmediates;
  u32 merged = 0;
  while (true) {
    const auto vertexData = reader.take(totalVtxBytes);
    CHECK(lastDraw->vertRange.offset + lastDraw->vertRange.size == verts->size(),
          "Non-consecutive vertex ranges ({} < {})", lastDraw->vertRange.offset + lastDraw->vertRange.size,
          verts->size());
    const u32 stagedBytes = deindex ? vtxCount * plan->dstStride : totalVtxBytes;
    if (deindex) {
      const gfx::perf::Bucket bucket{gfx::perf::g_fifoDeindexTicks, perfOn};
      deindex_records(*plan, vertexData.data(), vtxCount, verts->append_uninitialized(stagedBytes));
      if (perfOn) {
        gfx::perf::g_fifoDeindexedVerts.fetch_add(vtxCount, std::memory_order_relaxed);
      }
    } else if (totalVtxBytes > 0) {
      std::memcpy(verts->append_uninitialized(totalVtxBytes), vertexData.data(), totalVtxBytes);
    }
    lastDraw->vertRange.size += stagedBytes;
    if (lineMode != 0) {
      // Keep the existing quad indices; instances address the appended points
      // or independent pairs relative to the original draw's vertex start.
      lastDraw->instanceCount += prim == GX_LINES ? vtxCount / 2 : vtxCount;
    } else if (lastDraw->indexCount == 0 && prim != GX_TRIANGLES) {
      // The previous draw's own triangles take their list indices first
      const u32 count = list_index_count(GX_TRIANGLES, static_cast<u16>(lastDraw->vtxCount));
      u32 offset = 0;
      write_list_indices(reserve_staged_indices(*indices, count, true, offset), GX_TRIANGLES, 0,
                         static_cast<u16>(lastDraw->vtxCount));
      lastDraw->idxRange = gfx::Range{offset, static_cast<u32>(count * sizeof(u16))};
      lastDraw->indexCount = count;
    }
    if (lineMode == 0 && lastDraw->indexCount != 0) {
      const u32 count = list_index_count(prim, vtxCount);
      const bool newRange = lastDraw->idxRange.size == 0;
      u32 offset = 0;
      u16* const out = reserve_staged_indices(*indices, count, newRange, offset);
      write_list_indices(out, prim, static_cast<u16>(lastDraw->vtxCount), vtxCount);
      if (newRange) {
        lastDraw->idxRange = gfx::Range{offset, static_cast<u32>(count * sizeof(u16))};
      } else {
        CHECK(lastDraw->idxRange.offset + lastDraw->idxRange.size == offset, "Non-consecutive index ranges ({} < {})",
              lastDraw->idxRange.offset + lastDraw->idxRange.size, offset);
        lastDraw->idxRange.size += static_cast<u32>(count * sizeof(u16));
      }
      lastDraw->indexCount += count;
    }
    lastDraw->vtxCount += vtxCount;
    ++merged;

    // The next command, when it is another strip of this kind
    if (cmd == 0 || reader.remaining() < 3 || reader.data()[reader.offset()] != cmd) {
      break;
    }
    const u16 nextCount = read_bits<u16>(reader.data() + reader.offset() + 1);
    const u32 nextBytes = nextCount * vtxSize;
    if (nextCount == 0 || lastDraw->vtxCount + nextCount > UINT16_MAX || nextBytes > reader.remaining() - 3 ||
        (prim == GX_LINES && nextCount % 2 != 0) || (prim == GX_TRIANGLES && nextCount % 3 != 0)) {
      break;
    }
    reader.skip(3);
    vtxCount = nextCount;
    totalVtxBytes = nextBytes;
  }
  gfx::detail::increment_merged_draw_count(merged);
  if (perfOn) {
    gfx::perf::add(gfx::perf::g_fifoVertsTicks, t0, gfx::perf::tick());
    gfx::perf::g_fifoDrawsMerged.fetch_add(merged, std::memory_order_relaxed);
  }
}

static void handle_draw(u8 cmd, ByteReader& reader) noexcept {
  const auto fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
  const auto prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
  draw_prim(cmd, prim, fmt, reader.read<u16>(), reader);
}

void handle_aurora(ByteReader& reader) noexcept {
  ZoneScoped;
  const u16 subCmd = reader.read<u16>();

  if (subCmd == GX_AURORA_LOAD_VIEWPORT_RENDER) {
    const f32 left = reader.read<f32>();
    const f32 top = reader.read<f32>();
    const f32 width = reader.read<f32>();
    const f32 height = reader.read<f32>();
    const f32 nearZ = reader.read<f32>();
    const f32 farZ = reader.read<f32>();
    set_render_viewport({
        .left = left,
        .top = top,
        .width = width,
        .height = height,
        .znear = nearZ,
        .zfar = farZ,
    });
  } else if (subCmd == GX_AURORA_LOAD_SCISSOR_RENDER) {
    const s32 left = reader.read<s32>();
    const s32 top = reader.read<s32>();
    const s32 width = reader.read<s32>();
    const s32 height = reader.read<s32>();
    set_render_scissor({left, top, width, height});
  } else if (subCmd == GX_AURORA_LOAD_PROJECTION_FULL) {
    auto& proj = g_gxState.proj;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        proj[r][c] = reader.read<f32>();
      }
    }
    // Invalidate projection XF regs
    for (u32 reg = 0x20; reg <= 0x26; ++reg) {
      g_gxState.xfRegValid.reset(reg);
    }
    g_gxState.dirty |= DirtyUniform;
  } else if (subCmd == GX_AURORA_MAP_BATCH) {
    const bool enabled = reader.read<u8>() != 0;
    if (g_gxState.mapBatch != enabled) {
      g_gxState.mapBatch = enabled;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_STEREO_DRAW_ROUTE) {
    const u8 route = reader.read<u8>();
    if (route != gfx::stereo_draw_route()) {
      // The eye uniform copies depend on the route, and a draw under a new
      // route must not merge into the previous one.
      g_gxState.dirty |= DirtyUniform;
      gfx::set_stereo_draw_route(route);
    }
  } else if (subCmd == GX_AURORA_STEREO_HEAD_LOCKED_PLANE) {
    const f32 tanHalfWidth = reader.read<f32>();
    const f32 tanHalfHeight = reader.read<f32>();
    const f32 distance = reader.read<f32>();
    const auto current = gfx::stereo_head_locked_plane();
    if (current.tanHalfWidth != tanHalfWidth || current.tanHalfHeight != tanHalfHeight ||
        current.distance != distance) {
      // The eye copies of a 2D draw on the plane depend on it.
      g_gxState.dirty |= DirtyUniform;
      gfx::set_stereo_head_locked_plane(tanHalfWidth, tanHalfHeight, distance);
    }
  } else if (subCmd == GX_AURORA_STEREO_SCREEN_TEX_MTX) {
    const u8 texSlot = reader.read<u8>();
    const u8 pnSlot = reader.read<u8>();
    const auto current = gfx::stereo_screen_tex_mtx();
    if (current.texSlot != texSlot || current.pnSlot != pnSlot) {
      // The eye uniform copies depend on it.
      g_gxState.dirty |= DirtyUniform;
      gfx::set_stereo_screen_tex_mtx(texSlot, pnSlot);
    }
  } else if (subCmd >= GX_AURORA_LOAD_ARRAYBASE && subCmd <= (GX_AURORA_LOAD_ARRAYBASE | 0x0f)) {
    const u32 attrIdx = subCmd - GX_AURORA_LOAD_ARRAYBASE + GX_VA_POS;
    const u64 arrayAddr = reader.read<u64>();
    const u32 arraySize = reader.read<u32>();
    const bool le = reader.read<u8>() == 1;

    auto& array = g_gxState.arrays[attrIdx];
    const auto newData = reinterpret_cast<void*>(arrayAddr);
    if (array.data != newData || array.size != arraySize || array.le != le) {
      if (array.le != le) {
        // Endianness is baked into the shader
        g_gxState.dirty |= DirtyPipeline;
      }
      array.data = newData;
      array.size = arraySize;
      array.le = le;
      array.cachedRange = {};
      g_gxState.dirty |= DirtyImmediates;
    }
  } else if (subCmd == GX_AURORA_LOAD_TEXOBJ) {
    const auto texMapId = reader.read<u8>();
    CHECK(texMapId < MaxTextures, "invalid texture map id {}", texMapId);
    auto& slot = g_gxState.loadedTextures[texMapId];
    const auto newData = reinterpret_cast<const void*>(reader.read<u64>());
    const auto newUserData = reinterpret_cast<const void*>(reader.read<u64>());
    const u32 newWidth = reader.read<u32>();
    const u32 newHeight = reader.read<u32>();
    const auto newFormat = static_cast<GXTexFmt>(reader.read<u32>());
    const auto newTlut = static_cast<GXTlut>(reader.read<u32>());
    u8 newFlags = slot.flags & ~0x80u; // Reset no-cache flag
    if (reader.read<u8>() != 0) {
      newFlags |= 1u;
    } else {
      newFlags &= ~1u;
    }
    const u32 newTexObjId = reader.read<u32>();
    const u32 newTexDataVersion = reader.read<u32>();
    if (slot.data != newData || slot.userData != newUserData || slot.mWidth != newWidth || slot.mHeight != newHeight ||
        slot.mFormat != static_cast<u32>(newFormat) || slot.tlut != newTlut || slot.flags != newFlags ||
        slot.texObjId != newTexObjId || slot.texDataVersion != newTexDataVersion) {
      slot.data = newData;
      slot.userData = newUserData;
      slot.mWidth = newWidth;
      slot.mHeight = newHeight;
      slot.mFormat = newFormat;
      slot.tlut = newTlut;
      slot.flags = newFlags;
      slot.texObjId = newTexObjId;
      slot.texDataVersion = newTexDataVersion;
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX_AURORA_LOAD_TLUT) {
    const auto idx = reader.read<u8>();
    CHECK(idx < MaxTluts, "invalid tlut slot {}", idx);
    auto& slot = g_gxState.loadedTluts[idx];
    const auto newData = reinterpret_cast<const void*>(reader.read<u64>());
    const auto newFormat = static_cast<GXTlutFmt>(reader.read<u32>());
    const u16 newNumEntries = reader.read<u16>();
    const u32 newTlutObjId = reader.read<u32>();
    const u32 newTlutDataVersion = reader.read<u32>();
    const u8 newFlags = slot.flags & ~0x80u; // Reset no-cache flag
    if (slot.data != newData || slot.format != newFormat || slot.numEntries != newNumEntries ||
        slot.tlutObjId != newTlutObjId || slot.tlutDataVersion != newTlutDataVersion || slot.flags != newFlags) {
      if (slot.tlutObjId != newTlutObjId || slot.tlutDataVersion != newTlutDataVersion) {
        texture::invalidate_bindings();
      }
      slot.data = newData;
      slot.format = newFormat;
      slot.numEntries = newNumEntries;
      slot.tlutObjId = newTlutObjId;
      slot.tlutDataVersion = newTlutDataVersion;
      slot.flags = newFlags;
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX2_SET_POLYGON_OFFSET) {
    const f32 frontOffset = reader.read<f32>();
    const f32 frontScale = reader.read<f32>();
    const f32 backOffset = reader.read<f32>();
    const f32 backScale = reader.read<f32>();
    const f32 clamp = reader.read<f32>();
    if (g_gxState.frontOffset != frontOffset || g_gxState.frontScale != frontScale ||
        g_gxState.backOffset != backOffset || g_gxState.backScale != backScale || g_gxState.clamp != clamp) {
      g_gxState.frontOffset = frontOffset;
      g_gxState.frontScale = frontScale;
      g_gxState.backOffset = backOffset;
      g_gxState.backScale = backScale;
      g_gxState.clamp = clamp;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_LOAD_COPY_SRC) {
    const s32 left = reader.read<s32>();
    const s32 top = reader.read<s32>();
    const s32 width = reader.read<s32>();
    const s32 height = reader.read<s32>();
    g_gxState.texCopySrc = {left, top, width, height};
  } else if (subCmd == GX_AURORA_LOAD_COPY_DST) {
    g_gxState.texCopyDstWidth = reader.read<u32>();
    g_gxState.texCopyDstHeight = reader.read<u32>();
    g_gxState.texCopyFmt = static_cast<GXTexFmt>(reader.read<u32>());
    reader.skip(1); // mipmap is not implemented, but remains part of the command payload
    g_gxState.texCopyDstWide = true;
  } else if (subCmd == GX_AURORA_LOAD_COPY_DEST) {
    g_gxState.texCopyDest = reinterpret_cast<const void*>(reader.read<u64>());
  } else if (subCmd == GX_AURORA_REQUEST_DEPTH_SNAPSHOT) {
    gfx::depth_peek::request_snapshot();
  } else if (subCmd == GX_AURORA_BEGIN_OFFSCREEN) {
    const u32 width = reader.read<u32>();
    const u32 height = reader.read<u32>();
    gfx::begin_offscreen(width, height);
  } else if (subCmd == GX_AURORA_END_OFFSCREEN) {
    gfx::end_offscreen();
  } else if (subCmd == GX_AURORA_DESTROY_TEXOBJ) {
    evict_texture_object(reader.read<u32>());
  } else if (subCmd == GX_AURORA_DESTROY_TLUT) {
    evict_tlut_object(reader.read<u32>());
  } else if (subCmd == GX_AURORA_DESTROY_COPY_TEX) {
    evict_copy_texture(reinterpret_cast<const void*>(reader.read<u64>()));
  } else if (subCmd == GX_AURORA_DRAW_SIZED) {
    const u8 cmd = reader.read<u8>();
    const u32 byteLen = reader.read<u32>();
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    if (byteLen != 0) {
      u32 vtxSize;
      if (g_gxState.lastVtxFmt == fmt) {
        vtxSize = g_gxState.lastVtxSize;
      } else {
        vtxSize = calc_vtx_size(fmt);
      }
      AURORA_ASSERT(vtxSize != 0 && byteLen % vtxSize == 0,
                    "GX_AURORA_DRAW_SIZED: {} bytes is not a whole number of size-{} vertices", byteLen, vtxSize);
      u32 vtxCount = byteLen / vtxSize;
      AURORA_ASSERT(vtxCount <= 0xFFFF, "GX_AURORA_DRAW_SIZED: too many vertices ({})", vtxCount);
      draw_prim(0, prim, fmt, static_cast<u16>(vtxCount), reader);
    }
  } else if (subCmd == GX_AURORA_DRAW_INDEXED) {
    ZoneScopedN("DRAW_INDEXED");
    const u8 cmd = reader.read<u8>();
    const u16 vtxCount = reader.read<u16>();
    const u32 indexCount = reader.read<u32>();
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    AURORA_ASSERT(prim == GX_TRIANGLES, "GX_AURORA_DRAW_INDEXED: primitive must be GX_TRIANGLES, got {}",
                  static_cast<u32>(prim));
    const size_t idxBytes = static_cast<size_t>(indexCount) * sizeof(u16);
    // Index data is always host-endian; push it to the GPU buffer as-is
    const auto indexData = reader.take(idxBytes);
    const gfx::Range idxRange = gfx::push_indices(indexData.data(), indexData.size(), 4);
    u32 vtxSize;
    if (g_gxState.lastVtxFmt == fmt) {
      vtxSize = g_gxState.lastVtxSize;
    } else {
      vtxSize = calc_vtx_size(fmt);
    }
    const u32 totalVtxBytes = vtxCount * vtxSize;
    const auto vertexData = reader.take(totalVtxBytes);
    const DeindexPlan* const plan = g_gxState.deindexVertices ? &deindex_plan(fmt) : nullptr;
    const gfx::Range vertRange = push_vertex_records(plan, vertexData, vtxCount, 4);
    if (indexCount != 0) {
      push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, indexCount);
    }
  } else if (subCmd == GX_AURORA_DEBUG_GROUP_PUSH) {
    auto label = reader.read_string();
    gfx::push_debug_group(std::move(label));
  } else if (subCmd == GX_AURORA_DEBUG_GROUP_POP) {
    pop_debug_group();
  } else if (subCmd == GX_AURORA_DEBUG_MARKER_INSERT) {
    auto label = reader.read_string();
    gfx::insert_debug_marker(std::move(label));
  } else if (subCmd == GX_AURORA_SET_DRAW_SYNC) {
    aurora::gx::set_draw_sync_token(reader.read<u16>());
  } else if (subCmd == GX_AURORA_LOAD_ARRAY_BASE_INDEX) {
    const u32 attrIdx = (reader.read<u8>() & 0x0f) + GX_VA_POS;
    const u32 base = reader.read<u32>();
    if (attrIdx < g_gxState.arrays.size() && g_gxState.arrays[attrIdx].baseIndex != base) {
      g_gxState.arrays[attrIdx].baseIndex = base;
      g_gxState.dirty |= DirtyImmediates;
    }
  } else if (subCmd == GX_AURORA_SET_PBR) {
    const bool pbr = reader.read<u8>() != 0;
    if (g_gxState.pbr != pbr) {
      g_gxState.pbr = pbr;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_SET_SDF) {
    const u8 sdf = reader.read<u8>();
    if (g_gxState.sdf != sdf) {
      g_gxState.sdf = sdf;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_COPY_PROBE_FACE) {
    copy_probe_face(reader.read<u8>());
  } else if (subCmd == GX_AURORA_SET_PBR_PROBE) {
    Mat3x4<float> mtx;
    for (Vec4<float>* col : {&mtx.m0, &mtx.m1, &mtx.m2}) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      *col = Vec4<float>{x, y, z, w};
    }
    if (g_gxState.pbrProbe != mtx) {
      g_gxState.pbrProbe = mtx;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_CREATE_PBR_CUBE) {
    ++sProbeGeneration;
    const u32 id = reader.read<u32>();
    const u32 size = reader.read<u32>();
    const u32 mipCount = reader.read<u32>();
    const std::unique_ptr<std::vector<u8>> texels{reinterpret_cast<std::vector<u8>*>(reader.read<u64>())};
    gfx::probe::create_cube(id, size, mipCount, texels->data(), texels->size());
    g_gxState.dirty |= DirtyTextures;
  } else if (subCmd == GX_AURORA_DESTROY_PBR_CUBE) {
    ++sProbeGeneration;
    gfx::probe::destroy_cube(reader.read<u32>());
    g_gxState.dirty |= DirtyTextures;
  } else if (subCmd == GX_AURORA_SET_PBR_CUBE) {
    u32 id = reader.read<u32>();
    Vec4<float> value;
    for (int i = 0; i < 4; ++i) {
      value[i] = reader.read<f32>();
    }
    if (id == 0 || !gfx::probe::has_cube(id)) {
      // No such cube: the probe, which is not HDR.
      id = 0;
      value = {};
    }
    if (g_gxState.pbrCube != id) {
      g_gxState.pbrCube = id;
      g_gxState.dirty |= DirtyTextures;
    }
    if (g_gxState.pbrCubeParams != value) {
      g_gxState.pbrCubeParams = value;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_AMBIENT) {
    for (Vec4<float>& v : g_gxState.pbrAmbient) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      const Vec4<float> value{x, y, z, w};
      if (v != value) {
        v = value;
        g_gxState.dirty |= DirtyUniform;
      }
    }
  } else if (subCmd == GX_AURORA_CREATE_PBR_VOLUME) {
    ++sProbeGeneration;
    const u32 id = reader.read<u32>();
    const u32 sizeX = reader.read<u32>();
    const u32 sizeY = reader.read<u32>();
    const u32 sizeZ = reader.read<u32>();
    const std::unique_ptr<std::vector<u8>> texels{reinterpret_cast<std::vector<u8>*>(reader.read<u64>())};
    gfx::probe::create_volume(id, sizeX, sizeY, sizeZ, texels->data(), texels->size());
    g_gxState.dirty |= DirtyTextures;
  } else if (subCmd == GX_AURORA_DESTROY_PBR_VOLUME) {
    ++sProbeGeneration;
    gfx::probe::destroy_volume(reader.read<u32>());
    g_gxState.dirty |= DirtyTextures;
  } else if (subCmd == GX_AURORA_SET_PBR_VOLUME) {
    u32 id = reader.read<u32>();
    std::array<Vec4<float>, 6> rows;
    for (Vec4<float>& v : rows) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      v = {x, y, z, w};
    }
    if (id == 0 || !gfx::probe::has_volume(id)) {
      id = 0;
      rows = {};
    }
    if (g_gxState.pbrVolume != id) {
      g_gxState.pbrVolume = id;
      g_gxState.dirty |= DirtyTextures;
    }
    if (g_gxState.pbrVolumeRows != rows) {
      g_gxState.pbrVolumeRows = rows;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_TONE) {
    std::array<Vec4<float>, 3> rows;
    for (Vec4<float>& v : rows) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      v = {x, y, z, w};
    }
    if (g_gxState.pbrTone != rows) {
      g_gxState.pbrTone = rows;
      g_gxState.dirty |= DirtyUniform;
    }
  } else if (subCmd == GX_AURORA_SET_PBR_MATERIAL) {
    for (Vec4<float>* v :
         {&g_gxState.pbrEmissive, &g_gxState.pbrBacklight, &g_gxState.pbrLayer, &g_gxState.pbrLayerHeight,
          &g_gxState.pbrParam, &g_gxState.pbrUp}) {
      const f32 x = reader.read<f32>();
      const f32 y = reader.read<f32>();
      const f32 z = reader.read<f32>();
      const f32 w = reader.read<f32>();
      const Vec4<float> value{x, y, z, w};
      if (*v != value) {
        *v = value;
        g_gxState.dirty |= DirtyUniform;
      }
    }
  }

  else {
    Log.error("Unknown Aurora subcommand: {:04X}", subCmd);
  }
}

void clear_draw_cache() noexcept {
  // Uniform/vertex ranges belong to this frame, but texture bind groups do
  // not. Keep their descriptor cache warm; current_bind_group_entry() checks
  // the resource cache's lifetime before reusing an entry in a later frame.
  sDrawCache.bindGeneration = 0;
  sDrawCache.uniformRange = {};
  sDrawCache.stereoUniformOffsets = {UINT32_MAX, UINT32_MAX};
  sDrawCache.multiviewMode = MultiviewNone;
  sDrawCache.stereoBindGroups = {};
  sDrawCache.stereoEpoch = 0;
  sDrawCache.fogRange = {};
  sDrawCache.hasFogRange = false;
  // Vertex de-indexing changes between frames only, with the pipelines
  const bool deindex = sDeindexRequested.load(std::memory_order_relaxed);
  if (deindex != g_gxState.deindexVertices) {
    g_gxState.deindexVertices = deindex;
    g_gxState.dirty |= DirtyPipeline | DirtyImmediates;
    sDeindexPlan.fmt = GX_MAX_VTXFMT;
  }
}

void reset_draw_cache() noexcept {
  // Initialization/shutdown can replace layouts and default texture views.
  sBindGroupCache.clear();
  sDrawCache = {};
}

} // namespace aurora::gx::fifo

extern "C" void aurora_set_gx_deindex_vertices(bool enabled) {
  aurora::gx::fifo::sDeindexRequested.store(enabled, std::memory_order_relaxed);
}

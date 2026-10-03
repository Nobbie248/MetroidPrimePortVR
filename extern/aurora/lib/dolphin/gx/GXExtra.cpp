#include "gx.hpp"
#include "__gx.h"
#include "dolphin/gx/GXAurora.h"

#include <vector>

extern "C" {
void GXDestroyTexObj(GXTexObj* obj_) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  if (obj->texObjId != 0) {
    GX_WRITE_AURORA(GX_AURORA_DESTROY_TEXOBJ);
    GX_WRITE_U32(obj->texObjId);
  }
  obj->texObjId = 0;
}

void GXDestroyTlutObj(GXTlutObj* obj_) {
  auto* obj = reinterpret_cast<GXTlutObj_*>(obj_);
  if (obj->tlutObjId != 0) {
    GX_WRITE_AURORA(GX_AURORA_DESTROY_TLUT);
    GX_WRITE_U32(obj->tlutObjId);
  }
  obj->tlutObjId = 0;
}

void GXDestroyCopyTex(void* dest) {
  if (dest != nullptr) {
    GX_WRITE_AURORA(GX_AURORA_DESTROY_COPY_TEX);
    GX_WRITE_U64(reinterpret_cast<u64>(dest));
  }
}

void GXSetArrayBaseIndex(GXAttr attr, u32 base) {
  if (attr == GX_VA_NBT) {
    attr = GX_VA_NRM;
  }
  const u32 cpIdx = attr - GX_VA_POS;
  assert((cpIdx & ~0xF) == 0);
  GX_WRITE_AURORA(GX_AURORA_LOAD_ARRAY_BASE_INDEX);
  GX_WRITE_U8(static_cast<u8>(cpIdx));
  GX_WRITE_U32(base);
}

void GXSetPBR(GXBool enable) {
  GX_WRITE_AURORA(GX_AURORA_SET_PBR);
  GX_WRITE_U8(enable ? 1 : 0);
}

void GXSetSDF(u8 edge) {
  GX_WRITE_AURORA(GX_AURORA_SET_SDF);
  GX_WRITE_U8(edge);
}

void GXCopyProbeFace(u32 face) {
  GX_WRITE_AURORA(GX_AURORA_COPY_PROBE_FACE);
  GX_WRITE_U8(static_cast<u8>(face));
  aurora::gx::fifo::publish();
}

void GXSetPBRProbe(const f32 viewToProbe[3][3], f32 weight) {
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_PROBE);
  for (int col = 0; col < 3; ++col) {
    GX_WRITE_F32(viewToProbe[0][col]);
    GX_WRITE_F32(viewToProbe[1][col]);
    GX_WRITE_F32(viewToProbe[2][col]);
    GX_WRITE_F32(col == 0 ? weight : 0.f);
  }
}

static u32 sPBRDebugView = 0;

void GXSetPBRDebugView(u32 view) { sPBRDebugView = view; }

void GXSetPBRMaterial(const f32 emissive[3], const f32 backlight[3], f32 heightBlend, f32 mode, const f32 layer[5],
                      const f32 kind[6], const f32 up[3]) {
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_MATERIAL);
  GX_WRITE_F32(emissive[0]);
  GX_WRITE_F32(emissive[1]);
  GX_WRITE_F32(emissive[2]);
  GX_WRITE_F32(heightBlend);
  GX_WRITE_F32(backlight[0]);
  GX_WRITE_F32(backlight[1]);
  GX_WRITE_F32(backlight[2]);
  GX_WRITE_F32(mode);
  GX_WRITE_F32(layer[0]);
  GX_WRITE_F32(kind[0]);
  GX_WRITE_F32(kind[1]);
  GX_WRITE_F32(static_cast<f32>(sPBRDebugView));
  for (int i = 1; i < 5; ++i) {
    GX_WRITE_F32(layer[i]);
  }
  for (int i = 2; i < 6; ++i) {
    GX_WRITE_F32(kind[i]);
  }
  GX_WRITE_F32(up[0]);
  GX_WRITE_F32(up[1]);
  GX_WRITE_F32(up[2]);
  GX_WRITE_F32(0.f);
}

void GXCreatePBRCube(u32 id, u32 size, u32 mipCount, const void* texels, u32 length) {
  // The processor frees the copy.
  auto* copy = new std::vector<u8>(static_cast<const u8*>(texels), static_cast<const u8*>(texels) + length);
  GX_WRITE_AURORA(GX_AURORA_CREATE_PBR_CUBE);
  GX_WRITE_U32(id);
  GX_WRITE_U32(size);
  GX_WRITE_U32(mipCount);
  GX_WRITE_U64(reinterpret_cast<u64>(copy));
}

void GXDestroyPBRCube(u32 id) {
  GX_WRITE_AURORA(GX_AURORA_DESTROY_PBR_CUBE);
  GX_WRITE_U32(id);
}

void GXSetPBRCube(u32 id, const f32 params[4]) {
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_CUBE);
  GX_WRITE_U32(id);
  for (int i = 0; i < 4; ++i) {
    GX_WRITE_F32(params[i]);
  }
}

void GXSetPBRAmbient(const f32 rows[6][3], f32 mode) {
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_AMBIENT);
  for (int row = 0; row < 6; ++row) {
    for (int i = 0; i < 3; ++i) {
      GX_WRITE_F32(rows != nullptr ? rows[row][i] : 0.f);
    }
    GX_WRITE_F32(rows != nullptr && row == 0 ? mode : 0.f);
  }
}

void GXCreatePBRVolume(u32 id, u32 sizeX, u32 sizeY, u32 sizeZ, const void* texels, u32 length) {
  // The processor frees the copy.
  auto* copy = new std::vector<u8>(static_cast<const u8*>(texels), static_cast<const u8*>(texels) + length);
  GX_WRITE_AURORA(GX_AURORA_CREATE_PBR_VOLUME);
  GX_WRITE_U32(id);
  GX_WRITE_U32(sizeX);
  GX_WRITE_U32(sizeY);
  GX_WRITE_U32(sizeZ);
  GX_WRITE_U64(reinterpret_cast<u64>(copy));
}

void GXDestroyPBRVolume(u32 id) {
  GX_WRITE_AURORA(GX_AURORA_DESTROY_PBR_VOLUME);
  GX_WRITE_U32(id);
}

void GXSetPBRVolume(u32 id, const f32 rows[6][4]) {
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_VOLUME);
  GX_WRITE_U32(rows != nullptr ? id : 0);
  for (int row = 0; row < 6; ++row) {
    for (int i = 0; i < 4; ++i) {
      GX_WRITE_F32(rows != nullptr ? rows[row][i] : 0.f);
    }
  }
}

void GXSetPBRTone(const f32 rows[3][4]) {
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_TONE);
  for (int row = 0; row < 3; ++row) {
    for (int i = 0; i < 4; ++i) {
      GX_WRITE_F32(rows != nullptr ? rows[row][i] : 0.f);
    }
  }
}
}

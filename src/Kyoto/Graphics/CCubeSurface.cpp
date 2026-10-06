#include "Kyoto/Graphics/CCubeSurface.hpp"
#include "Kyoto/Basics/CBasics.hpp"
#include "Kyoto/Graphics/CCubeModel.hpp"
#include "Kyoto/Graphics/CGX.hpp"

#include <string.h>
#ifdef TARGET_PC
#include <dolphin/gx/GXExtra.h>
#include <stdlib.h>
#endif

const CVector3f CCubeSurface::skDefaultNormal(1.f, 0.f, 0.f);

#ifdef TARGET_PC
namespace {
// Port base indices ('PBIX', see PortBaseIndices). On disc they follow the
// bounds at 0x44, but the native SSurfaceData holds 64-bit pointers and its
// bounds overwrite that spot, so ConvertSurfaceHeader moves them past the
// struct. An extra size of 0x40 makes the header big enough to hold them.
const uint kPortMagic = 0x50424958; // 'PBIX'
const uint kPortDiscOffset = 0x2c + 0x18;
const uint kPortNativeOffset = 0x60;
const uint kPortExtraSize = 0x40;
const uint kPortWords = 1 + CCubeSurface::kPB_Count;
} // namespace
static_assert(sizeof(CCubeSurface::SSurfaceData) <= kPortNativeOffset, "PBIX block overlaps the surface header");
static_assert(((0x4b + kPortExtraSize) & ~31) >= kPortNativeOffset + kPortWords * 4, "PBIX block past the header");
#endif

void CCubeSurface::ConvertSurfaceHeader(void* rawData) {
#if TARGET_LITTLE_ENDIAN
  uchar* data = static_cast< uchar* >(rawData);
  float center[3];
  float normal[3];
  for (int i = 0; i < 3; ++i) {
    center[i] = CBasics::SwapBytes(*reinterpret_cast< const float* >(data + i * 4));
    normal[i] = CBasics::SwapBytes(*reinterpret_cast< const float* >(data + 0x20 + i * 4));
  }

  const uint materialIndex = CBasics::SwapBytes(*reinterpret_cast< const uint* >(data + 0xc));
  const uint displayListSize = CBasics::SwapBytes(*reinterpret_cast< const uint* >(data + 0x10));
  const uint extraSize = CBasics::SwapBytes(*reinterpret_cast< const uint* >(data + 0x1c));
  float bounds[6];
#ifdef TARGET_PC
  uint portWords[kPortWords];
  bool hasPortWords = false;
  if (extraSize >= kPortExtraSize) {
    memcpy(portWords, data + kPortDiscOffset, sizeof(portWords));
    for (uint i = 0; i < kPortWords; ++i) {
      portWords[i] = CBasics::SwapBytes(portWords[i]);
    }
    hasPortWords = portWords[0] == kPortMagic;
  }
#endif
  if (extraSize != 0) {
    for (int i = 0; i < 6; ++i) {
      bounds[i] = CBasics::SwapBytes(*reinterpret_cast< const float* >(data + 0x2c + i * 4));
    }
  }

  SSurfaceData* surface = static_cast< SSurfaceData* >(rawData);
  memcpy(&surface->mCenter, center, sizeof(center));
  surface->mMaterialIndex = materialIndex;
  surface->mDisplayListSizeAndNormalHint = displayListSize;
  surface->mExtraSize = extraSize;
  memcpy(&surface->mNormal, normal, sizeof(normal));
  if (extraSize != 0) {
    memcpy(&surface->mBounds, bounds, sizeof(bounds));
  }
#ifdef TARGET_PC
  if (hasPortWords) {
    memcpy(data + kPortNativeOffset, portWords, sizeof(portWords));
  }
#endif
#endif
}

#ifdef TARGET_PC
bool CCubeSurface::PortBaseIndices(uint* out) const {
  if (x0_data->mExtraSize < kPortExtraSize) {
    return false;
  }
  uint words[kPortWords];
  memcpy(words, x0_rawdata + kPortNativeOffset, sizeof(words));
  if (words[0] != kPortMagic) {
    return false;
  }
  memcpy(out, words + 1, kPB_Count * sizeof(uint));
  return true;
}

// Port: a static world model's surface draws as cached geometry: the FIFO processor
// reads the display list where it lives and, once the cache is in, keeps the resolved
// vertices on the GPU across frames. MP_GEOMETRY_CACHE=0 draws every surface the plain way.
void CCubeSurface::PortCallDisplayList() const {
  static const bool enabled = [] {
    const char* value = getenv("MP_GEOMETRY_CACHE");
    return value == nullptr || value[0] != '0';
  }();
  const CCubeModel* parent = x0_data->mParent;
  if (enabled && parent != nullptr && parent->PortCacheableGeometry()) {
    CGX::CallCachedDisplayList(parent->PortGeometrySet(), GetDisplayList(), GetDisplayListSize());
  } else {
    CGX::CallDisplayList(GetDisplayList(), GetDisplayListSize());
  }
}

void CCubeSurface::CallDisplayList() const {
  uint bases[kPB_Count];
  if (!PortBaseIndices(bases)) {
    PortCallDisplayList();
    return;
  }
  // The bases stay set in Aurora until changed, so reset them after the draw:
  // every other indexed draw assumes 0.
  const uint tex0 = CCubeModel::IsUsingPackedLightmaps() ? bases[kPB_PackedUV] : bases[kPB_UV];
  GXSetArrayBaseIndex(GX_VA_POS, bases[kPB_Pos]);
  GXSetArrayBaseIndex(GX_VA_NRM, bases[kPB_Nrm]);
  GXSetArrayBaseIndex(GX_VA_CLR0, bases[kPB_Clr]);
  GXSetArrayBaseIndex(GX_VA_TEX0, tex0);
  for (int i = GX_VA_TEX1; i <= GX_VA_TEX7; ++i) {
    GXSetArrayBaseIndex(static_cast< GXAttr >(i), bases[kPB_UV]);
  }
  PortCallDisplayList();
  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    if (i != GX_VA_CLR1) {
      GXSetArrayBaseIndex(static_cast< GXAttr >(i), 0);
    }
  }
}
#else
void CCubeSurface::CallDisplayList() const {
  CGX::CallDisplayList(GetDisplayList(), GetDisplayListSize());
}
#endif

CAABox CCubeSurface::GetBounds() const {
  if (x0_data->mExtraSize != 0) {
    return x0_data->mBounds;
  }

  return CAABox(x0_data->mCenter, x0_data->mCenter);
}

#include "Kyoto/Graphics/CCubeModel.hpp"
#include "Kyoto/Basics/CBasics.hpp"
#include <cstring>
#include <stdexcept>
#include <string>

#include "Kyoto/Graphics/CCubeSurface.hpp"
#include "Kyoto/Graphics/CGX.hpp"
#include "Kyoto/Graphics/CGX_Impl.hpp" // IWYU pragma: keep
#include "Kyoto/Graphics/CGraphics.hpp"
#include "dolphin/gx/GXVert.h"
#ifdef TARGET_PC
#include <dolphin/gx/GXExtra.h>
#include "port_room_env.h"
#include <vector>
#endif

static bool sDrawingOccluders = false;
static bool sDrawingWireframe = false;
bool CCubeModel::sUsingPackedLightmaps = false;

inline uint GetMaterialOffset(const uchar* materialData, const int idx) {
  materialData += (idx * 4);
  return CBasics::SwapBytes(*reinterpret_cast< const uint* >(materialData - 4));
}

CCubeModel::CCubeModel(rstl::vector< void* >* surfaces,
                       rstl::vector< TCachedToken< CTexture > >* textures, const void* materialData,
                       const void* positions, const void* normals, const void* colors,
                       const void* uvs, const void* compressedUvs, const CAABox& bounds,
                       const uchar visorFlags, const bool texturesLoaded, const uint idx,
                       const uint positionsSize, const uint normalsSize, const uint colorsSize,
                       const uint texCoordsSize, const uint packedTexCoordsSize)
: x0_instance(*surfaces, materialData, positions, normals, colors, uvs, compressedUvs,
              positionsSize, normalsSize, colorsSize, texCoordsSize, packedTexCoordsSize)
, x1c_textures(textures)
, x20_bounds(bounds)
, x38_firstUnsorted(nullptr)
, x3c_firstSorted(nullptr)
, x40_24_loadTextures(static_cast< uchar >(!texturesLoaded))
, x40_25_visible(false)
, x41_visorFlags(visorFlags)
, x44_idx(idx) {
  rstl::vector< void* >& surf = x0_instance.Surfaces();
  for (AUTO(it, surf.begin()); it != surf.end(); ++it) {
    CCubeSurface::SSurfaceData* data = static_cast< CCubeSurface::SSurfaceData* >(*it);
    data->mParent = this;
  }

  for (int i = surf.size(); i > 0; i--) {
    void*& data = surf[i - 1];
    uint materialIndex = static_cast< CCubeSurface::SSurfaceData* >(data)->mMaterialIndex;
    if (GetMaterialByIndex(materialIndex).IsFlagSet(kStateFlag_DepthSorting)) {
      static_cast< CCubeSurface::SSurfaceData* >(data)->mNextSurface = x3c_firstSorted.x0_rawdata;
      x3c_firstSorted.x0_rawdata = static_cast< uchar* >(data);
    } else {
      static_cast< CCubeSurface::SSurfaceData* >(data)->mNextSurface = x38_firstUnsorted.x0_rawdata;
      x38_firstUnsorted.x0_rawdata = static_cast< uchar* >(data);
    }
  }
}

void CCubeModel::MakeTexturesFromMats(const void* data,
                                      rstl::vector< TCachedToken< CTexture > >& textures,
                                      IObjectStore& store, const bool cache) {
  const uint* textureIds = static_cast< const uint* >(data);
  const uint textureCount = CBasics::SwapBytes(*static_cast< const int* >(data));
  textureIds++;
  textures.reserve(textureCount);

  for (int i = 0; i < textureCount; i++) {
    textures.push_back(store.GetObj(SObjectTag('TXTR', CBasics::SwapBytes(*textureIds))));
    if (!cache) {
      textures.back().ForceCache();
    }
    ++textureIds;
  }
}

void CCubeModel::SetStaticArraysCurrent() const {
  CGX::SetArray(GX_VA_CLR0, x0_instance.GetColorPointer(), x0_instance.GetColorSize(),
                sizeof(CColor));
  const void* packed = x0_instance.GetPackedTCPointer();
  const void* unpacked = x0_instance.GetTCPointer();
  if (!packed) {
    sUsingPackedLightmaps = false;
  }

  if (sUsingPackedLightmaps) {
    CGX::SetArray(GX_VA_TEX0, packed, x0_instance.GetPackedTCSize(), sizeof(ushort) * 2);
  } else {
    CGX::SetArray(GX_VA_TEX0, unpacked, x0_instance.GetTCSize(), sizeof(CVector2f));
  }

  if (unpacked) {
    for (int i = 1; i <= GX_VA_TEX7 - GX_VA_TEX0; ++i) {
      CGX::SetArray(static_cast< GXAttr >(i + GX_VA_TEX0), unpacked, x0_instance.GetTCSize(),
                    sizeof(CVector2f));
    }
  }

  CCubeMaterial::KillCachedViewDepState();
}

void CCubeModel::SetArraysCurrent() const {
#ifdef TARGET_PC
  mPortOwnArrays = true;
#endif
  CGX::SetArray(GX_VA_POS, x0_instance.GetVertexPointer(), x0_instance.GetVertexSize(),
                sizeof(CVector3f));
  const int stride = (x41_visorFlags & 1) ? sizeof(short) * 3 : sizeof(CVector3f);
  CGX::SetArray(GX_VA_NRM, x0_instance.GetNormalPointer(), x0_instance.GetNormalSize(), stride);
  SetStaticArraysCurrent();
}

void CCubeModel::SetSkinningArraysCurrent(const float* positions, const float* normals) const {
  // Port: CSkinnedModel's callback draws hand a single-bone (or unskinned) model
  // its own file arrays, which are big-endian (and may hold short normals).
  // Uploading them as native floats exploded the vertices (thermal pickups).
  if (positions == x0_instance.GetVertexPointer()) {
    SetArraysCurrent();
    return;
  }
  // The skinned workspaces reuse the same pointer each frame, so force the
  // backend to drop its cached copy or the new vertex data is never uploaded.
#ifdef TARGET_PC
  mPortOwnArrays = false;
#endif
  CGX::ClearArray(GX_VA_POS);
  CGX::ClearArray(GX_VA_NRM);
  CGraphics::sRenderState.SetVtxState(positions, normals,
                                      static_cast< const uint* >(x0_instance.GetColorPointer()));
  SetStaticArraysCurrent();
}

void CCubeModel::SetUsingPackedLightmaps(const bool use) const {
  sUsingPackedLightmaps = use;
  if (sUsingPackedLightmaps) {
    CGX::SetArray(GX_VA_TEX0, x0_instance.GetPackedTCPointer(), x0_instance.GetPackedTCSize(),
                  sizeof(ushort) * 2);
  } else {
    CGX::SetArray(GX_VA_TEX0, x0_instance.GetTCPointer(), x0_instance.GetTCSize(),
                  sizeof(CVector2f));
  }
}

CCubeMaterial CCubeModel::GetMaterialByIndex(const int idx) const {
  uint materialCount = 0;
  uint materialOffset = 0;
  const uchar* materialData = static_cast< const uchar* >(x0_instance.GetMaterialPointer()) +
                              (x1c_textures->size() + 1) * 4;
  materialCount = *reinterpret_cast< const uint* >(materialData++);
  materialCount = CBasics::SwapBytes(materialCount);
  if (idx < 0 || static_cast<uint>(idx) >= materialCount) {
    throw std::runtime_error("Surface material index " + std::to_string(idx) +
                             " exceeds material count " + std::to_string(materialCount));
  }
  materialData++;
  materialData++;
  materialData++;
  if (idx != 0) {
    materialOffset = GetMaterialOffset(materialData, idx);
  }

  materialData += (materialCount * 4);
  materialData += materialOffset;
  return CCubeMaterial(materialData);
}

#ifdef TARGET_PC
// Port: the material record a converter may append to a kStateFlag_PortPBR material, read
// back from the material's end so that nothing retail parses moves: six big-endian floats
// (emissive multiplier rgb, backlight weight rgb) and the tag 'PBRM', or eight (the same,
// then the height blend threshold and the shading mode) and 'PBR2', or thirteen (the same,
// then a second layer's edge width and the scale and offset of each layer's height) and
// 'PBR3', or nineteen (the same, then the kind of a special surface, its strength and four
// parameters; see GXSetPBRMaterial) and 'PBR4'. A material without one gets the neutral
// values.
int CCubeModel::PortReadPBRMaterial(const int idx, f32 values[19]) const {
  for (int i = 0; i < 19; ++i) {
    values[i] = i < 3 ? 1.f : 0.f;
  }
  const uchar* table = static_cast< const uchar* >(x0_instance.GetMaterialPointer()) +
                       (x1c_textures->size() + 1) * 4;
  const uint count = CBasics::SwapBytes(*reinterpret_cast< const uint* >(table));
  table += 4;
  const uint begin = idx != 0 ? GetMaterialOffset(table, idx) : 0;
  const uint end = GetMaterialOffset(table, idx + 1);
  const uchar* materialEnd = table + count * 4 + end;
  int floats = 0;
  if (end >= begin + 80 && memcmp(materialEnd - 4, "PBR4", 4) == 0) {
    floats = 19;
  } else if (end >= begin + 56 && memcmp(materialEnd - 4, "PBR3", 4) == 0) {
    floats = 13;
  } else if (end >= begin + 36 && memcmp(materialEnd - 4, "PBR2", 4) == 0) {
    floats = 8;
  } else if (end >= begin + 28 && memcmp(materialEnd - 4, "PBRM", 4) == 0) {
    floats = 6;
  }
  const uchar* record = materialEnd - 4 - floats * 4;
  for (int i = 0; i < floats; ++i) {
    uint bits;
    memcpy(&bits, record + i * 4, 4);
    bits = CBasics::SwapBytes(bits);
    memcpy(&values[i], &bits, 4);
  }
  return floats;
}

uint CCubeModel::PortMaterialCount() const {
  const uchar* table = static_cast< const uchar* >(x0_instance.GetMaterialPointer()) +
                       (x1c_textures->size() + 1) * 4;
  return CBasics::SwapBytes(*reinterpret_cast< const uint* >(table));
}

// Values a debugging session puts in place of a record's (the console's `roomgeo mat`).
namespace {
struct SPortPBROverride {
  const CCubeModel* model;
  int material;
  int field;
  f32 value;
};
std::vector< SPortPBROverride > sPortPBROverrides;
} // namespace

void CCubeModel::PortOverridePBR(const CCubeModel* model, const int material, const int field,
                                 const f32 value) {
  if (model == nullptr || field < 0 || field >= 19) {
    return;
  }
  for (SPortPBROverride& entry : sPortPBROverrides) {
    if (entry.model == model && entry.material == material && entry.field == field) {
      entry.value = value;
      return;
    }
  }
  const SPortPBROverride entry = {model, material, field, value};
  sPortPBROverrides.push_back(entry);
}

void CCubeModel::PortClearPBROverrides() { sPortPBROverrides.clear(); }

void CCubeModel::PortSetPBRMaterial(const int idx) const {
  f32 values[19];
  PortReadPBRMaterial(idx, values);
  for (const SPortPBROverride& entry : sPortPBROverrides) {
    if (entry.model == this && entry.material == idx) {
      values[entry.field] = entry.value;
    }
  }
  // A liquid's surface (kinds 5 and 6) moves: its first parameter is a rate, and the
  // shader gets the phase. So does falling water (kind 7); glass (8) does not move.
  if (values[13] > 4.5f && values[13] < 7.5f) {
    values[15] *= CGraphics::GetSecondsMod900();
  }
  // World up as the shader sees it: view space is right, up, -forward.
  const CTransform4f& view = CGraphics::GetViewMatrix();
  const f32 up[3] = {view.Get20(), view.Get22(), -view.Get21()};
  GXSetPBRMaterial(values, values + 3, values[6], values[7], values + 8, values + 13, up);
}
#endif

void CCubeModel::DrawSurface(const CCubeSurface& surface, const CModelFlags& modelFlags) const {
  const CCubeMaterial material = GetMaterialByIndex(surface.GetMaterialIndex());
  if (material.IsFlagSet(kStateFlag_ShadowOccluderMesh) && !sDrawingOccluders) {
    return;
  }

#ifdef TARGET_PC
  // Port: an alpha blend at full, untinted alpha draws as opaque, so a PBR material takes it
  // (the arm cannon is always drawn alpha blended for its fade). The PBR shader's alpha is
  // the base map's, which a blend would show through; TEV materials keep the retail path.
  const CModelFlags opaqueFlags(CModelFlags::kT_Opaque, static_cast< uchar >(modelFlags.GetShaderSet()),
                                static_cast< CModelFlags::EFlags >(modelFlags.GetOtherFlags()),
                                modelFlags.GetColorRef());
  const bool solidBlend = modelFlags.GetTrans() == CModelFlags::kT_Blend &&
                          modelFlags.GetColorRef() == CColor::White() &&
                          material.IsFlagSet(kStateFlag_PortPBR) &&
                          CCubeMaterial::PortPBRAllowed(opaqueFlags);
  const CModelFlags& drawFlags = solidBlend ? opaqueFlags : modelFlags;
  material.SetCurrent(drawFlags, surface, *this);
  // Port: PBR mod materials. The fallback TEV set above stays valid for the
  // paths PortPBRAllowed rejects.
  const bool pbr =
      material.IsFlagSet(kStateFlag_PortPBR) && CCubeMaterial::PortPBRAllowed(drawFlags);
#else
  material.SetCurrent(modelFlags, surface, *this);
#endif
#ifdef TARGET_PC
  if (pbr) {
    // The probe is in world space with Y and Z swapped (world Z-up to cube Y-up), and the
    // shader's reflection vector is in view space: right, up, -forward.
    const CTransform4f& view = CGraphics::GetViewMatrix();
    const f32 viewToWorld[3][3] = {
        {view.Get00(), view.Get02(), -view.Get01()},
        {view.Get10(), view.Get12(), -view.Get11()},
        {view.Get20(), view.Get22(), -view.Get21()},
    };
    // A room environment from a mod has a cube for where the model stands, which replaces
    // the live probe; it needs no captures, so only `probe off` turns it off.
    const CVector3f origin = CGraphics::GetModelMatrix().GetTranslation();
    const float pos[3] = {origin.GetX(), origin.GetY(), origin.GetZ()};
    PortRoomEnv::Selection env;
    const int mode = CCubeMaterial::sPortPBRProbeMode;
    const bool found = PortRoomEnv::Select(pos, env);
    if (mode != 0 && found && env.cube != 0) {
      f32 viewToCube[3][3];
      for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
          viewToCube[row][col] = env.worldToCube[row * 3] * viewToWorld[0][col] +
                                 env.worldToCube[row * 3 + 1] * viewToWorld[1][col] +
                                 env.worldToCube[row * 3 + 2] * viewToWorld[2][col];
        }
      }
      GXSetPBRProbe(viewToCube, mode > 1 ? static_cast< float >(mode) : 1.f);
      GXSetPBRCube(env.cube, env.params);
    } else {
      const f32 viewToProbe[3][3] = {
          {viewToWorld[0][0], viewToWorld[0][1], viewToWorld[0][2]},
          {viewToWorld[2][0], viewToWorld[2][1], viewToWorld[2][2]},
          {viewToWorld[1][0], viewToWorld[1][1], viewToWorld[1][2]},
      };
      static const f32 kNoCube[4] = {0.f, 0.f, 0.f, 0.f};
      GXSetPBRProbe(viewToProbe, CCubeMaterial::sPortPBRProbeWeight);
      GXSetPBRCube(0, kNoCube);
    }
    if (found && env.hasAmbient) {
      // The baked ambient's directions are in world space, the shader's normal in view space.
      f32 rows[6][3];
      memcpy(rows, env.ambient, sizeof(rows));
      for (int row = 3; row < 6; ++row) {
        for (int col = 0; col < 3; ++col) {
          rows[row][col] = env.ambient[row][0] * viewToWorld[0][col] + env.ambient[row][1] * viewToWorld[1][col] +
                           env.ambient[row][2] * viewToWorld[2][col];
        }
      }
      GXSetPBRAmbient(rows, env.ambientAbsolute ? 2.f : 1.f);
    } else {
      GXSetPBRAmbient(nullptr, 0.f);
    }
    if (found && env.volume != 0) {
      // The volume is in world space, the shader's position and normal in view space.
      const CVector3f eye = view.GetTranslation();
      const f32 at[3] = {eye.GetX(), eye.GetY(), eye.GetZ()};
      f32 rows[6][4];
      for (int row = 0; row < 3; ++row) {
        const f32* w = env.worldToVolume + row * 4;
        const f32* a = env.worldToAxes + row * 3;
        for (int col = 0; col < 3; ++col) {
          rows[row][col] = w[0] * viewToWorld[0][col] + w[1] * viewToWorld[1][col] + w[2] * viewToWorld[2][col];
          rows[3 + row][col] = a[0] * viewToWorld[0][col] + a[1] * viewToWorld[1][col] + a[2] * viewToWorld[2][col];
        }
        rows[row][3] = w[0] * at[0] + w[1] * at[1] + w[2] * at[2] + w[3];
      }
      rows[3][3] = env.volumeLevel;
      rows[4][3] = env.volumeBias;
      rows[5][3] = env.volumeDiagnostic;
      GXSetPBRVolume(env.volume, rows);
    } else {
      GXSetPBRVolume(0, nullptr);
    }
    // The frame's tone curve, when rooms are exposed as Remastered exposes them.
    f32 tone[3][4];
    GXSetPBRTone(PortRoomEnv::Tone(tone) ? tone : nullptr);
    PortSetPBRMaterial(surface.GetMaterialIndex());
    // Glass (kind 8) sees what is behind it: the screen so far, copied into map 7 as the
    // refracting particles copy it (CElementGen).
    f32 record[19];
    PortReadPBRMaterial(surface.GetMaterialIndex(), record);
    if (record[13] > 7.5f && record[13] < 8.5f && CCubeMaterial::PortScreenCopyUsed()) {
      int portLeft, portTop, portWidth, portHeight;
      CGraphics::GetViewport(portLeft, portTop, portWidth, portHeight);
      GXSetTexCopySrc(static_cast< u16 >(portLeft), static_cast< u16 >(portTop), static_cast< u16 >(portWidth),
                      static_cast< u16 >(portHeight));
      GXSetTexCopyDst(static_cast< u16 >(portWidth), static_cast< u16 >(portHeight), GX_TF_RGB565, GX_FALSE);
      const bool useVideoFilter = CGraphics::GetUseVideoFilter();
      CGraphics::SetUseVideoFilter(false);
      GXCopyTex(CGraphics::GetDolphinSpareBuffer(), GX_FALSE);
      CGraphics::SetUseVideoFilter(useVideoFilter);
      GXPixModeSync();
      CGraphics::LoadDolphinSpareTexture(portWidth, portHeight, GX_TF_RGB565, nullptr,
                                         CGraphics::kSpareBufferTexMapID);
    }
    GXSetPBR(GX_TRUE);
    ++CCubeMaterial::sPortPBRDraws;
  }
  if (CCubeMaterial::sPortPBRThermal == CCubeMaterial::kPT_Additive) {
    // As CFluidPlaneCPU::RenderSetup in the thermal visor's hot pass (a TEV fallback too).
    CGX::SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
    CGX::SetZMode(true, GX_LEQUAL, false);
  }
#endif
  surface.CallDisplayList();
#ifdef TARGET_PC
  if (pbr) {
    GXSetPBR(GX_FALSE);
  }
#endif
}

static inline const ushort ReadWireframeIndex(const uchar* data) {
  uchar bytes[2];
  bytes[0] = data[0];
  bytes[1] = data[1];
#ifdef __MWERKS__
  return CBasics::SwapBytes(*reinterpret_cast< const ushort* >(bytes));
#else
  ushort value;
  memcpy(&value, bytes, sizeof(value));
  return CBasics::SwapBytes(value);
#endif
}

void CCubeModel::DrawSurfaceWireframe(const CCubeSurface& surface) const {
  const CCubeMaterial material = GetMaterialByIndex(surface.GetMaterialIndex());

  static uint sLastDesc = 0;
  static uint sAttrCount = 0;
  uint vertexAttributes = material.GetVertexDesc();

  if (vertexAttributes != sLastDesc) {
    sAttrCount = 0;
    for (int i = 0; i < 16; ++i, sLastDesc = vertexAttributes) {
      if ((vertexAttributes >> (i * 2)) & 3) {
        sAttrCount++;
      }
    }
  }

  const int attrCountTimes2 = sAttrCount * 2;
  static const GXVtxDescList sDesc[] = {
      {GX_VA_POS, GX_INDEX16},
      {GX_VA_NULL, GX_NONE},
  };
  CGX::SetVtxDescv(sDesc);
  CGX::SetTevDirect(GX_TEVSTAGE0);
  CGX::SetNumIndStages(0);
  CGX::SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ONE);
  CGX::SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
  CGX::SetNumChans(0);
  CGX::SetNumTexGens(1);
  CGX::SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);

  const int displayListSize = surface.GetDisplayListSize();
  const uchar* dispList = static_cast< const uchar* >(surface.GetDisplayList());
  for (int bytesRead = 0; bytesRead < displayListSize;) {
    const uchar pType = *dispList & 0xf8;
    if (!pType) {
      break;
    }
    bytesRead += 3;
    ushort elementCount = ReadWireframeIndex(dispList + 1);
    dispList += 3;
    if (elementCount < 3U) {
      break;
    }

    CGX::Begin(GX_LINESTRIP, GX_VTXFMT0, 4);
    GXPosition1x16(ReadWireframeIndex(dispList));
    GXPosition1x16(ReadWireframeIndex(dispList + attrCountTimes2));
    GXPosition1x16(ReadWireframeIndex(dispList + attrCountTimes2 * 2));
    GXPosition1x16(ReadWireframeIndex(dispList));
    bytesRead += elementCount * attrCountTimes2;
    dispList += attrCountTimes2 * 3;
    CGX::End();
    if (pType == GX_TRIANGLES) {
      elementCount -= 3;
      for (int j = 0; j < elementCount; j += 3) {
        CGX::Begin(GX_LINESTRIP, GX_VTXFMT0, 4);
        GXPosition1x16(ReadWireframeIndex(dispList));
        GXPosition1x16(ReadWireframeIndex(dispList + attrCountTimes2));
        GXPosition1x16(ReadWireframeIndex(dispList + attrCountTimes2 * 2));
        GXPosition1x16(ReadWireframeIndex(dispList));
        dispList += attrCountTimes2 * 3;
        CGX::End();
      }
    } else if (pType == GX_TRIANGLESTRIP) {
      elementCount -= 3;
      uint winding = 1;
      for (int j = 0; j < elementCount; ++j) {
        CGX::Begin(GX_LINESTRIP, GX_VTXFMT0, 3);
        const uchar* last = dispList - attrCountTimes2 * ((winding ^ 1) + 1);
        const uchar* first = dispList - attrCountTimes2 * (winding + 1);
        winding ^= 1;
        GXPosition1x16(ReadWireframeIndex(first));
        GXPosition1x16(ReadWireframeIndex(dispList));
        dispList += attrCountTimes2;
        GXPosition1x16(ReadWireframeIndex(last));
        CGX::End();
      }
    } else {
      if (pType != GX_TRIANGLEFAN) {
        return;
      }
      elementCount -= 3;
      const uchar* indices = dispList - attrCountTimes2 * 3;

      for (int j = 0; j < elementCount; ++j) {
        const uchar* previous = dispList - attrCountTimes2;
        CGX::Begin(GX_LINESTRIP, GX_VTXFMT0, 3);
        GXPosition1x16(ReadWireframeIndex(previous));
        GXPosition1x16(ReadWireframeIndex(dispList));
        dispList += attrCountTimes2;
        GXPosition1x16(ReadWireframeIndex(indices));
        CGX::End();
      }
    }
  }
}

bool CCubeModel::TryLockTextures() const {
  if (!x40_24_loadTextures) {
    bool texturesLoading = false;
    for (int i = 0; i < GetTextures().size(); ++i) {
      GetTextures()[i].Lock();
      if (!GetTextures()[i].TryCache()) {
        texturesLoading = true;
      } else if (!GetTextures()[i].GetObject()->LoadToMRAM()) {
        texturesLoading = true;
      }
    }

    if (!texturesLoading) {
      x40_24_loadTextures = true;
    }
  }

  return !!x40_24_loadTextures;
}

void CCubeModel::DrawSurfaces(const CModelFlags& flags) const {
  if (sDrawingWireframe) {
    for (CCubeSurface surface = GetNormalSurfaces(); surface.IsValid();
         surface = surface.GetNextSurface()) {
      DrawSurfaceWireframe(surface);
    }
    for (CCubeSurface surface = GetAlphaSurfaces(); surface.IsValid();
         surface = surface.GetNextSurface()) {
      DrawSurfaceWireframe(surface);
    }
  } else if ((flags.GetOtherFlags() & CModelFlags::kF_NoTextureLock) || TryLockTextures()) {
    for (CCubeSurface surface = GetNormalSurfaces(); surface.IsValid();
         surface = surface.GetNextSurface()) {
      DrawSurface(surface, flags);
    }
    for (CCubeSurface surface = GetAlphaSurfaces(); surface.IsValid();
         surface = surface.GetNextSurface()) {
      DrawSurface(surface, flags);
    }
  }
}

void CCubeModel::DrawNormalSurfaces(const CModelFlags& flags) const {
  if (sDrawingWireframe) {
    for (CCubeSurface surface = GetNormalSurfaces(); surface.IsValid();
         surface = surface.GetNextSurface()) {
      DrawSurfaceWireframe(surface);
    }
  } else if (TryLockTextures()) {
    for (CCubeSurface surface = GetNormalSurfaces(); surface.IsValid();
         surface = surface.GetNextSurface()) {
      DrawSurface(surface, flags);
    }
  }
}

void CCubeModel::DrawAlphaSurfaces(const CModelFlags& flags) const {
  if (sDrawingWireframe) {
    for (CCubeSurface surface = GetAlphaSurfaces(); surface.IsValid();
         surface = surface.GetNextSurface()) {
      DrawSurfaceWireframe(surface);
    }
  } else if (TryLockTextures()) {
    for (CCubeSurface surface = GetAlphaSurfaces(); surface.IsValid();
         surface = surface.GetNextSurface()) {
      DrawSurface(surface, flags);
    }
  }
}

void CCubeModel::DrawFlat(const float* positions, const float* normals,
                          ESurfaceSelection which) const {
  if (positions != nullptr) {
    SetSkinningArraysCurrent(positions, normals);
  } else {
    SetArraysCurrent();
  }

  if (which != kSS_Sorted) {
    for (CCubeSurface surface = x38_firstUnsorted; surface.IsValid();
         surface = surface.GetNextSurface()) {
      CCubeMaterial material = GetMaterialByIndex(surface.GetMaterialIndex());
      CGX::SetVtxDescv_Compressed(material.GetVertexDescLwzx());
      surface.CallDisplayList();
    }
  }

  if (which != kSS_Unsorted) {
    for (CCubeSurface surface = x3c_firstSorted; surface.IsValid();
         surface = surface.GetNextSurface()) {
      CCubeMaterial material = GetMaterialByIndex(surface.GetMaterialIndex());
      CGX::SetVtxDescv_Compressed(material.GetVertexDescLwzx());
      surface.CallDisplayList();
    }
  }
}

void CCubeModel::Draw(const CModelFlags& flags) const {
  CCubeMaterial::KillCachedViewDepState();
  SetArraysCurrent();
  DrawSurfaces(flags);
}

void CCubeModel::Draw(const float* positions, const float* normals,
                      const CModelFlags& flags) const {
  CCubeMaterial::KillCachedViewDepState();
  SetSkinningArraysCurrent(positions, normals);
  DrawSurfaces(flags);
}

void CCubeModel::DrawNormal(const CModelFlags& flags) const {
  CCubeMaterial::KillCachedViewDepState();
  SetArraysCurrent();
  DrawNormalSurfaces(flags);
}

void CCubeModel::DrawAlpha(const CModelFlags& flags) const {
  CCubeMaterial::KillCachedViewDepState();
  SetArraysCurrent();
  DrawAlphaSurfaces(flags);
}

void CCubeModel::SetDrawingOccluders(const bool drawOccluders) {
  sDrawingOccluders = drawOccluders;
}

void CCubeModel::SetModelWireframe(const bool drawWireframe) { sDrawingWireframe = drawWireframe; }

void CCubeModel::UnlockTextures() const {
  for (AUTO(texture, x1c_textures->begin()); texture != x1c_textures->end(); ++texture) {
    texture->Unlock();
  }

  x40_24_loadTextures = false;
}

void CCubeModel::RemapMaterialData(const void* data,
                                   rstl::vector< TCachedToken< CTexture > >* texture) {

  x0_instance.SetMaterialPointer(data);
  x1c_textures = texture;
  x40_24_loadTextures = false;
}

void CCubeModel::DrawNormal(const float* positions, const float* normals,
                            ESurfaceSelection which) const {
  CGX::SetNumIndStages(0);
  CGX::SetNumTevStages(1);
  CGX::SetNumTexGens(1);
  CGX::SetZMode(true, GX_LEQUAL, true);
  CGX::SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
  CGX::SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
  CGX::SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
  CGX::SetStandardTevColorAlphaOp(GX_TEVSTAGE0);
  CGX::SetBlendMode(GX_BM_BLEND, GX_BL_ZERO, GX_BL_ONE, GX_LO_CLEAR);
  DrawFlat(positions, normals, which);
}

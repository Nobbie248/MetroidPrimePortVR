#ifndef _CCUBESURFACE
#define _CCUBESURFACE

#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/Math/CUnitVector3f.hpp"

class CCubeModel;
class CCubeSurface {
public:
  CCubeSurface(void* ptr) { x0_rawdata = static_cast< uchar* >(ptr); }
#pragma pack(push, 1)
  struct SSurfaceData {
    CVector3f mCenter;
    uint mMaterialIndex;
    uint mDisplayListSizeAndNormalHint;
    CCubeModel* mParent;
    void* mNextSurface;
    uint mExtraSize;
    CUnitVector3f mNormal;
    CAABox mBounds;
    uchar pad[7];
  };
#pragma pack(pop)

  static const CVector3f skDefaultNormal;
  static void ConvertSurfaceHeader(void* data);
  union {
    uchar* x0_rawdata;
    SSurfaceData* x0_data;
  };

  uint GetDisplayListSize() const { return x0_data->mDisplayListSizeAndNormalHint & 0x7fffffff; }
  const void* GetDisplayList() const {
    return reinterpret_cast< const SSurfaceData* >(x0_rawdata + GetSurfaceHeaderSize());
  }
  uint GetSurfaceHeaderSize() const { return (0x4b + x0_data->mExtraSize) & ~31; }
  const CVector3f& GetCenter() const { return x0_data->mCenter; }
  const CUnitVector3f& GetNormalHint() const { return x0_data->mNormal; }
  uint GetMaterialIndex() const { return x0_data->mMaterialIndex; }

  CAABox GetBounds() const;
  CCubeSurface GetNextSurface() const { return CCubeSurface(x0_data->mNextSurface); }

  bool IsValid() const { return x0_rawdata != nullptr; }

  // Port: calls the display list; on PC also applies the surface's vertex
  // array base indices (see PortBaseIndices), so models with more than 65536
  // vertices can keep 16-bit display-list indices.
  void CallDisplayList() const;
#ifdef TARGET_PC
  // Port: a centre-only surface cannot safely participate in box culling.
  bool HasBounds() const { return x0_data->mExtraSize >= sizeof(CAABox); }
  // Port: the display list call itself, as cached geometry when the parent model
  // allows it (CCubeModel::PortCacheableGeometry).
  void PortCallDisplayList() const;
  enum EPortBase { kPB_Pos, kPB_Nrm, kPB_Clr, kPB_UV, kPB_PackedUV, kPB_Count };
  // Surface extra data after the bounds: 'PBIX' then kPB_Count big-endian u32
  // base indices, in an extra block of at least 0x40 bytes. Returns false for
  // retail surfaces.
  bool PortBaseIndices(uint* out) const;
#endif

private:
};
#endif // _CCUBESURFACE

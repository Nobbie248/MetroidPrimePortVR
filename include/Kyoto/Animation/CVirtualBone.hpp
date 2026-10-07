#ifndef _CVIRTUALBONE
#define _CVIRTUALBONE

#include "Kyoto/Animation/CSegId.hpp"
#include "Kyoto/Math/CMatrix3f.hpp"
#include "Kyoto/Math/CTransform4f.hpp"

struct SSkinWeighting {
  CSegId x0_id;
  float x4_weight;
  explicit SSkinWeighting(CInputStream& in)
  : x0_id(in.Get< int >()), x4_weight(in.Get< float >()) {}
};

// Retail data never has more than three influences a vertex. The PC build holds four so
// converted Remastered models keep theirs; the GameCube layout is unchanged.
#ifdef TARGET_PC
#define SKIN_MAX_WEIGHTS 4
#else
#define SKIN_MAX_WEIGHTS 3
#endif

class CPoseAsTransforms;
class CVirtualBone {
public:
  explicit CVirtualBone(CInputStream& in);
  const rstl::reserved_vector< SSkinWeighting, SKIN_MAX_WEIGHTS >& GetWeights() const {
    return x0_weights;
  }
  int GetNumIndices() const { return x1c_vertexCount; }
  const CTransform4f& GetTransform() const { return x20_xf; }
  const CMatrix3f& GetRotation() const { return x50_rotation; }

  void BuildPoints(const ushort*, volatile void*, int) const;
  void BuildNormals(const ushort*, volatile void*, int) const;
  void BuildNormals(const CVector3f*, CVector3f*, int) const;
  void BuildAccumulatedTransform(const CPoseAsTransforms& pose, const CVector3f* points) const;
  void BuildFinalPosMatrix(const CPoseAsTransforms& pose, const CVector3f* points) const;

private:
  rstl::reserved_vector< SSkinWeighting, SKIN_MAX_WEIGHTS > x0_weights;
  int x1c_vertexCount;
  mutable CTransform4f x20_xf;
  mutable CMatrix3f x50_rotation;
};

CHECK_SIZEOF(CVirtualBone, 0x74)

#endif // _CVIRTUALBONE

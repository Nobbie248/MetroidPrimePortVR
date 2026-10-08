#include "rstl/list.hpp"

#include "Kyoto/Animation/CSkinnedModel.hpp"

#include "Kyoto/Alloc/CMemory.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Math/CVector3f.hpp"

#ifdef TARGET_PC
#include "Kyoto/Basics/CBasics.hpp"
#include <string.h>
#endif

#include "rstl/pair.hpp"
#include "rstl/vector.hpp"

typedef rstl::pair< CVector3f, rstl::list< uint > > TPosToVertListPair;

CSkinnedModelWithAvgNormals::CSkinnedModelWithAvgNormals(const CSkinnedModel& skinnedModel)
: x0_skinnedModel(skinnedModel), x3c_avgNormals(rs_new float[skinnedModel.GetNumPoints() * 12]) {
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  const uint vertexCount = skinnedModel.GetNumPoints();
#else
  int vertexCount = skinnedModel.GetNumPoints();
#endif
#ifdef TARGET_PC
  // Port: the model keeps its position and normal arrays as the file stores
  // them, big-endian (CSkinRules::PortBuildPointsAndNormals swaps each float as
  // it skins; CCubeModel::SetSkinningArraysCurrent notes the same). Summed as
  // native floats they were garbage: most came out denormal or zero, so the
  // normalised average was 0/0 or x/0, and CVertexMorphEffect, which weights
  // each vertex of the frozen (ice beam) shell by these normals, pushed the
  // vertices of a NaN or infinite normal out to infinity. Swap them once here.
  // NBT normals are nine (N, B, T) or fifteen floats per vertex; N is the first,
  // and only N is kept, so nativeNormals holds one vector per vertex.
  const uint srcNormalVecs = skinnedModel.GetModel()->GetCubeModel()->NormalVecs();
  rstl::vector< CVector3f > nativePositions(vertexCount);
  rstl::vector< CVector3f > nativeNormals(vertexCount);
  {
    const uchar* srcPositions =
        reinterpret_cast< const uchar* >(skinnedModel.GetModel()->GetPositions());
    const uchar* srcNormals =
        reinterpret_cast< const uchar* >(skinnedModel.GetModel()->GetNormals());
    for (uint i = 0; i < vertexCount; ++i) {
      float p[3];
      float n[3];
      memcpy(p, srcPositions + i * sizeof(p), sizeof(p));
      memcpy(n, srcNormals + i * srcNormalVecs * sizeof(n), sizeof(n));
      nativePositions.push_back(CVector3f(CBasics::SwapBytes(p[0]), CBasics::SwapBytes(p[1]),
                                          CBasics::SwapBytes(p[2])));
      nativeNormals.push_back(CVector3f(CBasics::SwapBytes(n[0]), CBasics::SwapBytes(n[1]),
                                        CBasics::SwapBytes(n[2])));
    }
  }
  const CVector3f* modelPositions = nativePositions.data();
#else
  const CVector3f* modelPositions =
      reinterpret_cast< const CVector3f* >(skinnedModel.GetModel()->GetPositions());
#endif

  rstl::vector< TPosToVertListPair > vertMap;
  vertMap.reserve(vertexCount);

  for (uint vertIdx = 0; vertIdx < vertexCount; ++vertIdx) {
    bool foundEqPos = false;
    uint vopolSize = vertMap.size();
    for (uint i = 0; i < vopolSize; ++i) {
      if (vertMap[i].first.IsEqu(modelPositions[vertIdx])) {
        foundEqPos = true;
        break;
      }
    }

    if (!foundEqPos) {
      rstl::list< uint > tmpList;
      for (uint j = vertIdx; j < vertexCount; ++j) {
        if (modelPositions[j] == modelPositions[vertIdx]) {
          tmpList.push_back(j);
        }
      }
      vertMap.push_back(TPosToVertListPair(modelPositions[vertIdx], tmpList));
    }
  }

#ifdef TARGET_PC
  const CVector3f* normals = nativeNormals.data();
  // The NBT entry stride (NormalVecs) was applied in the swap above: nativeNormals
  // already holds one N per vertex.
  const uint normalStride = 1;
#else
  const CVector3f* normals =
      reinterpret_cast< const CVector3f* >(skinnedModel.GetModel()->GetNormals());
  const uint normalStride = 1;
#endif
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  CVector3f* avgNormals = reinterpret_cast< CVector3f* >(x3c_avgNormals.get());
  AUTO(mapCur, vertMap.begin());
  AUTO(mapEnd, vertMap.end());
#else
  float* avgNormals = x3c_avgNormals.get();
  TPosToVertListPair* mapCur = vertMap.xc_items;
  TPosToVertListPair* mapEnd = mapCur + vertMap.x4_count;
#endif
  for (; mapCur != mapEnd; ++mapCur) {
    CVector3f accum(0.f, 0.f, 0.f);

    AUTO(lit, mapCur->second.begin());
    AUTO(listEnd, mapCur->second.end());
    for (; lit != listEnd; ++lit) {
      accum += normals[*lit * normalStride];
    }

    lit = mapCur->second.begin();
    CVector3f normalized = accum.AsNormalized();
    for (; lit != listEnd; ++lit) {
      reinterpret_cast< CVector3f* >(avgNormals)[*lit] = normalized;
    }
  }
}

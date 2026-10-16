#include "Kyoto/Particles/CElementGen.hpp"
#include "MetroidPrime/CExplosion.hpp"

#include <cmath>
#include <cstdio>

namespace {
int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

bool Close(float a, float b) { return std::fabs(a - b) < 1e-5f; }

void TestSourceSpawnOrigin() {
  // A parent at (10,20,30), scaled by (2,3,4), with inherited T_l=(1,2,3).
  // The production origin calculation has no EMTR or PMOP inputs.
  const CVector3f globalTranslation(10.f, 20.f, 30.f);
  const CTransform4f parentScale(2.f, 0.f, 0.f, 0.f, 0.f, 3.f, 0.f, 0.f,
                                 0.f, 0.f, 4.f, 0.f);
  const CVector3f parentT(1.f, 2.f, 3.f);
  const CVector3f sourceSpawnT = parentT;
  const CVector3f parentOrigin =
      CElementGen::PortWorldFromLocal(globalTranslation, parentScale, parentT);
  const CVector3f childOrigin =
      CElementGen::PortWorldFromLocal(globalTranslation, parentScale, sourceSpawnT);
  Check(Close(parentOrigin.GetX(), 12.f) && Close(parentOrigin.GetY(), 26.f) &&
            Close(parentOrigin.GetZ(), 42.f),
        "parent's numeric system origin includes scale and translation");
  Check(Close(childOrigin.GetX(), parentOrigin.GetX()) &&
            Close(childOrigin.GetY(), parentOrigin.GetY()) &&
            Close(childOrigin.GetZ(), parentOrigin.GetZ()),
        "default inherited mode-2 source T_l resolves to the parent origin without EMTR/PMOP offsets");
}

void TestNestedLightSourceIdentity() {
  constexpr CAssetId sourceId = 0x12345678u;
  Check(CExplosion::PortNestedLightSourceId(sourceId) == sourceId,
        "nested proxies keep the explosion's asset source ID for ordinary light grouping");
}

void TestNestedLightOwnerLifecycleKey() {
  alignas(CParticleGen) char liveOwnerStorage[sizeof(CParticleGen)];
  alignas(CParticleGen) char retiredOwnerStorage[sizeof(CParticleGen)];
  const CParticleGen* liveOwner = reinterpret_cast<const CParticleGen*>(liveOwnerStorage);
  const CParticleGen* retiredOwner = reinterpret_cast<const CParticleGen*>(retiredOwnerStorage);
  const CVector3f zero = CVector3f::Zero();
  const CLight light = CLight::BuildPoint(zero, CColor::White());
  const std::vector< CElementGen::CPortNestedLightSnapshot > snapshots = {
      {liveOwner, zero, light},
  };
  Check(CElementGen::PortHasNestedLightSnapshotFor(snapshots, liveOwner),
        "the same child-generator identity keeps its existing light proxy live");
  Check(!CElementGen::PortHasNestedLightSnapshotFor(snapshots, retiredOwner),
        "a child generator absent from the current ancestry is eligible for proxy retirement");
}

void TestBoundedHdrCarrier() {
  const CVector3f zero = CVector3f::Zero();
  CLight nested = CLight::BuildCustom(zero, CVector3f(1.f, 0.f, 0.f), CColor::White(),
                                     0.f, 0.f, 0.f, 1.f, 0.f, 0.f);
  Check(nested.GetRadius() > 1.0e30f, "legacy custom-light radius remains unbounded before PHDR opt-in");

  const float sourceRgba[4] = {1.2f, 0.4f, 0.1f, 1.25f};
  CElementGen::PortSetNestedHdrCarrier(nested, sourceRgba, 20.f * 0.5f, 0.25f, 3.f);
  const float* hdr = nested.GetPortHdrColor();
  Check(Close(hdr[0], 12.f) && Close(hdr[1], 4.f) && Close(hdr[2], 1.f),
        "PBR carrier multiplies evaluated LCLR by keyed LINT without clamping");
  Check(nested.GetPortHdrR0() == 0.25f && nested.GetPortHdrR1() == 3.f &&
            nested.GetPortHdrFalloff() == CLight::kPHF_Smooth,
        "carrier uses the source .25/3 smooth distance sampler");
  Check(nested.GetRadius() == 3.f && nested.GetPosition().GetX() == 0.f &&
            nested.GetPosition().GetY() == 0.f && nested.GetPosition().GetZ() == 0.f,
        "PHDR bounds CActorLights at the outer radius and leaves actor translation to CGameLight");

  CElementGen::PortSetNestedHdrCarrier(nested, sourceRgba, 20.f, 0.25f, 3.f);
  hdr = nested.GetPortHdrColor();
  Check(Close(hdr[0], 24.f) && Close(hdr[1], 8.f) && Close(hdr[2], 2.f),
        "a later KEYE intensity evaluation updates the carrier");

  CLight ordinaryHdr = CLight::BuildCustom(zero, CVector3f(1.f, 0.f, 0.f), CColor::White(),
                                            0.f, 0.f, 0.f, 1.f, 0.f, 0.f);
  const float rgb[3] = {1.f, 1.f, 1.f};
  CElementGen::PortSetNestedHdrCarrier(ordinaryHdr, sourceRgba, 20.f, 0.25f, 3.f);
  ordinaryHdr.SetPortHdr(rgb, zero, 0.25f, 3.f, CLight::kPHF_Smooth);
  Check(ordinaryHdr.GetRadius() > 1.0e30f,
        "a later ordinary HDR assignment clears the nested-light radius override");
}
}  // namespace

int main() {
  TestSourceSpawnOrigin();
  TestNestedLightSourceIdentity();
  TestNestedLightOwnerLifecycleKey();
  TestBoundedHdrCarrier();
  if (sFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", sFailures);
    return 1;
  }
  std::puts("port_nested_light_math: ok");
  return 0;
}

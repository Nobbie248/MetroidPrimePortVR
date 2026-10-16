#include "MetroidPrime/Weapons/CGunWeapon.hpp"

#include <cstdio>

namespace {
int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

bool Near(float a, float b) { return a > b - 0.0001f && a < b + 0.0001f; }

struct CGeneratorRecorder {
  explicit CGeneratorRecorder(bool remastered)
  : remastered(remastered)
  , orientationCalls(0)
  , translationCalls(0)
  , rotation{7.f, 8.f, 9.f, 10.f, 11.f, 12.f, 13.f, 14.f, 15.f}
  , translation{-1.f, -2.f, -3.f} {}

  bool PortIsRemastered() const { return remastered; }

  void SetGlobalOrientAndTrans(const CTransform4f& xf) {
    ++orientationCalls;
    rotation[0] = xf.Get00();
    rotation[1] = xf.Get01();
    rotation[2] = xf.Get02();
    rotation[3] = xf.Get10();
    rotation[4] = xf.Get11();
    rotation[5] = xf.Get12();
    rotation[6] = xf.Get20();
    rotation[7] = xf.Get21();
    rotation[8] = xf.Get22();
    SetGlobalTranslation(xf.GetTranslation());
  }

  void SetGlobalTranslation(const CVector3f& pos) {
    ++translationCalls;
    translation[0] = pos.GetX();
    translation[1] = pos.GetY();
    translation[2] = pos.GetZ();
  }

  bool remastered;
  int orientationCalls;
  int translationCalls;
  float rotation[9];
  float translation[3];
};

void TestConvertedLocatorRotationAndChildOffset() {
  // A 90-degree X rotation with a non-zero local translation. Local +Z becomes
  // world-local -Y, so a following child's 0.25 Z offset must move by -0.25 Y.
  const CTransform4f locator(1.f, 0.f, 0.f, 1.25f,
                             0.f, 0.f, -1.f, 2.5f,
                             0.f, 1.f, 0.f, 4.f);
  CGeneratorRecorder generator(true);
  NPortChargeFxDetail::SetLocalLocator(generator, locator);

  Check(generator.orientationCalls == 1, "converted effect receives locator orientation");
  Check(generator.translationCalls == 1, "converted effect receives locator translation");
  Check(Near(generator.rotation[0], 1.f) && Near(generator.rotation[4], 0.f) &&
            Near(generator.rotation[5], -1.f) && Near(generator.rotation[7], 1.f),
        "converted effect retains the non-identity locator rotation");
  Check(Near(generator.translation[0], 1.25f) && Near(generator.translation[1], 2.5f) &&
            Near(generator.translation[2], 4.f),
        "converted effect receives the locator translation");

  constexpr float childOffset = 0.25f;
  const float childX = generator.translation[0] + generator.rotation[2] * childOffset;
  const float childY = generator.translation[1] + generator.rotation[5] * childOffset;
  const float childZ = generator.translation[2] + generator.rotation[8] * childOffset;
  Check(Near(childX, 1.25f) && Near(childY, 2.25f) && Near(childZ, 4.f),
        "converted child +Z offset follows rotated locator frame numerically");
}

void TestRetailKeepsLegacyTranslationOnly() {
  const CTransform4f locator(1.f, 0.f, 0.f, 1.25f,
                             0.f, 0.f, -1.f, 2.5f,
                             0.f, 1.f, 0.f, 4.f);
  CGeneratorRecorder generator(false);
  NPortChargeFxDetail::SetLocalLocator(generator, locator);

  Check(generator.orientationCalls == 0, "retail effect does not receive locator orientation");
  Check(generator.translationCalls == 1, "retail effect keeps the legacy translation setter");
  Check(Near(generator.rotation[0], 7.f) && Near(generator.rotation[5], 12.f) &&
            Near(generator.rotation[8], 15.f),
        "retail effect orientation is left untouched");
  Check(Near(generator.translation[0], 1.25f) && Near(generator.translation[1], 2.5f) &&
            Near(generator.translation[2], 4.f),
        "retail effect retains locator translation");
}
} // namespace

int main() {
  TestConvertedLocatorRotationAndChildOffset();
  TestRetailKeepsLegacyTranslationOnly();
  if (sFailures != 0) {
    std::fprintf(stderr, "%d charge locator checks failed\n", sFailures);
    return 1;
  }
  std::puts("charge locator checks passed");
  return 0;
}

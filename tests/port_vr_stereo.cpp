// The stereo replay's uniform composition (extern/aurora/lib/gfx/stereo_uniform.hpp):
// the GX uniform layout the recorder assumes, what one eye's copy of a
// perspective draw's uniform must contain, and the head-locked plane that
// AURORA_STEREO_ROUTE_HEAD_LOCKED_2D lays 2D draws on and takes EFB copies
// through (stereo_replay.hpp HeadLockedPlane: the scan visor's window).

#include "gfx/stereo_uniform.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "vr stereo regression failed: %s\n", what);
    std::abort();
  }
}
bool Near(float a, float b) { return std::fabs(a - b) < 0.0001f; }

aurora::Mat3x4<float> Affine(float m00, float m01, float m02, float tx, float m10, float m11, float m12, float ty,
                             float m20, float m21, float m22, float tz) {
  aurora::Mat3x4<float> out{};
  out.m0 = aurora::Vec4<float>{m00, m01, m02, tx};
  out.m1 = aurora::Vec4<float>{m10, m11, m12, ty};
  out.m2 = aurora::Vec4<float>{m20, m21, m22, tz};
  return out;
}
} // namespace

int main() {
  using namespace aurora;
  using namespace aurora::gfx;

  // The layout of a lit shader with two TEV registers loaded, no line mode.
  const auto layout = StereoUniformLayout::for_gx(0, true, 2, 2400);
  Check(layout.projectionOffset == 16, "projection follows the four viewport sizes");
  Check(layout.positionOffset == 80, "position matrices follow the projection");
  Check(layout.normalOffset == 80 + 20 * 48, "normal matrices follow ten position and ten texture matrices");
  Check(layout.lightsOffset == 1040 + 10 * 48 + 2 * 16, "lights follow the normal matrices and the TEV registers");
  Check(layout.valid(), "layout fits the uniform");
  const auto line = StereoUniformLayout::for_gx(1, false, 0, 1600);
  Check(line.projectionOffset == 32 && line.lightsOffset == 0 && line.valid(), "line mode shifts the projection");
  Check(!StereoUniformLayout::for_gx(1, false, 0, 1200).valid(), "a short uniform is rejected");

  // A synthetic uniform: sizes, a perspective projection, position matrix 0 at
  // (1, 2, 3), identity normal matrix 0, a marker in texture matrix 0, and a
  // light five units ahead pointing forward.
  std::vector<uint8_t> uniform(2400, 0);
  const float sizes[4] = {1920.f, 1080.f, 640.f, 480.f};
  std::memcpy(uniform.data(), sizes, sizeof(sizes));
  Mat4x4<float> projection{};
  projection.m0 = Vec4<float>{1.5f, 0.f, 0.1f, 0.f};
  projection.m1 = Vec4<float>{0.f, 2.f, 0.2f, 0.f};
  projection.m2 = Vec4<float>{0.f, 0.f, -1.1f, -0.3f};
  projection.m3 = Vec4<float>{0.f, 0.f, -1.f, 0.f};
  std::memcpy(uniform.data() + layout.projectionOffset, &projection, sizeof(projection));
  Check(stereo_uniform_is_perspective(uniform.data(), layout), "a perspective projection is recognised");
  const auto position0 = Affine(1, 0, 0, 1, 0, 1, 0, 2, 0, 0, 1, 3);
  std::memcpy(uniform.data() + layout.positionOffset, &position0, sizeof(position0));
  const auto identity = Affine(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0);
  std::memcpy(uniform.data() + layout.normalOffset, &identity, sizeof(identity));
  const auto marker = Affine(7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7);
  const size_t textureMatrix0 = layout.positionOffset + kStereoPositionMatrices * sizeof(Mat3x4<float>);
  std::memcpy(uniform.data() + textureMatrix0, &marker, sizeof(marker));
  const float light[20] = {0.f, 0.f, -5.f, 0.f, 0.f, 0.f, -1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
  std::memcpy(uniform.data() + layout.lightsOffset, light, sizeof(light));

  // The eye: its own frustum terms, a quarter turn about Y (x' = -z, z' = x)
  // and a 32 mm offset to the right.
  Mat4x4<float> eyeProjection{};
  eyeProjection.m0 = Vec4<float>{0.8f, 0.f, 0.05f, 0.f};
  eyeProjection.m1 = Vec4<float>{0.f, 0.9f, -0.04f, 0.f};
  const auto view = Affine(0, 0, -1, 0.032f, 0, 1, 0, 0, 1, 0, 0, 0);

  auto eye = uniform;
  compose_stereo_uniform(eye.data(), layout, StereoEyeCompose{&eyeProjection, &view, 1.f, 1.f, 2.f, 0.5f});

  Mat4x4<float> outProjection;
  std::memcpy(&outProjection, eye.data() + layout.projectionOffset, sizeof(outProjection));
  Check(Near(outProjection.m0[0], 0.8f) && Near(outProjection.m0[2], 0.05f) && Near(outProjection.m1[1], 0.9f) &&
            Near(outProjection.m1[2], -0.04f),
        "the four frustum terms come from the eye");
  Check(Near(outProjection.m2[2], -1.1f) && Near(outProjection.m2[3], -0.3f) && Near(outProjection.m3[2], -1.f),
        "the depth rows are the game's");

  Mat3x4<float> outPosition;
  std::memcpy(&outPosition, eye.data() + layout.positionOffset, sizeof(outPosition));
  Check(Near(outPosition.m0[3], -3.f + 0.032f) && Near(outPosition.m1[3], 2.f) && Near(outPosition.m2[3], 1.f),
        "position matrix 0 is rotated and offset into the eye");
  Check(Near(outPosition.m0[2], -1.f) && Near(outPosition.m2[0], 1.f) && Near(outPosition.m0[0], 0.f),
        "position matrix 0 carries the eye rotation");

  Mat3x4<float> outNormal;
  std::memcpy(&outNormal, eye.data() + layout.normalOffset, sizeof(outNormal));
  Check(Near(outNormal.m0[2], -1.f) && Near(outNormal.m2[0], 1.f) && Near(outNormal.m0[3], 0.f),
        "normal matrix 0 is rotated without translation");

  Check(std::memcmp(eye.data() + textureMatrix0, uniform.data() + textureMatrix0, sizeof(marker)) == 0,
        "texture matrices are untouched");

  float outLight[20];
  std::memcpy(outLight, eye.data() + layout.lightsOffset, sizeof(outLight));
  Check(Near(outLight[0], 5.032f) && Near(outLight[1], 0.f) && Near(outLight[2], 0.f),
        "the light position follows the eye");
  Check(Near(outLight[4], 1.f) && Near(outLight[5], 0.f) && Near(outLight[6], 0.f),
        "the light direction is rotated only");
  Check(Near(outLight[8], 1.f) && Near(outLight[11], 1.f), "the light colour is untouched");

  float outSizes[4];
  std::memcpy(outSizes, eye.data(), sizeof(outSizes));
  Check(Near(outSizes[0], 3840.f) && Near(outSizes[1], 540.f) && Near(outSizes[2], 640.f) &&
            Near(outSizes[3], 480.f),
        "the render size becomes the eye's, the logical size stays");

  // Head-locked: the scales apply before the pose, across and along the view.
  auto headLocked = uniform;
  compose_stereo_uniform(headLocked.data(), layout,
                         StereoEyeCompose{&eyeProjection, &identity, 0.5f, 2.f, 1.f, 1.f});
  std::memcpy(&outPosition, headLocked.data() + layout.positionOffset, sizeof(outPosition));
  Check(Near(outPosition.m0[3], 0.5f) && Near(outPosition.m1[3], 1.f) && Near(outPosition.m2[3], 6.f) &&
            Near(outPosition.m0[0], 0.5f) && Near(outPosition.m2[2], 2.f),
        "head-locked scales shrink across the view and push along it");

  // An orthographic projection is 2D content: identical in both eyes.
  auto ortho = uniform;
  Mat4x4<float> orthoProjection{};
  orthoProjection.m0 = Vec4<float>{0.1f, 0.f, 0.f, -1.f};
  orthoProjection.m1 = Vec4<float>{0.f, 0.2f, 0.f, 1.f};
  orthoProjection.m2 = Vec4<float>{0.f, 0.f, -0.5f, 0.f};
  orthoProjection.m3 = Vec4<float>{0.f, 0.f, 0.f, 1.f};
  std::memcpy(ortho.data() + layout.projectionOffset, &orthoProjection, sizeof(orthoProjection));
  Check(!stereo_uniform_is_perspective(ortho.data(), layout), "an orthographic projection is 2D content");

  // The head-locked plane: the HUD camera's half-angle tangents and a depth,
  // scaled like a head-locked perspective draw (across by size times
  // distance, along by distance).
  const stereo_replay::HeadLockedPlane plane{1.2f, 0.6f, 20.f};
  Check(plane.valid() && !stereo_replay::HeadLockedPlane{}.valid(), "a plane needs its three extents");
  const auto screen = stereo_replay::head_locked_plane_screen(plane, 0.5625f, 0.75f);
  Check(Near(screen.halfWidth, 13.5f) && Near(screen.halfHeight, 6.75f) && Near(screen.distance, 15.f),
        "the plane scales across the view by the size and along it by the distance");
  Check(!stereo_replay::head_locked_plane_screen({}, 1.f, 1.f).valid(), "no plane, no screen");

  // An orthographic draw on the plane: its NDC lands on the plane, offset by
  // the eye, and its flat-screen depth survives the perspective divide. A
  // 90 degree symmetric eye frustum, an eye a tenth of a unit to the right,
  // a plane two by one half-units across at distance four.
  Mat4x4<float> flatFrustum{};
  flatFrustum.m0 = Vec4<float>{1.f, 0.f, 0.f, 0.f};
  flatFrustum.m1 = Vec4<float>{0.f, 1.f, 0.f, 0.f};
  flatFrustum.m3 = Vec4<float>{0.f, 0.f, -1.f, 0.f};
  const auto rightEye = Affine(1, 0, 0, 0.1f, 0, 1, 0, 0, 0, 0, 1, 0);
  const stereo_replay::HudScreen paneScreen{2.f, 1.f, 4.f};
  // ndc x = 0.1 x, ndc y = 0.2 y, depth = -0.5 z + 0.25
  Mat4x4<float> paneOrtho{};
  paneOrtho.m0 = Vec4<float>{0.1f, 0.f, 0.f, 0.f};
  paneOrtho.m1 = Vec4<float>{0.f, 0.2f, 0.f, 0.f};
  paneOrtho.m2 = Vec4<float>{0.f, 0.f, -0.5f, 0.25f};
  paneOrtho.m3 = Vec4<float>{0.f, 0.f, 0.f, 1.f};
  const auto onPlane =
      stereo_replay::compose_head_locked_2d_projection(flatFrustum, rightEye, paneScreen, paneOrtho);
  // The vertex (5, 0, 1): ndc x 0.5, so plane x 1, plus the eye's 0.1, at distance 4.
  const float vertex[4] = {5.f, 0.f, 1.f, 1.f};
  const auto clip = [&](const Vec4<float>& row) {
    return row[0] * vertex[0] + row[1] * vertex[1] + row[2] * vertex[2] + row[3] * vertex[3];
  };
  const float w = clip(onPlane.m3);
  Check(Near(w, 4.f), "the plane's distance is the clip w");
  Check(Near(clip(onPlane.m0) / w, 0.275f) && Near(clip(onPlane.m1) / w, 0.f),
        "the vertex lands on the plane, offset by the eye");
  Check(Near(clip(onPlane.m2) / w, -0.25f), "the flat-screen depth survives the divide");
  auto paneUniform = ortho;
  std::memcpy(paneUniform.data() + layout.projectionOffset, &paneOrtho, sizeof(paneOrtho));
  compose_stereo_2d_uniform(paneUniform.data(), layout,
                            StereoEye2DCompose{&flatFrustum, &rightEye, paneScreen, 2.f, 0.5f});
  Mat4x4<float> staged;
  std::memcpy(&staged, paneUniform.data() + layout.projectionOffset, sizeof(staged));
  Check(std::memcmp(&staged, &onPlane, sizeof(staged)) == 0, "the 2D eye uniform carries the plane projection");
  Check(std::memcmp(paneUniform.data() + layout.positionOffset, ortho.data() + layout.positionOffset,
                    sizeof(Mat3x4<float>)) == 0,
        "a 2D draw's position matrices are untouched");
  std::memcpy(outSizes, paneUniform.data(), sizeof(outSizes));
  Check(Near(outSizes[0], 3840.f) && Near(outSizes[1], 540.f), "the 2D eye uniform takes the eye's render size");

  // A copy through the plane: the part of the eye's view behind the plane's
  // rectangle, in the eye's texture coordinates.
  const auto uv =
      stereo_replay::head_locked_plane_uv_rect(flatFrustum, rightEye, paneScreen, -0.5f, 0.5f, 0.5f, -0.5f);
  Check(uv.valid() && Near(uv.u, 0.3875f) && Near(uv.v, 0.4375f) && Near(uv.width, 0.25f) &&
            Near(uv.height, 0.125f),
        "the copy follows the plane's rectangle, offset by the eye");
  // An asymmetric frustum: straight ahead is off the eye image's centre, and so is the copy.
  Mat4x4<float> cantedFrustum = flatFrustum;
  cantedFrustum.m0[2] = 0.5f;
  const auto canted =
      stereo_replay::head_locked_plane_uv_rect(cantedFrustum, identity, paneScreen, -0.5f, 0.5f, 0.5f, -0.5f);
  Check(canted.valid() && Near(canted.u + 0.5f * canted.width, 0.25f),
        "the copy is centred where the head points, not at the image's centre");
  Check(!stereo_replay::head_locked_plane_uv_rect(flatFrustum, rightEye, {}, -0.5f, 0.5f, 0.5f, -0.5f).valid(),
        "no plane, no copy rectangle");

  std::puts("port_vr_stereo_tests: ok");
  return 0;
}

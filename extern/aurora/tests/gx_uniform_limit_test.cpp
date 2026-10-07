#include <array>

#include <gtest/gtest.h>

#include "gfx/resources.hpp"
#include "gx/gx.hpp"
#include "gx/shader_info.hpp"

namespace aurora::gx {
namespace {

// gx::MaxUniformSize is one shared number: shader_info.cpp refuses to build a
// shader above it, frame.cpp binds exactly that much out of the uniform buffer
// and recording.cpp pads each frame's uniform packet by it. A legitimate PBR
// material did not fit the old 3840 and aborted the game.
class GxUniformLimitTest : public ::testing::Test {
protected:
  void SetUp() override { saved = setAlignment(256); }
  void TearDown() override { setAlignment(saved); }

  static u32 setAlignment(u32 alignment) {
    const auto previous = gfx::detail::resources().limits.minUniformBufferOffsetAlignment;
    gfx::detail::resources().limits.minUniformBufferOffsetAlignment = alignment;
    return previous;
  }

  // Everything that can add uniform data at once: 4 TEV stages each reading a
  // texture, a colour channel and a constant register, all 8 texcoords with a
  // post matrix, 4 indirect stages driving all 3 indirect matrices, fog,
  // lighting with register-sourced material/ambient, and PBR.
  static ShaderConfig maximal(bool pbr) {
    ShaderConfig config;
    config.pbr = pbr ? 1 : 0;
    config.fogType = GX_FOG_LIN;
    config.numIndStages = 4;
    config.tevStageCount = 4;
    constexpr std::array kc{GX_TEV_KCSEL_K0, GX_TEV_KCSEL_K1, GX_TEV_KCSEL_K2, GX_TEV_KCSEL_K3};
    constexpr std::array is{GX_INDTEXSTAGE0, GX_INDTEXSTAGE1, GX_INDTEXSTAGE2, GX_INDTEXSTAGE3};
    for (u32 i = 0; i < config.tevStageCount; ++i) {
      auto& stage = config.tevStages[i];
      stage.colorPass.a = GX_CC_CPREV;
      stage.colorPass.b = GX_CC_TEXC;
      stage.colorPass.c = GX_CC_RASC;
      stage.colorPass.d = GX_CC_KONST;
      stage.alphaPass.a = GX_CA_APREV;
      stage.alphaPass.b = GX_CA_TEXA;
      stage.alphaPass.c = GX_CA_RASA;
      stage.alphaPass.d = GX_CA_KONST;
      stage.kcSel = kc[i];
      stage.kaSel = static_cast<GXTevKAlphaSel>(GX_TEV_KASEL_K0_R + i * 4);
      stage.texCoordId = static_cast<GXTexCoordID>(i);
      stage.texMapId = static_cast<GXTexMapID>(i);
      stage.channelId = i < 2 ? GX_COLOR0A0 : GX_COLOR1A1;
      stage.indTexStage = is[i];
      stage.indTexMtxId = static_cast<GXIndTexMtxID>(i % 3);
      config.indStages[i].texMapId = static_cast<GXTexMapID>(i + 4);
      config.indStages[i].texCoordId = static_cast<GXTexCoordID>(i + 4);
    }
    for (auto& cc : config.colorChannels) {
      cc.lightingEnabled = true;
      cc.matSrc = GX_SRC_REG;
      cc.ambSrc = GX_SRC_REG;
    }
    for (u32 i = 0; i < MaxTexCoord; ++i) {
      config.tcgs[i].postMtx = static_cast<GXPTTexMtx>(GX_PTTEXMTX0 + 3 * (i % 8));
    }
    return config;
  }

  u32 saved = 0;
};

TEST_F(GxUniformLimitTest, MaximalPbrConfigNoLongerAborts) {
  constexpr u32 OldLimit = 3840; // the limit a legitimate PBR material did not fit
  const auto plain = build_shader_info(maximal(false));
  const auto pbr = build_shader_info(maximal(true));
  ASSERT_TRUE(pbr.usesPbr);
  EXPECT_TRUE(pbr.lightingEnabled);
  // The worst TEV-only config is what the old limit was sized for...
  EXPECT_LE(plain.uniformSize, OldLimit);
  // ...and PBR's Mat3x4 + 24 vec4 push it past it, which is the abort.
  EXPECT_GT(pbr.uniformSize, OldLimit);
  EXPECT_GE(pbr.uniformSize - plain.uniformSize, sizeof(Mat3x4<float>) + sizeof(Vec4<float>) * 24);
  EXPECT_LE(pbr.uniformSize, MaxUniformSize);
}

TEST_F(GxUniformLimitTest, MaximalConfigFitsAtEveryAlignment) {
  // Adapters report minUniformBufferOffsetAlignment of 64 or 256, and the
  // uniform is padded up to it, so the limit has to hold for both.
  for (const u32 alignment : {64u, 256u}) {
    const auto previous = setAlignment(alignment);
    const auto plain = build_shader_info(maximal(false));
    const auto pbr = build_shader_info(maximal(true));
    EXPECT_EQ(plain.uniformSize % alignment, 0u) << "alignment " << alignment;
    EXPECT_EQ(pbr.uniformSize % alignment, 0u) << "alignment " << alignment;
    EXPECT_LE(pbr.uniformSize, MaxUniformSize) << "alignment " << alignment;
    setAlignment(previous);
  }
}

TEST_F(GxUniformLimitTest, LimitIsBindable) {
  // WebGPU's default maxUniformBufferBindingSize is 64 KiB; the spec's minimum
  // guarantee is 16 KiB, and we never request more, so the default applies.
  static_assert(MaxUniformSize <= 16384);
  static_assert(MaxUniformSize % 256 == 0); // no partial binding of the last texel
  // frame.cpp binds MaxUniformSize out of the uniform buffer.
  static_assert(MaxUniformSize <= gfx::UniformBufferSize);
  EXPECT_EQ(MaxUniformSize, 8192u);
}

} // namespace
} // namespace aurora::gx

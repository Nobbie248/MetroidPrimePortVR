#include <gtest/gtest.h>

#include "gx/native_vertex.hpp"
#include "gx/pipeline.hpp"
#include "gx/shader_info.hpp"
#include "gfx/frame_packet.hpp"
#include "gfx/recording.hpp"
#include "webgpu/gpu.hpp"

#include <cstring>
#include <vector>

namespace aurora::gx {
namespace {
class NativeVertexTest : public ::testing::Test {
protected:
  void SetUp() override {
    webgpu::g_graphicsConfig.surfaceConfiguration.format = wgpu::TextureFormat::RGBA8Unorm;
    webgpu::g_graphicsConfig.depthFormat = wgpu::TextureFormat::Depth24Plus;
    webgpu::g_graphicsConfig.msaaSamples = 1;
    webgpu::g_frameBuffer.size = {640, 480, 1};
    webgpu::g_frameBuffer.format = wgpu::TextureFormat::RGBA8Unorm;
    webgpu::g_depthBuffer.size = {640, 480, 1};
    webgpu::g_depthBuffer.format = wgpu::TextureFormat::Depth24Plus;
    gfx::detail::testing::suppress_render_worker(true);
    gfx::detail::begin_recording(frame, 0);
  }
  void TearDown() override {
    gfx::finish();
    gfx::detail::end_recording();
    gfx::detail::shutdown_recording();
  }
  gfx::detail::FramePacket frame;
};

ShaderConfig source_config() {
  g_gxState = {};
  auto& state = g_gxState;
  state.deindexVertices = true;
  state.nativeVertices = true;
  for (auto attr : {GX_VA_PNMTXIDX, GX_VA_POS, GX_VA_NRM, GX_VA_CLR0, GX_VA_TEX0}) {
    state.vtxDesc[attr] = GX_DIRECT;
  }
  state.vtxDesc[GX_VA_POS] = GX_INDEX16;
  state.arrays[GX_VA_POS].le = false;
  auto& attrs = state.vtxFmts[0].attrs;
  attrs[GX_VA_POS] = {GX_POS_XYZ, GX_F32, 0};
  attrs[GX_VA_NRM] = {GX_NRM_XYZ, GX_S16, 14};
  attrs[GX_VA_CLR0] = {GX_CLR_RGBA, GX_RGBA8, 0};
  attrs[GX_VA_TEX0] = {GX_TEX_ST, GX_U16, 15};
  state.numTevStages = 1;
  state.tevStages[0].channelId = GX_COLOR0A0;
  state.tevStages[0].colorPass.d = GX_CC_RASC;
  state.tevStages[0].alphaPass.d = GX_CA_RASA;
  for (auto& channel : state.colorChannelConfig) { channel.matSrc = GX_SRC_VTX; }
  PipelineConfig config{};
  populate_pipeline_config(config, GX_TRIANGLES, GX_VTXFMT0);
  return config.shaderConfig;
}
}

TEST_F(NativeVertexTest, UsesTheExistingAlignedRecordAndAbsoluteIndexStride) {
  const auto config = source_config();
  const auto native = native_vertex::layout(config);
  ASSERT_TRUE(native.valid);
  ASSERT_EQ(native.count, 5u);
  EXPECT_EQ(config.vtxStride, 32);
  EXPECT_EQ(native.attributes[0].format, wgpu::VertexFormat::Uint32);
  EXPECT_EQ(native.attributes[0].offset, 0u);
  EXPECT_EQ(native.attributes[1].format, wgpu::VertexFormat::Float32x3);
  EXPECT_EQ(native.attributes[1].offset, 4u);
  EXPECT_EQ(native.attributes[2].format, wgpu::VertexFormat::Sint16x4);
  EXPECT_EQ(native.attributes[2].offset, 16u);
  EXPECT_EQ(native.attributes[3].format, wgpu::VertexFormat::Unorm8x4);
  EXPECT_EQ(native.attributes[3].offset, 24u);
  EXPECT_EQ(native.attributes[4].format, wgpu::VertexFormat::Uint16x2);
  EXPECT_EQ(native.attributes[4].offset, 28u);
}

TEST_F(NativeVertexTest, ConvertsEndianOnceWithoutChangingIndicesColoursOrPadding) {
  const auto config = source_config();
  const std::vector<u8> big{
      6, 0xaa, 0xbb, 0xcc, 0x3f, 0x80, 0, 0, 0xc0, 0, 0, 0, 0x40, 0x60, 0, 0,
      0x40, 0, 0xc0, 0, 0, 0, 0xdd, 0xee, 20, 40, 80, 160, 0x40, 0, 0x80, 0};
  auto bytes = big;
  bytes.insert(bytes.end(), big.begin(), big.end());
  ASSERT_TRUE(native_vertex::convert(config, bytes));
  for (size_t base : {0u, 32u}) {
    EXPECT_EQ(bytes[base], 6);
    EXPECT_EQ(bytes[base + 1], 0xaa);
    std::array<float, 3> position;
    std::memcpy(position.data(), bytes.data() + base + 4, sizeof(position));
    EXPECT_EQ(position, (std::array<float, 3>{1.f, -2.f, 3.5f}));
    std::array<int16_t, 3> normal;
    std::memcpy(normal.data(), bytes.data() + base + 16, sizeof(normal));
    EXPECT_EQ(normal, (std::array<int16_t, 3>{16384, -16384, 0}));
    EXPECT_EQ(bytes[base + 22], 0xdd);
    EXPECT_EQ(bytes[base + 24], 20);
    EXPECT_EQ(bytes[base + 27], 160);
    std::array<uint16_t, 2> uv;
    std::memcpy(uv.data(), bytes.data() + base + 28, sizeof(uv));
    EXPECT_EQ(uv, (std::array<uint16_t, 2>{16384, 32768}));
  }
  auto littleConfig = config;
  for (auto& attr : littleConfig.attrs) { attr.le = true; }
  const auto before = bytes;
  ASSERT_TRUE(native_vertex::convert(littleConfig, bytes));
  EXPECT_EQ(bytes, before);
}

TEST_F(NativeVertexTest, UnsupportedLayoutsFallBackBeforeChangingBytes) {
  const auto valid = source_config();
  std::vector<u8> bytes(valid.vtxStride, 0xab);
  for (int mode = 0; mode < 5; ++mode) {
    auto config = valid;
    if (mode == 0) { config.attrs[GX_VA_NRM].cnt = 9; }
    if (mode == 1) { config.attrs[GX_VA_CLR0].compType = GX_RGB565; }
    if (mode == 2) { config.attrs[GX_VA_POS].attrType = GX_INDEX16; }
    if (mode == 3) { config.lineMode = 1; }
    if (mode == 4) { config.vtxStride = 30; }
    EXPECT_FALSE(native_vertex::layout(config).valid);
    EXPECT_FALSE(native_vertex::convert(config, bytes));
    EXPECT_EQ(bytes, std::vector<u8>(valid.vtxStride, 0xab));
  }
}

TEST_F(NativeVertexTest, NativeAttributesReplaceStorageFetchInEveryStereoMode) {
  auto config = source_config();
  for (auto mode : {MultiviewNone, MultiviewClip, MultiviewFull, EyeClipImmediate}) {
    config.multiview = mode;
    const auto source = build_shader_source(config);
    const auto begin = source.find("fn vs_main(");
    const auto vertex = source.substr(begin, source.find("@fragment", begin) - begin);
    EXPECT_NE(vertex.find("@location(1) native_attr9: vec3f"), std::string::npos);
    EXPECT_NE(vertex.find("let in_pos = native_attr9;"), std::string::npos);
    EXPECT_NE(vertex.find("vec3f(native_attr10.xyz)"), std::string::npos);
    EXPECT_NE(vertex.find("/ 3u"), std::string::npos);
    EXPECT_EQ(vertex.find("&vbuf"), std::string::npos);
    EXPECT_EQ(vertex.find("&abuf"), std::string::npos);
  }
  config.nativeVertices = false;
  const auto plain = build_shader_source(config);
  EXPECT_EQ(plain.find("@location(1) native_attr9"), std::string::npos);
  EXPECT_NE(plain.find("fetch_f32_3(&vbuf"), std::string::npos);
}

TEST_F(NativeVertexTest, ShortPositionsAndSingleComponentTexcoordsKeepTheirMissingComponents) {
  auto config = source_config();
  config.attrs[GX_VA_POS].cnt = 2;
  config.attrs[GX_VA_TEX0].cnt = 1;
  auto source = build_shader_source(config);
  EXPECT_NE(source.find("let in_pos = vec3f(native_attr9, 0.0);"), std::string::npos);
  EXPECT_NE(source.find("vec2f((f32(native_attr13.x) *"), std::string::npos);
  config.attrs[GX_VA_TEX0].compType = GX_F32;
  source = build_shader_source(config);
  EXPECT_NE(source.find("vec2f(native_attr13, 0.0)"), std::string::npos);
}
} // namespace aurora::gx

// The static world geometry cache (gx/geometry_cache.hpp): a cached display list is
// resolved once into the geometry buffer, drawn from it with absolute 32-bit indices,
// merges with its neighbours of the same state, and goes with its set.

#include "gx_test_common.hpp"
#include "__gx.h"
#include "gx/command_processor.hpp"
#include "gx/geometry_cache.hpp"
#include "gx/pipeline.hpp"

#include <array>
#include <cstring>

using aurora::gx::g_gxState;

namespace aurora::gx::testing {
extern ShaderConfig nativeVertexSource;
}

namespace aurora::gfx {
extern gx::DrawData g_testLastDraw;
extern uint32_t g_testDrawCount;
namespace testing {
extern std::vector<uint8_t> pushedVerts;
extern bool mergeDraws;
extern aurora::ByteBuffer stagedVerts;
extern aurora::ByteBuffer stagedIndices;
struct GeometryUploadRecord {
  uint32_t offset;
  std::vector<uint8_t> bytes;
};
extern std::vector<GeometryUploadRecord> geometryUploads;
extern uint64_t geometryCapacity;
} // namespace testing
} // namespace aurora::gfx

namespace {
// Four positions of 12 bytes, told apart by their bytes
std::array<u8, 48> sPositions;

class GXGeometryCacheTest : public GXFifoTest {
protected:
  void SetUp() override {
    GXFifoTest::SetUp();
    // AuroraCallCachedDisplayList flushes GX's shadow state first, like a display
    // list call: have that out of the way before the state below is set by hand.
    flush_and_capture();
    for (u32 i = 0; i < sPositions.size(); ++i) {
      sPositions[i] = static_cast<u8>(i + 1);
    }
    g_gxState.deindexVertices = true;
    g_gxState.lastVtxFmt = GX_MAX_VTXFMT;
    g_gxState.dirty |= aurora::gx::DirtyAll;
    g_gxState.vtxDesc[GX_VA_POS] = GX_INDEX8;
    g_gxState.vtxFmts[0].attrs[GX_VA_POS] = {GX_POS_XYZ, GX_F32, 0};
    g_gxState.arrays[GX_VA_POS] = {.data = sPositions.data(), .size = sPositions.size(), .stride = 12, .le = false};
    aurora::gfx::g_testDrawCount = 0;
    aurora::gfx::testing::mergeDraws = true;
    aurora::gfx::testing::stagedVerts.clear();
    aurora::gfx::testing::stagedIndices.clear();
    aurora::gfx::testing::pushedVerts.clear();
    aurora::gfx::testing::geometryUploads.clear();
    aurora::gfx::testing::geometryCapacity = 1u << 20;
    aurora::gx::geometry_cache::clear();
    aurora::gx::fifo::clear_native_vertex_choices();
  }

  void TearDown() override {
    aurora::gx::testing::nativeVertexSource = {};
    g_gxState.nativeVertexInputRequested = false;
    g_gxState.nativeVertices = false;
    aurora::gx::geometry_cache::clear();
    aurora::gx::fifo::clear_native_vertex_choices();
    aurora::gfx::testing::mergeDraws = false;
    g_gxState.deindexVertices = false;
    g_gxState.vtxDesc[GX_VA_POS] = GX_NONE;
    g_gxState.arrays[GX_VA_POS] = {};
    g_gxState.lastVtxFmt = GX_MAX_VTXFMT;
    g_gxState.dirty |= aurora::gx::DirtyAll;
    GXFifoTest::TearDown();
  }

  // A display list of one triangle draw (format 0) of these position indices
  static void enable_native_input() {
    g_gxState.nativeVertexInputRequested = true;
    auto& source = aurora::gx::testing::nativeVertexSource;
    source.vtxStride = 12;
    source.attrs[GX_VA_POS] = {.attrType = GX_DIRECT, .cnt = 3, .compType = GX_F32, .le = false};
  }

  static std::vector<u8> display_list(std::initializer_list<u8> indices, GXPrimitive prim = GX_TRIANGLES) {
    std::vector<u8> bytes{static_cast<u8>(prim), 0, static_cast<u8>(indices.size())};
    bytes.insert(bytes.end(), indices.begin(), indices.end());
    return bytes;
  }

  void call(u32 set, const std::vector<u8>& list) {
    AuroraCallCachedDisplayList(set, list.data(), static_cast<u32>(list.size()));
    decode_fifo(capture_fifo());
  }

  static std::vector<u8> resolved(std::initializer_list<u8> indices) {
    std::vector<u8> bytes;
    for (const u8 index : indices) {
      bytes.insert(bytes.end(), sPositions.begin() + index * 12, sPositions.begin() + index * 12 + 12);
    }
    return bytes;
  }

  static std::vector<u32> staged_indices(const aurora::gx::DrawData& draw) {
    const auto& staged = aurora::gfx::testing::stagedIndices;
    std::vector<u32> out(draw.idxRange.size / sizeof(u32));
    std::memcpy(out.data(), staged.data() + draw.idxRange.offset, draw.idxRange.size);
    return out;
  }
};
} // namespace

TEST_F(GXGeometryCacheTest, ResolvesOnceAndDrawsFromTheBuffer) {
  const auto list = display_list({2, 0, 1});
  call(1, list);
  const auto& uploads = aurora::gfx::testing::geometryUploads;
  ASSERT_EQ(uploads.size(), 1u);
  EXPECT_EQ(uploads[0].offset, 0u);
  EXPECT_EQ(uploads[0].bytes, resolved({2, 0, 1}));
  ASSERT_EQ(aurora::gfx::g_testDrawCount, 1u);
  const auto& draw = aurora::gfx::g_testLastDraw;
  EXPECT_TRUE(draw.cachedGeometry);
  EXPECT_EQ(draw.immediateData.vtxStart, 0u);
  EXPECT_EQ(draw.vtxCount, 3u);
  EXPECT_EQ(draw.indexCount, 3u);
  EXPECT_EQ(staged_indices(draw), (std::vector<u32>{0, 1, 2}));
  EXPECT_EQ(aurora::gx::geometry_cache::stats().entries, 1u);

  // The same surface again, in the same state: a hit that joins the draw
  call(1, list);
  EXPECT_EQ(uploads.size(), 1u);
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 1u);
  EXPECT_EQ(draw.indexCount, 6u);
  EXPECT_EQ(staged_indices(draw), (std::vector<u32>{0, 1, 2, 0, 1, 2}));
}

TEST_F(GXGeometryCacheTest, SurfacesOfOneStateMergeWhereverTheirBlocksAre) {
  const auto first = display_list({0, 1, 2});
  const auto second = display_list({3, 2, 1, 0}, GX_TRIANGLESTRIP);
  call(1, first);
  call(1, second);
  const auto& uploads = aurora::gfx::testing::geometryUploads;
  ASSERT_EQ(uploads.size(), 2u);
  EXPECT_EQ(uploads[1].offset, 36u); // after the first block, a multiple of the 12-byte stride
  EXPECT_EQ(uploads[1].bytes, resolved({3, 2, 1, 0}));
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 1u);
  const auto& draw = aurora::gfx::g_testLastDraw;
  EXPECT_EQ(draw.vtxCount, 7u);
  EXPECT_EQ(draw.indexCount, 9u);
  // The strip's two triangles, from vertex 3 on, alternate their winding
  EXPECT_EQ(staged_indices(draw), (std::vector<u32>{0, 1, 2, 3, 4, 5, 5, 4, 6}));
}

TEST_F(GXGeometryCacheTest, AStateChangeStartsANewDraw) {
  // Both lists stay alive: a cached surface is identified by its address, which a
  // freed temporary would hand on to the next list.
  const auto first = display_list({0, 1, 2});
  const auto second = display_list({1, 2, 3});
  call(1, first);
  g_gxState.currentPnMtx = 3;
  g_gxState.dirty |= aurora::gx::DirtyPipeline;
  call(1, second);
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 2u);
  EXPECT_EQ(aurora::gfx::g_testLastDraw.indexCount, 3u);
  EXPECT_EQ(staged_indices(aurora::gfx::g_testLastDraw), (std::vector<u32>{3, 4, 5}));
}

TEST_F(GXGeometryCacheTest, CachedAndPlainDrawsDoNotJoin) {
  const auto list = display_list({0, 1, 2});
  call(1, list);
  decode_fifo({static_cast<u8>(GX_TRIANGLES), 0, 3, 2, 0, 1});
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 2u);
  EXPECT_FALSE(aurora::gfx::g_testLastDraw.cachedGeometry);
  call(1, list);
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 3u);
  EXPECT_TRUE(aurora::gfx::g_testLastDraw.cachedGeometry);
  EXPECT_EQ(aurora::gfx::testing::geometryUploads.size(), 1u);
}

TEST_F(GXGeometryCacheTest, NativeRecordsConvertOnceAndKeepSurfaceBatching) {
  enable_native_input();
  const auto first = display_list({0, 1, 2});
  const auto second = display_list({3, 2, 1, 0}, GX_TRIANGLESTRIP);
  call(1, first);
  call(1, second);
  call(1, first);
  const auto& uploads = aurora::gfx::testing::geometryUploads;
  ASSERT_EQ(uploads.size(), 2u);
  auto expected = resolved({0, 1, 2});
  for (size_t i = 0; i < expected.size(); i += 4) { std::reverse(expected.begin() + i, expected.begin() + i + 4); }
  EXPECT_EQ(uploads[0].bytes, expected);
  EXPECT_EQ(uploads[1].offset, 36u);
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 1u);
  const auto& draw = aurora::gfx::g_testLastDraw;
  EXPECT_TRUE(draw.cachedGeometry);
  EXPECT_TRUE(draw.nativeVertices);
  EXPECT_EQ(staged_indices(draw), (std::vector<u32>{0, 1, 2, 3, 4, 5, 5, 4, 6, 0, 1, 2}));
}

TEST_F(GXGeometryCacheTest, NativeModeNeverLeaksIntoDynamicDrawsOrAliasesPlainCacheEntries) {
  const auto list = display_list({0, 1, 2});
  call(1, list);
  EXPECT_FALSE(aurora::gfx::g_testLastDraw.nativeVertices);
  enable_native_input();
  call(1, list);
  EXPECT_EQ(aurora::gx::geometry_cache::stats().entries, 2u);
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 2u);
  EXPECT_TRUE(aurora::gfx::g_testLastDraw.nativeVertices);
  decode_fifo({static_cast<u8>(GX_TRIANGLES), 0, 3, 2, 0, 1});
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 3u);
  EXPECT_FALSE(aurora::gfx::g_testLastDraw.nativeVertices);
  EXPECT_FALSE(aurora::gfx::g_testLastDraw.cachedGeometry);
  EXPECT_EQ(aurora::gfx::testing::pushedVerts, resolved({2, 0, 1}));
  call(1, list);
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 4u);
  EXPECT_TRUE(aurora::gfx::g_testLastDraw.nativeVertices);
  EXPECT_EQ(aurora::gfx::testing::geometryUploads.size(), 2u);
  g_gxState.nativeVertexInputRequested = false;
  call(1, list);
  EXPECT_FALSE(aurora::gfx::g_testLastDraw.nativeVertices);
  EXPECT_EQ(aurora::gfx::testing::geometryUploads.size(), 2u);
  EXPECT_EQ(staged_indices(aurora::gfx::g_testLastDraw), (std::vector<u32>{0, 1, 2}));
}

TEST_F(GXGeometryCacheTest, UnsupportedNativeLayoutStillUsesTheResidentStoragePath) {
  enable_native_input();
  aurora::gx::testing::nativeVertexSource.attrs[GX_VA_POS].cnt = 9;
  call(1, display_list({0, 1, 2}));
  EXPECT_TRUE(aurora::gfx::g_testLastDraw.cachedGeometry);
  EXPECT_FALSE(aurora::gfx::g_testLastDraw.nativeVertices);
  ASSERT_EQ(aurora::gfx::testing::geometryUploads.size(), 1u);
  EXPECT_EQ(aurora::gfx::testing::geometryUploads[0].bytes, resolved({0, 1, 2}));
}

TEST_F(GXGeometryCacheTest, FreeingTheSetDropsItsEntriesAndReusesTheirBlocks) {
  const auto list = display_list({0, 1, 2});
  const auto other = display_list({1, 2, 3});
  call(1, list);
  call(2, other);
  EXPECT_EQ(aurora::gx::geometry_cache::stats().entries, 2u);
  AuroraFreeGeometrySet(1);
  decode_fifo(capture_fifo());
  EXPECT_EQ(aurora::gx::geometry_cache::stats().entries, 1u);
  EXPECT_EQ(aurora::gx::geometry_cache::stats().residentBytes, 36u);
  call(1, list);
  const auto& uploads = aurora::gfx::testing::geometryUploads;
  ASSERT_EQ(uploads.size(), 3u);
  EXPECT_EQ(uploads[2].offset, 0u);
  EXPECT_EQ(aurora::gx::geometry_cache::stats().entries, 2u);
}

TEST_F(GXGeometryCacheTest, AListWithOtherCommandsDrawsInPlace) {
  // A BP register load ahead of the draw: not a pure surface
  std::vector<u8> list{0x61, 0, 0, 0, 0};
  const auto draw = display_list({2, 0, 1});
  list.insert(list.end(), draw.begin(), draw.end());
  call(1, list);
  EXPECT_TRUE(aurora::gfx::testing::geometryUploads.empty());
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 1u);
  EXPECT_FALSE(aurora::gfx::g_testLastDraw.cachedGeometry);
  EXPECT_EQ(aurora::gfx::testing::pushedVerts, resolved({2, 0, 1}));
}

TEST_F(GXGeometryCacheTest, AFullBufferDrawsInPlace) {
  aurora::gfx::testing::geometryCapacity = 40;
  const auto first = display_list({0, 1, 2});
  const auto second = display_list({1, 2, 3});
  call(1, first);
  EXPECT_EQ(aurora::gfx::testing::geometryUploads.size(), 1u);
  call(1, second);
  EXPECT_EQ(aurora::gfx::testing::geometryUploads.size(), 1u);
  EXPECT_EQ(aurora::gfx::g_testDrawCount, 2u);
  EXPECT_FALSE(aurora::gfx::g_testLastDraw.cachedGeometry);
  EXPECT_EQ(aurora::gfx::testing::pushedVerts, resolved({1, 2, 3}));
}

TEST_F(GXGeometryCacheTest, BlocksSitAtMultiplesOfTheirStride) {
  using namespace aurora::gx::geometry_cache;
  const std::vector<u8> records12(24, 1);
  const std::vector<u8> records20(40, 2);
  const std::vector<u32> indices{0, 1, 2};
  ASSERT_NE(insert({1, 1, 1}, 12, records12, indices), nullptr);
  ASSERT_NE(insert({1, 2, 1}, 20, records20, indices), nullptr);
  ASSERT_NE(insert({1, 3, 1}, 12, records12, indices), nullptr);
  // Looked up after all three: an insertion may grow the map and move its entries.
  const Entry* a = find({1, 1, 1});
  const Entry* b = find({1, 2, 1});
  const Entry* c = find({1, 3, 1});
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(a->offset, 0u);
  EXPECT_EQ(b->offset, 40u); // the first multiple of 20 past 24
  EXPECT_EQ(c->offset, 84u); // the first multiple of 12 past 80
  EXPECT_EQ(b->indices, (std::vector<u32>{2, 3, 4}));
  EXPECT_EQ(c->indices, (std::vector<u32>{7, 8, 9}));
  EXPECT_EQ(stats().residentBytes, 88u);
  free_set(1);
  EXPECT_EQ(stats().entries, 0u);
  EXPECT_EQ(stats().residentBytes, 0u);
  const Entry* d = insert({2, 1, 1}, 20, records20, indices);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->offset, 0u);
}

#include "port_room_env.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

void PutFloat(std::vector<uint8_t>& out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  Put32(out, bits);
}

// An axis-aligned box probe: centre and half extents.
void PutProbe(std::vector<uint8_t>& out, float cx, float cy, float cz, float hx, float hy, float hz, uint32_t cube) {
  const float centre[3] = {cx, cy, cz};
  const float half[3] = {hx, hy, hz};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      PutFloat(out, row == col ? 1.f / half[row] : 0.f);
    }
    PutFloat(out, -centre[row] / half[row]);
  }
  for (int i = 0; i < 9; ++i) {
    PutFloat(out, i % 4 == 0 ? 1.f : 0.f);
  }
  Put32(out, 0);
  Put32(out, cube);
  PutFloat(out, 1.f);
  PutFloat(out, 0.05f);
}

void PutCube(std::vector<uint8_t>& out, uint32_t size, uint32_t mips, uint8_t fill) {
  const size_t bytes = PortRoomEnv::CubeBytes(size, mips);
  Put32(out, size);
  Put32(out, mips);
  Put32(out, 0);
  Put32(out, uint32_t(bytes));
  out.insert(out.end(), bytes, fill);
}

std::vector<uint8_t> MakeFile(uint32_t version = 1) {
  std::vector<uint8_t> out = {'M', 'P', 'E', 'V'};
  Put32(out, version);
  PutFloat(out, 4.f);
  PutFloat(out, 0.18f);
  PutFloat(out, 0.6f);
  PutFloat(out, 0.15f);
  Put32(out, 3);
  Put32(out, 2);
  PutProbe(out, 0.f, 0.f, 0.f, 10.f, 10.f, 10.f, 0);  // the room
  PutProbe(out, 2.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1);     // an alcove in it
  PutProbe(out, 40.f, 0.f, 0.f, 5.f, 5.f, 5.f, 0);    // next door
  PutCube(out, 8, 4, 0x11);
  PutCube(out, 4, 3, 0x22);
  return out;
}

// A grid point: grey light `level` (1.0 as a half is 0x3C00), with red arriving from +x of
// the grid, green from +y and blue from +z.
void PutPoint(std::vector<uint8_t>& out, uint16_t level) {
  for (int i = 0; i < 3; ++i) {
    out.push_back(uint8_t(level));
    out.push_back(uint8_t(level >> 8));
  }
  for (int i = 0; i < 3; ++i) {
    out.push_back(0x00);
    out.push_back(0x38); // 0.5
  }
  out.insert(out.end(), {255, 0, 51});
  for (int channel = 0; channel < 3; ++channel) {
    for (int axis = 0; axis < 3; ++axis) {
      out.push_back(channel == axis ? 255 : 128);
    }
  }
}

// Version 2: the same, then a 3 x 2 x 2 grid of 2 m cells whose point (0, 0, 0) is at world
// (10, 20, 30), with grid x along world y, y along world -x and z along world z. The
// points of the grid's x = 2 are empty, and those of x = 1 twice as bright as x = 0.
std::vector<uint8_t> MakeGridFile() {
  std::vector<uint8_t> out = MakeFile(2);
  Put32(out, 1);
  const float rows[12] = {0.f, 0.5f, 0.f, -10.f, -0.5f, 0.f, 0.f, 5.f, 0.f, 0.f, 0.5f, -15.f};
  for (float value : rows) {
    PutFloat(out, value);
  }
  Put32(out, 3);
  Put32(out, 2);
  Put32(out, 2);
  for (int i = 0; i < 12; ++i) {
    PutPoint(out, i % 3 == 0 ? 0x3C00 : i % 3 == 1 ? 0x4000 : 0);
  }
  return out;
}

bool Near(float a, float b) { return a > b - 0.01f && a < b + 0.01f; }

void TestGrid() {
  PortRoomEnv::File file;
  std::string error;
  const std::vector<uint8_t> good = MakeGridFile();
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(good), file, error), "grid: parse");
  Check(file.grids.size() == 1 && file.probes.size() == 3, "grid: counts");
  if (file.grids.size() != 1) {
    return;
  }
  const PortRoomEnv::Grid& grid = file.grids[0];
  Check(grid.size[0] == 3 && grid.size[1] == 2 && grid.size[2] == 2 && Near(grid.average, 1.414f), "grid: header");
  const size_t before = good.size() - 12 * 24 - 64;
  for (size_t length = before; length < good.size(); ++length) {
    Check(!PortRoomEnv::Parse(std::vector<uint8_t>(good.begin(), good.begin() + length), file, error), "grid: cut short");
  }
  std::vector<uint8_t> bad = good;
  bad[before + 4 + 48] = 0;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "grid: size");
  bad = good;
  bad[before] = 200;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "grid: count");
  Check(PortRoomEnv::Parse(MakeFile(), file, error) && file.grids.empty(), "grid: version 1 has none");
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(good), file, error), "grid: parse again");
  Check(file.exposure[0] == 0.f && file.exposure[1] == 0.f, "grid: version 2 has no exposure range");

  // Version 3 ends with the exposure range.
  std::vector<uint8_t> v3 = good;
  v3[4] = 3;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(v3), file, error), "exposure: cut short");
  PutFloat(v3, 5.5f);
  PutFloat(v3, 6.f);
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(v3), file, error) && file.exposure[0] == 5.5f && file.exposure[1] == 6.f,
        "exposure: range");
  PutFloat(v3, 0.f);
  std::memcpy(v3.data() + v3.size() - 8, v3.data() + v3.size() - 4, 4); // highest below lowest
  v3.resize(v3.size() - 4);
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(v3), file, error) && file.exposure[0] == 0.f && file.exposure[1] == 0.f,
        "exposure: a backwards range is none");

  // Version 4 follows the range with the exposure bias and the curve's contrast.
  std::vector<uint8_t> v4 = v3;
  v4[4] = 4;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(v4), file, error), "tone: cut short");
  PutFloat(v4, 1.5f);
  PutFloat(v4, 0.4f);
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(v4), file, error) && file.exposureBias == 1.5f && file.contrast == 0.4f,
        "tone: bias and contrast");
  v4.resize(v4.size() - 4);
  PutFloat(v4, 7.f);
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(v4), file, error) && file.contrast == 0.f,
        "tone: a contrast out of range is none");
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(good), file, error), "grid: parse once more");

  PortRoomEnv::Ambient a;
  const float atPoint[3] = {10.f, 20.f, 30.f};
  Check(PortRoomEnv::SampleGrid(file, file.grids[0], atPoint, a), "grid: at a point");
  Check(Near(a.mean[0], 1.f) && Near(a.lobe[1], 0.5f) && Near(a.sharpness[0], 1.f) && Near(a.sharpness[1], 0.f) &&
            Near(a.sharpness[2], 0.2f),
        "grid: the point's values");
  // Red comes from grid +x, which is world +y; green from grid +y, world -x.
  Check(Near(a.direction[0][0], 0.f) && Near(a.direction[0][1], 1.f) && Near(a.direction[0][2], 0.f), "grid: red's direction");
  Check(Near(a.direction[1][0], -1.f) && Near(a.direction[1][1], 0.f), "grid: green's direction");
  Check(Near(a.direction[2][2], 1.f), "grid: blue's direction");
  const float halfway[3] = {9.f, 21.f, 31.f}; // grid (0.5, 0.5, 0.5)
  Check(PortRoomEnv::SampleGrid(file, file.grids[0], halfway, a) && Near(a.mean[0], 1.5f), "grid: between points");
  const float byEmpty[3] = {10.f, 23.5f, 30.f}; // grid x 1.75: the empty point does not darken it
  Check(PortRoomEnv::SampleGrid(file, file.grids[0], byEmpty, a) && Near(a.mean[0], 2.f), "grid: next to an empty point");
  const float inEmpty[3] = {10.f, 24.f, 30.f};
  Check(!PortRoomEnv::SampleGrid(file, file.grids[0], inEmpty, a), "grid: an empty point");
  const float edge[3] = {10.f, 19.5f, 29.5f}; // a quarter cell outside two faces
  Check(PortRoomEnv::SampleGrid(file, file.grids[0], edge, a) && Near(a.mean[0], 1.f), "grid: just outside");
  const float outside[3] = {10.f, 18.f, 30.f};
  Check(!PortRoomEnv::SampleGrid(file, file.grids[0], outside, a), "grid: outside");
}

void TestNames() {
  uint32_t id = 0;
  Check(PortRoomEnv::ParseFileName("d1241219.RoomEnv", id) && id == 0xD1241219, "file name");
  Check(!PortRoomEnv::ParseFileName("D1241219.roomenvs", id), "file name: longer suffix");
  Check(!PortRoomEnv::ParseFileName("D124121G.roomenv", id), "file name: bad hex");
  Check(!PortRoomEnv::ParseFileName("D1241219.TXTR", id), "file name: a resource");
  Check(!PortRoomEnv::ParseFileName("", id), "file name: empty");
}

void TestParse() {
  Check(PortRoomEnv::CubeBytes(8, 4) == size_t(4 + 1 + 1 + 1) * 16 * 6, "cube bytes");
  PortRoomEnv::File file;
  std::string error;
  const std::vector<uint8_t> good = MakeFile();
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(good), file, error), "parse");
  Check(file.probes.size() == 3 && file.cubes.size() == 2, "parse: counts");
  Check(file.tonemap[1] == 0.18f, "parse: tonemap");
  Check(file.probes[1].cube == 1 && file.probes[1].blend == 0.05f, "parse: probe");
  Check(file.cubes[1].size == 4 && file.cubes[1].mipCount == 3 && file.data[file.cubes[1].offset] == 0x22 &&
            file.data[file.cubes[0].offset + file.cubes[0].length - 1] == 0x11,
        "parse: cubes");

  // Every way of cutting the file short fails, and none reads past the end.
  for (size_t length = 0; length < good.size(); length += length < 400 ? 1 : 97) {
    Check(!PortRoomEnv::Parse(std::vector<uint8_t>(good.begin(), good.begin() + length), file, error), "parse: cut short");
  }
  std::vector<uint8_t> bad = good;
  bad[4] = 5;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: version");
  bad = good;
  bad[32 + 88] = 2;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: cube index");
  bad = good;
  bad[32 + 300] = 6; // the first cube's size
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: cube size");
  bad = good;
  bad[32 + 2] = 0xC0;
  bad[32 + 3] = 0x7F; // NaN
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: not finite");
  bad = good;
  bad[24] = 0xFF;
  bad[25] = 0xFF;
  bad[26] = 0xFF;
  bad[27] = 0xFF;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: probe count");
}

void TestPick() {
  PortRoomEnv::File file;
  std::string error;
  Check(PortRoomEnv::Parse(MakeFile(), file, error), "pick: parse");
  const float inRoom[3] = {-5.f, 0.f, 0.f};
  const float inAlcove[3] = {2.5f, 0.5f, 0.f};
  const float between[3] = {30.f, 0.f, 0.f}; // 20 from the room, 5 from next door
  const float nearRoom[3] = {12.f, 0.f, 0.f};
  PortRoomEnv::Pick pick = PortRoomEnv::PickProbe(file, inRoom);
  Check(pick.probe == 0 && pick.inside, "pick: inside");
  pick = PortRoomEnv::PickProbe(file, inAlcove);
  Check(pick.probe == 1 && pick.inside && pick.score > 7.99f && pick.score < 8.01f, "pick: the smaller of two boxes");
  pick = PortRoomEnv::PickProbe(file, between);
  Check(pick.probe == 2 && !pick.inside && pick.score > 4.99f && pick.score < 5.01f, "pick: the nearest box");
  pick = PortRoomEnv::PickProbe(file, nearRoom);
  Check(pick.probe == 0 && !pick.inside, "pick: outside, nearest");

  PortRoomEnv::Pick none;
  Check(!none.Better(pick) && pick.Better(none), "pick: against none");
  Check(PortRoomEnv::PickProbe(PortRoomEnv::File(), inRoom).probe < 0, "pick: no probes");
}
} // namespace

int main() {
  TestNames();
  TestParse();
  TestPick();
  TestGrid();
  if (sFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", sFailures);
    return 1;
  }
  std::puts("port_room_env tests passed");
  return 0;
}

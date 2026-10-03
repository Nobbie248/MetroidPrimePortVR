#include "port_room_geo.h"

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

PortRoomGeo::Instance MakeInstance(uint32_t model, float x) {
  PortRoomGeo::Instance instance;
  instance.model = model;
  instance.transform[0] = instance.transform[5] = instance.transform[10] = 1.f;
  instance.transform[3] = x;
  return instance;
}

void TestRoundTrip() {
  std::vector<PortRoomGeo::Instance> in;
  in.push_back(MakeInstance(0x11111111, 1.f));
  PortRoomGeo::Instance gated = MakeInstance(0x22222222, -2.5f);
  gated.layer = 3;
  gated.active = false;
  gated.links.push_back({0x0c1a0042, 9, PortRoomGeo::kShow});
  gated.links.push_back({0x001a0043, 10, PortRoomGeo::kToggle});
  in.push_back(gated);
  in.push_back(MakeInstance(0x33333333, 0.f));

  std::vector<PortRoomGeo::Instance> out;
  std::string error;
  const std::vector<uint8_t> file = PortRoomGeo::Write(in);
  Check(PortRoomGeo::Parse(file, out, error), "v2 file parses");
  Check(out.size() == 3, "three instances");
  if (out.size() != 3) {
    return;
  }
  Check(out[0].layer == PortRoomGeo::kEveryLayer && out[0].active && out[0].links.empty(), "plain instance");
  Check(out[1].model == 0x22222222 && out[1].transform[3] == -2.5f, "model and transform");
  Check(out[1].layer == 3 && !out[1].active, "layer and active");
  Check(out[1].links.size() == 2, "two links");
  if (out[1].links.size() == 2) {
    Check(out[1].links[0].sender == 0x0c1a0042 && out[1].links[0].state == 9 &&
              out[1].links[0].action == PortRoomGeo::kShow,
          "first link");
    Check(out[1].links[1].action == PortRoomGeo::kToggle, "second link");
  }
  Check(out[2].model == 0x33333333, "instance after links");

  for (size_t cut = 12; cut < file.size(); ++cut) {
    const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
    if (PortRoomGeo::Parse(part, out, error)) {
      std::fprintf(stderr, "FAIL: truncated at %zu parses\n", cut);
      ++sFailures;
    }
  }
}

void TestVersion1() {
  std::vector<uint8_t> file;
  Put32(file, 0x4752504D);
  Put32(file, 1);
  Put32(file, 2);
  for (uint32_t i = 0; i < 2; ++i) {
    Put32(file, 0xabc00000 + i);
    for (int j = 0; j < 12; ++j) {
      Put32(file, 0);
    }
  }
  std::vector<PortRoomGeo::Instance> out;
  std::string error;
  Check(PortRoomGeo::Parse(file, out, error), "v1 file parses");
  Check(out.size() == 2 && out[1].model == 0xabc00001, "v1 instances");
  Check(out.size() == 2 && out[1].active && out[1].layer == PortRoomGeo::kEveryLayer, "v1 defaults");

  file[4] = 3;
  Check(!PortRoomGeo::Parse(file, out, error), "unknown version rejected");
}
} // namespace

int main() {
  TestRoundTrip();
  TestVersion1();
  uint32_t id = 0;
  Check(PortRoomGeo::ParseFileName("1a2B3c4D.ROOMGEO", id) && id == 0x1A2B3C4D, "file name");
  if (sFailures == 0) {
    std::printf("port_room_geo_tests: ok\n");
  }
  return sFailures == 0 ? 0 : 1;
}

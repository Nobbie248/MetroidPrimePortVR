#include "port_hud_bars.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

bool Close(float a, float b) { return std::fabs(a - b) < 1e-5f; }

// Five stations one unit apart along x, the texture running 0..1 with them.
PortHudBars::Bar Straight() {
  PortHudBars::Bar bar;
  bar.name = "energybart01_energybar";
  for (int i = 0; i < 5; ++i) {
    PortHudBars::Station station;
    station.a[0] = station.b[0] = float(i);
    station.b[2] = 0.5f;
    station.uvA[0] = station.uvB[0] = float(i) / 4.f;
    station.uvB[1] = 1.f;
    bar.stations.push_back(station);
  }
  Check(PortHudBars::Measure(bar), "a straight bar has a length");
  return bar;
}

void TestFileName() {
  uint32_t id = 0;
  Check(PortHudBars::ParseFileName("B10E1DCD.hudbars", id) && id == 0xB10E1DCD, "upper case name");
  Check(PortHudBars::ParseFileName("b10e1dcd.HUDBARS", id) && id == 0xB10E1DCD, "any case name");
  Check(!PortHudBars::ParseFileName("B10E1DCD.FRME", id), "another type");
  Check(!PortHudBars::ParseFileName("B10E1DC.hudbars", id), "short id");
  Check(!PortHudBars::ParseFileName("B10E1DCG.hudbars", id), "not hex");
}

void TestRoundTrip() {
  PortHudBars::Bars bars = {Straight(), Straight()};
  bars[1].name = "energybart01_bossbar";
  bars[1].stations.resize(2);
  std::vector<uint8_t> data;
  PortHudBars::WriteFile(bars, data);
  PortHudBars::Bars read;
  Check(PortHudBars::ParseFile(data.data(), data.size(), read), "parses what it wrote");
  Check(read.size() == 2 && read[0].name == bars[0].name && read[1].name == bars[1].name, "names");
  Check(read.size() == 2 && read[0].stations.size() == 5 && read[1].stations.size() == 2, "station counts");
  Check(read.size() == 2 && Close(read[0].stations[3].a[0], 3.f) && Close(read[0].stations[3].uvB[1], 1.f),
        "station values");

  for (size_t cut = 0; cut < data.size(); ++cut) {
    PortHudBars::Bars partial;
    if (PortHudBars::ParseFile(data.data(), cut, partial)) {
      Check(false, "a cut file is refused");
      break;
    }
  }
  std::vector<uint8_t> bad = data;
  bad[0] = 'X';
  Check(!PortHudBars::ParseFile(bad.data(), bad.size(), read), "wrong magic");
  bars[1].stations.resize(1);
  PortHudBars::WriteFile(bars, data);
  Check(!PortHudBars::ParseFile(data.data(), data.size(), read), "one station is no strip");
}

void TestSample() {
  const PortHudBars::Bar bar = Straight();
  PortHudBars::Station at = PortHudBars::Sample(bar, 0.f);
  Check(Close(at.a[0], 0.f) && Close(at.uvA[0], 0.f), "empty end");
  at = PortHudBars::Sample(bar, 1.f);
  Check(Close(at.a[0], 4.f) && Close(at.uvA[0], 1.f) && Close(at.b[2], 0.5f), "full end");
  at = PortHudBars::Sample(bar, 0.375f);
  Check(Close(at.a[0], 1.5f) && Close(at.uvB[0], 0.375f), "between stations");
  at = PortHudBars::Sample(bar, 2.f);
  Check(Close(at.a[0], 4.f), "past the end is the end");
  at = PortHudBars::Sample(bar, -1.f);
  Check(Close(at.a[0], 0.f), "before the start is the start");
}

void TestInside() {
  const PortHudBars::Bar bar = Straight();
  size_t first = 0;
  size_t last = 0;
  PortHudBars::Inside(bar, 0.f, 1.f, first, last);
  Check(first == 1 && last == 4, "whole bar: the three middle stations");
  PortHudBars::Inside(bar, 0.25f, 0.75f, first, last);
  Check(first == 2 && last == 3, "ends on stations are not repeated");
  PortHudBars::Inside(bar, 0.3f, 0.4f, first, last);
  Check(first >= last, "nothing between two points in one span");
  PortHudBars::Inside(bar, 0.1f, 0.6f, first, last);
  Check(first == 1 && last == 3, "spans cut at both ends");
}

// A long end piece, then short ones: half full is half the length, not half the stations.
void TestLength() {
  PortHudBars::Bar bar;
  for (float x : {0.f, 6.f, 7.f, 8.f, 8.f, 10.f}) {
    PortHudBars::Station station;
    station.a[0] = station.b[0] = x;
    station.uvA[0] = float(bar.stations.size());
    bar.stations.push_back(station);
  }
  Check(PortHudBars::Measure(bar), "measured");
  PortHudBars::Station at = PortHudBars::Sample(bar, 0.5f);
  Check(Close(at.a[0], 5.f), "half the length");
  at = PortHudBars::Sample(bar, 0.9f);
  Check(Close(at.a[0], 9.f) && Close(at.uvA[0], 4.5f), "past a seam, the later of the two stations there");
  size_t first = 0;
  size_t last = 0;
  PortHudBars::Inside(bar, 0.5f, 0.9f, first, last);
  Check(first == 1 && last == 5, "both stations of a seam are kept");

  PortHudBars::Bar flat;
  flat.stations.resize(3);
  Check(!PortHudBars::Measure(flat), "a strip with no length is refused");
}
} // namespace

int main() {
  TestFileName();
  TestRoundTrip();
  TestSample();
  TestInside();
  TestLength();
  if (sFailures == 0) {
    std::printf("port_hud_bars: all tests passed\n");
  }
  return sFailures == 0 ? 0 : 1;
}

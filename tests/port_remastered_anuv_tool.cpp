// Developer tool, not a test (it needs game data): flattens the ANUV program of each
// Remastered model given on the command line and prints the flattened transforms at
// a few times, for build/mpr/anuv/anuv.py to be compared against (tools/anuv_check.py).

#include "port_remastered_anuv.h"
#include "port_remastered_cmdl.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace PortRemastered;

int main(int argc, char** argv) {
  static const double kTimes[] = {0.0, 0.37, 1.5, 7.25, 100.0};
  for (int i = 1; i < argc; ++i) {
    std::ifstream f(argv[i], std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Model model;
    std::string error;
    if (!ParseModel(data.data(), data.size(), model, error)) {
      std::printf("MODEL %s unreadable %s\n", argv[i], error.c_str());
      continue;
    }
    if (model.anuv.empty()) {
      std::printf("MODEL %s none\n", argv[i]);
      continue;
    }
    Anuv anuv;
    if (!ParseAnuv(model.anuv.data(), model.anuv.size(), anuv, error)) {
      std::printf("MODEL %s unparsed %s\n", argv[i], error.c_str());
      continue;
    }
    std::printf("MODEL %s entries %zu\n", argv[i], anuv.entries.size());
    for (size_t e = 0; e < anuv.entries.size(); ++e) {
      if (anuv.entries[e].skip != AnuvSkip::None) {
        std::printf("SKIP %s %zu %s\n", argv[i], e, AnuvSkipName(anuv.entries[e].skip));
        continue;
      }
      for (int k = 0; k < 3; ++k) {
        for (double t : kTimes) {
          float m[8];
          EvalAnuvTransform(anuv.entries[e].xf[k], t, m);
          std::printf("EVAL %s %zu %d %g", argv[i], e, k, t);
          for (float v : m) {
            std::printf(" %.7g", v);
          }
          std::printf("\n");
        }
      }
    }
  }
  return 0;
}

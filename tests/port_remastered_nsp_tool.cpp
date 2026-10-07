// Lists or dumps files of a Metroid Prime Remastered .nsp through PortRemastered::Nsp.
//   port_remastered_nsp_tool <nsp> <keys> list
//   port_remastered_nsp_tool <nsp> <keys> cat <path> [offset [size]]
//   port_remastered_nsp_tool <nsp> <keys> brdf [out]   extracts the BRDF table, checks its SHA-256

#include "port_remastered_nsp.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <nsp> <keys> list | cat <path> [offset [size]]\n", argv[0]);
    return 2;
  }
  std::string command = argv[3];
  PortRemastered::Nsp nsp;
  std::string error;
  if (!nsp.Open(argv[1], argv[2], error)) {
    std::fprintf(stderr, "open failed: %s\n", error.c_str());
    return 1;
  }

  if (command == "list") {
    for (const PortRemastered::RomfsFile& file : nsp.Files()) {
      std::printf("%llu\t%s\n", static_cast<unsigned long long>(file.size), file.path.c_str());
    }
    return 0;
  }

  if (command == "brdf") {
    std::vector<uint8_t> lut;
    if (!PortRemastered::ExtractBrdfLut(nsp, lut, error)) {
      std::fprintf(stderr, "brdf failed: %s\n", error.c_str());
      return 1;
    }
    std::printf("brdf table: %zu bytes, SHA-256 matches the known table\n", lut.size());
    if (argc >= 5) {
      std::FILE* f = std::fopen(argv[4], "wb");
      if (!f || std::fwrite(lut.data(), 1, lut.size(), f) != lut.size()) {
        std::fprintf(stderr, "write failed\n");
        return 1;
      }
      std::fclose(f);
    }
    return 0;
  }

  if (command == "cat" && argc >= 5) {
    const PortRemastered::RomfsFile* file = nsp.Find(argv[4]);
    if (!file) {
      std::fprintf(stderr, "no such file: %s\n", argv[4]);
      return 1;
    }
    uint64_t offset = argc >= 6 ? std::strtoull(argv[5], nullptr, 0) : 0;
    uint64_t size = argc >= 7 ? std::strtoull(argv[6], nullptr, 0) : file->size - std::min(offset, file->size);
#ifdef _WIN32
    std::freopen(nullptr, "wb", stdout);
#endif
    std::vector<uint8_t> buffer(size_t(1) << 22);
    while (size) {
      size_t take = size_t(std::min<uint64_t>(size, buffer.size()));
      if (!nsp.Read(*file, offset, buffer.data(), take, error)) {
        std::fprintf(stderr, "read failed: %s\n", error.c_str());
        return 1;
      }
      if (std::fwrite(buffer.data(), 1, take, stdout) != take) {
        std::fprintf(stderr, "write failed\n");
        return 1;
      }
      offset += take;
      size -= take;
    }
    return 0;
  }

  std::fprintf(stderr, "unknown command\n");
  return 2;
}

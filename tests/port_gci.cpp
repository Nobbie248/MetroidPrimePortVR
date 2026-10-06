#include "port_gci.h"

#include <aurora/aurora.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Aurora's card library wants these from its host.
namespace aurora {
AuroraConfig g_config{};
char g_gameName[4] = {'G', 'M', '8', 'E'};
void log_internal(AuroraLogLevel, const char*, const char* message, unsigned int length) noexcept {
  std::fprintf(stderr, "%.*s\n", static_cast< int >(length), message);
}
} // namespace aurora

namespace fs = std::filesystem;

namespace {
int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

std::vector<uint8_t> MakeGci(const char* game, const char* name, uint16_t blocks, uint8_t fill) {
  std::vector<uint8_t> gci(PortGci::kHeaderSize + blocks * PortGci::kBlockSize, fill);
  std::memset(gci.data(), 0, PortGci::kHeaderSize);
  std::memcpy(gci.data(), game, 4);
  std::memcpy(gci.data() + 4, "01", 2);
  gci[6] = 0xFF;
  std::strncpy(reinterpret_cast< char* >(gci.data() + 8), name, 32);
  gci[0x36] = 0;
  gci[0x37] = 5; // first block
  gci[0x38] = uint8_t(blocks >> 8);
  gci[0x39] = uint8_t(blocks);
  std::memset(gci.data() + 0x2C, 0xFF, 4); // no icon
  std::memset(gci.data() + 0x3C, 0xFF, 4); // no comment
  return gci;
}

std::vector<uint8_t> Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void Write(const fs::path& path, const std::vector<uint8_t>& data) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast< const char* >(data.data()), std::streamsize(data.size()));
}

size_t CountGci(const fs::path& folder) {
  size_t count = 0;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    count += entry.path().extension() == ".gci" ? 1 : 0;
  }
  return count;
}
} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / "port-gci-tests";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root);

  // Header checks.
  {
    const auto good = MakeGci("GM8E", "MetroidPrime A", 3, 0x11);
    PortGci::Header header;
    std::string error;
    Check(PortGci::ParseHeader(good.data(), good.size(), header, error), "good header parses");
    Check(header.fileName == "MetroidPrime A" && header.blockCount == 3, "header fields");
    Check(PortGci::IsGameFile(header), "GM8E/01 is the game's");
    Check(PortGci::DiskName(header) == "01-GM8E-MetroidPrime A.gci", "Dolphin disk name");
    auto shortData = good;
    shortData.pop_back();
    Check(!PortGci::ParseHeader(shortData.data(), shortData.size(), header, error),
          "size mismatch rejected");
    auto zero = MakeGci("GM8E", "x", 1, 0);
    zero[0x39] = 0;
    Check(!PortGci::ParseHeader(zero.data(), zero.size(), header, error), "zero blocks rejected");
    const auto slashes = MakeGci("GM8E", "a/b:c", 1, 0);
    PortGci::ParseHeader(slashes.data(), slashes.size(), header, error);
    Check(PortGci::DiskName(header) == "01-GM8E-a_b_c.gci", "disk name sanitised");
  }

  // Import into a card folder: new, then replacing the same identity kept
  // under another disk name, which moves the old file aside.
  const fs::path card = root / "card";
  fs::create_directories(card);
  Write(card / "renamed.gci", MakeGci("GM8E", "MetroidPrime A", 3, 0x22));
  Write(card / "01-GZLE-other.gci", MakeGci("GZLE", "other", 1, 0x33));
  const fs::path source = root / "in.gci";
  const auto incoming = MakeGci("GM8E", "MetroidPrime A", 3, 0x44);
  Write(source, incoming);
  {
    const auto report = PortGci::ImportFile(source, card, root / "scratch");
    Check(report.copied == 1 && report.replaced == 1 && report.skipped == 0, "import replaces");
    Check(Read(card / "01-GM8E-MetroidPrime A.gci") == incoming, "imported bytes");
    Check(!fs::exists(card / "renamed.gci"), "old identity moved");
    Check(CountGci(card / "_replaced") == 1, "old file kept in _replaced");
    Check(fs::exists(card / "01-GZLE-other.gci"), "other game untouched");
    Check(PortGci::GameFiles(card).size() == 1, "one game file");
  }
  // Another game's .gci is refused.
  {
    Write(root / "zelda.gci", MakeGci("GZLE", "zelda", 1, 0));
    const auto report = PortGci::ImportFile(root / "zelda.gci", card, root / "scratch");
    Check(report.copied == 0 && report.skipped == 1, "other game's save refused");
    const auto junk = std::vector<uint8_t>(100, 7);
    Write(root / "junk.gci", junk);
    Check(PortGci::ImportFile(root / "junk.gci", card, root / "scratch").copied == 0,
          "junk refused");
  }

  // Export to a folder, then a raw card, and import the raw card back.
  {
    const fs::path dolphin = root / "dolphin" / "Card A";
    auto report = PortGci::ExportFolder(card, dolphin);
    Check(report.copied == 1 && Read(dolphin / "01-GM8E-MetroidPrime A.gci") == incoming,
          "export to folder");
    Check(PortGci::ExportFolder(card, card).copied == 0, "export into itself refused");

    const fs::path raw = root / "MemoryCardA.USA.raw";
    report = PortGci::ExportRaw(card, raw);
    Check(report.copied == 1 && report.skipped == 0, "export to a new raw card");
    report = PortGci::ExportRaw(card, raw);
    Check(report.copied == 1 && fs::exists(root / "MemoryCardA.USA.raw.bak"),
          "export over a raw card keeps a backup");

    const fs::path fresh = root / "fresh";
    report = PortGci::ImportFile(raw, fresh, root / "scratch");
    Check(report.copied == 1 && report.skipped == 0, "import from raw card");
    const auto back = Read(fresh / "01-GM8E-MetroidPrime A.gci");
    // Placement fields (first block) can differ; the name and data must not.
    Check(back.size() == incoming.size() &&
              std::memcmp(back.data() + 8, incoming.data() + 8, 32) == 0 &&
              std::memcmp(back.data() + 64, incoming.data() + 64, incoming.size() - 64) == 0,
          "raw round trip keeps the save");
  }

  // The game alternates between the A and B files and loads the newer, so an
  // import has to replace the whole set: importing A moves an existing B aside,
  // and exporting to a raw card drops a stale B from it.
  {
    const fs::path ab = root / "ab";
    fs::create_directories(ab);
    Write(ab / "01-GM8E-MetroidPrime B.gci", MakeGci("GM8E", "MetroidPrime B", 2, 0x55));
    const fs::path onlyA = root / "onlyA.gci";
    Write(onlyA, MakeGci("GM8E", "MetroidPrime A", 2, 0x66));
    const auto report = PortGci::ImportFile(onlyA, ab, root / "scratch");
    Check(report.copied == 1 && report.replaced == 1, "import A replaces B");
    Check(PortGci::GameFiles(ab).size() == 1 && fs::exists(ab / "01-GM8E-MetroidPrime A.gci"),
          "only A left");

    const fs::path raw = root / "ab.raw";
    const fs::path withB = root / "withB";
    fs::create_directories(withB);
    Write(withB / "01-GM8E-MetroidPrime B.gci", MakeGci("GM8E", "MetroidPrime B", 2, 0x77));
    Check(PortGci::ExportRaw(withB, raw).copied == 1, "raw card with B");
    Check(PortGci::ExportRaw(ab, raw).copied == 1, "raw card with A");
    const fs::path check = root / "abcheck";
    Check(PortGci::ImportFile(raw, check, root / "scratch").copied == 1 &&
              PortGci::GameFiles(check).size() == 1 &&
              fs::exists(check / "01-GM8E-MetroidPrime A.gci"),
          "stale B dropped from the raw card");
  }

  // The Quest launcher's hand-over: the .gci files left in pending_import are
  // one save set (B is not moved aside by A), then they are gone and a report
  // is written; an empty folder is nothing to do.
  {
    const fs::path user = root / "user";
    const fs::path pending = user / "primedgun" / "pending_import";
    const fs::path card = user / "USA" / "Card A";
    fs::create_directories(pending);
    fs::create_directories(card);
    Write(card / "01-GM8E-MetroidPrime A.gci", MakeGci("GM8E", "MetroidPrime A", 2, 0x10));
    Check(!PortGci::ImportPending(user, card), "nothing pending");
    Write(pending / "01-GM8E-MetroidPrime A.gci", MakeGci("GM8E", "MetroidPrime A", 2, 0x20));
    Write(pending / "01-GM8E-MetroidPrime B.gci", MakeGci("GM8E", "MetroidPrime B", 2, 0x30));
    Write(pending / "notes.txt", {'x'});
    Check(PortGci::ImportPending(user, card), "pending set imported");
    Check(PortGci::GameFiles(card).size() == 2, "both pending files in the card");
    Check(Read(card / "01-GM8E-MetroidPrime A.gci")[64] == 0x20, "pending A replaced the old A");
    Check(CountGci(pending) == 0, "pending files removed");
    const auto report = Read(user / "primedgun" / "import_report.txt");
    Check(std::string(report.begin(), report.end()).rfind("Imported 2 save files", 0) == 0,
          "import report written");
    Check(!PortGci::ImportPending(user, card), "nothing pending afterwards");
    Check(!fs::exists(pending) && !fs::exists(user / "primedgun" / "pending_import.claimed") &&
              !fs::exists(user / "primedgun" / "pending_import.done"),
          "the claimed folder is gone");

    // An upper-case .GCI is a save like any other.
    fs::create_directories(pending);
    Write(pending / "01-GM8E-MetroidPrime B.GCI", MakeGci("GM8E", "MetroidPrime B", 2, 0x40));
    Check(PortGci::ImportPending(user, card), "upper-case .GCI imported");
    Check(PortGci::GameFiles(card).size() == 1 && Read(card / "01-GM8E-MetroidPrime B.gci")[64] == 0x40,
          "the .GCI save replaced the set");

    // A claim left by an import that never finished is taken up at the next start.
    const fs::path claimed = user / "primedgun" / "pending_import.claimed";
    fs::create_directories(claimed);
    Write(claimed / "01-GM8E-MetroidPrime A.gci", MakeGci("GM8E", "MetroidPrime A", 2, 0x50));
    Check(PortGci::ImportPending(user, card), "an unfinished claim is imported");
    Check(Read(card / "01-GM8E-MetroidPrime A.gci")[64] == 0x50 && !fs::exists(claimed),
          "the unfinished claim's save is in the card");
  }

  fs::remove_all(root, ec);
  if (sFailures == 0) {
    std::puts("port_gci tests passed");
  }
  return sFailures == 0 ? 0 : 1;
}

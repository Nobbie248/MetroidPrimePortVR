// Reads the RomFS of a Switch title straight out of the user's .nsp with the
// user's own key file (port_remastered_nsp.h). The container layout and key
// derivation follow hactool (ISC licence), which is the reference for both.

#include "port_remastered_nsp.h"

#include <algorithm>
#include <cstring>
#include <map>

#include <cstdlib>
#include <sstream>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#if defined(MP_HAVE_OPENSSL)
#include <openssl/crypto.h>
#include <openssl/evp.h>
#endif

namespace PortRemastered {

bool SourceFile::Open(const std::string& path) {
  Close();
#if !defined(_WIN32)
  if (path.rfind("fd:", 0) == 0) {
    char* end = nullptr;
    const long fd = std::strtol(path.c_str() + 3, &end, 10);
    if (end == path.c_str() + 3 || *end != '\0' || fd < 0) {
      return false;
    }
    m_fd = dup(int(fd));
    return m_fd >= 0;
  }
#endif
  m_stream.open(path, std::ios::binary);
  return bool(m_stream);
}

void SourceFile::Close() {
#if !defined(_WIN32)
  if (m_fd >= 0) {
    close(m_fd);
    m_fd = -1;
  }
#endif
  m_stream.close();
  m_stream.clear();
}

size_t SourceFile::ReadSome(uint64_t offset, void* out, size_t size) {
#if !defined(_WIN32)
  if (m_fd >= 0) {
    // pread leaves the position alone, which the duplicate shares with the caller's.
    size_t done = 0;
    while (done < size) {
      const ssize_t got = pread(m_fd, static_cast<char*>(out) + done, size - done, off_t(offset + done));
      if (got <= 0) {
        break;
      }
      done += size_t(got);
    }
    return done;
  }
#endif
  m_stream.clear();
  m_stream.seekg(std::streamoff(offset), std::ios::beg);
  if (!m_stream) {
    return 0;
  }
  m_stream.read(static_cast<char*>(out), std::streamsize(size));
  return size_t(m_stream.gcount());
}

bool SourceFile::ReadAt(uint64_t offset, void* out, size_t size) { return ReadSome(offset, out, size) == size; }

#if defined(MP_HAVE_OPENSSL)
namespace {

constexpr uint64_t kMediaUnit = 0x200;
constexpr size_t kNcaHeaderSize = 0xC00;
// Reads are decrypted in pieces of this size through the scratch buffer.
constexpr size_t kChunk = 1u << 20;
// Far past what this title has; only here to reject garbage before allocating.
constexpr uint32_t kMaxPfs0Files = 4096;
constexpr uint32_t kMaxPfs0Strings = 1u << 20;
constexpr uint64_t kMaxRomfsTable = 256u << 20;

uint32_t ReadLE32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint64_t ReadLE64(const uint8_t* p) { return uint64_t(ReadLE32(p)) | (uint64_t(ReadLE32(p + 4)) << 32); }

int HexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// Wipes key material when the scope ends, so no exit path forgets to.
struct Wipe {
  void* data;
  size_t size;
  ~Wipe() { OPENSSL_cleanse(data, size); }
};

// Only the keys this title needs: the header key and one title KEK per master
// key generation. Parsing keeps nothing else from the file.
struct KeySet {
  uint8_t headerKey[32] = {};
  bool hasHeader = false;
  std::map<std::string, std::vector<uint8_t>> titleKeks;
  ~KeySet() {
    OPENSSL_cleanse(headerKey, sizeof(headerKey));
    for (auto& entry : titleKeks) {
      OPENSSL_cleanse(entry.second.data(), entry.second.size());
    }
  }
};

bool LoadKeys(const std::string& path, KeySet& keys, std::string& error) {
  // A key file is a few KB of text; one that isn't is not read past this.
  std::string text(1 << 20, '\0');
  {
    SourceFile file;
    if (!file.Open(path)) {
      error = "cannot open key file";
      return false;
    }
    text.resize(file.ReadSome(0, text.data(), text.size()));
  }
  Wipe wipe{text.data(), text.size()};
  std::istringstream f(text);
  std::string line;
  while (std::getline(f, line)) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    std::string name;
    for (size_t i = 0; i < eq; ++i) {
      char c = line[i];
      if (c != ' ' && c != '\t' && c != '\r') {
        name.push_back(char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
      }
    }
    bool wantHeader = name == "header_key";
    bool wantKek = name.rfind("titlekek_", 0) == 0 && name.size() == 11;
    if (!wantHeader && !wantKek) {
      continue;
    }
    std::vector<uint8_t> value;
    int high = -1;
    for (size_t i = eq + 1; i < line.size(); ++i) {
      int digit = HexDigit(line[i]);
      if (digit < 0) {
        continue;
      }
      if (high < 0) {
        high = digit;
      } else {
        value.push_back(uint8_t((high << 4) | digit));
        high = -1;
      }
    }
    if (wantHeader && value.size() == 32) {
      std::memcpy(keys.headerKey, value.data(), 32);
      keys.hasHeader = true;
    } else if (wantKek && value.size() == 16) {
      keys.titleKeks[name.substr(9)] = value;
    }
    OPENSSL_cleanse(value.data(), value.size());
  }
  if (!keys.hasHeader) {
    error = "key file has no valid header_key";
    return false;
  }
  return true;
}

// Nintendo's XTS sector tweak is the sector number big-endian, where the
// standard (and OpenSSL's) convention is little-endian, so the IV is built by
// hand for every 0x200-byte sector.
bool DecryptHeader(const uint8_t* key, uint8_t* data, size_t size) {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    return false;
  }
  bool ok = true;
  for (size_t sector = 0; ok && sector * 0x200 < size; ++sector) {
    uint8_t tweak[16] = {};
    for (int i = 15, shift = 0; i >= 8; --i, shift += 8) {
      tweak[i] = uint8_t(uint64_t(sector) >> shift);
    }
    uint8_t* block = data + sector * 0x200;
    int outLen = 0;
    ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_xts(), nullptr, key, tweak) == 1 &&
         EVP_DecryptUpdate(ctx, block, &outLen, block, 0x200) == 1 && outLen == 0x200;
  }
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

bool DecryptEcb(const uint8_t* key, const uint8_t* in, uint8_t* out) {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    return false;
  }
  int outLen = 0;
  bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr) == 1 &&
            EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 && EVP_DecryptUpdate(ctx, out, &outLen, in, 16) == 1 &&
            outLen == 16;
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

// The counter is the section's 8-byte nonce (stored reversed) followed by the
// big-endian count of 16-byte blocks from the start of the NCA.
bool DecryptCtr(const uint8_t* key, const uint8_t* nonce, uint64_t ncaOffset, uint8_t* data, size_t size) {
  uint8_t iv[16];
  for (int i = 0; i < 8; ++i) {
    iv[i] = nonce[7 - i];
  }
  uint64_t block = ncaOffset >> 4;
  for (int i = 15; i >= 8; --i) {
    iv[i] = uint8_t(block);
    block >>= 8;
  }
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    return false;
  }
  int outLen = 0;
  bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, key, iv) == 1 &&
            EVP_DecryptUpdate(ctx, data, &outLen, data, int(size)) == 1 && size_t(outLen) == size;
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

} // namespace

Nsp::~Nsp() { Close(); }

void Nsp::Close() {
  OPENSSL_cleanse(m_contentKey, sizeof(m_contentKey));
  m_file.Close();
  m_files.clear();
  m_scratch.clear();
  m_open = false;
}

bool Nsp::Open(const std::string& nspPath, const std::string& keysPath, std::string& error) {
  Close();

  KeySet keys;
  if (!LoadKeys(keysPath, keys, error)) {
    return false;
  }
  if (!m_file.Open(nspPath)) {
    error = "cannot open " + nspPath;
    return false;
  }

  // --- PFS0: the .nsp container ---------------------------------------------
  uint8_t pfsHeader[16];
  if (!m_file.ReadAt(0, pfsHeader, sizeof(pfsHeader)) || std::memcmp(pfsHeader, "PFS0", 4) != 0) {
    error = "not an .nsp (no PFS0 header)";
    return false;
  }
  uint32_t numFiles = ReadLE32(pfsHeader + 4);
  uint32_t stringSize = ReadLE32(pfsHeader + 8);
  if (numFiles == 0 || numFiles > kMaxPfs0Files || stringSize > kMaxPfs0Strings) {
    error = "implausible PFS0 header";
    return false;
  }
  size_t tableSize = size_t(numFiles) * 24 + stringSize;
  std::vector<uint8_t> table(tableSize);
  if (!m_file.ReadAt(16, table.data(), tableSize)) {
    error = "truncated PFS0 table";
    return false;
  }
  const uint64_t dataStart = 16 + tableSize;

  struct Entry {
    std::string name;
    uint64_t offset;
    uint64_t size;
  };
  std::vector<Entry> entries;
  for (uint32_t i = 0; i < numFiles; ++i) {
    const uint8_t* e = table.data() + size_t(i) * 24;
    uint32_t nameOffset = ReadLE32(e + 16);
    if (nameOffset >= stringSize) {
      error = "bad PFS0 name offset";
      return false;
    }
    const char* strings = reinterpret_cast<const char*>(table.data() + size_t(numFiles) * 24);
    size_t len = strnlen(strings + nameOffset, stringSize - nameOffset);
    entries.push_back({std::string(strings + nameOffset, len), dataStart + ReadLE64(e), ReadLE64(e + 8)});
  }

  // The game is the largest .nca; the small ones are metadata and the icon.
  const Entry* nca = nullptr;
  const Entry* ticket = nullptr;
  for (const Entry& entry : entries) {
    auto ends = [&](const char* suffix) {
      size_t n = std::strlen(suffix);
      return entry.name.size() >= n && entry.name.compare(entry.name.size() - n, n, suffix) == 0;
    };
    if (ends(".nca") && !ends(".cnmt.nca") && (!nca || entry.size > nca->size)) {
      nca = &entry;
    } else if (ends(".tik") && !ticket) {
      ticket = &entry;
    }
  }
  if (!nca) {
    error = "no .nca in the .nsp";
    return false;
  }
  if (!ticket) {
    error = "no .tik in the .nsp (title-key crypto needs the ticket)";
    return false;
  }
  if (nca->size < kNcaHeaderSize) {
    error = "NCA too small";
    return false;
  }

  // --- Ticket -----------------------------------------------------------------
  uint8_t tik[0x2C0];
  if (ticket->size < sizeof(tik) || !m_file.ReadAt(ticket->offset, tik, sizeof(tik))) {
    error = "truncated ticket";
    return false;
  }
  // 0x10004 is RSA-2048 + SHA-256, the layout the offsets below assume.
  if (ReadLE32(tik) != 0x10004) {
    error = "unsupported ticket signature type";
    return false;
  }
  if (tik[0x281] != 0) {
    error = "personalized ticket (the title key is console-encrypted); only common tickets are supported";
    return false;
  }
  Wipe wipeTik{tik, sizeof(tik)};

  // --- NCA header ----------------------------------------------------------------
  std::vector<uint8_t> hdr(kNcaHeaderSize);
  if (!m_file.ReadAt(nca->offset, hdr.data(), kNcaHeaderSize)) {
    error = "cannot read the NCA header";
    return false;
  }
  if (!DecryptHeader(keys.headerKey, hdr.data(), kNcaHeaderSize)) {
    error = "header decryption failed";
    return false;
  }
  if (std::memcmp(hdr.data() + 0x200, "NCA3", 4) != 0) {
    if (std::memcmp(hdr.data() + 0x200, "NCA2", 4) == 0 || std::memcmp(hdr.data() + 0x200, "NCA0", 4) == 0) {
      error = "NCA0/NCA2 is not supported";
    } else {
      error = "NCA header is not valid; wrong keys?";
    }
    return false;
  }
  const uint8_t* rightsId = hdr.data() + 0x230;
  bool hasRights = false;
  for (int i = 0; i < 16; ++i) {
    hasRights = hasRights || rightsId[i] != 0;
  }
  if (!hasRights) {
    error = "NCA has no rights id (key-area crypto is not supported)";
    return false;
  }
  if (std::memcmp(rightsId, tik + 0x2A0, 16) != 0) {
    error = "the ticket is not for this NCA's rights id";
    return false;
  }

  uint8_t cryptoType = std::max(hdr[0x206], hdr[0x220]);
  if (cryptoType) {
    --cryptoType;
  }
  static const char kHex[] = "0123456789abcdef";
  std::string kekName = std::string(1, kHex[cryptoType >> 4]) + kHex[cryptoType & 15];
  auto kek = keys.titleKeks.find(kekName);
  if (kek == keys.titleKeks.end()) {
    error = "key file lacks the title KEK for master key generation " + kekName;
    return false;
  }
  if (!DecryptEcb(kek->second.data(), tik + 0x180, m_contentKey)) {
    error = "title key decryption failed";
    return false;
  }

  // --- The RomFS section -------------------------------------------------------
  int section = -1;
  for (int i = 0; i < 4; ++i) {
    const uint8_t* entry = hdr.data() + 0x240 + i * 0x10;
    const uint8_t* fs = hdr.data() + 0x400 + i * 0x200;
    if (ReadLE32(entry) == 0) {
      continue;
    }
    uint8_t partition = fs[2];
    uint8_t fsType = fs[3];
    uint8_t crypt = fs[4];
    if (partition != 0 || fsType != 3) {
      continue;
    }
    if (crypt == 4) {
      error = "BKTR (patch) section: only a base-game RomFS is supported";
      return false;
    }
    if (crypt != 3) {
      error = "RomFS section is not AES-CTR";
      return false;
    }
    section = i;
    break;
  }
  if (section < 0) {
    error = "no RomFS section in the NCA";
    return false;
  }
  const uint8_t* entry = hdr.data() + 0x240 + section * 0x10;
  const uint8_t* fs = hdr.data() + 0x400 + section * 0x200;
  uint64_t start = uint64_t(ReadLE32(entry)) * kMediaUnit;
  uint64_t end = uint64_t(ReadLE32(entry + 4)) * kMediaUnit;
  if (end <= start || end > nca->size) {
    error = "bad section bounds";
    return false;
  }
  std::memcpy(m_ctrHigh, fs + 0x140, 8);

  const uint8_t* ivfc = fs + 8;
  uint32_t numLevels = ReadLE32(ivfc + 0xC);
  if (std::memcmp(ivfc, "IVFC", 4) != 0 || numLevels < 2 || numLevels > 7) {
    error = "RomFS section has no IVFC header";
    return false;
  }
  // The last level is the RomFS itself; the others are hash trees.
  m_romfsOffset = ReadLE64(ivfc + 0x10 + (numLevels - 2) * 0x18);

  m_sectionInNca = start;
  m_sectionBase = nca->offset + start;
  m_sectionSize = end - start;

  // --- RomFS tables -----------------------------------------------------------
  std::string readError;
  uint8_t rh[0x50];
  if (!ReadSection(m_romfsOffset, rh, sizeof(rh), readError)) {
    error = "cannot read the RomFS header: " + readError;
    Close();
    return false;
  }
  if (ReadLE64(rh) != 0x50) {
    error = "bad RomFS header (wrong keys or unsupported layout)";
    Close();
    return false;
  }
  uint64_t dirOffset = ReadLE64(rh + 0x18), dirSize = ReadLE64(rh + 0x20);
  uint64_t fileOffset = ReadLE64(rh + 0x38), fileSize = ReadLE64(rh + 0x40);
  m_dataOffset = ReadLE64(rh + 0x48);
  if (dirSize > kMaxRomfsTable || fileSize > kMaxRomfsTable) {
    error = "implausible RomFS table size";
    Close();
    return false;
  }
  std::vector<uint8_t> dirs(dirSize), files(fileSize);
  if (!ReadSection(m_romfsOffset + dirOffset, dirs.data(), dirs.size(), readError) ||
      !ReadSection(m_romfsOffset + fileOffset, files.data(), files.size(), readError)) {
    error = "cannot read the RomFS tables: " + readError;
    Close();
    return false;
  }

  constexpr uint32_t kEmpty = 0xFFFFFFFF;
  // Depth-first over the directory tree, with every entry visited at most once.
  struct Pending {
    uint32_t dir;
    std::string path;
  };
  std::vector<Pending> stack;
  stack.push_back({0, std::string()});
  size_t visited = 0;
  while (!stack.empty()) {
    Pending cur = std::move(stack.back());
    stack.pop_back();
    if (cur.dir + 24 > dirs.size() || ++visited > dirs.size() / 24 + 1) {
      error = "corrupt RomFS directory table";
      Close();
      return false;
    }
    const uint8_t* d = dirs.data() + cur.dir;
    uint32_t child = ReadLE32(d + 8), firstFile = ReadLE32(d + 12);
    for (uint32_t f = firstFile; f != kEmpty;) {
      if (uint64_t(f) + 32 > files.size()) {
        error = "corrupt RomFS file table";
        Close();
        return false;
      }
      const uint8_t* fe = files.data() + f;
      uint32_t nameSize = ReadLE32(fe + 28);
      if (uint64_t(f) + 32 + nameSize > files.size() || m_files.size() > files.size() / 32) {
        error = "corrupt RomFS file table";
        Close();
        return false;
      }
      RomfsFile file;
      file.path = cur.path + std::string(reinterpret_cast<const char*>(fe + 32), nameSize);
      file.offset = m_dataOffset + ReadLE64(fe + 8);
      file.size = ReadLE64(fe + 16);
      m_files.push_back(std::move(file));
      f = ReadLE32(fe + 4);
    }
    // Children are pushed with their full prefix, found by walking the child
    // chain here: a sibling link alone would not know the parent's prefix.
    for (uint32_t c = child; c != kEmpty;) {
      if (uint64_t(c) + 24 > dirs.size()) {
        error = "corrupt RomFS directory table";
        Close();
        return false;
      }
      const uint8_t* ce = dirs.data() + c;
      uint32_t nameSize = ReadLE32(ce + 20);
      if (uint64_t(c) + 24 + nameSize > dirs.size() || stack.size() > dirs.size() / 24 + 1) {
        error = "corrupt RomFS directory table";
        Close();
        return false;
      }
      stack.push_back({c, cur.path + std::string(reinterpret_cast<const char*>(ce + 24), nameSize) + "/"});
      c = ReadLE32(ce + 4);
    }
  }
  std::sort(m_files.begin(), m_files.end(), [](const RomfsFile& a, const RomfsFile& b) { return a.path < b.path; });

  m_open = true;
  return true;
}

const RomfsFile* Nsp::Find(const std::string& path) const {
  auto it = std::lower_bound(m_files.begin(), m_files.end(), path,
                             [](const RomfsFile& file, const std::string& key) { return file.path < key; });
  return it != m_files.end() && it->path == path ? &*it : nullptr;
}

bool Nsp::ReadSection(uint64_t offset, void* out, size_t size, std::string& error) const {
  uint8_t* dst = static_cast<uint8_t*>(out);
  if (offset > m_sectionSize || size > m_sectionSize - offset) {
    error = "read past the end of the section";
    return false;
  }
  while (size) {
    uint64_t position = offset;
    uint64_t aligned = position & ~uint64_t(15);
    size_t head = size_t(position - aligned);
    size_t take = std::min(size, kChunk - head);
    // CTR is a stream cipher, so only the start has to sit on a 16-byte boundary.
    m_scratch.resize(head + take);
    if (!m_file.ReadAt(m_sectionBase + aligned, m_scratch.data(), head + take)) {
      error = "short read from the .nsp";
      return false;
    }
    if (!DecryptCtr(m_contentKey, m_ctrHigh, m_sectionInNca + aligned, m_scratch.data(), head + take)) {
      error = "decryption failed";
      return false;
    }
    std::memcpy(dst, m_scratch.data() + head, take);
    dst += take;
    offset += take;
    size -= take;
  }
  return true;
}

bool Nsp::Read(const RomfsFile& file, uint64_t offset, void* out, size_t size, std::string& error) const {
  if (!m_open) {
    error = "not open";
    return false;
  }
  if (offset > file.size || size > file.size - offset) {
    error = "read past the end of " + file.path;
    return false;
  }
  return ReadSection(m_romfsOffset + file.offset + offset, out, size, error);
}

#else // !MP_HAVE_OPENSSL

Nsp::~Nsp() = default;

void Nsp::Close() {
  m_files.clear();
  m_open = false;
}

bool Nsp::Open(const std::string&, const std::string&, std::string& error) {
  error = "built without OpenSSL";
  return false;
}

const RomfsFile* Nsp::Find(const std::string&) const { return nullptr; }

bool Nsp::ReadSection(uint64_t, void*, size_t, std::string& error) const {
  error = "built without OpenSSL";
  return false;
}

bool Nsp::Read(const RomfsFile&, uint64_t, void*, size_t, std::string& error) const {
  error = "built without OpenSSL";
  return false;
}

#endif

} // namespace PortRemastered

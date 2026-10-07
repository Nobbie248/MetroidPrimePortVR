#ifndef METROID_PRIME_PORT_PORT_LOG_REDACT_H
#define METROID_PRIME_PORT_PORT_LOG_REDACT_H
#include <cstddef>
#include <cstring>

// Takes the user's name out of the log file. A log is something people attach
// to a public issue, and the paths it prints (the disc, the user folder, the
// card, caches) usually sit under the home folder, whose name is the account's.
// The home folder becomes "~" (Windows: "%USERPROFILE%"), and a name that shows
// up as a path part elsewhere (/run/media/<name>/, D:\Users\<name>\) becomes
// "<user>".
//
// No allocation: on Linux the copying runs in a process forked from the game,
// which may have had other threads holding the allocator's lock at the fork.
namespace PortLogRedact {

struct Rule {
  char from[512] = "";
  char to[32] = "";
  bool ignoreCase = false; // Windows paths
};

constexpr int kMaxRules = 6;

struct Rules {
  Rule rule[kMaxRules];
  int count = 0;

  // `from` must be a path or path part at least 2 characters long; anything
  // shorter, or too long to hold, is not added (a one-letter account name would
  // otherwise be cut out of every word).
  void Add(const char* from, const char* to, bool ignoreCase) {
    if (count >= kMaxRules || from == nullptr || to == nullptr) {
      return;
    }
    size_t length = std::strlen(from);
    while (length > 1 && (from[length - 1] == '/' || from[length - 1] == '\\')) {
      --length; // "/home/name/" matches as "/home/name"
    }
    if (length < 2 || length >= sizeof(Rule::from) || std::strlen(to) >= sizeof(Rule::to)) {
      return;
    }
    for (int i = 0; i < count; ++i) {
      if (std::strlen(rule[i].from) == length && std::strncmp(rule[i].from, from, length) == 0) {
        return;
      }
    }
    Rule& r = rule[count++];
    std::memcpy(r.from, from, length);
    r.from[length] = '\0';
    std::strcpy(r.to, to);
    r.ignoreCase = ignoreCase;
  }
};

inline char Fold(char c, bool ignoreCase) {
  if (ignoreCase) {
    if (c == '\\') {
      return '/';
    }
    if (c >= 'A' && c <= 'Z') {
      return static_cast< char >(c - 'A' + 'a');
    }
  }
  return c;
}

// Whether `c` can continue a name, so "/home/ann" leaves "/home/anna" alone.
inline bool NameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
         c == '.' || static_cast< unsigned char >(c) >= 0x80;
}

// Copies `in` to `out` with every rule applied, the first rule that matches at a
// spot winning. Returns the length written; `out` is cut short at `capacity`
// bytes (no terminator is added).
inline size_t Apply(const Rules& rules, const char* in, size_t size, char* out, size_t capacity) {
  size_t used = 0;
  size_t i = 0;
  while (i < size && used < capacity) {
    const Rule* hit = nullptr;
    size_t hitLength = 0;
    for (int r = 0; r < rules.count && hit == nullptr; ++r) {
      const Rule& rule = rules.rule[r];
      size_t k = 0;
      while (rule.from[k] != '\0' && i + k < size &&
             Fold(in[i + k], rule.ignoreCase) == Fold(rule.from[k], rule.ignoreCase)) {
        ++k;
      }
      if (rule.from[k] != '\0' || (i + k < size && NameChar(in[i + k]))) {
        continue;
      }
      // The match must start a path too: "/x/home/ann" is someone else's. A rule
      // that starts mid-path (":\Users\ann", after the drive letter) is exempt.
      const bool startsPath = NameChar(rule.from[0]) || rule.from[0] == '/' || rule.from[0] == '\\';
      if (i > 0 && NameChar(in[i - 1]) && startsPath) {
        continue;
      }
      hit = &rule;
      hitLength = k;
    }
    if (hit == nullptr) {
      out[used++] = in[i++];
      continue;
    }
    for (const char* t = hit->to; *t != '\0' && used < capacity; ++t) {
      out[used++] = *t;
    }
    i += hitLength;
  }
  return used;
}

} // namespace PortLogRedact

#endif // METROID_PRIME_PORT_PORT_LOG_REDACT_H

#include "port_embedded.h"

#include <algorithm>

#if defined(MP_EMBED_RESOURCES)
namespace PortEmbedded {
// Defined by the generated source.
extern const Entry kTable[];
extern const unsigned long long kCount;
} // namespace PortEmbedded
#endif

namespace PortEmbedded {
std::span<const Entry> All() {
#if defined(MP_EMBED_RESOURCES)
  return {kTable, static_cast<size_t>(kCount)};
#else
  return {};
#endif
}

std::span<const uint8_t> Find(std::string_view path) {
  const std::span<const Entry> all = All();
  const auto it = std::lower_bound(all.begin(), all.end(), path,
                                   [](const Entry& e, std::string_view p) { return std::string_view(e.path) < p; });
  if (it == all.end() || std::string_view(it->path) != path) {
    return {};
  }
  return {it->data, it->size};
}

std::span<const Entry> Under(std::string_view prefix) {
  const std::span<const Entry> all = All();
  const auto first = std::lower_bound(all.begin(), all.end(), prefix,
                                      [](const Entry& e, std::string_view p) { return std::string_view(e.path) < p; });
  auto last = first;
  while (last != all.end() && std::string_view(last->path).starts_with(prefix)) {
    ++last;
  }
  return {first, last};
}
} // namespace PortEmbedded

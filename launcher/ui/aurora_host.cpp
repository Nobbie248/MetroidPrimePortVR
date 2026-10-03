// SPDX-License-Identifier: GPL-3.0-or-later
//
// What Aurora's card library expects from its host, for platform/port_gci.cpp
// (the memory-card transfer). tests/port_gci.cpp provides the same.

#include <aurora/aurora.h>

#include <cstdio>

namespace aurora {
AuroraConfig g_config{};
char g_gameName[4] = {'G', 'M', '8', 'E'};
void log_internal(AuroraLogLevel, const char*, const char* message, unsigned int length) noexcept {
  std::fprintf(stderr, "%.*s\n", static_cast<int>(length), message);
}
} // namespace aurora

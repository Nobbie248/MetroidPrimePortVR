#include "port_env.h"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define SET_ENV(name, value) _putenv_s(name, value)
#define UNSET_ENV(name) _putenv_s(name, "")
#else
#define SET_ENV(name, value) setenv(name, value, 1)
#define UNSET_ENV(name) unsetenv(name)
#endif

namespace {

int sFailures = 0;

void Check(bool ok, const char* what, const char* value) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s with \"%s\"\n", what, value);
    ++sFailures;
  }
}

} // namespace

int main() {
  const char* kName = "MP_ENV_TEST_VALUE";

  // Unset: the fallback. (On Windows an unset variable is an empty one, so the
  // unset case is only checked where unsetenv exists.)
#ifndef _WIN32
  UNSET_ENV(kName);
  Check(!port::EnvFlag(kName), "unset flag, fallback off", "");
  Check(port::EnvFlag(kName, true), "unset flag, fallback on", "");
  Check(port::EnvString(kName) == nullptr, "unset string", "");
  Check(port::EnvInt(kName, 7) == 7, "unset int", "");
  Check(port::EnvFloat(kName, 1.5f) == 1.5f, "unset float", "");
  SET_ENV(kName, "");
  Check(!port::EnvFlag(kName, true), "empty flag is off", "");
  Check(port::EnvString(kName) != nullptr, "empty string is set", "");
#endif

  for (const char* off : {"0", "false", "FALSE", "Off", "no", "NO"}) {
    SET_ENV(kName, off);
    Check(!port::EnvFlag(kName, true), "off value", off);
  }
  for (const char* on : {"1", "true", "On", "yes", "2", "00", "nope", "x"}) {
    SET_ENV(kName, on);
    Check(port::EnvFlag(kName, false), "on value", on);
  }

  SET_ENV(kName, "42");
  Check(port::EnvInt(kName, 7) == 42, "int", "42");
  SET_ENV(kName, "-3");
  Check(port::EnvInt(kName, 7) == -3, "negative int", "-3");
  for (const char* bad : {"abc", "12x", " "}) {
    SET_ENV(kName, bad);
    Check(port::EnvInt(kName, 7) == 7, "unparsable int", bad);
    Check(port::EnvFloat(kName, 1.5f) == 1.5f, "unparsable float", bad);
  }
  SET_ENV(kName, "0.25");
  Check(port::EnvFloat(kName, 1.5f) == 0.25f, "float", "0.25");
  SET_ENV(kName, "2");
  Check(port::EnvFloat(kName, 1.5f) == 2.f, "integral float", "2");

  if (sFailures == 0) {
    std::puts("port_env_tests: ok");
  }
  return sFailures == 0 ? 0 : 1;
}

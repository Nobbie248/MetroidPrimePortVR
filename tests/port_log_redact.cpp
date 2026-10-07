#include "port_log_redact.h"

#include <cstdio>
#include <string>

namespace {
int sFailures = 0;

std::string Run(const PortLogRedact::Rules& rules, const std::string& in, size_t capacity = 4096) {
  std::string out(capacity, '\0');
  out.resize(PortLogRedact::Apply(rules, in.data(), in.size(), out.data(), capacity));
  return out;
}

void Expect(const PortLogRedact::Rules& rules, const std::string& in, const std::string& want) {
  const std::string got = Run(rules, in);
  if (got != want) {
    std::printf("FAIL: \"%s\" -> \"%s\", want \"%s\"\n", in.c_str(), got.c_str(), want.c_str());
    ++sFailures;
  }
}
} // namespace

int main() {
  PortLogRedact::Rules unixRules;
  unixRules.Add("/home/ann/", "~", false);
  unixRules.Add("/var/home/ann", "~", false);
  unixRules.Add("/run/media/ann", "/run/media/<user>", false);
  unixRules.Add("/media/ann", "/media/<user>", false);
  unixRules.Add("/home/ann", "~", false); // a repeat of the first is not added again
  if (unixRules.count != 4) {
    std::printf("FAIL: %d rules, want 4\n", unixRules.count);
    ++sFailures;
  }
  Expect(unixRules, "disc mounted: /home/ann/rom/Metroid Prime.iso\n", "disc mounted: ~/rom/Metroid Prime.iso\n");
  Expect(unixRules, "/home/ann", "~");
  Expect(unixRules, "user folder /home/ann/.local/share/Metroid Prime/ (portable)",
         "user folder ~/.local/share/Metroid Prime/ (portable)");
  // Someone else's home, or a longer name, is left alone.
  Expect(unixRules, "/home/anna/x and /home/ann.b/x", "/home/anna/x and /home/ann.b/x");
  Expect(unixRules, "/x/home/ann/y", "/x/home/ann/y");
  Expect(unixRules, "/var/home/ann/a", "~/a");
  Expect(unixRules, "/run/media/ann/Leo/disc.iso", "/run/media/<user>/Leo/disc.iso");
  Expect(unixRules, "/media/ann/USB/disc.iso", "/media/<user>/USB/disc.iso");
  Expect(unixRules, "'/home/ann/a' \"/home/ann\"", "'~/a' \"~\"");
  Expect(unixRules, "nothing personal here", "nothing personal here");
  Expect(unixRules, "/HOME/ANN/a", "/HOME/ANN/a"); // Linux paths are case-sensitive

  PortLogRedact::Rules winRules;
  winRules.Add("C:\\Users\\Ann", "%USERPROFILE%", true);
  winRules.Add(":\\Users\\Ann", ":\\Users\\<user>", true);
  Expect(winRules, "disc C:\\Users\\Ann\\Downloads\\mp.iso", "disc %USERPROFILE%\\Downloads\\mp.iso");
  Expect(winRules, "cache c:/users/ann/AppData/Roaming", "cache %USERPROFILE%/AppData/Roaming");
  Expect(winRules, "D:\\Users\\ANN\\Games\\mp.iso", "D:\\Users\\<user>\\Games\\mp.iso");
  Expect(winRules, "C:\\Users\\Anna\\x", "C:\\Users\\Anna\\x");

  // Too short to be told apart from ordinary text.
  PortLogRedact::Rules tiny;
  tiny.Add("/", "~", false);
  tiny.Add("a", "<user>", false);
  if (tiny.count != 0) {
    std::printf("FAIL: a one-character rule was added\n");
    ++sFailures;
  }

  // Output is cut at the capacity, not overrun.
  if (Run(unixRules, "/home/ann/abc", 3) != "~/a") {
    std::printf("FAIL: capacity\n");
    ++sFailures;
  }
  if (Run(unixRules, "/run/media/ann", 5) != "/run/") {
    std::printf("FAIL: capacity inside a replacement\n");
    ++sFailures;
  }

  if (sFailures != 0) {
    std::printf("%d failure(s)\n", sFailures);
    return 1;
  }
  std::printf("port_log_redact_tests: all passed\n");
  return 0;
}

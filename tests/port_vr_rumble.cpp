// The headset controllers' rumble by source (platform/include/vr/vr_rumble.h):
// the split target puts the arm cannon on the gun hand and the grapple arm on
// the off hand, the body on both; the others play every source on their hand(s).

#include "vr/vr_rumble.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "vr rumble regression failed: %s\n", what);
    std::abort();
  }
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

bool Hands(const std::array<float, 2>& hands, float left, float right) {
  return Near(hands[0], left) && Near(hands[1], right);
}
} // namespace

int main() {
  using namespace PortVr;

  const RumbleLevels body{1.0f, 0.0f, 0.0f};
  const RumbleLevels gun{0.0f, 1.0f, 0.0f};
  const RumbleLevels grapple{0.0f, 0.0f, 1.0f};
  const RumbleLevels quiet{};

  // Split, right-handed: the cannon on the right, the grapple on the left.
  Check(Hands(RumbleHandAmplitudes(RumbleHand::Split, true, body), 1.0f, 1.0f), "split body on both");
  Check(Hands(RumbleHandAmplitudes(RumbleHand::Split, true, gun), 0.0f, 1.0f), "split cannon on the gun hand");
  Check(Hands(RumbleHandAmplitudes(RumbleHand::Split, true, grapple), 1.0f, 0.0f),
        "split grapple on the off hand");
  // Left-handed: the hands swap.
  Check(Hands(RumbleHandAmplitudes(RumbleHand::Split, false, gun), 1.0f, 0.0f), "left-handed cannon");
  Check(Hands(RumbleHandAmplitudes(RumbleHand::Split, false, grapple), 0.0f, 1.0f), "left-handed grapple");
  // A landing during a charge: the gun hand plays the stronger of the two.
  Check(Hands(RumbleHandAmplitudes(RumbleHand::Split, true, {0.4f, 1.0f, 0.0f}), 0.4f, 1.0f),
        "split mixes body and cannon");
  Check(Hands(RumbleHandAmplitudes(RumbleHand::Split, true, {1.0f, 0.4f, 0.0f}), 1.0f, 1.0f),
        "split body over a weaker cannon");
  Check(Hands(RumbleHandAmplitudes(RumbleHand::Split, true, quiet), 0.0f, 0.0f), "split quiet");

  // The other targets play every source, as one pad would.
  Check(Hands(RumbleHandAmplitudes(RumbleHand::Right, true, gun), 0.0f, 1.0f), "right plays the cannon");
  Check(Hands(RumbleHandAmplitudes(RumbleHand::Right, true, grapple), 0.0f, 1.0f), "right plays the grapple");
  Check(Hands(RumbleHandAmplitudes(RumbleHand::Left, true, body), 1.0f, 0.0f), "left plays the body");
  Check(Hands(RumbleHandAmplitudes(RumbleHand::Left, false, gun), 1.0f, 0.0f), "left ignores the gun hand");
  Check(Hands(RumbleHandAmplitudes(RumbleHand::Both, true, grapple), 1.0f, 1.0f), "both plays the grapple");
  Check(Hands(RumbleHandAmplitudes(RumbleHand::Both, true, {0.2f, 0.7f, 0.5f}), 0.7f, 0.7f),
        "both plays the strongest");

  // The default.
  Check(PortVrSettings{}.rumble_hand == RumbleHand::Split, "split is the default");

  std::puts("vr rumble tests passed");
  return 0;
}

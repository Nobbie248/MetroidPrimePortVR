// The snap turn (platform/include/vr/vr_snap_turn.h): PrimedGun's flick
// thresholds, re-arm, cooldown, head zone and angle clamp.

#include "vr/vr_snap_turn.h"

#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "vr snap turn regression failed: %s\n", what);
    std::abort();
  }
}
} // namespace

int main() {
  using namespace PortVr::SnapTurn;

  Input in;
  in.enabled = true;
  in.connected = true;
  in.accepting = true;
  in.degrees = 45;
  double now = 1.0;
  const auto sample = [&](Tracker& tracker, float x) {
    in.stick_x = x;
    in.now = now;
    now += 1.0 / 90.0;
    return tracker.Update(in);
  };

  {
    Tracker t;
    Check(sample(t, 0.0f) == 0.0f, "a centred stick does nothing");
    Check(sample(t, 0.7f) == 0.0f, "0.7 is short of the flick");
    Check(sample(t, 0.75f) == 45.0f, "past 0.72 to the right: 45 degrees right");
    Check(sample(t, 1.0f) == 0.0f, "holding the stick does not snap again");
    Check(sample(t, 0.4f) == 0.0f, "easing off above 0.32 does not re-arm");
    now += 1.0;
    Check(sample(t, 1.0f) == 0.0f, "nor does waiting with the stick held");
    Check(sample(t, 0.3f) == 0.0f, "under 0.32 re-arms");
    Check(sample(t, -0.9f) == -45.0f, "a flick to the left: 45 degrees left");
  }

  {
    // Eight game frames between two snaps, even with the stick re-armed.
    Tracker t;
    Check(sample(t, 1.0f) == 45.0f, "first snap");
    Check(sample(t, 0.0f) == 0.0f, "back to centre");
    Check(sample(t, 1.0f) == 0.0f, "a flick inside the cooldown is ignored");
    Check(sample(t, 0.0f) == 0.0f, "back to centre again");
    now += kCooldownSeconds;
    Check(sample(t, 1.0f) == 45.0f, "after the cooldown it snaps");
  }

  {
    // The head zone: no snap, and the flick stays used up after leaving it.
    Tracker t;
    in.near_head = true;
    Check(sample(t, 1.0f) == 0.0f, "no snap at the head");
    in.near_head = false;
    Check(sample(t, 1.0f) == 0.0f, "leaving the zone with the stick pushed does not snap");
    Check(sample(t, 0.0f) == 0.0f, "release");
    Check(sample(t, 1.0f) == 45.0f, "a fresh flick snaps");
    in.near_head = true;
    now += 1.0;
    Check(sample(t, 0.0f) == 0.0f, "a centred stick at the head re-arms");
    in.near_head = false;
  }

  {
    // A flick Samus cannot act on is used up.
    Tracker t;
    in.accepting = false;
    Check(sample(t, 1.0f) == 0.0f, "no snap while Samus cannot turn");
    in.accepting = true;
    Check(sample(t, 1.0f) == 0.0f, "held through it: still no snap");
    Check(sample(t, 0.0f) == 0.0f, "release");
    Check(sample(t, 1.0f) == 45.0f, "then it snaps");
  }

  {
    // The setting off, or the controller gone, resets.
    Tracker t;
    Check(sample(t, 1.0f) == 45.0f, "snap");
    in.enabled = false;
    Check(sample(t, 1.0f) == 0.0f, "nothing with the setting off");
    in.enabled = true;
    now += 1.0;
    Check(sample(t, 1.0f) == 45.0f, "turning it back on re-arms");
    in.connected = false;
    Check(sample(t, 1.0f) == 0.0f, "nothing without the controller");
    in.connected = true;
    Check(sample(t, 1.0f) == 45.0f, "reconnecting re-arms and clears the cooldown");
  }

  {
    // The angle: PrimedGun's choices, clamped to 30..90.
    Tracker t;
    in.degrees = 90;
    Check(sample(t, 1.0f) == 90.0f, "90 degrees");
    sample(t, 0.0f);
    now += 1.0;
    in.degrees = 15;
    Check(sample(t, -1.0f) == -30.0f, "15 is clamped to 30");
    sample(t, 0.0f);
    now += 1.0;
    in.degrees = 120;
    Check(sample(t, 1.0f) == 90.0f, "120 is clamped to 90");
    Check(ClampDegrees(60) == 60, "60 stays");
  }

  std::puts("vr snap turn regression passed");
  return 0;
}

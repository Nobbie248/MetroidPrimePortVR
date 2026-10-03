// The visor gesture (platform/include/vr/vr_visor_dpad.h): PrimedGun's head
// zone and stick-to-D-pad rules with the values NativeRuntime.cpp gives for the
// same inputs, and the tracker's grace, latch and press gating.

#include "vr/vr_visor_dpad.h"

#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "vr visor gesture regression failed: %s\n", what);
    std::abort();
  }
}
} // namespace

int main() {
  using namespace PortVr::VisorDpad;

  // The zone, with PrimedGun's defaults (radius 0.28, below 0.02): 0.34 m
  // around the head, from 6 cm below it to 28 cm above.
  const std::array<float, 3> head{0.1f, 1.6f, -0.2f};
  const auto at = [&](float x, float y, float z) {
    return std::array<float, 3>{head[0] + x, head[1] + y, head[2] + z};
  };
  Check(HandNearHead(at(-0.2f, 0.0f, 0.0f), head, 0.28f, 0.02f), "a hand beside the head is in the zone");
  Check(HandNearHead(at(-0.33f, 0.0f, 0.0f), head, 0.28f, 0.02f), "the zone reaches 6 cm past the radius");
  Check(!HandNearHead(at(-0.35f, 0.0f, 0.0f), head, 0.28f, 0.02f), "a hand beyond it is not");
  Check(HandNearHead(at(-0.2f, -0.05f, 0.0f), head, 0.28f, 0.02f), "a hand just below the head is in the zone");
  Check(!HandNearHead(at(-0.2f, -0.07f, 0.0f), head, 0.28f, 0.02f), "a hand lower than that is not");
  Check(HandNearHead(at(-0.2f, -0.2f, 0.0f), head, 0.28f, 0.2f), "the 'below' setting lowers the floor");
  Check(!HandNearHead(at(0.0f, 0.29f, 0.0f), head, 0.5f, 0.02f), "the zone stops 28 cm above the head");
  Check(HandNearHead(at(0.0f, 0.0f, 0.3f), head, 0.28f, 0.02f), "the zone is round: behind the head counts");

  Check(EffectiveDeadzone(0.45f) == 0.25f && EffectiveDeadzone(0.1f) == 0.1f, "the deadzone counts up to 0.25");

  // The stick, deadzone 0.25 (exit at 0.1375).
  Check(StickToDir(0.0f, 0.3f, 0.25f, Dir::None) == Dir::Up, "up");
  Check(StickToDir(0.0f, -0.3f, 0.25f, Dir::None) == Dir::Down, "down");
  Check(StickToDir(-0.3f, 0.1f, 0.25f, Dir::None) == Dir::Left, "left");
  Check(StickToDir(0.9f, -0.5f, 0.25f, Dir::None) == Dir::Right, "the dominant axis wins");
  Check(StickToDir(0.3f, 0.3f, 0.25f, Dir::None) == Dir::Up, "a diagonal tie goes to the vertical axis");
  Check(StickToDir(0.0f, 0.2f, 0.25f, Dir::None) == Dir::None, "inside the deadzone, nothing new starts");
  Check(StickToDir(0.0f, 0.2f, 0.25f, Dir::Up) == Dir::Up, "a held direction survives inside the deadzone");
  Check(StickToDir(0.0f, 0.13f, 0.25f, Dir::Up) == Dir::None, "and ends at 55% of it");
  Check(StickToDir(0.2f, 0.16f, 0.25f, Dir::Up) == Dir::Up, "changing axis there needs a 35% lead");
  Check(StickToDir(0.22f, 0.16f, 0.25f, Dir::Up) == Dir::Right, "which a bigger lead has");

  // The tracker.
  Tracker tracker;
  Input in;
  in.armed = true;
  in.near_head = true;
  in.deadzone = 0.25f;
  in.now = 10.0;
  Output out = tracker.Update(in);
  Check(out.zone && out.held == Dir::None, "a centred stick in the zone holds nothing");

  in.stick_x = -0.8f;
  in.now += 0.011;
  out = tracker.Update(in);
  Check(out.zone && out.held == Dir::Left && out.direction == Dir::Left, "a push left holds the D-pad left");

  // The game starts the transition: the D-pad lets go, then presses again.
  in.accepting = false;
  in.now += 0.011;
  out = tracker.Update(in);
  Check(out.held == Dir::None && out.direction == Dir::Left, "while the game would drop it, nothing is held");
  in.accepting = true;
  in.now += 0.011;
  out = tracker.Update(in);
  Check(out.held == Dir::Left, "the press comes back when the game can take it");

  // A flick during a transition outlives the transition.
  in.stick_x = 0.0f;
  in.stick_y = -0.8f;
  in.accepting = false;
  in.now += 0.011;
  out = tracker.Update(in);
  Check(out.held == Dir::None && out.direction == Dir::Down, "a flick down during a transition is noted");
  in.stick_y = 0.0f;
  in.now += 0.35;
  out = tracker.Update(in);
  Check(out.held == Dir::None && out.direction == Dir::Down, "and kept while the transition runs");
  in.accepting = true;
  in.now += 0.02;
  out = tracker.Update(in);
  Check(out.held == Dir::Down, "and pressed when it ends");
  in.now += kLatchSeconds;
  out = tracker.Update(in);
  Check(out.zone && out.held == Dir::None && out.direction == Dir::None, "then dropped after the latch");

  // The zone's grace.
  in.stick_x = 0.8f;
  in.now += 0.011;
  out = tracker.Update(in);
  Check(out.held == Dir::Right, "right");
  in.near_head = false;
  in.now += 0.2;
  out = tracker.Update(in);
  Check(out.zone && out.held == Dir::Right, "the zone outlives the hand's exit briefly");
  in.now += 0.1;
  out = tracker.Update(in);
  Check(!out.zone && out.held == Dir::None, "then hands the stick back");
  in.near_head = true;
  in.stick_x = 0.0f;
  in.now += 0.011;
  out = tracker.Update(in);
  Check(out.zone && out.held == Dir::None, "and leaving it forgot the latch");

  // Out of gameplay (or with the gesture off) the tracker is idle.
  in.stick_y = 0.8f;
  in.armed = false;
  in.now += 0.011;
  out = tracker.Update(in);
  Check(!out.zone && out.held == Dir::None, "a disarmed tracker does nothing");
  in.armed = true;
  in.near_head = false;
  in.now += 0.011;
  out = tracker.Update(in);
  Check(!out.zone, "re-armed away from the head, there is no grace to inherit");

  std::puts("vr visor gesture regression passed");
  return 0;
}

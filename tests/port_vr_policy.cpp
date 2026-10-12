// SPDX-License-Identifier: GPL-3.0-or-later
//
// The presentation policy (platform/vr/prime_vr_policy.cpp): what the headset
// shows for each game mode, the debounce of a change from the world to the
// front end, and the content tag that keeps immersive replay running through a
// mode change that keeps the presentation (a message box in the immersive view).

#include "vr/prime_vr_policy.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
using namespace PortVr;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "vr policy regression failed: %s\n", what);
    std::abort();
  }
}

uint64_t g_frame = 0;

PrimeVRPolicyConfig Config() {
  PrimeVRPolicyConfig config{};
  config.enabled = true;
  return config;
}

void Start(const PrimeVRPolicyConfig& config) {
  PrimeVRPolicyReset();
  PrimeVRPolicyConfigure(config);
  PrimeVRPolicySetSessionActive(true);
}

PrimeVRPolicySnapshot Publish(VRGameMode mode) {
  PrimeVRPolicyPublishGameMode(mode, ++g_frame);
  return PrimeVRPolicyGetSnapshot();
}

VRPresentationMode For(VRGameMode mode) { return PrimeVRPolicyPresentationFor(mode); }

void TestMessageInView() {
  Check(PrimeVRPolicyConfig{}.message_box_immersive, "message boxes stay in view by default");

  Start(Config());
  PrimeVRPolicySnapshot s = Publish(VRGameMode::InGame);
  Check(s.presentation == VRPresentationMode::Immersive, "gameplay is immersive");
  const uint64_t tag = s.content_tag;
  const uint64_t generation = s.safety_generation;

  Check(For(VRGameMode::Message) == VRPresentationMode::Immersive, "a message box would be shown in view");
  Check(PrimeVRPolicyGetSnapshot().game_mode == VRGameMode::InGame, "asking publishes nothing");
  s = Publish(VRGameMode::Message);
  Check(s.game_mode == VRGameMode::Message && s.presentation == VRPresentationMode::Immersive,
        "a message box switches at once and stays in view");
  Check(s.content_tag == tag && s.safety_generation == generation,
        "the content tag holds through the message box, so replay goes on");
  s = Publish(VRGameMode::Message);
  s = Publish(VRGameMode::InGame);
  Check(s.game_mode == VRGameMode::InGame && s.presentation == VRPresentationMode::Immersive &&
            s.content_tag == tag,
        "back to the world at once with the same tag");

  // Leaving the world for the front end is still debounced.
  for (int i = 1; i < 45; ++i) {
    s = Publish(VRGameMode::FrontEnd);
  }
  Check(s.game_mode == VRGameMode::InGame && s.presentation == VRPresentationMode::Immersive,
        "a short front-end blip keeps the world");
  s = Publish(VRGameMode::FrontEnd);
  Check(s.game_mode == VRGameMode::FrontEnd && s.presentation == VRPresentationMode::VirtualScreen,
        "a lasting front end goes to the screen");
}

void TestMessageOnScreen() {
  Start(Config());
  Publish(VRGameMode::InGame);
  const uint64_t before = PrimeVRPolicyGetSnapshot().safety_generation;
  PrimeVRPolicyConfig off = Config();
  off.message_box_immersive = false;
  PrimeVRPolicyConfigure(off);
  PrimeVRPolicySnapshot s = PrimeVRPolicyGetSnapshot();
  Check(s.safety_generation != before && s.presentation == VRPresentationMode::Immersive,
        "the switch is a new safety state, gameplay stays in view");
  Check(For(VRGameMode::Message) == VRPresentationMode::VirtualScreen, "off: a message box would go to the screen");
  s = Publish(VRGameMode::Message);
  Check(s.game_mode == VRGameMode::Message && s.presentation == VRPresentationMode::VirtualScreen,
        "off: the screen from the box's first frame, no debounce");
  s = Publish(VRGameMode::FrontEnd);
  Check(s.game_mode == VRGameMode::FrontEnd && s.presentation == VRPresentationMode::VirtualScreen,
        "a front-end frame after the box changes nothing");
  s = Publish(VRGameMode::InGame);
  Check(s.presentation == VRPresentationMode::Immersive, "off: back in view when the world is drawn");
}

void TestOtherModes() {
  PrimeVRPolicyReset();
  PrimeVRPolicyConfigure(Config());
  Check(For(VRGameMode::Message) == VRPresentationMode::Desktop, "no session: the desktop");

  PrimeVRPolicyConfig flat = Config();
  flat.immersive = false;
  Start(flat);
  Check(For(VRGameMode::Message) == VRPresentationMode::VirtualScreen &&
            For(VRGameMode::InGame) == VRPresentationMode::VirtualScreen,
        "flat screen: everything, message boxes included, on the screen");

  // A box opened under a cinematic camera is published as Cinematic (main.cpp).
  Start(Config());
  Check(For(VRGameMode::Cinematic) == VRPresentationMode::VirtualScreen, "cinematics on the cinema screen");
  Check(For(VRGameMode::Paused) == VRPresentationMode::Immersive,
        "the pause menu, logbook and map stay in view by default");
  PrimeVRPolicyConfig detached = Config();
  detached.game_menu_screen = true;
  Start(detached);
  Check(For(VRGameMode::Paused) == VRPresentationMode::VirtualScreen &&
            For(VRGameMode::Message) == VRPresentationMode::Immersive,
        "detached: the game menus on the screen, the message boxes still in view");
  Publish(VRGameMode::InGame);
  PrimeVRPolicySnapshot paused = Publish(VRGameMode::Paused);
  Check(paused.game_mode == VRGameMode::Paused && paused.presentation == VRPresentationMode::VirtualScreen,
        "detached: a game menu goes to the screen at once");
  Start(Config());
  Publish(VRGameMode::InGame);
  const uint64_t inViewTag = PrimeVRPolicyGetSnapshot().content_tag;
  paused = Publish(VRGameMode::Paused);
  Check(paused.presentation == VRPresentationMode::Immersive && paused.content_tag == inViewTag,
        "in view: a game menu keeps the content tag, so replay goes on");
  Check(For(VRGameMode::Unknown) == VRPresentationMode::VirtualScreen, "unknown: the screen");
  PrimeVRPolicyConfig cinema = Config();
  cinema.cinematic_screen = false;
  Start(cinema);
  Check(For(VRGameMode::Cinematic) == VRPresentationMode::Immersive, "immersive cinematics");

  Start(Config());
  PrimeVRPolicySetSessionActive(false);
  Check(For(VRGameMode::Message) == VRPresentationMode::Desktop, "session over: the desktop");
}
} // namespace

int main() {
  TestMessageInView();
  TestMessageOnScreen();
  TestOtherModes();
  std::puts("vr policy tests passed");
  return 0;
}

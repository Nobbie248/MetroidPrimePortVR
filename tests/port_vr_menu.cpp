// PrimedGun's VR menu (platform/include/vr/vr_menu.h and the controls in
// openxr_settings_panel.h): the image keeps PrimedGun's pixels and byte order,
// clicks land where PrimedGun's hit boxes put them, the two-press actions
// expire, and the panel, laser and hit dot hang where PrimedGun put them.

#include "vr/openxr_settings_panel.h"
#include "vr/vr_menu.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using namespace PortVr;
using namespace PortVr::VrMenu;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "vr menu regression failed: %s\n", what);
    std::abort();
  }
}

bool Near(float a, float b, float tolerance = 1.0e-3f) { return std::fabs(a - b) <= tolerance; }

bool Near(const Vec3& a, const Vec3& b, float tolerance = 1.0e-3f) {
  return Near(a[0], b[0], tolerance) && Near(a[1], b[1], tolerance) && Near(a[2], b[2], tolerance);
}

Quat AxisAngle(const Vec3& axis, float degrees) {
  const float half = degrees * 3.14159265f / 360.0f;
  const float s = std::sin(half);
  return {axis[0] * s, axis[1] * s, axis[2] * s, std::cos(half)};
}

uint32_t At(const Pixels& pixels, int x, int y) { return pixels[static_cast<size_t>(y) * kWidth + x]; }

class RecordingActions final : public Actions {
public:
  void SaveSettings() override { ++saves; }
  void ExitGame() override { ++exits; }
  int StateSlot() const override { return slot; }
  void SelectStateSlot(int s) override { slot = s; }
  void LoadState(int s) override { loaded = s; }
  void SaveState(int s) override { saved = s; }
  void LoadNewestState() override { ++newest; }
  void SaveOldestState() override { ++oldest; }
  bool ApplyCannonSlot(int s) override {
    cannon = s;
    return true;
  }

  int saves = 0;
  int exits = 0;
  int slot = 1;
  int loaded = 0;
  int saved = 0;
  int newest = 0;
  int oldest = 0;
  int cannon = -1;
};

// The middle of row `index`'s hit band, and of its value box's halves.
float RowY(uint32_t tab, uint32_t index) { return static_cast<float>(RowTextY(tab, index)) + 4.0f; }
constexpr float kLabelX = 200.0f;
constexpr float kMinusX = 800.0f;
constexpr float kPlusX = 920.0f;

void TestImage() {
  const State state{};
  const PortVrSettings settings{};
  const Pixels pixels = BuildPixels(state, settings, 1, false);
  Check(pixels.size() == static_cast<size_t>(kWidth) * kImageHeight, "image size");
  Check(At(pixels, 2, 250) == 0xD0100804u, "panel background");
  Check(At(pixels, 500, 5) == 0xE0FFB030u && At(pixels, 500, 507) == 0xE0FFB030u, "top and bottom bars");
  // The bytes are R, G, B, A: PrimedGun's accent is #30B0FF at 224.
  uint8_t bytes[4];
  const uint32_t bar = At(pixels, 500, 5);
  std::memcpy(bytes, &bar, sizeof(bytes));
  Check(bytes[0] == 0x30 && bytes[1] == 0xB0 && bytes[2] == 0xFF && bytes[3] == 0xE0, "RGBA byte order");
  // 'P' of the title, scale 4, top-left cell lit.
  Check(At(pixels, 49, 29) == 0xFFFFD8A0u, "title text");
  // LAYOUT is the active tab: bright underline; CALIBRATION's is dim.
  Check(At(pixels, 30, 100) == 0xFFFFB030u && At(pixels, 195, 100) == 0x604A2C12u, "tab underlines");
  // The layout page's help line in PrimedGun's lime.
  bool lime = false;
  for (int x = 80; x < 950 && !lime; ++x) {
    lime = At(pixels, x, 175) == 0xFF40F0E0u;
  }
  Check(lime, "layout help line");
  // The sprite strip: nothing between the canvas and it, the laser block and the dot.
  Check(At(pixels, 1, 515) == 0u, "gap below the canvas");
  Check(At(pixels, kLaserRect.x, kLaserRect.y) == kPointerColor &&
            At(pixels, kLaserRect.x + kLaserRect.w - 1, kLaserRect.y + kLaserRect.h - 1) == kPointerColor,
        "laser block");
  Check(At(pixels, kDotRect.x + 12, kDotRect.y + 12) == kPointerColor && At(pixels, kDotRect.x, kDotRect.y) == 0u,
        "hit dot");

  // A settings tab: the selected row's accent is bright, the others dim, and
  // a numeric row has its -/+.
  State calibration{};
  calibration.tab = kCalibrationTab;
  calibration.selected = 2;
  const Pixels rows = BuildPixels(calibration, settings, 1, false);
  Check(At(rows, 54, RowTextY(kCalibrationTab, 2)) == 0xFFFFB030u, "selected row accent");
  Check(At(rows, 54, RowTextY(kCalibrationTab, 1) - 4) == 0x80FFB030u, "unselected row accent");
  Check(At(rows, 790 + 2, RowTextY(kCalibrationTab, 2) + 6) == 0xFFFFB030u, "minus sign");
  Check(ViewKey(calibration, settings, 1, false) != ViewKey(calibration, settings, 1, true), "notice in the key");
}

void TestRows() {
  State s{};
  Check(ItemCount(s) == 0, "layout has no rows");
  s.tab = kCalibrationTab;
  Check(ItemCount(s) == 13, "calibration page 1");
  s.calibration_page = 1;
  Check(ItemCount(s) == 13, "calibration page 2");
  s.tab = kControlTab;
  Check(ItemCount(s) == 9, "control page 1");
  s.tab = kMovementTab;
  Check(ItemCount(s) == 11, "movement");
  s.tab = kTexturesTab;
  Check(ItemCount(s) == 6, "textures");
  s.tab = kStatesTab;
  Check(ItemCount(s) == 12, "states");
  Check(RowTextY(kStatesTab, 4) == 146 + 4 * 22 + 18, "state slots sit lower");
  Check(RowFromTextureY(kCalibrationTab, 160.0f, 13) == 1 && RowFromTextureY(kCalibrationTab, 100.0f, 13) == -1,
        "row hit band");
  const PortVrSettings settings{};
  State calibration{};
  calibration.tab = kCalibrationTab;
  const auto rows = BuildRows(calibration, settings, 1);
  Check(rows.size() == 13 && std::strcmp(rows[11].label, "CULLING CONE") == 0 && rows[11].value == "115.00",
        "PrimedGun's culling cone text");
  Check(SnapTurnStep(45, 1) == 60 && SnapTurnStep(90, 1) == 90 && SnapTurnStep(30, -1) == 30, "snap turn steps");
}

void TestClicks() {
  RecordingActions actions;
  PortVrSettings v{};
  State s{};
  Open(s);
  double now = 100.0;

  // Tabs: inside a tab switches; the gap between two does not.
  Click(s, v, 22.0f + 166.0f * 2 + 10.0f, 80.0f, now, actions);
  Check(s.tab == kControlTab && s.selected == 0, "tab click");
  Click(s, v, 22.0f + 150.0f + 8.0f, 80.0f, now, actions);
  Check(s.tab == kControlTab, "tab gap ignored");
  Click(s, v, 22.0f + 166.0f + 10.0f, 80.0f, now, actions);
  Check(s.tab == kCalibrationTab, "calibration tab");

  // Numeric: right half up, left half down.
  Click(s, v, kPlusX, RowY(kCalibrationTab, 2), now, actions);
  Check(s.selected == 2 && Near(v.metroid_hud_distance, 0.80f), "HUD distance up");
  Click(s, v, kMinusX, RowY(kCalibrationTab, 2), now, actions);
  Check(Near(v.metroid_hud_distance, 0.75f), "HUD distance down");
  // Toggle anywhere else on the row.
  Click(s, v, kLabelX, RowY(kCalibrationTab, 1), now, actions);
  Check(!v.cinematic_screen_enabled, "cinema screen toggle");
  // HUD VERTICAL keeps up and down apart.
  Click(s, v, kMinusX, RowY(kCalibrationTab, 4), now, actions);
  Check(Near(v.metroid_hud_offset_down, 0.01f) && Near(v.metroid_hud_offset_up, 0.0f), "HUD vertical split");

  // PAGE: a click on the row turns the page; SAMUS ARM PRESET is on page 2.
  Click(s, v, kLabelX, RowY(kCalibrationTab, 0), now, actions);
  Check(s.calibration_page == 1, "page turn");
  Click(s, v, kLabelX, RowY(kCalibrationTab, 10), now, actions);
  Check(Near(v.model_offset_y, -0.3f) && Near(v.rot_offset_y, 20.0f) && Near(v.rot_offset_z, -90.0f),
        "Samus arm preset");

  // RESET CALIBRATION takes two clicks within six seconds.
  Click(s, v, kLabelX, RowY(kCalibrationTab, 8), now, actions);
  Check(s.reset_confirm == kResetCalibration && Near(v.rot_offset_z, -90.0f), "reset armed");
  now += 7.0;
  Refresh(s, now);
  Check(s.reset_confirm == kNoReset, "confirmation expires");
  Click(s, v, kLabelX, RowY(kCalibrationTab, 8), now, actions);
  Click(s, v, kLabelX, RowY(kCalibrationTab, 8), now + 1.0, actions);
  Check(s.reset_confirm == kNoReset && Near(v.rot_offset_z, 0.0f) && Near(v.model_offset_y, 0.0f),
        "reset confirmed");
  // DETACH VR MENU FROM HAND.
  Click(s, v, kLabelX, RowY(kCalibrationTab, 11), now, actions);
  Check(v.vr_menu_floating, "detach toggle");

  // SAVE SETTINGS shows the notice; RESET ALL keeps the renderer's settings.
  Click(s, v, 150.0f, 120.0f, now, actions);
  Check(actions.saves == 1 && s.saved_notice_until > now, "save settings");
  v.render_scale = 1.5f;
  v.metroid_hud_size = 2.0f;
  v.patch_cannon_rotation = false;
  Click(s, v, 400.0f, 120.0f, now, actions);
  Check(Near(v.metroid_hud_size, 2.0f), "reset all armed");
  Click(s, v, 400.0f, 120.0f, now, actions);
  Check(Near(v.metroid_hud_size, 0.75f) && Near(v.render_scale, 1.5f) && !v.patch_cannon_rotation &&
            !v.vr_menu_floating,
        "reset all");

  // Control: RUMBLE TARGET cycles BOTH -> LEFT -> RIGHT.
  Click(s, v, 22.0f + 166.0f * 2 + 10.0f, 80.0f, now, actions);
  Check(v.rumble_hand == RumbleHand::Right, "rumble default");
  Click(s, v, kLabelX, RowY(kControlTab, 3), now, actions);
  Check(v.rumble_hand == RumbleHand::Both, "rumble cycles");
  // Movement: SNAP TURN ANGLE steps through PrimedGun's choices.
  Click(s, v, 22.0f + 166.0f * 3 + 10.0f, 80.0f, now, actions);
  Click(s, v, kPlusX, RowY(kMovementTab, 9), now, actions);
  Check(v.snap_turn_degrees == 60, "snap turn angle");

  // Textures: a slot is applied, then marked.
  Click(s, v, 22.0f + 166.0f * 4 + 10.0f, 80.0f, now, actions);
  Click(s, v, kLabelX, RowY(kTexturesTab, 2), now, actions);
  Check(actions.cannon == 2 && v.cannon_texture_slot == 2, "cannon slot");

  // States: a slot row selects; LOAD STATE takes two clicks.
  Click(s, v, 22.0f + 166.0f * 5 + 10.0f, 80.0f, now, actions);
  Click(s, v, kLabelX, RowY(kStatesTab, 4 + 2), now, actions);
  Check(actions.slot == 3 && v.vr_state_slot == 3, "state slot");
  Click(s, v, kLabelX, RowY(kStatesTab, 0), now, actions);
  Check(actions.loaded == 0 && s.state_confirm == kLoadState, "load armed");
  Click(s, v, kLabelX, RowY(kStatesTab, 0), now, actions);
  Check(actions.loaded == 3 && s.state_confirm == kNoStateAction, "load confirmed");

  // EXIT GAME, from any tab: two clicks.
  Click(s, v, 860.0f, 40.0f, now, actions);
  Check(actions.exits == 0 && s.reset_confirm == kExitGame, "exit armed");
  Click(s, v, 860.0f, 40.0f, now, actions);
  Check(actions.exits == 1, "exit confirmed");

  // Hover selects; opening goes back to LAYOUT.
  s.tab = kCalibrationTab;
  Hover(s, RowY(kCalibrationTab, 5));
  Check(s.selected == 5, "hover selects");
  Open(s);
  Check(s.tab == kLayoutTab && s.selected == 0, "opens on layout");
}

void TestPlacement() {
  // On the hand: the panel faces the grip's +Y, 0.18 below and 0.10 ahead of it.
  Pose grip{};
  const Pose panel = PanelPoseFromGrip(grip);
  Check(Near(screen_math::Rotate(panel.orientation, {0.0f, 0.0f, 1.0f}), {0.0f, 1.0f, 0.0f}), "wrist panel normal");
  Check(Near(panel.position, {0.0f, -0.18f, -0.10f}), "wrist panel offset");

  // Floating: 2.7 m ahead of a head looking left, facing it, upright.
  Pose head{};
  head.position = {0.0f, 1.6f, 0.0f};
  head.orientation = AxisAngle({0.0f, 1.0f, 0.0f}, 90.0f);
  Pose floating{};
  Check(FloatingPoseFromHead(head, floating), "floating pose");
  Check(Near(floating.position, {-2.7f, 1.6f, 0.0f}), "floating position");
  Check(Near(screen_math::Rotate(floating.orientation, {0.0f, 0.0f, 1.0f}), {1.0f, 0.0f, 0.0f}),
        "floating panel faces the head");

  // The pointer: centre, an offset point, from behind, and too far.
  Pose screen{};
  screen.position = {0.0f, 0.0f, -2.0f};
  Pose aim{};
  PointerHit hit = HitPanel(screen, 4.0f, 2.0f, aim);
  Check(hit.valid && Near(hit.x, 0.5f) && Near(hit.y, 0.5f) && Near(hit.distance, 2.0f), "centre hit");
  aim.orientation = AxisAngle({0.0f, 1.0f, 0.0f}, -std::atan2(1.0f, 2.0f) * 180.0f / 3.14159265f);
  hit = HitPanel(screen, 4.0f, 2.0f, aim);
  Check(hit.valid && Near(hit.x, 0.75f) && Near(hit.y, 0.5f), "offset hit");
  Pose behind{};
  behind.position = {0.0f, 0.0f, -4.0f};
  behind.orientation = AxisAngle({0.0f, 1.0f, 0.0f}, 180.0f);
  Check(HitPanel(screen, 4.0f, 2.0f, behind).valid, "hit from behind, as in PrimedGun");
  screen.position = {0.0f, 0.0f, -9.0f};
  Check(!HitPanel(screen, 4.0f, 2.0f, Pose{}).valid, "beyond the laser's reach");

  // The laser runs along the ray and faces the eye; the dot sits just in front.
  const Pose laser = LaserPose(Pose{}, 2.0f, {0.0f, 0.3f, 0.2f});
  Check(Near(laser.position, {0.0f, 0.0f, -1.0f}), "laser centre");
  Check(Near(screen_math::Rotate(laser.orientation, {0.0f, 1.0f, 0.0f}), {0.0f, 0.0f, -1.0f}), "laser along ray");
  Check(Near(screen_math::Rotate(laser.orientation, {0.0f, 0.0f, 1.0f}), {0.0f, 1.0f, 0.0f}), "laser faces eye");
  screen.position = {0.0f, 0.0f, -2.0f};
  const Pose dot = DotPose(screen, Pose{}, 2.0f);
  Check(Near(dot.position, {0.0f, 0.0f, -2.0f + kDotLift}), "dot in front of the panel");
}

void TestControls() {
  using settings_panel::Controls;
  using settings_panel::ToggleOptions;
  std::array<HandInputs, 2> hands{};
  ToggleOptions options;
  options.panel_hand = 0;
  Controls controls;
  bool open = false;
  const auto step = [&](float dt = 1.0f / 90.0f) {
    return controls.Update(hands, open, dt, OpenXRControllerMode::PrimedGun, options);
  };

  // The left stick click opens it; the game sees nothing until it is released.
  hands[0].thumbstick_click = true;
  auto frame = step();
  Check(open && frame.withheld && frame.pointing_hand == 1, "left click opens");
  hands[0].thumbstick_click = false;
  frame = step();
  Check(open && frame.withheld && !frame.select, "open after release");
  // The right trigger clicks once per pull; the left one does nothing.
  hands[0].trigger = 1.0f;
  frame = step();
  Check(!frame.select, "off hand trigger ignored");
  hands[0].trigger = 0.0f;
  hands[1].trigger = 1.0f;
  frame = step();
  Check(frame.select && frame.clicked, "right trigger clicks");
  frame = step();
  Check(frame.select && !frame.clicked, "held trigger clicks once");
  hands[1].trigger = 0.0f;
  // The right stick click is SET HEIGHT, not the menu.
  hands[1].thumbstick_click = true;
  step();
  Check(open, "right stick leaves it open");
  hands[1].thumbstick_click = false;
  step();
  // The left menu button closes it; the controllers come back once released.
  hands[0].menu = true;
  frame = step();
  Check(!open && frame.withheld, "menu button closes");
  hands[0].menu = false;
  frame = step();
  Check(!frame.withheld, "controllers back");

  // Left-handed: the right stick opens it and the left hand points.
  options.panel_hand = 1;
  hands[1].thumbstick_click = true;
  frame = step();
  Check(open && frame.pointing_hand == 0, "left-handed opens with the right stick");
  hands[1].thumbstick_click = false;
  step();
  hands[1].thumbstick_click = true;
  step();
  hands[1].thumbstick_click = false;
  step();
  Check(!open, "left-handed closes");
  options.panel_hand = 0;

  // A one-second press opens it; one press closes it.
  options.long_press = true;
  hands[0].thumbstick_click = true;
  step(0.5f);
  Check(!open, "half a second is not enough");
  step(0.6f);
  Check(open, "a second opens");
  step(0.5f);
  Check(open, "still held stays open");
  hands[0].thumbstick_click = false;
  step();
  hands[0].thumbstick_click = true;
  step();
  Check(!open, "a press closes");
  step(2.0f);
  Check(!open, "the closing press does not reopen");
  hands[0].thumbstick_click = false;
  step();
  options.long_press = false;

  // Near the head only, when asked.
  options.needs_head_zone = true;
  hands[0].thumbstick_click = true;
  step();
  Check(!open, "away from the head");
  hands[0].thumbstick_click = false;
  step();
  options.hand_near_head = true;
  hands[0].thumbstick_click = true;
  step();
  Check(open, "next to the head");
  hands[0].thumbstick_click = false;
  step();
  options.needs_head_zone = false;

  // Not allowed (overlays off, no layer): it closes.
  options.allowed = false;
  step();
  Check(!open, "closes when not allowed");
  hands[0].thumbstick_click = true;
  step();
  Check(!open, "stays closed when not allowed");
}

} // namespace

int main() {
  TestImage();
  TestRows();
  TestClicks();
  TestPlacement();
  TestControls();
  std::puts("vr menu tests passed");
  return 0;
}

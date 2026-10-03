// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/vr_debug_tab.h"

#include "vr/openxr_integration.h"
#include "vr/prime_vr_policy.h"
#include "vr/vr_settings.h"

#include <imgui.h>

namespace PortVr {
namespace {

const char* PresentationName(VRPresentationMode mode) {
  switch (mode) {
  case VRPresentationMode::Desktop:
    return "desktop";
  case VRPresentationMode::VirtualScreen:
    return "virtual screen";
  case VRPresentationMode::Immersive:
    return "immersive";
  default:
    return "unknown";
  }
}

bool SliderMetres(const char* label, float& value, float limit) {
  return ImGui::SliderFloat(label, &value, -limit, limit, "%.3f m");
}

bool SliderDegrees(const char* label, float& value, float limit) {
  return ImGui::SliderFloat(label, &value, -limit, limit, "%.1f deg");
}

} // namespace

void DrawVrDebugTab() {
  PortVrSettings s = GetVrSettings();
  bool changed = false;

  const bool running = OpenXRIsRunning();
  const PrimeVRPolicySnapshot policy = PrimeVRPolicyGetSnapshot();
  ImGui::Text("Headset: %s   presentation: %s", running ? "running" : "not running",
              PresentationName(policy.presentation));

  ImGui::SeparatorText("Headset");
  changed |= ImGui::Checkbox("Enable the headset (takes effect at the next start)", &s.enabled);
  changed |= ImGui::Checkbox("Immersive replay", &s.immersive_replay);
  ImGui::TextWrapped(
      "On: the world is drawn per eye. Off: the headset shows the flat image on the virtual screen, "
      "which is also what happens for menus and cinematics.");
  changed |= ImGui::SliderFloat("World scale", &s.world_scale, 0.5f, 3.0f, "%.2f units per metre");
  changed |= ImGui::SliderFloat("Lean back", &s.lean_back_degrees, -kVrLeanBackDegreesLimit,
                                kVrLeanBackDegreesLimit, "%.0f deg");
  changed |= ImGui::Checkbox("Cull against where the head looks", &s.frustum_culling_enabled);
  changed |= ImGui::SliderFloat("Culling cone", &s.frustum_culling_degrees, 60.f, 179.f, "%.0f deg");

  ImGui::SeparatorText("HUD (in front of the head)");
  changed |= ImGui::SliderFloat("HUD size", &s.metroid_hud_size, 0.1f, 3.0f, "%.2f x");
  changed |= ImGui::SliderFloat("HUD distance", &s.metroid_hud_distance, 0.1f, 3.0f, "%.2f x");
  ImGui::TextWrapped("Factors of the HUD frame's authored size and distance (PrimedGun: 0.75 and 0.75).");

  ImGui::SeparatorText("Arm cannon");
  changed |= ImGui::Checkbox("Tracked cannon (follows the controller)", &s.patch_cannon_rotation);
  int hand = s.use_right_hand ? 1 : 0;
  if (ImGui::Combo("Hand", &hand, "Left\0Right\0")) {
    s.use_right_hand = hand == 1;
    changed = true;
  }
  changed |= SliderMetres("Position right", s.offset_x, 0.5f);
  changed |= SliderMetres("Position up", s.offset_y, 0.5f);
  changed |= SliderMetres("Position back", s.offset_z, 0.5f);
  changed |= SliderDegrees("Pitch offset", s.rot_offset_x, 90.f);
  changed |= SliderDegrees("Yaw offset", s.rot_offset_y, 90.f);
  changed |= SliderDegrees("Roll offset", s.rot_offset_z, 180.f);
  changed |= SliderMetres("Model right", s.model_offset_x, 0.5f);
  changed |= SliderMetres("Model forward", s.model_offset_y, 0.5f);
  changed |= SliderMetres("Model up", s.model_offset_z, 0.5f);
  if (ImGui::Button("Samus arm preset")) {
    ApplyVrSamusArmPreset();
    s = GetVrSettings();
    changed = false;
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset calibration")) {
    ResetVrCalibrationOffsets();
    s = GetVrSettings();
    changed = false;
  }

  ImGui::SeparatorText("Scan visor");
  changed |= ImGui::Checkbox("Look to scan", &s.patch_gun_ray_target);
  ImGui::TextWrapped("The scan target and the scan icons follow where the head looks, and a scan lock keeps the "
                     "body's facing (PrimedGun's gun ray / scan target hook).");
  changed |= ImGui::SliderFloat("Look reach", &s.gun_targeting_distance, 15.f, 100.f, "%.0f units");
  changed |= ImGui::SliderFloat("Look radius", &s.gun_targeting_radius, 0.5f, 12.f, "%.1f units");
  ImGui::TextWrapped("The radius widens the cones around the gaze that pick the target and show icons "
                     "(PrimedGun: 60 and 4).");

  ImGui::SeparatorText("Camera");
  changed |= ImGui::Checkbox("No camera bob or idle sway", &s.patch_no_idle_sway);
  changed |= ImGui::Checkbox("No arm cannon idle fidget", &s.patch_disable_arm_cannon_idle_fidget);
  changed |= ImGui::Checkbox("Remove cinematic bars", &s.remove_cinematic_bars);
  ImGui::TextWrapped("The morph ball and visor letterbox shrink the 3D view, which distorts it in the headset; "
                     "this keeps the view full and skips the cinema-bar overlay.");

  ImGui::SeparatorText("Controls");
  int mode = static_cast< int >(s.controller_mode);
  if (ImGui::Combo("Controller mode", &mode, "PrimedGun\0Gamepad\0None\0")) {
    s.controller_mode = static_cast< ControllerModeSetting >(mode);
    changed = true;
  }
  ImGui::TextWrapped("PrimedGun: the headset controllers drive pad 1 with PrimedGun's layout. "
                     "Gamepad: they act as a plain gamepad. None: a real pad plays.");
  changed |= ImGui::Checkbox("Rumble", &s.rumble_enabled);
  changed |= ImGui::SliderFloat("Rumble intensity", &s.rumble_intensity, 0.f, 1.f, "%.2f");
  changed |= ImGui::Checkbox("Grip inputs", &s.grip_inputs_enabled);
  changed |= ImGui::Checkbox("Jump with the primary button", &s.combat_jump_use_primary_button);

  ImGui::SeparatorText("Virtual screen (menus, cinematics)");
  changed |= ImGui::SliderFloat("Screen distance", &s.screen_distance_meters, 0.5f, 5.f, "%.2f m");
  changed |= ImGui::SliderFloat("Screen width", &s.screen_width_meters, 0.5f, 6.f, "%.2f m");
  changed |= ImGui::Checkbox("Cinematics on the screen", &s.cinematic_screen_enabled);
  changed |= ImGui::Checkbox("Pause, map and logbook on the screen", &s.game_menu_screen_enabled);

  ImGui::SeparatorText("Diagnostics");
  changed |= ImGui::Checkbox("[xr-diag] logging", &s.diagnostics_logging);

  if (changed) {
    SetVrSettings(s);
  }
}

} // namespace PortVr

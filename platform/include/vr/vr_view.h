// SPDX-License-Identifier: GPL-3.0-or-later
//
// The headset's view of the game, for the game thread: what the game camera
// must do differently while the headset shows the world per eye, and the
// tracked arm cannon.
//
// The eyes get the head's rotation and position through the stereo packet
// (aurora composes them into every draw), so the game camera stays the body
// facing: yaw from the stick, no pitch, no bob. The game still needs the head's
// facing for what it computes on the CPU: the culling frustum, the audio
// listener. The cannon follows the controller's aim pose, placed relative to
// the player's eye with PrimedGun's calibration (world scale, position and
// rotation offsets, model offset, smoothing).

#pragma once

#include "Kyoto/Math/CMatrix3f.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "MetroidPrime/TGameTypes.hpp"

class CFrustumPlanes;
class CPlayer;
class CStateManager;

namespace PortSurfaceCulling { struct StereoVolume; }
namespace PortVr {

// The headset is running and this frame is shown immersively (the world per
// eye rather than on the virtual screen).
bool VrImmersive() noexcept;

// `cameraXf` with the head's rotation (relative to the recentred base)
// composed in; `cameraXf` itself when not immersive or the head is unknown.
CTransform4f VrHeadFacingTransform(const CTransform4f& cameraXf) noexcept;

// The head's gaze in the world, as the eyes see it: from the head's position
// (the game camera plus the head's offset from the tracking base) along its
// forward, lean back included. False when not immersive or the head is unknown.
bool VrHeadGaze(const CTransform4f& cameraXf, CVector3f& origin, CVector3f& direction) noexcept;

// The eyes' facing in the world, as VrHeadGaze sees it (lean back included),
// kept upright: `cameraXf`'s rotation with the head's yaw and pitch composed
// in but not its roll, or `cameraXf`'s own rotation when not immersive or the
// head is unknown. For the billboards the game turns to the camera (scan
// icons, target reticles), which would otherwise face the body and show their
// edge when the head turns, and would roll with a tilted head.
CMatrix3f VrHeadViewRotation(const CTransform4f& cameraXf) noexcept;
// Head orientation and position for CPU effects; the world render camera
// remains unchanged so stereo replay does not apply head tracking twice.
CTransform4f VrHeadViewTransform(const CTransform4f& cameraXf) noexcept;

// Replaces `frustum` with the head-facing cone of the settings' culling angle
// when immersive and culling is enabled; false leaves it alone.
bool VrCullingFrustum(const CTransform4f& cameraXf, float nearZ, CFrustumPlanes& frustum) noexcept;

// Conservative actual-eye volumes for static world surface culling. Includes
// lean-back, positional tracking and a margin around each eye. False keeps all.
bool VrSurfaceCullingVolume(const CTransform4f& cameraXf,
                            PortSurfaceCulling::StereoVolume& volume) noexcept;

// Immersive: the first-person camera keeps its yaw only (the headset supplies
// the pitch), does not bob, and the arm cannon does not fidget.
bool VrFlattenLookPitch() noexcept;
bool VrNoCameraBob() noexcept;
bool VrNoArmCannonFidget() noexcept;
// The headset is running and the settings remove the letterbox: the game's
// viewport scale stays 1 and the cinema-bars camera filter is not drawn.
bool VrRemoveCinematicBars() noexcept;
// The headset is running and the settings put the sky at infinity: the sky
// draw is routed AURORA_STEREO_ROUTE_SKY (CWorld::DrawSky), so each eye sees
// the camera-centred sky dome with the head's rotation only, without the eye
// offset that would place it at its modelled sixty units.
bool VrSkyAtInfinity() noexcept;
// The headset is running and the settings turn the space warp off
// (CStateManager::DrawSpaceWarp draws nothing).
bool VrHideSpaceWarp() noexcept;
// Immersive and the settings turn the scan window's zoom off: the window's
// copy is the pane's own size (CPlayerVisor::DrawScanEffect).
bool VrScanWindowNoZoom() noexcept;
// The beam wheel's hover, for the HUD's beam menu (CHudVisorBeamMenu): the
// beam the aim ray points at while the weapon hand's B holds the wheel open
// (vr_pad.cpp), as CPlayerState::EBeamId (0 Power, 1 Ice, 2 Wave, 3 Plasma),
// or -1 when the wheel is closed, nothing is hovered, or the
// vr_beam_wheel_hud_highlight setting is off. The menu lights that beam's box,
// where PrimedGun drew its own panel with a frame around the hovered icon.
int VrBeamWheelHoverBeam() noexcept;

// The 6DOF cannon. Once per simulation tick from CPlayer::UpdateGunTransform:
// when the tracked controller drives the cannon, replaces `gunXf` (world) and
// returns true. `cameraXf` is the current game camera (its yaw is the body).
bool VrCannonTransform(const CStateManager& mgr, const CPlayer& player, const CTransform4f& cameraXf,
                       CTransform4f& gunXf) noexcept;
// Whether the last VrCannonTransform placed the cannon.
bool VrCannonTracked() noexcept;
// The selected cannon controller's calibrated world-space aim, without
// advancing model smoothing. False when the controller is not tracked.
bool VrCannonAim(const CPlayer& player, const CTransform4f& cameraXf,
                 CVector3f& origin, CVector3f& direction) noexcept;
// Idle reticle on the cannon ray's first world hit, or its maximum reach.
bool VrCannonTargetPoint(const CStateManager& mgr, CVector3f& point) noexcept;
// The body turned at once (the snap turn): the next VrCannonTransform places
// the cannon without easing from the old facing, as PrimedGun's snap turn
// dropped its cannon smoothing.
void VrResetCannonSmoothing() noexcept;

// Look to scan (PrimedGun's gun ray / scan target hook, vr_patch_gun_ray_target):
// in the scan visor the head picks the scan target and the scan icons, rather
// than the body's facing (vr/vr_look_scan.h has the maths). Game thread.
//
// Look to lock-on and look to grapple (vr_look_to_lock_on, vr_look_to_grapple)
// do the same outside the scan visor for the orbit target, each for its kind:
// grapple points, and everything else that can be locked.
//
// Once per tick from CPlayer::UpdateOrbitInput, after the orbitable objects
// are gathered: measures the scannable objects (scan visor) or the orbitable
// ones (other visors) around the head's gaze.
void VrLookTargetUpdate(CStateManager& mgr, const CPlayer& player) noexcept;
// True when look to scan owns this tick's choice (immersive, scan visor,
// unmorphed, setting on); `id` is then the head's pick, possibly none.
// CPlayer::FindOrbitTargetId returns it instead of its screen-box choice.
bool VrLookToScanTarget(const CStateManager& mgr, TUniqueId& id) noexcept;
// The scannable objects near where the head looks, for their scan icons
// (CPlayerVisor::UpdateScanObjectIndicators); null and 0 when inactive.
const TUniqueId* VrLookToScanNearby(const CStateManager& mgr, int& count) noexcept;
// True when look to lock-on or look to grapple owns this tick's choice
// (immersive, not the scan visor, unmorphed, either setting on); `id` is then
// the head's pick, or else `gamePick` (FindBestOrbitableObject's screen-box
// choice) if that is of a kind the head does not pick, or none.
// CPlayer::FindOrbitTargetId returns it.
bool VrLookToLockTarget(const CStateManager& mgr, TUniqueId gamePick, TUniqueId& id) noexcept;
// The body keeps its facing while look to scan is active, and, with
// vr_look_lock_no_camera_turn, during an orbit lock on a target of a kind the
// head picks (look to lock-on, look to grapple): the lock does not turn the
// camera and the player toward it, which would swing the whole world around
// the headset and take the target out from under the gaze
// (CFirstPersonCamera::UpdateTransform, CPlayer::UpdateOrbitOrientation), nor
// break when the body's facing drifts off it (CPlayer::ValidateCurrentOrbitTargetId).
bool VrLookHoldsFacing(const CStateManager& mgr, const CPlayer& player) noexcept;
// The rendered model's offset from the tracked pose (PrimedGun's model offset
// and its base forward offset), in world units; zero when not tracked.
CVector3f VrCannonModelOffsetWorld() noexcept;

// PrimedGun's VR menu (vr/vr_menu.h), once per presented frame from
// CGraphics::EndScene: while it is open in the headset, applies the laser's
// hover and clicks and hands Aurora the menu's image when it changes
// (vr_menu.cpp). Nothing without OpenXR.
void VrMenuUpdate() noexcept;

} // namespace PortVr

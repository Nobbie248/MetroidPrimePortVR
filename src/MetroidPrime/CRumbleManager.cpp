#include "MetroidPrime/CRumbleManager.hpp"

#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/RumbleFxTable.hpp"


#include "Kyoto/Math/CloseEnough.hpp"

#ifdef TARGET_PC
#include "vr/vr_pad.h"

namespace {
// While the headset controllers own port 0, each source pulses on a port of
// its own, which vr_pad.cpp turns into a haptics channel (vr/vr_rumble.h):
// the body on port 0, the arm cannon on port 1, the grapple arm on port 2.
// The port rides in the handle's bits 4-5, which CRumbleVoice leaves unused
// (owner << 8 | channel 0-3), so StopRumble stops the effect where it plays.
const int kPortRumbleHandlePortShift = 4;
const short kPortRumbleHandlePortMask = 3 << kPortRumbleHandlePortShift;

CRumbleManager::EPortRumbleSource PortRumbleSourceOf(ERumbleFxId fx) {
  switch (fx) {
  case kRFX_PlayerGunCharge:
  case kRFX_PlayerMissileFire:
    return CRumbleManager::kPRS_ArmCannon;
  case kRFX_PlayerGrappleFire:
  case kRFX_PlayerGrappleSwoosh:
    return CRumbleManager::kPRS_GrappleArm;
  default:
    return CRumbleManager::kPRS_Body;
  }
}
} // namespace
#endif

CRumbleManager::CRumbleManager() {}

CRumbleManager::~CRumbleManager() { x0_rumbleGenerator.HardStopAll(); }

short CRumbleManager::Rumble(CStateManager& mgr, ERumbleFxId fx, float gain,
                             ERumblePriority priority) {
#ifdef TARGET_PC
  return PortRumble(mgr, fx, gain, priority, PortRumbleSourceOf(fx));
#else
  if (gpGameState->GameOptions().GetIsRumbleEnabled())
    return x0_rumbleGenerator.Rumble(skRumbleFxTable[size_t(fx)], gain, priority, kIOP_Player1);
  return -1;
#endif
}

#ifdef TARGET_PC
short CRumbleManager::PortRumble(CStateManager& mgr, ERumbleFxId fx, float gain,
                                 ERumblePriority priority, EPortRumbleSource source) {
  if (!gpGameState->GameOptions().GetIsRumbleEnabled())
    return -1;
  EIOPort port = kIOP_Player1;
  if (PortVr::VrPadOwnsPort0()) {
    port = source == kPRS_ArmCannon    ? kIOP_Player2
           : source == kPRS_GrappleArm ? kIOP_Player3
                                       : kIOP_Player1;
  }
  const short handle =
      x0_rumbleGenerator.Rumble(skRumbleFxTable[size_t(fx)], gain, priority, port);
  if (handle == -1)
    return -1;
  return short(handle | (port << kPortRumbleHandlePortShift));
}
#endif

short CRumbleManager::Rumble(CStateManager& mgr, const CVector3f& pos, ERumbleFxId fx, float dist,
                             ERumblePriority priority) {
  if (!close_enough(dist, 0.f))
  {
    CVector3f delta = mgr.GetPlayer()->GetTranslation() - pos;
    if (delta.MagSquared() < dist * dist)
      return Rumble(mgr, fx, 1.f - delta.Magnitude() / dist, priority);
  }

  return -1;
}

void CRumbleManager::StopRumble(short id) {
  if (id == -1)
    return;
#ifdef TARGET_PC
  const EIOPort port = EIOPort((id & kPortRumbleHandlePortMask) >> kPortRumbleHandlePortShift);
  x0_rumbleGenerator.Stop(short(id & ~kPortRumbleHandlePortMask), port);
#else
  x0_rumbleGenerator.Stop(id, kIOP_Player1);
#endif
}

void CRumbleManager::Update(float dt) { x0_rumbleGenerator.Update(dt); }

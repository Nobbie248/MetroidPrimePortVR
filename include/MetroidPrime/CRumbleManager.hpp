#ifndef _CRUMBLEMANAGER
#define _CRUMBLEMANAGER

#include "types.h"

#include "Kyoto/Input/CRumbleGenerator.hpp"

class CStateManager;
class CVector3f;

class CRumbleManager {
public:
  CRumbleManager();
  ~CRumbleManager();

  short Rumble(CStateManager&, const CVector3f&, ERumbleFxId, float, ERumblePriority);
  short Rumble(CStateManager&, ERumbleFxId, float, ERumblePriority);
#ifdef TARGET_PC
  // The part of Samus an effect comes from. The headset controllers can play
  // each on its own hand (vr/vr_rumble.h); a pad plays them all alike. Rumble
  // takes it from the effect; PortRumble names it for an effect shared with
  // the body, as the Wave Buster's lock is a bump.
  enum EPortRumbleSource { kPRS_Body, kPRS_ArmCannon, kPRS_GrappleArm };
  short PortRumble(CStateManager&, ERumbleFxId, float, ERumblePriority, EPortRumbleSource);
  // The rumble a weapon's recoil shake gives through the camera manager, for
  // the headset, which skips that shake: the same pulse, from the arm cannon.
  void PortRecoilRumble(CStateManager& mgr) {
    PortRumble(mgr, kRFX_CameraShake, 1.f, kRP_Two, kPRS_ArmCannon);
  }
#endif

  void StopRumble(short id);
  void HardStopAll() { x0_rumbleGenerator.HardStopAll(); } 
  void Update(float dt);

private:
  CRumbleGenerator x0_rumbleGenerator;
};

#endif // _CRUMBLEMANAGER
